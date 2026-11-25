// The signer of an APK, read from its v2 signature (APK Signature Scheme v2, as Android documents it).
#pragma once

#include <CommonCrypto/CommonDigest.h>

#include <cstdint>
#include <cstring>
#include <span>
#include <string>

namespace nestvane::apk {

template <class Value>
inline bool load(std::span<const uint8_t> bytes, size_t at, Value& value) {
    if (at > bytes.size() || bytes.size() - at < sizeof(Value)) return false;
    std::memcpy(&value, bytes.data() + at, sizeof(Value));
    return true;
}

// A length-prefixed piece (uint32 length, then the bytes) at `at` inside `outer`.
inline bool piece(std::span<const uint8_t> outer, size_t at, std::span<const uint8_t>& inner) {
    uint32_t length = 0;
    if (!load(outer, at, length) || length > outer.size() - at - 4) return false;
    inner = outer.subspan(at + 4, length);
    return true;
}

// The first certificate of the first signer of an APK's v2 signature (APK Signature Scheme v2):
// the signing block sits right before the central directory, which the end record points to.
inline std::span<const uint8_t> firstV2Certificate(std::span<const uint8_t> apk) {
    size_t end = SIZE_MAX;
    for (size_t comment = 0; comment <= 0xffff && comment + 22 <= apk.size(); ++comment) {
        const size_t at = apk.size() - 22 - comment;
        uint32_t magic = 0;
        uint16_t length = 0;
        if (load(apk, at, magic) && magic == 0x06054b50 && load(apk, at + 20, length) && length == comment) {
            end = at;
            break;
        }
    }
    uint32_t directory = 0;
    if (end == SIZE_MAX || !load(apk, end + 16, directory) || directory < 32 || directory > end ||
        std::memcmp(apk.data() + directory - 16, "APK Sig Block 42", 16))
        return {};
    uint64_t blockSize = 0, sizeAgain = 0;
    if (!load(apk, directory - 24, blockSize) || blockSize < 24 || blockSize > directory - 8) return {};
    const size_t start = directory - blockSize - 8;
    if (!load(apk, start, sizeAgain) || sizeAgain != blockSize) return {};
    const size_t pairsEnd = directory - 24;
    for (size_t pair = start + 8; pair + 12 <= pairsEnd;) {
        uint64_t length = 0;
        uint32_t id = 0;
        if (!load(apk, pair, length) || !load(apk, pair + 8, id) || length < 4 || length > pairsEnd - pair - 8) return {};
        if (id == 0x7109871a) {
            // signers > signer > signed data > digests, then certificates > certificate
            const auto value = apk.subspan(pair + 12, length - 4);
            std::span<const uint8_t> signers, signer, signedData, digests, certificates, certificate;
            if (!piece(value, 0, signers) || !piece(signers, 0, signer) || !piece(signer, 0, signedData) ||
                !piece(signedData, 0, digests) || !piece(signedData, 4 + digests.size(), certificates) ||
                !piece(certificates, 0, certificate))
                return {};
            return certificate;
        }
        pair += 8 + length;
    }
    return {};
}

// The SHA-256 of bytes, in lowercase hex.
inline std::string sha256(std::span<const uint8_t> bytes) {
    uint8_t digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(bytes.data(), CC_LONG(bytes.size()), digest);
    static constexpr char hex[] = "0123456789abcdef";
    std::string text;
    for (const auto byte : digest) {
        text.push_back(hex[byte >> 4]);
        text.push_back(hex[byte & 15]);
    }
    return text;
}

// Whether a certificate is the one KernelSU's official Manager is signed with: its length and its
// SHA-256, as KernelSU's Kbuild gives them.
inline bool kernelSuManagerCertificate(std::span<const uint8_t> certificate) {
    return certificate.size() == 0x33b &&
           sha256(certificate) == "c371061b19d8c7d7d6133c6a9bafe198fa944e50c1b31c9d8daa8d7f1fc2d2d6";
}


} // namespace nestvane::apk
