// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

// The application's Windows, WIL, and WinFsp headers share many dependencies.
// Grouping them in one header unit avoids compiling those dependencies again
// in each source file that needs a different combination of Windows APIs.

#define RPC_USE_NATIVE_WCHAR
#define WIN32_NO_STATUS
#include <windows.h>
#undef WIN32_NO_STATUS
#include <ntstatus.h>

#include <winternl.h>
// `winternl.h` supplies `NTSTATUS` but not `PNTSTATUS`.
// `PNTSTATUS` is needed by `ntsecapi.h` and WinFsp, so we define it here.
using PNTSTATUS = NTSTATUS *;

// `_NTDEF_` makes `ntsecapi.h` reuse the types supplied by `winternl.h` instead
// of declaring conflicting STRING and UNICODE_STRING types.
#define _NTDEF_
#include <ntsecapi.h>
#undef _NTDEF_

#include <winsock2.h>
#include <aclapi.h>
#include <appmodel.h>
#include <bcrypt.h>
#include <DismApi.h>
#include <intrin.h>
#include <lm.h>
#include <lmcons.h>
#include <lmerr.h>
#include <msi.h>
#include <objbase.h>
#include <oleauto.h>
#include <roapi.h>
#include <rpc.h>
#include <rpcasync.h>
#include <sal.h>
#include <sddl.h>
#include <shlobj.h>
#define SECURITY_WIN32
#include <sspi.h>
#include <tlhelp32.h>
#include <userenv.h>
#include <vsserror.h>
#include <wincrypt.h>
#include <wininet.h>
#include <winioctl.h>
#include <winfsp/winfsp.h>

// Including `initguid.h` first makes `virtdisk.h` define its GUID constants,
// notably `VIRTUAL_STORAGE_TYPE_VENDOR_MICROSOFT`, instead of only declaring
// them.
#include <initguid.h>
#include <virtdisk.h>
#undef INITGUID
#include <guiddef.h>

// `wil/resource.h` defines `wil::unique_hlsa` only when `_NTLSA_` is defined.
#define _NTLSA_
#include <wil/resource.h>
#undef _NTLSA_
#include <wil/result.h>
#include <wil/safecast.h>
#include <wil/stl.h>
#include <wil/com.h>
#include <wil/filesystem.h>
#include <wil/network.h>
#include <wil/registry.h>
#include <wil/rpc_helpers.h>
#include <wil/token_helpers.h>
#include <wil/win32_helpers.h>

// When `materialize_oci.ixx` uses WIL's registry iterator, MSVC incorrectly
// reports that `unique_array_ptr` has too few template arguments. This
// explicit instantiation compiles the iterator's implementation as part of
// this header unit, which avoids the compiler defect when this header unit is
// imported.
template class wil::reg::key_iterator_data<wil::unique_process_heap_string>;

// The Windows `GetObject` macro conflicts with the `IJsonValue::GetObject`
// function defined by C++/WinRT.
#undef GetObject
// The `stderr` and `stdout` macros conflict with this project's stream writer.
#undef stderr
#undef stdout
