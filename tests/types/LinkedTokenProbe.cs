// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Security.Principal;
using Microsoft.Win32.SafeHandles;
using Windows.Win32.Foundation;
using Windows.Win32.Security;
using static Windows.Win32.PInvoke;

namespace DeviceFs.Tests;

public static unsafe class LinkedTokenProbe {
    public enum ElevationType {
        Default = 1,
        Full,
        Limited
    }

    public sealed record Identity(string Account, string Sid, ElevationType Elevation);

    private static Win32Exception NativeError(string operation, int error) {
        return new Win32Exception(error,
            $"{operation}: Windows error 0x{error:x8}: {new Win32Exception(error).Message}");
    }

    private static SafeFileHandle OpenCurrentToken() {
        if (!OpenProcessToken(GetCurrentProcess_SafeHandle(),
                TOKEN_ACCESS_MASK.TOKEN_QUERY, out var token)) {
            throw NativeError("OpenProcessToken", Marshal.GetLastWin32Error());
        }
        return token;
    }

    private static SecurityIdentifier ReadUserSid(SafeFileHandle token) {
        GetTokenInformation(token, TOKEN_INFORMATION_CLASS.TokenUser, default, out var length);
        var error = Marshal.GetLastWin32Error();
        if (error != (int)WIN32_ERROR.ERROR_INSUFFICIENT_BUFFER) {
            throw NativeError("GetTokenInformation(TokenUser) size query", error);
        }

        var buffer = new byte[checked((int)length)];
        // `TOKEN_USER.User.Sid` points into the returned buffer. The buffer
        // remains pinned from the query until `SecurityIdentifier` copies the
        // SID, so a garbage collection cannot invalidate that pointer.
        fixed (byte* data = buffer) {
            if (!GetTokenInformation(token, TOKEN_INFORMATION_CLASS.TokenUser,
                    data, length, out _)) {
                throw NativeError("GetTokenInformation(TokenUser)",
                    Marshal.GetLastWin32Error());
            }
            return new SecurityIdentifier((IntPtr)((TOKEN_USER*)data)->User.Sid.Value);
        }
    }

    private static Identity Describe(SafeFileHandle token) {
        var sid = ReadUserSid(token);
        var account = AccountName(sid);
        var elevation = new TOKEN_ELEVATION_TYPE();
        if (!GetTokenInformation(token, TOKEN_INFORMATION_CLASS.TokenElevationType,
                new Span<byte>(&elevation, sizeof(TOKEN_ELEVATION_TYPE)), out _)) {
            throw NativeError("GetTokenInformation(TokenElevationType)",
                Marshal.GetLastWin32Error());
        }
        return new Identity(account, sid.Value, (ElevationType)elevation);
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
        var linkedToken = new TOKEN_LINKED_TOKEN();
        if (!GetTokenInformation(token, TOKEN_INFORMATION_CLASS.TokenLinkedToken,
                new Span<byte>(&linkedToken, sizeof(TOKEN_LINKED_TOKEN)), out _)) {
            throw NativeError("GetTokenInformation(TokenLinkedToken)",
                Marshal.GetLastWin32Error());
        }
        using var linked = new SafeFileHandle(linkedToken.LinkedToken, ownsHandle: true);
        return Describe(linked);
    }
}
