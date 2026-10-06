// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Meari
/*
 * driver\crypto.c as a user-mode program (tools-dev\crypto-test.ps1): the same CNG calls against bcrypt.dll and the
 * driver's own known-answer tests, so a mistake in the CNG usage shows up on the PC before the driver goes to the
 * board.
 */
#include <windows.h>
#include <stdio.h>
#include "../compat/include/rtw/os.h"

VOID RtwCryptoInit (VOID);
VOID RtwCryptoExit (VOID);

int
main (
  void
  )
{
  int  Ok;

  RtwCryptoInit ();
  Ok = rtwk_crypto_ready ();
  RtwCryptoExit ();
  printf ("%s\n", Ok ? "PASS" : "FAIL");
  return Ok ? 0 : 1;
}
