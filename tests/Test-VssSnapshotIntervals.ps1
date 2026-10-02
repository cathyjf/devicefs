# SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
# SPDX-License-Identifier: GPL-3.0-or-later

#requires -Version 7.4
#requires -RunAsAdministrator

<#
.SYNOPSIS
Investigates descriptor coverage across adjacent and nonadjacent VSS snapshots.

.DESCRIPTION
Runs three independent cases by default: A -> B -> C, A -> I1 -> B -> C,
and A -> I1 -> I2 -> I3 -> B -> C. Each case creates its own 1 GiB fixed VHD
and NTFS volume. A is the baseline and B contains the files to be backed up.
C freezes the earlier descriptor stores while the test reads them through B.
The fixture creates these snapshots using Win32_ShadowCopy's ClientAccessible
context. The filesystem verifier also uses the DeviceFs VSS requester to
create a temporary successor while constructing its dirty map.

A separate, already allocated 12 MiB file is overwritten between each pair
of snapshots from A to B. Another file, hot.bin, receives repeated 512-byte
writes starting 256 bytes before file offset 16 KiB. The test checks the
files' expected contents and unchanged allocation in A and B. It then checks
whether the descriptors from A and the intermediate stores cover the changed
blocks in the 12 MiB files.

To demonstrate why intermediate stores are needed, the test counts changed
blocks covered by intermediate descriptors but absent from A's descriptors,
the allocation-change map, and System Volume Information extents. A nonadjacent
case with no such blocks is reported as nondiscriminating: that case cannot
distinguish a complete interval lookup from the old lookup of A alone.

The test also records raw B's catalog while latest, twice while nonlatest,
and the live volume catalog. Records retain catalog order, exact copy/store
IDs, raw creation FILETIMEs, and metadata offsets. VSS properties are recorded
separately so catalog times can be compared with the provider's public output.

Failures are collected per check. Independent checks and subsequent cases
continue; unavailable prerequisites produce explicit skips. Cleanup happens
after each case's available checks have finished. JSON summaries, CSV witness
coverage, a console transcript, and each tool's output remain in a new report
directory whose path is printed at startup, unless -RemoveTemporaryFiles is supplied.
Only snapshots created by this
invocation are deleted. Detached images are removed unless -KeepImages is supplied.

Each case exposes the production reconstruction of B and the corresponding
real-B VHDX, copies both files into Windows SystemTemp, and stops the exposure
process before attaching the copies read-only. The supervisor compares the two
mounted filesystems with its existing verifier. This separates attachment and
comparison from the DeviceFs processes serving the projected images. Content
selection defaults to 100 percent; namespace traversal still covers both complete
filesystems. Optional raw comparison reports block differences separately
because VSS does not guarantee equality of every raw block.

.PARAMETER SupervisorPath
Defaults to the native-architecture Release build. The suite uses its
--vss-descriptor-dump, --expose-synthetic-backup, and --verify-filesystems modes.

.PARAMETER VerificationPercentage
Percentage of stream-content chunks selected for comparison. Defaults to 100.

.PARAMETER VShadowInfoPath
Optional stock libvshadow 20260714 vshadowinfo executable. Compares each store's
descriptor output with DeviceFs using the same captured image of B. DeviceFs's
output from that image is also compared with its earlier output from B's device.

.PARAMETER IntermediateCounts
Numbers of intermediate snapshots to test. Defaults to 0, 1, 3.

.PARAMETER Repetitions
Repeat the complete case matrix to gather evidence about run-to-run variation.

.PARAMETER CompareRawImages
Also run the raw image comparison on each exact A/B interval. Differences are
recorded as diagnostic information separately from filesystem equivalence.

.PARAMETER OutputDirectory
Parent for a newly created, uniquely named report directory. Defaults to the
Windows `SystemTemp` directory.

.PARAMETER KeepImages
Retains the case's VHD, copied VHDX files, and any captured raw image after detaching.

.PARAMETER RemoveTemporaryFiles
Removes the report directory after the cases finish and their images are detached.
CTest supplies this option to avoid leaving temporary reports behind.

.EXAMPLE
pwsh -NoProfile -File .\tests\Test-VssSnapshotIntervals.ps1

.EXAMPLE
pwsh -NoProfile -File .\tests\Test-VssSnapshotIntervals.ps1 -Repetitions 3
#>
[CmdletBinding()]
param(
    [string] $SupervisorPath,
    [string] $VShadowInfoPath,
    [ValidateRange(0, 8)][int[]] $IntermediateCounts = @(0, 1, 3),
    [ValidateRange(1, 100)][int] $Repetitions = 1,
    [ValidateRange(1, 100)][int] $VerificationPercentage = 100,
    [string] $OutputDirectory = (Join-Path $env:WINDIR 'SystemTemp'),
    [switch] $CompareRawImages,
    [switch] $KeepImages,
    [switch] $RemoveTemporaryFiles
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
if ($KeepImages -and $RemoveTemporaryFiles) {
    throw '-KeepImages and -RemoveTemporaryFiles cannot be used together.'
}
. (Join-Path $PSScriptRoot 'include/DeviceFsTestProcess.ps1')
. (Join-Path $PSScriptRoot 'include/DeviceFsTestVolume.ps1')
. (Join-Path $PSScriptRoot 'include/DeviceFsTestTypes.ps1')
. (Join-Path $PSScriptRoot 'include/VssTestWorkload.ps1')
. (Join-Path $PSScriptRoot 'include/VssDescriptorOutput.ps1')

function Assert-Condition {
    param([bool] $Condition, [string] $Message)
    if (-not $Condition) { throw $Message }
}

if (-not $SupervisorPath) {
    $SupervisorPath = Get-DefaultTestExecutablePath 'backup-supervisor.exe'
}
$SupervisorPath = (Resolve-Path -LiteralPath $SupervisorPath).Path
if ($VShadowInfoPath) {
    $VShadowInfoPath = (Resolve-Path -LiteralPath $VShadowInfoPath).Path
}
if ($null -ne ([Management.Automation.PSTypeName]'DeviceFsTestNative').Type) {
    throw 'DeviceFsTestNative is already loaded. Run the suite in a fresh pwsh process.'
}
Add-DeviceFsTestTypes -SourcePath (Join-Path $PSScriptRoot 'types/DeviceFsTestNative.cs') -SupervisorPath $SupervisorPath
$root = (New-Item -ItemType Directory -Path (Join-Path $OutputDirectory (
    'devicefs-vss-intervals-' + [Guid]::NewGuid().ToString('N')))).FullName
Start-Transcript -LiteralPath (Join-Path $root 'console.txt') | Out-Null
Write-Host "VSS investigation reports: $root"

function Invoke-IntervalCase {
    param([int] $IntermediateCount, [int] $Repetition, [int] $CaseNumber)

    $directory = (New-Item -ItemType Directory -Path (Join-Path $root "case-$CaseNumber")).FullName
    $checks = [Collections.Generic.List[object]]::new()
    $snapshots = [Collections.Generic.List[object]]::new()
    $snapshot_ids = [Collections.Generic.List[Guid]]::new()
    $cleanup_errors = [Collections.Generic.List[string]]::new()
    $report = [ordered]@{
        SchemaVersion = 1; IntermediateCount = $IntermediateCount; Repetition = $Repetition
        StartedUtc = [DateTime]::UtcNow.ToString('O'); Verdict = 'Incomplete'
        Checks = $checks; Snapshots = $snapshots; Catalogs = [ordered]@{}
        Stores = [Collections.Generic.List[object]]::new()
        Witnesses = [Collections.Generic.List[object]]::new(); CleanupErrors = $cleanup_errors
    }
    $vhd = Join-Path $directory 'fixture.vhd'
    $mount = Join-Path $directory 'volume'
    $partition = $null
    $mounted = $false
    $copied_directory = Join-Path (Join-Path $env:WINDIR 'SystemTemp') (
        'devicefs-vss-copies-' + [Guid]::NewGuid().ToString('N'))
    $owned_copy_directories = [Collections.Generic.List[string]]::new()
    $copied_images = [Collections.Generic.List[string]]::new()
    $attachments = @{}
    $exposure = [ordered]@{
        Process = $null
        Children = @()
    }
    $block_size = 16KB
    $bulk_size = 12MB

    # A failed check is recorded and returns null so independent checks can
    # continue. Checks that need its result use SkipReason to explain why they
    # cannot run.
    function Invoke-Check {
        param([string] $Name, [scriptblock] $Operation, [string] $SkipReason,
            [switch] $Informational, [string] $ExpectedFailure)
        if ($SkipReason) {
            $checks.Add([pscustomobject]@{ Name = $Name; Status = 'Skipped'; Detail = $SkipReason })
            Write-Host "SKIP $Name`: $SkipReason"
            return $null
        }
        try {
            Write-Host "START $Name"
            $value = & $Operation
            $checks.Add([pscustomobject]@{ Name = $Name; Status = 'Passed'; Detail = $null })
            Write-Host "PASS $Name"
            return ,$value
        } catch {
            $status = if ($Informational) { 'Unavailable' } else { 'Failed' }
            $checks.Add([pscustomobject]@{ Name = $Name; Status = $status; Detail = $_.Exception.Message })
            if ($ExpectedFailure) {
                Write-Host (
                    "INFO $Name`: expected failure $ExpectedFailure; " +
                    "parser reported: $($_.Exception.Message)")
            } else {
                Write-Host "$status $Name`: $($_.Exception.Message)"
            }
            return $null
        }
    }

    function Read-Dump {
        param([string] $Name, [string] $Source, [string[]] $Options)
        $stdout = Join-Path $directory "$Name.stdout.txt"
        $stderr = Join-Path $directory "$Name.stderr.txt"
        & $SupervisorPath --vss-descriptor-dump --source $Source @Options >$stdout 2>$stderr
        $code = $LASTEXITCODE
        Assert-Condition ($code -eq 0) "$Name exited $code`: $([IO.File]::ReadAllText($stderr).Trim())"
        return [IO.File]::ReadAllText($stdout)
    }

    function Stop-Exposure {
        if ($null -eq $exposure.Process) { return }
        $process = $exposure.Process
        try {
            if (-not $process.HasExited) {
                [DeviceFsTestNative]::SendCtrlC()
                Assert-Condition ($process.WaitForExit(30000)) 'The backup exposure process did not stop within 30 seconds.'
            }
            # The foreground supervisor returns 130 after Ctrl+C cancellation.
            Assert-Condition ($process.ExitCode -eq 130) "The backup exposure process exited with code $($process.ExitCode)."
            foreach ($child in $exposure.Children) {
                Assert-Condition $child.HasExited "DeviceFs child $($child.Id) remained alive after exposure stopped."
            }
        } finally {
            # Exposure has no attached VHDX files. If ordinary stopping fails,
            # all surviving processes must be terminated before another case.
            if (-not $process.HasExited) {
                $process.Kill($true)
                Assert-Condition ($process.WaitForExit(5000)) 'The backup exposure process remained alive after termination.'
            }
            foreach ($child in $exposure.Children) {
                if (-not $child.HasExited) {
                    $child.Kill()
                    Assert-Condition ($child.WaitForExit(5000)) "DeviceFs child $($child.Id) remained alive after termination."
                }
            }
        }
    }

    function Read-Catalog {
        param([string] $Name, [string] $Source, [switch] $Required,
            [string] $SkipReason, [string] $ExpectedFailure)
        $result = Invoke-Check $Name {
            ConvertFrom-VssCatalogOutput (Read-Dump $Name $Source @('--catalog'))
        } -Informational:(-not $Required) -SkipReason $SkipReason `
            -ExpectedFailure $ExpectedFailure
        $report.Catalogs[$Name] = $result
        return ,$result
    }

    function Add-Snapshot {
        param([string] $Name)
        Write-Host "Creating snapshot $Name"
        $snapshot = New-TestShadowCopy $volume_name $snapshot_ids
        Assert-Condition (([Guid]$snapshot.ProviderID -eq
            [Guid]'b5946137-7b9f-4925-af80-51abd60b20d5') -and $snapshot.Differential) `
            "$Name was not created by the Microsoft differential provider."
        $entry = [pscustomobject]@{
            Name = $Name; CopyId = [Guid]$snapshot.ID; SetId = [Guid]$snapshot.SetID
            Device = $snapshot.DeviceObject; ProviderId = [Guid]$snapshot.ProviderID
            CreationFileTime = $snapshot.InstallDate.ToUniversalTime().ToFileTimeUtc()
            CreationUtc = $snapshot.InstallDate.ToUniversalTime().ToString('O')
            Context = 'ClientAccessible (Win32_ShadowCopy)'
        }
        $snapshots.Add($entry)
        Write-Host "Created $Name $($entry.CopyId) at $($entry.CreationUtc)"
        return $entry
    }

    try {
        Write-Host "Case $CaseNumber`: $IntermediateCount intermediate snapshot(s), repetition $Repetition"
        New-Item -ItemType Directory -Path $mount | Out-Null
        $partition = New-DeviceFsTestVolume -Path $vhd -SizeBytes 1GB -ClusterSize 4096 `
            -Label ('DFSVSS-' + [Guid]::NewGuid().ToString('N').Substring(0, 17)) -Fixed
        $partition.Mount($mount)
        $mounted = $true
        $volume_name = [DeviceFsTestNative]::GetVolumeName($mount)
        $identity = [DeviceFsTestNative]::InspectVolume($volume_name)
        Assert-Condition (($identity.DiskNumber -eq $partition.DiskNumber) -and
            ($identity.DiskStartingOffset -eq $partition.Offset) -and
            ($identity.DiskExtentLength -eq $partition.Size)) 'The fixture volume did not map to its new VHD.'
        $report['Volume'] = $volume_name
        $report['VolumeSize'] = $identity.Length
        Initialize-TestShadowStorage -VolumeName $volume_name

        $witnesses = @(for ($i = 0; $i -le $IntermediateCount; ++$i) {
            [pscustomobject]@{ File = "interval-$i.bin"; Before = [byte](0x31 + $i); After = [byte](0xA5 + $i) }
        })
        foreach ($witness in $witnesses) {
            Write-FilePattern (Join-Path $mount $witness.File) 0 $bulk_size $witness.Before -CreateNew
        }
        Write-FilePattern (Join-Path $mount 'hot.bin') 0 (2 * $block_size) 0x11 -CreateNew
        Write-FilePattern (Join-Path $mount 'unchanged.bin') 0 $block_size 0x52 -CreateNew
        Write-FilePattern (Join-Path $mount 'after-b.bin') 0 $bulk_size 0x63 -CreateNew
        $a = Add-Snapshot 'A'
        for ($i = 0; $i -le $IntermediateCount; ++$i) {
            Write-FilePattern (Join-Path $mount $witnesses[$i].File) 0 $bulk_size $witnesses[$i].After
            Write-FilePattern (Join-Path $mount 'hot.bin') ($block_size - 256) 512 ([byte](0xD0 + $i))
            if ($i -lt $IntermediateCount) { $null = Add-Snapshot "I$($i + 1)" }
        }
        $b = Add-Snapshot 'B'
        $null = Read-Catalog 'catalog-b-latest' $b.Device `
            -ExpectedFailure 'reading raw B while it is the latest snapshot'
        Write-FilePattern (Join-Path $mount 'after-b.bin') 0 $bulk_size 0xE5
        $successor = Invoke-Check 'create successor C' { Add-Snapshot 'C' }
        $successor_skip_reason = if ($null -eq $successor) {
            'Successor C is unavailable, so the descriptor stores have not been frozen.'
        }
        $before_catalog = Read-Catalog 'catalog-b-before' $b.Device -Required -SkipReason $successor_skip_reason
        $null = Read-Catalog 'catalog-live' $volume_name

        $interval_stores = Invoke-Check 'production multistore lookup' {
            $text = Read-Dump 'interval' $b.Device @('--baseline-id', $a.CopyId.ToString('D'),
                '--snapshot-id', $b.CopyId.ToString('D'))
            ConvertFrom-VssIntervalOutput $text $a.CopyId $b.CopyId $identity.Length $block_size
        } -SkipReason $successor_skip_reason

        $null = Invoke-Check 'exact interval and catalog chronology' {
            $ordered = @(foreach ($snapshot in $snapshots | Where-Object { $_.Name -ne 'C' }) {
                $matches = @($before_catalog | Where-Object { $_.CopyId -eq $snapshot.CopyId })
                Assert-Condition ($matches.Count -eq 1) "Raw B did not expose exactly one store for $($snapshot.Name) $($snapshot.CopyId)."
                Assert-Condition $matches[0].InVolume "Store for $($snapshot.Name) had no in-volume data."
                $matches[0]
            })
            for ($i = 1; $i -lt $ordered.Count; ++$i) {
                Assert-Condition ($ordered[$i - 1].CreationFileTime -lt $ordered[$i].CreationFileTime) `
                    'Exact snapshots did not have strictly increasing catalog creation times.'
            }
            $between = @($before_catalog | Where-Object {
                ($_.CreationFileTime -ge $ordered[0].CreationFileTime) -and
                ($_.CreationFileTime -lt $ordered[-1].CreationFileTime)
            })
            Assert-Condition ($between.Count -eq $IntermediateCount + 1) `
                'The observed A-inclusive, B-exclusive interval contained unexpected stores.'
        } -SkipReason $(if ($null -eq $before_catalog) { 'Raw B catalog is unavailable.' })

        $stores = @{}
        foreach ($snapshot in $snapshots | Where-Object { $_.Name -ne 'C' }) {
            $store = Invoke-Check "descriptors $($snapshot.Name) through raw B" {
                $text = Read-Dump "descriptors-$($snapshot.Name)" $b.Device @('--snapshot-id', $snapshot.CopyId.ToString('D'))
                ConvertFrom-VssDescriptorDumpOutput $text $snapshot.CopyId 0 1 2 $identity.Length $block_size
            } -Informational:($snapshot.Name -eq 'B') -SkipReason $successor_skip_reason
            if ($null -ne $store) {
                $stores[$snapshot.Name] = $store
                $report.Stores.Add([pscustomobject]@{
                    Name = $snapshot.Name; CopyId = $snapshot.CopyId; StoreId = $store.StoreId
                    Descriptors = $store.DescriptorCount; ListBlocks = $store.ListBlockCount
                    ProjectedBlocks = $store.DescriptorBlocks.Count
                })
            }
        }
        $null = Read-Catalog 'catalog-b-after' $b.Device -SkipReason $successor_skip_reason

        $bitmap_a = Invoke-Check 'allocation bitmap A' { [DeviceFsTestNative]::GetAllocationBitmap($a.Device, $true) }
        $bitmap_b = Invoke-Check 'allocation bitmap B' { [DeviceFsTestNative]::GetAllocationBitmap($b.Device, $true) }
        $allocation = Invoke-Check 'endpoint allocation changes' {
            [DeviceFsTestNative]::GetAllocationChangeBlocks($bitmap_a, $bitmap_b, $block_size)
        } -SkipReason $(if (($null -eq $bitmap_a) -or ($null -eq $bitmap_b)) { 'An endpoint allocation bitmap is unavailable.' })
        $svi = @{}
        $privilege = Invoke-Check 'enable backup privilege for SVI reads' {
            [DeviceFsTestNative]::EnableBackupPrivilege()
        }
        try {
            foreach ($snapshot in @($a, $b)) {
                $svi[$snapshot.Name] = Invoke-Check "SVI extents $($snapshot.Name)" {
                    $text = Read-Dump "svi-$($snapshot.Name)" $snapshot.Device @('--svi-extents')
                    ConvertFrom-SviExtentDumpOutput $text $block_size
                } -SkipReason $(if ($null -eq $privilege) { 'Backup privilege is unavailable.' })
            }
        } finally {
            if ($null -ne $privilege) {
                $null = Invoke-Check 'restore backup privilege' { $privilege.Dispose() }
            }
        }
        $other = Invoke-Check 'independent allocation and SVI evidence' {
            $offsets = [Collections.Generic.HashSet[long]]::new()
            $offsets.UnionWith([long[]]$allocation.BecameAllocated)
            $offsets.UnionWith([long[]]$allocation.BecameFree)
            $offsets.UnionWith([long[]]@($svi.A))
            $offsets.UnionWith([long[]]@($svi.B))
            return ,$offsets
        } -SkipReason $(if (($null -eq $allocation) -or ($null -eq $svi.A) -or ($null -eq $svi.B)) { 'Allocation or SVI evidence is unavailable.' })

        $contributors = @($snapshots | Where-Object { ($_.Name -ne 'B') -and ($_.Name -ne 'C') })
        $null = Invoke-Check 'interval lookup agrees with per-store evidence' {
            Assert-Condition ($interval_stores.Count -eq $contributors.Count) 'Interval returned the wrong number of stores.'
            for ($i = 0; $i -lt $contributors.Count; ++$i) {
                $snapshot = $contributors[$i]
                Assert-Condition ($interval_stores[$i].CopyId -eq $snapshot.CopyId) 'Interval returned the wrong store order.'
                Assert-Condition ($stores.ContainsKey($snapshot.Name)) "Store $($snapshot.Name) is unavailable."
                $differences = @(Compare-VssDescriptorStores $stores[$snapshot.Name] $interval_stores[$i])
                Assert-Condition ($differences.Count -eq 0) ($differences -join '; ')
            }
        } -SkipReason $(if ($null -eq $interval_stores) { 'Production interval lookup failed.' })
        $union = Invoke-Check 'complete descriptor union' {
            $offsets = [Collections.Generic.HashSet[long]]::new()
            foreach ($store in $interval_stores) {
                $offsets.UnionWith($store.DescriptorBlocks)
            }
            return ,$offsets
        } -SkipReason $(if ($null -eq $interval_stores) { 'Production interval lookup failed.' })
        $coverage = [Collections.Generic.List[object]]::new()
        $file_cases = @($witnesses | ForEach-Object {
            [pscustomobject]@{ File = $_.File; Length = $bulk_size; Before = $_.Before; After = $_.After; Hot = $false }
        }) + @(
            [pscustomobject]@{ File = 'hot.bin'; Length = 2 * $block_size; Before = [byte]0x11; After = [byte]0x11; Hot = $true },
            [pscustomobject]@{ File = 'unchanged.bin'; Length = $block_size; Before = [byte]0x52; After = [byte]0x52; Hot = $false },
            [pscustomobject]@{ File = 'after-b.bin'; Length = $bulk_size; Before = [byte]0x63; After = [byte]0x63; Hot = $false }
        )
        foreach ($file in $file_cases) {
            $paths = @("$($a.Device)\$($file.File)", "$($b.Device)\$($file.File)")
            $valid = @($null, $null)
            for ($endpoint = 0; $endpoint -lt $paths.Count; ++$endpoint) {
                $valid[$endpoint] = Invoke-Check "contents $($file.File) at $(@('A','B')[$endpoint])" {
                    $expected = [byte[]]::new($file.Length)
                    [Array]::Fill[byte]($expected, @($file.Before, $file.After)[$endpoint])
                    if ($file.Hot -and ($endpoint -eq 1)) {
                        for ($j = $block_size - 256; $j -lt $block_size + 256; ++$j) {
                            $expected[$j] = [byte](0xD0 + $IntermediateCount)
                        }
                    }
                    Assert-Condition ([IO.FileInfo]::new($paths[$endpoint]).Length -eq $file.Length) 'Witness file length changed.'
                    $actual = Read-FileRange $paths[$endpoint] 0 $file.Length
                    Assert-Condition (
                        [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($expected)) -ceq
                        [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($actual))) `
                        "File '$($paths[$endpoint])' did not contain its intended complete pattern."
                    return $true
                }
            }
            if (($file.Before -eq $file.After) -and (-not $file.Hot)) { continue }
            $blocks = Invoke-Check "stable allocation $($file.File)" {
                $blocks_a = Get-ObjectBlockOffsets $paths[0] $bitmap_a $block_size $false
                $blocks_b = Get-ObjectBlockOffsets $paths[1] $bitmap_b $block_size $false
                Assert-Condition ($blocks_a.SetEquals($blocks_b)) "File $($file.File) moved between endpoints."
                return ,$blocks_a
            } -SkipReason $(if (($null -eq $bitmap_a) -or ($null -eq $bitmap_b)) { 'An endpoint allocation bitmap is unavailable.' })
            # Only 512 bytes of hot.bin change. Requiring every block in its
            # file extents to appear in the dirty map would also require
            # unchanged blocks. Its expected contents were checked above.
            if ($file.Hot) { continue }
            $null = Invoke-Check "coverage $($file.File)" {
                $eligible = 0; $missing = 0; $exclusive = 0
                foreach ($offset in $blocks) {
                    if ($other.Contains($offset)) { continue }
                    ++$eligible
                    $in_a = $stores.A.DescriptorBlocks.Contains($offset)
                    $in_union = $union.Contains($offset)
                    if (-not $in_union) { ++$missing }
                    if ((-not $in_a) -and $in_union) { ++$exclusive }
                    $coverage.Add([pscustomobject]@{
                        File = $file.File; Offset = $offset; BaselineStore = $in_a; IntervalUnion = $in_union
                        Contributors = (@($interval_stores | Where-Object {
                            $_.DescriptorBlocks.Contains($offset)
                        } | ForEach-Object CopyId) -join ',')
                    })
                }
                $report.Witnesses.Add([pscustomobject]@{
                    File = $file.File; EligibleBlocks = $eligible; UnionMissing = $missing; IntermediateOnly = $exclusive
                })
                # More than 8 MiB of the 12 MiB overwrite must depend on
                # descriptor coverage. This allows some overlap with allocation
                # and SVI evidence without letting those terms cover most of
                # the file and conceal missing descriptors.
                Assert-Condition (($eligible * $block_size) -gt 8MB) "Too few descriptor-only witness blocks in $($file.File)."
                Assert-Condition ($missing -eq 0) "Interval union missed $missing blocks of $($file.File)."
            } -SkipReason $(if (($null -eq $valid[0]) -or ($null -eq $valid[1]) -or
                ($null -eq $blocks) -or ($null -eq $other) -or ($null -eq $union) -or
                (-not $stores.ContainsKey('A'))) { 'Endpoint contents, stable allocation, or complete map evidence is unavailable.' })
        }
        $null = Invoke-Check 'write witness coverage report' {
            $coverage | Export-Csv -LiteralPath (Join-Path $directory 'witness-coverage.csv') -NoTypeInformation
        }

        if ($VShadowInfoPath) {
            $flat = Join-Path $directory 'payload.img'
            $captured = Invoke-Check 'capture raw B for parser parity' {
                [DeviceFsTestNative]::CopyDeviceToFile($b.Device, $flat)
                return $true
            } -SkipReason $successor_skip_reason
            $reference = Invoke-Check 'vshadowinfo on captured B' {
                $stdout = Join-Path $directory 'vshadowinfo.stdout.txt'
                $stderr = Join-Path $directory 'vshadowinfo.stderr.txt'
                & $VShadowInfoPath -a $flat >$stdout 2>$stderr
                Assert-Condition ($LASTEXITCODE -eq 0) "vshadowinfo failed: $([IO.File]::ReadAllText($stderr))"
                ConvertFrom-VshadowInfoDescriptorOutput ([IO.File]::ReadAllText($stdout)) 1 2
            } -SkipReason $(if ($null -eq $captured) { 'Raw B capture failed.' })
            foreach ($snapshot in $contributors) {
                $actual = Invoke-Check "parse captured store $($snapshot.Name)" {
                    $id = $snapshot.CopyId.ToString('D')
                    $text = Read-Dump "flat-$($snapshot.Name)" $flat @('--snapshot-id', $id)
                    ConvertFrom-VssDescriptorDumpOutput $text $snapshot.CopyId 0 1 2 $identity.Length $block_size
                } -SkipReason $(if ($null -eq $captured) { 'Raw B capture failed.' })
                $null = Invoke-Check "parser parity $($snapshot.Name)" {
                    $id = $snapshot.CopyId.ToString('D')
                    Assert-Condition ($reference.ContainsKey($id)) "vshadowinfo did not expose $id."
                    $differences = @(Compare-VssDescriptorStores $reference[$id] $actual)
                    Assert-Condition ($differences.Count -eq 0) ($differences -join '; ')
                } -SkipReason $(if (($null -eq $reference) -or ($null -eq $actual)) { 'A parser parity input is unavailable.' })
                $null = Invoke-Check "raw and captured store $($snapshot.Name)" {
                    $differences = @(Compare-VssDescriptorStores $stores[$snapshot.Name] $actual)
                    Assert-Condition ($differences.Count -eq 0) ($differences -join '; ')
                } -SkipReason $(if (($null -eq $actual) -or (-not $stores.ContainsKey($snapshot.Name))) { 'A raw or captured store result is unavailable.' })
            }
        }
        $null = Invoke-Check 'payload outside selected volume is rejected' {
            # The report directory is on the host filesystem, outside the
            # mounted fixture volume. Selecting it must exclude B.
            $stdout = Join-Path $directory 'excluded-payload.stdout.txt'
            $stderr = Join-Path $directory 'excluded-payload.stderr.txt'
            & $SupervisorPath --incremental-stats --baseline $a.CopyId.ToString('D') `
                --payload $b.CopyId.ToString('D') --volume $directory >$stdout 2>$stderr
            Assert-Condition ($LASTEXITCODE -ne 0) 'A payload outside the selected volume was accepted.'
            Assert-Condition ([IO.File]::ReadAllText($stderr).Contains('not among the selected volumes')) `
                "Volume restriction did not produce the expected diagnostic; see '$stderr'."
        }
        $copies_ready = Invoke-Check 'copy the production multistore images' {
            $owned_copy_directories.Add((New-Item -ItemType Directory -Path $copied_directory).FullName)
            $view_root = Join-Path $directory 'projected-images'
            $filename = 'volume-' + ([Guid]$volume_name.Substring(10, 38)).ToString('D') + '.vhdx'
            $start_info = [Diagnostics.ProcessStartInfo]::new()
            $start_info.FileName = $SupervisorPath
            $start_info.UseShellExecute = $false
            # The child inherits the console so its setup and error messages
            # remain visible while the harness waits for the projected files.
            foreach ($argument in @('--expose-synthetic-backup', $view_root,
                    '--baseline', $a.CopyId.ToString('D'),
                    '--payload', $b.CopyId.ToString('D'), '--volume', $mount)) {
                $start_info.ArgumentList.Add($argument)
            }
            $exposure.Process = [Diagnostics.Process]::Start($start_info)
            $deadline = [Environment]::TickCount64 + 30000
            while ((-not [IO.File]::Exists((Join-Path $view_root "synthetic/$filename"))) -or
                (-not [IO.File]::Exists((Join-Path $view_root "real/$filename")))) {
                Assert-Condition (-not $exposure.Process.HasExited) 'The backup exposure process exited before its images were available.'
                Assert-Condition ([Environment]::TickCount64 -lt $deadline) 'The projected images did not become available within 30 seconds.'
                Start-Sleep -Milliseconds 100
            }
            $exposure.Children = @(Get-CimInstance -ClassName Win32_Process `
                -Filter "ParentProcessId = $($exposure.Process.Id)" | ForEach-Object {
                    [Diagnostics.Process]::GetProcessById([int]$_.ProcessId)
                })
            foreach ($view in 'synthetic', 'real') {
                $target = Join-Path $copied_directory "$view.vhdx"
                Write-Host "Copying $view VHDX to $target"
                [IO.File]::Copy((Join-Path $view_root "$view/$filename"), $target)
                $copied_images.Add($target)
            }
            return $true
        }
        $exposure_stopped = Invoke-Check 'stop backup image exposure' {
            Stop-Exposure
            return $true
        } -SkipReason $(if ($null -eq $exposure.Process) { 'Exposure did not start.' })
        $mounted_roots = @{}
        try {
            foreach ($view in 'synthetic', 'real') {
                $mounted_roots[$view] = Invoke-Check "attach the copied $view VHDX read-only" {
                    $image = Join-Path $copied_directory "$view.vhdx"
                    Write-Host "Attaching $image read-only"
                    $attachments[$image] = [DeviceFsTestDisk]::Open($image)
                    return $attachments[$image].VolumeName
                } -SkipReason $(if ((-not $copies_ready) -or (-not $exposure_stopped)) {
                    'Both images must be copied and exposure must stop before attachment.'
                })
            }
            $null = Invoke-Check 'filesystem equivalence of the production multistore image' {
                $log = Join-Path $directory 'filesystem-verifier.log'
                & $SupervisorPath --verify-filesystems $mounted_roots['synthetic'] $mounted_roots['real'] `
                    --verify-percentage $VerificationPercentage 2>&1 | Tee-Object -FilePath $log | Out-Host
                Assert-Condition ($LASTEXITCODE -eq 0) "Filesystem verification failed; see '$log'."
            } -SkipReason $(if ((-not $mounted_roots['synthetic']) -or (-not $mounted_roots['real'])) {
                'Both copied filesystems must be mounted before comparison.'
            })
        } finally {
            foreach ($image in $copied_images) {
                $null = Invoke-Check "detach $image" {
                    if ($attachments.ContainsKey($image)) {
                        $attachments[$image].Detach()
                    }
                }
            }
        }
        if ($CompareRawImages) {
            $null = Invoke-Check 'raw image comparison (diagnostic)' {
                $log = Join-Path $directory 'raw-verifier.log'
                & $SupervisorPath --incremental-verify --baseline $a.CopyId.ToString('D') `
                    --payload $b.CopyId.ToString('D') 2>&1 | Tee-Object -FilePath $log | Out-Host
                Assert-Condition ($LASTEXITCODE -eq 0) "Raw comparison reported differences or an error; see '$log'."
            } -Informational
        }
        $report.Verdict = if (@($checks | Where-Object Status -eq 'Failed').Count) {
            'Failed'
        } elseif (($IntermediateCount -gt 0) -and
            (($report.Witnesses | Measure-Object IntermediateOnly -Sum).Sum -eq 0)) {
            'Nondiscriminating'
        } else { 'Passed' }
    } catch {
        $checks.Add([pscustomobject]@{ Name = 'fixture'; Status = 'Failed'; Detail = $_.Exception.Message })
        $report.Verdict = 'Failed'
        Write-Host "FAIL case $CaseNumber fixture: $($_.Exception.Message)"
    } finally {
        if ($null -ne $exposure.Process) {
            if (-not $exposure.Process.HasExited) {
                try { Stop-Exposure } catch { $cleanup_errors.Add($_.Exception.Message) }
            }
            foreach ($child in $exposure.Children) { $child.Dispose() }
            $exposure.Process.Dispose()
        }
        foreach ($copies in $owned_copy_directories) {
            if ($KeepImages) {
                Write-Host "Copied VHDX files retained at $copies"
                continue
            }
            try {
                # Only the uniquely named directory created by this case is removed.
                $absolute = [IO.Path]::GetFullPath($copies)
                Assert-Condition ([IO.Path]::GetDirectoryName($absolute) -eq
                    [IO.Path]::GetFullPath((Join-Path $env:WINDIR 'SystemTemp'))) `
                    "Copied-image directory '$absolute' is outside SystemTemp."
                # File.Copy preserves the projected VHDX files' read-only
                # attribute, so deleting the copies requires -Force.
                Remove-Item -LiteralPath $absolute -Recurse -Force
            } catch { $cleanup_errors.Add($_.Exception.Message) }
        }
        for ($i = $snapshot_ids.Count - 1; $i -ge 0; --$i) {
            $id = $snapshot_ids[$i]
            try {
                Get-CimInstance -ClassName Win32_ShadowCopy |
                    Where-Object { [Guid]$_.ID -eq $id } | Remove-CimInstance
            } catch { $cleanup_errors.Add($_.Exception.Message) }
        }
        if ($mounted) {
            try { $partition.Unmount($mount) }
            catch { $cleanup_errors.Add($_.Exception.Message) }
        }
        if ([IO.File]::Exists($vhd)) {
            try {
                if ($null -ne $partition) { $partition.Detach() }
                if (-not $KeepImages) {
                    Remove-Item -LiteralPath $vhd
                }
            } catch { $cleanup_errors.Add($_.Exception.Message) }
        }
        if ((-not $KeepImages) -and [IO.File]::Exists((Join-Path $directory 'payload.img'))) {
            try { Remove-Item -LiteralPath (Join-Path $directory 'payload.img') }
            catch { $cleanup_errors.Add($_.Exception.Message) }
        }
        $report['FinishedUtc'] = [DateTime]::UtcNow.ToString('O')
        $report | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath (Join-Path $directory 'summary.json') -Encoding utf8
        foreach ($message in $cleanup_errors) { Write-Warning "Case $CaseNumber cleanup: $message" }
    }
    Write-Host "Case $CaseNumber result: $($report.Verdict)"
    return [pscustomobject]@{
        Case = $CaseNumber; IntermediateCount = $IntermediateCount; Repetition = $Repetition
        Verdict = $report.Verdict; CleanupErrors = $cleanup_errors.ToArray(); Report = (Join-Path $directory 'summary.json')
    }
}

$results = [Collections.Generic.List[object]]::new()
for ($repetition = 1; $repetition -le $Repetitions; ++$repetition) {
    foreach ($count in $IntermediateCounts) {
        $case_number = $results.Count + 1
        try {
            $results.Add((Invoke-IntervalCase $count $repetition $case_number))
        } catch {
            Write-Warning "Case $case_number could not finish its report: $($_.Exception.Message)"
            $results.Add([pscustomobject]@{
                Case = $case_number; IntermediateCount = $count; Repetition = $repetition
                Verdict = 'Failed'; CleanupErrors = @(); Report = $_.Exception.Message
            })
        }
        $results | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $root 'summary.json') -Encoding utf8
    }
}
$results | Format-Table Case, IntermediateCount, Repetition, Verdict
Stop-Transcript | Out-Null
if ($RemoveTemporaryFiles) {
    Remove-Item -LiteralPath $root -Recurse -Force
} else {
    Write-Host "Reports retained at $root"
}
if (@($results | Where-Object { ($_.Verdict -ne 'Passed') -or ($_.CleanupErrors.Count -ne 0) }).Count) { exit 1 }
