# SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
# SPDX-License-Identifier: GPL-3.0-or-later

param([Parameter(Mandatory)][string] $Executable)

$ErrorActionPreference = 'Stop'

# An isolated hidden console lets the tests change modes and insert input
# records without consuming keys or changing the console running CTest.
$start = [Diagnostics.ProcessStartInfo]::new($Executable)
$start.UseShellExecute = $false
$start.CreateNoWindow = $true
$start.RedirectStandardOutput = $true
$start.RedirectStandardError = $true
$start.ArgumentList.Add('--native-test')
$process = [Diagnostics.Process]::Start($start)
try {
    $output = $process.StandardOutput.ReadToEndAsync()
    $errors = $process.StandardError.ReadToEndAsync()
    $process.WaitForExit()
    [Console]::Out.Write($output.Result)
    [Console]::Error.Write($errors.Result)
    exit $process.ExitCode
} finally {
    $process.Dispose()
}
