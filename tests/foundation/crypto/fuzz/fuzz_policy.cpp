// crd-fuzz-crypto-policy: the signed crypto policy generations of crd-crypto (NET-STD;
// docs/design/network-foundation.md#policy-wire) under the bounded fuzz harness (REPO.DEV.9;
// docs/design/test-instruments.md).
//
// Input. The first byte picks the mode, the rest is its data.
//   0  a generation as bytes, decoded;
//   1  a generated sequence: a floor, then up to eight generations whose content, signers and arrival times are read
//      from the bytes, written by the writers, signed by the test signer and applied to one store.
//
// Oracles, none of them the codec's or the store's own verdict:
//   * an independent restatement of the layout, its bounds and the contract's validation order decides the
//     decoder's exact result for every input, and an accepted generation's fields and views equal the restatement's;
//   * a model of the contract's adoption rules (rollback before verification, every trust key's signature, conflict,
//     issue skew, expiry with grace, issue order) decides every apply's exact result and the floor after it;
//   * every snapshot's freshness, algorithm status and version minimums at the window and validity boundaries equal
//     the model's, restated from the contract (the build's ceiling and ignore-unknown included).
#include "../policy_test_signer.hpp"

#include <crd/fuzz/harness.hpp>

#include <crd/crypto/policy.hpp>

#include <cstring>

namespace
{
using crd::u16;
using crd::u32;
using crd::u64;
using crd::u8;
using crd::usize;
using crd::containers::ConstSpan;
using crd::containers::Span;
using namespace crd::crypto;
using crd::crypto::test::kTestSignatureBytes;
using crd::crypto::test::kTestTrust;
using crd::crypto::test::test_sign;
using crd::crypto::test::TestVerifier;

constexpr u64 kDay  = 86400U;
constexpr u64 kHour = 3600U;
constexpr u64 kT0   = 1800000000U;

u16 be16(const u8* p)
{
    return static_cast<u16>((p[0] << 8U) | p[1]);
}

u32 be32(const u8* p)
{
    return (static_cast<u32>(be16(p)) << 16U) | be16(p + 2U);
}

u64 be64(const u8* p)
{
    return (static_cast<u64>(be32(p)) << 32U) | be32(p + 4U);
}

// ---- The restated layout ---------------------------------------------------------------------------------------

struct Restated
{
    PolicyError   error = PolicyError::None;
    PolicyContent content;
    usize         body  = 0U;
    u32           count = 0U;
    usize         offsets[kMaxPolicySignatures] = {};
    usize         lengths[kMaxPolicySignatures] = {};
    u16           algorithms[kMaxPolicySignatures] = {};
};

// The contract's validation order: generation, dates, counts, minimums, then each rule's window and uniqueness.
PolicyError restate_content(const PolicyContent& c)
{
    if (c.generation == 0U)
    {
        return PolicyError::BadGeneration;
    }
    if (c.expires <= c.issued || c.expires - c.issued > kMaxPolicyLifetime || c.offline_grace > kMaxOfflineGrace)
    {
        return PolicyError::BadDates;
    }
    if (c.minimum_count > kMaxPolicyMinimums || c.rule_count > kMaxPolicyRules)
    {
        return PolicyError::TooLarge;
    }
    for (u32 i = 0U; i < c.minimum_count; ++i)
    {
        for (u32 j = 0U; j < i; ++j)
        {
            if (c.minimums[i].protocol == c.minimums[j].protocol)
            {
                return PolicyError::DuplicateMinimum;
            }
        }
    }
    for (u32 i = 0U; i < c.rule_count; ++i)
    {
        const AlgorithmRule& r = c.rules[i];
        if (static_cast<u16>(r.algorithm) == 0U || r.accepted_from > r.preferred_from ||
            r.preferred_from > r.preferred_until || r.preferred_until > r.refused_from ||
            r.accepted_from >= r.refused_from)
        {
            return PolicyError::BadRule;
        }
        for (u32 j = 0U; j < i; ++j)
        {
            if (c.rules[j].algorithm == r.algorithm)
            {
                return PolicyError::DuplicateRule;
            }
        }
    }
    return PolicyError::None;
}

Restated restate(const u8* d, usize n)
{
    Restated r;
    const auto fail = [&r](PolicyError error)
    {
        r.error = error;
        return r;
    };
    if (n > kMaxPolicyBytes)
    {
        return fail(PolicyError::TooLarge);
    }
    if (n < 8U)
    {
        return fail(PolicyError::Truncated);
    }
    if (std::memcmp(d, "CRDPOLCY", 8U) != 0)
    {
        return fail(PolicyError::BadMagic);
    }
    if (n < 9U)
    {
        return fail(PolicyError::Truncated);
    }
    if (d[8] != 1U)
    {
        return fail(PolicyError::UnsupportedFormat);
    }
    if (n < 37U)
    {
        return fail(PolicyError::Truncated);
    }
    PolicyContent& c  = r.content;
    c.generation      = be64(d + 9U);
    c.issued          = be64(d + 17U);
    c.expires         = be64(d + 25U);
    c.offline_grace   = be32(d + 33U);
    if (n < 38U)
    {
        return fail(PolicyError::Truncated);
    }
    c.minimum_count = d[37];
    if (c.minimum_count > 16U)
    {
        return fail(PolicyError::TooLarge);
    }
    usize at = 38U;
    if (n < at + 6U * c.minimum_count)
    {
        return fail(PolicyError::Truncated);
    }
    for (u32 i = 0U; i < c.minimum_count; ++i, at += 6U)
    {
        c.minimums[i] = {be16(d + at), be16(d + at + 2U), be16(d + at + 4U)};
    }
    if (n < at + 1U)
    {
        return fail(PolicyError::Truncated);
    }
    c.rule_count = d[at++];
    if (c.rule_count > 32U)
    {
        return fail(PolicyError::TooLarge);
    }
    if (n < at + 34U * c.rule_count)
    {
        return fail(PolicyError::Truncated);
    }
    for (u32 i = 0U; i < c.rule_count; ++i, at += 34U)
    {
        c.rules[i] = {static_cast<AlgorithmId>(be16(d + at)), be64(d + at + 2U), be64(d + at + 10U),
                      be64(d + at + 18U), be64(d + at + 26U)};
    }
    const PolicyError content = restate_content(c);
    if (content != PolicyError::None)
    {
        return fail(content);
    }
    r.body = at;
    if (n < at + 1U)
    {
        return fail(PolicyError::Truncated);
    }
    r.count = d[at++];
    if (r.count == 0U)
    {
        return fail(PolicyError::Unsigned);
    }
    if (r.count > 8U)
    {
        return fail(PolicyError::TooLarge);
    }
    for (u32 i = 0U; i < r.count; ++i)
    {
        if (n < at + 36U)
        {
            return fail(PolicyError::Truncated);
        }
        r.algorithms[i] = be16(d + at + 32U);
        r.lengths[i]    = be16(d + at + 34U);
        if (r.lengths[i] > 8192U)
        {
            return fail(PolicyError::TooLarge);
        }
        if (n < at + 36U + r.lengths[i])
        {
            return fail(PolicyError::Truncated);
        }
        r.offsets[i] = at;
        at += 36U + r.lengths[i];
    }
    if (at != n)
    {
        return fail(PolicyError::TrailingBytes);
    }
    for (u32 i = 0U; i < r.count; ++i)
    {
        if (r.lengths[i] == 0U)
        {
            return fail(PolicyError::BadSignatureEntry);
        }
        for (u32 j = 0U; j < i; ++j)
        {
            if (std::memcmp(d + r.offsets[i], d + r.offsets[j], 32U) == 0)
            {
                return fail(PolicyError::DuplicateSignature);
            }
        }
    }
    return r;
}

void decode_checked(const u8* data, usize size)
{
    const Restated expected = restate(data, size);
    PolicyMessage  out;
    CRD_FUZZ_REQUIRE(decode_policy(ConstSpan<u8>(data, size), out) == expected.error);
    if (expected.error != PolicyError::None)
    {
        return;
    }
    const PolicyContent& a = out.content;
    const PolicyContent& b = expected.content;
    CRD_FUZZ_REQUIRE(out.format == 1U && a.generation == b.generation && a.issued == b.issued &&
                     a.expires == b.expires && a.offline_grace == b.offline_grace);
    CRD_FUZZ_REQUIRE(a.minimum_count == b.minimum_count && a.rule_count == b.rule_count);
    for (u32 i = 0U; i < a.minimum_count; ++i)
    {
        CRD_FUZZ_REQUIRE(a.minimums[i].protocol == b.minimums[i].protocol &&
                         a.minimums[i].major == b.minimums[i].major && a.minimums[i].minor == b.minimums[i].minor);
    }
    for (u32 i = 0U; i < a.rule_count; ++i)
    {
        CRD_FUZZ_REQUIRE(a.rules[i].algorithm == b.rules[i].algorithm &&
                         a.rules[i].accepted_from == b.rules[i].accepted_from &&
                         a.rules[i].preferred_from == b.rules[i].preferred_from &&
                         a.rules[i].preferred_until == b.rules[i].preferred_until &&
                         a.rules[i].refused_from == b.rules[i].refused_from);
    }
    CRD_FUZZ_REQUIRE(out.signed_bytes.data() == data && out.signed_bytes.size() == expected.body);
    CRD_FUZZ_REQUIRE(out.signature_count == expected.count);
    for (u32 i = 0U; i < out.signature_count; ++i)
    {
        const PolicySignature& signature = out.signatures[i];
        CRD_FUZZ_REQUIRE(std::memcmp(signature.key.bytes, data + expected.offsets[i], kKeyIdBytes) == 0);
        CRD_FUZZ_REQUIRE(static_cast<u16>(signature.algorithm) == expected.algorithms[i]);
        CRD_FUZZ_REQUIRE(signature.signature.data() == data + expected.offsets[i] + 36U &&
                         signature.signature.size() == expected.lengths[i]);
    }
}

// ---- Generated sequences ---------------------------------------------------------------------------------------

// The algorithms a sequence draws from, and which of them the store's build supports.
constexpr AlgorithmId kDrawn[] = {AlgorithmId::X25519Mlkem768, AlgorithmId::X25519, AlgorithmId::Chacha20Poly1305,
                                  AlgorithmId::Secp256r1Mlkem768, static_cast<AlgorithmId>(0x7F01U), AlgorithmId{}};
constexpr bool        kDrawnSupported[] = {true, true, true, false, false, false};
constexpr AlgorithmId kSupported[]      = {AlgorithmId::X25519Mlkem768, AlgorithmId::X25519,
                                           AlgorithmId::Chacha20Poly1305, AlgorithmId::Ed25519, AlgorithmId::MlDsa87};
constexpr usize       kDrawnCount       = sizeof(kDrawn) / sizeof(kDrawn[0]);

class Draw
{
public:
    Draw(const u8* data, usize size) : m_data(data), m_size(size) {}

    u8 next()
    {
        return m_at < m_size ? m_data[m_at++] : 0U;
    }

    [[nodiscard]] bool done() const { return m_at >= m_size; }

private:
    const u8* m_data;
    usize     m_size;
    usize     m_at = 0U;
};

PolicyContent draw_content(Draw& draw)
{
    PolicyContent c;
    c.generation    = draw.next() % 8U;
    c.issued        = kT0 + draw.next() * kHour;
    const u8 life   = draw.next();
    c.expires       = c.issued + (life % 8U == 0U ? 0U : life * kDay);
    c.offline_grace = static_cast<u32>((draw.next() % 128U) * kDay);
    c.minimum_count = draw.next() % 3U;
    for (u32 i = 0U; i < c.minimum_count; ++i)
    {
        c.minimums[i] = {static_cast<u16>(draw.next() % 3U + 1U), static_cast<u16>(draw.next() % 3U),
                         static_cast<u16>(draw.next() % 3U)};
    }
    c.rule_count = draw.next() % 5U;
    for (u32 i = 0U; i < c.rule_count; ++i)
    {
        AlgorithmRule& r  = c.rules[i];
        r.algorithm       = kDrawn[draw.next() % kDrawnCount];
        r.accepted_from   = kT0 + draw.next() * kHour;
        r.preferred_from  = r.accepted_from + draw.next() * kHour;
        r.preferred_until = r.preferred_from + draw.next() * kHour;
        const u8 refused  = draw.next();
        r.refused_from    = refused == 255U ? kNever : r.preferred_until + refused * kHour;
        if (draw.next() % 16U == 0U)
        {
            r.preferred_from = r.preferred_until + 1U; // a window out of order
        }
    }
    return c;
}

// Who signs a generated generation: one bit per signer.
constexpr u8 kPostQuantum = 1U;
constexpr u8 kClassical   = 2U;
constexpr u8 kForged      = 4U; // the classical signature with a flipped bit

struct Encoded
{
    u8    bytes[4096] = {};
    usize size        = 0U;
};

bool encode(const PolicyContent& content, u8 signers, Encoded& out)
{
    u8    body[2048];
    usize body_size = 0U;
    const PolicyError written = write_policy_body(content, body, body_size);
    CRD_FUZZ_REQUIRE(written == restate_content(content));
    if (written != PolicyError::None)
    {
        return false;
    }
    PolicySignature signatures[2];
    u8              tags[2][kTestSignatureBytes];
    u32             count = 0U;
    for (u32 k = 0U; k < 2U; ++k)
    {
        if ((signers & (1U << k)) == 0U)
        {
            continue;
        }
        test_sign(kTestTrust[k].algorithm, kTestTrust[k].public_key, ConstSpan<u8>(body, body_size), tags[count]);
        if (k == 1U && (signers & kForged) != 0U)
        {
            tags[count][kTestSignatureBytes - 1U] ^= 0x80U;
        }
        signatures[count] = {kTestTrust[k].id, kTestTrust[k].algorithm, ConstSpan<u8>(tags[count])};
        ++count;
    }
    if (count == 0U)
    {
        return false; // nothing to apply: the writer refuses an unsigned generation (covered in mode 0)
    }
    CRD_FUZZ_REQUIRE(write_policy(ConstSpan<u8>(body, body_size), ConstSpan<PolicySignature>(signatures, count),
                                  out.bytes, out.size) == PolicyError::None);
    return true;
}

struct Model
{
    u64           floor = 0U;
    bool          has   = false;
    PolicyContent current;
};

bool same(const PolicyContent& a, const PolicyContent& b)
{
    if (a.generation != b.generation || a.issued != b.issued || a.expires != b.expires ||
        a.offline_grace != b.offline_grace || a.minimum_count != b.minimum_count || a.rule_count != b.rule_count)
    {
        return false;
    }
    for (u32 i = 0U; i < a.minimum_count; ++i)
    {
        if (a.minimums[i].protocol != b.minimums[i].protocol || a.minimums[i].major != b.minimums[i].major ||
            a.minimums[i].minor != b.minimums[i].minor)
        {
            return false;
        }
    }
    for (u32 i = 0U; i < a.rule_count; ++i)
    {
        const AlgorithmRule& x = a.rules[i];
        const AlgorithmRule& y = b.rules[i];
        if (x.algorithm != y.algorithm || x.accepted_from != y.accepted_from || x.preferred_from != y.preferred_from ||
            x.preferred_until != y.preferred_until || x.refused_from != y.refused_from)
        {
            return false;
        }
    }
    return true;
}

PolicyError expected_apply(Model& model, const PolicyContent& c, u8 signers, u64 now)
{
    if (c.generation < model.floor)
    {
        return PolicyError::Rollback;
    }
    if ((signers & kPostQuantum) == 0U || (signers & kClassical) == 0U)
    {
        return PolicyError::MissingSignature;
    }
    if ((signers & kForged) != 0U)
    {
        return PolicyError::BadSignature;
    }
    if (model.has && c.generation == model.current.generation)
    {
        return same(c, model.current) ? PolicyError::AlreadyCurrent : PolicyError::Conflict;
    }
    if (c.issued > now + kMaxIssueSkew)
    {
        return PolicyError::NotYetIssued;
    }
    if (now >= c.expires + c.offline_grace)
    {
        return PolicyError::Expired;
    }
    if (model.has && c.issued < model.current.issued)
    {
        return PolicyError::IssuedBeforeCurrent;
    }
    model.floor   = c.generation;
    model.has     = true;
    model.current = c;
    return PolicyError::None;
}

AlgorithmStatus expected_status(const Model& model, usize drawn, u64 t)
{
    if (!model.has || t >= model.current.expires + model.current.offline_grace || !kDrawnSupported[drawn])
    {
        return AlgorithmStatus::Refused;
    }
    for (u32 i = 0U; i < model.current.rule_count; ++i)
    {
        const AlgorithmRule& r = model.current.rules[i];
        if (r.algorithm == kDrawn[drawn])
        {
            if (t < r.accepted_from || t >= r.refused_from)
            {
                return AlgorithmStatus::Refused;
            }
            return t >= r.preferred_from && t < r.preferred_until ? AlgorithmStatus::Preferred
                                                                  : AlgorithmStatus::Accepted;
        }
    }
    return AlgorithmStatus::Refused;
}

Freshness expected_freshness(const PolicyContent& c, u64 t)
{
    if (t < c.expires)
    {
        return Freshness::Current;
    }
    return t < c.expires + c.offline_grace ? Freshness::Grace : Freshness::Expired;
}

void check_snapshot(const Model& model, const PolicySnapshot& snapshot, u64 now)
{
    CRD_FUZZ_REQUIRE(snapshot.generation() == (model.has ? model.current.generation : 0U));
    if (!model.has)
    {
        CRD_FUZZ_REQUIRE(snapshot.freshness(now) == Freshness::None);
        return;
    }
    const PolicyContent& c = model.current;
    u64                  times[4 + 5 * kMaxPolicyRules] = {now, c.expires - 1U, c.expires,
                                                           c.expires + c.offline_grace - 1U};
    usize                count                          = 4U;
    for (u32 i = 0U; i < c.rule_count; ++i)
    {
        times[count++] = c.rules[i].accepted_from;
        times[count++] = c.rules[i].preferred_from;
        times[count++] = c.rules[i].preferred_until;
        times[count++] = c.rules[i].refused_from - 1U;
        times[count++] = c.rules[i].refused_from;
    }
    for (usize k = 0U; k < count; ++k)
    {
        const u64 t = times[k];
        CRD_FUZZ_REQUIRE(snapshot.freshness(t) == expected_freshness(c, t));
        for (usize a = 0U; a < kDrawnCount; ++a)
        {
            CRD_FUZZ_REQUIRE(snapshot.status(kDrawn[a], t) == expected_status(model, a, t));
        }
    }
    for (u16 protocol = 1U; protocol <= 3U; ++protocol)
    {
        for (u16 major = 0U; major < 3U; ++major)
        {
            for (u16 minor = 0U; minor < 3U; ++minor)
            {
                bool allowed = true;
                for (u32 i = 0U; i < c.minimum_count; ++i)
                {
                    if (c.minimums[i].protocol == protocol)
                    {
                        allowed = major > c.minimums[i].major ||
                                  (major == c.minimums[i].major && minor >= c.minimums[i].minor);
                    }
                }
                CRD_FUZZ_REQUIRE(snapshot.allows_version(protocol, major, minor) == allowed);
            }
        }
    }
}

void generated_sequence(const u8* data, usize size)
{
    Draw               draw(data, size);
    const TestVerifier verifier;
    Model              model;
    model.floor = draw.next() % 4U;
    PolicyStore store({ConstSpan<TrustKey>(kTestTrust), &verifier, ConstSpan<AlgorithmId>(kSupported), model.floor});
    for (u32 step = 0U; step < 8U && !draw.done(); ++step)
    {
        const PolicyContent content = draw_content(draw);
        const u8            signers = draw.next() % 8U;
        const u64           now     = kT0 + draw.next() * kHour + (draw.next() % 64U) * kDay;
        static Encoded      encoded;
        if (!encode(content, signers, encoded))
        {
            continue;
        }
        const PolicyError want = expected_apply(model, content, signers, now);
        CRD_FUZZ_REQUIRE(store.apply(ConstSpan<u8>(encoded.bytes, encoded.size), now) == want);
        CRD_FUZZ_REQUIRE(store.floor_generation() == model.floor);
        check_snapshot(model, store.snapshot(), now);
    }
}

void add(crd::fuzz::SeedSink& sink, const char* name, u8 mode, const u8* data, usize size)
{
    static u8 input[kMaxPolicyBytes + 1U];
    input[0] = mode;
    std::memcpy(input + 1U, data, size);
    sink.add(name, input, size + 1U);
}
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    if (size == 0U || size > crd::fuzz::kMaxInputBytes)
    {
        return 0;
    }
    if (data[0] % 2U == 0U)
    {
        decode_checked(data + 1U, size - 1U);
    }
    else
    {
        generated_sequence(data + 1U, size - 1U);
    }
    return 0;
}

void crd_fuzz_seeds(crd::fuzz::SeedSink& sink)
{
    // An honest generation from the writers, its truncation and single-field changes.
    PolicyContent content;
    content.generation    = 3U;
    content.issued        = kT0;
    content.expires       = kT0 + 30U * kDay;
    content.offline_grace = 7U * kDay;
    content.minimums[0]   = {2U, 1U, 0U};
    content.minimum_count = 1U;
    content.rules[0]      = {AlgorithmId::X25519Mlkem768, kT0, kT0, kNever, kNever};
    content.rules[1]      = {AlgorithmId::X25519, kT0, kT0, kT0 + kDay, kT0 + 2U * kDay};
    content.rule_count    = 2U;
    static Encoded honest;
    if (!encode(content, kPostQuantum | kClassical, honest))
    {
        return;
    }
    const usize body = policy_body_size(content);
    add(sink, "policy", 0U, honest.bytes, honest.size);
    add(sink, "policy-truncated", 0U, honest.bytes, honest.size - 5U);
    add(sink, "policy-trailing", 0U, honest.bytes, honest.size + 1U);
    static Encoded changed;
    changed = honest;
    changed.bytes[0] = 'c';
    add(sink, "policy-bad-magic", 0U, changed.bytes, changed.size);
    changed = honest;
    changed.bytes[8] = 2U;
    add(sink, "policy-future-format", 0U, changed.bytes, changed.size);
    changed = honest;
    changed.bytes[37] = 17U;
    add(sink, "policy-too-many-minimums", 0U, changed.bytes, changed.size);
    changed = honest;
    changed.bytes[25 + 7] = 0U; // the expiry's low byte: before the issue time
    changed.bytes[25 + 6] = 0U;
    changed.bytes[25 + 5] = 0U;
    add(sink, "policy-bad-dates", 0U, changed.bytes, changed.size);
    changed = honest;
    changed.bytes[body] = 0U;
    add(sink, "policy-unsigned", 0U, changed.bytes, body + 1U);
    changed = honest;
    std::memcpy(changed.bytes + body + 1U + 36U + kTestSignatureBytes, changed.bytes + body + 1U, kKeyIdBytes);
    add(sink, "policy-duplicate-signature", 0U, changed.bytes, changed.size);
    changed = honest;
    changed.bytes[body + 1U + kKeyIdBytes + 2U] = 0x20U; // the first signature's length: 8193 bytes
    changed.bytes[body + 1U + kKeyIdBytes + 3U] = 0x01U;
    add(sink, "policy-oversized-signature", 0U, changed.bytes, changed.size);

    // Generated sequences: honest growth, a rollback and a replay, a forged and a half-signed generation, an expired
    // one and one issued in the future.
    const u8 growth[] = {0U, 1U, 10U, 30U, 7U, 1U, 2U, 1U, 0U, 2U, 0U, 1U, 2U, 3U, 255U, 1U, 4U, 5U, 6U, 7U, 1U, 3U,
                         20U, 0U, 2U, 12U, 40U, 30U, 7U, 0U, 1U, 1U, 2U, 3U, 4U, 9U, 1U, 3U, 24U, 1U};
    add(sink, "sequence-growth", 1U, growth, sizeof(growth));
    const u8 rollback[] = {2U, 3U, 0U, 30U, 7U, 0U, 0U, 3U, 0U, 0U, 1U, 5U, 0U, 30U, 7U, 0U, 0U, 3U, 0U, 0U,
                           3U, 0U, 30U, 7U, 0U, 1U, 2U, 0U, 1U, 1U, 1U, 1U, 1U, 3U, 0U, 0U};
    add(sink, "sequence-rollback-replay", 1U, rollback, sizeof(rollback));
    const u8 forged[] = {0U, 1U, 0U, 30U, 7U, 0U, 0U, 7U, 0U, 0U, 1U, 0U, 30U, 7U, 0U, 0U, 1U, 0U, 0U};
    add(sink, "sequence-forged-half-signed", 1U, forged, sizeof(forged));
    const u8 dated[] = {0U, 1U, 0U, 1U, 0U, 0U, 0U, 3U, 200U, 60U, 2U, 250U, 30U, 7U, 0U, 0U, 3U, 0U, 0U};
    add(sink, "sequence-expired-future", 1U, dated, sizeof(dated));

    // A whole lifecycle, one step per line (content, then signers and arrival): generation 1 adopted with a rule for
    // each drawn algorithm the build supports and one it does not; generation 1 again with other content (conflict);
    // generation 2 disabling ChaCha20-Poly1305 an hour after issue; generation 3 issued before 2; a forged generation
    // 3; generation 4 adopted in its grace period; generation 5 already past its grace period; generation 1 (rollback).
    const u8 lifecycle[] = {
        0U,
        1U, 0U, 30U, 7U, 1U, 1U, 1U, 0U, 4U, 0U, 0U, 0U, 24U, 255U, 1U, 1U, 0U, 0U, 5U, 10U, 1U,
        2U, 0U, 48U, 48U, 255U, 1U, 3U, 0U, 0U, 0U, 255U, 1U, 3U, 1U, 0U,
        1U, 0U, 30U, 6U, 0U, 0U, 3U, 2U, 0U,
        2U, 2U, 30U, 7U, 0U, 2U, 2U, 0U, 0U, 0U, 1U, 1U, 0U, 0U, 0U, 255U, 255U, 1U, 3U, 3U, 0U,
        3U, 1U, 30U, 7U, 0U, 0U, 3U, 4U, 0U,
        3U, 5U, 30U, 7U, 0U, 0U, 7U, 5U, 0U,
        4U, 6U, 1U, 7U, 0U, 1U, 1U, 0U, 0U, 5U, 10U, 1U, 3U, 6U, 3U,
        5U, 7U, 1U, 1U, 0U, 0U, 3U, 7U, 5U,
        1U, 0U, 30U, 7U, 0U, 0U, 3U, 8U, 5U,
    };
    add(sink, "sequence-lifecycle", 1U, lifecycle, sizeof(lifecycle));
}
