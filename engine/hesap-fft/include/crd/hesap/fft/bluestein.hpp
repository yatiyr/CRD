#pragma once
// v10-c — Bluestein (chirp-z) transform: an ARBITRARY-size DFT (non-power-of-two, prime, any n) computed in
// O(n log n) by reducing it to a single power-of-two FFT of size M >= 2n-1 (the engine `FftPlan<T>`).
//
// Identity (forward): n·k = (n² + k² − (k−n)²)/2 ⇒ X[k] = conj(b[k]) · (a ⊛ b)[k], where
//   a[j] = x[j]·conj(b[j]),  b[m] = exp(+iπ m²/n),  and (a ⊛ b) is a linear convolution.
// The convolution is done circularly at length M (a zero-padded to M; b symmetrised so b_circ[M−m]=b[m]) via
//   a ⊛ b = IFFT_M( FFT_M(a) · FFT_M(b) ),  with FFT_M(b) precomputed once in the plan.
// Inverse uses the standard forward-trick  IFFT(x) = conj(FFT(conj(x)))/n  so one (forward) chirp serves both.
//
// Deterministic plan-from-size (no runtime measurement); the chirp + FFT_M(b) tables are shared/read-only ⇒
// cross-thread bit-identical. Correctness gate = brute-force O(n²) DFT (NOT round-trip). f32 + f64.
#include <crd/hesap/fft/fft.hpp>

#include <crd/containers/array.hpp>

#include <cmath>

namespace crd::hesap::fft
{

template <typename T> class BluesteinPlan
{
public:
    BluesteinPlan(crd::memory::IAllocator* alloc, crd::usize n)
        : m_alloc(alloc), m_n(n), m_chirp(alloc), m_bfft(alloc), m_work(alloc)
    {
        CRD_ASSERT(n >= 1);
        // M = smallest power of two >= 2n-1 (a length-n linear convolution spans 2n-1 samples).
        m_m = 1;
        while (m_m < (2 * n - 1))
        {
            m_m <<= 1;
        }
        m_plan = static_cast<FftPlan<T>*>(m_alloc->allocate(sizeof(FftPlan<T>), alignof(FftPlan<T>)));
        ::new (static_cast<void*>(m_plan)) FftPlan<T>(m_alloc, m_m);

        // chirp[k] = exp(+iπ k²/n), k = 0..n-1. k² is reduced mod 2n (exp has period 2n in k²) for f32/f64
        // precision at large k. The forward convention; conj() gives the −iπ variant where the algebra needs it.
        constexpr double pi = 3.14159265358979323846;
        m_chirp.resize(n);
        for (crd::usize k = 0; k < n; ++k)
        {
            const crd::usize kk = (k % (2 * n)) * (k % (2 * n)) % (2 * n); // k² mod 2n (overflow-safe for practical n)
            const double th = pi * static_cast<double>(kk) / static_cast<double>(n);
            m_chirp[k] = Complex<T>{static_cast<T>(std::cos(th)), static_cast<T>(std::sin(th))};
        }

        // b_circ (length M): b_circ[0]=chirp[0]=1, b_circ[m]=chirp[m] and b_circ[M-m]=chirp[m] (chirp is even),
        // rest zero. Precompute FFT_M(b_circ) ONCE (shared, read-only).
        m_bfft.resize(m_m);
        for (crd::usize i = 0; i < m_m; ++i)
        {
            m_bfft[i] = Complex<T>{T(0), T(0)};
        }
        m_bfft[0] = m_chirp[0];
        for (crd::usize k = 1; k < n; ++k)
        {
            m_bfft[k] = m_chirp[k];
            m_bfft[m_m - k] = m_chirp[k];
        }
        m_plan->execute(crd::containers::Span<Complex<T>>(m_bfft.data(), m_m), FftDirection::Forward);
        m_work.resize(m_m);
    }

    ~BluesteinPlan()
    {
        if (m_plan != nullptr)
        {
            m_plan->~FftPlan();
            m_alloc->deallocate(m_plan);
        }
    }

    BluesteinPlan(const BluesteinPlan&) = delete;
    BluesteinPlan& operator=(const BluesteinPlan&) = delete;
    BluesteinPlan(BluesteinPlan&&) = delete;
    BluesteinPlan& operator=(BluesteinPlan&&) = delete;

    // In-place size-n transform. Forward = DFT; Inverse = IDFT (1/n scaled, round-trip exact).
    void execute(crd::containers::Span<Complex<T>> data, FftDirection dir) const
    {
        CRD_ASSERT(data.size() == m_n);
        const bool inv = (dir == FftDirection::Inverse);
        Complex<T>* const x = data.data();
        Complex<T>* const w = m_work.data();

        // a[k] = x[k]·conj(chirp[k]); for the inverse, conjugate x first (forward-trick).
        for (crd::usize k = 0; k < m_n; ++k)
        {
            const T xr = x[k].re;
            const T xi = inv ? -x[k].im : x[k].im;
            const T cr = m_chirp[k].re;
            const T ci = m_chirp[k].im; // conj(chirp) = (cr, -ci)
            w[k] = Complex<T>{xr * cr + xi * ci, xi * cr - xr * ci};
        }
        for (crd::usize k = m_n; k < m_m; ++k)
        {
            w[k] = Complex<T>{T(0), T(0)};
        }

        m_plan->execute(crd::containers::Span<Complex<T>>(w, m_m), FftDirection::Forward); // A = FFT_M(a)
        for (crd::usize k = 0; k < m_m; ++k)                                               // C = A · FFT_M(b)
        {
            const T ar = w[k].re;
            const T ai = w[k].im;
            const T br = m_bfft[k].re;
            const T bi = m_bfft[k].im;
            w[k] = Complex<T>{ar * br - ai * bi, ar * bi + ai * br};
        }
        m_plan->execute(crd::containers::Span<Complex<T>>(w, m_m), FftDirection::Inverse); // c = M·(a⊛b)

        // X[k] = conj(chirp[k])·(c[k]/M)  (the engine inverse is UNNORMALIZED ⇒ divide the convolution by M).
        // For the inverse direction, additionally conjugate the result and scale by 1/n (the forward-trick).
        const T cm = T(1) / static_cast<T>(m_m);
        for (crd::usize k = 0; k < m_n; ++k)
        {
            const T cr = m_chirp[k].re;
            const T ci = m_chirp[k].im;
            T yr = (w[k].re * cr + w[k].im * ci) * cm;
            T yi = (w[k].im * cr - w[k].re * ci) * cm;
            if (inv)
            {
                const T s = T(1) / static_cast<T>(m_n);
                yr *= s;
                yi = -yi * s;
            }
            x[k] = Complex<T>{yr, yi};
        }
    }

    [[nodiscard]] crd::usize size() const noexcept { return m_n; }
    [[nodiscard]] crd::usize conv_size() const noexcept { return m_m; } // the internal pow-2 FFT length

private:
    crd::memory::IAllocator* m_alloc;
    crd::usize m_n;
    crd::usize m_m;
    FftPlan<T>* m_plan = nullptr;
    crd::containers::Array<Complex<T>> m_chirp; // exp(+iπ k²/n), read-only after ctor
    crd::containers::Array<Complex<T>> m_bfft;  // FFT_M(b_circ), read-only after ctor
    mutable crd::containers::Array<Complex<T>> m_work; // size-M scratch
};

} // namespace crd::hesap::fft
