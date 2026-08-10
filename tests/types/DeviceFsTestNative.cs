// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

using System;
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
    public long NonzeroFreeBytes { get; internal set; }
}

public static class DeviceFsTestNative {
    private const uint GenericRead = 0x80000000;
    private const uint FileShareRead = 0x00000001;
    private const uint FileShareWrite = 0x00000002;
    private const uint FileShareDelete = 0x00000004;
    private const uint OpenExisting = 3;
    private const uint SecurityIdentification = 0x00010000;
    private const uint SecuritySqosPresent = 0x00100000;
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

    // These control codes are normally produced by Windows SDK CTL_CODE macros.
    private const uint FsctlGetNtfsVolumeData = 0x00090064;
    private const uint FsctlGetVolumeBitmap = 0x0009006F;
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
        uint volumeNameSize, out uint volumeSerialNumber,
        out uint maximumComponentLength, out uint fileSystemFlags,
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

    private static IoResult Control(SafeFileHandle device, uint code,
        byte[] input, int outputSize) {
        var output = outputSize == 0 ? null : new byte[outputSize];
        uint returned;
        if (!DeviceIoControl(device, code, input,
                (uint)(input == null ? 0 : input.Length), output,
                (uint)outputSize, out returned, IntPtr.Zero)) {
            var error = Marshal.GetLastWin32Error();
            throw Win32Error(
                string.Format("DeviceIoControl 0x{0:X8} failed", code), error);
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
        out uint serialNumber, out uint fileSystemFlags) {
        var labelBuffer = new StringBuilder(261);
        var fileSystemBuffer = new StringBuilder(261);
        if (!GetVolumeInformationByHandle(device, labelBuffer,
                (uint)labelBuffer.Capacity, out serialNumber,
                out _, out fileSystemFlags,
                fileSystemBuffer, (uint)fileSystemBuffer.Capacity)) {
            throw LastError("GetVolumeInformationByHandleW failed");
        }

        label = labelBuffer.ToString();
        fileSystemName = fileSystemBuffer.ToString();
    }

    private static NtfsBitmap QueryBitmap(SafeFileHandle device,
        bool requireReadOnly) {
        string fileSystemName;
        uint fileSystemFlags;
        QueryVolumeInformation(device, out _, out fileSystemName,
            out _, out fileSystemFlags);
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
        string label;
        string fileSystemName;
        QueryVolumeInformation(device, out label, out fileSystemName,
            out _, out _);
        if (!string.Equals(fileSystemName, "NTFS",
                StringComparison.OrdinalIgnoreCase)) {
            throw new InvalidDataException("volume is not NTFS");
        }

        var length = QueryLength(device);
        var sectorSize = QuerySectorSize(device);
        var ntfs = QueryNtfsData(device);
        ValidateNtfsGeometry(length, sectorSize, ntfs);

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

    private static void Seek(SafeFileHandle device, long offset) {
        if (!SetFilePointerEx(device, offset, out _, FileBegin)) {
            throw LastError("SetFilePointerEx failed");
        }
    }

    private static void ReadExact(SafeFileHandle device,
        AlignedBuffer buffer, int count) {
        uint read;
        if (!ReadFile(device, buffer.DangerousGetHandle(), (uint)count,
                out read, IntPtr.Zero)) {
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
        using (var buffer = new AlignedBuffer(rawLength)) {
            if ((buffer.DangerousGetHandle().ToInt64() % sectorSize) != 0) {
                throw new InvalidOperationException(
                    "VirtualAlloc did not satisfy volume alignment");
            }

            Seek(device, rawStart);
            ReadExact(device, buffer, rawLength);
            var result = new byte[available];
            Marshal.Copy(IntPtr.Add(buffer.DangerousGetHandle(),
                checked((int)(offset - rawStart))), result, 0, available);
            return result;
        }
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
        using (var device = OpenDevice(volumeName)) {
            return InspectHandle(device);
        }
    }

    public static NtfsBitmap GetNtfsBitmap(string devicePath,
        bool requireReadOnly) {
        using (var device = OpenDevice(devicePath)) {
            return QueryBitmap(device, requireReadOnly);
        }
    }

    public static byte[] ReadDeviceAt(string devicePath,
        long offset, int count) {
        using (var device = OpenRawReadDevice(devicePath)) {
            return ReadDeviceAt(device, QueryLength(device),
                QuerySectorSize(device), offset, count);
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
        byte[] zeroed, long offset, int count, NtfsBitmap bitmap,
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
                    throw new InvalidDataException(string.Format(
                        "normal devicefs view differs at offset 0x{0:X}, " +
                        "LCN {1}: source=0x{2:X2}, actual=0x{3:X2}",
                        absolute, cluster, source[i], normal[i]));
                }

                var expected = allocated ? source[i] : (byte)0;
                if (zeroed[i] != expected) {
                    throw new InvalidDataException(string.Format(
                        "zeroing devicefs view differs at offset 0x{0:X}, " +
                        "LCN {1}: allocated={2}, source=0x{3:X2}, " +
                        "expected=0x{4:X2}, actual=0x{5:X2}",
                        absolute, cluster, allocated, source[i],
                        expected, zeroed[i]));
                }

                if (!allocated) {
                    ++summary.FreeBytes;
                    if (source[i] != 0) {
                        ++summary.NonzeroFreeBytes;
                    }
                }
            }

            position = next;
        }

        summary.BytesCompared += count;
    }

    public static ComparisonSummary CompareViews(string sourceDevicePath,
        string normalImagePath, string zeroedImagePath, NtfsBitmap bitmap,
        int chunkSize) {
        if (chunkSize <= 0) {
            throw new ArgumentOutOfRangeException(nameof(chunkSize));
        }

        using (var source = OpenRawReadDevice(sourceDevicePath))
        using (var normal = new FileStream(normalImagePath, FileMode.Open,
            FileAccess.Read, FileShare.ReadWrite | FileShare.Delete, 1,
            FileOptions.SequentialScan))
        using (var zeroed = new FileStream(zeroedImagePath, FileMode.Open,
            FileAccess.Read, FileShare.ReadWrite | FileShare.Delete, 1,
            FileOptions.SequentialScan)) {
            var length = QueryLength(source);
            var sectorSize = QuerySectorSize(source);
            if ((length != bitmap.Length) ||
                (sectorSize != bitmap.SectorSize) ||
                (normal.Length != length) || (zeroed.Length != length)) {
                throw new InvalidDataException(
                    "source, bitmap, and devicefs view geometry do not agree");
            }

            var normalBytes = new byte[chunkSize];
            var zeroedBytes = new byte[chunkSize];
            var summary = new ComparisonSummary();
            for (var offset = 0L; offset < length;) {
                var current = (int)Math.Min((long)chunkSize, length - offset);
                if ((ReadManaged(normal, normalBytes, current) != current) ||
                    (ReadManaged(zeroed, zeroedBytes, current) != current)) {
                    throw new EndOfStreamException(
                        "a devicefs view completed a sequential read short");
                }

                var sourceBytes = ReadDeviceAt(
                    source, length, sectorSize, offset, current);
                CompareChunk(sourceBytes, normalBytes, zeroedBytes,
                    offset, current, bitmap, summary);
                offset += current;
            }

            return summary;
        }
    }

    public static ComparisonSummary CompareRange(string sourceDevicePath,
        string normalImagePath, string zeroedImagePath,
        NtfsBitmap bitmap, long offset, int requestedLength) {
        if ((offset < 0) || (requestedLength < 0) ||
            (offset > bitmap.Length)) {
            throw new ArgumentOutOfRangeException();
        }

        var count = (int)Math.Min((long)requestedLength,
            bitmap.Length - offset);
        var source = ReadDeviceAt(sourceDevicePath, offset, requestedLength);
        var normalBytes = new byte[requestedLength];
        var zeroedBytes = new byte[requestedLength];
        using (var normal = new FileStream(normalImagePath, FileMode.Open,
            FileAccess.Read, FileShare.ReadWrite | FileShare.Delete, 1,
            FileOptions.RandomAccess))
        using (var zeroed = new FileStream(zeroedImagePath, FileMode.Open,
            FileAccess.Read, FileShare.ReadWrite | FileShare.Delete, 1,
            FileOptions.RandomAccess)) {
            normal.Position = offset;
            zeroed.Position = offset;
            var normalCount = ReadManaged(normal, normalBytes, requestedLength);
            var zeroedCount = ReadManaged(zeroed, zeroedBytes, requestedLength);
            if ((normalCount != count) || (zeroedCount != count)) {
                throw new InvalidDataException(
                    "a devicefs view returned an unexpected targeted-read length");
            }
        }

        if (source.Length != count) {
            throw new InvalidDataException(
                "source volume returned an unexpected targeted-read length");
        }

        var summary = new ComparisonSummary();
        CompareChunk(source, normalBytes, zeroedBytes,
            offset, count, bitmap, summary);
        return summary;
    }
}
