// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <devicefs/strsafe_compat.h>

import std;
import <devicefs/windows_imports.h>;
import <devicefs/common.h>;
import devicefs.supervisor.oci_verification;

namespace {

// Public signatures from the signed 20260928.4 release, independently verified
// with GnuPG against the owner's exported key. No private key is needed here.
constexpr auto kAmd64Digest = "sha256:9c3cc95411275846157e8f9f5f62659234cc53ec8f63a797cd6320df1652a6a3";
constexpr auto kAmd64Signature =
    "iNQEABMKADkWIQTtxzY/WVxY0vB5MP62mn2VaDxuKgUCartfJBsUgAAAAAAEAA5tYW51MiwyLjUr"
    "MS4xMiwwLDMACgkQtpp9lWg8biox1gII2rVM4lQLc2LBnZcyrLbbpyZc2e2ZGnq2ySp2D8tCbRct"
    "Uof9Ttweu5bIuI1RrzoHRYr7QKlXx61JB9D82LtEONECCQG5wPmx7JyIOY5A7CF6cLbpNou83GxW"
    "tAaBzYMuA65iQRxVUxmymLNKHT0ZaAqkVbHyTgtKOpbC8qczL6gPwUj7xQ==";

constexpr auto kArm64Digest = "sha256:29aa621f63898f1ab67c2a17781152d8a0b6491b39d0064589f8ff4dad0af0e0";
constexpr auto kArm64Signature =
    "iNQEABMKADkWIQTtxzY/WVxY0vB5MP62mn2VaDxuKgUCartfNRsUgAAAAAAEAA5tYW51MiwyLjUr"
    "MS4xMiwwLDMACgkQtpp9lWg8bioS2QIJAcXU1J041IhYZyErCXANQjAfCbUYd9sD5S3Yu2juo5w0"
    "xMPflWLcnoE/Sy8ua3q73NMMj7MpJCOu14sbTLgn2fS4AgdeOSW14XTpWo+KrHhjvV5FJHmmikqI"
    "ktp4Ge7h5JDFJ6YkXiopoWnSAWDyvKre67yjlPRDfBG/R11VYUO3Ph0omg==";

auto RequireFailure(const auto &operation) -> void {
    try {
        operation();
    } catch (const std::runtime_error &) {
        return;
    }
    throw std::runtime_error("invalid OCI input was accepted");
}

[[nodiscard]] auto Encode(const std::span<const BYTE> bytes) -> std::string {
    auto size = DWORD{};
    constexpr auto flags = CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF;
    if (!CryptBinaryToStringA(bytes.data(), wil::safe_cast_failfast<DWORD>(bytes.size()),
            flags, nullptr, &size)) {
        WinError("could not measure the Base64 encoding of the test signature");
    }
    auto result = std::string(size, '\0');
    if (!CryptBinaryToStringA(bytes.data(), wil::safe_cast_failfast<DWORD>(bytes.size()),
            flags, result.data(), &size)) {
        WinError("could not encode the test signature as Base64");
    }
    result.resize(size);
    return result;
}

} // namespace

// The argument is an existing scratch directory for the file-hashing tests.
[[gsl::suppress("26429", justification:
    "C++ [basic.start.main] guarantees that `argv` is not null.")]]
auto main(const int argc, const char *const *const argv) -> int try {
    if (argc != 2) {
        throw std::runtime_error("supply a scratch directory for OCI verification tests");
    }
    VerifyOciLayerSignature(kAmd64Digest, kAmd64Signature);
    VerifyOciLayerSignature(kArm64Digest, kArm64Signature);
    RequireFailure([] { VerifyOciLayerSignature(kAmd64Digest, kArm64Signature); });
    RequireFailure([] { VerifyOciLayerSignature(kAmd64Digest, ""); });
    // A zero-length view over valid Base64 must not be decoded as a NUL-
    // terminated string by the Windows API's special zero-length convention.
    RequireFailure([] {
        VerifyOciLayerSignature(kAmd64Digest, std::string_view{kAmd64Signature, 0});
    });
    RequireFailure([] { VerifyOciLayerSignature(kAmd64Digest, "!not-base64!"); });
    RequireFailure([] { VerifyOciLayerSignature("sha256:00", kAmd64Signature); });
    RequireFailure([] { VerifyOciLayerSignature(std::string{kAmd64Digest} + "\n", kAmd64Signature); });

    auto size = DWORD{};
    if (!CryptStringToBinaryA(kAmd64Signature, 0, CRYPT_STRING_BASE64,
            nullptr, &size, nullptr, nullptr)) {
        WinError("could not measure the decoded AMD64 signature fixture");
    }
    auto packet = std::vector<BYTE>(size);
    if (!CryptStringToBinaryA(kAmd64Signature, 0, CRYPT_STRING_BASE64,
            packet.data(), &size, nullptr, nullptr)) {
        WinError("could not decode the AMD64 signature fixture");
    }
    for (auto length = std::size_t{1}; length < packet.size(); ++length) {
        const auto encoded = Encode(std::span{packet}.first(length));
        RequireFailure([&] {
            VerifyOciLayerSignature(kAmd64Digest, encoded);
        });
    }
    // The fixture uses a legacy header with a one-byte body length. The outer
    // packet header is excluded from the signature hash, so the same signed
    // body also verifies with each other supported definite-length encoding.
    const auto body = std::span{packet}.subspan(2);
    for (auto header : {std::vector<BYTE>{0x89, 0, 212},
            std::vector<BYTE>{0x8a, 0, 0, 0, 212},
            std::vector<BYTE>{0xc2, 192, 20},
            std::vector<BYTE>{0xc2, 255, 0, 0, 0, 212}}) {
        header.append_range(body);
        VerifyOciLayerSignature(kAmd64Digest, Encode(header));
    }
    for (const auto offset : {0uz, 1uz, 2uz, 3uz, 4uz, 5uz, 6uz, 7uz, 10uz, 213uz}) {
        auto altered = packet;
        altered[offset] ^= 0x40;
        const auto encoded = Encode(altered);
        RequireFailure([&] { VerifyOciLayerSignature(kAmd64Digest, encoded); });
    }
    // The legacy indeterminate-length header and the new partial-length header
    // cannot describe the single, explicitly delimited signature we accept.
    for (const auto header : {std::to_array<BYTE>({0x8b, 0}),
            std::to_array<BYTE>({0xc2, 224})}) {
        auto altered = packet;
        std::ranges::copy(header, altered.begin());
        const auto encoded = Encode(altered);
        RequireFailure([&] { VerifyOciLayerSignature(kAmd64Digest, encoded); });
    }
    // In this fixture the first MPI starts at byte 79, after both subpacket
    // areas and the hash prefix. A zero bit count leaves no first byte to
    // inspect; a count above 521 exceeds the P-521 scalar range.
    for (const auto bits : {0u, 522u}) {
        auto altered = packet;
        altered[79] = wil::safe_cast_failfast<BYTE>(bits >> 8);
        altered[80] = wil::safe_cast_failfast<BYTE>(bits & 0xff);
        const auto encoded = Encode(altered);
        RequireFailure([&] { VerifyOciLayerSignature(kAmd64Digest, encoded); });
    }
    packet.push_back(0);
    const auto encoded = Encode(packet);
    RequireFailure([&] { VerifyOciLayerSignature(kAmd64Digest, encoded); });

    const auto file = std::filesystem::path{argv[1]} / "oci-verification-layer-test";
    const auto cleanup = wil::scope_exit([&file] {
        auto error = std::error_code{};
        std::filesystem::remove(file, error);
    });
    const auto write = [&file](const std::string_view content) {
        auto output = std::ofstream{file, std::ios::binary};
        output << content;
        output.close();
        if (!output) {
            throw std::runtime_error("could not write the test layer");
        }
    };
    write("");
    VerifyOciLayerFile(file, "sha256:e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    write("abc");
    constexpr auto abc_digest = "sha256:ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    VerifyOciLayerFile(file, abc_digest);
    write("abd");
    RequireFailure([&] { VerifyOciLayerFile(file, abc_digest); });
    write("ab");
    RequireFailure([&] { VerifyOciLayerFile(file, abc_digest); });
    write(std::string(128 * 1024 + 17, 'X'));
    VerifyOciLayerFile(file, "sha256:7e7ec2f56ff62147f68ebf5a1739ba6229e72ebca4824500acb364a087a3ca15");
    std::println("OCI signature, malformed-packet, and streamed layer-hash checks passed.");
    return 0;
} catch (const std::exception &error) {
    std::println("OCI verification test failed: {}", error.what());
    return 1;
}
