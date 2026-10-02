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

public static class DeviceFsTestNative {
    private const uint GenericRead = 0x80000000;
    private const uint GenericWrite = 0x40000000;
    private const uint TokenQuery = 0x00000008;
    private const uint TokenAdjustPrivileges = 0x00000020;
    private const uint SePrivilegeEnabled = 0x00000002;
    private const uint FileShareRead = 0x00000001;
    private const uint FileShareWrite = 0x00000002;
    private const uint FileShareDelete = 0x00000004;
    private const uint OpenExisting = 3;
    private const uint SecurityIdentification = 0x00010000;
    private const uint SecuritySqosPresent = 0x00100000;
    private const uint FileFlagOpenReparsePoint = 0x00200000;
    private const uint FileFlagBackupSemantics = 0x02000000;
    private const uint FileReadOnlyVolume = 0x00080000;
    private const uint MemCommit = 0x00001000;
    private const uint MemReserve = 0x00002000;
    private const uint MemRelease = 0x00008000;
    private const uint PageReadWrite = 0x04;
    private const uint FileBegin = 0;
    private const int DiskLengthInformationSize = 8;
    private const int DiskGeometrySize = 24;
    private const int DiskGeometrySectorSizeOffset = 20;
    private const int NtfsVolumeDataSize = 96;
    private const int RefsVolumeDataSize = 152;
    private const int VolumeBitmapHeaderSize = 16;
    private const int VolumeBitmapStructureSize = 24;
    private const int VolumeDiskExtentsSize = 32;
    private const int RetrievalPointerBaseSize = 8;
    private const int RetrievalPointersHeaderSize = 16;
    private const int RetrievalPointersExtentSize = 16;
    private const int FileStreamInfo = 7;
    private const int FileAttributeTagInfo = 9;
    private const int FileFullDirectoryInfo = 14;
    private const int FileFullDirectoryRestartInfo = 15;
    private const int FileStreamInfoHeaderSize = 24;
    private const int InitialStreamInformationSize = 4096;
    private const int DirectoryInformationBufferSize = 64 * 1024;
    private const int ErrorNoMoreFiles = 18;
    private const int ErrorHandleEof = 38;
    private const int ErrorInsufficientBuffer = 122;
    private const int ErrorMoreData = 234;
    private const int ErrorNotAllAssigned = 1300;
    private const string BackupPrivilegeName = "SeBackupPrivilege";
    private const ushort KeyEvent = 0x0001;
    private const uint LeftCtrlPressed = 0x0008;

    [StructLayout(LayoutKind.Sequential)]
    private struct KeyEventRecord {
        public int KeyDown;
        public ushort RepeatCount;
        public ushort VirtualKeyCode;
        public ushort VirtualScanCode;
        public ushort UnicodeChar;
        public uint ControlKeyState;
    }

    // Only the key-event member of INPUT_RECORD's union is needed here.
    // Sequential layout supplies the padding before the DWORD-aligned member.
    // https://learn.microsoft.com/en-us/windows/console/input-record-str
    [StructLayout(LayoutKind.Sequential)]
    private struct InputRecord {
        public ushort EventType;
        public KeyEventRecord Key;
    }

    // These control codes are normally produced by Windows SDK CTL_CODE macros.
    private const uint FsctlGetNtfsVolumeData = 0x00090064;
    private const uint FsctlGetRefsVolumeData = 0x000902D8;
    private const uint FsctlGetVolumeBitmap = 0x0009006F;
    private const uint FsctlGetRetrievalPointers = 0x00090073;
    private const uint FsctlAllowExtendedDasdIo = 0x00090083;
    private const uint FsctlGetRetrievalPointerBase = 0x00090234;
    private const uint IoctlDiskGetDriveGeometry = 0x00070000;
    private const uint IoctlDiskGetLengthInfo = 0x0007405C;
    private const uint IoctlVolumeGetVolumeDiskExtents = 0x00560000;

    [StructLayout(LayoutKind.Sequential)]
    private struct NtfsVolumeData {
        public long VolumeSerialNumber;
        public long NumberSectors;
        public long TotalClusters;
        public long FreeClusters;
        public long TotalReserved;
        public uint BytesPerSector;
        public uint BytesPerCluster;
        public uint BytesPerFileRecordSegment;
        public uint ClustersPerFileRecordSegment;
        public long MftValidDataLength;
        public long MftStartLcn;
        public long Mft2StartLcn;
        public long MftZoneStart;
        public long MftZoneEnd;
    }

    // `REFS_VOLUME_DATA_BUFFER` also contains version, serial-number and
    // reserved fields. The geometry query only reads these three fields.
    [StructLayout(LayoutKind.Explicit, Size = RefsVolumeDataSize)]
    private struct RefsVolumeData {
        [FieldOffset(32)]
        public long TotalClusters;

        [FieldOffset(56)]
        public uint BytesPerSector;

        [FieldOffset(60)]
        public uint BytesPerCluster;
    }

    internal readonly struct IoResult {
        public byte[] Buffer { get; }
        public uint BytesReturned { get; }

        public IoResult(byte[] buffer, uint bytesReturned) {
            Buffer = buffer;
            BytesReturned = bytesReturned;
        }
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct FileAttributeTagInformation {
        public FileAttributes FileAttributes;
        public uint ReparseTag;
    }

    // FILE_FULL_DIR_INFO ends its fixed header immediately before FileName.
    [StructLayout(LayoutKind.Explicit, Size = 68)]
    private struct FileFullDirectoryInformationHeader {
        [FieldOffset(0)]
        public uint NextEntryOffset;

        [FieldOffset(56)]
        public FileAttributes FileAttributes;

        [FieldOffset(60)]
        public uint FileNameLength;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct Luid {
        public uint LowPart;
        public int HighPart;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct LuidAndAttributes {
        public Luid Luid;
        public uint Attributes;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct TokenPrivileges {
        public uint PrivilegeCount;
        public LuidAndAttributes Privileges;
    }

    private sealed class BackupPrivilegeScope : IDisposable {
        private SafeAccessTokenHandle token;
        private TokenPrivileges previousState;

        public BackupPrivilegeScope() {
            if (!OpenProcessToken(GetCurrentProcess(),
                    TokenAdjustPrivileges | TokenQuery, out var openedToken)) {
                throw LastError("could not open the PowerShell process token");
            }

            try {
                if (!LookupPrivilegeValue(
                        null, BackupPrivilegeName, out var luid)) {
                    throw LastError("could not identify SeBackupPrivilege");
                }
                var requestedState = new TokenPrivileges {
                    PrivilegeCount = 1,
                    Privileges = new LuidAndAttributes {
                        Luid = luid,
                        Attributes = SePrivilegeEnabled,
                    },
                };
                Marshal.SetLastPInvokeError(0);
                if (!AdjustTokenPrivileges(openedToken, false,
                        ref requestedState,
                        (uint)Marshal.SizeOf<TokenPrivileges>(),
                        out previousState, out _)) {
                    throw LastError("could not enable SeBackupPrivilege");
                }
                var error = Marshal.GetLastWin32Error();
                if (error != 0) {
                    var operation = error == ErrorNotAllAssigned
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
                var restored = RestoreTokenPrivileges(
                    token, false, ref state, 0, IntPtr.Zero, IntPtr.Zero);
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

            SetHandle(VirtualAlloc(IntPtr.Zero, new UIntPtr((uint)size),
                MemCommit | MemReserve, PageReadWrite));
            if (IsInvalid) {
                throw LastError("VirtualAlloc failed");
            }
        }

        protected override bool ReleaseHandle() {
            return VirtualFree(handle, UIntPtr.Zero, MemRelease);
        }
    }

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode,
        SetLastError = true, EntryPoint = "CreateFileW")]
    internal static extern SafeFileHandle CreateFile(string fileName,
        uint desiredAccess, uint shareMode, IntPtr securityAttributes,
        uint creationDisposition, uint flagsAndAttributes,
        IntPtr templateFile);

    [DllImport("kernel32.dll", SetLastError = true,
        EntryPoint = "WriteConsoleInputW")]
    private static extern bool WriteConsoleInput(SafeFileHandle input,
        [In] InputRecord[] records, uint count, out uint written);

    [DllImport("kernel32.dll")]
    private static extern IntPtr GetCurrentProcess();

    [DllImport("advapi32.dll", SetLastError = true)]
    private static extern bool OpenProcessToken(IntPtr process,
        uint desiredAccess, out SafeAccessTokenHandle token);

    [DllImport("advapi32.dll", CharSet = CharSet.Unicode,
        EntryPoint = "LookupPrivilegeValueW", SetLastError = true)]
    private static extern bool LookupPrivilegeValue(string systemName,
        string name, out Luid luid);

    [DllImport("advapi32.dll", EntryPoint = "AdjustTokenPrivileges",
        SetLastError = true)]
    private static extern bool AdjustTokenPrivileges(
        SafeAccessTokenHandle token, bool disableAllPrivileges,
        ref TokenPrivileges newState, uint bufferLength,
        out TokenPrivileges previousState, out uint returnLength);

    [DllImport("advapi32.dll", EntryPoint = "AdjustTokenPrivileges",
        SetLastError = true)]
    private static extern bool RestoreTokenPrivileges(
        SafeAccessTokenHandle token, bool disableAllPrivileges,
        ref TokenPrivileges newState, uint bufferLength,
        IntPtr previousState, IntPtr returnLength);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool GetFileInformationByHandleEx(
        SafeFileHandle file, int informationClass, IntPtr information,
        uint bufferSize);

    [DllImport("kernel32.dll", SetLastError = true)]
    internal static extern bool DeviceIoControl(SafeFileHandle device,
        uint controlCode, [In] byte[] input, uint inputSize,
        [Out] byte[] output, uint outputSize, out uint bytesReturned,
        IntPtr overlapped);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true,
        EntryPoint = "GetVolumeNameForVolumeMountPointW")]
    private static extern bool GetVolumeNameForVolumeMountPoint(
        string mountPoint, StringBuilder volumeName, uint bufferLength);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true,
        EntryPoint = "GetVolumeInformationByHandleW")]
    private static extern bool GetVolumeInformationByHandle(
        SafeFileHandle volume, StringBuilder volumeName,
        uint volumeNameSize, IntPtr volumeSerialNumber,
        IntPtr maximumComponentLength, out uint fileSystemFlags,
        StringBuilder fileSystemName, uint fileSystemNameSize);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool SetFilePointerEx(SafeFileHandle file,
        long distance, out long newPosition, uint moveMethod);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool ReadFile(SafeFileHandle file,
        IntPtr buffer, uint bytesToRead, out uint bytesRead,
        IntPtr overlapped);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern IntPtr VirtualAlloc(IntPtr address, UIntPtr size,
        uint allocationType, uint protection);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool VirtualFree(IntPtr address, UIntPtr size,
        uint freeType);

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
        using (var input = CreateFile("CONIN$", GenericWrite,
                FileShareRead | FileShareWrite, IntPtr.Zero, OpenExisting,
                0, IntPtr.Zero)) {
            if (input.IsInvalid) {
                throw LastError("could not open console input to send Ctrl+C");
            }
            var records = new[] {
                new InputRecord {
                    EventType = KeyEvent,
                    Key = new KeyEventRecord {
                        KeyDown = 1,
                        RepeatCount = 1,
                        VirtualKeyCode = 'C',
                        UnicodeChar = '\u0003',
                        ControlKeyState = LeftCtrlPressed,
                    },
                },
            };
            if (!WriteConsoleInput(input, records, (uint)records.Length,
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
        var handle = CreateFile(DevicePath(path), GenericRead,
            FileShareRead | FileShareWrite | FileShareDelete,
            IntPtr.Zero, OpenExisting,
            SecuritySqosPresent | SecurityIdentification, IntPtr.Zero);
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
            _ = Control(device, FsctlAllowExtendedDasdIo, null, 0);
            return device;
        } catch {
            device.Dispose();
            throw;
        }
    }

    private static SafeFileHandle OpenObjectForExtents(string path) {
        var handle = CreateFile(path, GenericRead,
            FileShareRead | FileShareWrite | FileShareDelete,
            IntPtr.Zero, OpenExisting,
            SecuritySqosPresent | SecurityIdentification |
                FileFlagOpenReparsePoint | FileFlagBackupSemantics,
            IntPtr.Zero);
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
        if (!DeviceIoControl(device, code, input,
                (uint)(input == null ? 0 : input.Length), output,
                (uint)outputSize, out var returned, IntPtr.Zero)) {
            var error = Marshal.GetLastWin32Error();
            if (!allowMoreData || (error != ErrorMoreData)) {
                throw Win32Error($"DeviceIoControl 0x{code:X8} failed", error);
            }
        }

        return new IoResult(output ?? Array.Empty<byte>(), returned);
    }

    private static long QueryLength(SafeFileHandle device) {
        var result = Control(
            device, IoctlDiskGetLengthInfo, null, DiskLengthInformationSize);
        if (result.BytesReturned < DiskLengthInformationSize) {
            throw new InvalidDataException(
                "IOCTL_DISK_GET_LENGTH_INFO returned incomplete data");
        }

        var length = BitConverter.ToInt64(result.Buffer, 0);
        if (length < 0) {
            throw new InvalidDataException(
                "IOCTL_DISK_GET_LENGTH_INFO returned a negative length");
        }

        return length;
    }

    private static uint QuerySectorSize(SafeFileHandle device) {
        var result = Control(
            device, IoctlDiskGetDriveGeometry, null, DiskGeometrySize);
        if (result.BytesReturned < DiskGeometrySize) {
            throw new InvalidDataException(
                "IOCTL_DISK_GET_DRIVE_GEOMETRY returned incomplete data");
        }

        var sectorSize = BitConverter.ToUInt32(
            result.Buffer, DiskGeometrySectorSizeOffset);
        if (sectorSize == 0) {
            throw new InvalidDataException(
                "IOCTL_DISK_GET_DRIVE_GEOMETRY returned a zero sector size");
        }

        return sectorSize;
    }

    private static NtfsVolumeData QueryNtfsData(SafeFileHandle device) {
        if (Marshal.SizeOf<NtfsVolumeData>() != NtfsVolumeDataSize) {
            throw new InvalidOperationException(
                "unexpected NTFS_VOLUME_DATA_BUFFER layout");
        }

        var result = Control(
            device, FsctlGetNtfsVolumeData, null, NtfsVolumeDataSize);
        if (result.BytesReturned < NtfsVolumeDataSize) {
            throw new InvalidDataException(
                "FSCTL_GET_NTFS_VOLUME_DATA returned incomplete data");
        }

        return MemoryMarshal.Read<NtfsVolumeData>(result.Buffer);
    }

    private static (long ClusterCount, uint ClusterSize, uint SectorSize)
        QueryVolumeGeometry(SafeFileHandle device, string fileSystemName) {
        if (string.Equals(fileSystemName, "NTFS", StringComparison.OrdinalIgnoreCase)) {
            var ntfs = QueryNtfsData(device);
            return (ntfs.TotalClusters, ntfs.BytesPerCluster, ntfs.BytesPerSector);
        }
        if (string.Equals(fileSystemName, "ReFS", StringComparison.OrdinalIgnoreCase)) {
            var result = Control(device, FsctlGetRefsVolumeData, null, RefsVolumeDataSize);
            if (result.BytesReturned < RefsVolumeDataSize) {
                throw new InvalidDataException("FSCTL_GET_REFS_VOLUME_DATA returned incomplete data");
            }
            var refs = MemoryMarshal.Read<RefsVolumeData>(result.Buffer);
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
        var labelBuffer = new StringBuilder(261);
        var fileSystemBuffer = new StringBuilder(261);
        if (!GetVolumeInformationByHandle(device, labelBuffer,
                (uint)labelBuffer.Capacity, IntPtr.Zero,
                IntPtr.Zero, out fileSystemFlags,
                fileSystemBuffer, (uint)fileSystemBuffer.Capacity)) {
            throw LastError("GetVolumeInformationByHandleW failed");
        }

        label = labelBuffer.ToString();
        fileSystemName = fileSystemBuffer.ToString();
    }

    private static VolumeAllocationBitmap QueryBitmap(SafeFileHandle device,
        bool requireReadOnly, Action<string> log) {
        QueryVolumeInformation(device, out _, out var fileSystemName,
            out var fileSystemFlags);
        if (requireReadOnly && ((fileSystemFlags & FileReadOnlyVolume) == 0)) {
            throw new InvalidDataException(
                "volume is not reported read-only");
        }

        var length = QueryLength(device);
        var sectorSize = QuerySectorSize(device);
        var geometry = QueryVolumeGeometry(device, fileSystemName);
        ValidateVolumeGeometry(length, sectorSize, geometry);

        var retrievalBase = Control(device, FsctlGetRetrievalPointerBase,
            null, RetrievalPointerBaseSize);
        if ((retrievalBase.BytesReturned < RetrievalPointerBaseSize) ||
            (BitConverter.ToInt64(retrievalBase.Buffer, 0) != 0)) {
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
        var result = Control(device, FsctlGetVolumeBitmap, input,
            Math.Max(VolumeBitmapStructureSize, (int)requiredSize), allowMoreData: true);
        if ((result.BytesReturned < requiredSize) ||
            (BitConverter.ToInt64(result.Buffer, 0) != 0) ||
            (BitConverter.ToInt64(result.Buffer, 8) < geometry.ClusterCount)) {
            throw new InvalidDataException(
                "FSCTL_GET_VOLUME_BITMAP returned incomplete or inconsistent data");
        }

        var bits = new byte[checked((int)bitmapBytes)];
        Buffer.BlockCopy(
            result.Buffer, VolumeBitmapHeaderSize, bits, 0, bits.Length);
        log($"{fileSystemName}: {geometry.ClusterCount} clusters of " +
            $"{geometry.ClusterSize} bytes; bitmap reports " +
            $"{BitConverter.ToInt64(result.Buffer, 8)} clusters, " +
            $"returned {result.BytesReturned} bytes.");
        return new VolumeAllocationBitmap(length, sectorSize, geometry.ClusterSize,
            geometry.ClusterCount, bits);
    }

    private static VolumeIdentity InspectHandle(SafeFileHandle device) {
        QueryVolumeInformation(device, out var label, out var fileSystemName,
            out _);

        var length = QueryLength(device);
        var extents = Control(device, IoctlVolumeGetVolumeDiskExtents,
            null, VolumeDiskExtentsSize);
        if ((extents.BytesReturned < VolumeDiskExtentsSize) ||
            (BitConverter.ToUInt32(extents.Buffer, 0) != 1)) {
            throw new InvalidDataException(
                "test volume does not have exactly one disk extent");
        }

        return new VolumeIdentity(label, fileSystemName, length,
            BitConverter.ToUInt32(extents.Buffer, 8),
            BitConverter.ToInt64(extents.Buffer, 16),
            BitConverter.ToInt64(extents.Buffer, 24));
    }

    private static void ReadExact(SafeFileHandle device,
        AlignedBuffer buffer, int count) {
        if (!ReadFile(device, buffer.DangerousGetHandle(), (uint)count,
                out var read, IntPtr.Zero)) {
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

        if (!SetFilePointerEx(device, rawStart, out _, FileBegin)) {
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

        var result = new StringBuilder(50);
        if (!GetVolumeNameForVolumeMountPoint(
                mountRoot, result, (uint)result.Capacity)) {
            throw LastError("GetVolumeNameForVolumeMountPointW failed");
        }

        return result.ToString();
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
            var completed = DeviceIoControl(handle,
                FsctlGetRetrievalPointers, input, (uint)input.Length,
                output, (uint)output.Length, out var returned, IntPtr.Zero);
            var error = completed ? 0 : Marshal.GetLastWin32Error();
            if ((!completed) && (error == ErrorHandleEof)) {
                return;
            }
            if ((!completed) && (error != ErrorMoreData)) {
                throw Win32Error(
                    $"could not retrieve the extents of '{path}'", error);
            }
            if (returned < RetrievalPointersHeaderSize) {
                throw new InvalidDataException(
                    $"extent data for '{path}' was incomplete");
            }

            var extentCount = BitConverter.ToUInt32(output, 0);
            var required = checked(RetrievalPointersHeaderSize +
                ((long)extentCount * RetrievalPointersExtentSize));
            if (required > returned) {
                throw new InvalidDataException(
                    $"extent data for '{path}' was truncated");
            }

            var currentVcn = BitConverter.ToInt64(output, 8);
            for (var index = 0; index < extentCount; ++index) {
                var offset = checked(RetrievalPointersHeaderSize +
                    (index * RetrievalPointersExtentSize));
                var nextVcn = BitConverter.ToInt64(output, offset);
                var lcn = BitConverter.ToInt64(output, offset + sizeof(long));
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
            var nextEntryOffset = unchecked((uint)Marshal.ReadInt32(entry));
            var nameByteLength = unchecked(
                (uint)Marshal.ReadInt32(entry, sizeof(uint)));
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
            if (GetFileInformationByHandleEx(handle, FileStreamInfo,
                    buffer.DangerousGetHandle(), (uint)bufferSize)) {
                return ParseNamedDataStreams(
                    buffer.DangerousGetHandle(), bufferSize, path);
            }

            var error = Marshal.GetLastWin32Error();
            if (error == ErrorHandleEof) {
                return Array.Empty<string>();
            }
            if ((error != ErrorInsufficientBuffer) &&
                (error != ErrorMoreData)) {
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
        var size = Marshal.SizeOf<FileAttributeTagInformation>();
        using var buffer = new AlignedBuffer(size);
        if (!GetFileInformationByHandleEx(handle, FileAttributeTagInfo,
                buffer.DangerousGetHandle(), (uint)size)) {
            throw LastError($"could not query the attributes of '{path}'");
        }

        return Marshal.PtrToStructure<FileAttributeTagInformation>(
            buffer.DangerousGetHandle()).FileAttributes;
    }

    private static void EnumerateDirectoryTree(string path,
        SafeFileHandle directory, List<NativeFileSystemEntry> result) {
        var headerSize = Marshal.SizeOf<
            FileFullDirectoryInformationHeader>();
        using var buffer = new AlignedBuffer(DirectoryInformationBufferSize);
        var informationClass = FileFullDirectoryRestartInfo;
        while (true) {
            if (!GetFileInformationByHandleEx(directory, informationClass,
                    buffer.DangerousGetHandle(),
                    DirectoryInformationBufferSize)) {
                var error = Marshal.GetLastWin32Error();
                if (error == ErrorNoMoreFiles) {
                    return;
                }
                throw Win32Error(
                    $"could not enumerate the directory '{path}'", error);
            }
            informationClass = FileFullDirectoryInfo;

            var offset = 0;
            while (true) {
                var remaining = DirectoryInformationBufferSize - offset;
                if (remaining < headerSize) {
                    throw new InvalidDataException(
                        $"directory information for '{path}' was truncated");
                }

                var entryAddress = IntPtr.Add(
                    buffer.DangerousGetHandle(), offset);
                var entry = Marshal.PtrToStructure<
                    FileFullDirectoryInformationHeader>(entryAddress);
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
                        childPath, entry.FileAttributes));
                    if (((entry.FileAttributes & FileAttributes.Directory) !=
                            0) &&
                        ((entry.FileAttributes & FileAttributes.ReparsePoint) ==
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
            for (var i = startIndex; i < startIndex + length; ++i) {
                var absolute = checked(offset + i);
                if (normal[i] != source[i]) {
                    throw new InvalidDataException(
                        $"normal devicefs view differs at offset 0x{absolute:X}, " +
                        $"LCN {cluster}: source=0x{source[i]:X2}, " +
                        $"actual=0x{normal[i]:X2}");
                }

                var expected = allocated ? source[i] : (byte)0;
                if (synthetic[i] != expected) {
                    throw new InvalidDataException(
                        $"synthetic devicefs view differs at offset 0x{absolute:X}, " +
                        $"LCN {cluster}: allocated={allocated}, " +
                        $"source=0x{source[i]:X2}, expected=0x{expected:X2}, " +
                        $"actual=0x{synthetic[i]:X2}");
                }

                if (!allocated) {
                    ++summary.FreeBytes;
                }
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
public sealed class DeviceFsTestDisk : IDisposable {
    private SafeFileHandle handle;
    private DeviceFsTestDisk() {}
    public uint DiskNumber { get; private set; }
    public long DiskLength { get; private set; }
    public uint PartitionNumber { get; private set; }
    public long Offset { get; private set; }
    public long Size { get; private set; }
    public string VolumeName { get; private set; }

    private const uint GenericRead = 0x80000000;
    private const uint GenericWrite = 0x40000000;
    private const uint AttachReadOnly = 1;
    private const uint AttachNoDriveLetter = 2;
    private const uint CreateFullAllocation = 1;
    private const uint PartitionStyleGpt = 1;
    // The data partition remains visible to Mount Manager but never receives
    // a default drive letter, including its first arrival before formatting.
    // https://learn.microsoft.com/en-us/windows/win32/api/winioctl/ns-winioctl-partition_information_gpt
    private const ulong NoDefaultDriveLetter = 0x8000000000000000;
    // One MiB is the fixture's partition-alignment policy, matching the earlier
    // Windows partitioning-tool layout.
    private const long PartitionAlignment = 1024 * 1024;
    private const uint GptEntryCount = 128;
    private static readonly Guid MicrosoftVirtualDiskVendor =
        new Guid("ec984aec-a0f9-47e9-901f-71415a66345b");
    private static readonly Guid BasicDataPartition =
        new Guid("ebd0a0a2-b9e5-4433-87c0-68b6b72699c7");
    // These are the Windows SDK CTL_CODE values for the corresponding IOCTLs.
    private const uint IoctlDiskCreateDisk = 0x0007c058;
    private const uint IoctlDiskGetDriveLayoutEx = 0x00070050;
    private const uint IoctlDiskSetDriveLayoutEx = 0x0007c054;
    private const uint IoctlStorageGetDeviceNumber = 0x002d1080;
    private const uint IoctlDiskGetLengthInfo = 0x0007405c;
    private const uint IoctlDiskUpdateProperties = 0x00070140;

    [StructLayout(LayoutKind.Sequential)]
    private struct StorageType { public uint DeviceId; public Guid VendorId; }
    // Version 2 supports both VHD and VHDX. The nested structure preserves the
    // SDK union's alignment, including padding before `MaximumSize` and paths.
    // https://learn.microsoft.com/en-us/windows/win32/api/virtdisk/ns-virtdisk-create_virtual_disk_parameters
    [StructLayout(LayoutKind.Sequential)]
    private struct CreateVersion2 {
        public Guid UniqueId;
        public ulong MaximumSize;
        public uint BlockSize, SectorSize, PhysicalSectorSize;
        public IntPtr ParentPath, SourcePath;
        public uint OpenFlags;
        public StorageType ParentType, SourceType;
        public Guid ResiliencyGuid;
    }
    [StructLayout(LayoutKind.Sequential)]
    private struct CreateParameters {
        public uint Version;
        public CreateVersion2 Data;
    }
    [StructLayout(LayoutKind.Sequential)]
    private struct OpenParameters {
        public uint Version;
        public int GetInfoOnly, ReadOnly;
        public Guid ResiliencyGuid;
    }
    [StructLayout(LayoutKind.Sequential)]
    private struct CreateGpt {
        public uint Style;
        public Guid DiskId;
        public uint MaxPartitions;
    }
    // The GPT members occupy the union storage in these SDK structures.
    // Windows supplies the usable range; the data partition is aligned within
    // that range, leaving the GPT headers and partition tables untouched.
    // https://learn.microsoft.com/en-us/windows/win32/api/winioctl/ns-winioctl-drive_layout_information_ex
    // https://learn.microsoft.com/en-us/windows/win32/api/winioctl/ns-winioctl-partition_information_ex
    [StructLayout(LayoutKind.Sequential)]
    private struct LayoutHeader {
        public uint Style, Count;
        public Guid DiskId;
        public long UsableStart, UsableLength;
        public uint MaxPartitions;
    }
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct Partition {
        public uint Style;
        public ushort Ordinal;
        public long Start, Length;
        public uint Number;
        public byte Rewrite, Service;
        public Guid Type, Id;
        public ulong Attributes;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 36)] public string Name;
    }

    [DllImport("virtdisk.dll", CharSet = CharSet.Unicode)]
    private static extern uint CreateVirtualDisk(ref StorageType type,
        string path, uint access, IntPtr security, uint flags, uint providerFlags,
        ref CreateParameters parameters, IntPtr overlapped, out SafeFileHandle disk);
    [DllImport("virtdisk.dll", CharSet = CharSet.Unicode)]
    private static extern uint OpenVirtualDisk(ref StorageType type,
        string path, uint access, uint flags, ref OpenParameters parameters,
        out SafeFileHandle disk);
    [DllImport("virtdisk.dll")]
    private static extern uint AttachVirtualDisk(SafeFileHandle disk,
        IntPtr security, uint flags, uint providerFlags, IntPtr parameters,
        IntPtr overlapped);
    [DllImport("virtdisk.dll")]
    private static extern uint DetachVirtualDisk(SafeFileHandle disk,
        uint flags, uint providerFlags);
    [DllImport("virtdisk.dll", CharSet = CharSet.Unicode)]
    private static extern uint GetVirtualDiskPhysicalPath(SafeFileHandle disk,
        ref uint pathSize, StringBuilder path);
    [DllImport("kernel32.dll", EntryPoint = "SetVolumeMountPointW",
        CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool SetVolumeMountPoint(string mount, string volume);
    [DllImport("kernel32.dll", EntryPoint = "DeleteVolumeMountPointW",
        CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool DeleteVolumeMountPoint(string mount);

    private static void RequireSuccess(uint result, string operation) {
        if (result != 0) {
            throw DeviceFsTestNative.Win32Error(operation, (int)result);
        }
    }
    private static byte[] Bytes<T>(T value) where T : struct {
        var buffer = new byte[Marshal.SizeOf<T>()];
        var pinned = GCHandle.Alloc(buffer, GCHandleType.Pinned);
        try { Marshal.StructureToPtr(value, pinned.AddrOfPinnedObject(), false); }
        finally { pinned.Free(); }
        return buffer;
    }
    private static T Structure<T>(byte[] buffer, int offset = 0) where T : struct {
        if (offset + Marshal.SizeOf<T>() > buffer.Length) {
            throw new InvalidDataException("The native disk layout was incomplete.");
        }
        var pinned = GCHandle.Alloc(buffer, GCHandleType.Pinned);
        try { return Marshal.PtrToStructure<T>(IntPtr.Add(pinned.AddrOfPinnedObject(), offset)); }
        finally { pinned.Free(); }
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
        private readonly Notification callback;
        private IntPtr registration;
        private delegate uint Notification(IntPtr notification, IntPtr context,
            uint action, IntPtr data, uint size);
        // CM_NOTIFY_FILTER's union includes a MAX_DEVICE_ID_LEN (200) WCHAR
        // instance ID. Its size is therefore 16 header bytes plus 400 bytes.
        // https://learn.microsoft.com/en-us/windows/win32/api/cfgmgr32/ns-cfgmgr32-cm_notify_filter
        [StructLayout(LayoutKind.Explicit, Size = 416)]
        private struct Filter {
            [FieldOffset(0)] public uint Size;
            [FieldOffset(16)] public Guid InterfaceClass;
        }
        [DllImport("cfgmgr32.dll")]
        private static extern uint CM_Register_Notification(ref Filter filter,
            IntPtr context, Notification callback, out IntPtr registration);
        [DllImport("cfgmgr32.dll")]
        private static extern uint CM_Unregister_Notification(IntPtr registration);
        public VolumeArrivals() {
            callback = (notification, context, action, data, size) => {
                try {
                    if (action == 0) { changed.Set(); }
                    return 0;
                } catch {
                    // A managed exception cannot cross the native callback.
                    return 31; // ERROR_GEN_FAILURE
                }
            };
            var filter = new Filter {
                Size = (uint)Marshal.SizeOf<Filter>(),
                InterfaceClass = new Guid("53f5630d-b6bf-11d0-94f2-00a0c91efb8b"),
            };
            var result = CM_Register_Notification(ref filter, IntPtr.Zero,
                callback, out registration);
            if (result != 0) {
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
            if (result != 0) {
                throw new IOException($"CM_Unregister_Notification failed with CONFIGRET 0x{result:x8}.");
            }
            GC.KeepAlive(callback);
            changed.Dispose();
        }
    }

    // Device IDs 2 and 3 select Microsoft's VHD and VHDX providers.
    // https://learn.microsoft.com/en-us/windows/win32/api/virtdisk/ns-virtdisk-virtual_storage_type
    private static StorageType TypeFor(string path) => new StorageType {
        DeviceId = System.IO.Path.GetExtension(path).Equals(".vhdx",
            StringComparison.OrdinalIgnoreCase) ? 3u : 2u,
        VendorId = MicrosoftVirtualDiskVendor,
    };

    // Create a blank disk, attach it writable, and create one data partition.
    // `fixedDisk` requests full allocation; otherwise the image grows on demand.
    // The returned object retains the attachment until `Detach` or `Dispose`.
    public static DeviceFsTestDisk Create(string path, ulong size, bool fixedDisk) {
        var type = TypeFor(path);
        var parameters = new CreateParameters {
            Version = 2,
            Data = new CreateVersion2 { MaximumSize = size, SectorSize = 512 },
        };
        RequireSuccess(CreateVirtualDisk(ref type, path, 0, IntPtr.Zero,
            fixedDisk ? CreateFullAllocation : 0, 0, ref parameters,
            IntPtr.Zero, out var handle), $"CreateVirtualDisk '{path}'");
        return Prepare(handle, false, true, path);
    }
    // Attach an existing test image and locate its single data partition.
    public static DeviceFsTestDisk Open(string path, bool readOnly = true) {
        var type = TypeFor(path);
        var parameters = new OpenParameters {
            Version = 2, ReadOnly = readOnly ? 1 : 0,
        };
        RequireSuccess(OpenVirtualDisk(ref type, path, 0, 0, ref parameters,
            out var handle), $"OpenVirtualDisk '{path}'");
        return Prepare(handle, readOnly, false, path);
    }
    private static DeviceFsTestDisk Prepare(SafeFileHandle handle,
        bool readOnly, bool initialize, string path) {
        var result = new DeviceFsTestDisk { handle = handle };
        try {
            using var arrivals = new VolumeArrivals();
            RequireSuccess(AttachVirtualDisk(handle, IntPtr.Zero,
                AttachNoDriveLetter | (readOnly ? AttachReadOnly : 0), 0,
                IntPtr.Zero, IntPtr.Zero), $"AttachVirtualDisk '{path}'");
            uint pathSize = 0;
            var status = GetVirtualDiskPhysicalPath(handle, ref pathSize, null);
            if (status != 122) { RequireSuccess(status, "GetVirtualDiskPhysicalPath size"); }
            var physicalPath = new StringBuilder(checked((int)pathSize / sizeof(char)));
            RequireSuccess(GetVirtualDiskPhysicalPath(handle, ref pathSize,
                physicalPath), $"GetVirtualDiskPhysicalPath '{path}'");
            // The virtual-disk handle identifies the physical disk to initialize,
            // so concurrent fixture creation cannot select another test's disk.
            using var disk = DeviceFsTestNative.CreateFile(physicalPath.ToString(),
                GenericRead | (initialize ? GenericWrite : 0), 7,
                IntPtr.Zero, 3, 0, IntPtr.Zero);
            if (disk.IsInvalid) {
                RequireSuccess((uint)Marshal.GetLastWin32Error(), $"Open '{physicalPath}'");
            }
            var number = Control(disk, IoctlStorageGetDeviceNumber, outputSize: 3 * sizeof(uint));
            result.DiskNumber = BitConverter.ToUInt32(number, sizeof(uint));
            result.DiskLength = BitConverter.ToInt64(
                Control(disk, IoctlDiskGetLengthInfo, outputSize: sizeof(long)), 0);
            if (initialize) {
                Control(disk, IoctlDiskCreateDisk, Bytes(new CreateGpt {
                    Style = PartitionStyleGpt, DiskId = Guid.NewGuid(),
                    MaxPartitions = GptEntryCount,
                }));
            }
            // `IOCTL_DISK_UPDATE_PROPERTIES` makes Windows reread the initialized
            // partition table before its usable range is queried.
            // https://learn.microsoft.com/en-us/windows/win32/api/winioctl/ni-winioctl-ioctl_disk_update_properties
            if (initialize) { Control(disk, IoctlDiskUpdateProperties); }
            var (header, partitions) = ReadLayout(disk);
            if (header.Style != PartitionStyleGpt) {
                throw new InvalidDataException($"'{path}' does not contain a GPT disk.");
            }
            if (initialize) {
                var start = header.UsableStart;
                foreach (var existing in partitions) {
                    start = Math.Max(start, existing.Start + existing.Length);
                }
                start = checked((start + PartitionAlignment - 1) /
                    PartitionAlignment * PartitionAlignment);
                var end = (header.UsableStart + header.UsableLength) /
                    PartitionAlignment * PartitionAlignment;
                partitions.Add(new Partition {
                    Style = PartitionStyleGpt, Start = start, Length = end - start,
                    Rewrite = 1,
                    Type = BasicDataPartition, Id = Guid.NewGuid(),
                    Attributes = NoDefaultDriveLetter, Name = "DeviceFs test",
                });
                header.Count = (uint)partitions.Count;
                var headerSize = Marshal.SizeOf<LayoutHeader>();
                var entrySize = Marshal.SizeOf<Partition>();
                var updated = new byte[headerSize + partitions.Count * entrySize];
                Bytes(header).CopyTo(updated, 0);
                for (int index = 0; index < partitions.Count; ++index) {
                    var partition = partitions[index];
                    partition.Rewrite = 1;
                    Bytes(partition).CopyTo(updated, headerSize + index * entrySize);
                }
                Control(disk, IoctlDiskSetDriveLayoutEx, updated);
                Control(disk, IoctlDiskUpdateProperties);
                (_, partitions) = ReadLayout(disk);
            }
            var selected = partitions.FindAll(
                partition => partition.Type == BasicDataPartition);
            if (selected.Count != 1) {
                throw new InvalidDataException($"'{path}' does not contain one basic-data partition.");
            }
            result.PartitionNumber = selected[0].Number;
            result.Offset = selected[0].Start;
            result.Size = selected[0].Length;
            result.VolumeName = arrivals.Wait(() => result.FindVolumeName(),
                $"volume arrival for '{path}'");
            return result;
        } catch {
            result.Dispose();
            throw;
        }
    }
    private static (LayoutHeader Header, List<Partition> Partitions) ReadLayout(
        SafeFileHandle disk) {
        var headerSize = Marshal.SizeOf<LayoutHeader>();
        var entrySize = Marshal.SizeOf<Partition>();
        // Both the created fixtures and VhdxViewer use 128 GPT entries.
        var layout = Control(disk, IoctlDiskGetDriveLayoutEx,
            outputSize: headerSize + checked((int)GptEntryCount) * entrySize);
        var header = Structure<LayoutHeader>(layout);
        var partitions = new List<Partition>();
        for (int index = 0; index < header.Count; ++index) {
            var entry = Structure<Partition>(layout, headerSize + index * entrySize);
            if (entry.Length != 0) { partitions.Add(entry); }
        }
        return (header, partitions);
    }
    [DllImport("kernel32.dll", EntryPoint = "FindFirstVolumeW",
        CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern IntPtr FindFirstVolume(StringBuilder name, uint size);
    [DllImport("kernel32.dll", EntryPoint = "FindNextVolumeW",
        CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool FindNextVolume(IntPtr search,
        StringBuilder name, uint size);
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool FindVolumeClose(IntPtr search);

    // Volume GUID names are available before formatting. Matching the storage
    // device number identifies the partition without opening a filesystem root,
    // which would require a filesystem already recognized by Windows.
    // https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-findfirstvolumew
    // https://learn.microsoft.com/en-us/windows/win32/api/winioctl/ni-winioctl-ioctl_storage_get_device_number
    private string FindVolumeName() {
        // A volume GUID name is \\?\Volume{36-character GUID}\ plus NUL.
        var name = new StringBuilder(50);
        var search = FindFirstVolume(name, (uint)name.Capacity);
        if (search == new IntPtr(-1)) {
            RequireSuccess((uint)Marshal.GetLastWin32Error(), "FindFirstVolume");
        }
        try {
            do {
                using var volume = DeviceFsTestNative.CreateFile(name.ToString().TrimEnd('\\'),
                    0, 7, IntPtr.Zero, 3, 0, IntPtr.Zero);
                if (!volume.IsInvalid) {
                    var number = new byte[3 * sizeof(uint)];
                    if (DeviceFsTestNative.DeviceIoControl(volume, IoctlStorageGetDeviceNumber,
                            null, 0, number, (uint)number.Length,
                            out var returned, IntPtr.Zero) &&
                        returned == number.Length &&
                        BitConverter.ToUInt32(number, sizeof(uint)) == DiskNumber &&
                        BitConverter.ToUInt32(number, 2 * sizeof(uint)) == PartitionNumber) {
                        return name.ToString();
                    }
                }
            } while (FindNextVolume(search, name, (uint)name.Capacity));
            var status = Marshal.GetLastWin32Error();
            if (status != 18) { RequireSuccess((uint)status, "FindNextVolume"); }
            return null;
        } finally { FindVolumeClose(search); }
    }
    public void Mount(string directory) {
        if (!SetVolumeMountPoint(directory.TrimEnd('\\') + "\\", VolumeName)) {
            RequireSuccess((uint)Marshal.GetLastWin32Error(), $"SetVolumeMountPoint '{directory}'");
        }
    }
    public void Unmount(string directory) {
        if (!DeleteVolumeMountPoint(directory.TrimEnd('\\') + "\\")) {
            RequireSuccess((uint)Marshal.GetLastWin32Error(), $"DeleteVolumeMountPoint '{directory}'");
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
