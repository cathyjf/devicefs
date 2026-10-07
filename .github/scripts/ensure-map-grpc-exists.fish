# SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
# SPDX-License-Identifier: GPL-3.0-or-later

proxmox-backup-client help map-grpc |
    read --line |
        grep -q '^Usage: map-grpc'

for i in $pipestatus
    test $i -eq 0 || {
        set -l exit_status $status
        printf 'Failure: `ensure-map-grpc-exists.fish` failed.\n' >&2
        exit $exit_status
    }
end

printf %s 'Success: the `proxmox-backup-client` binary provisioned by the ' \
    'installer contains the `map-grpc` command.'\n || true
