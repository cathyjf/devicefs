# SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
# SPDX-License-Identifier: GPL-3.0-or-later

param(
    [Parameter(Mandatory)]
    [string]$BuildDirectory,

    [Parameter(Mandatory)]
    [string]$ArtifactDirectory
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$BuildDirectory = [IO.Path]::GetFullPath($BuildDirectory)
$CMakeCachePath = Join-Path $BuildDirectory 'CMakeCache.txt'
. "$PSScriptRoot/../../src/unix/tests/include/cmake.ps1"

$files = Get-ChildItem -LiteralPath $BuildDirectory -Recurse -File |
    Where-Object {
        $_.Name -eq 'CTestTestfile.cmake' -or
        ($_.Directory.Name -eq 'Release' -and $_.Extension -eq '.exe')
    }
$files += Get-Item -LiteralPath (Join-Path $BuildDirectory `
    'Release/Windows.Win32.NativeMethods.g.cs')
foreach ($file in $files) {
    $destination = Join-Path $ArtifactDirectory `
        ([IO.Path]::GetRelativePath($BuildDirectory, $file.FullName))
    [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($destination)) |
        Out-Null
    Copy-Item -LiteralPath $file.FullName -Destination $destination
}

# CTest records absolute paths to executables and source scripts. The tester
# creates a symlink at the original checkout path when its checkout differs,
# so those paths remain valid without modifying CMake's generated files.
[IO.File]::WriteAllText((Join-Path $ArtifactDirectory 'source-directory.txt'),
    (Get-CachedPath 'devicefs_SOURCE_DIR'))
