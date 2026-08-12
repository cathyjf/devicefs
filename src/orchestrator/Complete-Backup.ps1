# SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
# SPDX-License-Identifier: GPL-3.0-or-later

#requires -Version 7.4
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Get-SuppliedShadowSetID {
    if (!(Test-Path -LiteralPath $env:VSHADOW_TEMP_FILE -PathType Leaf)) {
        throw 'Could not find data file supplied by the orchestrator.'
    }
    $magic_prefix = 'SET SHADOW_SET_ID='
    $setvar_lines = Get-Content -LiteralPath $env:VSHADOW_TEMP_FILE
    foreach ($line in $setvar_lines) {
        if (!($line.StartsWith($magic_prefix))) {
            continue
        }
        return $line.Substring($magic_prefix.Length)
    }
    throw 'Could not obtain SHADOW_SET_ID from data file.'
}

function Get-RelevantShadows {
    $shadow_set_id = Get-SuppliedShadowSetID
    $shadows = Get-CimInstance -ClassName Win32_ShadowCopy |
        Where-Object { $_.SetID -eq $shadow_set_id }
    if ($shadows.Length -eq 0) {
        throw 'Could not retrieve information on shadow copies.'
    }
    return $shadows
}

function Start-Process-With-Argv {
    <#
    .DESCRIPTION
        This function invokes a process with a proper argv[] array.

        This is necessary because the native PowerShell `Start-Process` function
        joins all of its arguments together with spaces, rather than offering
        meaningful argv[] semantics.
    #>
    [OutputType([Diagnostics.Process])]
    param(
        [Parameter(Mandatory)]
        [string]$FileName,
        [string[]]$ArgumentList = @(),
        [switch]$RedirectStreams
    )
    $psi = [Diagnostics.ProcessStartInfo]::new()
    $psi.FileName = $FileName
    $psi.UseShellExecute = $false
    $psi.WindowStyle = [Diagnostics.ProcessWindowStyle]::Hidden
    if ($RedirectStreams) {
        $psi.RedirectStandardInput = $true
        $psi.RedirectStandardOutput = $true
        $psi.RedirectStandardError = $true
    }
    $ArgumentList.ForEach({ $psi.ArgumentList.Add($_) })
    return [Diagnostics.Process]::Start($psi)
}

function Close-WslInvocation {
    param(
        [Parameter(Mandatory)]
        [pscustomobject]$Invocation
    )
    $Invocation.Process.StandardInput.Dispose()
    $Invocation.Process.StandardOutput.Dispose()
    $Invocation.Process.StandardError.Dispose()
    $Invocation.Process.Dispose()
}

function Start-Wsl-Fish {
    [OutputType([pscustomobject])]
    param(
        [Parameter(Mandatory)]
        [string]$Command,
        [string[]]$ArgumentList = @()
    )
    $wsl_argv = @(
        '--distribution', 'Debian',
        '--exec', '/usr/bin/fish', '-c', 'source - $argv'
    ) + $ArgumentList
    $backup_supervisor_path = $env:DEVICEFS_BACKUP_SUPERVISOR_PATH
    if (!$backup_supervisor_path) {
        throw 'The backup supervisor path was not supplied.'
    }
    if (!(Test-Path -LiteralPath $backup_supervisor_path -PathType Leaf)) {
        throw 'Could not locate backup-supervisor.exe.'
    }
    $process = Start-Process-With-Argv `
        -FileName $backup_supervisor_path `
        -ArgumentList (@('--run-wsl-as-pbs-vss') + $wsl_argv) `
        -RedirectStreams
    $invocation = [pscustomobject]@{
        Process = $process
        StreamCopies = [Threading.Tasks.Task[]]@()
    }
    try {
        $invocation.StreamCopies = [Threading.Tasks.Task[]]@(
            $process.StandardOutput.BaseStream.CopyToAsync(
                [Console]::OpenStandardOutput())
            $process.StandardError.BaseStream.CopyToAsync(
                [Console]::OpenStandardError())
        )
        try {
            $process.StandardInput.Write($Command)
        } finally {
            $process.StandardInput.Close()
        }
    } catch {
        Close-WslInvocation $invocation
        throw
    }
    return $invocation
}

function Wait-Wsl {
    [OutputType([int])]
    param(
        [Parameter(Mandatory)]
        [pscustomobject]$Invocation,
        [int]$TimeoutMilliseconds = [Threading.Timeout]::Infinite
    )
    if (!$Invocation.Process.WaitForExit($TimeoutMilliseconds)) {
        return $null
    }
    [Threading.Tasks.Task]::WaitAll($Invocation.StreamCopies)
    return $Invocation.Process.ExitCode
}

function Send-WslBackupSignal {
    param(
        [Parameter(Mandatory)]
        [string]$PidFile,
        [Parameter(Mandatory)]
        [string]$StopFile,
        [Parameter(Mandatory)]
        [ValidateSet('TERM', 'KILL')]
        [string]$Signal,
        [Parameter(Mandatory)]
        [int]$TimeoutMilliseconds
    )
    $control = $null
    try {
        $control = Start-Wsl-Fish `
            -Command 'touch $argv[2]; if test -s $argv[1]; kill -s $argv[3] (cat $argv[1]); end' `
            -ArgumentList @($PidFile, $StopFile, $Signal)
        $exit_code = Wait-Wsl `
            -Invocation $control `
            -TimeoutMilliseconds $TimeoutMilliseconds
        if ($null -eq $exit_code) {
            throw "The WSL ${Signal} request did not exit before its timeout."
        }
        if ($exit_code -ne 0) {
            throw "The WSL ${Signal} request exited with code ${exit_code}."
        }
    } finally {
        if ($null -ne $control) {
            Close-WslInvocation $control
        }
    }
}

function Stop-DeviceFs {
    [OutputType([bool])]
    param(
        [Parameter(Mandatory)]
        [Diagnostics.Process]$Process,
        [Parameter(Mandatory)]
        [string]$StopEventName,
        [Parameter(Mandatory)]
        [int]$TimeoutMilliseconds
    )
    $stop_requested = $false
    $timer = [Diagnostics.Stopwatch]::StartNew()
    while (!$Process.HasExited -and
        ($timer.ElapsedMilliseconds -lt $TimeoutMilliseconds)) {
        if (!$stop_requested) {
            $event = $null
            if ([Threading.EventWaitHandle]::TryOpenExisting(
                    $StopEventName, [ref]$event)) {
                try {
                    $stop_requested = $event.Set()
                } finally {
                    $event.Dispose()
                }
            }
        }
        [void] $Process.WaitForExit(100)
    }
    if (!$Process.HasExited) {
        throw 'devicefs did not exit before the shutdown timeout elapsed.'
    }
    return $stop_requested
}

function Open-SupervisorCancellationEvent {
    [OutputType([Threading.EventWaitHandle])]
    param()
    if (!$env:DEVICEFS_BACKUP_STOP_EVENT) {
        throw 'The backup supervisor cancellation event was not supplied.'
    }
    $event = $null
    if (![Threading.EventWaitHandle]::TryOpenExisting(
            $env:DEVICEFS_BACKUP_STOP_EVENT, [ref]$event)) {
        throw 'Could not open the backup supervisor cancellation event.'
    }
    return $event
}

function Wait-ProcessOrCancellation {
    [OutputType([bool])]
    param(
        [Parameter(Mandatory)]
        [Diagnostics.Process]$Process,
        [Parameter(Mandatory)]
        [Threading.EventWaitHandle]$CancellationEvent
    )
    while (!$Process.WaitForExit(100)) {
        if ($CancellationEvent.WaitOne(0)) {
            return $true
        }
    }
    return $false
}

function Wait-WslBackup {
    [OutputType([int])]
    param(
        [Parameter(Mandatory)]
        [pscustomobject]$Invocation,
        [Parameter(Mandatory)]
        [string]$PidFile,
        [Parameter(Mandatory)]
        [string]$StopFile,
        [Parameter(Mandatory)]
        [Threading.EventWaitHandle]$CancellationEvent
    )
    $control_timeout_milliseconds = 15000
    $term_timeout_milliseconds = 45000
    $kill_timeout_milliseconds = 30000
    try {
        $cancelled = Wait-ProcessOrCancellation `
            -Process $Invocation.Process `
            -CancellationEvent $CancellationEvent
        if (!$cancelled) {
            return Wait-Wsl -Invocation $Invocation
        }

        try {
            Send-WslBackupSignal `
                -PidFile $PidFile `
                -StopFile $StopFile `
                -Signal TERM `
                -TimeoutMilliseconds $control_timeout_milliseconds
        } catch {
            Write-Error -ErrorRecord $_ -ErrorAction Continue
        }

        $exit_code = Wait-Wsl `
            -Invocation $Invocation `
            -TimeoutMilliseconds $term_timeout_milliseconds
        if ($null -eq $exit_code) {
            try {
                Send-WslBackupSignal `
                    -PidFile $PidFile `
                    -StopFile $StopFile `
                    -Signal KILL `
                    -TimeoutMilliseconds $control_timeout_milliseconds
            } catch {
                Write-Error -ErrorRecord $_ -ErrorAction Continue
            }
            $exit_code = Wait-Wsl `
                -Invocation $Invocation `
                -TimeoutMilliseconds $kill_timeout_milliseconds
            if ($null -eq $exit_code) {
                throw 'The WSL backup did not exit after the KILL request.'
            }
        }
        if ($exit_code -ne 0) {
            Write-Host "The WSL backup exited with code ${exit_code} during cancellation."
        }
        return 130
    } finally {
        Close-WslInvocation $Invocation
    }
}

function Invoke-WslBackup {
    [OutputType([int])]
    param(
        [Parameter(Mandatory)]
        [Threading.EventWaitHandle]$CancellationEvent
    )
    $control_path = "/tmp/devicefs-$([guid]::NewGuid().ToString('N'))"
    $pid_file = "$control_path.pid"
    $stop_file = "$control_path.stop"
    $fish_program_path = $env:DEVICEFS_BACKUP_FISH_PROGRAM_PATH
    if (!$fish_program_path) {
        throw 'The Fish program path was not supplied by backup-supervisor.'
    }
    if (!(Test-Path -LiteralPath $fish_program_path -PathType Leaf)) {
        throw "Could not locate Fish program: ${fish_program_path}"
    }
    $fish_command = Get-Content -LiteralPath $fish_program_path -Raw
    $argv = @($pid_file, $stop_file, $env:COMPUTERNAME)
    $wsl = Start-Wsl-Fish -Command $fish_command `
        -ArgumentList $argv
    return Wait-WslBackup `
        -Invocation $wsl `
        -PidFile $pid_file `
        -StopFile $stop_file `
        -CancellationEvent $CancellationEvent
}

$cancellation_event = Open-SupervisorCancellationEvent
try {
    if ($cancellation_event.WaitOne(0)) {
        exit 130
    }

    $devicefs_start_timeout_milliseconds = 30000
    $devicefs_shutdown_timeout_milliseconds = 60000
    $mount_target = 'X:'
    $stop_event_name = "Global\devicefs-stop-$([guid]::NewGuid().ToString('N'))"
    $devicefs_args = @(
        '--zero-free-clusters',
        '--mount', $mount_target,
        '--read-user', 'pbs-vss',
        '--stop-event', $stop_event_name
    )
    $shadows = Get-RelevantShadows
    $readiness_path = $null
    foreach ($shadow in $shadows) {
        $image_filename = "volume-$([guid]$shadow.VolumeName.Substring(10, 38)).img"
        $readiness_path ??= "${mount_target}\$image_filename"
        $devicefs_args += @('--map', $image_filename, $shadow.DeviceObject)
    }

    $devicefs_path = $env:DEVICEFS_BACKUP_SUPERVISOR_PATH
    if (!$devicefs_path) {
        throw 'The backup supervisor path was not supplied.'
    }
    if (!(Test-Path -LiteralPath $devicefs_path -PathType Leaf)) {
        throw 'Could not locate backup-supervisor.exe.'
    }
    if ([Environment]::GetLogicalDrives() -contains "${mount_target}\") {
        throw "Mount target is already present: ${mount_target}"
    }
    $devicefs_invocation_args = @('--devicefs') + $devicefs_args
    Write-Host 'Setting up virtual filesystem:' $devicefs_path @devicefs_invocation_args
    $devicefs_process = Start-Process-With-Argv `
        -FileName $devicefs_path `
        -ArgumentList $devicefs_invocation_args

    $backup_succeeded = $false
    try {
        $startup_interrupted = $false
        $startup_timer = [Diagnostics.Stopwatch]::StartNew()
        while ($true) {
            if ($devicefs_process.WaitForExit(100)) {
                throw "devicefs exited during startup with code $($devicefs_process.ExitCode)."
            }
            if ($cancellation_event.WaitOne(0)) {
                $startup_interrupted = $true
                break
            }
            if ([IO.File]::Exists($readiness_path)) {
                break
            }
            if ($startup_timer.ElapsedMilliseconds -ge $devicefs_start_timeout_milliseconds) {
                throw "devicefs did not mount ${mount_target} before the startup timeout elapsed."
            }
        }
        if ($startup_interrupted) {
            $backup_exit_code = 130
        } else {
            $backup_exit_code = Invoke-WslBackup `
                -CancellationEvent $cancellation_event
            $backup_succeeded = $backup_exit_code -eq 0
        }
    } finally {
        try {
            try {
                $devicefs_stop_requested = Stop-DeviceFs `
                    -Process $devicefs_process `
                    -StopEventName $stop_event_name `
                    -TimeoutMilliseconds $devicefs_shutdown_timeout_milliseconds
                if ($backup_succeeded -and !$devicefs_stop_requested) {
                    throw "devicefs exited before shutdown was requested with code $($devicefs_process.ExitCode)."
                }
                if ($backup_succeeded -and ($devicefs_process.ExitCode -ne 0)) {
                    throw "devicefs exited with code $($devicefs_process.ExitCode)."
                }
            } catch {
                if ($backup_succeeded) {
                    throw
                }
                Write-Error -ErrorRecord $_ -ErrorAction Continue
            }
        } finally {
            $devicefs_process.Dispose()
        }
    }
} finally {
    $cancellation_event.Dispose()
}

exit $backup_exit_code
