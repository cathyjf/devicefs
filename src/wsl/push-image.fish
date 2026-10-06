#!/usr/bin/env -S fish -N
# SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
# SPDX-License-Identifier: GPL-3.0-or-later

set -l magic_subshell_flag --push-image-internal-subshell
set -l magic_publish_flag --push-image-internal-publish
set -l magic_flags $magic_subshell_flag $magic_publish_flag
set -l argv0 (status basename)
set -l image_repository ghcr.io/cathyjf/devicefs-wsl
set -l gpg_signing_fingerprint EDC7363F595C58D2F07930FEB69A7D95683C6E2A

source (status dirname)/include/utility.fish

function format_date
    date $argv +%FT%T%z
end

function prepend_line_with_timestamp -a line
    printf '[%s] %s\n' (format_date $argv[2..]) $line
end

function escape_argv
    echo -n -- '+ '
    string escape -- $argv | string join ' '
end

function prepend_timestamps
    prepend_line_with_timestamp (escape_argv $argv)
    $argv[1] $argv[2..] 2>&1 | while read -l line
        prepend_line_with_timestamp $line
    end
    return $pipestatus[1]
end

function print_and_invoke
    escape_argv $argv
    $argv[1] $argv[2..]
end

# Define `podman_` and `skopeo_`.
for i in podman skopeo
    function {$i}_ -V i
        print_and_invoke $i $argv
    end
end

function compute_signature -a layer_hash -V gpg_signing_fingerprint
    log 'Computing signature over message:' "'$layer_hash'" >&2
    echo -n -- $layer_hash | \
        gpg --local-user {$gpg_signing_fingerprint}! \
            --digest-algo SHA512 --no-armor --no-textmode --detach-sign | \
                base64 --wrap 0
    for exit_status in $pipestatus
        test "$exit_status" -eq 0 || return $exit_status
    end
end

function publish_image -a connection image_id_file result_file -V image_repository
    read --line -l image_id <$image_id_file || \
        die 'could not read the image ID for the successful build on' \
            $connection 'from' $image_id_file
    set -l image_archive (mktemp_autoclean)
    podman_ --connection $connection save \
        --format oci-archive --output $image_archive $image_id || \
            die 'failed to save image' $image_id 'from' $connection \
                'to' $image_archive
    set -l layer_hash \
        (skopeo inspect --format '{{index .Layers 0}}' oci-archive:{$image_archive}) || \
            die 'failed to read the layer digest from image archive' $image_archive
    set -l signature (compute_signature $layer_hash || \
        die 'failed to compute the signature for layer' $layer_hash)
    set -l image_digest (skopeo inspect --format '{{.Digest}}' oci-archive:{$image_archive}) || \
        die 'failed to read the manifest digest from image archive' $image_archive
    skopeo_ copy --preserve-digests oci-archive:{$image_archive} \
        docker://{$image_repository}@{$image_digest} || \
            die 'failed to publish image' $image_id 'to' $image_repository
    printf '%s\n%s\n' $image_digest $signature >$result_file || \
        die 'failed to record the published image digest and signature for' \
            $image_id 'in' $result_file
end

if ! contains -- "$argv[1]" $magic_flags
    prepend_timestamps (status fish-path) -N (status filename) \
        $magic_subshell_flag $argv
    exit
else if test "$argv[1]" = $magic_publish_flag
    publish_image $argv[2..]
    exit
else
    set -e argv[1]
end

function wait_and_cancel_on_failure -a ptr_exit_codes
    set -l pids $argv[2..]
    set -l jobs_remaining $pids
    while true
        wait -n $jobs_remaining || {
            set -l wait_status $status
            kill -- $jobs_remaining
            wait
            exit $wait_status
        }
        set jobs_remaining (for job in $jobs_remaining
            jobs -q -- $job && echo -- $job
        end)
        test (count $jobs_remaining) -gt 0 || break
        string match -qr '[^0]' $$ptr_exit_codes || continue
        # If we get here, a job has already failed, so kill the remaining jobs.
        kill -- $jobs_remaining
    end
end

function qualified_tag -a tag -V image_repository
    echo $image_repository:{$tag}
end

argparse -n $argv0 '/tag=' '/latest' -- $argv || exit
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
set -l publication_tags ({
    echo -- $version_tag
    set -q _flag_latest && qualified_tag 'latest'
})
for i in $publication_tags
    log 'using tag' "'$i'"
end

function unix_timestamp_now
    set -l timestamp (date +%s)
    printf %d $timestamp || \
        die 'unix timestamp was not an integer:' $timestamp
end

begin
    if set -l date_ (date -ur 100 +%T 2>&1) && test "$date_" = '00:01:40'
        function date_timestamp_args -a seconds
            printf %s\n -r $seconds
        end
    else if set -l date_ (date -ud @100 +%T 2>&1) && test "$date_" = '00:01:40'
        function date_timestamp_args -a seconds
            printf %s\n -d @$seconds
        end
    else
        set -g date_timestamp_args_unknown
        function date_timestamp_args
        end
    end
end

set -l gh_status (gh auth status 2>&1) || \
    die '`gh auth status` failed:' $gh_status
if ! string match -q "*'write:packages'*" $gh_status
    die 'GitHub token lacks `write:packages`; run `gh auth refresh -s write:packages`'
else if ! gh auth token | skopeo login ghcr.io -u cathyjf --password-stdin
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
log 'wall clock started at' \
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
    set -l awk_argv awk -v builder={$builder} \
        '{print "[" builder "] " $0; fflush()}'
    escape_argv $podman_argv | $awk_argv[1] $awk_argv[2..]
    source (status dirname)/include/pipeline.fish \
        (string join0 -- $podman_argv | psub) \
        (string join0 -- $awk_argv | psub)
    set -a podman_pids $__writer_pid
    set -a build_connections $hostname_
    set -a image_id_files $image_id_file
    set -a podman_exit_codes 0
    set -l index (count $podman_exit_codes)
    function __handle_podman_exit_{$index} -p $podman_pids[$index] -V index
        set -g podman_exit_codes[$index] $argv[3]
    end
end
test (count $podman_pids) -gt 0 || die 'did not launch any builders'

wait_and_cancel_on_failure podman_exit_codes $podman_pids

log 'collected podman exit codes:' $podman_exit_codes
for index in (seq (count $podman_exit_codes))
    test $podman_exit_codes[$index] -eq 0 || \
        die 'build failed on' $build_connections[$index] 'with exit status' \
            $podman_exit_codes[$index]
end

set -l publication_pids
set -l publication_result_files
set -g publication_exit_codes
for index in (seq (count $image_id_files))
    set -l result_file (mktemp_autoclean)
    set -a publication_result_files $result_file
    set -a publication_exit_codes 0
    $fish -N (status filename) $magic_publish_flag \
        $build_connections[$index] $image_id_files[$index] $result_file &
    set -a publication_pids $last_pid
    function __handle_publication_exit_{$index} -p $publication_pids[$index] -V index
        set -g publication_exit_codes[$index] $argv[3]
    end
end
wait_and_cancel_on_failure publication_exit_codes $publication_pids

set -l image_digests
set -l image_signatures
for index in (seq (count $publication_result_files))
    test $publication_exit_codes[$index] -eq 0 || \
        die 'image publication failed for' $build_connections[$index] 'with exit status' \
            $publication_exit_codes[$index]
    read --line -l image_digest signature <$publication_result_files[$index] || \
        die 'failed to read the published image digest and signature for' \
            $build_connections[$index] 'from' $publication_result_files[$index]
    set -a image_digests $image_digest
    set -a image_signatures (echo -n -- $signature | string collect --allow-empty)
end

! podman manifest exists $version_tag || \
    podman_ manifest rm $version_tag || \
        die 'failed to delete the existing image index' $version_tag \
            'to free that tag for a new index containing the new images'
! podman image exists $version_tag || \
    podman_ untag $version_tag $version_tag || \
        die 'failed to remove the tag' $version_tag \
            'to free it for a new index containing the new images'

set -l manifest_id (podman manifest create $version_tag \
    docker://{$image_repository}@{$image_digests}) || \
        die 'failed to create the image index' $version_tag 'for the new images'
podman_ manifest annotate --index $annotation_args $manifest_id || \
    die 'failed to annotate the image index' $version_tag

for index in (seq (count $image_digests))
    podman_ manifest annotate \
        --annotation "com.cathyjf.devicefs.signature=$image_signatures[$index]" \
        $manifest_id $image_digests[$index] || \
            die 'failed to add the signature for image' $image_digests[$index] \
                'as an annotation to the image index' $version_tag
end

set -l manifest_directory (mktemp_autoclean -d)
podman manifest inspect $manifest_id >$manifest_directory/manifest.json || \
    die 'failed to export the image index' $version_tag
for i in $publication_tags
    skopeo_ copy --multi-arch=index-only --preserve-digests \
        dir:{$manifest_directory} docker://{$i} || \
        die 'failed to publish' $i
end

set -l wall_clock_final (unix_timestamp_now)
log 'wall clock stopped at' \
    (format_date (date_timestamp_args $wall_clock_final))

set -gq date_timestamp_args_unknown || {
    # This formatter assumes that the time elapsed was less than 24 hours.
    # That is a reasonable assumption here.
    set -l date_args \
        (date_timestamp_args (math $wall_clock_final - $wall_clock_initial))
    log 'completed successfully in' (date -u $date_args +%T)
}

if jobs -q
    log 'waiting for log reader processes to exit'
    wait
    log 'all log reader processes have exited'
end
