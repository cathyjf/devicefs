# SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
# SPDX-License-Identifier: GPL-3.0-or-later

#requires -Version 7.4
#requires -RunAsAdministrator
#requires -Modules Hyper-V

<#
.SYNOPSIS
Checks a conservative VSS change map against devicefs synthetic views.

.DESCRIPTION
Creates a fixed VHD containing one NTFS volume and retains three VSS snapshots
around two mutation intervals. Each interval includes a large in-place
overwrite, a file creation, a file deletion, and a sector-sized overwrite of a
fixed file. One devicefs process exposes all three snapshots with
--synthetic-free-clusters, and the test compares consecutive views in 16 KiB
blocks. For each interval it constructs a conservative map from the preceding
snapshot's VSS descriptor store, changes to the NTFS allocation bitmap, and
allocated file-data, named data-stream, or directory extents reachable at or
beneath System Volume Information in either endpoint snapshot.

The default parameter set tests vss-descriptor-dump. Supply only
-VShadowInfoPath to test stock vshadowinfo instead. Supplying both parser paths
adds exact descriptor parity checks against the same source image.
When vss-descriptor-dump is used, the test also reports non-failing probes that
compare raw VSS metadata from snapshot B while it is latest and after snapshot
C makes it no longer latest, as well as from C while C is latest.

The test must run as SYSTEM so it can enumerate System Volume Information. It
temporarily enables SeBackupPrivilege while querying those extents. Named
streams of reparse points are not queried, and reparse targets are not followed.

For each snapshot store, the descriptor set includes the 16 KiB block
containing every in-range original and store-data offset, plus each forwarder's
in-range relative offset. File, directory, and named data-stream extents are
obtained directly from NTFS with FSCTL_GET_RETRIEVAL_POINTERS.

Each large overwrite is sufficient to exercise a chained VSS block list. The
test also requires every map component to contribute independently and rejects
a map covering the entire volume. The VHD and snapshots are removed during
cleanup. Use -KeepArtifactsOnFailure to retain the detached VHD, process logs,
and descriptor-tool output after a failure.
#>

[CmdletBinding(DefaultParameterSetName = 'DescriptorDump')]
param(
    [Parameter(Mandatory, Position = 0, ParameterSetName = 'DescriptorDump')]
    [Parameter(Mandatory, Position = 0, ParameterSetName = 'Parity')]
    [string] $VssDescriptorDumpPath,

    [Parameter(Mandatory, ParameterSetName = 'VShadowInfo')]
    [Parameter(Mandatory, ParameterSetName = 'Parity')]
    [string] $VShadowInfoPath,

    [Parameter(Mandatory)]
    [string] $DeviceFsPath,

    [switch] $KeepArtifactsOnFailure
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
$file_backed_virtual_bus_type = [UInt16]15
$microsoft_software_provider =
    [Guid]'b5946137-7b9f-4925-af80-51abd60b20d5'
$vss_block_size = 16KB
$vshadow_forwarder_flag = 1
$vshadow_overlay_flag = 2
. ([IO.Path]::Combine(
        $PSScriptRoot, 'include', 'DeviceFsTestProcess.ps1'))

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

function Write-FilePattern {
    param(
        [Parameter(Mandatory)]
        [string] $Path,

        [Parameter(Mandatory)]
        [long] $Offset,

        [Parameter(Mandatory)]
        [long] $Length,

        [Parameter(Mandatory)]
        [byte] $Value,

        [switch] $CreateNew
    )

    $buffer_size = [int][Math]::Min([long]1MB, $Length)
    $buffer = [byte[]]::new($buffer_size)
    [Array]::Fill[byte]($buffer, $Value)
    $mode = if ($CreateNew) {
        [IO.FileMode]::CreateNew
    } else {
        [IO.FileMode]::Open
    }
    $stream = [IO.File]::Open(
        $Path, $mode, [IO.FileAccess]::Write, [IO.FileShare]::Read)
    try {
        $stream.Position = $Offset
        for ($remaining = $Length; $remaining -gt 0;) {
            $count = [int][Math]::Min($buffer.Length, $remaining)
            $stream.Write($buffer, 0, $count)
            $remaining -= $count
        }
        $stream.Flush($true)
    } finally {
        $stream.Dispose()
    }
}

function Get-ObjectBlockOffsets {
    param(
        [Parameter(Mandatory)]
        [string] $Path,

        [Parameter(Mandatory)]
        [NtfsBitmap] $Bitmap,

        [Parameter(Mandatory)]
        [int] $BlockSize,

        [Parameter(Mandatory)]
        [bool] $EnumerateNamedDataStreams
    )

    $blocks = [Collections.Generic.HashSet[long]]::new()
    $ranges = [DeviceFsTestNative]::GetAllocatedClusterRanges(
        $Path, $EnumerateNamedDataStreams)
    foreach ($range in $ranges) {
        Assert-Condition (
            ($range.StartingCluster -ge 0) -and
                ($range.ClusterCount -gt 0) -and
                ($range.ClusterCount -le $Bitmap.ClusterCount) -and
                ($range.StartingCluster -le
                    ($Bitmap.ClusterCount - $range.ClusterCount))) `
            "NTFS returned an invalid extent for '$Path'."
        $start = [long](
            $range.StartingCluster * [long]$Bitmap.ClusterSize)
        $end = [long](
            $start + ($range.ClusterCount * [long]$Bitmap.ClusterSize))
        for ($offset = $start - ($start % $BlockSize);
            $offset -lt $end; $offset += $BlockSize) {
            $null = $blocks.Add($offset)
        }
    }
    return ,$blocks
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
        $object_blocks = Get-ObjectBlockOffsets $object.FullName $Bitmap `
            $BlockSize (-not $is_reparse_point)
        foreach ($offset in $object_blocks) {
            $null = $blocks.Add($offset)
        }
    }
    return ,$blocks
}

function Find-AllocationTransitionCluster {
    param(
        [Parameter(Mandatory)]
        [string] $Path,

        [Parameter(Mandatory)]
        [NtfsBitmap] $Before,

        [Parameter(Mandatory)]
        [NtfsBitmap] $After,

        [Parameter(Mandatory)]
        [bool] $BecameAllocated
    )

    foreach ($range in [DeviceFsTestNative]::GetAllocatedClusterRanges(
            $Path, $false)) {
        Assert-Condition (
            ($range.StartingCluster -ge 0) -and
                ($range.ClusterCount -gt 0) -and
                ($range.ClusterCount -le $After.ClusterCount) -and
                ($range.StartingCluster -le
                    ($After.ClusterCount - $range.ClusterCount))) `
            "NTFS returned an invalid extent for '$Path'."
        $end = $range.StartingCluster + $range.ClusterCount
        for ($cluster = $range.StartingCluster; $cluster -lt $end; ++$cluster) {
            $was_allocated = $Before.IsAllocated($cluster)
            $is_allocated = $After.IsAllocated($cluster)
            if (($is_allocated -eq $BecameAllocated) -and
                ($was_allocated -ne $is_allocated)) {
                return $cluster
            }
        }
    }

    $direction = if ($BecameAllocated) {
        'free to allocated'
    } else {
        'allocated to free'
    }
    throw "'$Path' did not contain a $direction cluster."
}

function Assert-ControlledOverwrite {
    param(
        [Parameter(Mandatory)]
        [string] $Description,

        [Parameter(Mandatory)]
        [string] $BeforePath,

        [Parameter(Mandatory)]
        [string] $AfterPath,

        [Parameter(Mandatory)]
        [NtfsBitmap] $BeforeBitmap,

        [Parameter(Mandatory)]
        [NtfsBitmap] $AfterBitmap,

        [Parameter(Mandatory)]
        $Differences,

        [Parameter(Mandatory)]
        $OriginalBlocks,

        [Parameter(Mandatory)]
        $AllocationBlocks,

        [Parameter(Mandatory)]
        $SviBlocks,

        [Parameter(Mandatory)]
        [int] $BlockSize
    )

    $before_blocks = Get-ObjectBlockOffsets `
        $BeforePath $BeforeBitmap $BlockSize $false
    $after_blocks = Get-ObjectBlockOffsets `
        $AfterPath $AfterBitmap $BlockSize $false
    Assert-Condition ($before_blocks.SetEquals($after_blocks)) `
        "$Description changed its 16 KiB allocation between snapshots."

    $controlled = 0
    foreach ($offset in $before_blocks) {
        if ($Differences.Contains($offset) -and
            $OriginalBlocks.Contains($offset) -and
            (-not $AllocationBlocks.Contains($offset)) -and
            (-not $SviBlocks.Contains($offset))) {
            ++$controlled
        }
    }
    # One 16 KiB block-list record holds at most 508 descriptors.
    Assert-Condition ($controlled -gt 512) (
        "$Description did not produce more than 512 stable changed blocks " +
        "in its VSS store's original-offset records.")
    return $controlled
}

function Test-DeltaCoverage {
    param(
        [Parameter(Mandatory)]
        [string] $Name,

        [Parameter(Mandatory)]
        $Differences,

        [Parameter(Mandatory)]
        $DescriptorBlocks,

        [Parameter(Mandatory)]
        $AllocationBlocks,

        [Parameter(Mandatory)]
        $SviBlocks,

        [Parameter(Mandatory)]
        [string] $ArtifactRoot,

        [Parameter(Mandatory)]
        [long] $VolumeLength,

        [Parameter(Mandatory)]
        [int] $BlockSize
    )

    $combined = [Collections.Generic.HashSet[long]]::new()
    foreach ($set in @($DescriptorBlocks, $AllocationBlocks, $SviBlocks)) {
        foreach ($offset in $set) {
            $null = $combined.Add($offset)
        }
    }

    # Bits represent descriptor, allocation-change, and SVI coverage.
    $coverage_masks = [long[]]::new(8)
    $missing = [Collections.Generic.List[long]]::new()
    foreach ($offset in $Differences) {
        $mask = 0
        if ($DescriptorBlocks.Contains($offset)) {
            $mask = $mask -bor 1
        }
        if ($AllocationBlocks.Contains($offset)) {
            $mask = $mask -bor 2
        }
        if ($SviBlocks.Contains($offset)) {
            $mask = $mask -bor 4
        }
        ++$coverage_masks[$mask]
        if ($mask -eq 0) {
            $missing.Add($offset)
        }
    }

    foreach ($artifact in @(
            @('synthetic-changed-blocks', $Differences),
            @('descriptor-blocks', $DescriptorBlocks),
            @('allocation-change-blocks', $AllocationBlocks),
            @('system-volume-information-blocks', $SviBlocks),
            @('candidate-blocks', $combined))) {
        [IO.File]::WriteAllLines(
            [IO.Path]::Combine(
                $ArtifactRoot, "$Name-$($artifact[0]).txt"),
            @($artifact[1] | Sort-Object |
                ForEach-Object { '0x{0:X}' -f $_ }))
    }

    if ($missing.Count -ne 0) {
        $sample = @($missing | Select-Object -First 8 |
            ForEach-Object { '0x{0:X}' -f $_ }) -join ', '
        throw "$Name conservative map missed $($missing.Count) changed " +
            "block(s): $sample"
    }

    $volume_blocks = [long][Math]::Ceiling(
        $VolumeLength / [double]$BlockSize)
    Assert-Condition ($combined.Count -lt $volume_blocks) `
        "$Name conservative map covered the entire volume."
    return [pscustomobject]@{
        Name = $Name
        DifferenceCount = $Differences.Count
        DescriptorOnly = $coverage_masks[1]
        AllocationOnly = $coverage_masks[2]
        SviOnly = $coverage_masks[4]
        Overlapping = $coverage_masks[3] + $coverage_masks[5] +
            $coverage_masks[6] + $coverage_masks[7]
        CandidateCount = $combined.Count
        Overincluded = $combined.Count - $Differences.Count
        CandidatePercent = 100.0 * $combined.Count / $volume_blocks
    }
}

if (-not [Environment]::Is64BitProcess) {
    throw 'This integration test requires 64-bit PowerShell.'
}
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$is_system = $identity.IsSystem
$read_user = $identity.Name
$identity.Dispose()
if (-not $is_system) {
    throw 'This integration test must run as NT AUTHORITY\SYSTEM.'
}

$use_descriptor_dump =
    $PSCmdlet.ParameterSetName -ne 'VShadowInfo'
$parity_requested = $PSCmdlet.ParameterSetName -eq 'Parity'
if ($use_descriptor_dump) {
    . ([IO.Path]::Combine(
            $PSScriptRoot, 'include', 'VssDescriptorOutput.ps1'))
    $VssDescriptorDumpPath =
        (Resolve-Path -LiteralPath $VssDescriptorDumpPath).Path
    Assert-Condition ([IO.File]::Exists($VssDescriptorDumpPath)) `
        "vss-descriptor-dump was not found at '$VssDescriptorDumpPath'."
}
if ($PSCmdlet.ParameterSetName -ne 'DescriptorDump') {
    $VShadowInfoPath =
        (Resolve-Path -LiteralPath $VShadowInfoPath).Path
    Assert-Condition ([IO.File]::Exists($VShadowInfoPath)) `
        "vshadowinfo was not found at '$VShadowInfoPath'."
}
$DeviceFsPath = (Resolve-Path -LiteralPath $DeviceFsPath).Path
Assert-Condition ([IO.File]::Exists($DeviceFsPath)) `
    "devicefs was not found at '$DeviceFsPath'."

$native_source_path = [IO.Path]::Combine(
    $PSScriptRoot, 'types', 'DeviceFsTestNative.cs')
if ($null -ne ([Management.Automation.PSTypeName]'DeviceFsTestNative').Type) {
    throw 'DeviceFsTestNative is already loaded. Run the test in a fresh pwsh process.'
}
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
$devicefs_invocation = $null
$devicefs_process_gone = $true
$delta_results = $null
$controlled_ab = 0
$controlled_bc = 0
$repeated_hot_blocks = 0
$forwarder_count = 0
$overlay_count = 0
$parity_failures = [Collections.Generic.List[string]]::new()
$endpoint_raw_diagnostics = [Collections.Generic.List[string]]::new()

$source_label = "DFSVSS-$($run_id.Substring(0, 17))"
$uninitialized_partition_style = [UInt16]0
$test_vhd_size = [UInt64](1GB)
$ntfs_cluster_size = 4096
$overwrite_length = 12MB
$transition_file_length = 4MB
$hot_file_length = 16KB
$sector_write_length = 512
$sample_length = 4096

try {
    $test_root = Join-Path $env:WINDIR 'SystemTemp' `
        "devicefs-libvshadow-test-$run_id"
    $test_root = (New-Item -ItemType Directory -Path $test_root).FullName
    $vhd_path = [IO.Path]::Combine($test_root, 'test.vhd')
    $source_mount = [IO.Path]::Combine($test_root, 'source')
    New-Item -ItemType Directory -Path $source_mount | Out-Null
    $synthetic_mount = [IO.Path]::Combine($test_root, 'synthetic')

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

    $bulk_ab_path = [IO.Path]::Combine($source_mount, 'bulk-ab.bin')
    $bulk_bc_path = [IO.Path]::Combine($source_mount, 'bulk-bc.bin')
    $hot_path = [IO.Path]::Combine($source_mount, 'hot.bin')
    $deleted_ab_path = [IO.Path]::Combine($source_mount, 'deleted-ab.bin')
    $turnover_path = [IO.Path]::Combine($source_mount, 'turnover.bin')
    $created_bc_path = [IO.Path]::Combine($source_mount, 'created-bc.bin')
    Write-FilePattern $bulk_ab_path 0 $overwrite_length 0x31 -CreateNew
    Write-FilePattern $bulk_bc_path 0 $overwrite_length 0x41 -CreateNew
    Write-FilePattern $hot_path 0 $hot_file_length 0x11 -CreateNew
    Write-FilePattern $deleted_ab_path 0 $transition_file_length 0x52 `
        -CreateNew

    $snapshot_a = New-TestShadowCopy $source_volume_name $snapshot_ids
    Assert-Condition (
        ([Guid]$snapshot_a.ProviderID -eq $microsoft_software_provider) -and
            $snapshot_a.Differential) `
        'Snapshot A was not made by the Microsoft differential provider.'

    # Allocate the new file before deleting the old one so both transition
    # directions are represented by distinct clusters.
    Write-FilePattern $turnover_path 0 $transition_file_length 0xC3 `
        -CreateNew
    Write-FilePattern $bulk_ab_path 0 $overwrite_length 0xA5
    Write-FilePattern $hot_path 0 $sector_write_length 0xB2
    Remove-Item -LiteralPath $deleted_ab_path -Force

    $snapshot_b = New-TestShadowCopy $source_volume_name $snapshot_ids
    Assert-Condition (
        ([Guid]$snapshot_b.ProviderID -eq $microsoft_software_provider) -and
            $snapshot_b.Differential) `
        'Snapshot B was not made by the Microsoft differential provider.'

    Write-FilePattern $created_bc_path 0 $transition_file_length 0xD3 `
        -CreateNew
    Write-FilePattern $bulk_bc_path 0 $overwrite_length 0xB5
    Write-FilePattern $hot_path $sector_write_length `
        $sector_write_length 0xD4
    Remove-Item -LiteralPath $turnover_path -Force

    # Test B immediately before it has a successor. This is supplemental
    # evidence about whether only the latest snapshot has an unstable raw VSS
    # metadata view.
    if ($use_descriptor_dump) {
        try {
            $latest_b_copy_id = ([Guid]$snapshot_a.ID).ToString('D')
            $latest_b_stdout = [IO.Path]::Combine(
                $test_root, 'endpoint-b-while-latest-select-a.stdout.txt')
            $latest_b_stderr = [IO.Path]::Combine(
                $test_root, 'endpoint-b-while-latest-select-a.stderr.txt')
            & $VssDescriptorDumpPath --source $snapshot_b.DeviceObject `
                --snapshot-id $latest_b_copy_id `
                >$latest_b_stdout 2>$latest_b_stderr
            $latest_b_exit_code = $LASTEXITCODE
            if ($latest_b_exit_code -ne 0) {
                $latest_b_error =
                    ([IO.File]::ReadAllText($latest_b_stderr) -replace
                        '\r?\n', ' ').Trim()
                $endpoint_raw_diagnostics.Add(
                    "snapshot B while latest selecting store A: parser " +
                    "exited $($latest_b_exit_code): $latest_b_error")
            } else {
                $latest_b_result = ConvertFrom-VssDescriptorDumpOutput `
                    ([IO.File]::ReadAllText($latest_b_stdout)) `
                    $latest_b_copy_id 0 $vshadow_forwarder_flag `
                    $vshadow_overlay_flag $source_identity.Length `
                    $vss_block_size
                $endpoint_raw_diagnostics.Add(
                    "snapshot B while latest selecting store A: parsed " +
                    "$($latest_b_result.DescriptorCount) descriptor(s) from " +
                    "$($latest_b_result.ListBlockCount) list block(s); " +
                    "volume size $($latest_b_result.VolumeSize), expected " +
                    "$($source_identity.Length)")
            }
        } catch {
            $message = ($_.Exception.Message -replace '\r?\n', ' ').Trim()
            $endpoint_raw_diagnostics.Add(
                "snapshot B while latest selecting store A: probe could " +
                "not be completed: $message")
        }
    }

    $snapshot_c = New-TestShadowCopy $source_volume_name $snapshot_ids
    Assert-Condition (
        ([Guid]$snapshot_c.ProviderID -eq $microsoft_software_provider) -and
            $snapshot_c.Differential) `
        'Snapshot C was not made by the Microsoft differential provider.'

    $snapshot_a_bulk_ab = "$($snapshot_a.DeviceObject)\bulk-ab.bin"
    $snapshot_b_bulk_ab = "$($snapshot_b.DeviceObject)\bulk-ab.bin"
    $snapshot_c_bulk_ab = "$($snapshot_c.DeviceObject)\bulk-ab.bin"
    $snapshot_a_bulk_bc = "$($snapshot_a.DeviceObject)\bulk-bc.bin"
    $snapshot_b_bulk_bc = "$($snapshot_b.DeviceObject)\bulk-bc.bin"
    $snapshot_c_bulk_bc = "$($snapshot_c.DeviceObject)\bulk-bc.bin"
    $snapshot_a_hot = "$($snapshot_a.DeviceObject)\hot.bin"
    $snapshot_b_hot = "$($snapshot_b.DeviceObject)\hot.bin"
    $snapshot_c_hot = "$($snapshot_c.DeviceObject)\hot.bin"
    $snapshot_a_deleted_ab =
        "$($snapshot_a.DeviceObject)\deleted-ab.bin"
    $snapshot_a_turnover = "$($snapshot_a.DeviceObject)\turnover.bin"
    $snapshot_a_created_bc =
        "$($snapshot_a.DeviceObject)\created-bc.bin"
    $snapshot_b_deleted_ab =
        "$($snapshot_b.DeviceObject)\deleted-ab.bin"
    $snapshot_b_turnover = "$($snapshot_b.DeviceObject)\turnover.bin"
    $snapshot_b_created_bc =
        "$($snapshot_b.DeviceObject)\created-bc.bin"
    $snapshot_c_deleted_ab =
        "$($snapshot_c.DeviceObject)\deleted-ab.bin"
    $snapshot_c_turnover = "$($snapshot_c.DeviceObject)\turnover.bin"
    $snapshot_c_created_bc =
        "$($snapshot_c.DeviceObject)\created-bc.bin"
    Assert-Condition (
        (Test-Path -LiteralPath $snapshot_a_deleted_ab -PathType Leaf) -and
            (-not (Test-Path -LiteralPath $snapshot_a_turnover)) -and
            (-not (Test-Path -LiteralPath $snapshot_a_created_bc)) -and
            (-not (Test-Path -LiteralPath $snapshot_b_deleted_ab)) -and
            (Test-Path -LiteralPath $snapshot_b_turnover -PathType Leaf) -and
            (-not (Test-Path -LiteralPath $snapshot_b_created_bc)) -and
            (-not (Test-Path -LiteralPath $snapshot_c_deleted_ab)) -and
            (-not (Test-Path -LiteralPath $snapshot_c_turnover)) -and
            (Test-Path -LiteralPath $snapshot_c_created_bc -PathType Leaf)) `
        'The snapshot file-presence matrix did not match the mutations.'

    $expected_ab_before = [byte[]]::new($sample_length)
    [Array]::Fill[byte]($expected_ab_before, 0x31)
    $expected_ab_after = [byte[]]::new($sample_length)
    [Array]::Fill[byte]($expected_ab_after, 0xA5)
    $expected_bc_before = [byte[]]::new($sample_length)
    [Array]::Fill[byte]($expected_bc_before, 0x41)
    $expected_bc_after = [byte[]]::new($sample_length)
    [Array]::Fill[byte]($expected_bc_after, 0xB5)
    foreach ($offset in @(0, ($overwrite_length / 2),
            ($overwrite_length - $sample_length))) {
        Assert-BytesEqual $expected_ab_before `
            (Read-FileRange $snapshot_a_bulk_ab $offset $sample_length) `
            "Snapshot A bulk A-B sample at $offset"
        foreach ($path in @($snapshot_b_bulk_ab, $snapshot_c_bulk_ab)) {
            Assert-BytesEqual $expected_ab_after `
                (Read-FileRange $path $offset $sample_length) `
                "Post-overwrite bulk A-B sample at $offset"
        }
        foreach ($path in @($snapshot_a_bulk_bc, $snapshot_b_bulk_bc)) {
            Assert-BytesEqual $expected_bc_before `
                (Read-FileRange $path $offset $sample_length) `
                "Pre-overwrite bulk B-C sample at $offset"
        }
        Assert-BytesEqual $expected_bc_after `
            (Read-FileRange $snapshot_c_bulk_bc $offset $sample_length) `
            "Snapshot C bulk B-C sample at $offset"
    }

    foreach ($fixture in @(
            @($snapshot_a_deleted_ab, [byte]0x52, 'A deleted A-B'),
            @($snapshot_b_turnover, [byte]0xC3, 'B turnover'),
            @($snapshot_c_created_bc, [byte]0xD3, 'C created B-C'))) {
        $expected = [byte[]]::new($sample_length)
        [Array]::Fill[byte]($expected, $fixture[1])
        Assert-BytesEqual $expected `
            (Read-FileRange $fixture[0] 0 $sample_length) `
            "Snapshot $($fixture[2]) sample"
    }

    $expected_hot_a = [byte[]]::new($hot_file_length)
    [Array]::Fill[byte]($expected_hot_a, 0x11)
    $expected_hot_b = [byte[]]$expected_hot_a.Clone()
    $expected_hot_c = [byte[]]$expected_hot_a.Clone()
    for ($i = 0; $i -lt $sector_write_length; ++$i) {
        $expected_hot_b[$i] = 0xB2
        $expected_hot_c[$i] = 0xB2
        $expected_hot_c[$i + $sector_write_length] = 0xD4
    }
    Assert-BytesEqual $expected_hot_a `
        (Read-FileRange $snapshot_a_hot 0 $hot_file_length) `
        'Snapshot A hot file'
    Assert-BytesEqual $expected_hot_b `
        (Read-FileRange $snapshot_b_hot 0 $hot_file_length) `
        'Snapshot B hot file'
    Assert-BytesEqual $expected_hot_c `
        (Read-FileRange $snapshot_c_hot 0 $hot_file_length) `
        'Snapshot C hot file'

    $bitmap_a = [DeviceFsTestNative]::GetNtfsBitmap(
        $snapshot_a.DeviceObject, $true)
    $bitmap_b = [DeviceFsTestNative]::GetNtfsBitmap(
        $snapshot_b.DeviceObject, $true)
    $bitmap_c = [DeviceFsTestNative]::GetNtfsBitmap(
        $snapshot_c.DeviceObject, $true)
    Assert-Condition (
        ($bitmap_a.Length -eq $source_identity.Length) -and
            ($bitmap_b.Length -eq $source_identity.Length) -and
            ($bitmap_c.Length -eq $source_identity.Length) -and
            ($bitmap_a.ClusterSize -eq $ntfs_cluster_size) -and
            ($bitmap_b.ClusterSize -eq $ntfs_cluster_size) -and
            ($bitmap_c.ClusterSize -eq $ntfs_cluster_size)) `
        'The snapshot NTFS geometry does not match the source volume.'

    $created_ab_lcn = Find-AllocationTransitionCluster `
        $snapshot_b_turnover $bitmap_a $bitmap_b $true
    $deleted_ab_lcn = Find-AllocationTransitionCluster `
        $snapshot_a_deleted_ab $bitmap_a $bitmap_b $false
    $created_bc_lcn = Find-AllocationTransitionCluster `
        $snapshot_c_created_bc $bitmap_b $bitmap_c $true
    $deleted_bc_lcn = Find-AllocationTransitionCluster `
        $snapshot_b_turnover $bitmap_b $bitmap_c $false
    $allocation_changes_ab = [DeviceFsTestNative]::GetAllocationChangeBlocks(
        $bitmap_a, $bitmap_b, $vss_block_size)
    $allocation_changes_bc = [DeviceFsTestNative]::GetAllocationChangeBlocks(
        $bitmap_b, $bitmap_c, $vss_block_size)
    Assert-Condition (
        ($allocation_changes_ab.BecameAllocated.Count -ne 0) -and
            ($allocation_changes_ab.BecameFree.Count -ne 0) -and
            ($allocation_changes_bc.BecameAllocated.Count -ne 0) -and
            ($allocation_changes_bc.BecameFree.Count -ne 0)) `
        'Both deltas did not contain both allocation-transition directions.'
    $allocation_blocks_ab = [Collections.Generic.HashSet[long]]::new()
    foreach ($offset in @($allocation_changes_ab.BecameAllocated) +
        @($allocation_changes_ab.BecameFree)) {
        $null = $allocation_blocks_ab.Add($offset)
    }
    $allocation_blocks_bc = [Collections.Generic.HashSet[long]]::new()
    foreach ($offset in @($allocation_changes_bc.BecameAllocated) +
        @($allocation_changes_bc.BecameFree)) {
        $null = $allocation_blocks_bc.Add($offset)
    }

    $devicefs_invocation = Start-DeviceFsTestProcess `
        -Executable $DeviceFsPath -MountPath $synthetic_mount `
        -ReadUser $read_user -StopEvent "Local\devicefs-vss-$run_id" `
        -Mappings ([ordered]@{
            'snapshot-a.img' = $snapshot_a.DeviceObject
            'snapshot-b.img' = $snapshot_b.DeviceObject
            'snapshot-c.img' = $snapshot_c.DeviceObject
        }) -SyntheticFreeClusters
    Wait-DeviceFsReady $devicefs_invocation
    $snapshot_a_image =
        $devicefs_invocation.ImagePaths['snapshot-a.img']
    $snapshot_b_image =
        $devicefs_invocation.ImagePaths['snapshot-b.img']
    $snapshot_c_image =
        $devicefs_invocation.ImagePaths['snapshot-c.img']
    Assert-Condition (
        ([IO.FileInfo]::new($snapshot_a_image).Length -eq
            $source_identity.Length) -and
            ([IO.FileInfo]::new($snapshot_b_image).Length -eq
                $source_identity.Length) -and
            ([IO.FileInfo]::new($snapshot_c_image).Length -eq
                $source_identity.Length)) `
        'The devicefs image lengths do not match the source volume.'
    $differences_ab = [Collections.Generic.HashSet[long]]::new()
    foreach ($offset in [DeviceFsTestNative]::GetDifferingFileBlockOffsets(
        $snapshot_a_image, $snapshot_b_image, $vss_block_size))
    {
        $null = $differences_ab.Add($offset)
    }
    $differences_bc = [Collections.Generic.HashSet[long]]::new()
    foreach ($offset in [DeviceFsTestNative]::GetDifferingFileBlockOffsets(
        $snapshot_b_image, $snapshot_c_image, $vss_block_size))
    {
        $null = $differences_bc.Add($offset)
    }
    Assert-Condition (
        ($differences_ab.Count -ne 0) -and ($differences_bc.Count -ne 0)) `
        'A devicefs synthetic delta contained no changed blocks.'

    $created_ab_block = $created_ab_lcn * [long]$ntfs_cluster_size
    $created_ab_block -= $created_ab_block % $vss_block_size
    $deleted_ab_block = $deleted_ab_lcn * [long]$ntfs_cluster_size
    $deleted_ab_block -= $deleted_ab_block % $vss_block_size
    $created_bc_block = $created_bc_lcn * [long]$ntfs_cluster_size
    $created_bc_block -= $created_bc_block % $vss_block_size
    $deleted_bc_block = $deleted_bc_lcn * [long]$ntfs_cluster_size
    $deleted_bc_block -= $deleted_bc_block % $vss_block_size
    Assert-Condition (
        $differences_ab.Contains($created_ab_block) -and
            $differences_ab.Contains($deleted_ab_block) -and
            $differences_bc.Contains($created_bc_block) -and
            $differences_bc.Contains($deleted_bc_block)) `
        'The synthetic views did not expose every controlled allocation change.'

    Stop-DeviceFsTestProcess $devicefs_invocation
    $devicefs_invocation.Process.Dispose()
    $devicefs_invocation = $null

    $svi_relative_path = 'System Volume Information'
    $backup_privilege = [DeviceFsTestNative]::EnableBackupPrivilege()
    $svi_blocks_a = Get-TreeBlockOffsets `
        "$($snapshot_a.DeviceObject)\$svi_relative_path" `
        $bitmap_a $vss_block_size
    $svi_blocks_b = Get-TreeBlockOffsets `
        "$($snapshot_b.DeviceObject)\$svi_relative_path" `
        $bitmap_b $vss_block_size
    $svi_blocks_c = Get-TreeBlockOffsets `
        "$($snapshot_c.DeviceObject)\$svi_relative_path" `
        $bitmap_c $vss_block_size
    $backup_privilege.Dispose()
    $backup_privilege = $null
    Assert-Condition (
        ($svi_blocks_a.Count -ne 0) -and
            ($svi_blocks_b.Count -ne 0) -and
            ($svi_blocks_c.Count -ne 0)) `
        'System Volume Information did not contain allocated extents.'

    $svi_blocks_ab = [Collections.Generic.HashSet[long]]::new()
    foreach ($set in @($svi_blocks_a, $svi_blocks_b)) {
        foreach ($offset in $set) {
            $null = $svi_blocks_ab.Add($offset)
        }
    }
    $svi_blocks_bc = [Collections.Generic.HashSet[long]]::new()
    foreach ($set in @($svi_blocks_b, $svi_blocks_c)) {
        foreach ($offset in $set) {
            $null = $svi_blocks_bc.Add($offset)
        }
    }

    $vshadow_source = [IO.Path]::Combine(
        $test_root, 'vshadow-source-volume.img')
    [DeviceFsTestNative]::CopyDeviceToFile(
        $source_volume_name, $vshadow_source)
    if ($use_descriptor_dump) {
        $requests = @(
            ([Guid]$snapshot_a.ID).ToString('D')
            ([Guid]$snapshot_b.ID).ToString('D')
            ([Guid]$snapshot_c.ID).ToString('D')
        )
        $converted = [Collections.Generic.List[string]]::new()
        $dump_by_copy_id = @{}
        $failures = [Collections.Generic.List[string]]::new()
        for ($i = 0; $i -lt $requests.Count; ++$i) {
            $name = [char]([int][char]'a' + $i)
            $stdout = [IO.Path]::Combine(
                $test_root, "vss-descriptor-dump-$name.stdout.txt")
            $stderr = [IO.Path]::Combine(
                $test_root, "vss-descriptor-dump-$name.stderr.txt")
            & $VssDescriptorDumpPath --source $vshadow_source `
                --snapshot-id $requests[$i] >$stdout 2>$stderr
            $exit_code = $LASTEXITCODE
            if ($exit_code -ne 0) {
                $error_text = ([IO.File]::ReadAllText($stderr) -replace
                    '\r?\n', ' ').Trim()
                $failures.Add(
                    "snapshot $($requests[$i]) exited $($exit_code): $error_text")
            } else {
                try {
                    $result = ConvertFrom-VssDescriptorDumpOutput `
                        ([IO.File]::ReadAllText($stdout)) $requests[$i] $i `
                        $vshadow_forwarder_flag $vshadow_overlay_flag `
                        $source_identity.Length $vss_block_size
                    $converted.Add($result.VshadowInfoProjection)
                    $dump_by_copy_id[$requests[$i]] = $result
                } catch {
                    $failures.Add(
                        "snapshot $($requests[$i]): $($_.Exception.Message)")
                }
            }
        }
        # Compare B after it has a successor with latest snapshot C. These
        # results are diagnostic and never affect the test verdict.
        foreach ($probe in @(
                [pscustomobject]@{
                    Name = 'snapshot B after C selecting store A'
                    Artifact = 'endpoint-b-after-c-select-a'
                    Source = $snapshot_b.DeviceObject
                    CopyId = $requests[0]
                    StoreIndex = 0
                    Differences = $differences_ab
                    AllocationBlocks = $allocation_blocks_ab
                    SviBlocks = $svi_blocks_ab
                },
                [pscustomobject]@{
                    Name = 'snapshot C while latest selecting store B'
                    Artifact = 'endpoint-c-while-latest-select-b'
                    Source = $snapshot_c.DeviceObject
                    CopyId = $requests[1]
                    StoreIndex = 1
                    Differences = $differences_bc
                    AllocationBlocks = $allocation_blocks_bc
                    SviBlocks = $svi_blocks_bc
                })) {
            try {
                $stdout = [IO.Path]::Combine(
                    $test_root, "$($probe.Artifact).stdout.txt")
                $stderr = [IO.Path]::Combine(
                    $test_root, "$($probe.Artifact).stderr.txt")
                & $VssDescriptorDumpPath --source $probe.Source `
                    --snapshot-id $probe.CopyId >$stdout 2>$stderr
                $exit_code = $LASTEXITCODE
                if ($exit_code -ne 0) {
                    $error_text =
                        ([IO.File]::ReadAllText($stderr) -replace
                            '\r?\n', ' ').Trim()
                    $endpoint_raw_diagnostics.Add(
                        "$($probe.Name): parser exited $($exit_code): " +
                        $error_text)
                    continue
                }
                $result = ConvertFrom-VssDescriptorDumpOutput `
                    ([IO.File]::ReadAllText($stdout)) $probe.CopyId `
                    $probe.StoreIndex `
                    $vshadow_forwarder_flag $vshadow_overlay_flag `
                    $source_identity.Length $vss_block_size
                $combined = [Collections.Generic.HashSet[long]]::new()
                foreach ($set in @($result.DescriptorBlocks,
                        $probe.AllocationBlocks, $probe.SviBlocks)) {
                    foreach ($offset in $set) {
                        $null = $combined.Add($offset)
                    }
                }
                $missing = [Collections.Generic.List[long]]::new()
                $descriptor_only = 0
                foreach ($offset in $probe.Differences) {
                    $descriptor = $result.DescriptorBlocks.Contains($offset)
                    $allocation = $probe.AllocationBlocks.Contains($offset)
                    $svi = $probe.SviBlocks.Contains($offset)
                    if (-not ($descriptor -or $allocation -or $svi)) {
                        $missing.Add($offset)
                    } elseif ($descriptor -and (-not $allocation) -and
                        (-not $svi)) {
                        ++$descriptor_only
                    }
                }
                if ($missing.Count -eq 0) {
                    $coverage =
                        "covered all $($probe.Differences.Count) changed block(s)"
                } else {
                    $sample = @($missing | Select-Object -First 8 |
                        ForEach-Object { '0x{0:X}' -f $_ }) -join ', '
                    $coverage =
                        "missed $($missing.Count) of " +
                        "$($probe.Differences.Count) changed block(s): $sample"
                }
                $volume_blocks = [long][Math]::Ceiling(
                    $source_identity.Length / [double]$vss_block_size)
                $candidate_percent =
                    100.0 * $combined.Count / $volume_blocks
                $volume_size = if (
                    $result.VolumeSize -eq [uint64]$source_identity.Length) {
                    'volume size matched'
                } else {
                    "volume size $($result.VolumeSize) differed from " +
                    "$($source_identity.Length)"
                }
                $endpoint_raw_diagnostics.Add(
                    "$($probe.Name): parsed $($result.DescriptorCount) " +
                    "descriptor(s) from $($result.ListBlockCount) list " +
                    "block(s); $coverage; descriptors alone covered " +
                    "$descriptor_only changed block(s); the map contained " +
                    "$($combined.Count) block(s) " +
                    ("({0:F2}% of the volume); $volume_size" -f
                        $candidate_percent))
            } catch {
                $message = ($_.Exception.Message -replace '\r?\n', ' ').Trim()
                $endpoint_raw_diagnostics.Add(
                    "$($probe.Name): probe could not be completed: $message")
            }
        }
        if ($parity_requested) {
            $reference_stdout = [IO.Path]::Combine(
                $test_root, 'vshadowinfo.stdout.txt')
            $reference_stderr = [IO.Path]::Combine(
                $test_root, 'vshadowinfo.stderr.txt')
            & $VShadowInfoPath -a $vshadow_source `
                >$reference_stdout 2>$reference_stderr
            $reference_exit_code = $LASTEXITCODE
            if ($reference_exit_code -ne 0) {
                $reference_error =
                    ([IO.File]::ReadAllText($reference_stderr) -replace
                        '\r?\n', ' ').Trim()
                $parity_failures.Add(
                    "vshadowinfo exited $($reference_exit_code): " +
                    $reference_error)
            } else {
                try {
                    $reference_stores =
                        ConvertFrom-VshadowInfoDescriptorOutput `
                            ([IO.File]::ReadAllText($reference_stdout)) `
                            $vshadow_forwarder_flag $vshadow_overlay_flag
                    if ($reference_stores.Count -ne $requests.Count) {
                        $parity_failures.Add(
                            "vshadowinfo reported $($reference_stores.Count) " +
                            "stores; expected $($requests.Count)")
                    }
                    for ($i = 0; $i -lt $requests.Count; ++$i) {
                        $copy_id = $requests[$i]
                        if (-not $reference_stores.ContainsKey($copy_id)) {
                            $parity_failures.Add(
                                "vshadowinfo did not report snapshot $copy_id")
                            continue
                        }
                        if (-not $dump_by_copy_id.ContainsKey($copy_id)) {
                            continue
                        }
                        $differences = @(Compare-VssDescriptorStores `
                                $reference_stores[$copy_id] `
                                $dump_by_copy_id[$copy_id])
                        if ($differences.Count -ne 0) {
                            $name = [char]([int][char]'a' + $i)
                            $artifact = [IO.Path]::Combine(
                                $test_root, "descriptor-parity-$name.txt")
                            [IO.File]::WriteAllLines($artifact, $differences)
                            $parity_failures.Add(
                                "snapshot $copy_id had " +
                                "$($differences.Count) parity difference(s); " +
                                "first: $($differences[0])")
                        }
                    }
                } catch {
                    $parity_failures.Add(
                        "could not parse vshadowinfo for parity: " +
                        $_.Exception.Message)
                }
            }
        }
        Assert-Condition ($failures.Count -eq 0) (
            'vss-descriptor-dump failed: ' + ($failures -join '; '))
        $vshadow_output = [string]::Join(
            [Environment]::NewLine, $converted)
    } else {
        $vshadow_stdout = [IO.Path]::Combine(
            $test_root, 'vshadowinfo.stdout.txt')
        $vshadow_stderr = [IO.Path]::Combine(
            $test_root, 'vshadowinfo.stderr.txt')
        & $VShadowInfoPath -a $vshadow_source `
            >$vshadow_stdout 2>$vshadow_stderr
        $vshadow_exit_code = $LASTEXITCODE
        $vshadow_output = [IO.File]::ReadAllText($vshadow_stdout)
        $vshadow_error = [IO.File]::ReadAllText($vshadow_stderr)
        Assert-Condition ($vshadow_exit_code -eq 0) `
            "vshadowinfo exited with code $($vshadow_exit_code): $vshadow_error"
    }

    $stores = [Collections.Generic.List[object]]::new()
    $current_store = $null
    $original = $null
    $relative = $null
    $store_offset = $null
    foreach ($line in $vshadow_output -split '\r?\n') {
        if ($line -match '^Store:\s+(\d+)$') {
            $current_store = [pscustomobject]@{
                Index = [Convert]::ToInt32($Matches[1])
                CopyId = $null
                PrintedCount = -1
                RecordCount = 0
                OriginalBlocks =
                    [Collections.Generic.HashSet[long]]::new()
                DescriptorBlocks =
                    [Collections.Generic.HashSet[long]]::new()
                ForwarderCount = 0
                OverlayCount = 0
            }
            $stores.Add($current_store)
        } elseif ($line -match
            '^\s+Shadow copy ID\s+:\s+([0-9a-f-]{36})$') {
            Assert-Condition (
                ($null -ne $current_store) -and
                    ($null -eq $current_store.CopyId)) `
                'vshadowinfo printed a misplaced shadow copy ID.'
            $current_store.CopyId = [Guid]$Matches[1]
        } elseif ($line -match '^\s+Number of blocks\s+:\s+(\d+)$') {
            Assert-Condition (
                ($null -ne $current_store) -and
                    ($current_store.PrintedCount -eq -1)) `
                'vshadowinfo printed a misplaced block count.'
            $current_store.PrintedCount =
                [Convert]::ToInt32($Matches[1])
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
                ($null -ne $current_store) -and
                    ($null -ne $current_store.CopyId) -and
                    ($current_store.PrintedCount -ge 0) -and
                    ($null -ne $original) -and ($null -ne $relative) -and
                    ($null -ne $store_offset)) `
                'vshadowinfo printed an incomplete block descriptor.'
            $flags = [Convert]::ToUInt32($Matches[1], 16)
            if (($original -ge 0) -and
                ($original -lt $source_identity.Length)) {
                $original_block =
                    $original - ($original % $vss_block_size)
                $null = $current_store.OriginalBlocks.Add($original_block)
            }
            $offsets = @($original, $store_offset)
            if (($flags -band $vshadow_forwarder_flag) -ne 0) {
                $offsets += $relative
                ++$current_store.ForwarderCount
            }
            if (($flags -band $vshadow_overlay_flag) -ne 0) {
                ++$current_store.OverlayCount
            }
            foreach ($offset in $offsets) {
                if (($offset -ge 0) -and
                    ($offset -lt $source_identity.Length)) {
                    $block_offset =
                        $offset - ($offset % $vss_block_size)
                    $null =
                        $current_store.DescriptorBlocks.Add($block_offset)
                }
            }
            ++$current_store.RecordCount
            $original = $null
            $relative = $null
            $store_offset = $null
        }
    }
    Assert-Condition (
        ($null -eq $original) -and ($null -eq $relative) -and
            ($null -eq $store_offset)) `
        'vshadowinfo ended within a block descriptor.'
    Assert-Condition ($stores.Count -eq 3) `
        'vshadowinfo did not report exactly three VSS stores.'

    $store_by_copy_id = @{}
    foreach ($store in $stores) {
        Assert-Condition (
            ($null -ne $store.CopyId) -and
                ($store.PrintedCount -ge 0) -and
                ($store.RecordCount -eq $store.PrintedCount)) `
            "vshadowinfo output for store $($store.Index) was incomplete."
        $key = $store.CopyId.ToString('D')
        Assert-Condition (-not $store_by_copy_id.ContainsKey($key)) `
            "vshadowinfo reported snapshot $key more than once."
        $store_by_copy_id[$key] = $store
    }

    $snapshot_a_id = ([Guid]$snapshot_a.ID).ToString('D')
    $snapshot_b_id = ([Guid]$snapshot_b.ID).ToString('D')
    $snapshot_c_id = ([Guid]$snapshot_c.ID).ToString('D')
    foreach ($id in @($snapshot_a_id, $snapshot_b_id, $snapshot_c_id)) {
        Assert-Condition ($store_by_copy_id.ContainsKey($id)) `
            "vshadowinfo did not report snapshot $id."
    }
    $store_a = $store_by_copy_id[$snapshot_a_id]
    $store_b = $store_by_copy_id[$snapshot_b_id]
    $store_c = $store_by_copy_id[$snapshot_c_id]

    $delta_ab = Test-DeltaCoverage 'a-b' $differences_ab `
        $store_a.DescriptorBlocks $allocation_blocks_ab $svi_blocks_ab `
        $test_root $source_identity.Length $vss_block_size
    $delta_bc = Test-DeltaCoverage 'b-c' $differences_bc `
        $store_b.DescriptorBlocks $allocation_blocks_bc $svi_blocks_bc `
        $test_root $source_identity.Length $vss_block_size
    $delta_results = @($delta_ab, $delta_bc)

    $controlled_ab = Assert-ControlledOverwrite `
        'A-B bulk overwrite' $snapshot_a_bulk_ab $snapshot_b_bulk_ab `
        $bitmap_a $bitmap_b $differences_ab $store_a.OriginalBlocks `
        $allocation_blocks_ab $svi_blocks_ab $vss_block_size
    $controlled_bc = Assert-ControlledOverwrite `
        'B-C bulk overwrite' $snapshot_b_bulk_bc $snapshot_c_bulk_bc `
        $bitmap_b $bitmap_c $differences_bc $store_b.OriginalBlocks `
        $allocation_blocks_bc $svi_blocks_bc $vss_block_size

    $cold_ab_blocks_b = Get-ObjectBlockOffsets `
        $snapshot_b_bulk_ab $bitmap_b $vss_block_size $false
    $cold_ab_blocks_c = Get-ObjectBlockOffsets `
        $snapshot_c_bulk_ab $bitmap_c $vss_block_size $false
    Assert-Condition ($cold_ab_blocks_b.SetEquals($cold_ab_blocks_c)) `
        'The unchanged A-B bulk file moved between snapshots B and C.'
    foreach ($cold_check in @(
            @('B', $cold_ab_blocks_b, $store_b.DescriptorBlocks),
            @('C A-B', $cold_ab_blocks_c, $store_c.DescriptorBlocks),
            @('C B-C',
                (Get-ObjectBlockOffsets $snapshot_c_bulk_bc $bitmap_c `
                    $vss_block_size $false),
                $store_c.DescriptorBlocks))) {
        $absent = 0
        foreach ($offset in $cold_check[1]) {
            if (-not $cold_check[2].Contains($offset)) {
                ++$absent
            }
        }
        Assert-Condition ($absent -gt 512) (
            "Store $($cold_check[0]) did not leave more than 512 known-cold " +
            'file blocks outside its candidate map.')
    }

    $hot_blocks_a = Get-ObjectBlockOffsets `
        $snapshot_a_hot $bitmap_a $vss_block_size $false
    $hot_blocks_b = Get-ObjectBlockOffsets `
        $snapshot_b_hot $bitmap_b $vss_block_size $false
    $hot_blocks_c = Get-ObjectBlockOffsets `
        $snapshot_c_hot $bitmap_c $vss_block_size $false
    Assert-Condition (
        $hot_blocks_a.SetEquals($hot_blocks_b) -and
            $hot_blocks_a.SetEquals($hot_blocks_c)) `
        'The hot file moved on disk between snapshots.'
    $repeated_hot_blocks = 0
    foreach ($offset in $hot_blocks_a) {
        if ($differences_ab.Contains($offset) -and
            $differences_bc.Contains($offset) -and
            $store_a.OriginalBlocks.Contains($offset) -and
            $store_b.OriginalBlocks.Contains($offset)) {
            ++$repeated_hot_blocks
        }
    }
    Assert-Condition ($repeated_hot_blocks -ne 0) `
        'No stable hot-file block appeared in both preceding VSS stores.'

    $descriptor_only = $delta_ab.DescriptorOnly + $delta_bc.DescriptorOnly
    $allocation_only = $delta_ab.AllocationOnly + $delta_bc.AllocationOnly
    $svi_only = $delta_ab.SviOnly + $delta_bc.SviOnly
    Assert-Condition (
        ($descriptor_only -ne 0) -and ($allocation_only -ne 0) -and
            ($svi_only -ne 0)) `
        'The fixture did not independently exercise every map component.'

    $forwarder_count = $store_a.ForwarderCount +
        $store_b.ForwarderCount + $store_c.ForwarderCount
    $overlay_count = $store_a.OverlayCount +
        $store_b.OverlayCount + $store_c.OverlayCount

} catch {
    $primary_error = $_
} finally {
    if ($null -ne $devicefs_invocation) {
        try {
            if (-not $devicefs_invocation.StartupExitObserved) {
                Stop-DeviceFsTestProcess $devicefs_invocation
            }
        } catch {
            $cleanup_errors.Add($_.Exception)
        } finally {
            $gone = $false
            try {
                $gone = $devicefs_invocation.Process.HasExited
            } catch {
                $cleanup_errors.Add($_.Exception)
            }
            if ($gone) {
                if (-not $devicefs_invocation.OutputCollected) {
                    try {
                        Save-TestProcessOutput $devicefs_invocation
                    } catch {
                        $cleanup_errors.Add($_.Exception)
                    }
                }
                $devicefs_invocation.Process.Dispose()
            } else {
                $devicefs_process_gone = $false
                Write-Warning (
                    "devicefs process $($devicefs_invocation.Process.Id) " +
                    'remains alive; snapshots and the VHD will be preserved.')
            }
        }
    }

    if ($null -ne $backup_privilege) {
        try {
            $backup_privilege.Dispose()
            $backup_privilege = $null
        } catch {
            $cleanup_errors.Add($_.Exception)
        }
    }

    if ($devicefs_process_gone) {
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
    }

    if ($devicefs_process_gone -and $source_access_path_added) {
        try {
            Remove-PartitionAccessPath -InputObject $source_partition `
                -AccessPath $source_mount -Confirm:$false
        } catch {
            $cleanup_errors.Add($_.Exception)
        }
    }

    if ($devicefs_process_gone -and
        ($null -ne $vhd_path) -and [IO.File]::Exists($vhd_path)) {
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
        $image_detached = ($null -eq $vhd_path) -or
            (-not [IO.File]::Exists($vhd_path))
    }

    $preserve = ((($null -ne $primary_error) -or
            ($parity_failures.Count -ne 0)) -and
            $KeepArtifactsOnFailure) -or
        ($cleanup_errors.Count -ne 0) -or (-not $devicefs_process_gone) -or
        (-not $image_detached)
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

foreach ($diagnostic in $endpoint_raw_diagnostics) {
    Write-Host "INFO raw endpoint $diagnostic"
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

foreach ($delta in $delta_results) {
    Write-Host (
        "PASS $($delta.Name): $($delta.DifferenceCount) changed 16 KiB " +
        "block(s) were covered. Exclusive coverage was " +
        "$($delta.DescriptorOnly) descriptor, " +
        "$($delta.AllocationOnly) allocation, and " +
        "$($delta.SviOnly) System Volume Information block(s); " +
        "$($delta.Overlapping) block(s) had overlapping coverage. " +
        'The map contains ' +
        "$($delta.CandidateCount) block(s) " +
        ("({0:F2}% of the volume); " -f $delta.CandidatePercent) +
        "$($delta.Overincluded) candidate block(s) were additional.")
}
Write-Host (
    "The fixture verified $controlled_ab and $controlled_bc stable bulk " +
    "original-offset blocks across the two chained stores and " +
    "$repeated_hot_blocks repeated hot-file block(s). The tool reported " +
    "$forwarder_count forwarder and $overlay_count overlay descriptor(s); " +
    'their occurrence is informational because the provider chooses the ' +
    'private on-disk encoding.')
if ($parity_requested) {
    if ($parity_failures.Count -ne 0) {
        throw ('Descriptor parser parity failed: ' +
            ($parity_failures -join '; '))
    }
    Write-Host (
        'PASS descriptor parity: vss-descriptor-dump matched vshadowinfo ' +
        'for all three stores on the same source image.')
}
