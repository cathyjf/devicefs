// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using Microsoft.Win32.SafeHandles;

public sealed class VolumeIdentity {
    public string Label { get; }
    public long Length { get; }
    public uint DiskNumber { get; }
    public long DiskStartingOffset { get; }
    public long DiskExtentLength { get; }

    internal VolumeIdentity(string label, long length, uint diskNumber,
        long diskStartingOffset, long diskExtentLength) {
        Label = label;
        Length = length;
        DiskNumber = diskNumber;
        DiskStartingOffset = diskStartingOffset;
        DiskExtentLength = diskExtentLength;
    }
}

public sealed class NtfsBitmap {
    private readonly byte[] bits;

    public long Length { get; }
    public uint SectorSize { get; }
    public uint ClusterSize { get; }
    public long ClusterCount { get; }

    internal NtfsBitmap(long length, uint sectorSize, uint clusterSize,
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

public static class DeviceFsTestNative {
    private const uint GenericRead = 0x80000000;
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
    private const int VolumeBitmapHeaderSize = 16;
    private const int VolumeBitmapStructureSize = 24;
    private const int VolumeDiskExtentsSize = 32;
    private const int RetrievalPointerBaseSize = 8;
    private const int RetrievalPointersHeaderSize = 16;
    private const int RetrievalPointersExtentSize = 16;
    private const int FileStreamInfo = 7;
    private const int FileStreamInfoHeaderSize = 24;
    private const int InitialStreamInformationSize = 4096;
    private const int ErrorHandleEof = 38;
    private const int ErrorInsufficientBuffer = 122;
    private const int ErrorMoreData = 234;
    private const int ErrorNotAllAssigned = 1300;
    private const string BackupPrivilegeName = "SeBackupPrivilege";

    // These control codes are normally produced by Windows SDK CTL_CODE macros.
    private const uint FsctlGetNtfsVolumeData = 0x00090064;
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

    private readonly struct IoResult {
        public byte[] Buffer { get; }
        public uint BytesReturned { get; }

        public IoResult(byte[] buffer, uint bytesReturned) {
            Buffer = buffer;
            BytesReturned = bytesReturned;
        }
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
    private static extern SafeFileHandle CreateFile(string fileName,
        uint desiredAccess, uint shareMode, IntPtr securityAttributes,
        uint creationDisposition, uint flagsAndAttributes,
        IntPtr templateFile);

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
    private static extern bool DeviceIoControl(SafeFileHandle device,
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

    private static Win32Exception Win32Error(string operation, int error) {
        var description = new Win32Exception(error).Message;
        return new Win32Exception(
            error, $"{operation}: {description} ({error})");
    }

    private static Win32Exception LastError(string operation) {
        return Win32Error(operation, Marshal.GetLastWin32Error());
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

    private static IoResult Control(SafeFileHandle device, uint code,
        byte[] input, int outputSize) {
        var output = outputSize == 0 ? null : new byte[outputSize];
        if (!DeviceIoControl(device, code, input,
                (uint)(input == null ? 0 : input.Length), output,
                (uint)outputSize, out var returned, IntPtr.Zero)) {
            var error = Marshal.GetLastWin32Error();
            throw Win32Error($"DeviceIoControl 0x{code:X8} failed", error);
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

    private static void ValidateNtfsGeometry(long length, uint sectorSize,
        NtfsVolumeData ntfs) {
        if ((ntfs.TotalClusters <= 0) || (ntfs.BytesPerCluster == 0) ||
            (ntfs.BytesPerSector != sectorSize) ||
            ((length % sectorSize) != 0) ||
            ((ntfs.BytesPerCluster % sectorSize) != 0) ||
            (ntfs.TotalClusters > (length / ntfs.BytesPerCluster))) {
            throw new InvalidDataException("invalid NTFS volume geometry");
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

    private static NtfsBitmap QueryBitmap(SafeFileHandle device,
        bool requireReadOnly) {
        QueryVolumeInformation(device, out _, out var fileSystemName,
            out var fileSystemFlags);
        if (!string.Equals(fileSystemName, "NTFS",
                StringComparison.OrdinalIgnoreCase)) {
            throw new InvalidDataException("volume is not NTFS");
        }
        if (requireReadOnly && ((fileSystemFlags & FileReadOnlyVolume) == 0)) {
            throw new InvalidDataException(
                "volume is not reported read-only");
        }

        var length = QueryLength(device);
        var sectorSize = QuerySectorSize(device);
        var ntfs = QueryNtfsData(device);
        ValidateNtfsGeometry(length, sectorSize, ntfs);

        var retrievalBase = Control(device, FsctlGetRetrievalPointerBase,
            null, RetrievalPointerBaseSize);
        if ((retrievalBase.BytesReturned < RetrievalPointerBaseSize) ||
            (BitConverter.ToInt64(retrievalBase.Buffer, 0) != 0)) {
            throw new InvalidDataException(
                "NTFS LCN 0 does not begin at device offset 0");
        }

        var bitmapBytes = checked((ntfs.TotalClusters / 8) +
            ((ntfs.TotalClusters % 8) == 0 ? 0 : 1));
        var requiredSize = checked(VolumeBitmapHeaderSize + bitmapBytes);
        if (requiredSize > int.MaxValue) {
            throw new InvalidDataException("NTFS allocation bitmap is too large");
        }

        var input = new byte[sizeof(long)];
        var result = Control(device, FsctlGetVolumeBitmap, input,
            Math.Max(VolumeBitmapStructureSize, (int)requiredSize));
        if ((result.BytesReturned < requiredSize) ||
            (BitConverter.ToInt64(result.Buffer, 0) != 0) ||
            (BitConverter.ToInt64(result.Buffer, 8) != ntfs.TotalClusters)) {
            throw new InvalidDataException(
                "FSCTL_GET_VOLUME_BITMAP returned incomplete or inconsistent data");
        }

        var bits = new byte[checked((int)bitmapBytes)];
        Buffer.BlockCopy(
            result.Buffer, VolumeBitmapHeaderSize, bits, 0, bits.Length);
        return new NtfsBitmap(length, sectorSize, ntfs.BytesPerCluster,
            ntfs.TotalClusters, bits);
    }

    private static VolumeIdentity InspectHandle(SafeFileHandle device) {
        QueryVolumeInformation(device, out var label, out var fileSystemName,
            out _);
        if (!string.Equals(fileSystemName, "NTFS",
                StringComparison.OrdinalIgnoreCase)) {
            throw new InvalidDataException("volume is not NTFS");
        }

        var length = QueryLength(device);
        var extents = Control(device, IoctlVolumeGetVolumeDiskExtents,
            null, VolumeDiskExtentsSize);
        if ((extents.BytesReturned < VolumeDiskExtentsSize) ||
            (BitConverter.ToUInt32(extents.Buffer, 0) != 1)) {
            throw new InvalidDataException(
                "test volume does not have exactly one disk extent");
        }

        return new VolumeIdentity(label, length,
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

    public static NtfsBitmap GetNtfsBitmap(string devicePath,
        bool requireReadOnly) {
        using var device = OpenDevice(devicePath);
        return QueryBitmap(device, requireReadOnly);
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

    public static long[] GetDifferingBlockOffsets(string firstDevicePath,
        string secondDevicePath, int blockSize) {
        if (blockSize <= 0) {
            throw new ArgumentOutOfRangeException(nameof(blockSize));
        }

        using var first = OpenRawReadDevice(firstDevicePath);
        using var second = OpenRawReadDevice(secondDevicePath);
        var length = QueryLength(first);
        if (QueryLength(second) != length) {
            throw new InvalidDataException("device lengths differ");
        }

        var firstSectorSize = QuerySectorSize(first);
        var secondSectorSize = QuerySectorSize(second);
        const int blocksPerRead = 256;
        var readSize = checked(blockSize * blocksPerRead);
        var result = new List<long>();
        for (var offset = 0L; offset < length; offset += readSize) {
            var count = (int)Math.Min((long)readSize, length - offset);
            var firstBytes = ReadDeviceAt(
                first, length, firstSectorSize, offset, count);
            var secondBytes = ReadDeviceAt(
                second, length, secondSectorSize, offset, count);
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
        byte[] synthetic, long offset, int count, NtfsBitmap bitmap,
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

    public static ComparisonSummary CompareViews(string sourceDevicePath,
        string normalImagePath, string syntheticImagePath, NtfsBitmap bitmap,
        int chunkSize) {
        if (chunkSize <= 0) {
            throw new ArgumentOutOfRangeException(nameof(chunkSize));
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
        for (var offset = 0L; offset < length;) {
            var current = (int)Math.Min((long)chunkSize, length - offset);
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
        NtfsBitmap bitmap, long offset, int requestedLength) {
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
