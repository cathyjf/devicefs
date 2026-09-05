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

#include <devicefs/strsafe_compat.h>

export module devicefs.supervisor.configuration;

import std;
import <devicefs/windows_imports.h>;
import <winrt/Windows.Data.Json.h>;
import <winrt/Windows.Foundation.Collections.h>;
import devicefs.supervisor.winrt_apartment;

// The Windows GetObject macro conflicts with C++/WinRT IJsonValue::GetObject.
#undef GetObject

export using SecureUtf8String = std::basic_string<
    char8_t, std::char_traits<char8_t>, wil::secure_allocator<char8_t>>;

export struct WslConfiguration {
    std::string distribution;
    std::optional<std::string> linux_user;
    std::u8string client_path;
    std::string rpc_helper_path;
    std::string samba_dcerpcd_path;
};

export struct BackupConfiguration {
    std::string windows_username;
    WslConfiguration wsl;
    std::u8string pbs_server;
    std::uint16_t pbs_port = 0;
    std::u8string pbs_datastore;
    std::u8string pbs_auth_id;
    std::u8string pbs_namespace;
    bool pbs_parallelize_image_upload = false;
    std::u8string pbs_fingerprint;
    SecureUtf8String pbs_authentication_secret;
    SecureUtf8String pbs_encryption_key;
    std::vector<std::string> volumes;
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
[[nodiscard]] auto ToUtf8(
    const winrt::hstring &value,
    const std::string_view member) {
    const auto size = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.c_str(), -1,
        nullptr, 0, nullptr, nullptr);
    if (size == 0) {
        throw std::runtime_error(std::format(
            "could not convert configuration member '{}' to UTF-8", member));
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
        throw std::runtime_error(std::format(
            "could not convert configuration member '{}' to UTF-8", member));
    }
    result.pop_back();
    return result;
}

template <typename Value>
struct MemberDestination {
    constexpr explicit MemberDestination(
        Value BackupConfiguration::*member) noexcept : member(member) {}

    [[nodiscard]] auto Get(
        BackupConfiguration &configuration) const noexcept -> Value & {
        return configuration.*member;
    }

    Value BackupConfiguration::*member;
};

template <typename String>
struct Utf8TextDestination : MemberDestination<String> {
    using MemberDestination<String>::MemberDestination;

    [[nodiscard]] static constexpr auto TemplateValue() noexcept {
        return std::string_view{"\"\""};
    }
};

template <typename String>
Utf8TextDestination(String BackupConfiguration::*)
    -> Utf8TextDestination<String>;

struct WindowsUsernameDestination : MemberDestination<std::string> {
    using MemberDestination<std::string>::MemberDestination;

    [[nodiscard]] static constexpr auto DefaultValue() noexcept {
        return std::string_view{"devicefs-backup-user"};
    }
};

struct OptionalStringDestination :
    MemberDestination<std::optional<std::string>> {
    using MemberDestination<
        std::optional<std::string>>::MemberDestination;

    [[nodiscard]] static constexpr auto TemplateValue() noexcept {
        return std::string_view{"null"};
    }
};

struct OptionalUtf8StringDestination :
    MemberDestination<std::u8string> {
    using MemberDestination<std::u8string>::MemberDestination;

    [[nodiscard]] static constexpr auto TemplateValue() noexcept {
        return std::string_view{"\"\""};
    }
};

struct OptionalBooleanDestination : MemberDestination<bool> {
    using MemberDestination<bool>::MemberDestination;

    [[nodiscard]] static constexpr auto DefaultValue() noexcept {
        return true;
    }

    [[nodiscard]] static constexpr auto TemplateValue() noexcept {
        return std::string_view{DefaultValue() ? "true" : "false"};
    }
};

struct PortDestination : MemberDestination<std::uint16_t> {
    constexpr PortDestination(
        std::uint16_t BackupConfiguration::*member,
        const std::uint16_t default_value) noexcept
        : MemberDestination(member), default_value(default_value) {}

    std::uint16_t default_value;
};

struct SerializedJsonObjectDestination :
    MemberDestination<SecureUtf8String> {
    using MemberDestination<SecureUtf8String>::MemberDestination;

    [[nodiscard]] static constexpr auto TemplateValue() noexcept {
        return std::string_view{"{}"};
    }
};

struct NonemptyStringArrayDestination :
    MemberDestination<std::vector<std::string>> {
    using MemberDestination<
        std::vector<std::string>>::MemberDestination;

    [[nodiscard]] static constexpr auto DefaultValue() noexcept {
        return std::array{std::string_view{"C:"}};
    }
};

struct WslConfigurationDestination :
    MemberDestination<WslConfiguration> {
    using MemberDestination<WslConfiguration>::MemberDestination;
};

struct WslDistributionDestination {
    [[nodiscard]] static constexpr auto DefaultValue() noexcept {
        return std::string_view{"Debian"};
    }
};

struct WslLinuxUserDestination {
    [[nodiscard]] static constexpr auto TemplateValue() noexcept {
        return std::string_view{"null"};
    }
};

struct WslClientPathDestination {
    [[nodiscard]] static constexpr auto DefaultValue() noexcept {
        return std::string_view{"proxmox-backup-client"};
    }
};

struct WslRpcHelperPathDestination {
    [[nodiscard]] static constexpr auto DefaultValue() noexcept {
        return std::string_view{"rpcd_devicefs"};
    }
};

struct WslSambaDcerpcdPathDestination {
    [[nodiscard]] static constexpr auto DefaultValue() noexcept {
        return std::string_view{"/usr/libexec/samba/samba-dcerpcd"};
    }
};

using WslFieldDestination = std::variant<
    WslDistributionDestination,
    WslLinuxUserDestination,
    WslClientPathDestination,
    WslRpcHelperPathDestination,
    WslSambaDcerpcdPathDestination>;

struct WslConfigurationField {
    constexpr WslConfigurationField(
        const std::string_view name,
        WslFieldDestination destination,
        const bool materialize_in_template = true) noexcept
        : name(name), destination(std::move(destination)),
          materialize_in_template(materialize_in_template) {}

    std::string_view name;
    WslFieldDestination destination;
    bool materialize_in_template;
};

[[nodiscard]] constexpr auto WslConfigurationDescription() {
    return std::to_array<WslConfigurationField>({
        {"distribution", WslDistributionDestination{}},
        {"linux_user", WslLinuxUserDestination{}, false},
        {"client_path", WslClientPathDestination{}, false},
        {"rpc_helper_path", WslRpcHelperPathDestination{}, false},
        {"samba_dcerpcd_path", WslSambaDcerpcdPathDestination{}, false},
    });
}

struct ConfigurationField;
using ConfigurationFields = std::vector<ConfigurationField>;

using FieldDestination = std::variant<
    Utf8TextDestination<std::string>,
    Utf8TextDestination<std::u8string>,
    Utf8TextDestination<SecureUtf8String>,
    WindowsUsernameDestination,
    OptionalStringDestination,
    OptionalUtf8StringDestination,
    OptionalBooleanDestination,
    PortDestination,
    SerializedJsonObjectDestination,
    NonemptyStringArrayDestination,
    WslConfigurationDestination,
    ConfigurationFields>;

struct ConfigurationField {
    constexpr ConfigurationField(
        const std::string_view name,
        FieldDestination destination) noexcept
        : name(name), destination(std::move(destination)) {}
    constexpr ConfigurationField(
        const ConfigurationField &) = default;
    constexpr ConfigurationField(
        ConfigurationField &&) = default;
    constexpr auto operator=(const ConfigurationField &)
        -> ConfigurationField & = default;
    constexpr auto operator=(ConfigurationField &&)
        -> ConfigurationField & = default;
    constexpr ~ConfigurationField();

    std::string_view name;
    FieldDestination destination;
};

// Delay the destructor definition until the recursive ConfigurationFields
// alternative has a complete element type.
constexpr ConfigurationField::~ConfigurationField() = default;

[[nodiscard]] constexpr auto ConfigurationDescription() {
    return ConfigurationFields{
        {"volumes",
            NonemptyStringArrayDestination{
                &BackupConfiguration::volumes}},
        {"pbs", ConfigurationFields{
            {"server", Utf8TextDestination{
                &BackupConfiguration::pbs_server}},
            {"port", PortDestination{
                &BackupConfiguration::pbs_port, 8007}},
            {"datastore",
                Utf8TextDestination{
                    &BackupConfiguration::pbs_datastore}},
            {"auth_id", Utf8TextDestination{
                &BackupConfiguration::pbs_auth_id}},
            {"namespace",
                OptionalUtf8StringDestination{
                    &BackupConfiguration::pbs_namespace}},
            {"parallelize_image_upload", OptionalBooleanDestination{
                &BackupConfiguration::pbs_parallelize_image_upload}},
            {"fingerprint",
                Utf8TextDestination{
                    &BackupConfiguration::pbs_fingerprint}},
            {"authentication_secret", Utf8TextDestination{
                &BackupConfiguration::pbs_authentication_secret}},
            {"encryption_key", SerializedJsonObjectDestination{
                &BackupConfiguration::pbs_encryption_key}},
        }},
        {"internal_windows_account", ConfigurationFields{
            {"username",
                WindowsUsernameDestination{
                    &BackupConfiguration::windows_username}},
            {"wsl", WslConfigurationDestination{
                &BackupConfiguration::wsl}},
        }},
    };
}

template <typename Fields>
constexpr auto WriteTemplateFields(
    std::string &output,
    const Fields &fields,
    std::size_t indentation) -> void;

template <typename Destination>
concept FixedTemplateValueDestination = requires {
    Destination::TemplateValue();
};

template <typename Destination>
concept DefaultTextDestination = requires {
    { Destination::DefaultValue() } -> std::convertible_to<std::string_view>;
};

struct TemplateValueWriter {
    constexpr TemplateValueWriter(
        std::string &output,
        const std::size_t indentation) noexcept
        : output(output), indentation(indentation) {}

    template <FixedTemplateValueDestination Destination>
    constexpr auto operator()(const Destination &) const -> void {
        output.get().append(Destination::TemplateValue());
    }

    constexpr auto operator()(
        const PortDestination &destination) const -> void {
        constexpr auto kMaximumPortTextLength =
            std::numeric_limits<std::uint16_t>::digits10 + 1;
        auto buffer = std::array<char, kMaximumPortTextLength>{};
        const auto converted = std::to_chars(
            buffer.data(), buffer.data() + buffer.size(),
            destination.default_value);
        if (converted.ec != std::errc{}) {
            std::unreachable();
        }
        output.get().append(buffer.data(), converted.ptr);
    }

    template <DefaultTextDestination Destination>
    constexpr auto operator()(const Destination &) const -> void {
        output.get().push_back('"');
        output.get().append(Destination::DefaultValue());
        output.get().push_back('"');
    }

    constexpr auto operator()(const ConfigurationFields &fields) const
        -> void {
        WriteTemplateFields(output.get(), fields, indentation);
    }

    constexpr auto operator()(const WslConfigurationDestination &) const
        -> void {
        WriteTemplateFields(output.get(),
            WslConfigurationDescription(), indentation);
    }

    constexpr auto operator()(
        const NonemptyStringArrayDestination &) const -> void {
        output.get().push_back('[');
        auto separator = std::string_view{};
        for (const auto value : NonemptyStringArrayDestination::DefaultValue()) {
            output.get().append(separator);
            output.get().push_back('"');
            output.get().append(value);
            output.get().push_back('"');
            separator = ", ";
        }
        output.get().push_back(']');
    }

    std::reference_wrapper<std::string> output;
    std::size_t indentation;
};

struct DefaultFieldReader {
    explicit DefaultFieldReader(
        BackupConfiguration &configuration) noexcept
        : configuration(configuration) {}

    template <typename Destination>
    auto operator()(const Destination &) const noexcept {
        return false;
    }

    auto operator()(const WindowsUsernameDestination &destination) const {
        destination.Get(configuration.get()) =
            WindowsUsernameDestination::DefaultValue();
        return true;
    }

    auto operator()(
        const OptionalStringDestination &destination) const noexcept {
        destination.Get(configuration.get()).reset();
        return true;
    }

    auto operator()(
        const OptionalUtf8StringDestination &destination) const noexcept {
        destination.Get(configuration.get()).clear();
        return true;
    }

    auto operator()(
        const OptionalBooleanDestination &destination) const noexcept {
        destination.Get(configuration.get()) =
            OptionalBooleanDestination::DefaultValue();
        return true;
    }

    auto operator()(const PortDestination &destination) const noexcept {
        destination.Get(configuration.get()) = destination.default_value;
        return true;
    }

    auto operator()(const NonemptyStringArrayDestination &destination) const {
        constexpr auto defaults = NonemptyStringArrayDestination::DefaultValue();
        destination.Get(configuration.get()).assign(
            defaults.begin(), defaults.end());
        return true;
    }

    std::reference_wrapper<BackupConfiguration> configuration;
};

template <typename Fields>
constexpr auto WriteTemplateFields(
    std::string &output,
    const Fields &fields,
    const std::size_t indentation) -> void {
    output.push_back('{');
    auto separator = std::string_view{"\n"};
    for (const auto &field : fields) {
        if constexpr (std::same_as<
                typename Fields::value_type, WslConfigurationField>) {
            if (!field.materialize_in_template) {
                continue;
            }
        }
        output.append(separator);
        output.append(indentation + 2, ' ');
        output.push_back('"');
        output.append(field.name);
        output.append("\": ");
        std::visit(
            TemplateValueWriter{output, indentation + 2}, field.destination);
        separator = ",\n";
    }
    output.push_back('\n');
    output.append(indentation, ' ');
    output.push_back('}');
}

template <typename Configuration, typename Fields>
auto ReadFields(
    Configuration &configuration,
    const JsonObject &object,
    const Fields &fields,
    std::string_view parent) -> void;

struct FieldReader {
    FieldReader(
        BackupConfiguration &configuration,
        const IJsonValue &value,
        const std::string_view member) noexcept
        : configuration(configuration), value(value), member(member) {}

    template <typename String>
    auto operator()(const Utf8TextDestination<String> &destination) const
        -> void {
        destination.Get(configuration.get()) =
            ToUtf8<String>(ReadString(value.get(), member), member);
    }

    auto operator()(const WindowsUsernameDestination &destination) const
        -> void {
        destination.Get(configuration.get()) = ToUtf8<std::string>(
            ReadString(value.get(), member), member);
    }

    auto operator()(
        const OptionalStringDestination &destination) const -> void {
        destination.Get(configuration.get()) = ToUtf8<std::string>(
            ReadString(value.get(), member), member);
    }

    auto operator()(
        const OptionalUtf8StringDestination &destination) const -> void {
        destination.Get(configuration.get()) = ToUtf8<std::u8string>(
            ReadString(value.get(), member), member);
    }

    auto operator()(
        const OptionalBooleanDestination &destination) const -> void {
        const auto &json_value = value.get();
        if (json_value.ValueType() != JsonValueType::Boolean) {
            ConfigurationError(member, "must be a boolean");
        }
        destination.Get(configuration.get()) = json_value.GetBoolean();
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
        destination.Get(configuration.get()) = port;
    }

    auto operator()(
        const SerializedJsonObjectDestination &destination) const -> void {
        const auto &json_value = value.get();
        if (json_value.ValueType() != JsonValueType::Object) {
            ConfigurationError(member, "must be an object");
        }
        destination.Get(configuration.get()) = ToUtf8<SecureUtf8String>(
            json_value.GetObject().Stringify(), member);
    }

    auto operator()(
        const NonemptyStringArrayDestination &destination) const -> void {
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
            destination.Get(configuration.get()).emplace_back(
                ToUtf8<std::string>(text, member));
        }
    }

    auto operator()(const ConfigurationFields &fields) const -> void {
        const auto &json_value = value.get();
        if (json_value.ValueType() != JsonValueType::Object) {
            ConfigurationError(member, "must be an object");
        }
        ReadFields(
            configuration.get(), json_value.GetObject(), fields, member);
    }

    auto operator()(const WslConfigurationDestination &destination) const
        -> void {
        const auto &json_value = value.get();
        if (json_value.ValueType() != JsonValueType::Object) {
            ConfigurationError(member, "must be an object");
        }
        ReadFields(destination.Get(configuration.get()), json_value.GetObject(),
            WslConfigurationDescription(), member);
    }

    std::reference_wrapper<BackupConfiguration> configuration;
    std::reference_wrapper<const IJsonValue> value;
    std::string_view member;
};

struct WslDefaultFieldReader {
    explicit WslDefaultFieldReader(
        WslConfiguration &configuration) noexcept
        : configuration(configuration) {}

    template <typename Destination>
    auto operator()(const Destination &) const noexcept {
        return false;
    }

    auto operator()(const WslDistributionDestination &) const {
        configuration.get().distribution =
            WslDistributionDestination::DefaultValue();
        return true;
    }

    auto operator()(const WslLinuxUserDestination &) const noexcept {
        configuration.get().linux_user.reset();
        return true;
    }

    auto operator()(const WslClientPathDestination &) const {
        configuration.get().client_path = std::u8string{
            WslClientPathDestination::DefaultValue().begin(),
            WslClientPathDestination::DefaultValue().end()};
        return true;
    }

    auto operator()(const WslRpcHelperPathDestination &) const {
        configuration.get().rpc_helper_path = std::string{
            WslRpcHelperPathDestination::DefaultValue().begin(),
            WslRpcHelperPathDestination::DefaultValue().end()};
        return true;
    }

    auto operator()(const WslSambaDcerpcdPathDestination &) const {
        configuration.get().samba_dcerpcd_path = std::string{
            WslSambaDcerpcdPathDestination::DefaultValue().begin(),
            WslSambaDcerpcdPathDestination::DefaultValue().end()};
        return true;
    }

    std::reference_wrapper<WslConfiguration> configuration;
};

struct WslFieldReader {
    WslFieldReader(
        WslConfiguration &configuration,
        const IJsonValue &value,
        const std::string_view member) noexcept
        : configuration(configuration), value(value), member(member) {}

    auto operator()(const WslDistributionDestination &) const -> void {
        configuration.get().distribution = ToUtf8<std::string>(
            ReadString(value.get(), member), member);
    }

    auto operator()(const WslLinuxUserDestination &) const -> void {
        configuration.get().linux_user = ToUtf8<std::string>(
            ReadString(value.get(), member), member);
    }

    auto operator()(const WslClientPathDestination &) const -> void {
        configuration.get().client_path = ToUtf8<std::u8string>(
            ReadString(value.get(), member), member);
    }

    auto operator()(const WslRpcHelperPathDestination &) const -> void {
        configuration.get().rpc_helper_path =
            ToUtf8<std::string>(ReadString(value.get(), member), member);
    }

    auto operator()(const WslSambaDcerpcdPathDestination &) const -> void {
        configuration.get().samba_dcerpcd_path =
            ToUtf8<std::string>(ReadString(value.get(), member), member);
    }

    std::reference_wrapper<WslConfiguration> configuration;
    std::reference_wrapper<const IJsonValue> value;
    std::string_view member;
};

template <typename Configuration, typename Fields>
auto ReadFields(
    Configuration &configuration,
    const JsonObject &object,
    const Fields &fields,
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
                [](const auto &field) { return field.name; })) {
            const auto member = member_path(name);
            ConfigurationError(member, "is not recognized");
        }
    }
    for (const auto &field : fields) {
        const auto member = member_path(field.name);
        const auto name = winrt::to_hstring(field.name);
        const auto read_default = [&] {
            if constexpr (std::same_as<
                    Configuration, BackupConfiguration>) {
                return std::visit(
                    DefaultFieldReader{configuration}, field.destination);
            } else {
                return std::visit(
                    WslDefaultFieldReader{configuration}, field.destination);
            }
        };
        if (!object.HasKey(name)) {
            if (read_default()) {
                continue;
            }
            ConfigurationError(member, "is required");
        }
        const auto value = object.GetNamedValue(name);
        if ((value.ValueType() == JsonValueType::Null) &&
            read_default()) {
            continue;
        }
        if constexpr (std::same_as<
                Configuration, BackupConfiguration>) {
            std::visit(
                FieldReader{configuration, value, member}, field.destination);
        } else {
            std::visit(WslFieldReader{configuration, value, member},
                field.destination);
        }
    }
}

[[nodiscard]] auto ReadBackupConfigurationImpl(
    const std::filesystem::path &path) {
    const auto apartment = WinrtApartment{
        "could not initialize the Windows Runtime while reading "
        "the backup configuration"};
    auto file = std::ifstream(path, std::ios::binary);
    if (!file.is_open()) {
        throw std::runtime_error(std::format(
            "could not open the backup configuration '{}'", path.string()));
    }
    const auto source = wil::secure_string(
        std::istreambuf_iterator<char>{file}, {});
    if (file.bad()) {
        throw std::runtime_error(std::format(
            "could not read the backup configuration '{}'", path.string()));
    }
    const auto document = winrt::to_hstring(
        std::string_view{source.data(), source.size()});
    const auto root = JsonObject::Parse(document);
    auto result = BackupConfiguration{};
    const auto fields = ConfigurationDescription();
    ReadFields(result, root, fields, {});
    return result;
}

[[nodiscard]] consteval auto GenerateConfigurationTemplateText() {
    const auto fields = ConfigurationDescription();
    auto result = std::string{};
    WriteTemplateFields(result, fields, 0);
    result.push_back('\n');
    return result;
}

} // namespace

export [[nodiscard]] consteval auto GenerateConfigurationTemplate() {
    constexpr auto size = GenerateConfigurationTemplateText().size();
    const auto text = GenerateConfigurationTemplateText();
    auto result = std::array<char, size>{};
    std::ranges::copy(text, result.begin());
    return result;
}

export [[nodiscard]] auto ReadBackupConfiguration(
    const std::filesystem::path &path) {
    try {
        return ReadBackupConfigurationImpl(path);
    } catch (const winrt::hresult_error &error) {
        throw std::runtime_error(std::format(
            "could not parse the backup configuration '{}': {}",
            path.string(), winrt::to_string(error.message())));
    }
}
