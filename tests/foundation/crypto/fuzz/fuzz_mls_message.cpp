// crd-fuzz-crypto-mls-message: MLS's message structures, framing and protection (NET.2;
// docs/design/network-foundation.md#mls) under the bounded fuzz harness (REPO.DEV.9; docs/design/test-instruments.md).
//
// Input. The first byte picks the mode, the rest is its data, read field by field (a short input ends the case):
//   0  decoding: the rest, fed to every decoder (an MLSMessage, a Proposal, a Commit, a KeyPackage, a LeafNode, a
//      FramedContent and an AuthenticatedContent);
//   1  a private message changed: the suite, the leaf count's exponent (mod 4) and the sender's leaf, the content type
//      (mod 3), the padding (u8), the byte to change (u16; past the end changes none) and its XOR mask, then the
//      application data (a proposal and a commit carry fixed bodies);
//   2  a hostile private message: the suite, then the bytes of an MLSMessage opened in a two-leaf group;
//   3  a public message changed: the suite, the sender (mod 4), the content type (bit 0: a member's commit; a new
//      member's commit always commits, the other senders propose), the byte to change (u16) and its mask, then the
//      authenticated data.
//
// Oracles, none of them the code's verdict on the same call:
//   * whatever a decoder accepts writes back to exactly the bytes it took, its size function agrees, and a private
//     message's header is the prefix of its encoding a restatement of section 6.3 marks;
//   * a private message opens to what was protected, under a receiver's own tree; changed in any byte it is refused,
//     its plaintext zeroed, and the honest message still opens afterwards (a refusal spends nothing); opened twice it
//     is Replayed;
//   * no hostile private message opens (its seals cannot be forged), and every leaf's first generation stays
//     receivable after it;
//   * a public message verifies (framing, membership tag, signature) exactly when it is the one protected; changed in
//     any byte, it does not decode, or its tag or its signature fails.
#include <crd/fuzz/harness.hpp>

#include <crd/crypto/mls_message.hpp>

#include <algorithm>
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
using crd::containers::WireReader;
using crd::containers::WireWriter;
using namespace crd::crypto;
namespace mls = crd::crypto::mls;

using mls::ContentType;
using mls::SenderType;
using mls::WireFormat;

constexpr usize kBufferBytes = 4096U;

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

    ConstSpan<u8> rest() noexcept
    {
        const ConstSpan<u8> out(m_data + m_at, m_size - m_at);
        m_at = m_size;
        return out;
    }

private:
    const u8* m_data;
    usize     m_size;
    usize     m_at = 0U;
    bool      m_ok = true;
};

bool same(ConstSpan<u8> a, ConstSpan<u8> b) noexcept
{
    return a.size() == b.size() && (a.empty() || std::memcmp(a.data(), b.data(), a.size()) == 0);
}

bool all_zero(ConstSpan<u8> bytes) noexcept
{
    return std::ranges::all_of(bytes, [](u8 b) { return b == 0U; });
}

const mls::CipherSuite& suite_of(u8 selector) noexcept
{
    const ConstSpan<mls::CipherSuite> suites = mls::cipher_suites();
    return suites[selector % suites.size()];
}

u8 g_buffer[kBufferBytes];
u8 g_copy[kBufferBytes];
u8 g_plain[kBufferBytes];

// ---- Mode 0: every decoder's acceptance writes back ----

template <typename T, typename Take, typename Size, typename Put>
void writes_back(ConstSpan<u8> bytes, Take take, Size size, Put put) noexcept
{
    WireReader in(bytes);
    T          value{};
    if (!take(in, value))
    {
        return;
    }
    const ConstSpan<u8> taken = bytes.first(bytes.size() - in.rest().size());
    CRD_FUZZ_REQUIRE(size(value) == taken.size());
    WireWriter writer(Span<u8>(g_buffer, taken.size()));
    put(writer, value);
    CRD_FUZZ_REQUIRE(same(ConstSpan<u8>(g_buffer, taken.size()), taken));
}

void decoding(Reader& in) noexcept
{
    const ConstSpan<u8> bytes = in.rest();
    if (bytes.size() > kBufferBytes)
    {
        return;
    }
    mls::Message message;
    if (mls::take_message(bytes, message))
    {
        CRD_FUZZ_REQUIRE(mls::message_bytes(message) == bytes.size());
        WireWriter writer(Span<u8>(g_buffer, bytes.size()));
        mls::put_message(writer, message);
        CRD_FUZZ_REQUIRE(same(ConstSpan<u8>(g_buffer, bytes.size()), bytes));
        if (message.wire_format == WireFormat::PrivateMessage)
        {
            // The header restated: the group id's vector, the epoch, the content type, the authenticated data's.
            const mls::PrivateMessage& priv = message.private_message;
            const usize                aad  = mls::vector_bytes(priv.group_id.size()) + 9U;
            CRD_FUZZ_REQUIRE(priv.sender_data_aad_bytes == aad);
            const usize header = aad + mls::vector_bytes(priv.authenticated_data.size());
            CRD_FUZZ_REQUIRE(same(priv.header, bytes.subspan(4U, header)));
        }
    }
    writes_back<mls::Proposal>(bytes, mls::take_proposal, mls::proposal_bytes, mls::put_proposal);
    writes_back<mls::Commit>(bytes, mls::take_commit, mls::commit_bytes, mls::put_commit);
    writes_back<mls::KeyPackage>(bytes, mls::take_key_package, mls::key_package_bytes, mls::put_key_package);
    writes_back<mls::LeafNode>(bytes, mls::take_leaf_node, mls::leaf_node_bytes, mls::put_leaf_node);
    writes_back<mls::FramedContent>(bytes, mls::take_framed_content, mls::framed_content_bytes,
                                    mls::put_framed_content);
    WireReader         reader(bytes);
    WireFormat         format = WireFormat::Welcome;
    mls::FramedContent content;
    mls::ContentAuth   auth;
    if (mls::take_authenticated_content(reader, format, content, auth))
    {
        const usize taken = bytes.size() - reader.rest().size();
        CRD_FUZZ_REQUIRE(mls::authenticated_content_bytes(content, auth) == taken);
        WireWriter writer(Span<u8>(g_buffer, taken));
        mls::put_authenticated_content(writer, format, content, auth);
        CRD_FUZZ_REQUIRE(same(ConstSpan<u8>(g_buffer, taken), bytes.first(taken)));
    }
}

// ---- Mode 1: a private message changed in one byte ----

const u8 kGroup[]    = {'f', 'u', 'z', 'z'};
const u8 kRemove[]   = {0U, 3U, 0U, 0U, 0U, 1U};
const u8 kCommit[]   = {0U, 0U};
const u8 kSignature[] = {0x5AU, 0xA5U, 0x01U};

struct Group
{
    u8              secret[mls::kMaxHashBytes]      = {};
    u8              sender_data[mls::kMaxHashBytes] = {};
    usize           hash                            = 0U;

    explicit Group(const mls::CipherSuite& suite) noexcept : hash(suite.hash_bytes())
    {
        std::memset(secret, 0x21, sizeof(secret));
        std::memset(sender_data, 0x22, sizeof(sender_data));
    }
    [[nodiscard]] ConstSpan<u8> encryption() const noexcept { return ConstSpan<u8>(secret, hash); }
    [[nodiscard]] ConstSpan<u8> sender() const noexcept { return ConstSpan<u8>(sender_data, hash); }
};

// The body a content of `type` carries: the application data, or a fixed Remove proposal or empty commit.
ConstSpan<u8> body_of(ContentType type, ConstSpan<u8> data) noexcept
{
    if (type == ContentType::Application)
    {
        return data;
    }
    return type == ContentType::Proposal ? ConstSpan<u8>(kRemove) : ConstSpan<u8>(kCommit);
}

CryptoError open(const mls::CipherSuite& suite, mls::SecretTree& tree, const Group& group, ConstSpan<u8> bytes,
                 mls::FramedContent& content, mls::ContentAuth& auth, bool& decoded) noexcept
{
    mls::Message message;
    decoded = mls::take_message(bytes, message) && message.wire_format == WireFormat::PrivateMessage;
    if (!decoded || message.private_message.ciphertext.size() < kAeadTagBytes)
    {
        decoded = false;
        return CryptoError::Malformed;
    }
    const usize plain = message.private_message.ciphertext.size() - kAeadTagBytes;
    std::memset(g_plain, 0xEE, plain);
    const CryptoError verdict =
        mls::open_private(suite, tree, group.sender(), message.private_message, Span<u8>(g_plain, plain), content,
                          auth);
    if (verdict != CryptoError::None)
    {
        CRD_FUZZ_REQUIRE(all_zero(ConstSpan<u8>(g_plain, plain)));
    }
    return verdict;
}

void private_changed(Reader& in) noexcept
{
    const mls::CipherSuite& suite   = suite_of(in.byte());
    const u32               leaves  = u32{1} << (in.byte() % 4U);
    const u32               leaf    = in.byte() % leaves;
    const auto              type    = static_cast<ContentType>(1U + in.byte() % 3U);
    const usize             padding = in.byte();
    const usize             at      = in.u16be();
    const u8                mask    = in.byte();
    const ConstSpan<u8>     data    = in.rest();
    if (!in.ok() || data.size() > 1024U)
    {
        return;
    }
    const Group                group(suite);
    const ConstSpan<u8>        body = body_of(type, data);
    u8                         tag[mls::kMaxHashBytes] = {};
    const ConstSpan<u8>        aad  = data.first(data.size() / 2U);
    const mls::FramedContent   content{kGroup, 3U, {SenderType::Member, leaf}, aad, type, body};
    const mls::ContentAuth     auth{kSignature, type == ContentType::Commit ? ConstSpan<u8>(tag, group.hash)
                                                                            : ConstSpan<u8>()};
    const usize                bytes = mls::private_message_bytes(content, auth, padding);
    mls::SecretTree            sender;
    mls::SecretTree            receiver;
    CRD_FUZZ_REQUIRE(bytes != 0U && bytes <= kBufferBytes);
    CRD_FUZZ_REQUIRE(sender.init(suite, group.encryption(), leaves) == CryptoError::None);
    CRD_FUZZ_REQUIRE(receiver.init(suite, group.encryption(), leaves) == CryptoError::None);
    CRD_FUZZ_REQUIRE(mls::protect_private(suite, sender, group.sender(), content, auth, padding,
                                          Span<u8>(g_buffer, bytes)) == CryptoError::None);
    std::memcpy(g_copy, g_buffer, bytes);

    mls::FramedContent opened;
    mls::ContentAuth   opened_auth;
    bool               decoded = false;
    if (at < bytes && mask != 0U)
    {
        g_copy[at] ^= mask;
        const CryptoError verdict = open(suite, receiver, group, ConstSpan<u8>(g_copy, bytes), opened, opened_auth,
                                         decoded);
        CRD_FUZZ_REQUIRE(verdict != CryptoError::None);
    }
    // The honest message opens (nothing was spent), to what was protected; then it is Replayed.
    CRD_FUZZ_REQUIRE(open(suite, receiver, group, ConstSpan<u8>(g_buffer, bytes), opened, opened_auth, decoded) ==
                     CryptoError::None);
    CRD_FUZZ_REQUIRE(same(opened.body, body));
    CRD_FUZZ_REQUIRE(same(opened.authenticated_data, content.authenticated_data));
    CRD_FUZZ_REQUIRE(same(opened_auth.signature, auth.signature));
    CRD_FUZZ_REQUIRE(same(opened_auth.confirmation_tag, auth.confirmation_tag));
    CRD_FUZZ_REQUIRE(opened.sender.index == leaf && opened.content_type == type && opened.epoch == 3U);
    CRD_FUZZ_REQUIRE(open(suite, receiver, group, ConstSpan<u8>(g_buffer, bytes), opened, opened_auth, decoded) ==
                     CryptoError::Replayed);
}

// ---- Mode 2: hostile private messages ----

void private_hostile(Reader& in) noexcept
{
    const mls::CipherSuite& suite = suite_of(in.byte());
    const ConstSpan<u8>     bytes = in.rest();
    if (!in.ok() || bytes.size() > kBufferBytes)
    {
        return;
    }
    const Group     group(suite);
    mls::SecretTree tree;
    CRD_FUZZ_REQUIRE(tree.init(suite, group.encryption(), 2U) == CryptoError::None);
    std::memcpy(g_copy, bytes.data(), bytes.size());
    mls::FramedContent content;
    mls::ContentAuth   auth;
    bool               decoded = false;
    CRD_FUZZ_REQUIRE(open(suite, tree, group, ConstSpan<u8>(g_copy, bytes.size()), content, auth, decoded) !=
                     CryptoError::None);
    CRD_FUZZ_REQUIRE(content.body.empty() && auth.signature.empty());
    for (u32 leaf = 0U; leaf < 2U; ++leaf)
    {
        for (const mls::RatchetKind kind : {mls::RatchetKind::Handshake, mls::RatchetKind::Application})
        {
            mls::MessageKey key;
            CRD_FUZZ_REQUIRE(tree.receive_key(leaf, kind, 0U, key) == CryptoError::None);
        }
    }
}

// ---- Mode 3: a public message changed in one byte ----

class Signers
{
public:
    Signers() noexcept
    {
        u8 seed[64];
        std::memset(seed, 0x13, sizeof(seed));
        for (usize i = 0U; i < kSuites; ++i)
        {
            const mls::CipherSuite& suite = mls::cipher_suites()[i];
            const usize             bytes = signature_sizes(suite.signature).seed;
            CRD_FUZZ_REQUIRE(m_keys[i].from_seed(suite.signature, ConstSpan<u8>(seed, bytes)) == CryptoError::None);
            m_public_bytes[i] = signature_sizes(suite.signature).public_key;
            CRD_FUZZ_REQUIRE(m_keys[i].public_key(Span<u8>(m_public[i], m_public_bytes[i])) == CryptoError::None);
        }
    }

    [[nodiscard]] const SigningKey& key(usize i) const noexcept { return m_keys[i]; }
    [[nodiscard]] ConstSpan<u8>     public_key(usize i) const noexcept
    {
        return ConstSpan<u8>(m_public[i], m_public_bytes[i]);
    }

private:
    static constexpr usize kSuites = 4U;
    SigningKey             m_keys[kSuites];
    u8                     m_public[kSuites][160]{};
    usize                  m_public_bytes[kSuites]{};
};

void public_changed(Reader& in) noexcept
{
    static const Signers          kSigners;
    static const ProviderVerifier kVerifier;
    const u8                      selector = in.byte();
    const mls::CipherSuite&       suite    = suite_of(selector);
    const usize                   index    = selector % mls::cipher_suites().size();
    constexpr SenderType          senders[] = {SenderType::Member, SenderType::External, SenderType::NewMemberProposal,
                                               SenderType::NewMemberCommit};
    const SenderType              sender   = senders[in.byte() % 4U];
    const u8                      kind     = in.byte();
    const bool                    commit   = sender == SenderType::NewMemberCommit ||
                         (sender == SenderType::Member && (kind & 1U) != 0U);
    const usize         at   = in.u16be();
    const u8            mask = in.byte();
    const ConstSpan<u8> aad  = in.rest();
    if (!in.ok() || aad.size() > 1024U)
    {
        return;
    }
    const Group             group(suite);
    u8                      tree_hash[4] = {1U, 2U, 3U, 4U};
    const mls::GroupContext context{suite.id, kGroup, 3U, tree_hash, tree_hash, {}};
    const u32               leaf = sender == SenderType::Member || sender == SenderType::External ? 1U : 0U;
    const mls::FramedContent content{kGroup, 3U, {sender, leaf}, aad,
                                     commit ? ContentType::Commit : ContentType::Proposal,
                                     commit ? ConstSpan<u8>(kCommit) : ConstSpan<u8>(kRemove)};
    u8                      signature[160] = {};
    usize                   signed_bytes   = 0U;
    CRD_FUZZ_REQUIRE(mls::sign_content(suite, kSigners.key(index), WireFormat::PublicMessage, content, context,
                                       signature, signed_bytes) == CryptoError::None);
    u8                     tag[mls::kMaxHashBytes] = {};
    const mls::ContentAuth auth{ConstSpan<u8>(signature, signed_bytes),
                                commit ? ConstSpan<u8>(tag, group.hash) : ConstSpan<u8>()};
    usize                  bytes = 0U;
    CRD_FUZZ_REQUIRE(mls::protect_public(suite, group.sender(), context, content, auth, g_buffer, bytes) ==
                     CryptoError::None);
    const auto verifies = [&](ConstSpan<u8> message_bytes)
    {
        mls::Message message;
        return mls::take_message(message_bytes, message) && message.wire_format == WireFormat::PublicMessage &&
               mls::verify_public(suite, group.sender(), context, message.public_message) == CryptoError::None &&
               mls::verify_content(suite, kVerifier, kSigners.public_key(index), WireFormat::PublicMessage,
                                   message.public_message.content, context,
                                   message.public_message.auth.signature) == Verification::Valid;
    };
    CRD_FUZZ_REQUIRE(verifies(ConstSpan<u8>(g_buffer, bytes)));
    if (at < bytes && mask != 0U)
    {
        std::memcpy(g_copy, g_buffer, bytes);
        g_copy[at] ^= mask;
        CRD_FUZZ_REQUIRE(!verifies(ConstSpan<u8>(g_copy, bytes)));
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
    switch (data[0] % 4U)
    {
    case 0U:
        decoding(in);
        break;
    case 1U:
        private_changed(in);
        break;
    case 2U:
        private_hostile(in);
        break;
    default:
        public_changed(in);
        break;
    }
    return 0;
}

void crd_fuzz_seeds(crd::fuzz::SeedSink& sink)
{
    // Decoding: a member's public Remove proposal, an empty commit, a leaf node and a private message's shape.
    const u8 public_remove[] = {0U, 0U, 1U, 0U, 1U, 1U, 'g', 0U, 0U, 0U, 0U, 0U, 0U, 0U, 1U, 1U, 0U, 0U, 0U,
                                0U, 0U, 2U, 0U, 3U, 0U, 0U, 0U, 1U, 0U, 0U};
    add(sink, "decode-public-remove", public_remove, sizeof(public_remove));
    const u8 commit[] = {0U, 3U, 2U, 1U, 0xEEU, 0U};
    add(sink, "decode-commit-reference", commit, sizeof(commit));
    const u8 leaf[] = {0U, 1U, 'e', 1U, 's', 0U, 2U, 3U, 2U, 'c', 'd', 2U, 0U, 1U, 0U, 0U, 0U, 0U, 3U, 1U, 'p',
                       4U, 0U, 1U, 1U, 'x', 1U, 0U};
    add(sink, "decode-leaf-commit", leaf, sizeof(leaf));
    const u8 private_shape[] = {0U, 0U, 1U, 0U, 2U, 1U, 'g', 0U, 0U, 0U, 0U, 0U, 0U, 0U, 1U, 1U, 0U, 0U, 0U};
    add(sink, "decode-private", private_shape, sizeof(private_shape));

    // A private message of each content type in each suite, unchanged and with one byte changed.
    for (u8 suite = 0U; suite < 4U; ++suite)
    {
        for (u8 type = 0U; type < 3U; ++type)
        {
            const u8 honest[] = {1U, suite, 2U, 3U, type, 7U, 0xFFU, 0xFFU, 0U, 'd', 'a', 't', 'a'};
            const char* const names[3][4] = {
                {"private-application-0", "private-application-1", "private-application-2", "private-application-3"},
                {"private-proposal-0", "private-proposal-1", "private-proposal-2", "private-proposal-3"},
                {"private-commit-0", "private-commit-1", "private-commit-2", "private-commit-3"},
            };
            add(sink, names[type][suite], honest, sizeof(honest));
        }
        const char* const changed[] = {"private-changed-0", "private-changed-1", "private-changed-2",
                                       "private-changed-3"};
        const u8          flipped[] = {1U, suite, 1U, 1U, 0U, 0U, 0U, 20U, 0x01U, 'x'};
        add(sink, changed[suite], flipped, sizeof(flipped));
    }

    // Hostile private messages: a well-formed one whose seals are all zero, and one whose sender data is too short.
    u8       zero_seals[80] = {};
    const u8 head[]         = {2U, 0U, 0U, 1U, 0U, 2U, 1U, 'g', 0U, 0U, 0U, 0U, 0U, 0U, 0U, 1U, 1U, 0U};
    usize    n              = sizeof(head);
    std::memcpy(zero_seals, head, n);
    zero_seals[n++] = static_cast<u8>(mls::kSealedSenderDataBytes);
    n += mls::kSealedSenderDataBytes;
    zero_seals[n++] = static_cast<u8>(kAeadTagBytes + 1U);
    n += kAeadTagBytes + 1U;
    add(sink, "hostile-zero-seals", zero_seals, n);
    const u8 short_sender[] = {2U, 1U, 0U, 1U, 0U, 2U, 1U, 'g', 0U, 0U, 0U, 0U, 0U, 0U, 0U, 1U, 1U, 0U, 1U, 0U, 0U};
    add(sink, "hostile-short-sender-data", short_sender, sizeof(short_sender));

    // Public messages from each sender, unchanged and changed in the signature's region.
    for (u8 sender = 0U; sender < 4U; ++sender)
    {
        const char* const honest_names[] = {"public-member", "public-external", "public-new-member-proposal",
                                            "public-new-member-commit"};
        const u8          honest[]       = {3U, sender, sender, 1U, 0xFFU, 0xFFU, 0U, 'a'};
        add(sink, honest_names[sender], honest, sizeof(honest));
        const char* const changed_names[] = {"public-member-changed", "public-external-changed",
                                             "public-new-member-proposal-changed", "public-new-member-commit-changed"};
        const u8          changed[]       = {3U, sender, sender, 1U, 0U, 30U, 0x80U, 'a'};
        add(sink, changed_names[sender], changed, sizeof(changed));
    }
}
