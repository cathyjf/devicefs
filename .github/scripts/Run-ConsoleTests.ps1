# SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
# SPDX-License-Identifier: GPL-3.0-or-later

param(
    [Parameter(Mandatory, Position = 0)]
    [string]$Command
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# Some tests require standard input to be attached to a console.
& ('build/windows-{0}/Release/backup-supervisor.exe' -f
    $env:RUNNER_ARCH.ToLowerInvariant()) --internal-run-in-console -- pwsh `
    -NoProfile -NonInteractive -Command (
        '$ErrorActionPreference = "Stop"; ' + $Command + "`n" +
        'exit $LASTEXITCODE')
exit $LASTEXITCODE
