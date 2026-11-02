// crd-fuzz-crypto-mls: MLS's presentation language, tree math, labelled operations and secret tree and the HPKE
// beneath them (NET.2; docs/design/network-foundation.md#mls and #hpke) under the bounded fuzz harness (REPO.DEV.9;
// docs/design/test-instruments.md).
//
// Input. The first byte picks the mode, the rest is its data, read field by field (a field's length is one byte
// unless noted; a short input ends the case):
//   0  headers: the bytes a decoder reads as a vector header, a vector and a presence octet;
//   1  tree math: the size's exponent (or, above 31, a raw 32-bit size) and two nodes (u32 each);
//   2  HPKE: the suite (its KEM, any of the profile's eight, its KDF and its AEAD; hpke_suite_of), the recipient's
//      input keying material, the index of a bit to change (u16; past the end changes none), the info, the
//      associated data, then the plaintext;
//   3  hostile openings: the suite, the recipient's input keying material, the encapsulation, the label, the
//      context, then the ciphertext;
//   4  hostile signatures: the suite, the public key, the signature (u16 lengths each), the label, then the content;
//   5  derivations: the suite, the secret, the label, the context, the generation (u32) and the length (u16);
//   6  the secret tree: the suite, the leaf count's exponent (mod 5), the window (out of order mod 9, ahead mod 48)
//      and the root secret's fill byte, then up to 128 operations of three bytes: the leaf (mod leaves + 1, one past
//      the tree), the action (mod 3: read, read and spend, send; bit 2 picks the application ratchet) and the
//      generation.
//
// Oracles, none of them the code's verdict on the same call:
//   * a header is accepted exactly when a restatement of RFC 9420 section 2.1.2 accepts it, with the same value, and
//     writes back to the bytes it was read from; a presence octet exactly when it is 0 or 1;
//   * tree math agrees with itself: a parent's children hold the node, siblings share their parent, a direct path
//     ends at the root, a common ancestor lies on both nodes' paths, and an invalid size or node has no answer;
//   * an honest HPKE message opens to its plaintext and a message with any bit changed is refused with no byte
//     released;
//   * a hostile opening is refused, with the output zeroed; a hostile signature's verdict is deterministic, never
//     Unsupported, and Invalid for every length the suite's algorithm cannot have;
//   * a derivation is deterministic, DeriveTreeSecret is ExpandWithLabel of the big-endian generation, and the
//     length is part of the label (a shorter output is not the prefix of a longer one).
//   * a secret tree answers each read with the verdict a model of section 9.2 and the window gives (late, spent,
//     dropped, too far ahead, outside the tree) and with the key of a restatement of section 9 walked from the root;
//     a read changes nothing, a spend moves the model, a send hands out the sender's generations in turn, and every
//     refusal clears its key.
#include <crd/fuzz/harness.hpp>

#include <crd/crypto/hpke.hpp>
#include <crd/crypto/mls.hpp>

#include <algorithm>
#include <cstring>
#include <optional>

namespace
{
using crd::u16;
using crd::u32;
using crd::u8;
using crd::usize;
using crd::containers::ConstSpan;
using crd::containers::Span;
using crd::containers::StringView;
using crd::containers::WireReader;
using crd::containers::WireWriter;
using namespace crd::crypto;
namespace mls  = crd::crypto::mls;
namespace tree = crd::crypto::mls::tree;

// Reads the input field by field; a field past the end leaves the reader failed.
class Reader
{
public:
    Reader(const u8* data, usize size) noexcept : m_data(data), m_size(size) {}

    [[nodiscard]] bool ok() const noexcept { return m_ok; }

    u8 byte() noexcept
    {
        if (m_at >= m_size)
        {
            m_ok = false;
            return 0U;
        }
        return m_data[m_at++];
    }

    u16 u16be() noexcept { return static_cast<u16>(static_cast<u16>(byte() << 8U) | byte()); }
    u32 u32be() noexcept { return static_cast<u32>(u16be()) << 16U | u16be(); }

    ConstSpan<u8> bytes(usize n) noexcept
    {
        if (n > m_size - m_at)
        {
            m_ok = false;
            return {};
        }
        const ConstSpan<u8> out(m_data + m_at, n);
        m_at += n;
        return out;
    }

    ConstSpan<u8> rest() noexcept { return bytes(m_size - m_at); }

private:
    const u8* m_data;
    usize     m_size;
    usize     m_at = 0U;
    bool      m_ok = true;
};

bool all_zero(ConstSpan<u8> bytes) noexcept
{
    return std::ranges::all_of(bytes, [](u8 b) { return b == 0U; });
}

bool same(ConstSpan<u8> a, ConstSpan<u8> b) noexcept
{
    return a.size() == b.size() && (a.empty() || std::memcmp(a.data(), b.data(), a.size()) == 0);
}

StringView text_of(ConstSpan<u8> bytes) noexcept
{
    return StringView(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

const mls::CipherSuite& suite_of(u8 selector) noexcept
{
    const ConstSpan<mls::CipherSuite> suites = mls::cipher_suites();
    return suites[selector % suites.size()];
}

// RFC 9420 section 2.1.2 restated: the value, or false for a short input, the prefix 0b11 and a non-minimal form.
bool restated_varint(ConstSpan<u8> in, usize& value, usize& length) noexcept
{
    if (in.empty() || in[0] >= 0xC0U)
    {
        return false;
    }
    // The two high bits are the base 2 logarithm of the length; the smallest value of each length is the first that
    // the shorter one cannot hold.
    constexpr usize lengths[]  = {1U, 2U, 4U};
    constexpr usize smallest[] = {0U, 64U, 16384U};
    const usize     prefix     = in[0] >> 6U;
    length                     = lengths[prefix];
    if (in.size() < length)
    {
        return false;
    }
    value = in[0] & 0x3FU;
    for (usize i = 1U; i < length; ++i)
    {
        value = value * 256U + in[i];
    }
    return value >= smallest[prefix];
}

void headers(Reader& in)
{
    const ConstSpan<u8> input = in.rest();
    usize               expected = 0U;
    usize               length   = 0U;
    const bool          valid    = restated_varint(input, expected, length);
    WireReader          reader(input);
    usize               value = 0U;
    CRD_FUZZ_REQUIRE(mls::take_varint(reader, value) == valid);
    if (valid)
    {
        CRD_FUZZ_REQUIRE(value == expected && reader.consumed() == length);
        CRD_FUZZ_REQUIRE(mls::varint_bytes(value) == length);
        u8         again[4] = {};
        WireWriter out(Span<u8>(again, length));
        mls::put_varint(out, value);
        CRD_FUZZ_REQUIRE(same(ConstSpan<u8>(again, length), input.subspan(0U, length)));
        WireReader    vectors(input);
        ConstSpan<u8> body;
        const bool    whole = input.size() - length >= value;
        CRD_FUZZ_REQUIRE(mls::take_vector(vectors, body) == whole);
        if (whole)
        {
            CRD_FUZZ_REQUIRE(body.data() == input.data() + length && body.size() == value);
        }
    }
    WireReader presence(input);
    bool       present = false;
    const bool octet   = !input.empty() && input[0] <= 1U;
    CRD_FUZZ_REQUIRE(mls::take_presence(presence, present) == octet);
    if (octet)
    {
        CRD_FUZZ_REQUIRE(present == (input[0] == 1U));
    }
}

void tree_math(Reader& in)
{
    const u8  exponent = in.byte();
    const u32 raw      = exponent > 31U ? in.u32be() : 0U;
    const u32 a        = in.u32be();
    const u32 b        = in.u32be();
    if (!in.ok())
    {
        return;
    }
    const u32 leaves = exponent > 31U ? raw : u32{1} << exponent;
    const u32 width  = tree::node_width(leaves);
    if (!tree::valid_size(leaves))
    {
        CRD_FUZZ_REQUIRE(width == 0U && !tree::root(leaves) && !tree::parent(a, leaves) && !tree::left(a, leaves));
        return;
    }
    CRD_FUZZ_REQUIRE(width == 2U * leaves - 1U && *tree::root(leaves) == leaves - 1U);
    const u32 node  = a % width;
    const u32 other = b % width;
    if (a >= width)
    {
        CRD_FUZZ_REQUIRE(!tree::parent(a, leaves) && !tree::sibling(a, leaves) && !tree::right(a, leaves));
    }
    if (const std::optional<u32> up = tree::parent(node, leaves))
    {
        CRD_FUZZ_REQUIRE(tree::level(*up) == tree::level(node) + 1U);
        CRD_FUZZ_REQUIRE(tree::left(*up, leaves) == node || tree::right(*up, leaves) == node);
        const std::optional<u32> brother = tree::sibling(node, leaves);
        CRD_FUZZ_REQUIRE(brother && *brother != node && tree::parent(*brother, leaves) == up);
    }
    else
    {
        CRD_FUZZ_REQUIRE(node == leaves - 1U);
    }
    u32         path[tree::kMaxDepth] = {};
    u32         co[tree::kMaxDepth]   = {};
    const usize depth                 = tree::direct_path(node, leaves, path);
    CRD_FUZZ_REQUIRE(tree::copath(node, leaves, co) == depth);
    CRD_FUZZ_REQUIRE(depth == 0U ? node == leaves - 1U : path[depth - 1U] == leaves - 1U);
    const std::optional<u32> ancestor = tree::common_ancestor(node, other, leaves);
    CRD_FUZZ_REQUIRE(ancestor.has_value());
    u32         other_path[tree::kMaxDepth] = {};
    const usize other_depth                 = tree::direct_path(other, leaves, other_path);
    const auto  on = [&](const u32* nodes, usize count, u32 self)
    { return *ancestor == self || std::find(nodes, nodes + count, *ancestor) != nodes + count; };
    CRD_FUZZ_REQUIRE(on(path, depth, node) && on(other_path, other_depth, other));
}

// Every HPKE suite of the profile from one byte: its KEM (the DHKEMs, ML-KEM and the hybrids), KDF and AEAD.
HpkeSuite hpke_suite_of(u8 selector) noexcept
{
    constexpr AlgorithmId kems[]  = {AlgorithmId::X25519,         AlgorithmId::Secp256r1,
                                     AlgorithmId::Secp384r1,      AlgorithmId::MlKem768,
                                     AlgorithmId::MlKem1024,      AlgorithmId::X25519Mlkem768,
                                     AlgorithmId::Secp256r1Mlkem768,
                                     AlgorithmId::Secp384r1Mlkem1024};
    constexpr AlgorithmId kdfs[]  = {AlgorithmId::HkdfSha256, AlgorithmId::HkdfSha384};
    constexpr AlgorithmId aeads[] = {AlgorithmId::Aes128Gcm, AlgorithmId::Aes256Gcm, AlgorithmId::Chacha20Poly1305};
    return {kems[selector % 8U], kdfs[(selector / 8U) % 2U], aeads[(selector / 16U) % 3U]};
}

void hpke_round_trip(Reader& in)
{
    const HpkeSuite     suite = hpke_suite_of(in.byte());
    const ConstSpan<u8> ikm   = in.bytes(in.byte());
    const usize         bit   = in.u16be();
    const ConstSpan<u8> info  = in.bytes(in.byte());
    const ConstSpan<u8> aad   = in.bytes(in.byte());
    const ConstSpan<u8> text  = in.rest();
    if (!in.ok())
    {
        return;
    }
    // DeriveKeyPair's floor: Nsk for a DHKEM (a classical group), 32 bytes for ML-KEM and the hybrids (decision H2).
    const HpkeSizes   sizes   = hpke_sizes(suite.kem);
    const bool        dhkem   = algorithm_class(suite.kem) == AlgorithmClass::ClassicalKeyExchange;
    const usize       floor   = dhkem ? sizes.private_key : 32U;
    HpkeKey           recipient;
    const CryptoError derived = recipient.derive(suite.kem, ikm);
    if (ikm.size() < floor)
    {
        CRD_FUZZ_REQUIRE(derived == CryptoError::BadLength && !recipient.ready());
        return;
    }
    CRD_FUZZ_REQUIRE(derived == CryptoError::None);
    // The longest public key and encapsulation of the profile: MLKEM1024-P384's 1665 bytes each.
    constexpr usize max_kem_bytes = 1665U;
    u8              recipient_public[max_kem_bytes];
    CRD_FUZZ_REQUIRE(recipient.public_key(Span<u8>(recipient_public, sizes.public_key)) == CryptoError::None);
    CRD_FUZZ_REQUIRE(hpke_check_public_key(suite.kem, ConstSpan<u8>(recipient_public, sizes.public_key)) ==
                     CryptoError::None);
    // enc || ciphertext in one buffer, so one bit index reaches either.
    static u8 sealed[crd::fuzz::kMaxInputBytes + max_kem_bytes + kHpkeTagBytes];
    static u8 opened[crd::fuzz::kMaxInputBytes];
    const Span<u8> enc(sealed, sizes.encapsulation);
    const Span<u8> ciphertext(sealed + sizes.encapsulation, text.size() + kHpkeTagBytes);
    CRD_FUZZ_REQUIRE(hpke_seal(suite, ConstSpan<u8>(recipient_public, sizes.public_key), info, aad, text, enc,
                               ciphertext) == CryptoError::None);
    const bool changed = bit < 8U * (enc.size() + ciphertext.size());
    if (changed)
    {
        sealed[bit / 8U] ^= static_cast<u8>(1U << (bit % 8U));
    }
    std::memset(opened, 0xA5, text.size());
    const Span<u8>    out(opened, text.size());
    const CryptoError result = recipient.open(suite, enc, info, aad, ciphertext, out);
    if (!changed)
    {
        CRD_FUZZ_REQUIRE(result == CryptoError::None && same(out, text));
        return;
    }
    CRD_FUZZ_REQUIRE(result == CryptoError::Forged || result == CryptoError::BadKey ||
                     result == CryptoError::WeakSecret);
    CRD_FUZZ_REQUIRE(all_zero(out));
}

void hostile_open(Reader& in)
{
    const mls::CipherSuite& suite      = suite_of(in.byte());
    const ConstSpan<u8>     ikm        = in.bytes(in.byte());
    const ConstSpan<u8>     enc        = in.bytes(in.byte());
    const ConstSpan<u8>     label      = in.bytes(in.byte());
    const ConstSpan<u8>     context    = in.bytes(in.byte());
    const ConstSpan<u8>     ciphertext = in.rest();
    if (!in.ok())
    {
        return;
    }
    HpkeKey key;
    if (key.derive(suite.kem, ikm) != CryptoError::None)
    {
        return;
    }
    static u8 opened[crd::fuzz::kMaxInputBytes];
    const usize length = ciphertext.size() >= kHpkeTagBytes ? ciphertext.size() - kHpkeTagBytes : 0U;
    std::memset(opened, 0xA5, length);
    const Span<u8>    out(opened, length);
    const CryptoError result =
        mls::decrypt_with_label(suite, key, text_of(label), context, enc, ciphertext, out);
    if (enc.size() != hpke_sizes(suite.kem).public_key || ciphertext.size() < kHpkeTagBytes)
    {
        CRD_FUZZ_REQUIRE(result == CryptoError::BadLength);
    }
    else
    {
        CRD_FUZZ_REQUIRE(result == CryptoError::Forged || result == CryptoError::BadKey ||
                         result == CryptoError::WeakSecret);
    }
    CRD_FUZZ_REQUIRE(all_zero(out));
}

void hostile_signature(Reader& in)
{
    const mls::CipherSuite& suite     = suite_of(in.byte());
    const ConstSpan<u8>     key       = in.bytes(in.u16be());
    const ConstSpan<u8>     signature = in.bytes(in.u16be());
    const ConstSpan<u8>     label     = in.bytes(in.byte());
    const ConstSpan<u8>     content   = in.rest();
    if (!in.ok())
    {
        return;
    }
    // Deterministic and never Unsupported; Invalid for every key or signature of a length the suite's algorithm never
    // has (a crafted small-order Ed25519 key can verify, so validity itself is the provider fuzz target's).
    const ProviderVerifier verifier;
    const StringView       name  = text_of(label);
    const Verification     first = mls::verify_with_label(suite, verifier, key, name, content, signature);
    CRD_FUZZ_REQUIRE(first != Verification::Unsupported);
    CRD_FUZZ_REQUIRE(mls::verify_with_label(suite, verifier, key, name, content, signature) == first);
    const SignatureSizes sizes = signature_sizes(suite.signature);
    if (key.size() != sizes.public_key || signature.empty() || signature.size() > sizes.signature)
    {
        CRD_FUZZ_REQUIRE(first == Verification::Invalid);
    }
}

void derivations(Reader& in)
{
    const mls::CipherSuite& suite      = suite_of(in.byte());
    const ConstSpan<u8>     secret     = in.bytes(in.byte());
    const ConstSpan<u8>     label      = in.bytes(in.byte());
    const ConstSpan<u8>     context    = in.bytes(in.byte());
    const u32               generation = in.u32be();
    const usize             length     = in.u16be();
    if (!in.ok())
    {
        return;
    }
    static u8         a[65536];
    static u8         b[65536];
    const Span<u8>    out(a, length);
    const CryptoError first = mls::expand_with_label(suite, secret, text_of(label), context, out);
    CRD_FUZZ_REQUIRE(mls::expand_with_label(suite, secret, text_of(label), context, Span<u8>(b, length)) == first);
    const bool valid = length != 0U && length <= 255U * suite.hash_bytes() && secret.size() >= suite.hash_bytes();
    CRD_FUZZ_REQUIRE((first == CryptoError::None) == valid);
    if (!valid)
    {
        CRD_FUZZ_REQUIRE(first == CryptoError::BadLength && all_zero(out));
        return;
    }
    CRD_FUZZ_REQUIRE(same(out, ConstSpan<u8>(b, length)));
    if (length > 1U)
    {
        CRD_FUZZ_REQUIRE(mls::expand_with_label(suite, secret, text_of(label), context, Span<u8>(b, length - 1U)) ==
                         CryptoError::None);
        CRD_FUZZ_REQUIRE(!same(ConstSpan<u8>(a, length - 1U), ConstSpan<u8>(b, length - 1U)));
    }
    const u8 be[4] = {static_cast<u8>(generation >> 24U), static_cast<u8>(generation >> 16U),
                      static_cast<u8>(generation >> 8U), static_cast<u8>(generation)};
    CRD_FUZZ_REQUIRE(mls::derive_tree_secret(suite, secret, text_of(label), generation, out) == CryptoError::None);
    CRD_FUZZ_REQUIRE(mls::expand_with_label(suite, secret, text_of(label), be, Span<u8>(b, length)) ==
                     CryptoError::None);
    CRD_FUZZ_REQUIRE(same(out, ConstSpan<u8>(b, length)));
}

// ---- The secret tree: a sequence of reads, spends and sends against section 9 restated ----

constexpr u32   kFuzzLeaves      = 16U;
constexpr u32   kFuzzGenerations = 256U; // a received generation is one byte; a send is one per operation
constexpr usize kFuzzOperations  = 128U;

// One ratchet's keys by generation, derived on demand from its generation-0 secret: the tree walked from its root
// by "tree" "left" and "right", the leaf's "handshake" or "application" expansion, then "secret", "key" and "nonce".
struct ReferenceRatchet
{
    u8   secret[mls::kMaxHashBytes] = {}; // the ratchet secret of generation `derived`
    u32  derived                    = 0U;
    bool started                    = false;
    u8   keys[kFuzzGenerations][mls::kMaxAeadKeyBytes + kAeadNonceBytes];
};

// What a receiver of one ratchet may answer: the next unconsumed generation and, below it, which generations are kept.
struct ModelRatchet
{
    u32  next = 0U;
    bool retained[kFuzzGenerations];
    bool spent[kFuzzGenerations];
    u32  sent = 0U;
};

ReferenceRatchet g_reference[2U * kFuzzLeaves];
ModelRatchet     g_model[2U * kFuzzLeaves];

ConstSpan<u8> reference_key(const mls::CipherSuite& suite, ConstSpan<u8> root, u32 leaves, u32 leaf,
                            mls::RatchetKind kind, u32 generation) noexcept
{
    const usize       hash = suite.hash_bytes();
    const usize       nk   = aead_key_bytes(suite.aead);
    ReferenceRatchet& r    = g_reference[2U * leaf + static_cast<u32>(kind)];
    if (!r.started)
    {
        u8 node_secret[mls::kMaxHashBytes] = {};
        std::memcpy(node_secret, root.data(), hash);
        u32       node   = *tree::root(leaves);
        const u32 target = 2U * leaf;
        while (node != target)
        {
            const bool          left = target < node;
            const ConstSpan<u8> side(reinterpret_cast<const u8*>(left ? "left" : "right"), left ? 4U : 5U);
            u8                  child[mls::kMaxHashBytes] = {};
            CRD_FUZZ_REQUIRE(mls::expand_with_label(suite, ConstSpan<u8>(node_secret, hash), "tree", side,
                                                    Span<u8>(child, hash)) == CryptoError::None);
            std::memcpy(node_secret, child, hash);
            node = left ? *tree::left(node, leaves) : *tree::right(node, leaves);
        }
        CRD_FUZZ_REQUIRE(mls::expand_with_label(suite, ConstSpan<u8>(node_secret, hash),
                                                kind == mls::RatchetKind::Handshake ? "handshake" : "application", {},
                                                Span<u8>(r.secret, hash)) == CryptoError::None);
        r.started = true;
        r.derived = 0U;
    }
    while (r.derived <= generation)
    {
        u8* const key = r.keys[r.derived];
        CRD_FUZZ_REQUIRE(mls::derive_tree_secret(suite, ConstSpan<u8>(r.secret, hash), "key", r.derived,
                                                 Span<u8>(key, nk)) == CryptoError::None);
        CRD_FUZZ_REQUIRE(mls::derive_tree_secret(suite, ConstSpan<u8>(r.secret, hash), "nonce", r.derived,
                                                 Span<u8>(key + nk, kAeadNonceBytes)) == CryptoError::None);
        u8 next[mls::kMaxHashBytes] = {};
        CRD_FUZZ_REQUIRE(mls::derive_tree_secret(suite, ConstSpan<u8>(r.secret, hash), "secret", r.derived,
                                                 Span<u8>(next, hash)) == CryptoError::None);
        std::memcpy(r.secret, next, hash);
        ++r.derived;
    }
    return ConstSpan<u8>(r.keys[generation], nk + kAeadNonceBytes);
}

bool key_is(const mls::MessageKey& key, ConstSpan<u8> expected, u32 generation) noexcept
{
    const usize nk = key.key_length;
    return key.generation == generation && nk + kAeadNonceBytes == expected.size() &&
           same(key.aead_key(), expected.first(nk)) &&
           same(ConstSpan<u8>(key.nonce, kAeadNonceBytes), expected.subspan(nk));
}

bool key_cleared(const mls::MessageKey& key) noexcept
{
    return key.key_length == 0U && key.generation == 0U && all_zero(ConstSpan<u8>(key.key, mls::kMaxAeadKeyBytes)) &&
           all_zero(ConstSpan<u8>(key.nonce, kAeadNonceBytes));
}

// The verdict of receiving `generation` from the model alone (section 9.2 and decision M5 restated).
CryptoError expected_receive(const ModelRatchet& model, mls::RatchetWindow window, u32 generation) noexcept
{
    if (generation >= model.next)
    {
        return generation - model.next > window.max_forward ? CryptoError::OutOfWindow : CryptoError::None;
    }
    if (model.next - generation > window.out_of_order)
    {
        return CryptoError::OutOfWindow;
    }
    if (model.retained[generation])
    {
        return CryptoError::None;
    }
    return model.spent[generation] ? CryptoError::Replayed : CryptoError::OutOfWindow;
}

void spend(ModelRatchet& model, mls::RatchetWindow window, u32 generation) noexcept
{
    if (generation < model.next)
    {
        model.retained[generation] = false;
        model.spent[generation]    = true;
        return;
    }
    for (u32 skipped = model.next; skipped < generation; ++skipped)
    {
        // Kept when the window after this spend still reaches it.
        model.retained[skipped] = generation + 1U - skipped <= window.out_of_order;
        model.spent[skipped]    = false;
    }
    model.retained[generation] = false;
    model.spent[generation]    = true;
    model.next                 = generation + 1U;
}

void secret_tree(Reader& in)
{
    const mls::CipherSuite&  suite  = suite_of(in.byte());
    const u32                leaves = u32{1} << (in.byte() % 5U);
    const mls::RatchetWindow window{in.byte() % 9U, in.byte() % 48U};
    const u8                 fill = in.byte();
    if (!in.ok())
    {
        return;
    }
    u8 root[mls::kMaxHashBytes];
    std::memset(root, fill, sizeof(root));
    const ConstSpan<u8> secret(root, suite.hash_bytes());
    mls::SecretTree     receiver;
    mls::SecretTree     sender;
    CRD_FUZZ_REQUIRE(receiver.init(suite, secret, leaves, window) == CryptoError::None);
    CRD_FUZZ_REQUIRE(sender.init(suite, secret, leaves) == CryptoError::None);
    for (ReferenceRatchet& r : g_reference)
    {
        r.started = false;
    }
    for (ModelRatchet& m : g_model)
    {
        m.next = 0U;
        m.sent = 0U;
        std::memset(m.retained, 0, sizeof(m.retained));
        std::memset(m.spent, 0, sizeof(m.spent));
    }
    for (usize op = 0U; op < kFuzzOperations; ++op)
    {
        const u8  leaf_byte  = in.byte();
        const u8  action     = in.byte();
        const u32 generation = in.byte();
        if (!in.ok())
        {
            return;
        }
        const u32              leaf = leaf_byte % (leaves + 1U); // one past the tree too
        const mls::RatchetKind kind = (action & 4U) != 0U ? mls::RatchetKind::Application : mls::RatchetKind::Handshake;
        mls::MessageKey        key;
        if (leaf == leaves)
        {
            CRD_FUZZ_REQUIRE(receiver.receive_key(leaf, kind, generation, key) == CryptoError::OutOfWindow);
            CRD_FUZZ_REQUIRE(receiver.consume(leaf, kind, generation) == CryptoError::OutOfWindow);
            CRD_FUZZ_REQUIRE(sender.send_key(leaf, kind, key) == CryptoError::OutOfWindow && key_cleared(key));
            continue;
        }
        ModelRatchet& model = g_model[2U * leaf + static_cast<u32>(kind)];
        if (action % 3U == 2U)
        {
            CRD_FUZZ_REQUIRE(sender.send_key(leaf, kind, key) == CryptoError::None);
            CRD_FUZZ_REQUIRE(key_is(key, reference_key(suite, secret, leaves, leaf, kind, model.sent), model.sent));
            ++model.sent;
            continue;
        }
        // A read changes nothing: twice the same verdict and key.
        const CryptoError verdict = expected_receive(model, window, generation);
        for (int read = 0; read < 2; ++read)
        {
            CRD_FUZZ_REQUIRE(receiver.receive_key(leaf, kind, generation, key) == verdict);
            CRD_FUZZ_REQUIRE(verdict == CryptoError::None
                                 ? key_is(key, reference_key(suite, secret, leaves, leaf, kind, generation), generation)
                                 : key_cleared(key));
        }
        if (action % 3U == 1U)
        {
            CRD_FUZZ_REQUIRE(receiver.consume(leaf, kind, generation) == verdict);
            if (verdict == CryptoError::None)
            {
                spend(model, window, generation);
            }
        }
    }
}

void add(crd::fuzz::SeedSink& sink, const char* name, const u8* data, usize size)
{
    sink.add(name, data, size);
}
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    if (size == 0U || size > crd::fuzz::kMaxInputBytes)
    {
        return 0;
    }
    Reader in(data + 1U, size - 1U);
    switch (data[0] % 7U)
    {
    case 0U:
        headers(in);
        break;
    case 1U:
        tree_math(in);
        break;
    case 2U:
        hpke_round_trip(in);
        break;
    case 3U:
        hostile_open(in);
        break;
    case 4U:
        hostile_signature(in);
        break;
    case 5U:
        derivations(in);
        break;
    default:
        secret_tree(in);
        break;
    }
    return 0;
}

void crd_fuzz_seeds(crd::fuzz::SeedSink& sink)
{
    // Headers: the deserialization vectors' four-byte maximum, a non-minimal two-byte zero and the prefix 0b11.
    const u8 longest[] = {0U, 0xBFU, 0xFFU, 0xFFU, 0xFFU};
    add(sink, "header-longest", longest, sizeof(longest));
    const u8 padded[] = {0U, 0x40U, 0x00U};
    add(sink, "header-non-minimal", padded, sizeof(padded));
    const u8 invalid[] = {0U, 0xC0U};
    add(sink, "header-invalid-prefix", invalid, sizeof(invalid));
    const u8 vector[] = {0U, 0x03U, 'm', 'l', 's'};
    add(sink, "header-vector", vector, sizeof(vector));

    // Tree math: 2^31 leaves, the first and the last leaf; a size of three leaves.
    const u8 widest[] = {1U, 31U, 0U, 0U, 0U, 0U, 0xFFU, 0xFFU, 0xFFU, 0xFEU};
    add(sink, "tree-widest", widest, sizeof(widest));
    const u8 three[] = {1U, 0xFFU, 0U, 0U, 0U, 3U, 0U, 0U, 0U, 1U, 0U, 0U, 0U, 4U};
    add(sink, "tree-three-leaves", three, sizeof(three));

    // HPKE: a round trip with each KEM of the profile (both KDFs and every AEAD among them), one honest and one with
    // the first bit of the ciphertext changed. A suite byte is the KEM, plus 8 for HKDF-SHA384 and 16 per AEAD.
    struct HpkeSeed
    {
        const char* name;
        const char* changed;
        u8          suite;
        u16         enc;
    };
    constexpr HpkeSeed hpke_seeds[] = {
        {"hpke-x25519-aes", "hpke-x25519-aes-changed", 0U, 32U},
        {"hpke-p256", "hpke-p256-changed", 1U, 65U},
        {"hpke-p384", "hpke-p384-changed", 26U, 97U},
        {"hpke-x25519-chacha", "hpke-x25519-chacha-changed", 32U, 32U},
        {"hpke-mlkem768", "hpke-mlkem768-changed", 27U, 1088U},
        {"hpke-mlkem1024", "hpke-mlkem1024-changed", 36U, 1568U},
        {"hpke-xwing", "hpke-xwing-changed", 5U, 1120U},
        {"hpke-mlkem768-p256", "hpke-mlkem768-p256-changed", 46U, 1153U},
        {"hpke-mlkem1024-p384", "hpke-mlkem1024-p384-changed", 31U, 1665U},
    };
    static u8 input[256];
    for (const HpkeSeed& seed : hpke_seeds)
    {
        usize n    = 0U;
        input[n++] = 2U;
        input[n++] = seed.suite;
        input[n++] = 48U;
        for (usize i = 0U; i < 48U; ++i)
        {
            input[n++] = static_cast<u8>(i * 11U);
        }
        input[n++] = 0xFFU; // bit 0xFFFF: past the end, nothing changed
        input[n++] = 0xFFU;
        input[n++] = 4U;
        for (const u8 c : {u8{'i'}, u8{'n'}, u8{'f'}, u8{'o'}})
        {
            input[n++] = c;
        }
        input[n++] = 0U;
        for (const u8 c : {u8{'s'}, u8{'e'}, u8{'c'}, u8{'r'}, u8{'e'}, u8{'t'}})
        {
            input[n++] = c;
        }
        add(sink, seed.name, input, n);
        const usize bit = 8U * seed.enc;
        input[51]       = static_cast<u8>(bit >> 8U);
        input[52]       = static_cast<u8>(bit);
        add(sink, seed.changed, input, n);
    }

    // A hostile opening and a hostile signature in the mandatory suite, and a derivation at the widest length.
    usize n    = 0U;
    input[n++] = 3U;
    input[n++] = 0U;
    input[n++] = 32U;
    for (usize i = 0U; i < 32U; ++i)
    {
        input[n++] = static_cast<u8>(i);
    }
    input[n++] = 32U;
    for (usize i = 0U; i < 32U; ++i)
    {
        input[n++] = static_cast<u8>(0x80U + i);
    }
    input[n++] = 4U;
    for (const u8 c : {u8{'N'}, u8{'o'}, u8{'d'}, u8{'e'}})
    {
        input[n++] = c;
    }
    input[n++] = 0U;
    for (usize i = 0U; i < 24U; ++i)
    {
        input[n++] = static_cast<u8>(i * 3U);
    }
    add(sink, "open-hostile", input, n);
    // An Ed25519 key and signature of zeros (u16 lengths 32 and 64), the label "key" and a one-byte content.
    n          = 0U;
    input[n++] = 4U;
    input[n++] = 0U;
    input[n++] = 0U;
    input[n++] = 32U;
    for (usize i = 0U; i < 32U; ++i)
    {
        input[n++] = 0U;
    }
    input[n++] = 0U;
    input[n++] = 64U;
    for (usize i = 0U; i < 64U; ++i)
    {
        input[n++] = 0U;
    }
    input[n++] = 3U;
    for (const u8 c : {u8{'k'}, u8{'e'}, u8{'y'}, u8{'c'}})
    {
        input[n++] = c;
    }
    add(sink, "signature-hostile", input, n);
    n          = 0U;
    input[n++] = 5U;
    input[n++] = 3U; // P-384
    input[n++] = 48U;
    for (usize i = 0U; i < 48U; ++i)
    {
        input[n++] = static_cast<u8>(i + 1U);
    }
    input[n++] = 3U;
    for (const u8 c : {u8{'k'}, u8{'e'}, u8{'y'}})
    {
        input[n++] = c;
    }
    input[n++] = 0U;
    for (const u8 c : {u8{0U}, u8{0U}, u8{0U}, u8{7U}, u8{0x2FU}, u8{0xD0U}}) // generation 7, length 255 * 48
    {
        input[n++] = c;
    }
    add(sink, "derive-widest", input, n);

    // The secret tree: eight leaves in each suite with the default-sized window, a jump past the window and late
    // reads, spends, replays, sends and a leaf past the tree; one leaf with no window.
    const u8 operations[] = {
        3U, 1U, 10U, // leaf 3, handshake: read and spend generation 10
        3U, 1U, 6U,  // a late generation, kept
        3U, 1U, 6U,  // replayed
        3U, 0U, 4U,  // dropped
        3U, 1U, 40U, // as far ahead as the window allows
        3U, 0U, 90U, // too far
        5U, 5U, 0U,  // leaf 5, application: a send
        5U, 5U, 0U,
        5U, 4U, 0U,  // read generation 0 of the receiver's application ratchet
        8U, 1U, 0U,  // one past the tree
        0U, 1U, 0U,  // leaf 0 opens its path after leaf 3's
    };
    for (u8 suite = 0U; suite < 4U; ++suite)
    {
        const u8 head[] = {6U, suite, 3U, 5U, 40U, static_cast<u8>(0x10U + suite)};
        n               = 0U;
        for (const u8 b : head)
        {
            input[n++] = b;
        }
        for (const u8 b : operations)
        {
            input[n++] = b;
        }
        const char* names[] = {"secret-tree-x25519-aes", "secret-tree-p256", "secret-tree-x25519-chacha",
                               "secret-tree-p384"};
        add(sink, names[suite], input, n);
    }
    const u8 no_window[] = {6U, 0U, 0U, 0U, 2U, 0x77U, 0U, 1U, 1U, 0U, 0U, 0U, 0U, 1U, 3U, 0U, 0U, 2U, 0U, 1U, 4U};
    add(sink, "secret-tree-no-window", no_window, sizeof(no_window));
}
