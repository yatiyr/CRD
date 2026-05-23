#include <crd/hesap/dense/tls.hpp>

#include <crd/containers/array.hpp>
#include <crd/core/assert.hpp>
#include <crd/hesap/complex.hpp>
#include <crd/hesap/dense/svd.hpp>

#include <cmath>
#include <limits>
#include <type_traits>
#include <utility>

namespace crd::hesap::dense
{
namespace
{
template <typename T>
inline RealType<T> mag(const T& z) noexcept
{
    if constexpr (is_complex_v<T>)
    {
        return crd::hesap::abs(z);
    }
    else
    {
        return std::abs(z);
    }
}

// Type-generic d×d inverse via Gauss-Jordan with partial pivoting (works for
// real and complex T). Returns false if singular (tiny pivot). `a` (d×d
// RowMajor, leading dim d) is overwritten; `inv` receives the inverse.
template <typename T>
bool gauss_jordan_inverse(T* a, crd::usize d, T* inv, crd::memory::IAllocator* /*alloc*/)
{
    using R = RealType<T>;
    for (crd::usize i = 0; i < d; ++i)
    {
        for (crd::usize j = 0; j < d; ++j)
        {
            inv[i * d + j] = (i == j) ? T{1} : T{0};
        }
    }
    for (crd::usize col = 0; col < d; ++col)
    {
        // Partial pivot: largest-magnitude entry in column `col`, rows >= col.
        crd::usize piv = col;
        R pmax = mag<T>(a[col * d + col]);
        for (crd::usize r = col + 1; r < d; ++r)
        {
            const R mm = mag<T>(a[r * d + col]);
            if (mm > pmax)
            {
                pmax = mm;
                piv = r;
            }
        }
        if (pmax <= std::numeric_limits<R>::epsilon() * static_cast<R>(16))
        {
            return false;  // singular
        }
        if (piv != col)
        {
            for (crd::usize j = 0; j < d; ++j)
            {
                std::swap(a[col * d + j], a[piv * d + j]);
                std::swap(inv[col * d + j], inv[piv * d + j]);
            }
        }
        const T diag = a[col * d + col];
        const T inv_diag = T{1} / diag;
        for (crd::usize j = 0; j < d; ++j)
        {
            a[col * d + j] = a[col * d + j] * inv_diag;
            inv[col * d + j] = inv[col * d + j] * inv_diag;
        }
        for (crd::usize r = 0; r < d; ++r)
        {
            if (r == col)
            {
                continue;
            }
            const T factor = a[r * d + col];
            for (crd::usize j = 0; j < d; ++j)
            {
                a[r * d + j] = a[r * d + j] - factor * a[col * d + j];
                inv[r * d + j] = inv[r * d + j] - factor * inv[col * d + j];
            }
        }
    }
    return true;
}
} // namespace

template <typename T>
TLS<T> tls(crd::memory::IAllocator* alloc, const Matrix<T>& a, const Matrix<T>& b)
{
    const crd::usize m = a.rows();
    const crd::usize n = a.cols();
    const crd::usize d = b.cols();
    CRD_ASSERT_MSG(b.rows() == m, "tls: A and B row count mismatch");

    TLS<T> out(alloc);
    out.x = Matrix<T>(alloc, n, d);
    out.x.set_zero();

    // Augmented C = [A | B]  (m × (n+d)).
    const crd::usize nc = n + d;
    Matrix<T> c(alloc, m, nc);
    for (crd::usize i = 0; i < m; ++i)
    {
        for (crd::usize j = 0; j < n; ++j)
        {
            c.at(i, j) = a.at(i, j);
        }
        for (crd::usize j = 0; j < d; ++j)
        {
            c.at(i, n + j) = b.at(i, j);
        }
    }

    SVD<T> s = svd<T>(alloc, c);
    const crd::usize k = s.s.size();  // min(m, n+d)
    // Need the full (n+d) right singular vectors so the last d columns of V are
    // available (the smallest-σ subspace). Requires m >= n+d.
    if (k < nc)
    {
        out.exists = false;  // underdetermined augmented system — no unique TLS
        return out;
    }

    // V is (n+d) × (n+d). Partition the LAST d columns: V12 = V[0:n, n:n+d],
    // V22 = V[n:n+d, n:n+d].  X = -V12 · V22⁻¹.
    Matrix<T> v22(alloc, d, d);
    for (crd::usize i = 0; i < d; ++i)
    {
        for (crd::usize j = 0; j < d; ++j)
        {
            v22.at(i, j) = s.v.at(n + i, n + j);
        }
    }
    crd::containers::Array<T> v22inv(alloc);
    v22inv.resize(d * d);
    if (!gauss_jordan_inverse<T>(v22.data(), d, v22inv.data(), alloc))
    {
        out.exists = false;  // V22 singular → TLS solution does not exist/unique
        return out;
    }

    // X[i][l] = -Σ_j V12[i][j] · V22inv[j][l],  V12[i][j] = V[i][n+j].
    for (crd::usize i = 0; i < n; ++i)
    {
        for (crd::usize l = 0; l < d; ++l)
        {
            T acc = T{};
            for (crd::usize j = 0; j < d; ++j)
            {
                acc = acc + s.v.at(i, n + j) * v22inv[j * d + l];
            }
            out.x.at(i, l) = -acc;
        }
    }
    out.exists = true;
    return out;
}

template <typename T>
TLS<T> tls(crd::memory::IAllocator* alloc, const Matrix<T>& a, const Vector<T>& b)
{
    CRD_ASSERT_MSG(a.rows() == b.size(), "tls: A rows != b size");
    Matrix<T> bm(alloc, b.size(), 1);
    for (crd::usize i = 0; i < b.size(); ++i)
    {
        bm.at(i, 0) = b(i);
    }
    return tls<T>(alloc, a, bm);
}

template TLS<float> tls<float>(crd::memory::IAllocator*, const Matrix<float>&, const Matrix<float>&);
template TLS<double> tls<double>(crd::memory::IAllocator*, const Matrix<double>&, const Matrix<double>&);
template TLS<Complex<float>> tls<Complex<float>>(crd::memory::IAllocator*,
                                                 const Matrix<Complex<float>>&,
                                                 const Matrix<Complex<float>>&);
template TLS<Complex<double>> tls<Complex<double>>(crd::memory::IAllocator*,
                                                   const Matrix<Complex<double>>&,
                                                   const Matrix<Complex<double>>&);
template TLS<float> tls<float>(crd::memory::IAllocator*, const Matrix<float>&, const Vector<float>&);
template TLS<double> tls<double>(crd::memory::IAllocator*, const Matrix<double>&, const Vector<double>&);
template TLS<Complex<float>> tls<Complex<float>>(crd::memory::IAllocator*,
                                                 const Matrix<Complex<float>>&,
                                                 const Vector<Complex<float>>&);
template TLS<Complex<double>> tls<Complex<double>>(crd::memory::IAllocator*,
                                                   const Matrix<Complex<double>>&,
                                                   const Vector<Complex<double>>&);

} // namespace crd::hesap::dense
