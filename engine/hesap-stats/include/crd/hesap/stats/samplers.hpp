#pragma once

// crd-hesap-stats v12-f — fast distribution samplers, generic over any BitGenerator, gated statistically (KS +
// moments vs the hesap-special CDFs) + determinism (same seed ⇒ same samples). The gold algorithms:
//   gamma   — Marsaglia-Tsang (2000): squeeze on a normal-cubed proposal, O(1) expected.
//   beta    — gamma ratio X/(X+Y).
//   poisson — Knuth (λ<10) + PTRS transformed-rejection (Hörmann; λ≥10), the NumPy method.
//   binomial— BINV inversion (np<30) + BTPE (Kachitvichyanukul-Schmeiser; the NumPy method).
//   alias   — Vose's O(1) categorical.

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/hesap/stats/bitgen.hpp>
#include <crd/hesap/stats/ziggurat.hpp>
#include <crd/hesap/special/gamma.hpp> // lgamma (PTRS)
#include <crd/memory/allocator.hpp>    // IAllocator (AliasTable)
#include <crd/core/types.hpp>

#include <crd/math/cmath.hpp>

namespace crd::hesap::stats
{
// Gamma(shape, scale) — Marsaglia & Tsang.
template <BitGenerator G>
[[nodiscard]] double gamma_dist(G& g, double shape, double scale = 1.0) noexcept
{
    if (shape < 1.0) // boost: Γ(a) = Γ(a+1)·U^(1/a)
    {
        const double u = 1.0 - next_double(g);
        return gamma_dist(g, shape + 1.0, scale) * crd::math::pow(u, 1.0 / shape);
    }
    const double d = shape - 1.0 / 3.0;
    const double c = 1.0 / crd::math::sqrt(9.0 * d);
    for (;;)
    {
        double x;
        double v;
        do
        {
            x = standard_normal(g);
            v = 1.0 + c * x;
        } while (v <= 0.0);
        v = v * v * v;
        const double u = next_double(g);
        const double x2 = x * x;
        if (u < 1.0 - 0.0331 * x2 * x2) // squeeze
        {
            return d * v * scale;
        }
        if (crd::math::log(u) < 0.5 * x2 + d * (1.0 - v + crd::math::log(v))) // exact
        {
            return d * v * scale;
        }
    }
}

// Beta(a, b) via the gamma ratio.
template <BitGenerator G>
[[nodiscard]] double beta_dist(G& g, double a, double b) noexcept
{
    const double x = gamma_dist(g, a, 1.0);
    const double y = gamma_dist(g, b, 1.0);
    const double s = x + y;
    return s > 0.0 ? x / s : 0.5;
}

// Chi-squared(k) = Gamma(k/2, 2).
template <BitGenerator G>
[[nodiscard]] double chi_squared(G& g, double k) noexcept
{
    return gamma_dist(g, 0.5 * k, 2.0);
}

// Poisson(lambda).
template <BitGenerator G>
[[nodiscard]] crd::i64 poisson(G& g, double lambda) noexcept
{
    if (lambda < 10.0) // Knuth multiplication
    {
        const double el = crd::math::exp(-lambda);
        crd::i64 k = 0;
        double p = 1.0;
        do
        {
            ++k;
            p *= next_double(g);
        } while (p > el);
        return k - 1;
    }
    // PTRS — transformed rejection with squeeze (Hörmann 1993).
    const double slam = crd::math::sqrt(lambda);
    const double loglam = crd::math::log(lambda);
    const double b = 0.931 + 2.53 * slam;
    const double a = -0.059 + 0.02483 * b;
    const double inv_alpha = 1.1239 + 1.1328 / (b - 3.4);
    const double vr = 0.9277 - 3.6224 / (b - 2.0);
    for (;;)
    {
        const double u = next_double(g) - 0.5;
        const double v = next_double(g);
        const double us = 0.5 - crd::math::fabs(u);
        const auto k = static_cast<crd::i64>(crd::math::floor((2.0 * a / us + b) * u + lambda + 0.43));
        if (us >= 0.07 && v <= vr)
        {
            return k;
        }
        if (k < 0 || (us < 0.013 && v > us))
        {
            continue;
        }
        const double lhs = crd::math::log(v) + crd::math::log(inv_alpha) - crd::math::log(a / (us * us) + b);
        const double rhs = -lambda + static_cast<double>(k) * loglam - crd::hesap::special::lgamma(static_cast<double>(k) + 1.0);
        if (lhs <= rhs)
        {
            return k;
        }
    }
}

// Stateful binomial sampler — precomputes the (n,p)-only constants ONCE (NumPy's `binomial_t` pattern). Reuse it
// across draws of the same distribution and the per-call BINV/BTPE setup (a `pow` or `sqrt`) amortizes to zero — for
// repeated Binomial(1000,0.5) draws this is ~25.3 → ~15.9 ns/draw (1.59×), which then beats NumPy. The free
// binomial(g,n,p) below builds a throwaway sampler (one-off setup); hold a BinomialSampler for repeated draws.
// Both share ONE implementation, and the draw consumes next_double in the same order as a recompute-per-call would,
// so a held sampler and the free function are bit-identical (gated in test_samplers).
class BinomialSampler
{
public:
    BinomialSampler(crd::i64 n, double p) noexcept : m_n(n)
    {
        if (n <= 0 || p <= 0.0)
        {
            m_mode = Mode::Constant;
            m_constant = 0;
            return;
        }
        if (p >= 1.0)
        {
            m_mode = Mode::Constant;
            m_constant = n;
            return;
        }
        m_flip = p > 0.5;                  // sample Binomial(n, r<=0.5), reflect (n-y) at the end if flipped
        m_r = m_flip ? 1.0 - p : p;
        m_q = 1.0 - m_r;
        if (static_cast<double>(n) * m_r < 30.0)
        {
            m_mode = Mode::Inversion;
            m_qn = crd::math::pow(m_q, static_cast<double>(n));
            const double np = static_cast<double>(n) * m_r;
            m_bound = np + 10.0 * crd::math::sqrt(np * m_q + 1.0);
        }
        else
        {
            m_mode = Mode::Btpe;
            const double fm = static_cast<double>(n) * m_r + m_r;
            m_m = static_cast<crd::i64>(fm);
            m_p1 = crd::math::floor(2.195 * crd::math::sqrt(static_cast<double>(n) * m_r * m_q) - 4.6 * m_q) + 0.5;
            m_xm = static_cast<double>(m_m) + 0.5;
            m_xl = m_xm - m_p1;
            m_xr = m_xm + m_p1;
            m_c = 0.134 + 20.5 / (15.3 + static_cast<double>(m_m));
            double al = (fm - m_xl) / (fm - m_xl * m_r);
            m_laml = al * (1.0 + 0.5 * al);
            al = (m_xr - fm) / (m_xr * m_q);
            m_lamr = al * (1.0 + 0.5 * al);
            m_p2 = m_p1 * (1.0 + 2.0 * m_c);
            m_p3 = m_p2 + m_c / m_laml;
            m_p4 = m_p3 + m_c / m_lamr;
            m_npq = static_cast<double>(n) * m_r * m_q;
        }
    }

    template <BitGenerator G>
    [[nodiscard]] crd::i64 sample(G& g) const noexcept
    {
        switch (m_mode)
        {
        case Mode::Constant:
            return m_constant;
        case Mode::Inversion:
            return reflect(draw_inversion(g));
        default:
            return reflect(draw_btpe(g));
        }
    }

private:
    enum class Mode
    {
        Constant,
        Inversion,
        Btpe
    };

    [[nodiscard]] crd::i64 reflect(crd::i64 y) const noexcept { return m_flip ? m_n - y : y; }

    // BINV — invert from the smaller tail (q^n must not underflow; reflection in the ctor guarantees r<=0.5).
    template <BitGenerator G>
    [[nodiscard]] crd::i64 draw_inversion(G& g) const noexcept
    {
        for (;;)
        {
            crd::i64 x = 0;
            double px = m_qn;
            double u = next_double(g);
            bool overflow = false;
            while (u > px)
            {
                u -= px;
                ++x;
                if (static_cast<double>(x) > m_bound)
                {
                    overflow = true; // numerical guard ⇒ resample
                    break;
                }
                px *= (static_cast<double>(m_n - x + 1) * m_r) / (static_cast<double>(x) * m_q);
            }
            if (!overflow)
            {
                return x;
            }
        }
    }

    // BTPE (Kachitvichyanukul-Schmeiser) — returns the UN-reflected y.
    template <BitGenerator G>
    [[nodiscard]] crd::i64 draw_btpe(G& g) const noexcept
    {
        crd::i64 y = 0;
        for (;;)
        {
            const double u = next_double(g) * m_p4;
            double v = next_double(g);
            if (u <= m_p1) // triangular region
            {
                y = static_cast<crd::i64>(m_xm - m_p1 * v + u);
                break;
            }
            bool reject = false;
            if (u <= m_p2) // parallelogram
            {
                const double x = m_xl + (u - m_p1) / m_c;
                v = v * m_c + 1.0 - crd::math::fabs(static_cast<double>(m_m) - x + 0.5) / m_p1;
                if (v > 1.0 || v <= 0.0)
                {
                    continue;
                }
                y = static_cast<crd::i64>(x);
            }
            else if (u <= m_p3) // left tail
            {
                y = static_cast<crd::i64>(m_xl + crd::math::log(v) / m_laml);
                if (y < 0)
                {
                    continue;
                }
                v = v * (u - m_p2) * m_laml;
            }
            else // right tail
            {
                y = static_cast<crd::i64>(m_xr - crd::math::log(v) / m_lamr);
                if (y > m_n)
                {
                    continue;
                }
                v = v * (u - m_p3) * m_lamr;
            }
            // acceptance: squeeze then exact (Stirling).
            const crd::i64 k = y > m_m ? y - m_m : m_m - y;
            if (k <= 20 || static_cast<double>(k) >= 0.5 * m_npq - 1.0)
            {
                const double s = m_r / m_q;
                const double a2 = s * static_cast<double>(m_n + 1);
                double f = 1.0;
                if (m_m < y)
                {
                    for (crd::i64 i = m_m + 1; i <= y; ++i)
                    {
                        f *= (a2 / static_cast<double>(i) - s);
                    }
                }
                else if (m_m > y)
                {
                    for (crd::i64 i = y + 1; i <= m_m; ++i)
                    {
                        f /= (a2 / static_cast<double>(i) - s);
                    }
                }
                reject = v > f;
            }
            else
            {
                const double amaxp =
                    (static_cast<double>(k) / m_npq) *
                    ((static_cast<double>(k) * (static_cast<double>(k) / 3.0 + 0.625) + 0.1666666666666) / m_npq + 0.5);
                const double ynorm = -static_cast<double>(k * k) / (2.0 * m_npq);
                const double alpha = crd::math::log(v);
                if (alpha < ynorm - amaxp)
                {
                    reject = false; // accept
                }
                else if (alpha > ynorm + amaxp)
                {
                    reject = true;
                }
                else
                {
                    const double x1 = static_cast<double>(y) + 1.0;
                    const double f1 = static_cast<double>(m_m) + 1.0;
                    const double z = static_cast<double>(m_n) + 1.0 - static_cast<double>(m_m);
                    const double w = static_cast<double>(m_n) - static_cast<double>(y) + 1.0;
                    const double z2 = z * z;
                    const double x2 = x1 * x1;
                    const double f2 = f1 * f1;
                    const double w2 = w * w;
                    const double t =
                        m_xm * crd::math::log(f1 / x1) + (static_cast<double>(m_n - m_m) + 0.5) * crd::math::log(z / w) +
                        static_cast<double>(y - m_m) * crd::math::log(w * m_r / (x1 * m_q)) +
                        (13680.0 - (462.0 - (132.0 - (99.0 - 140.0 / f2) / f2) / f2) / f2) / f1 / 166320.0 +
                        (13680.0 - (462.0 - (132.0 - (99.0 - 140.0 / z2) / z2) / z2) / z2) / z / 166320.0 -
                        (13680.0 - (462.0 - (132.0 - (99.0 - 140.0 / x2) / x2) / x2) / x2) / x1 / 166320.0 -
                        (13680.0 - (462.0 - (132.0 - (99.0 - 140.0 / w2) / w2) / w2) / w2) / w / 166320.0;
                    reject = alpha > t;
                }
            }
            if (!reject)
            {
                break;
            }
        }
        return y;
    }

    crd::i64 m_n;
    crd::i64 m_m = 0;
    crd::i64 m_constant = 0;
    Mode m_mode = Mode::Constant;
    bool m_flip = false;
    double m_r = 0.0;
    double m_q = 0.0;
    double m_qn = 0.0;
    double m_bound = 0.0;
    double m_p1 = 0.0;
    double m_xm = 0.0;
    double m_xl = 0.0;
    double m_xr = 0.0;
    double m_c = 0.0;
    double m_laml = 0.0;
    double m_lamr = 0.0;
    double m_p2 = 0.0;
    double m_p3 = 0.0;
    double m_p4 = 0.0;
    double m_npq = 0.0;
};

// Binomial(n, p) — one-off draw (recomputes the setup each call). For many draws of one (n,p), hold a BinomialSampler.
template <BitGenerator G>
[[nodiscard]] crd::i64 binomial(G& g, crd::i64 n, double p) noexcept
{
    return BinomialSampler(n, p).sample(g);
}

// Geometric(p) — number of failures before the first success (NumPy convention).
template <BitGenerator G>
[[nodiscard]] crd::i64 geometric(G& g, double p) noexcept
{
    if (p >= 1.0)
    {
        return 0;
    }
    return static_cast<crd::i64>(crd::math::floor(crd::math::log(1.0 - next_double(g)) / crd::math::log(1.0 - p)));
}

// Vose's alias method — O(1) sampling from an arbitrary categorical distribution.
class AliasTable
{
public:
    AliasTable() = default;

    // Build from (unnormalized) non-negative weights.
    void build(crd::containers::Span<const double> weights, crd::memory::IAllocator* alloc) noexcept
    {
        const crd::usize n = weights.size();
        m_prob = crd::containers::Array<double>(alloc);
        m_alias = crd::containers::Array<crd::u32>(alloc);
        m_prob.resize(n);
        m_alias.resize(n);
        double sum = 0.0;
        for (double w : weights)
        {
            sum += w;
        }
        crd::containers::Array<double> scaled(alloc);
        crd::containers::Array<crd::u32> small(alloc);
        crd::containers::Array<crd::u32> large(alloc);
        scaled.resize(n);
        for (crd::usize i = 0; i < n; ++i)
        {
            scaled[i] = weights[i] / sum * static_cast<double>(n);
            (scaled[i] < 1.0 ? small : large).push_back(static_cast<crd::u32>(i));
        }
        while (small.size() > 0 && large.size() > 0)
        {
            const crd::u32 s = small[small.size() - 1];
            small.pop_back();
            const crd::u32 l = large[large.size() - 1];
            large.pop_back();
            m_prob[s] = scaled[s];
            m_alias[s] = l;
            scaled[l] = (scaled[l] + scaled[s]) - 1.0;
            (scaled[l] < 1.0 ? small : large).push_back(l);
        }
        while (large.size() > 0)
        {
            m_prob[large[large.size() - 1]] = 1.0;
            large.pop_back();
        }
        while (small.size() > 0)
        {
            m_prob[small[small.size() - 1]] = 1.0;
            small.pop_back();
        }
    }

    template <BitGenerator G>
    [[nodiscard]] crd::u32 sample(G& g) const noexcept
    {
        const crd::usize n = m_prob.size();
        const auto col = static_cast<crd::u32>(bounded(g, static_cast<crd::u64>(n)));
        return next_double(g) < m_prob[col] ? col : m_alias[col];
    }

private:
    crd::containers::Array<double> m_prob;
    crd::containers::Array<crd::u32> m_alias;
};

// Reservoir sampling (Algorithm R): k items uniformly from a stream of unknown length, one pass.
template <BitGenerator G, typename T>
inline void reservoir_sample(G& g, crd::containers::Span<const T> stream, crd::containers::Span<T> out) noexcept
{
    const crd::usize k = out.size();
    crd::usize i = 0;
    for (; i < k && i < stream.size(); ++i)
    {
        out[i] = stream[i];
    }
    for (; i < stream.size(); ++i)
    {
        const crd::u64 j = bounded(g, static_cast<crd::u64>(i) + 1U);
        if (j < static_cast<crd::u64>(k))
        {
            out[static_cast<crd::usize>(j)] = stream[i];
        }
    }
}

} // namespace crd::hesap::stats
