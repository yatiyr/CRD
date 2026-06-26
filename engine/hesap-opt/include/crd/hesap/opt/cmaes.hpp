#pragma once

// cmaes.hpp — Phase 3.1.6 v7-q: CMA-ES (Hansen) — the (μ/μ_w, λ) Covariance Matrix Adaptation Evolution
// Strategy, implemented FAITHFULLY from Hansen's tutorial ("The CMA Evolution Strategy: A Tutorial",
// arXiv:1604.00772 — the same pseudocode pycma implements): weighted recombination of the top μ of λ
// Philox-sampled candidates, cumulative step-size adaptation (CSA, the pσ path against E‖N(0,I)‖), the
// rank-one (pc) + rank-μ covariance updates with the hσ stall guard, and the standard default parameters
// (λ = 4+⌊3 ln n⌋, the ln((λ+1)/2)−ln i weights, c_σ/d_σ/c_c/c_1/c_μ formulas). ACTIVE CMA (negative
// recombination weights — tutorial eqs. 46–53, pycma's CMA_active default) is ON by default: all λ ranked
// candidates feed the rank-μ update, the worst with negative weights scaled to
// −min(1+c1/cμ, 1+2μeff⁻/(μeff+2), (1−c1−cμ)/(n·cμ)) and each negative vector rescaled by n/‖z‖²
// (its Mahalanobis norm — pycma sampler.py) to guarantee positive definiteness. C is eigendecomposed every
// generation via the v3 `dense::eig_sym` (RNG-free) — Hansen's lazy-update is a CPU-cost trick, not a
// correctness piece (named scope). The gold standard for non-convex/rugged landscapes [pycma — the v7-z
// scoreboard]. ADR-0090.
//
// DETERMINISM: every sample comes from the (seed, stream)-keyed Philox normal stream; selection sorts are
// index-tie-broken insertion sorts; eig_sym is deterministic ⇒ the whole evolution path is bit-identical
// run-to-run by construction — the reproducibility pycma cannot promise across platforms.

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/dense/eig_sym.hpp>
#include <crd/hesap/dense/matrix_types.hpp>
#include <crd/hesap/opt/objective.hpp>
#include <crd/hesap/opt/opt_types.hpp>
#include <crd/hesap/stats/normal.hpp>
#include <crd/hesap/stats/philox.hpp>
#include <crd/memory/allocator.hpp>

#include <crd/math/cmath.hpp>
#include <limits>

namespace crd::hesap::opt
{

template <typename T> struct CmaesOptions
{
    T sigma0 = static_cast<T>(0.5); // initial step size (≈ 1/4 of the search-range width is typical)
    crd::usize lambda = 0;          // population size; 0 ⇒ 4 + ⌊3 ln n⌋ (Hansen's default)
    crd::usize max_evals = 0;       // 0 ⇒ 1000 · n · λ
    T ftol = static_cast<T>(1e-12); // generation f-spread tolerance (Hansen's TolFun flavor)
    T xtol = static_cast<T>(1e-12); // σ·max(diag C) tolerance (search-spread collapse)
    crd::u64 seed = 0x5EEDULL;      // the Philox stream key
    bool active = true;             // ACTIVE CMA (negative weights) — pycma's CMA_active default
};

// min f(x), unconstrained, value-only. Returns the BEST-EVER point.
template <typename T>
[[nodiscard]] OptResult<T> minimize_cmaes(const Objective<T>& obj, crd::containers::ConstSpan<T> x0,
                                          crd::memory::IAllocator* alloc, const CmaesOptions<T>& co = {})
{
    namespace dn = crd::hesap::dense;
    namespace st = crd::hesap::stats;
    const crd::usize n = obj.n();
    CRD_ASSERT_MSG(x0.size() == n, "minimize_cmaes: x0 size mismatch");

    OptResult<T> result(alloc);
    result.x.resize(n);
    for (crd::usize i = 0; i < n; ++i)
    {
        result.x[i] = x0[i];
    }
    if (n == 0)
    {
        result.status = OptStatus::Success;
        result.converged = true;
        return result;
    }

    const T nn = static_cast<T>(n);
    // ---- Hansen's default strategy parameters.
    const crd::usize lambda = co.lambda > 0 ? co.lambda : 4 + static_cast<crd::usize>(static_cast<T>(3) * crd::math::log(nn));
    const crd::usize mu = lambda / 2;
    // Raw weights w'_i = ln((λ+1)/2) − ln i over ALL λ ranks (tutorial eq. 49; positive for i ≤ μ).
    crd::containers::Array<T> weights(alloc);
    weights.resize(lambda);
    T wsum = static_cast<T>(0);     // Σ positive raw
    T wneg_sum = static_cast<T>(0); // Σ negative raw (≤ 0)
    T wneg_sq = static_cast<T>(0);
    for (crd::usize i = 0; i < lambda; ++i)
    {
        weights[i] = crd::math::log((static_cast<T>(lambda) + static_cast<T>(1)) / static_cast<T>(2)) -
                     crd::math::log(static_cast<T>(i + 1));
        if (i < mu)
        {
            wsum += weights[i];
        }
        else
        {
            wneg_sum += weights[i];
            wneg_sq += weights[i] * weights[i];
        }
    }
    T mueff_num = static_cast<T>(0);
    for (crd::usize i = 0; i < mu; ++i)
    {
        weights[i] /= wsum; // positives sum to 1
        mueff_num += weights[i] * weights[i];
    }
    const T mueff = static_cast<T>(1) / mueff_num;
    const T mueff_neg = wneg_sq > static_cast<T>(0) ? wneg_sum * wneg_sum / wneg_sq : static_cast<T>(0);
    const T csig = (mueff + static_cast<T>(2)) / (nn + mueff + static_cast<T>(5));
    const T sq = crd::math::sqrt((mueff - static_cast<T>(1)) / (nn + static_cast<T>(1))) - static_cast<T>(1);
    const T dsig = static_cast<T>(1) + static_cast<T>(2) * (sq > static_cast<T>(0) ? sq : static_cast<T>(0)) + csig;
    const T cc = (static_cast<T>(4) + mueff / nn) / (nn + static_cast<T>(4) + static_cast<T>(2) * mueff / nn);
    const T c1 = static_cast<T>(2) / ((nn + static_cast<T>(1.3)) * (nn + static_cast<T>(1.3)) + mueff);
    // Tutorial eq. 53 / pycma rankmu_offset = 0.25: cμ = min(1−c1, 2(¼ + μeff + 1/μeff − 2)/((n+2)² + μeff)).
    T cmu = static_cast<T>(2) * (static_cast<T>(0.25) + mueff + static_cast<T>(1) / mueff - static_cast<T>(2)) /
            ((nn + static_cast<T>(2)) * (nn + static_cast<T>(2)) + mueff);
    cmu = cmu < static_cast<T>(1) - c1 ? cmu : static_cast<T>(1) - c1;
    // Finalize the negative weights (tutorial eqs. 50–53; pycma finalize_negative_weights order):
    // normalize raw negatives to sum −1, then scale by min(αμ⁻, α_posdef⁻, αμeff⁻).
    T wsum_all = static_cast<T>(1); // Σ all final weights (the leading covariance-decay term)
    if (co.active && mu < lambda && wneg_sum < static_cast<T>(0) && cmu > static_cast<T>(0))
    {
        const T alpha_mu = static_cast<T>(1) + c1 / cmu;
        const T alpha_posdef = (static_cast<T>(1) - c1 - cmu) / (nn * cmu);
        const T alpha_mueff = static_cast<T>(1) + static_cast<T>(2) * mueff_neg / (mueff + static_cast<T>(2));
        T alpha = alpha_mu < alpha_posdef ? alpha_mu : alpha_posdef;
        alpha = alpha < alpha_mueff ? alpha : alpha_mueff;
        for (crd::usize i = mu; i < lambda; ++i)
        {
            weights[i] = alpha * weights[i] / -wneg_sum; // negatives now sum to −α
        }
        wsum_all = static_cast<T>(1) - alpha;
    }
    else
    {
        for (crd::usize i = mu; i < lambda; ++i)
        {
            weights[i] = static_cast<T>(0); // plain (non-active) CMA: the worst half does not feed C
        }
    }
    const T chin = crd::math::sqrt(nn) * (static_cast<T>(1) - static_cast<T>(1) / (static_cast<T>(4) * nn) +
                                    static_cast<T>(1) / (static_cast<T>(21) * nn * nn));

    // ---- State.
    crd::containers::Array<T> mean(alloc);
    crd::containers::Array<T> psig(alloc);
    crd::containers::Array<T> pc(alloc);
    crd::containers::Array<T> cmat(alloc); // C, n×n row-major full symmetric
    mean.resize(n);
    psig.resize(n);
    pc.resize(n);
    cmat.resize(n * n);
    for (crd::usize i = 0; i < n; ++i)
    {
        mean[i] = x0[i];
        psig[i] = static_cast<T>(0);
        pc[i] = static_cast<T>(0);
        for (crd::usize j = 0; j < n; ++j)
        {
            cmat[i * n + j] = i == j ? static_cast<T>(1) : static_cast<T>(0);
        }
    }
    T sigma = co.sigma0;

    // Per-generation buffers.
    crd::containers::Array<T> zs(alloc); // λ×n standard normals
    crd::containers::Array<T> ys(alloc); // λ×n: y = B·D·z
    crd::containers::Array<T> xs(alloc); // λ×n candidates
    crd::containers::Array<T> fs(alloc); // λ fitness
    crd::containers::Array<crd::u32> ord(alloc);
    crd::containers::Array<T> ymean(alloc);
    crd::containers::Array<T> cinvhalf_dy(alloc);
    zs.resize(lambda * n);
    ys.resize(lambda * n);
    xs.resize(lambda * n);
    fs.resize(lambda);
    ord.resize(lambda);
    ymean.resize(n);
    cinvhalf_dy.resize(n);

    st::PhiloxRng rng(co.seed, /*stream=*/0U);
    st::NormalSampler normal(rng);

    const crd::usize max_evals = co.max_evals > 0 ? co.max_evals : 1000 * n * lambda;
    crd::usize evals = 0;
    crd::usize gen = 0;
    T best_f = std::numeric_limits<T>::infinity();
    OptStatus status = OptStatus::MaxIterations;

    while (evals + lambda <= max_evals)
    {
        ++gen;
        // ---- Eigendecomposition C = B·diag(D²)·Bᵀ (deterministic; every generation — named scope).
        dn::Symmetric<T> csym(alloc, n);
        for (crd::usize i = 0; i < n; ++i)
        {
            for (crd::usize j = 0; j <= i; ++j)
            {
                csym.at(i, j) = (cmat[i * n + j] + cmat[j * n + i]) / static_cast<T>(2);
            }
        }
        const dn::EigSym<T> es = dn::eig_sym<T>(alloc, csym); // ascending values, column eigenvectors
        const T* lamv = es.values.data();
        T dmax = static_cast<T>(0);
        bool spd = true;
        crd::containers::Array<T> dvec(alloc); // D = sqrt(eigenvalues), floored at tiny
        dvec.resize(n);
        for (crd::usize i = 0; i < n; ++i)
        {
            if (lamv[i] <= static_cast<T>(0))
            {
                spd = false;
            }
            const T li = lamv[i] > static_cast<T>(1e-20) ? lamv[i] : static_cast<T>(1e-20);
            dvec[i] = crd::math::sqrt(li);
            dmax = dvec[i] > dmax ? dvec[i] : dmax;
        }
        if (!spd && gen > 1)
        {
            status = OptStatus::SmallStep; // covariance degenerated — an honest stall report
            break;
        }
        if (sigma * dmax < co.xtol)
        {
            status = OptStatus::Success; // the search spread collapsed
            break;
        }

        // ---- Sample λ candidates: z ~ N(0,I); y = B·(D∘z); x = m + σ·y.
        for (crd::usize k = 0; k < lambda; ++k)
        {
            T* z = zs.data() + k * n;
            T* y = ys.data() + k * n;
            T* x = xs.data() + k * n;
            normal.fill({z, n});
            for (crd::usize i = 0; i < n; ++i)
            {
                T acc = static_cast<T>(0);
                for (crd::usize j = 0; j < n; ++j)
                {
                    acc += es.vectors.at(i, j) * (dvec[j] * z[j]);
                }
                y[i] = acc;
                x[i] = mean[i] + sigma * acc;
            }
            fs[k] = obj.value({x, n});
            ++evals;
            ord[k] = static_cast<crd::u32>(k);
            if (fs[k] < best_f)
            {
                best_f = fs[k];
                for (crd::usize i = 0; i < n; ++i)
                {
                    result.x[i] = x[i];
                }
            }
        }
        // Stable insertion sort by fitness (index tie-break; no std::sort by repo rule).
        for (crd::usize i = 1; i < lambda; ++i)
        {
            const crd::u32 key = ord[i];
            crd::usize k = i;
            while (k > 0 && fs[ord[k - 1]] > fs[key])
            {
                ord[k] = ord[k - 1];
                --k;
            }
            ord[k] = key;
        }
        const T fspread = fs[ord[lambda - 1]] - fs[ord[0]];
        if (gen > 1 && fspread < co.ftol)
        {
            status = OptStatus::Success; // the population went flat (TolFun)
            break;
        }

        // ---- Recombination: m ← Σ wᵢ x_{i:λ};  ymean = Σ wᵢ y_{i:λ}.
        for (crd::usize i = 0; i < n; ++i)
        {
            T accx = static_cast<T>(0);
            T accy = static_cast<T>(0);
            for (crd::usize k = 0; k < mu; ++k)
            {
                accx += weights[k] * xs[ord[k] * n + i];
                accy += weights[k] * ys[ord[k] * n + i];
            }
            mean[i] = accx;
            ymean[i] = accy;
        }

        // ---- CSA: pσ ← (1−cσ)pσ + sqrt(cσ(2−cσ)μeff)·C^{−1/2}·ymean, C^{−1/2} = B·D^{−1}·Bᵀ.
        for (crd::usize i = 0; i < n; ++i)
        {
            // Bᵀ·ymean component j, scaled by 1/D, then B· — computed per output lane.
            T acc = static_cast<T>(0);
            for (crd::usize j = 0; j < n; ++j)
            {
                T btj = static_cast<T>(0);
                for (crd::usize l = 0; l < n; ++l)
                {
                    btj += es.vectors.at(l, j) * ymean[l];
                }
                acc += es.vectors.at(i, j) * (btj / dvec[j]);
            }
            cinvhalf_dy[i] = acc;
        }
        const T csig_scale = crd::math::sqrt(csig * (static_cast<T>(2) - csig) * mueff);
        T psig_norm_sq = static_cast<T>(0);
        for (crd::usize i = 0; i < n; ++i)
        {
            psig[i] = (static_cast<T>(1) - csig) * psig[i] + csig_scale * cinvhalf_dy[i];
            psig_norm_sq += psig[i] * psig[i];
        }
        const T psig_norm = crd::math::sqrt(psig_norm_sq);

        // hσ stall guard (Hansen's (1 − (1−cσ)^{2·evals/λ}) denominator).
        const T denom_pow =
            static_cast<T>(1) -
            crd::math::pow(static_cast<T>(1) - csig, static_cast<T>(2) * static_cast<T>(evals) / static_cast<T>(lambda));
        const T hsig_thresh =
            (static_cast<T>(1.4) + static_cast<T>(2) / (nn + static_cast<T>(1))) * chin * crd::math::sqrt(denom_pow);
        const T hsig = psig_norm < hsig_thresh ? static_cast<T>(1) : static_cast<T>(0);

        // ---- pc and the covariance update (rank-one + rank-μ, with the hσ correction).
        const T cc_scale = crd::math::sqrt(cc * (static_cast<T>(2) - cc) * mueff);
        for (crd::usize i = 0; i < n; ++i)
        {
            pc[i] = (static_cast<T>(1) - cc) * pc[i] + hsig * cc_scale * ymean[i];
        }
        const T c1a = c1 * (static_cast<T>(1) - (static_cast<T>(1) - hsig * hsig) * cc * (static_cast<T>(2) - cc));
        // Effective rank-μ weights: w°_k = w_k for w_k ≥ 0, else w_k·n/(‖z_k‖+1e-9)² — the Mahalanobis rescale
        // (‖C^{−1/2}y‖ = ‖z‖ since y = B·D·z) that keeps the active update positive definite (pycma sampler.py).
        crd::containers::Array<T> weff(alloc);
        weff.resize(lambda);
        for (crd::usize k = 0; k < lambda; ++k)
        {
            T w = weights[k];
            if (w < static_cast<T>(0))
            {
                const T* z = zs.data() + ord[k] * n;
                T znorm_sq = static_cast<T>(0);
                for (crd::usize i = 0; i < n; ++i)
                {
                    znorm_sq += z[i] * z[i];
                }
                const T znorm = crd::math::sqrt(znorm_sq) + static_cast<T>(1e-9);
                w *= nn / (znorm * znorm);
            }
            weff[k] = w;
        }
        for (crd::usize i = 0; i < n; ++i)
        {
            for (crd::usize j = 0; j < n; ++j)
            {
                T rank_mu = static_cast<T>(0);
                for (crd::usize k = 0; k < lambda; ++k)
                {
                    if (weff[k] != static_cast<T>(0))
                    {
                        rank_mu += weff[k] * ys[ord[k] * n + i] * ys[ord[k] * n + j];
                    }
                }
                cmat[i * n + j] =
                    (static_cast<T>(1) - c1a - cmu * wsum_all) * cmat[i * n + j] + c1 * pc[i] * pc[j] + cmu * rank_mu;
            }
        }

        // ---- Step-size: σ ← σ·exp((cσ/dσ)(‖pσ‖/E‖N(0,I)‖ − 1)).
        sigma *= crd::math::exp((csig / dsig) * (psig_norm / chin - static_cast<T>(1)));
    }

    result.fx = best_f;
    result.fn_evals = evals;
    result.iterations = gen;
    result.status = status;
    result.converged = status == OptStatus::Success;
    return result;
}

} // namespace crd::hesap::opt
