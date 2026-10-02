# SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
# SPDX-License-Identifier: GPL-3.0-or-later

# CsWin32's NuGet package includes a command line generator. Its output is
# compiled together with the test helpers by PowerShell's `Add-Type`.
# https://github.com/microsoft/CsWin32/tree/v0.3.335/src/CsWin32Generator
param(
    [string] $CMakeCachePath = [IO.Path]::GetFullPath(
        "../../build/windows-$([Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString().ToLowerInvariant())/CMakeCache.txt",
        $PSScriptRoot),
    [string] $Configuration = 'Release'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $true

$CMakeCachePath = [IO.Path]::GetFullPath($CMakeCachePath)
. "$PSScriptRoot/../../src/unix/tests/include/cmake.ps1"
$NuGet = Get-CachedPath 'NUGET_EXECUTABLE' -Optional
$DotNet = Get-CachedPath 'DOTNET_EXECUTABLE' -Optional
if (-not $NuGet -or -not $DotNet) {
    if (-not $NuGet) {
        Write-Warning 'Skipping CsWin32 generation: NuGet was not found. Run `winget install --id Microsoft.NuGet` as the user who will build the project, then rerun CMake configuration and build.'
    }
    if (-not $DotNet) {
        Write-Warning 'Skipping CsWin32 generation: dotnet was not found. Install a .NET runtime, then rerun CMake configuration and build.'
    }
    return
}
$BuildDirectory = Get-CachedPath 'devicefs_BINARY_DIR'
$PackagesDirectory = Join-Path $BuildDirectory 'packages'
$OutputDirectory = Join-Path $BuildDirectory $Configuration
$Platform = (Get-CachedPath 'CMAKE_GENERATOR_PLATFORM').ToLowerInvariant()

& $NuGet restore (Join-Path $PSScriptRoot 'packages.config') `
    -PackagesDirectory $PackagesDirectory -NonInteractive `
    -LockedMode -LockFilePath (Join-Path $PSScriptRoot 'packages.lock.json') `
    -ConfigFile (Join-Path $PSScriptRoot 'NuGet.Config')

[xml] $packages = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'packages.config') -Raw
$paths = @{}
foreach ($package in $packages.packages.package) {
    $paths[$package.id] = Join-Path $PackagesDirectory "$($package.id).$($package.version)"
}

# The same reference assemblies used by `Add-Type` let CsWin32 generate
# overloads for the types available in the PowerShell runtime, including spans
# and safe handles. DllImport declarations need no further generation pass.
$references = (Get-ChildItem -LiteralPath (Join-Path $PSHOME 'ref') -Filter '*.dll').FullName
& $DotNet (Join-Path $paths['Microsoft.Windows.CsWin32'] 'build/tools/CsWin32Generator.dll') `
    --native-methods-txt (Join-Path $PSScriptRoot 'NativeMethods.txt') `
    --native-methods-json (Join-Path $PSScriptRoot 'NativeMethods.json') `
    --metadata-paths (Join-Path $paths['Microsoft.Windows.SDK.Win32Metadata'] 'Windows.Win32.winmd') `
    --output-path $OutputDirectory --platform $Platform --references $references `
    --language-version 12
