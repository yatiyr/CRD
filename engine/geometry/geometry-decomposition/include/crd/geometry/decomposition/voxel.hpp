#pragma once

// ---------------------------------------------------------------------------
// crd::geometry::decomposition — voxel substrate. Phase 3.1.7 v9c-a.
//
// VoxelGrid is the output of `voxelize_mesh` (see voxelize.hpp) and the
// input of `vhacd_decompose` (see vhacd_decompose.hpp, v9c-b). Per-voxel
// state is `VoxelState` (u8; 2 bits used, 6 reserved for future per-voxel
// payload — material id, surface normal, distance estimate, ...). Storage
// is a plain dense `Array<u8>`; parallel surface marking via
// `crd::jobs::parallel_for` uses `std::atomic_ref<u8>::fetch_or(Surface)`
// per-cell for race-free union. (Plain `Array<std::atomic<u8>>` would not
// compile because std::atomic is neither copyable nor movable.)
//
// API discipline: the storage layout is OPAQUE (no raw buffer accessor).
// Callers go through `state_at()` / `set_state()` / iteration helpers. A
// future bricked / sparse backend can replace the dense array without
// breaking consumers (CAM @ 1024³ sparse coverage is a credible future
// consumer — see ADR-0076 §24 amendment at v9c-close for the trigger).
//
// Determinism: VoxelGrid contents are bit-identical across runs given
// identical input, despite parallel surface marking — `fetch_or(Surface)`
// is commutative+idempotent, and classification runs ONLY after every
// surface-marking job has joined (the strict two-pass contract; D126).
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/geometry/primitives/primitives.hpp>
#include <crd/math/vec.hpp>
#include <crd/memory/allocator.hpp>

#include <atomic> // std::atomic_ref for parallel surface-marking writes

namespace crd::geometry::decomposition
{

// --- Per-voxel state -------------------------------------------------------

// 2 bits used, 6 reserved. Values pinned: `fetch_or(Surface)` is the
// idempotent surface-marking write; `Outside`/`Inside` are written by
// classification pass 2 only into voxels still `Unknown`. DO NOT change
// these numeric values without auditing the atomic-union logic in
// voxelize_internal.hpp.
enum class VoxelState : crd::u8
{
    Unknown = 0,
    Outside = 1,
    Surface = 2,
    Inside  = 3,
};

[[nodiscard]] constexpr const char* to_string(VoxelState s) noexcept
{
    switch (s)
    {
        case VoxelState::Unknown: return "Unknown";
        case VoxelState::Outside: return "Outside";
        case VoxelState::Surface: return "Surface";
        case VoxelState::Inside:  return "Inside";
    }
    return "?";
}

// --- VoxelGrid -------------------------------------------------------------

// Dense 3D voxel state grid. Index order is `(ix, iy, iz)` with linear
// stride `linear_index = ix + nx * (iy + ny * iz)`. Sized by
// `(nx, ny, nz)`; origin is the input mesh AABB minimum minus padding.
//
// Construction allocates `nx * ny * nz` atomic bytes initialised to
// `VoxelState::Unknown`. Move-only (atomics aren't trivially copyable);
// callers explicitly clone if needed.
class VoxelGrid
{
public:
    VoxelGrid() noexcept = default;

    // Builds an `nx × ny × nz` grid of Unknown voxels. Reports allocator
    // failure by leaving the grid empty (`is_empty() == true`); caller
    // checks before use.
    VoxelGrid(crd::u32 nx, crd::u32 ny, crd::u32 nz, crd::memory::IAllocator* alloc) noexcept;

    VoxelGrid(const VoxelGrid&)            = delete;
    VoxelGrid& operator=(const VoxelGrid&) = delete;
    VoxelGrid(VoxelGrid&&) noexcept;
    VoxelGrid& operator=(VoxelGrid&&) noexcept;
    ~VoxelGrid()                           = default;

    // --- Geometry ----------------------------------------------------------

    [[nodiscard]] crd::u32 nx() const noexcept { return m_nx; }
    [[nodiscard]] crd::u32 ny() const noexcept { return m_ny; }
    [[nodiscard]] crd::u32 nz() const noexcept { return m_nz; }
    [[nodiscard]] crd::u64 voxel_count() const noexcept
    {
        return static_cast<crd::u64>(m_nx) * static_cast<crd::u64>(m_ny) * static_cast<crd::u64>(m_nz);
    }
    [[nodiscard]] bool is_empty() const noexcept { return voxel_count() == 0U; }
    [[nodiscard]] bool in_bounds(crd::i32 ix, crd::i32 iy, crd::i32 iz) const noexcept
    {
        return ix >= 0 && iy >= 0 && iz >= 0
               && static_cast<crd::u32>(ix) < m_nx
               && static_cast<crd::u32>(iy) < m_ny
               && static_cast<crd::u32>(iz) < m_nz;
    }

    // --- Per-voxel accessors (the only sanctioned read/write path) ---------

    [[nodiscard]] VoxelState state_at(crd::u32 ix, crd::u32 iy, crd::u32 iz) const noexcept
    {
        CRD_ASSERT(in_bounds(static_cast<crd::i32>(ix), static_cast<crd::i32>(iy), static_cast<crd::i32>(iz)));
        const std::atomic_ref<const crd::u8> ref{m_cells[linear_index(ix, iy, iz)]};
        return static_cast<VoxelState>(ref.load(std::memory_order_relaxed));
    }

    void set_state(crd::u32 ix, crd::u32 iy, crd::u32 iz, VoxelState s) noexcept
    {
        CRD_ASSERT(in_bounds(static_cast<crd::i32>(ix), static_cast<crd::i32>(iy), static_cast<crd::i32>(iz)));
        std::atomic_ref<crd::u8> ref{m_cells[linear_index(ix, iy, iz)]};
        ref.store(static_cast<crd::u8>(s), std::memory_order_relaxed);
    }

    // Surface-marking write: idempotent `fetch_or(Surface)`. Safe under
    // parallel writers; multiple triangles touching the same voxel all
    // converge on `Surface`. Returns the previous state (for telemetry /
    // tests that want to count first-write transitions).
    VoxelState mark_surface(crd::u32 ix, crd::u32 iy, crd::u32 iz) noexcept
    {
        CRD_ASSERT(in_bounds(static_cast<crd::i32>(ix), static_cast<crd::i32>(iy), static_cast<crd::i32>(iz)));
        std::atomic_ref<crd::u8> ref{m_cells[linear_index(ix, iy, iz)]};
        const auto prev = ref.fetch_or(static_cast<crd::u8>(VoxelState::Surface), std::memory_order_relaxed);
        return static_cast<VoxelState>(prev);
    }

private:
    [[nodiscard]] crd::u64 linear_index(crd::u32 ix, crd::u32 iy, crd::u32 iz) const noexcept
    {
        return static_cast<crd::u64>(ix)
               + static_cast<crd::u64>(m_nx) * (static_cast<crd::u64>(iy)
                                                + static_cast<crd::u64>(m_ny) * static_cast<crd::u64>(iz));
    }

    crd::containers::Array<crd::u8> m_cells{};
    crd::u32                        m_nx = 0;
    crd::u32                        m_ny = 0;
    crd::u32                        m_nz = 0;
};

// --- Options ---------------------------------------------------------------

// Classification policy for non-surface voxels.
// - WindingNumber: Jacobson 2013 generalised winding number test per voxel
//   centre. Robust on non-watertight / non-manifold input. O(triangles)
//   per voxel via brute force (v4c implementation); a BVH-accelerated
//   `mesh_winding_number_fast` is a planned follow-on. **Default — safest
//   choice for arbitrary input meshes.**
// - FloodFill: 6-connected BFS from a corner Outside seed. O(voxels), much
//   faster, but REQUIRES watertight input — open meshes leak the seed
//   inside and mis-classify the entire interior. Use only when input is
//   known watertight.
enum class ClassificationMode : crd::u8
{
    WindingNumber, // default, robust
    FloodFill,     // fast, requires watertight
};

[[nodiscard]] constexpr const char* to_string(ClassificationMode m) noexcept
{
    switch (m)
    {
        case ClassificationMode::WindingNumber: return "WindingNumber";
        case ClassificationMode::FloodFill:     return "FloodFill";
    }
    return "?";
}

// Sizing parameters — precedence: `fixed_resolution` wins if non-zero;
// otherwise `target_voxel_count` is consulted. At least one of the two
// must be non-zero. (`voxel_size: Length<T>` mode planned for v9c-b/close
// per advisor scope-trim — units-typed physical sizing for CAD/CAM —
// will sit at the top of the precedence ladder when shipped.)
//
// Rationale: a precedence rule beats "exactly one non-zero" because
// callers who want to override the default `target_voxel_count` don't
// have to zero it out first. The default is "you didn't specify a
// resolution? get ~262K voxels"; the override is "I want exactly N along
// the longest axis" — clearly the more specific request, so it wins.
//
// `padding_voxels` adds N layers of guaranteed-Outside cells around the
// mesh AABB. Default 1 ensures the corner cell is Outside, which is
// required for FloodFill mode AND useful for BVH-of-voxels traversals
// downstream.
struct VoxelizationOptions
{
    // Either knob may be non-zero. fixed_resolution > 0 wins; otherwise
    // target_voxel_count > 0 is used. Both zero → InvalidOptions.
    crd::u32 fixed_resolution    = 0;             // N → exactly N voxels along the longest mesh axis (wins).
    crd::u32 target_voxel_count  = 64U * 64U * 64U; // ~target total voxels; per-axis derived from mesh aspect.

    // Cells of guaranteed-Outside padding on every side. Default 1.
    crd::u32 padding_voxels      = 1;

    // Classification policy for non-surface voxels.
    ClassificationMode classify  = ClassificationMode::WindingNumber;

    // Hard cap on per-axis resolution to defend against pathological
    // aspect ratios. Default 1024 ⇒ max 1024³ ≈ 1 GiB at 1 B/voxel.
    crd::u32 max_resolution_per_axis = 1024U;
};

// --- Result + Status -------------------------------------------------------

enum class VoxelizationStatus : crd::u8
{
    Ok = 0,
    EmptyMesh,       // input has no triangles
    NonFiniteInput,  // NaN / Inf vertex
    DegenerateAABB,  // mesh AABB has zero extent on at least one axis
    InvalidOptions,  // both fixed_resolution and target_voxel_count zero, or both non-zero
    OutOfMemory,     // grid allocation failed
};

[[nodiscard]] constexpr const char* to_string(VoxelizationStatus s) noexcept
{
    switch (s)
    {
        case VoxelizationStatus::Ok:              return "Ok";
        case VoxelizationStatus::EmptyMesh:       return "EmptyMesh";
        case VoxelizationStatus::NonFiniteInput:  return "NonFiniteInput";
        case VoxelizationStatus::DegenerateAABB:  return "DegenerateAABB";
        case VoxelizationStatus::InvalidOptions:  return "InvalidOptions";
        case VoxelizationStatus::OutOfMemory:     return "OutOfMemory";
    }
    return "?";
}

template <typename T>
struct VoxelizationResult
{
    VoxelGrid                              grid{};
    crd::geometry::primitives::AABB3<T>    grid_aabb{};       // world-space AABB of the (padded) grid
    crd::math::Vec3<T>                     voxel_size_world{}; // world-space size of one voxel along each axis
    crd::u32                               surface_count = 0;
    crd::u32                               inside_count  = 0;
    crd::u32                               outside_count = 0;
    VoxelizationStatus                     status        = VoxelizationStatus::Ok;
};

using VoxelizationResultf = VoxelizationResult<crd::f32>;

} // namespace crd::geometry::decomposition
