// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <dlfcn.h>
#include <sys/utsname.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <print>
#include <ranges>
#include <string_view>
#include <vector>

extern "C" {
#include <tdb.h>
}

namespace {

using TdbOpen = decltype(&tdb_open_ex);

[[nodiscard]] auto IsWsl1Release(const std::string_view release) {
    const auto components = release
        | std::views::split('-')
        | std::views::transform([](const auto component) {
            return std::string_view{component};
        })
        | std::ranges::to<std::vector>();
    return (components.size() == 3) &&
        (components.at(0) == "4.4.0") &&
        !components.at(1).empty() &&
        std::ranges::all_of(components.at(1), [](const char character) {
            return (character >= '0') && (character <= '9');
        }) &&
        (components.at(2) == "Microsoft");
}

[[nodiscard]] auto IsWsl1Kernel() noexcept {
    /*
     * WSL1 reports its translated kernel as
     * "4.4.0-WINDOWS_BUILD-Microsoft". WSL2's real Linux kernel does not use
     * this format. Function-local static initialization performs uname and the
     * release parsing once and synchronizes concurrent first callers.
     */
    static const auto result = []() noexcept {
        auto enabled = false;
        try {
            auto kernel = utsname{};
            if (uname(&kernel) == 0) {
                enabled = IsWsl1Release(kernel.release);
            }
        } catch (...) {}
        std::println(stderr,
            "rpcd_devicefs: WSL1 TDB compatibility interposition {}",
            enabled ? "enabled" : "disabled");
        return enabled;
    }();
    return result;
}

[[nodiscard]] auto RealTdbOpen() noexcept -> TdbOpen {
    static const auto function = []() noexcept {
        static_cast<void>(dlerror());
        auto *const address = dlsym(RTLD_NEXT, "tdb_open_ex");
        if (const auto *const error = dlerror(); error != nullptr) {
            std::println(stderr,
                "rpcd_devicefs: could not resolve tdb_open_ex: {}", error);
            std::abort();
        }
        return reinterpret_cast<TdbOpen>(address);
    }();
    return function;
}

} // namespace

extern "C" auto tdb_open_ex(
    const char *const name,
    const int hash_size,
    int tdb_flags,
    const int open_flags,
    const mode_t mode,
    const struct tdb_logging_context *const log_context,
    const tdb_hash_func hash_function) -> struct tdb_context * {
    if (IsWsl1Kernel()) {
        /*
         * DeviceFs creates new private, state, cache, and lock directories for
         * every Samba view. A TDB opened by this worker therefore cannot
         * contain data left by an earlier view. TDB_CLEAR_IF_FIRST is
         * unnecessary in that environment: when O_CREAT produces an empty
         * file, tdb_open_ex initializes it after reading the empty header even
         * when this flag is absent.
         *
         * Keeping the flag breaks worker startup on WSL1. samba-dcerpcd
         * initializes names.tdb and retains a shared byte-range ACTIVE_LOCK.
         * WSL1 nevertheless lets rpcd_devicefs acquire the conflicting
         * exclusive lock, so TDB treats the worker as the first user of the
         * database. It then locks a range extending beyond the end of the file
         * and tries to truncate and rewrite the database; that sequence fails
         * with EINVAL on WSL1.
         *
         * Every TDB visible to the worker has the same per-view lifetime and
         * is stored on the same WSL1 filesystem. Remove TDB_CLEAR_IF_FIRST and
         * add TDB_NOMMAP on every call instead of depending on a list of Samba
         * database names. The parent daemon remains unmodified and initializes
         * the shared messaging database before it starts this worker.
         */
        tdb_flags &= ~TDB_CLEAR_IF_FIRST;
        tdb_flags |= TDB_NOMMAP;
    }
    return RealTdbOpen()(name, hash_size, tdb_flags, open_flags, mode,
        log_context, hash_function);
}
