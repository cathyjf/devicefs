# SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
# SPDX-License-Identifier: GPL-3.0-or-later

# Usage: source pipeline.fish WRITER_ARGV READER_ARGV
#
# Each of WRITER_ARGV and READER_ARGV is a filename. Each file contains a
# null-delimited argument vector describing a command to execute.
#
# The writer process PID and the reader process PID are stored in the variables
# `__writer_pid` and `__reader_pid`, respectively.

functions -q mktemp_autoclean || source (status dirname)/utility.fish

set -l writer_argv (string split0 <$argv[1])
set -l reader_argv (string split0 <$argv[2])

set -l log_file (mktemp_autoclean)
$writer_argv[1] $writer_argv[2..] &>$log_file &
set __writer_pid $last_pid

set -l log_reader '
    set -l parent_pid (ps -o ppid= -p $fish_pid | string trim)
    while pgrep -F (echo -- $argv[1] | psub) -P $parent_pid >/dev/null
        cat -u || return
        sleep 0.1
    end
    cat -u'
set -l fish_argv (status fish-path) -N -c $log_reader $__writer_pid
$fish_argv[1] $fish_argv[2..] <$log_file | $reader_argv[1] $reader_argv[2..] &
set __reader_pid $last_pid
