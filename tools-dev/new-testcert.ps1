# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Meari
# Make a self-signed code signing certificate for test-signing your own builds, in Cert:\CurrentUser\My (with its
# private key), and print its thumbprint for build.ps1 -CertThumbprint (or RTW89WIN_CERT / local.settings.psd1).
# A test-signed driver loads only on a Windows in test mode (bcdedit /set testsigning on; Secure Boot off). The
# install scripts trust the certificate (the rtw89win-test.cer of the package, no private key) only while they install.
#   .\tools-dev\new-testcert.ps1 [-Subject 'CN=rtw89win test'] [-Years 5]
param([string]$Subject = 'CN=rtw89win test', [int]$Years = 5)
$ErrorActionPreference = 'Stop'
$cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject $Subject -CertStoreLocation Cert:\CurrentUser\My `
  -KeyAlgorithm RSA -KeyLength 3072 -HashAlgorithm SHA256 -NotAfter (Get-Date).AddYears($Years)
"made $($cert.Subject), valid until $($cert.NotAfter.ToString('yyyy-MM-dd')), in Cert:\CurrentUser\My"
"thumbprint: $($cert.Thumbprint)"
"build:      .\build.ps1 -CertThumbprint $($cert.Thumbprint)"
