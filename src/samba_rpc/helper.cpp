// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <errno.h>
#include <stdio.h>
#ifdef __APPLE__
#include <sys/disk.h>
#endif
#ifdef __linux__
#include <linux/fs.h>
#endif
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "compat/rpc_worker.h"

extern "C" {
#include "generated/ndr_devicefs_block_device.h"

/*
 * Samba's PIDL interface-definition compiler emits this function in its
 * generated C server dispatcher. The same generated file contains a static
 * descriptor for the DeviceFs endpoint server; this function registers that
 * descriptor with Samba. PIDL's generated header declares the interface types
 * and the metadata used to marshal its calls, but it omits this registration
 * function, so the C++ worker supplies the declaration needed to call it.
 */
auto dcerpc_server_devicefs_block_device_init(TALLOC_CTX *) -> NTSTATUS;
}

import std;

namespace {

constexpr auto kDeviceEnvironmentVariable = "DEVICEFS_SAMBA_RPC_DEVICE";
constexpr auto kWorkerProcessCount = 1;
constexpr auto kIdleShutdownDelaySeconds = 1;

class BackingDevice {
public:
    explicit BackingDevice(const std::filesystem::path &path)
        : file_{std::fopen(path.c_str(), "rb")} {
        if (!file_) {
            throw std::system_error{errno, std::generic_category()};
        }
        const auto descriptor = fileno(file_.get());
        struct stat information {};
        if (fstat(descriptor, &information) == -1) {
            throw std::system_error{errno, std::generic_category()};
        }

        if (!S_ISBLK(information.st_mode)) {
            throw std::invalid_argument{"backing path is not a block device"};
        }

        /*
         * fstat identifies the opened object as a block device, but its
         * st_size field is not the device capacity. Linux provides that
         * capacity directly in bytes. macOS instead provides a logical block
         * size and a block count, whose product is the byte capacity.
         */
        length_ = [descriptor] {
#ifdef __linux__
            auto length = std::uint64_t{};
            if (ioctl(descriptor, BLKGETSIZE64, &length) == -1) {
                throw std::system_error{errno, std::generic_category()};
            }
            return length;
#elif defined(__APPLE__)
            auto block_size = std::uint32_t{};
            auto block_count = std::uint64_t{};
            if ((ioctl(descriptor, DKIOCGETBLOCKSIZE, &block_size) == -1) ||
                (ioctl(descriptor, DKIOCGETBLOCKCOUNT, &block_count) == -1)) {
                throw std::system_error{errno, std::generic_category()};
            }
            if ((block_size == 0) ||
                (block_count >
                    (std::numeric_limits<std::uint64_t>::max() /
                        block_size))) {
                // Multiplication must produce the exact capacity represented
                // by the two kernel values rather than wrapping in uint64_t.
                throw std::length_error{"invalid block-device geometry"};
            }
            return block_count * block_size;
#else
#error "The DeviceFs Samba RPC helper requires Linux or macOS"
#endif
        }();

        if (length_ > static_cast<std::uint64_t>(
                std::numeric_limits<off_t>::max())) {
            throw std::length_error{
                "backing device exceeds positioned-read range"};
        }
    }

    [[nodiscard]] auto Length() const noexcept -> std::uint64_t {
        return length_;
    }

    [[nodiscard]] auto Read(
        const std::span<std::uint8_t> buffer,
        const std::uint64_t offset) const noexcept -> ssize_t {
        auto result = ssize_t{};
        do {
            result = pread(fileno(file_.get()), buffer.data(), buffer.size(),
                static_cast<off_t>(offset));
        } while ((result == -1) && (errno == EINTR));
        return result;
    }

private:
    std::unique_ptr<std::FILE,
        decltype([](std::FILE *const file) noexcept {
            static_cast<void>(std::fclose(file));
        })> file_;
    std::uint64_t length_ = 0;
};

struct ServiceState {
    // Samba can ask the executable which interfaces it provides without also
    // requesting the endpoint servers that dispatch those interfaces. Keeping
    // the device optional allows interface discovery to remain side-effect
    // free; GetServers opens it only when Samba requests a dispatch server.
    std::optional<BackingDevice> backing_device;
    std::array<const dcesrv_endpoint_server *, 1> endpoint_servers{};
};

/*
 * The generated dispatcher calls GetLength and Read without passing the
 * ServiceState created in main. GetServers publishes its BackingDevice through
 * this read-only pointer after endpoint registration succeeds. rpc_worker_main
 * returns before main destroys the ServiceState, so the device outlives every
 * call that can use this pointer.
 */
const BackingDevice *active_backing_device = nullptr;

auto GetInterfaces(
    const ndr_interface_table ***const interfaces,
    void *) noexcept -> std::size_t {
    /*
     * rpc_worker_main asks which DCE/RPC interfaces this executable can serve.
     * Each array entry points to the generated table that identifies an
     * interface and describes how its calls are marshalled. rpc_worker_main
     * retains the returned array pointer, so the one-entry array has static
     * storage rather than belonging to this callback's stack frame.
     */
    static const ndr_interface_table *provided_interfaces[]{
        &ndr_table_devicefs_block_device,
    };
    *interfaces = provided_interfaces;
    return std::size(provided_interfaces);
}

auto GetServers(dcesrv_context *,
    const dcesrv_endpoint_server ***const servers,
    std::size_t *const server_count,
    void *const private_data) noexcept -> NTSTATUS {
    /*
     * main passes &state as rpc_worker_main's private_data because a C
     * callback cannot capture the C++ object. main does not destroy that state
     * until rpc_worker_main returns and has stopped invoking callbacks.
     */
    auto &state = *static_cast<ServiceState *>(private_data);
    if (!state.backing_device) {
        const auto *const path = std::getenv(kDeviceEnvironmentVariable);
        if (path == nullptr) {
            return NT_STATUS_INVALID_PARAMETER;
        }
        try {
            /*
             * BackingDevice opens the launcher-supplied pathname, verifies
             * that it names a block device, and caches the capacity. Storing
             * it in ServiceState keeps the same open device available to
             * every GetLength and Read call handled by this worker.
             *
             * This construction is the only C++ operation in the callback
             * that can throw. The catch belongs here because Samba invokes
             * GetServers through a noexcept C callback; a catch surrounding
             * rpc_worker_main in main could not receive an exception without
             * first allowing it to escape this ABI boundary.
             */
            state.backing_device.emplace(std::filesystem::path{path});
        } catch (...) {
            return NT_STATUS_UNSUCCESSFUL;
        }

        /*
         * PIDL's generated initializer adds the DeviceFs endpoint server to
         * Samba's registry and returns only a status. rpc_worker_main needs
         * the registered descriptor itself, so the following lookup retrieves
         * that descriptor by the interface name emitted by PIDL.
         */
        const auto status =
            dcerpc_server_devicefs_block_device_init(nullptr);
        if (!NT_STATUS_IS_OK(status)) {
            return status;
        }
        state.endpoint_servers.front() =
            dcesrv_ep_server_byname(NDR_DEVICEFS_BLOCK_DEVICE_NAME);
        if (state.endpoint_servers.front() == nullptr) {
            return NT_STATUS_NOT_FOUND;
        }

        /*
         * The generated dispatcher calls GetLength and Read without passing
         * rpc_worker_main's private_data. Publish the ServiceState-owned device
         * after registration succeeds so those two callbacks can reach it.
         * Each process hosts one DeviceFs endpoint and one backing device, so
         * there is no second service instance whose state could be confused
         * with this pointer.
         */
        active_backing_device = &*state.backing_device;
    }

    /*
     * The output is an array of endpoint-server pointers plus its element
     * count. The array is a ServiceState member, rather than callback-local
     * storage, because Samba can retain it while rpc_worker_main is running.
     * main keeps ServiceState alive for exactly that interval.
     */
    *servers = state.endpoint_servers.data();
    *server_count = state.endpoint_servers.size();
    return NT_STATUS_OK;
}

} // namespace

extern "C" NTSTATUS dcesrv_GetLength(dcesrv_call_state *,
    TALLOC_CTX *, GetLength *const request) noexcept {
    *request->out.length = active_backing_device->Length();
    return NT_STATUS_OK;
}

extern "C" NTSTATUS dcesrv_Read(dcesrv_call_state *,
    TALLOC_CTX *const memory, Read *const request) noexcept {
    request->out.buffer = request->in.wanted == 0
        ? nullptr
        : talloc_array(memory, std::uint8_t, request->in.wanted);
    if ((request->in.wanted != 0) && (request->out.buffer == nullptr)) {
        return NT_STATUS_NO_MEMORY;
    }

    const auto transferred = active_backing_device->Read(
        {request->out.buffer, request->in.wanted}, request->in.offset);
    if (transferred == -1) {
        return NT_STATUS_IO_DEVICE_ERROR;
    }
    *request->out.transferred = static_cast<std::uint32_t>(transferred);
    return NT_STATUS_OK;
}

auto main(const int argc, char *const argv[]) -> int {
    auto arguments = std::vector<const char *>{argv, argv + argc};
    auto state = ServiceState{};
    return rpc_worker_main(argc, arguments.data(), "rpcd_devicefs",
        kWorkerProcessCount, kIdleShutdownDelaySeconds,
        GetInterfaces, GetServers, &state);
}
