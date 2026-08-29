// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

extern "C" {
#include <sys/types.h>

#include <dcerpc.h>
#include <param.h>
#include <talloc.h>
#include <tevent.h>

#include "generated/ndr_devicefs_block_device_client.h"

// Samba's credentials.h contains C enum forward declarations that are not
// valid C++. The test's C adapter exposes only the credential setup it needs.
auto devicefs_test_credentials(TALLOC_CTX *, loadparm_context *, const char *,
    const char *) -> cli_credentials *;
}

import std;

namespace {

constexpr auto kExpected = std::string_view{"DeviceFs Samba RPC fixture\n"};
constexpr auto kBackingLength = std::uint64_t{1024 * 1024};

auto Check(const NTSTATUS status, const std::string_view operation) -> void {
    if (!NT_STATUS_IS_OK(status)) {
        throw std::runtime_error{
            std::format("{}: {}", operation, nt_errstr(status))};
    }
}

} // namespace

auto main(const int argc, char *const argv[]) -> int {
    try {
        if (argc != 5) {
            std::println(
                "usage: {} CONFIG BINDING USERNAME PASSWORD", argv[0]);
            return 2;
        }

        Check(dcerpc_init(), "initialize DCE/RPC");
        auto memory = std::unique_ptr<TALLOC_CTX,
            decltype([](TALLOC_CTX *const context) noexcept {
                static_cast<void>(talloc_free(context));
            })>{talloc_new(nullptr)};
        if (!memory) {
            throw std::bad_alloc{};
        }

        auto *const events = tevent_context_init(memory.get());
        auto *const configuration = loadparm_init(memory.get());
        if ((events == nullptr) || (configuration == nullptr)) {
            throw std::bad_alloc{};
        }
        if (!lpcfg_load(configuration, argv[1])) {
            throw std::runtime_error{"could not load Samba configuration"};
        }
        auto *const credentials = devicefs_test_credentials(
            memory.get(), configuration, argv[3], argv[4]);
        if (credentials == nullptr) {
            throw std::runtime_error{"could not configure credentials"};
        }
        dcerpc_pipe *pipe = nullptr;
        Check(dcerpc_pipe_connect(memory.get(), &pipe, argv[2],
            &ndr_table_devicefs_block_device, credentials, events,
            configuration), "connect");

        auto length = std::uint64_t{};
        auto result = NTSTATUS{};
        Check(dcerpc_GetLength(pipe->binding_handle, memory.get(), "fixture",
            &length, &result), "GetLength transport");
        Check(result, "GetLength");
        if (length != kBackingLength) {
            throw std::runtime_error{std::format(
                "GetLength returned {}, expected {}", length,
                kBackingLength)};
        }

        auto buffer = std::array<std::uint8_t, kExpected.size()>{};
        auto transferred = std::uint32_t{};
        Check(dcerpc_Read(pipe->binding_handle, memory.get(), "fixture", 0,
            buffer.size(), &transferred, buffer.data(), &result),
            "Read transport");
        Check(result, "Read");
        if ((transferred != buffer.size()) ||
            (std::string_view{std::bit_cast<const char *>(buffer.data()),
                transferred} != kExpected)) {
            throw std::runtime_error{"Read returned unexpected data"};
        }

        return 0;
    } catch (const std::exception &error) {
        std::println("rpcd_devicefs test failed: {}", error.what());
        return 1;
    }
}
