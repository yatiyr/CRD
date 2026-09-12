// ---------------------------------------------------------------------------
// VoxelGrid ctors + move semantics. Storage allocation lives here because
// `Array<u8>::resize(n, fill)` is the safest path to a zero-initialised
// buffer of the right size and we want OOM-tolerant construction (set the
// grid to empty + caller detects via `is_empty()`).
// ---------------------------------------------------------------------------

#include <crd/geometry/decomposition/voxel.hpp>

#include <utility>

namespace crd::geometry::decomposition
{

VoxelGrid::VoxelGrid(crd::u32 nx, crd::u32 ny, crd::u32 nz, crd::memory::IAllocator* alloc) noexcept
    : m_cells(alloc), m_nx(nx), m_ny(ny), m_nz(nz)
{
    const crd::u64 count = static_cast<crd::u64>(nx) * static_cast<crd::u64>(ny) * static_cast<crd::u64>(nz);
    if (count == 0U)
    {
        return;
    }
    if (!m_cells.try_reserve(static_cast<crd::usize>(count)))
    {
        m_nx = m_ny = m_nz = 0U;
        return;
    }
    m_cells.resize(static_cast<crd::usize>(count), static_cast<crd::u8>(VoxelState::Unknown));
}

VoxelGrid::VoxelGrid(VoxelGrid&& other) noexcept
    : m_cells(std::move(other.m_cells)), m_nx(other.m_nx), m_ny(other.m_ny), m_nz(other.m_nz)
{
    other.m_nx = other.m_ny = other.m_nz = 0U;
}

VoxelGrid& VoxelGrid::operator=(VoxelGrid&& other) noexcept
{
    if (this != &other)
    {
        m_cells = std::move(other.m_cells);
        m_nx    = other.m_nx;
        m_ny    = other.m_ny;
        m_nz    = other.m_nz;
        other.m_nx = other.m_ny = other.m_nz = 0U;
    }
    return *this;
}

} // namespace crd::geometry::decomposition
