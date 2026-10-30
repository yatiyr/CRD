// The AddressSanitizer stack-alignment control (SIMD-DISPATCH; docs/third-party-defects.md#tp-7). No Cerid code: a
// frame that a 512-bit temporary realigns to 64 bytes and that holds an address-taken 64-byte array. GCC 12 and 13
// (with Ubuntu's default -fstack-protector-strong) zero the array with a store that needs 64-byte alignment, while the
// frame ASan moves to its fake stack (detect_stack_use_after_return=1, the runtime's default) puts the array 32 bytes
// off that boundary, so the store faults. Capping GCC's expansion width (-mstore-max=256 -mmove-max=256) keeps it to
// 32-byte stores; Clang assumes 32. The dispatched host check's x64 stack-alignment leg builds it at -O0 for an AVX-512
// target and runs it under Intel SDE:
//     g++ -std=c++20 -O0 -march=x86-64-v4 -fsanitize=address -fno-omit-frame-pointer asan_stack_align_control.cpp
//     sde64 -skx -- ./a.out
#include <cstdio>
#include <immintrin.h>

namespace
{
double* volatile g_escape = nullptr;

[[gnu::noinline]] void keep(double* p, __m256d v)
{
    _mm256_storeu_pd(p, v);
    g_escape = p; // never read after the return: it only makes the array addressable
}

[[gnu::noinline]] double frame(const double* in)
{
    double local[8] = {};
    const __m512d v = _mm512_loadu_pd(in);
    keep(local, _mm512_castpd512_pd256(v));
    return local[0] + local[7];
}
} // namespace

int main()
{
    const double in[8] = {1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0};
    double sum = 0.0;
    for (int i = 0; i < 100; ++i)
    {
        sum += frame(in);
    }
    std::printf("100 frames, sum %.1f\n", sum);
    return 0;
}
