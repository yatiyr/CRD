// f32 forward FFT probe: Cerid (this build's default path) vs MKL f32, 1T.
// Build twice (with/without -DCRD_FFT_M16B_FUSED_BRIDGE_POC) to A/B the POC.
#include <crd/hesap/fft/fft.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <mkl_dfti.h>

using crd::usize;
using Cplx = crd::hesap::Complex<crd::f32>;

int main()
{
    crd::memory::GrowableTlsfAllocator alloc;
    for (const usize n : {usize{1024}, usize{2048}, usize{4096}, usize{8192}, usize{16384}, usize{32768},
                          usize{65536}, usize{131072}, usize{262144}, usize{524288}, usize{1048576}, usize{2097152},
                          usize{4194304}})
    {
        crd::hesap::fft::FftPlan<crd::f32> plan(&alloc, n);
        Cplx* x = static_cast<Cplx*>(alloc.allocate(n * sizeof(Cplx), 64));
        Cplx* x0 = static_cast<Cplx*>(alloc.allocate(n * sizeof(Cplx), 64));
        for (usize i = 0; i < n; ++i)
        {
            x0[i] = {static_cast<crd::f32>(std::sin(0.001 * static_cast<double>(i))),
                     static_cast<crd::f32>(std::cos(0.0017 * static_cast<double>(i)))};
        }
        // accuracy vs MKL
        DFTI_DESCRIPTOR_HANDLE h;
        DftiCreateDescriptor(&h, DFTI_SINGLE, DFTI_COMPLEX, 1, static_cast<MKL_LONG>(n));
        DftiCommitDescriptor(h);
        Cplx* mk = static_cast<Cplx*>(alloc.allocate(n * sizeof(Cplx), 64));
        for (usize i = 0; i < n; ++i)
        {
            mk[i] = x0[i];
            x[i] = x0[i];
        }
        DftiComputeForward(h, mk);
        plan.execute({x, n}, crd::hesap::fft::FftDirection::Forward);
        double maxrel = 0.0;
        double ref = 0.0;
        for (usize i = 0; i < n; ++i)
        {
            ref = std::max(ref, std::hypot(static_cast<double>(mk[i].re), static_cast<double>(mk[i].im)));
        }
        for (usize i = 0; i < n; ++i)
        {
            const double dr = static_cast<double>(x[i].re) - static_cast<double>(mk[i].re);
            const double di = static_cast<double>(x[i].im) - static_cast<double>(mk[i].im);
            maxrel = std::max(maxrel, std::hypot(dr, di) / ref);
        }
        // timing (best of reps, fresh input each rep; more reps at small n where a call is ~µs)
        const int reps = n <= usize{65536} ? 200 : 15;
        double best_c = 1e300;
        double best_m = 1e300;
        for (int r = 0; r < reps; ++r)
        {
            for (usize i = 0; i < n; ++i)
            {
                x[i] = x0[i];
            }
            auto t0 = std::chrono::steady_clock::now();
            plan.execute({x, n}, crd::hesap::fft::FftDirection::Forward);
            auto t1 = std::chrono::steady_clock::now();
            best_c = std::min(best_c, std::chrono::duration<double, std::milli>(t1 - t0).count());
            for (usize i = 0; i < n; ++i)
            {
                mk[i] = x0[i];
            }
            t0 = std::chrono::steady_clock::now();
            DftiComputeForward(h, mk);
            t1 = std::chrono::steady_clock::now();
            best_m = std::min(best_m, std::chrono::duration<double, std::milli>(t1 - t0).count());
        }
        DftiFreeDescriptor(&h);
        std::printf("n=%8zu  cerid %8.3f ms  mkl %8.3f ms  ratio %.2fx  maxrel %.1e\n", n, best_c, best_m,
                    best_m / best_c, maxrel);
        alloc.deallocate(x);
        alloc.deallocate(x0);
        alloc.deallocate(mk);
    }
    return 0;
}
