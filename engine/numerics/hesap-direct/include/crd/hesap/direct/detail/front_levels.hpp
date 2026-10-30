// The assembly tree's level schedule, shared by the multifrontal walks (LU, LDLT, QR, BLR Cholesky). A leaf front is
// on level 0, any other one level above its deepest child; a level lists its fronts in ascending index, so the
// schedule never depends on the job count (the determinism moat). Same-level fronts are independent: a front's
// contribution is read only by its parent, a level higher, after the level barrier.

#pragma once

#include <crd/containers/array.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::hesap::direct::detail
{

struct FrontLevels
{
    explicit FrontLevels(crd::memory::IAllocator* alloc) : ptr(alloc), list(alloc) {}

    [[nodiscard]] crd::u32 count() const noexcept { return static_cast<crd::u32>(ptr.size()) - 1U; }

    crd::containers::Array<crd::u32> ptr;  // level l holds list[ptr[l], ptr[l + 1]); count() + 1 entries
    crd::containers::Array<crd::u32> list; // the nf fronts by level
};

// Front f's children are child_idx[child_ptr[f], child_ptr[f + 1]). post lists the nf fronts children first, or is
// nullptr when ascending index already does.
[[nodiscard]] inline FrontLevels front_levels(crd::memory::IAllocator* alloc, crd::u32 nf, const crd::u32* child_ptr,
                                              const crd::u32* child_idx, const crd::u32* post)
{
    crd::containers::Array<crd::u32> level(alloc);
    level.resize(nf);
    crd::u32 nlevels = 0;
    for (crd::u32 i = 0; i < nf; ++i)
    {
        const crd::u32 f = post != nullptr ? post[i] : i;
        crd::u32 lev = 0;
        for (crd::u32 cc = child_ptr[f]; cc < child_ptr[f + 1]; ++cc)
        {
            const crd::u32 lk = level[child_idx[cc]] + 1;
            lev = lk > lev ? lk : lev;
        }
        level[f] = lev;
        nlevels = lev + 1 > nlevels ? lev + 1 : nlevels;
    }
    FrontLevels out(alloc);
    out.ptr.resize(static_cast<crd::usize>(nlevels) + 1); // value-initialised to 0
    for (crd::u32 f = 0; f < nf; ++f)
    {
        ++out.ptr[level[f] + 1];
    }
    for (crd::u32 l = 0; l < nlevels; ++l)
    {
        out.ptr[l + 1] += out.ptr[l];
    }
    crd::containers::Array<crd::u32> next(alloc);
    next.resize(nlevels);
    for (crd::u32 l = 0; l < nlevels; ++l)
    {
        next[l] = out.ptr[l];
    }
    out.list.resize(nf);
    for (crd::u32 f = 0; f < nf; ++f)
    {
        out.list[next[level[f]]++] = f;
    }
    return out;
}

} // namespace crd::hesap::direct::detail
