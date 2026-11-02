// crd-crypto -- MLS messages, RFC 9420 sections 6, 7.2, 8.2, 10 and 12 (NET.2; docs/design/network-foundation.md#mls),
// against the MLS working group's test vectors (the pinned mlswg/mls-implementations test-vectors, read at run time):
// every transcript-hashes vector (the commit's AuthenticatedContent, its confirmation tag and both hashes), every
// message-protection vector (each public message verified and protected again, byte for byte where the suite signs
// deterministically; each private message opened, then protected and opened again) in the suites of the profile, and
// every messages vector's key package, proposals, commit and public and private messages decoded and written back.
// Then PrivateMessage against a restatement of section 6.3 (both directions, the padding rule, hostile and replayed
// messages), the framing rules of section 6, Padme, and every refusal.

#include "wycheproof.hpp"

#include <crd/crypto/mls_message.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <iterator>

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
using namespace crd::crypto::test;
namespace json = crd::assetio::json;
namespace mls  = crd::crypto::mls;

using mls::ContentType;
using mls::SenderType;
using mls::WireFormat;

JsonFile mls_vectors(const char* name)
{
    return JsonFile(vector_path(CRD_MLS_VECTORS_DIR, name));
}

Span<u8> span_of(Bytes& bytes)
{
    return Span<u8>(bytes.data(), bytes.size());
}

Bytes copy_of(ConstSpan<u8> bytes)
{
    return Bytes(bytes.begin(), bytes.end());
}

Bytes joined(ConstSpan<u8> a, ConstSpan<u8> b)
{
    Bytes out = copy_of(a);
    for (const u8 byte : b)
    {
        out.push_back(byte);
    }
    return out;
}

// The suite of vector `v`, or null (with the check that it is one of the three outside the profile).
const mls::CipherSuite* profile_suite(const JsonFile& vectors, u32 v)
{
    const u16               id    = static_cast<u16>(vectors.number(v, "cipher_suite"));
    const mls::CipherSuite* suite = mls::cipher_suite(id);
    if (suite == nullptr)
    {
        CHECK((id >= 4U && id <= 6U)); // X448, Ed448 and P-521: outside the profile
    }
    return suite;
}

// A decoded message written back.
Bytes encoded(const mls::Message& message)
{
    Bytes out = filled(mls::message_bytes(message), 0U);
    REQUIRE(!out.empty());
    WireWriter writer(span_of(out));
    mls::put_message(writer, message);
    REQUIRE(writer.written() == out.size());
    return out;
}

mls::Message decoded(ConstSpan<u8> bytes)
{
    mls::Message message;
    REQUIRE(mls::take_message(bytes, message));
    CHECK(equal(encoded(message), bytes));
    return message;
}

template <typename T, typename Size, typename Put> Bytes written_back(const T& value, Size size, Put put)
{
    Bytes out = filled(size(value), 0U);
    REQUIRE(!out.empty());
    WireWriter writer(span_of(out));
    put(writer, value);
    REQUIRE(writer.written() == out.size());
    return out;
}

Bytes commit_written(const mls::Commit& commit)
{
    return written_back(commit, mls::commit_bytes, mls::put_commit);
}

// A structure decoded from the whole of `bytes` and written back to them.
template <typename T, typename Take, typename Size, typename Put>
T round_trip(ConstSpan<u8> bytes, Take take, Size size, Put put)
{
    WireReader in(bytes);
    T          value{};
    REQUIRE(take(in, value));
    CHECK(in.done());
    CHECK(equal(written_back(value, size, put), bytes));
    return value;
}

// One signer of a suite and its public key.
struct Member
{
    SigningKey key;
    Bytes      public_key;

    Member(const mls::CipherSuite& suite, ConstSpan<u8> seed)
    {
        REQUIRE(key.from_seed(suite.signature, seed) == CryptoError::None);
        public_key = filled(signature_sizes(suite.signature).public_key, 0U);
        REQUIRE(key.public_key(span_of(public_key)) == CryptoError::None);
    }
};

Bytes sign(const mls::CipherSuite& suite, const Member& member, WireFormat format, const mls::FramedContent& content,
           const mls::GroupContext& context)
{
    Bytes signature = filled(signature_sizes(suite.signature).signature, 0U);
    usize written   = 0U;
    REQUIRE(mls::sign_content(suite, member.key, format, content, context, span_of(signature), written) ==
            CryptoError::None);
    signature.resize(written);
    return signature;
}

Bytes protect_private(const mls::CipherSuite& suite, mls::SecretTree& tree, ConstSpan<u8> sender_data_secret,
                      const mls::FramedContent& content, const mls::ContentAuth& auth, usize padding)
{
    Bytes out = filled(mls::private_message_bytes(content, auth, padding), 0U);
    REQUIRE(!out.empty());
    REQUIRE(mls::protect_private(suite, tree, sender_data_secret, content, auth, padding, span_of(out)) ==
            CryptoError::None);
    return out;
}

// Opens a private message into a copy of its ciphertext's bytes (so `plaintext` outlives the call).
CryptoError open_copy(const mls::CipherSuite& suite, mls::SecretTree& tree, ConstSpan<u8> sender_data_secret,
                      const mls::Message& message, Bytes& plaintext, mls::FramedContent& content,
                      mls::ContentAuth& auth)
{
    REQUIRE(message.wire_format == WireFormat::PrivateMessage);
    plaintext = filled(message.private_message.ciphertext.size() - kAeadTagBytes, 0xEEU);
    return mls::open_private(suite, tree, sender_data_secret, message.private_message, span_of(plaintext), content,
                             auth);
}

mls::SecretTree secret_tree(const mls::CipherSuite& suite, ConstSpan<u8> encryption_secret, u32 leaves)
{
    mls::SecretTree tree;
    REQUIRE(tree.init(suite, encryption_secret, leaves) == CryptoError::None);
    return tree;
}

// ---- Section 6.3 restated, apart from protect_private and open_private ----

// What a restated PrivateMessage carries: the sender data and the plaintext, which the restatement does not parse.
struct Restated
{
    u32   leaf       = 0U;
    u32   generation = 0U;
    Bytes guard;
    Bytes plaintext;
};

void put_header(WireWriter& out, ConstSpan<u8> group_id, u64 epoch, ContentType type, ConstSpan<u8> aad)
{
    mls::put_vector(out, group_id);
    out.put_u64(epoch);
    out.put_u8(static_cast<u8>(type));
    mls::put_vector(out, aad);
}

// Seals `plaintext` (any bytes) from `leaf` at the tree's next generation under the section's layout, with `guard`.
Bytes restated_protect(const mls::CipherSuite& suite, mls::SecretTree& tree, ConstSpan<u8> sender_data_secret,
                       ConstSpan<u8> group_id, u64 epoch, ContentType type, ConstSpan<u8> aad, u32 leaf,
                       ConstSpan<u8> guard, ConstSpan<u8> plaintext)
{
    mls::MessageKey key;
    const auto kind = type == ContentType::Application ? mls::RatchetKind::Application : mls::RatchetKind::Handshake;
    REQUIRE(tree.send_key(leaf, kind, key) == CryptoError::None);
    Bytes nonce = copy_of(ConstSpan<u8>(key.nonce, kAeadNonceBytes));
    for (usize i = 0U; i < 4U; ++i)
    {
        nonce[i] ^= guard[i];
    }
    Bytes      header = filled(mls::vector_bytes(group_id.size()) + 9U + mls::vector_bytes(aad.size()), 0U);
    WireWriter header_writer(span_of(header));
    put_header(header_writer, group_id, epoch, type, aad);
    AeadKey content_key;
    REQUIRE(content_key.init(suite.aead, key.aead_key()) == CryptoError::None);
    Bytes ciphertext = filled(plaintext.size() + kAeadTagBytes, 0U);
    REQUIRE(content_key.seal(nonce, plaintext, header, span_of(ciphertext)) == CryptoError::None);

    Bytes sender_key   = filled(aead_key_bytes(suite.aead), 0U);
    Bytes sender_nonce = filled(kAeadNonceBytes, 0U);
    REQUIRE(mls::sender_data_key(suite, sender_data_secret, ciphertext, span_of(sender_key), span_of(sender_nonce)) ==
            CryptoError::None);
    u8         fields[12] = {};
    WireWriter data(fields);
    data.put_u32(leaf);
    data.put_u32(key.generation);
    data.put_bytes(guard);
    AeadKey sealer;
    REQUIRE(sealer.init(suite.aead, sender_key) == CryptoError::None);
    Bytes      sender_data = filled(28U, 0U);
    const auto sender_aad  = ConstSpan<u8>(header.data(), mls::vector_bytes(group_id.size()) + 9U);
    REQUIRE(sealer.seal(sender_nonce, fields, sender_aad, span_of(sender_data)) == CryptoError::None);

    Bytes out = filled(4U + header.size() + mls::vector_bytes(sender_data.size()) +
                           mls::vector_bytes(ciphertext.size()),
                       0U);
    WireWriter writer(span_of(out));
    writer.put_u16(1U);
    writer.put_u16(2U);
    writer.put_bytes(header);
    mls::put_vector(writer, sender_data);
    mls::put_vector(writer, ciphertext);
    return out;
}

// Opens a message protect_private wrote, under the same restatement; the tree is read and never spent.
Restated restated_open(const mls::CipherSuite& suite, const mls::SecretTree& tree, ConstSpan<u8> sender_data_secret,
                       ConstSpan<u8> bytes)
{
    WireReader in(bytes);
    u16        version = 0U;
    u16        format  = 0U;
    REQUIRE((in.take_u16(version) && version == 1U && in.take_u16(format) && format == 2U));
    const ConstSpan<u8> start = in.rest();
    ConstSpan<u8>       group_id;
    ConstSpan<u8>       aad;
    u64                 epoch = 0U;
    u8                  type  = 0U;
    REQUIRE((mls::take_vector(in, group_id) && in.take_u64(epoch) && in.take_u8(type)));
    const usize sender_aad = start.size() - in.rest().size();
    REQUIRE(mls::take_vector(in, aad));
    const ConstSpan<u8> header = start.first(start.size() - in.rest().size());
    ConstSpan<u8>       sender_data;
    ConstSpan<u8>       ciphertext;
    REQUIRE((mls::take_vector(in, sender_data) && mls::take_vector(in, ciphertext) && in.done()));

    Bytes sender_key   = filled(aead_key_bytes(suite.aead), 0U);
    Bytes sender_nonce = filled(kAeadNonceBytes, 0U);
    REQUIRE(mls::sender_data_key(suite, sender_data_secret, ciphertext, span_of(sender_key), span_of(sender_nonce)) ==
            CryptoError::None);
    AeadKey opener;
    REQUIRE(opener.init(suite.aead, sender_key) == CryptoError::None);
    u8 fields[12] = {};
    REQUIRE(opener.open(sender_nonce, sender_data, header.first(sender_aad), fields) == CryptoError::None);
    Restated   out;
    WireReader data(fields);
    REQUIRE((data.take_u32(out.leaf) && data.take_u32(out.generation)));
    out.guard = copy_of(ConstSpan<u8>(fields + 8U, 4U));

    mls::MessageKey key;
    const auto      kind = type == 1U ? mls::RatchetKind::Application : mls::RatchetKind::Handshake;
    REQUIRE(tree.receive_key(out.leaf, kind, out.generation, key) == CryptoError::None);
    Bytes nonce = copy_of(ConstSpan<u8>(key.nonce, kAeadNonceBytes));
    for (usize i = 0U; i < 4U; ++i)
    {
        nonce[i] ^= out.guard[i];
    }
    AeadKey content_key;
    REQUIRE(content_key.init(suite.aead, key.aead_key()) == CryptoError::None);
    out.plaintext = filled(ciphertext.size() - kAeadTagBytes, 0U);
    REQUIRE(content_key.open(nonce, ciphertext, header, span_of(out.plaintext)) == CryptoError::None);
    return out;
}

const u8 kGroup[]     = {'g', 'r', 'o', 'u', 'p'};
const u8 kAad[]       = {'a', 'a', 'd'};
const u8 kTreeHash[]  = {1U, 2U, 3U};
const u8 kConfirmed[] = {4U, 5U, 6U};

mls::GroupContext test_context(const mls::CipherSuite& suite)
{
    return {suite.id, kGroup, 7U, kTreeHash, kConfirmed, {}};
}

mls::FramedContent application(ConstSpan<u8> data, u32 leaf = 1U)
{
    return {kGroup, 7U, {SenderType::Member, leaf}, kAad, ContentType::Application, data};
}
} // namespace

// ---- The working group's vectors ----

TEST_CASE("MLS: every transcript-hashes vector of the profile", "[crypto][net-2][mls]")
{
    const JsonFile vectors = mls_vectors("transcript-hashes.json");
    const u32      count   = json::count_of(vectors.doc(), vectors.root());
    usize          checked = 0U;
    REQUIRE(count == 7U);
    for (u32 i = 0U; i < count; ++i)
    {
        const u32               v     = json::at(vectors.doc(), vectors.root(), i);
        const mls::CipherSuite* found = profile_suite(vectors, v);
        if (found == nullptr)
        {
            continue;
        }
        const mls::CipherSuite& suite = *found;
        INFO("cipher suite " << suite.id);
        ++checked;
        const Bytes authenticated = vectors.hex(v, "authenticated_content");
        const Bytes key           = vectors.hex(v, "confirmation_key");
        const Bytes before        = vectors.hex(v, "interim_transcript_hash_before");
        const Bytes confirmed     = vectors.hex(v, "confirmed_transcript_hash_after");
        const Bytes interim       = vectors.hex(v, "interim_transcript_hash_after");

        // The AuthenticatedContent holds a commit and writes back to its bytes.
        WireReader         in(authenticated);
        WireFormat         format = WireFormat::Welcome;
        mls::FramedContent content;
        mls::ContentAuth   auth;
        REQUIRE(mls::take_authenticated_content(in, format, content, auth));
        CHECK(in.done());
        REQUIRE(content.content_type == ContentType::Commit);
        Bytes      back = filled(mls::authenticated_content_bytes(content, auth), 0U);
        WireWriter writer(span_of(back));
        mls::put_authenticated_content(writer, format, content, auth);
        CHECK(equal(back, authenticated));
        WireReader  body(content.body);
        mls::Commit commit;
        REQUIRE(mls::take_commit(body, commit));
        CHECK(body.done());
        CHECK(equal(commit_written(commit), content.body));

        // Its tag is the MAC of the confirmed hash, which with the interim hash the two updates reproduce.
        CHECK(mls::confirmation_tag_valid(suite, key, confirmed, auth.confirmation_tag));
        Bytes tag = filled(suite.hash_bytes(), 0U);
        REQUIRE(mls::confirmation_tag(suite, key, confirmed, span_of(tag)) == CryptoError::None);
        CHECK(equal(tag, auth.confirmation_tag));
        Bytes hash = filled(suite.hash_bytes(), 0U);
        REQUIRE(mls::confirmed_transcript_hash(suite, before, format, content, auth.signature, span_of(hash)) ==
                CryptoError::None);
        CHECK(equal(hash, confirmed));
        REQUIRE(mls::interim_transcript_hash(suite, confirmed, auth.confirmation_tag, span_of(hash)) ==
                CryptoError::None);
        CHECK(equal(hash, interim));
        // ConfirmedTranscriptHashInput is the AuthenticatedContent up to the tag, restated.
        const Bytes input = joined(before, ConstSpan<u8>(authenticated.data(),
                                                         authenticated.size() -
                                                             mls::vector_bytes(auth.confirmation_tag.size())));
        REQUIRE(kdf_hash(suite.kdf, input, span_of(hash)) == CryptoError::None);
        CHECK(equal(hash, confirmed));

        // Another tag, key, hash or signature does not hold.
        Bytes changed = tag;
        changed[0] ^= 0x01U;
        CHECK_FALSE(mls::confirmation_tag_valid(suite, key, confirmed, changed));
        CHECK_FALSE(mls::confirmation_tag_valid(suite, before, confirmed, tag));
        CHECK_FALSE(mls::confirmation_tag_valid(suite, key, interim, tag));
        CHECK_FALSE(mls::confirmation_tag_valid(suite, key, confirmed, ConstSpan<u8>(tag.data(), tag.size() - 1U)));
        Bytes signature = copy_of(auth.signature);
        signature[signature.size() - 1U] ^= 0x80U;
        REQUIRE(mls::confirmed_transcript_hash(suite, before, format, content, signature, span_of(hash)) ==
                CryptoError::None);
        CHECK_FALSE(equal(hash, confirmed));
    }
    CHECK(checked == 4U);
}

TEST_CASE("MLS: every message-protection vector of the profile", "[crypto][net-2][mls]")
{
    const JsonFile         vectors = mls_vectors("message-protection.json");
    const u32              count   = json::count_of(vectors.doc(), vectors.root());
    const ProviderVerifier verifier;
    usize                  checked = 0U;
    REQUIRE(count == 7U);
    for (u32 i = 0U; i < count; ++i)
    {
        const u32               v     = json::at(vectors.doc(), vectors.root(), i);
        const mls::CipherSuite* found = profile_suite(vectors, v);
        if (found == nullptr)
        {
            continue;
        }
        const mls::CipherSuite& suite = *found;
        INFO("cipher suite " << suite.id);
        ++checked;
        const Bytes group_id     = vectors.hex(v, "group_id");
        const Bytes tree_hash    = vectors.hex(v, "tree_hash");
        const Bytes confirmed    = vectors.hex(v, "confirmed_transcript_hash");
        const Bytes encryption   = vectors.hex(v, "encryption_secret");
        const Bytes sender_data  = vectors.hex(v, "sender_data_secret");
        const Bytes membership   = vectors.hex(v, "membership_key");
        const Bytes signer_pub   = vectors.hex(v, "signature_pub");
        const bool  deterministic = suite.signature == AlgorithmId::Ed25519;
        const mls::GroupContext context{suite.id, group_id, vectors.number(v, "epoch"), tree_hash, confirmed, {}};
        const Member            member(suite, vectors.hex(v, "signature_priv"));
        CHECK(equal(member.public_key, signer_pub));
        struct Kind
        {
            const char* raw;
            const char* public_message; // none for application data
            const char* private_message;
            ContentType type;
        };
        for (const Kind& kind : {Kind{"proposal", "proposal_pub", "proposal_priv", ContentType::Proposal},
                                 Kind{"commit", "commit_pub", "commit_priv", ContentType::Commit},
                                 Kind{"application", nullptr, "application_priv", ContentType::Application}})
        {
            INFO(kind.raw);
            const Bytes     raw      = vectors.hex(v, kind.raw);
            mls::SecretTree receiver = secret_tree(suite, encryption, 2U); // a tree per message, as the vectors say

            // The public message verifies and carries the raw value; protected again, it verifies too, and where the
            // suite signs deterministically it is the vector's message byte for byte.
            if (kind.public_message != nullptr)
            {
                const Bytes        bytes   = vectors.hex(v, kind.public_message);
                const mls::Message message = decoded(bytes);
                REQUIRE(message.wire_format == WireFormat::PublicMessage);
                const mls::PublicMessage& received = message.public_message;
                CHECK(equal(received.content.body, raw));
                CHECK(received.content.sender.type == SenderType::Member);
                CHECK(received.content.sender.index == 1U);
                CHECK(mls::verify_public(suite, membership, context, received) == CryptoError::None);
                CHECK(mls::verify_content(suite, verifier, signer_pub, WireFormat::PublicMessage, received.content,
                                          context, received.auth.signature) == Verification::Valid);
                Bytes other = copy_of(membership);
                other[0] ^= 0x01U;
                CHECK(mls::verify_public(suite, other, context, received) == CryptoError::Forged);

                const Bytes      signature = sign(suite, member, WireFormat::PublicMessage, received.content, context);
                const mls::ContentAuth auth{signature, received.auth.confirmation_tag};
                Bytes                  out = filled(bytes.size() + 8U, 0U);
                usize                  written = 0U;
                REQUIRE(mls::protect_public(suite, membership, context, received.content, auth, span_of(out),
                                            written) == CryptoError::None);
                out.resize(written);
                const mls::Message again = decoded(out);
                CHECK(mls::verify_public(suite, membership, context, again.public_message) == CryptoError::None);
                CHECK(mls::verify_content(suite, verifier, signer_pub, WireFormat::PublicMessage,
                                          again.public_message.content, context,
                                          again.public_message.auth.signature) == Verification::Valid);
                if (deterministic)
                {
                    CHECK(equal(out, bytes));
                }
            }
            else
            {
                // Application data never travels in a PublicMessage.
                const mls::FramedContent content{group_id, context.epoch, {SenderType::Member, 1U}, {},
                                                 ContentType::Application, raw};
                Bytes signature = filled(signature_sizes(suite.signature).signature, 0U);
                usize written   = 0U;
                CHECK(mls::sign_content(suite, member.key, WireFormat::PublicMessage, content, context,
                                        span_of(signature), written) == CryptoError::Malformed);
                const mls::ContentAuth auth{ConstSpan<u8>(signature.data(), 8U), {}};
                Bytes                  out = filled(512U, 0xAAU);
                CHECK(mls::protect_public(suite, membership, context, content, auth, span_of(out), written) ==
                      CryptoError::Malformed);
                CHECK(written == 0U);
                CHECK(all_zero(out));
            }

            // The private message opens to the raw value from leaf 1 and its signature verifies.
            const Bytes        priv_bytes = vectors.hex(v, kind.private_message);
            const mls::Message priv       = decoded(priv_bytes);
            Bytes              plaintext;
            mls::FramedContent content;
            mls::ContentAuth   auth;
            REQUIRE(open_copy(suite, receiver, sender_data, priv, plaintext, content, auth) == CryptoError::None);
            CHECK(equal(content.body, raw));
            CHECK(equal(content.group_id, group_id));
            CHECK(content.epoch == context.epoch);
            CHECK(content.sender.type == SenderType::Member);
            CHECK(content.sender.index == 1U);
            CHECK(mls::verify_content(suite, verifier, signer_pub, WireFormat::PrivateMessage, content, context,
                                      auth.signature) == Verification::Valid);
            // Opened once, the generation is spent.
            Bytes again_text;
            CHECK(open_copy(suite, receiver, sender_data, priv, again_text, content, auth) == CryptoError::Replayed);
            CHECK(all_zero(again_text));

            // Protected again by a sender's own tree, it opens in a fresh receiver's and verifies.
            mls::SecretTree        sender_tree = secret_tree(suite, encryption, 2U);
            mls::SecretTree        fresh       = secret_tree(suite, encryption, 2U);
            const mls::FramedContent framed{group_id, context.epoch, {SenderType::Member, 1U}, {}, kind.type, raw};
            const Bytes      signature = sign(suite, member, WireFormat::PrivateMessage, framed, context);
            Bytes            tag       = filled(suite.hash_bytes(), 0x5AU);
            const bool             commits = framed.content_type == ContentType::Commit;
            const ConstSpan<u8>    own_tag = commits ? ConstSpan<u8>(tag) : ConstSpan<u8>();
            const mls::ContentAuth own{signature, own_tag};
            const usize padding = mls::padme_padding(raw.size() + signature.size() + 8U);
            const Bytes ours    = protect_private(suite, sender_tree, sender_data, framed, own, padding);
            const mls::Message opened = decoded(ours);
            REQUIRE(open_copy(suite, fresh, sender_data, opened, plaintext, content, auth) == CryptoError::None);
            CHECK(equal(content.body, raw));
            CHECK(content.content_type == framed.content_type);
            CHECK(equal(auth.signature, signature));
            CHECK(equal(auth.confirmation_tag, own.confirmation_tag));
            CHECK(mls::verify_content(suite, verifier, signer_pub, WireFormat::PrivateMessage, content, context,
                                      auth.signature) == Verification::Valid);
        }
    }
    CHECK(checked == 4U);
}

TEST_CASE("MLS: every messages vector's key package, proposals, commit and framed messages round-trip",
          "[crypto][net-2][mls]")
{
    const JsonFile vectors = mls_vectors("messages.json");
    const u32      count   = json::count_of(vectors.doc(), vectors.root());
    REQUIRE(count == 300U);
    // Each proposal vector is the proposal's body: decoded behind its type as a Proposal.
    struct Body
    {
        const char*       name;
        mls::ProposalType type;
    };
    const Body bodies[] = {
        {"add_proposal", mls::ProposalType::Add},
        {"update_proposal", mls::ProposalType::Update},
        {"remove_proposal", mls::ProposalType::Remove},
        {"pre_shared_key_proposal", mls::ProposalType::PreSharedKey},
        {"re_init_proposal", mls::ProposalType::ReInit},
        {"external_init_proposal", mls::ProposalType::ExternalInit},
        {"group_context_extensions_proposal", mls::ProposalType::GroupContextExtensions},
    };
    usize sources[4] = {};
    for (u32 i = 0U; i < count; ++i)
    {
        INFO("vector " << i);
        const u32 v = json::at(vectors.doc(), vectors.root(), i);

        const Bytes        package_bytes = vectors.hex(v, "mls_key_package");
        const mls::Message package       = decoded(package_bytes);
        REQUIRE(package.wire_format == WireFormat::KeyPackage);
        CHECK(package.key_package.leaf_node.source == mls::LeafNodeSource::KeyPackage);
        const Bytes leaf = written_back(package.key_package.leaf_node, mls::leaf_node_bytes, mls::put_leaf_node);
        CHECK(equal(package.key_package.leaf_node.signed_fields,
                    ConstSpan<u8>(leaf.data(), leaf.size() - mls::vector_bytes(
                                                                 package.key_package.leaf_node.signature.size()))));

        for (const Body& body : bodies)
        {
            INFO(body.name);
            const Bytes raw   = vectors.hex(v, body.name);
            const u8    type[] = {0U, static_cast<u8>(body.type)};
            const Bytes typed  = joined(type, raw);
            const auto proposal =
                round_trip<mls::Proposal>(typed, mls::take_proposal, mls::proposal_bytes, mls::put_proposal);
            CHECK(proposal.type == static_cast<u16>(body.type));
        }

        const Bytes commit_bytes = vectors.hex(v, "commit");
        const auto  commit =
            round_trip<mls::Commit>(commit_bytes, mls::take_commit, mls::commit_bytes, mls::put_commit);
        WireReader entries(commit.proposals);
        while (!entries.done())
        {
            mls::ProposalOrRef entry;
            REQUIRE(mls::take_proposal_or_ref(entries, entry));
        }
        if (commit.has_path)
        {
            ++sources[static_cast<usize>(commit.path_leaf.source)];
            WireReader nodes(commit.path_nodes);
            while (!nodes.done())
            {
                mls::UpdatePathNode node;
                REQUIRE(mls::take_update_path_node(nodes, node));
            }
        }

        for (const char* name : {"public_message_application", "public_message_proposal", "public_message_commit"})
        {
            INFO(name);
            const Bytes        bytes   = vectors.hex(v, name);
            const mls::Message message = decoded(bytes);
            REQUIRE(message.wire_format == WireFormat::PublicMessage);
            CHECK(static_cast<char>(name[15]) ==
                  "?apc"[static_cast<usize>(message.public_message.content.content_type)]);
        }
        // Each read within its full expression, while the bytes it views live.
        CHECK(decoded(vectors.hex(v, "private_message")).wire_format == WireFormat::PrivateMessage);
        CHECK(decoded(vectors.hex(v, "mls_welcome")).wire_format == WireFormat::Welcome);
        CHECK(decoded(vectors.hex(v, "mls_group_info")).wire_format == WireFormat::GroupInfo);
    }
    CHECK(sources[static_cast<usize>(mls::LeafNodeSource::Commit)] > 0U);
}

// ---- PrivateMessage against the restatement ----

TEST_CASE("MLS: private messages against a restatement of section 6.3", "[crypto][net-2][mls]")
{
    for (const mls::CipherSuite& suite : mls::cipher_suites())
    {
        INFO("cipher suite " << suite.id);
        const Bytes             encryption  = filled(suite.hash_bytes(), 0x31U);
        const Bytes             sender_data = filled(suite.hash_bytes(), 0x32U);
        const u8                data[]      = {'h', 'e', 'l', 'l', 'o'};
        const u8                signature[] = {9U, 8U, 7U, 6U};
        const mls::ContentAuth  auth{signature, {}};
        const mls::FramedContent content = application(data, 2U);

        // protect_private's message, opened by the restatement: the layout, the leaf, the generation, the padding.
        mls::SecretTree sender = secret_tree(suite, encryption, 4U);
        const Bytes     ours   = protect_private(suite, sender, sender_data, content, auth, 3U);
        const mls::SecretTree reader = secret_tree(suite, encryption, 4U);
        const Restated  seen   = restated_open(suite, reader, sender_data, ours);
        CHECK(seen.leaf == 2U);
        CHECK(seen.generation == 0U);
        const u8 expected[] = {5U, 'h', 'e', 'l', 'l', 'o', 4U, 9U, 8U, 7U, 6U, 0U, 0U, 0U};
        CHECK(equal(seen.plaintext, expected));
        // The reuse guard is fresh for every message.
        const Bytes    second = protect_private(suite, sender, sender_data, content, auth, 0U);
        const Restated next   = restated_open(suite, reader, sender_data, second);
        CHECK(next.generation == 1U);
        CHECK_FALSE(equal(next.guard, seen.guard));

        // The restatement's messages open: zero padding of any length, in place too.
        const u8 guard[] = {0xA5U, 0x5AU, 0x0FU, 0xF0U};
        for (const usize padding : {usize{0U}, usize{1U}, usize{100U}})
        {
            mls::SecretTree from = secret_tree(suite, encryption, 4U);
            mls::SecretTree to   = secret_tree(suite, encryption, 4U);
            Bytes           text = copy_of(ConstSpan<u8>(expected, 11U));
            text.resize(11U + padding, 0U);
            Bytes message = restated_protect(suite, from, sender_data, kGroup, 7U, ContentType::Application, kAad, 3U,
                                             guard, text);
            mls::Message decoded_message;
            REQUIRE(mls::take_message(message, decoded_message));
            // In place: the plaintext is written over the ciphertext.
            const mls::PrivateMessage& priv = decoded_message.private_message;
            const auto                 at = static_cast<usize>(priv.ciphertext.data() - message.data());
            const Span<u8>             in_place(message.data() + at, priv.ciphertext.size() - kAeadTagBytes);
            mls::FramedContent         opened;
            mls::ContentAuth           opened_auth;
            REQUIRE(mls::open_private(suite, to, sender_data, priv, in_place, opened, opened_auth) ==
                    CryptoError::None);
            CHECK(equal(opened.body, data));
            CHECK(equal(opened_auth.signature, signature));
            CHECK(opened.sender.index == 3U);
            CHECK(equal(opened.authenticated_data, kAad));
        }

        // Padding with any byte but zero, a content that does not decode, or bytes past it are malformed; an
        // authentic message refused so spends nothing.
        const auto refused = [&](ConstSpan<u8> text, CryptoError verdict)
        {
            mls::SecretTree from    = secret_tree(suite, encryption, 4U);
            mls::SecretTree to      = secret_tree(suite, encryption, 4U);
            const Bytes     message = restated_protect(suite, from, sender_data, kGroup, 7U, ContentType::Application,
                                                       kAad, 3U, guard, text);
            const mls::Message decoded_message = decoded(message);
            Bytes              plaintext;
            mls::FramedContent opened;
            mls::ContentAuth   opened_auth;
            CHECK(open_copy(suite, to, sender_data, decoded_message, plaintext, opened, opened_auth) == verdict);
            CHECK(all_zero(plaintext));
            CHECK(opened.body.empty());
            mls::MessageKey key;
            CHECK(to.receive_key(3U, mls::RatchetKind::Application, 0U, key) == CryptoError::None);
        };
        const Bytes nonzero = joined(ConstSpan<u8>(expected, 11U), ConstSpan<u8>(guard, 1U));
        refused(nonzero, CryptoError::Malformed);
        Bytes late_nonzero = copy_of(ConstSpan<u8>(expected, 11U));
        late_nonzero.resize(40U, 0U);
        late_nonzero[39] = 1U;
        refused(late_nonzero, CryptoError::Malformed);
        refused(ConstSpan<u8>(expected, 10U), CryptoError::Malformed); // the signature cut short
        const u8 overlong[] = {0x40U, 0x05U, 'h', 'e', 'l', 'l', 'o', 0U};
        refused(overlong, CryptoError::Malformed); // a header longer than needed
        refused(ConstSpan<u8>(), CryptoError::Malformed);

        // A commit (an empty one) carries a confirmation tag of exactly Nh bytes.
        const auto commit_with_tag = [&](usize tag_bytes)
        {
            mls::SecretTree from      = secret_tree(suite, encryption, 4U);
            mls::SecretTree to        = secret_tree(suite, encryption, 4U);
            Bytes           text      = filled(4U + mls::vector_bytes(tag_bytes), 0U);
            text[2]                   = 1U; // the signature: one zero byte
            text[4]                   = static_cast<u8>(tag_bytes);
            const Bytes        bytes   = restated_protect(suite, from, sender_data, kGroup, 7U, ContentType::Commit,
                                                          kAad, 0U, guard, text);
            const mls::Message message = decoded(bytes);
            Bytes              plaintext;
            mls::FramedContent opened;
            mls::ContentAuth   opened_auth;
            return open_copy(suite, to, sender_data, message, plaintext, opened, opened_auth);
        };
        CHECK(commit_with_tag(suite.hash_bytes()) == CryptoError::None);
        CHECK(commit_with_tag(suite.hash_bytes() - 1U) == CryptoError::Malformed);
        CHECK(commit_with_tag(0U) == CryptoError::Malformed);
    }
}

TEST_CASE("MLS: hostile, replayed and misdirected private messages", "[crypto][net-2][mls]")
{
    const mls::CipherSuite& suite       = *mls::cipher_suite(1U);
    const Bytes             encryption  = filled(32U, 0x41U);
    const Bytes             sender_data = filled(32U, 0x42U);
    const u8                data[]      = {1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U};
    const u8                signature[] = {0xFFU};
    const mls::ContentAuth  auth{signature, {}};
    mls::SecretTree         sender = secret_tree(suite, encryption, 2U);
    const Bytes             honest = protect_private(suite, sender, sender_data, application(data), auth, 5U);

    // Every byte changed, one at a time, is refused (by the decoder or a seal) and spends nothing.
    mls::SecretTree receiver = secret_tree(suite, encryption, 2U);
    for (usize at = 0U; at < honest.size(); ++at)
    {
        INFO("byte " << at);
        Bytes changed = honest;
        changed[at] ^= 0x01U;
        mls::Message message;
        if (!mls::take_message(changed, message) || message.wire_format != WireFormat::PrivateMessage)
        {
            continue;
        }
        Bytes              plaintext;
        mls::FramedContent content;
        mls::ContentAuth   opened;
        const CryptoError  verdict = open_copy(suite, receiver, sender_data, message, plaintext, content, opened);
        CHECK(verdict != CryptoError::None);
        CHECK(all_zero(plaintext));
    }
    mls::Message message = decoded(honest);
    Bytes              plaintext;
    mls::FramedContent content;
    mls::ContentAuth   opened;
    REQUIRE(open_copy(suite, receiver, sender_data, message, plaintext, content, opened) == CryptoError::None);
    CHECK(equal(content.body, data));
    CHECK(open_copy(suite, receiver, sender_data, message, plaintext, content, opened) == CryptoError::Replayed);

    // Another sender data secret, or another epoch's tree, is Forged; a leaf past the tree is OutOfWindow.
    mls::SecretTree other = secret_tree(suite, encryption, 2U);
    CHECK(open_copy(suite, other, encryption, message, plaintext, content, opened) == CryptoError::Forged);
    mls::SecretTree wrong = secret_tree(suite, sender_data, 2U);
    CHECK(open_copy(suite, wrong, sender_data, message, plaintext, content, opened) == CryptoError::Forged);
    mls::SecretTree wide   = secret_tree(suite, encryption, 8U);
    mls::SecretTree from   = secret_tree(suite, encryption, 8U);
    const Bytes     sixth  = protect_private(suite, from, sender_data, application(data, 6U), auth, 0U);
    mls::SecretTree narrow = secret_tree(suite, encryption, 2U);
    CHECK(open_copy(suite, narrow, sender_data, decoded(sixth), plaintext, content, opened) ==
          CryptoError::OutOfWindow);
    CHECK(open_copy(suite, wide, sender_data, decoded(sixth), plaintext, content, opened) == CryptoError::None);

    // The decoder's lengths: sender data of another size is malformed, a plaintext buffer of another size and a
    // message take_message did not decode are BadLength.
    mls::PrivateMessage cut = message.private_message;
    cut.encrypted_sender_data = cut.encrypted_sender_data.first(27U);
    Bytes buffer = filled(cut.ciphertext.size() - kAeadTagBytes, 0U);
    CHECK(mls::open_private(suite, other, sender_data, cut, span_of(buffer), content, opened) ==
          CryptoError::Malformed);
    cut                = message.private_message;
    cut.ciphertext     = cut.ciphertext.first(kAeadTagBytes - 1U);
    CHECK(mls::open_private(suite, other, sender_data, cut, span_of(buffer), content, opened) ==
          CryptoError::Malformed);
    Bytes short_buffer = filled(buffer.size() - 1U, 0U);
    CHECK(mls::open_private(suite, other, sender_data, message.private_message, span_of(short_buffer), content,
                            opened) == CryptoError::BadLength);
    mls::PrivateMessage built = message.private_message;
    built.header              = {};
    CHECK(mls::open_private(suite, other, sender_data, built, span_of(buffer), content, opened) ==
          CryptoError::BadLength);
}

// ---- Framing and protection rules ----

TEST_CASE("MLS: the senders and content types each wire format admits", "[crypto][net-2][mls]")
{
    using mls::Sender;
    const Sender member{SenderType::Member, 0U};
    const Sender external{SenderType::External, 0U};
    const Sender joining{SenderType::NewMemberProposal, 0U};
    const Sender committing{SenderType::NewMemberCommit, 0U};
    const auto   admits = [](WireFormat format, const Sender& sender, ContentType type)
    { return mls::framing_admits(format, sender, type); };
    for (const ContentType type : {ContentType::Application, ContentType::Proposal, ContentType::Commit})
    {
        CHECK(admits(WireFormat::PrivateMessage, member, type));
        CHECK_FALSE(admits(WireFormat::PrivateMessage, external, type));
        CHECK_FALSE(admits(WireFormat::PrivateMessage, joining, type));
        CHECK_FALSE(admits(WireFormat::PrivateMessage, committing, type));
        CHECK(admits(WireFormat::PublicMessage, member, type) == (type != ContentType::Application));
        CHECK(admits(WireFormat::PublicMessage, external, type) == (type == ContentType::Proposal));
        CHECK(admits(WireFormat::PublicMessage, joining, type) == (type == ContentType::Proposal));
        CHECK(admits(WireFormat::PublicMessage, committing, type) == (type == ContentType::Commit));
        for (const WireFormat other : {WireFormat::Welcome, WireFormat::GroupInfo, WireFormat::KeyPackage})
        {
            CHECK_FALSE(admits(other, member, type));
        }
    }
    CHECK_FALSE(admits(WireFormat::PrivateMessage, member, static_cast<ContentType>(0U)));
    CHECK_FALSE(admits(WireFormat::PublicMessage, member, static_cast<ContentType>(4U)));
    CHECK_FALSE(admits(WireFormat::PublicMessage, Sender{static_cast<SenderType>(5U), 0U}, ContentType::Proposal));
}

TEST_CASE("MLS: public messages from members and from outside the group", "[crypto][net-2][mls]")
{
    const ProviderVerifier verifier;
    for (const mls::CipherSuite& suite : mls::cipher_suites())
    {
        INFO("cipher suite " << suite.id);
        const mls::GroupContext context = test_context(suite);
        const Bytes             seed    = filled(signature_sizes(suite.signature).seed, 0x07U);
        const Member            member(suite, seed);
        const Bytes             key = filled(suite.hash_bytes(), 0x09U);
        // A Remove proposal (leaf 2) and an empty commit without a path.
        const u8 remove[] = {0U, 3U, 0U, 0U, 0U, 2U};
        const u8 empty[]  = {0U, 0U};
        Bytes    tag      = filled(suite.hash_bytes(), 0x0CU);

        struct Case
        {
            SenderType  sender;
            ContentType type;
        };
        for (const Case c : {Case{SenderType::Member, ContentType::Proposal},
                             Case{SenderType::Member, ContentType::Commit},
                             Case{SenderType::External, ContentType::Proposal},
                             Case{SenderType::NewMemberProposal, ContentType::Proposal},
                             Case{SenderType::NewMemberCommit, ContentType::Commit}})
        {
            INFO("sender " << static_cast<int>(c.sender) << " content " << static_cast<int>(c.type));
            const bool               indexed = c.sender == SenderType::Member || c.sender == SenderType::External;
            const u32                index   = indexed ? 1U : 0U;
            const mls::FramedContent content{kGroup, 7U, {c.sender, index}, kAad, c.type,
                                             c.type == ContentType::Commit ? ConstSpan<u8>(empty)
                                                                           : ConstSpan<u8>(remove)};
            const Bytes              signature = sign(suite, member, WireFormat::PublicMessage, content, context);
            const ConstSpan<u8>      own_tag = c.type == ContentType::Commit ? ConstSpan<u8>(tag) : ConstSpan<u8>();
            const mls::ContentAuth   auth{signature, own_tag};
            Bytes                    out     = filled(1024U, 0U);
            usize                    written = 0U;
            REQUIRE(mls::protect_public(suite, key, context, content, auth, span_of(out), written) ==
                    CryptoError::None);
            out.resize(written);
            const mls::Message message = decoded(out);
            const auto&        received = message.public_message;
            CHECK(received.membership_tag.empty() == (c.sender != SenderType::Member));
            CHECK(mls::verify_public(suite, key, context, received) == CryptoError::None);
            CHECK(mls::verify_content(suite, verifier, member.public_key, WireFormat::PublicMessage, received.content,
                                      context, received.auth.signature) == Verification::Valid);
            // Only the senders that sign the GroupContext are bound to it.
            mls::GroupContext later = context;
            later.epoch             = 8U;
            const bool bound = c.sender == SenderType::Member || c.sender == SenderType::NewMemberCommit;
            CHECK((mls::verify_content(suite, verifier, member.public_key, WireFormat::PublicMessage, received.content,
                                       later, received.auth.signature) == Verification::Valid) != bound);
            // The signature binds the wire format and every field.
            CHECK(mls::verify_content(suite, verifier, member.public_key, WireFormat::PrivateMessage, received.content,
                                      context, received.auth.signature) == Verification::Invalid);
            mls::FramedContent changed = received.content;
            changed.authenticated_data = {};
            CHECK(mls::verify_content(suite, verifier, member.public_key, WireFormat::PublicMessage, changed, context,
                                      received.auth.signature) == Verification::Invalid);
            // A member's membership tag binds the context and the signature.
            if (c.sender == SenderType::Member)
            {
                CHECK(mls::verify_public(suite, key, later, received) == CryptoError::Forged);
                mls::PublicMessage cut = received;
                cut.membership_tag     = cut.membership_tag.first(cut.membership_tag.size() - 1U);
                CHECK(mls::verify_public(suite, key, context, cut) == CryptoError::Forged);
                mls::PublicMessage resigned = received;
                resigned.auth.signature     = ConstSpan<u8>(signature.data(), signature.size() - 1U);
                CHECK(mls::verify_public(suite, key, context, resigned) == CryptoError::Forged);
            }
            // A commit without its tag, and a proposal with one, are malformed.
            mls::PublicMessage wrong_tag = received;
            wrong_tag.auth.confirmation_tag = c.type == ContentType::Commit ? ConstSpan<u8>() : ConstSpan<u8>(tag);
            CHECK(mls::verify_public(suite, key, context, wrong_tag) == CryptoError::Malformed);
            Bytes again   = filled(1024U, 0U);
            usize written_again = 0U;
            CHECK(mls::protect_public(suite, key, context, received.content, wrong_tag.auth, span_of(again),
                                      written_again) == CryptoError::Malformed);
            // A buffer one byte short.
            Bytes small = filled(written - 1U, 0xAAU);
            CHECK(mls::protect_public(suite, key, context, content, auth, span_of(small), written_again) ==
                  CryptoError::BadLength);
            CHECK(all_zero(small));
        }

        // Outside the profile's senders: an external commit, a new member's application message.
        const mls::FramedContent outsider{kGroup, 7U, {SenderType::External, 0U}, {}, ContentType::Commit, empty};
        Bytes                    buffer  = filled(512U, 0U);
        usize                    written = 0U;
        CHECK(mls::protect_public(suite, key, context, outsider, {tag, tag}, span_of(buffer), written) ==
              CryptoError::Malformed);
        mls::PublicMessage refused{outsider, {tag, tag}, {}};
        CHECK(mls::verify_public(suite, key, context, refused) == CryptoError::Malformed);
        CHECK(mls::verify_content(suite, verifier, member.public_key, WireFormat::PublicMessage, outsider, context,
                                  tag) == Verification::Invalid);
        // A signer of another algorithm.
        SigningKey other;
        REQUIRE(other.generate(suite.signature == AlgorithmId::Ed25519 ? AlgorithmId::EcdsaSecp256r1Sha256
                                                                       : AlgorithmId::Ed25519) == CryptoError::None);
        const mls::FramedContent proposal{kGroup, 7U, {SenderType::Member, 0U}, {}, ContentType::Proposal, remove};
        CHECK(mls::sign_content(suite, other, WireFormat::PublicMessage, proposal, context, span_of(buffer), written) ==
              CryptoError::Unsupported);
        CHECK(written == 0U);
    }
}

TEST_CASE("MLS: protect_private's refusals", "[crypto][net-2][mls]")
{
    const mls::CipherSuite& suite       = *mls::cipher_suite(3U);
    const Bytes             encryption  = filled(32U, 0x51U);
    const Bytes             sender_data = filled(32U, 0x52U);
    const u8                data[]      = {1U};
    const u8                signature[] = {2U};
    const u8                commit[]    = {0U, 0U};
    Bytes                   tag         = filled(32U, 3U);
    mls::SecretTree         tree        = secret_tree(suite, encryption, 2U);
    const mls::ContentAuth  auth{signature, {}};

    // A sender outside the group, a commit without its tag, an application message with one.
    mls::FramedContent external = application(data);
    external.sender             = {SenderType::External, 0U};
    Bytes out                   = filled(mls::private_message_bytes(external, auth, 0U), 0xAAU);
    CHECK(mls::protect_private(suite, tree, sender_data, external, auth, 0U, span_of(out)) == CryptoError::Malformed);
    CHECK(all_zero(out));
    const mls::FramedContent framed{kGroup, 7U, {SenderType::Member, 1U}, {}, ContentType::Commit, commit};
    out = filled(mls::private_message_bytes(framed, auth, 0U), 0U);
    CHECK(mls::protect_private(suite, tree, sender_data, framed, auth, 0U, span_of(out)) == CryptoError::Malformed);
    const mls::ContentAuth tagged{signature, tag};
    CHECK(mls::protect_private(suite, tree, sender_data, application(data), tagged, 0U, span_of(out)) ==
          CryptoError::Malformed);
    // A buffer of another size; a leaf past the tree; a sender data secret of another length.
    out = filled(mls::private_message_bytes(application(data), auth, 0U) + 1U, 0xAAU);
    CHECK(mls::protect_private(suite, tree, sender_data, application(data), auth, 0U, span_of(out)) ==
          CryptoError::BadLength);
    CHECK(all_zero(out));
    out = filled(mls::private_message_bytes(application(data, 2U), auth, 0U), 0xAAU);
    CHECK(mls::protect_private(suite, tree, sender_data, application(data, 2U), auth, 0U, span_of(out)) ==
          CryptoError::OutOfWindow);
    CHECK(all_zero(out));
    CHECK(mls::protect_private(suite, tree, ConstSpan<u8>(sender_data.data(), 31U), application(data), auth, 0U,
                               span_of(out)) == CryptoError::BadLength);
    CHECK(all_zero(out));
    // A padding no vector holds, and a content whose body cannot be sized.
    CHECK(mls::private_message_bytes(application(data), auth, mls::kMaxVarint + 1U) == 0U);
    mls::FramedContent bodiless{kGroup, 7U, {SenderType::Member, 1U}, {}, ContentType::Proposal, {}};
    CHECK(mls::private_message_bytes(bodiless, auth, 0U) == 0U);
    CHECK(mls::framed_content_bytes(bodiless) == 0U);
    CHECK(mls::protect_private(suite, tree, sender_data, bodiless, auth, 0U, span_of(out)) == CryptoError::BadLength);
    // An empty tree is NotReady.
    mls::SecretTree none;
    out = filled(mls::private_message_bytes(application(data), auth, 0U), 0U);
    CHECK(mls::protect_private(suite, none, sender_data, application(data), auth, 0U, span_of(out)) ==
          CryptoError::NotReady);
}

// ---- Decoding ----

TEST_CASE("MLS: the structure decoders refuse what the syntax does not allow", "[crypto][net-2][mls]")
{
    const auto proposal_ok = [](ConstSpan<u8> bytes)
    {
        WireReader    in(bytes);
        mls::Proposal proposal;
        return mls::take_proposal(in, proposal) && in.done();
    };
    // Remove, and the reserved type 0.
    const u8 remove[] = {0U, 3U, 0U, 0U, 0U, 9U};
    CHECK(proposal_ok(remove));
    const u8 reserved[] = {0U, 0U, 0U, 0U, 0U, 9U};
    CHECK_FALSE(proposal_ok(reserved));
    // A type this build does not know is its opaque vector, and writes back.
    const u8 custom[] = {0xF0U, 0x01U, 0x02U, 0xAAU, 0xBBU};
    const auto unknown = round_trip<mls::Proposal>(custom, mls::take_proposal, mls::proposal_bytes, mls::put_proposal);
    CHECK(unknown.type == 0xF001U);
    CHECK(unknown.custom.size() == 2U);
    // A PSK of an unknown type or usage; a resumption PSK.
    const u8 psk_type[] = {0U, 4U, 3U, 0U, 0U};
    CHECK_FALSE(proposal_ok(psk_type));
    const u8 usage[] = {0U, 4U, 2U, 4U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U};
    CHECK_FALSE(proposal_ok(usage));
    const u8 resumption[] = {0U, 4U, 2U, 3U, 1U, 'g', 0U, 0U, 0U, 0U, 0U, 0U, 0U, 5U, 1U, 'n'};
    const auto branch =
        round_trip<mls::Proposal>(resumption, mls::take_proposal, mls::proposal_bytes, mls::put_proposal);
    CHECK(branch.psk.type == mls::PskType::Resumption);
    CHECK(branch.psk.usage == mls::ResumptionUsage::Branch);
    CHECK(branch.psk.epoch == 5U);
    // GroupContextExtensions: an extension list must be a sequence of extensions.
    const u8 extensions[] = {0U, 7U, 4U, 0U, 1U, 1U, 'x'};
    CHECK(proposal_ok(extensions));
    const u8 broken[] = {0U, 7U, 3U, 0U, 1U, 1U};
    CHECK_FALSE(proposal_ok(broken));

    // A commit: the proposal list, the presence octet and the path's lists.
    const auto commit_ok = [](ConstSpan<u8> bytes)
    {
        WireReader  in(bytes);
        mls::Commit commit;
        return mls::take_commit(in, commit) && in.done();
    };
    const u8 empty[] = {0U, 0U};
    CHECK(commit_ok(empty));
    const u8 presence[] = {0U, 2U};
    CHECK_FALSE(commit_ok(presence));
    const u8 reference[] = {3U, 2U, 1U, 0xEEU, 0U};
    CHECK(commit_ok(reference));
    const u8 bad_entry[] = {3U, 3U, 1U, 0xEEU, 0U};
    CHECK_FALSE(commit_ok(bad_entry));
    const u8 inline_remove[] = {7U, 1U, 0U, 3U, 0U, 0U, 0U, 1U, 0U};
    CHECK(commit_ok(inline_remove));
    const u8 short_list[] = {6U, 1U, 0U, 3U, 0U, 0U, 0U, 1U, 0U};
    CHECK_FALSE(commit_ok(short_list));

    // A leaf node: its source and its codepoint lists, which hold whole 16-bit entries.
    // encryption_key, signature_key, a basic credential, five capability lists, the source, extensions, signature.
    const u8 leaf_update[] = {1U, 'e', 1U, 's', 0U, 1U, 1U, 'i', 2U, 0U, 1U, 0U, 0U, 0U, 0U, 2U, 0U, 1U, 0U};
    const auto leaf = round_trip<mls::LeafNode>(leaf_update, mls::take_leaf_node, mls::leaf_node_bytes,
                                                mls::put_leaf_node);
    CHECK(leaf.source == mls::LeafNodeSource::Update);
    CHECK(leaf.credential.type == static_cast<u16>(mls::CredentialType::Basic));
    CHECK(leaf.signed_fields.size() == sizeof(leaf_update) - 2U);
    const auto leaf_ok = [](ConstSpan<u8> bytes)
    {
        WireReader    in(bytes);
        mls::LeafNode node;
        return mls::take_leaf_node(in, node) && in.done();
    };
    u8 odd[sizeof(leaf_update)];
    std::copy(std::begin(leaf_update), std::end(leaf_update), odd);
    odd[8] = 1U; // a versions list of one byte
    CHECK_FALSE(leaf_ok(odd));
    std::copy(std::begin(leaf_update), std::end(leaf_update), odd);
    odd[15] = 4U; // a source outside the enum
    CHECK_FALSE(leaf_ok(odd));
    odd[15] = 0U;
    CHECK_FALSE(leaf_ok(odd));
    std::copy(std::begin(leaf_update), std::end(leaf_update), odd);
    odd[5] = 0U; // the reserved credential type 0
    CHECK_FALSE(leaf_ok(odd));
    // An X.509 credential's content is a list of certificates.
    const u8 x509[] = {1U, 'e', 1U, 's', 0U, 2U, 3U, 2U, 'c', 'd', 0U, 0U, 0U, 0U, 0U, 2U, 0U, 1U, 0U};
    CHECK(leaf_ok(x509));
    const u8 x509_broken[] = {1U, 'e', 1U, 's', 0U, 2U, 3U, 3U, 'c', 'd', 0U, 0U, 0U, 0U, 0U, 2U, 0U, 1U, 0U};
    CHECK_FALSE(leaf_ok(x509_broken));

    // MLSMessage: another version, a wire format outside the enum, a byte left over.
    const auto message_ok = [](ConstSpan<u8> bytes)
    {
        mls::Message message;
        return mls::take_message(bytes, message);
    };
    const u8 welcome[] = {0U, 1U, 0U, 3U, 0xAAU};
    CHECK(message_ok(welcome));
    const u8 version[] = {0U, 2U, 0U, 3U, 0xAAU};
    CHECK_FALSE(message_ok(version));
    const u8 format[] = {0U, 1U, 0U, 6U, 0xAAU};
    CHECK_FALSE(message_ok(format));
    const u8 private_short[] = {0U, 1U, 0U, 2U, 1U, 'g', 0U, 0U, 0U, 0U, 0U, 0U, 0U, 1U, 1U, 0U, 0U, 0U};
    CHECK(message_ok(private_short));
    const u8 private_extra[] = {0U, 1U, 0U, 2U, 1U, 'g', 0U, 0U, 0U, 0U, 0U, 0U, 0U, 1U, 1U, 0U, 0U, 0U, 0U};
    CHECK_FALSE(message_ok(private_extra));
    const u8 private_type[] = {0U, 1U, 0U, 2U, 1U, 'g', 0U, 0U, 0U, 0U, 0U, 0U, 0U, 1U, 4U, 0U, 0U, 0U};
    CHECK_FALSE(message_ok(private_type));
    // A public message: a sender outside the enum; a member's message without its membership tag.
    const u8 sender[] = {0U, 1U, 0U, 1U, 1U, 'g', 0U, 0U, 0U, 0U, 0U, 0U, 0U, 1U, 5U, 0U, 2U, 0U, 3U, 0U, 0U, 0U,
                         1U, 0U};
    CHECK_FALSE(message_ok(sender));
    const u8 untagged[] = {0U, 1U, 0U, 1U, 1U, 'g', 0U, 0U, 0U, 0U, 0U, 0U, 0U, 1U, 1U, 0U, 0U, 0U, 0U,
                           0U, 2U, 0U, 3U, 0U, 0U, 0U, 1U, 0U};
    CHECK_FALSE(message_ok(untagged));
    const u8 tagged[] = {0U, 1U, 0U, 1U, 1U, 'g', 0U, 0U, 0U, 0U, 0U, 0U, 0U, 1U, 1U, 0U, 0U, 0U, 0U,
                         0U, 2U, 0U, 3U, 0U, 0U, 0U, 1U, 0U, 0U};
    CHECK(message_ok(tagged));
    const u8 external[] = {0U, 1U, 0U, 1U, 1U, 'g', 0U, 0U, 0U, 0U, 0U, 0U, 0U, 1U, 3U, 0U, 2U, 0U, 3U, 0U, 0U,
                           0U, 1U, 0U};
    CHECK(message_ok(external));
    // An AuthenticatedContent of the reserved wire format 0: the tagged message's content behind it.
    Bytes reserved_format = copy_of(ConstSpan<u8>(tagged + 2U, sizeof(tagged) - 3U));
    reserved_format[0]    = 0U;
    reserved_format[1]    = 0U;
    WireReader         in(reserved_format);
    WireFormat         wire_format = WireFormat::Welcome;
    mls::FramedContent content;
    mls::ContentAuth   auth;
    CHECK_FALSE(mls::take_authenticated_content(in, wire_format, content, auth));

    // Sizing refuses a field too long and a value outside its enum.
    mls::LeafNode node = leaf;
    node.source        = static_cast<mls::LeafNodeSource>(9U);
    CHECK(mls::leaf_node_bytes(node) == 0U);
    mls::Proposal zero;
    CHECK(mls::proposal_bytes(zero) == 0U);
    mls::FramedContent strange = application(remove);
    strange.sender.type        = static_cast<SenderType>(0U);
    CHECK(mls::framed_content_bytes(strange) == 0U);
    mls::Message nothing;
    nothing.wire_format = WireFormat::Welcome;
    CHECK(mls::message_bytes(nothing) == 0U);
}

TEST_CASE("MLS: Padme padding", "[crypto][net-2][mls]")
{
    // The restated definition: keep the top floor(log2(E)) + 1 bits of the length, E = floor(log2(length)).
    const auto padme = [](usize length) -> usize
    {
        if (length < 2U)
        {
            return length;
        }
        usize exponent = 0U;
        while ((length >> (exponent + 1U)) != 0U)
        {
            ++exponent;
        }
        usize kept = 0U;
        while ((exponent >> kept) != 0U)
        {
            ++kept;
        }
        const usize step = usize{1} << (exponent - kept);
        return (length + step - 1U) / step * step;
    };
    const usize examples[][2] = {{0U, 0U}, {1U, 1U}, {2U, 2U}, {3U, 3U}, {9U, 10U}, {100U, 104U}, {1000U, 1024U},
                                 {1025U, 1088U}, {65535U, 65536U}};
    for (const auto& example : examples)
    {
        CHECK(example[0] + mls::padme_padding(example[0]) == example[1]);
    }
    for (usize length = 0U; length < 70000U; ++length)
    {
        const usize padded = length + mls::padme_padding(length);
        REQUIRE(padded == padme(length));
        REQUIRE(padded * 100U <= length * 112U + 100U); // at most 12 % more
    }
}
