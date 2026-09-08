// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/*
 * samba-dcerpcd's documented standalone mode listens for DCE/RPC connections,
 * starts an explicitly selected rpcd service, and passes accepted connections
 * to that service. The service must identify the interfaces and endpoint
 * servers it provides, then enter Samba's common RPC-worker implementation.
 * See https://www.samba.org/samba/docs/current/man-html/samba-dcerpcd.8.html.
 *
 * Samba's rpcd services enter that implementation through rpc_worker_main().
 * Its callbacks are the boundary through which this helper supplies the
 * generated DeviceFs interface and endpoint server. The installed Samba
 * distribution provides the function in its RPC-worker library but does not
 * install a header that declares it, so ordinary inclusion of Samba's public
 * headers is insufficient to build a separately developed rpcd service.
 *
 * This file supplies only that missing declaration. Keeping it in the compat
 * directory makes the non-installed dependency visible and prevents the rest
 * of the helper from accumulating declarations copied from Samba internals.
 */

extern "C" {
#include <dcesrv_core.h>
#include <ndr.h>

auto rpc_worker_main(int argc, const char *argv[],
    const char *daemon_config_name, int num_workers, int idle_seconds,
    size_t (*get_interfaces)(const struct ndr_interface_table ***interfaces,
        void *private_data),
    NTSTATUS (*get_servers)(struct dcesrv_context *context,
        const struct dcesrv_endpoint_server ***servers,
        size_t *server_count, void *private_data),
    void *private_data) -> int;
}
