# License: GPLv3 Copyright: 2026, Kitty contributors
# Bootstrap only: all probes live in kitty.health_check.
[CmdletBinding()]
param(
    [ValidateSet('quick', 'native')][string]$Mode = 'quick',
    [ValidateSet('text', 'json')][string]$Format = 'text',
    [ValidateRange(1, 3600)][int]$Timeout = 180,
    [ValidateRange(1, 3600)][int]$ProbeTimeout = 20
)
$ErrorActionPreference = 'Stop'
$exe = Join-Path $PSScriptRoot 'kitty.exe'
if (-not (Test-Path $exe)) {
    $exe = Join-Path $PSScriptRoot '..\kitty\launcher\kitty.exe'
}
$process = $null
$failure = 'Kitty runtime could not initialize. Reinstall this build or rebuild it with its matching runtime DLLs.'
try {
    $start = New-Object System.Diagnostics.ProcessStartInfo
    $start.FileName = [IO.Path]::GetFullPath($exe)
    $start.Arguments = "+health-check --format=$Format --mode=$Mode --timeout=$Timeout --probe-timeout=$ProbeTimeout"
    $start.UseShellExecute = $false
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $start.RedirectStandardInput = $true
    $process = New-Object System.Diagnostics.Process
    $process.StartInfo = $start
    if (-not $process.Start()) { throw 'start failed' }
    $process.StandardInput.Close()
    $stdout = $process.StandardOutput.ReadToEndAsync()
    $stderr = $process.StandardError.ReadToEndAsync()
    if (-not $process.WaitForExit(($Timeout + 30) * 1000)) {
        # Killing the controller closes its non-inheritable process jobs too.
        $process.Kill()
        $process.WaitForExit()
        throw 'runtime timed out'
    }
    $output = $stdout.Result
    if (-not [string]::IsNullOrWhiteSpace($output)) {
        # Never relay raw runtime stderr: import errors can contain private paths.
        [Console]::Out.Write($output)
        exit $process.ExitCode
    }
} catch {
    # Error details can include local paths; retain only a fixed diagnostic.
} finally {
    if ($null -ne $process) { $process.Dispose() }
}
if ($Format -eq 'json') {
    [ordered]@{
        schema_version = 1; mode = $Mode; exit_code = 3; internal_error = $true; cancelled = $false
        excluded_coverage = @('WSL sessions', 'physical input', 'hardware display/GPU')
        results = @([ordered]@{
            id = 'bootstrap.runtime'; required = $true; prerequisites = @(); timeout_seconds = $Timeout
            status = 'BLOCKED'; summary = $failure; evidence = @{}; duration_ms = 0
            remediation = 'Reinstall this build or rebuild it with its matching runtime DLLs.'
        })
    } | ConvertTo-Json -Depth 5
} else {
    [Console]::Out.WriteLine("BLOCKED bootstrap.runtime: $failure")
}
exit 3
