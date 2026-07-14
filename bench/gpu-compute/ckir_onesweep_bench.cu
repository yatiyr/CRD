// ckir_onesweep_bench.cu — THE CUDA-BACKEND CAMPAIGN: the CKIR onesweep radix sort (same IR that runs bit-exact on the CPU
// oracle + Vulkan) lowered to native CUDA (hardware __match_any_sync rank, stream-ordered launches) vs CUB DeviceRadixSort
// in the SAME binary, same method, same device. Kernels are GENERATED (ckir_onesweep_gen.cu, tool test [.emit-cuda-sort]).
// Build: nvcc -O3 -std=c++17 -arch=sm_89 -allow-unsupported-compiler -Xcompiler /Zc:preprocessor
//        -I "%CUDA_PATH%\include\cccl" ckir_onesweep_bench.cu -o ckir_onesweep_bench.exe

#include <cstdio> // device printf in the traced gen kernels
#include "ckir_onesweep_gen.cu"

#include <cuda_runtime.h>
#include <cub/device/device_radix_sort.cuh>

#include <chrono>
#include <thread>
#include <cstdio>
#include <cstdlib>

static void check(cudaError_t e, const char* what)
{
    if (e != cudaSuccess) { std::printf("CUDA ERROR %s: %s\n", what, cudaGetErrorString(e)); std::exit(1); }
}

int main()
{
    constexpr int n         = 1 << 24;
    constexpr int aux_words = 4 * 256 + 4 + 4 * 8192 * 256; // [ghist | 4 tickets | look], config-A sized (max)
    cudaDeviceProp prop{};
    cudaGetDeviceProperties(&prop, 0);
    std::printf("# CKIR ONESWEEP (CUDA backend, __match_any_sync) vs CUB DeviceRadixSort — %s\n", prop.name);

    unsigned* d_a  = nullptr;
    unsigned* d_b  = nullptr;
    unsigned* d_gb = nullptr;
    unsigned* d_ax = nullptr;
    check(cudaMalloc(&d_a, sizeof(unsigned) * n), "d_a");
    check(cudaMalloc(&d_b, sizeof(unsigned) * n), "d_b");
    check(cudaMalloc(&d_gb, sizeof(unsigned) * 4 * 256), "d_gb");
    check(cudaMalloc(&d_ax, sizeof(unsigned) * aux_words), "d_ax");
    unsigned* h = static_cast<unsigned*>(malloc(sizeof(unsigned) * n));
    unsigned  ixor = 0U;
    for (int i = 0; i < n; ++i)
    {
        h[i] = (static_cast<unsigned>(i) * 2654435761U) ^ (static_cast<unsigned>(i) << 11);
        ixor ^= h[i];
    }
    check(cudaMemcpy(d_a, h, sizeof(unsigned) * n, cudaMemcpyHostToDevice), "upload");

    // ── config A: epb 2048 (nblocks 8192) ─────────────────────────────────────────────────────────────────────────────
    const auto sort_a = [&]() {
        k_clear<<<(aux_words + 2047) / 2048, 256>>>(d_ax);
        k_ghist_a<<<8192, 256>>>(d_a, d_ax);
        k_gbase<<<4, 256>>>(d_ax, d_gb);
        k_sa0<<<8192, 256>>>(d_a, d_b, d_gb, d_ax);
        k_sa1<<<8192, 256>>>(d_b, d_a, d_gb, d_ax);
        k_sa2<<<8192, 256>>>(d_a, d_b, d_gb, d_ax);
        k_sa3<<<8192, 256>>>(d_b, d_a, d_gb, d_ax);
    };
    // ── config B: epb 4096 (nblocks 4096) ─────────────────────────────────────────────────────────────────────────────
    const auto sort_b = [&]() {
        k_clear<<<2049, 256>>>(d_ax); // B needs only [0, 4195332) — half of A's aux
        k_ghist_b<<<4096, 256>>>(d_a, d_ax);
        k_gbase<<<4, 256>>>(d_ax, d_gb);
        k_sb0<<<4096, 256>>>(d_a, d_b, d_gb, d_ax);
        k_sb1<<<4096, 256>>>(d_b, d_a, d_gb, d_ax);
        k_sb2<<<4096, 256>>>(d_a, d_b, d_gb, d_ax);
        k_sb3<<<4096, 256>>>(d_b, d_a, d_gb, d_ax);
    };

    const auto verify = [&](const char* nm) {
        check(cudaMemcpy(h, d_a, sizeof(unsigned) * n, cudaMemcpyDeviceToHost), "readback");
        int      bad = 0;
        unsigned sx  = 0U;
        for (int i = 0; i < n; ++i) { if (i > 0 && h[i - 1] > h[i]) { ++bad; } sx ^= h[i]; }
        std::printf("# %s: sorted=%d permutation=%d\n", nm, bad == 0 ? 1 : 0, sx == ixor ? 1 : 0);
        if (bad != 0 || sx != ixor) { std::exit(2); }
    };
    const auto bench = [&](const char* nm, const auto& fn) {
        for (int w = 0; w < 3; ++w) { fn(); }
        check(cudaDeviceSynchronize(), "warm");
        const int iters = 50;
        double    best  = 1e30;
        for (int r = 0; r < 10; ++r)
        {
            const auto t0 = std::chrono::high_resolution_clock::now();
            for (int it = 0; it < iters; ++it) { fn(); }
            check(cudaDeviceSynchronize(), "sync");
            const auto   t1 = std::chrono::high_resolution_clock::now();
            const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count() / iters;
            if (ms < best) { best = ms; }
        }
        std::printf("%-22s %.5f ms   %.1f Mkeys/s\n", nm, best, n / (best * 1.0e3));
    };

    setvbuf(stdout, nullptr, _IONBF, 0);
    sort_a();
    check(cudaDeviceSynchronize(), "A first");
    verify("ONESWEEP-A (2048)");
    bench("CKIR-ONESWEEP-A", sort_a);

    for (int i = 0; i < n; ++i) { h[i] = (static_cast<unsigned>(i) * 2654435761U) ^ (static_cast<unsigned>(i) << 11); }
    check(cudaMemcpy(d_a, h, sizeof(unsigned) * n, cudaMemcpyHostToDevice), "reupload");
    sort_b();
    check(cudaDeviceSynchronize(), "B first");
    verify("ONESWEEP-B (4096)");
    bench("CKIR-ONESWEEP-B", sort_b);

    // config C: epb 8192 (nblocks 2048) -- CUDA 48KB static shared, 128B write runs
    const auto sort_c = [&]() {
        k_clear<<<1025, 256>>>(d_ax); // C needs only [0, 2098180)
        k_ghist_c<<<2048, 256>>>(d_a, d_ax);
        k_gbase<<<4, 256>>>(d_ax, d_gb);
        k_sc0<<<2048, 256>>>(d_a, d_b, d_gb, d_ax);
        k_sc1<<<2048, 256>>>(d_b, d_a, d_gb, d_ax);
        k_sc2<<<2048, 256>>>(d_a, d_b, d_gb, d_ax);
        k_sc3<<<2048, 256>>>(d_b, d_a, d_gb, d_ax);
    };
    for (int i = 0; i < n; ++i) { h[i] = (static_cast<unsigned>(i) * 2654435761U) ^ (static_cast<unsigned>(i) << 11); }
    check(cudaMemcpy(d_a, h, sizeof(unsigned) * n, cudaMemcpyHostToDevice), "c upload");
    sort_c();
    check(cudaDeviceSynchronize(), "C first");
    verify("ONESWEEP-C (8192)");
    bench("CKIR-ONESWEEP-C", sort_c);

    { // per-kernel standalone profile (config B) -- aim the remaining tuning precisely
        const auto solo = [&](const char* nm, const auto& fn) {
            for (int w = 0; w < 3; ++w) { fn(); }
            cudaDeviceSynchronize();
            double best = 1e30;
            for (int r = 0; r < 5; ++r)
            {
                const auto t0 = std::chrono::high_resolution_clock::now();
                for (int it = 0; it < 100; ++it) { fn(); }
                cudaDeviceSynchronize();
                const auto   t1 = std::chrono::high_resolution_clock::now();
                const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count() / 100.0;
                if (ms < best) { best = ms; }
            }
            std::printf("# kprof %-10s %.5f ms\n", nm, best);
        };
        solo("clear_b", [&]() { k_clear<<<2049, 256>>>(d_ax); });
        solo("ghist_b", [&]() { k_ghist_b<<<4096, 256>>>(d_a, d_ax); });
        solo("gbase", [&]() { k_gbase<<<4, 256>>>(d_ax, d_gb); });
        // scatter standalone: fresh clear+prep per iteration would skew; time [clear+scatter] and subtract clear
        solo("clr+sb0", [&]() { k_clear<<<2049, 256>>>(d_ax); k_sb0<<<4096, 256>>>(d_a, d_b, d_gb, d_ax); });
    }

    // ── CUB gold, same binary, same method ────────────────────────────────────────────────────────────────────────────
    for (int i = 0; i < n; ++i) { h[i] = (static_cast<unsigned>(i) * 2654435761U) ^ (static_cast<unsigned>(i) << 11); }
    check(cudaMemcpy(d_a, h, sizeof(unsigned) * n, cudaMemcpyHostToDevice), "cub upload");
    void*  d_tmp = nullptr;
    size_t tmpb  = 0;
    cub::DeviceRadixSort::SortKeys(d_tmp, tmpb, d_a, d_b, n);
    check(cudaMalloc(&d_tmp, tmpb), "cub tmp");
    const auto cub_fn = [&]() { cub::DeviceRadixSort::SortKeys(d_tmp, tmpb, d_a, d_b, n); };
    cub_fn();
    check(cudaDeviceSynchronize(), "cub first");
    unsigned first = 1U, last = 0U;
    cudaMemcpy(&first, d_b, 4, cudaMemcpyDeviceToHost);
    cudaMemcpy(&last, d_b + (n - 1), 4, cudaMemcpyDeviceToHost);
    std::printf("# CUB: first=%u last=%u ok=%d\n", first, last, first <= last ? 1 : 0);
    {
        const int iters = 50;
        double    best  = 1e30;
        for (int r = 0; r < 10; ++r)
        {
            const auto t0 = std::chrono::high_resolution_clock::now();
            for (int it = 0; it < iters; ++it) { cub_fn(); }
            check(cudaDeviceSynchronize(), "cub sync");
            const auto   t1 = std::chrono::high_resolution_clock::now();
            const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count() / iters;
            if (ms < best) { best = ms; }
        }
        std::printf("%-22s %.5f ms   %.1f Mkeys/s\n", "CUB DeviceRadixSort", best, n / (best * 1.0e3));
    }
    free(h);
    return 0;
}
