# SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
# SPDX-License-Identifier: GPL-3.0-or-later

# Invoked by the native entry point with fish --no-config. Device identifiers
# come only from attachments owned by this run; supplied sources are never
# included in the detach list.
if test "$argv[1]" = --
    set -e argv[1]
end
set -g native "$argv[1]"
set -g output_root "$argv[2]"
set -g producer "$argv[3]"
set -g supplied_source "$argv[4]"
set -g supplied_snapshot "$argv[5]"
set -g jq "$argv[6]"
set -g source_disk
set -g seed_disk
set -g target_disk
set -g fuse_pid
set -g fuse_exit_status 1
set -g run
set -g mounted false
set -g restore_started false
set -g producer_finished false
set -g primary_status 0

function checked --argument-names operation
    $argv[2..]
    set -l code $status
    if test $code -ne 0
        printf 'Could not %s.\nCommand: %s\n' $operation \
            (string join ' ' -- (string escape -- $argv[2..])) >&2
    end
    return $code
end

function logged --argument-names log
    checked $argv[2..] 2>&1 | tee -a -- $log >&2
    return $pipestatus[1]
end

function report_cleanup_error --argument-names operation code
    printf 'Cleanup failed: %s (status %s). Run files: %s\n' "$operation" "$code" "$run" >&2
    if test $primary_status -eq 0
        set -g primary_status "$code"
    end
end

function marker --argument-names token
    printf '%s\n' "$token" >"$run/fuse/marker"
end

function cleanup
    if test "$mounted" = true
        if test $primary_status -ne 0
            marker failure || report_cleanup_error 'mark trace incomplete' $status
        end
        if test "$restore_started" = true && test "$producer_finished" = false
            marker producer-failure || report_cleanup_error 'mark producer failure' $status
            set -g producer_finished true
        end
    end

    if test -n "$target_disk"
        logged "$run/cleanup.log" "detach target '$target_disk'" \
            /usr/bin/hdiutil detach "$target_disk"
        set -l code $status
        if test $code -eq 0
            set -g target_disk
        else
            report_cleanup_error "detach target $target_disk" $code
        end
    end

    if test -n "$seed_disk"
        logged "$run/cleanup.log" "detach initial fixture '$seed_disk'" \
            /usr/bin/hdiutil detach "$seed_disk"
        set -l code $status
        if test $code -eq 0
            set -g seed_disk
        else
            report_cleanup_error "detach initial fixture $seed_disk" $code
        end
    end

    if test -n "$source_disk"
        logged "$run/cleanup.log" "detach source fixture '$source_disk'" \
            /usr/bin/hdiutil detach "$source_disk"
        set -l code $status
        if test $code -eq 0
            set -g source_disk
        else
            report_cleanup_error "detach source fixture $source_disk" $code
        end
    end

    if test "$mounted" = true
        if test -z "$target_disk"
            if test $primary_status -ne 0
                marker failure || report_cleanup_error 'mark trace incomplete' $status
            end
            if test "$producer_finished" = true
                marker detached || report_cleanup_error 'mark target detached' $status
            end
            logged "$run/cleanup.log" "unmount macFUSE at '$run/fuse'" \
                /sbin/umount "$run/fuse"
            set -l code $status
            if test $code -eq 0
                set -g mounted false
            else
                report_cleanup_error 'unmount macFUSE' $code
            end
        else
            # A failed detach may leave DiskImages using this filesystem. Keep
            # its owner alive, and retain the attachment for diagnosis.
            printf 'Target %s remains attached; macFUSE process %s remains running.\n' \
                "$target_disk" "$fuse_pid" >&2
        end
    end

    if test -n "$fuse_pid" && test "$mounted" = false
        wait "$fuse_pid"
        set -l code $fuse_exit_status
        set -g fuse_pid
        cat -- "$run/filesystem.log" >&2
        if test $code -ne 0
            report_cleanup_error 'finish macFUSE trace' $code
        end
    end
end

function interrupted --on-signal INT
    if test $primary_status -eq 0
        set -g primary_status 130
    end
end

function terminated --on-signal TERM
    if test $primary_status -eq 0
        set -g primary_status 143
    end
end

function plist_json --argument-names plist
    checked "convert '$plist' to JSON" \
        /usr/bin/plutil -convert json -o "$plist.json" "$plist"
end

function attached_disk --argument-names plist
    plist_json "$plist" || return
    # Formatted APFS images also expose a synthesized whole disk. Select the
    # actual GPT device; only an unformatted seed has a single bare whole disk.
    checked "identify the backing disk in '$plist'" $jq -er \
        '[
            ."system-entities"[]
            | select(."dev-entry"? | type == "string")
            | select(."dev-entry" | test("^/dev/disk[0-9]+$"))
        ] as $whole
        | [$whole[] | select(."content-hint" == "GUID_partition_scheme")] as $gpt
        | (if ($gpt | length) > 0 then $gpt else $whole end)
        | if length == 1 then
            .[0]."dev-entry"
          else
            error("attachment did not identify one backing whole disk")
          end' \
        "$plist.json"
end

function target_container --argument-names whole prefix
    checked "list partitions on target disk '$whole'" \
        diskutil list -plist $whole >"$prefix-list.plist" || return
    plist_json "$prefix-list.plist" || return
    set -l partition (checked "find the APFS partition on target disk '$whole'" \
        $jq -er --arg whole $whole \
        '[
            .AllDisksAndPartitions[]
            | select(.DeviceIdentifier == ($whole | ltrimstr("/dev/")))
            | .Partitions[]
            | select(.Content == "Apple_APFS")
            | .DeviceIdentifier
        ]
        | if length == 1 then
            "/dev/" + .[0]
          else
            error("target did not contain one APFS partition")
          end' \
        "$prefix-list.plist.json") || return

    checked "inspect target partition '$partition'" \
        diskutil info -plist $partition >"$prefix-partition.plist" || return
    plist_json "$prefix-partition.plist" || return
    checked "find the APFS container for target partition '$partition'" \
        $jq -er '.APFSContainerReference | select(. != null) | "/dev/" + .' \
        "$prefix-partition.plist.json"
end

function prepare_source
    if test -n "$supplied_source"
        set -g source "$supplied_source"
        set -g snapshot "$supplied_snapshot"
        return 0
    end

    logged "$run/source-create.log" "create source fixture '$run/source.dmg'" \
        /usr/bin/hdiutil create -size 2g \
        -fs APFS -volname AsrTraceSource -format UDRW -layout GPTSPUD \
        "$run/source.dmg" || return
    checked "attach source fixture '$run/source.dmg'" \
        /usr/bin/hdiutil attach -plist -nobrowse -mountpoint "$run/source" \
        "$run/source.dmg" >"$run/source-attach.plist" || return
    set -g source_disk (attached_disk "$run/source-attach.plist") || return
    set -g source "$run/source"
    set -g snapshot asr-image-trace-fixture

    logged "$run/source-populate.log" "create fixture payload '$source/payload.bin'" \
        /bin/dd if=/dev/urandom \
        of="$source/payload.bin" bs=1048576 count=96 || return
    printf 'snapshot content\n' >"$source/state.txt" || return
    printf 'present in the snapshot\n' >"$source/removed-after-snapshot.txt" || return
    checked "record checksums of the source fixture in '$source'" \
        /usr/bin/shasum -a 256 "$source/payload.bin" "$source/state.txt" \
        "$source/removed-after-snapshot.txt" >"$run/expected.sha256" || return
    logged "$run/snapshot.log" "create snapshot '$snapshot' on '$source'" \
        $native --create-snapshot "$source" "$snapshot" || return

    # These changes distinguish the requested snapshot from the live source.
    logged "$run/source-populate.log" "modify fixture payload '$source/payload.bin'" \
        /bin/dd if=/dev/urandom \
        of="$source/payload.bin" bs=1048576 count=8 conv=notrunc || return
    printf 'live content after snapshot\n' >"$source/state.txt" || return
    /bin/rm "$source/removed-after-snapshot.txt" || return
    printf 'absent from the snapshot\n' >"$source/created-after-snapshot.txt"
end

function prepare_seed --argument-names size
    # mkfile -n gives the image its logical length without allocating that length.
    # Only the initial empty GPT/APFS metadata is materialized. The initial image
    # stays read-only; the readback and rolling write caches retain data in memory.
    checked "create the empty target image '$run/seed.raw'" \
        mkfile -n "$size" "$run/seed.raw" || return
    if test "$producer" != asr
        return 0
    end
    checked "attach the empty target image '$run/seed.raw'" \
        /usr/bin/hdiutil attach -plist -nomount -readwrite -noautofsck \
        -imagekey diskimage-class=CRawDiskImage "$run/seed.raw" \
        >"$run/seed-attach.plist" || return
    set -g seed_disk (attached_disk "$run/seed-attach.plist") || return
    logged "$run/seed-format.log" "format the target image on '$seed_disk' as APFS" \
        /usr/sbin/diskutil eraseDisk \
        APFS AsrTraceTarget GPT "$seed_disk" || return
    logged "$run/seed-detach.log" "detach the prepared target image '$seed_disk'" \
        /usr/bin/hdiutil detach "$seed_disk" || return
    set -g seed_disk
end

function start_filesystem
    $native --mount "$run/seed.raw" "$run/trace.tsv" "$run/fuse" \
        >"$run/filesystem.log" 2>&1 &
    set -g fuse_pid $last_pid
    function record_filesystem_exit --on-process-exit $fuse_pid
        set -g fuse_exit_status $argv[3]
    end
    # The unique empty mount directory cannot contain a stale readiness file.
    for attempt in (seq 1 300)
        if test -f "$run/fuse/image.raw"
            set -g mounted true
            return 0
        end
        if ! kill -0 "$fuse_pid" 2>/dev/null
            wait "$fuse_pid"
            set -g fuse_pid
            printf 'macFUSE exited before mounting; see %s/filesystem.log\n' "$run" >&2
            cat -- "$run/filesystem.log" >&2
            return 1
        end
        sleep 0.1
    end
    printf 'macFUSE did not mount within 30 seconds; see %s/filesystem.log\n' "$run" >&2
    cat -- "$run/filesystem.log" >&2
    kill -TERM "$fuse_pid"
    return 1
end

function produce_asr
    checked "attach the virtual target image '$run/fuse/image.raw'" \
        /usr/bin/hdiutil attach -plist -nomount -readwrite -noautofsck \
        -imagekey diskimage-class=CRawDiskImage "$run/fuse/image.raw" \
        >"$run/target-attach.plist" || return
    set -g target_disk (attached_disk "$run/target-attach.plist") || return
    set -l container (target_container $target_disk "$run/target") || return

    marker restore-begin || return
    set -g restore_started true
    # A container target without --erase lets ASR create the restored volumes
    # within the existing container (asr(8), "Restoring a snapshot").
    set -l command /usr/sbin/asr restore --source "$source" --target "$container" \
        --noprompt --useReplication --toSnapshot "$snapshot" --verbose --debug
    string escape -- $command >"$run/asr-command.txt" || return
    logged "$run/asr.log" "restore snapshot '$snapshot' from '$source' to '$container'" \
        /usr/bin/sudo -- $command
    finish_producer $status
end

function produce_synthetic
    marker restore-begin || return
    set -g restore_started true
    logged "$run/producer.log" "run the '$producer' producer against '$run/fuse/image.raw'" \
        $native --synthetic "$producer" "$run/fuse/image.raw"
    finish_producer $status
end

function finish_producer --argument-names code
    if test $code -eq 0
        marker producer-success || return
    else
        # A marker error must not replace the producer's original failure.
        test $primary_status -ne 0 || set -g primary_status $code
        marker producer-failure || report_cleanup_error 'mark producer failure' $status
    end
    set -g producer_finished true
    return $code
end

function run_experiment
    mkdir -p -- "$output_root" || return
    set -g run (mktemp -d "$output_root/run-XXXXXX") || return
    mkdir -- "$run/fuse" "$run/source" || return
    printf 'Experiment files: %s\n' "$run"
    checked "record the macOS version" /usr/bin/sw_vers >"$run/macos-version.txt" || return

    if test "$producer" = asr
        prepare_source || return
        printf 'source=%s\nsnapshot=%s\n' "$source" "$snapshot" \
            >"$run/source-selection.txt" || return
        checked "inspect source volume '$source'" \
            /usr/sbin/diskutil info -plist "$source" >"$run/source-info.plist" || return
        checked "list snapshots on source volume '$source'" \
            /usr/sbin/diskutil apfs listSnapshots -plist "$source" \
            >"$run/source-snapshots.plist" || return
        plist_json "$run/source-info.plist" || return
        set -l size (checked "read the size of source volume '$source'" $jq -er \
            '.TotalSize | select(type == "number" and . > 0) | . + 536870912' \
            "$run/source-info.plist.json") || return
        prepare_seed "$size" || return
        start_filesystem || return
        produce_asr
    else
        prepare_seed 4194304 || return
        start_filesystem || return
        produce_synthetic
    end
end

run_experiment
set -l code $status
test $primary_status -ne 0 || set -g primary_status $code
cleanup
if test $primary_status -eq 0
    printf 'Experiment complete: %s/trace.tsv\n' "$run"
else
    printf 'Experiment failed with status %s. Diagnostic files: %s\n' "$primary_status" "$run" >&2
end
exit $primary_status
