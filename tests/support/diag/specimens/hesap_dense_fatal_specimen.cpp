// hesap-dense contract fatals (crd-hesap-dense-fatal-* CTests, tests/numerics/hesap-dense). The owning matrix types
// and the GEMM-family products allocate from the caller's allocator; one that refuses (a sub-budget allocator returns
// nullptr) leaves them out of memory, fatal in every build, never a null or short buffer filled (DIAG.3a; they wrote
// through the refused pointer, and sized some buffers by unchecked products, before). One mode per run (argv[1]); see
// expected_fatal.hpp for the exit protocol:
//   matrix-elements-refused     Matrix<f64>(4, 4): its elements are refused
//   matrix-size-overflow        Matrix<f64>(2^63, 2): the element count overflows (it wraps to 0)
//   vector-elements-refused     Vector<f64>(8): its elements are refused
//   permutation-refused         Permutation(8): its pivots are refused
//   structured-storage-refused  Symmetric<f64>(4): its storage is refused
//   banded-height-overflow      Banded<f64>(4, 4, SIZE_MAX, 1): the band height overflows (it wraps to 1)
//   gemm-pack-refused           gemm: the first packing buffer is refused
//   gemm-small-pack-refused     gemm_parallel on the small direct path: its packed B is refused
//   gemm-parallel-pack-refused  gemm_parallel on the packed path: the per-worker A pool is refused
//   syrk-pack-refused           syrk_lower_minus: the first packing buffer is refused
// The operands are built from the default allocator; the refusing allocator (refusing_allocator.hpp) refuses every
// request from the call the mode is about on, whose first allocation is the one the mode names.

#define CRD_DIAG_SPECIMEN_ROUTE_ABSENT // no sanitizer involved: this is an engine-fatal control

#include "expected_fatal.hpp"
#include "refusing_allocator.hpp"
#include "specimen_common.hpp"

#include <crd/hesap/dense/blas3.hpp>
#include <crd/hesap/dense/detail/gemm_pack.hpp>
#include <crd/hesap/dense/detail/syrk_microkernel.hpp>
#include <crd/hesap/dense/matrix.hpp>
#include <crd/hesap/dense/matrix_catalog.hpp>
#include <crd/hesap/dense/matrix_types.hpp>
#include <crd/hesap/dense/vector.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocator.hpp>

#include <cstdint>
#include <cstring>

namespace
{
namespace dense = crd::hesap::dense;
using crd_diag_fatal::RefusingAllocator;

[[nodiscard]] bool is(const char* mode, const char* name)
{
    return std::strcmp(mode, name) == 0;
}

// The storage modes. Returns false for an unknown mode.
[[nodiscard]] bool run_storage_mode(const char* mode)
{
    RefusingAllocator alloc("hesap-dense-fatal-refusing");
    if (is(mode, "matrix-elements-refused"))
    {
        crd_diag_fatal::arm(mode, dense::detail::kMatrixOutOfMemory);
        alloc.refuse_all();
        const dense::Matrix<crd::f64> m(&alloc, 4U, 4U);
        return true;
    }
    if (is(mode, "matrix-size-overflow"))
    {
        crd_diag_fatal::arm(mode, dense::detail::kMatrixOutOfMemory);
        const dense::Matrix<crd::f64> m(&alloc, crd::usize{1} << 63U, 2U);
        return true;
    }
    if (is(mode, "vector-elements-refused"))
    {
        crd_diag_fatal::arm(mode, dense::detail::kVectorOutOfMemory);
        alloc.refuse_all();
        const dense::Vector<crd::f64> v(&alloc, 8U);
        return true;
    }
    if (is(mode, "permutation-refused"))
    {
        crd_diag_fatal::arm(mode, dense::detail::kPermutationOutOfMemory);
        alloc.refuse_all();
        const dense::Permutation p(&alloc, 8U);
        return true;
    }
    if (is(mode, "structured-storage-refused"))
    {
        crd_diag_fatal::arm(mode, dense::detail::kStructuredStorageOutOfMemory);
        alloc.refuse_all();
        const dense::Symmetric<crd::f64> s(&alloc, 4U);
        return true;
    }
    if (is(mode, "banded-height-overflow"))
    {
        crd_diag_fatal::arm(mode, dense::detail::kStructuredStorageOutOfMemory);
        const dense::Banded<crd::f64> b(&alloc, 4U, 4U, SIZE_MAX, 1U);
        return true;
    }
    return false;
}

// The product modes. Returns false for an unknown mode.
[[nodiscard]] bool run_product_mode(const char* mode)
{
    const bool gemm = is(mode, "gemm-pack-refused");
    const bool small = is(mode, "gemm-small-pack-refused");
    const bool packed = is(mode, "gemm-parallel-pack-refused");
    const bool syrk = is(mode, "syrk-pack-refused");
    if (!gemm && !small && !packed && !syrk)
    {
        return false;
    }
    crd_diag_fatal::arm(mode, dense::detail::kGemmPackOutOfMemory);
    crd::jobs::init(crd::jobs::Config{.num_threads = 2});
    // 6 rows: a multiple of the f64 microkernel's MR, so the small direct path takes the RowMajor product.
    constexpr crd::usize n = 6U;
    dense::Matrix<crd::f64> a(crd::memory::default_allocator(), n, n);
    dense::Matrix<crd::f64> b(crd::memory::default_allocator(), n, n);
    dense::Matrix<crd::f64> c(crd::memory::default_allocator(), n, n);
    a.set_identity();
    b.set_identity();
    RefusingAllocator scratch("hesap-dense-fatal-refusing");
    scratch.refuse_all();
    if (gemm)
    {
        dense::gemm<crd::f64, dense::Layout::RowMajor>(1.0, a.cview(), b.cview(), 0.0, c.view(), dense::Trans::None,
                                                       dense::Trans::None, &scratch);
    }
    else if (small)
    {
        dense::gemm_parallel<crd::f64, dense::Layout::RowMajor>(2U, 1.0, a.cview(), b.cview(), 0.0, c.view(),
                                                                dense::Trans::None, dense::Trans::None, &scratch);
    }
    else if (packed)
    {
        // A transposed operand is not eligible for the small direct path, so the packed path runs.
        dense::gemm_parallel<crd::f64, dense::Layout::RowMajor>(2U, 1.0, a.cview(), b.cview(), 0.0, c.view(),
                                                                dense::Trans::Transpose, dense::Trans::None, &scratch);
    }
    else
    {
        dense::detail::syrk_lower_minus<crd::f64, dense::Layout::RowMajor>(a.cview(), c.view(), &scratch,
                                                                           /*allow_parallel=*/false);
    }
    crd::jobs::shutdown();
    return true;
}

// Runs one mode. Returns false for an unknown mode; returning at all for a known mode means the fatal did not fire.
[[nodiscard]] bool run_mode(const char* mode)
{
    return run_storage_mode(mode) || run_product_mode(mode);
}
} // namespace

int main(int argc, char** argv)
{
    crd_diag_harden();
    crd_diag_announce();
    if (argc < 2)
    {
        return 96;
    }
    // Exit 0 when the violation returned: the fatal did not fire, and the CTest (the marker line) rejects it.
    return run_mode(argv[1]) ? 0 : 96;
}
