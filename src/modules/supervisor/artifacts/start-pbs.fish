# SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
# SPDX-License-Identifier: GPL-3.0-or-later

set vss_mount_point /mnt/vss
set pbs_manifest_filename devicefs-manifest.conf

function unmount_vss
    timeout --kill-after=1s 5s fish --no-config -c 'while ! sudo -n umount $argv[1]; sleep 1; end' $vss_mount_point
end

function finish_operation --argument-names finalizer
    $finalizer
    set finalizer_exit_code $status
    rm -f $stop_file
    return $finalizer_exit_code
end

function cancel_before_start --argument-names finalizer
    if not test -e $stop_file
        return 0
    end
    finish_operation $finalizer || exit
    exit 143
end

function supervise_pbs --argument-names pbs_pid finalizer
    set --erase DEVICEFS_MANIFEST
    set --erase PBS_PASSWORD
    set -g pbs_exit_code 1
    function record_pbs_exit --on-process-exit $pbs_pid
        set -g pbs_exit_code $argv[3]
    end
    echo $pbs_pid >$pid_file
    if test -e $stop_file
        kill -TERM $pbs_pid
    end
    wait $pbs_pid
    rm -f $pid_file
    finish_operation $finalizer
    set finish_exit_code $status
    if test $pbs_exit_code -ne 0
        return $pbs_exit_code
    end
    return $finish_exit_code
end

function run_backup --argument-names parallel_images
    sudo -n mount $vss_mount_point || exit
    cancel_before_start unmount_vss
    set backup_argv backup --keyfd 0 --backup-id $backup_id $parallel_images
    for image_path in $vss_mount_point/*.img
        set image_filename (path basename $image_path)
        set --append backup_argv "$image_filename:$image_path"
    end
    if set -l jq (command -v jq)
        # If jq is installed, use it to prettify the manifest.
        set -l manifest_ (echo -- $DEVICEFS_MANIFEST | $jq | string collect)
        if ! string match -q -r '[^0]' $pipestatus
            set DEVICEFS_MANIFEST $manifest_
        end
    end
    printf 'Backup manifest:\n%s\n' $DEVICEFS_MANIFEST
    $DEVICEFS_PBS_CLIENT $backup_argv \
        {$pbs_manifest_filename}:(echo -- $DEVICEFS_MANIFEST | psub --file) &
    supervise_pbs $last_pid unmount_vss
end

function print_manifest
    cancel_before_start true
    timeout --kill-after=5s 60s \
        $DEVICEFS_PBS_CLIENT restore --keyfd 0 \
        host/{$backup_id} {$pbs_manifest_filename}.blob - &
    supervise_pbs $last_pid true
end

argparse /parallel-images /print-manifest -- $argv || exit

set pid_file $argv[1]
set stop_file $argv[2]
set backup_id $argv[3]
# Keep these reads in the same order as RunPbsFish's NUL-delimited records,
# adding future records before the final key document.
read --null --global DEVICEFS_PBS_CLIENT || exit
read --null --global --export PBS_SERVER || exit
read --null --global --export PBS_PORT || exit
read --null --global --export PBS_DATASTORE || exit
read --null --global --export PBS_AUTH_ID || exit
read --null --global --export PBS_NAMESPACE || exit
read --null --global --export PBS_FINGERPRINT || exit
read --null --global --export PBS_PASSWORD || exit
read --null --global DEVICEFS_MANIFEST || exit

set operation run_backup $_flag_parallel_images
if set --query _flag_print_manifest
    set operation print_manifest
end
$operation
