// crd-fuzz-crypto-mls: MLS's presentation language, tree math and labelled operations and the HPKE beneath them
// (NET.2; docs/design/network-foundation.md#mls and #hpke) under the bounded fuzz harness (REPO.DEV.9;
// docs/design/test-instruments.md).
//
// Input. The first byte picks the mode, the rest is its data, read field by field (a field's length is one byte
// unless noted; a short input ends the case):
//   0  headers: the bytes a decoder reads as a vector header, a vector and a presence octet;
//   1  tree math: the size's exponent (or, above 31, a raw 32-bit size) and two nodes (u32 each);
//   2  HPKE: the suite, the recipient's input keying material, the index of a bit to change (u16; past the end
//      changes none), the info, the associated data, then the plaintext;
//   3  hostile openings: the suite, the recipient's input keying material, the encapsulation, the label, the
//      context, then the ciphertext;
//   4  hostile signatures: the suite, the public key, the signature (u16 lengths each), the label, then the content;
//   5  derivations: the suite, the secret, the label, the context, the generation (u32) and the length (u16).
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

void hpke_round_trip(Reader& in)
{
    const mls::CipherSuite& suite = suite_of(in.byte());
    const ConstSpan<u8>     ikm   = in.bytes(in.byte());
    const usize             bit   = in.u16be();
    const ConstSpan<u8>     info  = in.bytes(in.byte());
    const ConstSpan<u8>     aad   = in.bytes(in.byte());
    const ConstSpan<u8>     text  = in.rest();
    if (!in.ok())
    {
        return;
    }
    HpkeKey           recipient;
    const CryptoError derived = recipient.derive(suite.kem, ikm);
    if (ikm.size() < hpke_sizes(suite.kem).private_key)
    {
        CRD_FUZZ_REQUIRE(derived == CryptoError::BadLength && !recipient.ready());
        return;
    }
    CRD_FUZZ_REQUIRE(derived == CryptoError::None);
    const usize public_bytes = hpke_sizes(suite.kem).public_key;
    u8          recipient_public[97];
    CRD_FUZZ_REQUIRE(recipient.public_key(Span<u8>(recipient_public, public_bytes)) == CryptoError::None);
    // enc || ciphertext in one buffer, so one bit index reaches either.
    static u8 sealed[crd::fuzz::kMaxInputBytes + 97U + kHpkeTagBytes];
    static u8 opened[crd::fuzz::kMaxInputBytes];
    const Span<u8> enc(sealed, public_bytes);
    const Span<u8> ciphertext(sealed + public_bytes, text.size() + kHpkeTagBytes);
    CRD_FUZZ_REQUIRE(hpke_seal(suite.hpke(), ConstSpan<u8>(recipient_public, public_bytes), info, aad, text, enc,
                               ciphertext) == CryptoError::None);
    const bool changed = bit < 8U * (enc.size() + ciphertext.size());
    if (changed)
    {
        sealed[bit / 8U] ^= static_cast<u8>(1U << (bit % 8U));
    }
    std::memset(opened, 0xA5, text.size());
    const Span<u8>    out(opened, text.size());
    const CryptoError result = recipient.open(suite.hpke(), enc, info, aad, ciphertext, out);
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
    switch (data[0] % 6U)
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
    default:
        derivations(in);
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

    // HPKE: a round trip in each suite, one honest and one with the first bit of the ciphertext changed.
    static u8 input[256];
    for (u8 suite = 0U; suite < 4U; ++suite)
    {
        usize n    = 0U;
        input[n++] = 2U;
        input[n++] = suite;
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
        const char* names[]   = {"hpke-x25519-aes", "hpke-p256", "hpke-x25519-chacha", "hpke-p384"};
        const char* changed[] = {"hpke-x25519-aes-changed", "hpke-p256-changed", "hpke-x25519-chacha-changed",
                                 "hpke-p384-changed"};
        add(sink, names[suite], input, n);
        constexpr usize enc_bytes[] = {32U, 65U, 32U, 97U};
        const usize     enc         = enc_bytes[suite];
        input[51]       = static_cast<u8>((8U * enc) >> 8U);
        input[52]       = static_cast<u8>(8U * enc);
        add(sink, changed[suite], input, n);
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
}
