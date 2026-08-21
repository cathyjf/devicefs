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

#include <windows.h>
#include <intrin.h>

export module devicefs.stream_writer;

import std;

namespace devicefs::stream_writer_detail {

template <typename Operation>
[[gsl::suppress("26447",
    justification: "Output streams with exceptions enabled will fail fast.")]]
auto Write(
    std::ostream &output,
    const Operation &operation) noexcept -> std::ostream & {
    if (output.exceptions() != std::ios_base::goodbit) [[unlikely]] {
        __fastfail(FAST_FAIL_INVALID_ARG);
    }
    auto failed = false;
    try {
        const auto ready = std::ostream::sentry{output};
        if (!ready) {
            return output;
        }
        failed = operation(
            std::ostreambuf_iterator<char>{output}).failed();
    } catch (...) {
        failed = true;
    }
    if (failed) {
        output.setstate(std::ios_base::badbit);
    }
    return output;
}

auto WriteUtf8(
    std::ostreambuf_iterator<char> output,
    const std::u8string_view text) -> std::ostreambuf_iterator<char> {
    return std::ranges::transform(
        text,
        output,
        [](const char8_t value) noexcept {
            return std::bit_cast<char>(value);
        }).out;
}

} // namespace devicefs::stream_writer_detail

export namespace devicefs {

auto WriteToStream(
    std::ostream &output,
    const std::u8string_view text) noexcept -> std::ostream & {
    return stream_writer_detail::Write(
        output,
        [text](const auto destination) {
            return stream_writer_detail::WriteUtf8(destination, text);
        });
}

template <typename... Arguments>
auto WriteToStream(
    std::ostream &output,
    const std::format_string<Arguments...> format,
    Arguments &&...arguments) noexcept -> std::ostream & {
    return stream_writer_detail::Write(
        output,
        [&](const auto destination) {
            return std::format_to(
                destination,
                format,
                std::forward<Arguments>(arguments)...);
        });
}

template <typename... Arguments>
auto WriteToStream(
    std::ostream &output,
    const std::wformat_string<Arguments...> format,
    Arguments &&...arguments) noexcept -> std::ostream & {
    return stream_writer_detail::Write(
        output,
        [&](const auto destination) {
            const auto text = std::format(
                format, std::forward<Arguments>(arguments)...);
            return stream_writer_detail::WriteUtf8(
                destination, std::filesystem::path{text}.u8string());
        });
}

} // namespace devicefs
