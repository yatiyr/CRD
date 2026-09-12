#pragma once
// ---------------------------------------------------------------------------
// crd-hesap-tensor — v14-l: DLPack producer/consumer (zero-copy interchange).
//
// detail/dlpack.h is the CANONICAL dlpack v1.1 ABI header, vendored VERBATIM
// from dmlc/dlpack (Apache-2.0 — its own license header retained). It is the
// standard interchange ABI (NumPy/PyTorch/JAX/CuPy all speak it), so it is
// exempt from house style as a vendored spec header. This file is the crd
// layer over it.
//
// LIFETIME CONTRACT (the DLPack spec's deleter protocol, spelled out):
//
//   PRODUCER (dlpack_export): the returned DLManagedTensorVersioned* is a
//   descriptor block that BORROWS the exported view's buffer — zero-copy,
//   dl_tensor.data IS view.data(). The consumer signals it is done by
//   calling managed->deleter(managed) exactly once; the deleter frees ONLY
//   the descriptor block (through the IAllocator that built it), never the
//   tensor storage. The caller must keep the underlying Tensor alive until
//   the consumer has called the deleter (the engine-wide Span discipline —
//   an exported capsule is a view, not a transfer; DLPACK_FLAG_BITMASK_IS_
//   COPIED is never set because nothing is copied).
//
//   CONSUMER (dlpack_import): on success the DlpackImported<T> handle OWNS
//   the producer's capsule — no borrowed-lifetime members: the handle stores
//   the capsule pointer and invokes the deleter exactly once (destructor or
//   release()). view() BORROWS the producer's buffer through that capsule,
//   so the view must not outlive the handle. On failure ownership stays
//   with the caller, with ONE spec-mandated exception: a major-version
//   mismatch, where the spec instructs the consumer to call the deleter
//   (it is the only field safe to touch) — we do, and return Unsupported.
//
// Strides are in ELEMENTS (DLPack == TensorView convention; NULL strides =
// compact row-major). byte_offset is honoured on import and emitted as 0 on
// export. Complex dtypes use the interleaved layout on both sides.
// ---------------------------------------------------------------------------

#include "io.hpp"

#include "detail/dlpack.h"

namespace crd::hesap::tensor
{

// ---- dtype mapping (both directions) ----------------------------------

[[nodiscard]] constexpr DLDataType dlpack_dtype_of(IoDtype d) noexcept
{
    DLDataType t{0U, 0U, 1U};
    switch (d)
    {
    case IoDtype::F32:
        t.code = kDLFloat;
        t.bits = 32U;
        break;
    case IoDtype::F64:
        t.code = kDLFloat;
        t.bits = 64U;
        break;
    case IoDtype::F16:
        t.code = kDLFloat;
        t.bits = 16U;
        break;
    case IoDtype::Bf16:
        t.code = kDLBfloat;
        t.bits = 16U;
        break;
    case IoDtype::I8:
        t.code = kDLInt;
        t.bits = 8U;
        break;
    case IoDtype::I16:
        t.code = kDLInt;
        t.bits = 16U;
        break;
    case IoDtype::I32:
        t.code = kDLInt;
        t.bits = 32U;
        break;
    case IoDtype::I64:
        t.code = kDLInt;
        t.bits = 64U;
        break;
    case IoDtype::U8:
        t.code = kDLUInt;
        t.bits = 8U;
        break;
    case IoDtype::U16:
        t.code = kDLUInt;
        t.bits = 16U;
        break;
    case IoDtype::U32:
        t.code = kDLUInt;
        t.bits = 32U;
        break;
    case IoDtype::U64:
        t.code = kDLUInt;
        t.bits = 64U;
        break;
    case IoDtype::Bool:
        t.code = kDLBool;
        t.bits = 8U;
        break;
    case IoDtype::C32:
        t.code = kDLComplex;
        t.bits = 64U;
        break;
    case IoDtype::C64:
        t.code = kDLComplex;
        t.bits = 128U;
        break;
    case IoDtype::Fp8E4m3:
        t.code = kDLFloat8_e4m3fn; // crd::math e4m3 IS the OCP e4m3fn convention
        t.bits = 8U;
        break;
    case IoDtype::Fp8E5m2:
        t.code = kDLFloat8_e5m2;
        t.bits = 8U;
        break;
    }
    return t;
}

[[nodiscard]] constexpr bool dlpack_dtype_to_io(DLDataType t, IoDtype& out) noexcept
{
    if (t.lanes != 1U)
    {
        return false; // vector lanes are not part of our interchange set
    }
    constexpr IoDtype all[] = {IoDtype::F32,  IoDtype::F64, IoDtype::F16, IoDtype::Bf16,    IoDtype::I8,
                               IoDtype::I16,  IoDtype::I32, IoDtype::I64, IoDtype::U8,      IoDtype::U16,
                               IoDtype::U32,  IoDtype::U64, IoDtype::Bool, IoDtype::C32,    IoDtype::C64,
                               IoDtype::Fp8E4m3, IoDtype::Fp8E5m2};
    for (const IoDtype d : all)
    {
        const DLDataType m = dlpack_dtype_of(d);
        if (m.code == t.code && m.bits == t.bits)
        {
            out = d;
            return true;
        }
    }
    return false;
}

namespace dlpackdetail
{

// The whole export descriptor lives in ONE allocation: the managed struct,
// its shape/strides arrays, and the allocator to free it with. The deleter
// frees this block only — never the tensor storage (see the header comment).
struct ExportBlockVersioned
{
    DLManagedTensorVersioned managed;
    crd::memory::IAllocator* alloc;
    crd::i64 shape[kMaxRank];
    crd::i64 strides[kMaxRank];
};

struct ExportBlockLegacy
{
    DLManagedTensor managed;
    crd::memory::IAllocator* alloc;
    crd::i64 shape[kMaxRank];
    crd::i64 strides[kMaxRank];
};

inline void versioned_deleter(DLManagedTensorVersioned* self)
{
    if (self != nullptr)
    {
        auto* blk = static_cast<ExportBlockVersioned*>(self->manager_ctx);
        blk->alloc->deallocate(blk);
    }
}

inline void legacy_deleter(DLManagedTensor* self)
{
    if (self != nullptr)
    {
        auto* blk = static_cast<ExportBlockLegacy*>(self->manager_ctx);
        blk->alloc->deallocate(blk);
    }
}

template <typename T>
inline void fill_dl_tensor(DLTensor& dl, const TensorView<T>& view, crd::i64* shape, crd::i64* strides) noexcept
{
    using U = std::remove_const_t<T>;
    for (crd::u32 d = 0; d < view.rank(); ++d)
    {
        shape[d] = static_cast<crd::i64>(view.shape(d));
        strides[d] = view.stride(d);
    }
    // spec note: a size-zero tensor should carry data == NULL
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast) — the READ_ONLY flag carries constness across the ABI
    dl.data = view.size() == 0U ? nullptr : const_cast<void*>(static_cast<const void*>(view.data()));
    dl.device = DLDevice{kDLCPU, 0};
    dl.ndim = static_cast<crd::i32>(view.rank());
    dl.dtype = dlpack_dtype_of(io_dtype_of<U>());
    dl.shape = shape;
    dl.strides = strides;
    dl.byte_offset = 0U;
}

} // namespace dlpackdetail

// ---- producer ----------------------------------------------------------

// Export a view as a versioned DLPack capsule (the current standard ABI).
// Zero-copy: the capsule borrows view's buffer (see the lifetime contract).
// A const element type stamps DLPACK_FLAG_BITMASK_READ_ONLY automatically;
// extra_flags is OR-ed in.
template <typename T>
[[nodiscard]] TensorStatus dlpack_export(const TensorView<T>& view, crd::memory::IAllocator* alloc,
                                         DLManagedTensorVersioned*& out, crd::u64 extra_flags = 0U) noexcept
{
    if (alloc == nullptr)
    {
        return TensorStatus::BadInput;
    }
    auto* blk = static_cast<dlpackdetail::ExportBlockVersioned*>(
        alloc->allocate(sizeof(dlpackdetail::ExportBlockVersioned), alignof(dlpackdetail::ExportBlockVersioned)));
    if (blk == nullptr)
    {
        return TensorStatus::AllocFailed;
    }
    blk->alloc = alloc;
    blk->managed.version = DLPackVersion{DLPACK_MAJOR_VERSION, DLPACK_MINOR_VERSION};
    blk->managed.manager_ctx = blk;
    blk->managed.deleter = &dlpackdetail::versioned_deleter;
    blk->managed.flags = extra_flags | (std::is_const_v<T> ? DLPACK_FLAG_BITMASK_READ_ONLY : 0U);
    dlpackdetail::fill_dl_tensor(blk->managed.dl_tensor, view, blk->shape, blk->strides);
    out = &blk->managed;
    return TensorStatus::Ok;
}

// Legacy (pre-v1.0) DLManagedTensor export — frameworks still consuming the
// old ABI. Same borrow semantics; read-only-ness cannot be expressed.
template <typename T>
[[nodiscard]] TensorStatus dlpack_export_legacy(const TensorView<T>& view, crd::memory::IAllocator* alloc,
                                                DLManagedTensor*& out) noexcept
{
    if (alloc == nullptr)
    {
        return TensorStatus::BadInput;
    }
    auto* blk = static_cast<dlpackdetail::ExportBlockLegacy*>(
        alloc->allocate(sizeof(dlpackdetail::ExportBlockLegacy), alignof(dlpackdetail::ExportBlockLegacy)));
    if (blk == nullptr)
    {
        return TensorStatus::AllocFailed;
    }
    blk->alloc = alloc;
    blk->managed.manager_ctx = blk;
    blk->managed.deleter = &dlpackdetail::legacy_deleter;
    dlpackdetail::fill_dl_tensor(blk->managed.dl_tensor, view, blk->shape, blk->strides);
    out = &blk->managed;
    return TensorStatus::Ok;
}

// ---- consumer ----------------------------------------------------------

// Owning handle for an imported capsule. The handle OWNS the capsule and
// calls its deleter exactly once; view() borrows the PRODUCER's buffer and
// must not outlive the handle. Move-only.
template <typename T> class DlpackImported
{
public:
    DlpackImported() noexcept = default;
    DlpackImported(const DlpackImported&) = delete;
    DlpackImported& operator=(const DlpackImported&) = delete;

    DlpackImported(DlpackImported&& other) noexcept
        : m_versioned(other.m_versioned), m_legacy(other.m_legacy), m_view(other.m_view),
          m_read_only(other.m_read_only)
    {
        other.m_versioned = nullptr;
        other.m_legacy = nullptr;
        other.m_view = TensorView<T>{};
    }

    DlpackImported& operator=(DlpackImported&& other) noexcept
    {
        if (this != &other)
        {
            release();
            m_versioned = other.m_versioned;
            m_legacy = other.m_legacy;
            m_view = other.m_view;
            m_read_only = other.m_read_only;
            other.m_versioned = nullptr;
            other.m_legacy = nullptr;
            other.m_view = TensorView<T>{};
        }
        return *this;
    }

    ~DlpackImported() { release(); }

    // Invoke the producer's deleter (exactly once) and drop the view.
    void release() noexcept
    {
        if (m_versioned != nullptr)
        {
            if (m_versioned->deleter != nullptr)
            {
                m_versioned->deleter(m_versioned);
            }
            m_versioned = nullptr;
        }
        if (m_legacy != nullptr)
        {
            if (m_legacy->deleter != nullptr)
            {
                m_legacy->deleter(m_legacy);
            }
            m_legacy = nullptr;
        }
        m_view = TensorView<T>{};
    }

    [[nodiscard]] bool valid() const noexcept { return m_versioned != nullptr || m_legacy != nullptr; }
    [[nodiscard]] const TensorView<T>& view() const noexcept { return m_view; }
    [[nodiscard]] bool read_only() const noexcept { return m_read_only; }

private:
    template <typename U>
    friend TensorStatus dlpack_import(DLManagedTensorVersioned* src, DlpackImported<U>& out) noexcept;
    template <typename U>
    friend TensorStatus dlpack_import_legacy(DLManagedTensor* src, DlpackImported<U>& out) noexcept;

    DLManagedTensorVersioned* m_versioned = nullptr;
    DLManagedTensor* m_legacy = nullptr;
    TensorView<T> m_view{};
    bool m_read_only = false;
};

namespace dlpackdetail
{

// Shared DLTensor -> TensorView validation/wrap (element type must match
// exactly — bit-exact interchange, never a silent convert).
template <typename T>
[[nodiscard]] inline TensorStatus wrap_dl_tensor(const DLTensor& dl, TensorView<T>& out) noexcept
{
    using U = std::remove_const_t<T>;
    if (dl.device.device_type != kDLCPU)
    {
        return TensorStatus::Unsupported; // CPU substrate — device tensors need a copy-in path
    }
    if (dl.ndim < 0)
    {
        return TensorStatus::BadInput;
    }
    if (static_cast<crd::u32>(dl.ndim) > kMaxRank)
    {
        return TensorStatus::RankOverflow;
    }
    const DLDataType want = dlpack_dtype_of(io_dtype_of<U>());
    if (dl.dtype.code != want.code || dl.dtype.bits != want.bits || dl.dtype.lanes != want.lanes)
    {
        return TensorStatus::ShapeMismatch; // dtype mismatch — same status as typed file reads
    }
    const crd::u32 rank = static_cast<crd::u32>(dl.ndim);
    crd::u64 shape[kMaxRank] = {};
    crd::i64 strides[kMaxRank] = {};
    crd::u64 elems = 1;
    for (crd::u32 d = 0; d < rank; ++d)
    {
        if (dl.shape[d] < 0)
        {
            return TensorStatus::BadInput;
        }
        shape[d] = static_cast<crd::u64>(dl.shape[d]);
        elems *= shape[d];
    }
    if (dl.strides != nullptr)
    {
        for (crd::u32 d = 0; d < rank; ++d)
        {
            strides[d] = dl.strides[d];
        }
    }
    else // NULL strides = compact row-major (the spec default)
    {
        crd::i64 s = 1;
        for (crd::u32 d = rank; d-- > 0U;)
        {
            strides[d] = s;
            s *= static_cast<crd::i64>(shape[d]);
        }
    }
    if (elems == 0U)
    {
        out = TensorView<T>{nullptr, {shape, rank}, {strides, rank}};
        return TensorStatus::Ok;
    }
    if (dl.data == nullptr)
    {
        return TensorStatus::BadInput;
    }
    const crd::u8* base = static_cast<const crd::u8*>(dl.data) + dl.byte_offset;
    if ((reinterpret_cast<crd::u64>(base) % alignof(U)) != 0U)
    {
        return TensorStatus::BadInput; // misaligned element pointer would be UB
    }
    out = TensorView<T>{reinterpret_cast<T*>(const_cast<crd::u8*>(base)), {shape, rank}, {strides, rank}}; // NOLINT
    return TensorStatus::Ok;
}

} // namespace dlpackdetail

// Import a versioned capsule. On success `out` owns the capsule (deleter
// called exactly once by the handle). On failure ownership stays with the
// caller, EXCEPT a major-version mismatch: per the spec the deleter is the
// only safe member, we call it and return Unsupported. A capsule flagged
// READ_ONLY imports only into a const element type (T = const U).
template <typename T>
[[nodiscard]] TensorStatus dlpack_import(DLManagedTensorVersioned* src, DlpackImported<T>& out) noexcept
{
    if (src == nullptr)
    {
        return TensorStatus::BadInput;
    }
    if (src->version.major > DLPACK_MAJOR_VERSION)
    {
        if (src->deleter != nullptr)
        {
            src->deleter(src); // the spec-mandated disposal on ABI mismatch
        }
        return TensorStatus::Unsupported;
    }
    const bool read_only = (src->flags & DLPACK_FLAG_BITMASK_READ_ONLY) != 0U;
    if (read_only && !std::is_const_v<T>)
    {
        return TensorStatus::Unsupported; // honour the producer's write protection
    }
    TensorView<T> view;
    const TensorStatus st = dlpackdetail::wrap_dl_tensor(src->dl_tensor, view);
    if (st != TensorStatus::Ok)
    {
        return st;
    }
    out.release();
    out.m_versioned = src;
    out.m_view = view;
    out.m_read_only = read_only;
    return TensorStatus::Ok;
}

// Import a legacy (pre-v1.0) capsule. No version/flags fields exist; the
// buffer is assumed writable per the old ABI's convention.
template <typename T>
[[nodiscard]] TensorStatus dlpack_import_legacy(DLManagedTensor* src, DlpackImported<T>& out) noexcept
{
    if (src == nullptr)
    {
        return TensorStatus::BadInput;
    }
    TensorView<T> view;
    const TensorStatus st = dlpackdetail::wrap_dl_tensor(src->dl_tensor, view);
    if (st != TensorStatus::Ok)
    {
        return st;
    }
    out.release();
    out.m_legacy = src;
    out.m_view = view;
    out.m_read_only = false;
    return TensorStatus::Ok;
}

} // namespace crd::hesap::tensor
