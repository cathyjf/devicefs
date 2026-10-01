# SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
# SPDX-License-Identifier: GPL-3.0-or-later

#requires -Version 7.4
<#
.SYNOPSIS
Tests the production VSS catalog diagnostic using small synthetic volume images.
.DESCRIPTION
Creates synthetic volume images in a temporary directory and checks catalog
order, snapshot IDs, creation times, type-3 record association, and selection
of every descriptor store between two snapshots. Malformed-image cases check
that a failed parse produces no partial output. Each case runs independently
so a failure does not prevent the remaining cases from running. The script
runs without elevation and removes its temporary directory when finished.
#>
[CmdletBinding()]
param([string] $SupervisorPath)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
. (Join-Path $PSScriptRoot 'include/DeviceFsTestProcess.ps1')
. (Join-Path $PSScriptRoot 'include/VssDescriptorOutput.ps1')
function Assert-Condition {
    param([bool] $Condition, [string] $Message)
    if (-not $Condition) { throw $Message }
}
if (-not $SupervisorPath) { $SupervisorPath = Get-DefaultTestExecutablePath 'backup-supervisor.exe' }
$SupervisorPath = (Resolve-Path -LiteralPath $SupervisorPath).Path
$directory = [IO.Directory]::CreateTempSubdirectory('devicefs-vss-catalog-')
$failures = [Collections.Generic.List[string]]::new()
try {
    $image = [byte[]]::new(1MB)
    # The fixture places metadata blocks at fixed offsets within this image.
    # Field positions within those blocks follow the libvshadow sources cited
    # beside their decoders in `vss_block_descriptors.ixx`.
    function Put {
        param([int] $Offset, [byte[]] $Bytes)
        [Array]::Copy($Bytes, 0, $image, $Offset, $Bytes.Length)
    }
    function Put-U32 { param([int] $Offset, [uint32] $Value); Put $Offset ([BitConverter]::GetBytes($Value)) }
    function Put-U64 { param([int] $Offset, [uint64] $Value); Put $Offset ([BitConverter]::GetBytes($Value)) }
    $identifier = [Convert]::FromHexString('6b87083876c1484eb7ae04046e6cc752')
    function Put-Header {
        param([int] $Offset, [uint32] $Type)
        Put $Offset $identifier
        Put-U32 ($Offset + 16) 1
        Put-U32 ($Offset + 20) $Type
    }
    foreach ($offset in @(0, ($image.Length - 512))) {
        Put ($offset + 3) ([Text.Encoding]::ASCII.GetBytes('NTFS    '))
        Put ($offset + 11) ([BitConverter]::GetBytes([uint16]512))
        Put-U64 ($offset + 40) ($image.Length / 512 - 1)
    }
    Put-Header 0x1e00 1
    Put-U64 (0x1e00 + 48) 0x4000
    Put-Header 0x4000 2
    $store_a = [Guid]'11111111-1111-1111-1111-111111111111'
    $store_b = [Guid]'22222222-2222-2222-2222-222222222222'
    $store_external = [Guid]'33333333-3333-3333-3333-333333333333'
    $copy_a = [Guid]'aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa'
    $copy_b = [Guid]'bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb'
    $copy_c = [Guid]'cccccccc-cccc-cccc-cccc-cccccccccccc'
    $time_a = [uint64]133000000000000001
    $time_b = $time_a + [uint64]10000000
    function Put-Type2 {
        param([int] $Offset, [Guid] $Store, [uint64] $Time)
        Put-U64 $Offset 2
        Put-U64 ($Offset + 8) $image.Length
        Put ($Offset + 16) $Store.ToByteArray()
        Put-U64 ($Offset + 48) $Time
    }
    function Put-Type3 {
        param([int] $Offset, [Guid] $Store, [uint64] $List, [uint64] $Header)
        Put-U64 $Offset 3
        Put-U64 ($Offset + 8) $List
        Put ($Offset + 16) $Store.ToByteArray()
        Put-U64 ($Offset + 32) $Header
    }
    # The catalog starts at 0x4000; its 128-byte entries follow a 128-byte
    # header. The type-2 entries deliberately list B, A, and the external store
    # in that order so physical order cannot stand in for creation-time order.
    # The type-3 entries are separated from their matching type-2 entries, and
    # the second type-3 entry for A replaces A's first pair of offsets.
    #
    # The final metadata locations are:
    #   Store    Header     Descriptor list
    #   A        0x8000     0x10000
    #   B        0xc000     0x14000
    # A's superseded offsets (0x1c000 and 0x18000) contain no valid blocks here;
    # following them would fail. `Set-IntervalFixture` later uses those blocks
    # for C by adding a type-3 entry to the previously external store.
    Put-Type2 0x4080 $store_b $time_b
    Put-Type2 0x4100 $store_a $time_a
    Put-Type2 0x4180 $store_external $time_a
    Put-Type3 0x4200 $store_a 0x18000 0x1c000
    Put-Type3 0x4280 $store_b 0x14000 0xc000
    Put-Type3 0x4300 $store_a 0x10000 0x8000
    Put-Header 0x8000 4
    Put (0x8000 + 144) $copy_a.ToByteArray()
    Put-Header 0xc000 4
    Put (0xc000 + 144) $copy_b.ToByteArray()
    Put-Header 0x10000 3
    # Each list has one ordinary descriptor after its 128-byte header.
    # The original and store offsets differ between A and B so the tests can
    # distinguish the individual stores from their combined block set.
    Put-U64 (0x10000 + 128) 0x20000
    Put-U64 (0x10000 + 128 + 16) 0x30000
    Put-Header 0x14000 3
    Put-U64 (0x14000 + 128) 0x24000
    Put-U64 (0x14000 + 128 + 16) 0x34000
    $original = $image.Clone()
    function Set-IntervalFixture {
        # An A-to-C query must return stores A and B even though the catalog
        # lists them in the order B, A, C.
        Put-Type2 0x4180 $store_external ($time_b + [uint64]10000000)
        Put-Type3 0x4380 $store_external 0x18000 0x1c000
        Put-Header 0x1c000 4
        Put (0x1c000 + 144) $copy_c.ToByteArray()
        Put-Header 0x18000 3
        Put-U64 (0x18000 + 128) 0x28000
    }
    function Read-Interval {
        param([Guid] $A = $copy_a, [Guid] $B = $copy_c, [switch] $Failure)
        $text = Invoke-Fixture @('--baseline-id', $A.ToString(), '--snapshot-id', $B.ToString()) -Failure:$Failure
        if (-not $Failure) {
            ConvertFrom-VssIntervalOutput $text $A $B $image.Length 16KB
        }
    }
    $path = Join-Path $directory.FullName 'volume.img'
    function Invoke-Fixture {
        param([string[]] $Options, [switch] $Failure)
        [IO.File]::WriteAllBytes($path, $image)
        $stdout = Join-Path $directory.FullName 'stdout.txt'
        $stderr = Join-Path $directory.FullName 'stderr.txt'
        & $SupervisorPath --vss-descriptor-dump --source $path @Options >$stdout 2>$stderr
        $code = $LASTEXITCODE
        $text = [IO.File]::ReadAllText($stdout)
        if ($Failure) {
            Assert-Condition ($code -ne 0) 'Malformed fixture unexpectedly succeeded.'
            Assert-Condition ($text.Length -eq 0) 'Failed parse produced partial machine-readable output.'
            Assert-Condition ([IO.File]::ReadAllText($stderr).Length -gt 0) 'Failed parse omitted its diagnostic.'
        } else {
            Assert-Condition ($code -eq 0) "Parser exited $code`: $([IO.File]::ReadAllText($stderr))"
        }
        return $text
    }
    $cases = [ordered]@{
        'catalog facts preserve physical order and equal times' = {
            $stores = ConvertFrom-VssCatalogOutput (Invoke-Fixture @('--catalog'))
            Assert-Condition ($stores.Count -eq 3) 'Wrong store count.'
            Assert-Condition (($stores[0].CopyId -eq $copy_b) -and ($stores[1].CopyId -eq $copy_a)) 'Catalog stores were reordered or misassociated.'
            Assert-Condition (($stores[0].CreationFileTime -eq $time_b) -and
                ($stores[1].CreationFileTime -eq $time_a) -and ($stores[2].CreationFileTime -eq $time_a)) 'Creation FILETIME changed.'
            Assert-Condition (($null -eq $stores[2].CopyId) -and (-not $stores[2].InVolume)) 'A store without in-volume data acquired a snapshot ID.'
            Assert-Condition (($stores[1].ListOffset -eq 0x10000) -and ($stores[1].HeaderOffset -eq 0x8000)) 'Later matching type-3 entry did not replace physical offsets.'
        }
        'exact store selection and intermediate-only descriptor' = {
            $a = ConvertFrom-VssDescriptorDumpOutput (Invoke-Fixture @('--snapshot-id', $copy_a.ToString())) $copy_a 0 1 2 $image.Length 16KB
            $b = ConvertFrom-VssDescriptorDumpOutput (Invoke-Fixture @('--snapshot-id', $copy_b.ToString())) $copy_b 1 1 2 $image.Length 16KB
            Assert-Condition (($a.DescriptorCount -eq 1) -and ($b.DescriptorCount -eq 1)) 'Descriptor count changed.'
            Assert-Condition ((-not $a.DescriptorBlocks.Contains(0x24000)) -and $b.DescriptorBlocks.Contains(0x24000)) 'Intermediate-only synthetic witness was not distinguished from A.'
        }
        'catalog does not need descriptor-list contents' = {
            $image[0x10000] = 0
            $null = Invoke-Fixture @('--catalog')
            $null = Invoke-Fixture @('--snapshot-id', $copy_a.ToString()) -Failure
        }
        'invalid secondary store header prevents partial catalog' = {
            Put-U32 (0xc000 + 20) 99
            $null = Invoke-Fixture @('--catalog') -Failure
        }
        'catalog cycle fails without partial output' = {
            Put-U64 (0x4000 + 40) 0x4000
            $null = Invoke-Fixture @('--catalog') -Failure
        }
        'missing exact endpoint fails' = {
            $null = Invoke-Fixture @('--snapshot-id', [Guid]::Empty.ToString()) -Failure
        }
        'duplicate copy identity remains caller-level failure' = {
            Put (0xc000 + 144) $copy_a.ToByteArray()
            $null = Invoke-Fixture @('--catalog')
            $null = Invoke-Fixture @('--snapshot-id', $copy_a.ToString()) -Failure
        }
        'catalog and store selection cannot be combined' = {
            $null = Invoke-Fixture @('--catalog', '--snapshot-id', $copy_a.ToString()) -Failure
        }
        'nonadjacent interval includes intermediate evidence and excludes payload' = {
            Set-IntervalFixture
            $stores = Read-Interval
            Assert-Condition (($stores.Count -eq 2) -and ($stores[0].CopyId -eq $copy_a) -and
                ($stores[1].CopyId -eq $copy_b)) 'Wrong contributing store order.'
            $union = [Collections.Generic.HashSet[long]]::new()
            foreach ($store in $stores) { $union.UnionWith($store.DescriptorBlocks) }
            Assert-Condition ((-not $stores[0].DescriptorBlocks.Contains(0x24000)) -and
                $union.Contains(0x24000)) 'Complete interval missed the intermediate-only witness.'
            Assert-Condition (-not $union.Contains(0x28000)) 'Payload store leaked into the interval.'
            foreach ($store in $stores) {
                $single = ConvertFrom-VssDescriptorDumpOutput (Invoke-Fixture @('--snapshot-id', $store.CopyId.ToString())) `
                    $store.CopyId 0 1 2 $image.Length 16KB
                Assert-Condition ([string]::Join(';', $single.Records) -ceq
                    [string]::Join(';', $store.Records)) 'Interval changed raw descriptor order or multiplicity.'
            }
        }
        'adjacent interval agrees with one-store lookup' = {
            Set-IntervalFixture
            $stores = Read-Interval -B $copy_b
            Assert-Condition (($stores.Count -eq 1) -and ($stores[0].CopyId -eq $copy_a)) 'Adjacent interval included another store.'
        }
        'interval fails as a whole for a malformed intermediate list' = {
            Set-IntervalFixture
            $image[0x14000] = 0
            Read-Interval -Failure
        }
        'interval does not read the payload descriptor list' = {
            Set-IntervalFixture
            $image[0x18000] = 0
            $null = Read-Interval
        }
        'interval does not read a later descriptor list' = {
            Set-IntervalFixture
            $image[0x18000] = 0
            $null = Read-Interval -B $copy_b
        }
        'three intermediate stores contribute in chronological order' = {
            Set-IntervalFixture
            Put-U64 (0x4180 + 48) ($time_b + [uint64]30000000)
            $extra_ids = @([Guid]'44444444-4444-4444-4444-444444444444',
                [Guid]'55555555-5555-5555-5555-555555555555')
            for ($i = 0; $i -lt $extra_ids.Count; ++$i) {
                $header = 0x40000 + $i * 0x8000
                $list = $header + 0x4000
                Put-Type2 (0x4400 + $i * 128) $extra_ids[$i] ($time_b + [uint64](($i + 1) * 10000000))
                Put-Type3 (0x4500 + $i * 128) $extra_ids[$i] $list $header
                Put-Header $header 4
                Put ($header + 144) $extra_ids[$i].ToByteArray()
                Put-Header $list 3
                Put-U64 ($list + 128) (0x60000 + $i * 16KB)
            }
            $stores = Read-Interval
            Assert-Condition ($stores.Count -eq 4) 'Longer interval omitted a store.'
            Assert-Condition (($stores[2].CopyId -eq $extra_ids[0]) -and
                ($stores[3].CopyId -eq $extra_ids[1])) 'Longer interval reordered its intermediate stores.'
            Assert-Condition ($stores[2].DescriptorBlocks.Contains(0x60000) -and
                $stores[3].DescriptorBlocks.Contains(0x64000)) 'Longer interval omitted intermediate descriptors.'
        }
        'interval rejects missing and reversed endpoints' = {
            Set-IntervalFixture
            Read-Interval -B ([Guid]::Empty) -Failure
            Read-Interval -A $copy_c -B $copy_a -Failure
        }
        'interval rejects equal creation times at either boundary' = {
            Set-IntervalFixture
            Put-U64 (0x4080 + 48) $time_a
            Read-Interval -Failure
            Put-U64 (0x4080 + 48) ($time_b + [uint64]10000000)
            Read-Interval -Failure
        }
        'interval rejects a contributing store without in-volume data' = {
            Set-IntervalFixture
            [Array]::Clear($image, 0x4280, 128)
            Read-Interval -Failure
        }
        'interval rejects inconsistent volume geometry' = {
            Set-IntervalFixture
            Put-U64 (0x4080 + 8) ($image.Length - 512)
            Read-Interval -Failure
        }
    }
    foreach ($case in $cases.GetEnumerator()) {
        $image = [byte[]]$original.Clone()
        try {
            & $case.Value
            Write-Host "PASS $($case.Key)"
        } catch {
            $failures.Add("$($case.Key): $($_.Exception.Message)")
            Write-Host "FAIL $($failures[-1])"
        }
    }
} finally {
    try { $directory.Delete($true) }
    catch { Write-Warning "Could not remove test directory '$($directory.FullName)': $($_.Exception.Message)" }
}
if ($failures.Count -ne 0) { exit 1 }
