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

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <condition_variable>
#include <deque>
#include <exception>
#include <filesystem>
#include <format>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <print>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "compat/rpc_worker.h"

#if DEVICEFS_ENABLE_GRPC_TRANSPORT
#include <grpc/grpc.h>
#include <grpcpp/create_channel.h>
#include "generated/block-device.grpc.pb.h"
#endif

extern "C" {
#include <tevent.h>

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
#if DEVICEFS_ENABLE_GRPC_TRANSPORT
constexpr auto kUnlimitedGrpcMessageSize = -1;
#endif

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

#if DEVICEFS_ENABLE_GRPC_TRANSPORT
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

    auto RequestStop() noexcept -> void {
        static_cast<void>(stop_source_.request_stop());
    }

    [[nodiscard]] auto Read(
        const std::span<std::uint8_t> buffer,
        const std::uint64_t offset) const noexcept -> ssize_t override {
        try {
            PrintDiagnostic(
                "rpcd_devicefs: calling gRPC Read(offset={}, count={})",
                offset, buffer.size());
            auto context = grpc::ClientContext{};
            const auto cancel = std::stop_callback{
                stop_source_.get_token(),
                [&context] noexcept { context.TryCancel(); }};
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
    std::stop_source stop_source_;
    std::uint64_t length_ = 0;
};
#endif

class ReadExecutor;

class ReadOperation final {
public:
    ReadOperation(ReadExecutor &executor,
        const AbstractBackingDevice &device,
        dcesrv_call_state &call,
        Read &request)
        : executor_{executor},
          device_{device},
          call_{call},
          request_{request},
          buffer_{request.out.buffer, request.in.wanted},
          offset_{request.in.offset},
          completion_context_{
              tevent_threaded_context_create(&call, call.event_ctx)},
          completion_event_{tevent_create_immediate(&call)} {
        if ((completion_context_ == nullptr) ||
            (completion_event_ == nullptr)) {
            throw std::bad_alloc{};
        }
    }

    auto Run() noexcept -> void;

private:
    static void Complete(tevent_context *, tevent_immediate *,
        void *private_data) noexcept;

    auto Reply() noexcept -> void;

    ReadExecutor &executor_;
    const AbstractBackingDevice &device_;
    dcesrv_call_state &call_;
    Read &request_;
    const std::span<std::uint8_t> buffer_;
    const std::uint64_t offset_;
    tevent_threaded_context *const completion_context_;
    tevent_immediate *const completion_event_;
    NTSTATUS status_ = NT_STATUS_IO_DEVICE_ERROR;
    std::uint32_t transferred_ = 0;
};

class ReadExecutor final {
public:
    explicit ReadExecutor(AbstractBackingDevice &device)
        : device_{device} {
        // At least two workers are required for reads to overlap. Above that,
        // use the machine's concurrency instead of imposing another fixed
        // limit below the RPC client and filesystem dispatcher.
        const auto worker_count = std::max(
            2u, std::thread::hardware_concurrency());
        workers_.reserve(worker_count);
        for (auto index = 0u; index < worker_count; ++index) {
            workers_.emplace_back(
                [this](const std::stop_token stop) noexcept {
                    Run(stop);
                });
        }
    }

    ReadExecutor(const ReadExecutor &) = delete;
    auto operator=(const ReadExecutor &) -> ReadExecutor & = delete;

    ~ReadExecutor() {
        for (auto &worker : workers_) {
            worker.request_stop();
        }
#if DEVICEFS_ENABLE_GRPC_TRANSPORT
        if (auto *const grpc_device =
                dynamic_cast<GrpcBackingDevice *>(&device_);
            grpc_device != nullptr) {
            grpc_device->RequestStop();
        }
#endif
        ready_.notify_all();
    }

    auto Submit(dcesrv_call_state &call, Read &request) -> void {
        auto operation = std::make_shared<ReadOperation>(
            *this, device_, call, request);
        {
            auto lock = std::lock_guard{mutex_};
            const auto [active, inserted] = active_.emplace(
                operation.get(), operation);
            if (!inserted) {
                std::terminate();
            }
            try {
                pending_.push_back(std::move(operation));
            } catch (...) {
                active_.erase(active);
                throw;
            }
        }
        ready_.notify_one();
    }

private:
    friend class ReadOperation;

    auto Run(const std::stop_token stop) noexcept -> void {
        while (!stop.stop_requested()) {
            auto lock = std::unique_lock{mutex_};
            if (!ready_.wait(lock, stop,
                    [this] { return !pending_.empty(); })) {
                return;
            }
            if (stop.stop_requested()) {
                return;
            }
            auto operation = std::move(pending_.front());
            pending_.pop_front();
            lock.unlock();
            operation->Run();
        }
    }

    [[nodiscard]] auto Take(ReadOperation *const operation) noexcept
        -> std::shared_ptr<ReadOperation> {
        auto lock = std::lock_guard{mutex_};
        auto active = active_.extract(operation);
        if (active.empty()) {
            std::terminate();
        }
        return std::move(active.mapped());
    }

    AbstractBackingDevice &device_;
    std::mutex mutex_;
    std::condition_variable_any ready_;
    std::deque<std::shared_ptr<ReadOperation>> pending_;
    std::unordered_map<ReadOperation *, std::shared_ptr<ReadOperation>>
        active_;
    // Destroying the jthreads first ensures that no worker can retain an
    // operation while the queues and backing device are being destroyed.
    std::vector<std::jthread> workers_;
};

auto ReadOperation::Run() noexcept -> void {
    const auto transferred = device_.Read(buffer_, offset_);
    if ((transferred >= 0) &&
        std::in_range<std::uint32_t>(transferred)) {
        status_ = NT_STATUS_OK;
        transferred_ = static_cast<std::uint32_t>(transferred);
    }

    /*
     * Samba owns the call and all of its NDR output. Only its event-loop
     * thread may finish that call. tevent transfers this completion back to
     * that thread after the blocking read has left the RPC dispatch path.
     */
    tevent_threaded_schedule_immediate(completion_context_,
        completion_event_, Complete, this);
}

void ReadOperation::Complete(tevent_context *, tevent_immediate *,
    void *const private_data) noexcept {
    auto *const operation = static_cast<ReadOperation *>(private_data);
    auto owner = operation->executor_.Take(operation);
    owner->Reply();
}

auto ReadOperation::Reply() noexcept -> void {
    request_.out.result = status_;
    *request_.out.transferred = transferred_;
    dcesrv_async_reply(&call_);
}

struct ServiceState {
    // Samba can ask the executable which interfaces it provides without also
    // requesting the endpoint servers that dispatch those interfaces. Keeping
    // the device optional allows interface discovery to remain side-effect
    // free; GetServers opens it only when Samba requests a dispatch server.
    std::unique_ptr<AbstractBackingDevice> backing_device;
    std::optional<ReadExecutor> read_executor;
    std::array<const dcesrv_endpoint_server *, 1> endpoint_servers{};
};

/*
 * The generated dispatcher calls GetLength and Read without passing the
 * ServiceState created in main. GetServers publishes the selected backing
 * device and read executor through read-only pointers after endpoint
 * registration succeeds. rpc_worker_main returns before main destroys the
 * ServiceState, so both objects outlive every call that can use them.
 */
const AbstractBackingDevice *active_backing_device = nullptr;
ReadExecutor *active_read_executor = nullptr;

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
        auto backing_device = std::unique_ptr<AbstractBackingDevice>{};
        try {
#if DEVICEFS_ENABLE_GRPC_TRANSPORT
            /*
             * If the launcher-supplied pathname names a Unix socket,
             * GrpcBackingDevice creates a client for that socket and caches
             * the service's length. Otherwise, BackingDevice opens the
             * pathname, verifies that it names a block device, and caches its
             * capacity. ServiceState owns the selected device for every
             * GetLength and Read call handled by this worker.
             */
            struct stat information {};
            if ((stat(path, &information) == 0) &&
                S_ISSOCK(information.st_mode)) {
                backing_device = std::make_unique<GrpcBackingDevice>(path);
            } else {
                backing_device = std::make_unique<BackingDevice>(path);
            }
#else
            backing_device = std::make_unique<BackingDevice>(path);
#endif
            state.backing_device = std::move(backing_device);
            state.read_executor.emplace(*state.backing_device);
        } catch (...) {
            state.read_executor.reset();
            state.backing_device.reset();
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
            state.read_executor.reset();
            state.backing_device.reset();
            return status;
        }
        state.endpoint_servers.front() =
            dcesrv_ep_server_byname(NDR_DEVICEFS_BLOCK_DEVICE_NAME);
        if (state.endpoint_servers.front() == nullptr) {
            state.read_executor.reset();
            state.backing_device.reset();
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
        active_read_executor = &*state.read_executor;
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

extern "C" NTSTATUS dcesrv_Read(dcesrv_call_state *const call,
    TALLOC_CTX *const memory, Read *const request) noexcept {
    if (!(call->state_flags & DCESRV_CALL_STATE_FLAG_MAY_ASYNC)) {
        return NT_STATUS_NOT_SUPPORTED;
    }

    request->out.buffer = request->in.wanted == 0
        ? nullptr
        : talloc_array(memory, std::uint8_t, request->in.wanted);
    if ((request->in.wanted != 0) && (request->out.buffer == nullptr)) {
        return NT_STATUS_NO_MEMORY;
    }

    if (request->in.wanted == 0) {
        *request->out.transferred = 0;
        return NT_STATUS_OK;
    }

    try {
        active_read_executor->Submit(*call, *request);
    } catch (...) {
        return NT_STATUS_NO_MEMORY;
    }
    call->state_flags |= DCESRV_CALL_STATE_FLAG_ASYNC;
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
