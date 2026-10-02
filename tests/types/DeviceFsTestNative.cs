// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

using System;
using System.Buffers.Binary;
using System.Collections.Generic;
using System.ComponentModel;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using Microsoft.Win32.SafeHandles;
using Windows.Win32;
using Windows.Win32.Foundation;
using Windows.Win32.Security;
using Windows.Win32.Storage.FileSystem;
using Windows.Win32.Storage.Vhd;
using Windows.Win32.System.Console;
using Windows.Win32.System.Memory;
using Windows.Win32.System.Ioctl;
using Windows.Win32.Devices.DeviceAndDriverInstallation;
using static Windows.Win32.PInvoke;

public sealed class VolumeIdentity {
    public string Label { get; }
    public string FileSystem { get; }
    public long Length { get; }
    public uint DiskNumber { get; }
    public long DiskStartingOffset { get; }
    public long DiskExtentLength { get; }

    internal VolumeIdentity(string label, string fileSystem, long length, uint diskNumber,
        long diskStartingOffset, long diskExtentLength) {
        Label = label;
        FileSystem = fileSystem;
        Length = length;
        DiskNumber = diskNumber;
        DiskStartingOffset = diskStartingOffset;
        DiskExtentLength = diskExtentLength;
    }
}

public sealed class VolumeAllocationBitmap {
    private readonly byte[] bits;

    public long Length { get; }
    public uint SectorSize { get; }
    public uint ClusterSize { get; }
    public long ClusterCount { get; }

    internal VolumeAllocationBitmap(long length, uint sectorSize, uint clusterSize,
        long clusterCount, byte[] bits) {
        Length = length;
        SectorSize = sectorSize;
        ClusterSize = clusterSize;
        ClusterCount = clusterCount;
        this.bits = bits;
    }

    public bool IsAllocated(long cluster) {
        if ((cluster < 0) || (cluster >= ClusterCount)) {
            return true;
        }

        return (bits[cluster / 8] & (1 << (int)(cluster % 8))) != 0;
    }

    public void VerifyKnownDataBitmap(byte[] map, long chunkSize) {
        ArgumentOutOfRangeException.ThrowIfNegativeOrZero(chunkSize);
        var chunkCount = Length / chunkSize + (Length % chunkSize != 0 ? 1 : 0);
        var expected = new byte[checked((int)((chunkCount + 7) / 8))];
        for (var chunk = 0L; chunk < chunkCount; ++chunk) {
            var start = chunk * chunkSize;
            var end = start + Math.Min(chunkSize, Length - start);
            var first = start / ClusterSize;
            var last = (end - 1) / ClusterSize;
            if (HasAllocatedClusters(first, last)) {
                expected[chunk / 8] |= (byte)(1 << (int)(chunk % 8));
            }
        }
        var completeBytes = checked((int)(chunkCount / 8));
        var remainingBits = (int)(chunkCount % 8);
        if (expected.AsSpan(0, completeBytes).SequenceEqual(
                map.AsSpan(0, completeBytes)) &&
                ((remainingBits == 0) ||
                    (((expected[completeBytes] ^ map[completeBytes]) &
                        ((1 << remainingBits) - 1)) == 0))) {
            return;
        }
        for (var chunk = 0L; chunk < chunkCount; ++chunk) {
            if (((expected[chunk / 8] ^ map[chunk / 8]) &
                    (1 << (int)(chunk % 8))) != 0) {
                throw new InvalidDataException(
                    $"Known-data bitmap differs at chunk {chunk}.");
            }
        }
    }

    private bool HasAllocatedClusters(long first, long last) {
        if (((first % 8) == 0) && (((last + 1) % 8) == 0) &&
                (last < ClusterCount)) {
            return bits.AsSpan(checked((int)(first / 8)),
                checked((int)((last - first + 1) / 8)))
                .ContainsAnyExcept((byte)0);
        }
        for (var cluster = first; cluster <= last; ++cluster) {
            if (IsAllocated(cluster)) {
                return true;
            }
        }
        return false;
    }
}

public sealed class ComparisonSummary {
    public long BytesCompared { get; internal set; }
    public long FreeBytes { get; internal set; }
}

public sealed class ClusterRange {
    public long StartingCluster { get; }
    public long ClusterCount { get; }

    internal ClusterRange(long startingCluster, long clusterCount) {
        StartingCluster = startingCluster;
        ClusterCount = clusterCount;
    }
}

public sealed class NativeFileSystemEntry {
    public string FullName { get; }
    public FileAttributes Attributes { get; }

    internal NativeFileSystemEntry(string fullName, FileAttributes attributes) {
        FullName = fullName;
        Attributes = attributes;
    }
}

public sealed class AllocationChangeBlocks {
    public long[] BecameAllocated { get; }
    public long[] BecameFree { get; }

    internal AllocationChangeBlocks(long[] becameAllocated,
        long[] becameFree) {
        BecameAllocated = becameAllocated;
        BecameFree = becameFree;
    }
}

public static unsafe class DeviceFsTestNative {
    private static readonly int VolumeBitmapHeaderSize =
        Marshal.OffsetOf<VOLUME_BITMAP_BUFFER>(nameof(VOLUME_BITMAP_BUFFER.Buffer)).ToInt32();
    private static readonly int RetrievalPointersHeaderSize =
        Marshal.OffsetOf<RETRIEVAL_POINTERS_BUFFER>(nameof(RETRIEVAL_POINTERS_BUFFER.Extents)).ToInt32();
    private static readonly int RetrievalPointersExtentSize =
        sizeof(RETRIEVAL_POINTERS_BUFFER._Anonymous_e__Struct);
    private static readonly int FileStreamInfoHeaderSize =
        Marshal.OffsetOf<FILE_STREAM_INFO>(nameof(FILE_STREAM_INFO.StreamName)).ToInt32();
    private const int InitialStreamInformationSize = 4096;
    private const int DirectoryInformationBufferSize = 64 * 1024;

    internal readonly struct IoResult {
        public byte[] Buffer { get; }
        public uint BytesReturned { get; }

        public IoResult(byte[] buffer, uint bytesReturned) {
            Buffer = buffer;
            BytesReturned = bytesReturned;
        }
    }

    private sealed class BackupPrivilegeScope : IDisposable {
        private SafeFileHandle token;
        private TOKEN_PRIVILEGES previousState;

        public BackupPrivilegeScope() {
            if (!OpenProcessToken(GetCurrentProcess_SafeHandle(),
                    (TOKEN_ACCESS_MASK.TOKEN_ADJUST_PRIVILEGES | TOKEN_ACCESS_MASK.TOKEN_QUERY), out var openedToken)) {
                throw LastError("could not open the PowerShell process token");
            }

            try {
                if (!LookupPrivilegeValue(
                        null, SE_BACKUP_NAME, out var luid)) {
                    throw LastError("could not identify SeBackupPrivilege");
                }
                var requestedState = new TOKEN_PRIVILEGES { PrivilegeCount = 1 };
                requestedState.Privileges[0] = new LUID_AND_ATTRIBUTES {
                    Luid = luid,
                    Attributes = TOKEN_PRIVILEGES_ATTRIBUTES.SE_PRIVILEGE_ENABLED,
                };
                var previous = new TOKEN_PRIVILEGES();
                // `ReturnLength` must be non-null when `PreviousState` is requested.
                // https://learn.microsoft.com/en-us/windows/win32/api/securitybaseapi/nf-securitybaseapi-adjusttokenprivileges
                Marshal.SetLastPInvokeError(0);
                if (!AdjustTokenPrivileges(openedToken, false,
                        &requestedState,
                        MemoryMarshal.AsBytes(MemoryMarshal.CreateSpan(ref previous, 1)),
                        out _)) {
                    throw LastError("could not enable SeBackupPrivilege");
                }
                var error = Marshal.GetLastWin32Error();
                previousState = previous;
                if (error != 0) {
                    var operation = error == (int)WIN32_ERROR.ERROR_NOT_ALL_ASSIGNED
                        ? "the process token does not contain SeBackupPrivilege"
                        : "could not enable SeBackupPrivilege";
                    throw Win32Error(operation, error);
                }
                token = openedToken;
            } catch {
                openedToken.Dispose();
                throw;
            }
        }

        public void Dispose() {
            if (token == null) {
                return;
            }

            // A count of zero means that the privilege was already in the
            // requested state and AdjustTokenPrivileges changed nothing.
            if (previousState.PrivilegeCount != 0) {
                var state = previousState;
                Marshal.SetLastPInvokeError(0);
                var restored = AdjustTokenPrivileges(
                    token, false, &state, default);
                var error = Marshal.GetLastWin32Error();
                if (!restored || (error != 0)) {
                    throw Win32Error(
                        "could not restore SeBackupPrivilege", error);
                }
            }

            token.Dispose();
            token = null;
        }
    }

    private sealed class AlignedBuffer : SafeHandleZeroOrMinusOneIsInvalid {
        public AlignedBuffer(int size) : base(true) {
            if (size <= 0) {
                throw new ArgumentOutOfRangeException(nameof(size));
            }

            SetHandle((IntPtr)VirtualAlloc(null, (nuint)size,
                (VIRTUAL_ALLOCATION_TYPE.MEM_COMMIT | VIRTUAL_ALLOCATION_TYPE.MEM_RESERVE),
                PAGE_PROTECTION_FLAGS.PAGE_READWRITE));
            if (IsInvalid) {
                throw LastError("VirtualAlloc failed");
            }
        }

        protected override bool ReleaseHandle() {
            return VirtualFree((void*)handle, 0, VIRTUAL_FREE_TYPE.MEM_RELEASE);
        }
    }

    internal static Win32Exception Win32Error(string operation, int error) {
        var description = new Win32Exception(error).Message;
        return new Win32Exception(
            error, $"{operation}: {description} ({error})");
    }

    private static Win32Exception LastError(string operation) {
        return Win32Error(operation, Marshal.GetLastWin32Error());
    }

    // The exposure supervisor shares the harness's console and reads Ctrl+C
    // as a key record with processed input disabled. Writing that record asks
    // the supervisor to use its existing cancellation and cleanup path.
    // The harness calls this while exposure is the console input reader.
    // https://learn.microsoft.com/en-us/windows/console/writeconsoleinput
    public static void SendCtrlC() {
        using (var input = CreateFile("CONIN$", (uint)GENERIC_ACCESS_RIGHTS.GENERIC_WRITE,
                (FILE_SHARE_MODE.FILE_SHARE_READ | FILE_SHARE_MODE.FILE_SHARE_WRITE), null,
                FILE_CREATION_DISPOSITION.OPEN_EXISTING, 0, null)) {
            if (input.IsInvalid) {
                throw LastError("could not open console input to send Ctrl+C");
            }
            var record = new INPUT_RECORD { EventType = (ushort)KEY_EVENT };
            record.Event.KeyEvent = new KEY_EVENT_RECORD {
                bKeyDown = true,
                wRepeatCount = 1,
                wVirtualKeyCode = 'C',
                dwControlKeyState = LEFT_CTRL_PRESSED,
            };
            record.Event.KeyEvent.uChar.UnicodeChar = '\u0003';
            var records = new[] { record };
            if (!WriteConsoleInput(input, records,
                    out var written)) {
                throw LastError("could not send Ctrl+C to console input");
            }
            if (written != records.Length) {
                throw new IOException("Ctrl+C was not written to console input.");
            }
        }
    }

    private static string DevicePath(string volumeName) {
        return volumeName.EndsWith("\\", StringComparison.Ordinal)
            ? volumeName.Substring(0, volumeName.Length - 1)
            : volumeName;
    }

    private static SafeFileHandle OpenDevice(string path) {
        var handle = CreateFile(DevicePath(path), (uint)GENERIC_ACCESS_RIGHTS.GENERIC_READ,
            (FILE_SHARE_MODE.FILE_SHARE_READ | FILE_SHARE_MODE.FILE_SHARE_WRITE | FILE_SHARE_MODE.FILE_SHARE_DELETE),
            null, FILE_CREATION_DISPOSITION.OPEN_EXISTING,
            (FILE_FLAGS_AND_ATTRIBUTES.SECURITY_SQOS_PRESENT | FILE_FLAGS_AND_ATTRIBUTES.SECURITY_IDENTIFICATION), null);
        if (handle.IsInvalid) {
            var error = LastError("could not open volume device");
            handle.Dispose();
            throw error;
        }

        return handle;
    }

    private static SafeFileHandle OpenRawReadDevice(string path) {
        var device = OpenDevice(path);
        try {
            _ = Control(device, FSCTL_ALLOW_EXTENDED_DASD_IO, null, 0);
            return device;
        } catch {
            device.Dispose();
            throw;
        }
    }

    private static SafeFileHandle OpenObjectForExtents(string path) {
        var handle = CreateFile(path, (uint)GENERIC_ACCESS_RIGHTS.GENERIC_READ,
            (FILE_SHARE_MODE.FILE_SHARE_READ | FILE_SHARE_MODE.FILE_SHARE_WRITE | FILE_SHARE_MODE.FILE_SHARE_DELETE),
            null, FILE_CREATION_DISPOSITION.OPEN_EXISTING,
            (FILE_FLAGS_AND_ATTRIBUTES.SECURITY_SQOS_PRESENT | FILE_FLAGS_AND_ATTRIBUTES.SECURITY_IDENTIFICATION |
                FILE_FLAGS_AND_ATTRIBUTES.FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAGS_AND_ATTRIBUTES.FILE_FLAG_BACKUP_SEMANTICS), null);
        if (handle.IsInvalid) {
            var error = LastError($"could not open '{path}'");
            handle.Dispose();
            throw error;
        }

        return handle;
    }

    internal static IoResult Control(SafeFileHandle device, uint code,
        byte[] input, int outputSize, bool allowMoreData = false) {
        var output = outputSize == 0 ? null : new byte[outputSize];
        if (!DeviceIoControl(device, code, input, output, out var returned, null)) {
            var error = Marshal.GetLastWin32Error();
            if (!allowMoreData || (error != (int)WIN32_ERROR.ERROR_MORE_DATA)) {
                throw Win32Error($"DeviceIoControl 0x{code:X8} failed", error);
            }
        }

        return new IoResult(output ?? Array.Empty<byte>(), returned);
    }

    private static long QueryLength(SafeFileHandle device) {
        var result = Control(
            device, IOCTL_DISK_GET_LENGTH_INFO, null, sizeof(GET_LENGTH_INFORMATION));
        if (result.BytesReturned < sizeof(GET_LENGTH_INFORMATION)) {
            throw new InvalidDataException(
                "IOCTL_DISK_GET_LENGTH_INFO returned incomplete data");
        }

        var length = MemoryMarshal.Read<GET_LENGTH_INFORMATION>(result.Buffer).Length;
        if (length < 0) {
            throw new InvalidDataException(
                "IOCTL_DISK_GET_LENGTH_INFO returned a negative length");
        }

        return length;
    }

    private static uint QuerySectorSize(SafeFileHandle device) {
        var result = Control(
            device, IOCTL_DISK_GET_DRIVE_GEOMETRY, null, sizeof(DISK_GEOMETRY));
        if (result.BytesReturned < sizeof(DISK_GEOMETRY)) {
            throw new InvalidDataException(
                "IOCTL_DISK_GET_DRIVE_GEOMETRY returned incomplete data");
        }

        var sectorSize = MemoryMarshal.Read<DISK_GEOMETRY>(result.Buffer).BytesPerSector;
        if (sectorSize == 0) {
            throw new InvalidDataException(
                "IOCTL_DISK_GET_DRIVE_GEOMETRY returned a zero sector size");
        }

        return sectorSize;
    }

    private static NTFS_VOLUME_DATA_BUFFER QueryNtfsData(SafeFileHandle device) {
        var result = Control(
            device, FSCTL_GET_NTFS_VOLUME_DATA, null, sizeof(NTFS_VOLUME_DATA_BUFFER));
        if (result.BytesReturned < sizeof(NTFS_VOLUME_DATA_BUFFER)) {
            throw new InvalidDataException(
                "FSCTL_GET_NTFS_VOLUME_DATA returned incomplete data");
        }

        return MemoryMarshal.Read<NTFS_VOLUME_DATA_BUFFER>(result.Buffer);
    }

    private static (long ClusterCount, uint ClusterSize, uint SectorSize)
        QueryVolumeGeometry(SafeFileHandle device, string fileSystemName) {
        if (string.Equals(fileSystemName, "NTFS", StringComparison.OrdinalIgnoreCase)) {
            var ntfs = QueryNtfsData(device);
            return (ntfs.TotalClusters, ntfs.BytesPerCluster, ntfs.BytesPerSector);
        }
        if (string.Equals(fileSystemName, "ReFS", StringComparison.OrdinalIgnoreCase)) {
            var result = Control(device, FSCTL_GET_REFS_VOLUME_DATA, null, sizeof(REFS_VOLUME_DATA_BUFFER));
            if (result.BytesReturned < sizeof(REFS_VOLUME_DATA_BUFFER)) {
                throw new InvalidDataException("FSCTL_GET_REFS_VOLUME_DATA returned incomplete data");
            }
            var refs = MemoryMarshal.Read<REFS_VOLUME_DATA_BUFFER>(result.Buffer);
            return (refs.TotalClusters, refs.BytesPerCluster, refs.BytesPerSector);
        }
        throw new InvalidDataException($"unsupported test filesystem '{fileSystemName}'");
    }

    private static void ValidateVolumeGeometry(long length, uint sectorSize,
        (long ClusterCount, uint ClusterSize, uint SectorSize) geometry) {
        if ((geometry.ClusterCount <= 0) || (geometry.ClusterSize == 0) ||
            (geometry.SectorSize != sectorSize) ||
            ((length % sectorSize) != 0) ||
            ((geometry.ClusterSize % sectorSize) != 0) ||
            (geometry.ClusterCount > (length / geometry.ClusterSize))) {
            throw new InvalidDataException("invalid volume geometry");
        }
    }

    private static void QueryVolumeInformation(SafeFileHandle device,
        out string label, out string fileSystemName,
        out uint fileSystemFlags) {
        var labelBuffer = new char[MAX_PATH + 1];
        var fileSystemBuffer = new char[MAX_PATH + 1];
        if (!GetVolumeInformationByHandle(device, labelBuffer,
                out _, out _, out fileSystemFlags, fileSystemBuffer)) {
            throw LastError("GetVolumeInformationByHandleW failed");
        }

        label = new string(labelBuffer).TrimEnd('\0');
        fileSystemName = new string(fileSystemBuffer).TrimEnd('\0');
    }

    private static VolumeAllocationBitmap QueryBitmap(SafeFileHandle device,
        bool requireReadOnly, Action<string> log) {
        QueryVolumeInformation(device, out _, out var fileSystemName,
            out var fileSystemFlags);
        if (requireReadOnly && ((fileSystemFlags & FILE_READ_ONLY_VOLUME) == 0)) {
            throw new InvalidDataException(
                "volume is not reported read-only");
        }

        var length = QueryLength(device);
        var sectorSize = QuerySectorSize(device);
        var geometry = QueryVolumeGeometry(device, fileSystemName);
        ValidateVolumeGeometry(length, sectorSize, geometry);

        var retrievalBase = Control(device, FSCTL_GET_RETRIEVAL_POINTER_BASE,
            null, sizeof(RETRIEVAL_POINTER_BASE));
        if ((retrievalBase.BytesReturned < sizeof(RETRIEVAL_POINTER_BASE)) ||
            (MemoryMarshal.Read<RETRIEVAL_POINTER_BASE>(retrievalBase.Buffer).FileAreaOffset != 0)) {
            throw new InvalidDataException(
                "LCN 0 does not begin at device offset 0");
        }

        var bitmapBytes = checked((geometry.ClusterCount / 8) +
            ((geometry.ClusterCount % 8) == 0 ? 0 : 1));
        var requiredSize = checked(VolumeBitmapHeaderSize + bitmapBytes);
        if (requiredSize > int.MaxValue) {
            throw new InvalidDataException("allocation bitmap is too large");
        }

        var input = new byte[sizeof(long)];
        var result = Control(device, FSCTL_GET_VOLUME_BITMAP, input,
            Math.Max(sizeof(VOLUME_BITMAP_BUFFER), (int)requiredSize), allowMoreData: true);
        var bitmap = MemoryMarshal.Read<VOLUME_BITMAP_BUFFER>(result.Buffer);
        if ((result.BytesReturned < requiredSize) ||
            (bitmap.StartingLcn != 0) ||
            (bitmap.BitmapSize < geometry.ClusterCount)) {
            throw new InvalidDataException(
                "FSCTL_GET_VOLUME_BITMAP returned incomplete or inconsistent data");
        }

        var bits = new byte[checked((int)bitmapBytes)];
        Buffer.BlockCopy(
            result.Buffer, VolumeBitmapHeaderSize, bits, 0, bits.Length);
        log($"{fileSystemName}: {geometry.ClusterCount} clusters of " +
            $"{geometry.ClusterSize} bytes; bitmap reports " +
            $"{bitmap.BitmapSize} clusters, " +
            $"returned {result.BytesReturned} bytes.");
        return new VolumeAllocationBitmap(length, sectorSize, geometry.ClusterSize,
            geometry.ClusterCount, bits);
    }

    private static VolumeIdentity InspectHandle(SafeFileHandle device) {
        QueryVolumeInformation(device, out var label, out var fileSystemName,
            out _);

        var length = QueryLength(device);
        var extents = Control(device, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS,
            null, sizeof(VOLUME_DISK_EXTENTS));
        if ((extents.BytesReturned < sizeof(VOLUME_DISK_EXTENTS)) ||
            (MemoryMarshal.Read<VOLUME_DISK_EXTENTS>(extents.Buffer).NumberOfDiskExtents != 1)) {
            throw new InvalidDataException(
                "test volume does not have exactly one disk extent");
        }

        var extent = MemoryMarshal.Read<VOLUME_DISK_EXTENTS>(extents.Buffer).Extents[0];
        return new VolumeIdentity(label, fileSystemName, length,
            extent.DiskNumber, extent.StartingOffset, extent.ExtentLength);
    }

    private static void ReadExact(SafeFileHandle device,
        AlignedBuffer buffer, int count) {
        var read = 0u;
        if (!ReadFile(device,
                (byte*)buffer.DangerousGetHandle(), (uint)count, &read, null)) {
            throw LastError("raw ReadFile failed");
        }
        if (read != count) {
            throw new EndOfStreamException("raw ReadFile completed short");
        }
    }

    private static byte[] ReadDeviceAt(SafeFileHandle device,
        long deviceLength, uint sectorSize, long offset, int count) {
        if ((offset < 0) || (count < 0)) {
            throw new ArgumentOutOfRangeException();
        }
        if ((count == 0) || (offset >= deviceLength)) {
            return Array.Empty<byte>();
        }

        var available = (int)Math.Min((long)count, deviceLength - offset);
        var rawStart = offset - (offset % sectorSize);
        var end = checked(offset + available);
        var rawEnd = checked(((end + sectorSize - 1) / sectorSize) * sectorSize);
        if (rawEnd > deviceLength) {
            rawEnd = deviceLength;
        }
        var rawLength = checked((int)(rawEnd - rawStart));
        using var buffer = new AlignedBuffer(rawLength);
        if ((buffer.DangerousGetHandle().ToInt64() % sectorSize) != 0) {
            throw new InvalidOperationException(
                "VirtualAlloc did not satisfy volume alignment");
        }

        if (!SetFilePointerEx(device, rawStart, out _, SET_FILE_POINTER_MOVE_METHOD.FILE_BEGIN)) {
            throw LastError("SetFilePointerEx failed");
        }
        ReadExact(device, buffer, rawLength);
        var result = new byte[available];
        Marshal.Copy(IntPtr.Add(buffer.DangerousGetHandle(),
            checked((int)(offset - rawStart))), result, 0, available);
        return result;
    }

    public static string GetVolumeName(string mountRoot) {
        if (!mountRoot.EndsWith("\\", StringComparison.Ordinal)) {
            mountRoot += "\\";
        }

        var result = new char[MAX_PATH + 1];
        if (!GetVolumeNameForVolumeMountPoint(
                mountRoot, result)) {
            throw LastError("GetVolumeNameForVolumeMountPointW failed");
        }

        return new string(result).TrimEnd('\0');
    }

    public static VolumeIdentity InspectVolume(string volumeName) {
        using var device = OpenDevice(volumeName);
        return InspectHandle(device);
    }

    public static VolumeAllocationBitmap GetAllocationBitmap(string devicePath,
        bool requireReadOnly, Action<string> log = null) {
        using var device = OpenDevice(devicePath);
        return QueryBitmap(device, requireReadOnly, log ?? Console.WriteLine);
    }

    public static byte[] ReadDeviceAt(string devicePath,
        long offset, int count) {
        using var device = OpenRawReadDevice(devicePath);
        return ReadDeviceAt(device, QueryLength(device),
            QuerySectorSize(device), offset, count);
    }

    public static IDisposable EnableBackupPrivilege() {
        return new BackupPrivilegeScope();
    }

    private static void AddAllocatedClusterRanges(SafeFileHandle handle,
        string path, List<ClusterRange> ranges) {
        var input = new byte[sizeof(long)];
        var output = new byte[RetrievalPointersHeaderSize +
            (256 * RetrievalPointersExtentSize)];
        var startingVcn = 0L;
        while (true) {
            Buffer.BlockCopy(
                BitConverter.GetBytes(startingVcn), 0, input, 0, input.Length);
            bool completed = DeviceIoControl(handle,
                FSCTL_GET_RETRIEVAL_POINTERS, input, output, out var returned, null);
            var error = completed ? 0 : Marshal.GetLastWin32Error();
            if ((!completed) && (error == (int)WIN32_ERROR.ERROR_HANDLE_EOF)) {
                return;
            }
            if ((!completed) && (error != (int)WIN32_ERROR.ERROR_MORE_DATA)) {
                throw Win32Error(
                    $"could not retrieve the extents of '{path}'", error);
            }
            if (returned < RetrievalPointersHeaderSize) {
                throw new InvalidDataException(
                    $"extent data for '{path}' was incomplete");
            }

            var extents = MemoryMarshal.Read<RETRIEVAL_POINTERS_BUFFER>(output);
            var extentCount = extents.ExtentCount;
            var required = checked(RetrievalPointersHeaderSize +
                ((long)extentCount * RetrievalPointersExtentSize));
            if (required > returned) {
                throw new InvalidDataException(
                    $"extent data for '{path}' was truncated");
            }

            var currentVcn = extents.StartingVcn;
            for (var index = 0; index < extentCount; ++index) {
                var offset = checked(RetrievalPointersHeaderSize +
                    (index * RetrievalPointersExtentSize));
                var extent = MemoryMarshal.Read<RETRIEVAL_POINTERS_BUFFER._Anonymous_e__Struct>(
                    output.AsSpan(offset));
                var nextVcn = extent.NextVcn;
                var lcn = extent.Lcn;
                if (nextVcn <= currentVcn) {
                    throw new InvalidDataException(
                        $"extent data for '{path}' did not advance");
                }
                if (lcn < -1) {
                    throw new InvalidDataException(
                        $"extent data for '{path}' contained an invalid LCN");
                }
                if (lcn >= 0) {
                    ranges.Add(new ClusterRange(lcn, nextVcn - currentVcn));
                }
                currentVcn = nextVcn;
            }

            if (completed) {
                return;
            }
            if ((extentCount == 0) || (currentVcn <= startingVcn)) {
                throw new InvalidDataException(
                    $"partial extent data for '{path}' did not advance");
            }
            startingVcn = currentVcn;
        }
    }

    private static string[] ParseNamedDataStreams(IntPtr buffer,
        int bufferSize, string path) {
        var result = new List<string>();
        var offset = 0;
        while (true) {
            var remaining = bufferSize - offset;
            if (remaining < FileStreamInfoHeaderSize) {
                throw new InvalidDataException(
                    $"stream information for '{path}' was truncated");
            }

            var entry = IntPtr.Add(buffer, offset);
            ref readonly var stream = ref System.Runtime.CompilerServices.Unsafe.AsRef<FILE_STREAM_INFO>((void*)entry);
            var nextEntryOffset = stream.NextEntryOffset;
            var nameByteLength = stream.StreamNameLength;
            if ((nameByteLength % sizeof(char)) != 0) {
                throw new InvalidDataException(
                    $"stream information for '{path}' contained an " +
                    "invalid name length");
            }

            var recordSize = checked(
                (long)FileStreamInfoHeaderSize + nameByteLength);
            if (recordSize > remaining) {
                throw new InvalidDataException(
                    $"stream information for '{path}' was truncated");
            }

            var characterCount = checked((int)(nameByteLength / sizeof(char)));
            var name = characterCount == 0
                ? string.Empty
                : Marshal.PtrToStringUni(
                    IntPtr.Add(entry, FileStreamInfoHeaderSize),
                    characterCount);
            if (name == null) {
                throw new InvalidDataException(
                    $"stream information for '{path}' contained an " +
                    "invalid name");
            }

            if ((name.Length != 0) &&
                !string.Equals(name, "::$DATA",
                    StringComparison.OrdinalIgnoreCase)) {
                if ((name[0] != ':') ||
                    !name.EndsWith(":$DATA",
                        StringComparison.OrdinalIgnoreCase) ||
                    (name.IndexOf('\0') >= 0) ||
                    (name.IndexOf('\\') >= 0) || (name.IndexOf('/') >= 0)) {
                    throw new InvalidDataException(
                        $"stream information for '{path}' contained an " +
                        "invalid stream name");
                }
                result.Add(name);
            }

            if (nextEntryOffset == 0) {
                return result.ToArray();
            }
            if (((nextEntryOffset % sizeof(long)) != 0) ||
                (nextEntryOffset < recordSize) ||
                (nextEntryOffset > remaining - FileStreamInfoHeaderSize)) {
                throw new InvalidDataException(
                    $"stream information for '{path}' contained an " +
                    "invalid entry offset");
            }
            offset = checked(offset + (int)nextEntryOffset);
        }
    }

    private static string[] GetNamedDataStreams(SafeFileHandle handle,
        string path) {
        var bufferSize = InitialStreamInformationSize;
        while (true) {
            using var buffer = new AlignedBuffer(bufferSize);
            if (GetFileInformationByHandleEx(handle,
                    FILE_INFO_BY_HANDLE_CLASS.FileStreamInfo,
                    new Span<byte>((void*)buffer.DangerousGetHandle(), bufferSize))) {
                return ParseNamedDataStreams(
                    buffer.DangerousGetHandle(), bufferSize, path);
            }

            var error = Marshal.GetLastWin32Error();
            if (error == (int)WIN32_ERROR.ERROR_HANDLE_EOF) {
                return Array.Empty<string>();
            }
            if ((error != (int)WIN32_ERROR.ERROR_INSUFFICIENT_BUFFER) &&
                (error != (int)WIN32_ERROR.ERROR_MORE_DATA)) {
                throw Win32Error(
                    $"could not enumerate streams of '{path}'", error);
            }
            if (bufferSize > (int.MaxValue / 2)) {
                throw new InvalidDataException(
                    $"stream information for '{path}' was too large");
            }
            bufferSize *= 2;
        }
    }

    private static FileAttributes GetFileAttributes(
        SafeFileHandle handle, string path) {
        var size = Marshal.SizeOf<FILE_ATTRIBUTE_TAG_INFO>();
        using var buffer = new AlignedBuffer(size);
        if (!GetFileInformationByHandleEx(handle,
                FILE_INFO_BY_HANDLE_CLASS.FileAttributeTagInfo,
                new Span<byte>((void*)buffer.DangerousGetHandle(), size))) {
            throw LastError($"could not query the attributes of '{path}'");
        }

        return (FileAttributes)Marshal.PtrToStructure<FILE_ATTRIBUTE_TAG_INFO>(
            buffer.DangerousGetHandle()).FileAttributes;
    }

    private static void EnumerateDirectoryTree(string path,
        SafeFileHandle directory, List<NativeFileSystemEntry> result) {
        var headerSize = Marshal.OffsetOf<FILE_FULL_DIR_INFO>(nameof(FILE_FULL_DIR_INFO.FileName)).ToInt32();
        using var buffer = new AlignedBuffer(DirectoryInformationBufferSize);
        var informationClass = FILE_INFO_BY_HANDLE_CLASS.FileFullDirectoryRestartInfo;
        while (true) {
            if (!GetFileInformationByHandleEx(directory, informationClass,
                    new Span<byte>((void*)buffer.DangerousGetHandle(),
                        DirectoryInformationBufferSize))) {
                var error = Marshal.GetLastWin32Error();
                if (error == (int)WIN32_ERROR.ERROR_NO_MORE_FILES) {
                    return;
                }
                throw Win32Error(
                    $"could not enumerate the directory '{path}'", error);
            }
            informationClass = FILE_INFO_BY_HANDLE_CLASS.FileFullDirectoryInfo;

            var offset = 0;
            while (true) {
                var remaining = DirectoryInformationBufferSize - offset;
                if (remaining < headerSize) {
                    throw new InvalidDataException(
                        $"directory information for '{path}' was truncated");
                }

                var entryAddress = IntPtr.Add(
                    buffer.DangerousGetHandle(), offset);
                ref readonly var entry = ref System.Runtime.CompilerServices.Unsafe.AsRef<
                    FILE_FULL_DIR_INFO>((void*)entryAddress);
                if ((entry.FileNameLength % sizeof(char)) != 0) {
                    throw new InvalidDataException(
                        $"directory information for '{path}' contained an " +
                        "invalid name length");
                }
                var recordSize = checked(
                    (long)headerSize + entry.FileNameLength);
                if (recordSize > remaining) {
                    throw new InvalidDataException(
                        $"directory information for '{path}' was truncated");
                }

                var characterCount = checked(
                    (int)(entry.FileNameLength / sizeof(char)));
                var name = characterCount == 0
                    ? string.Empty
                    : Marshal.PtrToStringUni(
                        IntPtr.Add(entryAddress, headerSize), characterCount);
                if ((name == null) || (name.Length == 0) ||
                    (name.IndexOf('\0') >= 0) || (name.IndexOf('\\') >= 0) ||
                    (name.IndexOf('/') >= 0)) {
                    throw new InvalidDataException(
                        $"directory information for '{path}' contained an " +
                        "invalid name");
                }

                if ((name != ".") && (name != "..")) {
                    var childPath = path.EndsWith("\\",
                            StringComparison.Ordinal)
                        ? path + name
                        : path + "\\" + name;
                    result.Add(new NativeFileSystemEntry(
                        childPath, (FileAttributes)entry.FileAttributes));
                    if ((((FileAttributes)entry.FileAttributes & FileAttributes.Directory) !=
                            0) &&
                        (((FileAttributes)entry.FileAttributes & FileAttributes.ReparsePoint) ==
                            0)) {
                        using var child = OpenObjectForExtents(childPath);
                        EnumerateDirectoryTree(childPath, child, result);
                    }
                }

                if (entry.NextEntryOffset == 0) {
                    break;
                }
                if (((entry.NextEntryOffset % sizeof(long)) != 0) ||
                    (entry.NextEntryOffset < recordSize) ||
                    (entry.NextEntryOffset > remaining - headerSize)) {
                    throw new InvalidDataException(
                        $"directory information for '{path}' contained an " +
                        "invalid entry offset");
                }
                offset = checked(offset + (int)entry.NextEntryOffset);
            }
        }
    }

    public static NativeFileSystemEntry[] EnumerateTreeObjects(string root) {
        if (string.IsNullOrEmpty(root)) {
            throw new ArgumentException(
                "the tree root must not be empty", nameof(root));
        }

        var result = new List<NativeFileSystemEntry>();
        using var handle = OpenObjectForExtents(root);
        var attributes = GetFileAttributes(handle, root);
        result.Add(new NativeFileSystemEntry(root, attributes));
        if (((attributes & FileAttributes.Directory) != 0) &&
            ((attributes & FileAttributes.ReparsePoint) == 0)) {
            EnumerateDirectoryTree(root, handle, result);
        }
        return result.ToArray();
    }

    public static ClusterRange[] GetAllocatedClusterRanges(string path,
        bool enumerateNamedDataStreams) {
        var ranges = new List<ClusterRange>();
        using var handle = OpenObjectForExtents(path);
        AddAllocatedClusterRanges(handle, path, ranges);
        if (!enumerateNamedDataStreams) {
            return ranges.ToArray();
        }

        foreach (var streamName in GetNamedDataStreams(handle, path)) {
            var streamPath = path + streamName;
            using var streamHandle = OpenObjectForExtents(streamPath);
            AddAllocatedClusterRanges(streamHandle, streamPath, ranges);
        }
        return ranges.ToArray();
    }

    public static AllocationChangeBlocks GetAllocationChangeBlocks(
        VolumeAllocationBitmap before, VolumeAllocationBitmap after, int blockSize) {
        if ((before == null) || (after == null)) {
            throw new ArgumentNullException();
        }
        if (blockSize <= 0) {
            throw new ArgumentOutOfRangeException(nameof(blockSize));
        }
        if ((before.Length != after.Length) ||
            (before.SectorSize != after.SectorSize) ||
            (before.ClusterSize != after.ClusterSize) ||
            (before.ClusterCount != after.ClusterCount)) {
            throw new InvalidDataException("allocation bitmap geometries differ");
        }

        var becameAllocated = new List<long>();
        var becameFree = new List<long>();
        for (var cluster = 0L; cluster < before.ClusterCount; ++cluster) {
            var wasAllocated = before.IsAllocated(cluster);
            var isAllocated = after.IsAllocated(cluster);
            if (wasAllocated == isAllocated) {
                continue;
            }

            var result = isAllocated ? becameAllocated : becameFree;
            var start = checked(cluster * (long)before.ClusterSize);
            var end = Math.Min(before.Length,
                checked(start + before.ClusterSize));
            for (var offset = start - (start % blockSize);
                offset < end; offset += blockSize) {
                if ((result.Count == 0) || (result[^1] != offset)) {
                    result.Add(offset);
                }
            }
        }

        return new AllocationChangeBlocks(
            becameAllocated.ToArray(), becameFree.ToArray());
    }

    public static long[] GetDifferingFileBlockOffsets(string firstImagePath,
        string secondImagePath, int blockSize) {
        if (blockSize <= 0) {
            throw new ArgumentOutOfRangeException(nameof(blockSize));
        }

        using var first = new FileStream(firstImagePath, FileMode.Open,
            FileAccess.Read, FileShare.ReadWrite | FileShare.Delete, 1,
            FileOptions.SequentialScan);
        using var second = new FileStream(secondImagePath, FileMode.Open,
            FileAccess.Read, FileShare.ReadWrite | FileShare.Delete, 1,
            FileOptions.SequentialScan);
        var length = first.Length;
        if (second.Length != length) {
            throw new InvalidDataException("image lengths differ");
        }

        const int blocksPerRead = 256;
        var readSize = checked(blockSize * blocksPerRead);
        var firstBytes = new byte[readSize];
        var secondBytes = new byte[readSize];
        var result = new List<long>();
        for (var offset = 0L; offset < length; offset += readSize) {
            var count = (int)Math.Min((long)readSize, length - offset);
            if ((ReadManaged(first, firstBytes, count) != count) ||
                (ReadManaged(second, secondBytes, count) != count)) {
                throw new EndOfStreamException(
                    "a devicefs image completed a sequential read short");
            }
            for (var index = 0; index < count; index += blockSize) {
                var current = Math.Min(blockSize, count - index);
                if (!firstBytes.AsSpan(index, current).SequenceEqual(
                        secondBytes.AsSpan(index, current))) {
                    result.Add(offset + index);
                }
            }
        }

        return result.ToArray();
    }

    public static void CopyDeviceToFile(string devicePath,
        string destinationPath) {
        using var source = OpenRawReadDevice(devicePath);
        var length = QueryLength(source);
        var sectorSize = QuerySectorSize(source);
        using var destination = new FileStream(destinationPath,
            FileMode.CreateNew, FileAccess.Write, FileShare.Read);
        const int readSize = 4 * 1024 * 1024;
        for (var offset = 0L; offset < length; offset += readSize) {
            var count = (int)Math.Min((long)readSize, length - offset);
            var bytes = ReadDeviceAt(
                source, length, sectorSize, offset, count);
            destination.Write(bytes, 0, bytes.Length);
        }
    }

    private static int ReadManaged(FileStream stream, byte[] buffer,
        int count) {
        var completed = 0;
        while (completed < count) {
            var current = stream.Read(buffer, completed, count - completed);
            if (current == 0) {
                break;
            }
            completed += current;
        }
        return completed;
    }

    private static void CompareChunk(byte[] source, byte[] normal,
        byte[] synthetic, long offset, int count, VolumeAllocationBitmap bitmap,
        ComparisonSummary summary) {
        var end = checked(offset + count);
        var position = offset;
        while (position < end) {
            var cluster = position / bitmap.ClusterSize;
            var next = Math.Min(end,
                checked((cluster + 1) * (long)bitmap.ClusterSize));
            var startIndex = checked((int)(position - offset));
            var length = checked((int)(next - position));
            var allocated = bitmap.IsAllocated(cluster);
            var sourceRange = source.AsSpan(startIndex, length);
            var syntheticRange = synthetic.AsSpan(startIndex, length);
            var syntheticMatches = allocated
                ? syntheticRange.SequenceEqual(sourceRange)
                : !syntheticRange.ContainsAnyExcept((byte)0);
            if ((!normal.AsSpan(startIndex, length).SequenceEqual(sourceRange)) ||
                    !syntheticMatches) {
                for (var i = startIndex; i < startIndex + length; ++i) {
                    var normalMatches = normal[i] == source[i];
                    var syntheticExpected = allocated ? source[i] : (byte)0;
                    if (normalMatches && (synthetic[i] == syntheticExpected)) {
                        continue;
                    }
                    var (view, expected, actual) = normalMatches
                        ? ("synthetic", syntheticExpected, synthetic[i])
                        : ("normal", source[i], normal[i]);
                    throw new InvalidDataException(
                        $"{view} devicefs view differs at offset 0x{checked(offset + i):X}, " +
                        $"LCN {cluster}: allocated={allocated}, " +
                        $"source=0x{source[i]:X2}, expected=0x{expected:X2}, " +
                        $"actual=0x{actual:X2}");
                }
            }
            if (!allocated) {
                summary.FreeBytes += length;
            }

            position = next;
        }

        summary.BytesCompared += count;
    }

    // Write sector-aligned witness bytes into a detached fixed or dynamic VHD.
    // Citations below refer to Microsoft's Virtual Hard Disk Image Format
    // Specification, October 11, 2006, v1.0, abbreviated as VHD Specification.
    // https://www.microsoft.com/en-us/download/details.aspx?id=23850
    // Multibyte fields use big-endian byte order. See VHD Specification v1.0,
    // "Introduction", p. 3.
    public static void WriteVhdWitness(string path, long diskLength, long diskOffset,
        byte[] pattern) {
        // Sector length is always 512 bytes. See VHD Specification v1.0,
        // "Introduction", p. 3.
        const int sectorSize = 512;
        if ((diskOffset < 0) || (diskOffset % sectorSize != 0) ||
            (pattern.Length == 0) || (pattern.Length % sectorSize != 0) ||
            (diskOffset > diskLength - pattern.Length)) {
            throw new ArgumentOutOfRangeException(nameof(diskOffset),
                "the witness must cover whole sectors within the virtual disk");
        }
        using var file = new FileStream(path, FileMode.Open, FileAccess.ReadWrite, FileShare.None);
        byte[] ReadAt(long offset, int count) {
            if ((offset < 0) || (offset > file.Length - count)) {
                throw new InvalidDataException($"VHD record at offset {offset} is outside '{path}'");
            }
            var bytes = new byte[count];
            file.Position = offset;
            file.ReadExactly(bytes);
            return bytes;
        }

        // The footer table places the eight-byte ASCII cookie at offset 0 and
        // the eight-byte `Current Size` at offset 48. `Current Size` counts the
        // virtual disk's bytes. This test writes new Windows VHDs with 512-byte
        // footers; the document also describes an older 511-byte footer.
        // See VHD Specification v1.0, "Hard Disk Footer Format" and
        // "Hard Disk Footer Field Descriptions", pp. 5–6.
        var footer = ReadAt(file.Length - sectorSize, sectorSize);
        if ((Encoding.ASCII.GetString(footer, 0, 8) != "conectix") ||
            (BinaryPrimitives.ReadUInt64BigEndian(footer.AsSpan(48)) != (ulong)diskLength)) {
            throw new InvalidDataException($"VHD footer in '{path}' does not match the test disk");
        }
        // The four-byte `Disk Type` field begins at offset 60. Values 2 and 3
        // identify fixed and dynamic disks. See VHD Specification v1.0,
        // "Hard Disk Footer Format", p. 5, and "Disk Type", p. 7.
        var diskType = BinaryPrimitives.ReadUInt32BigEndian(footer.AsSpan(60));
        // Fixed VHDs store disk bytes from file offset zero, followed by the
        // footer. See VHD Specification v1.0, "Fixed Hard Disk Image", p. 3.
        if (diskType == 2) {
            if (file.Length != checked(diskLength + sectorSize)) {
                throw new InvalidDataException($"fixed VHD '{path}' has an unexpected length");
            }
            file.Position = diskOffset;
            file.Write(pattern);
            file.Flush(true);
            return;
        }
        if (diskType != 3) {
            throw new InvalidDataException($"VHD '{path}' is neither fixed nor dynamic");
        }
        // The footer's eight-byte `Data Offset`, at offset 16, points to the
        // 1024-byte dynamic header. Its eight-byte cookie is `cxsparse`.
        // See VHD Specification v1.0, "Hard Disk Footer Format", pp. 5–6;
        // "Dynamic Hard Disk Image", p. 4; and "Dynamic Disk Header Format", p. 8.
        var header = ReadAt(checked((long)BinaryPrimitives.ReadUInt64BigEndian(
            footer.AsSpan(16))), 1024);
        if (Encoding.ASCII.GetString(header, 0, 8) != "cxsparse") {
            throw new InvalidDataException($"dynamic VHD header in '{path}' has an invalid cookie");
        }
        // The header table places `Table Offset` at 16 (eight bytes),
        // `Max Table Entries` at 28 (four), and `Block Size` at 32 (four).
        // The table offset counts file bytes; block size excludes the bitmap.
        // See VHD Specification v1.0, "Dynamic Disk Header Format" and its
        // field descriptions, pp. 8–9.
        var tableOffset = checked((long)BinaryPrimitives.ReadUInt64BigEndian(header.AsSpan(16)));
        var entryCount = BinaryPrimitives.ReadUInt32BigEndian(header.AsSpan(28));
        var blockSize = BinaryPrimitives.ReadUInt32BigEndian(header.AsSpan(32));
        // Blocks contain a power-of-two number of sectors. Because sectors are
        // 512 bytes, block size must be a power of two and at least 512 bytes.
        // See VHD Specification v1.0, "Block Size", p. 9, and
        // "Block Allocation Table and Data Blocks", p. 11.
        if ((blockSize < sectorSize) || ((blockSize & (blockSize - 1)) != 0)) {
            throw new InvalidDataException($"dynamic VHD '{path}' has an invalid block size");
        }
        // Each payload sector has one bitmap bit. The bitmap precedes the data
        // and is padded to a sector boundary, so its bit count is rounded first
        // to bytes, then to sectors. See VHD Specification v1.0,
        // "Block Allocation Table and Data Blocks", p. 11.
        var sectorsPerBlock = blockSize / sectorSize;
        var bitmapSize = checked((int)(((sectorsPerBlock + 7L) / 8 + sectorSize - 1)
            / sectorSize * sectorSize));
        for (var written = 0; written < pattern.Length;) {
            var offset = checked(diskOffset + written);
            // Division selects the BAT entry; the remainder below selects bytes
            // within its payload. These are the byte forms of the sector
            // formulas in VHD Specification v1.0, "Mapping a Disk Sector to
            // a Sector in the Block", p. 12.
            var blockIndex = offset / blockSize;
            if (blockIndex >= entryCount) {
                throw new InvalidDataException($"witness block is outside the BAT in '{path}'");
            }
            // BAT entries are four-byte absolute sector offsets. `0xFFFFFFFF`
            // denotes an unallocated block. See VHD Specification v1.0,
            // "Block Allocation Table and Data Blocks", p. 10.
            var entryOffset = checked(tableOffset + blockIndex * sizeof(uint));
            var entry = ReadAt(entryOffset, sizeof(uint));
            var sector = BinaryPrimitives.ReadUInt32BigEndian(entry);
            long blockOffset;
            byte[] bitmap;
            if (sector == uint.MaxValue) {
                // An absent BAT entry represents zeros. A new zero-filled
                // block replaces the old trailing footer, and the unchanged
                // footer is written at the new end. Its checksum stays valid
                // because no footer bytes change. See VHD Specification v1.0,
                // "Mapping a Disk Sector to a Sector in the Block", p. 12;
                // the zero-sector requirement in "Block Allocation Table and
                // Data Blocks", p. 11; and "Checksum", p. 7.
                blockOffset = file.Length - sectorSize;
                if (blockOffset % sectorSize != 0) {
                    throw new InvalidDataException($"dynamic VHD '{path}' is not sector aligned");
                }
                var zeroBlock = new byte[checked(bitmapSize + (int)blockSize)];
                file.Position = blockOffset;
                file.Write(zeroBlock);
                file.Write(footer);
                BinaryPrimitives.WriteUInt32BigEndian(entry,
                    checked((uint)(blockOffset / sectorSize)));
                file.Position = entryOffset;
                file.Write(entry);
                bitmap = new byte[bitmapSize];
            } else {
                // The BAT locates the start of the bitmap. Its padded length
                // separates that offset from the payload. See VHD Specification
                // v1.0, "Mapping a Disk Sector to a Sector in the Block", p. 12.
                blockOffset = (long)sector * sectorSize;
                if (blockOffset + bitmapSize + blockSize > file.Length - sectorSize) {
                    throw new InvalidDataException($"BAT entry in '{path}' extends beyond its data");
                }
                bitmap = ReadAt(blockOffset, bitmapSize);
            }
            var withinBlock = offset % blockSize;
            var count = (int)Math.Min(pattern.Length - written, blockSize - withinBlock);
            file.Position = blockOffset + bitmapSize + withinBlock;
            file.Write(pattern, written, count);
            // A zero bitmap bit requires the sector's stored bytes to be zero.
            // Marking all payload sectors valid therefore preserves the meaning
            // of unwritten sectors, and also makes the witness valid. Updating
            // complete bitmap bytes avoids depending on an intra-byte bit order
            // that the specification does not explicitly define. Padding bytes
            // after the payload's bitmap are left unchanged.
            // See VHD Specification v1.0, "Block Allocation Table and Data
            // Blocks", p. 11.
            Array.Fill(bitmap, byte.MaxValue, 0,
                checked((int)((sectorsPerBlock + 7L) / 8)));
            file.Position = blockOffset;
            file.Write(bitmap);
            written += count;
        }
        file.Flush(true);
    }

    public static ComparisonSummary CompareViews(string sourceDevicePath,
        string normalImagePath, string syntheticImagePath, VolumeAllocationBitmap bitmap,
        int chunkSize, long maximumBytes) {
        if (chunkSize <= 0) {
            throw new ArgumentOutOfRangeException(nameof(chunkSize));
        }
        if (maximumBytes <= 0) {
            throw new ArgumentOutOfRangeException(nameof(maximumBytes));
        }

        using var source = OpenRawReadDevice(sourceDevicePath);
        using var normal = new FileStream(normalImagePath, FileMode.Open,
            FileAccess.Read, FileShare.ReadWrite | FileShare.Delete, 1,
            FileOptions.SequentialScan);
        using var synthetic = new FileStream(syntheticImagePath, FileMode.Open,
            FileAccess.Read, FileShare.ReadWrite | FileShare.Delete, 1,
            FileOptions.SequentialScan);
        var length = QueryLength(source);
        var sectorSize = QuerySectorSize(source);
        if ((length != bitmap.Length) ||
            (sectorSize != bitmap.SectorSize) ||
            (normal.Length != length) || (synthetic.Length != length)) {
            throw new InvalidDataException(
                "source, bitmap, and devicefs view geometry do not agree");
        }

        var normalBytes = new byte[chunkSize];
        var syntheticBytes = new byte[chunkSize];
        var summary = new ComparisonSummary();
        var comparisonLength = Math.Min(length, maximumBytes);
        for (var offset = 0L; offset < comparisonLength;) {
            var current = (int)Math.Min((long)chunkSize, comparisonLength - offset);
            if ((ReadManaged(normal, normalBytes, current) != current) ||
                (ReadManaged(synthetic, syntheticBytes, current) != current)) {
                throw new EndOfStreamException(
                    "a devicefs view completed a sequential read short");
            }

            var sourceBytes = ReadDeviceAt(
                source, length, sectorSize, offset, current);
            CompareChunk(sourceBytes, normalBytes, syntheticBytes,
                offset, current, bitmap, summary);
            offset += current;
        }

        return summary;
    }

    public static ComparisonSummary CompareRange(string sourceDevicePath,
        string normalImagePath, string syntheticImagePath,
        VolumeAllocationBitmap bitmap, long offset, int requestedLength) {
        if ((offset < 0) || (requestedLength < 0) ||
            (offset > bitmap.Length)) {
            throw new ArgumentOutOfRangeException();
        }

        var count = (int)Math.Min((long)requestedLength,
            bitmap.Length - offset);
        var source = ReadDeviceAt(sourceDevicePath, offset, requestedLength);
        var normalBytes = new byte[requestedLength];
        var syntheticBytes = new byte[requestedLength];
        using var normal = new FileStream(normalImagePath, FileMode.Open,
            FileAccess.Read, FileShare.ReadWrite | FileShare.Delete, 1,
            FileOptions.RandomAccess);
        using var synthetic = new FileStream(syntheticImagePath, FileMode.Open,
            FileAccess.Read, FileShare.ReadWrite | FileShare.Delete, 1,
            FileOptions.RandomAccess);
        normal.Position = offset;
        synthetic.Position = offset;
        var normalCount = normal.Read(normalBytes, 0, requestedLength);
        var syntheticCount = synthetic.Read(syntheticBytes, 0, requestedLength);
        if ((normalCount != count) || (syntheticCount != count)) {
            throw new InvalidDataException(
                "a devicefs view returned an unexpected targeted-read length");
        }

        if (source.Length != count) {
            throw new InvalidDataException(
                "source volume returned an unexpected targeted-read length");
        }

        var summary = new ComparisonSummary();
        CompareChunk(source, normalBytes, syntheticBytes,
            offset, count, bitmap, summary);
        return summary;
    }
}

// The virtual-disk handle keeps the attachment alive. `Detach` reports native
// failures before closing the handle; `Dispose` also closes partially prepared
// attachments. No permanent attachment is requested, so closing the final
// handle releases the attachment even during exception unwinding.
// https://learn.microsoft.com/en-us/windows/win32/api/virtdisk/ne-virtdisk-attach_virtual_disk_flag
public sealed unsafe class DeviceFsTestDisk : IDisposable {
    private SafeFileHandle handle;
    private DeviceFsTestDisk() {}
    public uint DiskNumber { get; private set; }
    public long DiskLength { get; private set; }
    public uint PartitionNumber { get; private set; }
    public long Offset { get; private set; }
    public long Size { get; private set; }
    public string VolumeName { get; private set; }

    // The data partition remains visible to Mount Manager but never receives
    // a default drive letter, including its first arrival before formatting.
    // https://learn.microsoft.com/en-us/windows/win32/api/winioctl/ns-winioctl-partition_information_gpt
    private const GPT_ATTRIBUTES NoDefaultDriveLetter =
        GPT_ATTRIBUTES.GPT_BASIC_DATA_ATTRIBUTE_NO_DRIVE_LETTER;
    // One MiB is the fixture's partition-alignment policy, matching the earlier
    // Windows partitioning-tool layout.
    private const long PartitionAlignment = 1024 * 1024;
    private const uint GptEntryCount = 128;

    private static void RequireSuccess(WIN32_ERROR result, string operation) {
        if (result != WIN32_ERROR.ERROR_SUCCESS) {
            throw DeviceFsTestNative.Win32Error(operation, (int)result);
        }
    }
    private static byte[] Bytes<T>(T value) where T : unmanaged {
        return MemoryMarshal.AsBytes(MemoryMarshal.CreateReadOnlySpan(ref value, 1)).ToArray();
    }
    private static T Structure<T>(byte[] buffer, int offset = 0) where T : unmanaged {
        if (offset + sizeof(T) > buffer.Length) {
            throw new InvalidDataException("The native disk layout was incomplete.");
        }
        return MemoryMarshal.Read<T>(buffer.AsSpan(offset));
    }
    private static byte[] Control(SafeFileHandle disk, uint code,
        byte[] input = null, int outputSize = 0) {
        var result = DeviceFsTestNative.Control(disk, code, input, outputSize);
        return result.Buffer.AsSpan(0, checked((int)result.BytesReturned)).ToArray();
    }

    // Volume devices arrive asynchronously after disk changes.
    // Device notifications wake the query instead of guessed sleeps. Registration
    // precedes the change, and each query precedes waiting, so an early arrival
    // cannot be lost. The timeout reports failure rather than hanging a test.
    // https://learn.microsoft.com/en-us/windows/win32/api/cfgmgr32/nf-cfgmgr32-cm_register_notification
    private sealed class VolumeArrivals : IDisposable {
        private readonly System.Threading.AutoResetEvent changed =
            new System.Threading.AutoResetEvent(false);
        private readonly PCM_NOTIFY_CALLBACK callback;
        private HCMNOTIFICATION registration;
        public VolumeArrivals() {
            callback = (notification, context, action, data, size) => {
                try {
                    if (action == CM_NOTIFY_ACTION.CM_NOTIFY_ACTION_DEVICEINTERFACEARRIVAL) { changed.Set(); }
                    return 0;
                } catch {
                    // A managed exception cannot cross the native callback.
                    return (uint)WIN32_ERROR.ERROR_GEN_FAILURE;
                }
            };
            var filter = new CM_NOTIFY_FILTER {
                cbSize = (uint)sizeof(CM_NOTIFY_FILTER),
                FilterType = CM_NOTIFY_FILTER_TYPE.CM_NOTIFY_FILTER_TYPE_DEVICEINTERFACE,
            };
            filter.u.DeviceInterface.ClassGuid = GUID_DEVINTERFACE_VOLUME;
            var registered = new HCMNOTIFICATION();
            var result = CM_Register_Notification(&filter, null, callback, &registered);
            registration = registered;
            if (result != CONFIGRET.CR_SUCCESS) {
                changed.Dispose();
                throw new IOException($"CM_Register_Notification failed with CONFIGRET 0x{result:x8}.");
            }
        }
        public T Wait<T>(Func<T> query, string description) where T : class {
            var elapsed = System.Diagnostics.Stopwatch.StartNew();
            var timeout = TimeSpan.FromSeconds(30);
            for (;;) {
                var result = query();
                if (result != null) { return result; }
                var remaining = timeout - elapsed.Elapsed;
                if ((remaining <= TimeSpan.Zero) || !changed.WaitOne(remaining)) {
                    throw new IOException($"Timed out waiting for {description}.");
                }
            }
        }
        public void Dispose() {
            var result = CM_Unregister_Notification(registration);
            if (result != CONFIGRET.CR_SUCCESS) {
                throw new IOException($"CM_Unregister_Notification failed with CONFIGRET 0x{result:x8}.");
            }
            GC.KeepAlive(callback);
            changed.Dispose();
        }
    }

    // https://learn.microsoft.com/en-us/windows/win32/api/virtdisk/ns-virtdisk-virtual_storage_type
    private static VIRTUAL_STORAGE_TYPE TypeFor(string path) => new VIRTUAL_STORAGE_TYPE {
        DeviceId = System.IO.Path.GetExtension(path).Equals(".vhdx",
            StringComparison.OrdinalIgnoreCase)
                ? VIRTUAL_STORAGE_TYPE_DEVICE_VHDX : VIRTUAL_STORAGE_TYPE_DEVICE_VHD,
        VendorId = VIRTUAL_STORAGE_TYPE_VENDOR_MICROSOFT,
    };

    // Create a blank disk, attach it writable, and create one data partition.
    // `fixedDisk` requests full allocation; otherwise the image grows on demand.
    // The returned object retains the attachment until `Detach` or `Dispose`.
    public static DeviceFsTestDisk Create(string path, ulong size, bool fixedDisk) {
        var type = TypeFor(path);
        var parameters = new CREATE_VIRTUAL_DISK_PARAMETERS {
            Version = CREATE_VIRTUAL_DISK_VERSION.CREATE_VIRTUAL_DISK_VERSION_2,
        };
        parameters.Version2.MaximumSize = size;
        parameters.Version2.SectorSizeInBytes = 512;
        RequireSuccess(CreateVirtualDisk(type, path, 0, default,
            fixedDisk ? CREATE_VIRTUAL_DISK_FLAG.CREATE_VIRTUAL_DISK_FLAG_FULL_PHYSICAL_ALLOCATION : 0,
            0, parameters, null, out var handle), $"CreateVirtualDisk '{path}'");
        return Prepare(handle, false, true, path);
    }
    // Attach an existing test image and locate its single data partition.
    public static DeviceFsTestDisk Open(string path, bool readOnly = true) {
        var type = TypeFor(path);
        var parameters = new OPEN_VIRTUAL_DISK_PARAMETERS {
            Version = OPEN_VIRTUAL_DISK_VERSION.OPEN_VIRTUAL_DISK_VERSION_2,
        };
        parameters.Version2.ReadOnly = readOnly;
        RequireSuccess(OpenVirtualDisk(type, path, 0, 0, parameters,
            out var handle), $"OpenVirtualDisk '{path}'");
        return Prepare(handle, readOnly, false, path);
    }
    private static DeviceFsTestDisk Prepare(SafeFileHandle handle,
        bool readOnly, bool initialize, string path) {
        var result = new DeviceFsTestDisk { handle = handle };
        try {
            using var arrivals = new VolumeArrivals();
            RequireSuccess(AttachVirtualDisk(handle, default,
                ATTACH_VIRTUAL_DISK_FLAG.ATTACH_VIRTUAL_DISK_FLAG_NO_DRIVE_LETTER |
                    (readOnly ? ATTACH_VIRTUAL_DISK_FLAG.ATTACH_VIRTUAL_DISK_FLAG_READ_ONLY : 0),
                0, null, null), $"AttachVirtualDisk '{path}'");
            uint pathSize = 0;
            var status = GetVirtualDiskPhysicalPath(handle, ref pathSize, null);
            if (status != WIN32_ERROR.ERROR_INSUFFICIENT_BUFFER) {
                RequireSuccess(status, "GetVirtualDiskPhysicalPath size");
            }
            var physicalPathBuffer = new char[checked((int)pathSize / sizeof(char))];
            RequireSuccess(GetVirtualDiskPhysicalPath(handle, ref pathSize,
                physicalPathBuffer), $"GetVirtualDiskPhysicalPath '{path}'");
            var physicalPath = new string(physicalPathBuffer).TrimEnd('\0');
            // The virtual-disk handle identifies the physical disk to initialize,
            // so concurrent fixture creation cannot select another test's disk.
            using var disk = CreateFile(physicalPath,
                (uint)GENERIC_ACCESS_RIGHTS.GENERIC_READ | (initialize ? (uint)GENERIC_ACCESS_RIGHTS.GENERIC_WRITE : 0),
                FILE_SHARE_MODE.FILE_SHARE_READ | FILE_SHARE_MODE.FILE_SHARE_WRITE | FILE_SHARE_MODE.FILE_SHARE_DELETE,
                null, FILE_CREATION_DISPOSITION.OPEN_EXISTING, 0, null);
            if (disk.IsInvalid) {
                RequireSuccess((WIN32_ERROR)Marshal.GetLastWin32Error(), $"Open '{physicalPath}'");
            }
            var number = Structure<STORAGE_DEVICE_NUMBER>(Control(
                disk, IOCTL_STORAGE_GET_DEVICE_NUMBER, outputSize: sizeof(STORAGE_DEVICE_NUMBER)));
            result.DiskNumber = number.DeviceNumber;
            result.DiskLength = Structure<GET_LENGTH_INFORMATION>(Control(
                disk, IOCTL_DISK_GET_LENGTH_INFO, outputSize: sizeof(GET_LENGTH_INFORMATION))).Length;
            if (initialize) {
                var create = new CREATE_DISK { PartitionStyle = PARTITION_STYLE.PARTITION_STYLE_GPT };
                create.Gpt = new CREATE_DISK_GPT { DiskId = Guid.NewGuid(), MaxPartitionCount = GptEntryCount };
                Control(disk, IOCTL_DISK_CREATE_DISK, Bytes(create));
            }
            // `IOCTL_DISK_UPDATE_PROPERTIES` makes Windows reread the initialized
            // partition table before its usable range is queried.
            // https://learn.microsoft.com/en-us/windows/win32/api/winioctl/ni-winioctl-ioctl_disk_update_properties
            if (initialize) { Control(disk, IOCTL_DISK_UPDATE_PROPERTIES); }
            var (header, partitions) = ReadLayout(disk);
            if (header.PartitionStyle != (uint)PARTITION_STYLE.PARTITION_STYLE_GPT) {
                throw new InvalidDataException($"'{path}' does not contain a GPT disk.");
            }
            if (initialize) {
                var start = header.Gpt.StartingUsableOffset;
                foreach (var existing in partitions) {
                    start = Math.Max(start, existing.StartingOffset + existing.PartitionLength);
                }
                start = checked((start + PartitionAlignment - 1) /
                    PartitionAlignment * PartitionAlignment);
                var end = (header.Gpt.StartingUsableOffset + header.Gpt.UsableLength) /
                    PartitionAlignment * PartitionAlignment;
                var data = new PARTITION_INFORMATION_EX {
                    PartitionStyle = PARTITION_STYLE.PARTITION_STYLE_GPT,
                    StartingOffset = start, PartitionLength = end - start,
                    RewritePartition = true,
                };
                data.Gpt.PartitionType = PARTITION_BASIC_DATA_GUID;
                data.Gpt.PartitionId = Guid.NewGuid();
                data.Gpt.Attributes = NoDefaultDriveLetter;
                "DeviceFs test".AsSpan().CopyTo(data.Gpt.Name.AsSpan());
                partitions.Add(data);
                header.PartitionCount = (uint)partitions.Count;
                var headerSize = Marshal.OffsetOf<DRIVE_LAYOUT_INFORMATION_EX>(nameof(DRIVE_LAYOUT_INFORMATION_EX.PartitionEntry)).ToInt32();
                var entrySize = sizeof(PARTITION_INFORMATION_EX);
                var updated = new byte[headerSize + partitions.Count * entrySize];
                Bytes(header).AsSpan(0, headerSize).CopyTo(updated);
                for (int index = 0; index < partitions.Count; ++index) {
                    var partition = partitions[index];
                    partition.RewritePartition = true;
                    Bytes(partition).CopyTo(updated, headerSize + index * entrySize);
                }
                Control(disk, IOCTL_DISK_SET_DRIVE_LAYOUT_EX, updated);
                Control(disk, IOCTL_DISK_UPDATE_PROPERTIES);
                (_, partitions) = ReadLayout(disk);
            }
            var selected = partitions.FindAll(
                partition => partition.Gpt.PartitionType == PARTITION_BASIC_DATA_GUID);
            if (selected.Count != 1) {
                throw new InvalidDataException($"'{path}' does not contain one basic-data partition.");
            }
            result.PartitionNumber = selected[0].PartitionNumber;
            result.Offset = selected[0].StartingOffset;
            result.Size = selected[0].PartitionLength;
            result.VolumeName = arrivals.Wait(() => result.FindVolumeName(),
                $"volume arrival for '{path}'");
            return result;
        } catch {
            result.Dispose();
            throw;
        }
    }
    private static (DRIVE_LAYOUT_INFORMATION_EX Header, List<PARTITION_INFORMATION_EX> Partitions) ReadLayout(
        SafeFileHandle disk) {
        // `PartitionEntry` begins the variable-length partition list. The
        // generated structure includes its first element, but only the bytes
        // preceding that member belong to the fixed header.
        // https://learn.microsoft.com/en-us/windows/win32/api/winioctl/ns-winioctl-drive_layout_information_ex
        var headerSize = Marshal.OffsetOf<DRIVE_LAYOUT_INFORMATION_EX>(nameof(DRIVE_LAYOUT_INFORMATION_EX.PartitionEntry)).ToInt32();
        var entrySize = sizeof(PARTITION_INFORMATION_EX);
        // Both the created fixtures and VhdxViewer use 128 GPT entries.
        var layout = Control(disk, IOCTL_DISK_GET_DRIVE_LAYOUT_EX,
            outputSize: headerSize + checked((int)GptEntryCount) * entrySize);
        var header = new DRIVE_LAYOUT_INFORMATION_EX();
        layout.AsSpan(0, headerSize).CopyTo(
            MemoryMarshal.AsBytes(MemoryMarshal.CreateSpan(ref header, 1)));
        var partitions = new List<PARTITION_INFORMATION_EX>();
        for (int index = 0; index < header.PartitionCount; ++index) {
            var entry = Structure<PARTITION_INFORMATION_EX>(layout, headerSize + index * entrySize);
            if (entry.PartitionLength != 0) { partitions.Add(entry); }
        }
        return (header, partitions);
    }

    // Volume GUID names are available before formatting. Matching the storage
    // device number identifies the partition without opening a filesystem root,
    // which would require a filesystem already recognized by Windows.
    // https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-findfirstvolumew
    // https://learn.microsoft.com/en-us/windows/win32/api/winioctl/ni-winioctl-ioctl_storage_get_device_number
    private string FindVolumeName() {
        var name = new char[MAX_PATH + 1];
        var search = FindFirstVolume(name);
        if (search.IsInvalid) {
            RequireSuccess((WIN32_ERROR)Marshal.GetLastWin32Error(), "FindFirstVolume");
        }
        try {
            do {
                using var volume = CreateFile(new string(name).TrimEnd('\0').TrimEnd('\\'),
                    0, FILE_SHARE_MODE.FILE_SHARE_READ | FILE_SHARE_MODE.FILE_SHARE_WRITE |
                        FILE_SHARE_MODE.FILE_SHARE_DELETE, null,
                    FILE_CREATION_DISPOSITION.OPEN_EXISTING, 0, null);
                if (!volume.IsInvalid) {
                    var number = new byte[sizeof(STORAGE_DEVICE_NUMBER)];
                    if (DeviceIoControl(volume, IOCTL_STORAGE_GET_DEVICE_NUMBER,
                            null, number, out var returned, null) &&
                        returned == number.Length &&
                        Structure<STORAGE_DEVICE_NUMBER>(number).DeviceNumber == DiskNumber &&
                        Structure<STORAGE_DEVICE_NUMBER>(number).PartitionNumber == PartitionNumber) {
                        return new string(name).TrimEnd('\0');
                    }
                }
            } while (FindNextVolume((HANDLE)search.DangerousGetHandle(), name));
            var status = Marshal.GetLastWin32Error();
            if (status != (int)WIN32_ERROR.ERROR_NO_MORE_FILES) { RequireSuccess((WIN32_ERROR)status, "FindNextVolume"); }
            return null;
        } finally { search.Dispose(); }
    }
    public void Mount(string directory) {
        if (!SetVolumeMountPoint(directory.TrimEnd('\\') + "\\", VolumeName)) {
            RequireSuccess((WIN32_ERROR)Marshal.GetLastWin32Error(), $"SetVolumeMountPoint '{directory}'");
        }
    }
    public void Unmount(string directory) {
        if (!DeleteVolumeMountPoint(directory.TrimEnd('\\') + "\\")) {
            RequireSuccess((WIN32_ERROR)Marshal.GetLastWin32Error(), $"DeleteVolumeMountPoint '{directory}'");
        }
    }
    public void Detach() {
        if (!handle.IsClosed) {
            RequireSuccess(DetachVirtualDisk(handle, 0, 0), "DetachVirtualDisk");
            handle.Dispose();
        }
    }
    public void Dispose() { handle.Dispose(); }
}
