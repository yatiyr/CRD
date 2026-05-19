#pragma once

// ---------------------------------------------------------------------------
// GPU deterministic LSD radix sort of (Morton code, original index) pairs.
// Phase 3.1.7 v9a-b2.
//
// Algorithm: 4-bit-digit (16 bins) LSD radix, **8 passes** for 30-bit u32
// keys. The CPU oracle (v9a-b1 `sort_morton_pairs<u32>`) IS the algorithm
// definition — GPU output MUST be byte-identical to CPU output for any
// input. The conformance test is `bit_compare<MortonPair<u32>>(cpu, gpu)`.
//
// LSD radix is stable by construction; equal Morton codes preserve input
// index order because (a) the init kernel writes pairs with index =
// `gl_GlobalInvocationID.x` (the same monotonic-ascending sequence CPU
// v9a-b1 uses), and (b) the scatter phase uses a prefix-sum offset, not
// an atomic counter, so equal-bin items within a workgroup land in
// monotonic thread-id order and equal-bin items across workgroups land
// in monotonic workgroup-id order.
//
// **Determinism contract (D146)** — the phase doc described v9a-b2 as
// "throughput-tier (non-deterministic by atomic ordering)". We implement
// the OTHER variant: **prefix-sum scatter, not atomic-counter scatter**.
// Output is bit-deterministic across runs *and* byte-identical to CPU.
// Rationale: at 4-bit digit + 1024 items/block, the atomic-counter
// throughput advantage is marginal; the byte-identical-to-CPU contract
// is far more valuable (cheap conformance test → catch GPU bugs early).
// A future throughput-tier variant (`v9a-b2-atomics`) could trade
// determinism for higher throughput if a real consumer demands it.
//
// **Sizing pin (D147)** — v9a-b2 hard-caps N at `kRadixMaxItems` (1 M
// today). Beyond requires a recursive scan kernel — filed as
// `v9a-b2-large` follow-on. The cap is shared between host + shader via
// the constants below.
//
// **Pipeline pin (D148)** — workgroup size 256, items per thread 4 ⇒
// 1024 items per block. Scan workgroup 256 × 64 items/thread ⇒ 16 384
// histogram entries (= `kRadixMaxBlocks × kRadixBins`). Static-asserted
// below; if the constants change, the assert keeps the invariant.
//
// **Pair format pin (D149)** — pairs ping-pong from pass 0. The init
// kernel packs `{ codes[i], gl_GlobalInvocationID.x }` into pairs_a; all
// 8 passes operate uniformly on pair buffers. Even pass count ⇒ final
// output lands in pairs_a (same parity rule as CPU v9a-b1).
//
// API mirrors `MortonGpuPipeline` / `MortonGpu60BitPipeline` (sibling
// classes per the D140 pattern). Synchronous dispatch + fence wait at
// v9a-b2 — async-compute follow-on if a consumer asks.
//
// Two-layer typing (ADR-0078 §5 D34): inputs are dimensionless `u32`
// Morton bit codes; outputs are `MortonPair<u32>` (bit code + array
// index). No typed-wrapper layer needed; the upstream `*_typed.hpp` keeps
// the Length<T> annotation at the AABB → Morton boundary only.
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>
#include <crd/geometry/bvh_gpu/morton_sort.hpp>     // for MortonPair<u32>
#include <crd/memory/allocator.hpp>

#include <cstdint>
#include <memory>

namespace crd::rhi
{
class Device;
}

namespace crd::geometry::bvh_gpu
{

// --- Shared constants — these are the contract the GLSL shaders read.
// If you change any of these, update the corresponding `#define` block
// in the four shaders + re-run the calibration test.

constexpr crd::u32 kRadixWorkgroupSize    = 256U;
constexpr crd::u32 kRadixItemsPerThread   = 4U;
constexpr crd::u32 kRadixItemsPerBlock    = kRadixWorkgroupSize * kRadixItemsPerThread;   // 1024
constexpr crd::u32 kRadixMaxBlocks        = 1024U;
constexpr crd::u32 kRadixMaxItems         = kRadixItemsPerBlock * kRadixMaxBlocks;        // 1,048,576

constexpr crd::u32 kRadixDigitBits        = 4U;
constexpr crd::u32 kRadixBins             = 1U << kRadixDigitBits;                         // 16
constexpr crd::u32 kRadixNumPassesU32     = 32U / kRadixDigitBits;                         // 8

constexpr crd::u32 kScanWorkgroupSize     = 256U;
constexpr crd::u32 kScanItemsPerThread    = kRadixMaxBlocks / kScanWorkgroupSize;          // 4
constexpr crd::u32 kScanBlocksPerKernel   = kRadixMaxBlocks;                               // 1024
// Scan kernel processes ONE BIN AT A TIME (sequential over 16 bins inside
// the same workgroup launch) via a per-bin Hillis-Steele exclusive
// prefix scan. Shared memory budget = 4 KiB (s_scan) + 1 KiB (s_thread_sum)
// + small bin tables = ~5 KiB, well inside the Vulkan-portable 16 KiB
// floor and the Nvidia 48 KiB per-workgroup tier. Adding more blocks
// (beyond 1024) requires the v9a-b2-large recursive-scan follow-on.

static_assert(kScanBlocksPerKernel == kRadixMaxBlocks,
              "Scan kernel handles exactly kRadixMaxBlocks blocks per dispatch.");
static_assert(kScanItemsPerThread * kScanWorkgroupSize == kRadixMaxBlocks,
              "Scan kernel: each thread must cover an integer number of blocks.");

static_assert(kRadixNumPassesU32 % 2U == 0U,
              "Radix pass count must be even so the ping-pong terminates in pairs_a "
              "(matches the v9a-b1 CPU parity rule).");

static_assert(kRadixBins == 16U,
              "GLSL shaders hard-code 16-bin histograms (4-bit digit); changing "
              "this requires updating all four .comp files.");

// --- Pipeline class — caches the 4 compute pipelines + descriptor
// allocators across dispatches. Build once in the ctor; many dispatches.

class MortonRadixGpuPipeline
{
public:
    // Construct (compile 4 shaders + create 4 pipelines). On any failure
    // (shader file missing, validation error during pipeline create) the
    // returned object satisfies `is_valid() == false`; caller checks
    // before dispatching.
    //
    // `shader_dir` is the directory containing the 4 SPIR-V binaries:
    //   - radix_sort_init.comp.spv
    //   - radix_sort_histogram.comp.spv
    //   - radix_sort_scan.comp.spv
    //   - radix_sort_scatter.comp.spv
    MortonRadixGpuPipeline(crd::rhi::Device& device,
                            crd::containers::StringView shader_dir) noexcept;

    MortonRadixGpuPipeline(const MortonRadixGpuPipeline&)            = delete;
    MortonRadixGpuPipeline& operator=(const MortonRadixGpuPipeline&) = delete;
    MortonRadixGpuPipeline(MortonRadixGpuPipeline&&) noexcept;
    MortonRadixGpuPipeline& operator=(MortonRadixGpuPipeline&&) noexcept;
    ~MortonRadixGpuPipeline();

    [[nodiscard]] bool is_valid() const noexcept;

    // Sort `codes` ascending by value, equal-key pairs by ascending input
    // index (stable). Output byte-identical to v9a-b1
    // `sort_morton_pairs<u32>(codes, alloc)`.
    //
    //   Empty input  => empty output (no dispatch).
    //   N == 1       => single pair `{codes[0], 0}`, no sort work.
    //   N <= kRadixMaxItems  => full 8-pass sort.
    //   N >  kRadixMaxItems  => CRD_ASSERT — fence the slice cap (D147).
    //
    // **Synchronous**: 25 dispatches (1 init + 8 × 3 passes) + final
    // readback + fence wait before returning. Caller's allocator binds
    // the returned `Array<MortonPair<u32>>`.
    [[nodiscard]] crd::containers::Array<MortonPair<crd::u32>>
    dispatch_radix_sort(crd::containers::ConstSpan<crd::u32> codes,
                          crd::memory::IAllocator* alloc) noexcept;

    // Forward-declared opaque impl — public so the dispatch helper (a
    // free function in the .cpp) can reach the cached pipeline state.
    // Same pattern as `MortonGpuPipeline::Impl`.
    struct Impl;

private:
    std::unique_ptr<Impl> m_impl;
};

} // namespace crd::geometry::bvh_gpu
