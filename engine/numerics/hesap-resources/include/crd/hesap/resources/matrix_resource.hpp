#pragma once

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/complex.hpp>
#include <crd/hesap/sparse/sparse_format.hpp>
#include <crd/hesap/sparse/sparse_matrix.hpp>
#include <crd/hesap/sparse/sparse_pattern.hpp>
#include <crd/hesap/sparse/sparse_values.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/resources/crdr.hpp>

#include <cstring>
#include <type_traits>

namespace crd::hesap::resources
{
// -----------------------------------------------------------------------
// Phase 3.1.6 v4-corpus -- SuiteSparse matrices as first-class cooked
// engine resources (ADR-0084). This module is the bridge: it depends on
// BOTH crd-resources (CRDR/ILoader) and crd-hesap-sparse (SparseMatrix),
// and neither depends on it (one-way edge).
//
// A cooked matrix is a CRDR container with type_fourcc 'HMTX' and four
// chunks (CrdrWriter FourCC-sorts at finish, so the loader must use
// crdr_find_chunk, never positional access):
//
//   'MXHD' (40 bytes) -- MatrixFileInfo (header; pinned layout).
//   'MXOP' (u32 x rows+1) -- CSR outer_ptr.
//   'MXII' (u32 x nnz)    -- CSR inner_idx (column indices).
//   'MXVL' (T   x nnz)    -- values, raw T bytes (T per the variant tag).
//
// Endianness: little-endian-host memcpy posture (matches Profile/Mesh
// cooked artifacts). A future ARM/big-endian target is a project-wide
// concern, not this slice's.
// -----------------------------------------------------------------------

// CRDR type + chunk FourCCs.
inline constexpr crd::u32 kFourCC_HMTX = crd::resources::make_fourcc('H', 'M', 'T', 'X'); // container type
inline constexpr crd::u32 kFourCC_MXHD = crd::resources::make_fourcc('M', 'X', 'H', 'D'); // header
inline constexpr crd::u32 kFourCC_MXOP = crd::resources::make_fourcc('M', 'X', 'O', 'P'); // outer_ptr
inline constexpr crd::u32 kFourCC_MXII = crd::resources::make_fourcc('M', 'X', 'I', 'I'); // inner_idx
inline constexpr crd::u32 kFourCC_MXVL = crd::resources::make_fourcc('M', 'X', 'V', 'L'); // values

// Element-type tag stored in the header. PINNED + APPEND-ONLY (ADR-0084):
// values must NEVER be renumbered -- a reorder silently mis-types every
// cooked .crdr on disk. New element types append at the end.
enum class MatrixVariant : crd::u8
{
    F32 = 0,
    F64 = 1,
    C32 = 2, // Complex<f32>
    C64 = 3,
};

namespace detail
{
template <typename> inline constexpr bool kAlwaysFalse = false;
} // namespace detail

// Map a hesap scalar type to its pinned variant tag.
template <typename T> [[nodiscard]] constexpr MatrixVariant matrix_variant_of() noexcept
{
    if constexpr (std::is_same_v<T, crd::f32>)
    {
        return MatrixVariant::F32;
    }
    else if constexpr (std::is_same_v<T, crd::f64>)
    {
        return MatrixVariant::F64;
    }
    else if constexpr (std::is_same_v<T, crd::hesap::Complex<crd::f32>>)
    {
        return MatrixVariant::C32;
    }
    else if constexpr (std::is_same_v<T, crd::hesap::Complex<crd::f64>>)
    {
        return MatrixVariant::C64;
    }
    else
    {
        static_assert(detail::kAlwaysFalse<T>, "matrix_variant_of: T must be f32 / f64 / Complex<f32> / Complex<f64>");
        return MatrixVariant::F64;
    }
}

// Byte size of one stored value for a variant.
[[nodiscard]] constexpr crd::u32 variant_value_size(MatrixVariant v) noexcept
{
    switch (v)
    {
        case MatrixVariant::F32:
            return 4U;
        case MatrixVariant::F64:
            return 8U;
        case MatrixVariant::C32:
            return 8U; // 2 x f32
        case MatrixVariant::C64:
            return 16U; // 2 x f64
    }
    return 0U;
}

// 'MXHD' payload -- PINNED at 40 bytes (natural layout, no padding):
//   +0  u32 rows
//   +4  u32 cols
//   +8  u64 nnz            (u64: SuiteSparse approaches 4G entries)
//   +16 u8  variant        (MatrixVariant)
//   +17 u8  format         (SparseFormat; CSR=0 today, BSR/ELL/... reserved)
//   +18 u8  reserved[6]
//   +24 u64 topology_hash  (v1a determinism moat; loader asserts on mismatch)
//   +32 u64 frame_stamp    (SparseValues frame stamp; zero-extended u32)
struct MatrixFileInfo
{
    crd::u32 rows = 0;
    crd::u32 cols = 0;
    crd::u64 nnz = 0;
    crd::u8 variant = 0;
    crd::u8 format = 0;
    crd::u8 reserved[6] = {0, 0, 0, 0, 0, 0};
    crd::u64 topology_hash = 0;
    crd::u64 frame_stamp = 0;
};
static_assert(sizeof(MatrixFileInfo) == 40, "MatrixFileInfo pinned at 40 bytes (ADR-0084)");
static_assert(alignof(MatrixFileInfo) == 8, "MatrixFileInfo natural 8-byte alignment");

// -----------------------------------------------------------------------
// SparseMatrixResource -- the loaded payload owned by ResourceManager.
//
// Type-erased: holds the header + the (always-u32) CSR structure arrays +
// the raw value bytes (T varies, so values are stored as bytes). The loader
// fills it; the consumer, who knows T, calls build_csr<T>() to materialise a
// concrete owned SparseMatrix<T, Csr>.
// -----------------------------------------------------------------------
class SparseMatrixResource
{
public:
    explicit SparseMatrixResource(crd::memory::IAllocator* alloc) : m_outer(alloc), m_inner(alloc), m_value_bytes(alloc)
    {
    }

    SparseMatrixResource(const SparseMatrixResource&) = delete;
    SparseMatrixResource& operator=(const SparseMatrixResource&) = delete;
    SparseMatrixResource(SparseMatrixResource&&) noexcept = default;
    SparseMatrixResource& operator=(SparseMatrixResource&&) noexcept = default;
    ~SparseMatrixResource() = default;

    [[nodiscard]] crd::u32 rows() const noexcept { return m_info.rows; }
    [[nodiscard]] crd::u32 cols() const noexcept { return m_info.cols; }
    [[nodiscard]] crd::u64 nnz() const noexcept { return m_info.nnz; }
    [[nodiscard]] MatrixVariant variant() const noexcept { return static_cast<MatrixVariant>(m_info.variant); }
    [[nodiscard]] crd::u64 topology_hash() const noexcept { return m_info.topology_hash; }
    [[nodiscard]] const MatrixFileInfo& info() const noexcept { return m_info; }

    // ---- Loader fill surface (public, like ProfileResource's mutators) ----
    void set_info(const MatrixFileInfo& i) noexcept { m_info = i; }
    [[nodiscard]] crd::containers::Array<crd::u32>& mutable_outer() noexcept { return m_outer; }
    [[nodiscard]] crd::containers::Array<crd::u32>& mutable_inner() noexcept { return m_inner; }
    [[nodiscard]] crd::containers::Array<crd::u8>& mutable_value_bytes() noexcept { return m_value_bytes; }

    // Materialise a concrete owned CSR matrix in `alloc`. Asserts the
    // requested T matches the stored variant tag, and that the recomputed
    // topology hash matches the stored one (free corruption detector).
    template <typename T>
    [[nodiscard]] crd::hesap::sparse::SparseMatrix<T, crd::hesap::sparse::SparseFormat::Csr>
    build_csr(crd::memory::IAllocator* alloc) const
    {
        using namespace crd::hesap::sparse;
        CRD_ASSERT_MSG(matrix_variant_of<T>() == variant(),
                       "SparseMatrixResource::build_csr<T>: T does not match the stored variant tag");
        CRD_ASSERT_MSG(m_value_bytes.size() == static_cast<crd::usize>(m_info.nnz) * sizeof(T),
                       "SparseMatrixResource::build_csr<T>: value byte count != nnz * sizeof(T)");

        SparsePattern pat(alloc);
        pat.rows = m_info.rows;
        pat.cols = m_info.cols;
        pat.format = SparseFormat::Csr;
        pat.block_size = 1;
        pat.outer_ptr.resize(m_outer.size());
        if (!m_outer.empty())
        {
            std::memcpy(pat.outer_ptr.data(), m_outer.data(), m_outer.size() * sizeof(crd::u32));
        }
        pat.inner_idx.resize(m_inner.size());
        if (!m_inner.empty())
        {
            std::memcpy(pat.inner_idx.data(), m_inner.data(), m_inner.size() * sizeof(crd::u32));
        }
        pat.recompute_topology_hash();
        CRD_ASSERT_MSG(pat.topology_hash == m_info.topology_hash,
                       "SparseMatrixResource::build_csr<T>: topology hash mismatch (corruption)");

        SparseValues<T> vals(alloc);
        vals.values.resize(static_cast<crd::usize>(m_info.nnz));
        if (!m_value_bytes.empty())
        {
            std::memcpy(vals.values.data(), m_value_bytes.data(), m_value_bytes.size());
        }
        vals.frame_stamp = static_cast<crd::u32>(m_info.frame_stamp);

        return SparseMatrix<T, SparseFormat::Csr>(std::move(pat), std::move(vals));
    }

private:
    MatrixFileInfo m_info{};
    crd::containers::Array<crd::u32> m_outer;      // CSR outer_ptr (rows+1)
    crd::containers::Array<crd::u32> m_inner;      // CSR inner_idx (nnz)
    crd::containers::Array<crd::u8> m_value_bytes; // values, raw bytes (nnz * sizeof(T))
};

} // namespace crd::hesap::resources
