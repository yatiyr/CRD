// crd-shader-cook — D-007 D2: the OFFLINE shader cook (ADR-0104, IR-as-crdr).
//
// Turns a CKIR shader graph into a self-contained `.crdr` bundle: the serialized IR (D1) + the IR-derived reflection + one blob
// per target backend (SPIR-V and DXIL as REAL validated bytecode via shaderc/DXC; CUDA/MSL/WGSL as emitted source, since their
// final bytecode is produced by the target platform's toolchain — Metal/naga are not hosted here). The bundle is a standard CRDR
// container (ADR-0038), so the runtime load path (D4) is just `crdr_read` + pick-your-backend-chunk — ZERO runtime shader
// compilation on the shipping path. Cooking is content-hashed: an unchanged graph re-uses the cached `.crdr` byte-for-byte.
//
// This is a BUILD-TIME tool: it links both GPU backends' compilers. The runtime never does.
#pragma once

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/containers/string.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/kir/ckir.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/resources/crdr.hpp>

namespace crd::shadercook
{

// Target backends. SpirV+Dxil cook to real bytecode; Cuda/Msl/Wgsl cook to emitted source (their platform finishes the compile).
enum class CookBackend : crd::u8
{
    None  = 0U,
    SpirV = 1U << 0U, // GLSL -> SPIR-V (shaderc)              — Vulkan, real bytecode
    Dxil  = 1U << 1U, // HLSL -> DXIL   (dxc)                  — D3D12,  real bytecode
    Cuda  = 1U << 2U, // CUDA C++ source                      — NVRTC/nvcc finishes on the CUDA target
    Msl   = 1U << 3U, // Metal Shading Language source        — the Metal toolchain finishes on macOS
    Wgsl  = 1U << 4U, // WGSL source                          — naga/tint finishes on the WebGPU target
    All   = SpirV | Dxil | Cuda | Msl | Wgsl,
};
[[nodiscard]] constexpr crd::u32 operator|(CookBackend a, CookBackend b) noexcept { return static_cast<crd::u32>(a) | static_cast<crd::u32>(b); }
[[nodiscard]] constexpr bool     has_backend(crd::u32 mask, CookBackend b) noexcept { return (mask & static_cast<crd::u32>(b)) != 0U; }

struct CookOptions
{
    crd::u32    backends  = static_cast<crd::u32>(CookBackend::All);
    const char* cache_dir = nullptr; // if set: content-hash cache — an unchanged graph re-uses <cache_dir>/<id>.crdr verbatim
    bool        compress  = false;    // zstd-compress the per-backend blobs in the container
};

struct CookResult
{
    bool                    ok         = false;
    bool                    from_cache = false;
    crd::containers::Array<crd::u8> crdr;  // the cooked `.crdr` bundle bytes
    crd::containers::String         error;
    // per-backend cooked byte counts (0 = not requested / emit unsupported / toolchain absent).
    // ptx_bytes is the CUDA target's REAL bytecode (NVRTC), cooked alongside the CUDA source when the CUDA toolkit is present.
    crd::u32 spirv_bytes = 0U, dxil_bytes = 0U, cuda_bytes = 0U, msl_bytes = 0U, wgsl_bytes = 0U, ptx_bytes = 0U;
    explicit CookResult(crd::memory::IAllocator* a) : crdr(a), error(a) {}
};

// Cook one COMPUTE kernel graph into a `.crdr` shader bundle. `name` is diagnostic only.
[[nodiscard]] CookResult cook_compute_shader(
    const crd::kir::KGraph& g, const crd::kir::KEntry& e, crd::containers::StringView name,
    const CookOptions& opts, crd::memory::IAllocator* a);

// Cook a RASTER program — a vertex + fragment pair sharing one graph — into a single `.crdr`: SPVV (vertex) + SPVF (fragment)
// real bytecode (+ DXIL VS/FS), the IR, and the fragment reflection. This is the complete shippable form of a MATERIAL: the
// fragment IS the material, the vertex transforms geometry into the varyings it reads. `vs.stage` must be Vertex, `fs.stage`
// Fragment. Only the SpirV / Dxil backends apply to raster.
[[nodiscard]] CookResult cook_raster_shader(
    const crd::kir::KGraph& g, const crd::kir::KEntry& vs, const crd::kir::KEntry& fs, crd::containers::StringView name,
    const CookOptions& opts, crd::memory::IAllocator* a);

// ── Runtime read path (the seam D4 builds on) ──────────────────────────────
// A parsed `.crdr` shader bundle. The source `bytes` must OUTLIVE this (chunk payloads are views into it — ADR-0038).
struct ShaderBundle
{
    crd::resources::CrdrFile file;
    explicit ShaderBundle(crd::memory::IAllocator* a) : file(a) {}
    [[nodiscard]] crd::containers::ConstSpan<crd::u8> bytecode(CookBackend b) const noexcept; // {} if that backend wasn't cooked
    [[nodiscard]] crd::containers::ConstSpan<crd::u8> ir() const noexcept;                     // the serialized KGraph+KEntry (D1)
    [[nodiscard]] crd::containers::ConstSpan<crd::u8> reflection() const noexcept;             // the ShaderReflection POD blob
    [[nodiscard]] crd::containers::ConstSpan<crd::u8> ptx() const noexcept;                    // the CUDA target's REAL PTX (NVRTC), {} if not cooked
    [[nodiscard]] crd::containers::ConstSpan<crd::u8> vertex_spirv() const noexcept;           // raster: the VS SPIR-V (SPVV), {} if none
    [[nodiscard]] crd::containers::ConstSpan<crd::u8> fragment_spirv() const noexcept;         // raster: the FS SPIR-V (SPVF), {} if none
    [[nodiscard]] crd::containers::ConstSpan<crd::u8> vertex_dxil() const noexcept;            // raster: the VS DXIL (DXVV), {} if none
    [[nodiscard]] crd::containers::ConstSpan<crd::u8> fragment_dxil() const noexcept;          // raster: the FS DXIL (DXVF), {} if none
    [[nodiscard]] crd::containers::ConstSpan<crd::u8> vertex_reflection() const noexcept;      // raster: the VS reflection blob (REFV — the vertex INPUT layout), {} if none
};
[[nodiscard]] bool read_shader_bundle(crd::containers::ConstSpan<crd::u8> bytes, ShaderBundle& out);

} // namespace crd::shadercook
