// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

using System;
using System.Runtime.InteropServices;

namespace DeviceFs.Tests;

public static class DiskArbitration {
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

    [DllImport(DiskArbitrationLibrary)]
    private static extern IntPtr DASessionCreate(IntPtr allocator);

    [DllImport(DiskArbitrationLibrary)]
    private static extern void DARegisterDiskAppearedCallback(
        IntPtr session, IntPtr match, DiskAppearedCallback callback, IntPtr context);

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

    public static void WaitForDisk(string bsdName) {
        var runLoop = CFRunLoopGetCurrent();
        DiskAppearedCallback appeared = (_, _) => CFRunLoopStop(runLoop);
        // BSD names can be reused while Disk Arbitration still has the old
        // disk in its list. Matching the current IOMedia's registry ID prevents
        // that old disk's appearance notification from satisfying this wait.
        var media = IOServiceGetMatchingService(0,
            IOBSDNameMatching(0, 0, bsdName));
        if (media == 0) {
            throw new InvalidOperationException(
                $"failed to find IOMedia for '{bsdName}'");
        }
        var mediaMatch = IntPtr.Zero;
        var match = IntPtr.Zero;
        var session = IntPtr.Zero;
        try {
            var error = IORegistryEntryGetRegistryEntryID(media, out var entryId);
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
            IOObjectRelease(media);
            GC.KeepAlive(appeared);
        }
    }
}
