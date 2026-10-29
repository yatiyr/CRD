// v14-b elementwise bench (docs/bench harness) — vs numpy/torch single-thread.
#include <crd/hesap/tensor/elementwise.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <chrono>
#include <cstdio>

using namespace crd::hesap::tensor;

namespace
{
template <typename F> void bench(const char* name, crd::u64 elems, F fn)
{
    fn();
    const auto t0 = std::chrono::steady_clock::now();
    constexpr int kReps = 10;
    for (int r = 0; r < kReps; ++r)
    {
        fn();
    }
    const auto t1 = std::chrono::steady_clock::now();
    const double ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / (kReps * double(elems));
    std::printf("%-28s %8.4f ns/elem\n", name, ns);
}
} // namespace

int main()
{
    crd::memory::TlsfAllocator alloc(1ULL << 31U);

    // 1. contiguous 1M f32 add (memory-bound; parity with numpy is the ceiling)
    {
        const crd::u64 s[] = {1U << 20U};
        Tensor<crd::f32> a(&alloc, s);
        Tensor<crd::f32> b(&alloc, s);
        Tensor<crd::f32> r(&alloc, s);
        for (crd::u64 i = 0; i < a.size(); ++i)
        {
            a.data()[i] = 0.001F * static_cast<crd::f32>(i % 999U);
            b.data()[i] = 1.0F - a.data()[i];
        }
        bench("contig add f32 1M", a.size(),
              [&]
              {
                  (void)ew_binary(BinaryOp::Add, TensorView<const crd::f32>(a.view()),
                                  TensorView<const crd::f32>(b.view()), r.view());
              });
    }
    // 2. outer broadcast (4096,1)*(1,4096) f32 mul -> 16M
    {
        const crd::u64 sa[] = {4096U, 1U};
        const crd::u64 sb[] = {1U, 4096U};
        const crd::u64 so[] = {4096U, 4096U};
        Tensor<crd::f32> a(&alloc, sa);
        Tensor<crd::f32> b(&alloc, sb);
        Tensor<crd::f32> r(&alloc, so);
        for (crd::u64 i = 0; i < 4096U; ++i)
        {
            a.data()[i] = 0.5F + 0.0001F * static_cast<crd::f32>(i);
            b.data()[i] = 1.5F - 0.0001F * static_cast<crd::f32>(i);
        }
        bench("outer bcast mul f32 16M", r.size(),
              [&]
              {
                  (void)ew_binary(BinaryOp::Mul, TensorView<const crd::f32>(a.view()),
                                  TensorView<const crd::f32>(b.view()), r.view());
              });
    }
    // 3. row broadcast (2048,2048)+(2048,) f64
    {
        const crd::u64 sa[] = {2048U, 2048U};
        const crd::u64 sb[] = {2048U};
        Tensor<crd::f64> a(&alloc, sa);
        Tensor<crd::f64> b(&alloc, sb);
        Tensor<crd::f64> r(&alloc, sa);
        for (crd::u64 i = 0; i < a.size(); ++i)
        {
            a.data()[i] = 0.001 * static_cast<crd::f64>(i % 4999U);
        }
        for (crd::u64 i = 0; i < 2048U; ++i)
        {
            b.data()[i] = static_cast<crd::f64>(i);
        }
        bench("row bcast add f64 4M", a.size(),
              [&]
              {
                  (void)ew_binary(BinaryOp::Add, TensorView<const crd::f64>(a.view()),
                                  TensorView<const crd::f64>(b.view()), r.view());
              });
    }
    // 4. strided source: every-2nd-row slice of (4096,2048) f32, mul by scalar
    {
        const crd::u64 sa[] = {4096U, 2048U};
        Tensor<crd::f32> a(&alloc, sa);
        for (crd::u64 i = 0; i < a.size(); ++i)
        {
            a.data()[i] = 0.001F * static_cast<crd::f32>(i % 777U);
        }
        TensorView<const crd::f32> av = TensorView<const crd::f32>(a.view()).slice(0U, 0U, 4096U, 2U);
        Tensor<crd::f32> s(&alloc, {});
        s.data()[0] = 1.0009F;
        const crd::u64 so[] = {2048U, 2048U};
        Tensor<crd::f32> r(&alloc, so);
        bench("strided-row mul f32 4M", r.size(),
              [&] { (void)ew_binary(BinaryOp::Mul, av, TensorView<const crd::f32>(s.view()), r.view()); });
    }
    return 0;
}
