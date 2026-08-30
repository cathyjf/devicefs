# SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
# SPDX-License-Identifier: GPL-3.0-or-later

# Run the authenticated helper against synthetic Samba state and backing data.
#requires -Version 7.4

param(
    [Parameter(Mandatory)]
    [string] $MktempPath,

    [Parameter(Mandatory)]
    [string] $PdbEditPath,

    [Parameter(Mandatory)]
    [string] $SambaDcerpcdPath,

    [Parameter(Mandatory)]
    [string] $HelperPath,

    [Parameter(Mandatory)]
    [string] $ClientPath,

    [string] $HdiutilPath,

    [string] $LosetupPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
$expected = [Text.Encoding]::UTF8.GetBytes("DeviceFs Samba RPC fixture`n")
$backing_length = 1024 * 1024
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
    $backing_stream.Write($expected)
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
    private dir = private
    state directory = state
    cache directory = cache
    lock directory = lock
    pid directory = pid
    ncalrpc dir = ncalrpc
    log file = samba.log
"@)

$username = [Environment]::UserName
"$password`n$password" |
    & $PdbEditPath "--configfile=$configuration" --create `
        "--user=$username" --password-from-stdin
if ($LASTEXITCODE -ne 0) {
    throw "pdbedit failed with exit code $LASTEXITCODE."
}

$start_info = [Diagnostics.ProcessStartInfo]::new()
$start_info.FileName = $SambaDcerpcdPath
$start_info.WorkingDirectory = $root
$start_info.UseShellExecute = $false
foreach ($argument in @(
        '--foreground',
        '--debug-stdout',
        "--configfile=$configuration",
        $HelperPath)) {
    $start_info.ArgumentList.Add($argument)
}

$server = $null
$device = $null
$failure = $null
try {
    if ($IsMacOS) {
        $attachment = @(& $HdiutilPath attach -nomount -readonly `
            -imagekey diskimage-class=CRawDiskImage $backing 2>&1)
        if ($LASTEXITCODE -ne 0) {
            throw "hdiutil attach failed:`n$($attachment | Out-String)"
        }
        $device = ([string]($attachment | Select-Object -First 1)).Split()[0]
    } else {
        $attachment = @(& $LosetupPath --find --show --read-only `
            $backing 2>&1)
        if ($LASTEXITCODE -ne 0) {
            throw "losetup failed:`n$($attachment | Out-String)"
        }
        $device = ([string]($attachment | Select-Object -First 1)).Trim()
    }
    $start_info.Environment['DEVICEFS_SAMBA_RPC_DEVICE'] = $device
    $server = [Diagnostics.Process]::Start($start_info)
    $binding = "ncacn_ip_tcp:127.0.0.1[$port,connect]"
    for ($attempt = 0; $attempt -lt 50; ++$attempt) {
        $client_output =
            & $ClientPath $configuration $binding $username $password 2>&1 |
            Out-String
        $client_exit_code = $LASTEXITCODE
        if ($client_exit_code -eq 0) {
            break
        }
        if ($server.HasExited) {
            throw "samba-dcerpcd exited before accepting the connection."
        }
        Start-Sleep -Milliseconds 100
    }
    if ($client_exit_code -ne 0) {
        throw "The authenticated RPC client failed:`n$client_output"
    }

    & $ClientPath $configuration $binding $username 'wrong-password' 2>&1 |
        Out-Null
    if ($LASTEXITCODE -eq 0) {
        throw 'Samba accepted an incorrect password.'
    }
} catch {
    $failure = $_
} finally {
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
    Set-Location ([IO.Path]::GetDirectoryName($root))
    Remove-Item -LiteralPath $root -Recurse -Force
}
if ($null -ne $failure) {
    throw $failure
}
