#pragma once
// v10-e — N-dimensional FFT (2D / 3D / N-D) by row-column decomposition over the 1D engine. For each axis, every
// 1D line (strided, row-major) is transformed: power-of-two axes use `FftPlan<T>`, the rest use `BluesteinPlan<T>`
// (v10-c). Only the FORWARD DFT is ever applied per axis (both engines give the unnormalized forward DFT), and the
// inverse uses the N-D forward-trick  IFFT(x) = conj(FFT(conj x))/∏dᵢ  — so the two engines' differing inverse
// normalizations never clash, and the single 1/∏dᵢ lives at the N-D level. Deterministic plan-from-shape;
// the per-axis plans + twiddles are read-only after construction ⇒ cross-thread bit-identical.
//
// JOBS: a plan built for `job_plans` jobs holds that many sets of axis plans and line buffers, and an axis's lines
// are a batch (detail/batch_arms.hpp): a contiguous range of lines a job, each on its own set, so every job count gives
// the serial bits. One set (the default) runs every line on the calling thread. The inverse's conjugations stay serial.
#include <crd/hesap/fft/bluestein.hpp>
#include <crd/hesap/fft/detail/batch_arms.hpp>
#include <crd/hesap/fft/fft.hpp>

#include <crd/containers/array.hpp>
#include <crd/memory/construct.hpp> // construct_or_fatal (DIAG.3a)

namespace crd::hesap::fft
{
namespace detail
{
// DIAG.3a: a refused axis plan leaves the N-d plan out of memory, fatal in every build, never a null plan constructed
// into (crd-hesap-fft-fatal-* modes).
inline constexpr const char* kNdFftPlanOutOfMemory = "NdFftPlan: out of memory for an axis plan";
} // namespace detail

template <typename T> class NdFftPlan
{
public:
    NdFftPlan(crd::memory::IAllocator* alloc, crd::containers::ConstSpan<crd::usize> dims, crd::u32 job_plans = 1)
        : m_alloc(alloc), m_dims(alloc), m_strides(alloc), m_line(alloc), m_fft(alloc), m_blue(alloc)
    {
        CRD_ASSERT(dims.size() >= 1);
        CRD_ASSERT(job_plans >= 1);
        m_ndim = dims.size();
        m_job_plans = job_plans;
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
        m_maxd = maxd;
        const crd::usize sets = m_ndim * job_plans; // job j's plan of axis i at j * ndim + i
        m_line.resize(maxd * job_plans);
        m_fft.resize(sets);
        m_blue.resize(sets);
        for (crd::usize i = 0; i < sets; ++i)
        {
            m_fft[i] = nullptr;
            m_blue[i] = nullptr;
        }
        for (crd::usize i = 0; i < sets; ++i)
        {
            const crd::usize n = m_dims[i % m_ndim];
            if (n >= 2 && (n & (n - 1)) == 0) // power of two ⇒ the fast engine
            {
                m_fft[i] =
                    crd::memory::construct_or_fatal<FftPlan<T>>(*m_alloc, detail::kNdFftPlanOutOfMemory, m_alloc, n);
            }
            else if (n >= 2) // arbitrary size ⇒ Bluestein
            {
                m_blue[i] = crd::memory::construct_or_fatal<BluesteinPlan<T>>(*m_alloc, detail::kNdFftPlanOutOfMemory,
                                                                              m_alloc, n);
            }
            // n == 1: a degenerate axis, no transform needed.
        }
    }

    ~NdFftPlan()
    {
        for (crd::usize i = 0; i < m_fft.size(); ++i)
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
        execute_arm(data, dir, detail::BatchPick{});
    }

    // execute with each axis's lines on the jobs `pick` gives them, at most the plan's job_plans.
    void execute_arm(crd::containers::Span<Complex<T>> data, FftDirection dir, const detail::BatchPick& pick) const
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
        // Generated FFT codelets can spill ~420 KiB in GCC Debug before nested calls: the 2 MiB tier.
        constexpr crd::jobs::StackSize stack = crd::jobs::StackSize::Large;
        for (crd::usize ax = 0; ax < m_ndim; ++ax)
        {
            const crd::usize d = m_dims[ax];
            if (d < 2)
            {
                continue;
            }
            const crd::usize lines = m_total / d;
            const crd::u32 wanted = detail::batch_jobs(pick, lines, d);
            const crd::u32 jobs = wanted < m_job_plans ? wanted : m_job_plans;
            detail::run_batch(jobs, lines, stack, [&](crd::u32 job, crd::usize first, crd::usize last)
                              { transform_lines(x, ax, job, first, last); });
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
    [[nodiscard]] crd::u32 job_plans() const noexcept { return m_job_plans; }

private:
    // Lines [first, last) of axis `ax` on job `job`'s plan and line; line l sits in outer block l / stride (blocks of
    // dim x stride elements) at offset l % stride.
    void transform_lines(Complex<T>* x, crd::usize ax, crd::u32 job, crd::usize first, crd::usize last) const
    {
        const crd::usize d = m_dims[ax];
        const crd::usize st = m_strides[ax];
        const crd::usize set = static_cast<crd::usize>(job) * m_ndim + ax;
        Complex<T>* const line = m_line.data() + static_cast<crd::usize>(job) * m_maxd;
        for (crd::usize l = first; l < last; ++l)
        {
            const crd::usize base = (l / st) * d * st + l % st;
            for (crd::usize i = 0; i < d; ++i)
            {
                line[i] = x[base + i * st];
            }
            if (m_fft[set] != nullptr)
            {
                m_fft[set]->execute(crd::containers::Span<Complex<T>>(line, d), FftDirection::Forward);
            }
            else
            {
                m_blue[set]->execute(crd::containers::Span<Complex<T>>(line, d), FftDirection::Forward);
            }
            for (crd::usize i = 0; i < d; ++i)
            {
                x[base + i * st] = line[i];
            }
        }
    }

    crd::memory::IAllocator* m_alloc;
    crd::usize m_ndim = 0;
    crd::usize m_total = 1;
    crd::usize m_maxd = 1;
    crd::u32 m_job_plans = 1;
    crd::containers::Array<crd::usize> m_dims;
    crd::containers::Array<crd::usize> m_strides;
    mutable crd::containers::Array<Complex<T>> m_line; // a size-max(dim) line scratch a job
    crd::containers::Array<FftPlan<T>*> m_fft;         // per job and axis: pow-2 plan (or null)
    crd::containers::Array<BluesteinPlan<T>*> m_blue;  // per job and axis: Bluestein plan (or null)
};

} // namespace crd::hesap::fft
