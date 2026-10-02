# SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
# SPDX-License-Identifier: GPL-3.0-or-later

#requires -Version 7.4
#requires -RunAsAdministrator

<#
.SYNOPSIS
Exercises devicefs --synthetic-free-clusters against a disposable read-only VHD.

.DESCRIPTION
Runs NTFS with 4 KiB clusters, ReFS with 4 KiB and 64 KiB clusters, and
Dev Drive with 4 KiB clusters by default. The selected cases run in parallel,
and their results are collected after all cases have finished.
Cases whose filesystem Windows cannot format are reported as unavailable.
Each case creates a VHD containing one volume without assigning a drive
letter. The test selects a free cluster while the VHD is attached read-only,
detaches the VHD, writes a nonzero pattern into that cluster as described below,
and reattaches the VHD read-only. It then compares direct volume reads with normal and
synthetic devicefs views using an independently queried allocation bitmap.
The Dev Drive case uses a dynamically expanding 51 GiB VHD, leaving room for a
volume of at least 50 GiB. The other cases use fixed VHDs. All cases write the
witness into the detached VHD file; dynamic VHD writes use its block allocation table.
The Dev Drive sequential comparison covers the first 2 GiB; the
other cases compare the complete volume. Targeted reads also check the
free-cluster witness, cluster boundaries, and the end of the volume.
It also checks the exposed known-data bitmap. Use -KnownDataMapClusterSize to
test different byte counts per bit, including sizes that cross filesystem clusters.

The VHD is detached during cleanup. Use -KeepArtifactsOnFailure to retain the
detached VHD and logs after a failure.

.PARAMETER Cases
Selects filesystem and cluster-size cases using one comma-separated string,
for example `-Cases 'ReFS-4K,ReFS-64K'`. All cases run by default.
Every case checks read boundaries, free-cluster witnesses, and the known-data bitmap.

.PARAMETER VhdSizeMiB
Size of the NTFS fixture. ReFS fixtures use at least 2048 MiB.

.PARAMETER SupervisorPath
The `backup-supervisor.exe` to test. Its `--devicefs` mode runs the filesystem.
Defaults to the repository's `Release` build for the machine's native
architecture.
#>

[CmdletBinding()]
param(
    [string] $SupervisorPath,

    [ValidateRange(256, 8192)]
    [int] $VhdSizeMiB = 512,

    [ValidateRange(1, [long]::MaxValue)]
    [long] $KnownDataMapClusterSize = 4MB,

    [string] $Cases = 'NTFS-4K,ReFS-4K,ReFS-64K,DevDrive-4K',

    [switch] $KeepArtifactsOnFailure,

    [Parameter(DontShow)]
    [switch] $ParallelWorker
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$case_settings = @{
    'NTFS-4K' = @{ FileSystem = 'NTFS'; ClusterSize = 4KB }
    'ReFS-4K' = @{ FileSystem = 'ReFS'; ClusterSize = 4KB }
    'ReFS-64K' = @{ FileSystem = 'ReFS'; ClusterSize = 64KB }
    'DevDrive-4K' = @{ FileSystem = 'ReFS'; ClusterSize = 4KB; DevDrive = $true }
}
$selected_cases = $Cases -split ','
foreach ($case in $selected_cases) {
    if (-not $case_settings.ContainsKey($case)) {
        throw "Unknown case '$case'. Available cases: $($case_settings.Keys -join ', ')."
    }
}
. ([IO.Path]::Combine(
        $PSScriptRoot, 'include', 'DeviceFsTestProcess.ps1'))
. (Join-Path $PSScriptRoot 'include/DeviceFsTestVolume.ps1')
. (Join-Path $PSScriptRoot 'include/DeviceFsTestTypes.ps1')

function Write-TestLog {
    param(
        [string] $Message,
        [string] $CaseName = $case,
        [switch] $Warning
    )

    $write = if ($Warning) { 'Write-Warning' } else { 'Write-Host' }
    foreach ($line in $Message.Split("`n")) {
        & $write "[$CaseName] $($line.TrimEnd("`r"))"
    }
}

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

    [DeviceFsTestNative]::WriteVhdWitness(
        $Path, $DiskLength, ($PartitionOffset + $relative_offset), $Pattern)
}

function Mount-ValidatedReadOnlyVhd {
    param(
        [Parameter(Mandatory)]
        [string] $Path,

        [Parameter(Mandatory)]
        $Fixture
    )

    $attachment = [DeviceFsTestDisk]::Open($Path)
    try {
        Assert-Condition (
            ($attachment.DiskLength -eq $Fixture.DiskLength) -and
                ($attachment.PartitionNumber -eq $Fixture.PartitionNumber) -and
                ($attachment.Offset -eq $Fixture.PartitionOffset) -and
                ($attachment.Size -eq $Fixture.PartitionLength) -and
                ($attachment.VolumeName -eq $Fixture.VolumeName)) `
            'The read-only VHD partition identity changed.'
        $identity = [DeviceFsTestNative]::InspectVolume($Fixture.VolumeName)
        Assert-Condition (
            ($identity.Label -ceq $Fixture.VolumeLabel) -and
                ($identity.FileSystem -eq $Fixture.FileSystem) -and
                ($identity.Length -eq $Fixture.VolumeLength) -and
                ($identity.DiskNumber -eq $attachment.DiskNumber) -and
                ($identity.DiskStartingOffset -eq $attachment.Offset) -and
                ($identity.DiskExtentLength -eq $attachment.Size)) `
            'The read-only volume does not map to the expected VHD partition.'

        $bitmap = [DeviceFsTestNative]::GetAllocationBitmap(
            $Fixture.VolumeName, $true, { param($message) Write-TestLog $message })
        Assert-Condition (
            ($bitmap.Length -eq $Fixture.VolumeLength) -and
                ($bitmap.ClusterSize -eq $Fixture.ClusterSize)) `
            'The read-only allocation bitmap geometry is inconsistent with the fixture.'
        return [pscustomobject]@{ Attachment = $attachment; Bitmap = $bitmap }
    } catch {
        $attachment.Dispose()
        throw
    }
}

if (-not $IsWindows) {
    throw 'This integration test requires Windows.'
}
if (-not [Environment]::Is64BitProcess) {
    throw 'This integration test requires 64-bit PowerShell.'
}

if (-not $SupervisorPath) {
    $SupervisorPath = Get-DefaultTestExecutablePath 'backup-supervisor.exe'
}
$SupervisorPath = (Resolve-Path -LiteralPath $SupervisorPath).Path
Assert-Condition ([IO.File]::Exists($SupervisorPath)) `
    "backup-supervisor was not found at '$SupervisorPath'."

$native_source_path = [IO.Path]::Combine(
    $PSScriptRoot, 'types', 'DeviceFsTestNative.cs')
if (-not $ParallelWorker) {
    if ($null -ne ([Management.Automation.PSTypeName]'DeviceFsTestNative').Type) {
        throw 'DeviceFsTestNative is already loaded. Run the test in a fresh pwsh process.'
    }
    Add-DeviceFsTestTypes -SourcePath $native_source_path -SupervisorPath $SupervisorPath
}

function Invoke-SyntheticFreeClustersCase {
    param(
        [string] $FileSystem,
        [int] $ClusterSize,
        [switch] $DevDrive
    )

    $run_id = [Guid]::NewGuid().ToString('N')
    $test_root = $null
    $vhd_path = $null
    $source_mount = $null
    $source_partition = $null
    $read_only = $null
    $source_access_path_added = $false
    $normal_invocation = $null
    $synthetic_invocation = $null
    $comparison = $null
    $primary_error = $null
    $filesystem_unsupported = $false
    $cleanup_errors = [Collections.Generic.List[Exception]]::new()
    $devicefs_processes_gone = $true
    $image_detached = $false

    $source_label = "DFSTSRC-$($run_id.Substring(0, 16))"
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

        # Dev Drive requires a 50 GB volume; the extra GiB accommodates partitioning.
        # https://learn.microsoft.com/en-us/windows/dev-drive/#prerequisites
        $size_mib = if ($DevDrive) {
            51 * 1024
        } elseif ($FileSystem -eq 'ReFS') {
            [Math]::Max(2048, $VhdSizeMiB)
        } else {
            $VhdSizeMiB
        }
        $requested_disk_length = [UInt64]$size_mib * 1MB
        $source_partition = New-DeviceFsTestVolume -Path $vhd_path -SizeBytes $requested_disk_length `
            -ClusterSize $ClusterSize -FileSystem $FileSystem -Label $source_label -Fixed:(-not $DevDrive) `
            -DevDrive:$DevDrive
        $source_partition.Mount($source_mount)
        $source_access_path_added = $true

        $source_volume_name = [DeviceFsTestNative]::GetVolumeName($source_mount)
        $source_identity = [DeviceFsTestNative]::InspectVolume(
            $source_volume_name)
        Assert-Condition ($source_identity.Label -ceq $source_label) `
            'The source volume label does not match the unique test label.'
        Assert-Condition ($source_identity.FileSystem -eq $FileSystem) `
            "Expected $FileSystem, but the fixture uses $($source_identity.FileSystem)."
        Assert-Condition (
            ($source_identity.DiskNumber -eq $source_partition.DiskNumber) -and
                ($source_identity.DiskStartingOffset -eq
                    $source_partition.Offset) -and
                ($source_identity.DiskExtentLength -eq $source_partition.Size)) `
            'The source volume does not map to the expected VHD partition.'

        $fixture = [pscustomobject]@{
            DiskLength = [long]$source_partition.DiskLength
            PartitionNumber = $source_partition.PartitionNumber
            PartitionOffset = [long]$source_partition.Offset
            PartitionLength = [long]$source_partition.Size
            VolumeName = $source_volume_name
            VolumeLabel = $source_label
            FileSystem = $FileSystem
            VolumeLength = [long]$source_identity.Length
            ClusterSize = $ClusterSize
        }

        $witness_pattern = [byte[]]::new($ClusterSize)
        [Array]::Fill[byte]($witness_pattern, 0xA5)

        $source_partition.Unmount($source_mount)
        $source_access_path_added = $false
        $source_partition.Detach()

        $read_only = Mount-ValidatedReadOnlyVhd -Path $vhd_path -Fixture $fixture
        $selection_bitmap = $read_only.Bitmap
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

        $read_only.Attachment.Detach()
        Write-VhdCluster -Path $vhd_path -DiskLength $fixture.DiskLength `
            -PartitionOffset $fixture.PartitionOffset `
            -PartitionLength $fixture.PartitionLength `
            -ClusterSize $fixture.ClusterSize -Lcn $witness_lcn `
            -Pattern $witness_pattern

        $read_only = Mount-ValidatedReadOnlyVhd -Path $vhd_path -Fixture $fixture
        $bitmap = $read_only.Bitmap
        Assert-Condition (
            ($bitmap.SectorSize -eq $selection_bitmap.SectorSize) -and
                ($bitmap.ClusterSize -eq $selection_bitmap.ClusterSize) -and
                ($bitmap.ClusterCount -eq $selection_bitmap.ClusterCount)) `
            'The allocation bitmap geometry changed after offline modification.'
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
            -SupervisorPath $SupervisorPath -MountPath $normal_mount `
            -ReadUser $read_user -StopEvent "Local\devicefs-test-$run_id-normal" `
            -Mappings ([ordered]@{ 'volume.img' = $source_device })
        Wait-DeviceFsReady $normal_invocation
        $synthetic_invocation = Start-DeviceFsTestProcess `
            -SupervisorPath $SupervisorPath -MountPath $synthetic_mount `
            -ReadUser $read_user -StopEvent "Local\devicefs-test-$run_id-synthetic" `
            -Mappings ([ordered]@{ 'volume.img' = $source_device }) `
            -SyntheticFreeClusters -KnownDataMapClusterSize $KnownDataMapClusterSize
        Wait-DeviceFsReady $synthetic_invocation

        $normal_image = $normal_invocation.ImagePaths['volume.img']
        $synthetic_image = $synthetic_invocation.ImagePaths['volume.img']
        $map_path = "$synthetic_image.known-data.bitmap"
        Assert-Condition (-not [IO.File]::Exists("$normal_image.known-data.bitmap")) `
            'The filesystem exposed a bitmap without the option.'
        Write-TestLog 'PASS: no bitmap file is exposed when --expose-known-data-map is omitted.'

        $entries = @(Get-ChildItem -LiteralPath $synthetic_mount -Name)
        Assert-Condition (($entries.Count -eq 2) -and
            ($entries -contains 'volume.img') -and
            ($entries -contains 'volume.img.known-data.bitmap')) `
            'The directory listing did not contain the image and its bitmap.'
        Write-TestLog 'PASS: the directory lists the image and its bitmap file.'

        $map = [IO.File]::ReadAllBytes($map_path)
        $chunk_count = [long][Math]::Ceiling($bitmap.Length / $KnownDataMapClusterSize)
        Assert-Condition ($map.Length -eq [long][Math]::Ceiling($chunk_count / 8)) `
            'The exposed bitmap has the wrong length.'
        Write-TestLog "PASS: the bitmap has the expected length of $($map.Length) bytes."

        for ($chunk = 0L; $chunk -lt $chunk_count; ++$chunk) {
            $first = [long][Math]::Floor($chunk * $KnownDataMapClusterSize / $bitmap.ClusterSize)
            $end = [Math]::Min(($chunk + 1) * $KnownDataMapClusterSize, $bitmap.Length)
            $last = [long][Math]::Floor(($end - 1) / $bitmap.ClusterSize)
            $allocated = $false
            for ($cluster = $first; $cluster -le $last; ++$cluster) {
                if ($bitmap.IsAllocated($cluster)) {
                    $allocated = $true
                    break
                }
            }
            $index = [int][Math]::Floor($chunk / 8)
            $actual = ($map[$index] -band (1 -shl ($chunk % 8))) -ne 0
            Assert-Condition ($actual -eq $allocated) "Known-data bitmap differs at chunk $chunk."
        }
        Write-TestLog ("PASS: all $chunk_count bits match the allocation bitmap " +
            "at $KnownDataMapClusterSize bytes per bit.")

        $map_stream = [IO.File]::OpenRead($map_path)
        try {
            $null = $map_stream.Seek(-1, [IO.SeekOrigin]::End)
            Assert-Condition ($map_stream.ReadByte() -eq $map[-1]) 'Reading the last bitmap byte failed.'
            Write-TestLog 'PASS: seeking to the final bitmap byte returns the correct value.'
            Assert-Condition ($map_stream.ReadByte() -eq -1) 'Reading past the bitmap did not return EOF.'
            Write-TestLog 'PASS: reading past the bitmap returns EOF.'
        } finally {
            $map_stream.Dispose()
        }

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

        $comparison_length = if ($DevDrive) { [Math]::Min(2GB, $bitmap.Length) } else { $bitmap.Length }
        $comparison = [DeviceFsTestNative]::CompareViews(
            $source_device, $normal_image, $synthetic_image, $bitmap,
            $comparison_chunk_size, $comparison_length)
        Assert-Condition ($comparison.BytesCompared -eq $comparison_length) `
            'The sequential comparison did not cover the requested byte count.'
        Write-TestLog ("PASS: compared $($comparison.BytesCompared) bytes; " +
            "$($comparison.FreeBytes) bytes belonged to free clusters; targeted reads verified " +
            "$($bitmap.ClusterSize) controlled nonzero witness bytes that " +
            'the synthetic view replaced with zeros.')
    } catch {
        if (($_.Exception.Message -creplace '\s', '').Contains(
            ('ReFS file system is not supported on this device' -creplace '\s', ''))) {
            $filesystem_unsupported = $true
        } else {
            $primary_error = $_
        }
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
                    Write-TestLog -Warning (
                        "devicefs process $($invocation.Process.Id) remains " +
                        'alive; the VHD and test directory will be preserved.')
                }
            }
        }

        if ($devicefs_processes_gone -and $source_access_path_added) {
            try {
                $source_partition.Unmount($source_mount)
                $source_access_path_added = $false
            } catch {
                $cleanup_errors.Add($_.Exception)
            }
        }

        if ($devicefs_processes_gone -and (-not $source_access_path_added) -and
            ($null -ne $vhd_path) -and [IO.File]::Exists($vhd_path)) {
            try {
                if ($null -ne $read_only) { $read_only.Attachment.Detach() }
                if ($null -ne $source_partition) { $source_partition.Detach() }
                $image_detached = $true
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
                Write-TestLog -Warning "Test artifacts were preserved at '$test_root'."
            }
        } elseif (($null -ne $test_root) -and
            (Test-Path -LiteralPath $test_root)) {
            Write-TestLog -Warning "Test artifacts were preserved at '$test_root'."
        }
    }

    if ($null -ne $primary_error) {
        foreach ($cleanup_error in $cleanup_errors) {
            Write-TestLog -Warning "Cleanup also failed: $($cleanup_error.Message)"
        }
        throw $primary_error
    }
    if ($cleanup_errors.Count -ne 0) {
        throw [AggregateException]::new('Test cleanup failed.',
            $cleanup_errors)
    }
    return -not $filesystem_unsupported
}

if ($ParallelWorker) {
    $case = $selected_cases[0]
    $settings = $case_settings[$case]
    Write-TestLog 'Testing'
    try {
        return [pscustomobject]@{
            Case = $case
            Available = Invoke-SyntheticFreeClustersCase @settings
            Error = $null
        }
    } catch {
        return [pscustomobject]@{ Case = $case; Available = $false; Error = $_.Exception }
    }
}

$script_path = $PSCommandPath
$parameters = @{
    SupervisorPath = $SupervisorPath
    VhdSizeMiB = $VhdSizeMiB
    KnownDataMapClusterSize = $KnownDataMapClusterSize
    KeepArtifactsOnFailure = $KeepArtifactsOnFailure.IsPresent
    ParallelWorker = $true
}
$results = @($selected_cases | ForEach-Object -Parallel {
    $ErrorActionPreference = 'Stop'
    & $using:script_path @using:parameters -Cases $_
})

$failures = [Collections.Generic.List[Exception]]::new()
foreach ($result in $results) {
    if ($null -ne $result.Error) {
        Write-TestLog -CaseName $result.Case "FAIL: $($result.Error.Message)"
        $failures.Add($result.Error)
    } elseif ($result.Available) {
        Write-TestLog -CaseName $result.Case 'PASS'
    } else {
        Write-TestLog -CaseName $result.Case "Information: Windows does not support formatting $($case_settings[$result.Case].FileSystem) on this system."
    }
}
if ($failures.Count -ne 0) {
    throw [AggregateException]::new('Synthetic free-cluster tests failed.', $failures)
}
