// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <errno.h>
#include <stdio.h>
#if defined(__APPLE__)
#include <sys/disk.h>
#elif defined(__linux__)
#include <dlfcn.h>
#include <linux/fs.h>
#include <stdlib.h>
#endif
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <print>
#include <span>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <vector>

#include "compat/rpc_worker.h"

#include <grpc/grpc.h>
#include <grpcpp/create_channel.h>
#include "generated/block-device.grpc.pb.h"

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

namespace {

constexpr auto kDeviceEnvironmentVariable = "DEVICEFS_SAMBA_RPC_DEVICE";
constexpr auto kWorkerProcessCount = 1;
constexpr auto kIdleShutdownDelaySeconds = 1;
constexpr auto kUnlimitedGrpcMessageSize = -1;

template <typename... Arguments>
[[clang::always_inline]] void PrintDiagnostic(
    [[maybe_unused]] const std::format_string<Arguments...> format,
    [[maybe_unused]] Arguments &&...arguments) noexcept {
#if defined(DEVICEFS_SAMBA_RPC_DIAGNOSTICS)
    try {
        std::println(
            stderr, format, std::forward<Arguments>(arguments)...);
        std::fflush(stderr);
    } catch (...) {}
#endif
}

#if defined(__linux__)
using RpcWorkerLibrary = std::unique_ptr<void,
    decltype([](void *const library) noexcept {
        dlclose(library);
    })>;

struct RpcWorker {
    RpcWorkerLibrary library;
    decltype(&rpc_worker_main) entry_point;
};

[[nodiscard]] auto LoadRpcWorker() noexcept -> std::optional<RpcWorker> {
    auto library = RpcWorkerLibrary{dlopen(
        DEVICEFS_RPC_WORKER_LIBRARY, RTLD_NOW | RTLD_GLOBAL)};
    if (!library) {
        std::fprintf(stderr, "could not load Samba RPC worker library %s: %s\n",
            DEVICEFS_RPC_WORKER_LIBRARY, dlerror());
        return std::nullopt;
    }

    static_cast<void>(dlerror());
    auto *const address = dlsym(library.get(), "rpc_worker_main");
    if (const auto *const error = dlerror(); error != nullptr) {
        std::fprintf(stderr,
            "could not resolve rpc_worker_main in Samba RPC worker library "
            "%s: %s\n",
            DEVICEFS_RPC_WORKER_LIBRARY, error);
        return std::nullopt;
    }

    /*
     * Debian gives every build of this private library a different ELF symbol
     * version. Looking up the unqualified name selects the installed library's
     * current default definition instead of recording that build-specific
     * version in rpcd_devicefs.
     */
    return RpcWorker{
        .library = std::move(library),
        .entry_point =
            reinterpret_cast<decltype(&rpc_worker_main)>(address),
    };
}
#endif

class AbstractBackingDevice {
public:
    virtual ~AbstractBackingDevice() = default;

    [[nodiscard]] virtual auto Length() const noexcept -> std::uint64_t = 0;

    [[nodiscard]] virtual auto Read(
        std::span<std::uint8_t> buffer,
        std::uint64_t offset) const noexcept -> ssize_t = 0;
};

class BackingDevice final : public AbstractBackingDevice {
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
#if defined(__linux__)
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

    [[nodiscard]] auto Length() const noexcept -> std::uint64_t override {
        return length_;
    }

    [[nodiscard]] auto Read(
        const std::span<std::uint8_t> buffer,
        const std::uint64_t offset) const noexcept -> ssize_t override {
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

class GrpcBackingDevice final : public AbstractBackingDevice {
public:
    explicit GrpcBackingDevice(const std::filesystem::path &path) {
        PrintDiagnostic(
            "rpcd_devicefs: creating gRPC channel for '{}'", path.string());
        auto arguments = grpc::ChannelArguments{};
        /*
         * gRPC C++ otherwise derives HTTP/2 :authority from the Unix-socket
         * pathname. Tonic rejects that percent-encoded pathname with
         * PROTOCOL_ERROR before dispatching a request. The block-device
         * service does not route by authority, so localhost is valid for
         * every call on this channel.
         */
        arguments.SetString(GRPC_ARG_DEFAULT_AUTHORITY, "localhost");
        arguments.SetMaxReceiveMessageSize(kUnlimitedGrpcMessageSize);
        stub_ = proxmox::backup::BlockDevice::NewStub(
            grpc::CreateCustomChannel(
                std::format("unix:{}", path.string()),
                grpc::InsecureChannelCredentials(), arguments));

        auto context = grpc::ClientContext{};
        auto request = proxmox::backup::GetLengthRequest{};
        auto response = proxmox::backup::GetLengthResponse{};
        PrintDiagnostic("rpcd_devicefs: calling gRPC GetLength");
        const auto status = stub_->GetLength(&context, request, &response);
        if (!status.ok()) {
            PrintDiagnostic(
                "rpcd_devicefs: gRPC GetLength failed (code {}): {}",
                std::to_underlying(status.error_code()),
                status.error_message());
            throw std::runtime_error{status.error_message()};
        }
        length_ = response.length();
        PrintDiagnostic(
            "rpcd_devicefs: gRPC GetLength returned {} bytes", length_);
    }

    [[nodiscard]] auto Length() const noexcept -> std::uint64_t override {
        return length_;
    }

    [[nodiscard]] auto Read(
        const std::span<std::uint8_t> buffer,
        const std::uint64_t offset) const noexcept -> ssize_t override {
        try {
            PrintDiagnostic(
                "rpcd_devicefs: calling gRPC Read(offset={}, count={})",
                offset, buffer.size());
            auto context = grpc::ClientContext{};
            auto request = proxmox::backup::ReadRequest{};
            request.set_offset(offset);
            request.set_count(static_cast<std::uint32_t>(buffer.size()));
            auto response = proxmox::backup::ReadResponse{};
            const auto status = stub_->Read(&context, request, &response);
            if (!status.ok()) {
                PrintDiagnostic(
                    "rpcd_devicefs: gRPC Read failed (code {}): {}",
                    std::to_underlying(status.error_code()),
                    status.error_message());
                return -1;
            }

            const auto &data = response.data();
            PrintDiagnostic(
                "rpcd_devicefs: gRPC Read returned {} bytes", data.size());
            if (data.size() > buffer.size()) {
                return -1;
            }
            if (!data.empty()) {
                std::memcpy(buffer.data(), data.data(), data.size());
            }
            return static_cast<ssize_t>(data.size());
        } catch (...) {
            return -1;
        }
    }

private:
    std::unique_ptr<proxmox::backup::BlockDevice::Stub> stub_;
    std::uint64_t length_ = 0;
};

struct ServiceState {
    // Samba can ask the executable which interfaces it provides without also
    // requesting the endpoint servers that dispatch those interfaces. Keeping
    // the device optional allows interface discovery to remain side-effect
    // free; GetServers opens it only when Samba requests a dispatch server.
    std::unique_ptr<AbstractBackingDevice> backing_device;
    std::array<const dcesrv_endpoint_server *, 1> endpoint_servers{};
};

/*
 * The generated dispatcher calls GetLength and Read without passing the
 * ServiceState created in main. GetServers publishes the selected backing
 * device through this read-only pointer after endpoint registration succeeds.
 * rpc_worker_main returns before main destroys the ServiceState, so the device
 * outlives every call that can use this pointer.
 */
const AbstractBackingDevice *active_backing_device = nullptr;

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
        PrintDiagnostic("rpcd_devicefs: inspecting backing path '{}'", path);
        try {
            /*
             * If the launcher-supplied pathname names a Unix socket,
             * GrpcBackingDevice creates a client for that socket and caches
             * the service's length. Otherwise, BackingDevice opens the
             * pathname, verifies that it names a block device, and caches its
             * capacity. ServiceState owns the selected device for every
             * GetLength and Read call handled by this worker.
             *
             * This construction is the only C++ operation in the callback
             * that can throw. The catch belongs here because Samba invokes
             * GetServers through a noexcept C callback; a catch surrounding
             * rpc_worker_main in main could not receive an exception without
             * first allowing it to escape this ABI boundary.
             */
            struct stat information {};
            if ((stat(path, &information) == 0) &&
                S_ISSOCK(information.st_mode)) {
                state.backing_device =
                    std::make_unique<GrpcBackingDevice>(path);
            } else {
                state.backing_device =
                    std::make_unique<BackingDevice>(path);
            }
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
        active_backing_device = state.backing_device.get();
    }

    /*
     * The output is an array of endpoint-server pointers plus its element
     * count. The array is a ServiceState member, rather than callback-local
     * storage, because Samba can retain it while rpc_worker_main is running.
     * main keeps ServiceState alive for exactly that interval.
     */
    *servers = state.endpoint_servers.data();
    *server_count = state.endpoint_servers.size();
    PrintDiagnostic("rpcd_devicefs: Samba worker initialization complete");
    return NT_STATUS_OK;
}

} // namespace

extern "C" NTSTATUS dcesrv_GetLength(dcesrv_call_state *,
    TALLOC_CTX *, GetLength *const request) noexcept {
    const auto length = active_backing_device->Length();
    *request->out.length = length;
    PrintDiagnostic(
        "rpcd_devicefs: serving DCE GetLength ({} bytes)", length);
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
#if defined(__linux__)
    auto worker = LoadRpcWorker();
    if (!worker) {
        return EXIT_FAILURE;
    }
    const auto worker_main = worker->entry_point;
#else
    constexpr auto worker_main = rpc_worker_main;
#endif

    auto arguments = std::vector<const char *>{argv, argv + argc};
    auto state = ServiceState{};
    return worker_main(argc, arguments.data(), "rpcd_devicefs",
        kWorkerProcessCount, kIdleShutdownDelaySeconds,
        GetInterfaces, GetServers, &state);
}
