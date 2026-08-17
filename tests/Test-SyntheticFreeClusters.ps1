# SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
# SPDX-License-Identifier: GPL-3.0-or-later

#requires -Version 7.4
#requires -RunAsAdministrator
#requires -Modules Hyper-V

<#
.SYNOPSIS
Exercises devicefs --synthetic-free-clusters against a disposable read-only VHD.

.DESCRIPTION
Creates a fixed VHD containing one NTFS volume without assigning a drive
letter. The test selects a free cluster while the VHD is attached read-only,
detaches the VHD, writes a nonzero pattern into that cluster, and attaches the
VHD read-only again. It then compares direct volume reads with normal and
synthetic devicefs views using an independently queried NTFS allocation bitmap.

The VHD is detached during cleanup. Use -KeepArtifactsOnFailure to retain the
detached VHD and logs after a failure.
#>

[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string] $DeviceFsPath,

    [ValidateRange(256, 8192)]
    [int] $VhdSizeMiB = 512,

    [switch] $KeepArtifactsOnFailure
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$file_backed_virtual_bus_type = [UInt16]15
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

function Write-VhdCluster {
    param(
        [Parameter(Mandatory)]
        [string] $Path,

        [Parameter(Mandatory)]
        [long] $DiskLength,

        [Parameter(Mandatory)]
        [long] $PartitionOffset,

        [Parameter(Mandatory)]
        [long] $PartitionLength,

        [Parameter(Mandatory)]
        [int] $ClusterSize,

        [Parameter(Mandatory)]
        [long] $Lcn,

        [Parameter(Mandatory)]
        [byte[]] $Pattern
    )

    Assert-Condition ($Pattern.Length -eq $ClusterSize) `
        'The VHD witness pattern is not one cluster long.'
    Assert-Condition (
        ($PartitionOffset -ge 0) -and ($PartitionLength -gt 0) -and
            ($PartitionOffset + $PartitionLength -le $DiskLength)) `
        'The VHD partition bounds are invalid.'
    Assert-Condition ($Lcn -ge 0) 'The VHD witness LCN is negative.'

    $relative_offset = $Lcn * [long]$ClusterSize
    Assert-Condition (
        $relative_offset -le ($PartitionLength - $ClusterSize)) `
        'The VHD witness cluster is outside the partition.'

    $footer_size = 512
    $stream = [IO.File]::Open(
        $Path, [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite,
        [IO.FileShare]::None)
    try {
        Assert-Condition ($stream.Length -eq ($DiskLength + $footer_size)) `
            'The fixed VHD file has an unexpected length.'
        $footer_cookie = [byte[]]::new(8)
        $stream.Position = $DiskLength
        $stream.ReadExactly($footer_cookie, 0, $footer_cookie.Length)
        Assert-Condition (
            [Text.Encoding]::ASCII.GetString($footer_cookie) -ceq 'conectix') `
            'The fixed VHD footer is invalid.'

        $stream.Position = $PartitionOffset + $relative_offset
        $stream.Write($Pattern, 0, $Pattern.Length)
        $stream.Flush($true)
    } finally {
        $stream.Dispose()
    }
}

function Mount-ValidatedReadOnlyVhd {
    param(
        [Parameter(Mandatory)]
        [string] $Path,

        [Parameter(Mandatory)]
        $Fixture
    )

    $disk_image = Mount-DiskImage -ImagePath $Path -StorageType VHD `
        -Access ReadOnly -NoDriveLetter -PassThru
    $disks = @($disk_image | Get-Disk)
    Assert-Condition ($disks.Count -eq 1) `
        'The read-only VHD did not resolve to exactly one disk.'

    $disk = $disks[0]
    Assert-Condition (
        $disk.CimInstanceProperties['BusType'].Value -eq
            $script:file_backed_virtual_bus_type) `
        'The read-only test disk is not file-backed virtual storage.'
    Assert-Condition (
        ($disk.Size -eq $Fixture.DiskLength) -and (-not $disk.IsBoot) -and
            (-not $disk.IsSystem) -and (-not $disk.IsClustered) -and
            (-not $disk.IsOffline) -and $disk.IsReadOnly) `
        'The read-only VHD does not satisfy the test-disk safety policy.'

    $partition = Get-Partition -DiskNumber $disk.Number `
        -PartitionNumber $Fixture.PartitionNumber
    Assert-Condition (
        ($partition.Offset -eq $Fixture.PartitionOffset) -and
            ($partition.Size -eq $Fixture.PartitionLength) -and
            (-not $partition.IsHidden) -and
            $partition.NoDefaultDriveLetter -and
            ([char]$partition.DriveLetter -eq [char]0)) `
        'The read-only VHD partition identity or attributes changed.'
    Assert-Condition (-not (Get-Partition -DiskNumber $disk.Number |
            Where-Object { [char]$_.DriveLetter -ne [char]0 })) `
        'The read-only test VHD consumed a drive letter.'

    $identity = [DeviceFsTestNative]::InspectVolume($Fixture.VolumeName)
    Assert-Condition (
        ($identity.Label -ceq $Fixture.VolumeLabel) -and
            ($identity.Length -eq $Fixture.VolumeLength) -and
            ($identity.DiskNumber -eq $disk.Number) -and
            ($identity.DiskStartingOffset -eq $partition.Offset) -and
            ($identity.DiskExtentLength -eq $partition.Size)) `
        'The read-only volume does not map to the expected VHD partition.'

    $bitmap = [DeviceFsTestNative]::GetNtfsBitmap(
        $Fixture.VolumeName, $true)
    Assert-Condition (
        ($bitmap.Length -eq $Fixture.VolumeLength) -and
            ($bitmap.ClusterSize -eq $Fixture.ClusterSize)) `
        'The read-only NTFS bitmap geometry is inconsistent with the fixture.'
    return $bitmap
}

if (-not $IsWindows) {
    throw 'This integration test requires Windows.'
}
if (-not [Environment]::Is64BitProcess) {
    throw 'This integration test requires 64-bit PowerShell.'
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
$normal_invocation = $null
$synthetic_invocation = $null
$comparison = $null
$primary_error = $null
$cleanup_errors = [Collections.Generic.List[Exception]]::new()
$devicefs_processes_gone = $true
$image_detached = $false

$source_label = "DFSTSRC-$($run_id.Substring(0, 16))"
$uninitialized_partition_style = [UInt16]0
$ntfs_cluster_size = 4096
$free_run_length = 16
# Force the full comparison to cross ordinary sector and cluster boundaries.
$comparison_chunk_size = 1MB + 37

try {
    $test_root = Join-Path $env:WINDIR 'SystemTemp' "devicefs-test-$run_id"
    $test_root = (New-Item -ItemType Directory -Path $test_root).FullName
    $vhd_path = [IO.Path]::Combine($test_root, 'test.vhd')
    $source_mount = [IO.Path]::Combine($test_root, 'source')
    New-Item -ItemType Directory -Path $source_mount | Out-Null
    $normal_mount = [IO.Path]::Combine($test_root, 'normal')
    $synthetic_mount = [IO.Path]::Combine($test_root, 'synthetic')

    $requested_disk_length = [UInt64]$VhdSizeMiB * 1MB
    New-VHD -Path $vhd_path -SizeBytes $requested_disk_length `
        -Fixed | Out-Null

    $disk_image = Mount-DiskImage -ImagePath $vhd_path -StorageType VHD `
        -Access ReadWrite -NoDriveLetter -PassThru
    $disks = @($disk_image | Get-Disk)
    Assert-Condition ($disks.Count -eq 1) `
        'The exact VHD image did not resolve to exactly one disk.'
    $disk = $disks[0]
    Assert-Condition (
        $disk.CimInstanceProperties['BusType'].Value -eq
            $file_backed_virtual_bus_type) `
        'The attached test disk is not file-backed virtual storage.'
    Assert-Condition ($disk.Size -eq $requested_disk_length) `
        'The attached test disk has an unexpected size.'
    Assert-Condition (
        $disk.CimInstanceProperties['PartitionStyle'].Value -eq
            $uninitialized_partition_style) `
        'The newly created test disk is not uninitialized.'
    Assert-Condition (
        (-not $disk.IsBoot) -and (-not $disk.IsSystem) -and
            (-not $disk.IsClustered) -and (-not $disk.IsOffline) -and
            (-not $disk.IsReadOnly)) `
        'The attached test disk does not satisfy the safety policy.'
    Assert-Condition (@(Get-Partition -DiskNumber $disk.Number `
            -ErrorAction SilentlyContinue).Count -eq 0) `
        'The newly created test disk unexpectedly contains partitions.'

    Initialize-Disk -Number $disk.Number -PartitionStyle GPT `
        -PassThru | Out-Null
    $source_partition = New-Partition -DiskNumber $disk.Number `
        -UseMaximumSize -IsHidden
    # PowerShell exposes an unassigned CIM Char16 as U+0000.
    Assert-Condition (
        $source_partition.IsHidden -and
            ([char]$source_partition.DriveLetter -eq [char]0)) `
        'The temporary partition was not created hidden and letterless.'

    Set-Partition -InputObject $source_partition `
        -NoDefaultDriveLetter $true -IsHidden $false `
        -Confirm:$false | Out-Null
    $source_partition = Get-Partition -DiskNumber $disk.Number `
        -PartitionNumber $source_partition.PartitionNumber
    Assert-Condition (
        (-not $source_partition.IsHidden) -and
            $source_partition.NoDefaultDriveLetter -and
            ([char]$source_partition.DriveLetter -eq [char]0)) `
        'The temporary partition did not become visible without a drive letter.'

    Format-Volume -Partition $source_partition -FileSystem NTFS `
        -AllocationUnitSize $ntfs_cluster_size `
        -NewFileSystemLabel $source_label `
        -Force -Confirm:$false | Out-Null
    Add-PartitionAccessPath -InputObject $source_partition `
        -AccessPath $source_mount
    $source_access_path_added = $true

    Assert-Condition (-not (Get-Partition -DiskNumber $disk.Number |
            Where-Object { [char]$_.DriveLetter -ne [char]0 })) `
        'The test VHD consumed a drive letter.'

    $source_volume_name = [DeviceFsTestNative]::GetVolumeName($source_mount)
    $source_identity = [DeviceFsTestNative]::InspectVolume(
        $source_volume_name)
    Assert-Condition ($source_identity.Label -ceq $source_label) `
        'The source volume label does not match the unique test label.'
    Assert-Condition (
        ($source_identity.DiskNumber -eq $disk.Number) -and
            ($source_identity.DiskStartingOffset -eq
                $source_partition.Offset) -and
            ($source_identity.DiskExtentLength -eq $source_partition.Size)) `
        'The source volume does not map to the expected VHD partition.'

    $fixture = [pscustomobject]@{
        DiskLength = [long]$disk.Size
        PartitionNumber = $source_partition.PartitionNumber
        PartitionOffset = [long]$source_partition.Offset
        PartitionLength = [long]$source_partition.Size
        VolumeName = $source_volume_name
        VolumeLabel = $source_label
        VolumeLength = [long]$source_identity.Length
        ClusterSize = $ntfs_cluster_size
    }

    $witness_pattern = [byte[]]::new($ntfs_cluster_size)
    [Array]::Fill[byte]($witness_pattern, 0xA5)

    Remove-PartitionAccessPath -InputObject $source_partition `
        -AccessPath $source_mount -Confirm:$false
    $source_access_path_added = $false
    Dismount-DiskImage -ImagePath $vhd_path | Out-Null
    Assert-Condition (-not (Get-DiskImage -ImagePath $vhd_path).Attached) `
        'The VHD remained attached before the read-only bitmap query.'

    $selection_bitmap = Mount-ValidatedReadOnlyVhd `
        -Path $vhd_path -Fixture $fixture
    $witness_lcn = -1L
    $free_to_allocated_lcn = -1L
    $run_start = -1L
    $run_length = 0
    $previous_allocated = $selection_bitmap.IsAllocated(0)
    for ($cluster = 1L;
        ($cluster -lt $selection_bitmap.ClusterCount) -and
            (($witness_lcn -lt 0) -or ($free_to_allocated_lcn -lt 0));
        ++$cluster) {
        $allocated = $selection_bitmap.IsAllocated($cluster)
        if ($allocated) {
            if ((-not $previous_allocated) -and
                ($free_to_allocated_lcn -lt 0)) {
                $free_to_allocated_lcn = $cluster - 1
            }
            $run_start = -1L
            $run_length = 0
        } elseif ($previous_allocated) {
            $run_start = $cluster
            $run_length = 1
        } elseif ($run_start -ge 0) {
            ++$run_length
        }

        if ($run_length -eq $free_run_length) {
            $witness_lcn = $run_start
        }
        $previous_allocated = $allocated
    }
    Assert-Condition ($witness_lcn -ge 1) `
        'Could not find an allocated-to-free transition with a long free run.'
    Assert-Condition ($free_to_allocated_lcn -ge 0) `
        'Could not find a free-to-allocated transition.'

    Dismount-DiskImage -ImagePath $vhd_path | Out-Null
    Assert-Condition (-not (Get-DiskImage -ImagePath $vhd_path).Attached) `
        'The read-only VHD remained attached before offline modification.'
    # A fixed legacy VHD stores its raw disk payload at file offset zero.
    Write-VhdCluster -Path $vhd_path -DiskLength $fixture.DiskLength `
        -PartitionOffset $fixture.PartitionOffset `
        -PartitionLength $fixture.PartitionLength `
        -ClusterSize $fixture.ClusterSize -Lcn $witness_lcn `
        -Pattern $witness_pattern

    $bitmap = Mount-ValidatedReadOnlyVhd `
        -Path $vhd_path -Fixture $fixture
    Assert-Condition (
        ($bitmap.SectorSize -eq $selection_bitmap.SectorSize) -and
            ($bitmap.ClusterSize -eq $selection_bitmap.ClusterSize) -and
            ($bitmap.ClusterCount -eq $selection_bitmap.ClusterCount)) `
        'The NTFS bitmap geometry changed after offline modification.'
    Assert-Condition ($bitmap.IsAllocated($witness_lcn - 1)) `
        'The cluster preceding the witness is no longer allocated.'
    Assert-Condition (
        (-not $bitmap.IsAllocated($free_to_allocated_lcn)) -and
            $bitmap.IsAllocated($free_to_allocated_lcn + 1)) `
        'The selected free-to-allocated transition changed.'
    for ($cluster = $witness_lcn;
        $cluster -lt ($witness_lcn + $free_run_length);
        ++$cluster) {
        Assert-Condition (-not $bitmap.IsAllocated($cluster)) `
            'The selected free run changed after offline modification.'
    }

    $witness_offset = $witness_lcn * [long]$bitmap.ClusterSize
    $actual_pattern = [DeviceFsTestNative]::ReadDeviceAt(
        $fixture.VolumeName, $witness_offset, $bitmap.ClusterSize)
    Assert-BytesEqual $witness_pattern $actual_pattern `
        'The read-only free-cluster witness'

    $source_device = $fixture.VolumeName.TrimEnd([char]'\')
    $read_user = [Security.Principal.WindowsIdentity]::GetCurrent().Name
    $normal_invocation = Start-DeviceFsTestProcess `
        -Executable $DeviceFsPath -MountPath $normal_mount `
        -ReadUser $read_user -StopEvent "Local\devicefs-test-$run_id-normal" `
        -Mappings ([ordered]@{ 'volume.img' = $source_device })
    Wait-DeviceFsReady $normal_invocation
    $synthetic_invocation = Start-DeviceFsTestProcess `
        -Executable $DeviceFsPath -MountPath $synthetic_mount `
        -ReadUser $read_user -StopEvent "Local\devicefs-test-$run_id-synthetic" `
        -Mappings ([ordered]@{ 'volume.img' = $source_device }) `
        -SyntheticFreeClusters
    Wait-DeviceFsReady $synthetic_invocation

    $normal_image = $normal_invocation.ImagePaths['volume.img']
    $synthetic_image = $synthetic_invocation.ImagePaths['volume.img']
    $cluster_size = [long]$bitmap.ClusterSize
    $null = [DeviceFsTestNative]::CompareRange(
        $source_device, $normal_image, $synthetic_image, $bitmap,
        $witness_offset, [int]$cluster_size)

    foreach ($test_case in @(
            [pscustomobject]@{
                Offset = $witness_offset + 1
                Length = [int]($cluster_size - 2)
            },
            [pscustomobject]@{
                Offset = $witness_offset - 1
                Length = [int]($cluster_size + 1)
            },
            [pscustomobject]@{
                Offset = $witness_offset
                Length = [int]($free_run_length * $cluster_size)
            },
            [pscustomobject]@{
                Offset =
                    (($free_to_allocated_lcn + 1) * $cluster_size) - 1
                Length = [int]($cluster_size + 2)
            },
            [pscustomobject]@{
                Offset = $bitmap.Length - 1
                Length = [int]($cluster_size + 1)
            },
            [pscustomobject]@{
                Offset = $bitmap.Length
                Length = 1
            }
        )) {
        $null = [DeviceFsTestNative]::CompareRange(
            $source_device, $normal_image, $synthetic_image, $bitmap,
            $test_case.Offset, $test_case.Length)
    }

    $comparison = [DeviceFsTestNative]::CompareViews(
        $source_device, $normal_image, $synthetic_image, $bitmap,
        $comparison_chunk_size)
    Assert-Condition ($comparison.BytesCompared -eq $bitmap.Length) `
        'The full comparison did not cover the complete volume.'
} catch {
    $primary_error = $_
} finally {
    foreach ($invocation in @($synthetic_invocation, $normal_invocation)) {
        if ($null -eq $invocation) {
            continue
        }
        try {
            if (-not $invocation.StartupExitObserved) {
                Stop-DeviceFsTestProcess $invocation
            }
        } catch {
            $cleanup_errors.Add($_.Exception)
        } finally {
            $gone = $false
            try {
                $gone = $invocation.Process.HasExited
            } catch {
                $cleanup_errors.Add($_.Exception)
            }
            if ($gone) {
                if (-not $invocation.OutputCollected) {
                    try {
                        Save-TestProcessOutput $invocation
                    } catch {
                        $cleanup_errors.Add($_.Exception)
                    }
                }
                $invocation.Process.Dispose()
            } else {
                $devicefs_processes_gone = $false
                Write-Warning (
                    "devicefs process $($invocation.Process.Id) remains " +
                    'alive; the VHD and test directory will be preserved.')
            }
        }
    }

    if ($devicefs_processes_gone -and $source_access_path_added) {
        try {
            Remove-PartitionAccessPath -InputObject $source_partition `
                -AccessPath $source_mount -Confirm:$false
            $source_access_path_added = $false
        } catch {
            $cleanup_errors.Add($_.Exception)
        }
    }

    if ($devicefs_processes_gone -and (-not $source_access_path_added) -and
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

    $preserve = (($null -ne $primary_error) -and $KeepArtifactsOnFailure) -or
        ($cleanup_errors.Count -ne 0) -or (-not $devicefs_processes_gone) -or
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

Write-Host ("PASS: compared $($comparison.BytesCompared) bytes; " +
    "$($comparison.FreeBytes) bytes belonged to free clusters, including " +
    "$($bitmap.ClusterSize) controlled nonzero witness bytes that " +
    'the synthetic view replaced with zeros.')
