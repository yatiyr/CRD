#pragma once

#include <crd/containers/array.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/amg/aggregation.hpp>
#include <crd/hesap/amg/cf_splitting.hpp>
#include <crd/hesap/amg/prolongator.hpp>
#include <crd/hesap/amg/rs_interpolation.hpp>
#include <crd/hesap/amg/rs_strength.hpp>
#include <crd/hesap/amg/strength.hpp>
#include <crd/hesap/dense/lu.hpp>
#include <crd/hesap/dense/matrix.hpp>
#include <crd/hesap/dense/real_type.hpp>
#include <crd/hesap/linear_op.hpp>
#include <crd/hesap/preconditioners/ilu0.hpp> // ILU(0) smoother (convection-robust)
#include <crd/hesap/sparse/convert.hpp>
#include <crd/hesap/sparse/spgemm.hpp>
#include <crd/hesap/sparse/sparse_matrix.hpp>
#include <crd/memory/allocator.hpp>

#include <cmath>
#include <memory>
#include <utility>

namespace crd::hesap::amg
{
// ---------------------------------------------------------------------------
// SaAmg<T> -- Smoothed-Aggregation AMG (Vaněk-Mandel-Brezina 1996) as a
// LinearOp<T> preconditioner (M⁻¹ = one V-cycle) and standalone solver.
// Phase 3.1.6 v4k-a. The mesh-independent multilevel method (the crush vehicle).
//
// Build: at each level  S = strength(A,θ) → agg = aggregate(S) → T = tentative →
// ω = 4/(3λmax(D⁻¹A)) → P = (I−ωD⁻¹A)T → A_c = Pᵀ A P (Galerkin, two spgemm) →
// recurse until n ≤ coarse_threshold (or max_levels) → dense LU coarsest.
// apply: one V-cycle, Gauss-Seidel smoother (fwd pre ν₁ + bwd post ν₂); Ilu0
// smoother opt-in for near-symmetric diffusion (fewest iters, but can diverge
// on strong convection — that regime is the v4j inverse-based-ILU's job).
//
// Deterministic (D(amg)-1..5): aggregation/Galerkin/λmax/V-cycle all fixed; the
// factor is serial, only spmv/spgemm parallel (the moat). Real now; complex 2c.
// ---------------------------------------------------------------------------

template <typename T>
class SaAmg final : public crd::hesap::LinearOp<T>
{
public:
    using R    = crd::hesap::dense::RealType<T>;
    using Csr  = crd::hesap::sparse::SparseMatrix<T, crd::hesap::sparse::SparseFormat::Csr>;
    using LuT  = crd::hesap::dense::LU<T, crd::hesap::dense::Layout::RowMajor>;

    // Smoother choice. GaussSeidel is the robust default: it converges on
    // SPD/anisotropic diffusion AND degrades gracefully on convection (never
    // diverges). Ilu0 gives the fewest iters on pure diffusion but can DIVERGE on
    // strongly nonsymmetric convection (spectral radius > 1 on the modes the
    // coarse grid cannot see) — opt in only when the operator is near-symmetric.
    enum class Smoother : crd::u8 { GaussSeidel, Ilu0 };

    // Cycle type. V (default, robust, O(L)) ; W (γ=2 coarse recursions — a per-problem convection
    // lever that can diverge on conservative operators) ; F (one F-recursion then one V-recursion
    // per level — between V and W in cost/robustness) ; K = Krylov-accelerated cycle (Notay AGMG
    // 2010): each intermediate level's coarse correction is 2 GCR steps preconditioned by the
    // recursive cycle. The K projection STABILIZES the nonsymmetric coarse operator that W
    // amplifies ⇒ the robust convection cycle. The AMG apply is fully serial, so K's inner
    // products are deterministic (the moat is carried by the outer solver).
    enum class Cycle : crd::u8 { V, W, F, K };

    // Coarsening strategy. SmoothedAggregation (Vaněk, default): strength → aggregates → smoothed
    // tentative prolongator. RugeStuben (classical): directed strength → C/F splitting → direct
    // interpolation. RS is the textbook isotropic-diffusion AMG; SA is more robust on aniso/nonsym.
    enum class Coarsening : crd::u8 { SmoothedAggregation, RugeStuben };

    struct Options
    {
        Coarsening coarsening     = Coarsening::SmoothedAggregation; // default SA
        R        rs_theta         = R(static_cast<R>(0.25)); // classical RS strength threshold
        R        theta            = R(static_cast<R>(0.08)); // SA strength threshold
        Smoother smoother         = Smoother::GaussSeidel;   // robust default; Ilu0 = diffusion-only opt-in
        Cycle    cycle            = Cycle::V;                 // V robust default; W/K = convection levers
        bool     smooth_prolongator = true;  // true = SA (Vaněk); false = plain aggregation (AGMG-style, convection)
        bool     adaptive_candidate = false; // αSA: seed tentative T from a relaxed near-nullspace candidate
        crd::u32 n_candidate_sweeps = 4;     // relaxation sweeps to expose the near-nullspace (adaptive only)
        crd::u32 npre             = 2;
        crd::u32 npost            = 2;
        crd::u32 max_levels       = 25;
        crd::u32 coarse_threshold = 50; // n ≤ this ⇒ dense LU coarsest
    };

    SaAmg(const Csr& a, crd::memory::IAllocator* alloc, Options opts = {})
        : crd::hesap::LinearOp<T>(/*has_transpose=*/false, /*has_adjoint=*/false)
        , m_alloc(alloc), m_opts(opts), m_levels(alloc), m_coarse_lu(alloc), m_n(a.rows())
    {
        CRD_ASSERT_MSG(a.rows() == a.cols(), "SaAmg: matrix must be square");
        CRD_ASSERT_MSG(a.pattern().is_compressed(), "SaAmg: requires compressed CSR");
        build(a);
    }

    // z = M⁻¹ r  (one V-cycle from a zero initial guess).
    [[nodiscard]] bool apply(crd::containers::ConstSpan<T> r, crd::containers::Span<T> z) const override
    {
        for (crd::u32 i = 0; i < m_n; ++i) { z[i] = T{}; }
        vcycle(0, r.data(), z.data(), m_opts.cycle);
        return true;
    }

    [[nodiscard]] crd::usize n_rows() const noexcept override { return m_n; }
    [[nodiscard]] crd::usize n_cols() const noexcept override { return m_n; }

    [[nodiscard]] crd::u32   num_levels() const noexcept { return static_cast<crd::u32>(m_levels.size()) + 1; }
    [[nodiscard]] crd::usize operator_complexity() const noexcept
    {
        crd::usize tot = 0;
        for (crd::u32 k = 0; k < m_levels.size(); ++k) { tot += m_levels[k].a.nnz(); }
        tot += m_coarse_nnz;
        return tot;
    }
    [[nodiscard]] crd::u32 coarse_size() const noexcept { return m_coarse_n; }

private:
    struct Level
    {
        Csr                       a;     // operator at this level
        Csr                       p;     // prolongator (n_k × n_{k+1})
        Csr                       r;     // restriction (Petrov-Galerkin; = Pᵀ for symmetric)
        crd::containers::Array<T> dinv;  // 1/diag(a) (complex for complex A)
        std::unique_ptr<crd::hesap::preconditioners::Ilu0Preconditioner<T>> ilu; // smoother (convection-robust)
        crd::u32                  n;
        explicit Level(crd::memory::IAllocator* al) : a(al), p(al), r(al), dinv(al), ilu(), n(0) {}
    };

    [[nodiscard]] static R mag(T v)
    {
        if constexpr (crd::hesap::dense::is_complex_v<T>) { return std::sqrt(v.re * v.re + v.im * v.im); }
        else { return v < R(0) ? -v : v; }
    }

    void build(const Csr& a0)
    {
        Csr a = clone_csr(a0, m_alloc);
        for (crd::u32 lvl = 0; lvl < m_opts.max_levels; ++lvl)
        {
            const crd::u32 n = a.rows();
            if (n <= m_opts.coarse_threshold) { break; }
            Level level(m_alloc);
            level.n = n;
            // 1/diag
            level.dinv.resize(n);
            fill_dinv(a, level.dinv);
            if (m_opts.coarsening == Coarsening::RugeStuben)
            {
                // Classical Ruge-Stüben: directed strength → C/F split → direct interpolation.
                // R = Pᵀ (standard Galerkin; RS targets symmetric M-matrices).
                auto     srs      = rs_strength_matrix<T>(a, m_opts.rs_theta, m_alloc);
                crd::u32 n_coarse = 0;
                auto     cf       = rs_cf_split<T>(srs, n_coarse, m_alloc);
                if (n_coarse == 0 || n_coarse >= n) { break; } // no coarsening progress ⇒ dense-solve `a`
                level.p = rs_direct_interpolation<T>(a, srs, cf, n_coarse, m_alloc);
                level.r = crd::hesap::sparse::transpose<T>(level.p, m_alloc);
            }
            else
            {
                // Smoothed aggregation (Vaněk): strength → aggregate → tentative → smoothed P.
                auto     s = strength_matrix<T>(a, m_opts.theta, m_alloc);
                crd::u32 n_agg = 0;
                auto     agg   = aggregate<T>(s, n_agg, m_alloc);
                if (n_agg == 0 || n_agg >= n) { break; } // no coarsening progress ⇒ stop, dense-solve `a`
                // Tentative T: constant (SA) or seeded from a relaxed near-nullspace candidate (αSA).
                Csr tent(m_alloc);
                if (m_opts.adaptive_candidate)
                {
                    crd::containers::Array<T> cand(m_alloc);
                    relax_candidate(a, level.dinv, cand);
                    tent = tentative_prolongator_adaptive<T>(agg, n_agg, cand, m_alloc);
                }
                else { tent = tentative_prolongator<T>(agg, n_agg, m_alloc); }
                if (m_opts.smooth_prolongator)
                {
                    const R rho   = estimate_drinv_a_radius<T>(a, level.dinv, m_alloc);
                    const R omega = (rho > R(0)) ? R(static_cast<R>(4.0 / 3.0)) / rho : R(static_cast<R>(2.0 / 3.0));
                    level.p = smoothed_prolongator<T>(a, tent, level.dinv, omega, m_alloc);
                    // PETROV-GALERKIN restriction: smooth the tentative prolongator with Aᵀ (not A)
                    // then transpose. For symmetric A this is exactly Pᵀ; for NONSYMMETRIC A it
                    // adapts the restriction to the adjoint direction → a STABLE coarse operator.
                    level.r = build_restriction(a, tent, level.dinv, m_alloc);
                }
                else
                {
                    // Plain (unsmoothed) aggregation (AGMG-style): P = tentative, R = Pᵀ.
                    level.r = crd::hesap::sparse::transpose<T>(tent, m_alloc);
                    level.p = std::move(tent);
                }
            }
            level.a = std::move(a);
            if (m_opts.smoother == Smoother::Ilu0)
            {
                level.ilu = std::make_unique<crd::hesap::preconditioners::Ilu0Preconditioner<T>>(level.a, m_alloc);
            }
            // Galerkin A_c = R A P
            auto ap = crd::hesap::sparse::spgemm<T>(level.a, level.p, m_alloc);
            a       = crd::hesap::sparse::spgemm<T>(level.r, ap, m_alloc);
            m_levels.push_back(std::move(level));
        }
        // coarsest: dense LU
        build_coarse(a);
    }

    // Petrov-Galerkin restriction R = [ smoothed_prolongator(Aᵀ, T) ]ᵀ.
    // dinv(Aᵀ) = dinv(A) (same diagonal). For symmetric A, R = Pᵀ exactly.
    [[nodiscard]] static Csr build_restriction(const Csr& a, const Csr& tent,
                                               const crd::containers::Array<T>& dinv, crd::memory::IAllocator* alloc)
    {
        auto    at      = crd::hesap::sparse::transpose<T>(a, alloc);
        const R rho_adj = estimate_drinv_a_radius<T>(at, dinv, alloc);
        const R w_adj   = (rho_adj > R(0)) ? R(static_cast<R>(4.0 / 3.0)) / rho_adj : R(static_cast<R>(2.0 / 3.0));
        auto    p_adj   = smoothed_prolongator<T>(at, tent, dinv, w_adj, alloc);
        return crd::hesap::sparse::transpose<T>(p_adj, alloc);
    }

    void build_coarse(const Csr& a)
    {
        m_coarse_n   = a.rows();
        m_coarse_nnz = a.nnz();
        crd::hesap::dense::Matrix<T> dense(m_alloc, m_coarse_n, m_coarse_n);
        for (crd::u32 i = 0; i < m_coarse_n; ++i) { for (crd::u32 j = 0; j < m_coarse_n; ++j) { dense.at(i, j) = T{}; } }
        const auto* o = a.pattern().outer_ptr.data();
        const auto* c = a.pattern().inner_idx.data();
        const T*    v = a.values().values.data();
        for (crd::u32 i = 0; i < m_coarse_n; ++i) { for (crd::u32 q = o[i]; q < o[i + 1]; ++q) { dense.at(i, c[q]) = v[q]; } }
        m_coarse_lu = LuT(m_alloc, m_coarse_n);
        crd::hesap::dense::factor_lu<T, crd::hesap::dense::Layout::RowMajor>(m_coarse_lu, dense);
    }

    // αSA candidate: relax A·x = 0 from a deterministic seed with a few weighted-Jacobi sweeps
    // (x ← x − ω D⁻¹ A x, ω=2/3). The smoother kills the high-frequency error, leaving x rich in
    // the slow near-nullspace modes that a constant tentative can't represent on convection/
    // anisotropy. Deterministic seed (D(amg)-4 style) + fixed sweep count ⇒ reproducible.
    void relax_candidate(const Csr& a, const crd::containers::Array<T>& dinv, crd::containers::Array<T>& cand) const
    {
        const crd::u32 n = a.rows();
        cand.resize(n);
        for (crd::u32 i = 0; i < n; ++i) { cand[i] = T(R(1) + static_cast<R>(i % 7) / R(7)); }
        crd::containers::Array<T> ax(m_alloc);
        ax.resize(n);
        const T omega = T(R(static_cast<R>(2.0 / 3.0)));
        for (crd::u32 s = 0; s < m_opts.n_candidate_sweeps; ++s)
        {
            spmv(a, cand.data(), ax.data());
            for (crd::u32 i = 0; i < n; ++i) { cand[i] = cand[i] - omega * dinv[i] * ax[i]; }
        }
    }

    static void fill_dinv(const Csr& a, crd::containers::Array<T>& dinv)
    {
        const crd::u32 n     = a.rows();
        const auto*    outer = a.pattern().outer_ptr.data();
        const auto*    inner = a.pattern().inner_idx.data();
        const T*       vals  = a.values().values.data();
        dinv.resize(n);
        for (crd::u32 i = 0; i < n; ++i)
        {
            T d = T{};
            for (crd::u32 q = outer[i]; q < outer[i + 1]; ++q) { if (inner[q] == i) { d = vals[q]; break; } }
            dinv[i] = (mag(d) > R(0)) ? T(R(1)) / d : T(R(1)); // 1/a_ii (complex division for complex A)
        }
    }

    [[nodiscard]] static Csr clone_csr(const Csr& a, crd::memory::IAllocator* alloc)
    {
        crd::hesap::sparse::TripletBuilder<T> tb(alloc, a.rows(), a.cols());
        const auto* o = a.pattern().outer_ptr.data();
        const auto* c = a.pattern().inner_idx.data();
        const T*    v = a.values().values.data();
        for (crd::u32 i = 0; i < a.rows(); ++i) { for (crd::u32 q = o[i]; q < o[i + 1]; ++q) { tb.add(i, c[q], v[q]); } }
        return tb.compress();
    }

    // y = A x  (CSR spmv).
    static void spmv(const Csr& a, const T* x, T* y)
    {
        const crd::u32 n     = a.rows();
        const auto*    outer = a.pattern().outer_ptr.data();
        const auto*    inner = a.pattern().inner_idx.data();
        const T*       vals  = a.values().values.data();
        for (crd::u32 i = 0; i < n; ++i)
        {
            T s = T{};
            for (crd::u32 q = outer[i]; q < outer[i + 1]; ++q) { s = s + vals[q] * x[inner[q]]; }
            y[i] = s;
        }
    }

    // Multigrid cycle: solve (approximately) A_lvl e = b into x (x preset). `cyc` selects the
    // coarse-correction schedule at this level (V/W/F/K); the recursion propagates it.
    void vcycle(crd::u32 lvl, const T* b, T* x, Cycle cyc) const
    {
        if (lvl == m_levels.size())
        {
            // coarsest: exact dense solve  x = A_coarse⁻¹ b
            for (crd::u32 i = 0; i < m_coarse_n; ++i) { x[i] = b[i]; }
            crd::hesap::dense::solve_lu<T, crd::hesap::dense::Layout::RowMajor>(m_coarse_lu,
                crd::containers::Span<T>{x, m_coarse_n});
            return;
        }
        const Level&   L = m_levels[lvl];
        const crd::u32 n = L.n;
        const crd::u32 nc = L.p.cols();

        crd::containers::Array<T> r(m_alloc), rc(m_alloc), ec(m_alloc), ax(m_alloc), dx(m_alloc);
        r.resize(n); ax.resize(n); dx.resize(n); rc.resize(nc == 0 ? 1 : nc); ec.resize(nc == 0 ? 1 : nc);

        for (crd::u32 s = 0; s < m_opts.npre; ++s) { smooth(L, b, x, ax.data(), dx.data(), /*fwd=*/true); }
        // residual r = b − A x
        spmv(L.a, x, ax.data());
        for (crd::u32 i = 0; i < n; ++i) { r[i] = b[i] - ax[i]; }
        // restrict rc = R r
        spmv(L.r, r.data(), rc.data());
        // coarse solve ec ≈ A_c⁻¹ rc. V (γ=1) / W (γ=2 recursions) / K (Krylov-accelerated).
        // The coarsest level is an exact dense solve, so cycle type matters only above it.
        for (crd::u32 i = 0; i < nc; ++i) { ec[i] = T{}; }
        const bool next_is_coarsest = (lvl + 1 == m_levels.size());
        if (next_is_coarsest)
        {
            vcycle(lvl + 1, rc.data(), ec.data(), cyc); // exact dense coarse solve
        }
        else if (cyc == Cycle::K)
        {
            kcycle_coarse(lvl + 1, rc.data(), ec.data());
        }
        else if (cyc == Cycle::W)
        {
            vcycle(lvl + 1, rc.data(), ec.data(), Cycle::W);
            vcycle(lvl + 1, rc.data(), ec.data(), Cycle::W);
        }
        else if (cyc == Cycle::F)
        {
            // F-cycle: one F-recursion (heavier, down the deepening branch) then one V-recursion.
            vcycle(lvl + 1, rc.data(), ec.data(), Cycle::F);
            vcycle(lvl + 1, rc.data(), ec.data(), Cycle::V);
        }
        else // Cycle::V
        {
            vcycle(lvl + 1, rc.data(), ec.data(), Cycle::V);
        }
        // prolong x += P ec
        spmv(L.p, ec.data(), ax.data()); // ax = P ec
        for (crd::u32 i = 0; i < n; ++i) { x[i] = x[i] + ax[i]; }
        for (crd::u32 s = 0; s < m_opts.npost; ++s) { smooth(L, b, x, ax.data(), dx.data(), /*fwd=*/false); }
    }

    // K-cycle coarse solve (Notay AGMG 2010): approximately solve A_lvl x = b (x preset 0) with
    // TWO steps of flexible GCR, using ONE recursive cycle [vcycle(lvl,·)] as the variable
    // preconditioner. GCR minimizes the residual over span{z1,z2} (right choice for the
    // nonsymmetric convection operator) — the Krylov projection bounds the coarse-grid
    // correction that the plain W-cycle amplifies into divergence. All reductions are serial
    // scalar dot products ⇒ deterministic (D(amg)-6: fixed 2-step GCR, fixed recursion).
    void kcycle_coarse(crd::u32 lvl, const T* b, T* x) const
    {
        const crd::u32 n = m_levels[lvl].n;
        const Csr&     a = m_levels[lvl].a;

        crd::containers::Array<T> z1(m_alloc), z2(m_alloc), c1(m_alloc), c2(m_alloc), r1(m_alloc);
        z1.resize(n); z2.resize(n); c1.resize(n); c2.resize(n); r1.resize(n);

        // step 1: z1 = M⁻¹ b ; c1 = A z1 ; minimize ⇒ x = α z1, r1 = b − α c1.
        for (crd::u32 i = 0; i < n; ++i) { z1[i] = T{}; }
        vcycle(lvl, b, z1.data(), Cycle::K);
        spmv(a, z1.data(), c1.data());
        const T c1c1 = dotc(c1.data(), c1.data(), n);
        const T alpha = safe_div(dotc(c1.data(), b, n), c1c1);
        for (crd::u32 i = 0; i < n; ++i) { x[i] = alpha * z1[i]; r1[i] = b[i] - alpha * c1[i]; }

        // step 2: z2 = M⁻¹ r1 ; c2 = A z2 ; GCR-orthogonalize c2 ⊥ c1 (mirror on z2) ⇒ x += γ z2.
        for (crd::u32 i = 0; i < n; ++i) { z2[i] = T{}; }
        vcycle(lvl, r1.data(), z2.data(), Cycle::K);
        spmv(a, z2.data(), c2.data());
        const T beta = safe_div(dotc(c1.data(), c2.data(), n), c1c1);
        for (crd::u32 i = 0; i < n; ++i) { c2[i] = c2[i] - beta * c1[i]; z2[i] = z2[i] - beta * z1[i]; }
        const T gamma = safe_div(dotc(c2.data(), r1.data(), n), dotc(c2.data(), c2.data(), n));
        for (crd::u32 i = 0; i < n; ++i) { x[i] = x[i] + gamma * z2[i]; }
    }

    // Σ conj(x_i)·y_i (Hermitian inner product; conj is identity for real T).
    [[nodiscard]] static T dotc(const T* x, const T* y, crd::u32 n)
    {
        T s = T{};
        for (crd::u32 i = 0; i < n; ++i)
        {
            if constexpr (crd::hesap::dense::is_complex_v<T>) { s = s + T{x[i].re, -x[i].im} * y[i]; }
            else { s = s + x[i] * y[i]; }
        }
        return s;
    }

    // d/q guarded against a zero denominator (a deflated/converged GCR direction ⇒ skip it).
    [[nodiscard]] static T safe_div(T num, T den) { return (mag(den) > R(0)) ? num / den : T{}; }

    // One smoother sweep. ILU(0): x += M_ilu⁻¹ (b − A x) (convection-robust — the
    // standard AMG smoother for nonsymmetric/anisotropic where GS/Jacobi stall).
    // Else Gauss-Seidel (forward/backward). `sa`/`sd` are n-sized scratch.
    void smooth(const Level& L, const T* b, T* x, T* sa, T* sd, bool forward) const
    {
        if (L.ilu)
        {
            const crd::u32 n = L.n;
            spmv(L.a, x, sa);                                   // sa = A x
            for (crd::u32 i = 0; i < n; ++i) { sa[i] = b[i] - sa[i]; } // sa = r = b − A x
            (void)L.ilu->apply(crd::containers::ConstSpan<T>{sa, n}, crd::containers::Span<T>{sd, n}); // sd = M⁻¹ r
            for (crd::u32 i = 0; i < n; ++i) { x[i] = x[i] + sd[i]; }
        }
        else { gauss_seidel(L, b, x, forward); }
    }

    // One Gauss-Seidel sweep (in place): x_i ← (b_i − Σ_{j≠i} a_ij x_j)/a_ii, using
    // already-updated x_j within the sweep. Forward (i ascending) for pre-smooth,
    // backward (i descending) for post-smooth ⇒ a symmetric V-cycle smoother. GS
    // is far stronger than weighted Jacobi on nonsymmetric/anisotropic operators
    // (convection-diffusion), where Jacobi-smoothed SA-AMG stalls.
    void gauss_seidel(const Level& L, const T* b, T* x, bool forward) const
    {
        const crd::u32 n     = L.n;
        const auto*    outer = L.a.pattern().outer_ptr.data();
        const auto*    inner = L.a.pattern().inner_idx.data();
        const T*       vals  = L.a.values().values.data();
        auto sweep = [&](crd::u32 i) {
            T s = T{};
            for (crd::u32 q = outer[i]; q < outer[i + 1]; ++q) { if (inner[q] != i) { s = s + vals[q] * x[inner[q]]; } }
            x[i] = L.dinv[i] * (b[i] - s); // dinv = 1/a_ii (complex for complex A)
        };
        if (forward) { for (crd::u32 i = 0; i < n; ++i) { sweep(i); } }
        else { for (crd::u32 i = n; i-- > 0;) { sweep(i); } }
    }

    crd::memory::IAllocator* m_alloc;
    Options                  m_opts;
    crd::containers::Array<Level> m_levels;
    LuT                      m_coarse_lu;
    crd::u32                 m_coarse_n   = 0;
    crd::usize               m_coarse_nnz = 0;
    crd::u32                 m_n;
};

} // namespace crd::hesap::amg
