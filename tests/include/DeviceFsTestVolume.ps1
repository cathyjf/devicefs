# SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
# SPDX-License-Identifier: GPL-3.0-or-later

# Create and attach a new VHD or VHDX containing one writable NTFS partition,
# without assigning a drive letter. Return the partition for the caller to
# mount or populate. The filename selects VHD or VHDX; `Fixed` selects fully
# allocated storage rather than a dynamic image.
# The caller is responsible for detaching and deleting `Path`, including when
# creation fails after the image has been attached.
function New-DeviceFsTestVolume {
    param(
        [Parameter(Mandatory)]
        [string] $Path,

        [Parameter(Mandatory)]
        [UInt64] $SizeBytes,

        [Parameter(Mandatory)]
        [int] $ClusterSize,

        [Parameter(Mandatory)]
        [string] $Label,

        [switch] $Fixed
    )

    $allocation = if ($Fixed) { @{ Fixed = $true } } else { @{ Dynamic = $true } }
    New-VHD -Path $Path -SizeBytes $SizeBytes @allocation | Out-Null
    $image = Mount-DiskImage -ImagePath $Path -Access ReadWrite -NoDriveLetter -PassThru
    $disks = @($image | Get-Disk)
    if ($disks.Count -ne 1) {
        throw "The new image '$Path' did not resolve to exactly one disk."
    }
    $disk = $disks[0]
    # `MSFT_Disk` uses `STORAGE_BUS_TYPE`'s `BusTypeFileBackedVirtual` (15) and
    # `PartitionStyle`'s `RAW` (0). These checks precede all partition writes.
    $file_backed_virtual_bus_type = [UInt16]15
    $uninitialized_partition_style = [UInt16]0
    if (($disk.CimInstanceProperties['BusType'].Value -ne $file_backed_virtual_bus_type) -or
        ($disk.CimInstanceProperties['PartitionStyle'].Value -ne $uninitialized_partition_style) -or
        ($disk.Size -ne $SizeBytes) -or $disk.IsBoot -or $disk.IsSystem -or
        $disk.IsClustered -or $disk.IsOffline -or $disk.IsReadOnly) {
        throw "The new image '$Path' did not resolve to a writable, uninitialized virtual disk of the requested size."
    }
    if (@(Get-Partition -DiskNumber $disk.Number -ErrorAction SilentlyContinue).Count -ne 0) {
        throw "The new image '$Path' unexpectedly contains partitions."
    }

    Initialize-Disk -Number $disk.Number -PartitionStyle GPT -PassThru | Out-Null
    $partition = New-Partition -DiskNumber $disk.Number -UseMaximumSize -IsHidden
    # A hidden partition cannot acquire a drive letter during creation.
    # `NoDefaultDriveLetter` keeps it letterless when it becomes visible for
    # formatting. PowerShell represents an absent drive letter as `U+0000`.
    if ((-not $partition.IsHidden) -or ([char]$partition.DriveLetter -ne [char]0)) {
        throw "The test partition in '$Path' was not created hidden and letterless."
    }
    Set-Partition -InputObject $partition -NoDefaultDriveLetter $true `
        -IsHidden $false -Confirm:$false | Out-Null
    $partition = Get-Partition -DiskNumber $disk.Number -PartitionNumber $partition.PartitionNumber
    if ($partition.IsHidden -or (-not $partition.NoDefaultDriveLetter) -or
        ([char]$partition.DriveLetter -ne [char]0)) {
        throw "The test partition in '$Path' did not become visible without a drive letter."
    }
    Format-Volume -Partition $partition -FileSystem NTFS -AllocationUnitSize $ClusterSize `
        -NewFileSystemLabel $Label -Force -Confirm:$false | Out-Null
    return $partition
}
