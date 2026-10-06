# SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
# SPDX-License-Identifier: GPL-3.0-or-later

function Show-Ipv6Addresses {
    Write-Output 'IPv6 addresses assigned to host:'
    $ipOut = @(ipconfig /allcompartments |
        Select-String 'IPv6 Address' -NoEmphasis)
    if ($LASTEXITCODE -ne 0) {
        $ipOut = @(
            'Failed to query IPv6 addresses: ipconfig exited with code {0}' -f
                $LASTEXITCODE)
    } elseif ($ipOut.Count -eq 0) {
        $ipOut = @('(none)')
    }
    $ipOut | ForEach-Object {
        "    $_"
    }
}
