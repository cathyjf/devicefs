// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

export module devicefs.asr_trace.synthetic;
import std;
import devicefs.asr_trace.filesystem;

export namespace devicefs::asr_trace {

struct SerialWriting {
    static auto BlockIndex(const std::uint64_t position, const std::uint64_t count)
        -> std::uint64_t
        pre(position < count) {
        return position;
    }
};

struct JumpAroundWriting {
    static auto BlockIndex(const std::uint64_t position, const std::uint64_t count)
        -> std::uint64_t
        pre(position < count) {
        return (position % 2 == 0) ? count - 1 - position / 2 : position / 2;
    }
};

template <typename Policy>
concept WritingPolicy = requires(std::uint64_t position, std::uint64_t count) {
    { Policy::BlockIndex(position, count) } -> std::same_as<std::uint64_t>;
};

// The policy selects offsets only. Requests contain metadata, so no producer
// payload can enter a destination implementation.
template <WritingPolicy Policy>
class SyntheticProducer final {
public:
    template <ImageFilesystem Filesystem>
    auto Produce(Filesystem &filesystem) const -> void {
        const auto size = filesystem.Size;
        constexpr auto block_size = 65536ULL;
        const auto count = size / block_size + (size % block_size != 0);
        for (auto position = std::uint64_t{}; position < count; ++position) {
            const auto block = Policy::BlockIndex(position, count);
            contract_assert(block < count);
            const auto offset = block * block_size;
            const auto length = std::min<std::uint64_t>(block_size, size - offset);
            const auto result = filesystem.Write(offset, length);
            RequireTransfer(result, length, offset);
        }
        if (const auto error = filesystem.Synchronize(); error != 0) {
            throw std::system_error{error, std::generic_category(),
                "synthetic image fsync failed"};
        }
    }

private:
    static auto RequireTransfer(const IoResult result, const std::size_t requested,
        const std::uint64_t offset) -> void {
        if (result.error != 0) {
            throw std::system_error{result.error, std::generic_category(),
                std::format("synthetic write at offset {} after {} bytes",
                    offset, result.transferred)};
        }
        if (result.transferred != requested) {
            throw std::runtime_error{
                std::format("synthetic write at offset {} transferred {} of {} bytes",
                    offset, result.transferred, requested)};
        }
    }
};

} // namespace devicefs::asr_trace
