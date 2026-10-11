// DIAG.10c -- the shape/stride and value-domain probes at the tensor boundary (crd/hesap/tensor/probe.hpp) and their
// use at DLPack import (crd/hesap/tensor/dlpack.hpp). A capsule's shape and strides are a producer's claim: an element
// count that wraps, an offset past the i64 range, compact strides that do not fit, a missing shape array or a view
// that wraps the address space is refused; a valid expanded (stride-0) capsule and an empty one still import, and the
// layout probe reports the overlap without refusing it. The value probe names the first offender of a strided or
// complex view by its coordinates and lane. A producer's deleter that changes the floating-point mode is reported and
// undone. Every expected value is written by hand. ASCII test names.

#include <crd/core/fp_environment.hpp>
#include <crd/core/layout_probe.hpp>
#include <crd/core/value_domain.hpp>
#include <crd/hesap/tensor/dlpack.hpp>
#include <crd/hesap/tensor/probe.hpp>
#include <crd/hesap/tensor/tensor.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>
#include <limits>

namespace
{
using crd::f32;
using crd::f64;
using crd::i64;
using crd::u32;
using crd::u64;
using crd::hesap::tensor::c32;
using crd::hesap::tensor::DlpackImported;
using crd::hesap::tensor::TensorStatus;
using crd::hesap::tensor::TensorView;

constexpr u32 bit(crd::LayoutViolation v)
{
    return static_cast<u32>(v);
}

constexpr u32 vbit(crd::ValueViolation v)
{
    return static_cast<u32>(v);
}

f32 f32_of(u32 b)
{
    f32 f = 0.0F;
    std::memcpy(&f, &b, sizeof f);
    return f;
}

constexpr u32 kF32NaN = 0x7FC00000U;
constexpr u32 kF32Inf = 0x7F800000U;

// A versioned CPU f32 capsule over caller-owned arrays; no deleter unless one is set.
DLManagedTensorVersioned capsule(void* data, i64* shape, i64* strides, crd::i32 ndim)
{
    DLManagedTensorVersioned m{};
    m.version               = DLPackVersion{DLPACK_MAJOR_VERSION, DLPACK_MINOR_VERSION};
    m.dl_tensor.data        = data;
    m.dl_tensor.device      = DLDevice{kDLCPU, 0};
    m.dl_tensor.ndim        = ndim;
    m.dl_tensor.dtype       = DLDataType{kDLFloat, 32U, 1U};
    m.dl_tensor.shape       = shape;
    m.dl_tensor.strides     = strides;
    m.dl_tensor.byte_offset = 0U;
    return m;
}

void* address(std::uintptr_t a)
{
    return reinterpret_cast<void*>(a); // NOLINT(performance-no-int-to-ptr): never dereferenced, only range-checked
}

int g_deleted = 0;

// A producer whose deleter leaves flush-to-zero set (a framework bug the consumer must not inherit).
void ftz_deleter(DLManagedTensorVersioned* /*self*/)
{
    ++g_deleted;
    crd::FpEnvironment e = crd::read_fp_environment();
    e.mxcsr |= crd::kMxcsrFlushToZero;
    crd::write_fp_environment(e);
}

u32 expected_ftz_bits()
{
#if CRD_ARCH_X64
    return static_cast<u32>(crd::FpDifference::FlushToZero);
#else
    return 0U; // nothing is read on this architecture, so nothing is reported or put back
#endif
}
} // namespace

TEST_CASE("diag10c: dlpack import refuses a layout whose counts or offsets leave the integer ranges",
          "[hesap-tensor][dlpack][diag10c]")
{
    f32 buffer[8] = {};

    // [2^32, 2^32, 2] with zero strides on the big dims: the count is 2^65, which wraps to 0 (an "empty" tensor).
    i64 wrap_shape[3]   = {i64{1} << 32, i64{1} << 32, 2};
    i64 wrap_strides[3] = {0, 0, 1};
    DLManagedTensorVersioned wraps = capsule(buffer, wrap_shape, wrap_strides, 3);
    {
        DlpackImported<f32> imp;
        CHECK(crd::hesap::tensor::dlpack_import(&wraps, imp) == TensorStatus::BadInput);
        CHECK_FALSE(imp.valid());
    }

    // [3] with stride 2^62: element 2 sits at offset 2^63, so any walk over it is undefined pointer arithmetic.
    i64 three[1] = {3};
    i64 far[1]   = {i64{1} << 62};
    DLManagedTensorVersioned offsets = capsule(buffer, three, far, 1);
    {
        DlpackImported<f32> imp;
        CHECK(crd::hesap::tensor::dlpack_import(&offsets, imp) == TensorStatus::BadInput);
    }

    // [0, 2^40, 2^40] with NULL strides: empty, but its compact strides (2^80) do not fit an i64.
    i64 empty_big[3] = {0, i64{1} << 40, i64{1} << 40};
    DLManagedTensorVersioned compact = capsule(buffer, empty_big, nullptr, 3);
    {
        DlpackImported<f32> imp;
        CHECK(crd::hesap::tensor::dlpack_import(&compact, imp) == TensorStatus::BadInput);
    }

    // A view that would wrap the address space: two elements below an address of 64 with stride -16 (64 bytes back),
    // and eight elements above the last aligned address.
    i64 two[1]   = {2};
    i64 back[1]  = {-16};
    i64 eight[1] = {8};
    i64 unit[1]  = {1};
    DLManagedTensorVersioned low = capsule(address(32U), two, back, 1);
    DLManagedTensorVersioned high =
        capsule(address(std::numeric_limits<std::uintptr_t>::max() - 15U), eight, unit, 1); // 16-aligned, 28 bytes past
    {
        DlpackImported<f32> imp;
        CHECK(crd::hesap::tensor::dlpack_import(&low, imp) == TensorStatus::BadInput);
        CHECK(crd::hesap::tensor::dlpack_import(&high, imp) == TensorStatus::BadInput);
    }
}

TEST_CASE("diag10c: dlpack import keeps valid expanded and empty capsules and the probe reports them",
          "[hesap-tensor][dlpack][diag10c]")
{
    f32 row[4] = {1.0F, 2.0F, 3.0F, 4.0F};

    // An expanded [3, 4] view of one row (stride 0 on dim 0), as torch.expand gives: valid, writable, overlapping.
    i64 shape[2]   = {3, 4};
    i64 strides[2] = {0, 1};
    DLManagedTensorVersioned expanded = capsule(row, shape, strides, 2);
    {
        DlpackImported<f32> imp;
        REQUIRE(crd::hesap::tensor::dlpack_import(&expanded, imp) == TensorStatus::Ok);
        const crd::LayoutReport written = crd::hesap::tensor::probe_tensor_layout(imp.view());
        CHECK(written.overlap == crd::LayoutOverlap::Overlapping);
        CHECK(written.violations == bit(crd::LayoutViolation::Overlap));
        CHECK(written.element_count == 12U);
        CHECK_FALSE(written.storage_checked);
        CHECK(crd::hesap::tensor::probe_tensor_layout(imp.view(), 0, 4).storage_checked);
        CHECK(crd::hesap::tensor::probe_tensor_layout(imp.view(), 0, 3).violations ==
              (bit(crd::LayoutViolation::OutsideStorage) | bit(crd::LayoutViolation::Overlap)));
    }
    {
        DlpackImported<const f32> imp; // read as const: the overlap is a broadcast read, not a violation
        REQUIRE(crd::hesap::tensor::dlpack_import(&expanded, imp) == TensorStatus::Ok);
        const crd::LayoutReport read = crd::hesap::tensor::probe_tensor_layout(imp.view());
        CHECK(read.overlap == crd::LayoutOverlap::Overlapping);
        CHECK(read.ok());
    }

    // An empty [0, 4, 5] capsule with NULL strides imports (data may be null for size zero).
    i64 empty[3] = {0, 4, 5};
    DLManagedTensorVersioned none = capsule(nullptr, empty, nullptr, 3);
    {
        DlpackImported<f32> imp;
        REQUIRE(crd::hesap::tensor::dlpack_import(&none, imp) == TensorStatus::Ok);
        CHECK(imp.view().size() == 0U);
        CHECK(imp.view().stride(0) == 20); // compact strides of the extents as given
        CHECK(crd::hesap::tensor::probe_tensor_layout(imp.view()).ok());
    }
}

TEST_CASE("diag10c: dlpack import refuses a capsule with no shape array", "[hesap-tensor][dlpack][diag10c]")
{
    f32 buffer[2] = {};
    DLManagedTensorVersioned shapeless = capsule(buffer, nullptr, nullptr, 2);
    DlpackImported<f32> imp;
    CHECK(crd::hesap::tensor::dlpack_import(&shapeless, imp) == TensorStatus::BadInput);
}

TEST_CASE("diag10c: the tensor value probe names the first offender of a strided or complex view",
          "[hesap-tensor][diag10c]")
{
    // A [2, 3] view of column-major storage: logical (i, j) is at offset i + 2j.
    f32 storage[6] = {0.0F, 0.1F, 0.2F, 0.3F, 0.4F, 0.5F};
    storage[1 + 2 * 1] = f32_of(kF32NaN); // logical (1, 1): row-major index 4
    const u64 shape[2]   = {2U, 3U};
    const i64 strides[2] = {1, 2};
    const TensorView<const f32> view(storage, {shape, 2U}, {strides, 2U});
    const crd::StridedValueReport r = crd::hesap::tensor::probe_tensor_values(view, crd::finite_float_domain());
    REQUIRE(r.walked);
    CHECK(r.values.checked == 6U);
    CHECK(r.values.first_index == 4U);
    CHECK(r.first_coords[0] == 1U);
    CHECK(r.first_coords[1] == 1U);
    CHECK(r.values.first_violations == vbit(crd::ValueViolation::NaN));
    CHECK(crd::hesap::tensor::probe_tensor_values(view, crd::any_float_domain()).values.ok());

    // The same view permuted to [3, 2] names the same element by its new coordinates.
    const u32 order[2] = {1U, 0U};
    const crd::StridedValueReport t =
        crd::hesap::tensor::probe_tensor_values(view.permute({order, 2U}), crd::finite_float_domain());
    CHECK(t.values.first_index == 3U); // (1, 1) in a [3, 2] row-major walk
    CHECK(t.first_coords[0] == 1U);
    CHECK(t.first_coords[1] == 1U);

    // c32 values: element 2's imaginary part is +Inf, a distance domain admits it and a finite one does not.
    c32 z[3] = {c32(1.0F, 0.0F), c32(0.0F, 2.0F), c32(3.0F, f32_of(kF32Inf))};
    const u64 zshape[1] = {3U};
    const TensorView<c32> zv = TensorView<c32>::contiguous(z, {zshape, 1U});
    const crd::StridedValueReport zr = crd::hesap::tensor::probe_tensor_values(zv, crd::finite_float_domain());
    CHECK(zr.values.checked == 6U);
    CHECK(zr.values.first_index == 2U);
    CHECK(zr.first_lane == 1U);
    CHECK(zr.values.first_bits == kF32Inf);
    CHECK(crd::hesap::tensor::probe_tensor_values(zv, crd::distance_domain()).values.ok());

    // f64 through the same walk.
    f64 d[2] = {-1.0, 0.5};
    const u64 dshape[1] = {2U};
    const TensorView<f64> dv = TensorView<f64>::contiguous(d, {dshape, 1U});
    const crd::StridedValueReport dr = crd::hesap::tensor::probe_tensor_values(dv, crd::unit_interval_domain());
    CHECK(dr.values.violating == 1U);
    CHECK(dr.values.first_violations == vbit(crd::ValueViolation::BelowMinimum));
}

TEST_CASE("diag10c: a producer's deleter that changes the floating-point mode is reported and undone",
          "[hesap-tensor][dlpack][diag10c][fp]")
{
    f32 buffer[4] = {};
    i64 shape[1]  = {4};
    const crd::FpEnvironment before = crd::read_fp_environment();
    g_deleted = 0;

    DLManagedTensorVersioned first = capsule(buffer, shape, nullptr, 1);
    first.deleter = &ftz_deleter;
    {
        DlpackImported<f32> imp;
        REQUIRE(crd::hesap::tensor::dlpack_import(&first, imp) == TensorStatus::Ok);
        CHECK(imp.release() == expected_ftz_bits());
        CHECK(g_deleted == 1);
        CHECK(crd::fp_differences(crd::read_fp_environment(), before) == 0U);
        CHECK(imp.release() == 0U); // nothing left to release
    }

    // The destructor path puts the mode back too.
    DLManagedTensorVersioned second = capsule(buffer, shape, nullptr, 1);
    second.deleter = &ftz_deleter;
    {
        DlpackImported<f32> imp;
        REQUIRE(crd::hesap::tensor::dlpack_import(&second, imp) == TensorStatus::Ok);
    }
    CHECK(g_deleted == 2);
    CHECK(crd::fp_differences(crd::read_fp_environment(), before) == 0U);

    // So does the spec-mandated disposal of a capsule from a newer major version.
    DLManagedTensorVersioned future = capsule(buffer, shape, nullptr, 1);
    future.version = DLPackVersion{DLPACK_MAJOR_VERSION + 1U, 0U};
    future.deleter = &ftz_deleter;
    {
        DlpackImported<f32> imp;
        CHECK(crd::hesap::tensor::dlpack_import(&future, imp) == TensorStatus::Unsupported);
    }
    CHECK(g_deleted == 3);
    CHECK(crd::fp_differences(crd::read_fp_environment(), before) == 0U);
}
