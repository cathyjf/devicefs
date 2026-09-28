#!/usr/bin/env -S fish --no-config
# SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
# SPDX-License-Identifier: GPL-3.0-or-later

set -l magic_subshell_flag --push-image-internal-subshell
set -l magic_reader_flag --push-image-internal-reader
set -l magic_flags $magic_subshell_flag $magic_reader_flag
set -g argv0 (status basename)

function format_date
    date $argv +%FT%T%z
end

function read_while_job_exists -a pid
    set -l parent_pid (ps -o ppid= -p $fish_pid | string trim)
    while pgrep -F (echo -- $pid | psub) -P $parent_pid >/dev/null
        cat || return
        sleep 0.05
    end
    cat
end

function prepend_line_with_timestamp -a line
    printf '[%s] %s\n' (format_date $argv[2..]) $line
end

function escape_argv
    string escape -- $argv | string join ' '
end

function prepend_timestamps -V magic_reader_flag
    prepend_line_with_timestamp (escape_argv $argv)
    set -l log_file (mktemp) || begin
        prepend_line_with_timestamp \
            (printf '%s: failed to create log file; aborting now\n' $argv0)
        exit 1
    end
    function __remove_log_file -e fish_exit -V log_file
        rm -f -- $log_file
    end
    $argv[1] $argv[2..] >$log_file 2>&1 &
    set -l job_pid $last_pid
    set -g job_exit_code 1
    function __handle_child_exit -p $job_pid
        set -g job_exit_code $argv[3]
    end
    set -l fish (status fish-path)
    $fish --no-config (status filename) $magic_reader_flag $job_pid <$log_file | \
        while read -l line
            prepend_line_with_timestamp $line
        end
    wait $job_pid
    return $job_exit_code
end

if ! contains -- "$argv[1]" $magic_flags
    prepend_timestamps (status fish-path) --no-config (status filename) \
        $magic_subshell_flag $argv
    exit
else if test "$argv[1]" = $magic_reader_flag
    read_while_job_exists $argv[2..]
    # The return status of the reader subshell is ignored.
    exit 0
else
    set -e argv[1]
end

set -l podman_build (podman system info --format '{{.Client.Built}}')
if test "$status" -ne 0
    printf '%s: `podman system info` failed.\n' $argv0 1>&2
    exit 1
end

# A very new podman build is required to work around this defect:
#   https://github.com/podman-container-tools/podman/issues/25039
# This commit supplies the fix:
#   https://github.com/podman-container-tools/podman/commit/5745ac69e0b78bec0ea68ef03dd9161d8c51f455
#
# As a very rough proxy for the podman build including the fix, we require the
# Unix timestamp for the build to meet or exceed $minimum_podman_build.
set -l minimum_podman_build 1788542748
if test $podman_build -lt $minimum_podman_build
    printf '%s: podman build is too old; build timestamp %s or newer is required.\n' \
        $argv0 $minimum_podman_build 1>&2
    exit 1
end

argparse '/tag=' -- $argv || exit

if ! set -q _flag_tag
    printf '%s: option `--tag TAG` is required.\n' $argv0 1>&2
    exit 1
end

function __get_tag -a tag
    echo ghcr.io/cathyjf/devicefs-wsl:{$tag}
end

function unix_timestamp_now
    # The format string ensures that the timestamp is an integer.
    printf %d (date +%s)
end

function print_and_invoke
    escape_argv $argv
    $argv[1] $argv[2..]
end

if date --version &>/dev/null
    # The BSD `date` command does not support `--version`.
    function date_timestamp_args -a seconds
        printf %s\n -d @$seconds
    end
else
    function date_timestamp_args -a seconds
        printf %s\n -r $seconds
    end
end

set -l version_tag (__get_tag $_flag_tag)

set -l gh_status (gh auth status 2>&1)
if test "$status" -ne 0
    printf '%s: `gh auth status` failed.\n' $argv0 1>&2
    exit 1
end

if ! string match -q "*'write:packages'*" $gh_status
    printf '%s: GitHub token lacks `write:packages`; run `gh auth refresh -s write:packages`.\n' \
        $argv0 1>&2
    exit 1
else if ! gh auth token | podman login ghcr.io -u cathyjf --password-stdin
    printf '%s: failed to authenticate to ghcr.io.\n' \
        $argv0 1>&2
    exit 1
end

set -l labels \
    "org.opencontainers.image.source=https://github.com/cathyjf/devicefs" \
    "org.opencontainers.image.description=WSL image for the devicefs backup environment"

set -l label_args
set -l annotation_args
for i in $labels
    set -a label_args --label $i
    set -a annotation_args --annotation $i
end

set -l wall_clock_initial (unix_timestamp_now)
printf '%s: wall clock started at %s.\n' $argv0 \
    (format_date (date_timestamp_args $wall_clock_initial))

print_and_invoke podman --log-level=debug farm build \
    --format=oci \
    --local=false \
    --no-cache \
    --force-rm \
    --layers=false \
    --jobs=0 \
    --network=host \
    --squash-all \
    --tag $version_tag \
    $label_args \
    $annotation_args \
    (status dirname) || exit

print_and_invoke podman manifest annotate \
    --index $annotation_args $version_tag || exit

for i in $version_tag (__get_tag latest)
    print_and_invoke podman manifest push \
        --format oci $version_tag docker://{$i} || exit
end

set -l wall_clock_final (unix_timestamp_now)
printf '%s: wall clock stopped at %s.\n' $argv0 \
    (format_date (date_timestamp_args $wall_clock_final))

set -l date_args \
    (date_timestamp_args (math $wall_clock_final - $wall_clock_initial))
# This formatting method assumes that the time elapsed was less than 24 hours.
# That is a reasonable assumption here.
printf '%s: completed successfully in %s.\n' $argv0 (date -u $date_args +%T)