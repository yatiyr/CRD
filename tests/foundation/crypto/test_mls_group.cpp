// crd-crypto -- an MLS group, RFC 9420 sections 5.3.1, 7.3, 10, 11, 12.4.3 and 13.4 (NET.2;
// docs/design/network-foundation.md#mls), against the MLS working group's test vectors (the pinned
// mlswg/mls-implementations test-vectors, read at run time): every welcome vector of the profile (the Welcome opened
// with the member's init key, the GroupInfo verified and its epoch confirmed), every passive-client-welcome vector of
// the profile (a member joins from the Welcome, the tree given or carried in the GroupInfo, with external PSKs, and
// reaches the vector's epoch authenticator), and the messages vectors' Welcomes and GroupInfos decoded and written
// back. Then groups a creator starts and joiners enter from real key packages (with and without a path, PSKs and the
// tree in the GroupInfo), every refusal of a join leaving the group empty, and the validation of leaf nodes, key
// packages, credential types and extensions against a restatement of sections 7.3, 10.1, 11.1 and 13.

#include "mls_committer.hpp"
#include "mls_test_support.hpp"

#include <crd/crypto/mls_group.hpp>

#include <catch2/catch_test_macros.hpp>

#include <initializer_list>

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

Bytes joined(std::initializer_list<ConstSpan<u8>> parts)
{
    Bytes out;
    for (const ConstSpan<u8> part : parts)
    {
        for (const u8 byte : part)
        {
            out.push_back(byte);
        }
    }
    return out;
}

Bytes codepoints(std::initializer_list<u16> values)
{
    Bytes out;
    for (const u16 value : values)
    {
        out.push_back(static_cast<u8>(value >> 8U));
        out.push_back(static_cast<u8>(value));
    }
    return out;
}

// ---- The working group's vectors ----

TEST_CASE("MLS: every welcome vector of the profile", "[crypto][net-2][mls]")
{
    const JsonFile         vectors = mls_vectors("welcome.json");
    const ProviderVerifier verifier;
    const TestAuthority    authority;
    const TestStore        store;
    u32                    checked = 0U;
    for (u32 v = 0U; v < json::count_of(vectors.doc(), vectors.root()); ++v)
    {
        const u32               vector = json::at(vectors.doc(), vectors.root(), v);
        const mls::CipherSuite* suite  = profile_suite(vectors, vector);
        if (suite == nullptr)
        {
            continue;
        }
        INFO("cipher suite " << suite->id);
        const Bytes        package_bytes = vectors.hex(vector, "key_package");
        const Bytes        welcome_bytes = vectors.hex(vector, "welcome");
        const mls::Message package       = message_of(package_bytes);
        const mls::Message welcome       = message_of(welcome_bytes);
        REQUIRE(package.wire_format == mls::WireFormat::KeyPackage);
        REQUIRE(welcome.wire_format == mls::WireFormat::Welcome);

        // The Welcome and each of its entries decode whole and write back.
        WireReader   in(welcome.body);
        mls::Welcome decoded;
        REQUIRE(mls::take_welcome(in, decoded));
        CHECK(in.done());
        CHECK(decoded.cipher_suite == suite->id);
        CHECK(equal(written(decoded, mls::welcome_bytes, mls::put_welcome), welcome.body));
        WireReader entries(decoded.secrets);
        usize      count = 0U;
        while (!entries.done())
        {
            mls::EncryptedGroupSecrets entry;
            REQUIRE(mls::take_encrypted_group_secrets(entries, entry));
            CHECK(entry.new_member.size() == suite->hash_bytes());
            ++count;
        }
        CHECK(count >= 1U);

        // The key package is valid for a group of its suite, and the Welcome addresses it.
        const mls::GroupContext group{suite->id, {}, 0U, {}, {}, {}};
        CHECK(mls::check_key_package(group, package.key_package, verifier, authority, timeless()) ==
              CryptoError::None);
        mls::OpenedWelcome opened;
        REQUIRE(mls::open_welcome(welcome.body, package.key_package, vectors.hex(vector, "init_priv"), store, nullptr,
                                  opened) == CryptoError::None);
        REQUIRE(opened.ready());
        const mls::GroupInfo& info = opened.group_info();
        CHECK(opened.secrets().psks.empty());
        CHECK(info.context.cipher_suite == suite->id);
        CHECK(mls::verify_group_info(*suite, verifier, vectors.hex(vector, "signer_pub"), info) ==
              Verification::Valid);
        // The GroupSecrets write back and decode to the same fields.
        const Bytes       secrets = written(opened.secrets(), mls::group_secrets_bytes, mls::put_group_secrets);
        WireReader        secrets_in(view(secrets));
        mls::GroupSecrets secrets_back;
        REQUIRE(mls::take_group_secrets(secrets_in, secrets_back));
        CHECK(secrets_in.done());
        CHECK(equal(secrets_back.joiner_secret, opened.secrets().joiner_secret));
        CHECK(secrets_back.has_path_secret == opened.secrets().has_path_secret);
        CHECK(equal(secrets_back.path_secret, opened.secrets().path_secret));

        // The GroupInfo writes back to what decoded, and the joiner's epoch confirms its tag.
        const Bytes info_bytes = group_info_written(info);
        WireReader  again(view(info_bytes));
        mls::GroupInfo reread;
        REQUIRE(mls::take_group_info(again, reread));
        CHECK(again.done());
        CHECK(equal(reread.signed_fields, info.signed_fields));
        mls::EpochSecrets epoch;
        REQUIRE(epoch.join(*suite, opened.secrets().joiner_secret, opened.psk_secret(), context_bytes(info.context)) ==
                CryptoError::None);
        CHECK(mls::confirmation_tag_valid(*suite, epoch.get(mls::EpochSecret::Confirmation),
                                          info.context.confirmed_transcript_hash, info.confirmation_tag));
        ++checked;
    }
    CHECK(checked == 4U);
}

TEST_CASE("MLS: every passive-client-welcome vector of the profile joins at its epoch authenticator",
          "[crypto][net-2][mls]")
{
    const JsonFile         vectors = mls_vectors("passive-client-welcome.json");
    const ProviderVerifier verifier;
    const TestAuthority    authority;
    u32                    checked   = 0U;
    u32                    with_tree = 0U;
    u32                    with_psk  = 0U;
    for (u32 v = 0U; v < json::count_of(vectors.doc(), vectors.root()); ++v)
    {
        const u32               vector = json::at(vectors.doc(), vectors.root(), v);
        const mls::CipherSuite* suite  = profile_suite(vectors, vector);
        if (suite == nullptr)
        {
            continue;
        }
        INFO("vector " << v << ", cipher suite " << suite->id);
        TestStore store;
        const u32 psks = json::find(vectors.doc(), vector, "external_psks");
        for (u32 i = 0U; i < json::count_of(vectors.doc(), psks); ++i)
        {
            const u32      entry = json::at(vectors.doc(), psks, i);
            TestStore::Key key;
            key.name   = vectors.hex(entry, "psk_id");
            key.secret = vectors.hex(entry, "psk");
            store.keys.push_back(key);
        }
        with_psk += store.keys.empty() ? 0U : 1U;
        const Bytes tree = vectors.has(vector, "ratchet_tree") ? vectors.hex(vector, "ratchet_tree") : Bytes();
        with_tree += tree.empty() ? 0U : 1U;
        const Bytes        package_bytes = vectors.hex(vector, "key_package");
        const Bytes        welcome_bytes = vectors.hex(vector, "welcome");
        const mls::Message package       = message_of(package_bytes);
        const mls::Message welcome       = message_of(welcome_bytes);
        // The vectors' leaves live a year from March 2023.
        mls::LeafPolicy policy;
        policy.now          = 1700000000U;
        policy.max_lifetime = 366U * 24U * 3600U;
        const mls::GroupConfig config{&verifier, &authority, &store, policy, {}};
        mls::Group             group;
        REQUIRE(group.join(welcome.body, package.key_package, vectors.hex(vector, "init_priv"),
                           vectors.hex(vector, "encryption_priv"), view(tree), config) == CryptoError::None);
        CHECK(equal(group.epoch_authenticator(), vectors.hex(vector, "initial_epoch_authenticator")));
        CHECK(group.keys().holds(2U * group.own_leaf()));
        CHECK(group.secret_tree().leaves() == group.tree().leaves());
        CHECK(mls::check_key_package(group.context(), package.key_package, verifier, authority, policy) ==
              CryptoError::None);

        // A member that already takes part in the group refuses the same Welcome and keeps nothing.
        store.groups.push_back(Bytes(group.context().group_id.begin(), group.context().group_id.end()));
        CHECK(group.join(welcome.body, package.key_package, vectors.hex(vector, "init_priv"),
                         vectors.hex(vector, "encryption_priv"), view(tree), config) == CryptoError::Replayed);
        CHECK_FALSE(group.ready());
        CHECK(group.epoch_authenticator().empty());
        ++checked;
    }
    CHECK(checked == 32U); // eight of each suite of the profile
    CHECK(with_tree == 16U);
    CHECK(with_psk == 16U);
}

TEST_CASE("MLS: the messages vectors' Welcomes and GroupInfos decode and write back", "[crypto][net-2][mls]")
{
    const JsonFile vectors = mls_vectors("messages.json");
    u32            checked = 0U;
    for (u32 v = 0U; v < json::count_of(vectors.doc(), vectors.root()); ++v)
    {
        const u32          vector        = json::at(vectors.doc(), vectors.root(), v);
        const Bytes        welcome_bytes = vectors.hex(vector, "mls_welcome");
        const Bytes        info_bytes    = vectors.hex(vector, "mls_group_info");
        const mls::Message welcome       = message_of(welcome_bytes);
        const mls::Message info          = message_of(info_bytes);
        WireReader         in(welcome.body);
        mls::Welcome       decoded;
        REQUIRE(mls::take_welcome(in, decoded));
        CHECK(in.done());
        CHECK(equal(written(decoded, mls::welcome_bytes, mls::put_welcome), welcome.body));
        WireReader     info_in(info.body);
        mls::GroupInfo group_info;
        REQUIRE(mls::take_group_info(info_in, group_info));
        CHECK(info_in.done());
        CHECK(equal(group_info_written(group_info), info.body));
        ++checked;
    }
    CHECK(checked == json::count_of(vectors.doc(), vectors.root()));
    CHECK(checked > 0U);
}

// ---- A creator's group and its joiners ----

// A creator with a group of its own in `suite`.
struct Creator
{
    Identity   id;
    mls::Group group;
};

void create_group(const mls::CipherSuite& suite, Creator& creator, const mls::GroupConfig& config,
                  ConstSpan<u8> extensions = {})
{
    make_identity(suite, creator.id, 0U);
    const Bytes group_id = random_of(suite.hash_bytes());
    REQUIRE(creator.group.create(suite, view(group_id), extensions, creator.id.leaf, view(creator.id.hpke_private),
                                 config) == CryptoError::None);
}

// What a joined member must hold of the epoch its Welcome started.
void check_joined(const mls::CipherSuite& suite, const mls::Group& joined, const NextEpoch& epoch, u32 leaf,
                  const mls::Group& creator, bool path)
{
    REQUIRE(joined.ready());
    CHECK(&joined.suite() == mls::cipher_suite(suite.id));
    CHECK(joined.epoch() == creator.epoch() + 1U);
    CHECK(joined.own_leaf() == leaf);
    CHECK(equal(joined.tree().root_hash(), epoch.tree.root_hash()));
    CHECK(equal(joined.context().group_id, creator.context().group_id));
    CHECK(equal(joined.context().confirmed_transcript_hash, view(epoch.confirmed)));
    for (usize s = 0U; s < mls::kEpochSecretCount; ++s)
    {
        const auto which = static_cast<mls::EpochSecret>(s);
        CHECK(equal(joined.secret(which), epoch.secrets.get(which)));
    }
    Bytes interim = filled(suite.hash_bytes(), 0U);
    REQUIRE(mls::interim_transcript_hash(suite, view(epoch.confirmed), view(epoch.tag), span_of(interim)) ==
            CryptoError::None);
    CHECK(equal(joined.interim_transcript_hash(), view(interim)));
    CHECK(joined.secret_tree().leaves() == epoch.tree.leaves());
    const u32 ancestor = *mls::tree::common_ancestor(2U * creator.own_leaf(), 2U * leaf, epoch.tree.leaves());
    CHECK(joined.keys().holds(2U * leaf));
    CHECK(joined.keys().holds(ancestor) == path);
    CHECK(joined.keys().holds(*mls::tree::root(epoch.tree.leaves())) == path);
}

TEST_CASE("MLS: a creator's group welcomes joiners from their key packages in every suite", "[crypto][net-2][mls]")
{
    const ProviderVerifier verifier;
    const TestAuthority    authority;
    for (const mls::CipherSuite& suite : mls::cipher_suites())
    {
        INFO("cipher suite " << suite.id);
        TestStore              store;
        const mls::GroupConfig config{&verifier, &authority, &store, timeless(), {}};
        Creator                creator;
        create_group(suite, creator, config);
        REQUIRE(creator.group.ready());
        CHECK(creator.group.epoch() == 0U);
        CHECK(creator.group.own_leaf() == 0U);
        CHECK(creator.group.tree().leaves() == 1U);
        CHECK(creator.group.context().confirmed_transcript_hash.empty());
        CHECK(creator.group.interim_transcript_hash().size() == suite.hash_bytes());
        CHECK(creator.group.epoch_authenticator().size() == suite.hash_bytes());

        // A path, the tree in the GroupInfo, no PSK; then no path, the tree fetched, two external PSKs.
        TestStore::Key first;
        first.name   = Bytes{'a', 'l', 'p', 'h', 'a'};
        first.secret = random_of(32U);
        TestStore::Key second;
        second.name   = Bytes{'b', 'e', 't', 'a'};
        second.secret = random_of(48U);
        store.keys.push_back(first);
        store.keys.push_back(second);
        const Bytes       nonce_a = random_of(suite.hash_bytes());
        const Bytes       nonce_b = random_of(suite.hash_bytes());
        mls::PreSharedKey psks[2];
        psks[0].id.id     = view(store.keys[0].name);
        psks[0].id.nonce  = view(nonce_a);
        psks[0].secret    = view(store.keys[0].secret);
        psks[1].id.id     = view(store.keys[1].name);
        psks[1].id.nonce  = view(nonce_b);
        psks[1].secret    = view(store.keys[1].secret);
        for (const bool path : {true, false})
        {
            INFO("path " << path);
            Joiner joiners[3];
            for (u32 i = 0U; i < 3U; ++i)
            {
                make_joiner(suite, joiners[i], 1U + i);
            }
            AddOptions options;
            options.path         = path;
            options.tree_in_info = path;
            options.psks         = path ? ConstSpan<mls::PreSharedKey>() : ConstSpan<mls::PreSharedKey>(psks, 2U);
            NextEpoch epoch;
            commit_adds(suite, creator.id, creator.group, joiners, 3U, options, epoch);
            for (u32 i = 0U; i < 3U; ++i)
            {
                mls::Group joined;
                REQUIRE(joined.join(view(epoch.welcome), joiners[i].package, view(joiners[i].init_private),
                                    view(joiners[i].id.hpke_private), path ? ConstSpan<u8>() : view(epoch.tree_bytes),
                                    config) == CryptoError::None);
                check_joined(suite, joined, epoch, epoch.leaves[i], creator.group, path);
            }
        }
    }
}

// ---- Every refusal of a join ----

// A joiner's attempt, and that a refusal left nothing.
void refused(mls::Group& group, CryptoError expected, ConstSpan<u8> welcome, const Joiner& joiner,
             const mls::GroupConfig& config, ConstSpan<u8> tree = {})
{
    CHECK(group.join(welcome, joiner.package, view(joiner.init_private), view(joiner.id.hpke_private), tree,
                     config) == expected);
    CHECK_FALSE(group.ready());
    CHECK(group.epoch_authenticator().empty());
    CHECK_FALSE(group.tree().ready());
    CHECK_FALSE(group.keys().ready());
    CHECK_FALSE(group.secret_tree().ready());
    CHECK(group.context().group_id.empty());
}

// A Welcome made by hand for one key package: `info` (an encoded GroupInfo, well formed or not) sealed under the
// welcome key of `joiner_secret` with no PSK, and `secrets` (an encoded GroupSecrets, well formed or not) sealed to the
// package's init key under it (`info` itself when not `seal`ed).
Bytes hand_welcome(const mls::CipherSuite& suite, const mls::KeyPackage& package, const Bytes& secrets,
                   const Bytes& info, const Bytes& joiner_secret, bool seal = true)
{
    const usize hash    = suite.hash_bytes();
    const Bytes zero    = filled(hash, 0U);
    Bytes       welcome = filled(hash, 0U);
    Bytes       key     = filled(aead_key_bytes(suite.aead), 0U);
    Bytes       nonce   = filled(kAeadNonceBytes, 0U);
    REQUIRE(mls::welcome_secret(suite, view(joiner_secret), view(zero), span_of(welcome)) == CryptoError::None);
    REQUIRE(mls::welcome_key(suite, view(welcome), span_of(key), span_of(nonce)) == CryptoError::None);
    AeadKey aead;
    REQUIRE(aead.init(suite.aead, view(key)) == CryptoError::None);
    Bytes sealed = seal ? filled(info.size() + kAeadTagBytes, 0U) : info;
    if (seal)
    {
        REQUIRE(aead.seal(view(nonce), view(info), {}, span_of(sealed)) == CryptoError::None);
    }
    Bytes reference = filled(hash, 0U);
    REQUIRE(mls::key_package_ref(suite, package, span_of(reference)) == CryptoError::None);
    Bytes kem_output = filled(hpke_sizes(suite.kem).encapsulation, 0U);
    Bytes ciphertext = filled(secrets.size() + kHpkeTagBytes, 0U);
    REQUIRE(mls::encrypt_with_label(suite, package.init_key, "Welcome", view(sealed), view(secrets),
                                    span_of(kem_output), span_of(ciphertext)) == CryptoError::None);
    const auto vector = [](const Bytes& bytes)
    {
        Bytes      out = filled(mls::vector_bytes(bytes.size()), 0U);
        WireWriter writer(span_of(out));
        mls::put_vector(writer, view(bytes));
        return out;
    };
    const Bytes        entry = joined({view(vector(reference)), view(vector(kem_output)), view(vector(ciphertext))});
    const mls::Welcome message{suite.id, view(entry), view(sealed)};
    return written(message, mls::welcome_bytes, mls::put_welcome);
}

TEST_CASE("MLS: a join refuses each fault of section 12.4.3.1 and keeps nothing", "[crypto][net-2][mls]")
{
    const mls::CipherSuite& suite = suite_of(0x0001U);
    const ProviderVerifier  verifier;
    TestAuthority           authority;
    TestStore               store;
    const mls::GroupConfig  config{&verifier, &authority, &store, timeless(), {}};
    Creator                 creator;
    create_group(suite, creator, config);
    Joiner joiners[2];
    make_joiner(suite, joiners[0], 1U);
    make_joiner(suite, joiners[1], 2U);
    NextEpoch honest;
    commit_adds(suite, creator.id, creator.group, joiners, 1U, AddOptions{}, honest);
    mls::Group group;
    // A joined group refuses the next Welcome as wholly as an empty one.
    REQUIRE(group.join(view(honest.welcome), joiners[0].package, view(joiners[0].init_private),
                       view(joiners[0].id.hpke_private), {}, config) == CryptoError::None);

    SECTION("what the config lacks")
    {
        const mls::GroupConfig no_store{&verifier, &authority, nullptr, timeless(), {}};
        refused(group, CryptoError::Unsupported, view(honest.welcome), joiners[0], no_store);
        const mls::GroupConfig no_authority{&verifier, nullptr, &store, timeless(), {}};
        refused(group, CryptoError::Unsupported, view(honest.welcome), joiners[0], no_authority);
    }
    SECTION("a Welcome that does not decode whole")
    {
        refused(group, CryptoError::Malformed, ConstSpan<u8>(honest.welcome.data(), honest.welcome.size() - 1U),
                joiners[0], config);
        Bytes longer = honest.welcome;
        longer.push_back(0U);
        refused(group, CryptoError::Malformed, view(longer), joiners[0], config);
    }
    SECTION("another cipher suite, a key package not addressed, another init key")
    {
        Bytes other = honest.welcome;
        other[1]    = 0x03U;
        refused(group, CryptoError::Unsupported, view(other), joiners[0], config);
        refused(group, CryptoError::BadKey, view(honest.welcome), joiners[1], config);
        Joiner wrong_key;
        make_joiner(suite, wrong_key, 1U);
        wrong_key.package      = joiners[0].package;
        wrong_key.id.hpke_private = joiners[0].id.hpke_private;
        refused(group, CryptoError::BadKey, view(honest.welcome), wrong_key, config);
    }
    SECTION("a changed GroupSecrets or GroupInfo does not open")
    {
        // The last byte is the GroupInfo's tag; the byte before its vector is the entry's GroupSecrets' tag.
        Bytes info_changed = honest.welcome;
        info_changed.back() ^= 0x01U;
        refused(group, CryptoError::Forged, view(info_changed), joiners[0], config);
        const usize sealed          = honest.group_info.size() + kAeadTagBytes;
        Bytes       secrets_changed = honest.welcome;
        secrets_changed[secrets_changed.size() - sealed - mls::varint_bytes(sealed) - 1U] ^= 0x01U;
        refused(group, CryptoError::Forged, view(secrets_changed), joiners[0], config);
    }
    SECTION("a GroupInfo whose tag, signature or tree is not the epoch's")
    {
        AddOptions options;
        options.wrong_tag = true;
        NextEpoch wrong;
        commit_adds(suite, creator.id, creator.group, joiners, 1U, options, wrong);
        refused(group, CryptoError::Forged, view(wrong.welcome), joiners[0], config);
        AddOptions signer;
        signer.wrong_signer = true;
        NextEpoch unsigned_by_leaf;
        commit_adds(suite, creator.id, creator.group, joiners, 1U, signer, unsigned_by_leaf);
        refused(group, CryptoError::Forged, view(unsigned_by_leaf.welcome), joiners[0], config);
        // A fetched tree takes precedence over the GroupInfo's; another tree's root hash is not the context's.
        refused(group, CryptoError::Forged, view(honest.welcome), joiners[0], config,
                view(unsigned_by_leaf.tree_bytes));
        AddOptions fetched;
        fetched.tree_in_info = false;
        NextEpoch without;
        commit_adds(suite, creator.id, creator.group, joiners, 1U, fetched, without);
        refused(group, CryptoError::NotReady, view(without.welcome), joiners[0], config);
        const u8 hostile_tree[] = {0x01U, 0x00U};
        refused(group, CryptoError::Malformed, view(without.welcome), joiners[0], config, hostile_tree);
    }
    SECTION("a path secret whose keys are not the tree's")
    {
        Joiner three[3];
        for (u32 i = 0U; i < 3U; ++i)
        {
            make_joiner(suite, three[i], 1U + i);
        }
        AddOptions options;
        options.wrong_path = true;
        NextEpoch wrong;
        commit_adds(suite, creator.id, creator.group, three, 3U, options, wrong);
        // The third joiner's lowest common ancestor with the committer is the root of four leaves: its path
        // secret is replaced by the commit secret, which derives no node's key.
        refused(group, CryptoError::BadKey, view(wrong.welcome), three[2], config);
        // The first's ancestor is below the root, so it is given the root's secret instead.
        refused(group, CryptoError::BadKey, view(wrong.welcome), three[0], config);
    }
    SECTION("PSKs the store lacks, named twice, or of usage reinit or branch")
    {
        const Bytes       id    = Bytes{'k'};
        const Bytes       nonce = random_of(32U);
        const Bytes       key   = random_of(32U);
        mls::PreSharedKey psk;
        psk.id.id    = view(id);
        psk.id.nonce = view(nonce);
        psk.secret   = view(key);
        AddOptions options;
        options.psks = ConstSpan<mls::PreSharedKey>(&psk, 1U);
        NextEpoch with_psk;
        commit_adds(suite, creator.id, creator.group, joiners, 1U, options, with_psk);
        refused(group, CryptoError::NotReady, view(with_psk.welcome), joiners[0], config);
        TestStore::Key held;
        held.name   = id;
        held.secret = key;
        store.keys.push_back(held);
        mls::Group fine;
        CHECK(fine.join(view(with_psk.welcome), joiners[0].package, view(joiners[0].init_private),
                        view(joiners[0].id.hpke_private), {}, config) == CryptoError::None);
        const mls::PreSharedKey twice[2] = {psk, psk};
        options.psks                     = ConstSpan<mls::PreSharedKey>(twice, 2U);
        NextEpoch repeated;
        commit_adds(suite, creator.id, creator.group, joiners, 1U, options, repeated);
        refused(group, CryptoError::Malformed, view(repeated.welcome), joiners[0], config);
        for (const mls::ResumptionUsage usage : {mls::ResumptionUsage::Reinit, mls::ResumptionUsage::Branch})
        {
            mls::PreSharedKey resumption;
            resumption.id.type     = mls::PskType::Resumption;
            resumption.id.usage    = usage;
            resumption.id.group_id = creator.group.context().group_id;
            resumption.id.nonce    = view(nonce);
            resumption.secret      = view(key);
            options.psks           = ConstSpan<mls::PreSharedKey>(&resumption, 1U);
            NextEpoch reinit;
            commit_adds(suite, creator.id, creator.group, joiners, 1U, options, reinit);
            // Held only by the group it resumes, which this join does not name (sections 11.2 and 11.3).
            refused(group, CryptoError::NotReady, view(reinit.welcome), joiners[0], config);
        }
    }
    SECTION("GroupSecrets and a GroupInfo that open but do not decode, or are of another suite")
    {
        const usize             hash   = suite.hash_bytes();
        const Bytes             joiner = random_of(hash);
        const Bytes             path   = random_of(hash);
        const mls::GroupSecrets plain{view(joiner), false, {}, {}};
        const Bytes good_secrets = written(plain, mls::group_secrets_bytes, mls::put_group_secrets);
        const Bytes info         = honest.group_info;
        const auto  try_secrets  = [&](CryptoError expected, const mls::GroupSecrets& secrets)
        {
            const Bytes bytes = written(secrets, mls::group_secrets_bytes, mls::put_group_secrets);
            refused(group, expected, view(hand_welcome(suite, joiners[0].package, bytes, info, joiner)), joiners[0],
                    config);
        };
        // A joiner or path secret not Nh long, a byte after the GroupSecrets, a PSK whose nonce is not Nh long.
        try_secrets(CryptoError::Malformed, mls::GroupSecrets{ConstSpan<u8>(joiner.data(), hash - 1U), false, {}, {}});
        try_secrets(CryptoError::Malformed,
                    mls::GroupSecrets{view(joiner), true, ConstSpan<u8>(path.data(), hash - 1U), {}});
        const Bytes trailing = joined({view(good_secrets), view(Bytes{0U})});
        refused(group, CryptoError::Malformed, view(hand_welcome(suite, joiners[0].package, trailing, info, joiner)),
                joiners[0], config);
        const Bytes         psk_name    = Bytes{0x6BU};
        const Bytes         short_nonce = random_of(hash - 1U);
        mls::PreSharedKeyId id;
        id.id    = view(psk_name);
        id.nonce = view(short_nonce);
        const Bytes ids = written(id, mls::psk_id_bytes, mls::put_psk_id);
        try_secrets(CryptoError::Malformed, mls::GroupSecrets{view(joiner), false, {}, view(ids)});
        // A GroupInfo with a byte after it, a tag not Nh long, an extension named twice, or of another suite.
        WireReader     in(view(info));
        mls::GroupInfo decoded;
        REQUIRE(mls::take_group_info(in, decoded));
        const auto try_info = [&](CryptoError expected, const mls::GroupInfo& changed)
        {
            const Bytes bytes = group_info_written(changed);
            refused(group, expected, view(hand_welcome(suite, joiners[0].package, good_secrets, bytes, joiner)),
                    joiners[0], config);
        };
        const Bytes longer = joined({view(info), view(Bytes{0U})});
        refused(group, CryptoError::Malformed,
                view(hand_welcome(suite, joiners[0].package, good_secrets, longer, joiner)), joiners[0], config);
        mls::GroupInfo changed   = decoded;
        changed.confirmation_tag = ConstSpan<u8>(decoded.confirmation_tag.data(), hash - 1U);
        try_info(CryptoError::Malformed, changed);
        const Bytes twice  = joined({view(extension(0xFF00U, {})), view(extension(0xFF00U, {}))});
        changed            = decoded;
        changed.extensions = view(twice);
        try_info(CryptoError::Malformed, changed);
        changed                    = decoded;
        changed.context.extensions = view(twice);
        try_info(CryptoError::Malformed, changed);
        changed                      = decoded;
        changed.context.cipher_suite = 0x0003U;
        try_info(CryptoError::Unsupported, changed);
        // Well formed and of the joiner's epoch, but this joiner secret is not the epoch's: the tag fails.
        try_info(CryptoError::Forged, decoded);
        // A GroupInfo naming a signer leaf that is blank.
        changed        = decoded;
        changed.signer = 7U;
        try_info(CryptoError::Forged, changed);
        // A GroupInfo of three bytes, and a sealed GroupInfo shorter than its tag.
        const Bytes tiny = filled(3U, 0U);
        refused(group, CryptoError::Malformed,
                view(hand_welcome(suite, joiners[0].package, good_secrets, tiny, joiner)), joiners[0], config);
        refused(group, CryptoError::Forged,
                view(hand_welcome(suite, joiners[0].package, good_secrets, tiny, joiner, false)), joiners[0], config);
    }
    SECTION("a group the member is in, a member the Authentication Service refuses, an expired leaf")
    {
        store.groups.push_back(Bytes(creator.group.context().group_id.begin(), creator.group.context().group_id.end()));
        refused(group, CryptoError::Replayed, view(honest.welcome), joiners[0], config);
        store.groups.clear();
        authority.refused = creator.id.credential;
        refused(group, CryptoError::Forged, view(honest.welcome), joiners[0], config);
        authority.refused = Bytes();
        // The creator's key-package leaf lives forever: more than the default maximum, unless tree leaves are not
        // checked.
        mls::GroupConfig strict = config;
        strict.policy           = mls::LeafPolicy{};
        strict.policy.now       = 1700000000U;
        refused(group, CryptoError::Expired, view(honest.welcome), joiners[0], strict);
        strict.policy.tree_lifetimes = false;
        mls::Group lenient;
        CHECK(lenient.join(view(honest.welcome), joiners[0].package, view(joiners[0].init_private),
                           view(joiners[0].id.hpke_private), {}, strict) == CryptoError::None);
    }
    SECTION("a parent whose key no agreement can use, its parent hashes and signatures valid")
    {
        // A committer that publishes a small-order X25519 key at the root of two leaves, the commit leaf's parent
        // hash computed over it (section 7.9: ParentHashInput of the key, the root's empty parent hash and the
        // sibling's tree hash) and the leaf signed: every check before the key's passes.
        Identity committer;
        make_identity(suite, committer, 5U);
        const Bytes      group_id = random_of(32U);
        mls::RatchetTree two;
        u32              index = 0U;
        REQUIRE(two.create(suite, committer.leaf) == CryptoError::None);
        REQUIRE(two.add(joiners[0].package.leaf_node, index) == CryptoError::None);
        const Bytes weak = filled(32U, 0U);
        const auto  vector = [](ConstSpan<u8> bytes)
        {
            Bytes      out = filled(mls::vector_bytes(bytes.size()), 0U);
            WireWriter writer(span_of(out));
            mls::put_vector(writer, bytes);
            return out;
        };
        const Bytes input = joined({view(vector(view(weak))), view(vector({})), view(vector(two.tree_hash(2U)))});
        Bytes       parent_hash = filled(32U, 0U);
        REQUIRE(kdf_hash(suite.kdf, view(input), span_of(parent_hash)) == CryptoError::None);
        mls::LeafNode leaf  = committer.leaf;
        leaf.source         = mls::LeafNodeSource::Commit;
        leaf.parent_hash    = view(parent_hash);
        leaf.signature      = {};
        Bytes signature     = filled(signature_sizes(suite.signature).signature, 0U);
        usize written_bytes = 0U;
        REQUIRE(mls::sign_leaf_node(suite, committer.signer, leaf, view(group_id), 0U, span_of(signature),
                                    written_bytes) == CryptoError::None);
        signature.resize(written_bytes);
        leaf.signature = view(signature);
        const mls::ParentNode root{view(weak), {}, {}};
        const Bytes           leaf_bytes   = written(leaf, mls::leaf_node_bytes, mls::put_leaf_node);
        const Bytes           parent_bytes = written(root, mls::parent_node_bytes, mls::put_parent_node);
        const Bytes           joiner_bytes =
            written(joiners[0].package.leaf_node, mls::leaf_node_bytes, mls::put_leaf_node);
        const Bytes nodes = joined({view(Bytes{1U, 1U}), view(leaf_bytes), view(Bytes{1U, 2U}), view(parent_bytes),
                                    view(Bytes{1U, 1U}), view(joiner_bytes)});
        const Bytes tree  = vector(view(nodes));
        mls::RatchetTree hostile;
        REQUIRE(hostile.decode(suite, view(tree)) == CryptoError::None);
        REQUIRE(hostile.validate(verifier, view(group_id)) == CryptoError::None);
        // Its GroupInfo, signed by the committer over the hostile tree's hash.
        const Bytes             confirmed = random_of(32U);
        const Bytes             tag       = random_of(32U);
        const mls::GroupContext context{suite.id, view(group_id), 1U, hostile.root_hash(), view(confirmed), {}};
        mls::GroupInfo          info;
        info.context          = context;
        info.confirmation_tag = view(tag);
        Bytes info_signature  = filled(signature_sizes(suite.signature).signature, 0U);
        REQUIRE(mls::sign_group_info(suite, committer.signer, info, span_of(info_signature), written_bytes) ==
                CryptoError::None);
        info_signature.resize(written_bytes);
        info.signature              = view(info_signature);
        const Bytes             joiner  = random_of(32U);
        const mls::GroupSecrets secrets{view(joiner), false, {}, {}};
        const Bytes welcome = hand_welcome(suite, joiners[0].package,
                                           written(secrets, mls::group_secrets_bytes, mls::put_group_secrets),
                                           group_info_written(info), joiner);
        refused(group, CryptoError::BadKey, view(welcome), joiners[0], config, view(tree));
    }
    SECTION("a key package whose leaf is not the tree's, its encryption key the same")
    {
        // The Welcome addresses this package, and the tree holds a leaf with its encryption key, but not its leaf.
        Joiner other;
        make_joiner(suite, other, 1U);
        other.id.extension_types = codepoints({0xFF00U});
        other.id.hpke_private    = joiners[0].id.hpke_private;
        other.id.hpke_public     = joiners[0].id.hpke_public;
        sign_leaf(suite, other.id, mls::LeafNodeSource::KeyPackage, {}, 0U);
        other.package.leaf_node = other.id.leaf;
        sign_package(suite, other);
        const Bytes             joiner = random_of(32U);
        const mls::GroupSecrets secrets{view(joiner), false, {}, {}};
        const Bytes             welcome =
            hand_welcome(suite, other.package, written(secrets, mls::group_secrets_bytes, mls::put_group_secrets),
                         honest.group_info, joiner);
        refused(group, CryptoError::BadKey, view(welcome), other, config);
    }
    SECTION("an external sender the Authentication Service refuses")
    {
        const Bytes         server_key = random_of(32U);
        const Bytes         server     = Bytes{'d', 'e', 'l', 'i', 'v', 'e', 'r', 'y'};
        mls::ExternalSender sender;
        sender.signature_key      = view(server_key);
        sender.credential.type    = 1U;
        sender.credential.content = view(server);
        const Bytes entry         = written(sender, mls::external_sender_bytes, mls::put_external_sender);
        Bytes       list          = filled(mls::vector_bytes(entry.size()), 0U);
        WireWriter  writer(span_of(list));
        mls::put_vector(writer, view(entry));
        const Bytes extensions = extension(5U, view(list));
        Creator     with_senders;
        create_group(suite, with_senders, config, view(extensions));
        NextEpoch epoch;
        commit_adds(suite, with_senders.id, with_senders.group, joiners, 1U, AddOptions{}, epoch);
        mls::Group fine;
        CHECK(fine.join(view(epoch.welcome), joiners[0].package, view(joiners[0].init_private),
                        view(joiners[0].id.hpke_private), {}, config) == CryptoError::None);
        CHECK(equal(fine.context().extensions, view(extensions)));
        authority.refused = server;
        refused(group, CryptoError::Forged, view(epoch.welcome), joiners[0], config);
        authority.refused = Bytes();
    }
    SECTION("a leaf key that is not the member's")
    {
        Joiner wrong_leaf;
        make_joiner(suite, wrong_leaf, 1U);
        wrong_leaf.package      = joiners[0].package;
        wrong_leaf.init_private = joiners[0].init_private;
        refused(group, CryptoError::BadKey, view(honest.welcome), wrong_leaf, config);
    }
    SECTION("a GroupContext extension the member does not support")
    {
        Creator extended;
        make_identity(suite, extended.id, 0U);
        extended.id.extension_types = codepoints({0xFF00U});
        sign_leaf(suite, extended.id, mls::LeafNodeSource::KeyPackage, {}, 0U);
        const Bytes custom   = extension(0xFF00U, Bytes{1U, 2U, 3U});
        const Bytes group_id = random_of(32U);
        REQUIRE(extended.group.create(suite, view(group_id), view(custom), extended.id.leaf,
                                      view(extended.id.hpke_private), config) == CryptoError::None);
        NextEpoch epoch;
        commit_adds(suite, extended.id, extended.group, joiners, 1U, AddOptions{}, epoch);
        refused(group, CryptoError::Unsupported, view(epoch.welcome), joiners[0], config);
    }
}

// ---- Creating a group ----

TEST_CASE("MLS: creating a group checks its creator and its extensions", "[crypto][net-2][mls]")
{
    const mls::CipherSuite& suite = suite_of(0x0003U);
    const ProviderVerifier  verifier;
    TestAuthority           authority;
    const TestStore         store;
    const mls::GroupConfig  config{&verifier, &authority, &store, timeless(), {}};
    Identity                creator;
    make_identity(suite, creator, 0U);
    const Bytes group_id = random_of(32U);
    mls::Group  group;

    // Two groups of one creator differ in every secret: each epoch 0 starts from fresh randomness.
    mls::Group other;
    REQUIRE(group.create(suite, view(group_id), {}, creator.leaf, view(creator.hpke_private), config) ==
            CryptoError::None);
    REQUIRE(other.create(suite, view(group_id), {}, creator.leaf, view(creator.hpke_private), config) ==
            CryptoError::None);
    CHECK_FALSE(equal(group.epoch_authenticator(), other.epoch_authenticator()));
    CHECK(equal(group.tree().root_hash(), other.tree().root_hash()));
    CHECK(group.keys().holds(0U));
    CHECK(group.secret_tree().leaves() == 1U);

    const auto refuses = [&](CryptoError expected, const mls::CipherSuite& with, ConstSpan<u8> extensions,
                             const mls::LeafNode& leaf, ConstSpan<u8> leaf_private, const mls::GroupConfig& cfg)
    {
        CHECK(group.create(with, view(group_id), extensions, leaf, leaf_private, cfg) == expected);
        CHECK_FALSE(group.ready());
        CHECK_FALSE(group.tree().ready());
        CHECK(group.epoch_authenticator().empty());
    };
    mls::CipherSuite outside = suite;
    outside.id               = 0x0004U;
    refuses(CryptoError::Unsupported, outside, {}, creator.leaf, view(creator.hpke_private), config);
    const mls::GroupConfig no_verifier{nullptr, &authority, &store, timeless(), {}};
    refuses(CryptoError::Unsupported, suite, {}, creator.leaf, view(creator.hpke_private), no_verifier);
    const Bytes twice = joined({view(extension(0xFF00U, {})), view(extension(0xFF00U, {}))});
    refuses(CryptoError::Malformed, suite, view(twice), creator.leaf, view(creator.hpke_private), config);
    const Bytes unlisted = extension(0xFF00U, {});
    refuses(CryptoError::Unsupported, suite, view(unlisted), creator.leaf, view(creator.hpke_private), config);
    Identity  stranger;
    make_identity(suite, stranger, 1U);
    refuses(CryptoError::BadKey, suite, {}, creator.leaf, view(stranger.hpke_private), config);
    mls::LeafNode forged = creator.leaf;
    forged.signature     = view(stranger.signature);
    refuses(CryptoError::Forged, suite, {}, forged, view(creator.hpke_private), config);
    authority.refused = creator.credential;
    refuses(CryptoError::Forged, suite, {}, creator.leaf, view(creator.hpke_private), config);
    authority.refused = Bytes();
    mls::GroupConfig strict = config;
    strict.policy           = mls::LeafPolicy{};
    strict.policy.now       = 1700000000U;
    refuses(CryptoError::Expired, suite, {}, creator.leaf, view(creator.hpke_private), strict);
    sign_leaf(suite, creator, mls::LeafNodeSource::Update, view(group_id), 0U);
    refuses(CryptoError::Malformed, suite, {}, creator.leaf, view(creator.hpke_private), config);
}

// ---- Validation against a group (sections 7.3, 10.1, 11.1 and 13) ----

TEST_CASE("MLS: a leaf node is checked against its group as section 7.3 says, in order", "[crypto][net-2][mls]")
{
    const mls::CipherSuite& suite = suite_of(0x0002U);
    TestAuthority           authority;
    Identity                id;
    make_identity(suite, id, 1U);
    id.extension_types = codepoints({0xFF00U, 0x0A0AU});
    id.credentials     = codepoints({1U, 0xFF03U});
    sign_leaf(suite, id, mls::LeafNodeSource::KeyPackage, {}, 0U);
    const mls::GroupContext group{suite.id, {}, 0U, {}, {}, {}};
    const mls::LeafPolicy   policy = timeless();
    const auto check = [&](const mls::LeafNode& leaf, mls::LeafRole role = mls::LeafRole::KeyPackage,
                           const mls::GroupContext& context = mls::GroupContext{0x0002U, {}, 0U, {}, {}, {}},
                           const mls::LeafPolicy& with = timeless())
    { return mls::check_leaf(context, leaf, role, with, authority); };
    CHECK(check(id.leaf) == CryptoError::None);
    CHECK(check(id.leaf, mls::LeafRole::Tree) == CryptoError::None);

    // The source the role admits.
    CHECK(check(id.leaf, mls::LeafRole::Update) == CryptoError::Malformed);
    CHECK(check(id.leaf, mls::LeafRole::Commit) == CryptoError::Malformed);
    mls::LeafNode update = id.leaf;
    update.source        = mls::LeafNodeSource::Update;
    CHECK(check(update, mls::LeafRole::Update) == CryptoError::None);
    CHECK(check(update, mls::LeafRole::Tree) == CryptoError::None);
    CHECK(check(update, mls::LeafRole::KeyPackage) == CryptoError::Malformed);
    mls::LeafNode commit = id.leaf;
    commit.source        = mls::LeafNodeSource::Commit;
    CHECK(check(commit, mls::LeafRole::Commit) == CryptoError::None);

    // The leaf's extensions: each type once, each non-default type listed; a default one need not be.
    mls::LeafNode with_extensions = id.leaf;
    const Bytes   listed          = joined({view(extension(0xFF00U, {})), view(extension(1U, Bytes{7U}))});
    with_extensions.extensions    = view(listed);
    CHECK(check(with_extensions) == CryptoError::None);
    const Bytes repeated       = joined({view(extension(0xFF00U, {})), view(extension(0xFF00U, {}))});
    with_extensions.extensions = view(repeated);
    CHECK(check(with_extensions) == CryptoError::Malformed);
    const Bytes unlisted       = extension(0xFF01U, {});
    with_extensions.extensions = view(unlisted);
    CHECK(check(with_extensions) == CryptoError::Unsupported);

    // An encryption key no agreement can use: off the curve, or of the wrong length.
    mls::LeafNode bad_key = id.leaf;
    Bytes         point   = Bytes(id.leaf.encryption_key.begin(), id.leaf.encryption_key.end());
    point[64] ^= 0x01U;
    bad_key.encryption_key = view(point);
    CHECK(check(bad_key) == CryptoError::BadKey);
    bad_key.encryption_key = ConstSpan<u8>(point.data(), 64U);
    CHECK(check(bad_key) == CryptoError::BadKey);
    // An X25519 key of small order (u = 0) in a suite that uses X25519.
    const Bytes             zero_point = filled(32U, 0U);
    const mls::GroupContext x25519{0x0001U, {}, 0U, {}, {}, {}};
    Identity                ed;
    make_identity(suite_of(0x0001U), ed, 2U);
    mls::LeafNode small_order  = ed.leaf;
    small_order.encryption_key = view(zero_point);
    CHECK(check(ed.leaf, mls::LeafRole::KeyPackage, x25519) == CryptoError::None);
    CHECK(check(small_order, mls::LeafRole::KeyPackage, x25519) == CryptoError::BadKey);

    // The Authentication Service.
    authority.refused = id.credential;
    CHECK(check(id.leaf) == CryptoError::Forged);
    authority.refused = Bytes();

    // The capabilities: the leaf's own credential type. Whether they name the group's version and suite is not a
    // check of the leaf (decision M8): the working group's suite-7 welcome key package lists suites 1 to 6 only.
    mls::LeafNode lacking         = id.leaf;
    const Bytes   other_version   = codepoints({2U});
    lacking.capabilities.versions = view(other_version);
    CHECK(check(lacking) == CryptoError::None);
    lacking                            = id.leaf;
    const Bytes other_suites           = codepoints({1U, 3U, 7U, 0x0A0AU});
    lacking.capabilities.cipher_suites = view(other_suites);
    CHECK(check(lacking) == CryptoError::None);
    lacking                          = id.leaf;
    const Bytes x509_only            = codepoints({2U});
    lacking.capabilities.credentials = view(x509_only);
    CHECK(check(lacking) == CryptoError::Unsupported);
    // A group of another suite checks the key in its own KEM before the capabilities: a P-256 key is no X25519 key.
    CHECK(check(id.leaf, mls::LeafRole::KeyPackage, mls::GroupContext{0x0001U, {}, 0U, {}, {}, {}}) ==
          CryptoError::BadKey);

    // The GroupContext's extensions and its required capabilities (section 11.1).
    const auto with_context = [&](const Bytes& extensions)
    {
        return check(id.leaf, mls::LeafRole::KeyPackage,
                     mls::GroupContext{suite.id, {}, 0U, {}, {}, view(extensions)});
    };
    CHECK(with_context(extension(0xFF00U, {})) == CryptoError::None);
    CHECK(with_context(extension(0xFF02U, {})) == CryptoError::Unsupported);
    CHECK(with_context(extension(5U, {})) == CryptoError::None); // external_senders: a default type
    const auto required = [&](std::initializer_list<u16> extensions, std::initializer_list<u16> proposals,
                              std::initializer_list<u16> credentials)
    {
        const Bytes                     e = codepoints(extensions);
        const Bytes                     p = codepoints(proposals);
        const Bytes                     c = codepoints(credentials);
        const mls::RequiredCapabilities needed{view(e), view(p), view(c)};
        const Bytes data = written(needed, mls::required_capabilities_bytes, mls::put_required_capabilities);
        return with_context(extension(3U, view(data)));
    };
    CHECK(required({}, {}, {}) == CryptoError::None);
    CHECK(required({0xFF00U, 2U}, {1U, 7U}, {1U, 0xFF03U}) == CryptoError::None);
    CHECK(required({0xFF01U}, {}, {}) == CryptoError::Unsupported);
    CHECK(required({}, {0xFF02U}, {}) == CryptoError::Unsupported);
    CHECK(required({}, {}, {2U}) == CryptoError::Unsupported);
    CHECK(with_context(extension(3U, Bytes{0x01U})) == CryptoError::Malformed);

    // The lifetime of a key-package leaf: within it at `now`, and no longer than the maximum.
    mls::LeafPolicy bounded = policy;
    bounded.max_lifetime    = 1000U;
    mls::LeafNode timed     = id.leaf;
    timed.not_before        = policy.now - 500U;
    timed.not_after         = policy.now + 500U;
    CHECK(check(timed, mls::LeafRole::KeyPackage, group, bounded) == CryptoError::None);
    timed.not_after = policy.now + 501U;
    CHECK(check(timed, mls::LeafRole::KeyPackage, group, bounded) == CryptoError::Expired);
    timed.not_before = policy.now + 1U;
    timed.not_after  = policy.now + 2U;
    CHECK(check(timed, mls::LeafRole::KeyPackage, group, bounded) == CryptoError::Expired);
    timed.not_before = policy.now - 2U;
    timed.not_after  = policy.now - 1U;
    CHECK(check(timed, mls::LeafRole::KeyPackage, group, bounded) == CryptoError::Expired);
    timed.not_before = policy.now + 1U;
    timed.not_after  = policy.now - 1U;
    CHECK(check(timed, mls::LeafRole::KeyPackage, group, bounded) == CryptoError::Expired);
    // In a joined tree only when the policy says so; an update leaf has no lifetime.
    timed.not_before = 0U;
    CHECK(check(timed, mls::LeafRole::Tree, group, bounded) == CryptoError::Expired);
    bounded.tree_lifetimes = false;
    CHECK(check(timed, mls::LeafRole::Tree, group, bounded) == CryptoError::None);
    CHECK(check(timed, mls::LeafRole::KeyPackage, group, bounded) == CryptoError::Expired);
    CHECK(check(update, mls::LeafRole::Update, group, bounded) == CryptoError::None);
    CHECK(check(id.leaf, mls::LeafRole::KeyPackage, mls::GroupContext{0x0009U, {}, 0U, {}, {}, {}}) ==
          CryptoError::Unsupported);
    CHECK(mls::kDefaultMaxLifetime == 12U * 7U * 86400U + 3600U);
}

TEST_CASE("MLS: every member lists every credential type in use (section 13.3)", "[crypto][net-2][mls]")
{
    const mls::CipherSuite& suite = suite_of(0x0001U);
    Identity                a;
    Identity                b;
    make_identity(suite, a, 1U);
    make_identity(suite, b, 2U);
    b.leaf.credential.type = 0xFF03U;
    b.credentials          = codepoints({1U, 0xFF03U});
    b.leaf.capabilities.credentials = view(b.credentials);
    mls::RatchetTree tree;
    REQUIRE(tree.create(suite, a.leaf) == CryptoError::None);
    CHECK(mls::check_credential_types(tree) == CryptoError::None);
    u32 index = 0U;
    REQUIRE(tree.add(b.leaf, index) == CryptoError::None);
    CHECK(mls::check_credential_types(tree) == CryptoError::Unsupported); // a does not list 0xFF03
    a.credentials                   = codepoints({0xFF03U, 1U});
    a.leaf.capabilities.credentials = view(a.credentials);
    REQUIRE(tree.update(0U, a.leaf) == CryptoError::None);
    CHECK(mls::check_credential_types(tree) == CryptoError::None);
    // b listing only its own type no longer supports a's basic credential.
    b.credentials                   = codepoints({0xFF03U});
    b.leaf.capabilities.credentials = view(b.credentials);
    REQUIRE(tree.update(index, b.leaf) == CryptoError::None);
    CHECK(mls::check_credential_types(tree) == CryptoError::Unsupported);
    REQUIRE(tree.remove(0U) == CryptoError::None);
    CHECK(mls::check_credential_types(tree) == CryptoError::None);
}

TEST_CASE("MLS: a key package is checked as section 10.1 says", "[crypto][net-2][mls]")
{
    const mls::CipherSuite& suite = suite_of(0x0007U);
    const ProviderVerifier  verifier;
    const TestAuthority     authority;
    Joiner                  joiner;
    make_joiner(suite, joiner, 1U);
    const mls::GroupContext group{suite.id, {}, 0U, {}, {}, {}};
    const auto              check = [&](const mls::KeyPackage& package)
    { return mls::check_key_package(group, package, verifier, authority, timeless()); };
    CHECK(check(joiner.package) == CryptoError::None);

    mls::KeyPackage changed = joiner.package;
    changed.version         = 2U;
    CHECK(check(changed) == CryptoError::Unsupported);
    changed              = joiner.package;
    changed.cipher_suite = 0x0002U;
    CHECK(check(changed) == CryptoError::Unsupported);
    // The package's own fields, each re-signed so that only the field is wrong.
    const Bytes twice          = joined({view(extension(0xFF00U, {})), view(extension(0xFF00U, {}))});
    joiner.package.extensions  = view(twice);
    sign_package(suite, joiner);
    CHECK(check(joiner.package) == CryptoError::Malformed);
    // Section 10: a package's extensions are of types its leaf's capabilities list (a default type needs no listing);
    // an unknown one so listed is ignored (section 13.4), an unlisted one refused, GREASE types alike.
    const u16   other_grease = joiner.id.grease == 0x0A0AU ? 0x1A1AU : 0x0A0AU;
    const Bytes unknown      = joined({view(extension(joiner.id.grease, Bytes{9U})), view(extension(0x0001U, {}))});
    joiner.package.extensions = view(unknown);
    sign_package(suite, joiner);
    CHECK(check(joiner.package) == CryptoError::None);
    for (const u16 unlisted_type : {u16{0xFF00U}, other_grease})
    {
        const Bytes unlisted      = joined({view(extension(joiner.id.grease, {})), view(extension(unlisted_type, {}))});
        joiner.package.extensions = view(unlisted);
        sign_package(suite, joiner);
        CHECK(check(joiner.package) == CryptoError::Unsupported);
    }
    joiner.package.extensions = view(unknown);
    joiner.package.init_key   = joiner.package.leaf_node.encryption_key;
    sign_package(suite, joiner);
    CHECK(check(joiner.package) == CryptoError::BadKey);
    Bytes off_curve           = joiner.init_public;
    off_curve[96] ^= 0x01U;
    joiner.package.init_key = view(off_curve);
    sign_package(suite, joiner);
    CHECK(check(joiner.package) == CryptoError::BadKey);
    joiner.package.init_key = view(joiner.init_public);
    sign_package(suite, joiner);
    CHECK(check(joiner.package) == CryptoError::None);
    // Signatures: the package's and the leaf's, each under the leaf's key.
    Bytes signature            = joiner.signature;
    signature[signature.size() / 2U] ^= 0x01U;
    changed                    = joiner.package;
    changed.signature          = view(signature);
    CHECK(check(changed) == CryptoError::Forged);
    changed                    = joiner.package;
    Bytes leaf_signature       = joiner.id.signature;
    leaf_signature[leaf_signature.size() / 2U] ^= 0x01U;
    changed.leaf_node.signature = view(leaf_signature);
    CHECK(check(changed) == CryptoError::Forged);
    // The leaf's checks, as a key package's: a leaf of another source.
    sign_leaf(suite, joiner.id, mls::LeafNodeSource::Update, {}, 0U);
    joiner.package.leaf_node = joiner.id.leaf;
    sign_package(suite, joiner);
    CHECK(check(joiner.package) == CryptoError::Malformed);

    // KeyPackageRef: RefHash over the package's encoding; a package that does not encode is BadLength.
    Bytes reference = filled(48U, 0U);
    REQUIRE(mls::key_package_ref(suite, joiner.package, span_of(reference)) == CryptoError::None);
    const Bytes encoding = written(joiner.package, mls::key_package_bytes, mls::put_key_package);
    Bytes       restated = filled(48U, 0U);
    REQUIRE(mls::ref_hash(suite, "MLS 1.0 KeyPackage Reference", view(encoding), span_of(restated)) ==
            CryptoError::None);
    CHECK(equal(reference, restated));
    CHECK(mls::key_package_ref(suite, joiner.package, Span<u8>(reference.data(), 32U)) == CryptoError::BadLength);
    mls::KeyPackage unencodable          = joiner.package;
    unencodable.leaf_node.credential.type = 0U;
    CHECK(mls::key_package_ref(suite, unencodable, span_of(reference)) == CryptoError::BadLength);
}

// ---- Extensions and codecs ----

TEST_CASE("MLS: extension lists, capabilities and the group's extensions", "[crypto][net-2][mls]")
{
    // Default types are supported unlisted; others only listed.
    for (u16 type = 1U; type <= 5U; ++type)
    {
        CHECK(mls::default_extension(type));
    }
    for (u16 type = 1U; type <= 7U; ++type)
    {
        CHECK(mls::default_proposal(type));
    }
    for (const u16 type : {u16{0U}, u16{6U}, u16{0x0A0AU}, u16{0xFF00U}})
    {
        CHECK_FALSE(mls::default_extension(type));
    }
    CHECK_FALSE(mls::default_proposal(0U));
    CHECK_FALSE(mls::default_proposal(8U));
    const Bytes       listed = codepoints({0x0A0AU, 0xFF00U});
    mls::Capabilities caps;
    caps.extensions = view(listed);
    caps.proposals  = view(listed);
    CHECK(mls::lists(view(listed), 0xFF00U));
    CHECK_FALSE(mls::lists(view(listed), 0xFF01U));
    CHECK_FALSE(mls::lists({}, 1U));
    CHECK(mls::supports_extension(caps, 2U));
    CHECK(mls::supports_extension(caps, 0xFF00U));
    CHECK_FALSE(mls::supports_extension(caps, 0xFF01U));
    CHECK(mls::supports_proposal(caps, 7U));
    CHECK(mls::supports_proposal(caps, 0xFF00U));
    CHECK_FALSE(mls::supports_proposal(caps, 8U));
    // A GREASE value is never supported, listed or not: capabilities are compared without them (section 13.5).
    CHECK(mls::lists(view(listed), 0x0A0AU));
    CHECK_FALSE(mls::supports_extension(caps, 0x0A0AU));
    CHECK_FALSE(mls::supports_proposal(caps, 0x0A0AU));
    caps.credentials = view(listed);
    CHECK(mls::supports_credential(caps, 0xFF00U));
    CHECK_FALSE(mls::supports_credential(caps, 0x0A0AU));
    CHECK_FALSE(mls::supports_credential(caps, 1U));

    // Distinct types: pair by pair up to 32 entries, by a bitmap past them; a body that is not a list is refused.
    CHECK(mls::extensions_distinct({}));
    for (const usize count : {usize{1U}, usize{2U}, usize{32U}, usize{33U}, usize{300U}})
    {
        INFO(count << " entries");
        Bytes list;
        for (usize i = 0U; i < count; ++i)
        {
            list = joined({view(list), view(extension(static_cast<u16>(0x1000U + i), Bytes{static_cast<u8>(i)}))});
        }
        CHECK(mls::extensions_distinct(view(list)));
        ConstSpan<u8> data;
        REQUIRE(mls::find_extension(view(list), static_cast<u16>(0x1000U + count - 1U), data));
        CHECK((data.size() == 1U && data[0] == static_cast<u8>(count - 1U)));
        CHECK_FALSE(mls::find_extension(view(list), 0x0FFFU, data));
        CHECK(data.empty());
        const Bytes repeated = joined({view(list), view(extension(0x1000U, {}))});
        CHECK_FALSE(mls::extensions_distinct(view(repeated)));
        const Bytes broken = joined({view(list), view(Bytes{0x00U})});
        CHECK_FALSE(mls::extensions_distinct(view(broken)));
    }

    // RequiredCapabilities: written back; an odd list, a short one or a byte left over refused.
    const Bytes                     e = codepoints({0xFF00U});
    const Bytes                     p = codepoints({8U, 9U});
    const Bytes                     c = codepoints({1U});
    const mls::RequiredCapabilities required{view(e), view(p), view(c)};
    const Bytes data = written(required, mls::required_capabilities_bytes, mls::put_required_capabilities);
    mls::RequiredCapabilities decoded;
    REQUIRE(mls::take_required_capabilities(view(data), decoded));
    CHECK(equal(written(decoded, mls::required_capabilities_bytes, mls::put_required_capabilities), view(data)));
    const Bytes trailing = joined({view(data), view(Bytes{0U})});
    CHECK_FALSE(mls::take_required_capabilities(view(trailing), decoded));
    CHECK_FALSE(mls::take_required_capabilities(ConstSpan<u8>(data.data(), data.size() - 1U), decoded));
    const u8 odd[] = {0x01U, 0xFFU, 0x00U, 0x00U};
    CHECK_FALSE(mls::take_required_capabilities(odd, decoded));
    const mls::RequiredCapabilities unwritable{ConstSpan<u8>(odd, 1U), {}, {}};
    CHECK(mls::required_capabilities_bytes(unwritable) == 0U);

    // The external_senders extension: written back; a sender with no credential type or a byte left over refused.
    const Bytes        key      = random_of(32U);
    const Bytes        identity = Bytes{'s', 'e', 'r', 'v', 'e', 'r'};
    mls::ExternalSender sender;
    sender.signature_key      = view(key);
    sender.credential.type    = 1U;
    sender.credential.content = view(identity);
    const Bytes one   = written(sender, mls::external_sender_bytes, mls::put_external_sender);
    const Bytes list  = joined({view(one), view(one)});
    Bytes       field = filled(mls::vector_bytes(list.size()), 0U);
    WireWriter  writer(span_of(field));
    mls::put_vector(writer, view(list));
    ConstSpan<u8> body;
    REQUIRE(mls::take_external_senders(view(field), body));
    CHECK(equal(body, view(list)));
    WireReader          in(body);
    mls::ExternalSender back;
    REQUIRE(mls::take_external_sender(in, back));
    CHECK(equal(back.signature_key, view(key)));
    CHECK(equal(back.credential.content, view(identity)));
    const Bytes extra = joined({view(field), view(Bytes{0U})});
    CHECK_FALSE(mls::take_external_senders(view(extra), body));
    sender.credential.type = 0U;
    CHECK(mls::external_sender_bytes(sender) == 0U);

    // A GroupContext of another version is not one.
    const mls::GroupContext context{1U, view(key), 3U, view(key), {}, {}};
    Bytes                   encoded = context_bytes(context);
    encoded[1]                      = 2U;
    WireReader        reader(view(encoded));
    mls::GroupContext read;
    CHECK_FALSE(mls::take_group_context(reader, read));
}

TEST_CASE("MLS: GroupSecrets, Welcomes and their sealing refuse what they cannot carry", "[crypto][net-2][mls]")
{
    const mls::CipherSuite& suite = suite_of(0x0001U);
    // GroupSecrets: with and without a path secret, written back; a presence octet of 2 refused.
    const Bytes       joiner = random_of(32U);
    const Bytes       path   = random_of(32U);
    mls::GroupSecrets secrets{view(joiner), true, view(path), {}};
    for (const bool with_path : {true, false})
    {
        secrets.has_path_secret = with_path;
        secrets.path_secret     = with_path ? view(path) : ConstSpan<u8>();
        const Bytes       bytes = written(secrets, mls::group_secrets_bytes, mls::put_group_secrets);
        WireReader        in(view(bytes));
        mls::GroupSecrets back;
        REQUIRE(mls::take_group_secrets(in, back));
        CHECK(in.done());
        CHECK(back.has_path_secret == with_path);
        CHECK(equal(written(back, mls::group_secrets_bytes, mls::put_group_secrets), view(bytes)));
    }
    Bytes bad_presence = written(secrets, mls::group_secrets_bytes, mls::put_group_secrets);
    bad_presence[33]   = 2U;
    WireReader        in(view(bad_presence));
    mls::GroupSecrets back;
    CHECK_FALSE(mls::take_group_secrets(in, back));

    // seal_welcome: a joiner secret not Nh long, a member of another suite or with a short path secret.
    Joiner member;
    make_joiner(suite, member, 1U);
    mls::WelcomeMember         entry{member.package, {}};
    crd::containers::Array<u8> out;
    out.push_back(1U);
    const Bytes info = random_of(64U);
    CHECK(mls::seal_welcome(suite, ConstSpan<u8>(joiner.data(), 31U), {}, view(info),
                            ConstSpan<mls::WelcomeMember>(&entry, 1U), out) == CryptoError::BadLength);
    CHECK(out.empty());
    entry.path_secret = ConstSpan<u8>(path.data(), 31U);
    CHECK(mls::seal_welcome(suite, view(joiner), {}, view(info), ConstSpan<mls::WelcomeMember>(&entry, 1U), out) ==
          CryptoError::BadLength);
    entry.path_secret          = {};
    entry.package.cipher_suite = 0x0003U;
    CHECK(mls::seal_welcome(suite, view(joiner), {}, view(info), ConstSpan<mls::WelcomeMember>(&entry, 1U), out) ==
          CryptoError::Unsupported);
    CHECK(out.empty());
    // A Welcome for nobody seals the GroupInfo alone.
    REQUIRE(mls::seal_welcome(suite, view(joiner), {}, view(info), {}, out) == CryptoError::None);
    WireReader   welcome(ConstSpan<u8>(out.data(), out.size()));
    mls::Welcome decoded;
    REQUIRE(mls::take_welcome(welcome, decoded));
    CHECK(decoded.secrets.empty());
    CHECK(decoded.encrypted_group_info.size() == info.size() + kAeadTagBytes);
}
} // namespace
