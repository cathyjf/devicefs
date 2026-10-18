# SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
# SPDX-License-Identifier: GPL-3.0-or-later

#requires -Version 7.4

<#
.SYNOPSIS
Prints the mount manager unique ID associated with a specified device.

.DESCRIPTION
Opens the supplied Win32 device path and sends IOCTL_MOUNTDEV_QUERY_UNIQUE_ID.
Prints the ID's byte length and complete hexadecimal representation.

.PARAMETER DevicePath
A Win32 device path, such as \\?\GLOBALROOT\Device\HarddiskVolumeShadowCopy36
or \\.\C:. A trailing backslash is accepted and removed before opening.

.PARAMETER SupervisorPath
Overrides the supervisor path obtained from the CMake build directory.
The supervisor is not executed. The directory of the supervisor is assumed to
contain the generated CsWin32 bindings.

.PARAMETER CMakeCachePath
The build's CMakeCache.txt. Defaults to the native-architecture Windows build.

.PARAMETER Configuration
The build configuration containing the generated bindings. Defaults to Release.

.EXAMPLE
pwsh .\tests\Get-DeviceUniqueId.ps1 '\\?\GLOBALROOT\Device\HarddiskVolumeShadowCopy36'
#>

[CmdletBinding()]
param(
    [Parameter(Mandatory, Position = 0)]
    [string] $DevicePath,

    [string] $SupervisorPath,

    [string] $CMakeCachePath,

    [string] $Configuration = 'Release'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

. (Join-Path $PSScriptRoot 'include/DeviceFsTestTypes.ps1')

Add-DeviceFsTestTypes -SourcePath (Join-Path $PSScriptRoot 'types/DeviceFsTestNative.cs') `
    -SupervisorPath $SupervisorPath -CMakeCachePath $CMakeCachePath `
    -Configuration $Configuration

$identifier = [DeviceFsTestNative]::QueryDeviceUniqueId($DevicePath)
[pscustomobject]@{
    DevicePath = $DevicePath
    UniqueIdLength = $identifier.Length
    UniqueIdHex = [Convert]::ToHexString($identifier)
} | Format-List | Out-Host
