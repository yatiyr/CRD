// v14-f einsum execution bench — vs numpy/torch (matched 1T; plan prebuilt).
#include <crd/hesap/tensor/einsum_exec.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>

using namespace crd::hesap::tensor;

namespace
{
struct Case
{
    const char* expr;
    const char* names;
    crd::u64 sizes[8];
    int reps;
};

void run_case(crd::memory::IAllocator* alloc, const Case& c)
{
    crd::u64 idx_size[kEinsumMaxIndices];
    for (crd::u32 i = 0; i < kEinsumMaxIndices; ++i)
    {
        idx_size[i] = 1;
    }
    crd::u32 nn = 0;
    for (const char* p = c.names; *p != '\0'; ++p, ++nn)
    {
        idx_size[static_cast<crd::u32>(*p - 'a')] = c.sizes[nn];
    }
    crd::u32 ranks[kEinsumMaxOperands];
    crd::u32 n_ops = 0;
    {
        crd::u32 r = 0;
        for (const char* p = c.expr; *p != '\0' && *p != '-'; ++p)
        {
            if (*p == ',')
            {
                ranks[n_ops++] = r;
                r = 0;
            }
            else
            {
                ++r;
            }
        }
        ranks[n_ops++] = r;
    }
    EinsumExpr e;
    (void)einsum_parse(c.expr, {ranks, n_ops}, e);
    EinsumPlan plan;
    (void)einsum_plan_build(e, idx_size, EinsumOptimize::Optimal, plan);

    Tensor<crd::f64> ops[kEinsumMaxOperands];
    TensorView<const crd::f64> views[kEinsumMaxOperands];
    for (crd::u32 t = 0; t < n_ops; ++t)
    {
        crd::u64 shape[kMaxRank];
        for (crd::u32 d = 0; d < e.term[t].count; ++d)
        {
            shape[d] = idx_size[e.term[t].idx[d]];
        }
        ops[t] = Tensor<crd::f64>(alloc, {shape, e.term[t].count});
        for (crd::u64 i = 0; i < ops[t].size(); ++i)
        {
            ops[t].data()[i] = static_cast<crd::f64>((i * 2654435761ULL) % 1000ULL) * 1e-3 - 0.5;
        }
        views[t] = ops[t].view();
    }
    Tensor<crd::f64> out(alloc);
    (void)einsum_execute<crd::f64>(plan, {views, n_ops}, out, alloc); // warm

    double times[10];
    for (int r = 0; r < 10; ++r)
    {
        const auto t0 = std::chrono::steady_clock::now();
        for (int k = 0; k < c.reps; ++k)
        {
            (void)einsum_execute<crd::f64>(plan, {views, n_ops}, out, alloc);
        }
        const auto t1 = std::chrono::steady_clock::now();
        times[r] = std::chrono::duration<double, std::micro>(t1 - t0).count() / c.reps;
    }
    std::sort(times, times + 10);
    std::printf("%-24s %10.2f us\n", c.expr, times[4]);
}
} // namespace

int main()
{
    crd::memory::TlsfAllocator alloc(1ULL << 30U);
    const Case cases[] = {
        {"ab,bc,cd->ad", "abcd", {512, 512, 512, 512}, 1},
        {"ea,fb,abcd,gc,hd->efgh", "abcdefgh", {24, 24, 24, 24, 24, 24, 24, 24}, 1},
        {"ij,jk->ik", "ijk", {32, 32, 32}, 200},
        {"abc,bad->dc", "abcd", {96, 96, 96, 96}, 5},
    };
    for (const Case& c : cases)
    {
        run_case(&alloc, c);
    }
    return 0;
}
