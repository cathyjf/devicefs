# SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
# SPDX-License-Identifier: GPL-3.0-or-later

function Get-DefaultTestExecutablePath {
    param(
        [Parameter(Mandatory)] [string] $Name,
        [string] $CMakeCachePath,
        [string] $Configuration = 'Release'
    )

    if (-not $CMakeCachePath) {
        # The default build follows the OS architecture, which can differ from
        # the architecture of the PowerShell process.
        $architecture = [Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString().ToLowerInvariant()
        $CMakeCachePath = Join-Path $PSScriptRoot "../../build/windows-$architecture/CMakeCache.txt"
    }
    $CMakeCachePath = [IO.Path]::GetFullPath($CMakeCachePath)
    . (Join-Path $PSScriptRoot '../../src/unix/tests/include/cmake.ps1')
    return [IO.Path]::Combine(
        (Get-CachedPath 'devicefs_BINARY_DIR'), $Configuration, $Name)
}
