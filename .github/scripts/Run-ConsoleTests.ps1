# SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
# SPDX-License-Identifier: GPL-3.0-or-later

param(
    [Parameter(Mandatory, Position = 0)]
    [string]$Command
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# Some tests require standard input to be attached to a console.
# To satisfy that need, we run the test command inside `conhost.exe`.
# This requires creating a temporary file to collect the exit status.
$env:DEVICEFS_TEST_RESULT = Join-Path $env:RUNNER_TEMP `
    "console-tests-$([guid]::NewGuid()).exit"

$start = [Diagnostics.ProcessStartInfo]::new('conhost.exe')
$start.UseShellExecute = $false
$start.CreateNoWindow = $true
$start.RedirectStandardInput = $true
foreach ($argument in @(
    '--headless', 'pwsh', '-NoProfile', '-NonInteractive', '-Command',
    (
        '$ErrorActionPreference = "Stop"; ' + $Command + "`n" +
        '$LASTEXITCODE | Set-Content -LiteralPath $env:DEVICEFS_TEST_RESULT'
    )
)) {
    $start.ArgumentList.Add($argument)
}

$process = [Diagnostics.Process]::Start($start)
try {
    $process.WaitForExit()
} finally {
    $process.Dispose()
}

exit [int](Get-Content -LiteralPath $env:DEVICEFS_TEST_RESULT)
