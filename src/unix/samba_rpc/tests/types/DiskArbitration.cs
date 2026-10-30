// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices;
using System.Security;

namespace DeviceFs.Tests;

public static class DiskArbitration {
    private sealed class MediaHandle : SafeHandle {
        private readonly string description;

        public MediaHandle(string bsdName) : this(
            IOBSDNameMatching(0, 0, bsdName), $"BSD name '{bsdName}'") {}

        private MediaHandle(IntPtr matching, string description)
            : base(IntPtr.Zero, true) {
            this.description = description;
            if (matching == IntPtr.Zero) {
                throw new OutOfMemoryException();
            }
            SetHandle((IntPtr)IOServiceGetMatchingService(0, matching));
            if (IsInvalid) {
                throw new InvalidOperationException(
                    $"failed to find IOMedia for {description}");
            }
        }

        public static MediaHandle ForImage(string path) {
            var url = SecurityElement.Escape(new Uri(path).AbsoluteUri);
            var matching = IOCFUnserialize($"""
                <dict>
                  <key>IOProviderClass</key><string>IOMedia</string>
                  <key>IOPropertyMatch</key>
                  <dict><key>Whole</key><true/></dict>
                  <key>IOParentMatch</key>
                  <dict>
                    <key>IOProviderClass</key>
                    <string>AppleDiskImageDevice</string>
                    <key>IOPropertyMatch</key>
                    <dict><key>DiskImageURL</key><string>{url}</string></dict>
                  </dict>
                </dict>
                """, IntPtr.Zero, 0, IntPtr.Zero);
            return new MediaHandle(matching, $"image '{path}'");
        }

        public string DevicePath {
            get {
                const uint kCFStringEncodingUTF8 = 0x08000100;
                using var key = CFStringCreateWithCString(
                    IntPtr.Zero, "BSD Name", kCFStringEncodingUTF8);
                if (key.IsInvalid) {
                    throw new OutOfMemoryException();
                }
                using var name = IORegistryEntrySearchCFProperty(
                    Value, "IOService", key, IntPtr.Zero, 0);
                if (name.IsInvalid) {
                    throw new InvalidOperationException(
                        $"failed to get a BSD name for {description}");
                }
                var characters = new char[CFStringGetLength(name)];
                CFStringGetCharacters(name, new CFRange(0, characters.Length),
                    characters);
                return $"/dev/{new string(characters)}";
            }
        }

        public uint Value => (uint)handle;
        public override bool IsInvalid => handle == IntPtr.Zero;
        protected override bool ReleaseHandle() => IOObjectRelease(Value) == 0;
    }

    private sealed class CFObject : SafeHandle {
        public CFObject() : base(IntPtr.Zero, true) {}
        public override bool IsInvalid => handle == IntPtr.Zero;
        protected override bool ReleaseHandle() {
            CFRelease(handle);
            return true;
        }
    }

    private const string DiskArbitrationLibrary =
        "/System/Library/Frameworks/DiskArbitration.framework/DiskArbitration";
    private const string CoreFoundationLibrary =
        "/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation";
    private const string IOKitLibrary =
        "/System/Library/Frameworks/IOKit.framework/IOKit";

    // `kCFRunLoopDefaultMode` is an exported pointer, not a function.
    private static readonly IntPtr DefaultMode = Marshal.ReadIntPtr(
        NativeLibrary.GetExport(NativeLibrary.Load(CoreFoundationLibrary),
            "kCFRunLoopDefaultMode"));

    // Disk Arbitration accepts an IOKit matching dictionary under this key.
    // It is exported by the framework and declared in DiskArbitrationPrivate.h.
    private static readonly IntPtr MediaMatchKey = Marshal.ReadIntPtr(
        NativeLibrary.GetExport(NativeLibrary.Load(DiskArbitrationLibrary),
            "kDADiskDescriptionMediaMatchKey"));

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate void DiskAppearedCallback(IntPtr disk, IntPtr context);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate void IdleCallback(IntPtr context);

    [DllImport(DiskArbitrationLibrary)]
    private static extern IntPtr DASessionCreate(IntPtr allocator);

    [DllImport(DiskArbitrationLibrary)]
    private static extern void DARegisterDiskAppearedCallback(
        IntPtr session, IntPtr match, DiskAppearedCallback callback, IntPtr context);

    [DllImport(DiskArbitrationLibrary)]
    private static extern void DARegisterIdleCallback(
        IntPtr session, IdleCallback callback, IntPtr context);

    [DllImport(DiskArbitrationLibrary)]
    private static extern void DASessionScheduleWithRunLoop(
        IntPtr session, IntPtr runLoop, IntPtr mode);

    [DllImport(DiskArbitrationLibrary)]
    private static extern void DASessionUnscheduleFromRunLoop(
        IntPtr session, IntPtr runLoop, IntPtr mode);

    [DllImport(CoreFoundationLibrary)]
    private static extern IntPtr CFRunLoopGetCurrent();

    [DllImport(CoreFoundationLibrary)]
    private static extern void CFRunLoopRun();

    [DllImport(CoreFoundationLibrary)]
    private static extern void CFRunLoopStop(IntPtr runLoop);

    [DllImport(CoreFoundationLibrary)]
    private static extern void CFRelease(IntPtr value);

    [DllImport(CoreFoundationLibrary)]
    private static extern IntPtr CFDictionaryCreate(IntPtr allocator,
        IntPtr[] keys, IntPtr[] values, nint count,
        IntPtr keyCallbacks, IntPtr valueCallbacks);

    [DllImport(CoreFoundationLibrary)]
    private static extern CFObject CFStringCreateWithCString(IntPtr allocator,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string text, uint encoding);

    [DllImport(CoreFoundationLibrary)]
    private static extern nuint CFGetTypeID(CFObject value);

    [DllImport(CoreFoundationLibrary)]
    private static extern nuint CFNumberGetTypeID();

    [DllImport(CoreFoundationLibrary)]
    [return: MarshalAs(UnmanagedType.I1)]
    private static extern bool CFNumberGetValue(
        CFObject number, nint type, out long value);

    [DllImport(IOKitLibrary)]
    private static extern IntPtr IOCFUnserialize(
        [MarshalAs(UnmanagedType.LPUTF8Str)] string text,
        IntPtr allocator, uint options, IntPtr errorString);

    [DllImport(IOKitLibrary)]
    private static extern uint IOObjectIsEqualTo(uint left, uint right);

    [DllImport(CoreFoundationLibrary)]
    private static extern nint CFStringGetLength(CFObject text);

    [DllImport(CoreFoundationLibrary)]
    private static extern void CFStringGetCharacters(CFObject text,
        CFRange range, [Out, MarshalAs(UnmanagedType.LPArray,
            ArraySubType = UnmanagedType.U2)] char[] characters);

    [StructLayout(LayoutKind.Sequential)]
    private readonly struct CFRange(nint location, nint length) {
        public readonly nint Location = location;
        public readonly nint Length = length;
    }

    [DllImport(IOKitLibrary)]
    private static extern IntPtr IOBSDNameMatching(uint mainPort, uint options,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string bsdName);

    [DllImport(IOKitLibrary)]
    private static extern uint IOServiceGetMatchingService(
        uint mainPort, IntPtr matching);

    [DllImport(IOKitLibrary)]
    private static extern int IORegistryEntryGetRegistryEntryID(
        uint entry, out ulong entryId);

    [DllImport(IOKitLibrary)]
    private static extern IntPtr IORegistryEntryIDMatching(ulong entryId);

    [DllImport(IOKitLibrary)]
    private static extern int IOObjectRelease(uint entry);

    [DllImport(IOKitLibrary)]
    private static extern CFObject IORegistryEntrySearchCFProperty(
        uint entry, [MarshalAs(UnmanagedType.LPUTF8Str)] string plane,
        CFObject key, IntPtr allocator, uint options);

    public static IEnumerable<string> DescribeDisk(string bsdName) {
        using var media = new MediaHandle(bsdName);
        var error = IORegistryEntryGetRegistryEntryID(media.Value, out var id);
        yield return error == 0
            ? $"IOMedia BSD name '{bsdName}', registry ID=0x{id:x}"
            : $"failed to get IOMedia registry ID: IOKit error 0x{error:x8}";

        // The major/minor numbers identify the device node. `IOStorageFamily`
        // searches the `IOMedia` and its `IOService` parents for `owner-*` when
        // creating that node, so those values can be compared with `stat`.
        const uint kCFStringEncodingUTF8 = 0x08000100;
        const nint kCFNumberSInt64Type = 4;
        const uint kIORegistryIterateRecursively = 1;
        const uint kIORegistryIterateParents = 2;
        foreach (var name in new[] {
                "BSD Major", "BSD Minor",
                "owner-uid", "owner-gid", "owner-mode"}) {
            using var key = CFStringCreateWithCString(
                IntPtr.Zero, name, kCFStringEncodingUTF8);
            if (key.IsInvalid) {
                throw new OutOfMemoryException();
            }
            using var property = IORegistryEntrySearchCFProperty(
                media.Value, "IOService", key, IntPtr.Zero,
                kIORegistryIterateRecursively | kIORegistryIterateParents);
            if (property.IsInvalid) {
                yield return $"{name}=unavailable";
            } else if (CFGetTypeID(property) != CFNumberGetTypeID() ||
                !CFNumberGetValue(
                    property, kCFNumberSInt64Type, out var value)) {
                yield return $"{name}=unavailable numeric value " +
                    $"(CF type {CFGetTypeID(property)})";
            } else {
                yield return name == "owner-mode"
                    ? $"{name}=0{Convert.ToString(value, 8)}"
                    : $"{name}={value}";
            }
        }
    }

    public static string FindImageDevice(string imagePath) {
        using var media = MediaHandle.ForImage(imagePath);
        return media.DevicePath;
    }

    // Keep the selected image open until Samba exits, so its BSD name cannot
    // be reused between this lookup and Samba's open.
    public static FileStream OpenImage(string imagePath) {
        using var media = MediaHandle.ForImage(imagePath);
        var devicePath = media.DevicePath;
        WaitForDisk(media, Path.GetFileName(devicePath));
        var file = File.OpenRead(devicePath);
        try {
            using var current = MediaHandle.ForImage(imagePath);
            if (IOObjectIsEqualTo(media.Value, current.Value) == 0) {
                throw new IOException(
                    $"the attached image '{imagePath}' changed while " +
                    $"opening '{devicePath}'");
            }
            return file;
        } catch {
            file.Dispose();
            throw;
        }
    }

    private static void WaitForDisk(MediaHandle media, string bsdName) {
        var runLoop = CFRunLoopGetCurrent();
        var diskAppeared = false;
        DiskAppearedCallback appeared = (_, _) => diskAppeared = true;
        IdleCallback idle = _ => {
            if (diskAppeared) {
                CFRunLoopStop(runLoop);
            }
        };

        // BSD names can be reused while Disk Arbitration still has the old
        // disk in its list. Matching the current IOMedia's registry ID prevents
        // that old disk's appearance notification from satisfying this wait.
        var mediaMatch = IntPtr.Zero;
        var match = IntPtr.Zero;
        var session = IntPtr.Zero;
        try {
            var error = IORegistryEntryGetRegistryEntryID(
                media.Value, out var entryId);
            if (error != 0) {
                throw new InvalidOperationException(
                    $"failed to get IOMedia registry ID for '{bsdName}' " +
                    $"(IOKit error 0x{error:x8})");
            }
            mediaMatch = IORegistryEntryIDMatching(entryId);
            if (mediaMatch == IntPtr.Zero) {
                throw new OutOfMemoryException();
            }
            match = CFDictionaryCreate(IntPtr.Zero,
                new[] { MediaMatchKey }, new[] { mediaMatch }, 1,
                IntPtr.Zero, IntPtr.Zero);
            if (match == IntPtr.Zero) {
                throw new OutOfMemoryException();
            }
            session = DASessionCreate(IntPtr.Zero);
            if (session == IntPtr.Zero) {
                throw new InvalidOperationException(
                    $"failed to create Disk Arbitration session for '{bsdName}'");
            }
            // Appearance identifies the target disk; the subsequent idle
            // notification waits for Disk Arbitration's pending processing.
            // Registering `idle` first also covers an already-idle daemon:
            // registering `appeared` queues existing disk appearances followed
            // by an idle notification.
            DARegisterIdleCallback(session, idle, IntPtr.Zero);
            // Registration also reports disks that have already appeared, so
            // attachment can finish before this callback is registered.
            DARegisterDiskAppearedCallback(session, match, appeared, IntPtr.Zero);
            DASessionScheduleWithRunLoop(session, runLoop, DefaultMode);
            CFRunLoopRun();
        } finally {
            if (session != IntPtr.Zero) {
                DASessionUnscheduleFromRunLoop(session, runLoop, DefaultMode);
                CFRelease(session);
            }
            if (match != IntPtr.Zero) {
                CFRelease(match);
            }
            if (mediaMatch != IntPtr.Zero) {
                CFRelease(mediaMatch);
            }
            GC.KeepAlive(appeared);
            GC.KeepAlive(idle);
        }
    }
}
