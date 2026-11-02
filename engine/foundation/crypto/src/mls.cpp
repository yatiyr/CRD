// crd-crypto -- the Messaging Layer Security protocol, RFC 9420: cipher suites, the presentation language, tree math,
// the labelled operations, the key schedule and the secret tree (NET.2; contract:
// docs/design/network-foundation.md#mls).
#include <crd/crypto/mls.hpp>

#include "byte_spans.hpp"
#include "mls_detail.hpp"

#include <crd/core/assert.hpp>

#include <bit>
#include <utility>

namespace crd::crypto::mls
{
namespace
{
using detail::label_of;
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
    // draft-ietf-mls-pq-ciphersuites-07 (decision M2): codepoints IANA does not list yet, so the register marks them
    // unverified and the names say so here.
    {iana::unverified::kMls128Mlkem768x25519Aes128gcmSha256Ed25519, AlgorithmId::X25519Mlkem768,
     AlgorithmId::HkdfSha256, AlgorithmId::Aes128Gcm, AlgorithmId::Ed25519},
    {iana::unverified::kMls128Mlkem768x25519Aes256gcmSha384Ed25519, AlgorithmId::X25519Mlkem768,
     AlgorithmId::HkdfSha384, AlgorithmId::Aes256Gcm, AlgorithmId::Ed25519},
    {iana::unverified::kMls128Mlkem768p256Aes128gcmSha256P256, AlgorithmId::Secp256r1Mlkem768,
     AlgorithmId::HkdfSha256, AlgorithmId::Aes128Gcm, AlgorithmId::EcdsaSecp256r1Sha256},
    {iana::unverified::kMls128Mlkem768p256Aes256gcmSha384P256, AlgorithmId::Secp256r1Mlkem768,
     AlgorithmId::HkdfSha384, AlgorithmId::Aes256Gcm, AlgorithmId::EcdsaSecp256r1Sha256},
    {iana::unverified::kMls192Mlkem1024p384Aes256gcmSha384P384, AlgorithmId::Secp384r1Mlkem1024,
     AlgorithmId::HkdfSha384, AlgorithmId::Aes256Gcm, AlgorithmId::EcdsaSecp384r1Sha384},
    {iana::unverified::kMls128Mlkem768Aes256gcmSha384Ed25519, AlgorithmId::MlKem768, AlgorithmId::HkdfSha384,
     AlgorithmId::Aes256Gcm, AlgorithmId::Ed25519},
    {iana::unverified::kMls128Mlkem768Aes256gcmSha384P256, AlgorithmId::MlKem768, AlgorithmId::HkdfSha384,
     AlgorithmId::Aes256Gcm, AlgorithmId::EcdsaSecp256r1Sha256},
    {iana::unverified::kMls192Mlkem1024Aes256gcmSha384P384, AlgorithmId::MlKem1024, AlgorithmId::HkdfSha384,
     AlgorithmId::Aes256Gcm, AlgorithmId::EcdsaSecp384r1Sha384},
    {iana::unverified::kMls128Mlkem768x25519Chacha20poly1305Sha384Mldsa44, AlgorithmId::X25519Mlkem768,
     AlgorithmId::HkdfSha384, AlgorithmId::Chacha20Poly1305, AlgorithmId::MlDsa44},
    {iana::unverified::kMls192Mlkem768Aes256gcmSha384Mldsa65, AlgorithmId::MlKem768, AlgorithmId::HkdfSha384,
     AlgorithmId::Aes256Gcm, AlgorithmId::MlDsa65},
    {iana::unverified::kMls256Mlkem1024Aes256gcmSha384Mldsa87, AlgorithmId::MlKem1024, AlgorithmId::HkdfSha384,
     AlgorithmId::Aes256Gcm, AlgorithmId::MlDsa87},
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
    const usize head = labelled_head_bytes(label, content.size());
    return head == 0U ? 0U : head + content.size();
}

void put_labelled(cont::WireWriter& out, cont::StringView label, cont::ConstSpan<u8> content) noexcept
{
    put_labelled_head(out, label, content.size());
    out.put_bytes(content);
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

// Appendix C's parent of a node already known to lie inside a tree, below the root (whose level is at most 30, so
// every shift stays inside the word); the children are mls_detail.hpp's.
u32 parent_of(u32 node) noexcept
{
    const u32 k = tree::level(node);
    const u32 b = (node >> (k + 1U)) & 1U;
    return (node | (1U << k)) ^ (b << (k + 1U));
}

u32 sibling_of(u32 node) noexcept
{
    const u32 up = parent_of(node);
    return node < up ? right_of(up) : left_of(up);
}
} // namespace

usize labelled_head_bytes(cont::StringView label, usize content_bytes) noexcept
{
    const usize head = label_bytes(label);
    const usize body = vector_bytes(content_bytes);
    return head == 0U || body == 0U ? 0U : head + body - content_bytes;
}

void put_labelled_head(cont::WireWriter& out, cont::StringView label, usize content_bytes) noexcept
{
    put_label(out, label);
    put_varint(out, content_bytes);
}

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
    return in_tree(node, leaves) && level(node) != 0U ? std::optional<u32>(left_of(node)) : std::nullopt;
}

std::optional<u32> right(u32 node, u32 leaves) noexcept
{
    return in_tree(node, leaves) && level(node) != 0U ? std::optional<u32>(right_of(node)) : std::nullopt;
}

std::optional<u32> parent(u32 node, u32 leaves) noexcept
{
    return in_tree(node, leaves) && node != leaves - 1U ? std::optional<u32>(parent_of(node)) : std::nullopt;
}

std::optional<u32> sibling(u32 node, u32 leaves) noexcept
{
    return in_tree(node, leaves) && node != leaves - 1U ? std::optional<u32>(sibling_of(node)) : std::nullopt;
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
        at     = parent_of(at);
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
        out[i] = sibling_of(at);
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

CryptoError mac(const CipherSuite& suite, cont::ConstSpan<u8> key, cont::ConstSpan<u8> message,
                cont::Span<u8> out) noexcept
{
    if (out.size() != suite.hash_bytes())
    {
        return refuse(CryptoError::BadLength, out);
    }
    return passed(kdf_mac(suite.kdf, key, message, out), out);
}

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

// ---- The key schedule ----

namespace
{
// A secret of at most Nh bytes on the stack, zeroed when it goes out of scope.
class Scratch
{
public:
    Scratch() noexcept = default;
    ~Scratch() noexcept { secure_zero(cont::Span<u8>(m_bytes, kMaxHashBytes)); }
    Scratch(const Scratch&)            = delete;
    Scratch& operator=(const Scratch&) = delete;
    Scratch(Scratch&&)                 = delete;
    Scratch& operator=(Scratch&&)      = delete;

    [[nodiscard]] cont::Span<u8> span(usize bytes) noexcept { return cont::Span<u8>(m_bytes, bytes); }

private:
    u8 m_bytes[kMaxHashBytes] = {};
};

// Whether a suite's hash and AEAD key fit what a tree and an epoch hold.
bool fits(const CipherSuite& suite) noexcept
{
    const usize hash = suite.hash_bytes();
    const usize key  = aead_key_bytes(suite.aead);
    return hash != 0U && hash <= kMaxHashBytes && key != 0U && key <= kMaxAeadKeyBytes;
}

// The epoch's secrets each DeriveSecret(epoch_secret, label) derives (section 8, table 4).
struct Derived
{
    EpochSecret      which;
    cont::StringView label;
};
constexpr Derived kDerived[] = {
    {EpochSecret::SenderData, "sender data"},  {EpochSecret::Encryption, "encryption"},
    {EpochSecret::Exporter, "exporter"},       {EpochSecret::External, "external"},
    {EpochSecret::Confirmation, "confirm"},    {EpochSecret::Membership, "membership"},
    {EpochSecret::Resumption, "resumption"},   {EpochSecret::Authentication, "authentication"},
    {EpochSecret::Init, "init"},
};
} // namespace

usize group_context_bytes(const GroupContext& context) noexcept
{
    const usize fields[] = {vector_bytes(context.group_id.size()), vector_bytes(context.tree_hash.size()),
                            vector_bytes(context.confirmed_transcript_hash.size()),
                            vector_bytes(context.extensions.size())};
    usize       bytes    = 2U + 2U + 8U; // version, cipher suite, epoch
    for (const usize field : fields)
    {
        if (field == 0U)
        {
            return 0U;
        }
        bytes += field;
    }
    return bytes;
}

void put_group_context(cont::WireWriter& out, const GroupContext& context) noexcept
{
    out.put_u16(kProtocolVersion);
    out.put_u16(context.cipher_suite);
    put_vector(out, context.group_id);
    out.put_u64(context.epoch);
    put_vector(out, context.tree_hash);
    put_vector(out, context.confirmed_transcript_hash);
    put_vector(out, context.extensions);
}

usize psk_id_bytes(const PreSharedKeyId& id) noexcept
{
    usize body = 0U;
    switch (id.type)
    {
    case PskType::External:
        body = vector_bytes(id.id.size());
        break;
    case PskType::Resumption:
    {
        const bool  usage_known = id.usage == ResumptionUsage::Application || id.usage == ResumptionUsage::Reinit ||
                                  id.usage == ResumptionUsage::Branch;
        const usize group       = vector_bytes(id.group_id.size());
        body                    = usage_known && group != 0U ? 1U + group + 8U : 0U;
        break;
    }
    }
    const usize nonce = vector_bytes(id.nonce.size());
    return body == 0U || nonce == 0U ? 0U : 1U + body + nonce;
}

void put_psk_id(cont::WireWriter& out, const PreSharedKeyId& id) noexcept
{
    out.put_u8(static_cast<u8>(id.type));
    if (id.type == PskType::External)
    {
        put_vector(out, id.id);
    }
    else
    {
        out.put_u8(static_cast<u8>(id.usage));
        put_vector(out, id.group_id);
        out.put_u64(id.epoch);
    }
    put_vector(out, id.nonce);
}

bool take_group_context(cont::WireReader& in, GroupContext& out) noexcept
{
    u16 version = 0U;
    out         = {};
    return in.take_u16(version) && version == kProtocolVersion && in.take_u16(out.cipher_suite) &&
           take_vector(in, out.group_id) && in.take_u64(out.epoch) && take_vector(in, out.tree_hash) &&
           take_vector(in, out.confirmed_transcript_hash) && take_vector(in, out.extensions);
}

bool take_psk_id(cont::WireReader& in, PreSharedKeyId& out) noexcept
{
    u8 type = 0U;
    if (!in.take_u8(type))
    {
        return false;
    }
    PreSharedKeyId id;
    if (type == static_cast<u8>(PskType::External))
    {
        id.type = PskType::External;
        if (!take_vector(in, id.id))
        {
            return false;
        }
    }
    else if (type == static_cast<u8>(PskType::Resumption))
    {
        u8 usage = 0U;
        if (!in.take_u8(usage) || usage < static_cast<u8>(ResumptionUsage::Application) ||
            usage > static_cast<u8>(ResumptionUsage::Branch) || !take_vector(in, id.group_id) ||
            !in.take_u64(id.epoch))
        {
            return false;
        }
        id.type  = PskType::Resumption;
        id.usage = static_cast<ResumptionUsage>(usage);
    }
    else
    {
        return false;
    }
    if (!take_vector(in, id.nonce))
    {
        return false;
    }
    out = id;
    return true;
}

CryptoError psk_secret(const CipherSuite& suite, cont::ConstSpan<PreSharedKey> psks, cont::Span<u8> out) noexcept
{
    const usize hash = suite.hash_bytes();
    if (!fits(suite) || out.size() != hash || psks.size() > 0xFFFFU)
    {
        return refuse(CryptoError::BadLength, out);
    }
    // psk_secret_[0] is Nh zero bytes; psk_secret_[i] = KDF.Extract(psk_input_[i-1], psk_secret_[i-1]).
    secure_zero(out);
    const u8 zero[kMaxHashBytes] = {};
    for (usize i = 0U; i < psks.size(); ++i)
    {
        const PreSharedKey& psk      = psks[i];
        const usize         id_bytes = psk_id_bytes(psk.id);
        if (id_bytes == 0U || psk.id.nonce.size() != hash)
        {
            return refuse(CryptoError::BadLength, out);
        }
        LabelBuffer      label(id_bytes + 4U); // PSKLabel: the identity, its index and the count
        cont::WireWriter writer(label.span());
        put_psk_id(writer, psk.id);
        writer.put_u16(static_cast<u16>(i));
        writer.put_u16(static_cast<u16>(psks.size()));
        Scratch     extracted;
        Scratch     input;
        Scratch     next;
        CryptoError error =
            hkdf_extract(suite.kdf, cont::ConstSpan<u8>(zero, hash), psk.secret, extracted.span(hash));
        if (error == CryptoError::None)
        {
            error = expand_with_label(suite, extracted.span(hash), "derived psk", label.span(), input.span(hash));
        }
        if (error == CryptoError::None)
        {
            error = hkdf_extract(suite.kdf, input.span(hash), out, next.span(hash));
        }
        if (error != CryptoError::None)
        {
            return refuse(error, out);
        }
        copy_bytes(next.span(hash), out.data());
    }
    return CryptoError::None;
}

EpochSecrets::~EpochSecrets() noexcept
{
    wipe();
}

void EpochSecrets::wipe() noexcept
{
    secure_zero(cont::Span<u8>(&m_secrets[0][0], sizeof(m_secrets)));
    m_hash_bytes = 0U;
}

void EpochSecrets::take(EpochSecrets& other) noexcept
{
    if (&other == this)
    {
        return;
    }
    wipe();
    copy_bytes(cont::ConstSpan<u8>(&other.m_secrets[0][0], sizeof(m_secrets)), &m_secrets[0][0]);
    m_hash_bytes = other.m_hash_bytes;
    other.wipe();
}

cont::ConstSpan<u8> EpochSecrets::get(EpochSecret which) const noexcept
{
    return cont::ConstSpan<u8>(m_secrets[static_cast<usize>(which)], m_hash_bytes);
}

CryptoError EpochSecrets::advance(const CipherSuite& suite, cont::ConstSpan<u8> init_secret,
                                  cont::ConstSpan<u8> commit_secret, cont::ConstSpan<u8> psk_secret,
                                  cont::ConstSpan<u8> group_context) noexcept
{
    const usize hash = suite.hash_bytes();
    if (!fits(suite) || init_secret.size() != hash || commit_secret.size() != hash)
    {
        wipe();
        return CryptoError::BadLength;
    }
    // The previous epoch's init secret may be this object's own: the joiner secret is derived before anything here
    // is replaced.
    Scratch     extracted;
    Scratch     joiner;
    CryptoError error = hkdf_extract(suite.kdf, init_secret, commit_secret, extracted.span(hash));
    if (error == CryptoError::None)
    {
        error = expand_with_label(suite, extracted.span(hash), "joiner", group_context, joiner.span(hash));
    }
    if (error != CryptoError::None)
    {
        wipe();
        return error;
    }
    return join(suite, joiner.span(hash), psk_secret, group_context);
}

CryptoError EpochSecrets::join(const CipherSuite& suite, cont::ConstSpan<u8> joiner_secret,
                               cont::ConstSpan<u8> psk_secret, cont::ConstSpan<u8> group_context) noexcept
{
    const usize hash = suite.hash_bytes();
    if (!fits(suite) || joiner_secret.size() != hash || psk_secret.size() != hash)
    {
        wipe();
        return CryptoError::BadLength;
    }
    Scratch joiner;
    Scratch member;
    Scratch epoch;
    copy_bytes(joiner_secret, joiner.span(hash).data());
    CryptoError error = hkdf_extract(suite.kdf, joiner_secret, psk_secret, member.span(hash));
    wipe();
    copy_bytes(joiner.span(hash), m_secrets[static_cast<usize>(EpochSecret::Joiner)]);
    const auto slot = [&](EpochSecret which) { return cont::Span<u8>(m_secrets[static_cast<usize>(which)], hash); };
    if (error == CryptoError::None)
    {
        error = derive_secret(suite, member.span(hash), "welcome", slot(EpochSecret::Welcome));
    }
    if (error == CryptoError::None)
    {
        error = expand_with_label(suite, member.span(hash), "epoch", group_context, epoch.span(hash));
    }
    for (const Derived& derived : kDerived)
    {
        if (error == CryptoError::None)
        {
            error = derive_secret(suite, epoch.span(hash), derived.label, slot(derived.which));
        }
    }
    if (error != CryptoError::None)
    {
        wipe();
        return error;
    }
    m_hash_bytes = hash;
    return CryptoError::None;
}

CryptoError welcome_secret(const CipherSuite& suite, cont::ConstSpan<u8> joiner_secret,
                           cont::ConstSpan<u8> psk_secret, cont::Span<u8> out) noexcept
{
    const usize hash = suite.hash_bytes();
    if (!fits(suite) || joiner_secret.size() != hash || psk_secret.size() != hash || out.size() != hash)
    {
        return refuse(CryptoError::BadLength, out);
    }
    Scratch           member;
    const CryptoError error = hkdf_extract(suite.kdf, joiner_secret, psk_secret, member.span(hash));
    return passed(error == CryptoError::None ? derive_secret(suite, member.span(hash), "welcome", out) : error, out);
}

CryptoError welcome_key(const CipherSuite& suite, cont::ConstSpan<u8> welcome_secret, cont::Span<u8> key,
                        cont::Span<u8> nonce) noexcept
{
    if (!fits(suite) || welcome_secret.size() != suite.hash_bytes() || key.size() != aead_key_bytes(suite.aead) ||
        nonce.size() != kAeadNonceBytes)
    {
        return refuse(CryptoError::BadLength, key, nonce);
    }
    CryptoError error = expand_with_label(suite, welcome_secret, "key", {}, key);
    if (error == CryptoError::None)
    {
        error = expand_with_label(suite, welcome_secret, "nonce", {}, nonce);
    }
    return error == CryptoError::None ? error : refuse(error, key, nonce);
}

CryptoError export_secret(const CipherSuite& suite, cont::ConstSpan<u8> exporter_secret, cont::StringView label,
                          cont::ConstSpan<u8> context, cont::Span<u8> out) noexcept
{
    const usize hash = suite.hash_bytes();
    if (!fits(suite))
    {
        return refuse(CryptoError::Unsupported, out);
    }
    Scratch     derived;
    Scratch     hashed;
    CryptoError error = derive_secret(suite, exporter_secret, label, derived.span(hash));
    if (error == CryptoError::None)
    {
        error = kdf_hash(suite.kdf, context, hashed.span(hash));
    }
    if (error == CryptoError::None)
    {
        error = expand_with_label(suite, derived.span(hash), "exported", hashed.span(hash), out);
    }
    return passed(error, out);
}

// ---- The secret tree ----

CryptoError sender_data_key(const CipherSuite& suite, cont::ConstSpan<u8> sender_data_secret,
                            cont::ConstSpan<u8> ciphertext, cont::Span<u8> key, cont::Span<u8> nonce) noexcept
{
    if (!fits(suite) || key.size() != aead_key_bytes(suite.aead) || nonce.size() != kAeadNonceBytes)
    {
        return refuse(CryptoError::BadLength, key, nonce);
    }
    const cont::ConstSpan<u8> sample = ciphertext.first(ciphertext.size() < suite.hash_bytes() ? ciphertext.size()
                                                                                                : suite.hash_bytes());
    CryptoError error = expand_with_label(suite, sender_data_secret, "key", sample, key);
    if (error == CryptoError::None)
    {
        error = expand_with_label(suite, sender_data_secret, "nonce", sample, nonce);
    }
    return error == CryptoError::None ? error : refuse(error, key, nonce);
}

namespace
{
constexpr u64 kSpent = u64{1} << 32U; // a ratchet's next generation once generation 2^32 - 1 is spent

cont::StringView ratchet_label(RatchetKind kind) noexcept
{
    return kind == RatchetKind::Handshake ? cont::StringView("handshake") : cont::StringView("application");
}

// The key and nonce of `generation` from its ratchet secret (section 9.1).
CryptoError message_key(const CipherSuite& suite, cont::ConstSpan<u8> secret, u32 generation, u8* key,
                        u8* nonce) noexcept
{
    const cont::Span<u8> key_out(key, aead_key_bytes(suite.aead));
    const cont::Span<u8> nonce_out(nonce, kAeadNonceBytes);
    CryptoError          error = derive_tree_secret(suite, secret, "key", generation, key_out);
    if (error == CryptoError::None)
    {
        error = derive_tree_secret(suite, secret, "nonce", generation, nonce_out);
    }
    return error == CryptoError::None ? error : refuse(error, key_out, nonce_out);
}

// Replaces the ratchet secret of `generation` by that of the next one.
CryptoError next_secret(const CipherSuite& suite, cont::Span<u8> secret, u32 generation) noexcept
{
    Scratch           next;
    const CryptoError error = derive_tree_secret(suite, secret, "secret", generation, next.span(secret.size()));
    if (error == CryptoError::None)
    {
        copy_bytes(next.span(secret.size()), secret.data());
    }
    return error;
}

template <typename T>
void zero_all(cont::Array<T>& items) noexcept
{
    secure_zero(cont::Span<u8>(reinterpret_cast<u8*>(items.data()), items.size() * sizeof(T)));
    items.clear();
}
} // namespace

MessageKey::~MessageKey() noexcept
{
    secure_zero(cont::Span<u8>(key, kMaxAeadKeyBytes));
    secure_zero(cont::Span<u8>(nonce, kAeadNonceBytes));
}

SecretTree::~SecretTree() noexcept
{
    wipe();
}

SecretTree::SecretTree(SecretTree&& other) noexcept
    : m_suite(other.m_suite), m_leaves(other.m_leaves), m_window(other.m_window), m_nodes(std::move(other.m_nodes)),
      m_ratchets(std::move(other.m_ratchets)), m_slots(std::move(other.m_slots))
{
    other.m_leaves = 0U;
}

SecretTree& SecretTree::operator=(SecretTree&& other) noexcept
{
    if (this != &other)
    {
        wipe();
        m_suite        = other.m_suite;
        m_leaves       = other.m_leaves;
        m_window       = other.m_window;
        m_nodes        = std::move(other.m_nodes);
        m_ratchets     = std::move(other.m_ratchets);
        m_slots        = std::move(other.m_slots);
        other.m_leaves = 0U;
    }
    return *this;
}

void SecretTree::wipe() noexcept
{
    zero_all(m_nodes);
    zero_all(m_ratchets);
    zero_all(m_slots);
    m_leaves = 0U;
}

CryptoError SecretTree::init(const CipherSuite& suite, cont::ConstSpan<u8> encryption_secret, u32 leaves,
                             RatchetWindow window) noexcept
{
    wipe();
    if (!fits(suite))
    {
        return CryptoError::Unsupported;
    }
    if (encryption_secret.size() != suite.hash_bytes() || !tree::valid_size(leaves) || leaves > kMaxSecretTreeLeaves ||
        window.out_of_order > kMaxOutOfOrder || window.max_forward > kMaxForward)
    {
        return CryptoError::BadLength;
    }
    const usize nodes    = tree::node_width(leaves);
    const usize ratchets = usize{2} * leaves;
    const usize slots    = ratchets * window.out_of_order;
    if (!m_nodes.try_reserve(nodes) || !m_ratchets.try_reserve(ratchets) || !m_slots.try_reserve(slots))
    {
        wipe();
        return CryptoError::ProviderFailure;
    }
    m_nodes.resize(nodes);
    m_ratchets.resize(ratchets);
    m_slots.resize(slots);
    Node& root = m_nodes[leaves - 1U]; // the root of a perfect tree
    copy_bytes(encryption_secret, root.secret);
    root.present = true;
    m_suite      = suite;
    m_leaves     = leaves;
    m_window     = window;
    return CryptoError::None;
}

CryptoError SecretTree::locate(u32 leaf, RatchetKind kind, u32 generation, usize& ratchet) const noexcept
{
    if (!ready())
    {
        return CryptoError::NotReady;
    }
    if (leaf >= m_leaves)
    {
        return CryptoError::OutOfWindow;
    }
    ratchet          = usize{2} * leaf + static_cast<usize>(kind);
    const Ratchet& r = m_ratchets[ratchet];
    const u64      next = r.live ? r.next : 0U;
    if (generation < next ? next - generation > m_window.out_of_order : generation - next > m_window.max_forward)
    {
        return CryptoError::OutOfWindow;
    }
    return CryptoError::None;
}

usize SecretTree::slot_index(usize ratchet, u32 generation) const noexcept
{
    if (m_window.out_of_order == 0U)
    {
        return m_slots.size();
    }
    const usize index = ratchet * m_window.out_of_order + generation % m_window.out_of_order;
    const Slot& slot  = m_slots[index];
    return slot.state != SlotState::Empty && slot.generation == generation ? index : m_slots.size();
}

CryptoError SecretTree::first_secret(u32 leaf, RatchetKind kind, cont::Span<u8> out) const noexcept
{
    const usize hash   = out.size();
    const u32   target = 2U * leaf;
    u32         at     = target;
    while (!m_nodes[at].present)
    {
        at = parent_of(at); // a leaf whose ratchets are not live has a holder on its path
    }
    Scratch secret;
    copy_bytes(cont::ConstSpan<u8>(m_nodes[at].secret, hash), secret.span(hash).data());
    while (at != target)
    {
        const bool        left  = target < at;
        Scratch           child;
        const CryptoError error = expand_with_label(m_suite, secret.span(hash), "tree",
                                                    left ? label_of("left") : label_of("right"), child.span(hash));
        if (error != CryptoError::None)
        {
            return refuse(error, out);
        }
        copy_bytes(child.span(hash), secret.span(hash).data());
        at = left ? left_of(at) : right_of(at);
    }
    return passed(expand_with_label(m_suite, secret.span(hash), ratchet_label(kind), {}, out), out);
}

CryptoError SecretTree::open_leaf(u32 leaf) noexcept
{
    const usize hash   = m_suite.hash_bytes();
    const u32   target = 2U * leaf;
    u32         at     = target;
    while (!m_nodes[at].present)
    {
        at = parent_of(at);
    }
    while (at != target)
    {
        Node&                     parent = m_nodes[at];
        const u32                 left   = left_of(at);
        const u32                 right  = right_of(at);
        const cont::ConstSpan<u8> secret(parent.secret, hash);
        CryptoError error = expand_with_label(m_suite, secret, "tree", label_of("left"),
                                              cont::Span<u8>(m_nodes[left].secret, hash));
        if (error == CryptoError::None)
        {
            error = expand_with_label(m_suite, secret, "tree", label_of("right"),
                                      cont::Span<u8>(m_nodes[right].secret, hash));
        }
        if (error != CryptoError::None)
        {
            return error;
        }
        m_nodes[left].present  = true;
        m_nodes[right].present = true;
        secure_zero(cont::Span<u8>(parent.secret, hash));
        parent.present = false;
        at             = target < at ? left : right;
    }
    Node& node = m_nodes[target];
    for (const RatchetKind kind : {RatchetKind::Handshake, RatchetKind::Application})
    {
        Ratchet&          r     = m_ratchets[usize{2} * leaf + static_cast<usize>(kind)];
        const CryptoError error = expand_with_label(m_suite, cont::ConstSpan<u8>(node.secret, hash),
                                                    ratchet_label(kind), {}, cont::Span<u8>(r.secret, hash));
        if (error != CryptoError::None)
        {
            return error;
        }
    }
    for (const usize ratchet : {usize{2} * leaf, usize{2} * leaf + 1U})
    {
        m_ratchets[ratchet].live = true;
        m_ratchets[ratchet].next = 0U;
    }
    secure_zero(cont::Span<u8>(node.secret, hash));
    node.present = false;
    return CryptoError::None;
}

CryptoError SecretTree::receive_key(u32 leaf, RatchetKind kind, u32 generation, MessageKey& out) const noexcept
{
    secure_zero(cont::Span<u8>(out.key, kMaxAeadKeyBytes));
    secure_zero(cont::Span<u8>(out.nonce, kAeadNonceBytes));
    out.key_length = 0U;
    out.generation = 0U;
    usize             ratchet = 0U;
    const CryptoError located = locate(leaf, kind, generation, ratchet);
    if (located != CryptoError::None)
    {
        return located;
    }
    const usize    hash = m_suite.hash_bytes();
    const usize    nk   = aead_key_bytes(m_suite.aead);
    const Ratchet& r    = m_ratchets[ratchet];
    const u64      next = r.live ? r.next : 0U;
    if (generation < next)
    {
        const usize index = slot_index(ratchet, generation);
        if (index == m_slots.size())
        {
            return CryptoError::OutOfWindow;
        }
        const Slot& slot = m_slots[index];
        if (slot.state == SlotState::Spent)
        {
            return CryptoError::Replayed;
        }
        copy_bytes(cont::ConstSpan<u8>(slot.key, nk), out.key);
        copy_bytes(cont::ConstSpan<u8>(slot.nonce, kAeadNonceBytes), out.nonce);
    }
    else
    {
        Scratch     secret;
        CryptoError error = CryptoError::None;
        if (r.live)
        {
            copy_bytes(cont::ConstSpan<u8>(r.secret, hash), secret.span(hash).data());
        }
        else
        {
            error = first_secret(leaf, kind, secret.span(hash));
        }
        for (u64 g = next; g < generation && error == CryptoError::None; ++g)
        {
            error = next_secret(m_suite, secret.span(hash), static_cast<u32>(g));
        }
        if (error == CryptoError::None)
        {
            error = message_key(m_suite, secret.span(hash), generation, out.key, out.nonce);
        }
        if (error != CryptoError::None)
        {
            return error;
        }
    }
    out.key_length = nk;
    out.generation = generation;
    return CryptoError::None;
}

CryptoError SecretTree::consume(u32 leaf, RatchetKind kind, u32 generation) noexcept
{
    usize             ratchet = 0U;
    const CryptoError located = locate(leaf, kind, generation, ratchet);
    if (located != CryptoError::None)
    {
        return located;
    }
    const u32 window = m_window.out_of_order;
    Ratchet&  r      = m_ratchets[ratchet];
    if (generation < (r.live ? r.next : 0U))
    {
        const usize index = slot_index(ratchet, generation);
        if (index == m_slots.size())
        {
            return CryptoError::OutOfWindow;
        }
        Slot& slot = m_slots[index];
        if (slot.state == SlotState::Spent)
        {
            return CryptoError::Replayed;
        }
        slot.state = SlotState::Spent;
        secure_zero(cont::Span<u8>(slot.key, kMaxAeadKeyBytes));
        secure_zero(cont::Span<u8>(slot.nonce, kAeadNonceBytes));
        return CryptoError::None;
    }
    if (!r.live)
    {
        const CryptoError opened = open_leaf(leaf);
        if (opened != CryptoError::None)
        {
            return opened;
        }
    }
    // After this the window holds generations generation + 1 - window to generation: each generation passed on the
    // way that falls inside it takes its slot (every older occupant is outside the new window), retained when skipped
    // and spent when it is the one consumed.
    const usize hash = m_suite.hash_bytes();
    const u64   kept = u64{generation} + 1U > window ? u64{generation} + 1U - window : 0U;
    for (u64 g = r.next; g <= generation; ++g)
    {
        const u32 at = static_cast<u32>(g);
        if (window != 0U && g >= kept)
        {
            Slot& slot      = m_slots[ratchet * window + at % window];
            slot.generation = at;
            if (at == generation)
            {
                slot.state = SlotState::Spent;
                secure_zero(cont::Span<u8>(slot.key, kMaxAeadKeyBytes));
                secure_zero(cont::Span<u8>(slot.nonce, kAeadNonceBytes));
            }
            else
            {
                const CryptoError error =
                    message_key(m_suite, cont::ConstSpan<u8>(r.secret, hash), at, slot.key, slot.nonce);
                if (error != CryptoError::None)
                {
                    slot.state = SlotState::Empty;
                    return error;
                }
                slot.state = SlotState::Retained;
            }
        }
        const CryptoError error = next_secret(m_suite, cont::Span<u8>(r.secret, hash), at);
        if (error != CryptoError::None)
        {
            return error;
        }
        r.next = g + 1U;
    }
    if (r.next == kSpent)
    {
        secure_zero(cont::Span<u8>(r.secret, hash));
    }
    return CryptoError::None;
}

CryptoError SecretTree::send_key(u32 leaf, RatchetKind kind, MessageKey& out) noexcept
{
    secure_zero(cont::Span<u8>(out.key, kMaxAeadKeyBytes));
    secure_zero(cont::Span<u8>(out.nonce, kAeadNonceBytes));
    out.key_length = 0U;
    out.generation = 0U;
    if (!ready())
    {
        return CryptoError::NotReady;
    }
    if (leaf >= m_leaves)
    {
        return CryptoError::OutOfWindow;
    }
    const usize ratchet = usize{2} * leaf + static_cast<usize>(kind);
    if (!m_ratchets[ratchet].live)
    {
        const CryptoError opened = open_leaf(leaf);
        if (opened != CryptoError::None)
        {
            return opened;
        }
    }
    Ratchet& r = m_ratchets[ratchet];
    if (r.next == kSpent)
    {
        return CryptoError::Exhausted;
    }
    const usize hash       = m_suite.hash_bytes();
    const u32   generation = static_cast<u32>(r.next);
    CryptoError error      = message_key(m_suite, cont::ConstSpan<u8>(r.secret, hash), generation, out.key, out.nonce);
    if (error == CryptoError::None)
    {
        error = next_secret(m_suite, cont::Span<u8>(r.secret, hash), generation);
    }
    if (error != CryptoError::None)
    {
        secure_zero(cont::Span<u8>(out.key, kMaxAeadKeyBytes));
        secure_zero(cont::Span<u8>(out.nonce, kAeadNonceBytes));
        return error;
    }
    r.next = u64{generation} + 1U;
    if (r.next == kSpent)
    {
        secure_zero(cont::Span<u8>(r.secret, hash));
    }
    out.key_length = aead_key_bytes(m_suite.aead);
    out.generation = generation;
    return CryptoError::None;
}
} // namespace crd::crypto::mls
