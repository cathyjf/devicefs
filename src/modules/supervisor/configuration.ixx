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

// As a result of an apparent compiler defect, when the below WIL headers are
// imported (instead of included), MSVC++ is able to find the declaration of
// `StringValidateDestW` but not the definition of it, even though both the
// declaration and definition are contained within `strsafe.h` and the compiler
// should presumably either find both or neither. To work around this issue, we
// explicitly include `strsafe.h`.
#include <strsafe.h>

export module devicefs.supervisor.configuration;

import std;
import <wil/resource.h>;
import <wil/safecast.h>;
import <wil/stl.h>;
import <winrt/Windows.Data.Json.h>;
import <winrt/Windows.Foundation.Collections.h>;
import devicefs.supervisor.winrt_apartment;

// The Windows GetObject macro conflicts with C++/WinRT IJsonValue::GetObject.
#undef GetObject

export using SecureUtf8String = std::basic_string<
    char8_t, std::char_traits<char8_t>, wil::secure_allocator<char8_t>>;

export struct BackupConfiguration {
    std::wstring windows_username;
    wil::secure_wstring windows_password;
    std::wstring wsl_distribution;
    std::optional<std::wstring> wsl_linux_user;
    std::u8string wsl_client_path;
    std::u8string pbs_server;
    std::uint16_t pbs_port = 0;
    std::u8string pbs_datastore;
    std::u8string pbs_auth_id;
    std::u8string pbs_namespace;
    bool pbs_parallelize_image_upload = false;
    std::u8string pbs_fingerprint;
    SecureUtf8String pbs_authentication_secret;
    SecureUtf8String pbs_encryption_key;
    std::vector<std::wstring> volumes;
};

namespace {

using winrt::Windows::Data::Json::JsonObject;
using winrt::Windows::Data::Json::IJsonValue;
using winrt::Windows::Data::Json::JsonValueType;

[[noreturn]] auto ConfigurationError(
    const std::string_view member,
    const std::string_view requirement) {
    throw std::runtime_error(std::format(
        "configuration member '{}' {}", member, requirement));
}

[[nodiscard]] auto ReadString(
    const IJsonValue &value,
    const std::string_view member) {
    if (value.ValueType() != JsonValueType::String) {
        ConfigurationError(member, "must be a string");
    }
    const auto result = value.GetString();
    if (std::ranges::contains(result, L'\0')) {
        ConfigurationError(member, "must not contain a null character");
    }
    return result;
}

template <typename String>
[[nodiscard]] auto CopyWide(const winrt::hstring &value) {
    return String{value.begin(), value.end()};
}

template <typename String>
[[nodiscard]] auto ToUtf8(const winrt::hstring &value) {
    const auto size = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.c_str(), -1,
        nullptr, 0, nullptr, nullptr);
    if (size == 0) {
        throw std::runtime_error(
            "could not convert a backup configuration value to UTF-8");
    }
    static_assert(std::in_range<typename String::size_type>(
        std::numeric_limits<int>::max()));
    auto result = String(
        wil::safe_cast_failfast<typename String::size_type>(size),
        typename String::value_type{});
    void *const output = result.data();
    const auto converted = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.c_str(), -1,
        static_cast<char *>(output),
        size, nullptr, nullptr);
    if (converted != size) {
        throw std::runtime_error(
            "could not convert a backup configuration value to UTF-8");
    }
    result.pop_back();
    return result;
}

template <typename Value>
struct ReferenceDestination {
    explicit ReferenceDestination(Value &value) noexcept : value(value) {}

    std::reference_wrapper<Value> value;
};

template <typename String>
struct WideTextDestination : ReferenceDestination<String> {
    using ReferenceDestination<String>::ReferenceDestination;

    static constexpr auto kTemplateValue = std::string_view{"\"\""};
};

template <typename String>
struct Utf8TextDestination : ReferenceDestination<String> {
    using ReferenceDestination<String>::ReferenceDestination;

    static constexpr auto kTemplateValue = std::string_view{"\"\""};
};

struct OptionalWideStringDestination :
    ReferenceDestination<std::optional<std::wstring>> {
    using ReferenceDestination<
        std::optional<std::wstring>>::ReferenceDestination;

    static constexpr auto kTemplateValue = std::string_view{"null"};
};

struct OptionalUtf8StringDestination :
    ReferenceDestination<std::u8string> {
    using ReferenceDestination<std::u8string>::ReferenceDestination;

    static constexpr auto kTemplateValue = std::string_view{"\"\""};
};

struct OptionalBooleanDestination : ReferenceDestination<bool> {
    using ReferenceDestination<bool>::ReferenceDestination;

    static constexpr auto kTemplateValue = std::string_view{"false"};
};

struct PortDestination {
    PortDestination(
        std::uint16_t &value,
        const std::uint16_t default_value) noexcept
        : value(value), default_value(default_value) {}

    std::reference_wrapper<std::uint16_t> value;
    std::uint16_t default_value;
};

struct SerializedJsonObjectDestination :
    ReferenceDestination<SecureUtf8String> {
    using ReferenceDestination<SecureUtf8String>::ReferenceDestination;

    static constexpr auto kTemplateValue = std::string_view{"{}"};
};

struct NonemptyWideStringArrayDestination :
    ReferenceDestination<std::vector<std::wstring>> {
    using ReferenceDestination<
        std::vector<std::wstring>>::ReferenceDestination;

    static constexpr auto kTemplateValue = std::string_view{"[]"};
};

struct ConfigurationField;
using ConfigurationFields = std::vector<ConfigurationField>;

using FieldDestination = std::variant<
    WideTextDestination<std::wstring>,
    WideTextDestination<wil::secure_wstring>,
    Utf8TextDestination<std::u8string>,
    Utf8TextDestination<SecureUtf8String>,
    OptionalWideStringDestination,
    OptionalUtf8StringDestination,
    OptionalBooleanDestination,
    PortDestination,
    SerializedJsonObjectDestination,
    NonemptyWideStringArrayDestination,
    ConfigurationFields>;

struct ConfigurationField {
    ConfigurationField(
        const std::string_view name,
        FieldDestination destination) noexcept
        : name(name), destination(std::move(destination)) {}

    std::string_view name;
    FieldDestination destination;
};

[[nodiscard]] auto ConfigurationDescription(
    BackupConfiguration &configuration) {
    return ConfigurationFields{
        {"windows_account", ConfigurationFields{
            {"username",
                WideTextDestination{configuration.windows_username}},
            {"password",
                WideTextDestination{configuration.windows_password}},
        }},
        {"wsl", ConfigurationFields{
            {"distribution",
                WideTextDestination{configuration.wsl_distribution}},
            {"linux_user",
                OptionalWideStringDestination{configuration.wsl_linux_user}},
            {"client_path",
                Utf8TextDestination{configuration.wsl_client_path}},
        }},
        {"pbs", ConfigurationFields{
            {"server", Utf8TextDestination{configuration.pbs_server}},
            {"port", PortDestination{configuration.pbs_port, 8007}},
            {"datastore",
                Utf8TextDestination{configuration.pbs_datastore}},
            {"auth_id", Utf8TextDestination{configuration.pbs_auth_id}},
            {"namespace",
                OptionalUtf8StringDestination{configuration.pbs_namespace}},
            {"parallelize_image_upload", OptionalBooleanDestination{
                configuration.pbs_parallelize_image_upload}},
            {"fingerprint",
                Utf8TextDestination{configuration.pbs_fingerprint}},
            {"authentication_secret", Utf8TextDestination{
                configuration.pbs_authentication_secret}},
            {"encryption_key", SerializedJsonObjectDestination{
                configuration.pbs_encryption_key}},
        }},
        {"volumes",
            NonemptyWideStringArrayDestination{configuration.volumes}},
    };
}

auto WriteTemplateFields(
    std::string &output,
    const ConfigurationFields &fields,
    std::size_t indentation) -> void;

template <typename Destination>
concept FixedTemplateValueDestination = requires {
    Destination::kTemplateValue;
};

struct TemplateValueWriter {
    TemplateValueWriter(
        std::string &output,
        const std::size_t indentation) noexcept
        : output(output), indentation(indentation) {}

    template <FixedTemplateValueDestination Destination>
    auto operator()(const Destination &) const -> void {
        output.get().append(Destination::kTemplateValue);
    }

    auto operator()(const PortDestination &destination) const -> void {
        std::format_to(
            std::back_inserter(output.get()),
            "{}", destination.default_value);
    }

    auto operator()(const ConfigurationFields &fields) const -> void {
        WriteTemplateFields(output.get(), fields, indentation);
    }

    std::reference_wrapper<std::string> output;
    std::size_t indentation;
};

struct DefaultFieldReader {
    template <typename Destination>
    static auto operator()(const Destination &) noexcept {
        return false;
    }

    static auto operator()(
        const OptionalWideStringDestination &destination) noexcept {
        destination.value.get().reset();
        return true;
    }

    static auto operator()(
        const OptionalUtf8StringDestination &destination) noexcept {
        destination.value.get().clear();
        return true;
    }

    static auto operator()(
        const OptionalBooleanDestination &destination) noexcept {
        destination.value.get() = false;
        return true;
    }

    static auto operator()(const PortDestination &destination) noexcept {
        destination.value.get() = destination.default_value;
        return true;
    }
};

auto WriteTemplateFields(
    std::string &output,
    const ConfigurationFields &fields,
    const std::size_t indentation) -> void {
    output.append("{\n");
    for (auto index = std::size_t{}; index < fields.size(); ++index) {
        const auto &field = fields[index];
        output.append(indentation + 2, ' ');
        std::format_to(
            std::back_inserter(output), "\"{}\": ", field.name);
        std::visit(
            TemplateValueWriter{output, indentation + 2}, field.destination);
        output.append(index + 1 == fields.size() ? "\n" : ",\n");
    }
    output.append(indentation, ' ');
    output.push_back('}');
}

auto ReadFields(
    const JsonObject &object,
    const ConfigurationFields &fields,
    std::string_view parent) -> void;

struct FieldReader {
    FieldReader(
        const IJsonValue &value,
        const std::string_view member) noexcept
        : value(value), member(member) {}

    template <typename String>
    auto operator()(const WideTextDestination<String> &destination) const
        -> void {
        destination.value.get() =
            CopyWide<String>(ReadString(value.get(), member));
    }

    template <typename String>
    auto operator()(const Utf8TextDestination<String> &destination) const
        -> void {
        destination.value.get() =
            ToUtf8<String>(ReadString(value.get(), member));
    }

    auto operator()(
        const OptionalWideStringDestination &destination) const -> void {
        destination.value.get() = CopyWide<std::wstring>(
            ReadString(value.get(), member));
    }

    auto operator()(
        const OptionalUtf8StringDestination &destination) const -> void {
        destination.value.get() = ToUtf8<std::u8string>(
            ReadString(value.get(), member));
    }

    auto operator()(
        const OptionalBooleanDestination &destination) const -> void {
        const auto &json_value = value.get();
        if (json_value.ValueType() != JsonValueType::Boolean) {
            ConfigurationError(member, "must be a boolean");
        }
        destination.value.get() = json_value.GetBoolean();
    }

    auto operator()(const PortDestination &destination) const -> void {
        const auto &json_value = value.get();
        if (json_value.ValueType() != JsonValueType::Number) {
            ConfigurationError(member,
                "must be an integer from 0 through 65535");
        }
        const auto number = json_value.GetNumber();
        constexpr auto kMaximumPort =
            std::numeric_limits<std::uint16_t>::max();
        if (!std::isfinite(number) ||
            (std::floor(number) != number) ||
            (number < 0) || (number > kMaximumPort)) {
            ConfigurationError(member,
                "must be an integer from 0 through 65535");
        }
        [[gsl::suppress("type.1",
            justification: "The preceding range and integrality checks prove that the JSON number fits the port field.")]]
        const auto port = static_cast<std::uint16_t>(number);
        destination.value.get() = port;
    }

    auto operator()(
        const SerializedJsonObjectDestination &destination) const -> void {
        const auto &json_value = value.get();
        if (json_value.ValueType() != JsonValueType::Object) {
            ConfigurationError(member, "must be an object");
        }
        destination.value.get() = ToUtf8<SecureUtf8String>(
            json_value.GetObject().Stringify());
    }

    auto operator()(
        const NonemptyWideStringArrayDestination &destination) const -> void {
        const auto &json_value = value.get();
        if (json_value.ValueType() != JsonValueType::Array) {
            ConfigurationError(member, "must be an array");
        }
        const auto array = json_value.GetArray();
        if (array.Size() == 0) {
            ConfigurationError(member, "must not be empty");
        }
        for (const auto &element : array) {
            const auto text = ReadString(element, member);
            if (text.empty()) {
                ConfigurationError(member,
                    "must not contain an empty string");
            }
            destination.value.get().emplace_back(
                text.begin(), text.end());
        }
    }

    auto operator()(const ConfigurationFields &fields) const -> void {
        const auto &json_value = value.get();
        if (json_value.ValueType() != JsonValueType::Object) {
            ConfigurationError(member, "must be an object");
        }
        ReadFields(json_value.GetObject(), fields, member);
    }

    std::reference_wrapper<const IJsonValue> value;
    std::string_view member;
};

auto ReadFields(
    const JsonObject &object,
    const ConfigurationFields &fields,
    const std::string_view parent) -> void {
    const auto member_path = [parent](const std::string_view name) {
        return parent.empty()
            ? std::string{name}
            : std::format("{}.{}", parent, name);
    };
    for (const auto &entry : object) {
        const auto name = winrt::to_string(entry.Key());
        if (!std::ranges::contains(
                fields, std::string_view{name},
                &ConfigurationField::name)) {
            const auto member = member_path(name);
            ConfigurationError(member, "is not recognized");
        }
    }
    for (const auto &field : fields) {
        const auto member = member_path(field.name);
        const auto name = winrt::to_hstring(field.name);
        if (!object.HasKey(name)) {
            if (std::visit(DefaultFieldReader{}, field.destination)) {
                continue;
            }
            ConfigurationError(member, "is required");
        }
        const auto value = object.GetNamedValue(name);
        if ((value.ValueType() == JsonValueType::Null) &&
            std::visit(DefaultFieldReader{}, field.destination)) {
            continue;
        }
        std::visit(FieldReader{value, member}, field.destination);
    }
}

[[nodiscard]] auto ReadBackupConfigurationImpl(
    const std::filesystem::path &path) {
    const auto apartment = WinrtApartment{
        "could not initialize the Windows Runtime while reading "
        "the backup configuration"};
    auto file = std::ifstream(path, std::ios::binary);
    if (!file.is_open()) {
        throw std::runtime_error(
            "could not open the backup configuration");
    }
    const auto source = wil::secure_string(
        std::istreambuf_iterator<char>{file}, {});
    if (file.bad()) {
        throw std::runtime_error(
            "could not read the backup configuration");
    }
    const auto document = winrt::to_hstring(
        std::string_view{source.data(), source.size()});
    const auto root = JsonObject::Parse(document);
    auto result = BackupConfiguration{};
    const auto fields = ConfigurationDescription(result);
    ReadFields(root, fields, {});
    if (!result.wsl_client_path.starts_with(u8'/')) {
        ConfigurationError(
            "wsl.client_path", "must be an absolute Linux path");
    }
    return result;
}

} // namespace

export [[nodiscard]] auto GenerateConfigurationTemplate() {
    auto configuration = BackupConfiguration{};
    const auto fields = ConfigurationDescription(configuration);
    auto result = std::string{};
    WriteTemplateFields(result, fields, 0);
    result.push_back('\n');
    return result;
}

export [[nodiscard]] auto ReadBackupConfiguration(
    const std::filesystem::path &path) {
    try {
        return ReadBackupConfigurationImpl(path);
    } catch (const winrt::hresult_error &) {
        throw std::runtime_error(
            "could not parse the backup configuration");
    }
}
