# SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
# SPDX-License-Identifier: GPL-3.0-or-later

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
        [VolumeAllocationBitmap] $Bitmap,

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
