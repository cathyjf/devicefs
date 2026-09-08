// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <sys/statvfs.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <print>
#include <system_error>
#include <thread>

using namespace std::chrono_literals;

namespace {

constexpr auto kStartupTimeout = 1.5s;
constexpr auto kPollInterval = 10ms;
// `execvp` searches PATH for this name. An explicit path selects a fixed
// executable instead.
constexpr auto kFishExecutable = "fish";

auto WaitForReadOnlyRoot() -> void {
    // The DeviceFs image configures a WSL boot command that remounts `/`
    // read-only. However, WSL can start other processes before that command
    // finishes. The purpose of this broker is wait for the remount to complete
    // before allowing Fish to initialize and run the backup client.
    //
    // A failed boot command must not prevent backups indefinitely. If the
    // timeout expires, or the mount flags cannot be read, the broker warns
    // that the root's read-only state could not be confirmed and starts Fish
    // anyway. The design deliberately favors backup availability in that case.
    const auto deadline = std::chrono::steady_clock::now() + kStartupTimeout;
    while (std::chrono::steady_clock::now() < deadline) {
        auto status = (struct statvfs){};
        if (statvfs("/", &status) != 0) {
            std::println(std::cerr,
                "devicefs-wsl-startup-broker: could not query the mount flags "
                "for '/': {}; starting Fish without confirming that '/' is "
                "read-only",
                std::error_code{errno, std::generic_category()}.message());
            return;
        }
        if ((status.f_flag & ST_RDONLY) != 0) {
            return;
        }
        std::this_thread::sleep_for(kPollInterval);
    }
    std::println(std::cerr,
        "devicefs-wsl-startup-broker: '/' is still writable after waiting {}; "
        "starting Fish anyway", kStartupTimeout);
}

}

auto main(int, char *const argv[]) -> int {
    WaitForReadOnlyRoot();

    execvp(kFishExecutable, argv);
    std::println(std::cerr,
        "devicefs-wsl-startup-broker: could not execute '{}': {}",
        kFishExecutable,
        std::error_code{errno, std::generic_category()}.message());
    return EXIT_FAILURE;
}
