// crd-crypto -- the Messaging Layer Security protocol, RFC 9420: cipher suites, the presentation language, tree math
// and the labelled operations (NET.2; contract: docs/design/network-foundation.md#mls).
#include <crd/crypto/mls.hpp>

#include "byte_spans.hpp"

#include <crd/core/assert.hpp>

#include <bit>

namespace crd::crypto::mls
{
namespace
{
using detail::LabelBuffer;
using detail::refuse;

constexpr CipherSuite kSuites[] = {
    {iana::kMls128Dhkemx25519Aes128gcmSha256Ed25519, AlgorithmId::X25519, AlgorithmId::HkdfSha256,
     AlgorithmId::Aes128Gcm, AlgorithmId::Ed25519},
    {iana::kMls128Dhkemp256Aes128gcmSha256P256, AlgorithmId::Secp256r1, AlgorithmId::HkdfSha256,
     AlgorithmId::Aes128Gcm, AlgorithmId::EcdsaSecp256r1Sha256},
    {iana::kMls128Dhkemx25519Chacha20poly1305Sha256Ed25519, AlgorithmId::X25519, AlgorithmId::HkdfSha256,
     AlgorithmId::Chacha20Poly1305, AlgorithmId::Ed25519},
    {iana::kMls256Dhkemp384Aes256gcmSha384P384, AlgorithmId::Secp384r1, AlgorithmId::HkdfSha384,
     AlgorithmId::Aes256Gcm, AlgorithmId::EcdsaSecp384r1Sha384},
};

constexpr usize kMaxExpandBytes = 0xFFFFU; // KDFLabel.length is a uint16

cont::ConstSpan<u8> bytes_of(cont::StringView text) noexcept
{
    return cont::ConstSpan<u8>(reinterpret_cast<const u8*>(text.data()), text.size());
}

// The bytes of the vector "MLS 1.0 " + label; 0 when it cannot be encoded.
usize label_bytes(cont::StringView label) noexcept
{
    return vector_bytes(kLabelPrefix.size() + label.size());
}

void put_label(cont::WireWriter& out, cont::StringView label) noexcept
{
    put_varint(out, kLabelPrefix.size() + label.size());
    out.put_bytes(bytes_of(kLabelPrefix));
    out.put_bytes(bytes_of(label));
}

// The bytes of the struct {opaque label<V>; opaque content<V>;} that SignContent and EncryptContext share, with the
// prefixed label; 0 when a field cannot be encoded.
usize labelled_bytes(cont::StringView label, cont::ConstSpan<u8> content) noexcept
{
    const usize head = label_bytes(label);
    const usize body = vector_bytes(content.size());
    return head == 0U || body == 0U ? 0U : head + body;
}

void put_labelled(cont::WireWriter& out, cont::StringView label, cont::ConstSpan<u8> content) noexcept
{
    put_label(out, label);
    put_vector(out, content);
}

// A provider call's result, its output zeroed when it refused.
CryptoError passed(CryptoError error, cont::Span<u8> out) noexcept
{
    return error == CryptoError::None ? error : refuse(error, out);
}

// Whether `node` is a node of a valid tree of `leaves` leaves.
bool in_tree(u32 node, u32 leaves) noexcept
{
    return tree::valid_size(leaves) && node < tree::node_width(leaves);
}
} // namespace

cont::ConstSpan<CipherSuite> cipher_suites() noexcept
{
    return kSuites;
}

const CipherSuite* cipher_suite(u16 id) noexcept
{
    for (const CipherSuite& suite : kSuites)
    {
        if (suite.id == id)
        {
            return &suite;
        }
    }
    return nullptr;
}

// ---- The presentation language ----

usize varint_bytes(usize value) noexcept
{
    if (value <= 0x3FU)
    {
        return 1U;
    }
    if (value <= 0x3FFFU)
    {
        return 2U;
    }
    return value <= kMaxVarint ? 4U : 0U;
}

usize vector_bytes(usize length) noexcept
{
    const usize header = varint_bytes(length);
    return header == 0U ? 0U : header + length;
}

void put_varint(cont::WireWriter& out, usize value) noexcept
{
    switch (varint_bytes(value))
    {
    case 1U:
        out.put_u8(static_cast<u8>(value));
        break;
    case 2U:
        out.put_u16(static_cast<u16>(0x4000U | value));
        break;
    case 4U:
        out.put_u32(static_cast<u32>(0x80000000U | value));
        break;
    default:
        CRD_FATAL("mls::put_varint: a length above 2^30 - 1, which no vector header encodes");
    }
}

void put_vector(cont::WireWriter& out, cont::ConstSpan<u8> bytes) noexcept
{
    put_varint(out, bytes.size());
    out.put_bytes(bytes);
}

bool take_varint(cont::WireReader& in, usize& value) noexcept
{
    u8 first = 0U;
    if (!in.take_u8(first))
    {
        return false;
    }
    const u32 prefix = first >> 6U;
    if (prefix == 3U)
    {
        return false;
    }
    const usize length = usize{1} << prefix;
    usize       result = first & 0x3FU;
    for (usize i = 1U; i < length; ++i)
    {
        u8 next = 0U;
        if (!in.take_u8(next))
        {
            return false;
        }
        result = (result << 8U) | next;
    }
    // A value that fits half the length must have used it (section 2.1.2).
    if (prefix >= 1U && result < (usize{1} << (8U * (length / 2U) - 2U)))
    {
        return false;
    }
    value = result;
    return true;
}

bool take_vector(cont::WireReader& in, cont::ConstSpan<u8>& out) noexcept
{
    usize length = 0U;
    return take_varint(in, length) && in.take_bytes(length, out);
}

bool take_presence(cont::WireReader& in, bool& present) noexcept
{
    u8 octet = 0U;
    if (!in.take_u8(octet) || octet > 1U)
    {
        return false;
    }
    present = octet == 1U;
    return true;
}

// ---- Array-based trees ----

namespace tree
{
bool valid_size(u32 leaves) noexcept
{
    return leaves != 0U && leaves <= kMaxLeaves && std::has_single_bit(leaves);
}

u32 node_width(u32 leaves) noexcept
{
    return valid_size(leaves) ? 2U * (leaves - 1U) + 1U : 0U;
}

std::optional<u32> root(u32 leaves) noexcept
{
    // Appendix C's (1 << log2(node_width(n))) - 1, which for a perfect tree is n - 1.
    return valid_size(leaves) ? std::optional<u32>(leaves - 1U) : std::nullopt;
}

u32 level(u32 node) noexcept
{
    return static_cast<u32>(std::countr_one(node));
}

std::optional<u32> left(u32 node, u32 leaves) noexcept
{
    const u32 k = level(node);
    return in_tree(node, leaves) && k != 0U ? std::optional<u32>(node ^ (1U << (k - 1U))) : std::nullopt;
}

std::optional<u32> right(u32 node, u32 leaves) noexcept
{
    const u32 k = level(node);
    return in_tree(node, leaves) && k != 0U ? std::optional<u32>(node ^ (3U << (k - 1U))) : std::nullopt;
}

std::optional<u32> parent(u32 node, u32 leaves) noexcept
{
    if (!in_tree(node, leaves) || node == leaves - 1U)
    {
        return std::nullopt;
    }
    // Below the root a node's level is at most 30, so every shift stays inside the word.
    const u32 k = level(node);
    const u32 b = (node >> (k + 1U)) & 1U;
    return (node | (1U << k)) ^ (b << (k + 1U));
}

std::optional<u32> sibling(u32 node, u32 leaves) noexcept
{
    const std::optional<u32> up = parent(node, leaves);
    if (!up)
    {
        return std::nullopt;
    }
    return node < *up ? right(*up, leaves) : left(*up, leaves);
}

usize direct_path(u32 node, u32 leaves, cont::Span<u32> out) noexcept
{
    if (!in_tree(node, leaves))
    {
        return 0U;
    }
    // The root is at level log2(n); a node at level k has log2(n) - k ancestors.
    const usize depth = static_cast<usize>(std::countr_zero(leaves)) - level(node);
    if (out.size() < depth)
    {
        return 0U;
    }
    u32 at = node;
    for (usize i = 0U; i < depth; ++i)
    {
        at     = *parent(at, leaves);
        out[i] = at;
    }
    return depth;
}

usize copath(u32 node, u32 leaves, cont::Span<u32> out) noexcept
{
    u32         path[kMaxDepth] = {};
    const usize depth = direct_path(node, leaves, path);
    if (out.size() < depth)
    {
        return 0U;
    }
    // The sibling of the node itself, then of every ancestor but the root.
    u32 at = node;
    for (usize i = 0U; i < depth; ++i)
    {
        out[i] = *sibling(at, leaves);
        at     = path[i];
    }
    return depth;
}

std::optional<u32> common_ancestor(u32 a, u32 b, u32 leaves) noexcept
{
    if (!in_tree(a, leaves) || !in_tree(b, leaves))
    {
        return std::nullopt;
    }
    // Appendix C's common_ancestor_direct, in 64 bits: a level of 31 shifts by 32.
    const u64 x  = a;
    const u64 y  = b;
    const u64 lx = level(a) + 1U;
    const u64 ly = level(b) + 1U;
    if (lx <= ly && x >> ly == y >> ly)
    {
        return b;
    }
    if (ly <= lx && x >> lx == y >> lx)
    {
        return a;
    }
    u64 xn = x;
    u64 yn = y;
    u64 k  = 0U;
    while (xn != yn)
    {
        xn >>= 1U;
        yn >>= 1U;
        ++k;
    }
    return static_cast<u32>((xn << k) + (u64{1} << (k - 1U)) - 1U);
}
} // namespace tree

// ---- Labelled operations ----

CryptoError ref_hash(const CipherSuite& suite, cont::StringView label, cont::ConstSpan<u8> value,
                     cont::Span<u8> out) noexcept
{
    const usize head = vector_bytes(label.size());
    const usize body = vector_bytes(value.size());
    if (head == 0U || body == 0U || out.size() != suite.hash_bytes())
    {
        return refuse(CryptoError::BadLength, out);
    }
    LabelBuffer      input(head + body);
    cont::WireWriter writer(input.span());
    put_vector(writer, bytes_of(label));
    put_vector(writer, value);
    return passed(kdf_hash(suite.kdf, input.span(), out), out);
}

CryptoError expand_with_label(const CipherSuite& suite, cont::ConstSpan<u8> secret, cont::StringView label,
                              cont::ConstSpan<u8> context, cont::Span<u8> out) noexcept
{
    const usize fields = labelled_bytes(label, context);
    if (fields == 0U || out.empty() || out.size() > kMaxExpandBytes)
    {
        return refuse(CryptoError::BadLength, out);
    }
    LabelBuffer      info(2U + fields);
    cont::WireWriter writer(info.span());
    writer.put_u16(static_cast<u16>(out.size()));
    put_labelled(writer, label, context);
    return passed(hkdf_expand(suite.kdf, secret, info.span(), out), out);
}

CryptoError derive_secret(const CipherSuite& suite, cont::ConstSpan<u8> secret, cont::StringView label,
                          cont::Span<u8> out) noexcept
{
    if (out.size() != suite.hash_bytes())
    {
        return refuse(CryptoError::BadLength, out);
    }
    return expand_with_label(suite, secret, label, {}, out);
}

CryptoError derive_tree_secret(const CipherSuite& suite, cont::ConstSpan<u8> secret, cont::StringView label,
                               u32 generation, cont::Span<u8> out) noexcept
{
    u8               context[4];
    cont::WireWriter writer(context);
    writer.put_u32(generation);
    return expand_with_label(suite, secret, label, context, out);
}

CryptoError sign_with_label(const CipherSuite& suite, const Signer& signer, cont::StringView label,
                            cont::ConstSpan<u8> content, cont::Span<u8> out, usize& written) noexcept
{
    written = 0U;
    if (signer.algorithm() != suite.signature)
    {
        return refuse(CryptoError::Unsupported, out);
    }
    const usize bytes = labelled_bytes(label, content);
    if (bytes == 0U)
    {
        return refuse(CryptoError::BadLength, out);
    }
    LabelBuffer      sign_content(bytes);
    cont::WireWriter writer(sign_content.span());
    put_labelled(writer, label, content);
    const CryptoError error = signer.sign(sign_content.span(), out, written);
    if (error != CryptoError::None)
    {
        written = 0U;
        return refuse(error, out);
    }
    return error;
}

Verification verify_with_label(const CipherSuite& suite, const SignatureVerifier& verifier,
                               cont::ConstSpan<u8> public_key, cont::StringView label, cont::ConstSpan<u8> content,
                               cont::ConstSpan<u8> signature) noexcept
{
    const usize bytes = labelled_bytes(label, content);
    if (bytes == 0U)
    {
        return Verification::Invalid;
    }
    LabelBuffer      sign_content(bytes);
    cont::WireWriter writer(sign_content.span());
    put_labelled(writer, label, content);
    return verifier.verify(suite.signature, public_key, sign_content.span(), signature);
}

CryptoError encrypt_with_label(const CipherSuite& suite, cont::ConstSpan<u8> public_key, cont::StringView label,
                               cont::ConstSpan<u8> context, cont::ConstSpan<u8> plaintext,
                               cont::Span<u8> kem_output, cont::Span<u8> ciphertext) noexcept
{
    const usize bytes = labelled_bytes(label, context);
    if (bytes == 0U)
    {
        return refuse(CryptoError::BadLength, kem_output, ciphertext);
    }
    LabelBuffer      info(bytes);
    cont::WireWriter writer(info.span());
    put_labelled(writer, label, context);
    return hpke_seal(suite.hpke(), public_key, info.span(), {}, plaintext, kem_output, ciphertext);
}

CryptoError decrypt_with_label(const CipherSuite& suite, const HpkeKey& key, cont::StringView label,
                               cont::ConstSpan<u8> context, cont::ConstSpan<u8> kem_output,
                               cont::ConstSpan<u8> ciphertext, cont::Span<u8> plaintext) noexcept
{
    const usize bytes = labelled_bytes(label, context);
    if (bytes == 0U)
    {
        return refuse(CryptoError::BadLength, plaintext);
    }
    LabelBuffer      info(bytes);
    cont::WireWriter writer(info.span());
    put_labelled(writer, label, context);
    return key.open(suite.hpke(), kem_output, info.span(), {}, ciphertext, plaintext);
}
} // namespace crd::crypto::mls
