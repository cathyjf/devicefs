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

    // `kCFRunLoopDefaultMode` is an exported pointer, not a function.
    private static readonly IntPtr DefaultMode = Marshal.ReadIntPtr(
        NativeLibrary.GetExport(NativeLibrary.Load(CoreFoundationLibrary),
            "kCFRunLoopDefaultMode"));

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate void DiskAppearedCallback(IntPtr disk, IntPtr context);

    [DllImport(DiskArbitrationLibrary)]
    private static extern IntPtr DASessionCreate(IntPtr allocator);

    [DllImport(DiskArbitrationLibrary)]
    private static extern void DARegisterDiskAppearedCallback(
        IntPtr session, IntPtr match, DiskAppearedCallback callback, IntPtr context);

    [DllImport(DiskArbitrationLibrary)]
    private static extern IntPtr DADiskGetBSDName(IntPtr disk);

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

    public static void WaitForDisk(string bsdName) {
        var runLoop = CFRunLoopGetCurrent();
        DiskAppearedCallback appeared = (disk, _) => {
            if (Marshal.PtrToStringUTF8(DADiskGetBSDName(disk)) == bsdName) {
                CFRunLoopStop(runLoop);
            }
        };
        var session = DASessionCreate(IntPtr.Zero);
        if (session == IntPtr.Zero) {
            throw new InvalidOperationException(
                $"failed to create Disk Arbitration session for '{bsdName}'");
        }
        try {
            // Registration also reports disks that have already appeared, so
            // attachment can finish before this callback is registered.
            DARegisterDiskAppearedCallback(session, IntPtr.Zero,
                appeared, IntPtr.Zero);
            DASessionScheduleWithRunLoop(session, runLoop, DefaultMode);
            CFRunLoopRun();
        } finally {
            DASessionUnscheduleFromRunLoop(session, runLoop, DefaultMode);
            CFRelease(session);
            GC.KeepAlive(appeared);
        }
    }
}
