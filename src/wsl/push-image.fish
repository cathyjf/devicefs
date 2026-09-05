#!/usr/bin/env -S fish --no-config

set -l podman_build (podman system info --format '{{.Client.Built}}')
if test "$status" -ne 0
    printf '%s: `podman system info` failed.\n' (status filename) 1>&2
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
        (status filename) $minimum_podman_build 1>&2
    exit 1
end

argparse '/tag=' -- $argv || exit

if ! set -q _flag_tag
    printf '%s: option `--tag TAG` is required.\n' (status filename) 1>&2
    exit 1
end

function __get_tag -a tag
    echo ghcr.io/cathyjf/devicefs-wsl:{$tag}
end

set -l version_tag (__get_tag $_flag_tag)

set -l gh_status (gh auth status 2>&1)
if test "$status" -ne 0
    printf '%s: `gh auth status` failed.\n' (status filename) 1>&2
    exit 1
end

if ! string match -q "*'write:packages'*" $gh_status
    printf '%s: GitHub token lacks `write:packages`; run `gh auth refresh -s write:packages`.\n' \
        (status filename) 1>&2
    exit 1
else if ! gh auth token | podman login ghcr.io -u cathyjf --password-stdin
    printf '%s: failed to authenticate to ghcr.io.\n' \
        (status filename) 1>&2
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

podman --log-level=debug farm build \
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

podman manifest annotate --index $annotation_args $version_tag || exit

for i in $version_tag (__get_tag latest)
    podman manifest push --format oci $version_tag docker://{$i} || exit
end
