#!/usr/bin/env -S fish -N
# SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
# SPDX-License-Identifier: GPL-3.0-or-later

set -l magic_subshell_flag --push-image-internal-subshell
set -l magic_reader_flag --push-image-internal-reader
set -l magic_literal_prepend_flag --push-image-internal-literal-prefix-prepend-subshell
set -l magic_flags $magic_subshell_flag $magic_reader_flag $magic_literal_prepend_flag
set -l argv0 (status basename)

function format_date
    date $argv +%FT%T%z
end

function read_while_job_exists -a pid
    set -l parent_pid (ps -o ppid= -p $fish_pid | string trim)
    while pgrep -F (echo -- $pid | psub) -P $parent_pid >/dev/null
        cat || return
        sleep 0.1
    end
    cat
end

function prepend_line_with_timestamp -a line
    printf '[%s] %s\n' (format_date $argv[2..]) $line
end

function escape_argv
    echo -n -- '+ '
    string escape -- $argv | string join ' '
end

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
    set -l temp_file (mktemp) || die 'failed to create temp file'
    set -gq mktemp_autoclean_index || set -g mktemp_autoclean_index 0
    set -g mktemp_autoclean_index (math $mktemp_autoclean_index + 1)
    function __remove_temp_file_{$mktemp_autoclean_index} -e fish_exit -V temp_file
        rm -f -- $temp_file
    end
    echo -- $temp_file
end

function prepend_timestamps -V magic_reader_flag
    prepend_line_with_timestamp (escape_argv $argv)
    set -l log_file (mktemp_autoclean)
    $argv[1] $argv[2..] >$log_file 2>&1 &
    set -l job_pid $last_pid
    set -g job_exit_code 1
    function __handle_child_exit -p $job_pid
        set -g job_exit_code $argv[3]
    end
    set -l fish (status fish-path)
    $fish -N (status filename) $magic_reader_flag $job_pid <$log_file | \
        while read -l line
            prepend_line_with_timestamp $line
        end
    wait $job_pid
    return $job_exit_code
end

function prepend_literal_prefixes -a prefix
    while read -l line
        echo -- $prefix $line
    end
end

if ! contains -- "$argv[1]" $magic_flags
    prepend_timestamps (status fish-path) -N (status filename) \
        $magic_subshell_flag $argv
    exit
else if test "$argv[1]" = $magic_reader_flag
    read_while_job_exists $argv[2..]
    # The return status of the reader subshell is ignored.
    exit 0
else if test "$argv[1]" = $magic_literal_prepend_flag
    prepend_literal_prefixes $argv[2..]
    # The return status of the literal prepending subshell is ignored.
    exit 0
else
    set -e argv[1]
end

set -l image_repository ghcr.io/cathyjf/devicefs-wsl
function qualified_tag -a tag -V image_repository
    echo $image_repository:{$tag}
end

argparse -n $argv0 '/tag=' -- $argv || exit
if ! set -q _flag_tag
    set -l registry_tags (skopeo list-tags docker://{$image_repository}) || \
        die 'failed to list tags for' $image_repository 'to select a default tag'
    set -l existing_tags (printf '%s\n' $registry_tags | jq -r '.Tags[]') || \
        die 'failed to read the tag list for' $image_repository
    set -l date_tag (date +%Y%m%d) || \
        die 'failed to determine the date for the default tag'
    set _flag_tag $date_tag
    set -l suffix 0
    while contains -- $_flag_tag $existing_tags
        set suffix (math $suffix + 1)
        set _flag_tag {$date_tag}.{$suffix}
    end
end
set -l version_tag (qualified_tag $_flag_tag)
log "using tag '$version_tag'"

function unix_timestamp_now
    set -l timestamp (date +%s)
    printf %d $timestamp || \
        die 'unix timestamp was not an integer:' $timestamp
end

function podman_
    set -l invoke_argv podman $argv
    escape_argv $invoke_argv
    $invoke_argv[1] $invoke_argv[2..]
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

set -l gh_status (gh auth status 2>&1) || \
    die '`gh auth status` failed:' $gh_status
if ! string match -q "*'write:packages'*" $gh_status
    die 'GitHub token lacks `write:packages`; run `gh auth refresh -s write:packages`'
else if ! gh auth token | podman login ghcr.io -u cathyjf --password-stdin
    die 'failed to authenticate to ghcr.io'
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

set -l builders \
    (podman farm list --format \
        '{{if .Default}}{{range .Connections}}{{println .}}{{end}}{{end}}') || \
            die 'failed to list configured farms'
set -l fish (status fish-path)
set -l podman_pids
set -l build_connections
set -l image_id_files
set -g podman_exit_codes
for builder in $builders
    test -n "$builder" || continue
    # `hostname` is a reserved variable, so we use `hostname_` instead.
    string split ',' $builder | read --line -l hostname_ platform
    set -l platform_args
    test -z "$platform" || set -l platform_args --platform $platform
    set -l image_id_file (mktemp_autoclean)
    set -l podman_argv podman --connection $hostname_ build \
        $platform_args \
        --iidfile $image_id_file \
        --format=oci \
        --no-cache \
        --force-rm \
        --layers=false \
        --jobs=0 \
        --network=host \
        --squash-all \
        $label_args \
        $annotation_args \
        (status dirname)
    set -l prefix (printf '[%s]' $builder)
    escape_argv $podman_argv | prepend_literal_prefixes $prefix
    set -l log_file (mktemp_autoclean)
    $podman_argv[1] $podman_argv[2..] &>$log_file &
    set -a podman_pids $last_pid
    set -a build_connections $hostname_
    set -a image_id_files $image_id_file
    set -a podman_exit_codes 1
    set -l index (count $podman_exit_codes)
    function __handle_podman_exit_{$index} -p $podman_pids[$index] -V index
        set -g podman_exit_codes[$index] $argv[3]
    end
    $fish -N (status filename) $magic_reader_flag $podman_pids[$index] <$log_file |
        $fish -N (status filename) $magic_literal_prepend_flag $prefix &
end
test (count $podman_pids) -gt 0 || die 'did not launch any builders'

wait

printf '%s: collected exit codes: %s\n' \
    $argv0 (string join ' ' $podman_exit_codes)

for index in (seq (count $podman_exit_codes))
    test $podman_exit_codes[$index] -eq 0 || \
        die 'build failed on' $build_connections[$index] 'with exit status' \
            $podman_exit_codes[$index]
end

set -l image_ids
for index in (seq (count $image_id_files))
    read --line -l image_id <$image_id_files[$index] || \
        die 'could not read the image ID for the successful build on' \
            $build_connections[$index] 'from' $image_id_files[$index]
    set -a image_ids $image_id
    podman image exists $image_id && continue
    set -l image_archive (mktemp_autoclean)
    podman_ --connection $build_connections[$index] save \
        --format oci-archive --output $image_archive $image_id || \
            die 'failed to save image' $image_id 'from' $build_connections[$index] \
                'to' $image_archive
    podman_ load --input $image_archive || \
        die 'failed to load image' $image_id 'from' $image_archive \
            'into the default connection'
end

! podman image exists $version_tag || \
    podman_ untag $version_tag $version_tag || \
        die 'failed to remove the tag' $version_tag \
            'to free it for a new index containing the new images'
set -l manifest_id (podman manifest create $version_tag $image_ids) || \
    die 'failed to create the image index' $version_tag 'for the new images'
podman_ manifest annotate --index $annotation_args $manifest_id || \
    die 'failed to annotate the image index' $version_tag

for i in $version_tag (qualified_tag latest)
    podman_ manifest push --all --format oci $manifest_id docker://{$i} || \
        die 'failed to publish' $i
end

set -l wall_clock_final (unix_timestamp_now)
printf '%s: wall clock stopped at %s.\n' $argv0 \
    (format_date (date_timestamp_args $wall_clock_final))

set -l date_args \
    (date_timestamp_args (math $wall_clock_final - $wall_clock_initial))
# This formatting method assumes that the time elapsed was less than 24 hours.
# That is a reasonable assumption here.
printf '%s: completed successfully in %s.\n' $argv0 (date -u $date_args +%T)
