// crd-shader-cook — the offline cook (ADR-0104 D2). See cook.hpp for the contract.
#include <crd/shadercook/cook.hpp>

#include <crd/containers/span.hpp>
#include <crd/gpu/program.hpp>              // ShaderStage
#include <crd/gpu/vulkan_shader_compile.hpp> // compile_glsl_to_spirv
#include <crd/kir/ckir_cuda.hpp>
#include <crd/kir/ckir_glsl.hpp>
#include <crd/kir/ckir_hlsl.hpp>
#include <crd/kir/ckir_msl.hpp>
#include <crd/kir/ckir_serialize.hpp>       // serialize_graph, reflect
#include <crd/kir/ckir_wgsl.hpp>
#include <crd/platform/filesystem.hpp>
#include <crd/resources/resource_id.hpp>

#ifdef CRD_SHADERCOOK_HAS_DX12
#    include <crd/gpu/dx12_context.hpp>     // compile_hlsl_to_dxil
#endif

#ifdef CRD_SHADERCOOK_HAS_CUDA
#    include <nvrtc.h>                       // CUDA C -> real PTX
#endif

#include <cstring>
#include <type_traits>

namespace crd::shadercook
{
namespace
{
using crd::resources::make_fourcc;
constexpr crd::u32 kIrChunk    = make_fourcc('K', 'I', 'R', '0'); // serialized KGraph+KEntry (D1)
constexpr crd::u32 kReflChunk  = make_fourcc('R', 'E', 'F', 'L'); // ShaderReflection POD
constexpr crd::u32 kSpirvChunk = crd::resources::kFourCC_SPVC;    // compute SPIR-V (registered)
constexpr crd::u32 kDxilChunk  = make_fourcc('D', 'X', 'I', 'C'); // compute DXIL
constexpr crd::u32 kCudaChunk  = make_fourcc('C', 'U', 'D', 'A'); // CUDA C++ source
constexpr crd::u32 kMslChunk   = make_fourcc('M', 'S', 'L', 'C'); // Metal source
constexpr crd::u32 kWgslChunk  = make_fourcc('W', 'G', 'S', 'L'); // WGSL source
constexpr crd::u32 kPtxChunk   = make_fourcc('P', 'T', 'X', ' '); // CUDA REAL bytecode (NVRTC)
constexpr crd::u32 kSpvvChunk  = crd::resources::kFourCC_SPVV;    // raster: vertex SPIR-V (registered)
constexpr crd::u32 kSpvfChunk  = crd::resources::kFourCC_SPVF;    // raster: fragment SPIR-V (registered)
constexpr crd::u32 kDxvvChunk  = make_fourcc('D', 'X', 'V', 'V'); // raster: vertex DXIL
constexpr crd::u32 kDxvfChunk  = make_fourcc('D', 'X', 'V', 'F'); // raster: fragment DXIL
constexpr crd::u32 kRefvChunk  = make_fourcc('R', 'E', 'F', 'V'); // raster: VERTEX-stage reflection (the vertex INPUT layout)

static_assert(std::is_trivially_copyable_v<crd::kir::ShaderReflection>, "reflection blob is a raw POD copy");

#ifdef CRD_SHADERCOOK_HAS_CUDA
// Compile emitted CUDA C → REAL PTX via NVRTC. Same determinism flags as the kir-cuda runtime (`--fmad=false` + correctly-rounded
// div/sqrt), so the cooked PTX matches; a VIRTUAL arch (`compute_75`) keeps the PTX portable — the driver JITs it to the actual
// GPU at load. Returns false (and appends the NVRTC log to `err`) on failure.
[[nodiscard]] bool compile_cuda_to_ptx(const char* src, crd::containers::Array<crd::u8>& out_ptx, crd::containers::String& err)
{
    nvrtcProgram prog = nullptr;
    if (nvrtcCreateProgram(&prog, src, "ckir.cu", 0, nullptr, nullptr) != NVRTC_SUCCESS) { return false; }
    const char* const opts[] = {"--fmad=false", "--prec-div=true", "--prec-sqrt=true", "--gpu-architecture=compute_75"};
    if (nvrtcCompileProgram(prog, 4, opts) != NVRTC_SUCCESS)
    {
        crd::usize logsz = 0;
        nvrtcGetProgramLogSize(prog, &logsz);
        if (logsz > 1U)
        {
            crd::containers::Array<char> log(out_ptx.allocator());
            log.resize(logsz, '\0');
            nvrtcGetProgramLog(prog, log.data());
            err.append("ptx: ");
            err.append(log.data());
        }
        nvrtcDestroyProgram(&prog);
        return false;
    }
    crd::usize sz = 0;
    if (nvrtcGetPTXSize(prog, &sz) != NVRTC_SUCCESS || sz == 0U) { nvrtcDestroyProgram(&prog); return false; }
    out_ptx.resize(sz);
    const nvrtcResult gr = nvrtcGetPTX(prog, reinterpret_cast<char*>(out_ptx.data()));
    nvrtcDestroyProgram(&prog);
    return gr == NVRTC_SUCCESS;
}
#endif

[[nodiscard]] crd::u32 backend_fourcc(CookBackend b) noexcept
{
    switch (b)
    {
        case CookBackend::SpirV: return kSpirvChunk;
        case CookBackend::Dxil:  return kDxilChunk;
        case CookBackend::Cuda:  return kCudaChunk;
        case CookBackend::Msl:   return kMslChunk;
        case CookBackend::Wgsl:  return kWgslChunk;
        default:                 return 0U;
    }
}

[[nodiscard]] crd::containers::ConstSpan<crd::u8> bytes_of(const crd::containers::String& s) noexcept
{
    return {reinterpret_cast<const crd::u8*>(s.c_str()), s.size()};
}
} // namespace

CookResult cook_compute_shader(
    const crd::kir::KGraph& g, const crd::kir::KEntry& e, crd::containers::StringView name,
    const CookOptions& opts, crd::memory::IAllocator* a)
{
    CookResult out(a);

    // 1. Serialize the IR (D1) — the bundle's source of truth + the content-hash key.
    crd::containers::Array<crd::u8> ir = crd::kir::serialize_graph(g, e, a);
    const crd::resources::ResourceId id = crd::resources::ResourceId::from_content(crd::containers::as_const_span(ir));

    // 2. Content-hash cache: same IR + same backend set + same compression ⇒ re-use the cooked bundle byte-for-byte.
    crd::containers::String cache_path(a);
    if (opts.cache_dir != nullptr)
    {
        crd::containers::String idstr = id.to_string(a);
        cache_path.append(opts.cache_dir);
        cache_path.append("/");
        cache_path.append(idstr.c_str());
        char suffix[24];
        std::snprintf(suffix, sizeof(suffix), "_%08x%s.crdr", opts.backends, opts.compress ? "z" : "");
        cache_path.append(suffix);
        const crd::platform::fs::Path cp(cache_path.c_str());
        if (crd::platform::fs::is_file(cp) && crd::platform::fs::read_file_binary(cp, out.crdr))
        {
            out.ok = true;
            out.from_cache = true;
            return out;
        }
    }

    // 3. Reflection (IR-derived) — the descriptor/vertex layout the runtime binds from, no SPIRV-Cross.
    const crd::kir::ShaderReflection refl = crd::kir::reflect(g, e);

    crd::resources::CrdrWriter w(a, id, crd::resources::kFourCC_SHDR);
    w.add_chunk(kIrChunk, crd::containers::as_const_span(ir));
    w.add_chunk(kReflChunk, crd::containers::ConstSpan<crd::u8>{reinterpret_cast<const crd::u8*>(&refl), sizeof(refl)});

    const auto add_blob = [&](crd::u32 fourcc, crd::containers::ConstSpan<crd::u8> p) {
        if (opts.compress) { w.add_chunk_compressed(fourcc, p); }
        else { w.add_chunk(fourcc, p); }
    };
    const auto add_source = [&](CookBackend b, auto emit_fn) -> crd::u32 {
        if (!has_backend(opts.backends, b)) { return 0U; }
        crd::kir::GlslKernel k(a);
        if (!emit_fn(g, e, a, k)) { return 0U; }
        add_blob(backend_fourcc(b), bytes_of(k.source));
        return static_cast<crd::u32>(k.source.size());
    };

    // A raster/material entry (Fragment/Vertex) emits through the STAGE emitters; a compute kernel through the kernel emitters.
    // Both flow through this one cook — so a material variant goes through the SAME cook_variant_matrix as a compute kernel.
    const bool            is_kernel = e.is_kernel();
    crd::gpu::ShaderStage stage     = crd::gpu::ShaderStage::Compute;
    if (!is_kernel) { stage = (e.stage == crd::kir::KStage::Vertex) ? crd::gpu::ShaderStage::Vertex : crd::gpu::ShaderStage::Fragment; }

    // 4a. SPIR-V — GLSL → shaderc. Real bytecode; the runtime loads it into a shader module (that IS the validation).
    if (has_backend(opts.backends, CookBackend::SpirV))
    {
        crd::kir::GlslKernel k(a);
        const bool ok = is_kernel ? crd::kir::emit_compute_kernel_glsl(g, e, a, k) : crd::kir::emit_stage_glsl(g, e, a, k);
        if (ok)
        {
            crd::gpu::ShaderCompileResult r = crd::gpu::compile_glsl_to_spirv(stage, crd::containers::StringView(k.source.c_str(), k.source.size()), name, a);
            if (r.ok && !r.spirv.empty()) { add_blob(kSpirvChunk, crd::containers::as_const_span(r.spirv)); out.spirv_bytes = static_cast<crd::u32>(r.spirv.size()); }
            else { out.error.append("spirv: "); out.error.append(r.error_message.c_str()); }
        }
    }

    // 4b. DXIL — HLSL → dxc (guarded on the DX12 backend being linked).
#ifdef CRD_SHADERCOOK_HAS_DX12
    if (has_backend(opts.backends, CookBackend::Dxil))
    {
        crd::kir::GlslKernel k(a);
        const bool ok = is_kernel ? crd::kir::emit_compute_kernel_hlsl(g, e, a, k) : crd::kir::emit_stage_hlsl(g, e, a, k);
        if (ok)
        {
            crd::gpu::DxilCompileResult r = crd::gpu::compile_hlsl_to_dxil(stage, crd::containers::StringView(k.source.c_str(), k.source.size()), name, a);
            if (r.ok && !r.dxil.empty()) { add_blob(kDxilChunk, crd::containers::as_const_span(r.dxil)); out.dxil_bytes = static_cast<crd::u32>(r.dxil.size()); }
            else if (!r.ok) { out.error.append("dxil: "); out.error.append(r.error_message.c_str()); }
        }
    }
#endif

    // 4c/4d. CUDA / MSL / WGSL — the kernel emitters are COMPUTE-only. A raster material cooks SPIR-V + DXIL (its shipping
    // backends); MSL/WGSL raster-stage emit + a CUDA-less fragment path fold in later. Gate on the entry being a kernel.
    if (is_kernel)
    {
        out.msl_bytes  = add_source(CookBackend::Msl, crd::kir::emit_compute_kernel_msl);
        out.wgsl_bytes = add_source(CookBackend::Wgsl, crd::kir::emit_compute_kernel_wgsl);
        if (has_backend(opts.backends, CookBackend::Cuda))
        {
            crd::kir::GlslKernel k(a);
            if (crd::kir::emit_compute_kernel_cuda(g, e, a, k))
            {
                add_blob(kCudaChunk, bytes_of(k.source));
                out.cuda_bytes = static_cast<crd::u32>(k.source.size());
#ifdef CRD_SHADERCOOK_HAS_CUDA
                crd::containers::Array<crd::u8> ptx(a);
                if (compile_cuda_to_ptx(k.source.c_str(), ptx, out.error) && !ptx.empty())
                {
                    add_blob(kPtxChunk, crd::containers::as_const_span(ptx));
                    out.ptx_bytes = static_cast<crd::u32>(ptx.size());
                }
#endif
            }
        }
    }

    out.crdr = w.finish();
    out.ok   = !out.crdr.empty();

    if (out.ok && opts.cache_dir != nullptr)
    {
        (void)crd::platform::fs::write_file_binary(crd::platform::fs::Path(cache_path.c_str()), crd::containers::as_const_span(out.crdr));
    }
    return out;
}

CookResult cook_raster_shader(
    const crd::kir::KGraph& g, const crd::kir::KEntry& vs, const crd::kir::KEntry& fs, crd::containers::StringView name,
    const CookOptions& opts, crd::memory::IAllocator* a)
{
    CookResult out(a);

    // 1. Serialize both entries' IR (they share the graph) — the content-hash key is the (vs, fs) pair.
    crd::containers::Array<crd::u8> vs_ir = crd::kir::serialize_graph(g, vs, a);
    crd::containers::Array<crd::u8> fs_ir = crd::kir::serialize_graph(g, fs, a);
    crd::containers::Array<crd::u8> both(a);
    for (crd::usize i = 0; i < vs_ir.size(); ++i) { both.push_back(vs_ir[i]); }
    for (crd::usize i = 0; i < fs_ir.size(); ++i) { both.push_back(fs_ir[i]); }
    const crd::resources::ResourceId id = crd::resources::ResourceId::from_content(crd::containers::as_const_span(both));

    // 2. Content-hash cache (raster suffix so it never collides with a compute bundle of the same graph).
    crd::containers::String cache_path(a);
    if (opts.cache_dir != nullptr)
    {
        crd::containers::String idstr = id.to_string(a);
        cache_path.append(opts.cache_dir);
        cache_path.append("/");
        cache_path.append(idstr.c_str());
        char suffix[24];
        std::snprintf(suffix, sizeof(suffix), "_r%08x.crdr", opts.backends);
        cache_path.append(suffix);
        const crd::platform::fs::Path cp(cache_path.c_str());
        if (crd::platform::fs::is_file(cp) && crd::platform::fs::read_file_binary(cp, out.crdr))
        {
            out.ok = true;
            out.from_cache = true;
            return out;
        }
    }

    // 3. The COMPLETE graphics interface, IR-derived (no SPIRV-Cross): the fragment reflection (REFL: descriptor bindings +
    // colour-attachment count via the entry) AND the vertex reflection (REFV: the vertex INPUT layout — attributes). Together
    // they're everything a graphics PSO needs, so D4 builds one from the bundle alone.
    const crd::kir::ShaderReflection refl  = crd::kir::reflect(g, fs);
    const crd::kir::ShaderReflection vrefl = crd::kir::reflect(g, vs);
    crd::resources::CrdrWriter       w(a, id, crd::resources::kFourCC_SHDR);
    w.add_chunk(kIrChunk, crd::containers::as_const_span(fs_ir));
    w.add_chunk(kReflChunk, crd::containers::ConstSpan<crd::u8>{reinterpret_cast<const crd::u8*>(&refl), sizeof(refl)});
    w.add_chunk(kRefvChunk, crd::containers::ConstSpan<crd::u8>{reinterpret_cast<const crd::u8*>(&vrefl), sizeof(vrefl)});

    const auto add_blob = [&](crd::u32 fourcc, crd::containers::ConstSpan<crd::u8> p) {
        if (opts.compress) { w.add_chunk_compressed(fourcc, p); }
        else { w.add_chunk(fourcc, p); }
    };
    const auto stage_spv = [&](const crd::kir::KEntry& se, crd::gpu::ShaderStage st, crd::u32 fourcc) -> crd::u32 {
        crd::kir::GlslKernel k(a);
        if (!crd::kir::emit_stage_glsl(g, se, a, k)) { return 0U; }
        crd::gpu::ShaderCompileResult r = crd::gpu::compile_glsl_to_spirv(st, crd::containers::StringView(k.source.c_str(), k.source.size()), name, a);
        if (r.ok && !r.spirv.empty()) { add_blob(fourcc, crd::containers::as_const_span(r.spirv)); return static_cast<crd::u32>(r.spirv.size()); }
        out.error.append("spv: ");
        out.error.append(r.error_message.c_str());
        return 0U;
    };
    if (has_backend(opts.backends, CookBackend::SpirV))
    {
        out.spirv_bytes = stage_spv(vs, crd::gpu::ShaderStage::Vertex, kSpvvChunk) + stage_spv(fs, crd::gpu::ShaderStage::Fragment, kSpvfChunk);
    }

#ifdef CRD_SHADERCOOK_HAS_DX12
    const auto stage_dxil = [&](const crd::kir::KEntry& se, crd::gpu::ShaderStage st, crd::u32 fourcc) -> crd::u32 {
        crd::kir::GlslKernel k(a);
        if (!crd::kir::emit_stage_hlsl(g, se, a, k)) { return 0U; }
        crd::gpu::DxilCompileResult r = crd::gpu::compile_hlsl_to_dxil(st, crd::containers::StringView(k.source.c_str(), k.source.size()), name, a);
        if (r.ok && !r.dxil.empty()) { add_blob(fourcc, crd::containers::as_const_span(r.dxil)); return static_cast<crd::u32>(r.dxil.size()); }
        return 0U;
    };
    if (has_backend(opts.backends, CookBackend::Dxil))
    {
        out.dxil_bytes = stage_dxil(vs, crd::gpu::ShaderStage::Vertex, kDxvvChunk) + stage_dxil(fs, crd::gpu::ShaderStage::Fragment, kDxvfChunk);
    }
#endif

    out.crdr = w.finish();
    out.ok   = !out.crdr.empty();
    if (out.ok && opts.cache_dir != nullptr)
    {
        (void)crd::platform::fs::write_file_binary(crd::platform::fs::Path(cache_path.c_str()), crd::containers::as_const_span(out.crdr));
    }
    return out;
}

// ── read path ──────────────────────────────────────────────────────────────
crd::containers::ConstSpan<crd::u8> ShaderBundle::bytecode(CookBackend b) const noexcept
{
    const crd::u32 fourcc = backend_fourcc(b);
    if (fourcc == 0U) { return {}; }
    const crd::resources::CrdrChunk* c = crd::resources::crdr_find_chunk(file, fourcc);
    return c != nullptr ? c->payload : crd::containers::ConstSpan<crd::u8>{};
}
crd::containers::ConstSpan<crd::u8> ShaderBundle::ir() const noexcept
{
    const crd::resources::CrdrChunk* c = crd::resources::crdr_find_chunk(file, kIrChunk);
    return c != nullptr ? c->payload : crd::containers::ConstSpan<crd::u8>{};
}
crd::containers::ConstSpan<crd::u8> ShaderBundle::reflection() const noexcept
{
    const crd::resources::CrdrChunk* c = crd::resources::crdr_find_chunk(file, kReflChunk);
    return c != nullptr ? c->payload : crd::containers::ConstSpan<crd::u8>{};
}
crd::containers::ConstSpan<crd::u8> ShaderBundle::ptx() const noexcept
{
    const crd::resources::CrdrChunk* c = crd::resources::crdr_find_chunk(file, kPtxChunk);
    return c != nullptr ? c->payload : crd::containers::ConstSpan<crd::u8>{};
}
namespace
{
[[nodiscard]] crd::containers::ConstSpan<crd::u8> find(const crd::resources::CrdrFile& f, crd::u32 fourcc) noexcept
{
    const crd::resources::CrdrChunk* c = crd::resources::crdr_find_chunk(f, fourcc);
    return c != nullptr ? c->payload : crd::containers::ConstSpan<crd::u8>{};
}
} // namespace
crd::containers::ConstSpan<crd::u8> ShaderBundle::vertex_spirv() const noexcept { return find(file, kSpvvChunk); }
crd::containers::ConstSpan<crd::u8> ShaderBundle::fragment_spirv() const noexcept { return find(file, kSpvfChunk); }
crd::containers::ConstSpan<crd::u8> ShaderBundle::vertex_dxil() const noexcept { return find(file, kDxvvChunk); }
crd::containers::ConstSpan<crd::u8> ShaderBundle::fragment_dxil() const noexcept { return find(file, kDxvfChunk); }
crd::containers::ConstSpan<crd::u8> ShaderBundle::vertex_reflection() const noexcept { return find(file, kRefvChunk); }
bool read_shader_bundle(crd::containers::ConstSpan<crd::u8> bytes, ShaderBundle& out)
{
    return crd::resources::crdr_read(bytes, out.file, out.file.chunks.allocator()) == crd::resources::CrdrError::Ok;
}

} // namespace crd::shadercook
