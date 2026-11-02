// crd-crypto -- MLS, RFC 9420 (NET.2; docs/design/network-foundation.md#mls), against the MLS working group's test
// vectors (the pinned mlswg/mls-implementations test-vectors, read at run time): tree math for trees of 1 to 512
// leaves, the variable-size vector headers of the deserialization vectors, every crypto-basics operation of each
// cipher suite of the profile (and the refusal of the others), and DeriveKeyPair of each epoch's external key from
// the key-schedule vectors; then direct paths, copaths and common ancestors against their definitions, malformed
// headers and presence octets, and every refusal of the labelled operations.

#include "wycheproof.hpp"

#include <crd/crypto/mls.hpp>

#include <catch2/catch_test_macros.hpp>

#include <bit>
#include <optional>

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
using crd::containers::WireReader;
using crd::containers::WireWriter;
using namespace crd::crypto;
using namespace crd::crypto::test;
namespace json = crd::assetio::json;
namespace mls  = crd::crypto::mls;
namespace tree = crd::crypto::mls::tree;

JsonFile mls_vectors(const char* name)
{
    return JsonFile(vector_path(CRD_MLS_VECTORS_DIR, name));
}

Bytes filled(usize size, u8 value)
{
    Bytes out;
    out.resize(size, value);
    return out;
}

std::optional<u32> optional_at(const JsonFile& file, u32 array, u32 index)
{
    const u32 node = json::at(file.doc(), array, index);
    REQUIRE(node != json::kInvalid);
    if (file.doc().nodes[node].type == json::JsonType::Null)
    {
        return std::nullopt;
    }
    return static_cast<u32>(file.number(node));
}

const mls::CipherSuite& suite_of(u16 id)
{
    const mls::CipherSuite* suite = mls::cipher_suite(id);
    REQUIRE(suite != nullptr);
    return *suite;
}

// ---- Cipher suites ----

TEST_CASE("MLS: the cipher suites of the profile", "[crypto][net-2][mls]")
{
    const ConstSpan<mls::CipherSuite> suites = mls::cipher_suites();
    REQUIRE(suites.size() == 4U);
    const u16 ids[] = {iana::kMls128Dhkemx25519Aes128gcmSha256Ed25519, iana::kMls128Dhkemp256Aes128gcmSha256P256,
                       iana::kMls128Dhkemx25519Chacha20poly1305Sha256Ed25519, iana::kMls256Dhkemp384Aes256gcmSha384P384};
    for (usize i = 0U; i < suites.size(); ++i)
    {
        CHECK(suites[i].id == ids[i]);
        CHECK(mls::cipher_suite(ids[i]) == &suites[i]);
        CHECK(hpke_supported(suites[i].hpke()));
        CHECK(provider_implements(suites[i].signature));
    }
    CHECK(suite_of(0x0001U).hash_bytes() == 32U);
    CHECK(suite_of(0x0007U).hash_bytes() == 48U);
    CHECK(suite_of(0x0002U).signature == AlgorithmId::EcdsaSecp256r1Sha256);
    CHECK(suite_of(0x0003U).aead == AlgorithmId::Chacha20Poly1305);
    // X448, Ed448 and P-521 (0x0004 to 0x0006), the reserved 0x0000, a GREASE value and the private range are not.
    for (const u16 outside : {u16{0x0000U}, u16{0x0004U}, u16{0x0005U}, u16{0x0006U}, u16{0x0A0AU}, u16{0xF000U}})
    {
        CHECK(mls::cipher_suite(outside) == nullptr);
    }
}

// ---- The presentation language ----

TEST_CASE("MLS: vector headers against the deserialization vectors", "[crypto][net-2][mls]")
{
    const JsonFile vectors = mls_vectors("deserialization.json");
    const u32      count   = json::count_of(vectors.doc(), vectors.root());
    REQUIRE(count == 14U);
    for (u32 i = 0U; i < count; ++i)
    {
        const u32   v      = json::at(vectors.doc(), vectors.root(), i);
        const Bytes header = vectors.hex(v, "vlbytes_header");
        const usize length = vectors.number(v, "length");
        INFO("length " << length);
        WireReader in(header);
        usize      value = 0U;
        REQUIRE(mls::take_varint(in, value));
        CHECK(value == length);
        CHECK(in.done());
        CHECK(mls::varint_bytes(length) == header.size());
        CHECK(mls::vector_bytes(length) == header.size() + length);
        Bytes      written = filled(header.size(), 0U);
        WireWriter out(Span<u8>(written.data(), written.size()));
        mls::put_varint(out, length);
        CHECK(out.written() == header.size());
        CHECK(equal(written, header));
    }
}

TEST_CASE("MLS: malformed vector headers and presence octets", "[crypto][net-2][mls]")
{
    const auto take = [](std::initializer_list<u8> bytes, usize& value)
    {
        const Bytes input(bytes);
        WireReader  in(input);
        return mls::take_varint(in, value);
    };
    usize value = 0U;
    // The prefix 0b11 is invalid at every length.
    CHECK_FALSE(take({0xC0U}, value));
    CHECK_FALSE(take({0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU}, value));
    // A value that fits a shorter encoding: 63 in two bytes, 16383 in four, 0 in two and four.
    CHECK_FALSE(take({0x40U, 0x3FU}, value));
    CHECK_FALSE(take({0x40U, 0x00U}, value));
    CHECK_FALSE(take({0x80U, 0x00U, 0x3FU, 0xFFU}, value));
    CHECK_FALSE(take({0x80U, 0x00U, 0x00U, 0x00U}, value));
    // The shortest values of each length are minimal.
    CHECK(take({0x40U, 0x40U}, value));
    CHECK(value == 64U);
    CHECK(take({0x80U, 0x00U, 0x40U, 0x00U}, value));
    CHECK(value == 16384U);
    // Short inputs.
    CHECK_FALSE(take({}, value));
    CHECK_FALSE(take({0x40U}, value));
    CHECK_FALSE(take({0x80U, 0x00U, 0x40U}, value));

    // A vector shorter than its header says; one exactly as long.
    const Bytes   short_body = {0x03U, 0xAAU, 0xBBU};
    WireReader    short_in(short_body);
    ConstSpan<u8> body;
    CHECK_FALSE(mls::take_vector(short_in, body));
    const Bytes whole = {0x02U, 0xAAU, 0xBBU, 0x01U};
    WireReader  whole_in(whole);
    REQUIRE(mls::take_vector(whole_in, body));
    CHECK(body.size() == 2U);
    CHECK(body[1] == 0xBBU);
    bool present = false;
    REQUIRE(mls::take_presence(whole_in, present));
    CHECK(present);
    CHECK(whole_in.done());
    for (const u8 octet : {u8{2U}, u8{0x80U}, u8{0xFFU}})
    {
        const u8   bad[] = {octet};
        WireReader in(bad);
        CHECK_FALSE(mls::take_presence(in, present));
    }
    const u8   absent[] = {0x00U};
    WireReader absent_in(absent);
    REQUIRE(mls::take_presence(absent_in, present));
    CHECK_FALSE(present);
    WireReader nothing(ConstSpan<u8>{});
    CHECK_FALSE(mls::take_presence(nothing, present));

    // Header lengths at each boundary, and none above 2^30 - 1.
    CHECK(mls::varint_bytes(0U) == 1U);
    CHECK(mls::varint_bytes(63U) == 1U);
    CHECK(mls::varint_bytes(64U) == 2U);
    CHECK(mls::varint_bytes(16383U) == 2U);
    CHECK(mls::varint_bytes(16384U) == 4U);
    CHECK(mls::varint_bytes(mls::kMaxVarint) == 4U);
    CHECK(mls::varint_bytes(mls::kMaxVarint + 1U) == 0U);
    CHECK(mls::vector_bytes(mls::kMaxVarint + 1U) == 0U);

    // A vector written and read back.
    const u8   payload[] = {1U, 2U, 3U, 4U, 5U};
    u8         buffer[6] = {};
    WireWriter out(buffer);
    mls::put_vector(out, payload);
    CHECK(out.written() == 6U);
    WireReader back(buffer);
    REQUIRE(mls::take_vector(back, body));
    CHECK(equal(body, ConstSpan<u8>(payload)));
}

// ---- Tree math ----

TEST_CASE("MLS: tree math against the tree-math vectors", "[crypto][net-2][mls]")
{
    const JsonFile vectors = mls_vectors("tree-math.json");
    const u32      count   = json::count_of(vectors.doc(), vectors.root());
    REQUIRE(count == 10U);
    for (u32 i = 0U; i < count; ++i)
    {
        const u32 v      = json::at(vectors.doc(), vectors.root(), i);
        const u32 leaves = static_cast<u32>(vectors.number(v, "n_leaves"));
        const u32 nodes  = static_cast<u32>(vectors.number(v, "n_nodes"));
        INFO("leaves " << leaves);
        REQUIRE(tree::valid_size(leaves));
        CHECK(tree::node_width(leaves) == nodes);
        CHECK(tree::root(leaves) == std::optional<u32>(static_cast<u32>(vectors.number(v, "root"))));
        const u32 left    = json::find(vectors.doc(), v, "left");
        const u32 right   = json::find(vectors.doc(), v, "right");
        const u32 parent  = json::find(vectors.doc(), v, "parent");
        const u32 sibling = json::find(vectors.doc(), v, "sibling");
        REQUIRE(json::count_of(vectors.doc(), left) == nodes);
        REQUIRE(json::count_of(vectors.doc(), sibling) == nodes);
        for (u32 node = 0U; node < nodes; ++node)
        {
            INFO("node " << node);
            CHECK(tree::left(node, leaves) == optional_at(vectors, left, node));
            CHECK(tree::right(node, leaves) == optional_at(vectors, right, node));
            CHECK(tree::parent(node, leaves) == optional_at(vectors, parent, node));
            CHECK(tree::sibling(node, leaves) == optional_at(vectors, sibling, node));
        }
        // The node past the tree has no relatives.
        CHECK_FALSE(tree::left(nodes, leaves));
        CHECK_FALSE(tree::parent(nodes, leaves));
        CHECK_FALSE(tree::sibling(nodes, leaves));
    }
}

TEST_CASE("MLS: direct paths, copaths and common ancestors against their definitions", "[crypto][net-2][mls]")
{
    for (u32 leaves = 1U; leaves <= 64U; leaves *= 2U)
    {
        const u32 width = tree::node_width(leaves);
        for (u32 node = 0U; node < width; ++node)
        {
            INFO("leaves " << leaves << " node " << node);
            u32         path[tree::kMaxDepth] = {};
            u32         co[tree::kMaxDepth]   = {};
            const usize depth                 = tree::direct_path(node, leaves, path);
            REQUIRE(tree::copath(node, leaves, co) == depth);
            // The direct path is the chain of parents up to the root, and the copath their siblings below it.
            u32   at    = node;
            usize steps = 0U;
            while (const std::optional<u32> up = tree::parent(at, leaves))
            {
                REQUIRE(steps < depth);
                CHECK(co[steps] == *tree::sibling(at, leaves));
                at = *up;
                CHECK(path[steps] == at);
                ++steps;
            }
            CHECK(steps == depth);
            CHECK(at == *tree::root(leaves));
            CHECK(tree::level(at) == static_cast<u32>(std::countr_zero(leaves)));

            // The common ancestor is the lowest node on both direct paths (a node is on its own).
            for (u32 other = 0U; other < width; ++other)
            {
                u32                other_path[tree::kMaxDepth] = {};
                const usize        other_depth = tree::direct_path(other, leaves, other_path);
                std::optional<u32> lowest;
                const auto         consider = [&](u32 candidate)
                {
                    bool on_other = candidate == other;
                    for (usize k = 0U; k < other_depth; ++k)
                    {
                        on_other = on_other || other_path[k] == candidate;
                    }
                    if (on_other && (!lowest || tree::level(candidate) < tree::level(*lowest)))
                    {
                        lowest = candidate;
                    }
                };
                consider(node);
                for (usize k = 0U; k < depth; ++k)
                {
                    consider(path[k]);
                }
                CHECK(tree::common_ancestor(node, other, leaves) == lowest);
            }
        }
    }

    // A full-size tree: the deepest leaf has 31 ancestors.
    u32 deep[tree::kMaxDepth] = {};
    CHECK(tree::direct_path(0U, tree::kMaxLeaves, deep) == tree::kMaxDepth);
    CHECK(deep[tree::kMaxDepth - 1U] == tree::kMaxLeaves - 1U);
    CHECK(tree::common_ancestor(0U, 2U * (tree::kMaxLeaves - 1U), tree::kMaxLeaves) ==
          std::optional<u32>(tree::kMaxLeaves - 1U));
    CHECK(tree::node_width(tree::kMaxLeaves) == 0xFFFFFFFFU);

    // Sizes that are not a power of two, zero and above 2^31, and nodes outside the tree, have no answer.
    for (const u32 leaves : {0U, 3U, 6U, 100U, 0x80000001U, 0xFFFFFFFFU})
    {
        CHECK_FALSE(tree::valid_size(leaves));
        CHECK(tree::node_width(leaves) == 0U);
        CHECK_FALSE(tree::root(leaves));
        CHECK_FALSE(tree::parent(0U, leaves));
        CHECK_FALSE(tree::common_ancestor(0U, 0U, leaves));
    }
    u32 small[2] = {};
    CHECK(tree::direct_path(0U, 8U, small) == 0U); // three ancestors do not fit two entries
    CHECK(tree::copath(0U, 8U, small) == 0U);
    CHECK(tree::direct_path(15U, 8U, deep) == 0U);
    CHECK_FALSE(tree::common_ancestor(0U, 15U, 8U));
    CHECK_FALSE(tree::parent(7U, 8U)); // the root
    CHECK_FALSE(tree::sibling(7U, 8U));
    CHECK(tree::direct_path(0U, 1U, deep) == 0U); // a one-leaf tree's root is its leaf
}

// ---- Labelled operations ----

TEST_CASE("MLS: every crypto-basics vector of the profile", "[crypto][net-2][mls]")
{
    const JsonFile         vectors = mls_vectors("crypto-basics.json");
    const u32              count   = json::count_of(vectors.doc(), vectors.root());
    const ProviderVerifier verifier;
    usize                  checked = 0U;
    REQUIRE(count == 7U);
    for (u32 i = 0U; i < count; ++i)
    {
        const u32 v  = json::at(vectors.doc(), vectors.root(), i);
        const u16 id = static_cast<u16>(vectors.number(v, "cipher_suite"));
        INFO("cipher suite " << id);
        const mls::CipherSuite* found = mls::cipher_suite(id);
        if (found == nullptr)
        {
            CHECK((id >= 4U && id <= 6U)); // X448, Ed448 and P-521: outside the profile
            continue;
        }
        const mls::CipherSuite& suite = *found;
        ++checked;

        const u32 ref = json::find(vectors.doc(), v, "ref_hash");
        Bytes     out = filled(suite.hash_bytes(), 0U);
        REQUIRE(mls::ref_hash(suite, vectors.text(ref, "label"), vectors.hex(ref, "value"),
                              Span<u8>(out.data(), out.size())) == CryptoError::None);
        CHECK(equal(out, vectors.hex(ref, "out")));

        const u32 expand = json::find(vectors.doc(), v, "expand_with_label");
        Bytes     okm    = filled(vectors.number(expand, "length"), 0U);
        REQUIRE(mls::expand_with_label(suite, vectors.hex(expand, "secret"), vectors.text(expand, "label"),
                                       vectors.hex(expand, "context"),
                                       Span<u8>(okm.data(), okm.size())) == CryptoError::None);
        CHECK(equal(okm, vectors.hex(expand, "out")));

        const u32 derive = json::find(vectors.doc(), v, "derive_secret");
        REQUIRE(mls::derive_secret(suite, vectors.hex(derive, "secret"), vectors.text(derive, "label"),
                                   Span<u8>(out.data(), out.size())) == CryptoError::None);
        CHECK(equal(out, vectors.hex(derive, "out")));

        const u32 tree_secret = json::find(vectors.doc(), v, "derive_tree_secret");
        Bytes     generated   = filled(vectors.number(tree_secret, "length"), 0U);
        REQUIRE(mls::derive_tree_secret(suite, vectors.hex(tree_secret, "secret"), vectors.text(tree_secret, "label"),
                                        static_cast<u32>(vectors.number(tree_secret, "generation")),
                                        Span<u8>(generated.data(), generated.size())) == CryptoError::None);
        CHECK(equal(generated, vectors.hex(tree_secret, "out")));

        // The vector's signature verifies, a fresh one does too, and neither verifies another content or label.
        const u32  sign = json::find(vectors.doc(), v, "sign_with_label");
        SigningKey signer;
        REQUIRE(signer.from_seed(suite.signature, vectors.hex(sign, "priv")) == CryptoError::None);
        Bytes signer_public = filled(signature_sizes(suite.signature).public_key, 0U);
        REQUIRE(signer.public_key(Span<u8>(signer_public.data(), signer_public.size())) == CryptoError::None);
        const Bytes      sign_public = vectors.hex(sign, "pub");
        const Bytes      content     = vectors.hex(sign, "content");
        const StringView label       = vectors.text(sign, "label");
        CHECK(equal(signer_public, sign_public));
        CHECK(mls::verify_with_label(suite, verifier, sign_public, label, content, vectors.hex(sign, "signature")) ==
              Verification::Valid);
        Bytes signature = filled(signature_sizes(suite.signature).signature, 0U);
        usize written   = 0U;
        REQUIRE(mls::sign_with_label(suite, signer, label, content, Span<u8>(signature.data(), signature.size()),
                                     written) == CryptoError::None);
        const ConstSpan<u8> fresh(signature.data(), written);
        CHECK(mls::verify_with_label(suite, verifier, sign_public, label, content, fresh) == Verification::Valid);
        Bytes changed = content;
        changed[0] ^= 0x01U;
        CHECK(mls::verify_with_label(suite, verifier, sign_public, label, changed, fresh) == Verification::Invalid);
        CHECK(mls::verify_with_label(suite, verifier, sign_public, "Other", content, fresh) ==
              Verification::Invalid);

        // The vector's ciphertext opens, a fresh one does too, and neither opens under another label or context.
        const u32 encrypt = json::find(vectors.doc(), v, "encrypt_with_label");
        HpkeKey   key;
        REQUIRE(key.from_private(suite.kem, vectors.hex(encrypt, "priv")) == CryptoError::None);
        Bytes key_public = filled(hpke_sizes(suite.kem).public_key, 0U);
        REQUIRE(key.public_key(Span<u8>(key_public.data(), key_public.size())) == CryptoError::None);
        const Bytes      encrypt_public = vectors.hex(encrypt, "pub");
        const Bytes      context        = vectors.hex(encrypt, "context");
        const Bytes      plaintext      = vectors.hex(encrypt, "plaintext");
        const StringView encrypt_label  = vectors.text(encrypt, "label");
        CHECK(equal(key_public, encrypt_public));
        Bytes opened = filled(plaintext.size(), 0U);
        REQUIRE(mls::decrypt_with_label(suite, key, encrypt_label, context, vectors.hex(encrypt, "kem_output"),
                                        vectors.hex(encrypt, "ciphertext"),
                                        Span<u8>(opened.data(), opened.size())) == CryptoError::None);
        CHECK(equal(opened, plaintext));
        Bytes kem_output = filled(hpke_sizes(suite.kem).public_key, 0U);
        Bytes ciphertext = filled(plaintext.size() + kHpkeTagBytes, 0U);
        REQUIRE(mls::encrypt_with_label(suite, encrypt_public, encrypt_label, context, plaintext,
                                        Span<u8>(kem_output.data(), kem_output.size()),
                                        Span<u8>(ciphertext.data(), ciphertext.size())) == CryptoError::None);
        REQUIRE(mls::decrypt_with_label(suite, key, encrypt_label, context, kem_output, ciphertext,
                                        Span<u8>(opened.data(), opened.size())) == CryptoError::None);
        CHECK(equal(opened, plaintext));
        CHECK(mls::decrypt_with_label(suite, key, "Other", context, kem_output, ciphertext,
                                      Span<u8>(opened.data(), opened.size())) == CryptoError::Forged);
        CHECK(all_zero(opened));
        CHECK(mls::decrypt_with_label(suite, key, encrypt_label, {}, kem_output, ciphertext,
                                      Span<u8>(opened.data(), opened.size())) == CryptoError::Forged);
    }
    CHECK(checked == 4U);
}

TEST_CASE("MLS: DeriveKeyPair of each epoch's external key against the key-schedule vectors", "[crypto][net-2][mls]")
{
    const JsonFile vectors = mls_vectors("key-schedule.json");
    const u32      count   = json::count_of(vectors.doc(), vectors.root());
    usize          checked = 0U;
    REQUIRE(count == 7U);
    for (u32 i = 0U; i < count; ++i)
    {
        const u32               v     = json::at(vectors.doc(), vectors.root(), i);
        const mls::CipherSuite* suite = mls::cipher_suite(static_cast<u16>(vectors.number(v, "cipher_suite")));
        if (suite == nullptr)
        {
            continue;
        }
        const u32 epochs = json::find(vectors.doc(), v, "epochs");
        for (u32 e = 0U; e < json::count_of(vectors.doc(), epochs); ++e)
        {
            INFO("cipher suite " << suite->id << " epoch " << e);
            const u32 epoch = json::at(vectors.doc(), epochs, e);
            HpkeKey   external;
            REQUIRE(external.derive(suite->kem, vectors.hex(epoch, "external_secret")) == CryptoError::None);
            Bytes external_public = filled(hpke_sizes(suite->kem).public_key, 0U);
            REQUIRE(external.public_key(Span<u8>(external_public.data(), external_public.size())) ==
                    CryptoError::None);
            CHECK(equal(external_public, vectors.hex(epoch, "external_pub")));
            ++checked;
        }
    }
    CHECK(checked == 20U); // five epochs of each suite of the profile
}

TEST_CASE("MLS: refusals of the labelled operations", "[crypto][net-2][mls]")
{
    const mls::CipherSuite& suite  = suite_of(0x0001U);
    const mls::CipherSuite& p384   = suite_of(0x0007U);
    const Bytes             secret = filled(32U, 0x17U);
    Bytes                   out    = filled(32U, 0x55U);

    // Hash and derived-secret lengths are the suite's Nh; an expansion is 1 to 65535 bytes and at most 255 * Nh.
    CHECK(mls::ref_hash(suite, "label", secret, Span<u8>(out.data(), 31U)) == CryptoError::BadLength);
    CHECK(all_zero(ConstSpan<u8>(out.data(), 31U)));
    CHECK(mls::ref_hash(p384, "label", secret, Span<u8>(out.data(), out.size())) == CryptoError::BadLength);
    CHECK(mls::derive_secret(suite, secret, "label", Span<u8>(out.data(), 16U)) == CryptoError::BadLength);
    CHECK(mls::expand_with_label(suite, secret, "label", {}, Span<u8>()) == CryptoError::BadLength);
    Bytes long_out = filled(255U * 32U + 1U, 0x55U);
    CHECK(mls::expand_with_label(suite, secret, "label", {}, Span<u8>(long_out.data(), long_out.size())) ==
          CryptoError::BadLength);
    CHECK(all_zero(long_out));
    Bytes widest = filled(255U * 32U, 0U);
    CHECK(mls::expand_with_label(suite, secret, "label", {}, Span<u8>(widest.data(), widest.size())) ==
          CryptoError::None);
    // A secret shorter than Nh is no KDF key.
    out = filled(32U, 0x55U);
    CHECK(mls::derive_secret(suite, ConstSpan<u8>(secret.data(), 16U), "label", Span<u8>(out.data(), out.size())) ==
          CryptoError::BadLength);
    CHECK(all_zero(out));

    // Every derivation is a function of its secret, label, context and length.
    Bytes a = filled(32U, 0U);
    Bytes b = filled(32U, 0U);
    REQUIRE(mls::derive_tree_secret(suite, secret, "key", 7U, Span<u8>(a.data(), a.size())) == CryptoError::None);
    REQUIRE(mls::derive_tree_secret(suite, secret, "key", 8U, Span<u8>(b.data(), b.size())) == CryptoError::None);
    CHECK_FALSE(equal(a, b));
    REQUIRE(mls::derive_tree_secret(suite, secret, "nonce", 7U, Span<u8>(b.data(), b.size())) == CryptoError::None);
    CHECK_FALSE(equal(a, b));
    REQUIRE(mls::expand_with_label(suite, secret, "key", ConstSpan<u8>(), Span<u8>(b.data(), 16U)) ==
            CryptoError::None);
    Bytes c = filled(32U, 0U);
    REQUIRE(mls::expand_with_label(suite, secret, "key", ConstSpan<u8>(), Span<u8>(c.data(), c.size())) ==
            CryptoError::None);
    CHECK_FALSE(equal(ConstSpan<u8>(b.data(), 16U), ConstSpan<u8>(c.data(), 16U))); // the length is in the label
    // A context beyond the inline buffer.
    const Bytes long_context = filled(1000U, 0x21U);
    REQUIRE(mls::expand_with_label(suite, secret, "joiner", long_context, Span<u8>(a.data(), a.size())) ==
            CryptoError::None);
    REQUIRE(mls::expand_with_label(suite, secret, "joiner", ConstSpan<u8>(long_context.data(), 999U),
                                   Span<u8>(b.data(), b.size())) == CryptoError::None);
    CHECK_FALSE(equal(a, b));

    // A signer of another algorithm than the suite's.
    SigningKey ecdsa;
    REQUIRE(ecdsa.generate(AlgorithmId::EcdsaSecp256r1Sha256) == CryptoError::None);
    Bytes signature = filled(72U, 0x55U);
    usize written   = 9U;
    CHECK(mls::sign_with_label(suite, ecdsa, "label", secret, Span<u8>(signature.data(), signature.size()),
                               written) == CryptoError::Unsupported);
    CHECK(written == 0U);
    CHECK(all_zero(signature));
    // A signature output too short for the algorithm.
    SigningKey ed25519;
    REQUIRE(ed25519.generate(AlgorithmId::Ed25519) == CryptoError::None);
    signature = filled(63U, 0x55U);
    CHECK(mls::sign_with_label(suite, ed25519, "label", secret, Span<u8>(signature.data(), signature.size()),
                               written) != CryptoError::None);
    CHECK(written == 0U);
    CHECK(all_zero(signature));
    // Verification under the suite's algorithm only: an ECDSA key and signature are invalid under an Ed25519 suite.
    Bytes ecdsa_public = filled(65U, 0U);
    REQUIRE(ecdsa.public_key(Span<u8>(ecdsa_public.data(), ecdsa_public.size())) == CryptoError::None);
    signature = filled(72U, 0U);
    REQUIRE(mls::sign_with_label(suite_of(0x0002U), ecdsa, "label", secret,
                                 Span<u8>(signature.data(), signature.size()), written) == CryptoError::None);
    const ProviderVerifier verifier;
    CHECK(mls::verify_with_label(suite_of(0x0002U), verifier, ecdsa_public, "label", secret,
                                 ConstSpan<u8>(signature.data(), written)) == Verification::Valid);
    CHECK(mls::verify_with_label(suite, verifier, ecdsa_public, "label", secret,
                                 ConstSpan<u8>(signature.data(), written)) == Verification::Invalid);

    // Encryption to a key of another KEM, and decryption with one.
    HpkeKey x25519;
    REQUIRE(x25519.generate(AlgorithmId::X25519) == CryptoError::None);
    Bytes kem_output = filled(97U, 0x55U);
    Bytes ciphertext = filled(secret.size() + kHpkeTagBytes, 0x55U);
    CHECK(mls::encrypt_with_label(p384, ConstSpan<u8>(secret.data(), 32U), "label", {}, secret,
                                  Span<u8>(kem_output.data(), kem_output.size()),
                                  Span<u8>(ciphertext.data(), ciphertext.size())) == CryptoError::BadLength);
    CHECK(all_zero(kem_output));
    CHECK(all_zero(ciphertext));
    Bytes opened = filled(secret.size(), 0x55U);
    CHECK(mls::decrypt_with_label(p384, x25519, "label", {}, kem_output, ciphertext,
                                  Span<u8>(opened.data(), opened.size())) == CryptoError::Unsupported);
    CHECK(all_zero(opened));
}
} // namespace
