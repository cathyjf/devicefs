# SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
# SPDX-License-Identifier: GPL-3.0-or-later

function show_ipv6_addresses {
    printf 'IPv6 addresses assigned to host:\n'
    local ip_out
    ip_out=$(
        if [[ $(uname) == 'Darwin' ]]; then
            ifconfig -a -f inet6:cidr inet6 | \
                grep --color=never --only-matching 'inet6.*' || true
        else
            ip --color -6 address show
        fi
    )
    printf '%s\n' "${ip_out:-(none)}" | awk '{print "    " $0}'
}
