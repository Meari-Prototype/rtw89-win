// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Meari
/*
 * rtkbtflt: what BTHUSB sends down (IRP_MJ_INTERNAL_DEVICE_CONTROL), as a WDM preprocess routine.
 *
 * Everything goes down untouched (IoSkipCurrentIrpStackLocation) except three kinds of URB, which get a completion
 * routine and nothing else changed:
 *  - SELECT_CONFIGURATION / SELECT_INTERFACE: the interrupt IN pipe (HCI events) BTHUSB ends up with is noted; a D3 ->
 *    D0 session reads events from it.
 *  - BULK_OR_INTERRUPT_TRANSFER on that pipe (BTHUSB's event reads): counted while they are below us. If one of them
 *    completes during a session with exactly the Command Complete / Status of the command the session waits for
 *    (FltOfferStolen), the event is the session's: the same IRP is given back to the USB stack with its URB as BTHUSB
 *    built it, and BTHUSB never sees that event. Any other completion goes up as it is.
 * The routines run at <= DISPATCH_LEVEL and touch only non-paged memory. The dispatch side marks a hooked event read
 * pending and returns STATUS_PENDING, since its completion routine may send the IRP down again.
 */
#include "rtkbtflt.h"

/* One of BTHUSB's event reads below us: the URB fields to restore before giving it back to the USB stack. */
typedef struct _FLT_READ_CTX {
  PFLT_DEVICE       Dev;
  USBD_PIPE_HANDLE  Pipe;
  PVOID             Buffer;
  PMDL              Mdl;
  ULONG             Length;
  ULONG             Flags;
  ULONG             UsbdFlags;
} FLT_READ_CTX, *PFLT_READ_CTX;

// an interface's information without its pipes
#define FLT_IF_HDR  ((ULONG)FIELD_OFFSET (USBD_INTERFACE_INFORMATION, Pipes))

static IO_COMPLETION_ROUTINE  FltSelectComplete;
static IO_COMPLETION_ROUTINE  FltEventReadComplete;

/* The interrupt IN pipe of one interface's information, or NULL. Len: the bytes of it that lie inside the URB. */
static USBD_PIPE_HANDLE
FltInterruptInPipe (
  _In_ PUSBD_INTERFACE_INFORMATION  If,
  _In_ ULONG                        Len
  )
{
  ULONG  Max;
  ULONG  n;
  ULONG  j;

  if ((Len < FLT_IF_HDR) || (If->Length < FLT_IF_HDR) || (If->Length > Len)) {
    return NULL;
  }

  Max = (If->Length - FLT_IF_HDR) / sizeof (USBD_PIPE_INFORMATION);
  n   = min (If->NumberOfPipes, Max);
  for (j = 0; j < n; j++) {
    if ((If->Pipes[j].PipeType == UsbdPipeTypeInterrupt) && USB_ENDPOINT_DIRECTION_IN (If->Pipes[j].EndpointAddress)) {
      return If->Pipes[j].PipeHandle;
    }
  }

  return NULL;
}

/* BTHUSB's SELECT_CONFIGURATION / SELECT_INTERFACE completed: note its event pipe. */
static NTSTATUS
FltSelectComplete (
  _In_     PDEVICE_OBJECT  DeviceObject,
  _In_     PIRP            Irp,
  _In_opt_ PVOID           Context
  )
{
  PFLT_DEVICE                  Dev;
  PURB                         Urb;
  PUSBD_INTERFACE_INFORMATION  If;
  PUCHAR                       End;
  USBD_PIPE_HANDLE             Pipe;

  UNREFERENCED_PARAMETER (DeviceObject);

  if (Irp->PendingReturned) {
    IoMarkIrpPending (Irp);
  }

  Dev = (PFLT_DEVICE)Context;
  Urb = (PURB)IoGetCurrentIrpStackLocation (Irp)->Parameters.Others.Argument1;
  if ((Dev == NULL) || !NT_SUCCESS (Irp->IoStatus.Status) || !USBD_SUCCESS (Urb->UrbHeader.Status)) {
    return STATUS_CONTINUE_COMPLETION;
  }

  End = (PUCHAR)Urb + Urb->UrbHeader.Length;
  if (Urb->UrbHeader.Function == URB_FUNCTION_SELECT_CONFIGURATION) {
    if (Urb->UrbSelectConfiguration.ConfigurationDescriptor == NULL) {
      return STATUS_CONTINUE_COMPLETION;              // unconfigured: the pipe was cleared on the way down
    }

    InterlockedIncrement (&Dev->BthSelects);
    for (If = &Urb->UrbSelectConfiguration.Interface;
         ((PUCHAR)If < End) && ((ULONG)(End - (PUCHAR)If) >= FLT_IF_HDR) && (If->Length >= FLT_IF_HDR);
         If = (PUSBD_INTERFACE_INFORMATION)((PUCHAR)If + If->Length)) {
      Pipe = FltInterruptInPipe (If, (ULONG)(End - (PUCHAR)If));
      if (Pipe != NULL) {
        InterlockedExchange (&Dev->EventInterface, If->InterfaceNumber);
        InterlockedExchangePointer ((PVOID volatile *)&Dev->EventPipe, Pipe);
        break;
      }
    }
  } else {
    If = &Urb->UrbSelectInterface.Interface;
    if (((PUCHAR)If < End) && ((ULONG)(End - (PUCHAR)If) >= FLT_IF_HDR) &&
        (If->InterfaceNumber == (UCHAR)Dev->EventInterface)) {
      InterlockedExchangePointer ((PVOID volatile *)&Dev->EventPipe, FltInterruptInPipe (If, (ULONG)(End - (PUCHAR)If)));
    }
  }

  return STATUS_CONTINUE_COMPLETION;
}

/*
 * One of BTHUSB's event reads completed. The dispatch side returned STATUS_PENDING already, so nothing is marked here.
 */
static NTSTATUS
FltEventReadComplete (
  _In_     PDEVICE_OBJECT  DeviceObject,
  _In_     PIRP            Irp,
  _In_opt_ PVOID           Context
  )
{
  PFLT_READ_CTX  C;
  PFLT_DEVICE    Dev;
  PURB           Urb;
  PUCHAR         Buf;
  ULONG          Len;

  UNREFERENCED_PARAMETER (DeviceObject);

  C   = (PFLT_READ_CTX)Context;
  Dev = C->Dev;
  InterlockedDecrement (&Dev->EventReadsBelow);
  if (NT_SUCCESS (Irp->IoStatus.Status) && !Dev->SessionOpen) {
    InterlockedIncrement (&Dev->BthEvents);         // BTHUSB at work (the guard's release log)
  }

  if (NT_SUCCESS (Irp->IoStatus.Status) && Dev->SessionOpen) {
    Urb = (PURB)IoGetCurrentIrpStackLocation (Irp)->Parameters.Others.Argument1;
    Len = Urb->UrbBulkOrInterruptTransfer.TransferBufferLength;
    Buf = (C->Mdl != NULL) ? (PUCHAR)MmGetSystemAddressForMdlSafe (C->Mdl, NormalPagePriority | MdlMappingNoExecute)
                           : (PUCHAR)C->Buffer;
    if ((Buf != NULL) && (Len <= C->Length) && FltOfferStolen (Dev, Buf, Len)) {
      // the session's event: BTHUSB's read goes back to the USB stack as BTHUSB built it
      UsbBuildInterruptOrBulkTransferRequest (Urb, Urb->UrbHeader.Length, C->Pipe, C->Buffer, C->Mdl, C->Length,
                                              C->Flags, NULL);
      Urb->UrbHeader.UsbdFlags = C->UsbdFlags;
      InterlockedIncrement (&Dev->Stolen);
      InterlockedIncrement (&Dev->EventReadsBelow);
      IoCopyCurrentIrpStackLocationToNext (Irp);
      IoSetCompletionRoutine (Irp, FltEventReadComplete, C, TRUE, TRUE, TRUE);
      (VOID)IoCallDriver (Dev->Lower, Irp);
      return STATUS_MORE_PROCESSING_REQUIRED;
    }
  }

  ExFreePoolWithTag (C, FLT_POOL_TAG);
  return STATUS_CONTINUE_COMPLETION;
}

NTSTATUS
FltEvtWdmInternalIoctl (
  _In_    WDFDEVICE  Device,
  _Inout_ PIRP       Irp
  )
{
  PFLT_DEVICE         Dev;
  PIO_STACK_LOCATION  Sl;
  PURB                Urb;
  PFLT_READ_CTX       C;
  USBD_PIPE_HANDLE    Pipe;

  Dev = FltGetDevice (Device);
  Sl  = IoGetCurrentIrpStackLocation (Irp);
  Urb = (Sl->Parameters.DeviceIoControl.IoControlCode == IOCTL_INTERNAL_USB_SUBMIT_URB) ?
        (PURB)Sl->Parameters.Others.Argument1 : NULL;
  if (Urb != NULL) {
    switch (Urb->UrbHeader.Function) {
      case URB_FUNCTION_SELECT_CONFIGURATION:
        // the old pipes go away with it, whatever the result
        InterlockedExchangePointer ((PVOID volatile *)&Dev->EventPipe, NULL);
        IoCopyCurrentIrpStackLocationToNext (Irp);
        IoSetCompletionRoutine (Irp, FltSelectComplete, Dev, TRUE, TRUE, TRUE);
        return IoCallDriver (Dev->Lower, Irp);

      case URB_FUNCTION_SELECT_INTERFACE:
        if (Urb->UrbSelectInterface.Interface.InterfaceNumber == (UCHAR)Dev->EventInterface) {
          InterlockedExchangePointer ((PVOID volatile *)&Dev->EventPipe, NULL);
        }

        IoCopyCurrentIrpStackLocationToNext (Irp);
        IoSetCompletionRoutine (Irp, FltSelectComplete, Dev, TRUE, TRUE, TRUE);
        return IoCallDriver (Dev->Lower, Irp);

      case URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER:
        Pipe = Dev->EventPipe;
        if ((Pipe == NULL) || (Urb->UrbBulkOrInterruptTransfer.PipeHandle != Pipe) ||
            ((Urb->UrbBulkOrInterruptTransfer.TransferFlags & USBD_TRANSFER_DIRECTION_IN) == 0)) {
          break;
        }

        C = ExAllocatePool2 (POOL_FLAG_NON_PAGED, sizeof (*C), FLT_POOL_TAG);
        if (C == NULL) {
          break;                                      // not hooked: only a session could miss it
        }

        C->Dev       = Dev;
        C->Pipe      = Urb->UrbBulkOrInterruptTransfer.PipeHandle;
        C->Buffer    = Urb->UrbBulkOrInterruptTransfer.TransferBuffer;
        C->Mdl       = Urb->UrbBulkOrInterruptTransfer.TransferBufferMDL;
        C->Length    = Urb->UrbBulkOrInterruptTransfer.TransferBufferLength;
        C->Flags     = Urb->UrbBulkOrInterruptTransfer.TransferFlags;
        C->UsbdFlags = Urb->UrbHeader.UsbdFlags;
        InterlockedIncrement (&Dev->EventReadsBelow);
        IoMarkIrpPending (Irp);
        IoCopyCurrentIrpStackLocationToNext (Irp);
        IoSetCompletionRoutine (Irp, FltEventReadComplete, C, TRUE, TRUE, TRUE);
        (VOID)IoCallDriver (Dev->Lower, Irp);
        return STATUS_PENDING;

      default:
        break;
    }
  }

  IoSkipCurrentIrpStackLocation (Irp);
  return IoCallDriver (Dev->Lower, Irp);
}
