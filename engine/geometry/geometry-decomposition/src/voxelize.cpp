// ---------------------------------------------------------------------------
// voxelize_mesh — Phase 3.1.7 v9c-a driver. Strictly two passes per
// D126: parallel SAT surface marking → join → classification.
// ---------------------------------------------------------------------------

#include <crd/geometry/decomposition/voxelize.hpp>

#include "voxelize_internal.hpp"

#include <crd/containers/array.hpp>
#include <crd/core/assert.hpp>
#include <crd/geometry/mesh/mesh_winding_number.hpp>
#include <crd/geometry/primitives/is_finite.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/math/scalar.hpp>
#include <crd/math/vec.hpp>

#include <algorithm>
#include <atomic>
#include <crd/math/cmath.hpp>

namespace crd::geometry::decomposition
{

namespace
{

using crd::geometry::mesh::TriangleMeshViewf;
using crd::geometry::primitives::AABB3;
using crd::math::Vec3;

// One ijk triple — addresses a voxel.
struct VoxelIndex
{
    crd::u32 ix;
    crd::u32 iy;
    crd::u32 iz;
};

// Geometry of the voxel grid in world space. Pre-derived once per voxelize
// call so per-triangle and per-voxel hot loops only multiply / divide by
// reciprocals.
struct GridGeom
{
    Vec3<crd::f32> origin{};        // world-space coordinate of voxel (0,0,0) corner
    Vec3<crd::f32> voxel_size{};    // per-axis world size of one voxel
    Vec3<crd::f32> inv_voxel_size{};// reciprocal for world→ijk
    crd::u32       nx = 0;
    crd::u32       ny = 0;
    crd::u32       nz = 0;
};

// --- Input validation ------------------------------------------------------

[[nodiscard]] VoxelizationStatus
validate_options(const VoxelizationOptions& opts) noexcept
{
    // Precedence: fixed_resolution wins if non-zero; else target_voxel_count
    // is consulted. At least one MUST be non-zero. Both-zero ⇒ no sizing
    // signal at all ⇒ InvalidOptions.
    if (opts.fixed_resolution == 0U && opts.target_voxel_count == 0U)
    {
        return VoxelizationStatus::InvalidOptions;
    }
    if (opts.max_resolution_per_axis == 0U)
    {
        return VoxelizationStatus::InvalidOptions;
    }
    return VoxelizationStatus::Ok;
}

[[nodiscard]] VoxelizationStatus
validate_mesh(const TriangleMeshViewf& view) noexcept
{
    if (view.triangle_count() == 0U)
    {
        return VoxelizationStatus::EmptyMesh;
    }
    for (const auto& v : view.vertices)
    {
        if (!crd::geometry::primitives::is_finite(v))
        {
            return VoxelizationStatus::NonFiniteInput;
        }
    }
    return VoxelizationStatus::Ok;
}

[[nodiscard]] AABB3<crd::f32>
compute_aabb(const TriangleMeshViewf& view) noexcept
{
    Vec3<crd::f32> lo {  std::numeric_limits<crd::f32>::infinity(),
                          std::numeric_limits<crd::f32>::infinity(),
                          std::numeric_limits<crd::f32>::infinity() };
    Vec3<crd::f32> hi { -std::numeric_limits<crd::f32>::infinity(),
                         -std::numeric_limits<crd::f32>::infinity(),
                         -std::numeric_limits<crd::f32>::infinity() };
    for (const auto& v : view.vertices)
    {
        lo.x = std::min(lo.x, v.x); hi.x = std::max(hi.x, v.x);
        lo.y = std::min(lo.y, v.y); hi.y = std::max(hi.y, v.y);
        lo.z = std::min(lo.z, v.z); hi.z = std::max(hi.z, v.z);
    }
    return AABB3<crd::f32>{lo, hi};
}

// --- Grid sizing -----------------------------------------------------------

// Pick (nx, ny, nz) from options + mesh AABB. Honour the
// validate_options-asserted invariant "exactly one of {fixed_resolution,
// target_voxel_count} is non-zero".
//
// `fixed_resolution = N` → N voxels along the LONGEST axis; other axes
// scaled proportionally so voxels stay cubic in world units (≥ 1 voxel).
//
// `target_voxel_count = K` → derive cubic voxel size from total volume:
//   voxel_size_world = cbrt(volume(aabb) / K); per-axis n = ceil(extent / size).
//
// Both paths cap each axis at `max_resolution_per_axis` to defend against
// pathological aspect ratios (a 1 × 1 × 0.001 sliver still gets a sane grid).
[[nodiscard]] VoxelIndex
choose_resolution(const VoxelizationOptions& opts, const AABB3<crd::f32>& aabb) noexcept
{
    const crd::f32 ex = aabb.max.x - aabb.min.x;
    const crd::f32 ey = aabb.max.y - aabb.min.y;
    const crd::f32 ez = aabb.max.z - aabb.min.z;
    const crd::f32 ext_max = std::max({ex, ey, ez});

    crd::u32 nx = 1;
    crd::u32 ny = 1;
    crd::u32 nz = 1;

    if (opts.fixed_resolution != 0U)
    {
        const crd::f32 n_along_longest = static_cast<crd::f32>(opts.fixed_resolution);
        const crd::f32 voxel = ext_max / n_along_longest;
        nx = std::max(1U, static_cast<crd::u32>(crd::math::ceil(ex / voxel)));
        ny = std::max(1U, static_cast<crd::u32>(crd::math::ceil(ey / voxel)));
        nz = std::max(1U, static_cast<crd::u32>(crd::math::ceil(ez / voxel)));
    }
    else
    {
        const crd::f64 volume = static_cast<crd::f64>(ex)
                              * static_cast<crd::f64>(ey)
                              * static_cast<crd::f64>(ez);
        const crd::f64 voxel = crd::math::cbrt(volume / static_cast<crd::f64>(opts.target_voxel_count));
        const crd::f32 vox_f = static_cast<crd::f32>(voxel);
        nx = std::max(1U, static_cast<crd::u32>(crd::math::ceil(ex / vox_f)));
        ny = std::max(1U, static_cast<crd::u32>(crd::math::ceil(ey / vox_f)));
        nz = std::max(1U, static_cast<crd::u32>(crd::math::ceil(ez / vox_f)));
    }

    const crd::u32 cap = opts.max_resolution_per_axis;
    nx = std::min(nx, cap);
    ny = std::min(ny, cap);
    nz = std::min(nz, cap);
    return {nx, ny, nz};
}

// --- Surface marking (pass 1) ---------------------------------------------

void mark_surface_for_triangle(VoxelGrid& grid, const GridGeom& geom,
                                const Vec3<crd::f32>& a,
                                const Vec3<crd::f32>& b,
                                const Vec3<crd::f32>& c) noexcept
{
    // Triangle AABB in world space.
    const crd::f32 tmin_x = std::min({a.x, b.x, c.x});
    const crd::f32 tmax_x = std::max({a.x, b.x, c.x});
    const crd::f32 tmin_y = std::min({a.y, b.y, c.y});
    const crd::f32 tmax_y = std::max({a.y, b.y, c.y});
    const crd::f32 tmin_z = std::min({a.z, b.z, c.z});
    const crd::f32 tmax_z = std::max({a.z, b.z, c.z});

    // World → ijk (floor on min, ceil on max), clamp to grid bounds.
    const auto to_ix = [&](crd::f32 v) noexcept -> crd::i32 {
        return static_cast<crd::i32>(crd::math::floor((v - geom.origin.x) * geom.inv_voxel_size.x));
    };
    const auto to_iy = [&](crd::f32 v) noexcept -> crd::i32 {
        return static_cast<crd::i32>(crd::math::floor((v - geom.origin.y) * geom.inv_voxel_size.y));
    };
    const auto to_iz = [&](crd::f32 v) noexcept -> crd::i32 {
        return static_cast<crd::i32>(crd::math::floor((v - geom.origin.z) * geom.inv_voxel_size.z));
    };

    // First, detect "triangle entirely outside grid" via the UNCLAMPED
    // index range. Then clamp both endpoints to the valid voxel range.
    // The two steps are separate because a single std::clamp would lose
    // the entirely-outside-grid signal — e.g. a triangle at y > 1.0 with
    // `to_iy(tmin)=to_iy(tmax)=5` would both clamp to `ny-1=3` and we'd
    // incorrectly test the top voxels. Without separating: triangles at
    // the EXACT upper grid boundary (y=1.0 on a unit cube, common in
    // sealed meshes) round to `floor(N)=N` ⇒ both ≥ n ⇒ skipped, which
    // silently loses the top/right/back faces.
    const crd::i32 raw_ix0 = to_ix(tmin_x);
    const crd::i32 raw_iy0 = to_iy(tmin_y);
    const crd::i32 raw_iz0 = to_iz(tmin_z);
    const crd::i32 raw_ix1 = to_ix(tmax_x);
    const crd::i32 raw_iy1 = to_iy(tmax_y);
    const crd::i32 raw_iz1 = to_iz(tmax_z);

    // Early-out: triangle entirely below or strictly above grid. At the
    // exact upper boundary (e.g. y=1.0 on a unit cube + fixed_res=4 ⇒
    // raw_iy=floor(4.0)=4=ny), the triangle still touches voxel iy=ny-1,
    // so we use strict `> ny` not `>= ny` — and clamp loop bounds below.
    if (raw_ix1 < 0 || raw_ix0 > static_cast<crd::i32>(geom.nx)
        || raw_iy1 < 0 || raw_iy0 > static_cast<crd::i32>(geom.ny)
        || raw_iz1 < 0 || raw_iz0 > static_cast<crd::i32>(geom.nz))
    {
        return;
    }

    const crd::i32 ix0 = std::clamp(raw_ix0, 0, static_cast<crd::i32>(geom.nx) - 1);
    const crd::i32 iy0 = std::clamp(raw_iy0, 0, static_cast<crd::i32>(geom.ny) - 1);
    const crd::i32 iz0 = std::clamp(raw_iz0, 0, static_cast<crd::i32>(geom.nz) - 1);
    const crd::i32 ix1 = std::clamp(raw_ix1, 0, static_cast<crd::i32>(geom.nx) - 1);
    const crd::i32 iy1 = std::clamp(raw_iy1, 0, static_cast<crd::i32>(geom.ny) - 1);
    const crd::i32 iz1 = std::clamp(raw_iz1, 0, static_cast<crd::i32>(geom.nz) - 1);

    const Vec3<crd::f32> half = {
        geom.voxel_size.x * 0.5F,
        geom.voxel_size.y * 0.5F,
        geom.voxel_size.z * 0.5F,
    };

    for (crd::i32 iz = iz0; iz <= iz1; ++iz)
    {
        for (crd::i32 iy = iy0; iy <= iy1; ++iy)
        {
            for (crd::i32 ix = ix0; ix <= ix1; ++ix)
            {
                const Vec3<crd::f32> centre = {
                    geom.origin.x + (static_cast<crd::f32>(ix) + 0.5F) * geom.voxel_size.x,
                    geom.origin.y + (static_cast<crd::f32>(iy) + 0.5F) * geom.voxel_size.y,
                    geom.origin.z + (static_cast<crd::f32>(iz) + 0.5F) * geom.voxel_size.z,
                };
                if (detail::tri_box_overlap_sat<crd::f32>(centre, half, a, b, c))
                {
                    grid.mark_surface(static_cast<crd::u32>(ix),
                                       static_cast<crd::u32>(iy),
                                       static_cast<crd::u32>(iz));
                }
            }
        }
    }
}

void mark_surface_pass(VoxelGrid& grid, const GridGeom& geom,
                       const TriangleMeshViewf& view) noexcept
{
    const crd::u32 tri_count = view.triangle_count();
    if (tri_count == 0U)
    {
        return;
    }

    // Parallel triangle walk. Per-job range is contiguous in triangle order,
    // so each worker independently processes a sub-span of triangles; per-
    // voxel writes use atomic fetch_or so concurrent writers converge.
    //
    // Threshold: skip the jobs system for tiny meshes (~<256 tris) where
    // fan-out overhead exceeds the work. Pinned soft heuristic.
    constexpr crd::u32 parallel_threshold = 256U;
    if (tri_count < parallel_threshold)
    {
        for (crd::u32 ti = 0U; ti < tri_count; ++ti)
        {
            const crd::u32 i0 = view.indices[ti * 3U + 0U];
            const crd::u32 i1 = view.indices[ti * 3U + 1U];
            const crd::u32 i2 = view.indices[ti * 3U + 2U];
            mark_surface_for_triangle(grid, geom,
                                       view.vertices[i0], view.vertices[i1], view.vertices[i2]);
        }
        return;
    }

    constexpr crd::u32 jobs_per_call = 8U;
    auto* counter = crd::jobs::parallel_for(
        tri_count, jobs_per_call,
        [&grid, &geom, &view](crd::u32 begin, crd::u32 end) noexcept {
            for (crd::u32 ti = begin; ti < end; ++ti)
            {
                const crd::u32 i0 = view.indices[ti * 3U + 0U];
                const crd::u32 i1 = view.indices[ti * 3U + 1U];
                const crd::u32 i2 = view.indices[ti * 3U + 2U];
                mark_surface_for_triangle(grid, geom,
                                           view.vertices[i0], view.vertices[i1], view.vertices[i2]);
            }
        });
    crd::jobs::wait(counter);
}

// --- Classification (pass 2) ----------------------------------------------

// FloodFill from the corner voxel (assumed Outside thanks to padding).
// 6-connected BFS. Writes Outside into every Unknown voxel reachable
// without crossing a Surface cell.
void flood_fill_outside(VoxelGrid& grid, crd::memory::IAllocator* alloc) noexcept
{
    if (grid.is_empty())
    {
        return;
    }
    crd::containers::Array<VoxelIndex> stack(alloc);
    stack.reserve(1024U);

    // Seed: voxel (0, 0, 0) — guaranteed Outside iff padding ≥ 1 OR caller
    // accepts the risk. Skip seeding if the seed cell is Surface (mesh
    // touches the grid corner; flood-fill is unreliable, leave Unknown).
    if (grid.state_at(0U, 0U, 0U) != VoxelState::Unknown)
    {
        return;
    }
    grid.set_state(0U, 0U, 0U, VoxelState::Outside);
    stack.push_back({0U, 0U, 0U});

    const auto try_push = [&](crd::u32 nx, crd::u32 ny, crd::u32 nz) noexcept {
        if (grid.state_at(nx, ny, nz) == VoxelState::Unknown)
        {
            grid.set_state(nx, ny, nz, VoxelState::Outside);
            stack.push_back({nx, ny, nz});
        }
    };

    while (!stack.empty())
    {
        const VoxelIndex v = stack.back();
        stack.pop_back();
        if (v.ix > 0U)              { try_push(v.ix - 1U, v.iy,      v.iz); }
        if (v.ix + 1U < grid.nx())  { try_push(v.ix + 1U, v.iy,      v.iz); }
        if (v.iy > 0U)              { try_push(v.ix,      v.iy - 1U, v.iz); }
        if (v.iy + 1U < grid.ny())  { try_push(v.ix,      v.iy + 1U, v.iz); }
        if (v.iz > 0U)              { try_push(v.ix,      v.iy,      v.iz - 1U); }
        if (v.iz + 1U < grid.nz())  { try_push(v.ix,      v.iy,      v.iz + 1U); }
    }
}

void classify_floodfill(VoxelGrid& grid, crd::memory::IAllocator* alloc) noexcept
{
    flood_fill_outside(grid, alloc);
    // Anything still Unknown after flood is Inside (sealed pockets).
    for (crd::u32 iz = 0U; iz < grid.nz(); ++iz)
    {
        for (crd::u32 iy = 0U; iy < grid.ny(); ++iy)
        {
            for (crd::u32 ix = 0U; ix < grid.nx(); ++ix)
            {
                if (grid.state_at(ix, iy, iz) == VoxelState::Unknown)
                {
                    grid.set_state(ix, iy, iz, VoxelState::Inside);
                }
            }
        }
    }
}

void classify_winding_number(VoxelGrid& grid, const GridGeom& geom,
                              const TriangleMeshViewf& view) noexcept
{
    for (crd::u32 iz = 0U; iz < grid.nz(); ++iz)
    {
        for (crd::u32 iy = 0U; iy < grid.ny(); ++iy)
        {
            for (crd::u32 ix = 0U; ix < grid.nx(); ++ix)
            {
                if (grid.state_at(ix, iy, iz) != VoxelState::Unknown)
                {
                    continue;
                }
                const Vec3<crd::f32> centre = {
                    geom.origin.x + (static_cast<crd::f32>(ix) + 0.5F) * geom.voxel_size.x,
                    geom.origin.y + (static_cast<crd::f32>(iy) + 0.5F) * geom.voxel_size.y,
                    geom.origin.z + (static_cast<crd::f32>(iz) + 0.5F) * geom.voxel_size.z,
                };
                const crd::f32 w = crd::geometry::mesh::mesh_winding_number(view, centre);
                grid.set_state(ix, iy, iz, w > 0.5F ? VoxelState::Inside : VoxelState::Outside);
            }
        }
    }
}

// --- Telemetry -------------------------------------------------------------

void collect_telemetry(const VoxelGrid& grid,
                       crd::u32& surface, crd::u32& inside, crd::u32& outside) noexcept
{
    surface = inside = outside = 0U;
    for (crd::u32 iz = 0U; iz < grid.nz(); ++iz)
    {
        for (crd::u32 iy = 0U; iy < grid.ny(); ++iy)
        {
            for (crd::u32 ix = 0U; ix < grid.nx(); ++ix)
            {
                switch (grid.state_at(ix, iy, iz))
                {
                    case VoxelState::Surface: ++surface; break;
                    case VoxelState::Inside:  ++inside;  break;
                    case VoxelState::Outside: ++outside; break;
                    case VoxelState::Unknown: break;
                }
            }
        }
    }
}

} // namespace

VoxelizationResult<crd::f32>
voxelize_mesh(const TriangleMeshViewf& view,
              const VoxelizationOptions& opts,
              crd::memory::IAllocator* alloc) noexcept
{
    VoxelizationResult<crd::f32> result{};

    if (auto s = validate_options(opts); s != VoxelizationStatus::Ok)
    {
        result.status = s;
        return result;
    }
    if (auto s = validate_mesh(view); s != VoxelizationStatus::Ok)
    {
        result.status = s;
        return result;
    }

    const AABB3<crd::f32> raw_aabb = compute_aabb(view);
    const crd::f32 ex = raw_aabb.max.x - raw_aabb.min.x;
    const crd::f32 ey = raw_aabb.max.y - raw_aabb.min.y;
    const crd::f32 ez = raw_aabb.max.z - raw_aabb.min.z;
    if (!(ex > 0.0F && ey > 0.0F && ez > 0.0F))
    {
        result.status = VoxelizationStatus::DegenerateAABB;
        return result;
    }

    // Derive per-axis voxel resolution from sizing options on the RAW AABB.
    const VoxelIndex unpadded = choose_resolution(opts, raw_aabb);

    // Cubic voxel size in world units (derived from longest mesh axis ÷
    // raw resolution along it).
    const crd::f32 longest = std::max({ex, ey, ez});
    crd::f32 longest_res = static_cast<crd::f32>(unpadded.iz);
    if (ex == longest)
    {
        longest_res = static_cast<crd::f32>(unpadded.ix);
    }
    else if (ey == longest)
    {
        longest_res = static_cast<crd::f32>(unpadded.iy);
    }
    const crd::f32 voxel_world = longest / longest_res;

    // Apply padding: extend on every side, expanding the grid AABB OUTWARD.
    const crd::u32 pad = opts.padding_voxels;
    const crd::u32 nx_padded = unpadded.ix + 2U * pad;
    const crd::u32 ny_padded = unpadded.iy + 2U * pad;
    const crd::u32 nz_padded = unpadded.iz + 2U * pad;

    const Vec3<crd::f32> origin = {
        raw_aabb.min.x - static_cast<crd::f32>(pad) * voxel_world,
        raw_aabb.min.y - static_cast<crd::f32>(pad) * voxel_world,
        raw_aabb.min.z - static_cast<crd::f32>(pad) * voxel_world,
    };

    GridGeom geom{};
    geom.origin         = origin;
    geom.voxel_size     = {voxel_world, voxel_world, voxel_world};
    geom.inv_voxel_size = {1.0F / voxel_world, 1.0F / voxel_world, 1.0F / voxel_world};
    geom.nx             = nx_padded;
    geom.ny             = ny_padded;
    geom.nz             = nz_padded;

    VoxelGrid grid(geom.nx, geom.ny, geom.nz, alloc);
    if (grid.is_empty())
    {
        result.status = VoxelizationStatus::OutOfMemory;
        return result;
    }

    // Pass 1: SAT surface marking.
    mark_surface_pass(grid, geom, view);

    // Pass 2: classification of still-Unknown voxels.
    switch (opts.classify)
    {
        case ClassificationMode::WindingNumber:
            classify_winding_number(grid, geom, view);
            break;
        case ClassificationMode::FloodFill:
            classify_floodfill(grid, alloc);
            break;
    }

    // Telemetry.
    collect_telemetry(grid, result.surface_count, result.inside_count, result.outside_count);

    result.grid_aabb = AABB3<crd::f32>{
        origin,
        Vec3<crd::f32>{
            origin.x + static_cast<crd::f32>(geom.nx) * voxel_world,
            origin.y + static_cast<crd::f32>(geom.ny) * voxel_world,
            origin.z + static_cast<crd::f32>(geom.nz) * voxel_world,
        }
    };
    result.voxel_size_world = geom.voxel_size;
    result.grid             = std::move(grid);
    result.status           = VoxelizationStatus::Ok;
    return result;
}

} // namespace crd::geometry::decomposition
