# SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
# SPDX-License-Identifier: GPL-3.0-or-later

function Get-CachedPath([string] $Name) {
    $record = & grep -m 1 "^${Name}:" -- $CMakeCachePath
    if (($LASTEXITCODE -ne 0) -or $record.EndsWith('-NOTFOUND')) {
        throw "CMake cache '$CMakeCachePath' does not define $Name."
    }
    return ($record -split '=', 2)[1]
}
