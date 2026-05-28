#include <crd/hesap/dense/detail/apply_q_block.hpp>

#include <crd/containers/array.hpp>
#include <crd/hesap/dense/blas3.hpp>
#include <crd/hesap/dense/detail/block_reflector.hpp>
#include <crd/hesap/dense/matrix.hpp>

namespace crd::hesap::dense::detail
{
namespace
{
constexpr crd::usize kApplyBlock = 32;

// Materialize the unit-lower reflector block V (msub × wb) from qpacked
// starting at (kb, kb): V[i][j] = (i<j ? 0 : i==j ? 1 : qpacked[(kb+i)*ld+(kb+j)]).
template <typename T>
void materialize_v(const T* qpacked, crd::usize ld, crd::usize kb, crd::usize msub, crd::usize wb,
                   T* v, crd::usize v_ld) noexcept
{
    for (crd::usize i = 0; i < msub; ++i)
    {
        for (crd::usize j = 0; j < wb; ++j)
        {
            T val;
            if (i < j)
            {
                val = T{0};
            }
            else if (i == j)
            {
                val = T{1};
            }
            else
            {
                val = qpacked[(kb + i) * ld + (kb + j)];
            }
            v[i * v_ld + j] = val;
        }
    }
}
} // namespace

template <typename T>
void apply_q_block(const T* qpacked, crd::usize ld, crd::usize m, crd::usize k, const T* taus, T* c,
                   crd::usize ldc, crd::usize crows, crd::usize ccols, bool right, bool transpose,
                   crd::memory::IAllocator* alloc)
{
    constexpr Layout l = Layout::RowMajor;
    if (k == 0 || m == 0)
    {
        return;
    }

    const crd::usize wmajor = right ? crows : ccols;  // operand extent orthogonal to m

    crd::containers::Array<T> v_buf(alloc);
    crd::containers::Array<T> vtv_buf(alloc);
    crd::containers::Array<T> t_buf(alloc);
    crd::containers::Array<T> w_buf(alloc);
    crd::containers::Array<T> w2_buf(alloc);
    v_buf.resize(m * kApplyBlock);
    vtv_buf.resize(kApplyBlock * kApplyBlock);
    t_buf.resize(kApplyBlock * kApplyBlock);
    w_buf.resize(wmajor * kApplyBlock + kApplyBlock);
    w2_buf.resize(wmajor * kApplyBlock + kApplyBlock);

    // Block visitation order: forward for {Left,Trans} and {Right,NoTrans};
    // reverse for {Left,NoTrans} and {Right,Trans}.
    const bool forward = (!right && transpose) || (right && !transpose);

    const crd::usize nblocks = (k + kApplyBlock - 1) / kApplyBlock;
    for (crd::usize bi = 0; bi < nblocks; ++bi)
    {
        const crd::usize blk = forward ? bi : (nblocks - 1 - bi);
        const crd::usize kb = blk * kApplyBlock;
        const crd::usize wb = (kb + kApplyBlock <= k) ? kApplyBlock : (k - kb);
        const crd::usize msub = m - kb;

        materialize_v<T>(qpacked, ld, kb, msub, wb, v_buf.data(), kApplyBlock);
        MatrixView<const T, l> vview{v_buf.data(), msub, wb, kApplyBlock};

        // vtv = Vᵀ·V (wb×wb); build the compact-WY T block.
        MatrixView<T, l> vtv_view{vtv_buf.data(), wb, wb, kApplyBlock};
        gemm<T, l>(T{1}, vview, vview, T{0}, vtv_view, Trans::Transpose, Trans::None, alloc);
        build_block_t_from_vtv<T>(vtv_buf.data(), kApplyBlock, taus, kb, wb, t_buf.data(),
                                  kApplyBlock);
        MatrixView<const T, l> tview{t_buf.data(), wb, wb, kApplyBlock};

        if (!right)
        {
            // C_sub = C[kb:m, 0:ccols]: op(H)·C_sub.
            MatrixView<T, l> csub{c + kb * ldc, msub, ccols, ldc};
            MatrixView<const T, l> csub_c{c + kb * ldc, msub, ccols, ldc};
            // W = Vᵀ·C_sub  (wb × ccols).
            MatrixView<T, l> wv{w_buf.data(), wb, ccols, ccols};
            gemm<T, l>(T{1}, vview, csub_c, T{0}, wv, Trans::Transpose, Trans::None, alloc);
            // W2 = op(T)·W.
            MatrixView<const T, l> wv_c{w_buf.data(), wb, ccols, ccols};
            MatrixView<T, l> w2v{w2_buf.data(), wb, ccols, ccols};
            gemm<T, l>(T{1}, tview, wv_c, T{0}, w2v, transpose ? Trans::Transpose : Trans::None,
                       Trans::None, alloc);
            // C_sub -= V·W2.
            MatrixView<const T, l> w2v_c{w2_buf.data(), wb, ccols, ccols};
            gemm_parallel_auto<T, l>(T{-1}, vview, w2v_c, T{1}, csub, Trans::None, Trans::None, alloc);
        }
        else
        {
            // C_sub = C[0:crows, kb:m]: C_sub·op(H).
            MatrixView<T, l> csub{c + kb, crows, msub, ldc};
            MatrixView<const T, l> csub_c{c + kb, crows, msub, ldc};
            // W = C_sub·V  (crows × wb).
            MatrixView<T, l> wv{w_buf.data(), crows, wb, wb};
            gemm<T, l>(T{1}, csub_c, vview, T{0}, wv, Trans::None, Trans::None, alloc);
            // W2 = W·op(T)  (op = Tᵀ for transpose=true, T otherwise).
            MatrixView<const T, l> wv_c{w_buf.data(), crows, wb, wb};
            MatrixView<T, l> w2v{w2_buf.data(), crows, wb, wb};
            gemm<T, l>(T{1}, wv_c, tview, T{0}, w2v, Trans::None,
                       transpose ? Trans::Transpose : Trans::None, alloc);
            // C_sub -= W2·Vᵀ.
            MatrixView<const T, l> w2v_c{w2_buf.data(), crows, wb, wb};
            gemm_parallel_auto<T, l>(T{-1}, w2v_c, vview, T{1}, csub, Trans::None, Trans::Transpose,
                                     alloc);
        }
    }
}

template void apply_q_block<float>(const float*, crd::usize, crd::usize, crd::usize, const float*,
                                   float*, crd::usize, crd::usize, crd::usize, bool, bool,
                                   crd::memory::IAllocator*);
template void apply_q_block<double>(const double*, crd::usize, crd::usize, crd::usize, const double*,
                                    double*, crd::usize, crd::usize, crd::usize, bool, bool,
                                    crd::memory::IAllocator*);

} // namespace crd::hesap::dense::detail
