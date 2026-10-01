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

export module devicefs.vss_block_descriptors.cli;

import std;
import devicefs.guid_formatter;
import devicefs.svi_extents;
import devicefs.stream_writer;
import devicefs.vss_block_descriptors;

namespace {

struct Options {
    std::string_view source;
    std::string_view snapshot_identifier;
    std::string_view baseline_identifier;
    bool svi_extents = false;
    bool catalog = false;
    bool help = false;
};

auto Usage(const auto output) noexcept {
    devicefs::WriteToStream(
        output,
        "Usage: vss-descriptor-dump --source SOURCE --snapshot-id GUID\n"
        "       vss-descriptor-dump --source SOURCE --baseline-id A --snapshot-id B\n"
        "       vss-descriptor-dump --source SNAPSHOT --svi-extents\n"
        "       vss-descriptor-dump --source SOURCE --catalog\n\n"
        "Read raw VSS descriptors for one store or an A-inclusive, B-exclusive\n"
        "interval, inspect the catalog, or enumerate allocated SVI extents.\n\n"
        "Options:\n"
        "  --source SOURCE       Volume, snapshot device, or flat volume image\n"
        "  --snapshot-id GUID    Shadow-copy identifier to select\n"
        "  --baseline-id GUID    Read all stores from this copy up to --snapshot-id\n"
        "  --svi-extents         Print allocated SVI block offsets\n"
        "  --catalog             Print store IDs, creation FILETIMEs and offsets\n"
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
        } else if (argument == "--baseline-id") {
            result.baseline_identifier = next(index);
        } else if (argument == "--svi-extents") {
            result.svi_extents = true;
        } else if (argument == "--catalog") {
            result.catalog = true;
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
    const auto modes = std::array{!result.snapshot_identifier.empty(),
        result.svi_extents, result.catalog};
    if (std::ranges::count(modes, true) != 1) {
        throw std::invalid_argument(
            "exactly one of --snapshot-id, --svi-extents, or --catalog is required");
    }
    if (!result.baseline_identifier.empty() && result.snapshot_identifier.empty()) {
        throw std::invalid_argument("--baseline-id requires --snapshot-id");
    }
    return result;
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
        FormatGuid<false>(result.snapshot_identifier),
        FormatGuid<false>(result.store_identifier), result.volume_size,
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

[[nodiscard]] auto FormatCatalog(
    const std::vector<devicefs::vss::CatalogStore> &stores) {
    auto output = std::format("schema-version\t1\nstore-count\t{}\n",
        stores.size());
    // `index` records catalog traversal order; `creation-filetime` lets a
    // diagnostic compare that order with the stores' creation times.
    output += "columns\tindex\tstore-id\tsnapshot-id\tcreation-filetime\t"
        "volume-size\tin-volume\tlist-offset\theader-offset\n";
    for (auto index = 0uz; index < stores.size(); ++index) {
        const auto &store = stores[index];
        std::format_to(std::back_inserter(output),
            "store\t{}\t{}\t{}\t{}\t{}\t{}\t{:016x}\t{:016x}\n",
            index, FormatGuid<false>(store.store_identifier),
            store.snapshot_identifier
                ? FormatGuid<false>(*store.snapshot_identifier) : "-",
            store.creation_time, store.volume_size,
            store.has_in_volume_data ? 1 : 0,
            store.list_offset, store.header_offset);
    }
    return output;
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
    if (options.catalog) {
        const auto catalog = devicefs::vss::ReadStoreCatalog(options.source);
        WriteOutput(FormatCatalog(catalog));
        return 0;
    }
    const auto snapshot_identifier = ParseGuid(options.snapshot_identifier);
    if (!snapshot_identifier) {
        throw std::invalid_argument(std::format(
            "--snapshot-id is not a GUID: {}", options.snapshot_identifier));
    }
    if (!options.baseline_identifier.empty()) {
        const auto baseline = ParseGuid(options.baseline_identifier);
        if (!baseline) {
            throw std::invalid_argument(std::format(
                "--baseline-id is not a GUID: {}", options.baseline_identifier));
        }
        const auto interval = devicefs::vss::ReadBlockDescriptorIntervalCatalog(
            options.source, *baseline, *snapshot_identifier);
        const auto stores = devicefs::vss::ReadBlockDescriptors(
            options.source, interval.stores);
        auto output = std::format(
            "schema-version\t1\ninterval-baseline-id\t{}\n"
            "interval-payload-id\t{}\nstore-count\t{}\n",
            FormatGuid<false>(*baseline), FormatGuid<false>(*snapshot_identifier),
            stores.size());
        // The text between `begin-store` and `end-store` uses the single-store
        // schema, so both modes can use the same descriptor-output decoder.
        for (const auto &store : stores) {
            std::format_to(std::back_inserter(output),
                "begin-store\t{}\n{}end-store\n", store.catalog.creation_time,
                FormatResult(store.blocks));
        }
        WriteOutput(output);
        return 0;
    }
    const auto catalog = devicefs::vss::ReadStoreCatalog(options.source);
    const auto selected = devicefs::vss::SelectCatalogStore(
        catalog, *snapshot_identifier, options.source);
    const auto stores = devicefs::vss::ReadBlockDescriptors(
        options.source, std::array{selected});
    WriteOutput(FormatResult(stores.front().blocks));
    return 0;
}

} // namespace

export auto VssDescriptorDumpMain(
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
