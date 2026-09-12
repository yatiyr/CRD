// ---------------------------------------------------------------------------
// MortonRadixGpuPipeline — cached Vulkan compute pipeline + per-call dispatch
// for the v9a-b2 GPU LSD radix sort. Phase 3.1.7 v9a-b2; v17-i-c: migrated onto
// crd::gpu::VulkanComputeContext (ADR-0099) — 25 dispatches recorded into one
// multi-pass job on the compute queue, ping-pong via direct binding pointers.
//
// The dispatch path (25 dispatches per sort, one submit + fence-wait):
//   1. stage `codes` → GpuOnly (in_codes).
//   2. init: pack (code, id) pairs into pairs_a.
//   3. for pass 0..7: histogram → scan → scatter, ping-ponging pairs_a/pairs_b.
//   4. copy pairs_a → host readback; map into caller-owned Array<MortonPair<u32>>.
// ---------------------------------------------------------------------------

#include <crd/geometry/bvh_gpu/radix_sort.hpp>

#include <crd/containers/string.hpp>
#include <crd/core/assert.hpp>
#include <crd/gpu/compute.hpp>

#include <cstdint>
#include <cstring>

namespace crd::geometry::bvh_gpu
{

namespace
{

// Push-constant blocks per kernel. Each must match the GLSL `layout(push_constant)` declaration EXACTLY.
struct alignas(16) InitPushConstants
{
    std::uint32_t count  = 0U;
    std::uint32_t pad[3] = {0U, 0U, 0U};
};
static_assert(sizeof(InitPushConstants) == 16U, "InitPushConstants must match radix_sort_init.comp");

struct alignas(16) HistogramPushConstants
{
    std::uint32_t count  = 0U;
    std::uint32_t shift  = 0U;
    std::uint32_t pad[2] = {0U, 0U};
};
static_assert(sizeof(HistogramPushConstants) == 16U, "HistogramPushConstants must match radix_sort_histogram.comp");

struct alignas(16) ScanPushConstants
{
    std::uint32_t num_blocks = 0U;
    std::uint32_t pad[3]     = {0U, 0U, 0U};
};
static_assert(sizeof(ScanPushConstants) == 16U, "ScanPushConstants must match radix_sort_scan.comp");

struct alignas(16) ScatterPushConstants
{
    std::uint32_t count  = 0U;
    std::uint32_t shift  = 0U;
    std::uint32_t pad[2] = {0U, 0U};
};
static_assert(sizeof(ScatterPushConstants) == 16U, "ScatterPushConstants must match radix_sort_scatter.comp");

// Cache the pipeline for the cooked kernel `name` (n storage-buffer bindings, push_size push constant) — by name; the
// backend loads its own kernel, this code names no API or file format.
[[nodiscard]] std::unique_ptr<crd::gpu::ComputePipeline>
build_pipeline(crd::gpu::IComputeContext& ctx, crd::containers::StringView shader_dir, crd::containers::StringView name,
               int n_bindings, std::uint32_t push_size) noexcept
{
    return ctx.create_pipeline(shader_dir, name, n_bindings, push_size);
}

} // namespace

struct MortonRadixGpuPipeline::Impl
{
    crd::gpu::IComputeContext*                 ctx = nullptr;
    std::unique_ptr<crd::gpu::ComputePipeline> init{};
    std::unique_ptr<crd::gpu::ComputePipeline> histogram{};
    std::unique_ptr<crd::gpu::ComputePipeline> scan{};
    std::unique_ptr<crd::gpu::ComputePipeline> scatter{};
    bool                                       valid = false;
};

MortonRadixGpuPipeline::MortonRadixGpuPipeline(crd::gpu::IComputeContext&   ctx,
                                                  crd::containers::StringView shader_dir) noexcept
    : m_impl(std::make_unique<Impl>())
{
    auto& impl = *m_impl;
    impl.ctx   = &ctx;

    const auto sv = [](const char* s) { return crd::containers::StringView{s}; };
    impl.init      = build_pipeline(ctx, shader_dir, sv("radix_sort_init"), 2, sizeof(InitPushConstants));
    impl.histogram = build_pipeline(ctx, shader_dir, sv("radix_sort_histogram"), 2, sizeof(HistogramPushConstants));
    impl.scan      = build_pipeline(ctx, shader_dir, sv("radix_sort_scan"), 2, sizeof(ScanPushConstants));
    impl.scatter   = build_pipeline(ctx, shader_dir, sv("radix_sort_scatter"), 3, sizeof(ScatterPushConstants));
    if (impl.init == nullptr || impl.histogram == nullptr || impl.scan == nullptr || impl.scatter == nullptr) { return; }

    impl.valid = true;
}

MortonRadixGpuPipeline::MortonRadixGpuPipeline(MortonRadixGpuPipeline&&) noexcept            = default;
MortonRadixGpuPipeline& MortonRadixGpuPipeline::operator=(MortonRadixGpuPipeline&&) noexcept = default;
MortonRadixGpuPipeline::~MortonRadixGpuPipeline()                                              = default;

bool MortonRadixGpuPipeline::is_valid() const noexcept
{
    return m_impl != nullptr && m_impl->valid;
}

crd::containers::Array<MortonPair<crd::u32>>
MortonRadixGpuPipeline::dispatch_radix_sort(
    crd::containers::ConstSpan<crd::u32> codes,
    crd::memory::IAllocator* alloc) noexcept
{
    crd::containers::Array<MortonPair<crd::u32>> out(alloc);
    if (!is_valid()) { return out; }
    if (codes.empty()) { return out; }

    const crd::u32 count = static_cast<crd::u32>(codes.size());
    CRD_ASSERT_MSG(count <= kRadixMaxItems,
                   "dispatch_radix_sort: input exceeds the v9a-b2 cap (D147 — kRadixMaxItems). "
                   "Beyond this requires the v9a-b2-large recursive-scan follow-on.");

    auto& impl = *m_impl;
    auto& ctx  = *impl.ctx;

    const crd::u64 codes_bytes  = static_cast<crd::u64>(count) * sizeof(crd::u32);
    const crd::u64 pair_bytes   = static_cast<crd::u64>(count) * sizeof(MortonPair<crd::u32>);
    const crd::u32 num_blocks   = (count + kRadixItemsPerBlock - 1U) / kRadixItemsPerBlock;
    const crd::u64 hist_bytes   = static_cast<crd::u64>(num_blocks) * kRadixBins * sizeof(crd::u32);
    const crd::u64 offset_bytes = hist_bytes;

    namespace cu   = crd::gpu::compute_usage;
    using Mem      = crd::gpu::ComputeMemory;
    auto codes_staging   = ctx.create_buffer(codes_bytes, cu::transfer_src, Mem::CpuToGpu);
    auto codes_gpu       = ctx.create_buffer(codes_bytes, cu::storage | cu::transfer_dst, Mem::GpuOnly);
    auto pairs_a         = ctx.create_buffer(pair_bytes, cu::storage | cu::transfer_src, Mem::GpuOnly);
    auto pairs_b         = ctx.create_buffer(pair_bytes, cu::storage, Mem::GpuOnly);
    auto block_hist_buf  = ctx.create_buffer(hist_bytes, cu::storage, Mem::GpuOnly);
    auto scatter_off_buf = ctx.create_buffer(offset_bytes, cu::storage, Mem::GpuOnly);
    auto pairs_readback  = ctx.create_buffer(pair_bytes, cu::transfer_dst, Mem::GpuToCpu);
    if (codes_staging == nullptr || codes_gpu == nullptr || pairs_a == nullptr || pairs_b == nullptr
        || block_hist_buf == nullptr || scatter_off_buf == nullptr || pairs_readback == nullptr)
    {
        return out;
    }

    if (auto* dst = static_cast<crd::u32*>(codes_staging->map()))
    {
        std::memcpy(dst, codes.data(), codes_bytes);
        codes_staging->unmap();
    }
    else { return out; }

    using A = crd::gpu::ComputeAccess;
    auto& rec = ctx.begin();

    rec.copy(*codes_staging, *codes_gpu, 0U, 0U, codes_bytes);
    rec.barrier(*codes_gpu, A::TransferDst, A::ShaderRead);

    // Init: pack (code, gl_GlobalInvocationID.x) pairs into pairs_a.
    {
        crd::gpu::ComputeBuffer* b[] = {codes_gpu.get(), pairs_a.get()};
        InitPushConstants        pc{};
        pc.count                     = count;
        const crd::u32 init_groups   = (count + kRadixItemsPerBlock - 1U) / kRadixItemsPerBlock;
        rec.dispatch(*impl.init, crd::containers::ConstSpan<crd::gpu::ComputeBuffer*>(b, 2), &pc, sizeof(pc), init_groups, 1U, 1U);
    }
    rec.barrier(*pairs_a, A::ShaderWrite, A::ShaderRead);

    // 8 passes × 3 dispatches; pair buffer roles ping-pong each pass.
    for (crd::u32 p = 0U; p < kRadixNumPassesU32; ++p)
    {
        const crd::u32           shift    = p * kRadixDigitBits;
        crd::gpu::ComputeBuffer* pair_in  = (p % 2U == 0U) ? pairs_a.get() : pairs_b.get();
        crd::gpu::ComputeBuffer* pair_out = (p % 2U == 0U) ? pairs_b.get() : pairs_a.get();

        {
            crd::gpu::ComputeBuffer* b[] = {pair_in, block_hist_buf.get()};
            HistogramPushConstants   pc{};
            pc.count                     = count;
            pc.shift                     = shift;
            rec.dispatch(*impl.histogram, crd::containers::ConstSpan<crd::gpu::ComputeBuffer*>(b, 2), &pc, sizeof(pc), num_blocks, 1U, 1U);
        }
        rec.barrier(*block_hist_buf, A::ShaderWrite, A::ShaderRead);

        {
            crd::gpu::ComputeBuffer* b[] = {block_hist_buf.get(), scatter_off_buf.get()};
            ScanPushConstants        pc{};
            pc.num_blocks                = num_blocks;
            rec.dispatch(*impl.scan, crd::containers::ConstSpan<crd::gpu::ComputeBuffer*>(b, 2), &pc, sizeof(pc), 1U, 1U, 1U);
        }
        rec.barrier(*scatter_off_buf, A::ShaderWrite, A::ShaderRead);

        {
            crd::gpu::ComputeBuffer* b[] = {pair_in, pair_out, scatter_off_buf.get()};
            ScatterPushConstants     pc{};
            pc.count                     = count;
            pc.shift                     = shift;
            rec.dispatch(*impl.scatter, crd::containers::ConstSpan<crd::gpu::ComputeBuffer*>(b, 3), &pc, sizeof(pc), num_blocks, 1U, 1U);
        }
        rec.barrier(*pair_out, A::ShaderWrite, A::ShaderRead);
    }

    // After 8 (even) passes the result is in pairs_a; hand off to a transfer read + copy to host readback.
    rec.barrier(*pairs_a, A::ShaderRead, A::TransferSrc);
    rec.copy(*pairs_a, *pairs_readback, 0U, 0U, pair_bytes);
    ctx.submit_and_wait();

    if (auto* src = static_cast<MortonPair<crd::u32>*>(pairs_readback->map()))
    {
        out.resize(count);
        std::memcpy(out.data(), src, pair_bytes);
        pairs_readback->unmap();
    }

    return out;
}

} // namespace crd::geometry::bvh_gpu
