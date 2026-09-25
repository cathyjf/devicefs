# SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
# SPDX-License-Identifier: GPL-3.0-or-later

#requires -Version 7.4

<#
.SYNOPSIS
Reports the current process token and its linked token's user identities.

.DESCRIPTION
Run from PowerShell elevated through Administrator Protection to check whether
the linked token identifies the original, unelevated user. Both accounts' names,
SIDs, and elevation types are printed. The script also works without elevation
for comparison and reports the Windows error if the linked-token query fails.

The script does not impersonate either account or modify permissions.

.EXAMPLE
pwsh .\tests\Test-LinkedToken.ps1
#>

[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if (-not $IsWindows) {
    throw 'This test requires Windows.'
}

if (-not ('DeviceFs.Tests.LinkedTokenProbe' -as [type])) {
    Add-Type -Path (Join-Path $PSScriptRoot 'types/LinkedTokenProbe.cs')
}

Write-Host 'Current process token:'
$current = [DeviceFs.Tests.LinkedTokenProbe]::Current()
$current | Format-List Account, Sid, Elevation | Out-Host

Write-Host 'Linked token:'
$linked = [DeviceFs.Tests.LinkedTokenProbe]::Linked()
$linked | Format-List Account, Sid, Elevation | Out-Host

if ($current.Sid -eq $linked.Sid) {
    Write-Host 'The current and linked tokens belong to the same account.'
} else {
    Write-Host ('The linked token identifies a different account: {0}' -f $linked.Account)
}
