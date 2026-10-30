// The 3D seven-point stencil fixture shared by the multifrontal tests: a k x k x k grid (x fastest), `diag` on the
// diagonal and off(i, j) at each coupling i > j, mirrored to (j, i) through `mirror` (the identity for a symmetric
// matrix, the conjugate for a Hermitian one). The real overload is the Laplacian family: -1 off the diagonal, so 6 is
// the SPD Laplacian and 6 - sigma an indefinite shift.

#pragma once

#include <crd/core/types.hpp>
#include <crd/hesap/sparse/convert.hpp>
#include <crd/hesap/sparse/sparse_matrix.hpp>
#include <crd/hesap/sparse/triplet_builder.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::hesap::direct::test
{

template <typename T, typename Off, typename Mirror>
[[nodiscard]] crd::hesap::sparse::SparseMatrix<T, crd::hesap::sparse::SparseFormat::Csc>
stencil_3d(crd::u32 k, T diag, Off off, Mirror mirror, crd::memory::IAllocator* alloc)
{
    const crd::u32 n = k * k * k;
    crd::hesap::sparse::TripletBuilder<T> tb(alloc, n, n);
    const auto idx = [k](crd::u32 x, crd::u32 y, crd::u32 z)
    {
        return (z * k + y) * k + x;
    };
    const auto couple = [&](crd::u32 i, crd::u32 j)
    {
        const T v = off(i, j);
        tb.add(i, j, v);
        tb.add(j, i, mirror(v));
    };
    for (crd::u32 z = 0; z < k; ++z)
    {
        for (crd::u32 y = 0; y < k; ++y)
        {
            for (crd::u32 x = 0; x < k; ++x)
            {
                const crd::u32 i = idx(x, y, z);
                tb.add(i, i, diag);
                if (x > 0)
                {
                    couple(i, idx(x - 1, y, z));
                }
                if (y > 0)
                {
                    couple(i, idx(x, y - 1, z));
                }
                if (z > 0)
                {
                    couple(i, idx(x, y, z - 1));
                }
            }
        }
    }
    return crd::hesap::sparse::to_csc<T>(tb.compress(), alloc);
}

[[nodiscard]] inline crd::hesap::sparse::SparseMatrix<double, crd::hesap::sparse::SparseFormat::Csc>
stencil_3d(crd::u32 k, double diag, crd::memory::IAllocator* alloc)
{
    return stencil_3d<double>(k, diag, [](crd::u32, crd::u32) { return -1.0; }, [](double v) { return v; }, alloc);
}

} // namespace crd::hesap::direct::test
