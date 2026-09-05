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

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Web.Http.h>
#include <winrt/Windows.Web.Http.Headers.h>

export module devicefs.supervisor.https_download;

import std;
import devicefs.common;
import devicefs.stream_writer;

#undef stderr
#undef stdout

// HTTP and storage operations wait synchronously, which C++/WinRT permits
// only in a multithreaded apartment. The caller owns that apartment so it
// also outlives the supplied HTTP client.
export [[nodiscard]] auto DownloadFile(
    const winrt::Windows::Web::Http::HttpClient &client,
    const winrt::Windows::Foundation::Uri &url,
    const std::filesystem::path &destination) -> std::uint64_t {
    using namespace winrt::Windows::Foundation;
    using namespace winrt::Windows::Storage;
    using namespace winrt::Windows::Storage::Streams;
    using namespace winrt::Windows::Web::Http;

    try {
        const auto folder = StorageFolder::GetFolderFromPathAsync(
            destination.parent_path().native()).get();
        const auto file = folder.CreateFileAsync(destination.filename().native()).get();
        const auto response = client.GetAsync(
            url, HttpCompletionOption::ResponseHeadersRead).get();
        response.EnsureSuccessStatusCode();
        const auto content_length = response.Content().Headers().ContentLength();
        const auto total = content_length ? content_length.Value() : 0;
        const auto name = destination.filename().string();
        const auto progress = AsyncOperationProgressHandler<std::uint64_t, std::uint64_t>{
            [total, name, next = std::make_shared<std::atomic<std::uint64_t>>(1)](
                const auto &, const std::uint64_t received) noexcept {
                auto step = next->load();
                while ((total == 0)
                    ? (received / (16 * 1024 * 1024) >= step)
                    : ((step < 10) &&
                        (received >= (total / 10) * step + (total % 10) * step / 10))) {
                    if (next->compare_exchange_weak(step, step + 1)) {
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
                }
            }};
        const auto copy = RandomAccessStream::CopyAndCloseAsync(
            response.Content().ReadAsInputStreamAsync().get(),
            file.OpenAsync(FileAccessMode::ReadWrite).get());
        try {
            copy.Progress(progress);
        } catch (const winrt::hresult_error &error) {
            devicefs::WriteToStream(devicefs::stderr,
                "backup-supervisor: could not report download progress for '{}' "
                "(Windows error 0x{:08x})\n",
                name, ExplicitWin32Error::FromHresult(error.code()).value);
        }
        const auto bytes = copy.get();
        devicefs::WriteToStream(devicefs::stdout,
            "backup-supervisor: download '{}' 100% complete\n", name);
        return bytes;
    } catch (const winrt::hresult_error &error) {
        WinError("could not download '{}' to '{}': {}",
            std::wstring_view{url.AbsoluteUri()},
            std::wstring_view{destination.native()},
            std::wstring_view{error.message()},
            ExplicitWin32Error::FromHresult(error.code()));
    }
}
