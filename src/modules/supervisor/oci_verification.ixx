// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

module;

#include <devicefs/strsafe_compat.h>

export module devicefs.supervisor.oci_verification;

import std;
import <devicefs/windows_imports.h>;
import <devicefs/common.h>;
import devicefs.supervisor.math;

namespace {

// Each P-521 signature scalar occupies ceil(521 / 8) bytes in CNG's encoding.
constexpr auto p521_scalar_size = devicefs::math::Ceil<521, 8>();

[[noreturn]] auto InvalidSignature(const std::string_view digest,
    const std::string_view reason) -> void {
    throw std::runtime_error(std::format(
        "could not verify the signature for OCI layer '{}': {}", digest, reason));
}

auto RequireSha256Digest(const std::string_view digest) -> void {
    // The signed message is the OCI spelling: "sha256:" followed by 64
    // lowercase hexadecimal digits (71 ASCII bytes), without a final newline.
    // Requiring that spelling makes the signed text and file-hash comparison
    // agree. OCI specifies the SHA-256 encoding here:
    // https://github.com/opencontainers/image-spec/blob/v1.1.1/descriptor.md#sha-256
    if ((digest.size() != 71) || !digest.starts_with("sha256:") ||
        !std::ranges::all_of(digest.substr(7), [](const char value) {
            return ((value >= '0') && (value <= '9')) ||
                ((value >= 'a') && (value <= 'f'));
        })) {
        throw std::runtime_error(std::format(
            "invalid SHA-256 OCI layer digest '{}'", digest));
    }
}

// Packet lengths come from the registry's untrusted signature. Each reader is
// confined to its enclosing packet or field, so an inner length cannot consume
// bytes outside that enclosure. This implements the packet-boundary rule in
// https://www.rfc-editor.org/rfc/rfc9580.html#section-4.1
class SignatureReader {
public:
    SignatureReader(const std::span<const UCHAR> data, const std::string_view digest) noexcept
        : remaining_{data}, digest_{digest} {}

    [[nodiscard]] auto Read(const std::size_t count) -> std::span<const UCHAR> {
        if (count > remaining_.size()) {
            InvalidSignature(digest_, "truncated OpenPGP signature");
        }
        const auto result = remaining_.first(count);
        remaining_ = remaining_.subspan(count);
        return result;
    }

    [[nodiscard]] auto Integer(const std::size_t count) -> std::uint32_t {
        auto result = std::uint32_t{};
        for (const auto value : Read(count)) {
            result = (result << 8) | value;
        }
        return result;
    }

    auto RequireEnd() const -> void {
        if (!remaining_.empty()) {
            InvalidSignature(digest_, "unexpected data after the OpenPGP signature");
        }
    }

    auto ReadScalar(const std::span<UCHAR, p521_scalar_size> destination) -> void {
        // An OpenPGP multiprecision integer starts with a two-byte bit count,
        // followed by the minimum number of big-endian bytes. The count excludes
        // leading zero bits; checking the highest occupied bit rejects a count
        // inconsistent with the encoded value. See RFC 4880 section 3.2:
        // https://www.rfc-editor.org/rfc/rfc4880.html#section-3.2
        //
        // P-521's r and s must be nonzero and fit in 521 bits. CNG will verify
        // their mathematical validity; here they must fit in its 66-byte slots
        // (ceil(521 / 8)). The caller zeroes those slots so right-aligning each
        // integer supplies the fixed-width, big-endian representation.
        const auto bits = Integer(2);
        if ((bits == 0) || (bits > 521)) {
            InvalidSignature(digest_, "invalid P-521 signature scalar size");
        }
        const auto value = Read((bits + 7) / 8);
        if (((value.size() - 1) * 8 + std::bit_width(value.front())) != bits) {
            InvalidSignature(digest_, "invalid OpenPGP integer encoding");
        }
        std::ranges::copy(value, destination.last(value.size()).begin());
    }

private:
    std::span<const UCHAR> remaining_;
    std::string_view digest_;
};

[[nodiscard]] auto DecodeSignature(const std::string_view signature,
    const std::string_view digest) -> std::vector<UCHAR> {
    // `CryptStringToBinaryA` interprets a zero `cchString` as a request to scan
    // for a terminating NUL. An empty `std::string_view` may have a null data
    // pointer or point to nonempty text, so that convention would ignore the
    // view's bounds.
    // https://learn.microsoft.com/en-us/windows/win32/api/wincrypt/nf-wincrypt-cryptstringtobinarya
    if (signature.empty()) {
        InvalidSignature(digest, "missing Base64 signature");
    }
    const auto length = wil::safe_cast_failfast<DWORD>(signature.size());
    auto bytes = DWORD{};
    constexpr auto flags = CRYPT_STRING_BASE64 | CRYPT_STRING_STRICT;
    if (!CryptStringToBinaryA(signature.data(), length, flags,
            nullptr, &bytes, nullptr, nullptr)) {
        WinError("could not measure the decoded signature for OCI layer '{}'", digest);
    }
    auto result = std::vector<UCHAR>(bytes);
    if (!CryptStringToBinaryA(signature.data(), length, flags,
            result.data(), &bytes, nullptr, nullptr)) {
        WinError("could not decode the Base64 signature for OCI layer '{}'", digest);
    }
    result.resize(bytes);
    return result;
}

[[nodiscard]] auto CreateHash(const BCRYPT_ALG_HANDLE algorithm,
    const std::string_view digest) -> wil::unique_bcrypt_hash {
    auto hash = wil::unique_bcrypt_hash{};
    if (const auto status = BCryptCreateHash(algorithm, hash.addressof(),
            nullptr, 0, nullptr, 0, 0); !BCRYPT_SUCCESS(status)) {
        WinError("could not create a hash to verify OCI layer '{}'",
            digest, ExplicitHresult{HRESULT_FROM_NT(status)});
    }
    return hash;
}

auto HashData(const BCRYPT_HASH_HANDLE hash, const std::span<const UCHAR> data,
    const std::string_view digest) -> void {
    // https://learn.microsoft.com/en-us/windows/win32/api/bcrypt/nf-bcrypt-bcrypthashdata#parameters
    [[gsl::suppress("26492", justification:
        "The `pbInput` parameter to `BCryptHashData` is annotated with "
        "`_In_reads_bytes_(cbInput)`, and Microsoft explicitly states that "
        "the buffer is not modified. The `const_cast` accommodates the "
        "parameter's non-const pointer type.")]]
    auto *const input = const_cast<PUCHAR>(data.data());
    if (const auto status = BCryptHashData(hash, input,
            wil::safe_cast_failfast<ULONG>(data.size()), 0); !BCRYPT_SUCCESS(status)) {
        WinError("could not hash data to verify OCI layer '{}'",
            digest, ExplicitHresult{HRESULT_FROM_NT(status)});
    }
}

template <std::size_t Size>
[[nodiscard]] auto FinishHash(const BCRYPT_HASH_HANDLE hash,
    const std::string_view digest) -> std::array<UCHAR, Size> {
    auto result = std::array<UCHAR, Size>{};
    if (const auto status = BCryptFinishHash(hash, result.data(),
            CompileTimeCast<ULONG, result.size()>(), 0); !BCRYPT_SUCCESS(status)) {
        WinError("could not finish the hash to verify OCI layer '{}'",
            digest, ExplicitHresult{HRESULT_FROM_NT(status)});
    }
    return result;
}

[[nodiscard]] auto SigningKey() -> wil::unique_bcrypt_key {
    // The following initializer is generated by running the program named
    // `Export-OciSigningKey.ps1` found in the `misc` directory under the
    // repository root.
    constexpr auto coordinates = std::to_array<UCHAR>({
        // x
        0x00, 0x3d, 0x56, 0x68, 0x0b, 0xbd, 0x83, 0x36, 0x07, 0xeb, 0xe0,
        0x52, 0x81, 0x48, 0x71, 0x3d, 0xcd, 0xaf, 0x7f, 0x27, 0xdb, 0x42,
        0x21, 0xe8, 0xf3, 0x16, 0xa8, 0x2d, 0x8f, 0xa8, 0xcd, 0x28, 0x55,
        0xf3, 0x83, 0xfb, 0x44, 0xe5, 0x92, 0xa4, 0xcd, 0x50, 0x09, 0x82,
        0x8d, 0x09, 0xe4, 0xa9, 0x61, 0xc7, 0xf3, 0x8d, 0x5b, 0x25, 0x89,
        0x58, 0xfc, 0xb9, 0xde, 0xaa, 0x15, 0xd2, 0x9e, 0xe0, 0x79, 0xd3,
        // y
        0x00, 0x5b, 0xaa, 0x2c, 0x52, 0x54, 0x23, 0xa1, 0xbc, 0x6e, 0xd9,
        0x2d, 0xf8, 0xf9, 0x09, 0xdb, 0x29, 0xa5, 0x6b, 0xd0, 0x13, 0xc2,
        0xe7, 0xb2, 0x1d, 0xf0, 0xa6, 0x9c, 0x85, 0xba, 0x19, 0xa4, 0x09,
        0x8f, 0xfa, 0xad, 0x30, 0x3c, 0xd4, 0xac, 0x07, 0xb3, 0x55, 0x9e,
        0x0e, 0x14, 0x0d, 0xd0, 0xe6, 0x9c, 0x37, 0xc2, 0xe3, 0x1e, 0xdb,
        0xa7, 0x37, 0x35, 0x3e, 0x19, 0x1f, 0x48, 0xe8, 0x65, 0x09, 0x60,
    });
    struct PublicKey {
        BCRYPT_ECCKEY_BLOB header;
        decltype(coordinates) point;
    };
    constexpr auto public_key = PublicKey{
        .header = {
            .dwMagic = BCRYPT_ECDSA_PUBLIC_P521_MAGIC,
            .cbKey = CompileTimeCast<ULONG, coordinates.size() / 2>(),
        },
        .point = coordinates,
    };
    // CNG requires the header and coordinates without intervening or trailing
    // padding. The destination size makes `bit_cast` enforce that layout size.
    auto blob = std::bit_cast<std::array<UCHAR, sizeof(public_key.header) +
        sizeof(public_key.point)>>(public_key);
    auto key = wil::unique_bcrypt_key{};
    if (const auto status = BCryptImportKeyPair(BCRYPT_ECDSA_P521_ALG_HANDLE,
            nullptr, BCRYPT_ECCPUBLIC_BLOB, key.addressof(), blob.data(),
            CompileTimeCast<ULONG, blob.size()>(), 0); !BCRYPT_SUCCESS(status)) {
        WinError("could not import the pinned OCI signing key",
            ExplicitHresult{HRESULT_FROM_NT(status)});
    }
    return key;
}

} // namespace

// `push-image.fish` signs the full ASCII layer digest, including "sha256:" and
// no newline, and stores the binary detached signature as Base64. Its GnuPG
// invocation produces a v4 binary-document ECDSA/P-521/SHA-512 signature.
// CNG accepts the ECDSA integers and a completed hash, so this function decodes
// the OpenPGP packet and reconstructs the hash that GnuPG signed. Malformed
// input or failed verification produces an exception.
export auto VerifyOciLayerSignature(const std::string_view digest,
    const std::string_view signature) -> void {
    RequireSha256Digest(digest);
    const auto decoded = DecodeSignature(signature, digest);
    auto packet = SignatureReader{decoded, digest};
    const auto header = packet.Integer(1);
    const auto body_length = [&]() -> std::uint32_t {
        // Bit 7 marks an OpenPGP packet; bit 6 selects the new header format.
        // In that format the lower six bits are the packet tag, and tag 2 is
        // a signature. Both header formats are specified in RFC 9580 section 4.2:
        // https://www.rfc-editor.org/rfc/rfc9580.html#section-4.2
        if ((header & 0xc0) == 0xc0) {
            if ((header & 0x3f) != 2) {
                InvalidSignature(digest, "expected an OpenPGP signature packet");
            }
            const auto length = packet.Integer(1);
            // Values below 192 encode the length directly; 192..223 introduce
            // the specified two-byte encoding; 255 introduces a four-byte
            // length. Values 224..254 describe partial bodies. OpenPGP permits
            // those only for literal, compressed, or encrypted data packets,
            // so they cannot delimit this signature packet.
            // https://www.rfc-editor.org/rfc/rfc9580.html#section-4.2.1.4
            if (length < 192) {
                return length;
            }
            if (length < 224) {
                return (length - 192) * 256 + packet.Integer(1) + 192;
            }
            if (length == 255) {
                return packet.Integer(4);
            }
            InvalidSignature(digest, "partial lengths are invalid for an OpenPGP signature packet");
        }
        // A legacy header puts the tag in bits 5..2. Mask 0xbc checks the
        // packet marker and those tag bits against 0x80 | (2 << 2). Its low
        // two bits select 1, 2, or 4 length bytes; value 3 has no explicit
        // length. This verifier requires an explicit signature packet length
        // so it can reject any extra packets after the signature.
        // https://www.rfc-editor.org/rfc/rfc9580.html#section-4.2.2
        if (((header & 0xbc) != 0x88) || ((header & 3) == 3)) {
            InvalidSignature(digest, "expected a length-delimited OpenPGP signature packet");
        }
        return packet.Integer(std::size_t{1} << (header & 3));
    }();
    const auto body = packet.Read(body_length);
    packet.RequireEnd();
    auto fields = SignatureReader{body, digest};
    // These four fields are version 4, binary-document signature type 0,
    // public-key algorithm 19 (ECDSA), and hash algorithm 10 (SHA-512).
    // Requiring the binary-document type ensures that we verify a signature
    // over our message rather than a certification or another OpenPGP object.
    // The layout and ECDSA's two integer fields are specified here:
    // https://www.rfc-editor.org/rfc/rfc9580.html#section-5.2.3
    // https://www.rfc-editor.org/rfc/rfc9580.html#section-5.2.3.2
    if (!std::ranges::equal(fields.Read(4), std::array{4, 0, 19, 10})) {
        InvalidSignature(digest, "expected a v4 binary ECDSA/SHA-512 signature");
    }
    const auto hashed_length = fields.Integer(2);
    std::ignore = fields.Read(hashed_length);
    // GnuPG includes signature metadata in subpackets. Reconstructing its hash
    // requires the four fields above, the two-byte hashed-subpacket length,
    // and that many bytes verbatim; it does not require decoding the metadata.
    // The second subpacket area's length lets us skip the unhashed metadata
    // and locate the hash prefix and ECDSA integers that follow it.
    const auto signed_fields = body.first(std::size_t{6} + hashed_length);
    const auto unhashed_length = fields.Integer(2);
    std::ignore = fields.Read(unhashed_length);
    const auto hash_prefix = fields.Read(2);
    // OpenPGP stores ECDSA r and s as variable-length integers. CNG consumes
    // their fixed-width concatenation, r || s, with 66 bytes per P-521 integer.
    // The BCryptVerifySignature documentation does not describe this encoding;
    // libssh2's CNG backend demonstrates both the padding and the API call:
    // https://github.com/libssh2/libssh2/blob/libssh2-1.11.1/src/wincng.c#L1784-L1838
    // https://github.com/libssh2/libssh2/blob/libssh2-1.11.1/src/wincng.c#L2384-L2452
    auto scalars = std::array<UCHAR, 2 * p521_scalar_size>{};
    fields.ReadScalar(std::span{scalars}.first<p521_scalar_size>());
    fields.ReadScalar(std::span{scalars}.last<p521_scalar_size>());
    fields.RequireEnd();

    const auto hash = CreateHash(BCRYPT_SHA512_ALG_HANDLE, digest);
    // https://eel.is/c++draft/basic.lval#11
    [[gsl::suppress("26490", justification:
        "C++ [basic.lval] permits an object's stored value to be accessed through "
        "an `unsigned char` glvalue. `UCHAR` is an alias for `unsigned char`, so "
        "the `reinterpret_cast` allows the message bytes to be read through "
        "the pointer type required by `BCryptHashData`.")]]
    HashData(hash.get(), std::span{
        reinterpret_cast<const UCHAR *>(digest.data()), digest.size()}, digest);
    HashData(hash.get(), signed_fields, digest);
    // V4 hashes the message, then the signed fields, then a six-byte trailer:
    // version 4, marker 0xff, and the four-byte big-endian signed-fields length.
    // That length excludes both the original message and the outer packet
    // header. The 0xff masks below select individual bytes of that length.
    // https://www.rfc-editor.org/rfc/rfc9580.html#section-5.2.4
    const auto length = wil::safe_cast_failfast<std::uint32_t>(signed_fields.size());
    [[gsl::suppress("26472",
        justification: "n & 0xff ∈ [0, 255] ∀n ∈ ℤ such that n ≥ 0")]]
    const auto trailer = std::to_array<UCHAR>({
        4, 0xff,
        static_cast<UCHAR>((length >> 24) & 0xff),
        static_cast<UCHAR>((length >> 16) & 0xff),
        static_cast<UCHAR>((length >> 8) & 0xff),
        static_cast<UCHAR>(length & 0xff)
    });
    HashData(hash.get(), trailer, digest);
    // SHA-512 produces 512 bits; CNG's output buffer is measured in bytes.
    auto signed_hash = FinishHash<512 / 8>(hash.get(), digest);
    // The packet includes the first two bytes of the hash that GnuPG computed.
    // Comparing them with our reconstructed hash distinguishes a hash mismatch
    // from a failure in the subsequent ECDSA verification.
    if (!std::ranges::equal(hash_prefix, std::span{signed_hash}.first<2>())) {
        InvalidSignature(digest, "OpenPGP hash prefix does not match");
    }
    const auto key = SigningKey();
    if (const auto status = BCryptVerifySignature(key.get(), nullptr,
            signed_hash.data(), CompileTimeCast<ULONG, signed_hash.size()>(),
            scalars.data(), CompileTimeCast<ULONG, scalars.size()>(), 0);
        !BCRYPT_SUCCESS(status)) {
        WinError("signature verification failed for OCI layer '{}' with the pinned signing key",
            digest, ExplicitHresult{HRESULT_FROM_NT(status)});
    }
}

// The OCI layer descriptor identifies the stored blob, which may be compressed.
// That digest is used for both version checks and signatures, so the file is
// hashed before decompression; hashing the uncompressed tar stream would
// instead produce the rootfs DiffID:
// https://github.com/opencontainers/image-spec/blob/v1.1.1/config.md#layer-diffid
// `MaterializeOci` calls this after acquiring a replacement layer and before
// importing it into WSL. A read failure or digest mismatch stops the import.
export auto VerifyOciLayerFile(const std::filesystem::path &path,
    const std::string_view digest) -> void {
    RequireSha256Digest(digest);
    // Materialization retains a delete-on-close handle until WSL finishes
    // importing the layer, so this reader must permit delete sharing.
    const auto file = wil::unique_hfile{CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_SEQUENTIAL_SCAN, nullptr)};
    if (!file) {
        WinError("could not open OCI layer '{}' for hash verification",
            std::wstring_view{path.native()});
    }
    const auto hash = CreateHash(BCRYPT_SHA256_ALG_HANDLE, digest);
    auto buffer = std::vector<UCHAR>(64 * 1024);
    while (true) {
        auto received = DWORD{};
        if (!ReadFile(file.get(), buffer.data(),
                wil::safe_cast_failfast<DWORD>(buffer.size()), &received, nullptr)) {
            WinError("could not read OCI layer '{}' for hash verification",
                std::wstring_view{path.native()});
        }
        if (received == 0) {
            break;
        }
        HashData(hash.get(), std::span{buffer}.first(received), digest);
    }
    auto actual = std::string{"sha256:"};
    // SHA-256 produces 256 bits, with two hexadecimal digits per byte.
    for (const auto byte : FinishHash<256 / 8>(hash.get(), digest)) {
        std::format_to(std::back_inserter(actual), "{:02x}", byte);
    }
    if (actual != digest) {
        throw std::runtime_error(std::format(
            "OCI layer '{}' has digest '{}'; expected '{}'",
            path.string(), actual, digest));
    }
}
