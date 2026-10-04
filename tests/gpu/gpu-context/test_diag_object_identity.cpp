// DIAG.7a(c): the stable Cerid GPU object identity value type + its name-encoding contract (device-free).
#include <crd/gpu/object_identity.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <string_view>

namespace
{
namespace g = crd::gpu;

// encode then parse-back must reproduce the identity exactly.
[[nodiscard]] bool round_trips(g::ObjectIdentity id)
{
    char buffer[g::kObjectIdentityBufferSize] = {};
    const crd::usize written = g::encode(id, buffer, sizeof(buffer));
    if (written == 0U || written > g::kObjectIdentityMaxChars)
    {
        return false;
    }
    if (std::strlen(buffer) != written)
    {
        return false;
    }
    g::ObjectIdentity parsed;
    if (!g::parse(std::string_view{buffer, written}, parsed))
    {
        return false;
    }
    return parsed == id;
}
} // namespace

TEST_CASE("GPU object identity mirrors the SlotMap handle validity contract", "[gpu][diag][identity]")
{
    REQUIRE_FALSE(g::ObjectIdentity{}.valid());                                  // default is the null identity
    REQUIRE_FALSE((g::ObjectIdentity{g::ObjectKind::Resource, 5U, 0U}).valid()); // generation 0 is reserved
    REQUIRE_FALSE((g::ObjectIdentity{g::ObjectKind::Resource, 0xFFFFFFFFU, 3U}).valid()); // null index
    REQUIRE((g::ObjectIdentity{g::ObjectKind::Resource, 0U, 1U}).valid());       // index 0 is a real slot
    REQUIRE((g::ObjectIdentity{g::ObjectKind::Pass, 0xFFFFFFFEU, 0xFFFFFFFFU}).valid());
}

TEST_CASE("GPU object identity encode/parse round-trips for every kind and boundary", "[gpu][diag][identity]")
{
    const crd::u32 indices[] = {0U, 1U, 0x2AU, 0xFFFFFFFEU};      // 0xFFFFFFFF is the null slot, never encoded
    const crd::u32 generations[] = {1U, 2U, 0x7U, 0xFFFFFFFFU};   // 0 is reserved, never a live generation
    for (const g::ObjectKind kind : {g::ObjectKind::Resource, g::ObjectKind::Program, g::ObjectKind::Pass})
    {
        for (const crd::u32 index : indices)
        {
            for (const crd::u32 generation : generations)
            {
                const g::ObjectIdentity id{kind, index, generation};
                CAPTURE(g::to_string(kind), index, generation);
                REQUIRE(round_trips(id));
            }
        }
    }
}

TEST_CASE("GPU object identity encodes fixed-width lowercase hex", "[gpu][diag][identity]")
{
    char buffer[g::kObjectIdentityBufferSize] = {};
    const g::ObjectIdentity id{g::ObjectKind::Resource, 0x2AU, 0x7U};
    const crd::usize written = g::encode(id, buffer, sizeof(buffer));
    REQUIRE(written != 0U);
    REQUIRE(std::string_view{buffer, written} == "crd:res:0000002a:g00000007");

    const g::ObjectIdentity pass{g::ObjectKind::Pass, 0x0AU, 0x3U};
    REQUIRE(g::encode(pass, buffer, sizeof(buffer)) != 0U);
    REQUIRE(std::string_view{buffer} == "crd:pass:0000000a:g00000003");
}

TEST_CASE("GPU object identity refuses to encode an invalid identity", "[gpu][diag][identity]")
{
    char buffer[g::kObjectIdentityBufferSize];
    std::memset(buffer, '#', sizeof(buffer));
    REQUIRE(g::encode(g::ObjectIdentity{}, buffer, sizeof(buffer)) == 0U);              // null identity
    REQUIRE(g::encode(g::ObjectIdentity{g::ObjectKind::Program, 4U, 0U}, buffer, sizeof(buffer)) == 0U); // gen 0
    REQUIRE(buffer[0] == '#'); // nothing was written
}

TEST_CASE("GPU object identity parses a token embedded in a longer string", "[gpu][diag][identity]")
{
    g::ObjectIdentity parsed;
    REQUIRE(g::parse("[crd:pass:0000000a:g00000003] frame overlay 1920x1080", parsed));
    REQUIRE(parsed == (g::ObjectIdentity{g::ObjectKind::Pass, 0x0AU, 0x3U}));

    REQUIRE(g::parse("D3D12 WARNING: resource crd:res:0000002a:g00000007 was reset", parsed));
    REQUIRE(parsed == (g::ObjectIdentity{g::ObjectKind::Resource, 0x2AU, 0x7U}));

    // A malformed "crd:" earlier in the string must not stop the scan from finding the real token afterwards.
    REQUIRE(g::parse("crd:bogus then crd:prog:00000001:g00000002 here", parsed));
    REQUIRE(parsed == (g::ObjectIdentity{g::ObjectKind::Program, 1U, 2U}));

    // parse accepts uppercase hex (lenient); encode only ever emits lowercase, so this is a real asymmetry to pin.
    REQUIRE(g::parse("crd:res:0000002A:g0000007F", parsed));
    REQUIRE(parsed == (g::ObjectIdentity{g::ObjectKind::Resource, 0x2AU, 0x7FU}));
}

TEST_CASE("GPU object identity parse rejects malformed tokens", "[gpu][diag][identity]")
{
    g::ObjectIdentity parsed;
    REQUIRE_FALSE(g::parse("crx:res:00000001:g00000001", parsed)); // wrong prefix
    REQUIRE_FALSE(g::parse("crd:xyz:00000001:g00000001", parsed)); // unknown kind
    REQUIRE_FALSE(g::parse("crd:res:zzzzzzzz:g00000001", parsed)); // non-hex index
    REQUIRE_FALSE(g::parse("crd:res:00000001:00000001", parsed));  // missing ":g"
    REQUIRE_FALSE(g::parse("crd:res:00000001:gzzzzzzzz", parsed)); // non-hex generation
    REQUIRE_FALSE(g::parse("crd:res:0000000", parsed));            // truncated (fewer than 8 index hex)
    REQUIRE_FALSE(g::parse("crd:res:00000001:g000000", parsed));   // truncated generation
    REQUIRE_FALSE(g::parse("nothing to see here", parsed));        // no token at all
    REQUIRE_FALSE(g::parse("crd:res:00000000:g00000000", parsed)); // well-formed but generation 0 -> invalid
}

TEST_CASE("GPU object identity encode refuses a too-small buffer without overrun", "[gpu][diag][identity]")
{
    // A full "prog"/"pass" encode writes kObjectIdentityBufferSize bytes (chars + NUL) into [0, size); the canary
    // sits at index kObjectIdentityBufferSize, one past the largest legitimate write, and must survive untouched.
    char storage[g::kObjectIdentityBufferSize + 1U];
    std::memset(storage, '#', sizeof(storage));
    constexpr crd::usize canary = g::kObjectIdentityBufferSize;

    const g::ObjectIdentity prog{g::ObjectKind::Program, 0x2AU, 0x7U};
    const crd::usize need = std::strlen("crd:prog:0000002a:g00000007"); // 27 chars, needs 28 with NUL
    REQUIRE(g::encode(prog, storage, need) == 0U); // room for the chars but not the NUL -> refuse
    REQUIRE(storage[0] == '#');                    // nothing written on refusal
    REQUIRE(storage[canary] == '#');              // canary intact

    // Exactly enough room (chars + NUL) succeeds and stops there.
    REQUIRE(g::encode(prog, storage, need + 1U) == need);
    REQUIRE(std::string_view{storage} == "crd:prog:0000002a:g00000007");
    REQUIRE(storage[canary] == '#');              // did not run past what it needed
}

TEST_CASE("GPU object kind names are stable", "[gpu][diag][identity]")
{
    REQUIRE(std::string_view{g::to_string(g::ObjectKind::Resource)} == "res");
    REQUIRE(std::string_view{g::to_string(g::ObjectKind::Program)} == "prog");
    REQUIRE(std::string_view{g::to_string(g::ObjectKind::Pass)} == "pass");
}
