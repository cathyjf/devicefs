# SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
# SPDX-License-Identifier: GPL-3.0-or-later

function Format-VssDescriptorTuple {
    param(
        [uint64] $Original,
        [uint64] $Relative,
        [uint64] $StoreOffset,
        [uint32] $Flags,
        [uint32] $Bitmap
    )

    return ("{0:x16}`t{1:x16}`t{2:x16}`t{3:x8}`t{4:x8}" -f
        $Original, $Relative, $StoreOffset, $Flags, $Bitmap)
}

function ConvertFrom-VssDescriptorDumpOutput {
    param(
        [AllowEmptyString()][string] $Output,
        [Guid] $ExpectedCopyId,
        [int] $StoreIndex,
        [uint32] $ForwarderFlag,
        [uint32] $OverlayFlag,
        [long] $SourceLength,
        [int] $BlockSize
    )

    $guid_pattern =
        '[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}'
    $header_pattern = (
        '\Aschema-version\t1\r?\n' +
        "snapshot-id\t(?<copy>$guid_pattern)\r?\n" +
        "store-id\t(?<store>$guid_pattern)\r?\n" +
        'volume-size\t(?<volume>[0-9]+)\r?\n' +
        'list-block-count\t(?<lists>[0-9]+)\r?\n' +
        'descriptor-count\t(?<count>[0-9]+)\r?\n' +
        'forwarder-count\t(?<forwarders>[0-9]+)\r?\n' +
        'overlay-count\t(?<overlays>[0-9]+)\r?\n')
    Assert-Condition ($Output -match $header_pattern) `
        'vss-descriptor-dump printed an invalid header.'
    $header = $Matches
    $copy_id = [Guid]$header['copy']
    Assert-Condition ($copy_id -eq $ExpectedCopyId) (
        "vss-descriptor-dump returned snapshot $copy_id for request " +
        "$ExpectedCopyId.")

    $records = [Collections.Generic.List[string]]::new()
    $legacy = [Collections.Generic.List[string]]::new()
    $descriptor_blocks = [Collections.Generic.HashSet[long]]::new()
    $legacy.Add("Store: $StoreIndex")
    $legacy.Add("    Shadow copy ID : $copy_id")
    $legacy.Add("    Number of blocks : $($header['count'])")
    $forwarders = [uint64]0
    $overlays = [uint64]0
    $descriptor_output = $Output.Substring($header[0].Length)
    foreach ($line in $descriptor_output -split '\r?\n') {
        if ($line.Length -eq 0) {
            continue
        }
        Assert-Condition (
            $line -match
                ('^descriptor\t([0-9a-f]{16})\t' +
                    '([0-9a-f]{16})\t([0-9a-f]{16})\t' +
                    '([0-9a-f]{8})\t([0-9a-f]{8})$')) `
            "vss-descriptor-dump printed an unexpected line: $line"
        $original = [Convert]::ToUInt64($Matches[1], 16)
        $relative = [Convert]::ToUInt64($Matches[2], 16)
        $store_offset = [Convert]::ToUInt64($Matches[3], 16)
        $flags = [Convert]::ToUInt32($Matches[4], 16)
        $bitmap = [Convert]::ToUInt32($Matches[5], 16)
        $records.Add((Format-VssDescriptorTuple $original $relative `
                    $store_offset $flags $bitmap))
        if (($flags -band $ForwarderFlag) -ne 0) {
            ++$forwarders
        }
        if (($flags -band $OverlayFlag) -ne 0) {
            ++$overlays
        }
        $offsets = @($original, $store_offset)
        if (($flags -band $ForwarderFlag) -ne 0) {
            $offsets += $relative
        }
        foreach ($offset in $offsets) {
            if ($offset -lt [uint64]$SourceLength) {
                $block = $offset - ($offset % [uint64]$BlockSize)
                $null = $descriptor_blocks.Add([long]$block)
            }
        }
        $legacy.Add("    Original offset : 0x$($Matches[1])")
        $legacy.Add("    Relative offset : 0x$($Matches[2])")
        $legacy.Add("    Offset : 0x$($Matches[3])")
        $legacy.Add("    Flags : 0x$($Matches[4])")
    }

    $descriptor_count = [Convert]::ToUInt64($header['count'])
    $reported_forwarders = [Convert]::ToUInt64($header['forwarders'])
    $reported_overlays = [Convert]::ToUInt64($header['overlays'])
    Assert-Condition ([uint64]$records.Count -eq $descriptor_count) (
        "vss-descriptor-dump reported $descriptor_count descriptors but " +
        "printed $($records.Count).")
    Assert-Condition ($forwarders -eq $reported_forwarders) (
        "vss-descriptor-dump reported $reported_forwarders forwarders but " +
        "printed $forwarders.")
    Assert-Condition ($overlays -eq $reported_overlays) (
        "vss-descriptor-dump reported $reported_overlays overlays but " +
        "printed $overlays.")

    return [pscustomobject]@{
        CopyId = $copy_id
        StoreId = [Guid]$header['store']
        VolumeSize = [Convert]::ToUInt64($header['volume'])
        ListBlockCount = [Convert]::ToUInt64($header['lists'])
        DescriptorCount = $descriptor_count
        ForwarderCount = $forwarders
        OverlayCount = $overlays
        Records = $records
        DescriptorBlocks = $descriptor_blocks
        VshadowInfoProjection =
            [string]::Join([Environment]::NewLine, $legacy)
    }
}

function ConvertFrom-VshadowInfoDescriptorOutput {
    param(
        [AllowEmptyString()][string] $Output,
        [uint32] $ForwarderFlag,
        [uint32] $OverlayFlag
    )

    $stores = [Collections.Generic.List[object]]::new()
    $current_store = $null
    $original = $null
    $relative = $null
    $store_offset = $null
    $flags = $null
    foreach ($line in $Output -split '\r?\n') {
        if ($line -match '^Store:\s+(\d+)$') {
            Assert-Condition (
                ($null -eq $original) -and ($null -eq $relative) -and
                    ($null -eq $store_offset) -and ($null -eq $flags)) `
                'vshadowinfo began a store within a block descriptor.'
            $current_store = [pscustomobject]@{
                Index = [Convert]::ToInt32($Matches[1])
                StoreId = $null
                CopyId = $null
                VolumeSize = $null
                DescriptorCount = $null
                ForwarderCount = [uint64]0
                OverlayCount = [uint64]0
                Records = [Collections.Generic.List[string]]::new()
            }
            $stores.Add($current_store)
        } elseif ($line -match
            '^\s+Identifier\s+:\s+([0-9a-f-]{36})$') {
            Assert-Condition (
                ($null -ne $current_store) -and
                    ($null -eq $current_store.StoreId)) `
                'vshadowinfo printed a misplaced store identifier.'
            $current_store.StoreId = [Guid]$Matches[1]
        } elseif ($line -match
            '^\s+Shadow copy ID\s+:\s+([0-9a-f-]{36})$') {
            Assert-Condition (
                ($null -ne $current_store) -and
                    ($null -eq $current_store.CopyId)) `
                'vshadowinfo printed a misplaced shadow copy ID.'
            $current_store.CopyId = [Guid]$Matches[1]
        } elseif ($line -match
            '^\s+Volume size\s+:\s+[^()]+\(([0-9]+) bytes\)$') {
            Assert-Condition (
                ($null -ne $current_store) -and
                    ($null -eq $current_store.VolumeSize)) `
                'vshadowinfo printed a misplaced or repeated volume size.'
            $current_store.VolumeSize = [Convert]::ToUInt64($Matches[1])
        } elseif ($line -match
            '^\s+Volume size\s+:\s+([0-9]+) bytes$') {
            Assert-Condition (
                ($null -ne $current_store) -and
                    ($null -eq $current_store.VolumeSize)) `
                'vshadowinfo printed a misplaced or repeated volume size.'
            $current_store.VolumeSize = [Convert]::ToUInt64($Matches[1])
        } elseif ($line -match '^\s+Number of blocks\s+:\s+([0-9]+)$') {
            Assert-Condition (
                ($null -ne $current_store) -and
                    ($null -eq $current_store.DescriptorCount)) `
                'vshadowinfo printed a misplaced or repeated block count.'
            $current_store.DescriptorCount =
                [Convert]::ToUInt64($Matches[1])
        } elseif ($line -match
            '^\s+Original offset\s+:\s+0x([0-9a-f]+)$') {
            Assert-Condition (
                ($null -eq $original) -and ($null -eq $relative) -and
                    ($null -eq $store_offset) -and ($null -eq $flags)) `
                'vshadowinfo began overlapping block descriptors.'
            $original = [Convert]::ToUInt64($Matches[1], 16)
        } elseif ($line -match
            '^\s+Relative offset\s+:\s+0x([0-9a-f]+)$') {
            $relative = [Convert]::ToUInt64($Matches[1], 16)
        } elseif ($line -match '^\s+Offset\s+:\s+0x([0-9a-f]+)$') {
            $store_offset = [Convert]::ToUInt64($Matches[1], 16)
        } elseif ($line -match '^\s+Flags\s+:\s+0x([0-9a-f]+)$') {
            $flags = [Convert]::ToUInt32($Matches[1], 16)
        } elseif ($line -match '^\s+Bitmap\s+:\s+0x([0-9a-f]+)$') {
            Assert-Condition (
                ($null -ne $current_store) -and
                    ($null -ne $current_store.CopyId) -and
                    ($null -ne $current_store.DescriptorCount) -and
                    ($null -ne $original) -and ($null -ne $relative) -and
                    ($null -ne $store_offset) -and ($null -ne $flags)) `
                'vshadowinfo printed an incomplete block descriptor.'
            $bitmap = [Convert]::ToUInt32($Matches[1], 16)
            $current_store.Records.Add((Format-VssDescriptorTuple `
                    $original $relative $store_offset $flags $bitmap))
            if (($flags -band $ForwarderFlag) -ne 0) {
                ++$current_store.ForwarderCount
            }
            if (($flags -band $OverlayFlag) -ne 0) {
                ++$current_store.OverlayCount
            }
            $original = $null
            $relative = $null
            $store_offset = $null
            $flags = $null
        }
    }
    Assert-Condition (
        ($null -eq $original) -and ($null -eq $relative) -and
            ($null -eq $store_offset) -and ($null -eq $flags)) `
        'vshadowinfo ended within a block descriptor.'

    $by_copy_id = @{}
    foreach ($store in $stores) {
        Assert-Condition (
            ($null -ne $store.StoreId) -and
                ($null -ne $store.CopyId) -and
                ($null -ne $store.VolumeSize) -and
                ($null -ne $store.DescriptorCount) -and
                ([uint64]$store.Records.Count -eq $store.DescriptorCount)) `
            "vshadowinfo output for store $($store.Index) was incomplete."
        $key = $store.CopyId.ToString('D')
        Assert-Condition (-not $by_copy_id.ContainsKey($key)) `
            "vshadowinfo reported snapshot $key more than once."
        $by_copy_id[$key] = $store
    }
    return $by_copy_id
}

function Compare-VssDescriptorStores {
    param(
        [Parameter(Mandatory)] $VshadowInfo,
        [Parameter(Mandatory)] $DescriptorDump
    )

    $differences = [Collections.Generic.List[string]]::new()
    foreach ($field in @('CopyId', 'StoreId', 'VolumeSize',
            'DescriptorCount', 'ForwarderCount', 'OverlayCount')) {
        if ($VshadowInfo.$field -ne $DescriptorDump.$field) {
            $differences.Add(
                "$field differs: vshadowinfo=$($VshadowInfo.$field), " +
                "vss-descriptor-dump=$($DescriptorDump.$field)")
        }
    }

    $stock_counts =
        [Collections.Generic.Dictionary[string, int]]::new(
            [StringComparer]::Ordinal)
    $dump_counts =
        [Collections.Generic.Dictionary[string, int]]::new(
            [StringComparer]::Ordinal)
    $keys = [Collections.Generic.HashSet[string]]::new(
        [StringComparer]::Ordinal)
    foreach ($record in $VshadowInfo.Records) {
        if ($stock_counts.ContainsKey($record)) {
            ++$stock_counts[$record]
        } else {
            $stock_counts[$record] = 1
        }
        $null = $keys.Add($record)
    }
    foreach ($record in $DescriptorDump.Records) {
        if ($dump_counts.ContainsKey($record)) {
            ++$dump_counts[$record]
        } else {
            $dump_counts[$record] = 1
        }
        $null = $keys.Add($record)
    }
    foreach ($record in $keys) {
        $stock_count = 0
        $dump_count = 0
        $null = $stock_counts.TryGetValue($record, [ref]$stock_count)
        $null = $dump_counts.TryGetValue($record, [ref]$dump_count)
        if ($stock_count -ne $dump_count) {
            $differences.Add(
                "tuple $record differs: vshadowinfo=$stock_count, " +
                "vss-descriptor-dump=$dump_count")
        }
    }
    return $differences
}
