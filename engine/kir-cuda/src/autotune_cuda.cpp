// autotune_cuda.cpp — ADR-0098 §4 · AS-2b. The packaged AS-1 search (see autotune_cuda.hpp).
#include <crd/kir/cuda/autotune_cuda.hpp>

#include <crd/containers/array.hpp>
#include <crd/kir/ckir.hpp>
#include <crd/kir/ckir_autotune.hpp>

namespace crd::kir
{
namespace
{
// deterministic, NON-constant fills (constant A/B would make every output identical — a schedule bug would hide).
float av_at(int i, int kk) { return static_cast<float>((i * 7 + kk) % 13) * 0.01F - 0.06F; }
float bv_at(int kk, int j) { return static_cast<float>((kk * 5 + j) % 11) * 0.008F - 0.04F; }

// CHEAP correctness gate: recompute a SAMPLE of C[i,j] on the CPU (each a length-K dot) and compare to the GPU readback — catches
// a miscomputing schedule without a full host GEMM. FMA fast tier ⇒ relative tolerance, not bit-exact.
bool sampled_correct(const float* c, int m, int n, int k)
{
    float maxrel = 0.0F;
    for (int s = 0; s < 512; ++s)
    {
        const int i   = (s * 977) % m;
        const int j   = (s * 1471) % n;
        double    acc = 0.0;
        for (int kk = 0; kk < k; ++kk) { acc += static_cast<double>(av_at(i, kk)) * static_cast<double>(bv_at(kk, j)); }
        const float ref = static_cast<float>(acc);
        const float got = c[static_cast<crd::usize>(i) * n + j];
        const float rel = (got - ref) / (1.0F + (ref < 0.0F ? -ref : ref));
        const float ar  = rel < 0.0F ? -rel : rel;
        if (ar > maxrel) { maxrel = ar; }
    }
    return maxrel < 3e-3F;
}
} // namespace

AutotuneResult autotune_contract(KirBackendCuda& cu, int m, int n, int k, int top_k, bool measure_naive,
                                 crd::memory::IAllocator* a)
{
    AutotuneResult res;
    res.m = m;
    res.n = n;
    res.k = k;
    if (!cu.valid() || m <= 0 || n <= 0 || k <= 0) { return res; }

    KGraph    g(a);
    const int ain = g.input(make_shape({m, k}), DType::F32);
    const int bin = g.input(make_shape({k, n}), DType::F32);
    const int c   = g.contract(ain, bin);

    crd::containers::Array<float> av(a);
    crd::containers::Array<float> bv(a);
    crd::containers::Array<float> out(a);
    av.resize(static_cast<crd::usize>(m) * k);
    bv.resize(static_cast<crd::usize>(k) * n);
    out.resize(static_cast<crd::usize>(m) * n);
    for (int i = 0; i < m; ++i) { for (int kk = 0; kk < k; ++kk) { av[static_cast<crd::usize>(i) * k + kk] = av_at(i, kk); } }
    for (int kk = 0; kk < k; ++kk) { for (int j = 0; j < n; ++j) { bv[static_cast<crd::usize>(kk) * n + j] = bv_at(kk, j); } }
    const float* inputs[] = {av.data(), bv.data()};

    const auto measure = [&](const TileSchedule& s) -> ContractTiming {
        const ContractTiming r = cu.time_contract_schedule(g, c, s, inputs, 2, out.data(), 3, 12);
        if (r.ok && !sampled_correct(out.data(), m, n, k)) { return ContractTiming{}; } // determinism gate: wrong can't win
        return r;
    };

    double       best_ms = 1.0e30;
    bool         have    = false;

    // 1. the hand-seed (select_schedule) if the shape is eligible — the §4 seed the search refines from.
    const TileSchedule seed = select_schedule(g, c);
    if (seed.kind == Sched::WarpTiled)
    {
        const ContractTiming sr = measure(seed);
        if (sr.ok)
        {
            res.seed_ms = sr.min_ms;
            best_ms     = sr.min_ms;
            res.sched   = seed;
            have        = true;
            ++res.measured;
            ++res.correct;
        }
    }

    // 2. explore the heuristic top-K of the valid space.
    autotune::DeviceLimits          lim;
    crd::containers::Array<TileSchedule> cand(a);
    cand.resize(4096);
    const int nc = autotune::enumerate_contract_schedules(m, n, k, lim, cand.data(), 4096);
    if (nc > 0)
    {
        const int                kk = top_k > 0 ? top_k : 16;
        crd::containers::Array<int> idx(a);
        idx.resize(static_cast<crd::usize>(kk));
        const int ntop = autotune::rank_top_k(cand.data(), nc, idx.data(), kk);
        for (int t = 0; t < ntop; ++t)
        {
            const ContractTiming r = measure(cand[static_cast<crd::usize>(idx[static_cast<crd::usize>(t)])]);
            if (!r.ok) { continue; }
            ++res.measured;
            ++res.correct;
            if (r.min_ms < best_ms)
            {
                best_ms   = r.min_ms;
                res.sched = cand[static_cast<crd::usize>(idx[static_cast<crd::usize>(t)])];
                have      = true;
            }
        }
    }

    // 3. optional naive baseline (the crush board).
    if (measure_naive)
    {
        const ContractTiming nr = cu.time_contract_schedule(g, c, TileSchedule{}, inputs, 2, out.data(), 3, 6);
        if (nr.ok && sampled_correct(out.data(), m, n, k)) { res.naive_ms = nr.min_ms; }
    }

    res.ok = have;
    res.ms = have ? best_ms : 0.0;
    return res;
}

} // namespace crd::kir
