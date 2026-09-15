# SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
# SPDX-License-Identifier: GPL-3.0-or-later

set -l bundle "$argv[1]"
set -l codesign "$argv[2]"
set -l identity "$argv[3]"
set -l entitlements "$argv[4]"
set -g cancellation_status 0

function cleanup --on-event fish_exit --inherit-variable bundle
    /bin/rm -rf -- "$bundle" || printf 'Cannot remove failed signed bundle: %s\n' "$bundle" >&2
end

function interrupted --on-signal INT
    set -g cancellation_status 130
end

function terminated --on-signal TERM
    set -g cancellation_status 143
end

"$codesign" --force --sign "$identity" --entitlements "$entitlements" "$bundle" || exit
if test $cancellation_status -ne 0
    exit $cancellation_status
end
functions --erase cleanup
