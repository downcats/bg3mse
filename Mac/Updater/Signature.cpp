#include <Updater/Signature.h>

#include <CommonCrypto/CommonDigest.h>
#include <tinycrypt/constants.h>
#include <tinycrypt/ecc.h>
#include <tinycrypt/ecc_dh.h>
#include <tinycrypt/ecc_dsa.h>

#include <cstdlib>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

namespace bg3se::mac::updater {

namespace {

// Norbyte's PackageSignature (CoreLib/Crypto.h), byte for byte. His header
// needs the extender's precompiled header, so its layout is repeated here
// and checked.
#pragma pack(push, 1)
struct Footer
{
    static constexpr uint32_t kMagic = 'BGS1';
    static constexpr uint8_t kSignatureInManifest = 1;     // VER_SIGNATURE_IN_MANIFEST

    uint32_t Magic;
    uint8_t Signature[kSignatureSize];
    uint8_t Version;
    uint32_t SignedBytes;
    uint8_t Unused[187];
};
#pragma pack(pop)
static_assert(sizeof(Footer) == kFooterSize);
static_assert(offsetof(Footer, Signature) == 4 && offsetof(Footer, Version) == 68 && offsetof(Footer, SignedBytes) == 69);
static_assert(NUM_ECC_BYTES * 2 == kSignatureSize);

int SystemRandom(uint8_t* dest, unsigned size)
{
    arc4random_buf(dest, size);
    return 1;
}

}

Digest Sha256(std::span<uint8_t const> bytes)
{
    Digest digest{};
    CC_SHA256_CTX context;
    CC_SHA256_Init(&context);
    size_t done = 0;
    while (done < bytes.size()) {
        size_t piece = std::min<size_t>(bytes.size() - done, 1u << 30);
        CC_SHA256_Update(&context, bytes.data() + done, (CC_LONG)piece);
        done += piece;
    }
    CC_SHA256_Final(digest.data(), &context);
    return digest;
}

bool Sha256File(std::string const& path, Digest& digest, uint64_t* size)
{
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    CC_SHA256_CTX context;
    CC_SHA256_Init(&context);
    std::vector<uint8_t> buffer(1 << 20);
    uint64_t total = 0;
    for (;;) {
        ssize_t n = read(fd, buffer.data(), buffer.size());
        if (n < 0) {
            if (errno == EINTR) continue;
            close(fd);
            return false;
        }
        if (n == 0) break;
        CC_SHA256_Update(&context, buffer.data(), (CC_LONG)n);
        total += (uint64_t)n;
    }
    close(fd);
    CC_SHA256_Final(digest.data(), &context);
    if (size) *size = total;
    return true;
}

std::string Hex(std::span<uint8_t const> bytes)
{
    static char const digits[] = "0123456789abcdef";
    std::string text;
    text.reserve(bytes.size() * 2);
    for (uint8_t b : bytes) {
        text += digits[b >> 4];
        text += digits[b & 15];
    }
    return text;
}

bool FromHex(std::string_view text, std::span<uint8_t> out)
{
    if (text.size() != out.size() * 2) return false;
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < out.size(); i++) {
        int hi = nibble(text[2 * i]), lo = nibble(text[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = (uint8_t)(hi << 4 | lo);
    }
    return true;
}

bool MakeKeyPair(PublicKey& publicKey, PrivateKey& privateKey)
{
    uECC_set_rng(&SystemRandom);
    return uECC_make_key(publicKey.data(), privateKey.data(), uECC_secp256r1()) == TC_CRYPTO_SUCCESS;
}

bool PublicKeyOf(PrivateKey const& privateKey, PublicKey& publicKey)
{
    return uECC_compute_public_key(privateKey.data(), publicKey.data(), uECC_secp256r1()) != 0
        && uECC_valid_public_key(publicKey.data(), uECC_secp256r1()) == 0;
}

bool Sign(std::span<uint8_t const> payload, PrivateKey const& key, std::vector<uint8_t>& out)
{
    if (payload.size() > UINT32_MAX) return false;
    Footer footer{};
    footer.Magic = Footer::kMagic;
    footer.Version = Footer::kSignatureInManifest;
    footer.SignedBytes = (uint32_t)payload.size();
    Digest digest = Sha256(payload);
    uECC_set_rng(&SystemRandom);
    if (uECC_sign(key.data(), digest.data(), (unsigned)digest.size(), footer.Signature, uECC_secp256r1()) != TC_CRYPTO_SUCCESS) {
        return false;
    }
    out.assign(payload.begin(), payload.end());
    auto const* bytes = reinterpret_cast<uint8_t const*>(&footer);
    out.insert(out.end(), bytes, bytes + sizeof(footer));
    return true;
}

bool Verify(std::span<uint8_t const> file, PublicKey const& key, std::span<uint8_t const>& payload, std::string& why)
{
    if (file.size() < sizeof(Footer)) {
        why = "it is not signed (shorter than a signature)";
        return false;
    }
    Footer footer;
    memcpy(&footer, file.data() + file.size() - sizeof(Footer), sizeof(Footer));
    if (footer.Magic != Footer::kMagic) {
        why = "it is not signed (no signature at its end)";
        return false;
    }
    size_t signedBytes = file.size() - sizeof(Footer);
    if (footer.Version != Footer::kSignatureInManifest || footer.SignedBytes != signedBytes) {
        why = "its signature is not one of a whole manifest";
        return false;
    }
    for (uint8_t b : footer.Unused) {
        if (b != 0) {
            why = "its signature has data where there must be none";
            return false;
        }
    }
    if (uECC_valid_public_key(key.data(), uECC_secp256r1()) != 0) {
        why = "the update key built in is not a valid key";
        return false;
    }
    Digest digest = Sha256(file.first(signedBytes));
    if (uECC_verify(key.data(), digest.data(), (unsigned)digest.size(), footer.Signature, uECC_secp256r1()) != TC_CRYPTO_SUCCESS) {
        why = "its signature is not bg3mse's (wrong key, or the manifest was changed)";
        return false;
    }
    payload = file.first(signedBytes);
    return true;
}

}
