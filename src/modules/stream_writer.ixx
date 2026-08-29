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

export module devicefs.stream_writer;

import std;

namespace devicefs::stream_writer_detail {

struct Stdout {};
struct Stderr {};

auto Print(Stdout, std::string_view) noexcept -> bool;
auto Print(Stderr, std::string_view) noexcept -> bool;

template <typename Output, typename Operation>
auto Write(
    const Output output,
    const Operation &operation) noexcept -> bool {
    try {
        auto text = std::string{};
        operation(std::back_inserter(text));
        return Print(output, text);
    } catch (...) {
        return false;
    }
}

template <typename Output>
auto WriteUtf8(
    Output output,
    const std::u8string_view text) -> Output {
    return std::ranges::transform(
        text,
        output,
        [](const char8_t value) noexcept {
            return std::bit_cast<char>(value);
        }).out;
}

} // namespace devicefs::stream_writer_detail

export namespace devicefs {

namespace stream_writer_detail {

template <typename Character, typename... Arguments>
class BasicFormatString {
  public:
    template <typename String>
        requires std::convertible_to<
            const String &, std::basic_string_view<Character>>
    [[gsl::suppress("26447",
        justification:
            "std::basic_format_string is constructed during constant "
            "evaluation, so no exception can escape at runtime.")]]
    consteval BasicFormatString(const String &format) noexcept
        : format_{format} {
    }

    [[nodiscard]] constexpr auto get() const noexcept
        -> const std::basic_format_string<Character, Arguments...> & {
        return format_;
    }

  private:
    std::basic_format_string<Character, Arguments...> format_;
};

template <typename... Arguments>
using FormatString = BasicFormatString<
    char, std::type_identity_t<Arguments>...>;

template <typename... Arguments>
using WFormatString = BasicFormatString<
    wchar_t, std::type_identity_t<Arguments>...>;

} // namespace stream_writer_detail

constexpr auto stdout = stream_writer_detail::Stdout{};
constexpr auto stderr = stream_writer_detail::Stderr{};

template <typename Output>
auto WriteToStream(
    const Output output,
    const std::u8string_view text) noexcept -> bool {
    return stream_writer_detail::Write(
        output,
        [text](const auto destination) {
            return stream_writer_detail::WriteUtf8(destination, text);
        });
}

template <typename Output, typename... Arguments>
auto WriteToStream(
    const Output output,
    const stream_writer_detail::FormatString<Arguments...> format,
    Arguments &&...arguments) noexcept -> bool {
    return stream_writer_detail::Write(
        output,
        [&](const auto destination) {
            return std::format_to(
                destination,
                format.get(),
                std::forward<Arguments>(arguments)...);
        });
}

template <typename Output, typename... Arguments>
auto WriteToStream(
    const Output output,
    const stream_writer_detail::WFormatString<Arguments...> format,
    Arguments &&...arguments) noexcept -> bool {
    return stream_writer_detail::Write(
        output,
        [&](const auto destination) {
            const auto text = std::format(
                format.get(), std::forward<Arguments>(arguments)...);
            return stream_writer_detail::WriteUtf8(
                destination, std::filesystem::path{text}.u8string());
        });
}

} // namespace devicefs
