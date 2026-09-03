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

import std;
import devicefs.svi_extents;
import devicefs.stream_writer;
import devicefs.vss_block_descriptors;

namespace {

struct Options {
    std::string_view source;
    std::string_view snapshot_identifier;
    bool svi_extents = false;
    bool help = false;
};

auto Usage(const auto output) noexcept {
    devicefs::WriteToStream(
        output,
        "Usage: vss-descriptor-dump --source SOURCE --snapshot-id GUID\n"
        "       vss-descriptor-dump --source SNAPSHOT --svi-extents\n\n"
        "Read one VSS store's raw block descriptors or the allocated "
        "System Volume Information extents of a snapshot.\n\n"
        "Options:\n"
        "  --source SOURCE       Volume, snapshot device, or flat volume image\n"
        "  --snapshot-id GUID    Shadow-copy identifier to select\n"
        "  --svi-extents         Print allocated SVI block offsets\n"
        "  -h, --help            Show this help\n");
}

[[nodiscard]] auto ParseOptions(
    const std::span<const std::string_view> arguments) {
    auto result = Options{};
    const auto next = [&](auto &index) {
        if (++index == arguments.size()) {
            throw std::invalid_argument(std::format(
                "{} requires a value", arguments[index - 1]));
        }
        return arguments[index];
    };

    for (auto index = 0uz; index < arguments.size(); ++index) {
        const auto argument = arguments[index];
        if ((argument == "-h") || (argument == "--help")) {
            result.help = true;
        } else if (argument == "--source") {
            result.source = next(index);
        } else if (argument == "--snapshot-id") {
            result.snapshot_identifier = next(index);
        } else if (argument == "--svi-extents") {
            result.svi_extents = true;
        } else {
            throw std::invalid_argument(std::format(
                "unknown option '{}' at argument {}", argument, index + 1));
        }
    }
    if (result.help) {
        return result;
    }
    if (result.source.empty()) {
        throw std::invalid_argument("--source is required");
    }
    if (result.snapshot_identifier.empty() && !result.svi_extents) {
        throw std::invalid_argument(
            "--snapshot-id or --svi-extents is required");
    }
    if (!result.snapshot_identifier.empty() && result.svi_extents) {
        throw std::invalid_argument(
            "--snapshot-id and --svi-extents cannot be combined");
    }
    return result;
}

[[nodiscard]] auto ParseGuid(const std::string_view value) {
    auto text = std::filesystem::path{value}.wstring();
    if (!text.starts_with(L'{')) {
        text = std::format(L"{{{}}}", text);
    }
    auto result = GUID{};
    if (FAILED(CLSIDFromString(text.c_str(), &result))) {
        throw std::invalid_argument(std::format(
            "--snapshot-id is not a GUID: {}", value));
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

[[nodiscard]] auto FormatSviExtents(
    const std::set<std::uint64_t> &offsets) {
    auto output = std::format(
        "schema-version\t1\n"
        "block-size\t{}\n"
        "block-count\t{}\n",
        devicefs::vss::kBlockSize, offsets.size());
    for (const auto offset : offsets) {
        std::format_to(
            std::back_inserter(output), "block\t{:016x}\n", offset);
    }
    return output;
}

auto WriteOutput(const std::string_view output) {
    if (!devicefs::WriteToStream(devicefs::stdout, "{}", output)) {
        throw std::runtime_error("could not write output");
    }
}

auto Run(const std::span<const std::string_view> arguments) {
    const auto options = ParseOptions(arguments);
    if (options.help) {
        Usage(devicefs::stdout);
        return 0;
    }
    if (options.svi_extents) {
        const auto offsets =
            devicefs::svi::ReadBlockOffsets(options.source);
        WriteOutput(FormatSviExtents(offsets));
        return 0;
    }
    const auto snapshot_identifier = ParseGuid(options.snapshot_identifier);
    const auto result = devicefs::vss::ReadBlockDescriptors(
        options.source, snapshot_identifier);
    WriteOutput(FormatResult(result));
    return 0;
}

} // namespace

auto VssDescriptorDumpMain(
    const std::span<const std::string_view> arguments) -> int {
    try {
        return Run(arguments);
    } catch (const std::invalid_argument &error) {
        devicefs::WriteToStream(
            devicefs::stderr, "vss-descriptor-dump: {}\n\n", error.what());
        Usage(devicefs::stderr);
        return 2;
    }
}
