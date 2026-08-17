# SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
# SPDX-License-Identifier: GPL-3.0-or-later

function Start-DeviceFsTestProcess {
    param(
        [Parameter(Mandatory)]
        [string] $Executable,

        [Parameter(Mandatory)]
        [string] $MountPath,

        [Parameter(Mandatory)]
        [string] $ReadUser,

        [Parameter(Mandatory)]
        [string] $StopEvent,

        [Parameter(Mandatory)]
        [Collections.IDictionary] $Mappings,

        [switch] $SyntheticFreeClusters
    )

    if ($Mappings.Count -eq 0) {
        throw 'At least one devicefs test mapping is required.'
    }

    $start_info = [Diagnostics.ProcessStartInfo]::new()
    $start_info.FileName = $Executable
    $start_info.WorkingDirectory = [IO.Path]::GetDirectoryName($Executable)
    $start_info.UseShellExecute = $false
    $start_info.CreateNoWindow = $true
    $start_info.RedirectStandardOutput = $true
    $start_info.RedirectStandardError = $true
    foreach ($argument in @(
            '--mount', $MountPath,
            '--read-user', $ReadUser,
            '--stop-event', $StopEvent)) {
        $start_info.ArgumentList.Add($argument)
    }

    $image_paths = [ordered]@{}
    foreach ($mapping in $Mappings.GetEnumerator()) {
        $image_name = [string]$mapping.Key
        $source_device = [string]$mapping.Value
        $start_info.ArgumentList.Add('--map')
        $start_info.ArgumentList.Add($image_name)
        $start_info.ArgumentList.Add($source_device)
        $image_paths[$image_name] = [IO.Path]::Combine($MountPath, $image_name)
    }
    if ($SyntheticFreeClusters) {
        $start_info.ArgumentList.Add('--synthetic-free-clusters')
    }

    $process = [Diagnostics.Process]::Start($start_info)
    return [pscustomobject]@{
        Process = $process
        StandardOutputTask = $process.StandardOutput.ReadToEndAsync()
        StandardErrorTask = $process.StandardError.ReadToEndAsync()
        StopEvent = $StopEvent
        ImagePaths = $image_paths
        OutputLog = "$MountPath.stdout.log"
        ErrorLog = "$MountPath.stderr.log"
        OutputCollected = $false
        StartupExitObserved = $false
    }
}

function Save-TestProcessOutput {
    param(
        [Parameter(Mandatory)]
        $Invocation
    )

    if ($Invocation.OutputCollected) {
        return
    }
    if (-not $Invocation.Process.HasExited) {
        throw "Cannot collect output from running test process " +
            "$($Invocation.Process.Id)."
    }

    $Invocation.Process.WaitForExit()
    $stdout = $Invocation.StandardOutputTask.GetAwaiter().GetResult()
    $stderr = $Invocation.StandardErrorTask.GetAwaiter().GetResult()
    [IO.File]::WriteAllText($Invocation.OutputLog, $stdout)
    [IO.File]::WriteAllText($Invocation.ErrorLog, $stderr)
    $Invocation.OutputCollected = $true
}

function Wait-DeviceFsReady {
    param(
        [Parameter(Mandatory)]
        $Invocation,

        [int] $TimeoutSeconds = 30
    )

    $deadline = [Environment]::TickCount64 + ($TimeoutSeconds * 1000)
    while ([Environment]::TickCount64 -lt $deadline) {
        if ($Invocation.Process.HasExited) {
            $Invocation.StartupExitObserved = $true
            Save-TestProcessOutput $Invocation
            $stdout = [IO.File]::ReadAllText($Invocation.OutputLog)
            $stderr = [IO.File]::ReadAllText($Invocation.ErrorLog)
            throw "devicefs exited during startup with code " +
                "$($Invocation.Process.ExitCode).`n$stdout$stderr"
        }

        $ready = $true
        foreach ($image_path in $Invocation.ImagePaths.Values) {
            $stream = $null
            try {
                $stream = [IO.File]::Open(
                    $image_path,
                    [IO.FileMode]::Open,
                    [IO.FileAccess]::Read,
                    [IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete)
            } catch [IO.IOException] {
                $ready = $false
                break
            } finally {
                if ($null -ne $stream) {
                    $stream.Dispose()
                }
            }
        }
        if ($ready) {
            return
        }

        Start-Sleep -Milliseconds 100
    }

    $paths = @($Invocation.ImagePaths.Values) -join "', '"
    throw "Timed out waiting for '$paths' to become readable."
}

function Stop-DeviceFsTestProcess {
    param(
        [Parameter(Mandatory)]
        $Invocation,

        [int] $TimeoutSeconds = 30
    )

    $process = $Invocation.Process
    $process_id = $process.Id
    if ($process.HasExited) {
        Save-TestProcessOutput $Invocation
        throw "devicefs process $process_id exited before shutdown was requested."
    }

    $signal_error = $null
    $timed_out = $false
    try {
        $event = [Threading.EventWaitHandle]::OpenExisting($Invocation.StopEvent)
        try {
            if (-not $event.Set()) {
                throw "Could not signal '$($Invocation.StopEvent)'."
            }
        } finally {
            $event.Dispose()
        }
    } catch {
        $signal_error = $_
    }

    if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
        $timed_out = $true
        try {
            if (-not $process.HasExited) {
                $process.Kill()
            }
        } catch [InvalidOperationException] {
            if (-not $process.HasExited) {
                throw
            }
        }
        if ((-not $process.HasExited) -and
            (-not $process.WaitForExit($TimeoutSeconds * 1000))) {
            throw "devicefs process $process_id remained alive after termination."
        }
    }

    Save-TestProcessOutput $Invocation
    if ($timed_out) {
        throw "devicefs process $process_id did not stop within " +
            "$TimeoutSeconds seconds."
    }
    if ($null -ne $signal_error) {
        throw $signal_error
    }
    if ($process.ExitCode -ne 0) {
        throw "devicefs process $process_id exited with code " +
            "$($process.ExitCode)."
    }
}
