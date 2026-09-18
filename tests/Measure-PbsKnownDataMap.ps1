# SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
# SPDX-License-Identifier: GPL-3.0-or-later

#requires -Version 7.4
#requires -RunAsAdministrator
#requires -Modules Hyper-V

<#
.SYNOPSIS
Compare PBS backup throughput with and without known-data maps.

.DESCRIPTION
Create disposable, mostly empty NTFS volumes in dynamic VHDX files and expose
them through DeviceFs at the Windows mount that the installed WSL distribution uses.
The filesystem uses the same caching and synthetic-zero options as a backup.
The supervisor supplies the installed PBS configuration and encryption key.
No WSL administration or separately supplied PBS credentials are needed.

Component 1 measures the effects of exposing bitmap files, using the map reader,
and skipping known-zero reads. It runs four cases with both serial and parallel
image uploading:
  `hidden`:   DeviceFs does not expose a bitmap, and PBS reads the image normally.
  `unused`:   DeviceFs exposes the bitmap, but PBS still reads the image normally.
  `all-data`: PBS uses an all-set map, so the map reader reads every chunk.
  `known`:    PBS uses the actual DeviceFs map and skips known-zero chunks.
Every case backs up the same images, including their NTFS metadata. The all-set
map is conservative; an all-clear map would incorrectly discard that metadata.
A small companion volume gives the parallel uploader another image to finish
while the large volume continues through its zero ranges.

A preliminary backup supplies a previous snapshot for chunk reuse. It is
excluded from the comparisons. Subsequent passes alternate forward and reverse
case order. The CSV retains each measurement; the summary reports its range
as well as its median. PBS's own duration excludes Windows and WSL startup.
The individual logs also retain PBS progress and Fish's wall and CPU timings.

Compare `hidden` with `unused` to isolate exposing the filesystem bitmap. Compare
`unused` with `all-data` to isolate the map reader while retaining source
reads. Compare `all-data` with `known` to see what changes when reads are skipped.
The serial and parallel results separate the two upload implementations.

Component 2 compares the PBS map reader's buffer allocation methods with serial
uploading. The baseline exposes maps but leaves `--known-data-map` unset.
The other cases use both `all-data` and `known` with each diagnostic buffer mode:
  `zeroed`: Allocate every chunk with `BytesMut::zeroed` (the default).
  `resize`: Allocate with `BytesMut::with_capacity`, then fill with zeros.
  `reuse`: Reclaim consumed chunk storage, then fill with zeros.
All three modes still initialize every chunk, including those read from the
source. This isolates allocation behavior while preserving the read API.

Component 3 compares `read_exact` with `read_buf` using `all-data` maps and the
`resize` allocation mode. Both cases initialize the buffers before reading, so
their difference measures the read API rather than the cost of zeroing.

Components 2 and 3 run by default. They require a PBS client that supports the
temporary `--test-map-buffer-mode` and `--test-map-read-buf` options. All selected
components share the same volumes, backup group, preliminary backup, and results
archive. At the default two repetitions, they create 19 snapshots in total.

The test holds the installed backup lock while using the shared mount. It
creates a separate PBS group named `devicefs-map-test-<unique ID>` in the
configured namespace. Test snapshots remain there for inspection. Local
volumes are detached and deleted after the run. The results are collected into
a single `.tar.gz` archive beside the output directory, and its full path is
printed. After successful archiving, the individual files and the empty output
directory are deleted. Failed runs include the results that were collected
before the failure.

.PARAMETER SupervisorPath
The `backup-supervisor.exe` to test. Its `--devicefs` mode runs the filesystem.
Defaults to the repository's `Release` build for the machine's native architecture.

.PARAMETER SizeGiB
The virtual disk size. The default gives the long zero ranges enough work to
measure without preparing a multi-terabyte volume. A dynamic VHDX consumes
physical storage for its metadata, rather than its full virtual capacity.

.PARAMETER Repetitions
The number of measured passes through the selected cases. Alternate passes run
in reverse order. The preliminary backup is additional to these passes.

.PARAMETER Components
The numbered comparisons to run: 1 for map exposure and use, 2 for allocation,
and 3 for the read API. Accepts multiple numbers and defaults to 2,3.

.PARAMETER OutputDirectory
A new directory for executable identities, individual logs, and CSV results.
The default is a unique directory under Windows `SystemTemp`.

.EXAMPLE
pwsh .\tests\Measure-PbsKnownDataMap.ps1

.EXAMPLE
& .\tests\Measure-PbsKnownDataMap.ps1 -Components 1,2,3

.EXAMPLE
pwsh .\tests\Measure-PbsKnownDataMap.ps1 -Components 1

.EXAMPLE
pwsh .\tests\Measure-PbsKnownDataMap.ps1 .\build\windows-arm64\Release\backup-supervisor.exe -SizeGiB 256 -Repetitions 4
#>

[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [string] $SupervisorPath,

    [ValidateRange(1, [int]::MaxValue)]
    [int] $SizeGiB = 128,

    [ValidateRange(1, [int]::MaxValue)]
    [int] $Repetitions = 2,

    [ValidateSet(1, 2, 3)]
    [int[]] $Components = @(2, 3),

    [string] $OutputDirectory = (Join-Path $env:WINDIR 'SystemTemp' `
        "devicefs-pbs-map-test-$([Guid]::NewGuid().ToString('N'))")
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
. (Join-Path $PSScriptRoot 'include/DeviceFsTestProcess.ps1')
. (Join-Path $PSScriptRoot 'include/DeviceFsTestVolume.ps1')

if (-not $SupervisorPath) {
    $SupervisorPath = Get-DefaultTestExecutablePath 'backup-supervisor.exe'
}
$SupervisorPath = (Resolve-Path -LiteralPath $SupervisorPath).Path
$fish_program = Join-Path $PSScriptRoot 'measure-pbs-known-data-map.fish'
$OutputDirectory = (New-Item -ItemType Directory -Path $OutputDirectory).FullName

# The installed distribution can mount this one Windows path through its
# `sudo` rule. Holding the supervisor's existing lock prevents a scheduled
# backup from trying to use the same path during these measurements.
$installation = Join-Path $env:ProgramData 'devicefs'
$mount_path = 'C:\ProgramData\devicefs\backup-mount'
$backup_lock = [IO.File]::Open(
    (Join-Path $installation 'credentials/pbs-vss-backup.lock'),
    [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::None)
$invocation = $null
$vhd_paths = [Collections.Generic.List[string]]::new()
$run_id = [Guid]::NewGuid().ToString('N')
$backup_id = "devicefs-map-test-$run_id"
$results_path = Join-Path $OutputDirectory 'results.csv'
$results = [Collections.Generic.List[object]]::new()

try {
    if ((Test-Path -LiteralPath $mount_path) -and
        ((Get-Item -LiteralPath $mount_path).Attributes.HasFlag([IO.FileAttributes]::ReparsePoint) -or
            ([IO.Directory]::GetFileSystemEntries($mount_path).Length -ne 0))) {
        throw "The backup mount '$mount_path' is already in use."
    }
    $read_user = (Get-Content -Raw -LiteralPath `
        (Join-Path $installation 'credentials/backup.json') |
        ConvertFrom-Json).internal_windows_account.username

    Write-Host "Results: $OutputDirectory"
    Write-Host "PBS test group: host/$backup_id (in the configured namespace)"
    $ntfs_cluster_size = 4KB
    # The companion is deliberately much smaller so its progress can reveal
    # whether processing the large image delays work on another image.
    $companion_size = 512MB
    $sizes = [ordered]@{ volume = [UInt64]$SizeGiB * 1GB; companion = $companion_size }
    $mappings = [ordered]@{}
    $image_bytes = 0L
    foreach ($definition in $sizes.GetEnumerator()) {
        $vhd_path = Join-Path $OutputDirectory "$($definition.Key).vhdx"
        $vhd_paths.Add($vhd_path)
        $partition = New-DeviceFsTestVolume -Path $vhd_path -SizeBytes $definition.Value `
            -ClusterSize $ntfs_cluster_size -Label "PBS test $($definition.Key)"
        $partition_number = $partition.PartitionNumber
        Dismount-DiskImage -ImagePath $vhd_path
        $disk = Mount-DiskImage -ImagePath $vhd_path -Access ReadOnly `
            -NoDriveLetter -PassThru | Get-Disk
        $partition = Get-Partition -DiskNumber $disk.Number -PartitionNumber $partition_number
        $mappings["$($definition.Key).img"] = ($partition | Get-Volume).Path.TrimEnd([char]'\')
        $image_bytes += [long]$partition.Size
    }

    [ordered]@{
        StartedUtc = [DateTime]::UtcNow.ToString('O')
        Computer = $env:COMPUTERNAME
        Processor = @(Get-CimInstance Win32_Processor | Select-Object -ExpandProperty Name)
        Architecture = [Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString()
        Supervisor = $SupervisorPath
        SupervisorVersion = [Diagnostics.FileVersionInfo]::GetVersionInfo($SupervisorPath).ProductVersion
        SupervisorSha256 = (Get-FileHash -LiteralPath $SupervisorPath).Hash
        ImageBytes = $image_bytes
        VirtualDiskSizes = $sizes
        Repetitions = $Repetitions
        Components = $Components
        BackupGroup = "host/$backup_id"
    } | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $OutputDirectory 'run.json')

    $cases = @(foreach ($component in ($Components | Select-Object -Unique)) {
        switch ($component) {
            1 {
                foreach ($parallel in @($false, $true)) {
                    foreach ($map in @('hidden', 'unused', 'all-data', 'known')) {
                        [pscustomobject]@{
                            Component = $component; Map = $map; Parallel = $parallel
                            BufferMode = if ($map -in @('all-data', 'known')) { 'zeroed' } else { 'standard' }
                            ReadApi = if ($map -in @('all-data', 'known')) { 'exact' } else { 'standard' }
                        }
                    }
                }
            }
            2 {
                [pscustomobject]@{
                    Component = $component; Map = 'unused'; Parallel = $false
                    BufferMode = 'standard'; ReadApi = 'standard'
                }
                foreach ($map in @('all-data', 'known')) {
                    foreach ($buffer in @('zeroed', 'resize', 'reuse')) {
                        [pscustomobject]@{
                            Component = $component; Map = $map; Parallel = $false
                            BufferMode = $buffer; ReadApi = 'exact'
                        }
                    }
                }
            }
            3 {
                foreach ($read_api in @('exact', 'read-buf')) {
                    [pscustomobject]@{
                        Component = $component; Map = 'all-data'; Parallel = $false
                        BufferMode = 'resize'; ReadApi = $read_api
                    }
                }
            }
        }
    })
    Write-Host ('Components: {0}; {1} measured backups plus one preliminary backup.' -f
        ($Components -join ', '), ($cases.Count * $Repetitions))
    # The client uses its default 4 MiB image chunks. The exposed map must
    # describe those same chunks; no chunk-size override is passed to PBS.
    $pbs_chunk_size = 4MB
    for ($pass = 0; $pass -le $Repetitions; ++$pass) {
        $ordered_cases = @(if ($pass -eq 0) {
            [pscustomobject]@{
                Component = 0; Map = 'hidden'; Parallel = $false
                BufferMode = 'standard'; ReadApi = 'standard'
            }
        } else { $cases })
        if (($pass % 2) -eq 0) {
            [Array]::Reverse($ordered_cases)
        }
        foreach ($case in $ordered_cases) {
            $uploader = if ($case.Parallel) { 'parallel' } else { 'serial' }
            $name = if ($pass -eq 0) { 'warmup' } else {
                '{0}-component{1}-{2}-{3}-{4}-{5}' -f $pass, $case.Component,
                    $uploader, $case.Map, $case.BufferMode, $case.ReadApi
            }
            $log = Join-Path $OutputDirectory "$name.log"
            $map_size = if ($case.Map -eq 'hidden') { 0 } else { $pbs_chunk_size }
            $filesystem_timer = [Diagnostics.Stopwatch]::StartNew()
            $invocation = Start-DeviceFsTestProcess -Executable $SupervisorPath -UseSupervisor `
                -MountPath $mount_path -ReadUser $read_user `
                -StopEvent "Local\devicefs-map-test-$run_id-$name" `
                -Mappings $mappings `
                -SyntheticFreeClusters -Cache -KnownDataMapClusterSize $map_size
            $invocation.OutputLog = Join-Path $OutputDirectory "$name-devicefs.stdout.log"
            $invocation.ErrorLog = Join-Path $OutputDirectory "$name-devicefs.stderr.log"
            Wait-DeviceFsReady $invocation
            $filesystem_timer.Stop()

            Write-Host "Running $name ($image_bytes image bytes)..."
            # Component 1 does not require diagnostic options, so it can also
            # run with PBS clients that do not support those options.
            $pbs_arguments = @(if (($case.Component -gt 1) -and
                ($case.Map -in @('all-data', 'known'))) {
                '--test-map-buffer-mode'; $case.BufferMode
                '--test-map-read-buf'; ($case.ReadApi -eq 'read-buf').ToString().ToLowerInvariant()
            })
            $timer = [Diagnostics.Stopwatch]::StartNew()
            & $SupervisorPath --run-fish-program $fish_program -- `
                $backup_id $case.Map $uploader @pbs_arguments 2>&1 | ForEach-Object {
                    '[{0:O}] {1}' -f [DateTime]::UtcNow, $_
                } | Tee-Object -FilePath $log | Out-Host
            $exit_code = $LASTEXITCODE
            $timer.Stop()
            Stop-DeviceFsTestProcess $invocation
            $invocation.Process.Dispose()
            $invocation = $null

            $duration = [regex]::Match(
                [IO.File]::ReadAllText($log), 'Duration:\s+([0-9]+(?:\.[0-9]+)?)s')
            $seconds = if (($exit_code -eq 0) -and $duration.Success) {
                [double]::Parse($duration.Groups[1].Value, [Globalization.CultureInfo]::InvariantCulture)
            } else { $null }
            $row = [pscustomobject]@{
                Pass = $pass
                Component = $case.Component
                Map = $case.Map
                Uploader = $uploader
                BufferMode = $case.BufferMode
                ReadApi = $case.ReadApi
                ImageBytes = $image_bytes
                PbsSeconds = $seconds
                GiBPerSecond = if ($seconds -gt 0) { $image_bytes / 1GB / $seconds } else { $null }
                SupervisorSeconds = $timer.Elapsed.TotalSeconds
                FilesystemStartupSeconds = $filesystem_timer.Elapsed.TotalSeconds
                ExitCode = $exit_code
                Log = $log
            }
            $results.Add($row)
            $row | Export-Csv -LiteralPath $results_path -Append -NoTypeInformation
            if ($exit_code -eq 130) {
                throw 'The benchmark was cancelled. Completed measurements are included in the results.'
            }
            if ($null -eq $seconds) {
                Write-Warning "No successful PBS timing for '$name'; see '$log'."
                if ($pass -eq 0) {
                    throw 'The preliminary backup failed; the comparison requires a previous snapshot.'
                }
            }
        }
    }

    $summary = @($results | Where-Object { ($_.Pass -gt 0) -and ($null -ne $_.PbsSeconds) } |
        Group-Object Component, Uploader, Map, BufferMode, ReadApi | ForEach-Object {
            $times = @($_.Group.PbsSeconds | Sort-Object)
            $middle = [int][Math]::Floor($times.Count / 2)
            $median = if (($times.Count % 2) -eq 0) {
                ($times[$middle - 1] + $times[$middle]) / 2
            } else { $times[$middle] }
            [pscustomobject]@{
                Component = $_.Group[0].Component
                Uploader = $_.Group[0].Uploader
                Map = $_.Group[0].Map
                BufferMode = $_.Group[0].BufferMode
                ReadApi = $_.Group[0].ReadApi
                Runs = $times.Count
                MinimumSeconds = $times[0]
                MedianSeconds = $median
                MaximumSeconds = $times[-1]
                MedianGiBPerSecond = $image_bytes / 1GB / $median
            }
        })
    $summary | Export-Csv -LiteralPath (Join-Path $OutputDirectory 'summary.csv') -NoTypeInformation
    $summary | Format-Table -AutoSize | Out-Host
    Write-Host "PBS snapshots remain in host/$backup_id for inspection."
    $failed = @($results | Where-Object { $null -eq $_.PbsSeconds }).Count
    if ($failed) {
        throw "$failed measurements failed; see the collected logs for details."
    }
} finally {
    try {
        if ($null -ne $invocation) {
            if (-not $invocation.StartupExitObserved) {
                Stop-DeviceFsTestProcess $invocation
            }
            $invocation.Process.Dispose()
        }
        foreach ($vhd_path in $vhd_paths) {
            if (Test-Path -LiteralPath $vhd_path) {
                if ((Get-DiskImage -ImagePath $vhd_path).Attached) {
                    Dismount-DiskImage -ImagePath $vhd_path
                }
                Remove-Item -LiteralPath $vhd_path
            }
        }
    } finally {
        $backup_lock.Dispose()
        $archive = "$OutputDirectory.tar.gz"
        # A failed cleanup can leave a virtual disk in the output directory.
        # Only the diagnostic output belongs in the archive, not those disks.
        tar -czf $archive --exclude '*.vhdx' -C $OutputDirectory .
        if ($LASTEXITCODE -eq 0) {
            Write-Host "Results archive: $archive"
            Get-ChildItem -LiteralPath $OutputDirectory -File -Force |
                Where-Object Name -NotLike '*.vhdx' | ForEach-Object {
                    Remove-Item -LiteralPath $_.FullName -ErrorAction Continue
                }
            if ([IO.Directory]::GetFileSystemEntries($OutputDirectory).Length -eq 0) {
                Remove-Item -LiteralPath $OutputDirectory -ErrorAction Continue
            }
        } else {
            Write-Warning "Could not create results archive '$archive'; tar exited with code $LASTEXITCODE. The files remain in '$OutputDirectory'."
        }
    }
}
