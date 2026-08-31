// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <format>
#include <limits>
#include <memory>
#include <new>
#include <print>
#include <stdexcept>
#include <string_view>
#include <vector>

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

namespace {

constexpr auto kPrefix = std::string_view{"DeviceFs Samba RPC fixture\n"};
constexpr auto kBackingLength = std::uint64_t{1024 * 1024};

auto Check(const NTSTATUS status, const std::string_view operation) -> void {
    if (!NT_STATUS_IS_OK(status)) {
        throw std::runtime_error{
            std::format("{}: {}", operation, nt_errstr(status))};
    }
}

auto CallRead(dcerpc_pipe &pipe, TALLOC_CTX *const memory,
    const std::uint64_t offset, const std::uint32_t wanted)
    -> std::vector<std::uint8_t> {
    auto buffer = std::vector<std::uint8_t>(wanted);
    auto transferred = std::uint32_t{};
    auto result = NTSTATUS{};
    Check(dcerpc_Read(pipe.binding_handle, memory, "fixture", offset, wanted,
        &transferred, buffer.data(), &result), "Read transport");
    Check(result, "Read");
    buffer.resize(transferred);
    return buffer;
}

auto ExpectedPattern(const std::uint64_t offset, const std::size_t length)
    -> std::vector<std::uint8_t> {
    auto result = std::vector<std::uint8_t>(length);
    for (auto index = std::size_t{}; index < length; ++index) {
        // Masking with uint8_t's maximum proves that the arithmetic result is
        // representable by the cast's destination type.
        [[gsl::suppress("26472")]]
        result[index] = static_cast<std::uint8_t>(
            ((offset + index) * 37 + 11) &
            std::numeric_limits<std::uint8_t>::max());
    }
    return result;
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

        const auto prefix = CallRead(
            *pipe, memory.get(), 0, std::uint32_t{kPrefix.size()});
        if (!std::ranges::equal(prefix, kPrefix,
                [](const std::uint8_t actual, const char expected) {
                    return actual == std::bit_cast<std::uint8_t>(expected);
                })) {
            throw std::runtime_error{"Read at offset zero returned bad data"};
        }

        constexpr auto kOffset = std::uint64_t{4093};
        constexpr auto kCount = std::uint32_t{73};
        if (CallRead(*pipe, memory.get(), kOffset, kCount) !=
            ExpectedPattern(kOffset, kCount)) {
            throw std::runtime_error{
                "positioned Read returned data from the wrong offset"};
        }

        constexpr auto kEofCount = std::uint32_t{64};
        constexpr auto kEofRemaining = std::size_t{7};
        constexpr auto kEofOffset = kBackingLength - kEofRemaining;
        if (CallRead(*pipe, memory.get(), kEofOffset, kEofCount) !=
            ExpectedPattern(kEofOffset, kEofRemaining)) {
            throw std::runtime_error{"Read did not preserve the EOF short read"};
        }

        return 0;
    } catch (const std::exception &error) {
        std::println("rpcd_devicefs test failed: {}", error.what());
        return 1;
    }
}
