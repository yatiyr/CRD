// The AddressSanitizer use-after-return cost control (SIMD-DISPATCH;
// docs/design/test-instruments.md#host-separation-diag10a). No Cerid code: a loop of call trees whose every frame
// holds an addressable local array, the frames ASan moves to its fake stack while detect_stack_use_after_return is
// on. The dispatched host check's ARM64 sanitizer-cost legs build it at -O0 with each ARM64 compiler and time it with
// the option on and off:
//     g++ -std=c++20 -O0 -fsanitize=address -fno-omit-frame-pointer asan_uar_control.cpp
// Arguments: the number of call trees (default 100000) and each tree's depth (default 16).
#include <chrono>
#include <cstdio>
#include <cstdlib>

namespace
{
unsigned char* volatile g_escape = nullptr;

[[gnu::noinline]] unsigned frame(unsigned seed, unsigned depth)
{
    unsigned char local[64];
    for (unsigned i = 0; i < sizeof(local); ++i)
    {
        local[i] = static_cast<unsigned char>(seed + i);
    }
    g_escape = local; // never read after the return: it only makes the frame addressable
    const unsigned inner = depth > 1 ? frame(seed * 33U + 1U, depth - 1) : 0U;
    return inner + local[seed % sizeof(local)];
}
} // namespace

int main(int argc, char** argv)
{
    const unsigned long trees = argc > 1 ? std::strtoul(argv[1], nullptr, 10) : 100000UL;
    const unsigned depth = argc > 2 ? static_cast<unsigned>(std::strtoul(argv[2], nullptr, 10)) : 16U;
    const auto start = std::chrono::steady_clock::now();
    unsigned sum = 0;
    for (unsigned long i = 0; i < trees; ++i)
    {
        sum += frame(static_cast<unsigned>(i), depth);
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    const double calls = static_cast<double>(trees) * depth;
    std::printf("%lu trees of depth %u: %.3f s, %.1f ns a call (sum %u)\n", trees, depth, seconds,
                seconds * 1e9 / calls, sum);
    return 0;
}
