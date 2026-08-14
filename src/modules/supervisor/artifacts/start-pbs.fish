# SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
# SPDX-License-Identifier: GPL-3.0-or-later

set vss_mount_point /mnt/vss

function unmount_vss
    timeout --kill-after=1s 5s fish -c 'while ! sudo umount $argv[1]; sleep 1; end' $vss_mount_point
end

set pid_file $argv[1]
set stop_file $argv[2]
set backup_id $argv[3]
sudo mount $vss_mount_point || exit $status
if test -e $stop_file
    unmount_vss
    set unmount_exit_code $status
    rm -f $pid_file $stop_file
    if test $unmount_exit_code -ne 0
        exit $unmount_exit_code
    end
    exit 143
end
set backup_argv backup --backup-id $backup_id
for image_path in $vss_mount_point/*.img
    set image_filename (path basename $image_path)
    set --append backup_argv "$image_filename:$image_path"
end
~/bin/proxmox-backup-client $backup_argv &
set backup_pid $last_pid
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
