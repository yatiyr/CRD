// The crd-hesap-sparse tests' arm inputs: values with IEEE specials and skewed CSR matrices with empty rows, for the
// crd-jobs arms' bit gates (spgemm_arm, the sparse LinearOps' picks).

#pragma once

#include <crd/hesap/complex.hpp>
#include <crd/hesap/sparse/sparse_matrix.hpp>
#include <crd/hesap/sparse/triplet_builder.hpp>
#include <crd/memory/allocator.hpp>

#include <cstdint>
#include <cstring>
#include <limits>
#include <type_traits>

namespace crd_hesap_sparse_tests
{
template <typename T> struct ArmReal
{
    using type = T;
};
template <typename U> struct ArmReal<crd::hesap::Complex<U>>
{
    using type = U;
};

template <typename T> constexpr bool kArmComplex = !std::is_same_v<typename ArmReal<T>::type, T>;

// A value from hash h; with `specials`, every eleventh value is NaN, +Inf, -Inf, -0 or the smallest subnormal.
template <typename T> T arm_value(crd::u32 h, bool specials)
{
    using R = typename ArmReal<T>::type;
    R re = static_cast<R>(1 + h % 9) / static_cast<R>(7);
    if (specials && h % 11 < 5)
    {
        const R picks[] = {std::numeric_limits<R>::quiet_NaN(), std::numeric_limits<R>::infinity(),
                           -std::numeric_limits<R>::infinity(), -R(0), std::numeric_limits<R>::denorm_min()};
        re = picks[h % 11];
    }
    if constexpr (kArmComplex<T>)
    {
        return T{re, static_cast<R>(h % 5) / static_cast<R>(3) - R(1)};
    }
    else
    {
        return re;
    }
}

// Skewed row degrees (every ninth row five times the rest) and every seventh row empty.
template <typename T>
crd::hesap::sparse::SparseMatrix<T, crd::hesap::sparse::SparseFormat::Csr>
arm_csr(crd::memory::IAllocator* alloc, crd::u32 rows, crd::u32 cols, crd::u32 per_row, std::uint64_t seed,
        bool specials)
{
    std::uint64_t s = seed;
    auto next = [&]()
    {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<crd::u32>(s >> 33);
    };
    crd::hesap::sparse::TripletBuilder<T> b(alloc, rows, cols);
    for (crd::u32 r = 0; r < rows; ++r)
    {
        if (r % 7 == 3)
        {
            continue;
        }
        const crd::u32 deg = (next() % 9 == 0) ? (per_row * 5) : per_row;
        for (crd::u32 k = 0; k < deg; ++k)
        {
            const crd::u32 col = next() % cols;
            b.add(r, col, arm_value<T>(next(), specials));
        }
    }
    return b.compress();
}

template <typename U> inline bool same_bytes(const U* x, const U* y, crd::usize n)
{
    return n == 0 || std::memcmp(x, y, n * sizeof(U)) == 0;
}
} // namespace crd_hesap_sparse_tests
