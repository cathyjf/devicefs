# SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
# SPDX-License-Identifier: GPL-3.0-or-later

# The suite runs in a private mount namespace so changing its root mount flags
# cannot change the host's mounts. The test requires an initially writable root
# and Fish available on the PATH.
#requires -Version 7.5

param(
    [string] $CMakeCachePath = [IO.Path]::GetFullPath(
        '../../../build/unix/CMakeCache.txt', $PSScriptRoot)
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
if (-not $IsLinux) {
    throw 'The WSL startup broker test requires GNU/Linux mount namespaces.'
}
$CMakeCachePath = [IO.Path]::GetFullPath($CMakeCachePath)
. "$PSScriptRoot/include/cmake.ps1"
$UnsharePath = Get-CachedPath 'UNSHARE_EXECUTABLE'
$PowerShellPath = Get-CachedPath 'POWERSHELL_EXECUTABLE'

$suite = {
    param([string] $CMakeCachePath, [string] $CacheInclude)

    Set-StrictMode -Version Latest
    $ErrorActionPreference = 'Stop'
    $PSNativeCommandUseErrorActionPreference = $false
    . $CacheInclude
    $MountPath = Get-CachedPath 'MOUNT_EXECUTABLE'
    $FindmntPath = Get-CachedPath 'FINDMNT_EXECUTABLE'
    $BrokerPath = [IO.Path]::Combine(
        (Get-CachedPath 'devicefs-unix_BINARY_DIR'), 'devicefs-wsl-startup-broker')
    Write-Host "Testing startup broker '$BrokerPath'."

    function Invoke-Mount([string[]] $Arguments) {
        & $MountPath @Arguments
        if ($LASTEXITCODE -ne 0) {
            throw "Mount command '$MountPath $($Arguments -join ' ')' failed; " +
                "exit code: $LASTEXITCODE."
        }
    }

    $options = & $FindmntPath --noheadings --output VFS-OPTIONS --target /
    if (($LASTEXITCODE -ne 0) -or (($options -split ',') -notcontains 'rw')) {
        throw 'The startup broker test requires an initially writable root mount.'
    }
    # The test directory and PowerShell's temporary files must stay writable when
    # this namespace's root mount becomes read-only. A bind mount gives the
    # temporary directory independent mount flags without changing its storage.
    $temporary_path = [IO.Path]::GetTempPath()
    Invoke-Mount -Arguments @('--bind', $temporary_path, $temporary_path)

    $script = @'
read --local input
test "$input" = 'standard input'; or exit 91
test "$DEVICEFS_BROKER_TEST" = 'environment value'; or exit 92
test "$PWD" = "$HOME"; or exit 93
test (count $argv) -eq 2; or exit 94
test "$argv[1]" = 'argument with spaces'; or exit 95
test "$argv[2]" = '--literal-option'; or exit 96
echo $fish_pid
echo 'Fish standard error' >&2
exit 37
'@
    $directory = [IO.Directory]::CreateTempSubdirectory('devicefs-startup-test-')
    try {
        $start_info = [Diagnostics.ProcessStartInfo]::new()
        $start_info.FileName = $BrokerPath
        $start_info.UseShellExecute = $false
        $start_info.RedirectStandardInput = $true
        $start_info.RedirectStandardOutput = $true
        $start_info.RedirectStandardError = $true
        $start_info.WorkingDirectory = $directory.FullName
        $start_info.Environment['HOME'] = $directory.FullName
        $start_info.Environment['XDG_CONFIG_HOME'] = "$($directory.FullName)/config"
        $start_info.Environment['XDG_CACHE_HOME'] = "$($directory.FullName)/cache"
        $start_info.Environment['XDG_DATA_HOME'] = "$($directory.FullName)/data"
        $start_info.Environment['DEVICEFS_BROKER_TEST'] = 'environment value'
        foreach ($argument in '--no-config', '-c', $script, '--',
                'argument with spaces', '--literal-option') {
            [void]$start_info.ArgumentList.Add($argument)
        }

        foreach ($scenario in 'read-only', 'becomes-read-only', 'writable') {
            Write-Host ''
            switch ($scenario) {
                'read-only' {
                    Write-Host "Testing an already read-only root: Fish should start without a broker warning."
                }
                'becomes-read-only' {
                    Write-Host "Testing a delayed remount: '/' becomes read-only after 250 ms, then Fish should run without a broker warning."
                }
                'writable' {
                    Write-Host "Testing a root that stays writable: the broker should warn and start Fish within the ten-second completion limit."
                }
            }
            $flags = if ($scenario -eq 'read-only') { 'ro' } else { 'rw' }
            Invoke-Mount -Arguments @('-o', "remount,bind,$flags", '/')
            $elapsed = [Diagnostics.Stopwatch]::StartNew()
            $process = [Diagnostics.Process]::Start($start_info)
            try {
                $output = $process.StandardOutput.ReadToEndAsync()
                $errors = $process.StandardError.ReadToEndAsync()
                $process.StandardInput.WriteLine('standard input')
                $process.StandardInput.Close()
                if ($scenario -eq 'becomes-read-only') {
                    Start-Sleep -Milliseconds 250
                    if ($process.HasExited) {
                        throw "The process exited before the test remounted '/' read-only."
                    }
                    Write-Host "PASS: the process has not exited before the read-only remount."
                    Invoke-Mount -Arguments @('-o', 'remount,bind,ro', '/')
                }
                if (-not $process.WaitForExit(10000)) {
                    throw 'The broker did not finish within ten seconds.'
                }
                $elapsed.Stop()
                Write-Host ('PASS: the process completed in {0:F2} seconds (ten-second wait limit).' -f
                    $elapsed.Elapsed.TotalSeconds)
                if (($process.ExitCode -ne 37) -or
                        ($output.Result.Trim() -ne "$($process.Id)")) {
                    throw "Fish did not retain the broker's PID and inputs, or its exit " +
                        "status was lost. Exit code: $($process.ExitCode); " +
                        "stdout: '$($output.Result)'; stderr: '$($errors.Result)'."
                }
                Write-Host "PASS: Fish retained process ID $($process.Id) and returned the expected exit status 37."
                Write-Host 'PASS: stdin, the environment variable, working directory, and arguments containing spaces and a leading -- were preserved.'
                $expected_error = if ($scenario -eq 'writable') {
                    "devicefs-wsl-startup-broker: '/' is still writable " +
                        "after waiting *; starting Fish anyway`nFish standard error`n"
                } else {
                    "Fish standard error`n"
                }
                if ($errors.Result -cnotlike $expected_error) {
                    throw "Unexpected stderr for '$scenario': '$($errors.Result)'."
                }
                if ($scenario -eq 'writable') {
                    Write-Host "PASS: the broker warned that '/' remained writable and forwarded Fish's stderr."
                } else {
                    Write-Host "PASS: Fish's stderr was forwarded without a broker warning."
                }
            } finally {
                if (-not $process.HasExited) {
                    $process.Kill($true)
                    $process.WaitForExit()
                }
                $process.Dispose()
            }
        }
        Write-Host ''
        Write-Host 'All three startup broker scenarios passed.'
    } finally {
        $directory.Delete($true)
    }
}

# WSL1 reports its kernel release as `4.4.0-<Windows build number>-Microsoft`.
# WSL1 supports mount namespaces but not user namespaces, so on WSL1 this test
# omits the user namespace and relies on the caller having root privileges.
$namespace_arguments = @(
    if ((& uname -r) -cnotmatch '\A4\.4\.0-[0-9]+-Microsoft\z') {
        '--map-root-user'
    }
    '--mount'
    '--propagation'
    'private'
)
Write-Host "Creating the test namespaces: $UnsharePath $($namespace_arguments -join ' ')"
& $UnsharePath @namespace_arguments -- `
    $PowerShellPath -NoLogo -NoProfile -CommandWithArgs ($suite.ToString()) `
    $CMakeCachePath "$PSScriptRoot/include/cmake.ps1"
if ($LASTEXITCODE -ne 0) {
    [Console]::Error.WriteLine("Startup broker tests failed; exit code: $LASTEXITCODE.")
}
exit $LASTEXITCODE
