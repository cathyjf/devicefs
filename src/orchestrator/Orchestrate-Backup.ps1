# SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
# SPDX-License-Identifier: GPL-3.0-or-later

#requires -Version 7.4
param(
    [switch]$NoWriters
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$BACKUP_DRIVES = @('C:', 'D:', 'E:')

function Get-VShadow {
    $windows_kits_root_key = 'HKLM:\SOFTWARE\Microsoft\Windows Kits\Installed Roots'
    if (!(Test-Path -LiteralPath $windows_kits_root_key -PathType Container)) {
        throw 'No Windows kits found.'
    }
    $windows_kits_root = (Get-ItemProperty -LiteralPath $windows_kits_root_key)?.KitsRoot10
    if (!$windows_kits_root) {
        throw 'No Windows 10/11 SDK installation found.'
    }
    $sdk_versions = Get-ChildItem -LiteralPath $windows_kits_root_key |
        Select-Object -ExpandProperty PSChildName |
        Where-Object { $_ -as [version] } |
        Sort-Object { [version]$_ } -Descending
    foreach ($version in $sdk_versions) {
        $path = Join-Path $windows_kits_root 'bin' $version 'x64\vshadow.exe'
        if (Test-Path -LiteralPath $path -PathType Leaf) {
            return $path
        }
    }
    throw 'No installation of vshadow.exe could be located.'
}

function Get-SystemTempFile {
    $system_temp = $env:DEVICEFS_BACKUP_SYSTEM_TEMP_PATH
    if (!$system_temp) {
        throw 'The SystemTemp path was not supplied by backup-supervisor.'
    }
    $temp_file = Join-Path $system_temp "pbs-vss-$([guid]::NewGuid().ToString('N')).txt"
    New-Item -ItemType File -Path $temp_file | Out-Null
    return $temp_file
}

function Invoke-VShadowManually {
    [OutputType([int])]
    param(
        [Parameter(Mandatory)]
        [string]$FileName,
        [Parameter(Mandatory)]
        [string[]]$ArgumentList,
        [Parameter(Mandatory)]
        [Threading.EventWaitHandle]$CancellationEvent
    )
    $previous_control_c_mode = [Console]::TreatControlCAsInput
    $process = $null
    try {
        # Convert Ctrl+C into input so that only this process acts on it.
        [Console]::TreatControlCAsInput = $true
        $start_info = [Diagnostics.ProcessStartInfo]::new()
        $start_info.FileName = $FileName
        $start_info.UseShellExecute = $false
        $ArgumentList.ForEach({ [void] $start_info.ArgumentList.Add($_) })
        $process = [Diagnostics.Process]::Start($start_info)

        while (!$process.WaitForExit(100)) {
            while ([Console]::KeyAvailable) {
                $key = [Console]::ReadKey($true)
                if (($key.Key -eq [ConsoleKey]::C) -and
                    (($key.Modifiers -band [ConsoleModifiers]::Control) -ne 0)) {
                    if (!$CancellationEvent.WaitOne(0)) {
                        [void] $CancellationEvent.Set()
                        Write-Host 'Cancellation requested; waiting for backup cleanup.'
                    }
                }
            }
        }
        if ($CancellationEvent.WaitOne(0)) {
            return 130
        }
        return $process.ExitCode
    } finally {
        if ($null -ne $process) {
            $process.Dispose()
        }
        [Console]::TreatControlCAsInput = $previous_control_c_mode
    }
}

$backup_lock_path = $env:DEVICEFS_BACKUP_LOCK_PATH
if (!$backup_lock_path) {
    throw 'The backup lock path was not supplied by backup-supervisor.'
}
if (!(Test-Path -LiteralPath $backup_lock_path -PathType Leaf)) {
    throw "Backup lock file missing: ${backup_lock_path}"
}
$backup_lock = [IO.File]::Open(
    $backup_lock_path,
    [IO.FileMode]::Open,
    [IO.FileAccess]::Read,
    [IO.FileShare]::None
)

$cancellation_event = $null
$manual_cancellation_event = !$env:DEVICEFS_BACKUP_STOP_EVENT
try {
    if ($manual_cancellation_event) {
        $env:DEVICEFS_BACKUP_STOP_EVENT =
            "Local\devicefs-backup-stop-$([guid]::NewGuid().ToString('N'))"
        $created_new = $false
        $cancellation_event = [Threading.EventWaitHandle]::new(
            $false,
            [Threading.EventResetMode]::ManualReset,
            $env:DEVICEFS_BACKUP_STOP_EVENT,
            [ref]$created_new
        )
        if (!$created_new) {
            throw 'Could not create the backup cancellation event.'
        }
    } elseif (![Threading.EventWaitHandle]::TryOpenExisting(
            $env:DEVICEFS_BACKUP_STOP_EVENT, [ref]$cancellation_event)) {
        throw 'Could not open the backup supervisor cancellation event.'
    }
    if ($cancellation_event.WaitOne(0)) {
        exit 130
    }

    $vshadow = Get-VShadow
    Write-Host "Found vshadow.exe: ${vshadow}"

    $helper_script = $env:DEVICEFS_BACKUP_VSHADOW_HELPER_PATH
    if (!$helper_script) {
        throw 'The VShadow helper path was not supplied by backup-supervisor.'
    }
    if (!(Test-Path -LiteralPath $helper_script -PathType Leaf)) {
        throw "Helper script missing: ${helper_script}"
    }

    $env:VSHADOW_COMPLETION_SCRIPT =
        $env:DEVICEFS_BACKUP_COMPLETION_SCRIPT_PATH
    if (!$env:VSHADOW_COMPLETION_SCRIPT) {
        throw 'The completion script path was not supplied by backup-supervisor.'
    }
    if (!(Test-Path -LiteralPath $env:VSHADOW_COMPLETION_SCRIPT -PathType Leaf)) {
        throw "Helper script missing: ${env:VSHADOW_COMPLETION_SCRIPT}"
    }

    $env:VSHADOW_PWSH_PATH = [Environment]::ProcessPath
    $env:VSHADOW_TEMP_FILE = Get-SystemTempFile
    try {
        $vshadow_arguments = @(
            if ($NoWriters) {
                '-nw'
            }
            "-script=${env:VSHADOW_TEMP_FILE}"
            "-exec=${helper_script}"
        ) + $BACKUP_DRIVES
        if ($manual_cancellation_event) {
            $vshadow_exit_code = Invoke-VShadowManually `
                -FileName $vshadow `
                -ArgumentList $vshadow_arguments `
                -CancellationEvent $cancellation_event
        } else {
            $PSNativeCommandUseErrorActionPreference = $false
            & $vshadow @vshadow_arguments
            $vshadow_exit_code = $LASTEXITCODE
        }
    } finally {
        Remove-Item -LiteralPath $env:VSHADOW_TEMP_FILE -Force -ErrorAction SilentlyContinue
    }
} finally {
    if ($null -ne $cancellation_event) {
        $cancellation_event.Dispose()
    }
    if ($manual_cancellation_event) {
        Remove-Item Env:DEVICEFS_BACKUP_STOP_EVENT -ErrorAction SilentlyContinue
    }
    $backup_lock.Dispose()
}
exit $vshadow_exit_code
