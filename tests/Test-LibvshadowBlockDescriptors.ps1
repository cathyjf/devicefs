# SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
# SPDX-License-Identifier: GPL-3.0-or-later

#requires -Version 7.4
#requires -RunAsAdministrator
#requires -Modules Hyper-V

<#
.SYNOPSIS
Checks descriptors plus System Volume Information extents against differences.

.DESCRIPTION
Creates a fixed VHD containing one NTFS volume, writes a file, takes a VSS
snapshot, overwrites one allocated range, and takes another snapshot. It
compares the snapshots in 16 KiB blocks, copies the source volume to an ordinary
file, and constructs a conservative map from an unmodified vshadowinfo
executable. It also adds every 16 KiB block intersecting allocated file-data,
named data-stream, or directory extents reachable at or beneath System Volume
Information in either snapshot. The test must run as SYSTEM so it can enumerate
that directory. It temporarily enables SeBackupPrivilege while querying those
extents. Named streams of reparse points are not queried, and reparse targets
are not followed.

The descriptor set includes the 16 KiB block containing every in-range original
and store-data offset printed for every snapshot store, plus each forwarder's
in-range relative offset. File, directory, and named data-stream extents are
obtained directly from NTFS with FSCTL_GET_RETRIEVAL_POINTERS.

The VHD and snapshots are removed during cleanup. Use -KeepArtifactsOnFailure
to retain the detached VHD and vshadowinfo output after a failure.
#>

[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string] $VShadowInfoPath,

    [switch] $KeepArtifactsOnFailure
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$file_backed_virtual_bus_type = [UInt16]15
$microsoft_software_provider =
    [Guid]'b5946137-7b9f-4925-af80-51abd60b20d5'
$vss_block_size = 16KB
$vshadow_forwarder_flag = 1

function Assert-Condition {
    param(
        [Parameter(Mandatory)]
        [bool] $Condition,

        [Parameter(Mandatory)]
        [string] $Message
    )

    if (-not $Condition) {
        throw $Message
    }
}

function Assert-BytesEqual {
    param(
        [Parameter(Mandatory)]
        [byte[]] $Expected,

        [Parameter(Mandatory)]
        [byte[]] $Actual,

        [Parameter(Mandatory)]
        [string] $Description
    )

    Assert-Condition ($Expected.Length -eq $Actual.Length) `
        "$Description lengths differ."
    for ($i = 0; $i -lt $Expected.Length; ++$i) {
        if ($Expected[$i] -ne $Actual[$i]) {
            throw "$Description differs at byte $i."
        }
    }
}

function New-TestShadowCopy {
    param(
        [Parameter(Mandatory)]
        [string] $VolumeName,

        [Parameter(Mandatory)]
        [AllowEmptyCollection()]
        [Collections.Generic.List[Guid]] $SnapshotIds
    )

    $result = Invoke-CimMethod -ClassName Win32_ShadowCopy `
        -MethodName Create -Arguments @{
            Volume = $VolumeName
            Context = 'ClientAccessible'
        }
    Assert-Condition ($result.ReturnValue -eq 0) `
        "VSS snapshot creation failed with result $($result.ReturnValue)."

    $snapshot_id = [Guid]$result.ShadowID
    $SnapshotIds.Add($snapshot_id)
    $matches = @(Get-CimInstance -ClassName Win32_ShadowCopy |
        Where-Object { [Guid]$_.ID -eq $snapshot_id })
    Assert-Condition ($matches.Count -eq 1) `
        'The new VSS snapshot could not be identified uniquely.'
    return $matches[0]
}

function Read-FileRange {
    param(
        [Parameter(Mandatory)]
        [string] $Path,

        [Parameter(Mandatory)]
        [long] $Offset,

        [Parameter(Mandatory)]
        [int] $Length
    )

    $bytes = [byte[]]::new($Length)
    $stream = [IO.File]::Open(
        $Path, [IO.FileMode]::Open, [IO.FileAccess]::Read,
        [IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete)
    try {
        $stream.Position = $Offset
        $stream.ReadExactly($bytes, 0, $bytes.Length)
    } finally {
        $stream.Dispose()
    }
    return ,$bytes
}

function Get-TreeBlockOffsets {
    param(
        [Parameter(Mandatory)]
        [string] $Root,

        [Parameter(Mandatory)]
        [NtfsBitmap] $Bitmap,

        [Parameter(Mandatory)]
        [int] $BlockSize
    )

    $blocks = [Collections.Generic.HashSet[long]]::new()
    $objects = @(
        Get-Item -LiteralPath $Root -Force
        Get-ChildItem -LiteralPath $Root -Force -Recurse
    )
    foreach ($object in $objects) {
        $is_reparse_point = (($object.Attributes -band
                [IO.FileAttributes]::ReparsePoint) -ne 0)
        $ranges = [DeviceFsTestNative]::GetAllocatedClusterRanges(
            $object.FullName, -not $is_reparse_point)
        foreach ($range in $ranges) {
            Assert-Condition (
                ($range.StartingCluster -ge 0) -and
                    ($range.ClusterCount -gt 0) -and
                    ($range.ClusterCount -le $Bitmap.ClusterCount) -and
                    ($range.StartingCluster -le
                        ($Bitmap.ClusterCount - $range.ClusterCount))) `
                "NTFS returned an invalid extent for '$($object.FullName)'."
            $start = [long](
                $range.StartingCluster * [long]$Bitmap.ClusterSize)
            $end = [long](
                $start + ($range.ClusterCount * [long]$Bitmap.ClusterSize))
            for ($offset = $start - ($start % $BlockSize);
                $offset -lt $end; $offset += $BlockSize) {
                $null = $blocks.Add($offset)
            }
        }
    }
    return ,$blocks
}

if (-not [Environment]::Is64BitProcess) {
    throw 'This integration test requires 64-bit PowerShell.'
}
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$is_system = $identity.IsSystem
$identity.Dispose()
if (-not $is_system) {
    throw 'This integration test must run as NT AUTHORITY\SYSTEM.'
}

$VShadowInfoPath = (Resolve-Path -LiteralPath $VShadowInfoPath).Path
Assert-Condition ([IO.File]::Exists($VShadowInfoPath)) `
    "vshadowinfo was not found at '$VShadowInfoPath'."

$native_source_path = [IO.Path]::Combine(
    $PSScriptRoot, 'types', 'DeviceFsTestNative.cs')
Add-Type -Path $native_source_path

$run_id = [Guid]::NewGuid().ToString('N')
$test_root = $null
$vhd_path = $null
$source_mount = $null
$source_partition = $null
$source_access_path_added = $false
$snapshot_ids = [Collections.Generic.List[Guid]]::new()
$primary_error = $null
$cleanup_errors = [Collections.Generic.List[Exception]]::new()
$image_detached = $false
$backup_privilege = $null

$source_label = "DFSVSS-$($run_id.Substring(0, 17))"
$uninitialized_partition_style = [UInt16]0
$test_vhd_size = [UInt64](1GB)
$ntfs_cluster_size = 4096
$witness_offset = 512KB
$witness_length = 4096

try {
    $test_root = Join-Path $env:WINDIR 'SystemTemp' `
        "devicefs-libvshadow-test-$run_id"
    $test_root = (New-Item -ItemType Directory -Path $test_root).FullName
    $vhd_path = [IO.Path]::Combine($test_root, 'test.vhd')
    $source_mount = [IO.Path]::Combine($test_root, 'source')
    New-Item -ItemType Directory -Path $source_mount | Out-Null

    New-VHD -Path $vhd_path -SizeBytes $test_vhd_size `
        -Fixed | Out-Null
    $disk_image = Mount-DiskImage -ImagePath $vhd_path -StorageType VHD `
        -Access ReadWrite -NoDriveLetter -PassThru
    $disks = @($disk_image | Get-Disk)
    Assert-Condition ($disks.Count -eq 1) `
        'The VHD did not resolve to exactly one disk.'

    $disk = $disks[0]
    Assert-Condition (
        $disk.CimInstanceProperties['BusType'].Value -eq
            $file_backed_virtual_bus_type) `
        'The test disk is not file-backed virtual storage.'
    Assert-Condition (
        ($disk.Size -eq $test_vhd_size) -and (-not $disk.IsBoot) -and
            (-not $disk.IsSystem) -and (-not $disk.IsClustered) -and
            (-not $disk.IsOffline) -and (-not $disk.IsReadOnly) -and
            ($disk.CimInstanceProperties['PartitionStyle'].Value -eq
                $uninitialized_partition_style)) `
        'The new VHD does not satisfy the test-disk safety policy.'
    Assert-Condition (@(Get-Partition -DiskNumber $disk.Number `
            -ErrorAction SilentlyContinue).Count -eq 0) `
        'The new VHD unexpectedly contains partitions.'

    Initialize-Disk -Number $disk.Number -PartitionStyle GPT `
        -PassThru | Out-Null
    $source_partition = New-Partition -DiskNumber $disk.Number `
        -UseMaximumSize -IsHidden
    Set-Partition -InputObject $source_partition `
        -NoDefaultDriveLetter $true -IsHidden $false `
        -Confirm:$false | Out-Null
    $source_partition = Get-Partition -DiskNumber $disk.Number `
        -PartitionNumber $source_partition.PartitionNumber
    Assert-Condition ([char]$source_partition.DriveLetter -eq [char]0) `
        'The test partition unexpectedly acquired a drive letter.'
    Format-Volume -Partition $source_partition -FileSystem NTFS `
        -AllocationUnitSize $ntfs_cluster_size `
        -NewFileSystemLabel $source_label -Force -Confirm:$false | Out-Null
    Add-PartitionAccessPath -InputObject $source_partition `
        -AccessPath $source_mount
    $source_access_path_added = $true

    $source_volume_name = [DeviceFsTestNative]::GetVolumeName($source_mount)
    $source_identity = [DeviceFsTestNative]::InspectVolume($source_volume_name)
    Assert-Condition (
        ($source_identity.Label -ceq $source_label) -and
            ($source_identity.DiskNumber -eq $disk.Number) -and
            ($source_identity.DiskStartingOffset -eq $source_partition.Offset) -and
            ($source_identity.DiskExtentLength -eq $source_partition.Size)) `
        'The NTFS volume does not map to the expected VHD partition.'

    $vssadmin = [IO.Path]::Combine(
        $env:WINDIR, 'System32', 'vssadmin.exe')
    $vssadmin_volume = $source_volume_name.TrimEnd([char]'\')
    # Windows 11 creates a missing diff-area association when resizing it.
    $storage_output = @(& $vssadmin resize shadowstorage `
        "/for=$vssadmin_volume" "/on=$vssadmin_volume" `
        '/maxsize=512MB' 2>&1)
    $storage_exit_code = $LASTEXITCODE
    Assert-Condition ($storage_exit_code -eq 0) (
        "VSS storage configuration failed with exit code " +
        "$storage_exit_code`: $($storage_output -join [Environment]::NewLine)")

    $witness_path = [IO.Path]::Combine($source_mount, 'witness.bin')
    $baseline = [byte[]]::new(1MB)
    [Array]::Fill[byte]($baseline, 0x31)
    $stream = [IO.File]::Open(
        $witness_path, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write,
        [IO.FileShare]::Read)
    try {
        $stream.Write($baseline, 0, $baseline.Length)
        $stream.Flush($true)
    } finally {
        $stream.Dispose()
    }

    $snapshot_a = New-TestShadowCopy $source_volume_name $snapshot_ids
    Assert-Condition (
        ([Guid]$snapshot_a.ProviderID -eq $microsoft_software_provider) -and
            $snapshot_a.Differential) `
        'Snapshot A was not made by the Microsoft differential provider.'

    $replacement = [byte[]]::new($witness_length)
    [Array]::Fill[byte]($replacement, 0xA5)
    $stream = [IO.File]::Open(
        $witness_path, [IO.FileMode]::Open, [IO.FileAccess]::Write,
        [IO.FileShare]::Read)
    try {
        $stream.Position = $witness_offset
        $stream.Write($replacement, 0, $replacement.Length)
        $stream.Flush($true)
    } finally {
        $stream.Dispose()
    }

    $snapshot_b = New-TestShadowCopy $source_volume_name $snapshot_ids
    Assert-Condition (
        ([Guid]$snapshot_b.ProviderID -eq $microsoft_software_provider) -and
            $snapshot_b.Differential) `
        'Snapshot B was not made by the Microsoft differential provider.'

    $snapshot_a_file = "$($snapshot_a.DeviceObject)\witness.bin"
    $snapshot_b_file = "$($snapshot_b.DeviceObject)\witness.bin"
    $expected_baseline = [byte[]]::new($witness_length)
    [Array]::Fill[byte]($expected_baseline, 0x31)
    Assert-BytesEqual $expected_baseline `
        (Read-FileRange $snapshot_a_file $witness_offset $witness_length) `
        'Snapshot A witness'
    Assert-BytesEqual $replacement `
        (Read-FileRange $snapshot_b_file $witness_offset $witness_length) `
        'Snapshot B witness'

    $differences = @([DeviceFsTestNative]::GetDifferingBlockOffsets(
        $snapshot_a.DeviceObject, $snapshot_b.DeviceObject, $vss_block_size))
    Assert-Condition ($differences.Count -ne 0) `
        'The snapshot comparison found no changed blocks.'

    $bitmap_a = [DeviceFsTestNative]::GetNtfsBitmap(
        $snapshot_a.DeviceObject, $true)
    $bitmap_b = [DeviceFsTestNative]::GetNtfsBitmap(
        $snapshot_b.DeviceObject, $true)
    Assert-Condition (
        ($bitmap_a.Length -eq $source_identity.Length) -and
            ($bitmap_b.Length -eq $source_identity.Length) -and
            ($bitmap_a.ClusterSize -eq $ntfs_cluster_size) -and
            ($bitmap_b.ClusterSize -eq $ntfs_cluster_size)) `
        'The snapshot NTFS geometry does not match the source volume.'

    $svi_relative_path = 'System Volume Information'
    $backup_privilege = [DeviceFsTestNative]::EnableBackupPrivilege()
    $svi_blocks_a = Get-TreeBlockOffsets `
        "$($snapshot_a.DeviceObject)\$svi_relative_path" `
        $bitmap_a $vss_block_size
    $svi_blocks_b = Get-TreeBlockOffsets `
        "$($snapshot_b.DeviceObject)\$svi_relative_path" `
        $bitmap_b $vss_block_size
    $backup_privilege.Dispose()
    $backup_privilege = $null
    $svi_blocks = [Collections.Generic.HashSet[long]]::new()
    Assert-Condition (
        ($svi_blocks_a.Count -ne 0) -and ($svi_blocks_b.Count -ne 0)) `
        'System Volume Information did not contain allocated extents.'
    foreach ($offset in $svi_blocks_a) {
        $null = $svi_blocks.Add($offset)
    }
    foreach ($offset in $svi_blocks_b) {
        $null = $svi_blocks.Add($offset)
    }

    $vshadow_source = [IO.Path]::Combine(
        $test_root, 'vshadow-source-volume.img')
    [DeviceFsTestNative]::CopyDeviceToFile(
        $source_volume_name, $vshadow_source)
    $vshadow_stdout = [IO.Path]::Combine(
        $test_root, 'vshadowinfo.stdout.txt')
    $vshadow_stderr = [IO.Path]::Combine(
        $test_root, 'vshadowinfo.stderr.txt')
    & $VShadowInfoPath -a $vshadow_source >$vshadow_stdout 2>$vshadow_stderr
    $vshadow_exit_code = $LASTEXITCODE
    $vshadow_output = [IO.File]::ReadAllText($vshadow_stdout)
    $vshadow_error = [IO.File]::ReadAllText($vshadow_stderr)
    Assert-Condition ($vshadow_exit_code -eq 0) `
        "vshadowinfo exited with code $($vshadow_exit_code): $vshadow_error"

    foreach ($snapshot in @($snapshot_a, $snapshot_b)) {
        $copy_id = ([Guid]$snapshot.ID).ToString()
        Assert-Condition (
            $vshadow_output.Contains(
                $copy_id, [StringComparison]::OrdinalIgnoreCase)) `
            "vshadowinfo did not report snapshot $copy_id."
    }

    $candidate = [Collections.Generic.HashSet[long]]::new()
    $original = $null
    $relative = $null
    $store_offset = $null
    $reported_descriptor_count = 0
    $descriptor_count = 0
    foreach ($line in $vshadow_output -split '\r?\n') {
        if ($line -match '^\s+Number of blocks\s+:\s+(\d+)$') {
            $reported_descriptor_count += [Convert]::ToInt32($Matches[1])
        } elseif ($line -match
            '^\s+Original offset\s+:\s+0x([0-9a-f]+)$') {
            $original = [Convert]::ToInt64($Matches[1], 16)
        } elseif ($line -match
            '^\s+Relative offset\s+:\s+0x([0-9a-f]+)$') {
            $relative = [Convert]::ToInt64($Matches[1], 16)
        } elseif ($line -match '^\s+Offset\s+:\s+0x([0-9a-f]+)$') {
            $store_offset = [Convert]::ToInt64($Matches[1], 16)
        } elseif ($line -match '^\s+Flags\s+:\s+0x([0-9a-f]+)$') {
            Assert-Condition (
                ($null -ne $original) -and ($null -ne $relative) -and
                    ($null -ne $store_offset)) `
                'vshadowinfo printed an incomplete block descriptor.'
            $flags = [Convert]::ToUInt32($Matches[1], 16)
            $offsets = @($original, $store_offset)
            if (($flags -band $vshadow_forwarder_flag) -ne 0) {
                $offsets += $relative
            }
            foreach ($offset in $offsets) {
                if (($offset -ge 0) -and
                    ($offset -lt $source_identity.Length)) {
                    $block_offset =
                        $offset - ($offset % $vss_block_size)
                    $null = $candidate.Add($block_offset)
                }
            }
            ++$descriptor_count
            $original = $null
            $relative = $null
            $store_offset = $null
        }
    }
    Assert-Condition (
        $descriptor_count -eq $reported_descriptor_count) `
        'vshadowinfo block descriptor output was incomplete.'

    $combined = [Collections.Generic.HashSet[long]]::new()
    foreach ($offset in $candidate) {
        $null = $combined.Add($offset)
    }
    foreach ($offset in $svi_blocks) {
        $null = $combined.Add($offset)
    }

    $descriptor_covered = 0
    $svi_covered = 0
    $missing = [Collections.Generic.List[long]]::new()
    foreach ($offset in $differences) {
        if ($candidate.Contains($offset)) {
            ++$descriptor_covered
        } elseif ($svi_blocks.Contains($offset)) {
            ++$svi_covered
        } else {
            $missing.Add($offset)
        }
    }
    [IO.File]::WriteAllLines(
        [IO.Path]::Combine($test_root, 'actual-changed-blocks.txt'),
        @($differences | ForEach-Object { '0x{0:X}' -f $_ }))
    [IO.File]::WriteAllLines(
        [IO.Path]::Combine($test_root, 'descriptor-blocks.txt'),
        @($candidate | Sort-Object | ForEach-Object { '0x{0:X}' -f $_ }))
    [IO.File]::WriteAllLines(
        [IO.Path]::Combine(
            $test_root, 'system-volume-information-blocks.txt'),
        @($svi_blocks | Sort-Object | ForEach-Object { '0x{0:X}' -f $_ }))
    [IO.File]::WriteAllLines(
        [IO.Path]::Combine($test_root, 'candidate-blocks.txt'),
        @($combined | Sort-Object | ForEach-Object { '0x{0:X}' -f $_ }))

    if ($missing.Count -ne 0) {
        $sample = @($missing | Select-Object -First 8 |
            ForEach-Object { '0x{0:X}' -f $_ }) -join ', '
        throw "the conservative map missed $($missing.Count) changed " +
            "block(s): $sample"
    }

} catch {
    $primary_error = $_
} finally {
    if ($null -ne $backup_privilege) {
        try {
            $backup_privilege.Dispose()
            $backup_privilege = $null
        } catch {
            $cleanup_errors.Add($_.Exception)
        }
    }

    for ($i = $snapshot_ids.Count - 1; $i -ge 0; --$i) {
        try {
            $snapshot_id = $snapshot_ids[$i]
            $matches = @(Get-CimInstance -ClassName Win32_ShadowCopy |
                Where-Object { [Guid]$_.ID -eq $snapshot_id })
            foreach ($snapshot in $matches) {
                Remove-CimInstance -InputObject $snapshot
            }
        } catch {
            $cleanup_errors.Add($_.Exception)
        }
    }

    if ($source_access_path_added) {
        try {
            Remove-PartitionAccessPath -InputObject $source_partition `
                -AccessPath $source_mount -Confirm:$false
        } catch {
            $cleanup_errors.Add($_.Exception)
        }
    }

    if (($null -ne $vhd_path) -and [IO.File]::Exists($vhd_path)) {
        try {
            $current_image = Get-DiskImage -ImagePath $vhd_path
            if ($current_image.Attached) {
                Dismount-DiskImage -ImagePath $vhd_path | Out-Null
            }
            $image_detached =
                -not (Get-DiskImage -ImagePath $vhd_path).Attached
        } catch {
            $cleanup_errors.Add($_.Exception)
        }
    } else {
        $image_detached = $true
    }

    $preserve = (($null -ne $primary_error) -and $KeepArtifactsOnFailure) -or
        ($cleanup_errors.Count -ne 0) -or (-not $image_detached)
    if (($null -ne $test_root) -and
        (Test-Path -LiteralPath $test_root) -and (-not $preserve)) {
        try {
            Remove-Item -LiteralPath $test_root -Recurse -Force
        } catch {
            $cleanup_errors.Add($_.Exception)
            Write-Warning "Test artifacts were preserved at '$test_root'."
        }
    } elseif (($null -ne $test_root) -and
        (Test-Path -LiteralPath $test_root)) {
        Write-Warning "Test artifacts were preserved at '$test_root'."
    }
}

if ($null -ne $primary_error) {
    foreach ($cleanup_error in $cleanup_errors) {
        Write-Warning "Cleanup also failed: $($cleanup_error.Message)"
    }
    throw $primary_error
}
if ($cleanup_errors.Count -ne 0) {
    throw [AggregateException]::new('The test passed, but cleanup failed.',
        $cleanup_errors)
}

$overincluded = $combined.Count - $differences.Count
$volume_blocks = [Math]::Ceiling(
    $source_identity.Length / [double]$vss_block_size)
$candidate_percent = 100.0 * $combined.Count / $volume_blocks
Write-Host (
    "PASS: $($differences.Count) changed 16 KiB block(s) were covered: " +
    "$descriptor_covered by descriptors and $svi_covered only by System " +
    "Volume Information extents. The conservative map contains " +
    "$($combined.Count) block(s) " +
    ("({0:F2}% of the volume); " -f $candidate_percent) +
    "$overincluded candidate block(s) were additional.")
