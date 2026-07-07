#pragma once

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/tensor/io.hpp>
#include <crd/hesap/tensor/tensor.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/resources/crdr.hpp>
#include <crd/resources/resource_id.hpp>

#include <cstring>

namespace crd::hesap::resources
{
// -----------------------------------------------------------------------
// Phase 3.1.6 v14-l -- dense tensors as cooked engine resources: the 'TNSR'
// CRDR artifact, following the ADR-0084 'HMTX' pattern exactly (this module
// is the bridge: it depends on crd-resources AND crd-hesap-tensor; neither
// depends on it).
//
// A cooked tensor is a CRDR container with type_fourcc 'TNSR' and two
// chunks (CrdrWriter FourCC-sorts at finish, so the loader must use
// crdr_find_chunk, never positional access):
//
//   'TNHD' (96 bytes)   -- TensorFileInfo (header; pinned layout).
//   'TNVL' (raw bytes)  -- the C-order payload (element type per the
//                          dtype tag; storage dtypes carry bit patterns).
//
// The dtype tag is crd::hesap::tensor::IoDtype -- PINNED + APPEND-ONLY
// (ADR-0084 posture): a renumber silently mis-types every cooked .crdr.
//
// payload_hash is FNV-1a-64 over the payload bytes; the loader recomputes
// and asserts on mismatch (the free corruption detector, mirroring
// MatrixFileInfo::topology_hash).
//
// Endianness: little-endian-host memcpy posture (matches every cooked
// artifact family). Cook-time in-memory only: this header emits/consumes
// CRDR bytes; file I/O lives with the caller (asset cooker / tools).
// -----------------------------------------------------------------------

// CRDR type + chunk FourCCs.
inline constexpr crd::u32 kFourCC_TNSR = crd::resources::make_fourcc('T', 'N', 'S', 'R'); // container type
inline constexpr crd::u32 kFourCC_TNHD = crd::resources::make_fourcc('T', 'N', 'H', 'D'); // header
inline constexpr crd::u32 kFourCC_TNVL = crd::resources::make_fourcc('T', 'N', 'V', 'L'); // values

namespace tensordetail
{

// FNV-1a-64 over raw bytes (the house corruption-detector hash — the same
// construction sparse_pattern.cpp uses for topology_hash).
[[nodiscard]] inline crd::u64 fnv1a_bytes(crd::containers::ConstSpan<crd::u8> bytes) noexcept
{
    crd::u64 h = 0xCBF29CE484222325ULL;
    for (const crd::u8 b : bytes)
    {
        h ^= b;
        h *= 0x00000100000001B3ULL;
    }
    return h;
}

} // namespace tensordetail

// 'TNHD' payload -- PINNED at 96 bytes (natural layout, no padding):
//   +0  u8  dtype          (crd::hesap::tensor::IoDtype -- append-only)
//   +1  u8  rank           (<= kMaxRank = 8)
//   +2  u8  reserved[6]
//   +8  u64 shape[8]       (dims beyond rank are zero)
//   +72 u64 element_count  (product of shape -- redundant, validated)
//   +80 u64 payload_hash   (FNV-1a-64 of TNVL; loader asserts on mismatch)
//   +88 u64 reserved2      (future: frame stamp / quantization tag)
struct TensorFileInfo
{
    crd::u8 dtype = 0;
    crd::u8 rank = 0;
    crd::u8 reserved[6] = {0, 0, 0, 0, 0, 0};
    crd::u64 shape[crd::hesap::tensor::kMaxRank] = {};
    crd::u64 element_count = 0;
    crd::u64 payload_hash = 0;
    crd::u64 reserved2 = 0;
};
static_assert(sizeof(TensorFileInfo) == 96, "TensorFileInfo pinned at 96 bytes (v14-l; ADR-0084 posture)");
static_assert(alignof(TensorFileInfo) == 8, "TensorFileInfo natural 8-byte alignment");

namespace tensordetail
{
[[nodiscard]] inline crd::containers::ConstSpan<crd::u8> tn_bytes_of(const void* p, crd::usize n) noexcept
{
    return crd::containers::ConstSpan<crd::u8>{reinterpret_cast<const crd::u8*>(p), n};
}
} // namespace tensordetail

// Cook a raw dtype-tagged payload into a 'TNSR' CRDR blob (the type-erased
// cook site — storage dtypes pass their bit patterns). Returns an empty
// blob and sets `status` on invalid input.
[[nodiscard]] inline crd::containers::Array<crd::u8>
cook_tensor_bits(crd::memory::IAllocator* alloc, crd::resources::ResourceId id, crd::hesap::tensor::IoDtype dtype,
                 crd::containers::ConstSpan<crd::u64> shape, crd::containers::ConstSpan<crd::u8> payload,
                 crd::hesap::tensor::TensorStatus& status)
{
    using crd::hesap::tensor::TensorStatus;
    status = TensorStatus::Ok;
    if (shape.size() > crd::hesap::tensor::kMaxRank)
    {
        status = TensorStatus::RankOverflow;
        return crd::containers::Array<crd::u8>{alloc};
    }
    crd::u64 elems = 1;
    for (const crd::u64 s : shape)
    {
        elems *= s;
    }
    if (payload.size() != elems * crd::hesap::tensor::io_dtype_size(dtype))
    {
        status = TensorStatus::BadInput;
        return crd::containers::Array<crd::u8>{alloc};
    }

    TensorFileInfo info{};
    info.dtype = static_cast<crd::u8>(dtype);
    info.rank = static_cast<crd::u8>(shape.size());
    for (crd::usize d = 0; d < shape.size(); ++d)
    {
        info.shape[d] = shape[d];
    }
    info.element_count = elems;
    info.payload_hash = tensordetail::fnv1a_bytes(payload);

    crd::resources::CrdrWriter writer{alloc, id, kFourCC_TNSR};
    writer.add_chunk(kFourCC_TNHD, tensordetail::tn_bytes_of(&info, sizeof(info)));
    writer.add_chunk(kFourCC_TNVL, payload);
    return writer.finish();
}

// Cook a typed C-contiguous view (the common cook site).
template <typename T>
[[nodiscard]] crd::containers::Array<crd::u8>
cook_tensor(crd::memory::IAllocator* alloc, crd::resources::ResourceId id,
            const crd::hesap::tensor::TensorView<const T>& view, crd::hesap::tensor::TensorStatus& status)
{
    using crd::hesap::tensor::TensorStatus;
    if (!view.is_contiguous())
    {
        status = TensorStatus::NotContiguous;
        return crd::containers::Array<crd::u8>{alloc};
    }
    return cook_tensor_bits(alloc, id, crd::hesap::tensor::io_dtype_of<T>(), view.shape(),
                            tensordetail::tn_bytes_of(view.data(), static_cast<crd::usize>(view.size()) * sizeof(T)),
                            status);
}

// -----------------------------------------------------------------------
// TensorResource -- the loaded payload (type-erased: header + raw bytes;
// the consumer, who knows T, materialises via build<T>() / build_bits<T>()).
// Mirrors SparseMatrixResource's loader-fill surface.
// -----------------------------------------------------------------------
class TensorResource
{
public:
    explicit TensorResource(crd::memory::IAllocator* alloc) : m_payload(alloc) {}

    TensorResource(const TensorResource&) = delete;
    TensorResource& operator=(const TensorResource&) = delete;
    TensorResource(TensorResource&&) noexcept = default;
    TensorResource& operator=(TensorResource&&) noexcept = default;
    ~TensorResource() = default;

    [[nodiscard]] crd::hesap::tensor::IoDtype dtype() const noexcept
    {
        return static_cast<crd::hesap::tensor::IoDtype>(m_info.dtype);
    }
    [[nodiscard]] crd::u32 rank() const noexcept { return m_info.rank; }
    [[nodiscard]] crd::containers::ConstSpan<crd::u64> shape() const noexcept
    {
        return {m_info.shape, m_info.rank};
    }
    [[nodiscard]] crd::u64 element_count() const noexcept { return m_info.element_count; }
    [[nodiscard]] const TensorFileInfo& info() const noexcept { return m_info; }
    [[nodiscard]] crd::containers::ConstSpan<crd::u8> payload() const noexcept
    {
        return crd::containers::as_const_span(m_payload);
    }

    // ---- Loader fill surface (mirrors SparseMatrixResource) ----
    void set_info(const TensorFileInfo& i) noexcept { m_info = i; }
    [[nodiscard]] crd::containers::Array<crd::u8>& mutable_payload() noexcept { return m_payload; }

    // Materialise an owned Tensor<T>. Asserts the T/dtype match and the
    // recomputed payload hash (corruption detector — the ADR-0084 pattern).
    template <typename T>
    [[nodiscard]] crd::hesap::tensor::Tensor<T> build(crd::memory::IAllocator* alloc) const
    {
        CRD_ASSERT_MSG(crd::hesap::tensor::io_dtype_of<T>() == dtype(),
                       "TensorResource::build<T>: T does not match the stored dtype tag");
        return build_bits<T>(alloc);
    }

    // Bits-carrier materialisation for storage dtypes (f16/bf16/bool/fp8):
    // T is the bit-pattern type (u16/u8) and must match the dtype's size.
    template <typename T>
    [[nodiscard]] crd::hesap::tensor::Tensor<T> build_bits(crd::memory::IAllocator* alloc) const
    {
        using crd::hesap::tensor::Tensor;
        using crd::hesap::tensor::TensorStatus;
        CRD_ASSERT_MSG(crd::hesap::tensor::io_dtype_size(dtype()) == sizeof(T),
                       "TensorResource::build_bits<T>: sizeof(T) does not match the stored dtype");
        CRD_ASSERT_MSG(m_payload.size() == m_info.element_count * sizeof(T),
                       "TensorResource::build_bits<T>: payload byte count != element_count * sizeof(T)");
        CRD_ASSERT_MSG(tensordetail::fnv1a_bytes(payload()) == m_info.payload_hash,
                       "TensorResource::build_bits<T>: payload hash mismatch (corruption)");
        Tensor<T> t(alloc);
        [[maybe_unused]] const TensorStatus st = t.resize(shape());
        CRD_ASSERT_MSG(st == TensorStatus::Ok, "TensorResource::build_bits<T>: resize failed");
        if (!m_payload.empty())
        {
            std::memcpy(t.data(), m_payload.data(), m_payload.size());
        }
        return t;
    }

private:
    TensorFileInfo m_info{};
    crd::containers::Array<crd::u8> m_payload;
};

// Parse a 'TNSR' CRDR blob into a TensorResource (validates the container,
// the header, and the payload size; the hash gate rides build_bits).
// HOME-> flag: the ResourceManager ILoader registration (a src/ loader .cpp
// like matrix_resource_loader.cpp + CMake edit) is the integrator's wiring
// step — this header owns the format, cook, and load logic.
[[nodiscard]] inline crd::hesap::tensor::TensorStatus load_tensor(crd::containers::ConstSpan<crd::u8> crdr_bytes,
                                                                  TensorResource& out,
                                                                  crd::memory::IAllocator* alloc)
{
    using crd::hesap::tensor::TensorStatus;
    crd::resources::CrdrFile file(alloc);
    if (crd::resources::crdr_read(crdr_bytes, file, alloc) != crd::resources::CrdrError::Ok)
    {
        return TensorStatus::BadInput;
    }
    if (file.type_fourcc != kFourCC_TNSR)
    {
        return TensorStatus::BadInput;
    }
    const crd::resources::CrdrChunk* hd = crd::resources::crdr_find_chunk(file, kFourCC_TNHD);
    const crd::resources::CrdrChunk* vl = crd::resources::crdr_find_chunk(file, kFourCC_TNVL);
    if (hd == nullptr || vl == nullptr || hd->payload.size() != sizeof(TensorFileInfo))
    {
        return TensorStatus::BadInput;
    }
    TensorFileInfo info{};
    std::memcpy(&info, hd->payload.data(), sizeof(info));
    if (info.rank > crd::hesap::tensor::kMaxRank)
    {
        return TensorStatus::RankOverflow;
    }
    const crd::u32 esize = crd::hesap::tensor::io_dtype_size(static_cast<crd::hesap::tensor::IoDtype>(info.dtype));
    if (esize == 0U)
    {
        return TensorStatus::Unsupported; // dtype tag from a newer schema
    }
    crd::u64 elems = 1;
    for (crd::u32 d = 0; d < info.rank; ++d)
    {
        elems *= info.shape[d];
    }
    if (elems != info.element_count || vl->payload.size() != elems * esize)
    {
        return TensorStatus::BadInput;
    }
    out.set_info(info);
    crd::containers::Array<crd::u8>& dst = out.mutable_payload();
    dst.clear();
    if (!dst.try_reserve(vl->payload.size()))
    {
        return TensorStatus::AllocFailed;
    }
    dst.resize_uninitialized(vl->payload.size());
    if (!vl->payload.empty())
    {
        std::memcpy(dst.data(), vl->payload.data(), vl->payload.size());
    }
    return TensorStatus::Ok;
}

} // namespace crd::hesap::resources
