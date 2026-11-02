// crd-crypto -- MLS's key schedule and secret tree, RFC 9420 sections 6.3.2, 8 and 9 (NET.2;
// docs/design/network-foundation.md#mls), against the MLS working group's test vectors (the pinned
// mlswg/mls-implementations test-vectors, read at run time): every epoch of the key-schedule vectors (its GroupContext,
// each secret, the exporter and the external key), every psk_secret vector and every secret-tree vector (the sender
// data keys and each leaf's handshake and application keys at generations 0 and 15, received in order, received out
// of order and sent) in the suites of the profile; then the receive window against a restatement of section 9 (late,
// replayed, dropped and too-distant generations, the deletion schedule, a window of none) and every refusal.

#include "wycheproof.hpp"

#include <crd/crypto/mls.hpp>

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
using crd::containers::StringView;
using crd::containers::WireWriter;
using namespace crd::crypto;
using namespace crd::crypto::test;
namespace json = crd::assetio::json;
namespace mls  = crd::crypto::mls;
namespace tree = crd::crypto::mls::tree;

using mls::RatchetKind;

JsonFile mls_vectors(const char* name)
{
    return JsonFile(vector_path(CRD_MLS_VECTORS_DIR, name));
}

const mls::CipherSuite& suite_of(u16 id)
{
    const mls::CipherSuite* suite = mls::cipher_suite(id);
    REQUIRE(suite != nullptr);
    return *suite;
}

Span<u8> span_of(Bytes& bytes)
{
    return Span<u8>(bytes.data(), bytes.size());
}

ConstSpan<u8> bytes_of(StringView text)
{
    return ConstSpan<u8>(reinterpret_cast<const u8*>(text.data()), text.size());
}

// ---- Section 9 restated, apart from SecretTree ----

// The ratchet secret of generation 0 of a leaf: the tree walked from its root by "tree" "left" and "right", then the
// leaf's "handshake" or "application" expansion.
Bytes reference_ratchet(const mls::CipherSuite& suite, ConstSpan<u8> encryption_secret, u32 leaves, u32 leaf,
                        RatchetKind kind)
{
    const usize hash   = suite.hash_bytes();
    Bytes       secret = filled(hash, 0U);
    std::copy(encryption_secret.begin(), encryption_secret.end(), secret.begin());
    u32       node   = *tree::root(leaves);
    const u32 target = 2U * leaf;
    while (node != target)
    {
        const bool  left    = target < node;
        const char* side    = left ? "left" : "right";
        Bytes       child   = filled(hash, 0U);
        REQUIRE(mls::expand_with_label(suite, secret, "tree", bytes_of(side), span_of(child)) == CryptoError::None);
        secret = child;
        node   = left ? *tree::left(node, leaves) : *tree::right(node, leaves);
    }
    Bytes ratchet = filled(hash, 0U);
    REQUIRE(mls::expand_with_label(suite, secret, kind == RatchetKind::Handshake ? "handshake" : "application", {},
                                   span_of(ratchet)) == CryptoError::None);
    return ratchet;
}

struct ReferenceKey
{
    Bytes key;
    Bytes nonce;
};

// The key and nonce of `generation` of a ratchet whose generation-0 secret is `secret`.
ReferenceKey reference_key(const mls::CipherSuite& suite, ConstSpan<u8> first, u32 generation)
{
    const usize hash   = suite.hash_bytes();
    Bytes       secret = filled(hash, 0U);
    std::copy(first.begin(), first.end(), secret.begin());
    for (u32 g = 0U; g < generation; ++g)
    {
        Bytes next = filled(hash, 0U);
        REQUIRE(mls::derive_tree_secret(suite, secret, "secret", g, span_of(next)) == CryptoError::None);
        secret = next;
    }
    ReferenceKey out{filled(aead_key_bytes(suite.aead), 0U), filled(kAeadNonceBytes, 0U)};
    REQUIRE(mls::derive_tree_secret(suite, secret, "key", generation, span_of(out.key)) == CryptoError::None);
    REQUIRE(mls::derive_tree_secret(suite, secret, "nonce", generation, span_of(out.nonce)) == CryptoError::None);
    return out;
}

bool matches(const mls::MessageKey& key, const ReferenceKey& expected, u32 generation)
{
    return key.generation == generation && equal(key.aead_key(), expected.key) &&
           equal(ConstSpan<u8>(key.nonce, kAeadNonceBytes), expected.nonce);
}

bool cleared(const mls::MessageKey& key)
{
    return key.key_length == 0U && key.generation == 0U && all_zero(ConstSpan<u8>(key.key, mls::kMaxAeadKeyBytes)) &&
           all_zero(ConstSpan<u8>(key.nonce, kAeadNonceBytes));
}

void dirty(mls::MessageKey& key)
{
    std::fill(std::begin(key.key), std::end(key.key), u8{0x55U});
    std::fill(std::begin(key.nonce), std::end(key.nonce), u8{0x55U});
    key.key_length = 7U;
    key.generation = 9U;
}

// ---- The secret tree against its vectors ----

TEST_CASE("MLS: the secret tree and sender data keys against the secret-tree vectors", "[crypto][net-2][mls]")
{
    const JsonFile vectors = mls_vectors("secret-tree.json");
    const u32      count   = json::count_of(vectors.doc(), vectors.root());
    REQUIRE(count == 21U);
    usize checked = 0U;
    for (u32 i = 0U; i < count; ++i)
    {
        const u32               v     = json::at(vectors.doc(), vectors.root(), i);
        const mls::CipherSuite* suite = mls::cipher_suite(static_cast<u16>(vectors.number(v, "cipher_suite")));
        if (suite == nullptr)
        {
            continue;
        }
        const u32 sender_data = json::find(vectors.doc(), v, "sender_data");
        const u32 leaf_list   = json::find(vectors.doc(), v, "leaves");
        const u32 leaves      = json::count_of(vectors.doc(), leaf_list);
        INFO("cipher suite " << suite->id << ", " << leaves << " leaves");

        // The sender data keys of the vector's ciphertext.
        Bytes key   = filled(aead_key_bytes(suite->aead), 0U);
        Bytes nonce = filled(kAeadNonceBytes, 0U);
        REQUIRE(mls::sender_data_key(*suite, vectors.hex(sender_data, "sender_data_secret"),
                                     vectors.hex(sender_data, "ciphertext"), span_of(key),
                                     span_of(nonce)) == CryptoError::None);
        CHECK(equal(key, vectors.hex(sender_data, "key")));
        CHECK(equal(nonce, vectors.hex(sender_data, "nonce")));

        // Three trees over the same root: received in generation order leaf by leaf, received newest first with a
        // window that keeps the older generation (leaves taken from the last), and sent.
        const Bytes     root = vectors.hex(v, "encryption_secret");
        mls::SecretTree in_order;
        mls::SecretTree newest_first;
        mls::SecretTree sent;
        REQUIRE(in_order.init(*suite, root, leaves) == CryptoError::None);
        REQUIRE(newest_first.init(*suite, root, leaves, mls::RatchetWindow{16U, 1000U}) == CryptoError::None);
        REQUIRE(sent.init(*suite, root, leaves) == CryptoError::None);
        for (u32 n = 0U; n < leaves; ++n)
        {
            const u32 leaf    = n;
            const u32 reverse = leaves - 1U - n;
            for (const RatchetKind kind : {RatchetKind::Handshake, RatchetKind::Application})
            {
                const char* key_name   = kind == RatchetKind::Handshake ? "handshake_key" : "application_key";
                const char* nonce_name = kind == RatchetKind::Handshake ? "handshake_nonce" : "application_nonce";
                const u32   forward    = json::at(vectors.doc(), leaf_list, leaf);
                const u32   backward   = json::at(vectors.doc(), leaf_list, reverse);
                const u32   entries    = json::count_of(vectors.doc(), forward);
                REQUIRE(entries == 2U);
                for (u32 e = 0U; e < entries; ++e)
                {
                    const u32          entry      = json::at(vectors.doc(), forward, e);
                    const u32          generation = static_cast<u32>(vectors.number(entry, "generation"));
                    const ReferenceKey expected{vectors.hex(entry, key_name), vectors.hex(entry, nonce_name)};
                    mls::MessageKey    received;
                    REQUIRE(in_order.receive_key(leaf, kind, generation, received) == CryptoError::None);
                    CHECK(matches(received, expected, generation));
                    REQUIRE(in_order.consume(leaf, kind, generation) == CryptoError::None);

                    const u32          late       = json::at(vectors.doc(), backward, entries - 1U - e);
                    const u32          late_gen   = static_cast<u32>(vectors.number(late, "generation"));
                    const ReferenceKey late_key{vectors.hex(late, key_name), vectors.hex(late, nonce_name)};
                    REQUIRE(newest_first.receive_key(reverse, kind, late_gen, received) == CryptoError::None);
                    CHECK(matches(received, late_key, late_gen));
                    REQUIRE(newest_first.consume(reverse, kind, late_gen) == CryptoError::None);
                }
                // Sending hands out generations 0 to 15 in turn.
                for (u32 g = 0U; g <= 15U; ++g)
                {
                    mls::MessageKey out;
                    REQUIRE(sent.send_key(leaf, kind, out) == CryptoError::None);
                    for (u32 e = 0U; e < entries; ++e)
                    {
                        const u32 entry = json::at(vectors.doc(), forward, e);
                        if (vectors.number(entry, "generation") == g)
                        {
                            const ReferenceKey expected{vectors.hex(entry, key_name), vectors.hex(entry, nonce_name)};
                            CHECK(matches(out, expected, g));
                        }
                    }
                }
            }
            ++checked;
        }
    }
    CHECK(checked == 4U * (1U + 8U + 32U)); // the profile's four suites, each with trees of 1, 8 and 32 leaves
}

TEST_CASE("MLS: sender data keys sample at most Nh bytes of the ciphertext", "[crypto][net-2][mls]")
{
    for (const mls::CipherSuite& suite : mls::cipher_suites())
    {
        INFO("cipher suite " << suite.id);
        const usize hash   = suite.hash_bytes();
        const Bytes secret = filled(hash, 0x31U);
        Bytes       ciphertext(filled(hash + 9U, 0U));
        for (usize i = 0U; i < ciphertext.size(); ++i)
        {
            ciphertext[i] = static_cast<u8>(i * 7U + 1U);
        }
        const usize nk    = aead_key_bytes(suite.aead);
        Bytes       key   = filled(nk, 0U);
        Bytes       nonce = filled(kAeadNonceBytes, 0U);
        Bytes       other = filled(nk, 0U);
        Bytes       other_nonce = filled(kAeadNonceBytes, 0U);
        // Bytes past Nh change nothing; a ciphertext shorter than Nh is sampled whole.
        REQUIRE(mls::sender_data_key(suite, secret, ciphertext, span_of(key), span_of(nonce)) == CryptoError::None);
        REQUIRE(mls::sender_data_key(suite, secret, ConstSpan<u8>(ciphertext.data(), hash), span_of(other),
                                     span_of(other_nonce)) == CryptoError::None);
        CHECK(equal(key, other));
        CHECK(equal(nonce, other_nonce));
        const ConstSpan<u8> shorter(ciphertext.data(), hash - 1U);
        REQUIRE(mls::sender_data_key(suite, secret, shorter, span_of(key), span_of(nonce)) == CryptoError::None);
        REQUIRE(mls::expand_with_label(suite, secret, "key", shorter, span_of(other)) == CryptoError::None);
        REQUIRE(mls::expand_with_label(suite, secret, "nonce", shorter, span_of(other_nonce)) == CryptoError::None);
        CHECK(equal(key, other));
        CHECK(equal(nonce, other_nonce));
        REQUIRE(mls::sender_data_key(suite, secret, {}, span_of(key), span_of(nonce)) == CryptoError::None);
        REQUIRE(mls::expand_with_label(suite, secret, "key", {}, span_of(other)) == CryptoError::None);
        CHECK(equal(key, other));

        // A key or nonce of another length, and a secret shorter than Nh: both outputs zeroed.
        Bytes wrong = filled(nk + 1U, 0x55U);
        CHECK(mls::sender_data_key(suite, secret, ciphertext, span_of(wrong), span_of(nonce)) ==
              CryptoError::BadLength);
        CHECK(all_zero(wrong));
        CHECK(all_zero(nonce));
        Bytes short_nonce = filled(kAeadNonceBytes - 1U, 0x55U);
        key               = filled(nk, 0x55U);
        CHECK(mls::sender_data_key(suite, secret, ciphertext, span_of(key), span_of(short_nonce)) ==
              CryptoError::BadLength);
        CHECK(all_zero(key));
        CHECK(all_zero(short_nonce));
        nonce = filled(kAeadNonceBytes, 0x55U);
        key   = filled(nk, 0x55U);
        CHECK(mls::sender_data_key(suite, ConstSpan<u8>(secret.data(), hash - 1U), ciphertext, span_of(key),
                                   span_of(nonce)) == CryptoError::BadLength);
        CHECK(all_zero(key));
        CHECK(all_zero(nonce));
    }
}

// ---- The receive window ----

TEST_CASE("MLS: the secret tree's receive window, deletion schedule and refusals", "[crypto][net-2][mls]")
{
    const mls::CipherSuite& suite  = suite_of(0x0001U);
    const Bytes             root   = filled(32U, 0x42U);
    constexpr u32           leaves = 8U;
    const auto first = [&](u32 leaf, RatchetKind kind) { return reference_ratchet(suite, root, leaves, leaf, kind); };

    mls::SecretTree window;
    mls::MessageKey key;
    // Nothing before init.
    CHECK_FALSE(window.ready());
    dirty(key);
    CHECK(window.receive_key(0U, RatchetKind::Handshake, 0U, key) == CryptoError::NotReady);
    CHECK(cleared(key));
    CHECK(window.consume(0U, RatchetKind::Handshake, 0U) == CryptoError::NotReady);
    CHECK(window.send_key(0U, RatchetKind::Handshake, key) == CryptoError::NotReady);

    // init's refusals, each leaving the tree empty.
    CHECK(window.init(suite, ConstSpan<u8>(root.data(), 31U), leaves) == CryptoError::BadLength);
    CHECK(window.init(suite, root, 0U) == CryptoError::BadLength);
    CHECK(window.init(suite, root, 6U) == CryptoError::BadLength);
    CHECK(window.init(suite, root, mls::kMaxSecretTreeLeaves * 2U) == CryptoError::BadLength);
    CHECK(window.init(suite, root, leaves, mls::RatchetWindow{mls::kMaxOutOfOrder + 1U, 10U}) ==
          CryptoError::BadLength);
    CHECK(window.init(suite, root, leaves, mls::RatchetWindow{5U, mls::kMaxForward + 1U}) == CryptoError::BadLength);
    mls::CipherSuite unknown = suite;
    unknown.kdf              = AlgorithmId::Aes128Gcm;
    CHECK(window.init(unknown, root, leaves) == CryptoError::Unsupported);
    CHECK_FALSE(window.ready());
    CHECK(window.leaves() == 0U);

    REQUIRE(window.init(suite, root, leaves) == CryptoError::None); // the default window: 5 behind, 1000 ahead
    CHECK(window.ready());
    CHECK(window.leaves() == leaves);
    const Bytes h3 = first(3U, RatchetKind::Handshake);

    // A leaf outside the tree.
    dirty(key);
    CHECK(window.receive_key(leaves, RatchetKind::Handshake, 0U, key) == CryptoError::OutOfWindow);
    CHECK(cleared(key));
    CHECK(window.consume(leaves, RatchetKind::Handshake, 0U) == CryptoError::OutOfWindow);
    CHECK(window.send_key(leaves, RatchetKind::Handshake, key) == CryptoError::OutOfWindow);

    // Reading changes nothing: the same key twice, then generation 10 spent, skipping 0 to 9 (6 to 9 kept).
    REQUIRE(window.receive_key(3U, RatchetKind::Handshake, 10U, key) == CryptoError::None);
    CHECK(matches(key, reference_key(suite, h3, 10U), 10U));
    REQUIRE(window.receive_key(3U, RatchetKind::Handshake, 10U, key) == CryptoError::None);
    CHECK(matches(key, reference_key(suite, h3, 10U), 10U));
    REQUIRE(window.consume(3U, RatchetKind::Handshake, 10U) == CryptoError::None);
    dirty(key);
    CHECK(window.receive_key(3U, RatchetKind::Handshake, 10U, key) == CryptoError::Replayed);
    CHECK(cleared(key));
    CHECK(window.consume(3U, RatchetKind::Handshake, 10U) == CryptoError::Replayed);
    // Five behind is the oldest the window holds; generation 5 was dropped when the ratchet moved past it.
    CHECK(window.receive_key(3U, RatchetKind::Handshake, 5U, key) == CryptoError::OutOfWindow);
    CHECK(window.receive_key(3U, RatchetKind::Handshake, 4U, key) == CryptoError::OutOfWindow);
    for (const u32 late : {6U, 9U, 7U})
    {
        REQUIRE(window.receive_key(3U, RatchetKind::Handshake, late, key) == CryptoError::None);
        CHECK(matches(key, reference_key(suite, h3, late), late));
        REQUIRE(window.consume(3U, RatchetKind::Handshake, late) == CryptoError::None);
        CHECK(window.receive_key(3U, RatchetKind::Handshake, late, key) == CryptoError::Replayed);
        CHECK(window.consume(3U, RatchetKind::Handshake, late) == CryptoError::Replayed);
    }
    REQUIRE(window.receive_key(3U, RatchetKind::Handshake, 8U, key) == CryptoError::None);
    CHECK(matches(key, reference_key(suite, h3, 8U), 8U));

    // At most 1000 ahead of the next unconsumed generation (11).
    CHECK(window.receive_key(3U, RatchetKind::Handshake, 1012U, key) == CryptoError::OutOfWindow);
    CHECK(window.consume(3U, RatchetKind::Handshake, 1012U) == CryptoError::OutOfWindow);
    REQUIRE(window.receive_key(3U, RatchetKind::Handshake, 1011U, key) == CryptoError::None);
    CHECK(matches(key, reference_key(suite, h3, 1011U), 1011U));
    REQUIRE(window.consume(3U, RatchetKind::Handshake, 1011U) == CryptoError::None);
    // The jump dropped generation 8 (still kept a moment ago) and kept 1007 to 1010.
    CHECK(window.receive_key(3U, RatchetKind::Handshake, 8U, key) == CryptoError::OutOfWindow);
    CHECK(window.consume(3U, RatchetKind::Handshake, 8U) == CryptoError::OutOfWindow);
    CHECK(window.receive_key(3U, RatchetKind::Handshake, 1006U, key) == CryptoError::OutOfWindow);
    REQUIRE(window.receive_key(3U, RatchetKind::Handshake, 1007U, key) == CryptoError::None);
    CHECK(matches(key, reference_key(suite, h3, 1007U), 1007U));
    REQUIRE(window.receive_key(3U, RatchetKind::Handshake, 1012U, key) == CryptoError::None);
    CHECK(matches(key, reference_key(suite, h3, 1012U), 1012U));

    // The application ratchet of the same leaf, and every other leaf, are untouched: their generation 0 is the
    // reference's, received after leaf 3 opened its path (its siblings' secrets held, its ancestors' zeroed).
    for (u32 leaf = 0U; leaf < leaves; ++leaf)
    {
        for (const RatchetKind kind : {RatchetKind::Handshake, RatchetKind::Application})
        {
            if (leaf == 3U && kind == RatchetKind::Handshake)
            {
                continue;
            }
            INFO("leaf " << leaf);
            REQUIRE(window.receive_key(leaf, kind, 2U, key) == CryptoError::None);
            CHECK(matches(key, reference_key(suite, first(leaf, kind), 2U), 2U));
            REQUIRE(window.consume(leaf, kind, 0U) == CryptoError::None);
            CHECK(window.receive_key(leaf, kind, 0U, key) == CryptoError::Replayed);
        }
    }

    // Sending: generations in turn, each spent; the receive side of another tree reads the same keys.
    mls::SecretTree sender;
    REQUIRE(sender.init(suite, root, leaves) == CryptoError::None);
    const Bytes a5 = first(5U, RatchetKind::Application);
    for (u32 g = 0U; g < 4U; ++g)
    {
        REQUIRE(sender.send_key(5U, RatchetKind::Application, key) == CryptoError::None);
        CHECK(matches(key, reference_key(suite, a5, g), g));
    }
    CHECK(sender.consume(5U, RatchetKind::Application, 3U) == CryptoError::OutOfWindow); // sending keeps no window
    REQUIRE(sender.send_key(5U, RatchetKind::Handshake, key) == CryptoError::None);
    CHECK(matches(key, reference_key(suite, first(5U, RatchetKind::Handshake), 0U), 0U));

    // init again replaces everything; a moved tree carries its state and leaves the source empty.
    REQUIRE(sender.init(suite, root, leaves) == CryptoError::None);
    REQUIRE(sender.send_key(5U, RatchetKind::Application, key) == CryptoError::None);
    CHECK(matches(key, reference_key(suite, a5, 0U), 0U));
    mls::SecretTree moved(static_cast<mls::SecretTree&&>(sender));
    CHECK_FALSE(sender.ready());
    CHECK(sender.send_key(5U, RatchetKind::Application, key) == CryptoError::NotReady);
    REQUIRE(moved.send_key(5U, RatchetKind::Application, key) == CryptoError::None);
    CHECK(matches(key, reference_key(suite, a5, 1U), 1U));
    window = static_cast<mls::SecretTree&&>(moved);
    CHECK_FALSE(moved.ready());
    REQUIRE(window.send_key(5U, RatchetKind::Application, key) == CryptoError::None);
    CHECK(matches(key, reference_key(suite, a5, 2U), 2U));
}

TEST_CASE("MLS: a secret tree of one leaf, of the high-assurance suite and with no window", "[crypto][net-2][mls]")
{
    // One leaf: the root is the leaf, derived without a "tree" step.
    const mls::CipherSuite& p384 = suite_of(0x0007U);
    const Bytes             root = filled(48U, 0x07U);
    mls::SecretTree         one;
    REQUIRE(one.init(p384, root, 1U) == CryptoError::None);
    mls::MessageKey key;
    REQUIRE(one.receive_key(0U, RatchetKind::Application, 3U, key) == CryptoError::None);
    CHECK(key.key_length == 32U);
    CHECK(matches(key, reference_key(p384, reference_ratchet(p384, root, 1U, 0U, RatchetKind::Application), 3U), 3U));
    CHECK(one.receive_key(1U, RatchetKind::Application, 0U, key) == CryptoError::OutOfWindow);

    // No window: only generations at or ahead of the next unconsumed one, and nothing behind it.
    const mls::CipherSuite& suite = suite_of(0x0003U);
    const Bytes             secret = filled(32U, 0x03U);
    mls::SecretTree         strict;
    REQUIRE(strict.init(suite, secret, 4U, mls::RatchetWindow{0U, 2U}) == CryptoError::None);
    const Bytes h2 = reference_ratchet(suite, secret, 4U, 2U, RatchetKind::Handshake);
    CHECK(strict.receive_key(2U, RatchetKind::Handshake, 3U, key) == CryptoError::OutOfWindow);
    REQUIRE(strict.receive_key(2U, RatchetKind::Handshake, 2U, key) == CryptoError::None);
    REQUIRE(strict.consume(2U, RatchetKind::Handshake, 2U) == CryptoError::None);
    CHECK(strict.receive_key(2U, RatchetKind::Handshake, 2U, key) == CryptoError::OutOfWindow);
    CHECK(strict.receive_key(2U, RatchetKind::Handshake, 1U, key) == CryptoError::OutOfWindow);
    CHECK(strict.consume(2U, RatchetKind::Handshake, 0U) == CryptoError::OutOfWindow);
    REQUIRE(strict.receive_key(2U, RatchetKind::Handshake, 5U, key) == CryptoError::None);
    CHECK(matches(key, reference_key(suite, h2, 5U), 5U));
    CHECK(strict.receive_key(2U, RatchetKind::Handshake, 6U, key) == CryptoError::OutOfWindow);
}

// ---- The key schedule ----

TEST_CASE("MLS: the key schedule against the key-schedule vectors", "[crypto][net-2][mls]")
{
    const JsonFile vectors = mls_vectors("key-schedule.json");
    const u32      count   = json::count_of(vectors.doc(), vectors.root());
    REQUIRE(count == 7U);
    usize checked = 0U;
    for (u32 i = 0U; i < count; ++i)
    {
        const u32               v     = json::at(vectors.doc(), vectors.root(), i);
        const mls::CipherSuite* suite = mls::cipher_suite(static_cast<u16>(vectors.number(v, "cipher_suite")));
        if (suite == nullptr)
        {
            continue;
        }
        const Bytes       group_id = vectors.hex(v, "group_id");
        const u32         epochs   = json::find(vectors.doc(), v, "epochs");
        mls::EpochSecrets member;
        Bytes             init = vectors.hex(v, "initial_init_secret");
        for (u32 e = 0U; e < json::count_of(vectors.doc(), epochs); ++e)
        {
            INFO("cipher suite " << suite->id << " epoch " << e);
            const u32   epoch      = json::at(vectors.doc(), epochs, e);
            const Bytes tree_hash  = vectors.hex(epoch, "tree_hash");
            const Bytes transcript = vectors.hex(epoch, "confirmed_transcript_hash");
            const Bytes expected   = vectors.hex(epoch, "group_context");

            const mls::GroupContext context{suite->id, group_id, e, tree_hash, transcript, {}};
            const usize             bytes = mls::group_context_bytes(context);
            REQUIRE(bytes == expected.size());
            Bytes      encoded = filled(bytes, 0U);
            WireWriter writer(span_of(encoded));
            mls::put_group_context(writer, context);
            CHECK(writer.written() == bytes);
            CHECK(equal(encoded, expected));

            // The first epoch from the initial init secret, each later one from this object's own init secret.
            const Bytes commit = vectors.hex(epoch, "commit_secret");
            const Bytes psk    = vectors.hex(epoch, "psk_secret");
            REQUIRE(member.advance(*suite, e == 0U ? ConstSpan<u8>(init) : member.get(mls::EpochSecret::Init), commit,
                                   psk, encoded) == CryptoError::None);
            const struct
            {
                mls::EpochSecret which;
                const char*      name;
            } named[] = {
                {mls::EpochSecret::Joiner, "joiner_secret"},
                {mls::EpochSecret::Welcome, "welcome_secret"},
                {mls::EpochSecret::SenderData, "sender_data_secret"},
                {mls::EpochSecret::Encryption, "encryption_secret"},
                {mls::EpochSecret::Exporter, "exporter_secret"},
                {mls::EpochSecret::External, "external_secret"},
                {mls::EpochSecret::Confirmation, "confirmation_key"},
                {mls::EpochSecret::Membership, "membership_key"},
                {mls::EpochSecret::Resumption, "resumption_psk"},
                {mls::EpochSecret::Authentication, "epoch_authenticator"},
                {mls::EpochSecret::Init, "init_secret"},
            };
            static_assert(sizeof(named) / sizeof(named[0]) == mls::kEpochSecretCount);
            for (const auto& secret : named)
            {
                INFO(secret.name);
                CHECK(equal(member.get(secret.which), vectors.hex(epoch, secret.name)));
            }

            // A joiner from the Welcome's joiner secret reaches the same epoch.
            mls::EpochSecrets joiner;
            REQUIRE(joiner.join(*suite, member.get(mls::EpochSecret::Joiner), psk, encoded) == CryptoError::None);
            for (const auto& secret : named)
            {
                INFO(secret.name);
                CHECK(equal(joiner.get(secret.which), member.get(secret.which)));
            }

            // The external key pair and the exporter.
            HpkeKey external;
            REQUIRE(external.derive(suite->kem, member.get(mls::EpochSecret::External)) == CryptoError::None);
            Bytes external_public = filled(hpke_sizes(suite->kem).public_key, 0U);
            REQUIRE(external.public_key(span_of(external_public)) == CryptoError::None);
            CHECK(equal(external_public, vectors.hex(epoch, "external_pub")));
            const u32   exporter = json::find(vectors.doc(), epoch, "exporter");
            Bytes       exported = filled(vectors.number(exporter, "length"), 0U);
            // The label is the vector's string as it stands (its hex digits are the label's characters).
            REQUIRE(mls::export_secret(*suite, member.get(mls::EpochSecret::Exporter), vectors.text(exporter, "label"),
                                       vectors.hex(exporter, "context"), span_of(exported)) == CryptoError::None);
            CHECK(equal(exported, vectors.hex(exporter, "secret")));
            ++checked;
        }
    }
    CHECK(checked == 20U); // five epochs of each suite of the profile
}

TEST_CASE("MLS: pre-shared keys against the psk_secret vectors", "[crypto][net-2][mls]")
{
    const JsonFile vectors = mls_vectors("psk_secret.json");
    const u32      count   = json::count_of(vectors.doc(), vectors.root());
    REQUIRE(count == 77U);
    usize checked = 0U;
    for (u32 i = 0U; i < count; ++i)
    {
        const u32               v     = json::at(vectors.doc(), vectors.root(), i);
        const mls::CipherSuite* suite = mls::cipher_suite(static_cast<u16>(vectors.number(v, "cipher_suite")));
        if (suite == nullptr)
        {
            continue;
        }
        const u32 list = json::find(vectors.doc(), v, "psks");
        const u32 n    = json::count_of(vectors.doc(), list);
        INFO("cipher suite " << suite->id << ", " << n << " keys");
        crd::containers::Array<Bytes>             fields;
        crd::containers::Array<mls::PreSharedKey> psks;
        for (u32 k = 0U; k < n; ++k)
        {
            const u32 psk = json::at(vectors.doc(), list, k);
            fields.push_back(vectors.hex(psk, "psk_id"));
            fields.push_back(vectors.hex(psk, "psk_nonce"));
            fields.push_back(vectors.hex(psk, "psk"));
        }
        for (u32 k = 0U; k < n; ++k)
        {
            mls::PreSharedKey psk;
            psk.id.type  = mls::PskType::External;
            psk.id.id    = fields[3U * k];
            psk.id.nonce = fields[3U * k + 1U];
            psk.secret   = fields[3U * k + 2U];
            psks.push_back(psk);
        }
        Bytes out = filled(suite->hash_bytes(), 0x55U);
        REQUIRE(mls::psk_secret(*suite, ConstSpan<mls::PreSharedKey>(psks.data(), psks.size()), span_of(out)) ==
                CryptoError::None);
        CHECK(equal(out, vectors.hex(v, "psk_secret")));
        ++checked;
    }
    CHECK(checked == 44U); // 0 to 10 keys in each suite of the profile
}

TEST_CASE("MLS: a resumption key's PSKLabel, and the key schedule's refusals", "[crypto][net-2][mls]")
{
    const mls::CipherSuite& suite = suite_of(0x0002U);
    const Bytes             nonce = filled(32U, 0x0EU);
    const Bytes             group = filled(5U, 0x67U);
    const Bytes             value = filled(32U, 0xAAU);

    // Section 8.4 restated for one resumption key: Extract(0, psk), ExpandWithLabel(., "derived psk", PSKLabel{type 2,
    // usage, group_id<V>, epoch, nonce<V>, index 0, count 1}, Nh), then Extract(that, 0).
    mls::PreSharedKey resumption;
    resumption.id.type     = mls::PskType::Resumption;
    resumption.id.usage    = mls::ResumptionUsage::Branch;
    resumption.id.group_id = group;
    resumption.id.epoch    = 0x0102030405060708ULL;
    resumption.id.nonce    = nonce;
    resumption.secret      = value;
    Bytes label            = filled(0U, 0U);
    label.push_back(2U);
    label.push_back(3U);
    label.push_back(5U);
    for (const u8 b : group)
    {
        label.push_back(b);
    }
    for (u8 b = 1U; b <= 8U; ++b)
    {
        label.push_back(b);
    }
    label.push_back(32U);
    for (const u8 b : nonce)
    {
        label.push_back(b);
    }
    for (const u8 b : {u8{0U}, u8{0U}, u8{0U}, u8{1U}})
    {
        label.push_back(b);
    }
    Bytes extracted = filled(32U, 0U);
    Bytes input     = filled(32U, 0U);
    Bytes expected  = filled(32U, 0U);
    REQUIRE(hkdf_extract(suite.kdf, filled(32U, 0U), value, span_of(extracted)) == CryptoError::None);
    REQUIRE(mls::expand_with_label(suite, extracted, "derived psk", label, span_of(input)) == CryptoError::None);
    REQUIRE(hkdf_extract(suite.kdf, input, filled(32U, 0U), span_of(expected)) == CryptoError::None);
    Bytes out = filled(32U, 0U);
    REQUIRE(mls::psk_secret(suite, ConstSpan<mls::PreSharedKey>(&resumption, 1U), span_of(out)) == CryptoError::None);
    CHECK(equal(out, expected));

    // None is Nh zero bytes; the refusals zero the output.
    out = filled(32U, 0x55U);
    REQUIRE(mls::psk_secret(suite, {}, span_of(out)) == CryptoError::None);
    CHECK(all_zero(out));
    const auto refused = [&](const mls::PreSharedKey& psk)
    {
        out                     = filled(32U, 0x55U);
        const CryptoError error = mls::psk_secret(suite, ConstSpan<mls::PreSharedKey>(&psk, 1U), span_of(out));
        return error == CryptoError::BadLength && all_zero(out);
    };
    mls::PreSharedKey bad = resumption;
    bad.id.nonce          = ConstSpan<u8>(nonce.data(), 31U); // a nonce is Nh bytes
    CHECK(refused(bad));
    bad          = resumption;
    bad.id.usage = static_cast<mls::ResumptionUsage>(0U);
    CHECK(refused(bad));
    bad.id.usage = static_cast<mls::ResumptionUsage>(4U);
    CHECK(refused(bad));
    bad         = resumption;
    bad.id.type = static_cast<mls::PskType>(3U);
    CHECK(refused(bad));
    out = filled(31U, 0x55U);
    CHECK(mls::psk_secret(suite, {}, span_of(out)) == CryptoError::BadLength);
    CHECK(all_zero(out));
    crd::containers::Array<mls::PreSharedKey> many;
    many.resize(0x10000U, resumption);
    out = filled(32U, 0x55U);
    CHECK(mls::psk_secret(suite, ConstSpan<mls::PreSharedKey>(many.data(), many.size()), span_of(out)) ==
          CryptoError::BadLength);
    CHECK(all_zero(out));

    // The epoch's secret lengths are Nh; a refusal leaves nothing behind.
    const Bytes       secret  = filled(32U, 0x21U);
    const Bytes       context = filled(40U, 0x01U);
    mls::EpochSecrets epoch;
    CHECK(epoch.get(mls::EpochSecret::Init).empty());
    REQUIRE(epoch.advance(suite, secret, secret, secret, context) == CryptoError::None);
    CHECK(epoch.get(mls::EpochSecret::Init).size() == 32U);
    CHECK(epoch.advance(suite, ConstSpan<u8>(secret.data(), 31U), secret, secret, context) == CryptoError::BadLength);
    CHECK(epoch.get(mls::EpochSecret::Init).empty());
    REQUIRE(epoch.advance(suite, secret, secret, secret, context) == CryptoError::None);
    CHECK(epoch.advance(suite, secret, ConstSpan<u8>(secret.data(), 31U), secret, context) == CryptoError::BadLength);
    CHECK(epoch.get(mls::EpochSecret::Joiner).empty());
    CHECK(epoch.join(suite, secret, ConstSpan<u8>(secret.data(), 31U), context) == CryptoError::BadLength);
    CHECK(epoch.join(suite, ConstSpan<u8>(secret.data(), 31U), secret, context) == CryptoError::BadLength);
    // Every input is in the derivation: another commit secret, PSK secret or context gives another epoch.
    mls::EpochSecrets other;
    REQUIRE(epoch.advance(suite, secret, secret, secret, context) == CryptoError::None);
    const Bytes changed = filled(32U, 0x22U);
    REQUIRE(other.advance(suite, secret, changed, secret, context) == CryptoError::None);
    CHECK_FALSE(equal(other.get(mls::EpochSecret::Joiner), epoch.get(mls::EpochSecret::Joiner)));
    REQUIRE(other.advance(suite, secret, secret, changed, context) == CryptoError::None);
    CHECK(equal(other.get(mls::EpochSecret::Joiner), epoch.get(mls::EpochSecret::Joiner)));
    CHECK_FALSE(equal(other.get(mls::EpochSecret::Welcome), epoch.get(mls::EpochSecret::Welcome)));
    REQUIRE(other.advance(suite, secret, secret, secret, ConstSpan<u8>(context.data(), 39U)) == CryptoError::None);
    CHECK_FALSE(equal(other.get(mls::EpochSecret::Init), epoch.get(mls::EpochSecret::Init)));
    // No two of an epoch's secrets are equal.
    for (usize a = 0U; a < mls::kEpochSecretCount; ++a)
    {
        for (usize b = a + 1U; b < mls::kEpochSecretCount; ++b)
        {
            CHECK_FALSE(
                equal(epoch.get(static_cast<mls::EpochSecret>(a)), epoch.get(static_cast<mls::EpochSecret>(b))));
        }
    }

    // The exporter: the length is in the derivation; an empty output is refused.
    Bytes a = filled(16U, 0U);
    Bytes b = filled(17U, 0U);
    REQUIRE(mls::export_secret(suite, epoch.get(mls::EpochSecret::Exporter), "sframe", {}, span_of(a)) ==
            CryptoError::None);
    REQUIRE(mls::export_secret(suite, epoch.get(mls::EpochSecret::Exporter), "sframe", {}, span_of(b)) ==
            CryptoError::None);
    CHECK_FALSE(equal(a, ConstSpan<u8>(b.data(), 16U)));
    CHECK(mls::export_secret(suite, epoch.get(mls::EpochSecret::Exporter), "sframe", {}, Span<u8>()) ==
          CryptoError::BadLength);
    CHECK(mls::export_secret(suite, ConstSpan<u8>(secret.data(), 16U), "sframe", {}, span_of(a)) ==
          CryptoError::BadLength);
    CHECK(all_zero(a));
}
} // namespace
