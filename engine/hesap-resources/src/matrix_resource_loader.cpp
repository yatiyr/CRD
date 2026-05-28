#include <crd/hesap/resources/matrix_resource.hpp>
#include <crd/hesap/resources/matrix_resource_loader.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>
#include <crd/memory/allocators/thread_safe_allocator.hpp>
#include <crd/resources/crdr.hpp>
#include <crd/resources/loader.hpp>
#include <crd/resources/resource_manager.hpp>

#include <cstring>
#include <memory>

namespace crd::hesap::resources
{
void hesap_matrix_resource_anchor() noexcept {}

[[nodiscard]] bool read_matrix_resource(crd::containers::ConstSpan<crd::u8> bytes, SparseMatrixResource& out,
                                        crd::memory::IAllocator* scratch)
{
    using namespace crd::resources;

    CrdrFile file(scratch);
    if (crdr_read(bytes, file, scratch) != CrdrError::Ok)
    {
        return false;
    }

    const CrdrChunk* hd = crdr_find_chunk(file, kFourCC_MXHD);
    const CrdrChunk* op = crdr_find_chunk(file, kFourCC_MXOP);
    const CrdrChunk* ii = crdr_find_chunk(file, kFourCC_MXII);
    const CrdrChunk* vl = crdr_find_chunk(file, kFourCC_MXVL);
    if (hd == nullptr || op == nullptr || ii == nullptr || vl == nullptr)
    {
        return false;
    }
    if (hd->payload.size() != sizeof(MatrixFileInfo))
    {
        return false;
    }

    MatrixFileInfo info{};
    std::memcpy(&info, hd->payload.data(), sizeof(MatrixFileInfo));

    // Validate chunk sizes against the header (reject truncation / mistyping).
    const crd::usize expect_outer = (static_cast<crd::usize>(info.rows) + 1U) * sizeof(crd::u32);
    const crd::usize expect_inner = static_cast<crd::usize>(info.nnz) * sizeof(crd::u32);
    const crd::usize value_sz = variant_value_size(static_cast<MatrixVariant>(info.variant));
    const crd::usize expect_vals = static_cast<crd::usize>(info.nnz) * value_sz;
    if (value_sz == 0U || op->payload.size() != expect_outer || ii->payload.size() != expect_inner ||
        vl->payload.size() != expect_vals)
    {
        return false;
    }

    out.set_info(info);

    out.mutable_outer().resize(static_cast<crd::usize>(info.rows) + 1U);
    std::memcpy(out.mutable_outer().data(), op->payload.data(), expect_outer);

    out.mutable_inner().resize(static_cast<crd::usize>(info.nnz));
    if (expect_inner != 0U)
    {
        std::memcpy(out.mutable_inner().data(), ii->payload.data(), expect_inner);
    }

    out.mutable_value_bytes().resize(expect_vals);
    if (expect_vals != 0U)
    {
        std::memcpy(out.mutable_value_bytes().data(), vl->payload.data(), expect_vals);
    }

    return true;
}

namespace
{
constexpr crd::u32 kMatrixLoaderVersion = 1U;

// Loads a 'HMTX' CRDR blob into a SparseMatrixResource (type-erased CSR).
// Single loader for all 4 variants -- the element type is the MXHD tag.
class MatrixResourceLoaderImpl final : public crd::resources::ILoader
{
public:
    [[nodiscard]] crd::u32 type_fourcc() const noexcept override { return kFourCC_HMTX; }

    [[nodiscard]] crd::u32 loader_version() const noexcept override { return kMatrixLoaderVersion; }

    [[nodiscard]] void* load(const crd::resources::LoadContext& ctx) override
    {
        void* raw = m_alloc.allocate(sizeof(SparseMatrixResource), alignof(SparseMatrixResource));
        if (raw == nullptr)
        {
            return nullptr;
        }
        auto* res = new (raw) SparseMatrixResource(&m_alloc);
        if (!read_matrix_resource(ctx.bytes, *res, &m_alloc))
        {
            res->~SparseMatrixResource();
            m_alloc.deallocate(res);
            return nullptr;
        }
        return res;
    }

    void unload(void* payload) noexcept override
    {
        if (payload == nullptr)
        {
            return;
        }
        auto* res = static_cast<SparseMatrixResource*>(payload);
        res->~SparseMatrixResource();
        m_alloc.deallocate(res);
    }

private:
    // Concurrent async loads share this single loader instance; wrapper serializes the
    // single-threaded TLSF heap while keeping per-type pool locality (see texture loader).
    crd::memory::GrowableTlsfAllocator m_inner;
    crd::memory::ThreadSafeAllocator   m_alloc{&m_inner};
};

} // namespace

void register_hesap_matrix_loader(crd::resources::ResourceManager* rm)
{
    rm->register_loader(std::make_unique<MatrixResourceLoaderImpl>());
}

} // namespace crd::hesap::resources
