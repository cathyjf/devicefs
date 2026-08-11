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

module;

#include <wil/stl.h>

#include <atomic>
#include <bit>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <memory>
#include <span>

export module devicefs.free_cluster_measurement;

export class FreeClusterMeasurement {
    static constexpr auto kClustersPerClaimWord =
        sizeof(std::uint64_t) * CHAR_BIT;

    [[nodiscard]] static auto CountFreeClusters(
        const std::span<const unsigned char> bitmap,
        const std::uint64_t cluster_count) noexcept {
        auto allocated_clusters = std::uint64_t{};
        const auto full_bytes = cluster_count / CHAR_BIT;
        for (auto i = 0uz; i < full_bytes; ++i) {
            allocated_clusters += std::popcount(bitmap[i]);
        }
        const auto remaining_bits = cluster_count % CHAR_BIT;
        if (remaining_bits != 0) {
            const auto mask = (1u << remaining_bits) - 1;
            allocated_clusters += std::popcount(bitmap[full_bytes] & mask);
        }
        return cluster_count - allocated_clusters;
    }

    [[nodiscard]] auto IsAllocated(const std::uint64_t cluster) const noexcept {
        if (cluster >= cluster_count_) {
            return true;
        }
        return (bitmap_[cluster / CHAR_BIT] &
            (1u << (cluster % CHAR_BIT))) != 0;
    }

    [[nodiscard]] auto Claim(const std::uint64_t cluster) noexcept {
        const auto word_index = cluster / kClustersPerClaimWord;
        const auto mask = 1ULL << (cluster % kClustersPerClaimWord);
        return (claims_[word_index].fetch_or(
            mask, std::memory_order_relaxed) & mask) == 0;
    }

public:
    FreeClusterMeasurement(
        const std::span<const unsigned char> bitmap,
        const std::uint32_t cluster_size,
        const std::uint64_t cluster_count)
        : bitmap_(bitmap),
          cluster_size_(cluster_size),
          cluster_count_(cluster_count),
          claims_(std::make_unique<std::atomic<std::uint64_t>[]>(
              cluster_count / kClustersPerClaimWord +
              ((cluster_count % kClustersPerClaimWord) != 0))),
          total_free_clusters_(CountFreeClusters(bitmap, cluster_count)) {
    }

    auto ObserveRead(
        const std::span<const unsigned char> data,
        const std::uint64_t offset) noexcept {
        if (data.empty()) {
            return;
        }

        const auto end = offset + data.size();
        const auto first_cluster = offset / cluster_size_;
        const auto last_cluster = (end - 1) / cluster_size_;
        auto fully_examined_clusters = std::uint64_t{};
        auto nonzero_clusters = std::uint64_t{};
        auto nonzero_bytes = std::uint64_t{};

        // Count a cluster only when this read returned all of its bytes.
        for (auto cluster = first_cluster; cluster <= last_cluster; ++cluster) {
            const auto cluster_begin = cluster * cluster_size_;
            const auto cluster_end = cluster_begin + cluster_size_;
            if (IsAllocated(cluster) || (cluster_begin < offset) ||
                (cluster_end > end) || !Claim(cluster)) {
                continue;
            }

            ++fully_examined_clusters;
            auto cluster_nonzero_bytes = std::uint64_t{};
            const auto cluster_data = data.subspan(
                cluster_begin - offset, cluster_size_);
            for (const auto value : cluster_data) {
                cluster_nonzero_bytes += value != 0;
            }
            nonzero_bytes += cluster_nonzero_bytes;
            nonzero_clusters += cluster_nonzero_bytes != 0;
        }
        if (fully_examined_clusters == 0) {
            return;
        }
        fully_examined_clusters_.fetch_add(
            fully_examined_clusters, std::memory_order_relaxed);
        nonzero_clusters_.fetch_add(
            nonzero_clusters, std::memory_order_relaxed);
        nonzero_bytes_.fetch_add(nonzero_bytes, std::memory_order_relaxed);
    }

    auto Report(const wil::zwstring_view name) const noexcept {
        const auto fully_examined_clusters =
            fully_examined_clusters_.load(std::memory_order_relaxed);
        const auto nonzero_clusters =
            nonzero_clusters_.load(std::memory_order_relaxed);
        const auto nonzero_bytes =
            nonzero_bytes_.load(std::memory_order_relaxed);
        std::fwprintf(stderr,
            L"devicefs: free-cluster measurement for '%ls': "
            L"%llu of %llu bitmap-free clusters fully examined in one read; "
            L"%llu of those clusters contained nonzero data (%llu bytes total)\n",
            name.c_str(),
            fully_examined_clusters,
            total_free_clusters_,
            nonzero_clusters,
            nonzero_bytes);
    }

private:
    const std::span<const unsigned char> bitmap_;
    const std::uint32_t cluster_size_;
    const std::uint64_t cluster_count_;
    std::unique_ptr<std::atomic<std::uint64_t>[]> claims_;
    const std::uint64_t total_free_clusters_;
    std::atomic<std::uint64_t> fully_examined_clusters_ = 0;
    std::atomic<std::uint64_t> nonzero_clusters_ = 0;
    std::atomic<std::uint64_t> nonzero_bytes_ = 0;
};
