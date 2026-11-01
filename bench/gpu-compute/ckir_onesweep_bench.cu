// ckir_onesweep_bench.cu — the CUDA-backend sort duel: the CKIR onesweep radix sort (the IR that runs bit-exact on the
// CPU oracle and on Vulkan) lowered to native CUDA (hardware __match_any_sync rank, stream-ordered launches) against
// CUB's DeviceRadixSort in the same binary, by the same method, on the same device. The kernels are emitted by CKIR
// into ckir_onesweep_gen.cu (test_ckir_subgroup.cpp, the hidden [.emit-cuda-sort] case) in three configurations:
// A (2048 elements per block), B (4096) and C (8192, 41 KiB of static shared memory and 128-byte write runs). Built by
// CRD_BUILD_GPU_VS_CUDA (bench/CMakeLists.txt) and run by scripts/run_bench_gpu_cuda.sh; a failed CUDA call, or a sort
// from either side whose output is not the sorted permutation of its input, fails the run.

#include "ckir_onesweep_gen.cu" // k_clear, k_ghist_{a,b,c}, k_gbase and k_s{a,b,c}{0..3}
#include "cuda_bench.hpp"

#include <cub/device/device_radix_sort.cuh>
#include <cuda_runtime.h>

#include <cstdio>
#include <memory>

namespace
{
constexpr int kKeys = 1 << 24;
// [global histograms (4 x 256) | 4 tickets | per-block look-back (4 x blocks x 256)], sized for configuration A
constexpr int kAuxWords = 4 * 256 + 4 + 4 * 8192 * 256;

unsigned key(int i)
{
    return (static_cast<unsigned>(i) * 2654435761U) ^ (static_cast<unsigned>(i) << 11);
}

// The keys sorted and a permutation of the input (their XOR, a check that holds for any reordering, unchanged).
bool sorted_permutation(const char* what, const unsigned* device, unsigned input_xor)
{
    const auto host = std::make_unique<unsigned[]>(kKeys);
    CRD_CUDA_CHECK(cudaMemcpy(host.get(), device, sizeof(unsigned) * kKeys, cudaMemcpyDeviceToHost));
    int      inversions = 0;
    unsigned output_xor = 0U;
    for (int i = 0; i < kKeys; ++i)
    {
        if (i > 0 && host[i - 1] > host[i])
        {
            ++inversions;
        }
        output_xor ^= host[i];
    }
    const bool permutation = output_xor == input_xor;
    std::printf("# %s: sorted=%d permutation=%d\n", what, inversions == 0 ? 1 : 0, permutation ? 1 : 0);
    if (inversions != 0 || !permutation)
    {
        std::fprintf(stderr, "%s: %d inversions, %s\n", what, inversions,
                     permutation ? "a permutation of the input" : "not a permutation of the input");
        return false;
    }
    return true;
}

// Ten runs of `calls` back-to-back sorts after three warm-up calls; the least host time per sort.
template <typename Body>
double time_sort(const char* name, int calls, Body&& body)
{
    for (int call = 0; call < 3; ++call)
    {
        body();
    }
    const double best = crd_bench::best_wall_ms(10, calls, body);
    std::printf("%-22s %.5f ms   %.1f Mkeys/s\n", name, best, kKeys / (best * 1.0e3));
    return best;
}

// One kernel's standalone time (config B): the least per call over 5 runs of 100 after three warm-up calls.
template <typename Body>
void time_kernel(const char* name, Body&& body)
{
    for (int call = 0; call < 3; ++call)
    {
        body();
    }
    std::printf("# kprof %-10s %.5f ms\n", name, crd_bench::best_wall_ms(5, 100, body));
}
} // namespace

int main()
{
    cudaDeviceProp prop{};
    CRD_CUDA_CHECK(cudaGetDeviceProperties(&prop, 0));
    std::printf("# CKIR ONESWEEP (CUDA backend, __match_any_sync) vs CUB DeviceRadixSort — %s\n", prop.name);

    const auto d_a  = crd_bench::device_array<unsigned>(kKeys);
    const auto d_b  = crd_bench::device_array<unsigned>(kKeys);
    const auto d_gb = crd_bench::device_array<unsigned>(4 * 256);
    const auto d_ax = crd_bench::device_array<unsigned>(kAuxWords);
    const auto keys = std::make_unique<unsigned[]>(kKeys);
    unsigned   input_xor = 0U;
    for (int i = 0; i < kKeys; ++i)
    {
        keys[i] = key(i);
        input_xor ^= keys[i];
    }
    const auto upload_keys = [&]
    {
        CRD_CUDA_CHECK(cudaMemcpy(d_a.get(), keys.get(), sizeof(unsigned) * kKeys, cudaMemcpyHostToDevice));
    };
    unsigned* const a  = d_a.get();
    unsigned* const b  = d_b.get();
    unsigned* const gb = d_gb.get();
    unsigned* const ax = d_ax.get();

    // Each configuration clears the auxiliary words it uses, builds the global histograms and their bases, then runs
    // four 8-bit scatter passes that ping-pong between a and b (the sorted keys end in a).
    const auto sort_a = [&]
    {
        k_clear<<<(kAuxWords + 2047) / 2048, 256>>>(ax);
        k_ghist_a<<<8192, 256>>>(a, ax);
        k_gbase<<<4, 256>>>(ax, gb);
        k_sa0<<<8192, 256>>>(a, b, gb, ax);
        k_sa1<<<8192, 256>>>(b, a, gb, ax);
        k_sa2<<<8192, 256>>>(a, b, gb, ax);
        k_sa3<<<8192, 256>>>(b, a, gb, ax);
        CRD_CUDA_CHECK(cudaGetLastError());
    };
    const auto sort_b = [&]
    {
        k_clear<<<2049, 256>>>(ax); // B uses [0, 4195332), half of A's words
        k_ghist_b<<<4096, 256>>>(a, ax);
        k_gbase<<<4, 256>>>(ax, gb);
        k_sb0<<<4096, 256>>>(a, b, gb, ax);
        k_sb1<<<4096, 256>>>(b, a, gb, ax);
        k_sb2<<<4096, 256>>>(a, b, gb, ax);
        k_sb3<<<4096, 256>>>(b, a, gb, ax);
        CRD_CUDA_CHECK(cudaGetLastError());
    };
    const auto sort_c = [&]
    {
        k_clear<<<1025, 256>>>(ax); // C uses [0, 2098180)
        k_ghist_c<<<2048, 256>>>(a, ax);
        k_gbase<<<4, 256>>>(ax, gb);
        k_sc0<<<2048, 256>>>(a, b, gb, ax);
        k_sc1<<<2048, 256>>>(b, a, gb, ax);
        k_sc2<<<2048, 256>>>(a, b, gb, ax);
        k_sc3<<<2048, 256>>>(b, a, gb, ax);
        CRD_CUDA_CHECK(cudaGetLastError());
    };

    upload_keys();
    sort_a();
    if (!sorted_permutation("ONESWEEP-A (2048)", a, input_xor))
    {
        return 1;
    }
    time_sort("CKIR-ONESWEEP-A", 50, sort_a);

    upload_keys();
    sort_b();
    if (!sorted_permutation("ONESWEEP-B (4096)", a, input_xor))
    {
        return 1;
    }
    time_sort("CKIR-ONESWEEP-B", 50, sort_b);

    upload_keys();
    sort_c();
    if (!sorted_permutation("ONESWEEP-C (8192)", a, input_xor))
    {
        return 1;
    }
    time_sort("CKIR-ONESWEEP-C", 50, sort_c);

    // configuration B's kernels on their own, to aim the tuning; a scatter needs a fresh clear, so it is timed with one
    time_kernel("clear_b", [&]
    {
        k_clear<<<2049, 256>>>(ax);
        CRD_CUDA_CHECK(cudaGetLastError());
    });
    time_kernel("ghist_b", [&]
    {
        k_ghist_b<<<4096, 256>>>(a, ax);
        CRD_CUDA_CHECK(cudaGetLastError());
    });
    time_kernel("gbase", [&]
    {
        k_gbase<<<4, 256>>>(ax, gb);
        CRD_CUDA_CHECK(cudaGetLastError());
    });
    // ghist_b's repeated calls added into histograms nothing cleared, so gbase's bases now point past the keys: build
    // configuration B's histograms and bases once from a cleared state before a scatter reads them
    k_clear<<<2049, 256>>>(ax);
    k_ghist_b<<<4096, 256>>>(a, ax);
    k_gbase<<<4, 256>>>(ax, gb);
    CRD_CUDA_CHECK(cudaGetLastError());
    CRD_CUDA_CHECK(cudaDeviceSynchronize());
    time_kernel("clr+sb0", [&]
    {
        k_clear<<<2049, 256>>>(ax);
        k_sb0<<<4096, 256>>>(a, b, gb, ax);
        CRD_CUDA_CHECK(cudaGetLastError());
    });

    // CUB, the vendor's sort: same keys, same method; its output (in b) judged as the CKIR sorts' are
    upload_keys();
    size_t temp_bytes = 0;
    CRD_CUDA_CHECK(cub::DeviceRadixSort::SortKeys(nullptr, temp_bytes, a, b, kKeys));
    const auto d_temp   = crd_bench::device_array<unsigned char>(temp_bytes);
    const auto cub_sort = [&]
    {
        CRD_CUDA_CHECK(cub::DeviceRadixSort::SortKeys(d_temp.get(), temp_bytes, a, b, kKeys));
    };
    cub_sort();
    if (!sorted_permutation("CUB", b, input_xor))
    {
        return 1;
    }
    time_sort("CUB DeviceRadixSort", 50, cub_sort);
    return 0;
}
