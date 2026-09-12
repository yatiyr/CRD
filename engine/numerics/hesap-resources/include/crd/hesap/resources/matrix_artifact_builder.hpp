#pragma once

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/resources/matrix_resource.hpp>
#include <crd/hesap/sparse/matrix_market.hpp>
#include <crd/hesap/sparse/sparse_matrix.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/resources/crdr.hpp>
#include <crd/resources/resource_id.hpp>

namespace crd::hesap::resources
{
// -----------------------------------------------------------------------
// MatrixArtifactBuilder -- the cook step (.mtx / SparseMatrix -> 'HMTX'
// CRDR bytes). Cook-time, NOT runtime: honours authoring-text/runtime-binary
// (the .mtx is authoring text; the engine consumes the cooked binary CSR).
//
// Header-only template (T is known at the cook site). Reuses the v1g
// read_matrix_market reader verbatim for the .mtx path.
// -----------------------------------------------------------------------

namespace detail
{
[[nodiscard]] inline crd::containers::ConstSpan<crd::u8> mx_bytes_of(const void* p, crd::usize n) noexcept
{
    return crd::containers::ConstSpan<crd::u8>{reinterpret_cast<const crd::u8*>(p), n};
}
} // namespace detail

// Cook a compressed CSR matrix into a 'HMTX' CRDR blob. The matrix's
// topology_hash must be current (compress()/make_compressed() set it).
template <typename T>
[[nodiscard]] crd::containers::Array<crd::u8>
cook_sparse_matrix(crd::memory::IAllocator* alloc, crd::resources::ResourceId id,
                   const crd::hesap::sparse::SparseMatrix<T, crd::hesap::sparse::SparseFormat::Csr>& a)
{
    using namespace crd::hesap::sparse;
    CRD_ASSERT_MSG(a.is_compressed(), "cook_sparse_matrix: matrix must be compressed CSR");

    const SparsePattern& pat = a.pattern();

    MatrixFileInfo info{};
    info.rows = pat.rows;
    info.cols = pat.cols;
    info.nnz = static_cast<crd::u64>(pat.nnz());
    info.variant = static_cast<crd::u8>(matrix_variant_of<T>());
    info.format = static_cast<crd::u8>(SparseFormat::Csr);
    info.topology_hash = pat.topology_hash;
    info.frame_stamp = static_cast<crd::u64>(a.values().frame_stamp);

    crd::resources::CrdrWriter writer{alloc, id, kFourCC_HMTX};
    writer.add_chunk(kFourCC_MXHD, detail::mx_bytes_of(&info, sizeof(info)));
    writer.add_chunk(kFourCC_MXOP, detail::mx_bytes_of(pat.outer_ptr.data(), pat.outer_ptr.size() * sizeof(crd::u32)));
    writer.add_chunk(kFourCC_MXII, detail::mx_bytes_of(pat.inner_idx.data(), pat.inner_idx.size() * sizeof(crd::u32)));
    writer.add_chunk(kFourCC_MXVL, detail::mx_bytes_of(a.values().values.data(), a.values().size() * sizeof(T)));
    return writer.finish();
}

// Cook Matrix Market text directly into a 'HMTX' CRDR blob. On parse error,
// sets err and returns an empty blob.
template <typename T>
[[nodiscard]] crd::containers::Array<crd::u8>
cook_matrix_market(crd::memory::IAllocator* alloc, crd::resources::ResourceId id, crd::containers::StringView mtx_text,
                   crd::hesap::sparse::MatrixMarketError& err)
{
    auto a = crd::hesap::sparse::read_matrix_market<T>(mtx_text, alloc, err);
    if (!err.ok)
    {
        return crd::containers::Array<crd::u8>{alloc};
    }
    return cook_sparse_matrix<T>(alloc, id, a);
}

} // namespace crd::hesap::resources
