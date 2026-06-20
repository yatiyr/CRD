#pragma once
// v10-e — N-dimensional FFT (2D / 3D / N-D) by row-column decomposition over the 1D engine. For each axis, every
// 1D line (strided, row-major) is transformed: power-of-two axes use `FftPlan<T>`, the rest use `BluesteinPlan<T>`
// (v10-c). Only the FORWARD DFT is ever applied per axis (both engines give the unnormalized forward DFT), and the
// inverse uses the N-D forward-trick  IFFT(x) = conj(FFT(conj x))/∏dᵢ  — so the two engines' differing inverse
// normalizations never clash, and the single 1/∏dᵢ lives at the N-D level. Deterministic plan-from-shape;
// the per-axis plans + twiddles are read-only after construction ⇒ cross-thread bit-identical.
#include <crd/hesap/fft/bluestein.hpp>
#include <crd/hesap/fft/fft.hpp>

#include <crd/containers/array.hpp>

namespace crd::hesap::fft
{

template <typename T> class NdFftPlan
{
public:
    NdFftPlan(crd::memory::IAllocator* alloc, crd::containers::ConstSpan<crd::usize> dims)
        : m_alloc(alloc), m_dims(alloc), m_strides(alloc), m_line(alloc), m_fft(alloc), m_blue(alloc)
    {
        CRD_ASSERT(dims.size() >= 1);
        m_ndim = dims.size();
        m_dims.resize(m_ndim);
        m_strides.resize(m_ndim);
        m_total = 1;
        crd::usize maxd = 1;
        for (crd::usize i = 0; i < m_ndim; ++i)
        {
            CRD_ASSERT(dims[i] >= 1);
            m_dims[i] = dims[i];
            m_total *= dims[i];
            maxd = (dims[i] > maxd) ? dims[i] : maxd;
        }
        crd::usize s = 1; // row-major strides: stride[i] = ∏_{j>i} dims[j]
        for (crd::usize i = m_ndim; i-- > 0;)
        {
            m_strides[i] = s;
            s *= m_dims[i];
        }
        m_line.resize(maxd);
        m_fft.resize(m_ndim);
        m_blue.resize(m_ndim);
        for (crd::usize i = 0; i < m_ndim; ++i)
        {
            m_fft[i] = nullptr;
            m_blue[i] = nullptr;
            const crd::usize n = m_dims[i];
            if (n >= 2 && (n & (n - 1)) == 0) // power of two ⇒ the fast engine
            {
                m_fft[i] = static_cast<FftPlan<T>*>(m_alloc->allocate(sizeof(FftPlan<T>), alignof(FftPlan<T>)));
                ::new (static_cast<void*>(m_fft[i])) FftPlan<T>(m_alloc, n);
            }
            else if (n >= 2) // arbitrary size ⇒ Bluestein
            {
                m_blue[i] =
                    static_cast<BluesteinPlan<T>*>(m_alloc->allocate(sizeof(BluesteinPlan<T>), alignof(BluesteinPlan<T>)));
                ::new (static_cast<void*>(m_blue[i])) BluesteinPlan<T>(m_alloc, n);
            }
            // n == 1: a degenerate axis, no transform needed.
        }
    }

    ~NdFftPlan()
    {
        for (crd::usize i = 0; i < m_ndim; ++i)
        {
            if (m_fft[i] != nullptr)
            {
                m_fft[i]->~FftPlan();
                m_alloc->deallocate(m_fft[i]);
            }
            if (m_blue[i] != nullptr)
            {
                m_blue[i]->~BluesteinPlan();
                m_alloc->deallocate(m_blue[i]);
            }
        }
    }

    NdFftPlan(const NdFftPlan&) = delete;
    NdFftPlan& operator=(const NdFftPlan&) = delete;
    NdFftPlan(NdFftPlan&&) = delete;
    NdFftPlan& operator=(NdFftPlan&&) = delete;

    // In-place N-D transform (row-major data, size ∏dᵢ). Forward = N-D DFT; Inverse = 1/∏dᵢ-scaled IDFT.
    void execute(crd::containers::Span<Complex<T>> data, FftDirection dir) const
    {
        CRD_ASSERT(data.size() == m_total);
        Complex<T>* const x = data.data();
        const bool inv = (dir == FftDirection::Inverse);
        if (inv)
        {
            for (crd::usize i = 0; i < m_total; ++i)
            {
                x[i].im = -x[i].im;
            }
        }
        Complex<T>* const line = m_line.data();
        for (crd::usize ax = 0; ax < m_ndim; ++ax)
        {
            const crd::usize d = m_dims[ax];
            if (d < 2)
            {
                continue;
            }
            const crd::usize st = m_strides[ax];
            const crd::usize outer = m_total / (d * st); // ∏_{j<ax} dims[j]
            for (crd::usize o = 0; o < outer; ++o)
            {
                for (crd::usize in = 0; in < st; ++in)
                {
                    const crd::usize base = o * d * st + in;
                    for (crd::usize i = 0; i < d; ++i)
                    {
                        line[i] = x[base + i * st];
                    }
                    if (m_fft[ax] != nullptr)
                    {
                        m_fft[ax]->execute(crd::containers::Span<Complex<T>>(line, d), FftDirection::Forward);
                    }
                    else
                    {
                        m_blue[ax]->execute(crd::containers::Span<Complex<T>>(line, d), FftDirection::Forward);
                    }
                    for (crd::usize i = 0; i < d; ++i)
                    {
                        x[base + i * st] = line[i];
                    }
                }
            }
        }
        if (inv)
        {
            const T s = T(1) / static_cast<T>(m_total);
            for (crd::usize i = 0; i < m_total; ++i)
            {
                x[i].re *= s;
                x[i].im = -x[i].im * s;
            }
        }
    }

    [[nodiscard]] crd::usize size() const noexcept { return m_total; }
    [[nodiscard]] crd::usize ndim() const noexcept { return m_ndim; }

private:
    crd::memory::IAllocator* m_alloc;
    crd::usize m_ndim = 0;
    crd::usize m_total = 1;
    crd::containers::Array<crd::usize> m_dims;
    crd::containers::Array<crd::usize> m_strides;
    mutable crd::containers::Array<Complex<T>> m_line; // size-max(dim) line scratch (serial)
    crd::containers::Array<FftPlan<T>*> m_fft;         // per axis: pow-2 plan (or null)
    crd::containers::Array<BluesteinPlan<T>*> m_blue;  // per axis: Bluestein plan (or null)
};

} // namespace crd::hesap::fft
