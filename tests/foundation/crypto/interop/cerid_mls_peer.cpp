// crd-mls-peer -- MLS group members through crd-crypto's groups (RFC 9420), driven over its standard streams by
// tests/foundation/crypto/interop/mls_interop.py against crd-mlspp-peer (MLSpp) (NET.2, decision M4;
// docs/design/network-foundation.md#mls).
//
// The protocol both peers speak: each request is one line of tokens separated by single spaces, byte strings in
// lowercase hex ("-" for none); each reply is one line, "ok" and key=value tokens, or "error" and the refusal's name
// (crypto_error_name's here, MLSpp's exception text there, or a word of the peer's own). A process holds up to 16
// members by name, each in at most one group.
//   member NAME IDENTITY SUITE NOT_BEFORE NOT_AFTER  -> ok key_package=HEX
//       a new member: its basic credential's identity, its suite (a decimal codepoint), fresh signature, leaf and
//       init keys, and its KeyPackage MLSMessage, the leaf living from NOT_BEFORE to NOT_AFTER (decimal seconds)
//   create NAME GROUP_ID                  -> ok epoch=0
//   join NAME WELCOME TREE                -> ok epoch=N      TREE "-": the tree of the GroupInfo's extension
//   psk NAME ID SECRET                    -> ok              an external PSK the member holds
//   propose NAME WIRE add KEY_PACKAGE | update | remove LEAF | psk ID  -> ok message=HEX
//   commit NAME WIRE PATH INLINE [add=KEY_PACKAGE] [remove=LEAF] [psk=ID] ...
//                                         -> ok commit=HEX welcome=HEX group_info=HEX tree=HEX epoch=N
//       the proposals cached this epoch and these by value, a path when PATH is 1 or a proposal requires one, the
//       ratchet tree in the GroupInfo (the Welcome's and the reply's) when INLINE is 1; merged at once (the driver
//       never hands a member its own commit); welcome "-" when nobody is added
//   process NAME MESSAGE                  -> ok type=application data=HEX | ok type=proposal
//                                            | ok type=commit epoch=N removed=0|1
//   protect NAME DATA                     -> ok message=HEX
//   group_info NAME INLINE                -> ok group_info=HEX tree=HEX   with the external key, the tree inline
//                                            when INLINE is 1
//   external_join NAME GROUP_INFO TREE [resync=LEAF] [psk=ID] ...  -> ok commit=HEX epoch=N
//   state NAME  -> ok epoch=N leaf=I authenticator=HEX exporter=HEX tree_hash=HEX roster=I:IDENTITY,...
//       exporter: MLS-Exporter("cerid interop", "context", 32); roster: every non-blank leaf's index and identity
// WIRE is public or private (a PublicMessage or a PrivateMessage). Exit 0 at the end of input, 2 when a line is
// longer than 16 MiB or cut short.
//
// Cerid's members: capabilities listing mls10, their suite and basic credentials, each list with a GREASE value
// (section 13.5), a GREASE leaf extension of the listed type and a GREASE key package extension; the Authentication
// Service accepts every basic credential and a successor of the same identity; the store holds the external PSKs the
// driver gives; every lifetime is judged at the process's clock under the default policy (decision M8).

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/containers/wire_bytes.hpp>
#include <crd/crypto/hpke.hpp>
#include <crd/crypto/mls.hpp>
#include <crd/crypto/mls_group.hpp>
#include <crd/crypto/mls_message.hpp>
#include <crd/crypto/mls_tree.hpp>
#include <crd/crypto/provider.hpp>

#include <cstdio>
#include <cstring>
#include <ctime>
#include <initializer_list>

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
using crd::containers::StringView;
using crd::containers::WireWriter;
using namespace crd::crypto;
namespace mls = crd::crypto::mls;
using Bytes = Array<u8>;

constexpr usize      kMaxMembers      = 16U;
constexpr usize      kMaxTokens       = 64U;
constexpr usize      kMaxPsks         = 8U;
constexpr usize      kMaxLine         = usize{1} << 24U;
constexpr StringView kExportLabel     = "cerid interop";
constexpr u8         kExportContext[] = {'c', 'o', 'n', 't', 'e', 'x', 't'};
constexpr usize      kExportBytes     = 32U;
constexpr u8         kVersions[]      = {0x00U, 0x01U}; // mls10
constexpr u16        kBasic           = static_cast<u16>(mls::CredentialType::Basic);

char  g_line[kMaxLine];
char  g_reply[kMaxLine + 1U]; // a reply and its newline
usize g_reply_size = 0U;
bool  g_overflow   = false;

// A request's outcome: empty when done, otherwise the refusal's name.
using Outcome = StringView;
constexpr Outcome kDone{};

Outcome refused(CryptoError error)
{
    return crypto_error_name(error);
}

ConstSpan<u8> view(const Bytes& bytes)
{
    return ConstSpan<u8>(bytes.data(), bytes.size());
}

Span<u8> span_of(Bytes& bytes)
{
    return Span<u8>(bytes.data(), bytes.size());
}

void say(StringView text)
{
    if (text.size() > kMaxLine - g_reply_size)
    {
        g_overflow = true;
        return;
    }
    std::memcpy(g_reply + g_reply_size, text.data(), text.size());
    g_reply_size += text.size();
}

void say_number(StringView key, u64 value)
{
    char      text[64];
    const int written = std::snprintf(text, sizeof(text), " %.*s=%llu", static_cast<int>(key.size()), key.data(),
                                      static_cast<unsigned long long>(value));
    say(StringView(text, static_cast<usize>(written)));
}

void say_hex_digits(ConstSpan<u8> value)
{
    static constexpr char kDigits[] = "0123456789abcdef";
    for (const u8 byte : value)
    {
        const char pair[2] = {kDigits[byte >> 4U], kDigits[byte & 0x0FU]};
        say(StringView(pair, 2U));
    }
}

void say_hex(StringView key, ConstSpan<u8> value)
{
    say(" ");
    say(key);
    say("=");
    if (value.empty())
    {
        say("-");
        return;
    }
    say_hex_digits(value);
}

int nibble(char c)
{
    if (c >= '0' && c <= '9')
    {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f')
    {
        return c - 'a' + 10;
    }
    return -1;
}

// A token's bytes ("-" for none); false for anything but lowercase hex pairs.
bool hex_bytes(StringView token, Bytes& out)
{
    out.clear();
    if (token == "-")
    {
        return true;
    }
    if (token.size() % 2U != 0U)
    {
        return false;
    }
    out.resize(token.size() / 2U);
    for (usize i = 0U; i < out.size(); ++i)
    {
        const int high = nibble(token[2U * i]);
        const int low  = nibble(token[2U * i + 1U]);
        if (high < 0 || low < 0)
        {
            return false;
        }
        out[i] = static_cast<u8>((high << 4) | low);
    }
    return true;
}

bool number(StringView token, u64 limit, u64& out)
{
    if (token.empty())
    {
        return false;
    }
    out = 0U;
    for (const char c : token)
    {
        const u64 digit = static_cast<u64>(c - '0');
        if (c < '0' || c > '9' || out > (~u64{0} - digit) / 10U)
        {
            return false;
        }
        out = out * 10U + digit;
    }
    return out <= limit;
}

bool wire_format(StringView token, mls::WireFormat& out)
{
    if (token == "public")
    {
        out = mls::WireFormat::PublicMessage;
        return true;
    }
    if (token == "private")
    {
        out = mls::WireFormat::PrivateMessage;
        return true;
    }
    return false;
}

// Decodes a hex token as an MLSMessage of `format`; its fields view `holder`.
bool message_of(StringView token, mls::WireFormat format, Bytes& holder, mls::Message& out)
{
    return hex_bytes(token, holder) && mls::take_message(view(holder), out) && out.wire_format == format;
}

// A tree token: the ratchet_tree extension's encoding, or none.
bool tree_of(StringView token, Bytes& out)
{
    return hex_bytes(token, out);
}

void say_tree(const mls::RatchetTree& tree)
{
    Bytes encoded;
    encoded.resize(tree.encoded_bytes());
    WireWriter writer(span_of(encoded));
    tree.encode(writer);
    say_hex("tree", view(encoded));
}

// The Authentication Service of the interop: every basic credential with an identity, and a successor that presents
// the same identity (one client).
class PeerAuthority final : public mls::AuthenticationService
{
public:
    [[nodiscard]] bool validate(const mls::Credential& credential, ConstSpan<u8>) const noexcept override
    {
        return credential.type == kBasic && !credential.content.empty();
    }

    [[nodiscard]] bool valid_successor(const mls::Credential& predecessor,
                                       const mls::Credential& successor) const noexcept override
    {
        return predecessor.type == successor.type && predecessor.content.size() == successor.content.size() &&
               std::memcmp(predecessor.content.data(), successor.content.data(), successor.content.size()) == 0;
    }
};

// A member's store: the external PSKs the driver gave it, and its one group.
class PeerStore final : public mls::GroupStore
{
public:
    struct Key
    {
        Bytes id;
        Bytes secret;
    };
    Key              keys[kMaxPsks];
    usize            count = 0U;
    const mls::Group* group = nullptr;

    [[nodiscard]] ConstSpan<u8> psk(const mls::PreSharedKeyId& id) const noexcept override
    {
        if (id.type != mls::PskType::External)
        {
            return {}; // a group keeps its own resumption PSKs
        }
        for (usize i = 0U; i < count; ++i)
        {
            if (keys[i].id.size() == id.id.size() &&
                std::memcmp(keys[i].id.data(), id.id.data(), id.id.size()) == 0)
            {
                return view(keys[i].secret);
            }
        }
        return {};
    }

    [[nodiscard]] bool member_of(ConstSpan<u8> group_id) const noexcept override
    {
        if (group == nullptr || !group->ready())
        {
            return false;
        }
        const ConstSpan<u8> own = group->context().group_id;
        return own.size() == group_id.size() && std::memcmp(own.data(), group_id.data(), own.size()) == 0;
    }
};

// One member: its keys, its leaf and key package (whose fields view its own bytes) and its group.
struct Member
{
    bool                    used = false;
    Bytes                   name;
    const mls::CipherSuite* suite = nullptr;
    SigningKey              signer;
    Bytes                   signature_key;
    Bytes                   identity;
    Bytes                   leaf_private;
    Bytes                   leaf_public;
    Bytes                   init_private;
    Bytes                   init_public;
    Bytes                   suites;
    Bytes                   extension_types;
    Bytes                   credentials;
    Bytes                   leaf_extensions;
    Bytes                   package_extensions;
    Bytes                   leaf_signature;
    Bytes                   package_signature;
    Bytes                   key_package; // its MLSMessage
    mls::LeafNode           leaf;
    mls::KeyPackage         package;
    PeerStore               store;
    mls::GroupConfig        config;
    mls::Group              group;
};

const ProviderVerifier kVerifier;
const PeerAuthority    kAuthority;
Member                 g_members[kMaxMembers];

Member* find_member(StringView name)
{
    for (Member& member : g_members)
    {
        if (member.used && member.name.size() == name.size() &&
            std::memcmp(member.name.data(), name.data(), name.size()) == 0)
        {
            return &member;
        }
    }
    return nullptr;
}

// A fresh HPKE key pair of `kem`: a random private key in SerializePrivateKey form, drawn again in the rare case it is
// out of range.
CryptoError fresh_key(AlgorithmId kem, Bytes& private_key, Bytes& public_key)
{
    const HpkeSizes sizes = hpke_sizes(kem);
    private_key.resize(sizes.private_key);
    public_key.resize(sizes.public_key);
    HpkeKey key;
    for (u32 attempt = 0U; attempt < 64U; ++attempt)
    {
        if (const CryptoError error = random_bytes(span_of(private_key)); error != CryptoError::None)
        {
            return error;
        }
        if (key.from_private(kem, view(private_key)) == CryptoError::None)
        {
            return key.public_key(span_of(public_key));
        }
    }
    return CryptoError::ProviderFailure;
}

// A list of 16-bit codepoints: `value` (none for 0) and a GREASE value drawn for it.
CryptoError listing(u16 value, Bytes& out, u16& grease)
{
    if (const CryptoError error = mls::random_grease(grease); error != CryptoError::None)
    {
        return error;
    }
    out.clear();
    if (value != 0U)
    {
        out.push_back(static_cast<u8>(value >> 8U));
        out.push_back(static_cast<u8>(value));
    }
    out.push_back(static_cast<u8>(grease >> 8U));
    out.push_back(static_cast<u8>(grease));
    return CryptoError::None;
}

CryptoError sign_into(const Member& member, bool package, Bytes& out)
{
    out.resize(signature_sizes(member.suite->signature).signature);
    usize             written = 0U;
    const CryptoError error   = package ? mls::sign_key_package(*member.suite, member.signer, member.package,
                                                                span_of(out), written)
                                        : mls::sign_leaf_node(*member.suite, member.signer, member.leaf, {}, 0U,
                                                              span_of(out), written);
    out.resize(written);
    return error;
}

// member NAME IDENTITY SUITE NOT_BEFORE NOT_AFTER
Outcome make_member(const StringView* tokens)
{
    u64 suite_id   = 0U;
    u64 not_before = 0U;
    u64 not_after  = 0U;
    if (!number(tokens[3], 0xFFFFU, suite_id) || !number(tokens[4], ~u64{0}, not_before) ||
        !number(tokens[5], ~u64{0}, not_after))
    {
        return "usage";
    }
    if (find_member(tokens[1]) != nullptr)
    {
        return "exists";
    }
    Member* member = nullptr;
    for (Member& slot : g_members)
    {
        if (!slot.used)
        {
            member = &slot;
            break;
        }
    }
    if (member == nullptr)
    {
        return "full";
    }
    const mls::CipherSuite* suite = mls::cipher_suite(static_cast<u16>(suite_id));
    if (suite == nullptr)
    {
        return refused(CryptoError::Unsupported);
    }
    Member& m = *member;
    m.suite   = suite;
    if (!hex_bytes(tokens[2], m.identity) || m.identity.empty())
    {
        return "usage";
    }
    m.name.clear();
    for (const char c : tokens[1])
    {
        m.name.push_back(static_cast<u8>(c));
    }
    u16 suite_grease     = 0U;
    u16 extension_grease = 0U;
    u16 credential_grease = 0U;
    u16 package_grease   = 0U;
    m.signature_key.resize(signature_sizes(suite->signature).public_key);
    CryptoError error = m.signer.generate(suite->signature);
    error = error == CryptoError::None ? m.signer.public_key(span_of(m.signature_key)) : error;
    error = error == CryptoError::None ? fresh_key(suite->kem, m.leaf_private, m.leaf_public) : error;
    error = error == CryptoError::None ? fresh_key(suite->kem, m.init_private, m.init_public) : error;
    error = error == CryptoError::None ? listing(suite->id, m.suites, suite_grease) : error;
    error = error == CryptoError::None ? listing(0U, m.extension_types, extension_grease) : error;
    error = error == CryptoError::None ? listing(kBasic, m.credentials, credential_grease) : error;
    error = error == CryptoError::None ? mls::random_grease(package_grease) : error;
    if (error != CryptoError::None)
    {
        return refused(error);
    }
    // A GREASE leaf extension of the listed GREASE type holding one byte, and an empty GREASE key package extension.
    m.leaf_extensions.clear();
    for (const u8 byte : {static_cast<u8>(extension_grease >> 8U), static_cast<u8>(extension_grease), u8{0x01U},
                          u8{0x5AU}})
    {
        m.leaf_extensions.push_back(byte);
    }
    m.package_extensions.clear();
    for (const u8 byte : {static_cast<u8>(package_grease >> 8U), static_cast<u8>(package_grease), u8{0x00U}})
    {
        m.package_extensions.push_back(byte);
    }

    m.leaf                            = {};
    m.leaf.encryption_key             = view(m.leaf_public);
    m.leaf.signature_key              = view(m.signature_key);
    m.leaf.credential.type            = kBasic;
    m.leaf.credential.content         = view(m.identity);
    m.leaf.capabilities.versions      = ConstSpan<u8>(kVersions, sizeof(kVersions));
    m.leaf.capabilities.cipher_suites = view(m.suites);
    m.leaf.capabilities.extensions    = view(m.extension_types);
    m.leaf.capabilities.credentials   = view(m.credentials);
    m.leaf.source                     = mls::LeafNodeSource::KeyPackage;
    m.leaf.not_before                 = not_before;
    m.leaf.not_after                  = not_after;
    m.leaf.extensions                 = view(m.leaf_extensions);
    if (const CryptoError signed_leaf = sign_into(m, false, m.leaf_signature); signed_leaf != CryptoError::None)
    {
        return refused(signed_leaf);
    }
    m.leaf.signature = view(m.leaf_signature);

    m.package              = {};
    m.package.cipher_suite = suite->id;
    m.package.init_key     = view(m.init_public);
    m.package.leaf_node    = m.leaf;
    m.package.extensions   = view(m.package_extensions);
    if (const CryptoError signed_package = sign_into(m, true, m.package_signature);
        signed_package != CryptoError::None)
    {
        return refused(signed_package);
    }
    m.package.signature = view(m.package_signature);

    mls::Message message;
    message.wire_format = mls::WireFormat::KeyPackage;
    message.key_package = m.package;
    m.key_package.resize(mls::message_bytes(message));
    WireWriter writer(span_of(m.key_package));
    mls::put_message(writer, message);

    m.store.count          = 0U;
    m.store.group          = &m.group;
    m.config               = {};
    m.config.verifier       = &kVerifier;
    m.config.authentication = &kAuthority;
    m.config.store          = &m.store;
    m.config.policy.now     = static_cast<u64>(std::time(nullptr));
    m.used                  = true;
    say("ok");
    say_hex("key_package", view(m.key_package));
    return kDone;
}

// create NAME GROUP_ID
Outcome create(Member& member, const StringView* tokens)
{
    Bytes group_id;
    if (!hex_bytes(tokens[2], group_id))
    {
        return "usage";
    }
    if (const CryptoError error = member.group.create(*member.suite, view(group_id), {}, member.leaf,
                                                      view(member.leaf_private), member.config);
        error != CryptoError::None)
    {
        return refused(error);
    }
    say("ok");
    say_number("epoch", member.group.epoch());
    return kDone;
}

// join NAME WELCOME TREE
Outcome join(Member& member, const StringView* tokens)
{
    Bytes        welcome_bytes;
    Bytes        tree;
    mls::Message welcome;
    if (!message_of(tokens[2], mls::WireFormat::Welcome, welcome_bytes, welcome) || !tree_of(tokens[3], tree))
    {
        return "usage";
    }
    if (const CryptoError error = member.group.join(welcome.body, member.package, view(member.init_private),
                                                    view(member.leaf_private), view(tree), member.config);
        error != CryptoError::None)
    {
        return refused(error);
    }
    say("ok");
    say_number("epoch", member.group.epoch());
    return kDone;
}

// psk NAME ID SECRET
Outcome hold_psk(Member& member, const StringView* tokens)
{
    if (member.store.count == kMaxPsks)
    {
        return "full";
    }
    PeerStore::Key& key = member.store.keys[member.store.count];
    if (!hex_bytes(tokens[2], key.id) || !hex_bytes(tokens[3], key.secret) || key.id.empty() || key.secret.empty())
    {
        return "usage";
    }
    ++member.store.count;
    say("ok");
    return kDone;
}

// An external PSK's identity with a fresh nonce in `nonce`.
CryptoError external_psk(const Member& member, const Bytes& id, Bytes& nonce, mls::PreSharedKeyId& out)
{
    nonce.resize(member.suite->hash_bytes());
    out       = {};
    out.type  = mls::PskType::External;
    out.id    = view(id);
    out.nonce = view(nonce);
    return random_bytes(span_of(nonce));
}

// One proposal by value from a commit's or a proposal's token: "add" and a key package, "remove" and a leaf, "psk"
// and an id; its fields view `holder` and `nonce`.
Outcome proposal_of(const Member& member, StringView kind, StringView argument, Bytes& holder, Bytes& nonce,
                    mls::Proposal& out)
{
    out = {};
    if (kind == "add")
    {
        mls::Message package;
        if (!message_of(argument, mls::WireFormat::KeyPackage, holder, package))
        {
            return "usage";
        }
        out.type = static_cast<u16>(mls::ProposalType::Add);
        out.add  = package.key_package;
        return kDone;
    }
    if (kind == "remove")
    {
        u64 leaf = 0U;
        if (!number(argument, 0xFFFFFFFFU, leaf))
        {
            return "usage";
        }
        out.type   = static_cast<u16>(mls::ProposalType::Remove);
        out.remove = static_cast<u32>(leaf);
        return kDone;
    }
    if (kind == "psk")
    {
        if (!hex_bytes(argument, holder))
        {
            return "usage";
        }
        out.type = static_cast<u16>(mls::ProposalType::PreSharedKey);
        if (const CryptoError error = external_psk(member, holder, nonce, out.psk); error != CryptoError::None)
        {
            return refused(error);
        }
        return kDone;
    }
    return "usage";
}

// propose NAME WIRE add KEY_PACKAGE | update | remove LEAF | psk ID
Outcome propose(Member& member, const StringView* tokens, usize count)
{
    mls::SendOptions send;
    if (!wire_format(tokens[2], send.wire_format))
    {
        return "usage";
    }
    u8                   ref[mls::kMaxHashBytes] = {};
    const Span<u8>       ref_span(ref, member.suite->hash_bytes());
    Array<u8>            out;
    CryptoError          error = CryptoError::None;
    if (tokens[3] == "update" && count == 4U)
    {
        error = member.group.propose_update(member.signer, send, out, ref_span);
    }
    else if (count == 5U)
    {
        Bytes         holder;
        Bytes         nonce;
        mls::Proposal proposal;
        if (const Outcome outcome = proposal_of(member, tokens[3], tokens[4], holder, nonce, proposal);
            !outcome.empty())
        {
            return outcome;
        }
        error = member.group.propose(member.signer, proposal, send, out, ref_span);
    }
    else
    {
        return "usage";
    }
    if (error != CryptoError::None)
    {
        return refused(error);
    }
    say("ok");
    say_hex("message", view(out));
    return kDone;
}

// commit NAME WIRE PATH INLINE [add=KEY_PACKAGE] [remove=LEAF] [psk=ID] ...
Outcome commit(Member& member, const StringView* tokens, usize count)
{
    mls::CommitOptions options;
    if (!wire_format(tokens[2], options.send.wire_format) || (tokens[3] != "0" && tokens[3] != "1") ||
        (tokens[4] != "0" && tokens[4] != "1"))
    {
        return "usage";
    }
    options.path                    = tokens[3] == "1";
    options.group_info.ratchet_tree = tokens[4] == "1";
    Bytes         holders[kMaxTokens];
    Bytes         nonces[kMaxTokens];
    mls::Proposal proposals[kMaxTokens];
    usize         by_value = 0U;
    for (usize i = 5U; i < count; ++i)
    {
        const usize equals = tokens[i].find('=');
        if (equals == StringView::npos)
        {
            return "usage";
        }
        if (const Outcome outcome = proposal_of(member, tokens[i].substr(0U, equals), tokens[i].substr(equals + 1U),
                                                holders[by_value], nonces[by_value], proposals[by_value]);
            !outcome.empty())
        {
            return outcome;
        }
        ++by_value;
    }
    options.proposals = ConstSpan<mls::Proposal>(proposals, by_value);
    mls::CommitResult result;
    if (const CryptoError error = member.group.commit(member.signer, options, result); error != CryptoError::None)
    {
        return refused(error);
    }
    if (const CryptoError error = member.group.merge(result.staged); error != CryptoError::None)
    {
        return refused(error);
    }
    say("ok");
    say_hex("commit", view(result.message));
    say_hex("welcome", view(result.welcome));
    say_hex("group_info", view(result.group_info));
    say_tree(member.group.tree());
    say_number("epoch", member.group.epoch());
    return kDone;
}

// process NAME MESSAGE
Outcome process(Member& member, const StringView* tokens)
{
    Bytes message;
    if (!hex_bytes(tokens[2], message))
    {
        return "usage";
    }
    mls::ReceivedMessage received;
    if (const CryptoError error = member.group.process(view(message), received); error != CryptoError::None)
    {
        return refused(error);
    }
    switch (received.content_type())
    {
    case mls::ContentType::Application:
        say("ok type=application");
        say_hex("data", received.application_data());
        return kDone;
    case mls::ContentType::Proposal:
        say("ok type=proposal");
        return kDone;
    case mls::ContentType::Commit:
        break;
    }
    mls::StagedCommit& staged  = received.commit();
    const u64          epoch   = staged.epoch();
    const bool         removed = staged.removes_self();
    if (const CryptoError error = member.group.merge(staged); error != CryptoError::None)
    {
        return refused(error);
    }
    say("ok type=commit");
    say_number("epoch", epoch);
    say(removed ? " removed=1" : " removed=0");
    return kDone;
}

// protect NAME DATA
Outcome protect(Member& member, const StringView* tokens)
{
    Bytes data;
    if (!hex_bytes(tokens[2], data))
    {
        return "usage";
    }
    Array<u8> out;
    if (const CryptoError error = member.group.protect(member.signer, view(data), {}, out); error != CryptoError::None)
    {
        return refused(error);
    }
    say("ok");
    say_hex("message", view(out));
    return kDone;
}

// group_info NAME INLINE
Outcome group_info(Member& member, const StringView* tokens)
{
    if (tokens[2] != "0" && tokens[2] != "1")
    {
        return "usage";
    }
    mls::GroupInfoOptions options;
    options.external_pub = true;
    options.ratchet_tree = tokens[2] == "1";
    Array<u8> out;
    if (const CryptoError error = member.group.group_info(member.signer, options, out); error != CryptoError::None)
    {
        return refused(error);
    }
    say("ok");
    say_hex("group_info", view(out));
    say_tree(member.group.tree());
    return kDone;
}

// external_join NAME GROUP_INFO TREE [resync=LEAF] [psk=ID] ...
Outcome external_join(Member& member, const StringView* tokens, usize count)
{
    Bytes info;
    Bytes tree;
    if (!hex_bytes(tokens[2], info) || !tree_of(tokens[3], tree))
    {
        return "usage";
    }
    mls::ExternalJoinOptions options;
    options.group_info.external_pub = true; // the next joiner may join from the joiner's GroupInfo
    Bytes               ids[kMaxTokens];
    Bytes               nonces[kMaxTokens];
    mls::PreSharedKeyId psks[kMaxTokens];
    usize               psk_count = 0U;
    for (usize i = 4U; i < count; ++i)
    {
        const StringView token = tokens[i];
        u64              leaf  = 0U;
        if (token.starts_with("resync=") && number(token.substr(7U), 0xFFFFFFFFU, leaf))
        {
            options.resync = static_cast<u32>(leaf);
        }
        else if (token.starts_with("psk=") && hex_bytes(token.substr(4U), ids[psk_count]))
        {
            if (const CryptoError error = external_psk(member, ids[psk_count], nonces[psk_count], psks[psk_count]);
                error != CryptoError::None)
            {
                return refused(error);
            }
            ++psk_count;
        }
        else
        {
            return "usage";
        }
    }
    options.psks = ConstSpan<mls::PreSharedKeyId>(psks, psk_count);
    mls::ExternalJoinResult result;
    if (const CryptoError error = member.group.join_external(member.signer, member.leaf, view(info), view(tree),
                                                             options, member.config, result);
        error != CryptoError::None)
    {
        return refused(error);
    }
    say("ok");
    say_hex("commit", view(result.message));
    say_number("epoch", member.group.epoch());
    return kDone;
}

// state NAME
Outcome report(const Member& member)
{
    u8 exported[kExportBytes] = {};
    if (const CryptoError error =
            mls::export_secret(*member.suite, member.group.secret(mls::EpochSecret::Exporter), kExportLabel,
                               ConstSpan<u8>(kExportContext, sizeof(kExportContext)),
                               Span<u8>(exported, sizeof(exported)));
        error != CryptoError::None)
    {
        return refused(error);
    }
    say("ok");
    say_number("epoch", member.group.epoch());
    say_number("leaf", member.group.own_leaf());
    say_hex("authenticator", member.group.epoch_authenticator());
    say_hex("exporter", ConstSpan<u8>(exported, sizeof(exported)));
    say_hex("tree_hash", member.group.tree().root_hash());
    say(" roster=");
    bool first = true;
    for (u32 i = 0U; i < member.group.tree().leaves(); ++i)
    {
        mls::LeafNode leaf;
        if (!member.group.tree().leaf_node(i, leaf))
        {
            continue;
        }
        char      index[16];
        const int written = std::snprintf(index, sizeof(index), "%s%u:", first ? "" : ",", i);
        say(StringView(index, static_cast<usize>(written)));
        first = false;
        say_hex_digits(leaf.credential.content);
    }
    return kDone;
}

Outcome dispatch(const StringView* tokens, usize count)
{
    const StringView op = tokens[0];
    if (op == "member")
    {
        return count == 6U ? make_member(tokens) : "usage";
    }
    if (count < 2U)
    {
        return "usage";
    }
    Member* member = find_member(tokens[1]);
    if (member == nullptr)
    {
        return "no-member";
    }
    if (op == "psk")
    {
        return count == 4U ? hold_psk(*member, tokens) : "usage";
    }
    if (op == "create")
    {
        return count == 3U ? create(*member, tokens) : "usage";
    }
    if (op == "join")
    {
        return count == 4U ? join(*member, tokens) : "usage";
    }
    if (op == "external_join")
    {
        return count >= 4U ? external_join(*member, tokens, count) : "usage";
    }
    if (!member->group.ready())
    {
        return "no-group";
    }
    if (op == "propose")
    {
        return count >= 4U ? propose(*member, tokens, count) : "usage";
    }
    if (op == "commit")
    {
        return count >= 5U ? commit(*member, tokens, count) : "usage";
    }
    if (op == "process")
    {
        return count == 3U ? process(*member, tokens) : "usage";
    }
    if (op == "protect")
    {
        return count == 3U ? protect(*member, tokens) : "usage";
    }
    if (op == "group_info")
    {
        return count == 3U ? group_info(*member, tokens) : "usage";
    }
    if (op == "state")
    {
        return count == 2U ? report(*member) : "usage";
    }
    return "usage";
}
} // namespace

int main()
{
    while (std::fgets(g_line, sizeof(g_line), stdin) != nullptr)
    {
        usize length = std::strlen(g_line);
        if (length == 0U || g_line[length - 1U] != '\n')
        {
            return 2; // a line longer than the buffer, or input cut short
        }
        g_line[--length] = '\0';
        const StringView line(g_line, length);
        StringView       tokens[kMaxTokens];
        usize            count = 0U;
        usize            start = 0U;
        while (start < line.size() && count < kMaxTokens)
        {
            const usize end = line.find(' ', start);
            tokens[count++] = end == StringView::npos ? line.substr(start) : line.substr(start, end - start);
            start           = end == StringView::npos ? line.size() : end + 1U;
        }
        g_reply_size = 0U;
        g_overflow   = false;
        Outcome outcome = count == 0U || start < line.size() ? Outcome("usage") : dispatch(tokens, count);
        if (outcome.empty() && g_overflow)
        {
            outcome = "reply-too-long";
        }
        if (!outcome.empty())
        {
            g_reply_size = 0U;
            say("error ");
            say(outcome);
        }
        g_reply[g_reply_size] = '\n';
        if (std::fwrite(g_reply, 1U, g_reply_size + 1U, stdout) != g_reply_size + 1U || std::fflush(stdout) != 0)
        {
            return 1;
        }
    }
    return 0;
}
