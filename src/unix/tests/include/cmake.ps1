# SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
# SPDX-License-Identifier: GPL-3.0-or-later

function Get-CachedPath([string] $Name, [switch] $Optional) {
    $record = Select-String -LiteralPath $CMakeCachePath -Pattern "^${Name}:" -List |
        Select-Object -ExpandProperty Line
    if (-not $record -or $record.EndsWith('-NOTFOUND')) {
        if ($Optional) {
            return $null
        }
        throw "CMake cache '$CMakeCachePath' does not define $Name."
    }
    return ($record -split '=', 2)[1]
}
