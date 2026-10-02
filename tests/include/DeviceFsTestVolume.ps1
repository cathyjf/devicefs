# SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
# SPDX-License-Identifier: GPL-3.0-or-later

# Create and attach a new VHD or VHDX containing one writable partition,
# without assigning a drive letter. Return the attachment for the caller to
# mount or populate. The filename selects VHD or VHDX; `Fixed` selects fully
# allocated storage rather than a dynamic image.
# `DevDrive` formats the partition as a Dev Drive.
# The caller detaches the returned `DeviceFsTestDisk` before deleting `Path`.
# A failure during creation or formatting closes the attachment automatically.
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

        [switch] $Fixed,

        [switch] $DevDrive,

        [ValidateSet('NTFS', 'ReFS')]
        [string] $FileSystem = 'NTFS'
    )

    $PSNativeCommandUseErrorActionPreference = $false
    $disk = [DeviceFsTestDisk]::Create($Path, $SizeBytes, $Fixed.IsPresent)
    try {
        # `format.com` rejects a volume GUID without a mount point or drive
        # letter. A temporary directory mount satisfies that requirement while
        # the command still identifies the volume by its GUID.
        $format_mount = New-Item -ItemType Directory -Path (Join-Path (
            [IO.Path]::GetDirectoryName($Path)) ('format-' + [Guid]::NewGuid().ToString('N')))
        try {
            $disk.Mount($format_mount.FullName)
            try {
                # `/A` accepts byte counts through 8192, then sizes such as
                # `64K`. `/Y` prevents prompts; `/Q` avoids a full disk scan.
                # https://learn.microsoft.com/en-us/windows-server/administration/windows-commands/format
                $allocation_size = if ($ClusterSize -ge 16KB) {
                    "$($ClusterSize / 1KB)K"
                } else { "$ClusterSize" }
                $format_executable = Join-Path $env:WINDIR 'System32/format.com'
                $format_arguments = @(
                    $disk.VolumeName.TrimEnd('\')
                    "/FS:$FileSystem"
                    "/A:$allocation_size"
                    "/V:$Label"
                    '/Q'
                    '/Y'
                    if ($DevDrive) { '/DevDrv' }
                )
                $display_command = (@($format_executable) + $format_arguments |
                    ForEach-Object { "'" + $_.Replace("'", "''") + "'" }) -join ' '
                Write-Host "Formatting '$Path': & $display_command"
                $format_output = & $format_executable @format_arguments 2>&1
                if ($LASTEXITCODE -ne 0) {
                    throw "Could not format the test volume in '$Path' as $FileSystem (format exit code $LASTEXITCODE):`n$($format_output -join "`n")"
                }
            } finally {
                $disk.Unmount($format_mount.FullName)
            }
        } finally {
            Remove-Item -LiteralPath $format_mount.FullName
        }
        return $disk
    } catch {
        $disk.Dispose()
        throw
    }
}
