<#
.SYNOPSIS
  Opens a Certum SimplySign session on a CI runner, so that signtool can sign with the
  cloud code signing certificate without anyone typing a one-time code.

.DESCRIPTION
  Certum keeps the private key in its cloud HSM. SimplySign Desktop presents it to Windows as a
  virtual smart card, and the certificate appears in Cert:\CurrentUser\My while a session is
  open. A session is opened by typing the account name and a TOTP code into the app's login
  dialog; there is no command line or API login. So this script installs the app (pinned by
  hash), sets its options so the dialog shows on start and the CSP keeps the session PIN,
  computes the TOTP code from the otpauth:// URI behind the SimplySign mobile app's QR code,
  pastes the credentials into the dialog and waits for the certificate to appear.

  TOTP codes are single use and repeated rejections lock the Certum account, so callers must
  never run two logins at once (see the concurrency group in windows-build.yml), and this script
  gives up after three rejected codes. Nothing about the account or the code is logged.

  The approach follows the MIT licensed Connect-SimplySign.ps1 of FrodeHus/elevate, itself after
  dismine/windows-app-signing-setup-action. It lives in this repository so that the TOTP seed,
  which is as sensitive as the signing key, never reaches third-party action code.

.PARAMETER UserName
  The SimplySign account e-mail (default: CERTUM_USERNAME).

.PARAMETER OtpUri
  The otpauth://totp/... URI from the SimplySign mobile app (default: CERTUM_OTP_URI).

.PARAMETER Thumbprint
  SHA-1 thumbprint of the code signing certificate (default: CERTUM_CERT_SHA1). Not secret.

.PARAMETER SelfTest
  Only check the TOTP implementation against the RFC 6238 test vectors, then exit.
#>
[CmdletBinding()]
param(
    [string]$UserName = $env:CERTUM_USERNAME,
    [string]$OtpUri = $env:CERTUM_OTP_URI,
    [string]$Thumbprint = $env:CERTUM_CERT_SHA1,
    [switch]$SelfTest
)

$ErrorActionPreference = 'Stop'

# TOTP (RFC 6238) {{{
function ConvertFrom-Base32([string]$Text) {
    $alphabet = 'ABCDEFGHIJKLMNOPQRSTUVWXYZ234567'
    $bytes = [Collections.Generic.List[byte]]::new()
    $buffer = 0; $bits = 0
    foreach ($c in $Text.Trim().TrimEnd('=').ToUpperInvariant().ToCharArray()) {
        $v = $alphabet.IndexOf($c)
        if ($v -lt 0) { throw 'The TOTP secret is not valid Base32' }
        $buffer = (($buffer -shl 5) -bor $v) -band 0xFFFF; $bits += 5
        if ($bits -ge 8) { $bits -= 8; $bytes.Add([byte](($buffer -shr $bits) -band 0xFF)) }
    }
    , $bytes.ToArray()
}

function Get-TotpCode([byte[]]$Secret, [long]$Counter, [int]$Digits, [string]$Algorithm) {
    $hmac = switch ($Algorithm) {
        'SHA256' { [Security.Cryptography.HMACSHA256]::new($Secret) }
        'SHA512' { [Security.Cryptography.HMACSHA512]::new($Secret) }
        default { [Security.Cryptography.HMACSHA1]::new($Secret) }
    }
    $message = [BitConverter]::GetBytes($Counter)
    if ([BitConverter]::IsLittleEndian) { [Array]::Reverse($message) }
    try { $h = $hmac.ComputeHash($message) } finally { $hmac.Dispose() }
    $o = $h[$h.Length - 1] -band 0x0F
    $binary = (([int]$h[$o] -band 0x7F) -shl 24) -bor ([int]$h[$o + 1] -shl 16) -bor ([int]$h[$o + 2] -shl 8) -bor [int]$h[$o + 3]
    ($binary % [long][Math]::Pow(10, $Digits)).ToString().PadLeft($Digits, '0')
}

if ($SelfTest) {
    $ascii = [Text.Encoding]::ASCII
    $keys = @{
        SHA1 = $ascii.GetBytes('12345678901234567890')
        SHA256 = $ascii.GetBytes('12345678901234567890123456789012')
        SHA512 = $ascii.GetBytes('1234567890123456789012345678901234567890123456789012345678901234')
    }
    # RFC 6238 appendix B
    $vectors = @(
        @(59, 'SHA1', '94287082'), @(59, 'SHA256', '46119246'), @(59, 'SHA512', '90693936'),
        @(1111111109, 'SHA1', '07081804'), @(1111111109, 'SHA256', '68084774'), @(1111111109, 'SHA512', '25091201'),
        @(1234567890, 'SHA1', '89005924'), @(2000000000, 'SHA256', '90698825'), @(20000000000, 'SHA512', '47863826')
    )
    foreach ($v in $vectors) {
        $code = Get-TotpCode $keys[$v[1]] ([long][Math]::Floor($v[0] / 30)) 8 $v[1]
        if ($code -ne $v[2]) { throw "TOTP $($v[1]) at $($v[0]) is $code, expected $($v[2])" }
    }
    $base32 = ConvertFrom-Base32 'GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ'
    if ($ascii.GetString($base32) -ne '12345678901234567890') { throw 'Base32 decoding is wrong' }
    Write-Host "TOTP self test passed ($($vectors.Count) RFC 6238 vectors)"
    exit 0
}
# }}}
# Bump deliberately: the dialog automation below is tested against this version
$InstallerUrl = 'https://files.certum.eu/software/SimplySignDesktop/Windows/9.4.3.90/SimplySignDesktop-9.4.3.90-64-bit-en.msi'
$InstallerSha256 = 'ce2b38ef0124a574bc08e5558726099caac79a7c58b8d3d712f629371fe1bcb7'
$InstallerPublisher = 'CN=Asseco Data Systems S.A.,*'

foreach ($name in 'UserName', 'OtpUri', 'Thumbprint') {
    if ([string]::IsNullOrWhiteSpace((Get-Variable $name -ValueOnly))) { throw "$name is not set, see the help of this script" }
}
$Thumbprint = ($Thumbprint -replace '[^0-9A-Fa-f]', '').ToUpperInvariant()

function Get-SigningCertificate {
    Get-ChildItem Cert:\CurrentUser\My -ErrorAction SilentlyContinue |
        Where-Object { $_.Thumbprint -eq $Thumbprint } | Select-Object -First 1
}

if ($cert = Get-SigningCertificate) { Write-Host "SimplySign session already open: $($cert.Subject)"; exit 0 }

# Install {{{
$exe = Join-Path $env:ProgramFiles 'Certum\SimplySign Desktop\SimplySignDesktop.exe'
if (-not (Test-Path $exe)) {
    $temp = if ($env:RUNNER_TEMP) { $env:RUNNER_TEMP } else { [IO.Path]::GetTempPath() }
    $msi = Join-Path $temp 'SimplySignDesktop.msi'
    $log = Join-Path $temp 'SimplySignDesktop-install.log'
    Write-Host '== Installing SimplySign Desktop'
    Invoke-WebRequest -Uri $InstallerUrl -OutFile $msi -UseBasicParsing
    $hash = (Get-FileHash -Algorithm SHA256 $msi).Hash.ToLowerInvariant()
    if ($hash -ne $InstallerSha256) { throw "The SimplySign Desktop installer has SHA-256 $hash, not the pinned $InstallerSha256" }
    $sig = Get-AuthenticodeSignature -LiteralPath $msi
    if ($sig.Status -ne 'Valid' -or $sig.SignerCertificate.Subject -notlike $InstallerPublisher) {
        throw "The SimplySign Desktop installer is not signed by Certum: $($sig.Status) $($sig.SignerCertificate.Subject)"
    }
    $p = Start-Process msiexec.exe -Wait -PassThru -ArgumentList @(
        '/i', "`"$msi`"", '/quiet', '/norestart', '/l*v', "`"$log`"", 'ALLUSERS=1', 'REBOOT=ReallySuppress')
    if ($p.ExitCode -notin 0, 3010 -or -not (Test-Path $exe)) {
        if (Test-Path $log) { Get-Content $log -Tail 40 | Out-Host }
        throw "SimplySign Desktop did not install, msiexec exit code: $($p.ExitCode)"
    }
}
# }}}

# Configure {{{
# Show the login dialog on start so there is something to type into, keep the PIN in the CSP
# for the session so signtool never prompts, forget it on disconnect, do not autostart.
$key = 'HKCU:\Software\Certum\SimplySign'
New-Item -Path $key -Force | Out-Null
$settings = @{
    ShowLoginDialogOnStart = 1; ShowLoginDialogOnAppRequest = 1; RememberLastUserName = 1; Autostart = 0
    UnregisterCertificatesOnDisconnect = 0; RememberPINinCSP = 1; ForgetPINinCSPonDisconnect = 1; LangID = 9
}
foreach ($s in $settings.GetEnumerator()) {
    New-ItemProperty -Path $key -Name $s.Key -PropertyType DWord -Value $s.Value -Force | Out-Null
}
# }}}

# The login code {{{
$uri = [Uri]$OtpUri
if ($uri.Scheme -ne 'otpauth') { throw 'OtpUri must be an otpauth:// URI' }
$query = @{}
foreach ($part in $uri.Query.TrimStart('?') -split '&') {
    $kv = $part -split '=', 2
    if ($kv.Count -eq 2) { $query[$kv[0].ToLowerInvariant()] = [Uri]::UnescapeDataString($kv[1]) }
}
if (-not $query['secret']) { throw 'The otpauth:// URI has no secret' }
$secret = ConvertFrom-Base32 $query['secret']
$digits = if ($query['digits']) { [int]$query['digits'] } else { 6 }
$period = if ($query['period']) { [int]$query['period'] } else { 30 }
$algorithm = if ($query['algorithm']) { $query['algorithm'].ToUpperInvariant() } else { 'SHA1' }

function Get-Counter { [long][Math]::Floor([DateTimeOffset]::UtcNow.ToUnixTimeSeconds() / $period) }

# A code sent late in its window can expire before it is checked, and SimplySign refuses a second
# code from a window in which one was rejected: so submit early in a window after the last one.
function Get-FreshCode([long]$After = -1) {
    while ((Get-Counter) -le $After) { Start-Sleep -Seconds 2 }
    $left = $period - ([DateTimeOffset]::UtcNow.ToUnixTimeSeconds() % $period)
    if ($left -lt 20) { Start-Sleep -Seconds ($left + 1) }
    Get-TotpCode $secret (Get-Counter) $digits $algorithm
}
# }}}

# Drive the login dialog {{{
Add-Type @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public static class KitsuSimplySignWindows {
    delegate bool EnumProc(IntPtr hWnd, IntPtr lParam);
    [StructLayout(LayoutKind.Sequential)] struct RECT { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc callback, IntPtr lParam);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint pid);
    [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr hWnd);
    [DllImport("user32.dll")] static extern bool GetWindowRect(IntPtr hWnd, out RECT rect);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    public static IntPtr[] Visible(uint pid) {
        var found = new List<IntPtr>();
        EnumWindows((h, l) => { uint p; GetWindowThreadProcessId(h, out p); if (p == pid && IsWindowVisible(h)) found.Add(h); return true; }, IntPtr.Zero);
        return found.ToArray();
    }
    public static long Area(IntPtr h) { RECT r; GetWindowRect(h, out r); return (long)(r.Right - r.Left) * (r.Bottom - r.Top); }
}
'@

Get-Process -Name SimplySignDesktop -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Write-Host '== Opening the SimplySign session'
$process = Start-Process -FilePath $exe -PassThru
$shell = New-Object -ComObject WScript.Shell

function Get-AppWindows { @([KitsuSimplySignWindows]::Visible([uint32]$process.Id)) }

function Set-Focus([IntPtr]$Handle) {
    for ($i = 0; $i -lt 12; $i++) {
        [KitsuSimplySignWindows]::SetForegroundWindow($Handle) | Out-Null
        Start-Sleep -Milliseconds 300
        if ([KitsuSimplySignWindows]::GetForegroundWindow() -eq $Handle) { return $true }
    }
    $false
}

# SendKeys drops characters in Qt fields now and then, a paste does not
function Send-Text([string]$Text) {
    Set-Clipboard -Value $Text
    $shell.SendKeys('^a'); Start-Sleep -Milliseconds 120
    $shell.SendKeys('{DEL}'); Start-Sleep -Milliseconds 120
    $shell.SendKeys('^v'); Start-Sleep -Milliseconds 250
    Set-Clipboard -Value ' '
}

# All the app's windows share a title. The login dialog is the largest; any other is a modal:
# the "new version" prompt (Yes/No, never answer Yes, that starts a download) or the "invalid
# user name or token" error (OK). Trying "No" before Enter means Enter only reaches the OK box.
function Close-Modals([IntPtr]$Login) {
    $closed = $false
    foreach ($modal in (Get-AppWindows | Where-Object { $_ -ne $Login })) {
        foreach ($keys in '%n', '{TAB} ', '{ENTER}') {
            Set-Focus $modal | Out-Null
            $shell.SendKeys($keys); Start-Sleep -Milliseconds 800
            if (-not (Get-AppWindows | Where-Object { $_ -eq $modal })) { $closed = $true; break }
        }
    }
    $closed
}

$windows = @()
for ($i = 0; $i -lt 30 -and $windows.Count -eq 0; $i++) {
    Start-Sleep -Seconds 1
    $process.Refresh()
    if ($process.HasExited) { throw "SimplySign Desktop exited during startup with code $($process.ExitCode)" }
    $windows = Get-AppWindows
}
if ($windows.Count -eq 0) { throw 'SimplySign Desktop showed no login dialog within 30 seconds' }
$login = $windows | Sort-Object { [KitsuSimplySignWindows]::Area($_) } -Descending | Select-Object -First 1

# let the asynchronous version check raise its prompt, and close it, before typing
for ($i = 0; $i -lt 5; $i++) { Close-Modals $login | Out-Null; Start-Sleep -Seconds 1 }

if (-not (Set-Focus $login)) { throw 'Could not focus the SimplySign login dialog' }
Start-Sleep -Milliseconds 400
Send-Text $UserName
$shell.SendKeys('{TAB}'); Start-Sleep -Milliseconds 200
Send-Text (Get-FreshCode)
$last = Get-Counter
$shell.SendKeys('{ENTER}')

# After a rejection the dialog keeps the account name and focuses the empty code field
$rejected = 0
for ($elapsed = 0; $elapsed -lt 180; $elapsed += 5) {
    Start-Sleep -Seconds 5
    if ($cert = Get-SigningCertificate) {
        Write-Host "SimplySign session open: $($cert.Subject), certificate valid until $($cert.NotAfter.ToString('u'))"
        exit 0
    }
    if (Get-AppWindows | Where-Object { $_ -ne $login }) {
        if (++$rejected -gt 3) { throw 'SimplySign rejected the login 3 times, check the account name, the otpauth URI and the clock' }
        if (Close-Modals $login) {
            Start-Sleep -Milliseconds 500
            Send-Text (Get-FreshCode -After $last)
            $last = Get-Counter
            $shell.SendKeys('{ENTER}')
        }
    }
}
throw "The certificate $Thumbprint did not appear in Cert:\CurrentUser\My within 180 seconds"
# }}}
