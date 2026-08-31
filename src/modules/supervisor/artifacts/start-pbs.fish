# SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
# SPDX-License-Identifier: GPL-3.0-or-later

set vss_mount_point /mnt/vss
set pbs_manifest_filename devicefs-manifest.conf

# Set this to 1 to enable the experimental `map-grpc` mode.
set --export use_map_grpc 0

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
    if test ! -e $stop_file
        return 0
    end
    finish_operation $finalizer || exit
    exit 143
end

function publish_child --argument-names child_pid
    echo $child_pid >$pid_file
    if test -e $stop_file
        kill -TERM $child_pid
    end
end

function wait_for_published_child --argument-names child_pid
    set -g child_exit_code 1
    function record_child_exit --on-process-exit $child_pid
        set -g child_exit_code $argv[3]
    end
    publish_child $child_pid
    wait $child_pid
    rm -f $pid_file
    return $child_exit_code
end

function supervise_pbs --argument-names child_pid finalizer
    set --erase DEVICEFS_MANIFEST
    set --erase PBS_PASSWORD
    wait_for_published_child $child_pid
    set -l child_exit_code $status
    finish_operation $finalizer
    set finish_exit_code $status
    if test $child_exit_code -ne 0
        return $child_exit_code
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

function finish_view
    set --erase DEVICEFS_RPC_PASSWORD
    set --erase PBS_PASSWORD

    set -l map_exit_code 0
    set -l map_output_exit_code 0
    if set --query view_map_pid[1]
        if test -n "$view_loop_device"
            printf 'Stopping PBS archive mapping on %s.\n' $view_loop_device 1>&2
        else
            printf 'Stopping PBS archive mapping.\n' 1>&2
        end
        set -l termination_requested 0
        if test -e $stop_file
            set termination_requested 1
        end
        echo $view_map_pid >$pid_file
        kill -TERM $view_map_pid 2>/dev/null
        if test $status -eq 0
            set termination_requested 1
        end
        wait $view_map_pid
        if test $termination_requested -eq 0
            set map_exit_code $view_map_exit_code
        end
        rm -f $pid_file
    end
    if set --query view_map_output_pid[1]
        wait $view_map_output_pid
        set map_output_exit_code $view_map_output_exit_code
    end

    set -l remove_exit_code 0
    if set --query view_root[1]
        printf 'Removing view state %s.\n' $view_root 1>&2
        rm -rf -- $view_root
        set remove_exit_code $status
    end

    if test $map_exit_code -ne 0
        return $map_exit_code
    end
    if test $map_output_exit_code -ne 0
        return $map_output_exit_code
    end
    return $remove_exit_code
end

function read_view_map_output --argument-names output_path device_path
    set -l map_ready
    while read --local map_line
        printf '%s\n' $map_line 1>&2
        if test -z "$map_ready"
            if test $use_map_grpc -eq 1
                test "$map_line" = "$device_path.sock" || continue
                printf '%s\n' $device_path.sock >$device_path
                set map_ready 1
                continue
            end
            for candidate in (
                string match --regex --all --groups-only \
                    '\s(/dev/loop[0-9]+)(?:\s|$)' $map_line
            )
                printf '%s\n' $candidate >$device_path
                set map_ready 1
                break
            end
        end
    end <$output_path
    if test -z "$map_ready"
        printf '\n' >$device_path
    end
end

function prepare_view_samba --argument-names address port
    mkdir -- $view_root/private $view_root/state $view_root/cache \
        $view_root/lock $view_root/pid $view_root/ncalrpc || return

    set -l unix_user (id --user --name) || return
    set -l username_map $view_root/username.map
    printf '%s = devicefs\n' $unix_user >$username_map || return

    set -l samba_configuration $view_root/smb.conf
    printf '%s\n' \
        '[global]' \
        '    server role = standalone server' \
        '    workgroup = DEVICEFS' \
        '    netbios name = DEVICEFSVIEW' \
        '    rpc start on demand helpers = no' \
        "    rpc server dynamic port range = $port-$port" \
        '    allow dcerpc auth level connect:devicefs_block_device = yes' \
        "    interfaces = $address" \
        '    bind interfaces only = yes' \
        "    passdb backend = smbpasswd:$view_root/private/smbpasswd" \
        "    username map = $username_map" \
        "    private dir = $view_root/private" \
        "    state directory = $view_root/state" \
        "    cache directory = $view_root/cache" \
        "    lock directory = $view_root/lock" \
        "    pid directory = $view_root/pid" \
        "    ncalrpc dir = $view_root/ncalrpc" \
        >$samba_configuration || return

    printf '%s\n%s\n' $DEVICEFS_RPC_PASSWORD $DEVICEFS_RPC_PASSWORD |
        pdbedit "--configfile=$samba_configuration" --create \
            "--user=$unix_user" --password-from-stdin 1>&2
    set -l password_pipeline_status $pipestatus
    set --erase DEVICEFS_RPC_PASSWORD
    if test $password_pipeline_status[1] -ne 0
        return $password_pipeline_status[1]
    end
    return $password_pipeline_status[2]
end

function run_view --argument-names snapshot_override archive address port rpc_helper samba_dcerpcd timestamp
    set -g view_root (mktemp -d -t devicefs-view.XXXXXXXXXX) || return
    cancel_before_start finish_view

    set -l snapshot host/{$backup_id}
    if test -n "$snapshot_override"
        set snapshot $snapshot_override
    end
    if test -n "$timestamp"
        set snapshot "$snapshot/$timestamp"
    end
    printf "Mapping PBS archive '%s' from snapshot '%s'.\n" $archive $snapshot 1>&2
    set -l map_output $view_root/map-output
    set -l mapped_device_output $view_root/mapped-device
    mkfifo -- $map_output $mapped_device_output
    set -l output_exit_code $status
    if test $output_exit_code -ne 0
        finish_operation finish_view
        return $output_exit_code
    end
    set -l map_output_reader $view_root/read-map-output.fish
    begin
        functions -- read_view_map_output
        printf '%s\n' 'read_view_map_output $argv[1] $argv[2]'
    end >$map_output_reader
    set output_exit_code $status
    if test $output_exit_code -ne 0
        finish_operation finish_view
        return $output_exit_code
    end
    set -l fish (status fish-path)
    env -u PBS_PASSWORD $fish --no-config $map_output_reader \
        $map_output $mapped_device_output </dev/null &
    set -g view_map_output_pid $last_pid
    set -g view_map_output_exit_code 1
    function record_view_map_output_exit --on-process-exit $view_map_output_pid
        set -g view_map_output_exit_code $argv[3]
    end
    set -l map_command map --verbose
    set -l map_operands $snapshot $archive
    if test $use_map_grpc -eq 1
        set map_command map-grpc
        set --append map_operands $mapped_device_output.sock
    end
    $DEVICEFS_PBS_CLIENT $map_command --keyfile (psub --file) \
        $map_operands &>$map_output &
    set -g view_map_pid $last_pid
    set -g view_map_exit_code 1
    function record_view_map_exit --on-process-exit $view_map_pid
        set -g view_map_exit_code $argv[3]
    end
    publish_child $view_map_pid

    read --local mapped_device <$mapped_device_output
    set --erase PBS_PASSWORD
    if test -n "$mapped_device"
        set -g view_loop_device $mapped_device
    end
    if test -z "$mapped_device"
        cancel_before_start finish_view
        printf 'Could not identify the mapped PBS loop device.\n' 1>&2
        finish_operation finish_view
        set -l finish_exit_code $status
        if test $finish_exit_code -ne 0
            return $finish_exit_code
        end
        return 1
    end
    printf 'Mapped PBS archive on %s.\n' $view_loop_device 1>&2
    cancel_before_start finish_view

    prepare_view_samba $address $port
    set -l preparation_exit_code $status
    if test $preparation_exit_code -ne 0
        finish_operation finish_view
        return $preparation_exit_code
    end
    cancel_before_start finish_view

    printf 'Starting Samba view endpoint on %s port %s.\n' $address $port 1>&2
    set --function --export DEVICEFS_SAMBA_RPC_DEVICE $view_loop_device
    $samba_dcerpcd --foreground --ready-signal-fd=1 \
        "--log-basename=$view_root" \
        "--configfile=$view_root/smb.conf" \
        $rpc_helper </dev/null &
    set --erase DEVICEFS_MANIFEST
    set --erase PBS_PASSWORD
    wait_for_published_child $last_pid
    set -l child_exit_code $status
    set -l stop_requested 0
    if test -e $stop_file
        set stop_requested 1
    end
    finish_operation finish_view
    set -l finish_exit_code $status
    if test '(' $stop_requested -eq 0 ')' -a '(' $child_exit_code -ne 0 ')'
        return $child_exit_code
    end
    return $finish_exit_code
end

argparse /parallel-images /print-manifest /view -- $argv || exit

set pid_file $argv[1]
set stop_file $argv[2]
set backup_id $argv[3]
# Keep these reads in the same order as StartPbsFish's NUL-delimited records,
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
read --null --global DEVICEFS_RPC_PASSWORD || exit

set operation run_backup $_flag_parallel_images
if set --query _flag_view
    set operation run_view $argv[4..10]
else if set --query _flag_print_manifest
    set operation print_manifest
end
$operation
