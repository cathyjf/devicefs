# SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
# SPDX-License-Identifier: GPL-3.0-or-later

# Usage: source utility.fish
#
# The variable `argv0` should be defined before sourcing this program.
# It specifies the name to identify the program with in log messages.

function log -V argv0
    printf '%s: %s.\n' $argv0 (string join ' ' $argv)
end

function die
    set -l exit_status $status
    test "$exit_status" -ne 0 || set -l exit_status 1
    log $argv >&2
    exit $exit_status
end

function mktemp_autoclean
    set -l temp_file (mktemp $argv) || die 'failed to create temporary path'
    set -gq mktemp_autoclean_index || set -g mktemp_autoclean_index 0
    set -g mktemp_autoclean_index (math $mktemp_autoclean_index + 1)
    function __remove_temp_file_{$mktemp_autoclean_index} -e fish_exit -V temp_file
        rm -rf -- $temp_file
    end
    echo -- $temp_file
end
