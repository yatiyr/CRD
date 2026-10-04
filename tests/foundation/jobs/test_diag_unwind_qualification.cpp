// DIAG.6c(g): optimized / fiber stack-unwind qualification.
//
// Acceptance clause 2 (runtime-diagnostics): "WPA/xperf unwinds optimized native frames via PDB (5d symbol
// identity feeds matching); qualify crd-jobs fiber stacks or explicitly mark them missing."
//
// x64 stack unwinding is table-driven (.pdata/.xdata on Windows; .eh_frame on the SysV ABI), so it is correct
// through /O2 frames with NO frame pointers. WPA/xperf read exactly those tables via the PDB; RtlCaptureStack-
// BackTrace reads the SAME tables in-process. So the correctness of optimized-frame unwinding is provable HERE,
// with no kernel sampler and no elevation -- and it MUST be exercised on the shipping (/O2) lane: win-debug
// keeps frame pointers and proves nothing about optimized unwinding. The external xperf `-stackwalk` confirmation
// is the elevation-gated half (see scripts/sample-cpu-wpr.py --stacks); this in-process oracle is the observable
// core of clause 2.
//
// The fiber case is the "hard case the acceptance calls out": crd fibers switch via a hand-rolled fiber_switch
// (a register swap onto a manually-built stack whose trampoline carries NO unwind info). A walk taken on a fiber
// stack therefore unwinds the fiber's own frames correctly but TERMINATES at the fiber entry -- it cannot cross
// the switch onto the scheduler/thread stack that resumed the fiber. That truncation is the qualified, documented
// limitation: this test proves the fiber's own frames unwind AND that the walk is bounded at the boundary (never
// a silent drop, never a runaway into garbage).

#include <catch2/catch_test_macros.hpp>

#include <crd/core/platform.hpp>
#include <crd/core/types.hpp>
#include <crd/jobs/detail/fiber_context.hpp>

#include <cstdint>

#if CRD_OS_WINDOWS
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h> // RtlCaptureStackBackTrace
#  include <intrin.h>  // _ReturnAddress
#  pragma intrinsic(_ReturnAddress)
// The intrinsic must expand in the CALLER's frame; a function would report its own return address instead.
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#  define CRD_RETADDR() (_ReturnAddress())
#  define CRD_NOINLINE  __declspec(noinline)
#else
#  include <execinfo.h> // backtrace
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage): as above, __builtin_return_address must expand in the caller
#  define CRD_RETADDR() (__builtin_return_address(0))
#  define CRD_NOINLINE  __attribute__((noinline))
#endif

namespace
{
using crd::jobs::detail::FiberContext;
using crd::jobs::detail::fiber_init_stack;
using crd::jobs::detail::fiber_switch;

constexpr int kChain     = 6;  // number of noinline functions in the recorded call chain
constexpr int kMaxFrames = 64; // capture buffer depth

// The chain records, in each function, the return address into ITS caller (_ReturnAddress). A correct unwind of
// the same chain must contain every one of those addresses, in strictly increasing frame order. This is a
// symbol-free, exact oracle: it does not depend on PDB symbolication, only on the unwind tables being right.
volatile crd::u64 g_sink    = 0;
void*             g_ret[kChain]     = {};
void*             g_frames[kMaxFrames] = {};
int               g_nframes = 0;

int capture_bt(void** out, int max) noexcept
{
#if CRD_OS_WINDOWS
    return static_cast<int>(RtlCaptureStackBackTrace(0, static_cast<ULONG>(max), out, nullptr));
#else
    return backtrace(out, max);
#endif
}

// chain_0 is the innermost frame: it records its return address and captures the backtrace. Every function folds
// the callee's result into a volatile sink AFTER the call so /O2 cannot tail-call-eliminate the frame (a bare
// `return f();` would legitimately vanish from the stack even with noinline).
CRD_NOINLINE crd::u64 chain_0() noexcept
{
    g_ret[0]  = CRD_RETADDR();
    g_nframes = capture_bt(g_frames, kMaxFrames);
    g_sink += static_cast<crd::u64>(reinterpret_cast<std::uintptr_t>(g_ret[0]));
    return g_sink;
}

#define CRD_CHAIN_FN(N, CALL)                        \
    CRD_NOINLINE crd::u64 chain_##N() noexcept       \
    {                                                \
        g_ret[N]     = CRD_RETADDR();                \
        crd::u64 r = (CALL);                         \
        g_sink += r + crd::u64{N};                   \
        return g_sink;                               \
    }

CRD_CHAIN_FN(1, chain_0())
CRD_CHAIN_FN(2, chain_1())
CRD_CHAIN_FN(3, chain_2())
CRD_CHAIN_FN(4, chain_3())
CRD_CHAIN_FN(5, chain_4())

// Find each recorded return address in the captured backtrace; require all present and in strictly increasing
// frame order. Returns the index in g_frames of the outermost chain frame (for boundary reasoning).
int check_chain_present_and_ordered()
{
    int last = -1;
    for (int i = 0; i < kChain; ++i)
    {
        int at = -1;
        for (int f = 0; f < g_nframes; ++f)
        {
            if (g_frames[f] == g_ret[i])
            {
                at = f;
                break;
            }
        }
        INFO("chain frame " << i << " return address absent from the captured backtrace (unwind broke)");
        REQUIRE(at >= 0);
        INFO("chain frame " << i << " out of order at index " << at << " (prev " << last << ")");
        REQUIRE(at > last);
        last = at;
    }
    return last;
}

// --- fiber driver: run the same chain on a crd fiber stack, then switch back ------------------------------------
FiberContext g_fiber_ctx;
FiberContext g_caller_ctx;
bool         g_fiber_ran = false;

// A generous, page-aligned fiber stack owned by this TU (the raw primitive takes a plain memory region).
alignas(64) crd::u8 g_fiber_stack[256U * 1024U];

void fiber_entry()
{
    chain_5();          // runs the recorded chain + capture ON THE FIBER STACK
    g_fiber_ran = true;
    fiber_switch(&g_fiber_ctx, &g_caller_ctx); // return to the test; a fiber must never fall off its entry
}

// A THREAD-stack marker: the return address recorded here lives in this function's caller (the test body), on the
// ordinary thread stack. If the fiber walk had crossed the hand-rolled fiber_switch it would contain this address;
// proving its ABSENCE is what makes "the walk cannot cross the switch" observed rather than merely inferred.
void* g_thread_marker = nullptr;

CRD_NOINLINE void drive_fiber_and_switch()
{
    g_thread_marker = CRD_RETADDR();
    fiber_switch(&g_caller_ctx, &g_fiber_ctx); // resume the fiber; returns when fiber_entry switches back
}
} // namespace

TEST_CASE("optimized native frames unwind correctly (table-driven, no frame pointers)", "[jobs][diag][unwind]")
{
    // Runs on the ordinary thread stack, atop the full test + Catch2 + CRT frames -> a DEEP capture.
    g_nframes = 0;
    crd::u64 top = chain_5();
    CHECK(top != 0); // the chain actually executed (defeats a fully-elided no-op)

    REQUIRE(g_nframes > kChain); // at least the chain plus the frames that called into it
    check_chain_present_and_ordered();
}

TEST_CASE("crd fiber stacks unwind their own frames and truncate at the switch boundary", "[jobs][diag][unwind][fiber]")
{
    // Baseline: the same chain on the thread stack -> deep.
    g_nframes = 0;
    (void) chain_5();
    check_chain_present_and_ordered();
    const int thread_frames = g_nframes;

    // Now drive one fiber by hand with the public low-level primitives (no scheduler needed). drive_fiber_and_switch
    // records a thread-stack marker, then switches; the fiber runs the chain, captures, and switches back.
    g_fiber_ran     = false;
    g_thread_marker = nullptr;
    g_nframes       = 0;
    fiber_init_stack(g_fiber_ctx, g_fiber_stack, sizeof(g_fiber_stack), &fiber_entry);
    drive_fiber_and_switch();
    REQUIRE(g_fiber_ran);
    REQUIRE(g_thread_marker != nullptr);

    // The fiber's OWN chain frames unwound correctly...
    check_chain_present_and_ordered();
    const int fiber_frames = g_nframes;
    WARN("fiber capture depth = " << fiber_frames << " (thread capture depth = " << thread_frames << ")");

    // ...and the walk did NOT cross the hand-rolled fiber_switch onto the thread stack: the thread-side marker
    // recorded by drive_fiber_and_switch is absent from the fiber capture. This is the direct, observed proof of
    // the truncation the acceptance requires be marked -- not inferred from depth alone.
    bool crossed = false;
    for (int f = 0; f < g_nframes; ++f)
    {
        if (g_frames[f] == g_thread_marker)
        {
            crossed = true;
            break;
        }
    }
    INFO("the fiber walk crossed the switch: a thread-stack return address appeared in the fiber capture");
    REQUIRE(!crossed);

    // Consistent with that: the fiber capture is strictly shallower than the same chain on a full thread stack, and
    // bounded near the chain depth (no runaway into garbage past the burned trampoline). This truncation at the
    // fiber-switch boundary is the qualified, documented limitation.
    INFO("fiber capture (" << fiber_frames << ") should be shallower than the thread capture (" << thread_frames << ")");
    REQUIRE(fiber_frames < thread_frames);
    INFO("fiber capture should be bounded near the chain depth (no garbage past the trampoline)");
    REQUIRE(fiber_frames <= kChain + 6);
}
