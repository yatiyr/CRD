// DIAG.2a -- typed diagnostic events, identities, authorable policy and the code bridge.
// Contract: docs/design/runtime-diagnostics.md#diag-2a; the schema lives in crd-perf.

#include <crd/perf/diagnostics.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string_view>

namespace
{
namespace cp   = crd::perf;
namespace cont = crd::containers;

// A `find`-free substring check for these tests (crd::String exposes no find).
bool json_contains(const cont::String& j, std::string_view needle)
{
    return std::string_view{j.c_str(), j.size()}.find(needle) != std::string_view::npos;
}

cp::SourceIdentity here()
{
    return cp::SourceIdentity{cont::StringView{"test_diagnostics.cpp"}, cont::StringView{"test"}, 42U};
}
} // namespace

TEST_CASE("diag: severity taxonomy is distinct and fatal is separable", "[diag][events]")
{
    CHECK(cont::StringView{cp::severity_name(cp::Severity::RecoverableError)} ==
          cont::StringView{"recoverable_error"});
    CHECK(cont::StringView{cp::severity_name(cp::Severity::FatalInvariant)} ==
          cont::StringView{"fatal_invariant"});
    CHECK(cp::is_fatal(cp::Severity::FatalInvariant));
    CHECK_FALSE(cp::is_fatal(cp::Severity::DeveloperAssertion));
    CHECK_FALSE(cp::is_fatal(cp::Severity::InstrumentFailure));
}

TEST_CASE("diag: a valid policy commits and advances the generation", "[diag][policy]")
{
    cp::DiagnosticPolicyStore store;
    const crd::u32            g0 = store.generation();

    cp::DiagnosticPolicy p;
    p.min_severity      = cp::Severity::RecoverableError;
    p.max_events        = 8192U;
    p.max_message_bytes = 1024U;

    CHECK(store.load(p) == cp::PolicyLoadStatus::Committed);
    CHECK(store.generation() == g0 + 1U);
    CHECK(store.active().max_message_bytes == 1024U);
}

TEST_CASE("diag: a malformed or oversized policy cannot move the active generation", "[diag][policy]")
{
    cp::DiagnosticPolicyStore store;

    // Establish a known-good baseline.
    cp::DiagnosticPolicy good;
    good.max_message_bytes = 256U;
    REQUIRE(store.load(good) == cp::PolicyLoadStatus::Committed);
    const crd::u32 g = store.generation();

    // Unknown schema version -> rejected, generation + active retained (last-known-good).
    cp::DiagnosticPolicy bad_version = good;
    bad_version.schema_version       = 999U;
    CHECK(store.load(bad_version) == cp::PolicyLoadStatus::RejectedUnknownVersion);
    CHECK(store.generation() == g);
    CHECK(store.active().max_message_bytes == 256U);

    // Oversized -> rejected, still unchanged.
    cp::DiagnosticPolicy oversized = good;
    oversized.max_message_bytes     = cp::kMaxPolicyMessageBytes + 1U;
    CHECK(store.load(oversized) == cp::PolicyLoadStatus::RejectedOutOfBounds);
    CHECK(store.generation() == g);

    // Zero bound -> rejected.
    cp::DiagnosticPolicy zero = good;
    zero.max_events           = 0U;
    CHECK(store.load(zero) == cp::PolicyLoadStatus::RejectedOutOfBounds);
    CHECK(store.generation() == g);
    CHECK(store.active().max_message_bytes == 256U);
}

TEST_CASE("diag: message truncation to the policy bound is explicit", "[diag][events]")
{
    cp::DiagnosticPolicyStore store;
    cp::DiagnosticPolicy      p;
    p.max_message_bytes = 8U;
    REQUIRE(store.load(p) == cp::PolicyLoadStatus::Committed);

    const cp::DiagnosticEvent big =
        cp::make_event(1U, cp::Severity::Warning, here(), cont::StringView{"0123456789ABCDEF"}, store);
    CHECK(big.message_truncated);
    CHECK(big.message.size() == 8U);

    const cp::DiagnosticEvent small =
        cp::make_event(1U, cp::Severity::Warning, here(), cont::StringView{"short"}, store);
    CHECK_FALSE(small.message_truncated);
    CHECK(small.message.size() == 5U);
}

TEST_CASE("diag: policy admits by severity but never gates a fatal invariant", "[diag][policy]")
{
    cp::DiagnosticPolicy p;
    p.min_severity = cp::Severity::RecoverableError;

    CHECK_FALSE(cp::policy_admits(p, cp::Severity::Info));
    CHECK_FALSE(cp::policy_admits(p, cp::Severity::Warning));
    CHECK(cp::policy_admits(p, cp::Severity::RecoverableError));
    CHECK(cp::policy_admits(p, cp::Severity::FatalInvariant)); // always admitted
}

TEST_CASE("diag: JSON is deterministic, escaped and carries the structured context", "[diag][json]")
{
    cp::DiagnosticPolicyStore store;
    cp::DiagnosticEvent       e =
        cp::make_event(7U, cp::Severity::RecoverableError, here(), cont::StringView{"a \"quote\"\nline"}, store);
    e.generation.value = 99U;

    cont::String j;
    cp::to_json(e, j);

    CHECK(j.size() > 0U);
    CHECK(json_contains(j, "\"schema\":1"));
    CHECK(json_contains(j, "\"code\":7"));
    CHECK(json_contains(j, "\"module\":\"foundation\""));
    CHECK(json_contains(j, "\"severity\":\"recoverable_error\""));
    CHECK(json_contains(j, "\"generation\":99"));
    CHECK(json_contains(j, "\\\"quote\\\"")); // escaped quotes
    CHECK(json_contains(j, "\\n"));           // escaped newline

    // Deterministic: the same event serialises identically twice.
    cont::String j2;
    cp::to_json(e, j2);
    CHECK(cont::StringView{j.c_str(), j.size()} == cont::StringView{j2.c_str(), j2.size()});
}

TEST_CASE("diag: the code bridge assigns modules without a CEIR dependency", "[diag][bridge]")
{
    cp::reset_code_registry();

    CHECK(cont::StringView{cp::owning_module(0x100U)} == cont::StringView{"foundation"});
    CHECK(cont::StringView{cp::owning_module(0x20000U)} == cont::StringView{"unassigned"});

    // A module registers its range from its own side.
    CHECK(cp::register_code_range(cont::StringView{"ceir"}, 0x10000U, 0x1FFFFU));
    CHECK(cont::StringView{cp::owning_module(0x10005U)} == cont::StringView{"ceir"});

    // Overlap, foundation-range and inverted ranges are all refused.
    CHECK_FALSE(cp::register_code_range(cont::StringView{"dup"}, 0x1FFFFU, 0x20000U)); // overlaps ceir
    CHECK_FALSE(cp::register_code_range(cont::StringView{"low"}, 0x0FFFFU, 0x10000U)); // into foundation
    CHECK_FALSE(cp::register_code_range(cont::StringView{"inv"}, 0x30000U, 0x20000U)); // inverted

    // A CEIR-coded event and a foundation-coded event serialise through the same path,
    // differing only in the module tag -- the "same structured context" acceptance.
    cp::DiagnosticPolicyStore store;
    const cp::DiagnosticEvent ceir_ev =
        cp::make_event(0x10005U, cp::Severity::RecoverableError, here(), cont::StringView{"x"}, store);
    cont::String j;
    cp::to_json(ceir_ev, j);
    CHECK(json_contains(j, "\"module\":\"ceir\""));

    cp::reset_code_registry();
}

namespace
{
cp::DiagnosticEvent g_captured;
bool                g_fired = false;
void                capture_fatal(const cp::DiagnosticEvent& e)
{
    g_captured = e; // move-assign copy of the event (String owns its bytes)
    g_fired    = true;
}
} // namespace

TEST_CASE("diag: check_invariant is always-on and routes a fatal event to the handler", "[diag][fatal]")
{
    g_fired = false;
    cp::set_fatal_invariant_handler(capture_fatal);

    // Passing invariant: no fire.
    cp::check_invariant(true, 5U, here(), cont::StringView{"ok"});
    CHECK_FALSE(g_fired);

    // Failing invariant: fires with a FatalInvariant event carrying the message.
    cp::check_invariant(false, 5U, here(), cont::StringView{"boom"});
    CHECK(g_fired);
    CHECK(cp::is_fatal(g_captured.severity));
    CHECK(cont::StringView{g_captured.message.c_str(), g_captured.message.size()} ==
          cont::StringView{"boom"});

    cp::set_fatal_invariant_handler(nullptr);
}
