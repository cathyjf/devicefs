// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.

#include <windows.h>
#include <objbase.h>

#include <cstdio>

import std;
import devicefs.common;
import devicefs.vss_block_descriptors;

namespace {

struct Options {
    std::wstring_view source;
    std::wstring_view snapshot_identifier;
    bool help = false;
};

auto Usage(std::FILE *const output) noexcept {
    std::fputws(
        L"Usage: vss-descriptor-dump --source SOURCE --snapshot-id GUID\n\n"
        L"Read one VSS store's raw block descriptors from an NTFS volume or "
        L"flat volume image.\n\n"
        L"Options:\n"
        L"  --source SOURCE       Live volume path or flat raw-volume image\n"
        L"  --snapshot-id GUID    Shadow-copy identifier to select\n"
        L"  -h, --help            Show this help\n",
        output);
}

[[nodiscard]] auto ParseOptions(
    const std::span<const wchar_t *const> arguments) {
    auto result = Options{};
    const auto next = [&](auto &index) {
        if (++index == arguments.size()) {
            throw std::invalid_argument(std::format(
                "missing value after argument {}", index));
        }
        return std::wstring_view{arguments[index]};
    };

    for (auto index = 0uz; index < arguments.size(); ++index) {
        const auto argument = std::wstring_view{arguments[index]};
        if ((argument == L"-h") || (argument == L"--help")) {
            result.help = true;
        } else if (argument == L"--source") {
            result.source = next(index);
        } else if (argument == L"--snapshot-id") {
            result.snapshot_identifier = next(index);
        } else {
            throw std::invalid_argument(std::format(
                "unknown option at argument {}", index + 1));
        }
    }
    if (!result.help &&
        (result.source.empty() || result.snapshot_identifier.empty())) {
        throw std::invalid_argument(
            "--source and --snapshot-id are required");
    }
    return result;
}

[[nodiscard]] auto ParseGuid(const std::wstring_view value) {
    auto text = std::wstring{value};
    if (!text.starts_with(L'{')) {
        text = std::format(L"{{{}}}", text);
    }
    auto result = GUID{};
    if (FAILED(CLSIDFromString(text.c_str(), &result))) {
        throw std::invalid_argument("--snapshot-id is not a GUID");
    }
    return result;
}

[[nodiscard]] auto FormatGuid(const GUID &value) {
    return std::format(
        "{:08x}-{:04x}-{:04x}-{:02x}{:02x}-"
        "{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}",
        value.Data1, value.Data2, value.Data3,
        value.Data4[0], value.Data4[1], value.Data4[2], value.Data4[3],
        value.Data4[4], value.Data4[5], value.Data4[6], value.Data4[7]);
}

[[nodiscard]] auto FormatResult(
    const devicefs::vss::StoreBlockDescriptors &result) {
    const auto forwarders = std::ranges::count_if(
        result.descriptors, [](const auto &descriptor) {
            return (descriptor.flags & devicefs::vss::kForwarderFlag) != 0;
        });
    const auto overlays = std::ranges::count_if(
        result.descriptors, [](const auto &descriptor) {
            return (descriptor.flags & devicefs::vss::kOverlayFlag) != 0;
        });

    auto output = std::string{};
    std::format_to(std::back_inserter(output),
        "schema-version\t1\n"
        "snapshot-id\t{}\n"
        "store-id\t{}\n"
        "volume-size\t{}\n"
        "list-block-count\t{}\n"
        "descriptor-count\t{}\n"
        "forwarder-count\t{}\n"
        "overlay-count\t{}\n",
        FormatGuid(result.snapshot_identifier),
        FormatGuid(result.store_identifier), result.volume_size,
        result.list_block_count, result.descriptors.size(),
        forwarders, overlays);
    for (const auto &descriptor : result.descriptors) {
        std::format_to(std::back_inserter(output),
            "descriptor\t{:016x}\t{:016x}\t{:016x}\t{:08x}\t{:08x}\n",
            descriptor.original_offset, descriptor.relative_offset,
            descriptor.store_offset, descriptor.flags, descriptor.bitmap);
    }
    return output;
}

auto WriteOutput(const std::string_view output) {
    if (std::fwrite(output.data(), 1, output.size(), stdout) != output.size()) {
        throw std::runtime_error("could not write descriptor output");
    }
    if (std::fflush(stdout) != 0) {
        throw std::runtime_error("could not flush descriptor output");
    }
}

auto Run(const std::span<const wchar_t *const> arguments) {
    const auto options = ParseOptions(arguments);
    if (options.help) {
        Usage(stdout);
        return 0;
    }
    const auto snapshot_identifier = ParseGuid(options.snapshot_identifier);
    const auto result = devicefs::vss::ReadBlockDescriptors(
        options.source, snapshot_identifier);
    WriteOutput(FormatResult(result));
    return 0;
}

} // namespace

auto wmain(const int argc, wchar_t **const argv) -> int {
    try {
        HardenProcess();
        try {
            return Run({argv + 1, argv + argc});
        } catch (const std::invalid_argument &error) {
            std::fwprintf(stderr, L"vss-descriptor-dump: %hs\n\n", error.what());
            Usage(stderr);
            return 2;
        }
    } catch (const std::runtime_error &error) {
        std::fwprintf(stderr, L"vss-descriptor-dump: %hs\n", error.what());
        return 1;
    }
}
