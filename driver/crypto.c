// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Meari
/*
 * rtw89win: AES-128 for software-protected management frames, the rtwk_aes_* primitives of
 * compat\include\rtw\os.h on the kernel's CNG (ksecdd.sys): AES-CCM for CCMP (13-byte nonce, 8-byte MIC) and
 * AES-CMAC for BIP-CMAC-128. PASSIVE_LEVEL only: the providers are opened without BCRYPT_PROV_DISPATCH; the mac
 * layer calls these with wiphy->mtx held, a KMUTEX, so still at PASSIVE_LEVEL. A key object is made for every call
 * (management frames are few) and destroyed before it returns, so no key outlives the frame it was used for.
 * RtwCryptoInit (DriverEntry) opens the providers and runs known-answer tests: RFC 4493 AES-CMAC examples 1-4 and
 * the CCMP test vector of IEEE 802.11-2020 J.6.4 (encryption, decryption in place, a changed MIC refused). Only
 * pass/fail goes to the log; rtwk_crypto_ready() is 1 only if every test passed, and the mac layer offers MFP and
 * SAE only then (mac\pmf.c).
 * tools-dev\crypto-test.ps1 builds this file as a user-mode program against bcrypt.dll (RTW_CRYPTO_USER) and runs
 * the same tests there.
 */
#ifdef RTW_CRYPTO_USER
#include <windows.h>
#include <bcrypt.h>
#include <stdio.h>
#define RtwLog(...)             ((void)printf (__VA_ARGS__), (void)printf ("\n"))
#define RtwAtPassive()          TRUE
#else
#include "rtw89win.h"
#include <bcrypt.h>
#define RtwAtPassive()          (KeGetCurrentIrql () == PASSIVE_LEVEL)
#endif
#include "../compat/include/rtw/os.h"

#ifndef STATUS_AUTH_TAG_MISMATCH
#define STATUS_AUTH_TAG_MISMATCH  ((NTSTATUS)0xC000A002L)
#endif

// Linux errno values
#define RTW_EIO       5
#define RTW_ENODEV    19
#define RTW_EINVAL    22
#define RTW_EBADMSG   74

#define RTW_AES_KEY   16
#define RTW_CCM_NONCE 13
#define RTW_CCM_MIC   8

static BCRYPT_ALG_HANDLE  mCcm;       // AES, chaining mode CCM
static BCRYPT_ALG_HANDLE  mCmac;      // AES-CMAC
static BOOLEAN            mOpen;
static BOOLEAN            mReady;     // the known-answer tests passed

int
rtwk_crypto_ready (
  void
  )
{
  return mReady;
}

static int
RtwCcm (
  BOOLEAN        Encrypt,
  const uint8_t  *Key,
  const uint8_t  *Nonce,
  const uint8_t  *Aad,
  uint32_t       AadLen,
  uint8_t        *Data,
  uint32_t       Len,
  uint8_t        *Mic
  )
{
  BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO  Info;
  BCRYPT_KEY_HANDLE                      Handle;
  NTSTATUS                               Status;
  ULONG                                  Done;
  UCHAR                                  Empty;

  if (!mOpen || !RtwAtPassive ()) {
    return -RTW_ENODEV;
  }

  if ((Data == NULL) && (Len != 0)) {
    return -RTW_EINVAL;
  }

  Status = BCryptGenerateSymmetricKey (mCcm, &Handle, NULL, 0, (PUCHAR)Key, RTW_AES_KEY, 0);
  if (!BCRYPT_SUCCESS (Status)) {
    return -RTW_EIO;
  }

  BCRYPT_INIT_AUTH_MODE_INFO (Info);
  Info.pbNonce    = (PUCHAR)Nonce;
  Info.cbNonce    = RTW_CCM_NONCE;
  Info.pbAuthData = (PUCHAR)Aad;
  Info.cbAuthData = AadLen;
  Info.pbTag      = Mic;
  Info.cbTag      = RTW_CCM_MIC;
  Done            = 0;
  if (Data == NULL) {
    Data = &Empty;            // CNG wants a buffer even for no data
  }

  // in place: the output buffer is the input buffer
  if (Encrypt) {
    Status = BCryptEncrypt (Handle, Data, Len, &Info, NULL, 0, Data, Len, &Done, 0);
  } else {
    Status = BCryptDecrypt (Handle, Data, Len, &Info, NULL, 0, Data, Len, &Done, 0);
  }

  BCryptDestroyKey (Handle);
  if (Status == STATUS_AUTH_TAG_MISMATCH) {
    return -RTW_EBADMSG;
  }

  return (BCRYPT_SUCCESS (Status) && (Done == Len)) ? 0 : -RTW_EIO;
}

int
rtwk_aes_ccm_encrypt (
  const uint8_t  *key,
  const uint8_t  *nonce,
  const uint8_t  *aad,
  uint32_t       aad_len,
  uint8_t        *data,
  uint32_t       len,
  uint8_t        *mic
  )
{
  return RtwCcm (TRUE, key, nonce, aad, aad_len, data, len, mic);
}

int
rtwk_aes_ccm_decrypt (
  const uint8_t  *key,
  const uint8_t  *nonce,
  const uint8_t  *aad,
  uint32_t       aad_len,
  uint8_t        *data,
  uint32_t       len,
  const uint8_t  *mic
  )
{
  UCHAR  Tag[RTW_CCM_MIC];

  // CNG takes the expected tag through a non-const pointer
  RtlCopyMemory (Tag, mic, sizeof (Tag));
  return RtwCcm (FALSE, key, nonce, aad, aad_len, data, len, Tag);
}

int
rtwk_aes_cmac (
  const uint8_t  *key,
  const uint8_t  *data,
  uint32_t       len,
  uint8_t        *mac
  )
{
  BCRYPT_HASH_HANDLE  Hash;
  NTSTATUS            Status;

  if (!mOpen || !RtwAtPassive ()) {
    return -RTW_ENODEV;
  }

  Status = BCryptCreateHash (mCmac, &Hash, NULL, 0, (PUCHAR)key, RTW_AES_KEY, 0);
  if (!BCRYPT_SUCCESS (Status)) {
    return -RTW_EIO;
  }

  if (len != 0) {
    Status = BCryptHashData (Hash, (PUCHAR)data, len, 0);
  }

  if (BCRYPT_SUCCESS (Status)) {
    Status = BCryptFinishHash (Hash, mac, 16, 0);
  }

  BCryptDestroyHash (Hash);
  return BCRYPT_SUCCESS (Status) ? 0 : -RTW_EIO;
}

/* ---- known-answer tests ---- */

// RFC 4493 section 4: the key, the 64-byte message (examples use 0, 16, 40 and 64 bytes of it), the four CMACs
static const UCHAR  mCmacKey[16] = {
  0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6, 0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf, 0x4f, 0x3c
};
static const UCHAR  mCmacMsg[64] = {
  0x6b, 0xc1, 0xbe, 0xe2, 0x2e, 0x40, 0x9f, 0x96, 0xe9, 0x3d, 0x7e, 0x11, 0x73, 0x93, 0x17, 0x2a,
  0xae, 0x2d, 0x8a, 0x57, 0x1e, 0x03, 0xac, 0x9c, 0x9e, 0xb7, 0x6f, 0xac, 0x45, 0xaf, 0x8e, 0x51,
  0x30, 0xc8, 0x1c, 0x46, 0xa3, 0x5c, 0xe4, 0x11, 0xe5, 0xfb, 0xc1, 0x19, 0x1a, 0x0a, 0x52, 0xef,
  0xf6, 0x9f, 0x24, 0x45, 0xdf, 0x4f, 0x9b, 0x17, 0xad, 0x2b, 0x41, 0x7b, 0xe6, 0x6c, 0x37, 0x10
};
static const ULONG  mCmacLen[4] = { 0, 16, 40, 64 };
static const UCHAR  mCmacOut[4][16] = {
  { 0xbb, 0x1d, 0x69, 0x29, 0xe9, 0x59, 0x37, 0x28, 0x7f, 0xa3, 0x7d, 0x12, 0x9b, 0x75, 0x67, 0x46 },
  { 0x07, 0x0a, 0x16, 0xb4, 0x6b, 0x4d, 0x41, 0x44, 0xf7, 0x9b, 0xdd, 0x9d, 0xd0, 0x4a, 0x28, 0x7c },
  { 0xdf, 0xa6, 0x67, 0x47, 0xde, 0x9a, 0xe6, 0x30, 0x30, 0xca, 0x32, 0x61, 0x14, 0x97, 0xc8, 0x27 },
  { 0x51, 0xf0, 0xbe, 0xbf, 0x7e, 0x3b, 0x9d, 0x92, 0xfc, 0x49, 0x74, 0x17, 0x79, 0x36, 0x3c, 0xfe }
};

// IEEE 802.11-2020 J.6.4 (CCMP-128): TK, the AAD and nonce of its data MPDU (PN 0xB5039776E70C), plaintext,
// ciphertext and MIC
static const UCHAR  mCcmpTk[16] = {
  0xc9, 0x7c, 0x1f, 0x67, 0xce, 0x37, 0x11, 0x85, 0x51, 0x4a, 0x8a, 0x19, 0xf2, 0xbd, 0xd5, 0x2f
};
static const UCHAR  mCcmpAad[22] = {
  0x08, 0x40, 0x0f, 0xd2, 0xe1, 0x28, 0xa5, 0x7c, 0x50, 0x30, 0xf1, 0x84, 0x44, 0x08, 0xab, 0xae,
  0xa5, 0xb8, 0xfc, 0xba, 0x00, 0x00
};
static const UCHAR  mCcmpNonce[13] = {
  0x00, 0x50, 0x30, 0xf1, 0x84, 0x44, 0x08, 0xb5, 0x03, 0x97, 0x76, 0xe7, 0x0c
};
static const UCHAR  mCcmpPlain[20] = {
  0xf8, 0xba, 0x1a, 0x55, 0xd0, 0x2f, 0x85, 0xae, 0x96, 0x7b, 0xb6, 0x2f, 0xb6, 0xcd, 0xa8, 0xeb,
  0x7e, 0x78, 0xa0, 0x50
};
static const UCHAR  mCcmpCipher[20] = {
  0xf3, 0xd0, 0xa2, 0xfe, 0x9a, 0x3d, 0xbf, 0x23, 0x42, 0xa6, 0x43, 0xe4, 0x32, 0x46, 0xe8, 0x0c,
  0x3c, 0x04, 0xd0, 0x19
};
static const UCHAR  mCcmpMic[8] = { 0x78, 0x45, 0xce, 0x0b, 0x16, 0xf9, 0x76, 0x23 };

static BOOLEAN
RtwSame (
  const UCHAR  *A,
  const UCHAR  *B,
  ULONG        Len
  )
{
  ULONG  i;

  for (i = 0; i < Len; i++) {
    if (A[i] != B[i]) {
      return FALSE;
    }
  }

  return TRUE;
}

/* 0 if every test passed, else the number of the first that failed */
static int
RtwCryptoSelfTest (
  VOID
  )
{
  UCHAR  Mac[16];
  UCHAR  Buf[sizeof (mCcmpPlain)];
  UCHAR  Mic[RTW_CCM_MIC];
  ULONG  i;

  for (i = 0; i < 4; i++) {
    if ((rtwk_aes_cmac (mCmacKey, mCmacMsg, mCmacLen[i], Mac) != 0) || !RtwSame (Mac, mCmacOut[i], 16)) {
      return 1 + (int)i;
    }
  }

  RtlCopyMemory (Buf, mCcmpPlain, sizeof (Buf));
  if ((rtwk_aes_ccm_encrypt (mCcmpTk, mCcmpNonce, mCcmpAad, sizeof (mCcmpAad), Buf, sizeof (Buf), Mic) != 0) ||
      !RtwSame (Buf, mCcmpCipher, sizeof (Buf)) || !RtwSame (Mic, mCcmpMic, sizeof (Mic)))
  {
    return 5;
  }

  if ((rtwk_aes_ccm_decrypt (mCcmpTk, mCcmpNonce, mCcmpAad, sizeof (mCcmpAad), Buf, sizeof (Buf), mCcmpMic) != 0) ||
      !RtwSame (Buf, mCcmpPlain, sizeof (Buf)))
  {
    return 6;
  }

  // a frame whose MIC does not match is refused
  RtlCopyMemory (Buf, mCcmpCipher, sizeof (Buf));
  RtlCopyMemory (Mic, mCcmpMic, sizeof (Mic));
  Mic[7] ^= 0x01;
  if (rtwk_aes_ccm_decrypt (mCcmpTk, mCcmpNonce, mCcmpAad, sizeof (mCcmpAad), Buf, sizeof (Buf), Mic) != -RTW_EBADMSG) {
    return 7;
  }

  return 0;
}

VOID
RtwCryptoInit (
  VOID
  )
{
  NTSTATUS  Status;
  int       Failed;

  mReady = FALSE;
  Status = BCryptOpenAlgorithmProvider (&mCcm, BCRYPT_AES_ALGORITHM, NULL, 0);
  if (BCRYPT_SUCCESS (Status)) {
    Status = BCryptSetProperty (mCcm, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_CCM,
                                sizeof (BCRYPT_CHAIN_MODE_CCM), 0);
    if (!BCRYPT_SUCCESS (Status)) {
      BCryptCloseAlgorithmProvider (mCcm, 0);
    }
  }

  if (!BCRYPT_SUCCESS (Status)) {
    RtwLog ("crypto: AES-CCM not available (0x%08x); management frame protection and SAE off", Status);
    return;
  }

  Status = BCryptOpenAlgorithmProvider (&mCmac, BCRYPT_AES_CMAC_ALGORITHM, NULL, 0);
  if (!BCRYPT_SUCCESS (Status)) {
    BCryptCloseAlgorithmProvider (mCcm, 0);
    RtwLog ("crypto: AES-CMAC not available (0x%08x); management frame protection and SAE off", Status);
    return;
  }

  mOpen  = TRUE;
  Failed = RtwCryptoSelfTest ();
  mReady = (Failed == 0);
  if (mReady) {
    RtwLog ("crypto: AES-CCM and AES-CMAC (CNG) self-test passed");
  } else {
    RtwLog ("crypto: AES-CCM and AES-CMAC (CNG) self-test FAILED (test %d); management frame protection and SAE off",
            Failed);
  }
}

VOID
RtwCryptoExit (
  VOID
  )
{
  if (!mOpen) {
    return;
  }

  mReady = FALSE;
  mOpen  = FALSE;
  BCryptCloseAlgorithmProvider (mCmac, 0);
  BCryptCloseAlgorithmProvider (mCcm, 0);
}
