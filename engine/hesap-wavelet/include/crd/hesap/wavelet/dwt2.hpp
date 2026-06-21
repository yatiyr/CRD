#pragma once

// ---------------------------------------------------------------------------
// crd-hesap-wavelet v11w-e — 2-D discrete wavelet transform (separable).
//
//   dwt2 / idwt2          single-level 2-D DWT: row-then-column separable
//                         filtering ⇒ 4 subbands cA (LL), cH (LH), cV (HL),
//                         cD (HH) (pywt layout cA,(cH,cV,cD)).
//   wavedec2 / waverec2   multilevel 2-D (recurse on cA).
//
// Gate (ADR-0093): subbands vs pywt.dwt2 + perfect reconstruction + run-twice
// bit-identical. Image / volume denoising + medical-imaging consumer.
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/wavelet/dwt.hpp>
#include <crd/hesap/wavelet/families.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::hesap::wavelet
{

// Single-level 2-D DWT result: 4 subbands, each rows×cols row-major.
template <typename T> struct Dwt2Result
{
    crd::containers::Array<T> cA, cH, cV, cD; // LL, LH, HL, HH
    crd::usize rows = 0, cols = 0;
    explicit Dwt2Result(crd::memory::IAllocator* a) : cA(a), cH(a), cV(a), cD(a) {}
};

namespace detail
{
// alloc-free 1-D analysis convolution (lo+hi) given pre-reversed filters. Writes out_lo/out_hi (length out_len).
template <typename T>
inline void analysis_1d(const T* x, crd::usize n, crd::containers::ConstSpan<T> rlo, crd::containers::ConstSpan<T> rhi,
                        SignalExtensionMode mode, T* out_lo, T* out_hi, crd::usize out_len) noexcept
{
    if (mode == SignalExtensionMode::Periodization)
    {
        downsampling_convolution_periodization<T>(x, n, rlo, out_lo, out_len);
        downsampling_convolution_periodization<T>(x, n, rhi, out_hi, out_len);
    }
    else
    {
        downsampling_convolution<T>(x, n, rlo, mode, out_lo, out_len);
        downsampling_convolution<T>(x, n, rhi, mode, out_hi, out_len);
    }
}
} // namespace detail

// Single-level 2-D DWT of a rows×cols (row-major) matrix. Separable row-then-column. The row/column transforms are
// independent ⇒ MULTI-THREADED (alloc-free kernels + per-job column scratch, disjoint writes) = bit-identical
// across thread counts (the moat). Serial fallback when workers <= 1.
template <typename T>
[[nodiscard]] Dwt2Result<T> dwt2(crd::memory::IAllocator* alloc, const T* data, crd::usize rows, crd::usize cols,
                                 const Wavelet& w, SignalExtensionMode mode)
{
    const crd::usize f = w.len();
    const crd::usize c2 = dwt_coeff_len(cols, f, mode);
    const crd::usize r2 = dwt_coeff_len(rows, f, mode);
    crd::containers::Array<T> rlo(alloc), rhi(alloc); // pre-reversed dec filters (alloc-free kernels below)
    rlo.resize(f);
    rhi.resize(f);
    for (crd::usize k = 0; k < f; ++k)
    {
        rlo[k] = static_cast<T>(w.dec_lo[f - 1 - k]);
        rhi[k] = static_cast<T>(w.dec_hi[f - 1 - k]);
    }
    const crd::containers::ConstSpan<T> rlos(rlo.data(), f), rhis(rhi.data(), f);

    crd::containers::Array<T> a1(alloc), d1(alloc); // R×c2 row-transformed halves
    a1.resize(rows * c2);
    d1.resize(rows * c2);
    Dwt2Result<T> out(alloc);
    out.rows = r2;
    out.cols = c2;
    out.cA.resize(r2 * c2);
    out.cH.resize(r2 * c2);
    out.cV.resize(r2 * c2);
    out.cD.resize(r2 * c2);

    const crd::u32 nw = crd::jobs::num_workers();
    const bool par = (nw > 1) && (rows >= 16) && (cols >= 16);
    const crd::u32 njobs = par ? nw : 1U;

    // per-job column scratch: gather buffer (rows) + two conv outputs (r2). Built serially.
    crd::containers::Array<T> colbuf(alloc), outlo(alloc), outhi(alloc);
    colbuf.resize(static_cast<crd::usize>(njobs) * rows);
    outlo.resize(static_cast<crd::usize>(njobs) * r2);
    outhi.resize(static_cast<crd::usize>(njobs) * r2);

    auto row_pass = [&](crd::usize r) // step 1: dwt along each row (contiguous, alloc-free)
    {
        detail::analysis_1d<T>(data + r * cols, cols, rlos, rhis, mode, a1.data() + r * c2, d1.data() + r * c2, c2);
    };
    auto col_pass = [&](crd::u32 job, crd::usize j) // step 2: dwt along each column (gather → conv → scatter)
    {
        T* cb = colbuf.data() + static_cast<crd::usize>(job) * rows;
        T* ol = outlo.data() + static_cast<crd::usize>(job) * r2;
        T* oh = outhi.data() + static_cast<crd::usize>(job) * r2;
        for (crd::usize i = 0; i < rows; ++i)
        {
            cb[i] = a1[i * c2 + j];
        }
        detail::analysis_1d<T>(cb, rows, rlos, rhis, mode, ol, oh, r2); // lo→cA, hi→cH
        for (crd::usize i = 0; i < r2; ++i)
        {
            out.cA[i * c2 + j] = ol[i];
            out.cH[i * c2 + j] = oh[i];
        }
        for (crd::usize i = 0; i < rows; ++i)
        {
            cb[i] = d1[i * c2 + j];
        }
        detail::analysis_1d<T>(cb, rows, rlos, rhis, mode, ol, oh, r2); // lo→cV, hi→cD
        for (crd::usize i = 0; i < r2; ++i)
        {
            out.cV[i * c2 + j] = ol[i];
            out.cD[i * c2 + j] = oh[i];
        }
    };

    if (!par)
    {
        for (crd::usize r = 0; r < rows; ++r)
        {
            row_pass(r);
        }
        for (crd::usize j = 0; j < c2; ++j)
        {
            col_pass(0, j);
        }
    }
    else
    {
        crd::jobs::Counter* cr = crd::jobs::parallel_for(static_cast<crd::u32>(rows), njobs, [&](crd::u32 b, crd::u32 e) {
            for (crd::u32 r = b; r < e; ++r)
            {
                row_pass(r);
            }
        });
        crd::jobs::wait(cr);
        crd::jobs::Counter* cc = crd::jobs::parallel_for(njobs, njobs, [&](crd::u32 jb, crd::u32 je) {
            for (crd::u32 job = jb; job < je; ++job)
            {
                const crd::usize j0 = static_cast<crd::usize>(job) * c2 / njobs;
                const crd::usize j1 = static_cast<crd::usize>(job + 1) * c2 / njobs;
                for (crd::usize j = j0; j < j1; ++j)
                {
                    col_pass(job, j);
                }
            }
        });
        crd::jobs::wait(cc);
    }
    return out;
}

// Inverse single-level 2-D DWT. Reconstructs a (out_rows × out_cols) matrix into `out` (row-major).
template <typename T>
void idwt2(crd::memory::IAllocator* alloc, const Dwt2Result<T>& c, const Wavelet& w, SignalExtensionMode mode,
           crd::containers::Array<T>& out, crd::usize& out_rows, crd::usize& out_cols)
{
    const crd::usize r2 = c.rows, c2 = c.cols;
    // step 1 (inverse of step 2): reconstruct columns. (cA,cH)->A1 columns; (cV,cD)->D1 columns.
    crd::containers::Array<T> caCol(alloc), cdCol(alloc), rec(alloc);
    // determine reconstructed row count from a probe column.
    {
        caCol.resize(r2);
        cdCol.resize(r2);
        idwt<T>(alloc, crd::containers::ConstSpan<T>(caCol.data(), r2), crd::containers::ConstSpan<T>(cdCol.data(), r2),
                w, mode, rec);
    }
    const crd::usize rrec = rec.size();
    crd::containers::Array<T> a1(alloc), d1(alloc);
    a1.resize(rrec * c2);
    d1.resize(rrec * c2);
    for (crd::usize j = 0; j < c2; ++j)
    {
        for (crd::usize i = 0; i < r2; ++i)
        {
            caCol[i] = c.cA[i * c2 + j];
            cdCol[i] = c.cH[i * c2 + j];
        }
        idwt<T>(alloc, crd::containers::ConstSpan<T>(caCol.data(), r2), crd::containers::ConstSpan<T>(cdCol.data(), r2),
                w, mode, rec);
        for (crd::usize i = 0; i < rrec; ++i)
        {
            a1[i * c2 + j] = rec[i];
        }
        for (crd::usize i = 0; i < r2; ++i)
        {
            caCol[i] = c.cV[i * c2 + j];
            cdCol[i] = c.cD[i * c2 + j];
        }
        idwt<T>(alloc, crd::containers::ConstSpan<T>(caCol.data(), r2), crd::containers::ConstSpan<T>(cdCol.data(), r2),
                w, mode, rec);
        for (crd::usize i = 0; i < rrec; ++i)
        {
            d1[i * c2 + j] = rec[i];
        }
    }
    // step 2 (inverse of step 1): reconstruct rows from (a1, d1).
    crd::containers::Array<T> rowA(alloc), rowD(alloc);
    rowA.resize(c2);
    rowD.resize(c2);
    idwt<T>(alloc, crd::containers::ConstSpan<T>(rowA.data(), c2), crd::containers::ConstSpan<T>(rowD.data(), c2), w,
            mode, rec);
    const crd::usize crec = rec.size();
    out_rows = rrec;
    out_cols = crec;
    out.resize(rrec * crec);
    for (crd::usize i = 0; i < rrec; ++i)
    {
        for (crd::usize j = 0; j < c2; ++j)
        {
            rowA[j] = a1[i * c2 + j];
            rowD[j] = d1[i * c2 + j];
        }
        idwt<T>(alloc, crd::containers::ConstSpan<T>(rowA.data(), c2), crd::containers::ConstSpan<T>(rowD.data(), c2), w,
                mode, rec);
        for (crd::usize j = 0; j < crec; ++j)
        {
            out[i * crec + j] = rec[j];
        }
    }
}

// Multilevel 2-D decomposition: [cA_n, {cH,cV,cD}_n, ..., {cH,cV,cD}_1] (the detail tuples, coarse-first).
template <typename T> struct WaveDec2
{
    crd::containers::Array<T> cA; // coarsest approximation (last_rows × last_cols)
    crd::usize a_rows = 0, a_cols = 0;
    crd::containers::Array<Dwt2Result<T>> details; // coarse-first detail levels (cA unused inside)
    explicit WaveDec2(crd::memory::IAllocator* a) : cA(a), details(a) {}
};

template <typename T>
[[nodiscard]] WaveDec2<T> wavedec2(crd::memory::IAllocator* alloc, const T* data, crd::usize rows, crd::usize cols,
                                   const Wavelet& w, SignalExtensionMode mode, crd::usize level)
{
    WaveDec2<T> result(alloc);
    crd::containers::Array<Dwt2Result<T>> fine_first(alloc);
    fine_first.reserve(level);
    crd::containers::Array<T> cur(alloc);
    cur.resize(rows * cols);
    for (crd::usize i = 0; i < rows * cols; ++i)
    {
        cur[i] = data[i];
    }
    crd::usize cr = rows, cc = cols;
    for (crd::usize l = 0; l < level; ++l)
    {
        Dwt2Result<T> d = dwt2<T>(alloc, cur.data(), cr, cc, w, mode);
        cur.resize(d.rows * d.cols);
        for (crd::usize i = 0; i < d.rows * d.cols; ++i)
        {
            cur[i] = d.cA[i];
        }
        cr = d.rows;
        cc = d.cols;
        fine_first.push_back(std::move(d));
    }
    result.cA.resize(cr * cc);
    for (crd::usize i = 0; i < cr * cc; ++i)
    {
        result.cA[i] = cur[i];
    }
    result.a_rows = cr;
    result.a_cols = cc;
    result.details.reserve(level);
    for (crd::usize l = 0; l < level; ++l)
    {
        result.details.push_back(std::move(fine_first[level - 1 - l])); // coarse-first
    }
    return result;
}

template <typename T>
[[nodiscard]] crd::containers::Array<T> waverec2(crd::memory::IAllocator* alloc, const WaveDec2<T>& dec,
                                                 const Wavelet& w, SignalExtensionMode mode, crd::usize& out_rows,
                                                 crd::usize& out_cols)
{
    crd::containers::Array<T> approx(alloc);
    approx.resize(dec.cA.size());
    for (crd::usize i = 0; i < dec.cA.size(); ++i)
    {
        approx[i] = dec.cA[i];
    }
    crd::usize ar = dec.a_rows, ac = dec.a_cols;
    for (crd::usize d = 0; d < dec.details.size(); ++d)
    {
        const Dwt2Result<T>& det = dec.details[d];
        Dwt2Result<T> level(alloc); // assemble approx + this level's details (trim approx to detail size)
        level.rows = det.rows;
        level.cols = det.cols;
        const crd::usize sz = det.rows * det.cols;
        level.cA.resize(sz);
        for (crd::usize i = 0; i < det.rows; ++i)
        {
            for (crd::usize j = 0; j < det.cols; ++j)
            {
                level.cA[i * det.cols + j] = (i < ar && j < ac) ? approx[i * ac + j] : T(0);
            }
        }
        level.cH = det.cH;
        level.cV = det.cV;
        level.cD = det.cD;
        crd::usize rr = 0, rc = 0;
        idwt2<T>(alloc, level, w, mode, approx, rr, rc);
        ar = rr;
        ac = rc;
    }
    out_rows = ar;
    out_cols = ac;
    return approx;
}

} // namespace crd::hesap::wavelet
