# SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
# SPDX-License-Identifier: GPL-3.0-or-later

# This script runs one PBS operation for the Windows supervisor. The invocation
# and input records are read at the bottom of this file. The selected operation
# starts the required child processes and waits for completion or cancellation.

set vss_mount_point /mnt/vss
set pbs_manifest_filename devicefs-manifest.conf
# If this is 1, view cleanup includes Samba's log files in its diagnostics.
set print_samba_logs 0
# If this is 1, PBS serves image reads over a Unix socket using `map-grpc`;
# otherwise, `map` exposes a kernel loop device. The output-reader child needs
# this exported setting to recognize the corresponding readiness message.
set --export use_map_grpc 1

# Unmount the DeviceFs snapshot images after PBS has finished reading them.
# A busy mount is retried, with a timeout so cleanup cannot wait indefinitely.
# The exit status reports whether unmounting succeeded or the timeout expired.
function unmount_vss
    timeout --kill-after=1s 5s fish --no-config -c 'while ! sudo -n umount $argv[1]; sleep 1; end' $vss_mount_point
end

# Complete operation-specific cleanup by calling `finalizer` without arguments,
# then remove the supervisor's stop request. Return the finalizer's status;
# callers decide whether an earlier operation failure takes precedence.
function finish_operation --argument-names finalizer
    $finalizer
    set finalizer_exit_code $status
    rm -f $stop_file
    return $finalizer_exit_code
end

# Check for cancellation before launching the next child. If a stop request
# exists, `finalizer` releases resources already acquired and this Fish process
# exits. Successful cleanup yields status 143 (the conventional SIGTERM status).
# A cleanup failure supplies its own status. Otherwise, return zero so startup
# can proceed.
function cancel_before_start --argument-names finalizer
    if test ! -e $stop_file
        return 0
    end
    finish_operation $finalizer || exit
    exit 143
end

# Make the supplied child PIDs available to `SendPbsFishSignal` in pbs.cpp,
# one per line in `pid_file`. The supervisor creates `stop_file` before reading
# those PIDs. Checking it after publication also handles a cancellation request
# that arrived before the supervisor could discover the children.
function publish_children
    printf '%s\n' $argv >$pid_file
    if test -e $stop_file
        kill -TERM $argv
    end
end

# Write `child_pid` to the supervisor's PID file, wait for that process, and
# return its exit status.
# Fish's `wait` does not return the child's status, so an exit handler records it.
# Fish passes process-exit handlers the event name, PID, and exit status as their
# three arguments. The other process-exit handlers below use the same convention.
# https://github.com/fish-shell/fish-shell/blob/master/tests/checks/wait.fish
# https://github.com/fish-shell/fish-shell/blob/master/src/event.rs
function wait_for_published_child --argument-names child_pid
    set -g child_exit_code 1
    # Record the process-exit event's result for `wait_for_published_child`.
    function record_child_exit --on-process-exit $child_pid
        set -g child_exit_code $argv[3]
    end
    publish_children $child_pid
    wait $child_pid
    rm -f $pid_file
    return $child_exit_code
end

# Wait for an already started PBS child, then call `finalizer` to release the
# operation's resources. A child failure takes precedence over a cleanup failure.
# The child has already inherited its password, and any manifest has been copied
# to a temporary file for its job, so the parent can erase those variables.
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

# Print the version of the proxmox-backup-client installed in the image, as
# reported by the `proxmox-backup.commit` file created during the Containerfile
# building process, if that file exists.
function maybe_print_pbs_client_version
    set -l pbs_client_marker /usr/share/devicefs-build/proxmox-backup.commit
    if test ! -f $pbs_client_marker
        return
    end
    read --line -l pbs_version <$pbs_client_marker
    printf 'Reported proxmox-backup-client build: %s\n' \
        (string replace -ar '[^\w ,\.\-+]' '' $pbs_version)
end

# Upload the mounted DeviceFs images and `DEVICEFS_MANIFEST` to `host/$backup_id`
# in `PBS_NAMESPACE`. `parallel_images` supplies the optional PBS parallel-images
# argument. PBS reads the encryption key from the remaining standard input.
# The mount stays available until PBS exits; the result includes unmount failure
# when the upload itself succeeded. PBS output and the printed manifest are
# forwarded to the Windows supervisor.
function run_backup --argument-names parallel_images
    set -l manifest_ (echo -- $DEVICEFS_MANIFEST | jq | string collect)
    if ! string match -q -r '[^0]' $pipestatus
        set DEVICEFS_MANIFEST $manifest_
    end
    set -l manifest_file (mktemp -t devicefs-manifest.XXXXXXXXXX) || return
    # Remove the manifest when this Fish process exits, including after cancellation.
    function remove_backup_manifest --on-event fish_exit --inherit-variable manifest_file
        rm -f -- $manifest_file
    end
    echo -- $DEVICEFS_MANIFEST >$manifest_file || return
    sudo -n mount $vss_mount_point || exit
    cancel_before_start unmount_vss
    set -l backup_argv backup --keyfd 0 --backup-id $backup_id $parallel_images
    for image_path in $vss_mount_point/*.img
        set -l image_filename (path basename $image_path)
        set -a backup_argv {$image_filename}:{$image_path}

        set -l bitmap_filename {$image_path}.known-data.bitmap
        if test -f $bitmap_filename
            set -a backup_argv --known-data-map {$image_filename}:$bitmap_filename
        end
    end
    printf 'Backup manifest:\n%s\n' $DEVICEFS_MANIFEST
    set -a backup_argv {$pbs_manifest_filename}:$manifest_file
    maybe_print_pbs_client_version
    echo -- $DEVICEFS_PBS_CLIENT $backup_argv
    $DEVICEFS_PBS_CLIENT $backup_argv &
    supervise_pbs $last_pid unmount_vss
end

# Restore the DeviceFs manifest to standard output, using the encryption key
# from standard input. An empty `snapshot` selects the latest backup in
# `host/$backup_id`; otherwise the supplied PBS group or snapshot is used.
# The retrieval has a deadline, and its exit status reaches the caller so an
# optional-manifest consumer can continue when the archive is unavailable.
function print_manifest --argument-names snapshot
    if test -z "$snapshot"
        set snapshot host/{$backup_id}
    end
    cancel_before_start true
    timeout --kill-after=5s 60s \
        $DEVICEFS_PBS_CLIENT restore --keyfd 0 \
        $snapshot {$pbs_manifest_filename}.blob - &
    supervise_pbs $last_pid true
end

# Start a PBS catalog query with the arguments after `output`, writing its JSON
# to that file. The caller uses `$last_pid` to publish and wait for the process.
# Remove the file if the query fails so incomplete results are not consumed.
function start_catalog_query --argument-names output
    $DEVICEFS_PBS_CLIENT $argv[2..] >$output &
    function catalog_query_finished_$last_pid --on-process-exit $last_pid --inherit-variable output \
            --argument-names event process_id exit_code
        test $exit_code -eq 0 || rm -f -- $output
    end
end

# Write a JSON catalog to standard output using `directory` for temporary
# query results. The object contains the starting `namespace` and a `snapshots`
# array whose entries each retain their full namespace for later PBS requests.
# The caller supplies an empty `directory/snapshots` file and removes the directory.
#
# Query `PBS_NAMESPACE` while discovering its child namespaces. If discovery
# or parsing fails, the starting namespace is still queried. Snapshot requests
# run concurrently; failed requests are omitted while their diagnostics remain
# on standard error. Cancellation returns 143, and JSON assembly errors return
# jq's failure status.
function collect_backup_catalog --argument-names directory
    set -l namespaces $PBS_NAMESPACE
    start_catalog_query $directory/query namespace list $PBS_NAMESPACE --output-format json
    set -l discovery $last_pid
    # Omitting the group lists every snapshot in this namespace in one request.
    start_catalog_query $directory/1.json snapshot list --ns $PBS_NAMESPACE --output-format json
    set -l children $last_pid
    publish_children $discovery $children
    wait $discovery
    if test ! -e $stop_file && test -f $directory/query
        if set -l discovered (jq -r --arg namespace $PBS_NAMESPACE \
                '.data[].ns | select(. != $namespace)' $directory/query)
            set --append namespaces $discovered
        end
    end
    for index in (seq 2 (count $namespaces))
        start_catalog_query $directory/$index.json snapshot list --ns $namespaces[$index] --output-format json
        set --append children $last_pid
        publish_children $children
    end
    wait $children
    rm -f -- $pid_file
    if test -e $stop_file
        return 143
    end
    for index in (seq (count $namespaces))
        if test -f $directory/$index.json
            # PBS supplies `size` only when it can read the snapshot's own
            # manifest. Exclude unfinished snapshots and unreadable manifests.
            # https://github.com/proxmox/proxmox-backup/blob/master/src/tools/mod.rs
            jq --arg namespace $namespaces[$index] '.[] | select(.size != null) | . + {namespace: $namespace}' \
                $directory/$index.json >>$directory/snapshots || return
        end
    end
    jq --slurp --arg namespace $PBS_NAMESPACE \
        '{namespace: $namespace, snapshots: .}' $directory/snapshots
end

# Retrieve the backup catalog and print it as JSON to standard output. Create
# temporary files for the queries, remove them afterward, and return the
# retrieval status. Keep `PBS_PASSWORD` available during collection because
# each query needs to inherit it, then erase it before returning.
function list_backups
    cancel_before_start true
    set -l directory (mktemp -d -t devicefs-catalog.XXXXXXXXXX) || return
    printf '' >$directory/snapshots
    collect_backup_catalog $directory
    set -l result $status
    set --erase PBS_PASSWORD
    finish_operation true
    rm -rf -- $directory
    return $result
end

# Stop the image mapper, wait for its output reader, and remove the temporary
# Samba directory. The `view_*` globals identify what was started or created,
# including during incomplete startup. The reader needs the mapper to close its
# output before it can finish, so the mapper is stopped first.
# Return the first failure among an unexpected mapper exit, an output-reader
# failure, and directory removal;
# a mapper exit caused by requested termination is treated as normal cleanup.
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
        if test "$print_samba_logs" -eq 1
            # Samba writes its diagnostics to files in `view_root`. These may
            # explain why a view failed, but deleting the directory would
            # discard them without showing them to the administrator.
            #
            # Reporting the logs only when Samba exits without a stop request
            # would miss failures in DeviceFs: the supervisor responds to those
            # failures by requesting shutdown, just as it does when the user
            # ends a view. Consequently, we print all nonempty Samba logs
            # before removing the directory, including after a requested
            # shutdown.
            for log in $view_root/log.*
                if test -s $log
                    printf "Samba log '%s':\n" $log 1>&2
                    while read --line --local line
                        printf '  %s\n' $line 1>&2
                    end < $log
                end
            end
        end
        printf 'Removing temporary state: %s.\n' $view_root 1>&2
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

# Read mapper output from the `output_path` FIFO and forward every line to
# standard error. Once the mapping is ready, write its Unix socket path
# (`map-grpc`) or loop-device path (`map`) to the `device_path` FIFO so `run_view`
# can start Samba. Continue draining mapper output until EOF. If EOF arrives
# before readiness, write an empty line to release the waiting `run_view` reader.
function read_view_map_output --argument-names output_path device_path
    set -l map_ready
    while read --local map_line
        echo -- $map_line 1>&2
        if test -z "$map_ready"
            if test $use_map_grpc -eq 1
                test "$map_line" = "$device_path.sock" || continue
                echo -- $device_path.sock >$device_path
                set map_ready 1
                continue
            end
            for candidate in (
                string match --regex --all --groups-only \
                    '\s(/dev/loop[0-9]+)(?:\s|$)' $map_line
            )
                echo -- $candidate >$device_path
                set map_ready 1
                break
            end
        end
    end <$output_path
    if test -z "$map_ready"
        printf '\n' >$device_path
    end
end

# Create Samba configuration and account state under `view_root` for the given
# listening address and port. The Windows RPC username `devicefs` is mapped to
# the current Unix user, whose Samba password is supplied in `DEVICEFS_RPC_PASSWORD`.
# `pdbedit` reads that password twice for confirmation; the variable is erased
# afterward. Return failure if preparing the files or password database fails.
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

# Make one PBS image `archive` available through the Samba block-device RPC
# helper. `snapshot_override` selects a PBS group or snapshot; an empty value
# uses `host/$backup_id`. A supplied `timestamp` is appended to select that backup.
# `address` and `port` select the listener, while `rpc_helper` and `samba_dcerpcd`
# identify the executables to run. The encryption key arrives on standard input.
#
# The mapper and its output reader remain alive while Samba serves the image.
# Mapper diagnostics go to standard error; Samba's readiness signal goes to
# standard output for the Windows supervisor. On exit, `finish_view` releases
# the mapping and temporary state. An unexpected Samba failure takes precedence
# over cleanup failure; requested termination returns the cleanup result.
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
    # Separate FIFOs let the reader keep draining diagnostics after it reports
    # readiness, while this function proceeds to run the Samba server.
    mkfifo -- $map_output $mapped_device_output
    set -l output_exit_code $status
    if test $output_exit_code -ne 0
        finish_operation finish_view
        return $output_exit_code
    end
    set -l map_output_reader $view_root/read-map-output.fish
    # Fish functions cannot run as background jobs directly. A separate Fish
    # process runs this function's definition and receives only the FIFO paths.
    # https://fishshell.com/docs/current/language.html#job-control
    begin
        functions -- read_view_map_output
        echo -- 'read_view_map_output $argv[1] $argv[2]'
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
    # Save the output reader's result for `finish_view`, which waits for it
    # only after stopping the mapper.
    function record_view_map_output_exit --on-process-exit $view_map_output_pid
        set -g view_map_output_exit_code $argv[3]
    end
    set -l map_command map --verbose
    set -l map_operands $snapshot $archive
    if test $use_map_grpc -eq 1
        set map_command map-grpc
        set --append map_operands $mapped_device_output.sock
    end
    # This `psub` reads the key document left on standard input and keeps its
    # temporary key file available for the lifetime of the mapper job.
    $DEVICEFS_PBS_CLIENT $map_command --keyfile (psub --file) \
        $map_operands &>$map_output &
    set -g view_map_pid $last_pid
    set -g view_map_exit_code 1
    # Save the mapper's result so `finish_view` can report an unexpected exit
    # even when the process ended before cleanup began.
    function record_view_map_exit --on-process-exit $view_map_pid
        set -g view_map_exit_code $argv[3]
    end
    publish_children $view_map_pid

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
        (command -v $rpc_helper) </dev/null &
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

argparse /parallel-images /print-manifest /list-backups /view -- $argv || exit

# The first three positional arguments are supervisor control-file paths and
# the local backup ID. Remaining arguments belong to the selected operation.
set pid_file $argv[1]
set stop_file $argv[2]
set backup_id $argv[3]

# `StartPbsFish` in pbs.cpp supplies every NUL-delimited record below, including
# empty records for unused values. The remaining bytes are the encryption-key
# document for operations that need it. Reading extra records would consume key
# data, so changes to the record order must be made in both producer and consumer.
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
    set operation run_view
else if set --query _flag_print_manifest
    set operation print_manifest
else if set --query _flag_list_backups
    set operation list_backups
end
$operation $argv[4..]
