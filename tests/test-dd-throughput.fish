#!/usr/bin/env -S fish --no-config
# SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
# SPDX-License-Identifier: GPL-3.0-or-later

set -l argv0 (status basename)
argparse -n $argv0 '/internal-run-dd' '/prefix=' -- $argv || exit

function internal_run_dd -a prefix
    function __format_dd_line -a prefix line
        set -l line (string trim $line)
        if test -n "$line"
            printf '[%s]: %s\n' $prefix $line
        end
    end
    __format_dd_line $prefix (string join ' ' dd $argv[2..-1])
    dd $argv[2..-1] 2>&1 | stdbuf -o0 tr '\r' '\n' | while read --line -l line
        __format_dd_line $prefix $line
    end
    set -l dd_status $pipestatus[1]
    if test "$dd_status" -ne 0
        __format_dd_line $prefix (printf 'dd failed with exit status %u.' $dd_status)
    end
    return $dd_status
end

if set -ql _flag_internal_run_dd
    if ! set -ql _flag_prefix
        printf '%s: Error: Argument --prefix is required with --internal-run-dd.\n' $argv0 1>&2
        exit 1
    end
    internal_run_dd $_flag_prefix $argv
    exit
end

if test (count $argv) -eq 0
    printf '%s: Error: At least one filename positional argument must be supplied.\n' $argv0 1>&2
    exit 1
end

set -l data_gibibytes 100
set -l dd_block_mebibytes 4
set -l dd_count_base (math "$data_gibibytes * 1024 / $dd_block_mebibytes")
set -l dd_argv of=/dev/zero bs={$dd_block_mebibytes}M iflag=fullblock

set -l files
set -l dd_count_args
for i in $argv
    echo -- $i | read -d ',' -l filename factor
    if test -z "$filename"
        printf '%s: Error: Positional argument contains no filename: \'%s\'.\n' $argv0 $i 1>&2
        exit 1
    else if test ! -e "$filename"
        printf '%s: Error: File not found: \'%s\'.\n' $argv0 $filename 1>&2
        exit 1
    end
    test -n "$factor" || set -l factor 1
    if ! string match -qr '^[0-9]+(\.[0-9]+)?$' -- $factor
        printf '%s: Error: Specified factor is not a non-negative number: \'%s\'.\n' $argv0 $factor 1>&2
        exit 1
    end
    set -l dd_count (math "round($dd_count_base * $factor)")
    set -a files $filename
    set -a dd_count_args "count=$dd_count"
end

echo 'Testing serial sequential read:'
for i in (seq (count $files))
    set -l filename $files[$i]
    set -l dd_count $dd_count_args[$i]
    internal_run_dd $filename "if=$filename" $dd_argv $dd_count
end
echo

echo 'Testing parallel sequential read:'
set -l fish (status fish-path)
set -l this_script (status filename)
for i in (seq (count $files))
    set -l filename $files[$i]
    set -l dd_count $dd_count_args[$i]
    $fish --no-config $this_script \
        --internal-run-dd --prefix=$filename -- "if=$filename" $dd_argv $dd_count &
end
wait