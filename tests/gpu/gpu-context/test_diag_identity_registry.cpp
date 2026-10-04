// DIAG.7a(d2a): the IdentityRegistry (mints alias-proof ObjectIdentity values) + format_debug_name (builds the
// bracketed native debug name (d1)'s parse() finds). Device-free.
#include <crd/gpu/identity_registry.hpp>
#include <crd/gpu/object_identity.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <cstring>
#include <string_view>
#include <thread>

namespace
{
namespace g = crd::gpu;
} // namespace

TEST_CASE("IdentityRegistry mints valid, live identities", "[gpu][diag][identity][registry]")
{
    g::IdentityRegistry reg;
    REQUIRE(reg.live_count(g::ObjectKind::Resource) == 0U);

    const g::ObjectIdentity a = reg.mint(g::ObjectKind::Resource);
    CHECK(a.valid());
    CHECK(a.kind == g::ObjectKind::Resource);
    CHECK(a.generation == 1U); // never-used slot
    CHECK(reg.alive(a));
    CHECK(reg.live_count(g::ObjectKind::Resource) == 1U);

    const g::ObjectIdentity b = reg.mint(g::ObjectKind::Resource);
    CHECK(b.index != a.index); // a distinct fresh slot while a is still live
    CHECK(reg.alive(b));
    CHECK(reg.live_count(g::ObjectKind::Resource) == 2U);
}

TEST_CASE("IdentityRegistry retire is idempotent and rejects invalid", "[gpu][diag][identity][registry]")
{
    g::IdentityRegistry reg;
    const g::ObjectIdentity a = reg.mint(g::ObjectKind::Program);
    REQUIRE(reg.alive(a));

    CHECK(reg.retire(a));            // first retire succeeds
    CHECK_FALSE(reg.alive(a));       // now stale
    CHECK_FALSE(reg.retire(a));      // double-retire rejected, not corruption
    CHECK(reg.live_count(g::ObjectKind::Program) == 0U);

    CHECK_FALSE(reg.retire(g::ObjectIdentity{})); // invalid identity
    CHECK_FALSE(reg.alive(g::ObjectIdentity{}));
}

TEST_CASE("IdentityRegistry reuses a slot with a bumped generation (alias-proof)", "[gpu][diag][identity][registry]")
{
    g::IdentityRegistry reg;
    const g::ObjectIdentity a = reg.mint(g::ObjectKind::Resource);
    REQUIRE(reg.retire(a));
    const g::ObjectIdentity b = reg.mint(g::ObjectKind::Resource); // reuses a's slot (LIFO free list)

    CHECK(b.index == a.index);           // same physical slot
    CHECK(b.generation == a.generation + 1U); // generation bumped so the old identity cannot alias the new one
    CHECK_FALSE(reg.alive(a));           // the retired identity stays dead even though its slot is live again
    CHECK(reg.alive(b));
    CHECK_FALSE(a == b);                 // and they never compare equal
}

// DIAG.7a(e): the END-TO-END lifecycle proof. The earlier cases prove the two halves separately -- the registry knows a
// retired identity is dead across a slot reuse, and parse() extracts a token quoted inside a longer message. This ties
// them into the property the design states (runtime-diagnostics.md: "IDs cannot silently alias when a ... GPU object is
// reused"; "Preserve deleted generation metadata"): a validation/debug-layer message that QUOTES a freed object's native
// name must resolve, through parse() -> registry, to retired-not-alive -- and a recycled slot must not make that stale
// message alias the new live object. Device-free.
TEST_CASE("IdentityRegistry: a message quoting a freed object resolves to retired-not-alive (lifecycle, alias-proof end to end)",
          "[gpu][diag][identity][registry][lifecycle]")
{
    g::IdentityRegistry reg;

    // A resource is created; its identity is stamped into a native debug name, which a driver message later QUOTES.
    const g::ObjectIdentity live = reg.mint(g::ObjectKind::Resource);
    char name[g::kDebugNamePrefixChars + 64U] = {};
    REQUIRE(g::format_debug_name(live, "shadowmap 2048x2048x1", name, sizeof(name)) != 0U);
    char msg[g::kDebugNamePrefixChars + 128U];
    std::snprintf(msg, sizeof(msg), "VALIDATION: object %s accessed after free", name); // token INSIDE a longer message

    // While the object is alive, the quoted name resolves THROUGH THE REGISTRY to a live identity.
    g::ObjectIdentity from_live;
    REQUIRE(g::parse(std::string_view{msg}, from_live));
    REQUIRE(from_live == live);
    CHECK(reg.alive(from_live));

    // The object is destroyed. The SAME message text still parses (the token is unchanged) -- but the registry now
    // resolves it to retired-not-alive: a stale message cannot be mistaken for a live object.
    REQUIRE(reg.retire(live));
    g::ObjectIdentity from_dead;
    REQUIRE(g::parse(std::string_view{msg}, from_dead));
    REQUIRE(from_dead == live);
    CHECK_FALSE(reg.alive(from_dead));

    // A NEW resource reuses the freed slot with a bumped generation. Parsing the OLD message must STILL resolve DEAD
    // (no silent alias), while the new object is the live one -- proven end to end, not merely at the value level.
    const g::ObjectIdentity reused = reg.mint(g::ObjectKind::Resource);
    CHECK(reused.index == live.index);           // same physical slot
    CHECK(reused.generation != live.generation); // bumped
    g::ObjectIdentity from_stale;
    REQUIRE(g::parse(std::string_view{msg}, from_stale));
    REQUIRE(from_stale == live);          // the stale token still decodes to the OLD identity...
    CHECK_FALSE(reg.alive(from_stale));   // ... which the registry correctly reports DEAD against the recycled slot
    CHECK(reg.alive(reused));             // the new object is the live one
}

TEST_CASE("IdentityRegistry keeps kinds in independent index spaces", "[gpu][diag][identity][registry]")
{
    g::IdentityRegistry reg;
    const g::ObjectIdentity resource = reg.mint(g::ObjectKind::Resource);
    const g::ObjectIdentity program  = reg.mint(g::ObjectKind::Program);
    const g::ObjectIdentity pass     = reg.mint(g::ObjectKind::Pass);

    CHECK(resource.index == 0U); // each kind starts its own index space at 0
    CHECK(program.index == 0U);
    CHECK(pass.index == 0U);
    CHECK(reg.alive(resource));
    CHECK(reg.alive(program));
    CHECK(reg.alive(pass));
    CHECK_FALSE(resource == program); // same index, different kind -> distinct identities
    CHECK_FALSE(resource == pass);
    CHECK(reg.live_count(g::ObjectKind::Resource) == 1U);
    CHECK(reg.live_count(g::ObjectKind::Program) == 1U);
    CHECK(reg.live_count(g::ObjectKind::Pass) == 1U);
}

TEST_CASE("IdentityRegistry mints uniquely under concurrent creators", "[gpu][diag][identity][registry]")
{
    // Pins the documented thread-safety: many threads minting at once must yield distinct indices (a dropped lock
    // would let SlotMap's internal arrays race -> duplicate/garbage indices or a crash).
    g::IdentityRegistry reg;
    constexpr crd::u32 thread_count = 4U;
    constexpr crd::u32 per_thread     = 100U;
    constexpr crd::u32 total   = thread_count * per_thread;
    crd::u32 indices[total] = {}; // each thread writes only its own slice -> no data race on the array itself
    {
        std::jthread workers[thread_count];
        for (crd::u32 t = 0; t < thread_count; ++t)
        {
            workers[t] = std::jthread([&reg, &indices, t]
            {
                for (crd::u32 i = 0; i < per_thread; ++i)
                {
                    indices[t * per_thread + i] = reg.mint(g::ObjectKind::Resource).index;
                }
            });
        }
    } // jthreads join here
    CHECK(reg.live_count(g::ObjectKind::Resource) == total);

    bool seen[total] = {};
    bool in_range = true;
    bool distinct = true;
    for (crd::u32 k = 0; k < total; ++k)
    {
        const crd::u32 idx = indices[k];
        if (idx >= total)
        {
            in_range = false;
            continue;
        }
        if (seen[idx])
        {
            distinct = false;
        }
        seen[idx] = true;
    }
    CHECK(in_range);  // 400 fresh mints, no retires -> indices exactly fill [0, 400)
    CHECK(distinct);  // no two mints returned the same index
}

TEST_CASE("format_debug_name builds the bracketed prefix parse() finds", "[gpu][diag][identity][registry]")
{
    const g::ObjectIdentity id{g::ObjectKind::Resource, 0x2AU, 0x7U};
    char out[g::kDebugNamePrefixChars + 64U] = {};
    const crd::usize written = g::format_debug_name(id, "shadowmap 2048x2048x1", out, sizeof(out));
    REQUIRE(written != 0U);
    REQUIRE(std::strlen(out) == written);
    // Exact string, NOT just "parse finds it": a dropped bracket would still parse but must fail this equality.
    REQUIRE(std::string_view{out} == "[crd:res:0000002a:g00000007] shadowmap 2048x2048x1");

    // And the built name round-trips back to the identity, with the human label preserved after "] ".
    g::ObjectIdentity parsed;
    REQUIRE(g::parse(std::string_view{out}, parsed));
    REQUIRE(parsed == id);
    const std::string_view text{out, written};
    const auto close = text.find("] ");
    REQUIRE(close != std::string_view::npos);
    REQUIRE(text.substr(close + 2U) == "shadowmap 2048x2048x1");
}

TEST_CASE("format_debug_name refuses invalid identity and a too-small buffer", "[gpu][diag][identity][registry]")
{
    char out[g::kDebugNamePrefixChars + 16U];
    std::memset(out, '#', sizeof(out));
    REQUIRE(g::format_debug_name(g::ObjectIdentity{}, "x", out, sizeof(out)) == 0U); // invalid identity
    REQUIRE(out[0] == '#');

    // Buffer one byte short of the exact need (chars but no room for the NUL) must refuse without overrun.
    const g::ObjectIdentity id{g::ObjectKind::Pass, 0x1U, 0x1U};
    const crd::usize need = std::strlen("[crd:pass:00000001:g00000001] lbl");
    std::memset(out, '#', sizeof(out));
    const char canary = out[need]; // the byte the NUL would occupy on success
    (void)canary;
    REQUIRE(g::format_debug_name(id, "lbl", out, need) == 0U); // no room for NUL -> refuse
    REQUIRE(out[0] == '#');
    REQUIRE(out[need] == '#'); // nothing written

    REQUIRE(g::format_debug_name(id, "lbl", out, need + 1U) == need); // exact fit succeeds
    REQUIRE(std::string_view{out} == "[crd:pass:00000001:g00000001] lbl");
}
