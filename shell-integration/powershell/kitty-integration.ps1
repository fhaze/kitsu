if ((Test-Path variable:global:__KittyShellIntegration) -or $ExecutionContext.SessionState.LanguageMode -ne 'FullLanguage') {
    return
}

$options = @($env:KITTY_SHELL_INTEGRATION -split ' ' | Where-Object { $_ })
$env:KITTY_SHELL_INTEGRATION = $null
$Global:__KittyShellIntegration = @{
    OriginalPrompt = $function:Prompt
    OriginalPSConsoleHostReadLine = $null
    LastHistoryId = -1
    IsInExecution = $false
    Escape = [char]27
    Mark = $options -notcontains 'no-prompt-mark'
    ReportCwd = $options -notcontains 'no-cwd'
    SetTitle = $options -notcontains 'no-title'
    ChangeCursor = ([Environment]::OSVersion.Platform -eq 'Win32NT') -and ($options -notcontains 'no-cursor')
}

function Global:__Kitty-Sanitize-Title([string]$value) {
    [regex]::Replace($value, '[\x00-\x1f\x7f]', '')
}

function Global:__Kitty-Location {
    if ($pwd.Provider.Name -eq 'FileSystem') {
        return $pwd.ProviderPath
    }
    return $pwd.Path
}

function Global:Prompt {
    $failed = [int](-not $global:?)
    Set-StrictMode -Off
    $history = Get-History -Count 1
    $result = ''
    $escape = $Global:__KittyShellIntegration.Escape
    if ($Global:__KittyShellIntegration.Mark -and $Global:__KittyShellIntegration.LastHistoryId -ne -1 -and $Global:__KittyShellIntegration.IsInExecution) {
        $Global:__KittyShellIntegration.IsInExecution = $false
        if ($history.Id -eq $Global:__KittyShellIntegration.LastHistoryId) {
            $result += "$escape]133;D`a"
        } else {
            $result += "$escape]133;D;$failed`a"
        }
    }
    $location = __Kitty-Location
    if ($Global:__KittyShellIntegration.SetTitle) {
        $result += "$escape]2;$(__Kitty-Sanitize-Title $location)`a"
    }
    if ($Global:__KittyShellIntegration.ReportCwd -and $pwd.Provider.Name -eq 'FileSystem') {
        $result += "$escape]7;$([Uri]::new($pwd.ProviderPath).AbsoluteUri)`a"
    }
    if ($Global:__KittyShellIntegration.Mark) {
        $result += "$escape]133;A`a"
    }
    if ($failed) {
        Write-Error failure -ErrorAction Ignore
    }
    $result += $Global:__KittyShellIntegration.OriginalPrompt.Invoke()
    if ($Global:__KittyShellIntegration.Mark) {
        $result += "$escape]133;B`a"
    }
    if ($Global:__KittyShellIntegration.ChangeCursor) {
        $result += "$escape[5 q"
    }
    $Global:__KittyShellIntegration.LastHistoryId = $history.Id
    return $result
}

if (Get-Module -Name PSReadLine) {
    $Global:__KittyShellIntegration.OriginalPSConsoleHostReadLine = $function:PSConsoleHostReadLine
    function Global:PSConsoleHostReadLine {
        $commandLine = $Global:__KittyShellIntegration.OriginalPSConsoleHostReadLine.Invoke()
        $Global:__KittyShellIntegration.IsInExecution = $true
        $result = ''
        $escape = $Global:__KittyShellIntegration.Escape
        if ($Global:__KittyShellIntegration.ChangeCursor) {
            $result += "$escape[0 q"
        }
        if ($Global:__KittyShellIntegration.SetTitle -and -not [string]::IsNullOrWhiteSpace($commandLine)) {
            $result += "$escape]2;$(__Kitty-Sanitize-Title $commandLine)`a"
        }
        if ($Global:__KittyShellIntegration.Mark) {
            $result += "$escape]133;C`a"
        }
        [Console]::Write($result)
        return $commandLine
    }
}

if ($env:KITTY_SI_RUN_COMMAND_AT_STARTUP) {
    $startupCommand = $env:KITTY_SI_RUN_COMMAND_AT_STARTUP
    $env:KITTY_SI_RUN_COMMAND_AT_STARTUP = $null
    Invoke-Expression $startupCommand
}
