# SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
# SPDX-License-Identifier: GPL-3.0-or-later

# Run DeviceFs against the authenticated helper and a synthetic FAT32 image.
# -TestClient runs the automated protocol and authentication checks instead;
# -GrpcBacking routes those checks through the helper's gRPC backing device.
#requires -Version 7.4

param(
    [string] $CMakeCachePath = [IO.Path]::GetFullPath(
        '../../../build/samba_rpc/CMakeCache.txt', $PSScriptRoot),

    [switch] $TestClient,

    [switch] $GrpcBacking
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
$CMakeCachePath = [IO.Path]::GetFullPath($CMakeCachePath)
if ($GrpcBacking -and -not $TestClient) {
    throw '-GrpcBacking requires -TestClient.'
}
function Get-CachedPath([string] $Name) {
    $record = & grep -m 1 "^${Name}:" -- $CMakeCachePath
    if (($LASTEXITCODE -ne 0) -or $record.EndsWith('-NOTFOUND')) {
        throw "CMake cache does not define $Name."
    }
    return ($record -split '=', 2)[1]
}
function Write-TestPattern(
    [IO.Stream] $Stream,
    [uint64] $Offset,
    [int] $Count
) {
    $bytes = [byte[]]::new($Count)
    for ($index = 0; $index -lt $Count; ++$index) {
        $value = (($Offset + [uint64]$index) * 37 + 11) -band 0xff
        $bytes[$index] = [byte]$value
    }
    $Stream.Position = [int64]$Offset
    $Stream.Write($bytes)
}
function Invoke-TestClient(
    [string] $Executable,
    [string[]] $Arguments,
    [int] $TimeoutMilliseconds
) {
    $start_info = [Diagnostics.ProcessStartInfo]::new()
    $start_info.FileName = $Executable
    $start_info.UseShellExecute = $false
    $start_info.RedirectStandardOutput = $true
    $start_info.RedirectStandardError = $true
    foreach ($argument in $Arguments) {
        $start_info.ArgumentList.Add($argument)
    }

    $process = [Diagnostics.Process]::Start($start_info)
    try {
        $standard_output = $process.StandardOutput.ReadToEndAsync()
        $standard_error = $process.StandardError.ReadToEndAsync()
        $timed_out = -not $process.WaitForExit($TimeoutMilliseconds)
        if ($timed_out) {
            $process.Kill($true)
            $process.WaitForExit()
        }
        return [pscustomobject]@{
            ExitCode = $process.ExitCode
            Output = $standard_output.Result + $standard_error.Result
            TimedOut = $timed_out
        }
    } finally {
        $process.Dispose()
    }
}
$MktempPath = Get-CachedPath 'MKTEMP_EXECUTABLE'
$PdbEditPath = Get-CachedPath 'PDBEDIT_EXECUTABLE'
$SambaDcerpcdPath = Get-CachedPath 'SAMBA_DCERPCD_EXECUTABLE'
$build_directory = [IO.Path]::GetDirectoryName($CMakeCachePath)
$HelperPath = [IO.Path]::Combine($build_directory, 'rpcd_devicefs')
$ClientPath = [IO.Path]::Combine(
    $build_directory, 'rpcd_devicefs_test_client')
$GrpcServerPath = [IO.Path]::Combine(
    $build_directory, 'rpcd_devicefs_grpc_test_server')
if (-not $GrpcBacking) {
    if ($IsMacOS) {
        $HdiutilPath = Get-CachedPath 'HDIUTIL_EXECUTABLE'
    } else {
        $LosetupPath = Get-CachedPath 'LOSETUP_EXECUTABLE'
    }
}
if (-not $TestClient) {
    if (-not $IsLinux) {
        throw 'The DeviceFs fixture mode requires Linux under WSL.'
    }
    $DeviceFsPath = Get-CachedPath 'DEVICEFS_EXECUTABLE'
    $MkfsFatPath = Get-CachedPath 'MKFS_FAT_EXECUTABLE'
    $MountPath = Get-CachedPath 'MOUNT_EXECUTABLE'
    $UmountPath = Get-CachedPath 'UMOUNT_EXECUTABLE'
    $WindowsPowerShellPath =
        Get-CachedPath 'WINDOWS_POWERSHELL_EXECUTABLE'
    $used_drive_letters = [char[]](& $WindowsPowerShellPath -NoProfile -Command `
        '[Console]::Write(((Get-Volume).DriveLetter) -join [string]::Empty)')
    if ($LASTEXITCODE -ne 0) {
        throw 'Could not obtain the mounted Windows drive letters.'
    }
    Write-Host "Mounted Windows drive letters: $($used_drive_letters -join ', ')"
    $mount_drive = 'D'..'Z' |
        Where-Object { -not $used_drive_letters.Contains($_) } |
        Select-Object -First 1
    if ($null -eq $mount_drive) {
        throw 'No Windows drive letter is available for DeviceFs.'
    }
    $devicefs_mount = "${mount_drive}:"
}
$expected = [Text.Encoding]::UTF8.GetBytes("DeviceFs Samba RPC fixture`n")
$backing_length = if ($TestClient) { 1024 * 1024 } else { 64 * 1024 * 1024 }
$password = 'devicefs-fixture-password'
$root = ([string](& $MktempPath --directory)).Trim()
if (($LASTEXITCODE -ne 0) -or ($root.Length -eq 0)) {
    throw 'mktemp could not create the test directory.'
}
foreach ($directory in 'private', 'state', 'cache', 'lock', 'pid', 'ncalrpc') {
    [void][IO.Directory]::CreateDirectory(
        [IO.Path]::Combine($root, $directory))
}
Set-Location $root

$backing = [IO.Path]::Combine($root, 'backing.img')
$backing_stream = [IO.File]::Create($backing)
try {
    $backing_stream.SetLength($backing_length)
    if ($TestClient) {
        $backing_stream.Write($expected)
        Write-TestPattern $backing_stream 4093 73
        Write-TestPattern $backing_stream ($backing_length - 7) 7
    }
} finally {
    $backing_stream.Dispose()
}
$listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, 0)
try {
    $listener.Start()
    $port = ([Net.IPEndPoint]$listener.LocalEndpoint).Port
} finally {
    $listener.Stop()
}

$username = [Environment]::UserName
$username_map = [IO.Path]::Combine($root, 'username.map')
# DeviceFs authenticates with the fixed name "devicefs". Map that name to the
# existing Unix user whose isolated Samba password entry is created below.
[IO.File]::WriteAllText($username_map, "$username = devicefs`n")
$configuration = [IO.Path]::Combine($root, 'smb.conf')
# Samba creates Unix-domain sockets beneath its runtime directories. Relative
# values keep those socket names short when the build directory itself is long.
[IO.File]::WriteAllText($configuration, @"
[global]
    server role = standalone server
    workgroup = DEVICEFS
    netbios name = DEVICEFSTEST
    rpc start on demand helpers = no
    rpc server dynamic port range = $port-$port
    allow dcerpc auth level connect:devicefs_block_device = yes
    interfaces = 127.0.0.1
    bind interfaces only = yes
    passdb backend = smbpasswd:private/smbpasswd
    username map = $username_map
    private dir = private
    state directory = state
    cache directory = cache
    lock directory = lock
    pid directory = pid
    ncalrpc dir = ncalrpc
    log file = log.%m
"@)

"$password`n$password" |
    & $PdbEditPath "--configfile=$configuration" --create `
        "--user=$username" --password-from-stdin
if ($LASTEXITCODE -ne 0) {
    throw "pdbedit failed with exit code $LASTEXITCODE."
}

$grpc_server = $null
$ready_pipe = $null
$server = $null
$device = $null
$mounted_directory = $null
$failure = $null
$show_logs = $false
try {
    if (-not $TestClient) {
        $format_output = & $MkfsFatPath -F 32 $backing 2>&1
        if ($LASTEXITCODE -ne 0) {
            throw "mkfs.fat failed:`n$($format_output | Out-String)"
        }
        $mounted_directory = [IO.Path]::Combine($root, 'volume')
        [void][IO.Directory]::CreateDirectory($mounted_directory)
        $mount_output = & $MountPath -o loop $backing $mounted_directory 2>&1
        if ($LASTEXITCODE -ne 0) {
            $mounted_directory = $null
            throw "mount failed:`n$($mount_output | Out-String)"
        }
        [IO.File]::WriteAllText(
            [IO.Path]::Combine($mounted_directory, 'magic.txt'),
            'hello world from the Samba helper')
        $unmount_output = & $UmountPath $mounted_directory 2>&1
        if ($LASTEXITCODE -ne 0) {
            throw "umount failed:`n$($unmount_output | Out-String)"
        }
        $mounted_directory = $null
    }
    if ($GrpcBacking) {
        $socket = [IO.Path]::Combine($root, 'grpc.sock')
        $grpc_start_info = [Diagnostics.ProcessStartInfo]::new()
        $grpc_start_info.FileName = $GrpcServerPath
        $grpc_start_info.WorkingDirectory = $root
        $grpc_start_info.UseShellExecute = $false
        $grpc_start_info.RedirectStandardOutput = $true
        $grpc_start_info.ArgumentList.Add($socket)
        $grpc_start_info.ArgumentList.Add($backing)
        $grpc_server = [Diagnostics.Process]::Start($grpc_start_info)
        # The server writes this exact path only after BuildAndStart returns a
        # running server. A socket node alone is not the readiness contract.
        $grpc_ready = $grpc_server.StandardOutput.ReadLineAsync()
        if (-not $grpc_ready.Wait(1500)) {
            throw 'The gRPC fixture server did not report readiness.'
        }
        if ($grpc_ready.Result -ne $socket) {
            throw 'The gRPC fixture server returned an invalid readiness record.'
        }
        if ($grpc_server.HasExited) {
            throw 'The gRPC fixture server exited during initialization.'
        }
        $helper_backing = $socket
    } else {
        if ($IsMacOS) {
            $attachment = @(& $HdiutilPath attach -nomount -readonly `
                -imagekey diskimage-class=CRawDiskImage $backing 2>&1)
            if ($LASTEXITCODE -ne 0) {
                throw "hdiutil attach failed:`n$($attachment | Out-String)"
            }
            $device =
                ([string]($attachment | Select-Object -First 1)).Split()[0]
        } else {
            $attachment = @(& $LosetupPath --find --show --read-only `
                $backing 2>&1)
            if ($LASTEXITCODE -ne 0) {
                throw "losetup failed:`n$($attachment | Out-String)"
            }
            $device = ([string]($attachment | Select-Object -First 1)).Trim()
        }
        $helper_backing = $device
    }

    # Only samba-dcerpcd should inherit the client end of this pipe. Creating
    # it after the gRPC process is running prevents that unrelated process from
    # keeping the readiness channel open if Samba exits without signaling.
    $ready_pipe = [IO.Pipes.AnonymousPipeServerStream]::new(
        [IO.Pipes.PipeDirection]::In,
        [IO.HandleInheritability]::Inheritable)
    $start_info = [Diagnostics.ProcessStartInfo]::new()
    $start_info.FileName = $SambaDcerpcdPath
    $start_info.WorkingDirectory = $root
    $start_info.UseShellExecute = $false
    foreach ($argument in @(
            '--foreground',
            '--debug-stdout',
            "--ready-signal-fd=$($ready_pipe.GetClientHandleAsString())",
            "--log-basename=$root",
            "--configfile=$configuration",
            $HelperPath)) {
        $start_info.ArgumentList.Add($argument)
    }
    $start_info.Environment['DEVICEFS_SAMBA_RPC_DEVICE'] = $helper_backing
    $server = [Diagnostics.Process]::Start($start_info)
    $ready_pipe.DisposeLocalCopyOfClientHandle()
    $ready_buffer = [byte[]]::new(1)
    $ready_read = $ready_pipe.ReadAsync($ready_buffer, 0, 1)
    if (-not $ready_read.Wait(1500)) {
        throw 'samba-dcerpcd did not report readiness.'
    }
    $ready = if ($ready_read.Result -eq 0) { -1 } else { $ready_buffer[0] }
    $ready_pipe.Dispose()
    $ready_pipe = $null
    if ($ready -eq -1) {
        throw 'samba-dcerpcd closed the readiness pipe without signaling.'
    }
    if ($server.HasExited) {
        throw 'samba-dcerpcd exited during initialization.'
    }
    Write-Host 'samba-dcerpcd is ready'
    if (-not $TestClient) {
        Write-Host "Temporary directory: $root"
        Write-Host "Backing device: $device"
        Write-Host "RPC endpoint: 127.0.0.1:$port"
        Write-Host "DeviceFs mount point: $devicefs_mount"
        $password | & $DeviceFsPath --vhdx `
            --mount $devicefs_mount `
            --map fixture.vhdx "\\\tcp:127.0.0.1:$port"
        if ($LASTEXITCODE -ne 0) {
            throw "DeviceFs failed with exit code $LASTEXITCODE."
        }
    } else {
        $binding = "ncacn_ip_tcp:127.0.0.1[$port,connect]"
        $client = Invoke-TestClient $ClientPath @(
            $configuration, $binding, $username, $password) 3000
        if ($client.TimedOut) {
            Write-Host 'The authenticated RPC client was still running after 3 seconds.'
            $show_logs = $true
        } else {
            if ($client.ExitCode -ne 0) {
                if ($server.HasExited) {
                    throw 'samba-dcerpcd exited before accepting the connection.'
                }
                throw "The authenticated RPC client failed:`n$($client.Output)"
            }

            $client = Invoke-TestClient $ClientPath @(
                $configuration, $binding, $username, 'wrong-password') 1000
            if ($client.TimedOut) {
                throw 'The unauthenticated RPC client did not exit within 1 second.'
            }
            if ($client.ExitCode -eq 0) {
                throw 'Samba accepted an incorrect password.'
            }
        }
    }
} catch {
    $failure = $_
} finally {
    if ($null -ne $ready_pipe) {
        $ready_pipe.Dispose()
    }
    try {
        if (($null -ne $server) -and (-not $server.HasExited)) {
            & /bin/kill -TERM $server.Id
            if (-not $server.WaitForExit(5000)) {
                $server.Kill($true)
                $server.WaitForExit()
            }
        }
    } catch {
        if ($null -eq $failure) {
            $failure = $_
        } else {
            Write-Warning "Stopping samba-dcerpcd also failed: $_"
        }
    }
    try {
        if (($null -ne $grpc_server) -and (-not $grpc_server.HasExited)) {
            & /bin/kill -TERM $grpc_server.Id
            if (-not $grpc_server.WaitForExit(5000)) {
                $grpc_server.Kill($true)
                $grpc_server.WaitForExit()
            }
        }
    } catch {
        if ($null -eq $failure) {
            $failure = $_
        } else {
            Write-Warning "Stopping the gRPC fixture server also failed: $_"
        }
    }
    if ($null -ne $mounted_directory) {
        $unmount_output = & $UmountPath $mounted_directory 2>&1
        if ($LASTEXITCODE -ne 0) {
            $unmount_failure =
                "umount failed:`n$($unmount_output | Out-String)"
            if ($null -eq $failure) {
                $failure = $unmount_failure
            } else {
                Write-Warning $unmount_failure
            }
        }
    }
    if ($null -ne $device) {
        if ($IsMacOS) {
            $detach_operation = 'hdiutil detach'
            $detach_output = & $HdiutilPath detach $device 2>&1
        } else {
            $detach_operation = 'losetup --detach'
            $detach_output = & $LosetupPath --detach $device 2>&1
        }
        if ($LASTEXITCODE -ne 0) {
            $detach_failure =
                "$detach_operation failed:`n$($detach_output | Out-String)"
            if ($null -eq $failure) {
                $failure = $detach_failure
            } else {
                Write-Warning $detach_failure
            }
        }
    }
    if (($null -ne $failure) -or $show_logs) {
        foreach ($log in Get-ChildItem -LiteralPath $root -Filter 'log.*') {
            Write-Host "Samba log '$($log.Name)':"
            Get-Content -LiteralPath $log.FullName | Write-Host
        }
    }
    Set-Location ([IO.Path]::GetDirectoryName($root))
    Remove-Item -LiteralPath $root -Recurse -Force
}
if ($null -ne $failure) {
    throw $failure
}
