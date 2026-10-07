# SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
# SPDX-License-Identifier: GPL-3.0-or-later

# Usage: source pipeline.fish ptr_writer_argv ptr_reader_argv
#
# Each of `ptr_writer_argv` and `ptr_reader_argv` is the name of an array.
# Each array contains the argument vector of a command to execute.
#
# The writer process PID and the reader process PID are stored in the variables
# `__writer_pid` and `__reader_pid`, respectively.

functions -q mktemp_autoclean || source (status dirname)/utility.fish

set -l log_file (mktemp_autoclean)
$$argv[1] &>$log_file &
set __writer_pid $last_pid

set -l log_reader '
    set -l parent_pid (ps -o ppid= -p $fish_pid | string trim)
    while pgrep -F (echo -- $argv[1] | psub) -P $parent_pid >/dev/null
        cat -u || return
        sleep 0.1
    end
    cat -u'
set -l fish_argv (status fish-path) -N -c $log_reader $__writer_pid
$fish_argv <$log_file | $$argv[2] &
set __reader_pid $last_pid
