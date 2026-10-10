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

// As a result of an apparent compiler defect, when certain WinRT headers are
// imported (instead of included), MSVC++ is able to find the declaration of
// `memcpy_s` but not the definition of it, even though both the declaration
// and definition are contained within `corecrt_memcpy_s.h` and the compiler
// should presumably either find both or neither.
//
// Textually including `corecrt_memcpy_s.h` in this file resolves the defect.
#include <corecrt_memcpy_s.h>

export module devicefs.supervisor.https_download;

import std;
import <devicefs/common.h>;
import <devicefs/windows_imports.h>;
import <devicefs/winrt_imports.h>;
import devicefs.stream_writer;
import devicefs.terminal.transcoding;

using devicefs::terminal::Transcode;

namespace {

using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Storage::Streams;

// Adapt a borrowed synchronous file handle for WinRT stream-copy operations.
// The caller keeps the handle alive until the copy completes. Closing this
// adapter detaches it from the file without closing the caller's handle.
class NativeFileOutputStream
    : public winrt::implements<NativeFileOutputStream, IOutputStream, IClosable> {
public:
    explicit NativeFileOutputStream(
        _Pre_satisfies_(file != INVALID_HANDLE_VALUE) const HANDLE file)
        : file_{file} {}

    auto WriteAsync(const IBuffer &buffer)
        -> IAsyncOperationWithProgress<std::uint32_t, std::uint32_t> {
        auto written = DWORD{};
        winrt::check_bool(WriteFile(file_, buffer.data(), buffer.Length(),
            &written, nullptr));
        co_return written;
    }

    auto FlushAsync() -> IAsyncOperation<bool> {
        winrt::check_bool(FlushFileBuffers(file_));
        co_return true;
    }

    auto Close() noexcept -> void {
        file_ = INVALID_HANDLE_VALUE;
    }

private:
    HANDLE file_;
};

} // namespace

// Return an HTTP client with caching disabled.
//
// DeviceFs does not download the same installer or OCI layer again after
// installation, so caching these downloads only wastes disk space.
// https://learn.microsoft.com/en-us/uwp/api/windows.web.http.filters.httpcachecontrol
export [[nodiscard]] auto MakeUncachedHttpClient() {
    using namespace winrt::Windows::Web::Http::Filters;
    const auto filter = HttpBaseProtocolFilter{};
    filter.CacheControl().ReadBehavior(HttpCacheReadBehavior::NoCache);
    filter.CacheControl().WriteBehavior(HttpCacheWriteBehavior::NoCache);
    return winrt::Windows::Web::Http::HttpClient{filter};
}

// HTTP and storage operations wait synchronously, which C++/WinRT permits
// only in a multithreaded apartment. The caller owns that apartment so it
// also outlives the supplied HTTP client.
// The caller keeps the synchronous write handle `output` alive until return.
// The path `destination` is used only for progress messages and diagnostics.
export [[nodiscard]] auto DownloadFileIntoHandle(
    const winrt::Windows::Web::Http::HttpClient &client,
    const winrt::Windows::Foundation::Uri &url,
    const std::filesystem::path &destination,
    const HANDLE output) -> std::uint64_t {
    using namespace winrt::Windows::Foundation;
    using namespace winrt::Windows::Storage::Streams;
    using namespace winrt::Windows::Web::Http;

    try {
        const auto response = client.GetAsync(
            url, HttpCompletionOption::ResponseHeadersRead).get();
        response.EnsureSuccessStatusCode();
        const auto content_length = response.Content().Headers().ContentLength();
        const auto total = content_length ? content_length.Value() : 0;
        const auto name = Transcode<std::string>(destination.filename().native());
        const auto progress = AsyncOperationProgressHandler<std::uint64_t, std::uint64_t>{
            [total, name, next = std::make_shared<std::atomic<std::uint64_t>>(1)](
                const auto &, const std::uint64_t received) noexcept {
                auto step = next->load();
                while ((total == 0)
                    ? (received / (16 * 1024 * 1024) >= step)
                    : ((step < 10) &&
                        (received >= (total / 10) * step + (total % 10) * step / 10))) {
                    if (!next->compare_exchange_weak(step, step + 1)) {
                        continue;
                    }
                    if (total == 0) {
                        devicefs::WriteToStream(devicefs::stdout,
                            "backup-supervisor: download '{}' received {} MiB\n",
                            name, step * 16);
                    } else {
                        devicefs::WriteToStream(devicefs::stdout,
                            "backup-supervisor: download '{}' {}% complete\n",
                            name, step * 10);
                    }
                    ++step;
                }
            }};
        const auto copy = RandomAccessStream::CopyAndCloseAsync(
            response.Content().ReadAsInputStreamAsync().get(),
            winrt::make<NativeFileOutputStream>(output));
        try {
            copy.Progress(progress);
        } catch (const winrt::hresult_error &error) {
            devicefs::WriteToStream(devicefs::stderr,
                "backup-supervisor: could not report download progress "
                "for '{}': {}\n",
                name, TryConstructWinError("{}",
                    std::wstring_view{error.message()}, ExplicitHresult{error.code()}));
        }
        const auto bytes = copy.get();
        devicefs::WriteToStream(devicefs::stdout,
            "backup-supervisor: download '{}' 100% complete\n", name);
        return bytes;
    } catch (const winrt::hresult_error &error) {
        WinError("failed to download '{}' to '{}': {}",
            std::wstring_view{url.AbsoluteUri()},
            std::wstring_view{destination.native()},
            std::wstring_view{error.message()},
            ExplicitHresult{error.code()});
    }
}

// Create or replace `destination` and download into it.
export [[nodiscard]] auto DownloadFileIntoPath(
    const winrt::Windows::Web::Http::HttpClient &client,
    const winrt::Windows::Foundation::Uri &url,
    const std::filesystem::path &destination) -> std::uint64_t {
    const auto file = wil::unique_hfile{CreateFileW(destination.c_str(),
        GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr)};
    if (!file) {
        WinError("failed to create download destination '{}'",
            std::wstring_view{destination.native()});
    }
    return DownloadFileIntoHandle(client, url, destination, file.get());
}
