#pragma once

// crd-ceir-cook — DIAG.10c: the wrong-but-noncrashing result oracle at the CEIR boundary. A program that returns the
// wrong value runs to completion with no error, so no executor check can see it. Only something independent of the
// program can: an invariant the results must satisfy (an integer domain, crd/core/value_domain.hpp) or the values a
// reference computed another way (a closed form, a slower algorithm, a recorded good run). A hot reload replaces a
// program while plans and interpreters built from the previous generation may still run, so a wrong value is
// attributed to the generation that produced it (ReloadSet::locate_result): the asset, the generation, its state and
// content hash, and the op that defined the offending result in that generation's own source.
//
// The CEIR executors are integer-only (exec.hpp), so the domain here is an IntDomain; the float probes of
// value_domain.hpp wait for a float boundary. The oracle only judges the values of a run that succeeded: a run that
// failed has its error and the op it blames already.

#include <crd/ceir/cook/hot_reload.hpp>
#include <crd/containers/span.hpp>
#include <crd/containers/string.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>
#include <crd/core/value_domain.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::ceir::cook
{

// NOLINTNEXTLINE(performance-enum-size)
enum class ResultVerdict : crd::u8
{
    Agrees,               // every result is in the domain and equals the reference (when there is one)
    InvalidDomain,        // the expectation's domain is invalid (minimum > maximum): nothing was judged
    CountMismatch,        // the program returned another number of results than the reference holds
    OutOfDomain,          // a result violates the domain (an invariant, no reference needed)
    DiffersFromReference, // a result is in the domain but differs from the reference
};
[[nodiscard]] containers::StringView result_verdict_name(ResultVerdict v) noexcept;

// What the results must satisfy. `reference` is read only when `has_reference` (an empty reference expects no
// results). The domain is checked first at each index, so a value both out of the domain and different from the
// reference is OutOfDomain.
struct ResultExpectation
{
    IntDomain                       domain{};
    containers::ConstSpan<crd::i64> reference{};
    bool                            has_reference = false;
};

// The first offending result. `index` is its position; for CountMismatch it is the shorter of the two counts, and
// `value`/`expected` are 0 where that side has no value. `violations` holds the ValueViolation bits of OutOfDomain.
struct ResultCheck
{
    ResultVerdict verdict        = ResultVerdict::Agrees;
    crd::u32      index          = 0U;
    crd::i64      value          = 0;
    crd::i64      expected       = 0;
    crd::u32      violations     = 0U;
    crd::usize    count          = 0U; // results the program returned
    crd::usize    expected_count = 0U; // results the reference holds (0 without a reference)
    [[nodiscard]] bool wrong() const noexcept { return verdict != ResultVerdict::Agrees; }
};

// Judge `values` against `e` (pure: no program, no generation).
[[nodiscard]] ResultCheck check_results(containers::ConstSpan<crd::i64> values, const ResultExpectation& e) noexcept;

// A judged run, attributed. `site` is set only for a wrong result (CountMismatch, OutOfDomain, DiffersFromReference):
// the generation `h` names, current, retiring or gone, and `where` the op that defined result `check.index` there.
// Agrees and InvalidDomain leave it empty (asset 0, Gone): nothing is blamed.
struct ResultFinding
{
    ResultCheck    check;
    GenerationSite site;
    explicit ResultFinding(memory::IAllocator* a) : site(a) {}
};

// `values` are the results of running `entry` of the program `h` was minted for (from an interpreter or a plan built
// from that generation). Judges them and, when wrong, attributes them through `set.locate_result`.
[[nodiscard]] ResultFinding check_program_results(const ReloadSet& set, const ProgramHandle& h,
                                                  containers::StringView entry, containers::ConstSpan<crd::i64> values,
                                                  const ResultExpectation& e, memory::IAllocator* out);

// "<verdict>" for Agrees and InvalidDomain; otherwise "<verdict> result#<i> = <value>" then ", expected <expected>"
// (CountMismatch and DiffersFromReference), " [<violation>,...]" (OutOfDomain) or " (<n> results, expected <m>)"
// (CountMismatch), then " at " and render_generation_site.
[[nodiscard]] containers::String render_result_finding(const ResultFinding& f, memory::IAllocator* out);

} // namespace crd::ceir::cook
