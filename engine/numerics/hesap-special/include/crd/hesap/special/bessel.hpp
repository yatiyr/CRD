#pragma once

// crd-hesap-special v12-b — Bessel & Hankel functions (real order, real argument), gold-standard.
//
// Cylindrical J_ν, Y_ν, I_ν, K_ν (+ derivatives) by the Steed/Temme method (Temme 1975, J. Comp. Phys. 19 for K; Temme
// 1976, J. Comput. Phys. 21 for Y; Barnett, Feng, Steed and Goldfarb 1974; Thompson and Barnett 1986; DLMF chapter 10),
// organised the way Numerical Recipes §6.7 organises `bessjy`/`bessik`. The code text below is Cerid's own, written
// from those papers; the Chebyshev table of bes_temme_gammas and the step-count rule of bes_base_steps/bes_jy_steps
// keep their Numerical Recipes origin. CF1 (Lentz continued fraction) gives the logarithmic derivative, a downward
// recurrence carries it to a base order μ ∈ [−½,½], then Temme's power series (x small) or Steed's CF2 (x large) gives
// the base values, which the Wronskian normalises, and an upward recurrence in order finishes. No fragile
// rational-coefficient tables, only continued fractions and a tiny Chebyshev fit for the Γ-ratios near μ=0. Accurate to
// ~1e-14 over the whole range. Spherical j_n/y_n (closed-form recurrence), Hankel H^(1,2) = J ± iY. Internals compute
// in f64; the public templates cast for f32. Gated vs scipy.special (jv/yv/iv/kv/spherical_jn/yn) to <1e-13.

#include <crd/hesap/special/gamma.hpp> // Real concept

#include <crd/math/cmath.hpp>
#include <complex>
#include <limits>

namespace crd::hesap::special
{
namespace detail
{
inline constexpr double kBesPi = 3.14159265358979323846264338327950288;
inline constexpr double kBesRelTol = 1.0e-16; // convergence test of the series and fractions; small-argument limit
inline constexpr double kBesTiny = 1.0e-300;  // floor that keeps Lentz's tails and the unnormalised seed off zero
inline constexpr double kBesTemmeLimit = 2.0; // below: Temme's series; from here on: Steed's CF2
inline constexpr int kBesIterCap = 10000;

// Small value types: a pair of neighbouring orders, the four results of the J/Y and I/K cores, a complex number, and
// the Γ-ratio set of Temme's series.
struct BesPair
{
    double base; // order μ (or ν)
    double next; // order μ+1 (or ν+1)
};
struct BesJY
{
    double j;
    double y;
    double jp;
    double yp;
};
struct BesIK
{
    double i;
    double k;
    double ip;
    double kp;
};
struct BesZ
{
    double re;
    double im;
};
struct BesGammas
{
    double g1;              // Γ1(μ) = (1/Γ(1−μ) − 1/Γ(1+μ))/(2μ)
    double g2;              // Γ2(μ) = (1/Γ(1−μ) + 1/Γ(1+μ))/2
    double inv_gamma_plus;  // 1/Γ(1+μ) = Γ2 − μΓ1
    double inv_gamma_minus; // 1/Γ(1−μ) = Γ2 + μΓ1
};

[[nodiscard]] inline BesZ bes_z_mul(BesZ a, BesZ b) noexcept
{
    return {a.re * b.re - a.im * b.im, a.re * b.im + a.im * b.re};
}

[[nodiscard]] inline BesZ bes_z_inv(BesZ a) noexcept
{
    const double norm = a.re * a.re + a.im * a.im;
    return {a.re / norm, -a.im / norm};
}

// r/z for real r: r·conj(z)/|z|².
[[nodiscard]] inline BesZ bes_z_real_over(double r, BesZ z) noexcept
{
    const double ratio = r / (z.re * z.re + z.im * z.im);
    return {z.re * ratio, -z.im * ratio};
}

// The Γ-ratio combinations of Temme (1975; 1976) for |μ| ≤ ½. Γ1 and Γ2 are even in μ, so each is a Chebyshev series in
// t = 8μ² − 1 ∈ [−1, 1]; the fit has no 0/0 cancellation at μ → 0, which integer orders need (their base order is
// exactly 0). The coefficients are the table that Numerical Recipes (2nd ed., §6.7, `beschb`) prints for these two
// fits. Both series are summed together by Clenshaw's recurrence (Clenshaw 1955): ½c_0 + Σ_{k≥1} c_k T_k(t); the Γ1
// table has seven entries, and its eighth, zero, entry lets one loop serve both.
[[nodiscard]] inline BesGammas bes_temme_gammas(double mu) noexcept
{
    static constexpr double kFitGamma1[8] = {
        -1.142022680371168e0, 6.5165112670737e-3, 3.087090173086e-4, -3.4706269649e-6,
        6.9437664e-9,         3.67795e-11,        -1.356e-13,        0.0};
    static constexpr double kFitGamma2[8] = {
        1.843740587300905e0, -7.68528408447867e-2, 1.2719271366546e-3, -4.9717367042e-6,
        -3.31261198e-8,      2.423096e-10,         -1.702e-13,         -1.49e-15};
    const double t = 8.0 * mu * mu - 1.0;
    const double two_t = t + t;
    double a1 = 0.0; // Clenshaw b_{k+1} of the Γ1 series
    double a2 = 0.0; // b_{k+2}
    double c1 = 0.0; // the same pair for the Γ2 series
    double c2 = 0.0;
    for (int k = 7; k >= 1; --k)
    {
        const double a0 = kFitGamma1[k] + two_t * a1 - a2;
        const double c0 = kFitGamma2[k] + two_t * c1 - c2;
        a2 = a1;
        a1 = a0;
        c2 = c1;
        c1 = c0;
    }
    const double g1 = 0.5 * kFitGamma1[0] + t * a1 - a2;
    const double g2 = 0.5 * kFitGamma2[0] + t * c1 - c2;
    return {g1, g2, g2 - mu * g1, g2 + mu * g1};
}

// Downward-recurrence count that puts the base order μ = ν − n in [−½, ½] (the step rule NR's bessjy/bessik use for J,
// Y, I and K). The C cast truncates the half-offset sum toward zero, which is round-half-up for ν ≥ 0. It is kept
// exactly, as an explicit trunc of the same sum, rather than replaced by lround: ν = 0.49999999999999994 gives ν + 0.5
// == 1.0 in binary64 (cast → 1, lround → 0), and the cyl_*_prime entry points pass a negative ν through unreflected,
// where lround's half-away-from-zero differs from truncation. Either substitution moves μ for those orders and changes
// the results bit for bit, which the hesap bit-exact contract forbids.
[[nodiscard]] inline int bes_base_steps(double xnu) noexcept
{
    return static_cast<int>(crd::math::trunc(xnu + 0.5));
}

// Step count for J and Y (the rule NR's bessjy uses): below the Temme/CF2 switch it is the base count above; from the
// switch on, the recurrence starts from an order about one below x, so that CF2 converges quickly, and the count is
// floored at zero. The 1.5 offset is a shift, not a rounding, and stays as written.
[[nodiscard]] inline int bes_jy_steps(double x, double xnu) noexcept
{
    if (x < kBesTemmeLimit)
    {
        return bes_base_steps(xnu);
    }
    const int shifted = static_cast<int>(xnu - x + 1.5);
    return (shifted > 0) ? shifted : 0;
}

// Result of the J fraction: the ratio J'_ν/J_ν and the sign of the unnormalised J_ν it was found with.
struct BesCf1
{
    double ratio;
    int sign;
};

// CF1 for the logarithmic derivative (Barnett et al. 1974). The order recurrence w_{ν−1} + s·w_{ν+1} = (2ν/x)w_ν,
// with s = +1 for J and −1 for I, gives
//   J'/J = ν/x − 1/(2(ν+1)/x − 1/(2(ν+2)/x − …)),   I'/I = ν/x + 1/(2(ν+1)/x + 1/(2(ν+2)/x + …)).
// Both are evaluated by Lentz's modified algorithm (Lentz 1976; Thompson & Barnett 1986): the tails are
// C_k = b_k + t/C_{k−1} and 1/D_k = b_k + t·D_{k−1}, with b_k = 2(ν+k)/x and t = −1 for J, +1 for I, and the value is
// multiplied by C_k·D_k until that factor is 1. Each b_k is formed from its index, so no rounding accumulates over the
// thousands of steps a large argument needs.

// The J fraction. A denominator can vanish or turn negative, so both tails are held off zero, and every negative D_k
// flips the sign of the unnormalised J (the parity of the flips is returned as a sign).
[[nodiscard]] inline BesCf1 bes_cf1_oscillating(double inv_x, double nu) noexcept
{
    const double spacing = 2.0 * inv_x;
    double value = nu * inv_x;
    if (value < kBesTiny)
    {
        value = kBesTiny;
    }
    double num_tail = value;
    double den_tail = 0.0;
    double index = 0.0;
    unsigned flips = 0U;
    for (int step = 1; step <= kBesIterCap; ++step)
    {
        index += 1.0;
        const double b = spacing * (nu + index);
        double den = b - den_tail;
        double num = b - 1.0 / num_tail;
        // Both floors sit behind one rarely taken test on the smaller magnitude, so no select lengthens the
        // dependency chain.
        const double den_mag = crd::math::fabs(den);
        const double num_mag = crd::math::fabs(num);
        const double smaller = (den_mag < num_mag) ? den_mag : num_mag;
        if (smaller < kBesTiny) [[unlikely]]
        {
            if (den_mag < kBesTiny)
            {
                den = kBesTiny;
            }
            if (num_mag < kBesTiny)
            {
                num = kBesTiny;
            }
        }
        den_tail = 1.0 / den;
        num_tail = num;
        if (den_tail < 0.0)
        {
            ++flips;
        }
        const double factor = num_tail * den_tail;
        value *= factor;
        if (crd::math::fabs(factor - 1.0) < kBesRelTol)
        {
            break;
        }
    }
    return {value, ((flips & 1U) != 0U) ? -1 : 1};
}

// The I fraction. Every tail is positive, so neither a floor nor a sign is needed. The D recurrence is advanced before
// the C recurrence.
[[nodiscard]] inline double bes_cf1_growing(double inv_x, double nu) noexcept
{
    const double spacing = 2.0 * inv_x;
    double value = nu * inv_x;
    if (value < kBesTiny)
    {
        value = kBesTiny;
    }
    double num_tail = value;
    double den_tail = 0.0;
    double index = 0.0;
    for (int step = 1; step <= kBesIterCap; ++step)
    {
        index += 1.0;
        const double b = spacing * (nu + index);
        den_tail = 1.0 / (b + den_tail);
        num_tail = b + 1.0 / num_tail;
        const double factor = num_tail * den_tail;
        value *= factor;
        if (crd::math::fabs(factor - 1.0) < kBesRelTol)
        {
            break;
        }
    }
    return value;
}

// Steed's CF2 for p + iq = H^(1)'_μ(x)/H^(1)_μ(x) (x ≥ 2, |μ| ≤ ½; Barnett et al. 1974, Thompson & Barnett 1986):
//   p + iq = −1/(2x) + i + (i/x)·a_1/(b_1 + a_2/(b_2 + …)),  a_k = (k − ½)² − μ², b_k = 2(x + ik),
// summed by the modified Lentz recurrence on complex terms (tails C_k = b_k + a_k/C_{k−1}, 1/D_k = b_k + a_k·D_{k−1},
// running value multiplied by C_k·D_k). The leading term −1/(2x) + i plays the role of b_0, and the first partial
// numerator i·a_1/x is imaginary, so the first factor is formed on its own and the loop covers the real a_k, k ≥ 2.
[[nodiscard]] inline BesZ bes_cf2_hankel(double x, double mu2) noexcept
{
    const double inv_x = 1.0 / x;
    BesZ value{-0.5 * inv_x, 1.0};
    BesZ b{2.0 * x, 2.0};
    const BesZ lead = bes_z_mul(BesZ{0.0, (0.25 - mu2) * inv_x}, bes_z_inv(value));
    BesZ num_tail{b.re + lead.re, b.im + lead.im};
    BesZ den_tail = bes_z_inv(b);
    BesZ factor = bes_z_mul(num_tail, den_tail);
    value = bes_z_mul(value, factor);
    for (int k = 2; k <= kBesIterCap; ++k)
    {
        const double a = (k - 0.5) * (k - 0.5) - mu2;
        b.im += 2.0;
        BesZ den{b.re + a * den_tail.re, b.im + a * den_tail.im};
        const BesZ back = bes_z_real_over(a, num_tail);
        num_tail = {b.re + back.re, b.im + back.im};
        // Both floors sit behind one rarely taken test on the smaller magnitude, so no select lengthens the
        // dependency chain.
        const double den_mag = crd::math::fabs(den.re) + crd::math::fabs(den.im);
        const double num_mag = crd::math::fabs(num_tail.re) + crd::math::fabs(num_tail.im);
        const double smaller = (den_mag < num_mag) ? den_mag : num_mag;
        if (smaller < kBesTiny) [[unlikely]]
        {
            if (den_mag < kBesTiny)
            {
                den.re = kBesTiny;
            }
            if (num_mag < kBesTiny)
            {
                num_tail.re = kBesTiny;
            }
        }
        den_tail = bes_z_inv(den);
        factor = bes_z_mul(num_tail, den_tail);
        value = bes_z_mul(value, factor);
        if (crd::math::fabs(factor.re - 1.0) + crd::math::fabs(factor.im) < kBesRelTol)
        {
            break;
        }
    }
    return value;
}

// Quantities shared by Temme's series for Y_μ (Temme 1976) and for K_μ (Temme 1975), with σ = μ ln(2/x):
//   f_0 = (πμ/sin πμ)·[Γ1(μ) cosh σ + Γ2(μ)·(sinh σ/σ)·ln(2/x)]   (Y carries an extra factor 2/π),
// together with e^σ, 1/Γ(1±μ) and πμ. The two removable singularities (πμ → 0 and σ → 0) take their limit 1.
struct BesTemmeHead
{
    double f0;
    double e_sigma;
    double inv_gamma_plus;
    double inv_gamma_minus;
    double mu_pi;
};

[[nodiscard]] inline BesTemmeHead bes_temme_head(double x, double mu) noexcept
{
    const double mu_pi = kBesPi * mu;
    const double ln_2_over_x = -crd::math::log(0.5 * x);
    const double sigma = mu * ln_2_over_x;
    const double x_over_sin = (crd::math::fabs(mu_pi) < kBesRelTol) ? 1.0 : mu_pi / crd::math::sin(mu_pi);
    const double sinhc = (crd::math::fabs(sigma) < kBesRelTol) ? 1.0 : crd::math::sinh(sigma) / sigma;
    const BesGammas g = bes_temme_gammas(mu);
    return {x_over_sin * (g.g1 * crd::math::cosh(sigma) + g.g2 * sinhc * ln_2_over_x), crd::math::exp(sigma),
            g.inv_gamma_plus, g.inv_gamma_minus, mu_pi};
}

// Temme's series (Temme 1976) for Y_μ(x) and Y_{μ+1}(x), x < 2, |μ| ≤ ½ (DLMF 10.8):
//   Y_μ = −Σ c_k g_k,  Y_{μ+1} = −(2/x) Σ c_k h_k,  c_k = (−x²/4)^k/k!,
//   g_k = f_k + (2/μ)sin²(μπ/2)·q_k,  h_k = p_k − k·g_k,
// with the coupling f_k = (k f_{k−1} + p_{k−1} + q_{k−1})/(k² − μ²), p_k = p_{k−1}/(k − μ), q_k = q_{k−1}/(k + μ).
// One reciprocal of k² − μ² serves the f, p and q updates: 1/(k∓μ) = (k±μ)/(k² − μ²).
[[nodiscard]] inline BesPair bes_temme_y(double x, double mu) noexcept
{
    const BesTemmeHead s = bes_temme_head(x, mu);
    const double half_mu_pi = 0.5 * s.mu_pi;
    const double sinc = (crd::math::fabs(half_mu_pi) < kBesRelTol) ? 1.0 : crd::math::sin(half_mu_pi) / half_mu_pi;
    const double skew = 0.5 * kBesPi * s.mu_pi * sinc * sinc; // (2/μ)sin²(μπ/2), finite at μ = 0
    const double weight = -0.25 * x * x;
    double fk = (2.0 / kBesPi) * s.f0;
    double pk = s.e_sigma / (kBesPi * s.inv_gamma_plus);
    double qk = 1.0 / (kBesPi * s.e_sigma * s.inv_gamma_minus);
    double ck = 1.0;
    double sum_g = fk + skew * qk; // Σ c_k g_k
    double sum_h = pk;             // Σ c_k h_k
    for (int k = 1; k <= kBesIterCap; ++k)
    {
        const double kd = k;
        const double inv_kk = 1.0 / ((kd - mu) * (kd + mu));
        fk = (kd * fk + pk + qk) * inv_kk;
        pk *= (kd + mu) * inv_kk;
        qk *= (kd - mu) * inv_kk;
        ck *= weight / kd;
        const double gk = fk + skew * qk;
        sum_g += ck * gk;
        sum_h += ck * (pk - kd * gk);
        if (crd::math::fabs(ck * gk) < (1.0 + crd::math::fabs(sum_g)) * kBesRelTol)
        {
            break;
        }
    }
    return {-sum_g, -2.0 * sum_h / x};
}

// Temme's series for K_μ(x) and K_{μ+1}(x), x < 2, |μ| ≤ ½ (Temme 1975; DLMF 10.31): K_μ = Σ c_k f_k,
// K_{μ+1} = (2/x) Σ c_k h_k, c_k = (x²/4)^k/k!, h_k = p_k − k f_k, with f_k, p_k, q_k as in bes_temme_y.
[[nodiscard]] inline BesPair bes_temme_k(double x, double mu) noexcept
{
    const BesTemmeHead s = bes_temme_head(x, mu);
    const double weight = 0.25 * x * x;
    double fk = s.f0;
    double pk = 0.5 * s.e_sigma / s.inv_gamma_plus;
    double qk = 0.5 / (s.e_sigma * s.inv_gamma_minus);
    double ck = 1.0;
    double sum_f = fk; // Σ c_k f_k
    double sum_h = pk; // Σ c_k h_k
    for (int k = 1; k <= kBesIterCap; ++k)
    {
        const double kd = k;
        const double inv_kk = 1.0 / ((kd - mu) * (kd + mu));
        fk = (kd * fk + pk + qk) * inv_kk;
        pk *= (kd + mu) * inv_kk;
        qk *= (kd - mu) * inv_kk;
        ck *= weight / kd;
        const double term = ck * fk;
        sum_f += term;
        sum_h += ck * (pk - kd * fk);
        if (crd::math::fabs(term) < crd::math::fabs(sum_f) * kBesRelTol)
        {
            break;
        }
    }
    return {sum_f, 2.0 * sum_h / x};
}

// Steed's CF2 for K (x ≥ 2, |μ| ≤ ½; Temme 1975, Thompson & Barnett 1986): K_μ(x) = √(π/2x)·e^{−x}/S. With
// a_k = (k − ½)² − μ² and b_k = 2(x + k), two Steed recurrences run side by side. The fraction
//   h = 1/(b_1 − a_2/(b_2 − a_3/(b_3 − …)))  is accumulated as h = Σ Δh_k, with D_k = 1/(b_k − a_k D_{k−1}) and
//   Δh_k = (b_k D_k − 1)Δh_{k−1};  the weights are c_k = a_k c_{k−1}/k  and  Q_k = (b_{k−1}Q_{k−1} − Q_{k−2})/a_k,
// and S = 1 + Σ_k (Σ_{j≤k} c_j Q_j)·Δh_k. Then K_{μ+1}/K_μ = (μ + x + ½ + (μ² − ¼)h)/x. Returns K_μ and K_{μ+1}.
[[nodiscard]] inline BesPair bes_cf2_k(double x, double mu) noexcept
{
    const double mu2 = mu * mu;
    const double a1 = 0.25 - mu2;
    double b_prev = 2.0 * (x + 1.0); // b_{k−1}
    double d = 1.0 / b_prev;         // D_1
    double dh = d;                   // Δh_1
    double h = dh;
    double q_old = 0.0;           // Q_{k−2}
    double q_cur = 1.0;           // Q_{k−1}
    double weight = a1;           // c_1
    double weighted = a1 * q_cur; // Σ_{j≤k} c_j Q_j
    double s = 1.0 + weighted * dh;
    for (int k = 2; k <= kBesIterCap; ++k)
    {
        const double kd = k;
        const double a = (kd - 0.5) * (kd - 0.5) - mu2;
        const double b = 2.0 * (x + kd);
        weight *= a / kd;
        const double q_new = (b_prev * q_cur - q_old) / a;
        q_old = q_cur;
        q_cur = q_new;
        weighted += weight * q_new;
        d = 1.0 / (b - a * d);
        dh *= b * d - 1.0;
        h += dh;
        const double ds = weighted * dh;
        s += ds;
        b_prev = b;
        if (crd::math::fabs(ds) < crd::math::fabs(s) * kBesRelTol)
        {
            break;
        }
    }
    const double kmu = crd::math::sqrt(kBesPi / (2.0 * x)) * crd::math::exp(-x) / s;
    return {kmu, kmu * (mu + x + 0.5 - a1 * h) / x};
}

// Carries a function and its neighbour down in order from ν to the base order μ = ν − steps, on the unnormalised scale
// of the CF1 seed. From w_ν (seed) and w'_ν (seed_d) the order ν−1 value is w_{ν−1} = (ν/x)w_ν + w'_ν (J and I alike);
// the order recurrence w_{m−1} = (2m/x)w_m + s·w_{m+1} (s = −1 for J, +1 for I) then lowers the pair (w_{m+1}, w_m),
// and at the end w'_μ = (μ/x)w_μ + s·w_{μ+1}. Returns {w_μ, w'_μ}.
template <bool Oscillatory>
[[nodiscard]] inline BesPair bes_lower_to_base(double seed, double seed_d, double nu, int steps, double inv_x) noexcept
{
    constexpr double kLink = Oscillatory ? -1.0 : 1.0;
    if (steps <= 0)
    {
        return {seed, seed_d};
    }
    const double two_inv_x = 2.0 * inv_x;
    const double mu = nu - steps;
    double hi = seed;                       // order m + 1
    double lo = nu * inv_x * seed + seed_d; // order m = ν − 1
    for (int j = steps - 1; j >= 1; --j)
    {
        const double next = (mu + j) * two_inv_x * lo + kLink * hi;
        hi = lo;
        lo = next;
    }
    return {lo, mu * inv_x * lo + kLink * hi};
}

// Carries a pair (w_μ, w_{μ+1}) up in order to (w_ν, w_{ν+1}), ν = μ + steps, by w_{m+1} = (2m/x)w_m + s·w_{m−1}
// (s = −1 for Y, +1 for K).
template <bool Oscillatory>
[[nodiscard]] inline BesPair bes_raise_from_base(BesPair pair, double mu, int steps, double two_inv_x) noexcept
{
    constexpr double kLink = Oscillatory ? -1.0 : 1.0;
    for (int m = 1; m <= steps; ++m)
    {
        const double upper = (mu + m) * two_inv_x * pair.next + kLink * pair.base;
        pair.base = pair.next;
        pair.next = upper;
    }
    return pair;
}

// J_ν, Y_ν, J'_ν, Y'_ν for x > 0, ν ≥ 0 by the Steed/Temme method (Barnett, Feng, Steed and Goldfarb 1974; Temme 1976;
// Thompson & Barnett 1986): CF1 gives J'/J at order ν, a downward recurrence carries it to the base order μ, there
// Temme's series (x < 2) or Steed's CF2 (x ≥ 2) give Y_μ and Y_{μ+1} and, with the Wronskian J Y' − J' Y = 2/(πx), J_μ;
// J and J' at order ν are rescaled from the unnormalised recurrence, and Y is carried upward to order ν.
[[nodiscard]] inline BesJY bes_jy_core(double x, double nu) noexcept
{
    const int steps = bes_jy_steps(x, nu);
    const double mu = nu - steps;
    const double inv_x = 1.0 / x;
    const BesCf1 cf1 = bes_cf1_oscillating(inv_x, nu);
    const double seed = cf1.sign * kBesTiny; // J_ν, up to the scale fixed below
    const double seed_d = cf1.ratio * seed;  // J'_ν on the same scale
    const BesPair lowered = bes_lower_to_base<true>(seed, seed_d, nu, steps, inv_x);
    const double j_raw = (lowered.base == 0.0) ? kBesRelTol : lowered.base; // unnormalised J_μ
    const double log_deriv = lowered.next / j_raw;                          // J'_μ/J_μ
    const double wronskian = 2.0 * inv_x / kBesPi;                          // J_μ Y'_μ − J'_μ Y_μ
    double j_mu = 0.0;
    BesPair y{};
    if (x < kBesTemmeLimit)
    {
        y = bes_temme_y(x, mu);
        const double y_mu_d = mu * inv_x * y.base - y.next; // Y'_μ
        j_mu = wronskian / (y_mu_d - log_deriv * y.base);
    }
    else
    {
        // J' = pJ − qY and Y' = qJ + pY give Y/J = (p − f)/q, and the Wronskian q(J² + Y²) = 2/(πx).
        const BesZ pq = bes_cf2_hankel(x, mu * mu);
        const double y_over_j = (pq.re - log_deriv) / pq.im;
        const double mag = crd::math::sqrt(wronskian / (pq.im * (1.0 + y_over_j * y_over_j)));
        j_mu = (j_raw >= 0.0) ? crd::math::fabs(mag) : -crd::math::fabs(mag);
        const double y_mu_d = j_mu * (pq.im + pq.re * y_over_j);
        y.base = j_mu * y_over_j;
        y.next = mu * inv_x * y.base - y_mu_d;
    }
    const double rescale = j_mu / j_raw;
    const BesPair top = bes_raise_from_base<true>(y, mu, steps, 2.0 * inv_x);
    return {seed * rescale, top.base, seed_d * rescale, nu * inv_x * top.base - top.next};
}

// I_ν, K_ν, I'_ν, K'_ν for x > 0, ν ≥ 0 by the same scheme (Temme 1975; Thompson & Barnett 1986): CF1 for I'/I
// (Lentz 1976), downward recurrence to the base order μ, Temme's series (x < 2) or Steed's CF2 for K_μ and K_{μ+1},
// then I from the Wronskian I K' − I' K = −1/x, and K carried upward to order ν.
[[nodiscard]] inline BesIK bes_ik_core(double x, double nu) noexcept
{
    const int steps = bes_base_steps(nu);
    const double mu = nu - steps;
    const double inv_x = 1.0 / x;
    const double seed = kBesTiny; // I_ν, up to the scale fixed below
    const double seed_d = bes_cf1_growing(inv_x, nu) * seed;
    const BesPair lowered = bes_lower_to_base<false>(seed, seed_d, nu, steps, inv_x);
    const double log_deriv = lowered.next / lowered.base; // I'_μ/I_μ
    const BesPair k = (x < kBesTemmeLimit) ? bes_temme_k(x, mu) : bes_cf2_k(x, mu);
    const double k_mu_d = mu * inv_x * k.base - k.next;        // K'_μ
    const double i_mu = inv_x / (log_deriv * k.base - k_mu_d); // I_μ from the Wronskian
    const BesPair top = bes_raise_from_base<false>(k, mu, steps, 2.0 * inv_x);
    // The seed is 1e-300 and I_μ can be large, so the product is formed before the division by the raw I_μ.
    return {i_mu * seed / lowered.base, top.base, i_mu * seed_d / lowered.base, nu * inv_x * top.base - top.next};
}

// ---- dedicated J_ν fast path (real x > 0, ν ≥ 0): ascending series (small x) + Hankel asymptotic (large x). ----
// Avoids computing the Y/J'/Y' quartet of bes_jy_core ⇒ ~3× fewer ops ⇒ beats Boost's cyl_bessel_j.
[[nodiscard]] inline double besselj_series(double nu, double x) noexcept
{
    const double half = 0.5 * x;
    double t = crd::math::exp(nu * crd::math::log(half) - lgamma(nu + 1.0)); // (x/2)^ν / Γ(ν+1)
    double sum = t;
    const double z2 = -half * half;
    for (int k = 1; k <= 400; ++k)
    {
        t *= z2 / (k * (nu + k));
        sum += t;
        if (crd::math::fabs(t) <= 1e-18 * crd::math::fabs(sum))
        {
            break;
        }
    }
    return sum;
}
[[nodiscard]] inline double besselj_asymp(double nu, double x) noexcept
{
    const double mu = 4.0 * nu * nu;
    const double omega = x - (0.5 * nu + 0.25) * kBesPi;
    const double inv = 1.0 / x;
    double ak = 1.0;
    double xpow = 1.0;
    double psum = 1.0; // P = Σ (−1)^m a_{2m}/x^{2m}
    double qsum = 0.0; // Q = Σ (−1)^m a_{2m+1}/x^{2m+1}
    double prev = 1e300;
    for (int k = 1; k <= 60; ++k)
    {
        ak *= (mu - (2.0 * k - 1.0) * (2.0 * k - 1.0)) / (8.0 * k);
        xpow *= inv;
        const double term = ak * xpow;
        if ((k & 1) == 0)
        {
            psum += (((k / 2) & 1) == 0) ? term : -term;
        }
        else
        {
            qsum += ((((k - 1) / 2) & 1) == 0) ? term : -term;
        }
        if (crd::math::fabs(term) > prev)
        {
            break; // optimal truncation
        }
        prev = crd::math::fabs(term);
    }
    return crd::math::sqrt(2.0 / (kBesPi * x)) * (crd::math::cos(omega) * psum - crd::math::sin(omega) * qsum);
}
[[nodiscard]] inline double besselj_real(double nu, double x) noexcept
{
    if (x <= 9.0)
    {
        return besselj_series(nu, x); // small x: series accurate to ~1e-13 (cancellation grows beyond)
    }
    if (x >= 17.5 && x >= 2.0 * nu)
    {
        return besselj_asymp(nu, x); // x ≫ ν: Hankel asymptotic
    }
    // hard middle / x≈ν region: fall back to the robust Steed/Temme (still correct, just not faster)
    return bes_jy_core(x, nu).j;
}

// Order-reflection for negative ν: J_{-ν} = cos(νπ)J_ν − sin(νπ)Y_ν ; Y_{-ν} = sin(νπ)J_ν + cos(νπ)Y_ν.
// I_{-ν} = I_ν + (2/π)sin(νπ)K_ν ; K_{-ν} = K_ν. (Used so the public API accepts any real order.)
} // namespace detail

// ---- cylindrical Bessel (real order ν, x > 0) ----
template <Real T>
[[nodiscard]] T cyl_bessel_j(T nu, T x) noexcept
{
    const double n = static_cast<double>(nu);
    const double xd = static_cast<double>(x);
    if (n >= 0.0)
    {
        return static_cast<T>(detail::besselj_real(n, xd)); // dedicated J-only fast path
    }
    const detail::BesJY jy = detail::bes_jy_core(xd, -n);
    const double s = crd::math::sin(-n * detail::kBesPi);
    const double co = crd::math::cos(-n * detail::kBesPi);
    return static_cast<T>(co * jy.j - s * jy.y);
}

template <Real T>
[[nodiscard]] T cyl_neumann(T nu, T x) noexcept // Y_ν
{
    const double n = static_cast<double>(nu);
    const double xd = static_cast<double>(x);
    if (n >= 0.0)
    {
        return static_cast<T>(detail::bes_jy_core(xd, n).y);
    }
    const detail::BesJY jy = detail::bes_jy_core(xd, -n);
    const double s = crd::math::sin(-n * detail::kBesPi);
    const double co = crd::math::cos(-n * detail::kBesPi);
    return static_cast<T>(s * jy.j + co * jy.y);
}

template <Real T>
[[nodiscard]] T cyl_bessel_i(T nu, T x) noexcept
{
    const double n = static_cast<double>(nu);
    const double xd = static_cast<double>(x);
    if (n >= 0.0)
    {
        return static_cast<T>(detail::bes_ik_core(xd, n).i);
    }
    const detail::BesIK ik = detail::bes_ik_core(xd, -n);
    return static_cast<T>(ik.i + (2.0 / detail::kBesPi) * crd::math::sin(-n * detail::kBesPi) * ik.k);
}

template <Real T>
[[nodiscard]] T cyl_bessel_k(T nu, T x) noexcept
{
    const double n = crd::math::fabs(static_cast<double>(nu)); // K_{-ν} = K_ν
    const double xd = static_cast<double>(x);
    return static_cast<T>(detail::bes_ik_core(xd, n).k);
}

// ---- derivatives (real order ν, x > 0) ----
template <Real T>
[[nodiscard]] T cyl_bessel_j_prime(T nu, T x) noexcept
{
    return static_cast<T>(detail::bes_jy_core(static_cast<double>(x), static_cast<double>(nu)).jp);
}
template <Real T>
[[nodiscard]] T cyl_neumann_prime(T nu, T x) noexcept
{
    return static_cast<T>(detail::bes_jy_core(static_cast<double>(x), static_cast<double>(nu)).yp);
}
template <Real T>
[[nodiscard]] T cyl_bessel_i_prime(T nu, T x) noexcept
{
    return static_cast<T>(detail::bes_ik_core(static_cast<double>(x), static_cast<double>(nu)).ip);
}
template <Real T>
[[nodiscard]] T cyl_bessel_k_prime(T nu, T x) noexcept
{
    return static_cast<T>(detail::bes_ik_core(static_cast<double>(x), static_cast<double>(nu)).kp);
}

// ---- spherical Bessel j_n, y_n (integer n ≥ 0, x > 0) via the stable recurrences ----
// j_0 = sin x / x, j_1 = sin x / x² − cos x / x; y_0 = −cos x / x, y_1 = −cos x / x² − sin x / x.
// y_n: upward recurrence (stable). j_n: upward is stable for x ≥ n; otherwise use j_n = √(π/2x) J_{n+½}.
template <Real T>
[[nodiscard]] T sph_bessel(int n, T x) noexcept
{
    const double xd = static_cast<double>(x);
    if (static_cast<double>(n) <= xd) // upward recurrence stable
    {
        double j0 = crd::math::sin(xd) / xd;
        if (n == 0)
        {
            return static_cast<T>(j0);
        }
        double j1 = crd::math::sin(xd) / (xd * xd) - crd::math::cos(xd) / xd;
        for (int k = 1; k < n; ++k)
        {
            const double jn = (2.0 * k + 1.0) / xd * j1 - j0;
            j0 = j1;
            j1 = jn;
        }
        return static_cast<T>(j1);
    }
    // j_n = √(π/2x) J_{n+½}
    return static_cast<T>(crd::math::sqrt(detail::kBesPi / (2.0 * xd)) * detail::bes_jy_core(xd, n + 0.5).j);
}

template <Real T>
[[nodiscard]] T sph_neumann(int n, T x) noexcept // y_n
{
    const double xd = static_cast<double>(x);
    double y0 = -crd::math::cos(xd) / xd;
    if (n == 0)
    {
        return static_cast<T>(y0);
    }
    double y1 = -crd::math::cos(xd) / (xd * xd) - crd::math::sin(xd) / xd;
    for (int k = 1; k < n; ++k)
    {
        const double yn = (2.0 * k + 1.0) / xd * y1 - y0;
        y0 = y1;
        y1 = yn;
    }
    return static_cast<T>(y1);
}

// ---- Hankel functions H^(1,2)_ν(x) = J_ν(x) ± i Y_ν(x) (real order, x > 0) ----
template <Real T>
[[nodiscard]] std::complex<T> cyl_hankel_1(T nu, T x) noexcept
{
    return {cyl_bessel_j(nu, x), cyl_neumann(nu, x)};
}
template <Real T>
[[nodiscard]] std::complex<T> cyl_hankel_2(T nu, T x) noexcept
{
    return {cyl_bessel_j(nu, x), -cyl_neumann(nu, x)};
}

// =====================================================================================================
// Complex argument z (real order ν). J/I via ascending series (moderate |z|) + Hankel/I-K asymptotic (large |z|);
// Y/K via the connection formulae (non-integer ν) or the large-|z| asymptotic. Gated vs scipy.special over the
// complex plane. Branch: principal (arg z ∈ (−π,π]). For integer ν the connection 0/0 is taken at ν+ε (≈1e-7 there;
// full accuracy for non-integer ν and for all ν via the asymptotic at large |z|).
// =====================================================================================================
namespace detail
{
using cplx = std::complex<double>;

// J (kind=−1) or I (kind=+1) ascending series: Σ (kind·(z/2)²)^k (z/2)^ν / (k! Γ(ν+k+1)).
[[nodiscard]] inline cplx bessel_ji_series(double nu, cplx z, double kind) noexcept
{
    const cplx cz = 0.5 * z;
    cplx t = crd::math::pow(cz, nu) / gamma(nu + 1.0); // (z/2)^ν / Γ(ν+1); SIGNED Γ (ν may be < 0 ⇒ Γ(ν+1) < 0)
    cplx sum = t;
    const cplx z2 = kind * (cz * cz);
    for (int k = 1; k <= 400; ++k)
    {
        t *= z2 / (static_cast<double>(k) * (nu + k));
        sum += t;
        if (std::abs(t) <= 1e-18 * std::abs(sum))
        {
            break;
        }
    }
    return sum;
}

// Hankel-type asymptotic coefficients shared by J/Y: returns P and Q (complex) for order ν at z.
inline void besseljy_asymp_pq(double nu, cplx z, cplx& p, cplx& q) noexcept
{
    const double mu = 4.0 * nu * nu;
    const cplx inv = 1.0 / z;
    cplx xpow = 1.0;
    double ak = 1.0;
    p = 1.0;
    q = 0.0;
    double prev = 1e300;
    for (int k = 1; k <= 60; ++k)
    {
        ak *= (mu - (2.0 * k - 1.0) * (2.0 * k - 1.0)) / (8.0 * k);
        xpow *= inv;
        const cplx term = ak * xpow;
        if ((k & 1) == 0)
        {
            p += (((k / 2) & 1) == 0) ? term : -term;
        }
        else
        {
            q += ((((k - 1) / 2) & 1) == 0) ? term : -term;
        }
        if (std::abs(term) > prev)
        {
            break;
        }
        prev = std::abs(term);
    }
}

// I (kind=+1) or K (kind=−1) large-|z| asymptotic Σ coefficients (a_k = ∏(4ν²−(2j−1)²)/(8j)).
inline cplx besselik_asymp(double nu, cplx z, double kind) noexcept
{
    const double mu = 4.0 * nu * nu;
    const cplx inv = 1.0 / z;
    cplx xpow = 1.0;
    cplx sum = 1.0;
    double ak = 1.0;
    double prev = 1e300;
    for (int k = 1; k <= 60; ++k)
    {
        ak *= (mu - (2.0 * k - 1.0) * (2.0 * k - 1.0)) / (8.0 * k);
        xpow *= inv;
        const double coeff = (kind < 0.0 || (k & 1) == 0) ? ak : -ak; // K: +a_k ; I: (−1)^k a_k
        const cplx term = coeff * xpow;
        sum += term;
        if (std::abs(term) > prev)
        {
            break;
        }
        prev = std::abs(term);
    }
    if (kind > 0.0) // I: e^z/√(2πz) · Σ(−1)^k a_k/z^k  (dominant; |arg z|<π/2)
    {
        return crd::math::exp(z) / crd::math::sqrt(2.0 * kBesPi * z) * sum;
    }
    return crd::math::sqrt(kBesPi / (2.0 * z)) * crd::math::exp(-z) * sum; // K: √(π/2z) e^{−z} Σ a_k/z^k
}

[[nodiscard]] inline bool ji_use_series(double nu, cplx z) noexcept
{
    const double az = std::abs(z);
    return az < 17.5 || az < 2.0 * nu; // series unless |z| large AND |z| ≫ ν
}

// J_ν(z), any real ν — series (moderate |z|) or Hankel asymptotic (large |z|). Direct (no reflection ⇒ no recursion):
// the series Σ(z/2)^{ν+2k}/… is valid for any ν (negative-integer terms vanish via 1/Γ(non-positive)=0).
[[nodiscard]] inline cplx besselj_c(double nu, cplx z) noexcept
{
    if (ji_use_series(crd::math::fabs(nu), z))
    {
        return bessel_ji_series(nu, z, -1.0);
    }
    cplx p;
    cplx q;
    besseljy_asymp_pq(nu, z, p, q);
    const cplx omega = z - (0.5 * nu + 0.25) * kBesPi;
    return crd::math::sqrt(2.0 / (kBesPi * z)) * (crd::math::cos(omega) * p - crd::math::sin(omega) * q);
}

// Integer-order Y_n(z) ascending series (n ≥ 0), DLMF 10.8.1 — exact (log + digamma), no ε-offset cancellation.
[[nodiscard]] inline cplx bessely_n_int(int n, cplx z) noexcept
{
    const cplx cz = 0.5 * z;
    const cplx z2 = cz * cz;
    cplx r = (2.0 / kBesPi) * crd::math::log(cz) * besselj_c(static_cast<double>(n), z);
    if (n > 0)
    {
        cplx finite = 0.0;
        cplx pw = 1.0; // (z²/4)^k
        for (int k = 0; k < n; ++k)
        {
            finite += (gamma(static_cast<double>(n - k)) / gamma(static_cast<double>(k + 1))) * pw;
            pw *= z2;
        }
        r -= (1.0 / kBesPi) * crd::math::pow(cz, -static_cast<double>(n)) * finite;
    }
    cplx s = 0.0;
    cplx pw = 1.0;
    for (int k = 0; k <= 400; ++k)
    {
        const double psi = digamma(static_cast<double>(k + 1)) + digamma(static_cast<double>(n + k + 1));
        const cplx term = ((k & 1) ? -1.0 : 1.0) * psi /
                          (gamma(static_cast<double>(k + 1)) * gamma(static_cast<double>(n + k + 1))) * pw;
        s += term;
        if (k >= n && std::abs(term) <= 1e-18 * std::abs(s))
        {
            break;
        }
        pw *= z2;
    }
    return r - (1.0 / kBesPi) * crd::math::pow(cz, static_cast<double>(n)) * s;
}

// Integer-order K_n(z) ascending series (n ≥ 0), DLMF 10.31.2 — exact (log + digamma).
[[nodiscard]] inline cplx besselk_n_int(int n, cplx z) noexcept
{
    const cplx cz = 0.5 * z;
    const cplx z2 = cz * cz;
    cplx r = ((n & 1) ? 1.0 : -1.0) * crd::math::log(cz) *
             bessel_ji_series(static_cast<double>(n), z, 1.0); // (−1)^{n+1} ln·I_n
    if (n > 0)
    {
        cplx finite = 0.0;
        cplx pw = 1.0; // (−z²/4)^k
        for (int k = 0; k < n; ++k)
        {
            finite += (gamma(static_cast<double>(n - k)) / gamma(static_cast<double>(k + 1))) * pw;
            pw *= (-z2);
        }
        r += 0.5 * crd::math::pow(cz, -static_cast<double>(n)) * finite;
    }
    cplx s = 0.0;
    cplx pw = 1.0;
    for (int k = 0; k <= 400; ++k)
    {
        const double psi = digamma(static_cast<double>(k + 1)) + digamma(static_cast<double>(n + k + 1));
        const cplx term = psi / (gamma(static_cast<double>(k + 1)) * gamma(static_cast<double>(n + k + 1))) * pw;
        s += term;
        if (k >= n && std::abs(term) <= 1e-18 * std::abs(s))
        {
            break;
        }
        pw *= z2;
    }
    return r + ((n & 1) ? -1.0 : 1.0) * 0.5 * crd::math::pow(cz, static_cast<double>(n)) * s; // (−1)^n
}

// Y_ν(z), any real ν. Large |z|: asymptotic (signed ω). Moderate |z|: connection (J_ν cosνπ − J_{−ν})/sinνπ
// (integer ν ⇒ the exact integer series above).
[[nodiscard]] inline cplx bessely_c(double nu, cplx z) noexcept
{
    if (!ji_use_series(crd::math::fabs(nu), z))
    {
        cplx p;
        cplx q;
        besseljy_asymp_pq(nu, z, p, q);
        const cplx omega = z - (0.5 * nu + 0.25) * kBesPi;
        return crd::math::sqrt(2.0 / (kBesPi * z)) * (crd::math::sin(omega) * p + crd::math::cos(omega) * q);
    }
    const double rn = crd::math::round(nu);
    if (crd::math::fabs(nu - rn) < 1e-12) // integer order: exact ascending series
    {
        const int n = static_cast<int>(crd::math::fabs(rn));
        const cplx yn = bessely_n_int(n, z);
        return (rn < 0.0 && (n & 1)) ? -yn : yn; // Y_{−n} = (−1)^n Y_n
    }
    return (besselj_c(nu, z) * crd::math::cos(nu * kBesPi) - besselj_c(-nu, z)) / crd::math::sin(nu * kBesPi);
}

// I_ν(z), any real ν — series (moderate |z|) or dominant asymptotic (large |z|, where I_{−ν}≈I_ν).
[[nodiscard]] inline cplx besseli_c(double nu, cplx z) noexcept
{
    if (ji_use_series(crd::math::fabs(nu), z))
    {
        return bessel_ji_series(nu, z, 1.0);
    }
    return besselik_asymp(crd::math::fabs(nu), z, 1.0);
}

// K_ν(z) = K_{−ν}(z). Large |z|: asymptotic. Moderate |z|: (π/2)(I_{−ν}−I_ν)/sinνπ (integer ν ⇒ ε-offset).
[[nodiscard]] inline cplx besselk_c(double nu, cplx z) noexcept
{
    const double n = crd::math::fabs(nu);
    if (std::abs(z) >= 9.0 && std::abs(z) >= 2.0 * n) // K-specific: its series cancels (K is exp-small) ⇒ switch early
    {
        return besselik_asymp(n, z, -1.0);
    }
    const double rn = crd::math::round(n);
    if (crd::math::fabs(n - rn) < 1e-12) // integer order: exact ascending series
    {
        return besselk_n_int(static_cast<int>(rn), z);
    }
    return (0.5 * kBesPi) * (bessel_ji_series(-n, z, 1.0) - bessel_ji_series(n, z, 1.0)) / crd::math::sin(n * kBesPi);
}
} // namespace detail

namespace detail
{
template <Real T>
[[nodiscard]] inline std::complex<T> to_user(cplx r) noexcept
{
    return {static_cast<T>(r.real()), static_cast<T>(r.imag())};
}
template <Real T>
[[nodiscard]] inline cplx to_d(std::complex<T> z) noexcept
{
    return {static_cast<double>(z.real()), static_cast<double>(z.imag())};
}
} // namespace detail

template <Real T>
[[nodiscard]] std::complex<T> cyl_bessel_j(T nu, std::complex<T> z) noexcept
{
    return detail::to_user<T>(detail::besselj_c(static_cast<double>(nu), detail::to_d(z)));
}
template <Real T>
[[nodiscard]] std::complex<T> cyl_neumann(T nu, std::complex<T> z) noexcept
{
    return detail::to_user<T>(detail::bessely_c(static_cast<double>(nu), detail::to_d(z)));
}
template <Real T>
[[nodiscard]] std::complex<T> cyl_bessel_i(T nu, std::complex<T> z) noexcept
{
    return detail::to_user<T>(detail::besseli_c(static_cast<double>(nu), detail::to_d(z)));
}
template <Real T>
[[nodiscard]] std::complex<T> cyl_bessel_k(T nu, std::complex<T> z) noexcept
{
    return detail::to_user<T>(detail::besselk_c(static_cast<double>(nu), detail::to_d(z)));
}
template <Real T>
[[nodiscard]] std::complex<T> cyl_hankel_1(T nu, std::complex<T> z) noexcept // J + iY
{
    const detail::cplx j = detail::besselj_c(static_cast<double>(nu), detail::to_d(z));
    const detail::cplx y = detail::bessely_c(static_cast<double>(nu), detail::to_d(z));
    return detail::to_user<T>(j + detail::cplx(0.0, 1.0) * y);
}
template <Real T>
[[nodiscard]] std::complex<T> cyl_hankel_2(T nu, std::complex<T> z) noexcept // J − iY
{
    const detail::cplx j = detail::besselj_c(static_cast<double>(nu), detail::to_d(z));
    const detail::cplx y = detail::bessely_c(static_cast<double>(nu), detail::to_d(z));
    return detail::to_user<T>(j - detail::cplx(0.0, 1.0) * y);
}

} // namespace crd::hesap::special
