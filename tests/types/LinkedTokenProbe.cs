// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Security.Principal;
using Microsoft.Win32.SafeHandles;

namespace DeviceFs.Tests;

public static class LinkedTokenProbe {
    public enum ElevationType {
        Default = 1,
        Full,
        Limited
    }

    public sealed record Identity(string Account, string Sid, ElevationType Elevation);

    private enum TokenInformationClass {
        User = 1,
        ElevationType = 18,
        LinkedToken = 19
    }

    private const uint TokenQuery = 0x0008;
    private const int ErrorInsufficientBuffer = 122;

    [DllImport("kernel32.dll", ExactSpelling = true)]
    private static extern IntPtr GetCurrentProcess();

    [DllImport("advapi32.dll", ExactSpelling = true, SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool OpenProcessToken(
        IntPtr process, uint desiredAccess, out SafeAccessTokenHandle token);

    [DllImport("advapi32.dll", ExactSpelling = true, SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GetTokenInformation(
        SafeAccessTokenHandle token, TokenInformationClass informationClass,
        IntPtr information, uint informationLength, out uint returnLength);

    [DllImport("advapi32.dll", ExactSpelling = true, SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GetTokenInformation(
        SafeAccessTokenHandle token, TokenInformationClass informationClass,
        out ElevationType information, uint informationLength, out uint returnLength);

    [DllImport("advapi32.dll", ExactSpelling = true, SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GetTokenInformation(
        SafeAccessTokenHandle token, TokenInformationClass informationClass,
        out IntPtr information, uint informationLength, out uint returnLength);

    private static Win32Exception NativeError(string operation, int error) {
        return new Win32Exception(error,
            $"{operation}: Windows error 0x{error:x8}: {new Win32Exception(error).Message}");
    }

    private static SafeAccessTokenHandle OpenCurrentToken() {
        if (!OpenProcessToken(GetCurrentProcess(), TokenQuery, out var token)) {
            throw NativeError("OpenProcessToken", Marshal.GetLastWin32Error());
        }
        return token;
    }

    private static SecurityIdentifier ReadUserSid(SafeAccessTokenHandle token) {
        GetTokenInformation(token, TokenInformationClass.User, IntPtr.Zero, 0, out var length);
        var error = Marshal.GetLastWin32Error();
        if (error != ErrorInsufficientBuffer) {
            throw NativeError("GetTokenInformation(TokenUser) size query", error);
        }

        var buffer = Marshal.AllocHGlobal(checked((int)length));
        try {
            if (!GetTokenInformation(token, TokenInformationClass.User, buffer, length, out length)) {
                throw NativeError("GetTokenInformation(TokenUser)",
                    Marshal.GetLastWin32Error());
            }
            // TOKEN_USER begins with SID_AND_ATTRIBUTES, whose first member is
            // the SID pointer. SecurityIdentifier copies the SID before the
            // buffer is freed.
            return new SecurityIdentifier(Marshal.ReadIntPtr(buffer));
        } finally {
            Marshal.FreeHGlobal(buffer);
        }
    }

    private static Identity Describe(SafeAccessTokenHandle token) {
        var sid = ReadUserSid(token);
        var account = AccountName(sid);
        if (!GetTokenInformation(token, TokenInformationClass.ElevationType,
            out ElevationType elevation, sizeof(int), out _)) {
            throw NativeError("GetTokenInformation(TokenElevationType)",
                Marshal.GetLastWin32Error());
        }
        return new Identity(account, sid.Value, elevation);
    }

    private static string AccountName(SecurityIdentifier sid) {
        try {
            return sid.Translate(typeof(NTAccount)).Value;
        } catch (IdentityNotMappedException) {
            return "(account name unavailable)";
        }
    }

    public static Identity Current() {
        using var token = OpenCurrentToken();
        return Describe(token);
    }

    public static Identity Linked() {
        using var token = OpenCurrentToken();
        // TOKEN_LINKED_TOKEN contains one handle, which the caller must close.
        // https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-token_linked_token
        if (!GetTokenInformation(token, TokenInformationClass.LinkedToken,
            out IntPtr linkedHandle, (uint)IntPtr.Size, out _)) {
            throw NativeError("GetTokenInformation(TokenLinkedToken)",
                Marshal.GetLastWin32Error());
        }
        using var linked = new SafeAccessTokenHandle(linkedHandle);
        return Describe(linked);
    }
}
