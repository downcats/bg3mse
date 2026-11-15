#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// How a channel manifest is signed: the bytes of the manifest, then the
// footer Norbyte's updater puts after a signed package (CoreLib/Crypto.h,
// PackageSignature): 260 bytes, the magic 'BGS1', an ECDSA P-256 signature
// over the SHA-256 of every byte before the footer, the footer version, the
// number of signed bytes, and zeros. The signature is made and checked with
// TinyCrypt, as Norbyte's is; the key is bg3mse's own, not his.

namespace bg3se::mac::updater {

inline constexpr size_t kPublicKeySize = 64;    // x and y of the P-256 point, big-endian
inline constexpr size_t kPrivateKeySize = 32;
inline constexpr size_t kSignatureSize = 64;
inline constexpr size_t kFooterSize = 260;

using PublicKey = std::array<uint8_t, kPublicKeySize>;
using PrivateKey = std::array<uint8_t, kPrivateKeySize>;
using Digest = std::array<uint8_t, 32>;

Digest Sha256(std::span<uint8_t const> bytes);
// SHA-256 of a file, read in pieces; false when it cannot be read.
bool Sha256File(std::string const& path, Digest& digest, uint64_t* size = nullptr);
std::string Hex(std::span<uint8_t const> bytes);
// Lower- or upper-case hexadecimal of exactly out.size() bytes.
bool FromHex(std::string_view text, std::span<uint8_t> out);

// A new key pair from the system's random source.
bool MakeKeyPair(PublicKey& publicKey, PrivateKey& privateKey);
// The public half of a private key.
bool PublicKeyOf(PrivateKey const& privateKey, PublicKey& publicKey);

// `payload` followed by its footer.
bool Sign(std::span<uint8_t const> payload, PrivateKey const& key, std::vector<uint8_t>& out);

// Checks the footer at the end of `file` against `key`. On success `payload`
// is the signed bytes, the file without its footer; otherwise `why` says
// what is wrong. Nothing about the payload is looked at before this.
bool Verify(std::span<uint8_t const> file, PublicKey const& key, std::span<uint8_t const>& payload, std::string& why);

}
