// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

module;

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>

export module devicefs.asr_trace.filesystem;

import std;
import devicefs.terminal.safecast;

export namespace devicefs::asr_trace {

struct IoResult {
    std::size_t transferred = 0;
    int error = 0;
};

// Write describes a range only; destinations receive no write payload.
// Images advertise a fixed size. Reads beyond the initial image return zeroes;
// discarded writes acknowledge their full length. A failed transfer retains
// its accepted prefix in the result.
template <typename T>
concept ImageFilesystem = requires(T &filesystem,
    std::uint64_t offset, std::size_t length, std::span<char> destination) {
    { filesystem.Size } -> std::same_as<const std::uint64_t &>;
    { filesystem.Read(offset, destination) } -> std::same_as<IoResult>;
    { filesystem.Write(offset, length) } -> std::same_as<IoResult>;
    { filesystem.Synchronize() } -> std::same_as<int>;
};

}

namespace devicefs::asr_trace::detail {

static_assert(std::numeric_limits<std::size_t>::max() <=
    std::numeric_limits<std::uint64_t>::max());

auto ReadLength(const std::uint64_t size, const std::uint64_t offset,
    const std::size_t requested) noexcept -> std::size_t
    post(result: result <= requested) {
    return std::min<std::uint64_t>(requested, std::saturating_sub(size, offset));
}

template <typename Operation>
auto RetryInterrupted(Operation operation) {
    auto result = operation();
    while ((result < 0) && (errno == EINTR)) {
        result = operation();
    }
    return result;
}

}

export namespace devicefs::asr_trace {

auto OpenFile(const std::filesystem::path &path, const std::ios::openmode mode)
    -> std::fstream {
    auto file = std::fstream{path, mode | std::ios::binary};
    if (!file) {
        throw std::system_error{errno, std::generic_category(),
            std::format("open file '{}'", path.string())};
    }
    if (::fcntl(file.native_handle(), F_SETFD, FD_CLOEXEC) < 0) {
        throw std::system_error{errno, std::generic_category(),
            std::format("set close-on-exec for file '{}'", path.string())};
    }
    return file;
}

// Synthetic requests need only a fixed address space: reads return zeroes and
// successful writes acknowledge their length without retaining their payload.
class SyntheticFilesystem {
public:
    const std::uint64_t Size;

    explicit SyntheticFilesystem(const std::uint64_t size) : Size{size} {}

    auto Read(std::uint64_t, const std::span<char> destination)
        const noexcept -> IoResult {
        std::ranges::fill(destination, '\0');
        return {.transferred = destination.size()};
    }

    auto Write(std::uint64_t, const std::size_t length)
        const noexcept -> IoResult {
        return {.transferred = length};
    }

    auto Synchronize() const noexcept -> int {
        return 0;
    }
};

}

namespace devicefs::asr_trace::detail {

enum class ImageWritePolicy { Discard, Forward };

template <ImageWritePolicy WritePolicy>
class PositionalFilesystem {
    std::fstream file_;

public:
    const std::uint64_t Size;

    explicit PositionalFilesystem(const std::filesystem::path &path)
        : file_{OpenFile(path, WritePolicy == ImageWritePolicy::Forward
            ? std::ios::in | std::ios::out : std::ios::in)},
          Size{[descriptor = file_.native_handle(), &path] {
            struct stat info{};
            if (detail::RetryInterrupted([descriptor, &info] {
                return ::fstat(descriptor, &info);
            }) < 0) {
                throw std::system_error{errno, std::generic_category(),
                    std::format("inspect image file '{}'", path.string())};
            }
            static_assert(std::numeric_limits<decltype(info.st_size)>::digits <=
                std::numeric_limits<std::uint64_t>::digits);
            return devicefs::terminal::FailFastCast<std::uint64_t>(info.st_size);
        }()} {}

    auto Read(const std::uint64_t offset, const std::span<char> destination)
        -> IoResult {
        const auto length = detail::ReadLength(Size, offset, destination.size());
        const auto result = length == 0 ? IoResult{} :
            Transfer<false>(offset, destination.first(length), length);
        if (result.error != 0) {
            return result;
        }
        std::ranges::fill(destination.subspan(length), '\0');
        return {.transferred = destination.size()};
    }

    auto Write(const std::uint64_t offset, const std::size_t length)
        -> IoResult {
        if constexpr (WritePolicy == ImageWritePolicy::Forward) {
            if (length == 0) {
                return {};
            }
            // The mounted client generates virtual-file requests from lengths.
            // Reusing 64 KiB of zeroes bounds transient memory for any request.
            const auto zeroes = std::array<char, 64 * 1024>{};
            return Transfer<true>(offset, zeroes, length);
        } else {
            return {.transferred = length};
        }
    }

    auto Synchronize() -> int {
        if constexpr (WritePolicy == ImageWritePolicy::Forward) {
            if (detail::RetryInterrupted([descriptor = file_.native_handle()] {
                return ::fsync(descriptor);
            }) < 0) {
                return errno;
            }
        }
        return 0;
    }

private:
    template <bool Writing>
    auto Transfer(const std::uint64_t offset,
        const std::span<std::conditional_t<Writing, const char, char>> buffer,
        const std::size_t length)
        -> IoResult
        pre(Writing || (length <= buffer.size()))
        pre((length == 0) || !buffer.empty())
        post(result: result.transferred <= length) {
        auto result = IoResult{};
        while (result.transferred < length) {
            // POSIX leaves counts above SSIZE_MAX implementation-defined.
            // Short syscalls still advance only by their reported byte count.
            const auto buffer_offset = Writing ? 0uz : result.transferred;
            const auto count = std::min({length - result.transferred,
                buffer.size() - buffer_offset, std::size_t{SSIZE_MAX}});
            const auto position = offset + result.transferred;
            const auto transferred = detail::RetryInterrupted(
                [descriptor = file_.native_handle(), data = buffer.data() + buffer_offset,
                    count, position] {
                    return [: [] consteval {
                        return Writing ? ^^::pwrite : ^^::pread;
                    }() :](descriptor, data, count, position);
                });
            if (transferred < 0) {
                result.error = errno;
                return result;
            }
            if (transferred == 0) {
                // Read already clips to the fixed image extent, so zero here
                // is unexpected EOF rather than a successful image boundary.
                result.error = EIO;
                return result;
            }
            result.transferred += transferred;
        }
        return result;
    }
};

}

export namespace devicefs::asr_trace {

// Seed reads expose the initially prepared image. Its descriptor is read-only;
// accepted writes neither change the seed nor populate a payload cache.
using SeedFilesystem = detail::PositionalFilesystem<detail::ImageWritePolicy::Discard>;

// This client adapter sends requests through image.raw on the mounted
// filesystem, which owns request logging and the readback and rolling write caches.
using MountedImageFilesystem = detail::PositionalFilesystem<detail::ImageWritePolicy::Forward>;

static_assert(ImageFilesystem<SyntheticFilesystem>);
static_assert(ImageFilesystem<SeedFilesystem>);
static_assert(ImageFilesystem<MountedImageFilesystem>);

}
