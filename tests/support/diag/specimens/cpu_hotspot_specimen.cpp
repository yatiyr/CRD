// DIAG.6c(f): the "known unscoped CPU hotspot" specimen for the external WPR/perf sampling runbook.
//
// A tight compute loop in one grep-able, non-inlinable function with NO CRD_PERF_SCOPE anywhere -- so the work is
// INVISIBLE to the in-process scope profiler and can ONLY be found by an external sampler. `scripts/sample-cpu-wpr.py`
// records this exe under WPR/xperf and checks that `crd_diag_unscoped_hotspot_burn` shows up in the sampled profile;
// that is acceptance clause 3 ("a known unscoped CPU hotspot is visible through sampling; scope-only charts do not
// satisfy this case").
//
// Hygiene: __declspec(noinline) + a volatile sink so /O2 cannot fold the loop away and erase the symbol from every
// sampled stack. Standalone (links nothing) so it builds on every lane, including clang-cl-shipping.

#include <cstdint>

#if defined(_MSC_VER)
#define CRD_HOTSPOT_NOINLINE __declspec(noinline)
#else
#define CRD_HOTSPOT_NOINLINE __attribute__((noinline))
#endif

namespace
{
volatile std::uint64_t g_sink = 0U; // volatile: the loop's result must be observed, so it cannot be optimized out.
}

// Burn CPU in a named frame for roughly `iters` iterations. The name is the needle the runbook greps for.
CRD_HOTSPOT_NOINLINE std::uint64_t crd_diag_unscoped_hotspot_burn(std::uint64_t iters) noexcept
{
    std::uint64_t acc = 1U;
    for (std::uint64_t i = 1U; i <= iters; ++i)
    {
        // A dependent arithmetic chain (mul + xor + rotate) so the optimizer keeps every iteration.
        acc = acc * 6364136223846793005ULL + 1442695040888963407ULL;
        acc ^= (acc >> 29);
        acc += i;
        g_sink = g_sink + acc;
    }
    return acc;
}

int main(int argc, char** argv)
{
    // ~2 seconds of burn by default; a smaller count for a quick self-check via any argument.
    std::uint64_t iters = 2'000'000'000ULL;
    if (argc > 1 && argv[1] != nullptr && argv[1][0] == 'q')
    {
        iters = 2'000'000ULL;
    }
    const std::uint64_t r = crd_diag_unscoped_hotspot_burn(iters);
    g_sink = g_sink + r; // consume the result (volatile) so the whole call cannot be elided
    return 0;
}
