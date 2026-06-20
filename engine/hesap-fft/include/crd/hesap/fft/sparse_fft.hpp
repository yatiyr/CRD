#pragma once
// v10-h — Sparse FFT (Hassanieh-Indyk-Katabi-Price, SODA 2012 "Simple and Practical"), END-TO-END SUB-LINEAR and
// NOISE-ROBUST. For a spectrum with k significant frequencies (exactly sparse OR k dominant tones + noise), recover
// them in ~O(R·log n·(w + B log B)) — NO O(n) step anywhere.
//
// Each round: pick a random odd permutation σ, and bucketise the (permuted, flat-filtered, folded) signal into B
// bins via a B-point FFT at log₂(n)+1 spatial offsets. The reference offset b=0 plus the dyadic offsets
// β_j = 2^{m-j-1} give, for a bin holding one tone f,  ẑ_{β}[t]/ẑ_0[t] = e^{2πi β f/n}, and at β_j the phase is
// π·f/2^j — whose sign (after correcting for the already-found low bits) IS bit j of f. So f is recovered BIT BY
// BIT, each bit from a coarse ±π/2-robust phase (vs the noise-fragile single-offset ratio). A bin's tone is then
// VOTED across rounds (consistent f ⇒ real tone; collisions/noise ⇒ inconsistent), and its coefficient is the
// component-wise MEDIAN of the per-round bucket estimates n·ẑ_0[t]/Ĝ(σf − t·n/B) — robust to leakage and noise.
//
// The flat filter G is a Gaussian-windowed sinc: Ĝ ≈ 1 over a width-n/B band with Gaussian-decay sidelobes.
// Deterministic (seeded σ) ⇒ reproducible. Gates: exact k-sparse (frequencies exact, coeffs to filter accuracy)
// AND noisy k-sparse (dominant tones recovered within the noise floor).
#include <crd/hesap/fft/fft.hpp>

#include <crd/containers/array.hpp>
#include <crd/containers/sort.hpp>

#include <algorithm>
#include <cmath>

namespace crd::hesap::fft
{

template <typename T> class SparseFftPlan
{
public:
    // n: signal length (power of two). k: target sparsity. rounds: voting rounds (more ⇒ more collision/noise robust).
    SparseFftPlan(crd::memory::IAllocator* alloc, crd::usize n, crd::usize k, crd::usize rounds = 8)
        : m_alloc(alloc), m_n(n), m_k(k), m_rounds(rounds), m_filter(alloc), m_bucket(alloc), m_zbuf(alloc),
          m_cand(alloc)
    {
        CRD_ASSERT(n >= 2 && (n & (n - 1)) == 0);
        CRD_ASSERT(k >= 1 && rounds >= 1);
        m_m = 0;
        while ((crd::usize{1} << m_m) < n)
        {
            ++m_m; // m = log2(n)
        }
        m_b = 1;
        while (m_b < 8 * k) // B ≈ 8k buckets (pow-2): rare collisions AND a narrow n/B band ⇒ low per-bucket noise
        {
            m_b <<= 1;
        }
        if (m_b > n)
        {
            m_b = n;
        }
        m_plan = static_cast<FftPlan<T>*>(m_alloc->allocate(sizeof(FftPlan<T>), alignof(FftPlan<T>)));
        ::new (static_cast<void*>(m_plan)) FftPlan<T>(m_alloc, m_b);

        // Flat filter G = sinc(·/B)·Gaussian(·): the sinc sets an n/B passband, the Gaussian window gives low
        // sidelobes (little leakage). Support w (sub-linear when w ≪ n). Normalised so the passband gain Ĝ(0)=1.
        constexpr double pi = 3.14159265358979323846;
        m_w = m_b * 16;
        if (m_w > n)
        {
            m_w = n;
        }
        m_filter.resize(m_w);
        const double s = static_cast<double>(m_w) / 8.0;
        const double c = static_cast<double>(m_w) / 2.0;
        double sum = 0.0;
        for (crd::usize i = 0; i < m_w; ++i)
        {
            const double xp = static_cast<double>(i) - c;
            const double sinc = (std::abs(xp) < 1e-12)
                                    ? 1.0
                                    : std::sin(pi * xp / static_cast<double>(m_b)) / (pi * xp / static_cast<double>(m_b));
            const double g = sinc * std::exp(-0.5 * (xp / s) * (xp / s));
            m_filter[i] = static_cast<T>(g);
            sum += g;
        }
        for (crd::usize i = 0; i < m_w; ++i)
        {
            m_filter[i] = static_cast<T>(static_cast<double>(m_filter[i]) / sum);
        }
        m_bucket.resize(m_b);
        m_zbuf.resize((m_m + 1) * m_b); // [0]=offset 0 (reference); [j+1]=offset β_j = 2^{m-j-1}
        m_cand.resize(m_rounds * m_b);  // (f, coeff) votes
    }

    ~SparseFftPlan()
    {
        if (m_plan != nullptr)
        {
            m_plan->~FftPlan();
            m_alloc->deallocate(m_plan);
        }
    }

    SparseFftPlan(const SparseFftPlan&) = delete;
    SparseFftPlan& operator=(const SparseFftPlan&) = delete;
    SparseFftPlan(SparseFftPlan&&) = delete;
    SparseFftPlan& operator=(SparseFftPlan&&) = delete;

    // Recover up to k dominant frequencies of x. Writes (freq, coeff = x̂[freq]) and returns the count found.
    // Sub-linear: touches w + B samples per offset, no O(n) pass. `seed` makes the recovery reproducible.
    crd::usize recover(crd::containers::ConstSpan<Complex<T>> x, crd::containers::Span<crd::usize> freqs,
                       crd::containers::Span<Complex<T>> coeffs, crd::u64 seed = 0x243F6A8885A308D3ULL) const
    {
        CRD_ASSERT(x.size() == m_n);
        constexpr double pi = 3.14159265358979323846;
        const Complex<T>* xp = x.data();
        crd::usize ncand = 0;
        crd::u64 rng = seed;

        for (crd::usize round = 0; round < m_rounds; ++round)
        {
            rng = rng * 6364136223846793005ULL + 1442695040888963407ULL;
            const crd::usize sigma = ((rng >> 16) | 1ULL) & (m_n - 1); // random odd permutation stride
            bucketize(xp, sigma, 0, m_zbuf.data());                    // reference (offset 0)
            for (crd::usize j = 0; j < m_m; ++j)                       // dyadic offsets β_j = 2^{m-j-1}
            {
                const crd::usize beta = crd::usize{1} << (m_m - j - 1);
                bucketize(xp, sigma, beta, m_zbuf.data() + (j + 1) * m_b);
            }
            double zmax = 0.0;
            for (crd::usize t = 0; t < m_b; ++t)
            {
                zmax = std::max(zmax, std::hypot(static_cast<double>(m_zbuf[t].re), static_cast<double>(m_zbuf[t].im)));
            }
            const double thresh = 1e-3 * zmax + 1e-300; // a bin holds a tone if it carries energy

            for (crd::usize t = 0; t < m_b; ++t)
            {
                const double z0r = static_cast<double>(m_zbuf[t].re);
                const double z0i = static_cast<double>(m_zbuf[t].im);
                if (std::hypot(z0r, z0i) < thresh)
                {
                    continue;
                }
                // multi-scale binary location: bit j of f from the sign of the (low-bit-corrected) β_j phase.
                crd::usize f = 0;
                for (crd::usize j = 0; j < m_m; ++j)
                {
                    const double zbr = static_cast<double>(m_zbuf[(j + 1) * m_b + t].re);
                    const double zbi = static_cast<double>(m_zbuf[(j + 1) * m_b + t].im);
                    const double ph = std::atan2(zbi * z0r - zbr * z0i, zbr * z0r + zbi * z0i); // angle(z_β/z_0)
                    const double corr = pi * static_cast<double>(f & ((crd::usize{1} << j) - 1)) /
                                        static_cast<double>(crd::usize{1} << j);
                    if (std::cos(ph - corr) < 0.0) // ph − corr ≈ π·bit_j  ⇒  bit set when cos < 0 (±π/2 robust)
                    {
                        f |= (crd::usize{1} << j);
                    }
                }
                // VERIFY: f must hash back to this bin (round(σf·B/n) == t). Collisions ⇒ garbage f ⇒ rejected.
                const long long bh =
                    llround(static_cast<double>((sigma * f) % m_n) * static_cast<double>(m_b) /
                            static_cast<double>(m_n)) %
                    static_cast<long long>(m_b);
                if (bh != static_cast<long long>(t))
                {
                    continue;
                }
                // per-round coefficient estimate x̂[f] = n·ẑ_0[t]/Ĝ(σf − t·n/B) — averaged by the final median.
                long long nu = static_cast<long long>((sigma * f) % m_n) - static_cast<long long>(t * (m_n / m_b));
                nu = ((nu % static_cast<long long>(m_n)) + static_cast<long long>(m_n)) % static_cast<long long>(m_n);
                if (nu > static_cast<long long>(m_n) / 2)
                {
                    nu -= static_cast<long long>(m_n);
                }
                double gr = 0.0;
                double gi = 0.0;
                filter_freq(nu, gr, gi);
                const double gd = gr * gr + gi * gi;
                if (gd < 1e-6)
                {
                    continue;
                }
                const double nn = static_cast<double>(m_n);
                m_cand[ncand].f = f;
                m_cand[ncand].cr = nn * (z0r * gr + z0i * gi) / gd;
                m_cand[ncand].ci = nn * (z0i * gr - z0r * gi) / gd;
                ++ncand;
            }
        }

        // CONSOLIDATE: group the votes by frequency, take the k with the most votes, coefficient = component median.
        crd::containers::sort(m_cand.data(), m_cand.data() + ncand, [](const Cand& a, const Cand& b) { return a.f < b.f; });
        crd::usize nfound = 0;
        crd::usize i = 0;
        // collect (freq, votecount, median coeff) into the front of m_cand-derived scratch (reuse freqs/coeffs first,
        // then prune to top-k). We over-collect then select the k highest-voted.
        crd::containers::Array<crd::usize> gf(m_alloc);
        crd::containers::Array<crd::usize> gc(m_alloc);
        crd::containers::Array<T> gr(m_alloc);
        crd::containers::Array<T> gi(m_alloc);
        while (i < ncand)
        {
            crd::usize j = i;
            while (j < ncand && m_cand[j].f == m_cand[i].f)
            {
                ++j;
            }
            // median of cr and ci over [i, j)
            const crd::usize cnt = j - i;
            crd::containers::sort(m_cand.data() + i, m_cand.data() + j, [](const Cand& a, const Cand& b) { return a.cr < b.cr; });
            const double mr = m_cand[i + cnt / 2].cr;
            crd::containers::sort(m_cand.data() + i, m_cand.data() + j, [](const Cand& a, const Cand& b) { return a.ci < b.ci; });
            const double mi = m_cand[i + cnt / 2].ci;
            gf.push_back(m_cand[i].f);
            gc.push_back(cnt);
            gr.push_back(static_cast<T>(mr));
            gi.push_back(static_cast<T>(mi));
            i = j;
        }
        // select the k groups with the most votes.
        const crd::usize ng = gf.size();
        crd::containers::Array<crd::usize> order(m_alloc);
        order.resize(ng);
        for (crd::usize a = 0; a < ng; ++a)
        {
            order[a] = a;
        }
        crd::containers::sort(order.data(), order.data() + ng,
                  [&gc](crd::usize a, crd::usize b) { return gc[a] > gc[b]; });
        const crd::usize lim = std::min(m_k, freqs.size());
        for (crd::usize a = 0; a < ng && nfound < lim; ++a)
        {
            const crd::usize idx = order[a];
            freqs[nfound] = gf[idx];
            coeffs[nfound] = Complex<T>{gr[idx], gi[idx]};
            ++nfound;
        }
        return nfound;
    }

    [[nodiscard]] crd::usize size() const noexcept { return m_n; }
    [[nodiscard]] crd::usize buckets() const noexcept { return m_b; }
    [[nodiscard]] crd::usize filter_support() const noexcept { return m_w; }

private:
    struct Cand
    {
        crd::usize f;
        double cr;
        double ci;
    };

    void bucketize(const Complex<T>* x, crd::usize sigma, crd::usize b0, Complex<T>* zout) const
    {
        for (crd::usize t = 0; t < m_b; ++t)
        {
            m_bucket[t] = Complex<T>{T(0), T(0)};
        }
        crd::usize idx = b0 & (m_n - 1);
        for (crd::usize i = 0; i < m_w; ++i)
        {
            const T g = m_filter[i];
            Complex<T>& bk = m_bucket[i & (m_b - 1)];
            bk.re += g * x[idx].re;
            bk.im += g * x[idx].im;
            idx = (idx + sigma) & (m_n - 1);
        }
        for (crd::usize t = 0; t < m_b; ++t)
        {
            zout[t] = m_bucket[t];
        }
        m_plan->execute(crd::containers::Span<Complex<T>>(zout, m_b), FftDirection::Forward);
    }

    // Ĝ(ν) = Σ_i filter[i] e^{2πi i ν / n}  (ν integer) — the filter's frequency response, for the bucket estimate.
    void filter_freq(long long nu, double& gr, double& gi) const
    {
        const double w = 2.0 * 3.14159265358979323846 * static_cast<double>(nu) / static_cast<double>(m_n);
        double re = 0.0;
        double im = 0.0;
        for (crd::usize i = 0; i < m_w; ++i)
        {
            const double a = w * static_cast<double>(i);
            const double f = static_cast<double>(m_filter[i]);
            re += f * std::cos(a);
            im += f * std::sin(a);
        }
        gr = re;
        gi = im;
    }

    crd::memory::IAllocator* m_alloc;
    crd::usize m_n;
    crd::usize m_k;
    crd::usize m_rounds;
    crd::usize m_m = 0; // log2(n)
    crd::usize m_b = 1; // buckets
    crd::usize m_w = 0; // filter support
    FftPlan<T>* m_plan = nullptr;
    crd::containers::Array<T> m_filter;
    mutable crd::containers::Array<Complex<T>> m_bucket;
    mutable crd::containers::Array<Complex<T>> m_zbuf; // (m+1)·B multi-offset bucket spectra
    mutable crd::containers::Array<Cand> m_cand;       // votes
};

} // namespace crd::hesap::fft
