/*
 * SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * PIDL generates the server dispatcher as C but leaves the service's manager
 * declarations and interface hooks to its consumer. Keep that C-only adapter
 * here so the generated file remains build output and the service stays C++.
 */

#include <sys/types.h>
#include <string.h>

#include <dcesrv_core.h>
#include <util/debug.h>

#include "generated/ndr_devicefs_block_device.h"

NTSTATUS dcesrv_GetLength(struct dcesrv_call_state *call,
    TALLOC_CTX *memory, struct GetLength *request);
NTSTATUS dcesrv_Read(struct dcesrv_call_state *call,
    TALLOC_CTX *memory, struct Read *request);

static NTSTATUS devicefs_block_device_bind(
    struct dcesrv_connection_context *context,
    const struct dcesrv_interface *interface)
{
    return dcesrv_interface_bind_allow_connect(context, interface);
}

/*
 * PIDL's server output treats DCESRV_INTERFACE_<name>_BIND and
 * DCESRV_INTERFACE_<name>_FLAGS as optional compile-time hooks while it builds
 * the dcesrv_interface table. Without the first definition, the generated bind
 * callback accepts every bind without applying an interface-specific
 * authentication-level rule.
 *
 * DeviceFs uses authenticated RPC at DCERPC_AUTH_LEVEL_CONNECT, so its bind
 * callback delegates that decision to dcesrv_interface_bind_allow_connect().
 * Samba documents this level as authenticating the association without adding
 * per-message integrity or privacy, and lets smb.conf enable it for a named
 * interface through "allow dcerpc auth level connect:<interface>". See
 * https://www.samba.org/samba/docs/current/man-html/smb.conf.5.
 *
 * The second definition records that GetLength and Read do not create DCE/RPC
 * context handles. Samba therefore need not maintain context-handle state for
 * this interface.
 */
#define DCESRV_INTERFACE_DEVICEFS_BLOCK_DEVICE_BIND \
    devicefs_block_device_bind
#define DCESRV_INTERFACE_DEVICEFS_BLOCK_DEVICE_FLAGS \
    DCESRV_INTERFACE_FLAGS_HANDLES_NOT_USED

/*
 * PIDL's Samba 4 server generator emits diagnostic calls through DEBUG,
 * DEBUGLEVEL, and NDR_PRINT_FUNCTION_DEBUG. The generated dispatcher does not
 * need those diagnostics to decode, dispatch, or encode an RPC call. On an
 * installed Samba development environment, the macros name debug functions
 * that are implemented by a private Samba library rather than by the public
 * libraries used by this helper.
 *
 * This adapter deliberately removes only the generated diagnostics. Doing so
 * keeps PIDL's complete dispatcher while preventing its object file from
 * acquiring references to Samba's private debug implementation.
 */
#undef DEBUG
#define DEBUG(level, body) ((void)0)
#undef DEBUGLEVEL
#define DEBUGLEVEL (-1)
#undef NDR_PRINT_FUNCTION_DEBUG
#define NDR_PRINT_FUNCTION_DEBUG(type, flags, value) ((void)0)

#include "generated/ndr_devicefs_block_device_dispatch.c"
