// crd-mlspp-peer -- MLS group members through Cisco's MLSpp (the pinned commit, cmake/pins.json mlspp), the
// independent peer of crd-mls-peer in tests/foundation/crypto/interop/mls_interop.py (NET.2, decision M4;
// docs/design/network-foundation.md#mls). It speaks crd-mls-peer's protocol (its header states it) through MLSpp's
// public interface only, and links MLSpp and the pinned OpenSSL alone: the two implementations meet on the wire.
//
// MLSpp specifics the protocol leaves to each peer:
//   key packages  MLSpp's default capabilities (every version, suite and credential type it implements; it adds
//                 GREASE values to each list and a GREASE extension to every leaf and key package itself, RFC 9420
//                 section 13.5) and the lifetime the request names.
//   wire formats  MessageOpts::encrypt chooses a PrivateMessage for proposals and commits; application data is always
//                 one.
//   proposals     MLSpp caches a proposal when it handles its message, so a member handles its own at once, as
//                 the delivery service would return it.
//   removal       MLSpp's State has no outcome for a commit that removes the member (it reads the member's own leaf
//                 in the next tree) and refuses to cache a Remove proposal of the member itself (State::valid), so
//                 the peer unwraps such messages through MLSpp (their membership tag or seals and their signature
//                 verified), notes each Remove proposal's reference itself, and reads a commit's Removes, by value and
//                 by reference; a removed member's state is dropped.
//   external PSKs MLSpp's external join injects every PSK the joiner holds, so its psk= tokens name exactly those.
//   key packages  MLSpp's KeyPackage adds GREASE extensions after its leaf is signed, of types the leaf's capabilities
//                 need not list, though RFC 9420 section 10 requires them listed (TP-11, which Cerid and OpenMLS
//                 refuse): the peer gives each such extension a GREASE type the capabilities list and none of the
//                 package's other extensions has, and signs the package again, through MLSpp's public fields and sign;
//                 `member` replies with the package as MLSpp made it too (as_made=HEX), which TP-11's case refuses.
// An exception MLSpp throws is the request's refusal: "error refused:" and its message, spaces replaced.

#include <mls/state.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <initializer_list>
#include <optional>
#include <string_view>
#include <utility>

namespace
{
using mls::bytes_ns::bytes;
namespace variant = mls::tls::var;
using std::size_t;
using std::uint32_t;
using std::uint64_t;
using std::uint8_t;

constexpr size_t kMaxMembers      = 16U;
constexpr size_t kMaxTokens       = 64U;
constexpr size_t kMaxRemovals     = 64U;
constexpr size_t kMaxLine         = size_t{1} << 24U;
constexpr char   kExportLabel[]   = "cerid interop";
constexpr char   kExportContext[] = "context";
constexpr size_t kExportBytes     = 32U;

char   g_line[kMaxLine];
char   g_reply[kMaxLine + 1U]; // a reply and its newline
size_t g_reply_size = 0U;

// A failed request: its reply word.
struct Refusal
{
    const char* word;
};

bool g_overflow = false; // a reply longer than its buffer, refused once the request ends

void say(std::string_view text) noexcept
{
    if (text.size() > kMaxLine - g_reply_size)
    {
        g_overflow = true;
        return;
    }
    std::memcpy(g_reply + g_reply_size, text.data(), text.size());
    g_reply_size += text.size();
}

void say_number(const char* key, uint64_t value)
{
    char text[64];
    const int written = std::snprintf(text, sizeof(text), " %s=%llu", key, static_cast<unsigned long long>(value));
    say(std::string_view(text, static_cast<size_t>(written)));
}

void say_hex_digits(const bytes& value)
{
    static constexpr char kDigits[] = "0123456789abcdef";
    for (const uint8_t byte : value)
    {
        const char pair[2] = {kDigits[byte >> 4U], kDigits[byte & 0x0FU]};
        say(std::string_view(pair, 2U));
    }
}

void say_hex(const char* key, const bytes& value)
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

struct Removal
{
    bytes    ref;
    uint32_t leaf = 0U;
};

struct Member
{
    bytes                                   name;
    mls::CipherSuite                        suite;
    std::optional<mls::HPKEPrivateKey>      init_priv;
    std::optional<mls::HPKEPrivateKey>      leaf_priv;
    std::optional<mls::SignaturePrivateKey> sig_priv;
    std::optional<mls::KeyPackage>          key_package;
    std::optional<mls::State>               state;
    std::map<bytes, bytes> psks; // crd-lint-allow-std-container: MLSpp's interface takes the held PSKs as a map
    Removal removals[kMaxRemovals];
    size_t  removal_count = 0U;
};

std::optional<Member> g_members[kMaxMembers];

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

bytes hex_bytes(std::string_view token)
{
    bytes out;
    if (token == "-")
    {
        return out;
    }
    if (token.size() % 2U != 0U)
    {
        throw Refusal{"usage"};
    }
    out.reserve(token.size() / 2U);
    for (size_t i = 0U; i < token.size(); i += 2U)
    {
        const int high = nibble(token[i]);
        const int low  = nibble(token[i + 1U]);
        if (high < 0 || low < 0)
        {
            throw Refusal{"usage"};
        }
        out.push_back(static_cast<uint8_t>((high << 4) | low));
    }
    return out;
}

uint64_t number(std::string_view token)
{
    if (token.empty())
    {
        throw Refusal{"usage"};
    }
    uint64_t value = 0U;
    for (const char c : token)
    {
        const uint64_t digit = static_cast<uint64_t>(c - '0');
        if (c < '0' || c > '9' || value > (~uint64_t{0} - digit) / 10U)
        {
            throw Refusal{"usage"};
        }
        value = value * 10U + digit;
    }
    return value;
}

mls::LeafIndex leaf_index(std::string_view token)
{
    const uint64_t value = number(token);
    if (value > 0xFFFFFFFFU)
    {
        throw Refusal{"usage"};
    }
    return mls::LeafIndex{static_cast<uint32_t>(value)};
}

template <typename T>
bytes marshal_message(T value)
{
    return mls::tls::marshal(mls::MLSMessage{std::move(value)});
}

template <typename T>
T message_body(std::string_view token)
{
    const mls::MLSMessage message = mls::tls::get<mls::MLSMessage>(hex_bytes(token));
    if (!variant::holds_alternative<T>(message.message))
    {
        throw Refusal{"wire-format"};
    }
    return variant::get<T>(message.message);
}

std::optional<mls::TreeKEMPublicKey> tree_of(std::string_view token)
{
    if (token == "-")
    {
        return std::nullopt;
    }
    return mls::tls::get<mls::TreeKEMPublicKey>(hex_bytes(token));
}

bool encrypted(std::string_view wire)
{
    if (wire == "public")
    {
        return false;
    }
    if (wire == "private")
    {
        return true;
    }
    throw Refusal{"usage"};
}

std::optional<Member>* slot_of(std::string_view name)
{
    for (std::optional<Member>& slot : g_members)
    {
        if (slot && slot->name.size() == name.size() && std::memcmp(slot->name.data(), name.data(), name.size()) == 0)
        {
            return &slot;
        }
    }
    return nullptr;
}

Member& member_of(std::string_view name)
{
    std::optional<Member>* slot = slot_of(name);
    if (slot == nullptr)
    {
        throw Refusal{"no-member"};
    }
    return **slot;
}

mls::State& state_of(Member& member)
{
    if (!member.state)
    {
        throw Refusal{"no-group"};
    }
    return *member.state;
}

void adopt(Member& member, mls::State next)
{
    member.state.emplace(std::move(next));
    member.removal_count = 0U;
}

bool is_grease(uint16_t value)
{
    return (value & 0x0F0FU) == 0x0A0AU && value != 0xFAFAU;
}

bool has_extension(const mls::ExtensionList& list, uint16_t type)
{
    return std::ranges::any_of(list.extensions,
                               [type](const mls::Extension& extension) { return extension.type == type; });
}

bool lists(const mls::Capabilities& capabilities, uint16_t type)
{
    return std::ranges::any_of(capabilities.extensions, [type](mls::Extension::Type listed) { return listed == type; });
}

// TP-11's adaptation: each GREASE package extension of a type the leaf's capabilities do not list takes a listed GREASE
// type no other package extension has, and the package is signed again.
void list_package_extensions(mls::KeyPackage& package, const mls::SignaturePrivateKey& sig_priv)
{
    const mls::Capabilities& capabilities = package.leaf_node.capabilities;
    for (mls::Extension& extension : package.extensions.extensions)
    {
        if (!is_grease(extension.type) || lists(capabilities, extension.type))
        {
            continue;
        }
        bool retyped = false;
        for (const mls::Extension::Type listed : capabilities.extensions)
        {
            if (is_grease(listed) && !has_extension(package.extensions, listed))
            {
                extension.type = listed;
                retyped        = true;
                break;
            }
        }
        if (!retyped)
        {
            throw Refusal{"tp-11"};
        }
    }
    package.sign(sig_priv);
}

// member NAME IDENTITY SUITE NOT_BEFORE NOT_AFTER
void make_member(const std::string_view* tokens)
{
    if (slot_of(tokens[1]) != nullptr)
    {
        throw Refusal{"exists"};
    }
    std::optional<Member>* free = nullptr;
    for (std::optional<Member>& slot : g_members)
    {
        if (!slot)
        {
            free = &slot;
            break;
        }
    }
    if (free == nullptr)
    {
        throw Refusal{"full"};
    }
    Member member;
    member.name = bytes(tokens[1].size());
    std::memcpy(member.name.data(), tokens[1].data(), tokens[1].size());
    const uint64_t suite = number(tokens[3]);
    if (suite > 0xFFFFU)
    {
        throw Refusal{"usage"};
    }
    member.suite = mls::CipherSuite(static_cast<mls::CipherSuite::ID>(suite));
    member.init_priv.emplace(mls::HPKEPrivateKey::generate(member.suite));
    member.leaf_priv.emplace(mls::HPKEPrivateKey::generate(member.suite));
    member.sig_priv.emplace(mls::SignaturePrivateKey::generate(member.suite));
    const mls::LeafNode leaf(member.suite, member.leaf_priv->public_key, member.sig_priv->public_key,
                             mls::Credential::basic(hex_bytes(tokens[2])), mls::Capabilities::create_default(),
                             mls::Lifetime{number(tokens[4]), number(tokens[5])}, {}, *member.sig_priv);
    member.key_package.emplace(member.suite, member.init_priv->public_key, leaf, mls::ExtensionList{},
                               *member.sig_priv);
    const bytes as_made = marshal_message(*member.key_package);
    list_package_extensions(*member.key_package, *member.sig_priv);
    say("ok");
    say_hex("key_package", marshal_message(*member.key_package));
    say_hex("as_made", as_made);
    free->emplace(std::move(member));
}

// create NAME GROUP_ID
void create(const std::string_view* tokens)
{
    Member& member = member_of(tokens[1]);
    adopt(member, mls::State(hex_bytes(tokens[2]), member.suite, *member.leaf_priv, *member.sig_priv,
                             member.key_package->leaf_node, {}));
    for (const auto& [id, secret] : member.psks)
    {
        member.state->add_external_psk(id, secret);
    }
    say("ok");
    say_number("epoch", member.state->epoch());
}

// join NAME WELCOME TREE
void join(const std::string_view* tokens)
{
    Member& member = member_of(tokens[1]);
    adopt(member, mls::State(*member.init_priv, *member.leaf_priv, *member.sig_priv, *member.key_package,
                             message_body<mls::Welcome>(tokens[2]), tree_of(tokens[3]), member.psks));
    say("ok");
    say_number("epoch", member.state->epoch());
}

// psk NAME ID SECRET
void hold_psk(const std::string_view* tokens)
{
    Member&     member = member_of(tokens[1]);
    const bytes id     = hex_bytes(tokens[2]);
    const bytes secret = hex_bytes(tokens[3]);
    member.psks.insert_or_assign(id, secret);
    if (member.state)
    {
        member.state->add_external_psk(id, secret);
    }
    say("ok");
}

// A proposal cached for the epoch's commits, and a Remove's reference noted.
void cache_proposal(Member& member, const mls::ValidatedContent& validated)
{
    const mls::AuthenticatedContent& authenticated = validated.authenticated_content();
    const mls::Proposal&             proposal      = variant::get<mls::Proposal>(authenticated.content.content);
    if (variant::holds_alternative<mls::Remove>(proposal.content))
    {
        if (member.removal_count == kMaxRemovals)
        {
            throw Refusal{"full"};
        }
        const uint32_t removed = variant::get<mls::Remove>(proposal.content).removed.val;
        member.removals[member.removal_count++] = {member.suite.ref(authenticated), removed};
        if (removed == member.state->index().val)
        {
            return; // MLSpp caches no Remove of the member itself; the commit that holds it removes the member
        }
    }
    if (member.state->handle(validated))
    {
        throw Refusal{"proposal-made-a-state"};
    }
}

// propose NAME WIRE add KEY_PACKAGE | update | remove LEAF | psk ID
void propose(const std::string_view* tokens, size_t count)
{
    Member&                member  = member_of(tokens[1]);
    mls::State&            state   = state_of(member);
    const mls::MessageOpts options = {encrypted(tokens[2]), {}, 0U};
    const std::string_view kind    = tokens[3];
    std::optional<mls::MLSMessage> message;
    if (kind == "update" && count == 4U)
    {
        message.emplace(state.update(mls::HPKEPrivateKey::generate(member.suite), {}, options));
    }
    else if (kind == "add" && count == 5U)
    {
        message.emplace(state.add(message_body<mls::KeyPackage>(tokens[4]), options));
    }
    else if (kind == "remove" && count == 5U)
    {
        message.emplace(state.remove(leaf_index(tokens[4]), options));
    }
    else if (kind == "psk" && count == 5U)
    {
        message.emplace(state.pre_shared_key(hex_bytes(tokens[4]), options));
    }
    else
    {
        throw Refusal{"usage"};
    }
    // MLSpp caches a proposal when it is handled, its sender's own as the delivery service returns it.
    cache_proposal(member, state.unwrap(*message));
    say("ok");
    say_hex("message", mls::tls::marshal(*message));
}

// commit NAME WIRE PATH INLINE [add=KEY_PACKAGE] [remove=LEAF] [psk=ID] ...
void commit(const std::string_view* tokens, size_t count)
{
    Member&         member = member_of(tokens[1]);
    mls::State&     state  = state_of(member);
    mls::CommitOpts options;
    options.force_path  = tokens[3] == "1";
    options.inline_tree = tokens[4] == "1";
    for (size_t i = 5U; i < count; ++i)
    {
        const std::string_view token = tokens[i];
        if (token.starts_with("add="))
        {
            options.extra_proposals.push_back(state.add_proposal(message_body<mls::KeyPackage>(token.substr(4U))));
        }
        else if (token.starts_with("remove="))
        {
            options.extra_proposals.push_back(state.remove_proposal(leaf_index(token.substr(7U))));
        }
        else if (token.starts_with("psk="))
        {
            options.extra_proposals.push_back(state.pre_shared_key_proposal(hex_bytes(token.substr(4U))));
        }
        else
        {
            throw Refusal{"usage"};
        }
    }
    const bool inline_tree        = options.inline_tree;
    auto [message, welcome, next] = state.commit(mls::random_bytes(member.suite.secret_size()), options,
                                                 mls::MessageOpts{encrypted(tokens[2]), {}, 0U});
    say("ok");
    say_hex("commit", mls::tls::marshal(message));
    say_hex("welcome", welcome.secrets.empty() ? bytes() : marshal_message(welcome));
    say_hex("group_info", marshal_message(next.group_info(inline_tree)));
    say_hex("tree", mls::tls::marshal(next.tree()));
    say_number("epoch", next.epoch());
    adopt(member, std::move(next));
}

// Whether a commit removes the member: a Remove of its leaf by value, or a reference to one it handled this epoch.
bool removes(const Member& member, const mls::Commit& commit)
{
    const uint32_t own = member.state->index().val;
    for (const mls::ProposalOrRef& entry : commit.proposals)
    {
        if (variant::holds_alternative<mls::Proposal>(entry.content))
        {
            const mls::Proposal& proposal = variant::get<mls::Proposal>(entry.content);
            if (variant::holds_alternative<mls::Remove>(proposal.content) &&
                variant::get<mls::Remove>(proposal.content).removed.val == own)
            {
                return true;
            }
            continue;
        }
        const bytes& ref = variant::get<mls::ProposalRef>(entry.content);
        for (size_t i = 0U; i < member.removal_count; ++i)
        {
            if (member.removals[i].ref == ref && member.removals[i].leaf == own)
            {
                return true;
            }
        }
    }
    return false;
}

// process NAME MESSAGE
void process(const std::string_view* tokens)
{
    Member&                          member        = member_of(tokens[1]);
    mls::State&                      state         = state_of(member);
    const mls::MLSMessage            message       = mls::tls::get<mls::MLSMessage>(hex_bytes(tokens[2]));
    const mls::ValidatedContent      validated     = state.unwrap(message);
    const mls::AuthenticatedContent& authenticated = validated.authenticated_content();
    const mls::GroupContent&         content       = authenticated.content;
    switch (content.content_type())
    {
    case mls::ContentType::application:
        say("ok type=application");
        say_hex("data", variant::get<mls::ApplicationData>(content.content).data);
        return;
    case mls::ContentType::proposal:
        cache_proposal(member, validated);
        say("ok type=proposal");
        return;
    case mls::ContentType::commit:
    {
        const uint64_t next_epoch = state.epoch() + 1U;
        if (removes(member, variant::get<mls::Commit>(content.content)))
        {
            member.state.reset();
            say("ok type=commit");
            say_number("epoch", next_epoch);
            say(" removed=1");
            return;
        }
        std::optional<mls::State> next = state.handle(validated);
        if (!next)
        {
            throw Refusal{"commit-made-no-state"};
        }
        adopt(member, std::move(*next));
        say("ok type=commit");
        say_number("epoch", member.state->epoch());
        say(" removed=0");
        return;
    }
    default:
        throw Refusal{"content-type"};
    }
}

// protect NAME DATA
void protect(const std::string_view* tokens)
{
    mls::State& state = state_of(member_of(tokens[1]));
    say("ok");
    say_hex("message", mls::tls::marshal(state.protect({}, hex_bytes(tokens[2]), 0U)));
}

// group_info NAME INLINE
void group_info(const std::string_view* tokens)
{
    const mls::State& state = state_of(member_of(tokens[1]));
    if (tokens[2] != "0" && tokens[2] != "1")
    {
        throw Refusal{"usage"};
    }
    say("ok");
    say_hex("group_info", marshal_message(state.group_info(tokens[2] == "1")));
    say_hex("tree", mls::tls::marshal(state.tree()));
}

// external_join NAME GROUP_INFO TREE [resync=LEAF] [psk=ID] ...
void external_join(const std::string_view* tokens, size_t count)
{
    Member&                       member = member_of(tokens[1]);
    std::optional<mls::LeafIndex> resync;
    size_t                        named  = 0U;
    for (size_t i = 4U; i < count; ++i)
    {
        const std::string_view token = tokens[i];
        if (token.starts_with("resync="))
        {
            resync = leaf_index(token.substr(7U));
        }
        else if (token.starts_with("psk=") && member.psks.count(hex_bytes(token.substr(4U))) == 1U)
        {
            ++named;
        }
        else
        {
            throw Refusal{"usage"};
        }
    }
    if (named != member.psks.size())
    {
        throw Refusal{"usage"};
    }
    auto [message, next] = mls::State::external_join(
        mls::random_bytes(member.suite.secret_size()), *member.sig_priv, *member.key_package,
        message_body<mls::GroupInfo>(tokens[2]), tree_of(tokens[3]), mls::MessageOpts{false, {}, 0U}, resync,
        member.psks);
    say("ok");
    say_hex("commit", mls::tls::marshal(message));
    say_number("epoch", next.epoch());
    adopt(member, std::move(next));
}

// state NAME
void report(const std::string_view* tokens)
{
    const mls::State& state = state_of(member_of(tokens[1]));
    say("ok");
    say_number("epoch", state.epoch());
    say_number("leaf", state.index().val);
    say_hex("authenticator", state.epoch_authenticator());
    say_hex("exporter", state.do_export(kExportLabel, mls::bytes_ns::from_ascii(kExportContext), kExportBytes));
    say_hex("tree_hash", state.tree().root_hash());
    say(" roster=");
    bool first = true;
    for (uint32_t i = 0U; i < state.tree().size.val; ++i)
    {
        const std::optional<mls::LeafNode> leaf = state.tree().leaf_node(mls::LeafIndex{i});
        if (!leaf)
        {
            continue;
        }
        char index[16];
        const int written = std::snprintf(index, sizeof(index), "%s%u:", first ? "" : ",", i);
        say(std::string_view(index, static_cast<size_t>(written)));
        first = false;
        say_hex_digits(leaf->credential.get<mls::BasicCredential>().identity);
    }
}

void dispatch(const std::string_view* tokens, size_t count)
{
    const std::string_view op = tokens[0];
    if (op == "member" && count == 6U)
    {
        make_member(tokens);
    }
    else if (op == "create" && count == 3U)
    {
        create(tokens);
    }
    else if (op == "join" && count == 4U)
    {
        join(tokens);
    }
    else if (op == "psk" && count == 4U)
    {
        hold_psk(tokens);
    }
    else if (op == "propose" && count >= 4U)
    {
        propose(tokens, count);
    }
    else if (op == "commit" && count >= 5U)
    {
        commit(tokens, count);
    }
    else if (op == "process" && count == 3U)
    {
        process(tokens);
    }
    else if (op == "protect" && count == 3U)
    {
        protect(tokens);
    }
    else if (op == "group_info" && count == 3U)
    {
        group_info(tokens);
    }
    else if (op == "external_join" && count >= 4U)
    {
        external_join(tokens, count);
    }
    else if (op == "state" && count == 2U)
    {
        report(tokens);
    }
    else
    {
        throw Refusal{"usage"};
    }
}

// The refusal's reply, replacing whatever the request had said.
void refuse(const char* word, const char* detail)
{
    g_reply_size = 0U;
    say("error ");
    say(word);
    if (detail != nullptr)
    {
        say(":");
        for (const char* c = detail; *c != '\0' && g_reply_size < 4096U; ++c)
        {
            const char kept = *c == ' ' || *c == '\n' || *c == '\r' ? '_' : *c;
            say(std::string_view(&kept, 1U));
        }
    }
}

int serve()
{
    while (std::fgets(g_line, sizeof(g_line), stdin) != nullptr)
    {
        size_t length = std::strlen(g_line);
        if (length == 0U || g_line[length - 1U] != '\n')
        {
            return 2; // a line longer than the buffer, or input cut short
        }
        g_line[--length] = '\0';
        const std::string_view line(g_line, length);
        std::string_view       tokens[kMaxTokens];
        size_t                 count = 0U;
        size_t                 start = 0U;
        while (start < line.size() && count < kMaxTokens)
        {
            const size_t end = line.find(' ', start);
            tokens[count++]  = end == std::string_view::npos ? line.substr(start) : line.substr(start, end - start);
            start            = end == std::string_view::npos ? line.size() : end + 1U;
        }
        g_reply_size = 0U;
        g_overflow   = false;
        try
        {
            if (count == 0U || start < line.size())
            {
                throw Refusal{"usage"};
            }
            dispatch(tokens, count);
            if (g_overflow)
            {
                throw Refusal{"reply-too-long"};
            }
        }
        catch (const Refusal& refusal)
        {
            refuse(refusal.word, nullptr);
        }
        catch (const std::exception& error)
        {
            refuse("refused", error.what());
        }
        g_reply[g_reply_size] = '\n';
        if (std::fwrite(g_reply, 1U, g_reply_size + 1U, stdout) != g_reply_size + 1U || std::fflush(stdout) != 0)
        {
            return 1;
        }
    }
    return 0;
}
} // namespace

int main()
{
    try
    {
        return serve();
    }
    catch (...)
    {
        return 3; // a failure outside any request (no memory for a reply)
    }
}
