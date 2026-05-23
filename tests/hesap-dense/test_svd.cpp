#include <catch2/catch_test_macros.hpp>

#include <crd/containers/array.hpp>
#include <crd/hesap/cli/command_registry.hpp>
#include <crd/hesap/dense/cli_anchor.hpp>
#include <crd/hesap/dense/detail/bdsqr.hpp>
#include <crd/hesap/dense/detail/orgbr.hpp>
#include <crd/hesap/dense/detail/svd_complex.hpp>
#include <crd/hesap/dense/detail/svd_dc.hpp>
#include <crd/hesap/dense/detail/svd_secular.hpp>
#include <crd/hesap/dense/eig_sym.hpp>
#include <crd/hesap/dense/matrix_types.hpp>
#include <crd/hesap/dense/svd.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <algorithm>
#include <cmath>
#include <tuple>

namespace
{
// Force cli_register_svd.cpp's TU (static-init) to be linked in.
struct SvdAnchorPull
{
    SvdAnchorPull() noexcept { crd::hesap::dense::register_svd_cli_anchor(); }
};
const SvdAnchorPull kSvdAnchorPull;
} // namespace

// =======================================================================
// Phase 3.1.6 v3b-1a — Golub-Kahan bidiagonalization isolation test.
// Reduce A (m>=n) to upper bidiagonal Q^T A P = B and verify
// A == Q B P^T (reconstruction), Q/P orthonormal, B bidiagonal.
// =======================================================================

namespace
{
// Apply H(i) (left reflector, v in column i of `rfl`, v[0]=1) from the LEFT to
// all columns of R (mr x mc, row-major ld=mc), rows i..mr-1.
void apply_h_left(double* r, int mc, const double* rfl, int lda, int i, int mr, double tau)
{
    if (tau == 0.0)
    {
        return;
    }
    for (int c = 0; c < mc; ++c)
    {
        double dot = r[i * mc + c];  // v[0]=1
        for (int k = i + 1; k < mr; ++k)
        {
            dot += rfl[k * lda + i] * r[k * mc + c];
        }
        const double s = tau * dot;
        r[i * mc + c] -= s;
        for (int k = i + 1; k < mr; ++k)
        {
            r[k * mc + c] -= rfl[k * lda + i] * s;
        }
    }
}

// Apply G(i) (right reflector, v in row i of `rfl`, v[0]=1 at col i+1) from the
// LEFT to all columns of R (nn x nn), rows i+1..nn-1.
void apply_g_left(double* r, int nn, const double* rfl, int lda, int i, double tau)
{
    if (tau == 0.0)
    {
        return;
    }
    for (int c = 0; c < nn; ++c)
    {
        double dot = r[(i + 1) * nn + c];  // v[0]=1 at row i+1
        for (int k = i + 2; k < nn; ++k)
        {
            dot += rfl[i * lda + k] * r[k * nn + c];
        }
        const double s = tau * dot;
        r[(i + 1) * nn + c] -= s;
        for (int k = i + 2; k < nn; ++k)
        {
            r[k * nn + c] -= rfl[i * lda + k] * s;
        }
    }
}

double check_bidiag(int m, int n)
{
    crd::memory::TlsfAllocator alloc(8U * 1024U * 1024U);
    crd::containers::Array<double> a(m * n, &alloc);
    crd::containers::Array<double> acopy(m * n, &alloc);
    crd::containers::Array<double> d(n, &alloc);
    crd::containers::Array<double> e(n, &alloc);
    crd::containers::Array<double> tauq(n, &alloc);
    crd::containers::Array<double> taup(n, &alloc);
    crd::u64 s = 0xC0FFEE1234567ULL + static_cast<crd::u64>(m * 131 + n);
    auto next = [&s]() {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<double>((s >> 11) & 0xFFFFFFFFFFFFFULL) / static_cast<double>(1ULL << 52);
    };
    for (int i = 0; i < m * n; ++i)
    {
        a.data()[i] = 2.0 * next() - 1.0;
        acopy.data()[i] = a.data()[i];
    }

    crd::hesap::dense::bidiagonalize<double>(a.data(), static_cast<crd::usize>(m), static_cast<crd::usize>(n),
                                             static_cast<crd::usize>(n), d.data(), e.data(), tauq.data(),
                                             taup.data(), &alloc);

    // Form Q (m x m) = H(0)...H(n-1) applied to I.
    crd::containers::Array<double> q(m * m, &alloc);
    for (int i = 0; i < m * m; ++i)
    {
        q.data()[i] = (i / m == i % m) ? 1.0 : 0.0;
    }
    for (int i = n - 1; i >= 0; --i)
    {
        apply_h_left(q.data(), m, a.data(), n, i, m, tauq.data()[i]);
    }
    // Form P (n x n) = G(0)...G(n-2) applied to I.
    crd::containers::Array<double> p(n * n, &alloc);
    for (int i = 0; i < n * n; ++i)
    {
        p.data()[i] = (i / n == i % n) ? 1.0 : 0.0;
    }
    for (int i = n - 2; i >= 0; --i)
    {
        apply_g_left(p.data(), n, a.data(), n, i, taup.data()[i]);
    }

    // B (m x n) bidiagonal.
    crd::containers::Array<double> b(m * n, &alloc);
    for (int i = 0; i < m * n; ++i)
    {
        b.data()[i] = 0.0;
    }
    for (int i = 0; i < n; ++i)
    {
        b.data()[i * n + i] = d.data()[i];
        if (i + 1 < n)
        {
            b.data()[i * n + (i + 1)] = e.data()[i];
        }
    }

    // R = Q * B * P^T, compare to acopy.
    crd::containers::Array<double> qb(m * n, &alloc);
    for (int i = 0; i < m; ++i)
    {
        for (int j = 0; j < n; ++j)
        {
            double acc = 0.0;
            for (int k = 0; k < m; ++k)
            {
                acc += q.data()[i * m + k] * b.data()[k * n + j];
            }
            qb.data()[i * n + j] = acc;
        }
    }
    double worst = 0.0;
    for (int i = 0; i < m; ++i)
    {
        for (int j = 0; j < n; ++j)
        {
            double acc = 0.0;
            for (int k = 0; k < n; ++k)
            {
                acc += qb.data()[i * n + k] * p.data()[j * n + k];  // (P^T)[k][j] = P[j][k]
            }
            worst = std::max(worst, std::abs(acc - acopy.data()[i * n + j]));
        }
    }
    return worst;
}

double orth_err(int dim, const double* q)
{
    double worst = 0.0;
    for (int i = 0; i < dim; ++i)
    {
        for (int j = 0; j < dim; ++j)
        {
            double dot = 0.0;
            for (int k = 0; k < dim; ++k)
            {
                dot += q[k * dim + i] * q[k * dim + j];
            }
            worst = std::max(worst, std::abs(dot - (i == j ? 1.0 : 0.0)));
        }
    }
    return worst;
}
} // namespace

namespace
{
using crd::hesap::dense::detail::dbdsqr;
using crd::hesap::dense::detail::dlartg;
using crd::hesap::dense::detail::dlas2;
using crd::hesap::dense::detail::dlasr_lv;
using crd::hesap::dense::detail::dlasr_rv;
using crd::hesap::dense::detail::dlasv2;

struct Rng
{
    crd::u64 s;
    double next()
    {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<double>((s >> 11) & 0xFFFFFFFFFFFFFULL) / static_cast<double>(1ULL << 52);
    }
};

// Reference: build the dense plane-rotation product P (z x z) and apply it.
// PIVOT='V' (plane (k,k+1)), R(k) = [c s; -s c].
void build_p(int z, const double* c, const double* s, bool forward, double* p)
{
    for (int i = 0; i < z * z; ++i)
    {
        p[i] = (i / z == i % z) ? 1.0 : 0.0;
    }
    auto mul_left = [&](int k) {  // p := G_k * p, G_k acts on rows k,k+1 (0-based)
        for (int col = 0; col < z; ++col)
        {
            const double a = p[k * z + col];
            const double b = p[(k + 1) * z + col];
            p[k * z + col] = c[k] * a + s[k] * b;
            p[(k + 1) * z + col] = -s[k] * a + c[k] * b;
        }
    };
    if (forward)  // P = G(z-2)...G(0)
    {
        for (int k = 0; k <= z - 2; ++k)
        {
            mul_left(k);
        }
    }
    else  // P = G(0)...G(z-2)
    {
        for (int k = z - 2; k >= 0; --k)
        {
            mul_left(k);
        }
    }
}
} // namespace

TEST_CASE("dlartg: rotation zeroes g, c>=0, c^2+s^2==1", "[hesap][svd][bdsqr]")
{
    Rng rng{0x1234ABCDULL};
    double worst = 0.0;
    for (int t = 0; t < 200; ++t)
    {
        // Mix in the boundary branches (g==0, f==0) plus general values.
        double f = (t % 7 == 0) ? 0.0 : (2.0 * rng.next() - 1.0) * std::pow(10.0, (rng.next() - 0.5) * 8.0);
        double g = (t % 5 == 0) ? 0.0 : (2.0 * rng.next() - 1.0) * std::pow(10.0, (rng.next() - 0.5) * 8.0);
        double c;
        double s;
        double r;
        dlartg(f, g, c, s, r);
        CHECK(c >= -1e-300);  // c >= 0
        worst = std::max(worst, std::abs(c * c + s * s - 1.0));
        worst = std::max(worst, std::abs(c * f + s * g - r));  // [c s] . [f;g] = r
        worst = std::max(worst, std::abs(-s * f + c * g));     // [-s c] . [f;g] = 0
    }
    CHECK(worst < 1e-12);
}

TEST_CASE("dlas2/dlasv2: 2x2 singular values + reconstruction", "[hesap][svd][bdsqr]")
{
    Rng rng{0x99AA55ULL};
    double worst = 0.0;
    for (int t = 0; t < 200; ++t)
    {
        const double f = 2.0 * rng.next() - 1.0;
        const double g = 2.0 * rng.next() - 1.0;
        const double h = 2.0 * rng.next() - 1.0;
        double ssmin2;
        double ssmax2;
        dlas2(f, g, h, ssmin2, ssmax2);
        double ssmin;
        double ssmax;
        double snr;
        double csr;
        double snl;
        double csl;
        dlasv2(f, g, h, ssmin, ssmax, snr, csr, snl, csl);
        // dlas2 magnitudes agree with dlasv2 magnitudes.
        worst = std::max(worst, std::abs(std::abs(ssmax) - ssmax2));
        worst = std::max(worst, std::abs(std::abs(ssmin) - ssmin2));
        // [csl snl; -snl csl] [f g; 0 h] [csr -snr; snr csr] = diag(ssmax, ssmin).
        // Compute M = L * A * R and check off-diagonals ~0, diagonals ssmax/ssmin.
        const double a00 = f;
        const double a01 = g;
        const double a11 = h;
        // L*A:
        const double la00 = csl * a00 + snl * 0.0;
        const double la01 = csl * a01 + snl * a11;
        const double la10 = -snl * a00 + csl * 0.0;
        const double la11 = -snl * a01 + csl * a11;
        // (L*A)*R, R = [csr -snr; snr csr]:
        const double m00 = la00 * csr + la01 * snr;
        const double m01 = la00 * (-snr) + la01 * csr;
        const double m10 = la10 * csr + la11 * snr;
        const double m11 = la10 * (-snr) + la11 * csr;
        worst = std::max(worst, std::abs(m00 - ssmax));
        worst = std::max(worst, std::abs(m11 - ssmin));
        worst = std::max(worst, std::abs(m01));
        worst = std::max(worst, std::abs(m10));
        worst = std::max(worst, std::abs(csl * csl + snl * snl - 1.0));
        worst = std::max(worst, std::abs(csr * csr + snr * snr - 1.0));
    }
    CHECK(worst < 1e-12);
}

TEST_CASE("dlasr: matches dense plane-rotation product (all side/direction)", "[hesap][svd][bdsqr]")
{
    crd::memory::TlsfAllocator alloc(8U * 1024U * 1024U);
    Rng rng{0x5EED01ULL};
    const int m = 6;
    const int n = 5;
    auto fill = [&](double* x, int len) {
        for (int i = 0; i < len; ++i)
        {
            x[i] = 2.0 * rng.next() - 1.0;
        }
    };
    auto rand_cs = [&](double* c, double* s, int z) {
        for (int k = 0; k < z - 1; ++k)
        {
            const double th = (2.0 * rng.next() - 1.0) * 3.14159265358979;
            c[k] = std::cos(th);
            s[k] = std::sin(th);
        }
    };

    for (int side = 0; side < 2; ++side)  // 0 = L, 1 = R
    {
        for (int dir = 0; dir < 2; ++dir)  // 0 = forward, 1 = backward
        {
            const bool forward = (dir == 0);
            const int z = (side == 0) ? m : n;
            crd::containers::Array<double> a(m * n, &alloc);
            crd::containers::Array<double> a2(m * n, &alloc);
            crd::containers::Array<double> c(z, &alloc);
            crd::containers::Array<double> s(z, &alloc);
            crd::containers::Array<double> p(z * z, &alloc);
            fill(a.data(), m * n);
            for (int i = 0; i < m * n; ++i)
            {
                a2.data()[i] = a.data()[i];
            }
            rand_cs(c.data(), s.data(), z);
            build_p(z, c.data(), s.data(), forward, p.data());

            // Reference result.
            crd::containers::Array<double> ref(m * n, &alloc);
            if (side == 0)  // P * A
            {
                for (int i = 0; i < m; ++i)
                {
                    for (int j = 0; j < n; ++j)
                    {
                        double acc = 0.0;
                        for (int k = 0; k < m; ++k)
                        {
                            acc += p.data()[i * m + k] * a.data()[k * n + j];
                        }
                        ref.data()[i * n + j] = acc;
                    }
                }
                dlasr_lv(forward, m, n, c.data(), s.data(), a2.data(), n);
            }
            else  // A * P^T
            {
                for (int i = 0; i < m; ++i)
                {
                    for (int j = 0; j < n; ++j)
                    {
                        double acc = 0.0;
                        for (int k = 0; k < n; ++k)
                        {
                            acc += a.data()[i * n + k] * p.data()[j * n + k];  // (P^T)[k][j]=P[j][k]
                        }
                        ref.data()[i * n + j] = acc;
                    }
                }
                dlasr_rv(forward, m, n, c.data(), s.data(), a2.data(), n);
            }
            double worst = 0.0;
            for (int i = 0; i < m * n; ++i)
            {
                worst = std::max(worst, std::abs(ref.data()[i] - a2.data()[i]));
            }
            CHECK(worst < 1e-12);
        }
    }
}

TEST_CASE("dbdsqr: bidiagonal SVD reconstruction + orthogonality + values", "[hesap][svd][bdsqr]")
{
    crd::memory::TlsfAllocator alloc(32U * 1024U * 1024U);
    for (int n : {3, 8, 16, 25})
    {
        Rng rng{0xB1D5A0ULL + static_cast<crd::u64>(n)};
        crd::containers::Array<double> d(n, &alloc);
        crd::containers::Array<double> e(n, &alloc);
        crd::containers::Array<double> d0(n, &alloc);
        crd::containers::Array<double> e0(n, &alloc);
        for (int i = 0; i < n; ++i)
        {
            d.data()[i] = 2.0 * rng.next() - 1.0 + 1.5;  // keep away from 0 for conditioning
            d0.data()[i] = d.data()[i];
        }
        for (int i = 0; i < n - 1; ++i)
        {
            e.data()[i] = 2.0 * rng.next() - 1.0;
            e0.data()[i] = e.data()[i];
        }
        e.data()[n - 1] = 0.0;
        e0.data()[n - 1] = 0.0;

        // U = I (n x n), VT = I (n x n), RowMajor.
        crd::containers::Array<double> u(n * n, &alloc);
        crd::containers::Array<double> vt(n * n, &alloc);
        for (int i = 0; i < n * n; ++i)
        {
            u.data()[i] = (i / n == i % n) ? 1.0 : 0.0;
            vt.data()[i] = (i / n == i % n) ? 1.0 : 0.0;
        }
        crd::containers::Array<double> work(4 * n, &alloc);
        const int info = dbdsqr<double>(true, n, n, n, 0, d.data(), e.data(), vt.data(), n, u.data(), n, nullptr,
                                        1, work.data());
        CHECK(info == 0);

        // Singular values descending + non-negative.
        bool desc = true;
        for (int i = 0; i < n; ++i)
        {
            if (d.data()[i] < -1e-14)
            {
                desc = false;
            }
            if (i > 0 && d.data()[i] > d.data()[i - 1] + 1e-12)
            {
                desc = false;
            }
        }
        CHECK(desc);

        // Reconstruct B = U * diag(s) * VT and compare to the original bidiagonal.
        double rec = 0.0;
        for (int i = 0; i < n; ++i)
        {
            for (int j = 0; j < n; ++j)
            {
                double acc = 0.0;
                for (int k = 0; k < n; ++k)
                {
                    acc += u.data()[i * n + k] * d.data()[k] * vt.data()[k * n + j];
                }
                double bij = 0.0;
                if (i == j)
                {
                    bij = d0.data()[i];
                }
                else if (j == i + 1)
                {
                    bij = e0.data()[i];
                }
                rec = std::max(rec, std::abs(acc - bij));
            }
        }
        CHECK(rec < 1e-12);

        // Orthogonality: U^T U = I (columns), VT VT^T = I (rows).
        double orthu = 0.0;
        double orthv = 0.0;
        for (int a = 0; a < n; ++a)
        {
            for (int b = 0; b < n; ++b)
            {
                double du = 0.0;
                double dv = 0.0;
                for (int k = 0; k < n; ++k)
                {
                    du += u.data()[k * n + a] * u.data()[k * n + b];
                    dv += vt.data()[a * n + k] * vt.data()[b * n + k];
                }
                const double exp = (a == b) ? 1.0 : 0.0;
                orthu = std::max(orthu, std::abs(du - exp));
                orthv = std::max(orthv, std::abs(dv - exp));
            }
        }
        CHECK(orthu < 1e-12);
        CHECK(orthv < 1e-12);

        // Values oracle: singular values of B == sqrt(eigenvalues of B^T B).
        crd::hesap::dense::Symmetric<double> btb(&alloc, static_cast<crd::usize>(n));
        for (int i = 0; i < n; ++i)
        {
            for (int j = 0; j <= i; ++j)
            {
                // (B^T B)[i][j] = sum_k B[k][i] B[k][j]; B upper-bidiagonal.
                double acc = 0.0;
                for (int k = 0; k < n; ++k)
                {
                    double bki = 0.0;
                    if (k == i)
                    {
                        bki = d0.data()[i];
                    }
                    else if (i == k + 1)
                    {
                        bki = e0.data()[k];
                    }
                    double bkj = 0.0;
                    if (k == j)
                    {
                        bkj = d0.data()[j];
                    }
                    else if (j == k + 1)
                    {
                        bkj = e0.data()[k];
                    }
                    acc += bki * bkj;
                }
                btb.at(static_cast<crd::usize>(i), static_cast<crd::usize>(j)) = acc;
            }
        }
        const auto eig = crd::hesap::dense::eig_sym<double>(&alloc, btb);
        crd::containers::Array<double> svref(n, &alloc);
        for (int i = 0; i < n; ++i)
        {
            const double ev = eig.values.data()[i];
            svref.data()[i] = std::sqrt(ev > 0.0 ? ev : 0.0);
        }
        std::sort(svref.data(), svref.data() + n, [](double x, double y) { return x > y; });
        double vworst = 0.0;
        for (int i = 0; i < n; ++i)
        {
            vworst = std::max(vworst, std::abs(svref.data()[i] - d.data()[i]));
        }
        CHECK(vworst < 1e-10);
    }
}

namespace
{
// Run the full svd() and return worst-case reconstruction / orthogonality /
// descending errors for a random m x n matrix.
void check_svd(int m, int n, double& rec, double& orthu, double& orthv, bool& desc)
{
    crd::memory::TlsfAllocator alloc(64U * 1024U * 1024U);
    crd::hesap::dense::Matrix<double> a(&alloc, static_cast<crd::usize>(m), static_cast<crd::usize>(n));
    Rng rng{0x5D0011ULL + static_cast<crd::u64>(m * 257 + n)};
    for (int i = 0; i < m; ++i)
    {
        for (int j = 0; j < n; ++j)
        {
            a.at(static_cast<crd::usize>(i), static_cast<crd::usize>(j)) = 2.0 * rng.next() - 1.0;
        }
    }
    const auto s = crd::hesap::dense::svd<double>(&alloc, a);
    const int k = std::min(m, n);

    // Reconstruction A == U diag(S) V^T.
    rec = 0.0;
    for (int i = 0; i < m; ++i)
    {
        for (int j = 0; j < n; ++j)
        {
            double acc = 0.0;
            for (int t = 0; t < k; ++t)
            {
                acc += s.u.at(static_cast<crd::usize>(i), static_cast<crd::usize>(t)) * s.s.data()[t] *
                       s.v.at(static_cast<crd::usize>(j), static_cast<crd::usize>(t));
            }
            rec = std::max(rec, std::abs(acc - a.at(static_cast<crd::usize>(i), static_cast<crd::usize>(j))));
        }
    }

    // Orthonormal columns of U (k columns) and V (k columns).
    orthu = 0.0;
    orthv = 0.0;
    for (int p = 0; p < k; ++p)
    {
        for (int q = 0; q < k; ++q)
        {
            double du = 0.0;
            double dv = 0.0;
            for (int i = 0; i < m; ++i)
            {
                du += s.u.at(static_cast<crd::usize>(i), static_cast<crd::usize>(p)) *
                      s.u.at(static_cast<crd::usize>(i), static_cast<crd::usize>(q));
            }
            for (int i = 0; i < n; ++i)
            {
                dv += s.v.at(static_cast<crd::usize>(i), static_cast<crd::usize>(p)) *
                      s.v.at(static_cast<crd::usize>(i), static_cast<crd::usize>(q));
            }
            const double exp = (p == q) ? 1.0 : 0.0;
            orthu = std::max(orthu, std::abs(du - exp));
            orthv = std::max(orthv, std::abs(dv - exp));
        }
    }

    desc = true;
    for (int i = 0; i < k; ++i)
    {
        if (s.s.data()[i] < -1e-14)
        {
            desc = false;
        }
        if (i > 0 && s.s.data()[i] > s.s.data()[i - 1] + 1e-12)
        {
            desc = false;
        }
    }
}
} // namespace

TEST_CASE("svd: A == U S V^T reconstruction + orthogonality (m=n, m>n, m<n)", "[hesap][svd]")
{
    for (auto mn : {std::pair<int, int>{8, 8}, {12, 7}, {5, 11}, {20, 20}, {3, 5}})
    {
        double rec;
        double orthu;
        double orthv;
        bool desc;
        check_svd(mn.first, mn.second, rec, orthu, orthv, desc);
        CHECK(rec < 1e-11);
        CHECK(orthu < 1e-11);
        CHECK(orthv < 1e-11);
        CHECK(desc);
    }
}

TEST_CASE("svd: blocked-path reconstruction + orthogonality (n > block size)", "[hesap][svd]")
{
    // m=n=80 and m>n with n=72 hit the blocked bidiagonalization directly; the
    // m<n case (70,100) transposes to a working n=100 > 64 (also blocked).
    for (auto mn : {std::pair<int, int>{80, 80}, {100, 72}, {70, 100}, {128, 96}})
    {
        double rec;
        double orthu;
        double orthv;
        bool desc;
        check_svd(mn.first, mn.second, rec, orthu, orthv, desc);
        CHECK(rec < 1e-10);
        CHECK(orthu < 1e-10);
        CHECK(orthv < 1e-10);
        CHECK(desc);
    }
}

// v3b-2.1: dlasd5 — the 2x2 SVD secular base. Each computed sigma_i must satisfy
// f(sigma)=1+rho*sum z_j^2/(d_j^2-sigma^2)=0 (evaluated cancellation-free as
// delta_j*work_j = d_j^2-sigma^2) and interlace the poles.
TEST_CASE("dlasd5: 2x2 secular roots satisfy f(sigma)=0 + interlacing", "[hesap][svd][dc]")
{
    using crd::hesap::dense::detail::dlasd5;
    Rng rng{0x5D5EEDULL};
    double worst_resid = 0.0;
    for (int t = 0; t < 400; ++t)
    {
        const double d0 = rng.next() * 2.0;                  // >= 0
        const double d1 = d0 + 0.05 + rng.next() * 3.0;      // > d0
        const double d[2] = {d0, d1};
        double z0 = 2.0 * rng.next() - 1.0;
        double z1 = 2.0 * rng.next() - 1.0;
        const double nz = std::sqrt(z0 * z0 + z1 * z1);
        if (nz < 1e-12)
        {
            continue;
        }
        z0 /= nz;
        z1 /= nz;  // ||z|| = 1 (dlasd5 precondition)
        const double z[2] = {z0, z1};
        const double rho = 0.01 + rng.next() * 5.0;  // > 0
        for (int i = 0; i < 2; ++i)
        {
            double delta[2];
            double work[2];
            double sigma = 0.0;
            dlasd5<double>(i, d, z, delta, rho, sigma, work);
            // Secular residual via the cancellation-free factorization.
            double f = 1.0;
            for (int j = 0; j < 2; ++j)
            {
                f += rho * z[j] * z[j] / (delta[j] * work[j]);
            }
            worst_resid = std::max(worst_resid, std::abs(f));
            // Interlacing: d0 < sigma_0 < d1 < sigma_1 < sqrt(d1^2 + rho).
            if (i == 0)
            {
                CHECK(sigma > d0 - 1e-12);
                CHECK(sigma < d1 + 1e-12);
            }
            else
            {
                CHECK(sigma > d1 - 1e-12);
                CHECK(sigma < std::sqrt(d1 * d1 + rho) + 1e-9);
            }
            // delta/work carry the gaps (d_j -/+ sigma).
            CHECK(std::abs(delta[0] - (d0 - sigma)) < 1e-9 * (1.0 + std::abs(sigma)));
            CHECK(std::abs(work[1] - (d1 + sigma)) < 1e-9 * (1.0 + std::abs(sigma)));
        }
    }
    CHECK(worst_resid < 1e-9);
}

// v3b-2.3: dlasdq_upper base-case SVD — reconstruct an n x (n+sqre) upper
// bidiagonal (validates the row-major-dbdsqr -> column-major layout bridge).
TEST_CASE("dlasdq_upper: base-case bidiagonal SVD reconstruction", "[hesap][svd][dc]")
{
    using crd::hesap::dense::detail::dlasdq_upper;
    crd::memory::TlsfAllocator alloc(16U * 1024U * 1024U);
    double worst_rec = 0.0;
    double worst_orth = 0.0;
    for (int sqre = 0; sqre <= 1; ++sqre)
    {
        for (int n : {2, 3, 5, 8, 12})
        {
            const int m = n + sqre;
            const int ne = (sqre == 1) ? n : (n - 1);
            crd::containers::Array<double> d(n, &alloc);
            crd::containers::Array<double> e(n, &alloc);
            crd::containers::Array<double> d0(n, &alloc);
            crd::containers::Array<double> e0(n, &alloc);
            crd::containers::Array<double> u(n * n, &alloc);
            crd::containers::Array<double> vt(m * m, &alloc);
            Rng rng{0xDA5D9ULL + static_cast<crd::u64>(sqre * 97 + n)};
            for (int i = 0; i < n; ++i)
            {
                d.data()[i] = d0.data()[i] = 1.0 + rng.next() * 2.0;
                e.data()[i] = e0.data()[i] = 0.0;
            }
            for (int i = 0; i < ne; ++i)
            {
                e.data()[i] = e0.data()[i] = 2.0 * rng.next() - 1.0;
            }
            const int info = dlasdq_upper<double>(n, sqre, d.data(), e.data(), u.data(), n, vt.data(), m, &alloc);
            CHECK(info == 0);
            // Reconstruct B[i][j] = sum_k U(i,k) * S(k) * VT(k,j) (column-major).
            for (int i = 0; i < n; ++i)
            {
                for (int j = 0; j < m; ++j)
                {
                    double acc = 0.0;
                    for (int kk = 0; kk < n; ++kk)
                    {
                        acc += u.data()[kk * n + i] * d.data()[kk] * vt.data()[j * m + kk];
                    }
                    double bij = 0.0;
                    if (j == i)
                    {
                        bij = d0.data()[i];
                    }
                    else if (j == i + 1 && i < ne)
                    {
                        bij = e0.data()[i];
                    }
                    worst_rec = std::max(worst_rec, std::abs(acc - bij));
                }
            }
            // U columns orthonormal; VT rows 1..n orthonormal.
            for (int p = 0; p < n; ++p)
            {
                for (int q = 0; q < n; ++q)
                {
                    double du = 0.0;
                    double dv = 0.0;
                    for (int i = 0; i < n; ++i)
                    {
                        du += u.data()[p * n + i] * u.data()[q * n + i];
                    }
                    for (int jj = 0; jj < m; ++jj)
                    {
                        dv += vt.data()[jj * m + p] * vt.data()[jj * m + q];
                    }
                    const double ex = (p == q) ? 1.0 : 0.0;
                    worst_orth = std::max(worst_orth, std::abs(du - ex));
                    worst_orth = std::max(worst_orth, std::abs(dv - ex));
                }
            }
        }
    }
    CHECK(worst_rec < 1e-11);
    CHECK(worst_orth < 1e-11);
}

// v3b-1c: complex Householder (zlarfg) — H^H x = beta*e0 (beta real), H unitary.
TEST_CASE("make_complex_householder: reflector zeroes the tail + unitary", "[hesap][svd][cplx]")
{
    using C = crd::hesap::Complex<double>;
    using crd::hesap::dense::detail::make_complex_householder;
    crd::memory::TlsfAllocator alloc(2U * 1024U * 1024U);
    Rng rng{0xC9114ULL};
    double worst = 0.0;
    for (int trial = 0; trial < 200; ++trial)
    {
        const int n = 2 + (trial % 7);
        crd::containers::Array<C> x(static_cast<crd::usize>(n), &alloc);
        crd::containers::Array<C> xo(static_cast<crd::usize>(n), &alloc);
        for (int i = 0; i < n; ++i)
        {
            x.data()[i] = xo.data()[i] = C{2.0 * rng.next() - 1.0, 2.0 * rng.next() - 1.0};
        }
        const auto h = make_complex_householder<C>(x.data(), static_cast<crd::usize>(n));
        // v[0] = 1, v[i>=1] = x[i] (modified tail). Apply H^H = I - conj(tau) v v^H to xo.
        C s = xo.data()[0];  // v^H xo = conj(1)*xo[0] + sum conj(v[i]) xo[i]
        for (int i = 1; i < n; ++i)
        {
            s += crd::hesap::conj(x.data()[i]) * xo.data()[i];
        }
        const C ctau = crd::hesap::conj(h.tau);
        const C r0 = xo.data()[0] - ctau * s;  // result[0], should = beta (real)
        worst = std::max(worst, crd::hesap::abs(r0 - C{h.beta, 0.0}));
        for (int i = 1; i < n; ++i)
        {
            const C ri = xo.data()[i] - ctau * s * x.data()[i];  // should be 0
            worst = std::max(worst, crd::hesap::abs(ri));
        }
    }
    CHECK(worst < 1e-12);
}

// v3b-1c: complex bidiagonalization — A = Q B P^H with B REAL bidiagonal,
// Q (m x n) / P (n x n) unitary. Validates bidiagonalize_complex + form_q/p.
TEST_CASE("bidiagonalize_complex: A = Q B P^H reconstruction + unitary", "[hesap][svd][cplx]")
{
    using C = crd::hesap::Complex<double>;
    using crd::hesap::dense::detail::bidiagonalize_complex;
    using crd::hesap::dense::detail::form_p_complex;
    using crd::hesap::dense::detail::form_q_complex;
    crd::memory::TlsfAllocator alloc(16U * 1024U * 1024U);
    double worst_rec = 0.0;
    double worst_orth = 0.0;
    for (auto mn : {std::pair<int, int>{8, 8}, {12, 8}, {16, 16}, {10, 7}, {20, 20}})
    {
        const int m = mn.first;
        const int n = mn.second;
        crd::containers::Array<C> a(static_cast<crd::usize>(m * n), &alloc);
        crd::containers::Array<C> a0(static_cast<crd::usize>(m * n), &alloc);
        Rng rng{0xC0119ULL + static_cast<crd::u64>(m * 53 + n)};
        for (int i = 0; i < m * n; ++i)
        {
            a.data()[i] = a0.data()[i] = C{2.0 * rng.next() - 1.0, 2.0 * rng.next() - 1.0};
        }
        crd::containers::Array<double> d(static_cast<crd::usize>(n), &alloc);
        crd::containers::Array<double> e(static_cast<crd::usize>(n), &alloc);
        crd::containers::Array<C> tauq(static_cast<crd::usize>(n), &alloc);
        crd::containers::Array<C> taup(static_cast<crd::usize>(n), &alloc);
        bidiagonalize_complex<C>(a.data(), static_cast<crd::usize>(m), static_cast<crd::usize>(n),
                                 static_cast<crd::usize>(n), d.data(), e.data(), tauq.data(), taup.data(),
                                 &alloc);
        crd::containers::Array<C> q(static_cast<crd::usize>(m * n), &alloc);
        crd::containers::Array<C> p(static_cast<crd::usize>(n * n), &alloc);
        form_q_complex<C>(a.data(), static_cast<crd::usize>(m), static_cast<crd::usize>(n),
                          static_cast<crd::usize>(n), tauq.data(), q.data());
        form_p_complex<C>(a.data(), static_cast<crd::usize>(n), static_cast<crd::usize>(n), taup.data(),
                          p.data());
        // R = Q B P^H. (QB)[i][j] = Q[i][j] d[j] + (j>=1 ? Q[i][j-1] e[j-1] : 0).
        for (int i = 0; i < m; ++i)
        {
            for (int l = 0; l < n; ++l)
            {
                C r{0.0, 0.0};
                for (int j = 0; j < n; ++j)
                {
                    C qb = q.data()[i * n + j] * d.data()[j];
                    if (j >= 1)
                    {
                        qb += q.data()[i * n + (j - 1)] * e.data()[j - 1];
                    }
                    r += qb * crd::hesap::conj(p.data()[l * n + j]);  // (P^H)[j][l] = conj(P[l][j])
                }
                worst_rec = std::max(worst_rec, crd::hesap::abs(r - a0.data()[i * n + l]));
            }
        }
        // Q^H Q = I_n ; P^H P = I_n.
        for (int pp = 0; pp < n; ++pp)
        {
            for (int qq = 0; qq < n; ++qq)
            {
                C dq{0.0, 0.0};
                C dp{0.0, 0.0};
                for (int i = 0; i < m; ++i)
                {
                    dq += crd::hesap::conj(q.data()[i * n + pp]) * q.data()[i * n + qq];
                }
                for (int i = 0; i < n; ++i)
                {
                    dp += crd::hesap::conj(p.data()[i * n + pp]) * p.data()[i * n + qq];
                }
                const C ex{(pp == qq) ? 1.0 : 0.0, 0.0};
                worst_orth = std::max(worst_orth, crd::hesap::abs(dq - ex));
                worst_orth = std::max(worst_orth, crd::hesap::abs(dp - ex));
            }
        }
    }
    CHECK(worst_rec < 1e-11);
    CHECK(worst_orth < 1e-11);
}

// v3b-1c: full complex SVD A = U S V^H. Reconstruction + U/V unitary + S
// descending >= 0, across m>=n / m<n / large (D&C bidiagonal path, n>=64).
TEST_CASE("svd complex: A = U S V^H reconstruction + unitary", "[hesap][svd][cplx]")
{
    using C = crd::hesap::Complex<double>;
    using crd::hesap::dense::svd;
    crd::memory::TlsfAllocator alloc(256U * 1024U * 1024U);
    double worst_rec = 0.0;
    double worst_orth = 0.0;
    bool desc_ok = true;
    for (auto mn : {std::pair<int, int>{8, 8}, {12, 7}, {7, 12}, {20, 20}, {80, 80}, {70, 100}})
    {
        const int m = mn.first;
        const int n = mn.second;
        const int k = m < n ? m : n;
        crd::hesap::dense::Matrix<C> a(&alloc, static_cast<crd::usize>(m), static_cast<crd::usize>(n));
        Rng rng{0x5CC119ULL + static_cast<crd::u64>(m * 131 + n)};
        for (int i = 0; i < m; ++i)
        {
            for (int j = 0; j < n; ++j)
            {
                a.at(static_cast<crd::usize>(i), static_cast<crd::usize>(j)) =
                    C{2.0 * rng.next() - 1.0, 2.0 * rng.next() - 1.0};
            }
        }
        const auto s = svd<C>(&alloc, a);
        REQUIRE(static_cast<int>(s.s.size()) == k);
        // A[i][j] = sum_t U(i,t) S(t) conj(V(j,t)).
        for (int i = 0; i < m; ++i)
        {
            for (int j = 0; j < n; ++j)
            {
                C acc{0.0, 0.0};
                for (int t = 0; t < k; ++t)
                {
                    acc += s.u.at(static_cast<crd::usize>(i), static_cast<crd::usize>(t)) * s.s.data()[t] *
                           crd::hesap::conj(s.v.at(static_cast<crd::usize>(j), static_cast<crd::usize>(t)));
                }
                worst_rec = std::max(worst_rec,
                                     crd::hesap::abs(acc - a.at(static_cast<crd::usize>(i),
                                                               static_cast<crd::usize>(j))));
            }
        }
        for (int p = 0; p < k; ++p)
        {
            for (int q = 0; q < k; ++q)
            {
                C du{0.0, 0.0};
                C dv{0.0, 0.0};
                for (int i = 0; i < m; ++i)
                {
                    du += crd::hesap::conj(s.u.at(static_cast<crd::usize>(i), static_cast<crd::usize>(p))) *
                          s.u.at(static_cast<crd::usize>(i), static_cast<crd::usize>(q));
                }
                for (int i = 0; i < n; ++i)
                {
                    dv += crd::hesap::conj(s.v.at(static_cast<crd::usize>(i), static_cast<crd::usize>(p))) *
                          s.v.at(static_cast<crd::usize>(i), static_cast<crd::usize>(q));
                }
                const C ex{(p == q) ? 1.0 : 0.0, 0.0};
                worst_orth = std::max(worst_orth, crd::hesap::abs(du - ex));
                worst_orth = std::max(worst_orth, crd::hesap::abs(dv - ex));
            }
        }
        for (int t = 0; t < k; ++t)
        {
            if (s.s.data()[t] < -1e-12)
            {
                desc_ok = false;
            }
            if (t > 0 && s.s.data()[t] > s.s.data()[t - 1] + 1e-9)
            {
                desc_ok = false;
            }
        }
    }
    CHECK(worst_rec < 1e-9);
    CHECK(worst_orth < 1e-9);
    CHECK(desc_ok);
}

// v3b-3: randomized SVD — recover an EXACT rank-r matrix A = X Y^T. With
// oversampling, the range finder captures range(A) exactly, so reconstruction
// ‖A − U S Vᵀ‖ should hit ~machine precision; U/V columns orthonormal; s
// non-negative descending.
TEST_CASE("rsvd: exact low-rank reconstruction + orthonormality", "[hesap][svd][rsvd]")
{
    using crd::hesap::dense::rsvd;
    crd::memory::TlsfAllocator alloc(128U * 1024U * 1024U);
    double worst_rec = 0.0;
    double worst_orth = 0.0;
    for (auto mnr : {std::tuple<int, int, int>{40, 30, 5}, {60, 60, 8}, {100, 50, 10}, {50, 80, 6}})
    {
        const int m = std::get<0>(mnr);
        const int n = std::get<1>(mnr);
        const int r = std::get<2>(mnr);
        crd::containers::Array<double> x(m * r, &alloc);
        crd::containers::Array<double> yv(n * r, &alloc);
        Rng rng{0x12500ULL + static_cast<crd::u64>(m * 31 + n)};
        for (int i = 0; i < m * r; ++i)
        {
            x.data()[i] = 2.0 * rng.next() - 1.0;
        }
        for (int i = 0; i < n * r; ++i)
        {
            yv.data()[i] = 2.0 * rng.next() - 1.0;
        }
        crd::hesap::dense::Matrix<double> a(&alloc, static_cast<crd::usize>(m), static_cast<crd::usize>(n));
        for (int i = 0; i < m; ++i)
        {
            for (int j = 0; j < n; ++j)
            {
                double acc = 0.0;
                for (int kk = 0; kk < r; ++kk)
                {
                    acc += x.data()[i * r + kk] * yv.data()[j * r + kk];
                }
                a.at(static_cast<crd::usize>(i), static_cast<crd::usize>(j)) = acc;
            }
        }
        const auto s = rsvd<double>(&alloc, a, static_cast<crd::usize>(r), 8, 2);
        REQUIRE(static_cast<int>(s.s.size()) == r);
        for (int i = 0; i < m; ++i)
        {
            for (int j = 0; j < n; ++j)
            {
                double acc = 0.0;
                for (int kk = 0; kk < r; ++kk)
                {
                    acc += s.u.at(static_cast<crd::usize>(i), static_cast<crd::usize>(kk)) * s.s.data()[kk] *
                           s.v.at(static_cast<crd::usize>(j), static_cast<crd::usize>(kk));
                }
                worst_rec = std::max(worst_rec, std::abs(acc - a.at(static_cast<crd::usize>(i),
                                                                     static_cast<crd::usize>(j))));
            }
        }
        bool desc = true;
        for (int kk = 0; kk < r; ++kk)
        {
            if (s.s.data()[kk] < -1e-12)
            {
                desc = false;
            }
            if (kk > 0 && s.s.data()[kk] > s.s.data()[kk - 1] + 1e-9)
            {
                desc = false;
            }
        }
        CHECK(desc);
        for (int p = 0; p < r; ++p)
        {
            for (int q = 0; q < r; ++q)
            {
                double du = 0.0;
                double dv = 0.0;
                for (int i = 0; i < m; ++i)
                {
                    du += s.u.at(static_cast<crd::usize>(i), static_cast<crd::usize>(p)) *
                          s.u.at(static_cast<crd::usize>(i), static_cast<crd::usize>(q));
                }
                for (int i = 0; i < n; ++i)
                {
                    dv += s.v.at(static_cast<crd::usize>(i), static_cast<crd::usize>(p)) *
                          s.v.at(static_cast<crd::usize>(i), static_cast<crd::usize>(q));
                }
                const double ex = (p == q) ? 1.0 : 0.0;
                worst_orth = std::max(worst_orth, std::abs(du - ex));
                worst_orth = std::max(worst_orth, std::abs(dv - ex));
            }
        }
    }
    CHECK(worst_rec < 1e-8);
    CHECK(worst_orth < 1e-9);
}

// v3b-3: randomized symmetric eig (rsyev) — recover an EXACT rank-r PSD
// A = X Xᵀ. Top-r eigenpairs: residual ‖A v − λ v‖ ~ machine, λ ≥ 0 descending.
TEST_CASE("rsyev: low-rank symmetric eig recovery", "[hesap][svd][rsvd]")
{
    using crd::hesap::dense::rsyev;
    crd::memory::TlsfAllocator alloc(64U * 1024U * 1024U);
    double worst_resid = 0.0;
    for (auto nr : {std::pair<int, int>{40, 5}, {60, 8}, {80, 6}})
    {
        const int n = nr.first;
        const int r = nr.second;
        crd::containers::Array<double> xb(n * r, &alloc);
        Rng rng{0x59E50ULL + static_cast<crd::u64>(n)};
        for (int i = 0; i < n * r; ++i)
        {
            xb.data()[i] = 2.0 * rng.next() - 1.0;
        }
        crd::hesap::dense::Symmetric<double> a(&alloc, static_cast<crd::usize>(n));
        for (int i = 0; i < n; ++i)
        {
            for (int j = 0; j <= i; ++j)
            {
                double acc = 0.0;
                for (int kk = 0; kk < r; ++kk)
                {
                    acc += xb.data()[i * r + kk] * xb.data()[j * r + kk];
                }
                a.at(static_cast<crd::usize>(i), static_cast<crd::usize>(j)) = acc;
            }
        }
        const auto e = rsyev<double>(&alloc, a, static_cast<crd::usize>(r), 8, 2);
        REQUIRE(static_cast<int>(e.values.size()) == r);
        bool desc = true;
        for (int kk = 0; kk < r; ++kk)
        {
            if (e.values.data()[kk] < -1e-9)
            {
                desc = false;  // PSD => non-negative
            }
            if (kk > 0 && e.values.data()[kk] > e.values.data()[kk - 1] + 1e-7)
            {
                desc = false;  // descending
            }
        }
        CHECK(desc);
        // Residual ‖A v_k − λ_k v_k‖_inf.
        auto aij = [&](int i, int j) {
            return (i >= j) ? a.at(static_cast<crd::usize>(i), static_cast<crd::usize>(j))
                            : a.at(static_cast<crd::usize>(j), static_cast<crd::usize>(i));
        };
        for (int kk = 0; kk < r; ++kk)
        {
            const double lam = e.values.data()[kk];
            for (int i = 0; i < n; ++i)
            {
                double av = 0.0;
                for (int j = 0; j < n; ++j)
                {
                    av += aij(i, j) * e.vectors.at(static_cast<crd::usize>(j), static_cast<crd::usize>(kk));
                }
                const double vv = e.vectors.at(static_cast<crd::usize>(i), static_cast<crd::usize>(kk));
                worst_resid = std::max(worst_resid, std::abs(av - lam * vv));
            }
        }
    }
    CHECK(worst_resid < 1e-7);
}

// v3b-2.3 + GATE for v3b-2.2: full divide-and-conquer SVD of a random n x n
// upper bidiagonal via dlasd0 (smlsiz=4 forces multi-level recursion + merges),
// reconstruct B = U S VT. This validates dlasd0 + dlasd1/dlasd2/dlasd3 + dlasdq
// end-to-end (the merge's first rigorous gate).
TEST_CASE("dlasd0: full D&C bidiagonal SVD reconstruction", "[hesap][svd][dc]")
{
    using crd::hesap::dense::detail::dlasd0;
    crd::memory::TlsfAllocator alloc(128U * 1024U * 1024U);
    const int smlsiz = 4;
    double worst_rec = 0.0;
    double worst_orth = 0.0;
    for (int n : {6, 9, 13, 20, 33, 50})
    {
        const int m = n;  // sqre = 0 (square bidiagonal)
        crd::containers::Array<double> d(n, &alloc);
        crd::containers::Array<double> e(n, &alloc);
        crd::containers::Array<double> d0(n, &alloc);
        crd::containers::Array<double> e0(n, &alloc);
        crd::containers::Array<double> u(n * n, &alloc);
        crd::containers::Array<double> vt(m * m, &alloc);
        Rng rng{0xD45D0ULL + static_cast<crd::u64>(n)};
        for (int i = 0; i < n; ++i)
        {
            d.data()[i] = d0.data()[i] = 1.0 + rng.next() * 2.0;
            e.data()[i] = e0.data()[i] = 0.0;
        }
        for (int i = 0; i < n - 1; ++i)
        {
            e.data()[i] = e0.data()[i] = 2.0 * rng.next() - 1.0;
        }
        for (int i = 0; i < n * n; ++i)
        {
            u.data()[i] = 0.0;
        }
        for (int i = 0; i < m * m; ++i)
        {
            vt.data()[i] = 0.0;
        }
        for (int i = 0; i < n; ++i)
        {
            u.data()[i * n + i] = 1.0;  // U = I (column-major)
        }
        for (int i = 0; i < m; ++i)
        {
            vt.data()[i * m + i] = 1.0;  // VT = I
        }
        const int info = dlasd0<double>(n, 0, d.data(), e.data(), u.data(), n, vt.data(), m, smlsiz, &alloc);
        CHECK(info == 0);
        for (int i = 0; i < n; ++i)
        {
            for (int j = 0; j < m; ++j)
            {
                double acc = 0.0;
                for (int kk = 0; kk < n; ++kk)
                {
                    acc += u.data()[kk * n + i] * d.data()[kk] * vt.data()[j * m + kk];
                }
                double bij = 0.0;
                if (j == i)
                {
                    bij = d0.data()[i];
                }
                else if (j == i + 1)
                {
                    bij = e0.data()[i];
                }
                worst_rec = std::max(worst_rec, std::abs(acc - bij));
            }
        }
        for (int p = 0; p < n; ++p)
        {
            for (int q = 0; q < n; ++q)
            {
                double du = 0.0;
                double dv = 0.0;
                for (int i = 0; i < n; ++i)
                {
                    du += u.data()[p * n + i] * u.data()[q * n + i];
                    dv += vt.data()[p * m + i] * vt.data()[q * m + i];
                }
                const double ex = (p == q) ? 1.0 : 0.0;
                worst_orth = std::max(worst_orth, std::abs(du - ex));
                worst_orth = std::max(worst_orth, std::abs(dv - ex));
            }
        }
    }
    CHECK(worst_rec < 1e-9);
    CHECK(worst_orth < 1e-9);
}

// v3b-2.2: dlasd1/dlasd2/dlasd3 merge SMOKE (compile + run + basic sanity). The
// RIGOROUS gate is bidiagonal reconstruction via the dbdsdc driver (v3b-2.3) —
// a standalone merge input can't be hand-constructed without testing the
// construction more than the merge.
TEST_CASE("dlasd1: merge runs + non-negative singular values (smoke)", "[hesap][svd][dc]")
{
    using crd::hesap::dense::detail::dlasd1;
    crd::memory::TlsfAllocator alloc(8U * 1024U * 1024U);
    const int nl = 1;
    const int nr = 1;
    const int sqre = 0;
    const int n = 3;
    const int m = 3;
    crd::containers::Array<double> d(n, &alloc);
    crd::containers::Array<double> u(n * n, &alloc);
    crd::containers::Array<double> vt(m * m, &alloc);
    crd::containers::Array<int> idxq(n, &alloc);
    d.data()[0] = 2.0;
    d.data()[1] = 0.0;  // placeholder (dlasd1 zeroes it)
    d.data()[2] = 3.0;
    for (int i = 0; i < n * n; ++i)
    {
        u.data()[i] = 0.0;
    }
    for (int i = 0; i < n; ++i)
    {
        u.data()[i * n + i] = 1.0;  // U = I (column-major)
    }
    for (int i = 0; i < m * m; ++i)
    {
        vt.data()[i] = 0.0;
    }
    const double cc = 0.8;
    const double ss = 0.6;
    vt.data()[0 * m + 0] = cc;   // VT(1,1)
    vt.data()[1 * m + 0] = ss;   // VT(1,2)
    vt.data()[0 * m + 1] = -ss;  // VT(2,1)
    vt.data()[1 * m + 1] = cc;   // VT(2,2)
    vt.data()[2 * m + 2] = 1.0;  // VT(3,3)
    idxq.data()[0] = 1;
    idxq.data()[1] = 1;
    idxq.data()[2] = 1;
    double alpha = 1.5;
    double beta = 0.7;
    const int info = dlasd1<double>(nl, nr, sqre, d.data(), alpha, beta, u.data(), n, vt.data(), m, idxq.data(),
                                    &alloc);
    CHECK(info == 0);
    for (int i = 0; i < n; ++i)
    {
        CHECK(std::isfinite(d.data()[i]));
        CHECK(d.data()[i] >= -1e-12);
    }
}

// v3b-2.1: dlasd4 — the general SVD secular root solver. Each sigma_i must
// satisfy f(sigma)=1+rho*sum z_j^2/(d_j^2-sigma^2)=0 (cancellation-free via
// delta_j*work_j = d_j^2-sigma^2) and interlace the poles. Sizes span
// well-separated and tightly-clustered spectra (the SWTCH3/dlaed6 stress).
TEST_CASE("dlasd4: secular roots satisfy f(sigma)=0 + interlacing", "[hesap][svd][dc]")
{
    using crd::hesap::dense::detail::dlasd4;
    crd::memory::TlsfAllocator alloc(8U * 1024U * 1024U);
    double worst = 0.0;
    for (int n : {3, 4, 5, 8, 16, 32})
    {
        Rng rng{0xD45D4ULL + static_cast<crd::u64>(n)};
        for (int trial = 0; trial < 12; ++trial)
        {
            crd::containers::Array<double> d(n, &alloc);
            crd::containers::Array<double> z(n, &alloc);
            crd::containers::Array<double> delta(n, &alloc);
            crd::containers::Array<double> work(n, &alloc);
            // Ascending poles 0 <= d0 < d1 < ... ; mix wide + tight gaps.
            double acc = rng.next() * 0.5;
            for (int j = 0; j < n; ++j)
            {
                const double gap = (trial % 3 == 0) ? (1e-3 + rng.next() * 0.01)   // tight cluster
                                                     : (0.1 + rng.next() * 2.0);   // well separated
                acc += gap;
                d.data()[j] = acc;
            }
            double nrm = 0.0;
            for (int j = 0; j < n; ++j)
            {
                z.data()[j] = 2.0 * rng.next() - 1.0;
                nrm += z.data()[j] * z.data()[j];
            }
            nrm = std::sqrt(nrm);
            if (nrm < 1e-12)
            {
                continue;
            }
            for (int j = 0; j < n; ++j)
            {
                z.data()[j] /= nrm;  // ||z|| = 1
            }
            const double rho = 0.05 + rng.next() * 4.0;
            for (int i = 0; i < n; ++i)
            {
                int info = -1;
                double sigma = 0.0;
                dlasd4<double>(n, i, d.data(), z.data(), delta.data(), rho, sigma, work.data(), info);
                CHECK(info == 0);
                double f = 1.0;
                for (int j = 0; j < n; ++j)
                {
                    f += rho * z.data()[j] * z.data()[j] / (delta.data()[j] * work.data()[j]);
                }
                worst = std::max(worst, std::abs(f));
                // Interlacing.
                if (i < n - 1)
                {
                    CHECK(sigma > d.data()[i] - 1e-9);
                    CHECK(sigma < d.data()[i + 1] + 1e-9);
                }
                else
                {
                    CHECK(sigma > d.data()[n - 1] - 1e-9);
                    CHECK(sigma < std::sqrt(d.data()[n - 1] * d.data()[n - 1] + rho) + 1e-7);
                }
            }
        }
    }
    CHECK(worst < 1e-8);
}

// v3b-1b-perf: blocked dorgbr (orgbr_q / orgbr_p) must reproduce the scalar
// reflector-apply oracle (apply_h_left builds Q m x m; apply_g_left builds
// P n x n) bit-close. Sizes span the scalar/blocked crossover (2*kOrgbrBlock
// = 64) and several multi-panel sizes, m=n and m>n.
TEST_CASE("orgbr_q/orgbr_p: blocked formation == scalar oracle", "[hesap][svd][orgbr]")
{
    crd::memory::TlsfAllocator alloc(128U * 1024U * 1024U);
    for (auto mn : {std::pair<int, int>{40, 40}, {65, 65}, {96, 72}, {100, 100}, {150, 90}, {200, 200}})
    {
        const int m = mn.first;
        const int n = mn.second;
        crd::containers::Array<double> a(m * n, &alloc);
        crd::containers::Array<double> d(n, &alloc);
        crd::containers::Array<double> e(n, &alloc);
        crd::containers::Array<double> tauq(n, &alloc);
        crd::containers::Array<double> taup(n, &alloc);
        crd::u64 s = 0xB10C4EDULL + static_cast<crd::u64>(m * 911 + n);
        auto next = [&s]() {
            s = s * 6364136223846793005ULL + 1442695040888963407ULL;
            return static_cast<double>((s >> 11) & 0xFFFFFFFFFFFFFULL) / static_cast<double>(1ULL << 52);
        };
        for (int i = 0; i < m * n; ++i)
        {
            a.data()[i] = 2.0 * next() - 1.0;
        }
        crd::hesap::dense::bidiagonalize<double>(a.data(), static_cast<crd::usize>(m), static_cast<crd::usize>(n),
                                                 static_cast<crd::usize>(n), d.data(), e.data(), tauq.data(),
                                                 taup.data(), &alloc);

        // Oracle Q (m x m), P (n x n).
        crd::containers::Array<double> q(m * m, &alloc);
        for (int i = 0; i < m * m; ++i)
        {
            q.data()[i] = (i / m == i % m) ? 1.0 : 0.0;
        }
        for (int i = n - 1; i >= 0; --i)
        {
            apply_h_left(q.data(), m, a.data(), n, i, m, tauq.data()[i]);
        }
        crd::containers::Array<double> p(n * n, &alloc);
        for (int i = 0; i < n * n; ++i)
        {
            p.data()[i] = (i / n == i % n) ? 1.0 : 0.0;
        }
        for (int i = n - 2; i >= 0; --i)
        {
            apply_g_left(p.data(), n, a.data(), n, i, taup.data()[i]);
        }

        // Blocked.
        crd::containers::Array<double> u(m * n, &alloc);
        crd::containers::Array<double> vt(n * n, &alloc);
        crd::hesap::dense::detail::orgbr_q<double>(a.data(), static_cast<crd::usize>(m),
                                                   static_cast<crd::usize>(n), static_cast<crd::usize>(n),
                                                   tauq.data(), u.data(), &alloc);
        crd::hesap::dense::detail::orgbr_p<double>(a.data(), static_cast<crd::usize>(n),
                                                   static_cast<crd::usize>(n), taup.data(), vt.data(), &alloc);

        // orgbr_q == Q[:, :n]; orgbr_p == P^T (vt[i][j] == p[j][i]).
        double worst_q = 0.0;
        for (int i = 0; i < m; ++i)
        {
            for (int j = 0; j < n; ++j)
            {
                worst_q = std::max(worst_q, std::abs(u.data()[i * n + j] - q.data()[i * m + j]));
            }
        }
        double worst_p = 0.0;
        for (int i = 0; i < n; ++i)
        {
            for (int j = 0; j < n; ++j)
            {
                worst_p = std::max(worst_p, std::abs(vt.data()[i * n + j] - p.data()[j * n + i]));
            }
        }
        CHECK(worst_q < 1e-11);
        CHECK(worst_p < 1e-11);
    }
}

TEST_CASE("svdvals: matches svd() singular values", "[hesap][svd]")
{
    crd::memory::TlsfAllocator alloc(64U * 1024U * 1024U);
    for (auto mn : {std::pair<int, int>{10, 10}, {14, 9}, {6, 13}, {80, 80}, {100, 72}, {70, 110}})
    {
        const int m = mn.first;
        const int n = mn.second;
        crd::hesap::dense::Matrix<double> a(&alloc, static_cast<crd::usize>(m), static_cast<crd::usize>(n));
        Rng rng{0xACE777ULL + static_cast<crd::u64>(m * 31 + n)};
        for (int i = 0; i < m; ++i)
        {
            for (int j = 0; j < n; ++j)
            {
                a.at(static_cast<crd::usize>(i), static_cast<crd::usize>(j)) = 2.0 * rng.next() - 1.0;
            }
        }
        const auto full = crd::hesap::dense::svd<double>(&alloc, a);
        const auto vals = crd::hesap::dense::svdvals<double>(&alloc, a);
        const int k = std::min(m, n);
        double worst = 0.0;
        for (int i = 0; i < k; ++i)
        {
            worst = std::max(worst, std::abs(vals.data()[i] - full.s.data()[i]));
        }
        CHECK(worst < 1e-10);
    }
}

TEST_CASE("svd CLI: commands registered + correct singular values", "[hesap][svd][cli]")
{
    using crd::hesap::cli::CommandArgs;
    using crd::hesap::cli::CommandRegistry;
    using crd::hesap::cli::ResultBinaryBlob;

    REQUIRE(CommandRegistry::global().find("hesap.dense.svd.f32") != nullptr);
    REQUIRE(CommandRegistry::global().find("hesap.dense.svdvals.f32") != nullptr);
    const auto* rec = CommandRegistry::global().find("hesap.dense.svd.f64");
    REQUIRE(rec != nullptr);

    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(1U * 1024U * 1024U));
    // 3x3 diagonal {4,1,3} flattened row-major -> singular values {4,3,1}.
    const crd::f64 a_flat[] = {4.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 3.0};
    CommandArgs args{&alloc};
    args.set_u64("m", 3);
    args.set_u64("n", 3);
    args.set_f64_array("A", crd::containers::ConstSpan<crd::f64>{a_flat, 9});
    const auto result = rec->impl(args);
    REQUIRE(result.ok);
    const auto* blob = std::get_if<ResultBinaryBlob>(&result.value);
    REQUIRE(blob != nullptr);
    REQUIRE(blob->bytes.size() == 3 * sizeof(crd::f64));
    const auto* vals = reinterpret_cast<const crd::f64*>(blob->bytes.data());
    CHECK(std::abs(vals[0] - 4.0) < 1e-12);
    CHECK(std::abs(vals[1] - 3.0) < 1e-12);
    CHECK(std::abs(vals[2] - 1.0) < 1e-12);
}

TEST_CASE("bidiagonalize: A == Q B P^T reconstruction", "[hesap][svd][bidiag]")
{
    CHECK(check_bidiag(10, 10) < 1.0e-12);
    CHECK(check_bidiag(12, 8) < 1.0e-12);
    CHECK(check_bidiag(16, 16) < 1.0e-12);
}

// v3b-1a-perf: exercise the BLOCKED dgebrd path (n > 2*kBidiagBlock = 64). These
// sizes route through dlabrd_upper panels + the trailing BLAS-3 rank-2k update +
// the unblocked tail. 65 sits one past the unblocked crossover; 96 = exactly 3
// panels (no tail); 100/150 leave a non-trivial unblocked tail; m>n covered too.
TEST_CASE("bidiagonalize: blocked path reconstruction (n > block size)", "[hesap][svd][bidiag]")
{
    CHECK(check_bidiag(65, 65) < 1.0e-11);
    CHECK(check_bidiag(96, 96) < 1.0e-11);
    CHECK(check_bidiag(100, 72) < 1.0e-11);
    CHECK(check_bidiag(128, 128) < 1.0e-11);
    CHECK(check_bidiag(150, 100) < 1.0e-11);
}

TEST_CASE("bidiagonalize: Q and P are orthonormal", "[hesap][svd][bidiag]")
{
    // Re-run a case and verify orthogonality of the formed Q, P.
    crd::memory::TlsfAllocator alloc(8U * 1024U * 1024U);
    const int m = 12;
    const int n = 9;
    crd::containers::Array<double> a(m * n, &alloc);
    crd::containers::Array<double> d(n, &alloc);
    crd::containers::Array<double> e(n, &alloc);
    crd::containers::Array<double> tauq(n, &alloc);
    crd::containers::Array<double> taup(n, &alloc);
    crd::u64 s = 0x5151ABCDULL;
    auto next = [&s]() {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<double>((s >> 11) & 0xFFFFFFFFFFFFFULL) / static_cast<double>(1ULL << 52);
    };
    for (int i = 0; i < m * n; ++i)
    {
        a.data()[i] = 2.0 * next() - 1.0;
    }
    crd::hesap::dense::bidiagonalize<double>(a.data(), static_cast<crd::usize>(m), static_cast<crd::usize>(n),
                                             static_cast<crd::usize>(n), d.data(), e.data(), tauq.data(),
                                             taup.data(), &alloc);
    crd::containers::Array<double> q(m * m, &alloc);
    for (int i = 0; i < m * m; ++i)
    {
        q.data()[i] = (i / m == i % m) ? 1.0 : 0.0;
    }
    for (int i = n - 1; i >= 0; --i)
    {
        apply_h_left(q.data(), m, a.data(), n, i, m, tauq.data()[i]);
    }
    crd::containers::Array<double> p(n * n, &alloc);
    for (int i = 0; i < n * n; ++i)
    {
        p.data()[i] = (i / n == i % n) ? 1.0 : 0.0;
    }
    for (int i = n - 2; i >= 0; --i)
    {
        apply_g_left(p.data(), n, a.data(), n, i, taup.data()[i]);
    }
    CHECK(orth_err(m, q.data()) < 1.0e-12);
    CHECK(orth_err(n, p.data()) < 1.0e-12);
}
