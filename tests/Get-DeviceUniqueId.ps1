# SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
# SPDX-License-Identifier: GPL-3.0-or-later

#requires -Version 7.4

<#
.SYNOPSIS
Looks up a device's mount manager unique ID, or finds devices by that ID.

.DESCRIPTION
Opens the supplied Win32 device path and sends IOCTL_MOUNTDEV_QUERY_UNIQUE_ID.
Prints the ID's byte length and complete hexadecimal representation.
With -Reverse, sends the hexadecimal ID to IOCTL_MOUNTMGR_QUERY_POINTS and
prints the matching device paths without opening those devices.

.PARAMETER Value
A Win32 device path, such as \\?\GLOBALROOT\Device\HarddiskVolumeShadowCopy36
or \\.\C:. A trailing backslash is accepted and removed before opening.
With -Reverse, a hexadecimal ID in the format printed by the forward lookup.
Hexadecimal letters may be uppercase or lowercase.

.PARAMETER Reverse
Finds device paths from the hexadecimal ID supplied as Value.

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

.EXAMPLE
pwsh .\tests\Get-DeviceUniqueId.ps1 -Reverse '4C0DFEE05ECBF111A222000C2951C1CD'
#>

[CmdletBinding()]
param(
    [Parameter(Mandatory, Position = 0)]
    [Alias('DevicePath')]
    [string] $Value,

    [switch] $Reverse,

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

if ($Reverse) {
    $identifier = [Convert]::FromHexString($Value)
    $devicePaths = @([DeviceFsTestNative]::QueryDevicePathsByUniqueId($identifier))
    if ($devicePaths.Count -eq 0) {
        Write-Host "The mount manager returned no device paths for unique ID '$Value'."
    }
} else {
    $identifier = [DeviceFsTestNative]::QueryDeviceUniqueId($Value)
    $devicePaths = @($Value)
}

$devicePaths | ForEach-Object {
    [pscustomobject]@{
        DevicePath = $_
        UniqueIdLength = $identifier.Length
        UniqueIdHex = [Convert]::ToHexString($identifier)
    }
} | Format-List | Out-Host
