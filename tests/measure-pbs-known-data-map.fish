#!/usr/bin/env -S fish --no-config
# SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
# SPDX-License-Identifier: GPL-3.0-or-later

# Run one backup for `Measure-PbsKnownDataMap.ps1`. The supervisor executes this
# after `start-pbs.fish`, which supplies PBS credentials, the encryption key on
# `stdin`, and the existing mount and child-supervision functions. Arguments are
# the test backup ID, map case, and uploader (`serial` or `parallel`), followed
# by any diagnostic PBS options for this measurement. Those options are passed
# directly to the backup command and appear in its printed command line.
# Each invocation gets a fresh key stream, so the PowerShell driver starts a
# separate supervisor invocation for every measurement.

set -l test_backup_id $argv[1]
set -l map_case $argv[2]
set -l uploader $argv[3]
if ! contains -- $map_case hidden unused all-data known
    printf 'Unknown data-map test case: %s\n' $map_case 1>&2
    exit 1
end
if ! contains -- $uploader serial parallel
    printf 'Unknown image uploader: %s\n' $uploader 1>&2
    exit 1
end

set -l map_directory
if test $map_case = all-data
    set map_directory (mktemp -d -t devicefs-all-data-maps.XXXXXXXXXX)
    if test $status -ne 0
        echo 'Could not create a temporary directory for the all-set maps.' 1>&2
        exit 1
    end
    # Remove this invocation's maps when Fish exits, after PBS has finished.
    function remove_all_data_maps --on-event fish_exit --inherit-variable map_directory
        rm -rf -- $map_directory
    end
end

sudo -n mount $vss_mount_point || exit
cancel_before_start unmount_vss
set -l parallel_images false
if test $uploader = parallel
    set parallel_images true
end
set -l backup_arguments backup --keyfd 0 --backup-id $test_backup_id \
    --parallel-images $parallel_images
set -a backup_arguments $argv[4..]
for archive in volume.img companion.img
    set -l image $vss_mount_point/$archive
    set -l map {$image}.known-data.bitmap
    set -a backup_arguments {$archive}:$image
    if test $map_case = known
        set -a backup_arguments --known-data-map {$archive}:$map
    else if test $map_case = all-data
        set -l all_data_map $map_directory/$archive.bitmap
        head --bytes (stat --format=%s $map) /dev/zero | tr '\000' '\377' >$all_data_map
        if string match -q -r '[^0]' $pipestatus
            printf 'Could not create the all-set map for %s.\n' $archive 1>&2
            unmount_vss
            exit 1
        end
        set -a backup_arguments --known-data-map {$archive}:$all_data_map
    end
end

maybe_print_pbs_client_version
uname -a
openssl version
openssl info -cpusettings
echo -- $DEVICEFS_PBS_CLIENT $backup_arguments

# Fish includes a background process's CPU time when the timed block waits for
# that process to finish. This lets us measure PBS CPU time while retaining
# cancellation through `supervise_pbs`. PBS's own `Duration` line supplies the
# CSV timing; Fish adds user and system CPU times to the log.
time begin
    $DEVICEFS_PBS_CLIENT $backup_arguments &
    supervise_pbs $last_pid unmount_vss
end
exit $status
