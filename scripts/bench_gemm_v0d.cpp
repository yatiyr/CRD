// v0d GEMM crush pass — standalone f64 gemm across sizes (1T), GF/s.
#include <crd/hesap/dense/blas3.hpp>
#include <crd/hesap/dense/matrix.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>

using namespace crd::hesap::dense;

int main()
{
    crd::memory::TlsfAllocator alloc(1ULL << 31U);
    for (const crd::usize n : {256U, 384U, 512U, 768U, 1024U, 1536U, 2048U})
    {
        Matrix<crd::f64, Layout::RowMajor> a(&alloc, n, n);
        Matrix<crd::f64, Layout::RowMajor> b(&alloc, n, n);
        Matrix<crd::f64, Layout::RowMajor> c(&alloc, n, n);
        for (crd::usize i = 0; i < n * n; ++i)
        {
            a.data()[i] = static_cast<crd::f64>((i * 2654435761ULL) % 1000ULL) * 1e-3 - 0.5;
            b.data()[i] = static_cast<crd::f64>((i * 40503ULL) % 1000ULL) * 1e-3 - 0.5;
        }
        gemm<crd::f64, Layout::RowMajor>(1.0, a.cview(), b.cview(), 0.0, c.view(), Trans::None, Trans::None,
                                         &alloc);
        double times[7];
        const int reps = n <= 512U ? 5 : 1;
        for (int r = 0; r < 7; ++r)
        {
            const auto t0 = std::chrono::steady_clock::now();
            for (int q = 0; q < reps; ++q)
            {
                gemm<crd::f64, Layout::RowMajor>(1.0, a.cview(), b.cview(), 0.0, c.view(), Trans::None,
                                                 Trans::None, &alloc);
            }
            const auto t1 = std::chrono::steady_clock::now();
            times[r] = std::chrono::duration<double>(t1 - t0).count() / reps;
        }
        std::sort(times, times + 7);
        const double gf = 2.0 * double(n) * double(n) * double(n) / times[3] / 1e9;
        std::printf("n=%4zu  %8.3f ms  %6.2f GF/s\n", n, times[3] * 1e3, gf);
    }
    return 0;
}
