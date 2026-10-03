# SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
# SPDX-License-Identifier: GPL-3.0-or-later

#requires -Version 7.4

param(
    [Parameter(Mandatory)]
    [string]$rootfs,

    [string]$logs_directory = [IO.Path]::Combine( `
        [IO.Path]::GetTempPath(), [guid]::NewGuid().ToString())
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $true
# Set wsl.exe's stdout and stderr encoding to UTF-8.
# See <https://github.com/microsoft/WSL/discussions/8641>.
$env:WSL_UTF8 = '1'

$test_script = @'
checkout=$1
log_directory=$2

rm -rf /usr/src/devicefs || exit
ln -s "${checkout}" /usr/src/devicefs || exit

/opt/cmake/bin/ctest \
    --presets-file /usr/src/devicefs/src/unix/CMakePresets.json \
    --preset release --test-dir /usr/src/devicefs-build \
    --no-tests=error \
    -R '^rpcd_devicefs_grpc_authenticated$'
test_status=$?

cp -r /usr/src/devicefs-build/Testing "${log_directory}/"
exit "${test_status}"
'@

function ConvertTo-WslPath {
    param(
        [string]$distribution,
        [string]$path
    )

    (wsl.exe -d $distribution -u root -e wslpath -u $path).Trim()
}

function New-RandomTemporaryDirectory {
    $directory = [IO.Path]::Combine( `
        [IO.Path]::GetTempPath(), [guid]::NewGuid().ToString())
    [IO.Directory]::CreateDirectory($directory) | Out-Null
    return $directory
}

$tester = "devicefs-samba-test-$([guid]::NewGuid().ToString())"
Write-Host 'Importing the test environment into WSL1'
$tester_directory = New-RandomTemporaryDirectory
try {
    wsl.exe --import $tester $tester_directory $rootfs --version 1
    [IO.Directory]::CreateDirectory($logs_directory) | Out-Null
    Write-Host (
        "Running tests under WSL1; logs will be retained in '{0}'" -f
        $logs_directory)
    wsl.exe -d $tester -u root -e /bin/sh -uc $test_script argv0 `
        (ConvertTo-WslPath $tester (Get-Location).ProviderPath) `
        (ConvertTo-WslPath $tester $logs_directory)
} finally {
    try {
        wsl.exe --unregister $tester
    } catch {}
    Remove-Item -ErrorAction SilentlyContinue -Recurse -Force `
        -Path $tester_directory
}
