// crd-fuzz-crypto-provider: crd-crypto's provider interface (NET.2; docs/design/network-foundation.md#provider)
// under the bounded fuzz harness (REPO.DEV.9; docs/design/test-instruments.md).
//
// Input. The first byte picks the mode and the algorithm, the rest is its data, read field by field (a field's length
// is one byte unless noted; a short input ends the case):
//   0  AEAD open: algorithm, key, nonce length, nonce, associated length, associated data, then the sealed bytes;
//   1  verification: algorithm, public key (u16 length), signature (u16 length), then the message;
//   2  key agreement: algorithm, own private key, then the peer's public key;
//   3  HKDF: algorithm, salt, input key material, info, two output lengths (u16 each);
//   4  sign then verify: algorithm, seed, then the message;
//   5  KEM round trip: algorithm, seed, the index of a ciphertext bit to change (u16), then a ciphertext;
//   6  KEM encapsulation key: algorithm, then the key;
//   7  table PRF: key (16 bytes), then the message.
//
// Oracles, none of them the provider's verdict on the same call:
//   * an AEAD opening that succeeds re-seals to exactly the input (the AEAD's own round trip), and a refused one
//     releases no byte;
//   * every verdict is deterministic, every refusal names the documented error for the lengths given, and no
//     signature algorithm is ever "unsupported";
//   * an accepted EC peer is an uncompressed point of the exact length, an X25519 secret is never all zero, and
//     derivation is deterministic;
//   * HKDF's output of one length is the prefix of every longer one (RFC 5869's counter construction);
//   * a key from any seed signs a message that verifies, and the same signature over a changed message does not;
//   * a KEM key from any seed decapsulates its own encapsulations to the sent secret, a changed ciphertext to
//     another secret, and any ciphertext of the right length to a deterministic one (implicit rejection, never an
//     error);
//   * an encapsulation key is accepted exactly when a restatement of FIPS 203's modulus check (every 12-bit
//     coefficient below q = 3329) accepts it;
//   * the table PRF equals a restatement of SipHash-2-4 (Aumasson and Bernstein) for every key and message.
#include <crd/fuzz/harness.hpp>

#include <crd/crypto/provider.hpp>

#include <algorithm>
#include <cstring>

namespace
{
using crd::u16;
using crd::u64;
using crd::u8;
using crd::usize;
using crd::containers::ConstSpan;
using crd::containers::Span;
using namespace crd::crypto;

constexpr AlgorithmId kAeads[]      = {AlgorithmId::Aes128Gcm, AlgorithmId::Aes256Gcm, AlgorithmId::Chacha20Poly1305};
constexpr AlgorithmId kSignatures[] = {AlgorithmId::MlDsa44, AlgorithmId::MlDsa65, AlgorithmId::MlDsa87,
                                       AlgorithmId::Ed25519, AlgorithmId::EcdsaSecp256r1Sha256,
                                       AlgorithmId::EcdsaSecp384r1Sha384};
constexpr AlgorithmId kAgreements[] = {AlgorithmId::X25519, AlgorithmId::Secp256r1, AlgorithmId::Secp384r1};
constexpr AlgorithmId kKdfs[]       = {AlgorithmId::HkdfSha256, AlgorithmId::HkdfSha384};
constexpr AlgorithmId kKems[]       = {AlgorithmId::MlKem768, AlgorithmId::MlKem1024};

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

template <usize N> AlgorithmId pick(const AlgorithmId (&table)[N], u8 selector) noexcept
{
    return table[selector % N];
}

bool all_zero(ConstSpan<u8> bytes) noexcept
{
    return std::ranges::all_of(bytes, [](u8 b) { return b == 0U; });
}

bool same(ConstSpan<u8> a, ConstSpan<u8> b) noexcept
{
    return a.size() == b.size() && (a.empty() || std::memcmp(a.data(), b.data(), a.size()) == 0);
}

void aead_open(Reader& in)
{
    const AlgorithmId   alg        = pick(kAeads, in.byte());
    const ConstSpan<u8> key        = in.bytes(in.byte());
    const ConstSpan<u8> nonce      = in.bytes(in.byte());
    const ConstSpan<u8> associated = in.bytes(in.byte());
    const ConstSpan<u8> sealed     = in.rest();
    if (!in.ok())
    {
        return;
    }
    AeadKey           aead;
    const CryptoError init = aead.init(alg, key);
    CRD_FUZZ_REQUIRE((init == CryptoError::None) == (key.size() == aead_key_bytes(alg)));
    if (init != CryptoError::None)
    {
        CRD_FUZZ_REQUIRE(init == CryptoError::BadLength);
        return;
    }
    static u8 plain[crd::fuzz::kMaxInputBytes];
    static u8 again[crd::fuzz::kMaxInputBytes];
    const usize length = sealed.size() >= kAeadTagBytes ? sealed.size() - kAeadTagBytes : 0U;
    std::memset(plain, 0xA5, length);
    const CryptoError opened = aead.open(nonce, sealed, associated, Span<u8>(plain, length));
    if (nonce.size() != kAeadNonceBytes || sealed.size() < kAeadTagBytes)
    {
        CRD_FUZZ_REQUIRE(opened == CryptoError::BadLength);
        return;
    }
    if (opened != CryptoError::None)
    {
        CRD_FUZZ_REQUIRE(opened == CryptoError::Forged);
        CRD_FUZZ_REQUIRE(all_zero(ConstSpan<u8>(plain, length)));
        return;
    }
    CRD_FUZZ_REQUIRE(aead.seal(nonce, ConstSpan<u8>(plain, length), associated, Span<u8>(again, sealed.size())) ==
                     CryptoError::None);
    CRD_FUZZ_REQUIRE(same(ConstSpan<u8>(again, sealed.size()), sealed));
}

void verify(Reader& in)
{
    const AlgorithmId   alg       = pick(kSignatures, in.byte());
    const ConstSpan<u8> key       = in.bytes(in.u16be());
    const ConstSpan<u8> signature = in.bytes(in.u16be());
    const ConstSpan<u8> message   = in.rest();
    if (!in.ok())
    {
        return;
    }
    const ProviderVerifier verifier;
    const Verification     first = verifier.verify(alg, key, message, signature);
    CRD_FUZZ_REQUIRE(first != Verification::Unsupported);
    CRD_FUZZ_REQUIRE(verifier.verify(alg, key, message, signature) == first);
    const SignatureSizes sizes = signature_sizes(alg);
    if (key.size() != sizes.public_key || signature.empty() || signature.size() > sizes.signature)
    {
        CRD_FUZZ_REQUIRE(first == Verification::Invalid);
    }
}

void agree(Reader& in)
{
    const AlgorithmId   alg  = pick(kAgreements, in.byte());
    const ConstSpan<u8> own  = in.bytes(in.byte());
    const ConstSpan<u8> peer = in.rest();
    if (!in.ok())
    {
        return;
    }
    const AgreementSizes sizes = agreement_sizes(alg);
    AgreementKey         key;
    const CryptoError    loaded = key.from_private(alg, own);
    if (own.size() != sizes.private_key)
    {
        CRD_FUZZ_REQUIRE(loaded == CryptoError::BadLength);
        return;
    }
    if (loaded != CryptoError::None)
    {
        CRD_FUZZ_REQUIRE(loaded == CryptoError::BadKey && alg != AlgorithmId::X25519);
        return;
    }
    u8                secret[48];
    u8                repeat[48];
    const auto        out    = Span<u8>(secret, sizes.secret);
    const CryptoError result = key.derive(peer, out);
    CRD_FUZZ_REQUIRE(key.derive(peer, Span<u8>(repeat, sizes.secret)) == result);
    if (peer.size() != sizes.public_key)
    {
        CRD_FUZZ_REQUIRE(result == CryptoError::BadLength);
        return;
    }
    if (result != CryptoError::None)
    {
        CRD_FUZZ_REQUIRE(all_zero(out));
        const CryptoError refusal = alg == AlgorithmId::X25519 ? CryptoError::WeakSecret : CryptoError::BadKey;
        CRD_FUZZ_REQUIRE(result == refusal);
        return;
    }
    CRD_FUZZ_REQUIRE(same(out, ConstSpan<u8>(repeat, sizes.secret)));
    if (alg == AlgorithmId::X25519)
    {
        CRD_FUZZ_REQUIRE(!all_zero(out));
    }
    else
    {
        CRD_FUZZ_REQUIRE(peer[0] == 0x04U);
    }
}

void derive(Reader& in)
{
    const AlgorithmId   alg          = pick(kKdfs, in.byte());
    const ConstSpan<u8> salt         = in.bytes(in.byte());
    const ConstSpan<u8> ikm          = in.bytes(in.byte());
    const ConstSpan<u8> info         = in.bytes(in.byte());
    const usize         short_length = in.u16be();
    const usize         long_length  = in.u16be();
    if (!in.ok())
    {
        return;
    }
    const usize hash = hkdf_hash_bytes(alg);
    u8          prk[48];
    CRD_FUZZ_REQUIRE(hkdf_extract(alg, salt, ikm, Span<u8>(prk, hash)) == CryptoError::None);
    static u8   a[65536];
    static u8   b[65536];
    const usize lo = short_length < long_length ? short_length : long_length;
    const usize hi = short_length < long_length ? long_length : short_length;
    const CryptoError first  = hkdf_expand(alg, ConstSpan<u8>(prk, hash), info, Span<u8>(a, lo));
    const CryptoError second = hkdf_expand(alg, ConstSpan<u8>(prk, hash), info, Span<u8>(b, hi));
    CRD_FUZZ_REQUIRE((first == CryptoError::None) == (lo >= 1U && lo <= 255U * hash));
    CRD_FUZZ_REQUIRE((second == CryptoError::None) == (hi >= 1U && hi <= 255U * hash));
    if (first == CryptoError::None && second == CryptoError::None)
    {
        CRD_FUZZ_REQUIRE(same(ConstSpan<u8>(a, lo), ConstSpan<u8>(b, lo)));
    }
}

void sign_verify(Reader& in)
{
    const AlgorithmId   alg     = pick(kSignatures, in.byte());
    const ConstSpan<u8> seed    = in.bytes(in.byte());
    const ConstSpan<u8> message = in.rest();
    if (!in.ok())
    {
        return;
    }
    const SignatureSizes sizes = signature_sizes(alg);
    SigningKey           key;
    const CryptoError    loaded = key.from_seed(alg, seed);
    if (seed.size() != sizes.seed)
    {
        CRD_FUZZ_REQUIRE(loaded == CryptoError::BadLength);
        return;
    }
    if (loaded != CryptoError::None)
    {
        // Only an ECDSA scalar outside [1, n - 1] is refused.
        CRD_FUZZ_REQUIRE(loaded == CryptoError::BadKey);
        CRD_FUZZ_REQUIRE(alg == AlgorithmId::EcdsaSecp256r1Sha256 || alg == AlgorithmId::EcdsaSecp384r1Sha384);
        return;
    }
    static u8 pub[2592];
    static u8 sig[4627];
    usize     written = 0U;
    CRD_FUZZ_REQUIRE(key.public_key(Span<u8>(pub, sizes.public_key)) == CryptoError::None);
    CRD_FUZZ_REQUIRE(key.sign(message, Span<u8>(sig, sizes.signature), written) == CryptoError::None);
    const ProviderVerifier verifier;
    const ConstSpan<u8>    public_key(pub, sizes.public_key);
    const ConstSpan<u8>    signature(sig, written);
    CRD_FUZZ_REQUIRE(verifier.verify(alg, public_key, message, signature) == Verification::Valid);
    static u8 changed[crd::fuzz::kMaxInputBytes + 1U];
    std::memcpy(changed, message.data(), message.size());
    changed[message.size()] = 0x5AU; // one byte longer: another message
    CRD_FUZZ_REQUIRE(verifier.verify(alg, public_key, ConstSpan<u8>(changed, message.size() + 1U), signature) ==
                     Verification::Invalid);
}

void kem_round_trip(Reader& in)
{
    const AlgorithmId   alg       = pick(kKems, in.byte());
    const ConstSpan<u8> seed      = in.bytes(in.byte());
    const usize         flip      = in.u16be();
    const ConstSpan<u8> arbitrary = in.rest();
    if (!in.ok())
    {
        return;
    }
    const KemSizes    sizes  = kem_sizes(alg);
    KemKey            key;
    const CryptoError loaded = key.from_seed(alg, seed);
    CRD_FUZZ_REQUIRE((loaded == CryptoError::None) == (seed.size() == sizes.seed));
    if (loaded != CryptoError::None)
    {
        CRD_FUZZ_REQUIRE(loaded == CryptoError::BadLength);
        return;
    }
    static u8  ek[1568];
    static u8  ciphertext[1568];
    u8         sent[32];
    u8         received[32];
    u8         repeat[32];
    const auto public_key = Span<u8>(ek, sizes.public_key);
    const auto sealed     = Span<u8>(ciphertext, sizes.ciphertext);
    CRD_FUZZ_REQUIRE(key.public_key(public_key) == CryptoError::None);
    CRD_FUZZ_REQUIRE(kem_encapsulate(alg, public_key, sealed, sent) == CryptoError::None);
    CRD_FUZZ_REQUIRE(key.decapsulate(sealed, received) == CryptoError::None);
    CRD_FUZZ_REQUIRE(same(sent, received));
    sealed[flip % sizes.ciphertext] ^= static_cast<u8>(1U << (flip / sizes.ciphertext % 8U));
    CRD_FUZZ_REQUIRE(key.decapsulate(sealed, received) == CryptoError::None);
    CRD_FUZZ_REQUIRE(key.decapsulate(sealed, repeat) == CryptoError::None);
    CRD_FUZZ_REQUIRE(same(received, repeat));
    CRD_FUZZ_REQUIRE(!same(received, sent));
    const CryptoError opened = key.decapsulate(arbitrary, received);
    CRD_FUZZ_REQUIRE(opened == (arbitrary.size() == sizes.ciphertext ? CryptoError::None : CryptoError::BadLength));
    if (opened != CryptoError::None)
    {
        CRD_FUZZ_REQUIRE(all_zero(received));
        return;
    }
    CRD_FUZZ_REQUIRE(key.decapsulate(arbitrary, repeat) == CryptoError::None);
    CRD_FUZZ_REQUIRE(same(received, repeat));
}

// FIPS 203 section 7.2's encapsulation key check, restated: ByteDecode12 of each 384-byte polynomial yields
// coefficients below q.
bool reduced(ConstSpan<u8> key) noexcept
{
    const usize polynomials = (key.size() - 32U) / 384U;
    for (usize i = 0U; i < polynomials * 384U; i += 3U)
    {
        const unsigned low  = key[i] | (key[i + 1U] & 0x0FU) << 8U;
        const unsigned high = key[i + 1U] >> 4U | key[i + 2U] << 4U;
        if (low >= 3329U || high >= 3329U)
        {
            return false;
        }
    }
    return true;
}

void kem_public_key(Reader& in)
{
    const AlgorithmId   alg = pick(kKems, in.byte());
    const ConstSpan<u8> key = in.rest();
    if (!in.ok())
    {
        return;
    }
    const KemSizes    sizes = kem_sizes(alg);
    static u8         ciphertext[1568];
    u8                secret[32];
    const auto        sealed = Span<u8>(ciphertext, sizes.ciphertext);
    const CryptoError result = kem_encapsulate(alg, key, sealed, secret);
    if (key.size() != sizes.public_key)
    {
        CRD_FUZZ_REQUIRE(result == CryptoError::BadLength);
    }
    else
    {
        CRD_FUZZ_REQUIRE(result == (reduced(key) ? CryptoError::None : CryptoError::BadKey));
    }
    if (result != CryptoError::None)
    {
        CRD_FUZZ_REQUIRE(all_zero(sealed));
        CRD_FUZZ_REQUIRE(all_zero(secret));
    }
}

u64 load_le64(const u8* bytes) noexcept
{
    u64 value = 0U;
    for (usize i = 0U; i < 8U; ++i)
    {
        value |= static_cast<u64>(bytes[i]) << (8U * i);
    }
    return value;
}

u64 rotate(u64 value, unsigned bits) noexcept
{
    return value << bits | value >> (64U - bits);
}

// SipHash-2-4 restated from its paper: two compression rounds per 8-byte word, the length in the last word's top
// byte, four finalization rounds.
u64 siphash24(u64 k0, u64 k1, ConstSpan<u8> message) noexcept
{
    u64        v0    = 0x736f6d6570736575ULL ^ k0;
    u64        v1    = 0x646f72616e646f6dULL ^ k1;
    u64        v2    = 0x6c7967656e657261ULL ^ k0;
    u64        v3    = 0x7465646279746573ULL ^ k1;
    const auto round = [&]
    {
        v0 += v1;
        v1 = rotate(v1, 13U) ^ v0;
        v0 = rotate(v0, 32U);
        v2 += v3;
        v3 = rotate(v3, 16U) ^ v2;
        v0 += v3;
        v3 = rotate(v3, 21U) ^ v0;
        v2 += v1;
        v1 = rotate(v1, 17U) ^ v2;
        v2 = rotate(v2, 32U);
    };
    const usize whole = message.size() & ~usize{7U};
    for (usize i = 0U; i < whole; i += 8U)
    {
        const u64 word = load_le64(message.data() + i);
        v3 ^= word;
        round();
        round();
        v0 ^= word;
    }
    u64 last = static_cast<u64>(message.size()) << 56U;
    for (usize i = whole; i < message.size(); ++i)
    {
        last |= static_cast<u64>(message[i]) << (8U * (i - whole));
    }
    v3 ^= last;
    round();
    round();
    v0 ^= last;
    v2 ^= 0xFFU;
    for (unsigned i = 0U; i < 4U; ++i)
    {
        round();
    }
    return v0 ^ v1 ^ v2 ^ v3;
}

void table_prf(Reader& in)
{
    const ConstSpan<u8> key     = in.bytes(kPrfKeyBytes);
    const ConstSpan<u8> message = in.rest();
    if (!in.ok())
    {
        return;
    }
    PrfKey prf;
    CRD_FUZZ_REQUIRE(prf.init(AlgorithmId::Siphash24, key) == CryptoError::None);
    u64 value = 0U;
    CRD_FUZZ_REQUIRE(prf.hash(message, value) == CryptoError::None);
    CRD_FUZZ_REQUIRE(value == siphash24(load_le64(key.data()), load_le64(key.data() + 8U), message));
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
    switch (data[0] % 8U)
    {
    case 0U:
        aead_open(in);
        break;
    case 1U:
        verify(in);
        break;
    case 2U:
        agree(in);
        break;
    case 3U:
        derive(in);
        break;
    case 4U:
        sign_verify(in);
        break;
    case 5U:
        kem_round_trip(in);
        break;
    case 6U:
        kem_public_key(in);
        break;
    default:
        table_prf(in);
        break;
    }
    return 0;
}

void crd_fuzz_seeds(crd::fuzz::SeedSink& sink)
{
    // An honest ChaCha20-Poly1305 message (sealed here: AEAD sealing is deterministic) and its forgery.
    u8 key[32];
    u8 nonce[kAeadNonceBytes];
    for (usize i = 0U; i < sizeof(key); ++i)
    {
        key[i] = static_cast<u8>(i * 7U);
    }
    for (usize i = 0U; i < sizeof(nonce); ++i)
    {
        nonce[i] = static_cast<u8>(0xC0U + i);
    }
    const u8 associated[] = {'h', 'e', 'a', 'd'};
    const u8 text[]       = {'c', 'e', 'r', 'i', 'd', ' ', 'p', 'a', 'c', 'k', 'e', 't'};
    AeadKey  aead;
    u8       sealed[sizeof(text) + kAeadTagBytes];
    if (aead.init(AlgorithmId::Chacha20Poly1305, key) != CryptoError::None ||
        aead.seal(nonce, text, associated, sealed) != CryptoError::None)
    {
        return;
    }
    static u8 input[512];
    usize     n = 0U;
    const auto put = [&](const void* bytes, usize size)
    {
        std::memcpy(input + n, bytes, size);
        n += size;
    };
    const auto put_byte = [&](u8 value) { input[n++] = value; };
    put_byte(0U);
    put_byte(2U); // ChaCha20-Poly1305
    put_byte(32U);
    put(key, sizeof(key));
    put_byte(kAeadNonceBytes);
    put(nonce, sizeof(nonce));
    put_byte(sizeof(associated));
    put(associated, sizeof(associated));
    put(sealed, sizeof(sealed));
    add(sink, "aead-honest", input, n);
    input[n - 1U] ^= 0x01U;
    add(sink, "aead-forged", input, n);

    // Ed25519: RFC 8032 TEST 2 (key, one-byte message, its deterministic signature).
    const u8 ed_public[] = {0x3d, 0x40, 0x17, 0xc3, 0xe8, 0x43, 0x89, 0x5a, 0x92, 0xb7, 0x0a, 0xa7,
                            0x4d, 0x1b, 0x7e, 0xbc, 0x9c, 0x98, 0x2c, 0xcf, 0x2e, 0xc4, 0x96, 0x8c,
                            0xc0, 0xcd, 0x55, 0xf1, 0x2a, 0xf4, 0x66, 0x0c};
    const u8 ed_secret[] = {0x4c, 0xcd, 0x08, 0x9b, 0x28, 0xff, 0x96, 0xda, 0x9d, 0xb6, 0xc3, 0x46,
                            0xec, 0x11, 0x4e, 0x0f, 0x5b, 0x8a, 0x31, 0x9f, 0x35, 0xab, 0xa6, 0x24,
                            0xda, 0x8c, 0xf6, 0xed, 0x4f, 0xb8, 0xa6, 0xfb};
    SigningKey ed;
    u8         ed_sig[64];
    usize      ed_written = 0U;
    const u8   ed_message[] = {0x72};
    if (ed.from_seed(AlgorithmId::Ed25519, ed_secret) != CryptoError::None ||
        ed.sign(ed_message, ed_sig, ed_written) != CryptoError::None)
    {
        return;
    }
    n = 0U;
    put_byte(1U);
    put_byte(3U); // Ed25519
    put_byte(0U);
    put_byte(sizeof(ed_public));
    put(ed_public, sizeof(ed_public));
    put_byte(0U);
    put_byte(static_cast<u8>(ed_written));
    put(ed_sig, ed_written);
    put(ed_message, sizeof(ed_message));
    add(sink, "verify-ed25519", input, n);

    // X25519 with the base point (its public key), and a small-order peer (zero).
    n = 0U;
    put_byte(2U);
    put_byte(0U);
    put_byte(32U);
    put(key, sizeof(key));
    put_byte(9U);
    for (usize i = 1U; i < 32U; ++i)
    {
        put_byte(0U);
    }
    add(sink, "agree-x25519-base", input, n);
    input[n - 32U] = 0U;
    add(sink, "agree-x25519-zero", input, n);

    // P-256 with an off-curve peer of the right length.
    n = 0U;
    put_byte(2U);
    put_byte(1U);
    put_byte(32U);
    put(key, sizeof(key));
    put_byte(0x04U);
    for (usize i = 0U; i < 64U; ++i)
    {
        put_byte(static_cast<u8>(i));
    }
    add(sink, "agree-p256-off-curve", input, n);

    // HKDF-SHA256 with a short and a two-block output, and an over-long one.
    n = 0U;
    put_byte(3U);
    put_byte(0U);
    put_byte(4U);
    put(associated, sizeof(associated));
    put_byte(12U);
    put(text, sizeof(text));
    put_byte(4U);
    put(associated, sizeof(associated));
    put_byte(0U);
    put_byte(10U);
    put_byte(0U);
    put_byte(64U);
    add(sink, "hkdf-prefix", input, n);
    input[n - 2U] = 0x1FU; // 8000 bytes: above 255 * 32
    add(sink, "hkdf-too-long", input, n);

    // Sign then verify: ML-DSA-65 from a fixed seed, and an ECDSA P-256 scalar of zero.
    n = 0U;
    put_byte(4U);
    put_byte(1U); // ML-DSA-65
    put_byte(32U);
    put(key, sizeof(key));
    put(text, sizeof(text));
    add(sink, "sign-mldsa65", input, n);
    n = 0U;
    put_byte(4U);
    put_byte(4U); // ECDSA P-256
    put_byte(32U);
    for (usize i = 0U; i < 32U; ++i)
    {
        put_byte(0U);
    }
    put(text, sizeof(text));
    add(sink, "sign-ecdsa-zero-scalar", input, n);

    // ML-KEM-768 from a fixed seed: a round trip with one bit changed, then a ciphertext of the right length.
    static u8 kem_input[2U + 1568U];
    kem_input[0] = 5U;
    kem_input[1] = 0U; // ML-KEM-768
    kem_input[2] = 64U;
    std::memcpy(kem_input + 3U, key, sizeof(key));
    std::memcpy(kem_input + 3U + sizeof(key), key, sizeof(key));
    kem_input[67] = 0x02U; // bit 0x021D: byte 541, bit 0
    kem_input[68] = 0x1DU;
    for (usize i = 0U; i < 1088U; ++i)
    {
        kem_input[69U + i] = text[i % sizeof(text)];
    }
    add(sink, "kem-mlkem768-round-trip", kem_input, 69U + 1088U);

    // The ML-KEM-1024 encapsulation key of the same fixed seed, and that key with its first coefficient raised to q.
    KemKey kem;
    if (kem.from_seed(AlgorithmId::MlKem1024, ConstSpan<u8>(kem_input + 3U, 64U)) != CryptoError::None)
    {
        return;
    }
    kem_input[0] = 6U;
    kem_input[1] = 1U; // ML-KEM-1024
    if (kem.public_key(Span<u8>(kem_input + 2U, 1568U)) != CryptoError::None)
    {
        return;
    }
    add(sink, "kem-mlkem1024-public-key", kem_input, 2U + 1568U);
    kem_input[2] = 0x01U; // 0x001 | 0xD << 8 = 3329
    kem_input[3] = static_cast<u8>((kem_input[3] & 0xF0U) | 0x0DU);
    add(sink, "kem-mlkem1024-unreduced", kem_input, 2U + 1568U);

    // SipHash-2-4: the paper's appendix A key (00 01 ... 0f) and message (00 01 ... 0e).
    n = 0U;
    put_byte(7U);
    for (usize i = 0U; i < kPrfKeyBytes; ++i)
    {
        put_byte(static_cast<u8>(i));
    }
    for (usize i = 0U; i < 15U; ++i)
    {
        put_byte(static_cast<u8>(i));
    }
    add(sink, "prf-siphash-paper", input, n);
}
