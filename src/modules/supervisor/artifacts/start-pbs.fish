# SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
# SPDX-License-Identifier: GPL-3.0-or-later

set vss_mount_point /mnt/vss

function unmount_vss
    timeout --kill-after=1s 5s fish --no-config -c 'while ! sudo -n umount $argv[1]; sleep 1; end' $vss_mount_point
end

argparse /parallel-images -- $argv || exit

set pid_file $argv[1]
set stop_file $argv[2]
set backup_id $argv[3]
# Keep these reads in the same order as RunWslBackup's NUL-delimited records,
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
sudo -n mount $vss_mount_point || exit
if test -e $stop_file
    unmount_vss
    set unmount_exit_code $status
    rm -f $pid_file $stop_file
    if test $unmount_exit_code -ne 0
        exit $unmount_exit_code
    end
    exit 143
end
set backup_argv backup --keyfd 0 --backup-id $backup_id
if set --query _flag_parallel_images
    set --append backup_argv --parallel-images
end
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
    'devicefs-manifest.conf:'(echo -- $DEVICEFS_MANIFEST | psub --file) &
set backup_pid $last_pid
set --erase DEVICEFS_MANIFEST
set --erase PBS_PASSWORD
set -g backup_exit_code 1
function record_backup_exit --on-process-exit $backup_pid
    set -g backup_exit_code $argv[3]
end
echo $backup_pid >$pid_file
if test -e $stop_file
    kill -TERM $backup_pid
end
wait $backup_pid
rm -f $pid_file
unmount_vss
set unmount_exit_code $status
rm -f $stop_file
if test $backup_exit_code -ne 0
    exit $backup_exit_code
end
exit $unmount_exit_code
