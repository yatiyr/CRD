// crd-fuzz-crypto-mls-group: an MLS group's extensions, GroupInfo, GroupSecrets and Welcome, and joining from a
// Welcome (NET.2; docs/design/network-foundation.md#mls) under the bounded fuzz harness (REPO.DEV.9;
// docs/design/test-instruments.md).
//
// Input. The first byte picks the mode, the rest is its data, read field by field (a short input ends the case):
//   0  decoding: the rest as a Welcome, a GroupInfo, a GroupSecrets, a GroupContext, the data of a
//      required_capabilities and an external_senders extension, and an Extension list's body;
//   1  a Welcome changed: the suite, the byte to change (u16; past the end changes none) and its XOR mask, applied to
//      an honest Welcome a creator sealed to a joiner of its group; then whether the joiner fetches the tree.
//
// Oracles, none of them the code's verdict on the same call:
//   * whatever a decoder accepts writes back to exactly the bytes it took; an Extension list's verdict on repeated
//     types is a pairwise restatement's, and the extension each type finds is the first a linear scan meets;
//   * a changed Welcome is refused with the group left empty (no tree, keys, secrets or context), or it is the honest
//     Welcome and the joiner reaches exactly the honest epoch: no change yields another epoch that is accepted.
#include "mls_fuzz_support.hpp"

namespace
{
using crd::u16;
using crd::u32;
using crd::u64;
using crd::u8;
using crd::usize;
using crd::containers::Array;
using crd::containers::ConstSpan;
using crd::containers::Span;
using crd::containers::WireReader;
using crd::containers::WireWriter;
using namespace crd::crypto;
using namespace crd::crypto::fuzz_mls;

// A decoder over the whole input: what it accepts writes back to the bytes it took.
template <typename T, typename Take, typename Size, typename Put>
void round_trip(ConstSpan<u8> bytes, Take take, Size size, Put put)
{
    WireReader in(bytes);
    T          value{};
    if (take(in, value))
    {
        const usize taken = bytes.size() - in.rest().size();
        CRD_FUZZ_REQUIRE(size(value) == taken);
        CRD_FUZZ_REQUIRE(same(view(written(value, size, put)), bytes.first(taken)));
    }
}

// ---- Mode 0: decoding ----

// Section 13.4's rule restated: no two entries of the list share a type; the first entry of a type is what is found.
void check_extensions(ConstSpan<u8> body)
{
    Array<u16>           types;
    Array<ConstSpan<u8>> datas;
    WireReader           in(body);
    bool                 whole = true;
    while (!in.done())
    {
        u16           type = 0U;
        ConstSpan<u8> data;
        if (!in.take_u16(type) || !mls::take_vector(in, data))
        {
            whole = false;
            break;
        }
        types.push_back(type);
        datas.push_back(data);
    }
    bool distinct = whole;
    for (usize i = 0U; distinct && i < types.size(); ++i)
    {
        for (usize j = i + 1U; j < types.size(); ++j)
        {
            distinct = distinct && types[i] != types[j];
        }
    }
    CRD_FUZZ_REQUIRE(mls::extensions_distinct(body) == distinct);
    if (!whole)
    {
        return;
    }
    for (usize i = 0U; i < types.size(); ++i)
    {
        usize first = i;
        for (usize j = 0U; j < i; ++j)
        {
            if (types[j] == types[i])
            {
                first = j;
                break;
            }
        }
        ConstSpan<u8> found;
        CRD_FUZZ_REQUIRE(mls::find_extension(body, types[i], found));
        CRD_FUZZ_REQUIRE(found.data() == datas[first].data() && found.size() == datas[first].size());
    }
}

void decoding(Reader& in)
{
    const ConstSpan<u8> bytes = in.rest();
    round_trip<mls::Welcome>(bytes, mls::take_welcome, mls::welcome_bytes, mls::put_welcome);
    round_trip<mls::GroupInfo>(bytes, mls::take_group_info, mls::group_info_bytes, mls::put_group_info);
    round_trip<mls::GroupSecrets>(bytes, mls::take_group_secrets, mls::group_secrets_bytes, mls::put_group_secrets);
    round_trip<mls::GroupContext>(bytes, mls::take_group_context, mls::group_context_bytes, mls::put_group_context);
    round_trip<mls::ExternalSender>(bytes, mls::take_external_sender, mls::external_sender_bytes,
                                    mls::put_external_sender);
    mls::RequiredCapabilities required;
    if (mls::take_required_capabilities(bytes, required))
    {
        CRD_FUZZ_REQUIRE(same(view(written(required, mls::required_capabilities_bytes,
                                           mls::put_required_capabilities)),
                              bytes));
    }
    ConstSpan<u8> senders;
    if (mls::take_external_senders(bytes, senders))
    {
        WireReader list(senders);
        while (!list.done())
        {
            mls::ExternalSender sender;
            CRD_FUZZ_REQUIRE(mls::take_external_sender(list, sender));
        }
    }
    check_extensions(bytes);
}

// ---- Mode 1: a changed Welcome ----

// One suite's honest Welcome: a creator's group of one, a joiner added with no path, the next epoch's GroupInfo
// (with the tree) and the Welcome; the joiner's key package and the epoch authenticator it must reach.
struct Honest
{
    u8                 suites[2] = {};
    Member             creator;
    Member             joiner;
    mls::KeyPackage    package;
    Array<u8>          tree;
    Array<u8>          welcome;
    Array<u8>          authenticator;
    ProviderVerifier   verifier;
    AnyCredential      authority;
    EmptyStore         store;
    mls::GroupConfig   config;
};

void seal_honest(const mls::CipherSuite& suite, Honest& honest)
{
    honest.suites[0] = static_cast<u8>(suite.id >> 8U);
    honest.suites[1] = static_cast<u8>(suite.id);
    make_member(suite, honest.creator, honest.suites);
    make_member(suite, honest.joiner, honest.suites);
    make_package(suite, honest.joiner, honest.package);
    usize written_bytes = 0U;
    honest.config.verifier       = &honest.verifier;
    honest.config.authentication = &honest.authority;
    honest.config.store          = &honest.store;
    honest.config.policy.now          = 1700000000U;
    honest.config.policy.max_lifetime = ~u64{0};

    mls::Group  group;
    const u8    group_id[16] = {'f', 'u', 'z', 'z', '-', 'g', 'r', 'o', 'u', 'p'};
    CRD_FUZZ_REQUIRE(group.create(suite, group_id, {}, honest.creator.leaf, view(honest.creator.leaf_private),
                                  honest.config) == CryptoError::None);
    mls::RatchetTree tree;
    u32              index = 0U;
    CRD_FUZZ_REQUIRE(tree.copy_from(group.tree()) == CryptoError::None);
    CRD_FUZZ_REQUIRE(tree.add(honest.joiner.leaf, index) == CryptoError::None);
    honest.tree.resize(tree.encoded_bytes());
    WireWriter tree_writer(Span<u8>(honest.tree.data(), honest.tree.size()));
    tree.encode(tree_writer);

    const usize       hash                    = suite.hash_bytes();
    const u8          zero[mls::kMaxHashBytes] = {};
    u8                confirmed[mls::kMaxHashBytes];
    u8                tag[mls::kMaxHashBytes];
    CRD_FUZZ_REQUIRE(random_bytes(Span<u8>(confirmed, hash)) == CryptoError::None);
    const mls::GroupContext next{suite.id, group_id, 1U, tree.root_hash(), ConstSpan<u8>(confirmed, hash), {}};
    const Array<u8>         context = written(next, mls::group_context_bytes, mls::put_group_context);
    mls::EpochSecrets       secrets;
    CRD_FUZZ_REQUIRE(secrets.advance(suite, group.secret(mls::EpochSecret::Init), ConstSpan<u8>(zero, hash),
                                     ConstSpan<u8>(zero, hash), view(context)) == CryptoError::None);
    CRD_FUZZ_REQUIRE(mls::confirmation_tag(suite, secrets.get(mls::EpochSecret::Confirmation),
                                           ConstSpan<u8>(confirmed, hash), Span<u8>(tag, hash)) == CryptoError::None);
    honest.authenticator.resize(hash);
    std::memcpy(honest.authenticator.data(), secrets.get(mls::EpochSecret::Authentication).data(), hash);

    // The GroupInfo carries the tree; the joiner may also be handed it.
    Array<u8> extension;
    extension.resize(2U + mls::vector_bytes(honest.tree.size()));
    WireWriter extension_writer(Span<u8>(extension.data(), extension.size()));
    extension_writer.put_u16(static_cast<u16>(mls::ExtensionType::RatchetTree));
    mls::put_vector(extension_writer, view(honest.tree));
    mls::GroupInfo info;
    info.context          = next;
    info.extensions       = view(extension);
    info.confirmation_tag = ConstSpan<u8>(tag, hash);
    Array<u8> signature;
    signature.resize(signature_sizes(suite.signature).signature);
    CRD_FUZZ_REQUIRE(mls::sign_group_info(suite, honest.creator.signer, info,
                                          Span<u8>(signature.data(), signature.size()), written_bytes) ==
                     CryptoError::None);
    signature.resize(written_bytes);
    info.signature                 = view(signature);
    const Array<u8>          bytes = written(info, mls::group_info_bytes, mls::put_group_info);
    const mls::WelcomeMember member{honest.package, {}};
    CRD_FUZZ_REQUIRE(mls::seal_welcome(suite, secrets.get(mls::EpochSecret::Joiner), {}, view(bytes),
                                       ConstSpan<mls::WelcomeMember>(&member, 1U), honest.welcome) ==
                     CryptoError::None);
}

Honest& honest_of(u8 selector)
{
    static Honest     honests[4];
    static bool       sealed[4] = {};
    const usize       at        = selector % 4U;
    if (!sealed[at])
    {
        seal_honest(mls::cipher_suites()[at], honests[at]);
        sealed[at] = true;
    }
    return honests[at];
}

void welcome_changed(Reader& in)
{
    Honest&   honest = honest_of(in.byte());
    const u16 at     = in.u16be();
    const u8  mask   = in.byte();
    const u8  fetch  = in.byte();
    if (!in.ok())
    {
        return;
    }
    Array<u8> welcome = honest.welcome;
    if (at < welcome.size())
    {
        welcome[at] = static_cast<u8>(welcome[at] ^ mask);
    }
    const bool       unchanged = at >= welcome.size() || mask == 0U;
    mls::Group       group;
    const CryptoError result = group.join(view(welcome), honest.package, view(honest.joiner.init_private),
                                          view(honest.joiner.leaf_private),
                                          (fetch & 1U) != 0U ? view(honest.tree) : ConstSpan<u8>(), honest.config);
    if (result != CryptoError::None)
    {
        CRD_FUZZ_REQUIRE(!unchanged);
        CRD_FUZZ_REQUIRE(!group.ready() && !group.tree().ready() && !group.keys().ready());
        CRD_FUZZ_REQUIRE(group.epoch_authenticator().empty() && group.context().group_id.empty());
        return;
    }
    CRD_FUZZ_REQUIRE(unchanged);
    CRD_FUZZ_REQUIRE(same(group.epoch_authenticator(), view(honest.authenticator)));
}
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    if (size == 0U || size > crd::fuzz::kMaxInputBytes)
    {
        return 0;
    }
    Reader in(data + 1U, size - 1U);
    if (data[0] % 2U == 0U)
    {
        decoding(in);
    }
    else
    {
        welcome_changed(in);
    }
    return 0;
}

void crd_fuzz_seeds(crd::fuzz::SeedSink& sink)
{
    // Decoding: a Welcome with one entry, a GroupInfo, GroupSecrets with a path secret and a PSK, a GroupContext,
    // required capabilities, two external senders, extension lists with and without a repeat, and a short input.
    const u8 welcome[] = {0U, 0x00U, 0x01U, 0x09U, 0x02U, 0xAAU, 0xBBU, 0x01U, 0xCCU, 0x03U, 0xDDU, 0xEEU, 0xFFU,
                          0x04U, 0x01U, 0x02U, 0x03U, 0x04U};
    sink.add("decode-welcome", welcome, sizeof(welcome));
    // A GroupContext (mls10, suite 1, an id, epoch 5, a tree hash, no transcript, two extensions), one GroupInfo
    // extension, a tag, signer 1 and a signature.
    const u8 group_info[] = {0U,    0x00U, 0x01U, 0x00U, 0x01U, 0x02U, 0x47U, 0x31U, 0x00U, 0x00U, 0x00U,
                             0x00U, 0x00U, 0x00U, 0x00U, 0x05U, 0x02U, 0x11U, 0x22U, 0x00U, 0x07U, 0x00U,
                             0x03U, 0x01U, 0x00U, 0xFFU, 0x00U, 0x00U, 0x06U, 0x00U, 0x02U, 0x02U, 0x77U,
                             0x88U, 0x01U, 0x99U, 0x00U, 0x00U, 0x00U, 0x01U, 0x01U, 0xABU};
    sink.add("decode-group-info", group_info, sizeof(group_info));
    const u8 secrets[] = {0U,    0x04U, 0x10U, 0x20U, 0x30U, 0x40U, 0x01U, 0x02U, 0x55U, 0x66U,
                          0x09U, 0x01U, 0x02U, 0x6BU, 0x31U, 0x04U, 0x0AU, 0x0BU, 0x0CU, 0x0DU};
    sink.add("decode-group-secrets", secrets, sizeof(secrets));
    const u8 required[] = {0U, 0x02U, 0xFFU, 0x00U, 0x04U, 0x00U, 0x08U, 0x00U, 0x09U, 0x02U, 0x00U, 0x01U};
    sink.add("decode-required", required, sizeof(required));
    const u8 senders[] = {0U,    0x10U, 0x02U, 0xA1U, 0xA2U, 0x00U, 0x01U, 0x02U, 0x73U, 0x31U,
                          0x02U, 0xB1U, 0xB2U, 0x00U, 0x01U, 0x02U, 0x73U, 0x32U};
    sink.add("decode-senders", senders, sizeof(senders));
    const u8 distinct[] = {0U, 0x00U, 0x01U, 0x01U, 0x41U, 0xFFU, 0x00U, 0x00U, 0x0AU, 0x0AU, 0x02U, 0x42U, 0x43U};
    sink.add("decode-extensions", distinct, sizeof(distinct));
    const u8 repeated[] = {0U, 0x00U, 0x05U, 0x01U, 0x41U, 0x00U, 0x03U, 0x00U, 0x00U, 0x05U, 0x00U};
    sink.add("decode-extensions-repeat", repeated, sizeof(repeated));
    const u8 short_input[] = {0U, 0x00U};
    sink.add("decode-short", short_input, sizeof(short_input));

    // A Welcome unchanged (with and without the fetched tree) and changed at offsets across it, in each suite.
    const char* unchanged_names[4] = {"welcome-unchanged-0", "welcome-unchanged-1", "welcome-unchanged-2",
                                      "welcome-unchanged-3"};
    for (u8 suite = 0U; suite < 4U; ++suite)
    {
        const u8 unchanged[] = {1U, suite, 0xFFU, 0xFFU, 0x00U, static_cast<u8>(suite & 1U)};
        sink.add(unchanged_names[suite], unchanged, sizeof(unchanged));
    }
    const u16   offsets[] = {0U, 1U, 3U, 10U, 40U, 80U, 120U, 200U, 400U, 700U, 1000U, 1500U};
    const char* names[12] = {"welcome-at-0",   "welcome-at-1",   "welcome-at-3",   "welcome-at-10",
                             "welcome-at-40",  "welcome-at-80",  "welcome-at-120", "welcome-at-200",
                             "welcome-at-400", "welcome-at-700", "welcome-at-1000", "welcome-at-1500"};
    for (usize i = 0U; i < 12U; ++i)
    {
        const u8 changed[] = {1U, static_cast<u8>(i % 4U), static_cast<u8>(offsets[i] >> 8U),
                              static_cast<u8>(offsets[i]), 0x01U, static_cast<u8>(i & 1U)};
        sink.add(names[i], changed, sizeof(changed));
    }
}
