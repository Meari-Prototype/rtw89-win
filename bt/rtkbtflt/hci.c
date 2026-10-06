// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Meari
/*
 * rtkbtflt: the patch file and one session.
 *
 * A session runs in D0Entry (PASSIVE_LEVEL), before BTHUSB knows the device is in D0, and talks HCI over USB itself:
 * commands as class requests on the default control pipe (bmRequestType 0x20, bRequest 0), events from the interrupt
 * IN pipe of the HCI interface. One command at a time, each answered by its Command Complete before the next one.
 *  - At a start (D3Final) the device is unconfigured and BTHUSB has not started: we select the configuration ourselves
 *    (every interface in alternate setting 0, as BTHUSB does) to get the interrupt pipe. We do not unconfigure it
 *    afterwards; BTHUSB selects the configuration again when it starts, which replaces our pipes.
 *  - At a D3 -> D0 the device is in BTHUSB's configuration: we never select one then (BTHUSB's pipe handles would go
 *    stale) and use the interrupt pipe seen in BTHUSB's SELECT_CONFIGURATION (urb.c).
 * Our interrupt read is one WDFREQUEST, resubmitted from its completion routine while the session is open; what it
 * reads goes into Dev->Acc, where the waiting command takes its event from. If BTHUSB still has an event read pending
 * below us, that read gets the next event instead; urb.c hands the awaited Command Complete over to us
 * (FltOfferStolen) and gives BTHUSB's read back to the USB stack, so BTHUSB never sees our events.
 * The download follows btrtl (epatch.c): HCI_Read_Local_Version -> ROM? -> 0xFC6D ROM version -> 0xFC61 key id ->
 * 0xFC20 fragments -> HCI_Read_Local_Version again. Nothing is retried; any failure ends the session with the device
 * left to BTHUSB as it is.
 */
#include "rtkbtflt.h"

static const UCHAR  mRegSecProj[5] = { 0x10, 0xa4, 0xad, 0x00, 0xb0 };   // btrtl RTL_SEC_PROJ

typedef struct {
  UCHAR   HciVer;
  USHORT  HciRev;
  UCHAR   LmpVer;
  USHORT  Mfg;
  USHORT  LmpSubver;
} FLT_VERSION_INFO;

EVT_WDF_REQUEST_COMPLETION_ROUTINE  FltReadComplete;

/* ---- the patch file ---- */

/* Reads a file of the driver's own directory (the package's directory in the driver store) into non-paged pool. */
static NTSTATUS
FltReadDriverFile (
  _In_  PCWSTR   Name,
  _Out_ PUCHAR   *Data,
  _Out_ SIZE_T   *Len
  )
{
  UNICODE_STRING             Path;
  OBJECT_ATTRIBUTES          Oa;
  IO_STATUS_BLOCK            Iosb;
  FILE_STANDARD_INFORMATION  Info;
  LARGE_INTEGER              Offset;
  HANDLE                     Dir;
  HANDLE                     File;
  PUCHAR                     Buf;
  NTSTATUS                   Status;

  *Data  = NULL;
  *Len   = 0;
  Status = IoGetDriverDirectory (WdfDriverWdmGetDriverObject (WdfGetDriver ()), DriverDirectoryImage, 0, &Dir);
  if (!NT_SUCCESS (Status)) {
    return Status;
  }

  RtlInitUnicodeString (&Path, Name);
  InitializeObjectAttributes (&Oa, &Path, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, Dir, NULL);
  Status = ZwCreateFile (&File, GENERIC_READ | SYNCHRONIZE, &Oa, &Iosb, NULL, FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ,
                         FILE_OPEN, FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE, NULL, 0);
  ZwClose (Dir);
  if (!NT_SUCCESS (Status)) {
    return Status;
  }

  Buf    = NULL;
  Status = ZwQueryInformationFile (File, &Iosb, &Info, sizeof (Info), FileStandardInformation);
  if (NT_SUCCESS (Status) && ((Info.EndOfFile.QuadPart <= 0) || (Info.EndOfFile.QuadPart > FLT_FILE_MAX))) {
    Status = STATUS_FILE_INVALID;
  }

  if (NT_SUCCESS (Status)) {
    Buf = ExAllocatePool2 (POOL_FLAG_NON_PAGED, (SIZE_T)Info.EndOfFile.QuadPart, FLT_POOL_TAG);
    if (Buf == NULL) {
      Status = STATUS_INSUFFICIENT_RESOURCES;
    }
  }

  if (NT_SUCCESS (Status)) {
    Offset.QuadPart = 0;
    Status = ZwReadFile (File, NULL, NULL, NULL, &Iosb, Buf, Info.EndOfFile.LowPart, &Offset, NULL);
    if (NT_SUCCESS (Status) && (Iosb.Information != Info.EndOfFile.LowPart)) {
      Status = STATUS_END_OF_FILE;
    }
  }

  ZwClose (File);
  if (!NT_SUCCESS (Status)) {
    if (Buf != NULL) {
      ExFreePoolWithTag (Buf, FLT_POOL_TAG);
    }

    return Status;
  }

  *Data = Buf;
  *Len  = Info.EndOfFile.LowPart;
  return STATUS_SUCCESS;
}

VOID
FltFreeFirmware (
  _In_ PFLT_DEVICE  Dev
  )
{
  Dev->FwOk = FALSE;
  if (Dev->Fw != NULL) {
    ExFreePoolWithTag (Dev->Fw, FLT_POOL_TAG);
    Dev->Fw = NULL;
  }

  if (Dev->Cfg != NULL) {
    ExFreePoolWithTag (Dev->Cfg, FLT_POOL_TAG);
    Dev->Cfg = NULL;
  }

  Dev->FwLen  = 0;
  Dev->CfgLen = 0;
}

/* The first session of a start, under the guard: rtl8852bu_fw.bin (must be a v1 epatch for project 20, the 8852B)
   and rtl8852bu_config.bin (optional, as in btrtl; when present it must be valid). Told in every session's log. */
VOID
FltLoadFirmware (
  _In_ PFLT_DEVICE  Dev
  )
{
  CHAR      Err[96];
  int       Entries;
  NTSTATUS  Status;

  FltFreeFirmware (Dev);
  Dev->FwTried = TRUE;
  Status = FltReadDriverFile (FLT_FW_NAME, &Dev->Fw, &Dev->FwLen);
  if (!NT_SUCCESS (Status)) {
    (VOID)RtlStringCbPrintfA (Dev->FwNote, sizeof (Dev->FwNote), "patch file %ws not read (0x%08x)", FLT_FW_NAME,
                              Status);
    goto Fail;
  }

  if (rtkbt_epatch_parse (Dev->Fw, Dev->FwLen, &Dev->Ep, Err, sizeof (Err)) != 0) {
    (VOID)RtlStringCbPrintfA (Dev->FwNote, sizeof (Dev->FwNote), "patch file: %s", Err);
    goto Fail;
  }

  if (Dev->Ep.project_id != RTKBT_PROJECT_8852B) {
    (VOID)RtlStringCbPrintfA (Dev->FwNote, sizeof (Dev->FwNote), "patch file is for project %d, not the 8852B (%d)",
                              Dev->Ep.project_id, RTKBT_PROJECT_8852B);
    goto Fail;
  }

  Status = FltReadDriverFile (FLT_CFG_NAME, &Dev->Cfg, &Dev->CfgLen);
  if ((Status == STATUS_OBJECT_NAME_NOT_FOUND) || (Status == STATUS_NO_SUCH_FILE)) {
    Dev->CfgLen = 0;              // btrtl then sends the patch alone
  } else if (!NT_SUCCESS (Status)) {
    (VOID)RtlStringCbPrintfA (Dev->FwNote, sizeof (Dev->FwNote), "config %ws not read (0x%08x)", FLT_CFG_NAME, Status);
    goto Fail;
  } else {
    Entries = rtkbt_config_check (Dev->Cfg, Dev->CfgLen, Err, sizeof (Err));
    if (Entries < 0) {
      (VOID)RtlStringCbPrintfA (Dev->FwNote, sizeof (Dev->FwNote), "config: %s", Err);
      goto Fail;
    }
  }

  Dev->FwOk = TRUE;
  (VOID)RtlStringCbPrintfA (Dev->FwNote, sizeof (Dev->FwNote), "patch file %lu bytes, fw_version 0x%08x, %u patches; "
                            "config %lu bytes", (ULONG)Dev->FwLen, Dev->Ep.fw_version, Dev->Ep.num_patches,
                            (ULONG)Dev->CfgLen);
  FltLog (NULL, "%s", Dev->FwNote);
  return;

Fail:
  FltLog (NULL, "%s", Dev->FwNote);
  FltFreeFirmware (Dev);
}

/* ---- USB ---- */

/* One URB to the device below, synchronously, cancelled after TimeoutMs. PASSIVE_LEVEL. */
static NTSTATUS
FltSubmitUrb (
  _In_ PFLT_DEVICE  Dev,
  _In_ PURB         Urb,
  _In_ ULONG        TimeoutMs
  )
{
  WDF_MEMORY_DESCRIPTOR     Desc;
  WDF_REQUEST_SEND_OPTIONS  Opts;
  NTSTATUS                  Status;

  WDF_MEMORY_DESCRIPTOR_INIT_BUFFER (&Desc, Urb, Urb->UrbHeader.Length);
  WDF_REQUEST_SEND_OPTIONS_INIT (&Opts, WDF_REQUEST_SEND_OPTION_TIMEOUT | WDF_REQUEST_SEND_OPTION_IGNORE_TARGET_STATE);
  WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT (&Opts, WDF_REL_TIMEOUT_IN_MS (TimeoutMs));
  Status = WdfIoTargetSendInternalIoctlOthersSynchronously (Dev->Target, NULL, IOCTL_INTERNAL_USB_SUBMIT_URB, &Desc,
                                                             NULL, NULL, &Opts, NULL);
  if (NT_SUCCESS (Status) && !USBD_SUCCESS (Urb->UrbHeader.Status)) {
    Status = STATUS_UNSUCCESSFUL;
  }

  return Status;
}

/* GET_DESCRIPTOR (configuration) into a new non-paged buffer of Len bytes. */
static NTSTATUS
FltGetConfigDescriptor (
  _In_  PFLT_DEVICE                     Dev,
  _In_  USHORT                          Len,
  _Out_ PUSB_CONFIGURATION_DESCRIPTOR   *Desc
  )
{
  PUSB_CONFIGURATION_DESCRIPTOR  D;
  NTSTATUS                       Status;

  *Desc = NULL;
  D     = ExAllocatePool2 (POOL_FLAG_NON_PAGED, Len, FLT_POOL_TAG);
  if (D == NULL) {
    return STATUS_INSUFFICIENT_RESOURCES;
  }

  RtlZeroMemory (&Dev->CmdUrb, sizeof (Dev->CmdUrb));
  UsbBuildGetDescriptorRequest (&Dev->CmdUrb, (USHORT)sizeof (struct _URB_CONTROL_DESCRIPTOR_REQUEST),
                                USB_CONFIGURATION_DESCRIPTOR_TYPE, 0, 0, D, NULL, Len, NULL);
  Status = FltSubmitUrb (Dev, &Dev->CmdUrb, FLT_T_SEND);
  if (NT_SUCCESS (Status) &&
      ((Dev->CmdUrb.UrbControlDescriptorRequest.TransferBufferLength != Len) ||
       (D->bDescriptorType != USB_CONFIGURATION_DESCRIPTOR_TYPE) || (D->bLength < sizeof (*D)))) {
    Status = STATUS_DEVICE_DATA_ERROR;
  }

  if (!NT_SUCCESS (Status)) {
    FltLog (Dev, "configuration descriptor (%u bytes): 0x%08x (USBD 0x%08x, %lu bytes)", Len, Status,
            Dev->CmdUrb.UrbHeader.Status, Dev->CmdUrb.UrbControlDescriptorRequest.TransferBufferLength);
    ExFreePoolWithTag (D, FLT_POOL_TAG);
    return Status;
  }

  *Desc = D;
  return STATUS_SUCCESS;
}

/*
 * A start only (the device is unconfigured, BTHUSB has not started): selects configuration 1 with every interface in
 * alternate setting 0 and takes the interrupt IN pipe as the event pipe.
 */
static NTSTATUS
FltSelectOwnConfig (
  _In_ PFLT_DEVICE  Dev
  )
{
  PUSB_CONFIGURATION_DESCRIPTOR  Hdr;
  PUSB_CONFIGURATION_DESCRIPTOR  Cfg;
  PUSBD_INTERFACE_LIST_ENTRY     List;
  PUSB_INTERFACE_DESCRIPTOR      Ifd;
  PUSBD_INTERFACE_INFORMATION    If;
  PURB                           Sel;
  PUCHAR                         Pos;
  USHORT                         Total;
  ULONG                          NumIf;
  ULONG                          n;
  ULONG                          i;
  ULONG                          j;
  NTSTATUS                       Status;

  Cfg  = NULL;
  List = NULL;
  Sel  = NULL;
  Status = FltGetConfigDescriptor (Dev, sizeof (USB_CONFIGURATION_DESCRIPTOR), &Hdr);
  if (!NT_SUCCESS (Status)) {
    return Status;
  }

  Total = Hdr->wTotalLength;
  NumIf = Hdr->bNumInterfaces;
  ExFreePoolWithTag (Hdr, FLT_POOL_TAG);
  if ((Total < sizeof (USB_CONFIGURATION_DESCRIPTOR)) || (Total > 4096) || (NumIf == 0) || (NumIf > 32)) {
    FltLog (Dev, "configuration descriptor: unexpected total length %u / %lu interfaces", Total, NumIf);
    return STATUS_DEVICE_DATA_ERROR;
  }

  Status = FltGetConfigDescriptor (Dev, Total, &Cfg);
  if (!NT_SUCCESS (Status)) {
    return Status;
  }

  if ((Cfg->wTotalLength != Total) || (Cfg->bNumInterfaces != NumIf)) {
    FltLog (Dev, "configuration descriptor changed between the two reads");
    Status = STATUS_DEVICE_DATA_ERROR;
    goto Done;
  }

  List = ExAllocatePool2 (POOL_FLAG_NON_PAGED, (NumIf + 1) * sizeof (*List), FLT_POOL_TAG);
  if (List == NULL) {
    Status = STATUS_INSUFFICIENT_RESOURCES;
    goto Done;
  }

  // every interface in alternate setting 0, in descriptor order
  n   = 0;
  Pos = (PUCHAR)Cfg;
  while (n < NumIf) {
    Ifd = USBD_ParseConfigurationDescriptorEx (Cfg, Pos, -1, 0, -1, -1, -1);
    if ((Ifd == NULL) || (Ifd->bLength == 0)) {
      break;
    }

    List[n++].InterfaceDescriptor = Ifd;
    Pos = (PUCHAR)Ifd + Ifd->bLength;
  }

  if (n == 0) {
    FltLog (Dev, "configuration descriptor: no interface");
    Status = STATUS_DEVICE_DATA_ERROR;
    goto Done;
  }

  Sel = USBD_CreateConfigurationRequestEx (Cfg, List);
  if (Sel == NULL) {
    Status = STATUS_INSUFFICIENT_RESOURCES;
    goto Done;
  }

  Status = FltSubmitUrb (Dev, Sel, FLT_T_SELECT);
  if (!NT_SUCCESS (Status)) {
    FltLog (Dev, "SELECT_CONFIGURATION: 0x%08x (USBD 0x%08x)", Status, Sel->UrbHeader.Status);
    goto Done;
  }

  // the interrupt IN endpoint (a Bluetooth controller has one: HCI events)
  for (i = 0; i < n; i++) {
    If = List[i].Interface;
    for (j = 0; (If != NULL) && (j < If->NumberOfPipes); j++) {
      if ((If->Pipes[j].PipeType == UsbdPipeTypeInterrupt) && USB_ENDPOINT_DIRECTION_IN (If->Pipes[j].EndpointAddress)) {
        FltLog (Dev, "own configuration %u selected (%lu interfaces): event pipe = endpoint 0x%02x of interface %u, "
                "max packet %u", Cfg->bConfigurationValue, n, If->Pipes[j].EndpointAddress, If->InterfaceNumber,
                If->Pipes[j].MaximumPacketSize);
        InterlockedExchange (&Dev->EventInterface, If->InterfaceNumber);
        InterlockedExchangePointer ((PVOID volatile *)&Dev->EventPipe, If->Pipes[j].PipeHandle);
        goto Done;
      }
    }
  }

  FltLog (Dev, "own configuration selected, but no interrupt IN endpoint");
  Status = STATUS_DEVICE_DATA_ERROR;

Done:
  if (Sel != NULL) {
    ExFreePool (Sel);             // allocated by USBD_CreateConfigurationRequestEx
  }

  if (List != NULL) {
    ExFreePoolWithTag (List, FLT_POOL_TAG);
  }

  if (Cfg != NULL) {
    ExFreePoolWithTag (Cfg, FLT_POOL_TAG);
  }

  return Status;
}

/* ---- the event stream ---- */

/* Appends event bytes; a full buffer is emptied first (counted), whatever it held is lost. Lock held. */
static VOID
FltAppendLocked (
  _In_ PFLT_DEVICE                Dev,
  _In_reads_bytes_ (Len) PCUCHAR  Buf,
  _In_ ULONG                      Len
  )
{
  if (Len > FLT_ACC_SIZE - Dev->AccLen) {
    Dev->Overflows++;
    Dev->AccLen = 0;
    if (Len > FLT_ACC_SIZE) {
      return;
    }
  }

  RtlCopyMemory (Dev->Acc + Dev->AccLen, Buf, Len);
  Dev->AccLen += Len;
}

/*
 * Takes complete events off the front of the buffer until the Command Complete (1, return parameters in Ret) or
 * Command Status (2, *CmdStatus) for Opcode; other complete events are counted and dropped. 0: not (yet) there.
 * Lock held.
 */
static int
FltTakeEventLocked (
  _In_  PFLT_DEVICE              Dev,
  _In_  USHORT                   Opcode,
  _Out_writes_bytes_to_ (RetCap, *RetLen) PUCHAR  Ret,
  _In_  ULONG                    RetCap,
  _Out_ PULONG                   RetLen,
  _Out_ PUCHAR                   CmdStatus
  )
{
  ULONG   L;
  size_t  Off;
  size_t  RLen;
  int     R;

  *RetLen    = 0;
  *CmdStatus = 0;
  while (Dev->AccLen >= 2) {
    L = 2 + (ULONG)Dev->Acc[1];
    if (Dev->AccLen < L) {
      break;                      // the rest of this event is still to come
    }

    R = rtkbt_evt_match (Dev->Acc, L, Opcode, &Off, &RLen, CmdStatus);
    if (R == 1) {
      *RetLen = (ULONG)min (RLen, (size_t)RetCap);
      RtlCopyMemory (Ret, Dev->Acc + Off, *RetLen);
    } else if (R == 0) {
      Dev->Dropped++;
      Dev->LastDropped = Dev->Acc[0];
    }

    RtlMoveMemory (Dev->Acc, Dev->Acc + L, Dev->AccLen - L);
    Dev->AccLen -= L;
    if (R != 0) {
      return R;
    }
  }

  return 0;
}

/*
 * urb.c, completion of one of BTHUSB's event reads (<= DISPATCH_LEVEL): takes it if it is exactly one complete event
 * and the Command Complete / Status the session waits for, and nothing of ours is half received. TRUE = taken (the
 * caller gives BTHUSB's read back to the USB stack); FALSE = BTHUSB gets it as it is.
 */
BOOLEAN
FltOfferStolen (
  _In_ PFLT_DEVICE                Dev,
  _In_reads_bytes_ (Len) PCUCHAR  Buf,
  _In_ ULONG                      Len
  )
{
  KIRQL    Irql;
  BOOLEAN  Take;
  size_t   Off;
  size_t   RLen;
  UCHAR    St;

  if ((Len < 2) || (Len != 2 + (ULONG)Buf[1])) {
    return FALSE;
  }

  Take = FALSE;
  KeAcquireSpinLock (&Dev->Lock, &Irql);
  if (Dev->SessionOpen && (Dev->WaitOpcode != 0) && (Dev->AccLen == 0) &&
      (rtkbt_evt_match (Buf, Len, Dev->WaitOpcode, &Off, &RLen, &St) != 0)) {
    FltAppendLocked (Dev, Buf, Len);
    Take = TRUE;
  }

  KeReleaseSpinLock (&Dev->Lock, Irql);
  if (Take) {
    KeSetEvent (&Dev->DataEvent, IO_NO_INCREMENT, FALSE);
  }

  return Take;
}

/* (Re)sends our interrupt read. The request must be ours (not pending). <= DISPATCH_LEVEL. */
static BOOLEAN
FltReadSend (
  _In_ PFLT_DEVICE  Dev
  )
{
  WDF_REQUEST_REUSE_PARAMS  Reuse;
  WDF_REQUEST_SEND_OPTIONS  Opts;
  NTSTATUS                  Status;

  WDF_REQUEST_REUSE_PARAMS_INIT (&Reuse, WDF_REQUEST_REUSE_NO_FLAGS, STATUS_SUCCESS);
  Status = WdfRequestReuse (Dev->ReadRequest, &Reuse);
  if (!NT_SUCCESS (Status)) {
    return FALSE;
  }

  RtlZeroMemory (&Dev->ReadUrb, sizeof (Dev->ReadUrb));
  UsbBuildInterruptOrBulkTransferRequest (&Dev->ReadUrb, (USHORT)sizeof (struct _URB_BULK_OR_INTERRUPT_TRANSFER),
                                          Dev->ReadPipe, Dev->ReadBuf, NULL, FLT_READ_SIZE,
                                          USBD_TRANSFER_DIRECTION_IN | USBD_SHORT_TRANSFER_OK, NULL);
  Status = WdfIoTargetFormatRequestForInternalIoctlOthers (Dev->Target, Dev->ReadRequest, IOCTL_INTERNAL_USB_SUBMIT_URB,
                                                            Dev->ReadUrbMemory, NULL, NULL, NULL, NULL, NULL);
  if (!NT_SUCCESS (Status)) {
    return FALSE;
  }

  WdfRequestSetCompletionRoutine (Dev->ReadRequest, FltReadComplete, Dev);
  WDF_REQUEST_SEND_OPTIONS_INIT (&Opts, WDF_REQUEST_SEND_OPTION_IGNORE_TARGET_STATE);
  return WdfRequestSend (Dev->ReadRequest, Dev->Target, &Opts);
}

/* Our interrupt read completed (<= DISPATCH_LEVEL): keep the bytes, read again while the session wants events. */
VOID
FltReadComplete (
  _In_ WDFREQUEST                      Request,
  _In_ WDFIOTARGET                     Target,
  _In_ PWDF_REQUEST_COMPLETION_PARAMS  Params,
  _In_ WDFCONTEXT                      Context
  )
{
  PFLT_DEVICE  Dev;
  KIRQL        Irql;
  BOOLEAN      Again;
  ULONG        n;

  UNREFERENCED_PARAMETER (Request);
  UNREFERENCED_PARAMETER (Target);

  Dev = (PFLT_DEVICE)Context;
  KeAcquireSpinLock (&Dev->Lock, &Irql);
  Dev->ReadStatus     = Params->IoStatus.Status;
  Dev->ReadUsbdStatus = Dev->ReadUrb.UrbHeader.Status;
  Dev->Reads++;
  if (NT_SUCCESS (Params->IoStatus.Status)) {
    n = Dev->ReadUrb.UrbBulkOrInterruptTransfer.TransferBufferLength;
    FltAppendLocked (Dev, Dev->ReadBuf, min (n, (ULONG)FLT_READ_SIZE));
  }

  Again = NT_SUCCESS (Params->IoStatus.Status) && Dev->SessionOpen && !Dev->ReadStop;
  if (!Again) {
    Dev->ReadPending = FALSE;
  }

  KeReleaseSpinLock (&Dev->Lock, Irql);
  KeSetEvent (&Dev->DataEvent, IO_NO_INCREMENT, FALSE);
  if (Again && !FltReadSend (Dev)) {
    KeAcquireSpinLock (&Dev->Lock, &Irql);
    Dev->ReadPending = FALSE;
    KeReleaseSpinLock (&Dev->Lock, Irql);
    Again = FALSE;
  }

  if (!Again) {
    // the last thing done with the request: FltReadStop waits for it
    KeSetEvent (&Dev->ReadIdle, IO_NO_INCREMENT, FALSE);
  }
}

static BOOLEAN
FltReadStart (
  _In_ PFLT_DEVICE       Dev,
  _In_ USBD_PIPE_HANDLE  Pipe
  )
{
  KIRQL  Irql;

  KeAcquireSpinLock (&Dev->Lock, &Irql);
  Dev->ReadPipe       = Pipe;
  Dev->ReadStop       = FALSE;
  Dev->ReadPending    = TRUE;
  Dev->Reads          = 0;
  Dev->ReadStatus     = STATUS_SUCCESS;
  Dev->ReadUsbdStatus = USBD_STATUS_SUCCESS;
  KeClearEvent (&Dev->ReadIdle);
  KeReleaseSpinLock (&Dev->Lock, Irql);
  if (!FltReadSend (Dev)) {
    KeAcquireSpinLock (&Dev->Lock, &Irql);
    Dev->ReadPending = FALSE;
    KeReleaseSpinLock (&Dev->Lock, Irql);
    KeSetEvent (&Dev->ReadIdle, IO_NO_INCREMENT, FALSE);
    FltLog (Dev, "interrupt read not sent (0x%08x)", WdfRequestGetStatus (Dev->ReadRequest));
    return FALSE;
  }

  return TRUE;
}

/*
 * Stops our interrupt read: no more resubmits, cancel what is pending (again, in case a resubmit raced with the
 * cancel), and wait for the completion routine to be done with it. If it does not come back within FLT_T_READ_STOP
 * the request stays with the USB stack (it and its buffer live in the device context) and no session runs again on
 * this device object.
 */
static VOID
FltReadStop (
  _In_ PFLT_DEVICE  Dev
  )
{
  LARGE_INTEGER  Wait;
  KIRQL          Irql;
  BOOLEAN        Pending;
  ULONG          Waited;

  Wait.QuadPart = -10000LL * 200;
  for (Waited = 0; ; Waited += 200) {
    KeAcquireSpinLock (&Dev->Lock, &Irql);
    Dev->ReadStop = TRUE;
    Pending       = Dev->ReadPending;
    KeReleaseSpinLock (&Dev->Lock, Irql);
    if (Pending) {
      (VOID)WdfRequestCancelSentRequest (Dev->ReadRequest);
    }

    if (KeWaitForSingleObject (&Dev->ReadIdle, Executive, KernelMode, FALSE, &Wait) == STATUS_SUCCESS) {
      return;
    }

    if (Waited >= FLT_T_READ_STOP) {
      Dev->ReadStuck = TRUE;
      FltLog (Dev, "our interrupt read did not come back after its cancel: no more sessions on this device object");
      return;
    }
  }
}

/* ---- HCI ---- */

/*
 * Sends one command and waits up to TimeoutMs for its Command Complete; Ret gets the return parameters (status
 * first). A Command Status instead, a failed transfer or no event in time is a failure (logged).
 */
static NTSTATUS
FltHciCmd (
  _In_ PFLT_DEVICE                       Dev,
  _In_ USHORT                            Opcode,
  _In_reads_bytes_opt_ (PLen) PCUCHAR    Params,
  _In_ UCHAR                             PLen,
  _Out_writes_bytes_to_ (RetCap, *RetLen) PUCHAR  Ret,
  _In_ ULONG                             RetCap,
  _Out_ PULONG                           RetLen,
  _In_ ULONG                             TimeoutMs
  )
{
  LARGE_INTEGER  Wait;
  ULONG64        Deadline;
  ULONG64        Now;
  KIRQL          Irql;
  UCHAR          CmdStatus;
  int            R;
  NTSTATUS       Status;

  *RetLen        = 0;
  Dev->CmdBuf[0] = (UCHAR)(Opcode & 0xff);
  Dev->CmdBuf[1] = (UCHAR)(Opcode >> 8);
  Dev->CmdBuf[2] = PLen;
  if ((Params != NULL) && (PLen > 0)) {
    RtlCopyMemory (Dev->CmdBuf + 3, Params, PLen);
  }

  // from now on an event for this opcode is ours, also when BTHUSB's pending read gets it
  KeAcquireSpinLock (&Dev->Lock, &Irql);
  Dev->WaitOpcode = Opcode;
  KeReleaseSpinLock (&Dev->Lock, Irql);

  RtlZeroMemory (&Dev->CmdUrb, sizeof (Dev->CmdUrb));
  UsbBuildVendorRequest (&Dev->CmdUrb, URB_FUNCTION_CLASS_DEVICE,
                         (USHORT)sizeof (struct _URB_CONTROL_VENDOR_OR_CLASS_REQUEST), USBD_TRANSFER_DIRECTION_OUT, 0,
                         0, 0, 0, Dev->CmdBuf, NULL, 3u + PLen, NULL);
  Status = FltSubmitUrb (Dev, &Dev->CmdUrb, FLT_T_SEND);
  if (!NT_SUCCESS (Status)) {
    FltLog (Dev, "command 0x%04x: control transfer failed 0x%08x (USBD 0x%08x)", Opcode, Status,
            Dev->CmdUrb.UrbHeader.Status);
    goto Done;
  }

  Deadline = KeQueryInterruptTime () + (ULONG64)TimeoutMs * 10000;
  for (;;) {
    KeAcquireSpinLock (&Dev->Lock, &Irql);
    R = FltTakeEventLocked (Dev, Opcode, Ret, RetCap, RetLen, &CmdStatus);
    KeReleaseSpinLock (&Dev->Lock, Irql);
    if (R == 1) {
      Status = STATUS_SUCCESS;
      break;
    }

    if (R == 2) {
      FltLog (Dev, "command 0x%04x: Command Status 0x%02x instead of a Command Complete", Opcode, CmdStatus);
      Status = STATUS_NOT_SUPPORTED;
      break;
    }

    Now = KeQueryInterruptTime ();
    if (Now >= Deadline) {
      FltLog (Dev, "command 0x%04x: no Command Complete within %lu ms (our read: %lu completions, last 0x%08x, "
              "USBD 0x%08x)", Opcode, TimeoutMs, Dev->Reads, Dev->ReadStatus, Dev->ReadUsbdStatus);
      Status = STATUS_IO_TIMEOUT;
      break;
    }

    Wait.QuadPart = -(LONGLONG)(Deadline - Now);
    (VOID)KeWaitForSingleObject (&Dev->DataEvent, Executive, KernelMode, FALSE, &Wait);
  }

Done:
  KeAcquireSpinLock (&Dev->Lock, &Irql);
  Dev->WaitOpcode = 0;
  KeReleaseSpinLock (&Dev->Lock, Irql);
  return Status;
}

/* HCI_Read_Local_Version_Information (a standard command: the filter is not limited to OGF 0x3F as bthport's
   vendor IOCTL is). */
static NTSTATUS
FltReadVersion (
  _In_  PFLT_DEVICE        Dev,
  _Out_ FLT_VERSION_INFO   *V,
  _In_  ULONG              TimeoutMs
  )
{
  UCHAR     R[16];
  ULONG     Len;
  NTSTATUS  Status;

  RtlZeroMemory (V, sizeof (*V));
  Status = FltHciCmd (Dev, RTKBT_OP_READ_LOCAL_VER, NULL, 0, R, sizeof (R), &Len, TimeoutMs);
  if (!NT_SUCCESS (Status)) {
    return Status;
  }

  if ((Len < 9) || (R[0] != 0)) {
    FltLog (Dev, "HCI_Read_Local_Version: status 0x%02x, %lu return bytes", (Len > 0) ? R[0] : 0xff, Len);
    return STATUS_DEVICE_PROTOCOL_ERROR;
  }

  V->HciVer    = R[1];
  V->HciRev    = (USHORT)(R[2] | (R[3] << 8));
  V->LmpVer    = R[4];
  V->Mfg       = (USHORT)(R[5] | (R[6] << 8));
  V->LmpSubver = (USHORT)(R[7] | (R[8] << 8));
  FltLog (Dev, "HCI_Read_Local_Version: hci_ver 0x%02x hci_rev 0x%04x lmp_ver 0x%02x manufacturer 0x%04x "
          "lmp_subver 0x%04x", V->HciVer, V->HciRev, V->LmpVer, V->Mfg, V->LmpSubver);
  return STATUS_SUCCESS;
}

/* A vendor read with a fixed answer: status 0 and ExpectLen return bytes; *Value = the byte after the status. */
static NTSTATUS
FltVendorRead (
  _In_  PFLT_DEVICE                     Dev,
  _In_  USHORT                          Opcode,
  _In_reads_bytes_opt_ (PLen) PCUCHAR   Params,
  _In_  UCHAR                           PLen,
  _In_  ULONG                           ExpectLen,
  _Out_ PUCHAR                          Value
  )
{
  UCHAR     R[16];
  ULONG     Len;
  NTSTATUS  Status;

  *Value = 0;
  Status = FltHciCmd (Dev, Opcode, Params, PLen, R, sizeof (R), &Len, FLT_T_EVT);
  if (!NT_SUCCESS (Status)) {
    return Status;
  }

  if ((Len != ExpectLen) || (R[0] != 0)) {
    FltLog (Dev, "command 0x%04x: status 0x%02x, %lu return bytes (expected status 0 and %lu)", Opcode,
            (Len > 0) ? R[0] : 0xff, Len, ExpectLen);
    return STATUS_DEVICE_PROTOCOL_ERROR;
  }

  *Value = R[1];
  return STATUS_SUCCESS;
}

/*
 * The version check and, on the ROM, the download. *Downloaded: fragments were sent. *Rev / *Subver: the controller's
 * version at the end (0 = unknown).
 */
static FLT_RESULT
FltCheckAndDownload (
  _In_  PFLT_DEVICE  Dev,
  _Out_ PBOOLEAN     Downloaded,
  _Out_ PUSHORT      Rev,
  _Out_ PUSHORT      Subver
  )
{
  FLT_VERSION_INFO  V;
  CHAR              Err[96];
  UCHAR             Param[1 + RTKBT_FRAG_LEN];
  UCHAR             R[8];
  ULONG             RLen;
  UCHAR             Rom;
  UCHAR             KeyId;
  PUCHAR            Img;
  size_t            ImgLen;
  size_t            Count;
  size_t            i;
  size_t            Off;
  size_t            FLen;
  SIZE_T            CfgLen;
  UCHAR             Idx;
  int               Sel;
  BOOLEAN           LastTried;
  BOOLEAN           Failed;
  ULONG64           T0;
  FLT_RESULT        Result;
  NTSTATUS          Status;

  *Downloaded = FALSE;
  *Rev        = 0;
  *Subver     = 0;

  // 1. what runs now
  if (!NT_SUCCESS (FltReadVersion (Dev, &V, FLT_T_FIRST_EVT))) {
    FltLog (Dev, "no version: pass-through");
    return FltResultNoVersion;
  }

  *Rev    = V.HciRev;
  *Subver = V.LmpSubver;
  if (V.Mfg != RTKBT_MFG_REALTEK) {
    FltLog (Dev, "not a Realtek controller: pass-through");
    return FltResultNotRom;
  }

  if (!rtkbt_is_rom_8852b (V.LmpSubver, V.HciRev, V.HciVer)) {
    if (V.LmpSubver == RTKBT_LMP_SUBVER_8852) {
      FltLog (Dev, "subversion 0x8852 but not the 8852B USB's ROM (hci_rev 0x000b, hci_ver 0x0b): pass-through");
      return FltResultNotRom;
    }

    FltLog (Dev, "the controller runs a patch already (fw version 0x%04x%04x): nothing to do", V.HciRev, V.LmpSubver);
    return FltResultAlreadyPatched;
  }

  // 2. which patch, and whether the config goes with it
  if (!NT_SUCCESS (FltVendorRead (Dev, RTKBT_OP_READ_ROM_VER, NULL, 0, 2, &Rom))) {
    FltLog (Dev, "no ROM version: pass-through (the controller runs its ROM)");
    return FltResultFailedRom;
  }

  if (!NT_SUCCESS (FltVendorRead (Dev, RTKBT_OP_READ_REG16, mRegSecProj, sizeof (mRegSecProj), 3, &KeyId))) {
    FltLog (Dev, "no key id: pass-through (the controller runs its ROM)");
    return FltResultFailedRom;
  }

  CfgLen = (KeyId == 0) ? Dev->CfgLen : 0;
  ImgLen = rtkbt_epatch_image_len (Dev->Fw, Dev->FwLen, &Dev->Ep, Rom, CfgLen, &Sel, Err, sizeof (Err));
  if (ImgLen == 0) {
    FltLog (Dev, "ROM version %u: %s; pass-through", Rom, Err);
    return FltResultFailedRom;
  }

  Img = ExAllocatePool2 (POOL_FLAG_NON_PAGED, ImgLen, FLT_POOL_TAG);
  if (Img == NULL) {
    FltLog (Dev, "no memory for the image (%lu bytes): pass-through", (ULONG)ImgLen);
    return FltResultFailedRom;
  }

  if (rtkbt_epatch_build_into (Dev->Fw, Dev->FwLen, &Dev->Ep, Rom, Dev->Cfg, CfgLen, Img, ImgLen, &ImgLen, &Sel, Err,
                               sizeof (Err)) != 0) {
    FltLog (Dev, "image: %s; pass-through", Err);
    ExFreePoolWithTag (Img, FLT_POOL_TAG);
    return FltResultFailedRom;
  }

  Count = rtkbt_frag_count (ImgLen);
  FltLog (Dev, "ROM version %u, key id %u: patch %d (chip id %u, %u bytes) + config %lu bytes = %lu bytes, %lu "
          "fragments", Rom, KeyId, Sel, Dev->Ep.chip_id[Sel], Dev->Ep.length[Sel], (ULONG)CfgLen, (ULONG)ImgLen,
          (ULONG)Count);

  // 3. the download; the patch is taken only with the last fragment (index bit 7)
  FltProgress (Dev, "downloading", TRUE);
  *Downloaded = TRUE;
  LastTried   = FALSE;
  Failed      = FALSE;
  T0          = KeQueryInterruptTime ();
  for (i = 0; i < Count; i++) {
    rtkbt_frag_get (ImgLen, i, &Idx, &Off, &FLen);
    if (i + 1 == Count) {
      LastTried = TRUE;
    } else if (KeQueryInterruptTime () - T0 > (ULONG64)FLT_T_DOWNLOAD * 10000) {
      FltLog (Dev, "download: over %u ms at fragment %lu: stopped", FLT_T_DOWNLOAD, (ULONG)i);
      Failed = TRUE;
      break;
    }

    Param[0] = Idx;
    RtlCopyMemory (Param + 1, Img + Off, FLen);
    Status = FltHciCmd (Dev, RTKBT_OP_DOWNLOAD, Param, (UCHAR)(1 + FLen), R, sizeof (R), &RLen, FLT_T_EVT);
    if (!NT_SUCCESS (Status) || (RLen < 1) || (R[0] != 0)) {
      FltLog (Dev, "download: fragment %lu (index 0x%02x) failed (0x%08x, status 0x%02x)", (ULONG)i, Idx, Status,
              (RLen >= 1) ? R[0] : 0xff);
      Failed = TRUE;
      break;
    }
  }

  ExFreePoolWithTag (Img, FLT_POOL_TAG);
  if (!Failed) {
    FltLog (Dev, "download: %lu fragments in %llu ms", (ULONG)Count, (KeQueryInterruptTime () - T0) / 10000);
  } else if (!LastTried) {
    FltLog (Dev, "the last fragment was not sent: the controller still runs its ROM; pass-through");
    return FltResultFailedRom;
  }

  // 4. what runs now (btrtl prints "fw version 0x<hci_rev><lmp_subver>" here)
  FltProgress (Dev, "download done, reading the version", FALSE);
  if (!NT_SUCCESS (FltReadVersion (Dev, &V, FLT_T_EVT))) {
    *Rev    = 0;
    *Subver = 0;
    if (Failed) {
      FltLog (Dev, "the last fragment went out without a good completion and no version can be read: state unknown");
      return FltResultUnknown;
    }

    FltLog (Dev, "downloaded, but no version afterwards: taken as patched, not confirmed");
    return FltResultPatched;
  }

  *Rev    = V.HciRev;
  *Subver = V.LmpSubver;
  if (V.LmpSubver == RTKBT_LMP_SUBVER_8852) {
    FltLog (Dev, "the controller still reports its ROM: the patch was not taken; pass-through");
    return FltResultFailedRom;
  }

  FltLog (Dev, "patched: fw version 0x%04x%04x%s", V.HciRev, V.LmpSubver,
          Failed ? " (the last fragment's completion was missing or bad)" : "");
  Result = FltResultPatched;
  return Result;
}

/*
 * One session (D0Entry from D3 or D3Final, normal mode). The guard records it before the patch file is parsed and
 * anything goes to the device; when the session changed the device, the guard stays set while BTHUSB starts on it.
 * Start: the first D0 of a start (we configure the device ourselves); otherwise a D3 -> D0 with BTHUSB's configuration.
 */
VOID
FltSession (
  _In_ PFLT_DEVICE  Dev,
  _In_ BOOLEAN      Start
  )
{
  USBD_PIPE_HANDLE  Pipe;
  FLT_RESULT        Result;
  BOOLEAN           Downloaded;
  BOOLEAN           OwnConfig;
  BOOLEAN           Held;
  USHORT            Rev;
  USHORT            Subver;
  KIRQL             Irql;
  NTSTATUS          Status;

  // the hold of the last session ends here at the latest (its D0 exit to D3 normally ended it already)
  FltGuardRelease (Dev, "next session");
  Downloaded = FALSE;
  OwnConfig  = FALSE;
  Rev        = 0;
  Subver     = 0;
  FltLog (Dev, "D1/D2 resumes since the last session: %lu; shutdown notification %s", Dev->D2Resumes,
          Dev->ShutdownHook ? "on" : "off");
  Dev->D2Resumes = 0;
  if (Dev->ReadStuck) {
    FltLog (Dev, "an earlier interrupt read never came back: pass-through");
    FltSaveSession (Dev, FltResultPassThrough, FALSE, 0, 0, FALSE);
    return;
  }

  if (!Start && (Dev->EventPipe == NULL)) {
    FltLog (Dev, "no event pipe known (BTHUSB has not configured the device): pass-through");
    FltSaveSession (Dev, FltResultUsbError, FALSE, 0, 0, FALSE);
    return;
  }

  Status = FltGuardBegin (Dev, Start ? FLT_T_HIVE_START : FLT_T_HIVE_RESUME);
  if (Status == STATUS_CANCELLED) {
    // pass-through from now on (the hooks stay, without a session they only count)
    Dev->Mode = FltModeGuard;
    (VOID)RtlStringCbCopyA (Dev->ModeNote, sizeof (Dev->ModeNote), "pass-through: guard tripped, rtkbtflt.ps1 clear");
    FltSavePassThrough (Dev);
    return;
  }

  if (!NT_SUCCESS (Status)) {
    FltSaveSession (Dev, FltResultHive, FALSE, 0, 0, FALSE);
    return;
  }

  // under the guard from here on, the parsing of the patch file included
  if (!Dev->FwTried) {
    FltLoadFirmware (Dev);
  }

  FltLog (Dev, "%s", Dev->FwOk ? Dev->FwNote : "no usable patch file");
  if (!Dev->FwOk) {
    FltLog (Dev, "%s: pass-through", Dev->FwNote);
    FltGuardEnd (Dev);
    FltSaveSession (Dev, FltResultNoFirmware, FALSE, 0, 0, FALSE);
    return;
  }

  FltProgress (Dev, Start ? "selecting our configuration" : "reading the version", TRUE);

  // the session opens: events for our commands are collected from here on
  KeAcquireSpinLock (&Dev->Lock, &Irql);
  Dev->SessionOpen = TRUE;
  Dev->WaitOpcode  = 0;
  Dev->AccLen      = 0;
  Dev->Dropped     = 0;
  Dev->LastDropped = 0;
  Dev->Overflows   = 0;
  KeReleaseSpinLock (&Dev->Lock, Irql);
  KeClearEvent (&Dev->DataEvent);
  InterlockedExchange (&Dev->Stolen, 0);

  Result = FltResultUsbError;
  if (Start) {
    OwnConfig = TRUE;             // the device may have changed even if this fails
    Status    = FltSelectOwnConfig (Dev);
    if (!NT_SUCCESS (Status)) {
      FltLog (Dev, "own configuration failed (0x%08x): pass-through", Status);
      goto Close;
    }
  }

  Pipe = (USBD_PIPE_HANDLE)InterlockedCompareExchangePointer ((PVOID volatile *)&Dev->EventPipe, NULL, NULL);
  FltLog (Dev, "event pipe %p (%s); BTHUSB event reads pending below: %ld", Pipe, Start ? "ours" : "BTHUSB's",
          Dev->EventReadsBelow);
  if ((Pipe == NULL) || !FltReadStart (Dev, Pipe)) {
    goto Close;
  }

  Result = FltCheckAndDownload (Dev, &Downloaded, &Rev, &Subver);
  FltReadStop (Dev);

Close:
  KeAcquireSpinLock (&Dev->Lock, &Irql);
  Dev->SessionOpen = FALSE;
  Dev->WaitOpcode  = 0;
  KeReleaseSpinLock (&Dev->Lock, Irql);
  FltLog (Dev, "events: our read %lu completions (last 0x%08x, USBD 0x%08x), %lu taken from BTHUSB's reads, %lu "
          "others dropped (last code 0x%02x), %lu overflows", Dev->Reads, Dev->ReadStatus, Dev->ReadUsbdStatus,
          (ULONG)Dev->Stolen, Dev->Dropped, Dev->LastDropped, Dev->Overflows);

  // a changed device: the guard stays set while BTHUSB / bthport start on it (driver.c)
  Held = Downloaded || OwnConfig;
  if (Held) {
    FltGuardHold (Dev);
  } else {
    FltGuardEnd (Dev);
  }

  FltSaveSession (Dev, Result, Downloaded, Rev, Subver, Held);
}
