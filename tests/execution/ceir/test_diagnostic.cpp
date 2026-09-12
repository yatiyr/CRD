// CEIR-8g (ADR-0117, U-§102) — the DiagnosticEngine. One structured surface: stable code + severity + SourceLoc +
// message + notes + fix-its. emit collects; render prints code-name + file:line:col + message; has_errors reflects
// Error/Fatal. ⛔ THE lifetime probe: all text is COPIED into the engine arena, so a diagnostic outlives the dying
// buffer it was emitted from (an ASan use-after-scope probe on the money config). Host-only. ASCII names.

#include <crd/ceir/ceir.hpp>
#include <crd/ceir/diagnostic.hpp>

#include <crd/memory/allocators/growable_tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstring> // std::memcmp

using namespace crd::ceir; // NOLINT(google-build-using-namespace)
using crd::containers::String;
using crd::containers::StringView;
using crd::usize;

namespace
{
[[nodiscard]] bool sv_eq(StringView a, const char* b) noexcept
{
    const usize bl = StringView(b).size();
    return a.size() == bl && (bl == 0U || std::memcmp(a.data(), b, bl) == 0);
}
} // namespace

TEST_CASE("ceir 8g: emit collects diagnostics; has_errors reflects Error/Fatal; render prints code + loc + message", "[ceir][diagnostic]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    const crd::u32               file = ctx.register_file("src/a.ceir");
    DiagnosticEngine             eng(ctx, &root);
    CHECK_FALSE(eng.has_errors());

    eng.emit(Severity::Warning, make_diagnostic_code("ceir.shadowed"), StringView("ceir.shadowed"),
             SourceLoc{file, 3U, 7U}, StringView("name shadows an outer binding"));
    CHECK(eng.count() == 1U);
    CHECK_FALSE(eng.has_errors()); // a warning is not an error

    const StringView notes[1] = {StringView("previous definition here")};
    eng.emit(Severity::Error, make_diagnostic_code("ceir.unresolved_symbol"), StringView("ceir.unresolved_symbol"),
             SourceLoc{file, 12U, 4U}, StringView("unresolved symbol 'foo'"),
             crd::containers::ConstSpan<StringView>(notes, 1U));
    CHECK(eng.count() == 2U);
    CHECK(eng.has_errors());

    const String r = eng.render(eng.at(1), &root);
    // "ceir.unresolved_symbol [error] src/a.ceir:12:4: unresolved symbol 'foo'\n  note: previous definition here"
    const StringView rv(r.data(), r.size());
    CHECK(std::memcmp(rv.data(), "ceir.unresolved_symbol [error] src/a.ceir:12:4: unresolved symbol 'foo'", 69U) == 0);
    // the note is appended
    bool has_note = false;
    for (usize i = 0; i + 6U <= rv.size(); ++i)
    {
        if (std::memcmp(rv.data() + i, "note: ", 6U) == 0) { has_note = true; break; }
    }
    CHECK(has_note);
}

TEST_CASE("ceir 8g: a Fatal diagnostic sets has_errors", "[ceir][diagnostic]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    DiagnosticEngine             eng(ctx, &root);
    eng.emit(Severity::Fatal, make_diagnostic_code("ceir.ice"), StringView("ceir.ice"), SourceLoc{},
             StringView("internal compiler error"));
    CHECK(eng.has_errors());
    // a 0 file_id renders "<unknown>"
    const String     r  = eng.render(eng.at(0), &root);
    const StringView rv(r.data(), r.size());
    bool             has_unknown = false;
    for (usize i = 0; i + 9U <= rv.size(); ++i)
    {
        if (std::memcmp(rv.data() + i, "<unknown>", 9U) == 0) { has_unknown = true; break; }
    }
    CHECK(has_unknown);
}

TEST_CASE("ceir 8g: the engine COPIES emitted text - a diagnostic outlives the buffer it came from", "[ceir][diagnostic]")
{
    // ⛔ the alloc-outlives-borrowers probe: emit from a SCOPE-LOCAL String, let it die, then read the diagnostic. If
    // emit did not copy, this is a use-after-scope ASan catches on the money config.
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    DiagnosticEngine             eng(ctx, &root);
    {
        String msg(&root);
        msg.append("temporary message text");
        String note(&root);
        note.append("temporary note text");
        const StringView notes[1] = {StringView(note.data(), note.size())};
        eng.emit(Severity::Error, make_diagnostic_code("ceir.temp"), StringView("ceir.temp"), SourceLoc{},
                 StringView(msg.data(), msg.size()), crd::containers::ConstSpan<StringView>(notes, 1U));
    } // msg + note die here
    // read AFTER the source buffers died — the engine's copies must survive.
    const Diagnostic& d = eng.at(0);
    CHECK(sv_eq(d.message, "temporary message text"));
    REQUIRE(d.notes.size() == 1U);
    CHECK(sv_eq(d.notes[0], "temporary note text"));
    CHECK(sv_eq(d.code_name, "ceir.temp"));
}

TEST_CASE("ceir 8g: diagnostic codes are distinct FNVs sharing the one hash routine", "[ceir][diagnostic]")
{
    CHECK(make_diagnostic_code("ceir.a") != make_diagnostic_code("ceir.b"));
    CHECK(make_diagnostic_code("ceir.a").valid());
    // the ONE-shared-fnv1a_ct pin: a diagnostic code and an analysis id of the SAME name are the SAME hash value.
    CHECK(make_diagnostic_code("crd.same").value == make_analysis_id("crd.same").value);
}
