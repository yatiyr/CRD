// v14-l tensor I/O wall-clock board: OUR npy + safetensors read/write vs
// python numpy / safetensors (scripts/v14l_io_oracle.py bench) on a ~512 MB
// f32 corpus. Same protocol both sides: write-to-file / read-from-file
// through the OS page cache (files in /tmp), best of 5, GB/s over PAYLOAD
// bytes. Boards -> docs/bench/2026-07-05-v14l-io.md. Harness:
// build/crd_io_bench.sh (pinned, 1T).
#include <crd/hesap/tensor/io.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <chrono>
#include <cstdio>

using crd::hesap::tensor::IoDtype;
using crd::hesap::tensor::Tensor;
using crd::hesap::tensor::TensorStatus;
using crd::hesap::tensor::TensorView;

namespace
{

double now_ms()
{
    return static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   std::chrono::steady_clock::now().time_since_epoch())
                                   .count()) *
           1e-6;
}

template <typename Fn>
double best_of(int reps, Fn&& fn)
{
    double best = 1e300;
    for (int r = 0; r < reps; ++r)
    {
        const double t0 = now_ms();
        fn();
        const double t1 = now_ms();
        if (t1 - t0 < best)
        {
            best = t1 - t0;
        }
    }
    return best;
}

void require_ok(TensorStatus st, const char* what)
{
    if (st != TensorStatus::Ok)
    {
        std::printf("FATAL: %s failed (status %d)\n", what, static_cast<int>(st));
        std::exit(1);
    }
}

} // namespace

int main()
{
    constexpr crd::u64 n = 128ULL * 1024ULL * 1024ULL; // 128 Mi f32 = 512 MB payload
    const double gb = static_cast<double>(n * sizeof(crd::f32)) / 1e9;
    crd::memory::TlsfAllocator alloc(3ULL << 30); // 3 GB pool: tensor + encode buffer + read-back

    const crd::u64 shp[1] = {n};
    Tensor<crd::f32> t(&alloc, {shp, 1});
    require_ok(crd::hesap::tensor::philox_fill_uniform(t.view(), 99U, 0U, 1U), "philox_fill");

    const crd::containers::StringView npy_path = "/tmp/crd_v14l_bench/ours.npy";
    const crd::containers::StringView st_path = "/tmp/crd_v14l_bench/ours.safetensors";

    // ---- npy ----
    {
        const double w = best_of(5, [&] {
            require_ok(crd::hesap::tensor::npy_write_file<crd::f32>(&alloc, npy_path,
                                                                    TensorView<const crd::f32>(t.view())),
                       "npy_write_file");
        });
        const double r = best_of(5, [&] {
            Tensor<crd::f32> back(&alloc);
            require_ok(crd::hesap::tensor::npy_read_file<crd::f32>(&alloc, npy_path, back), "npy_read_file");
        });
        // correctness anchor: bit-exact read-back
        Tensor<crd::f32> back(&alloc);
        require_ok(crd::hesap::tensor::npy_read_file<crd::f32>(&alloc, npy_path, back), "npy verify");
        if (std::memcmp(back.data(), t.data(), n * sizeof(crd::f32)) != 0)
        {
            std::printf("FATAL: npy read-back mismatch\n");
            return 1;
        }
        std::printf("crd     npy  write %.3fs (%.2f GB/s)  read %.3fs (%.2f GB/s)\n", w * 1e-3,
                    gb / (w * 1e-3), r * 1e-3, gb / (r * 1e-3));
    }

    // ---- safetensors ----
    {
        const double w = best_of(5, [&] {
            crd::hesap::tensor::SafetensorsWriter wr(&alloc);
            require_ok(wr.add<crd::f32>("w", TensorView<const crd::f32>(t.view())), "st add");
            require_ok(wr.finish_file(st_path), "st finish_file");
        });
        const double r = best_of(5, [&] {
            Tensor<crd::f32> back(&alloc);
            require_ok(crd::hesap::tensor::safetensors_read_tensor_file<crd::f32>(&alloc, st_path, "w", back),
                       "st read");
        });
        // correctness anchor
        Tensor<crd::f32> back(&alloc);
        require_ok(crd::hesap::tensor::safetensors_read_tensor_file<crd::f32>(&alloc, st_path, "w", back),
                   "st verify");
        if (std::memcmp(back.data(), t.data(), n * sizeof(crd::f32)) != 0)
        {
            std::printf("FATAL: safetensors read-back mismatch\n");
            return 1;
        }
        std::printf("crd safetensors write %.3fs (%.2f GB/s)  read %.3fs (%.2f GB/s)\n", w * 1e-3,
                    gb / (w * 1e-3), r * 1e-3, gb / (r * 1e-3));
    }
    std::printf("payload bytes: %llu\n", static_cast<unsigned long long>(n * sizeof(crd::f32)));
    return 0;
}
