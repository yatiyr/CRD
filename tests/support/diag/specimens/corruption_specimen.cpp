// DIAG.9c: the small intentional corruption that every reverse-debugging and watchpoint route must find.
//
// One global block of five 64-bit words: four cells and, right after them, a sentinel. `crd_diag_corrupt_neighbour`
// adds to every cell up to an inclusive last index. In `corrupt` mode main passes the cell count as that index, an
// off-by-one, so the loop also writes the sentinel; in `clean` mode it passes the true last index and the sentinel
// keeps its value. After the write, `crd_diag_corruption_churn` keeps changing the cells for a bounded number of steps
// (so a recording whose instruction log is too small loses the corrupting write), then main checks the sentinel.
//
// scripts/reverse-debug-recipes.py drives this specimen under gdb's process record (live reverse execution and an
// offline replay of the saved log), gdb and cdb hardware watchpoints, rr and Windows TTD. A route passes only if it
// names `crd_diag_corrupt_neighbour` at the marked line below as the writer of the sentinel, and only if the
// specimen's own uninstrumented run detected the same corruption. In `clean` mode a route that names any writer is
// an instrument failure.
//
// Debugger contract (the script and the docs name these symbols, so keep them stable):
// - `crd_diag_corruption_block`, the watched object; the sentinel is word 4, byte offset 32.
// - `crd_diag_corruption_window_begin` and `crd_diag_corruption_window_end` bound the recorded window. No library
//   call runs between them: gdb's process record cannot step through glibc's AVX string routines.
// - `crd_diag_corruption_detected` runs only when the check finds the sentinel changed.
// - The lines marked CRD_DIAG_WRITER_BEGIN, CRD_DIAG_CORRUPTING_WRITE and CRD_DIAG_WRITER_END locate the writer.
//
// Exit codes: 3 corruption detected, 0 clean, 64 bad arguments. Standalone (links nothing), single-threaded: rr and
// gdb's record serialize execution, so real multicore races stay with the TSan and stress specimens.

#include "specimen_common.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

#if defined(_MSC_VER)
#define CRD_CORRUPTION_NOINLINE __declspec(noinline) // NOLINT(cppcoreguidelines-macro-usage): attribute spelling.
#else
#define CRD_CORRUPTION_NOINLINE __attribute__((noinline)) // NOLINT(cppcoreguidelines-macro-usage): attribute.
#endif

struct CrdDiagCorruptionBlock
{
    std::uint64_t words[5]; // NOLINT(cppcoreguidelines-avoid-c-arrays): a fixed layout the debuggers address.
};

constexpr std::uint64_t kCrdDiagSentinel = 0x5EED5EEDC0FFEE11ULL;
constexpr std::uint64_t kCrdDiagCellCount = 4U;
constexpr std::uint64_t kCrdDiagChurnSteps = 2000U;

// Named, external globals: the debuggers resolve them by name. Volatile so every access is a real load or store.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables): the watched object of the specimen.
volatile CrdDiagCorruptionBlock crd_diag_corruption_block = {{1U, 2U, 3U, 4U, kCrdDiagSentinel}};
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables): distinct phase stores keep the markers distinct.
volatile std::uint64_t crd_diag_corruption_phase = 0U;

// Each marker stores its own phase value so identical-code folding cannot merge two markers into one address.
CRD_CORRUPTION_NOINLINE void crd_diag_corruption_window_begin() noexcept
{
    crd_diag_corruption_phase = 1U;
    std::atomic_signal_fence(std::memory_order_seq_cst);
}

CRD_CORRUPTION_NOINLINE void crd_diag_corruption_detected() noexcept
{
    crd_diag_corruption_phase = 2U;
    std::atomic_signal_fence(std::memory_order_seq_cst);
}

CRD_CORRUPTION_NOINLINE void crd_diag_corruption_window_end() noexcept
{
    crd_diag_corruption_phase = 3U;
    std::atomic_signal_fence(std::memory_order_seq_cst);
}

// CRD_DIAG_WRITER_BEGIN
CRD_CORRUPTION_NOINLINE void crd_diag_corrupt_neighbour(volatile CrdDiagCorruptionBlock& block,
                                                        std::uint64_t last_index, std::uint64_t delta) noexcept
{
    for (std::uint64_t i = 0U; i <= last_index; ++i)
    {
        block.words[i] = block.words[i] + delta; // CRD_DIAG_CORRUPTING_WRITE
    }
}
// CRD_DIAG_WRITER_END

// Bounded work after the write that touches only the cells, never the sentinel.
CRD_CORRUPTION_NOINLINE void crd_diag_corruption_churn(volatile CrdDiagCorruptionBlock& block) noexcept
{
    for (std::uint64_t step = 0U; step < kCrdDiagChurnSteps; ++step)
    {
        const std::uint64_t cell = step % kCrdDiagCellCount;
        block.words[cell] = (block.words[cell] * 3U) ^ step;
    }
}

int main(int argc, char** argv)
{
    crd_diag_harden();
    crd_diag_announce();

    bool corrupt = false;
    if (argc == 2 && std::strcmp(argv[1], "corrupt") == 0)
    {
        corrupt = true;
    }
    else if (argc != 2 || std::strcmp(argv[1], "clean") != 0)
    {
        std::fprintf(stderr, "usage: %s corrupt|clean\n", argc > 0 ? argv[0] : "crd-diag-corruption-specimen");
        return 64;
    }

    // In corrupt mode the inclusive last index is the cell count: one word too far, which is the sentinel.
    const std::uint64_t last_index = corrupt ? kCrdDiagCellCount : kCrdDiagCellCount - 1U;

    crd_diag_corruption_window_begin();
    crd_diag_corrupt_neighbour(crd_diag_corruption_block, last_index, 7U);
    crd_diag_corruption_churn(crd_diag_corruption_block);
    const bool detected = crd_diag_corruption_block.words[kCrdDiagCellCount] != kCrdDiagSentinel;
    if (detected)
    {
        crd_diag_corruption_detected();
    }
    crd_diag_corruption_window_end();

    if (detected)
    {
        std::printf("CRD_DIAG_CORRUPTION=detected word=%llu\n", static_cast<unsigned long long>(kCrdDiagCellCount));
        std::fflush(stdout);
        return 3;
    }
    std::printf("CRD_DIAG_CORRUPTION=none\n");
    std::fflush(stdout);
    return 0;
}
