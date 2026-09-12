#pragma once

// v12-r — Regression / GLM / multivariate (crd-hesap-stats). Linear models (OLS/WLS/GLS) ride the shipped dense
// least-squares (QR/COD/SVD); regularized (ridge/lasso/elastic-net) via closed form / coordinate descent; GLM via IRLS.
// SANITY 8: reuse crd-hesap-dense's `lstsq`/`pinv` rather than reimplement. Gold: statsmodels · sklearn · MATLAB · R.

#include <crd/hesap/stats/descriptive.hpp> // Real
#include <crd/hesap/stats/threefry.hpp>     // ThreefryRng (RANSAC sampling = determinism moat)
#include <crd/hesap/dense/eig_sym.hpp>      // eig_sym (PCA / LDA / QDA / factor analysis)
#include <crd/hesap/dense/lstsq.hpp>        // lstsq, pinv (the shipped fast factorizations)
#include <crd/hesap/dense/matrix.hpp>
#include <crd/hesap/dense/matrix_types.hpp> // Symmetric
#include <crd/hesap/dense/vector.hpp>

#include <crd/containers/array.hpp>
#include <crd/core/types.hpp>
#include <crd/math/cmath.hpp>
#include <crd/memory/allocator.hpp>

#include <algorithm> // std::sort (robust scale median)

namespace crd::hesap::stats
{

template <Real T> struct OlsResult
{
    crd::containers::Array<T> coef; // p coefficients
    T r_squared;
    crd::containers::Array<T> se; // p standard errors
    T sigma2;                     // residual variance RSS/(n-p)
};

namespace detail
{
// beta = argmin ||Xb - y|| via the shipped least-squares (X is n*p row-major). Returns coef[p] + RSS.
template <Real T>
void ls_solve(crd::containers::ConstSpan<T> x, crd::containers::ConstSpan<T> y, crd::usize n, crd::usize p,
              crd::memory::IAllocator* alloc, crd::containers::Array<T>& coef_out, T& rss_out)
{
    crd::hesap::dense::Matrix<T> xm(alloc, n, p);
    for (crd::usize i = 0; i < n; ++i)
    {
        for (crd::usize j = 0; j < p; ++j)
        {
            xm.at(i, j) = x[i * p + j];
        }
    }
    crd::hesap::dense::Vector<T> yv(alloc, y);
    auto ls = crd::hesap::dense::lstsq(alloc, xm, yv);
    coef_out.resize(p);
    for (crd::usize j = 0; j < p; ++j)
    {
        coef_out[j] = ls.x.at(j, 0);
    }
    const T res = ls.residual.data()[0];
    rss_out = res * res;
}
} // namespace detail

// Ordinary least squares: coefficients + R^2 + standard errors. statsmodels.OLS.
template <Real T>
[[nodiscard]] OlsResult<T> ols(crd::containers::ConstSpan<T> x, crd::containers::ConstSpan<T> y, crd::usize n,
                               crd::usize p, crd::memory::IAllocator* alloc)
{
    OlsResult<T> r;
    T rss = static_cast<T>(0);
    detail::ls_solve(x, y, n, p, alloc, r.coef, rss);
    T ybar = static_cast<T>(0);
    for (crd::usize i = 0; i < n; ++i)
    {
        ybar += y[i];
    }
    ybar /= static_cast<T>(n);
    T tss = static_cast<T>(0);
    for (crd::usize i = 0; i < n; ++i)
    {
        const T d = y[i] - ybar;
        tss += d * d;
    }
    r.r_squared = static_cast<T>(1) - rss / tss;
    r.sigma2 = rss / static_cast<T>(n - p);
    // se = sqrt(sigma2 * diag((X^T X)^-1))
    crd::hesap::dense::Matrix<T> xtx(alloc, p, p);
    for (crd::usize a = 0; a < p; ++a)
    {
        for (crd::usize b = 0; b < p; ++b)
        {
            T s = static_cast<T>(0);
            for (crd::usize i = 0; i < n; ++i)
            {
                s += x[i * p + a] * x[i * p + b];
            }
            xtx.at(a, b) = s;
        }
    }
    auto inv = crd::hesap::dense::pinv(alloc, xtx);
    r.se.resize(p);
    for (crd::usize j = 0; j < p; ++j)
    {
        r.se[j] = crd::math::sqrt(r.sigma2 * inv.at(j, j));
    }
    return r;
}

// Weighted least squares: OLS on the sqrt(w)-scaled data. statsmodels.WLS.
template <Real T>
[[nodiscard]] crd::containers::Array<T> wls(crd::containers::ConstSpan<T> x, crd::containers::ConstSpan<T> y,
                                            crd::containers::ConstSpan<T> w, crd::usize n, crd::usize p,
                                            crd::memory::IAllocator* alloc)
{
    crd::containers::Array<T> xt(alloc);
    crd::containers::Array<T> yt(alloc);
    xt.resize(n * p);
    yt.resize(n);
    for (crd::usize i = 0; i < n; ++i)
    {
        const T sw = crd::math::sqrt(w[i]);
        for (crd::usize j = 0; j < p; ++j)
        {
            xt[i * p + j] = sw * x[i * p + j];
        }
        yt[i] = sw * y[i];
    }
    crd::containers::Array<T> coef(alloc);
    T rss = static_cast<T>(0);
    detail::ls_solve(crd::containers::ConstSpan<T>{xt.data(), n * p}, crd::containers::ConstSpan<T>{yt.data(), n}, n, p,
                     alloc, coef, rss);
    return coef;
}

// Generalized least squares: beta = (X^T S^-1 X)^-1 X^T S^-1 y. statsmodels.GLS.
template <Real T>
[[nodiscard]] crd::containers::Array<T> gls(crd::containers::ConstSpan<T> x, crd::containers::ConstSpan<T> y,
                                            crd::containers::ConstSpan<T> sigma, crd::usize n, crd::usize p,
                                            crd::memory::IAllocator* alloc)
{
    crd::hesap::dense::Matrix<T> sig(alloc, n, n);
    for (crd::usize i = 0; i < n; ++i)
    {
        for (crd::usize j = 0; j < n; ++j)
        {
            sig.at(i, j) = sigma[i * n + j];
        }
    }
    auto sinv = crd::hesap::dense::pinv(alloc, sig); // S^-1 (n*n)
    crd::hesap::dense::Matrix<T> a(alloc, p, p);
    crd::containers::Array<T> bvec(alloc);
    bvec.resize(p);
    for (crd::usize qa = 0; qa < p; ++qa)
    {
        for (crd::usize qb = 0; qb < p; ++qb)
        {
            T s = static_cast<T>(0);
            for (crd::usize i = 0; i < n; ++i)
            {
                for (crd::usize j = 0; j < n; ++j)
                {
                    s += x[i * p + qa] * sinv.at(i, j) * x[j * p + qb];
                }
            }
            a.at(qa, qb) = s;
        }
        T sb = static_cast<T>(0);
        for (crd::usize i = 0; i < n; ++i)
        {
            for (crd::usize j = 0; j < n; ++j)
            {
                sb += x[i * p + qa] * sinv.at(i, j) * y[j];
            }
        }
        bvec[qa] = sb;
    }
    crd::hesap::dense::Vector<T> bv(alloc, crd::containers::ConstSpan<T>{bvec.data(), p});
    auto ls = crd::hesap::dense::lstsq(alloc, a, bv);
    crd::containers::Array<T> coef(alloc);
    coef.resize(p);
    for (crd::usize j = 0; j < p; ++j)
    {
        coef[j] = ls.x.at(j, 0);
    }
    return coef;
}

// Ridge regression (L2): (X^T X + alpha I) beta = X^T y. sklearn Ridge(fit_intercept=False).
template <Real T>
[[nodiscard]] crd::containers::Array<T> ridge(crd::containers::ConstSpan<T> x, crd::containers::ConstSpan<T> y,
                                              crd::usize n, crd::usize p, T alpha, crd::memory::IAllocator* alloc)
{
    crd::hesap::dense::Matrix<T> a(alloc, p, p);
    crd::containers::Array<T> xty(alloc);
    xty.resize(p);
    for (crd::usize qa = 0; qa < p; ++qa)
    {
        for (crd::usize qb = 0; qb < p; ++qb)
        {
            T s = static_cast<T>(0);
            for (crd::usize i = 0; i < n; ++i)
            {
                s += x[i * p + qa] * x[i * p + qb];
            }
            a.at(qa, qb) = s + (qa == qb ? alpha : static_cast<T>(0));
        }
        T sb = static_cast<T>(0);
        for (crd::usize i = 0; i < n; ++i)
        {
            sb += x[i * p + qa] * y[i];
        }
        xty[qa] = sb;
    }
    crd::hesap::dense::Vector<T> bv(alloc, crd::containers::ConstSpan<T>{xty.data(), p});
    auto ls = crd::hesap::dense::lstsq(alloc, a, bv);
    crd::containers::Array<T> coef(alloc);
    coef.resize(p);
    for (crd::usize j = 0; j < p; ++j)
    {
        coef[j] = ls.x.at(j, 0);
    }
    return coef;
}

// Elastic-net via cyclic coordinate descent: (1/2n)||y-Xb||^2 + alpha*l1*||b||_1 + (alpha*(1-l1)/2)||b||^2.
// sklearn ElasticNet(fit_intercept=False); l1_ratio=1 gives Lasso.
template <Real T>
[[nodiscard]] crd::containers::Array<T> elastic_net(crd::containers::ConstSpan<T> x, crd::containers::ConstSpan<T> y,
                                                    crd::usize n, crd::usize p, T alpha, T l1_ratio,
                                                    crd::memory::IAllocator* alloc, crd::usize max_iter = 100000,
                                                    T tol = static_cast<T>(1e-12))
{
    crd::containers::Array<T> beta(alloc);
    crd::containers::Array<T> r(alloc);
    crd::containers::Array<T> norm2(alloc);
    beta.resize(p);
    r.resize(n);
    norm2.resize(p);
    for (crd::usize j = 0; j < p; ++j)
    {
        beta[j] = static_cast<T>(0);
        T s = static_cast<T>(0);
        for (crd::usize i = 0; i < n; ++i)
        {
            s += x[i * p + j] * x[i * p + j];
        }
        norm2[j] = s;
    }
    for (crd::usize i = 0; i < n; ++i)
    {
        r[i] = y[i]; // residual at beta = 0
    }
    const T l1 = static_cast<T>(n) * alpha * l1_ratio;
    const T l2 = static_cast<T>(n) * alpha * (static_cast<T>(1) - l1_ratio);
    for (crd::usize it = 0; it < max_iter; ++it)
    {
        T max_change = static_cast<T>(0);
        for (crd::usize j = 0; j < p; ++j)
        {
            T xjr = static_cast<T>(0);
            for (crd::usize i = 0; i < n; ++i)
            {
                xjr += x[i * p + j] * r[i];
            }
            const T rho = xjr + beta[j] * norm2[j]; // x_j . R_{-j}
            T bj = static_cast<T>(0);
            if (rho > l1)
            {
                bj = (rho - l1) / (norm2[j] + l2);
            }
            else if (rho < -l1)
            {
                bj = (rho + l1) / (norm2[j] + l2);
            }
            const T delta = bj - beta[j];
            if (delta != static_cast<T>(0))
            {
                for (crd::usize i = 0; i < n; ++i)
                {
                    r[i] -= delta * x[i * p + j];
                }
                const T ad = delta < static_cast<T>(0) ? -delta : delta;
                if (ad > max_change)
                {
                    max_change = ad;
                }
            }
            beta[j] = bj;
        }
        if (max_change < tol)
        {
            break;
        }
    }
    return beta;
}

template <Real T>
[[nodiscard]] crd::containers::Array<T> lasso(crd::containers::ConstSpan<T> x, crd::containers::ConstSpan<T> y,
                                              crd::usize n, crd::usize p, T alpha, crd::memory::IAllocator* alloc)
{
    return elastic_net(x, y, n, p, alpha, static_cast<T>(1), alloc);
}

enum class GlmFamily
{
    Logistic, // Binomial + logit link
    Poisson,  // Poisson + log link
    GammaLog  // Gamma + log link
};

// Generalized linear model via iteratively-reweighted least squares (Fisher scoring). statsmodels.GLM.
template <Real T>
[[nodiscard]] crd::containers::Array<T> glm(crd::containers::ConstSpan<T> x, crd::containers::ConstSpan<T> y,
                                            crd::usize n, crd::usize p, GlmFamily fam, crd::memory::IAllocator* alloc,
                                            crd::usize max_iter = 100, T tol = static_cast<T>(1e-11))
{
    crd::containers::Array<T> beta(alloc);
    crd::containers::Array<T> eta(alloc);
    crd::containers::Array<T> w(alloc);
    crd::containers::Array<T> z(alloc);
    beta.resize(p);
    eta.resize(n);
    w.resize(n);
    z.resize(n);
    for (crd::usize j = 0; j < p; ++j)
    {
        beta[j] = static_cast<T>(0);
    }
    for (crd::usize it = 0; it < max_iter; ++it)
    {
        for (crd::usize i = 0; i < n; ++i)
        {
            T e = static_cast<T>(0);
            for (crd::usize j = 0; j < p; ++j)
            {
                e += x[i * p + j] * beta[j];
            }
            eta[i] = e;
            T m = static_cast<T>(0);
            T dmde = static_cast<T>(0);
            T var = static_cast<T>(1);
            if (fam == GlmFamily::Logistic)
            {
                m = static_cast<T>(1) / (static_cast<T>(1) + crd::math::exp(-e));
                m = m < static_cast<T>(1e-12) ? static_cast<T>(1e-12)
                                              : (m > static_cast<T>(1) - static_cast<T>(1e-12)
                                                     ? static_cast<T>(1) - static_cast<T>(1e-12)
                                                     : m);
                dmde = m * (static_cast<T>(1) - m);
                var = dmde;
            }
            else if (fam == GlmFamily::Poisson)
            {
                m = crd::math::exp(e);
                dmde = m;
                var = m;
            }
            else // GammaLog
            {
                m = crd::math::exp(e);
                dmde = m;
                var = m * m;
            }
            w[i] = dmde * dmde / var;
            z[i] = e + (y[i] - m) / dmde;
        }
        crd::hesap::dense::Matrix<T> a(alloc, p, p);
        crd::containers::Array<T> b(alloc);
        b.resize(p);
        for (crd::usize qa = 0; qa < p; ++qa)
        {
            for (crd::usize qb = 0; qb < p; ++qb)
            {
                T s = static_cast<T>(0);
                for (crd::usize i = 0; i < n; ++i)
                {
                    s += x[i * p + qa] * w[i] * x[i * p + qb];
                }
                a.at(qa, qb) = s;
            }
            T sb = static_cast<T>(0);
            for (crd::usize i = 0; i < n; ++i)
            {
                sb += x[i * p + qa] * w[i] * z[i];
            }
            b[qa] = sb;
        }
        crd::hesap::dense::Vector<T> bv(alloc, crd::containers::ConstSpan<T>{b.data(), p});
        auto ls = crd::hesap::dense::lstsq(alloc, a, bv);
        T max_change = static_cast<T>(0);
        for (crd::usize j = 0; j < p; ++j)
        {
            const T nb = ls.x.at(j, 0);
            const T dlt = nb - beta[j];
            const T ad = dlt < static_cast<T>(0) ? -dlt : dlt;
            if (ad > max_change)
            {
                max_change = ad;
            }
            beta[j] = nb;
        }
        if (max_change < tol)
        {
            break;
        }
    }
    return beta;
}

template <Real T> struct PcaResult
{
    crd::containers::Array<T> explained_variance; // p, descending
    crd::containers::Array<T> components;         // p*p row-major: components[k*p + j] = PC k, feature j
};

// Principal component analysis: eigendecomposition of the sample covariance (= sklearn PCA's S^2/(n-1) and right
// singular vectors). Components sign-fixed to max-abs-element positive (the sklearn convention). sklearn.decomposition.PCA.
template <Real T>
[[nodiscard]] PcaResult<T> pca(crd::containers::ConstSpan<T> x, crd::usize n, crd::usize p,
                               crd::memory::IAllocator* alloc)
{
    crd::containers::Array<T> mean(alloc);
    mean.resize(p);
    for (crd::usize j = 0; j < p; ++j)
    {
        T s = static_cast<T>(0);
        for (crd::usize i = 0; i < n; ++i)
        {
            s += x[i * p + j];
        }
        mean[j] = s / static_cast<T>(n);
    }
    crd::hesap::dense::Symmetric<T> cov(alloc, p);
    for (crd::usize a = 0; a < p; ++a)
    {
        for (crd::usize b = 0; b < p; ++b)
        {
            T s = static_cast<T>(0);
            for (crd::usize i = 0; i < n; ++i)
            {
                s += (x[i * p + a] - mean[a]) * (x[i * p + b] - mean[b]);
            }
            cov.at(a, b) = s / static_cast<T>(n - 1);
        }
    }
    auto e = crd::hesap::dense::eig_sym(alloc, cov); // ascending values, eigenvectors as columns
    PcaResult<T> r;
    r.explained_variance.resize(p);
    r.components.resize(p * p);
    for (crd::usize k = 0; k < p; ++k)
    {
        const crd::usize src = p - 1 - k; // reverse to descending
        r.explained_variance[k] = e.values.data()[src];
        crd::usize mi = 0;
        T ma = static_cast<T>(0);
        for (crd::usize j = 0; j < p; ++j)
        {
            const T v = e.vectors.at(j, src);
            const T av = v < static_cast<T>(0) ? -v : v;
            if (av > ma)
            {
                ma = av;
                mi = j;
            }
        }
        const T sign = e.vectors.at(mi, src) >= static_cast<T>(0) ? static_cast<T>(1) : static_cast<T>(-1);
        for (crd::usize j = 0; j < p; ++j)
        {
            r.components[k * p + j] = sign * e.vectors.at(j, src);
        }
    }
    return r;
}

namespace detail
{
// Inverse + log-determinant of a p*p covariance (row-major), via the symmetric eigensolver.
template <Real T>
void cov_inv_logdet(crd::containers::ConstSpan<T> cov, crd::usize p, crd::memory::IAllocator* alloc,
                    crd::containers::Array<T>& sinv_out, T& logdet_out)
{
    crd::hesap::dense::Symmetric<T> s(alloc, p);
    for (crd::usize a = 0; a < p; ++a)
    {
        for (crd::usize b = 0; b < p; ++b)
        {
            s.at(a, b) = cov[a * p + b];
        }
    }
    auto e = crd::hesap::dense::eig_sym(alloc, s);
    logdet_out = static_cast<T>(0);
    for (crd::usize k = 0; k < p; ++k)
    {
        logdet_out += crd::math::log(e.values.data()[k]);
    }
    sinv_out.resize(p * p);
    for (crd::usize a = 0; a < p; ++a)
    {
        for (crd::usize b = 0; b < p; ++b)
        {
            T acc = static_cast<T>(0);
            for (crd::usize k = 0; k < p; ++k)
            {
                acc += e.vectors.at(a, k) * (static_cast<T>(1) / e.values.data()[k]) * e.vectors.at(b, k);
            }
            sinv_out[a * p + b] = acc;
        }
    }
}

// Per-class means + counts of a labeled dataset.
template <Real T>
void class_means(crd::containers::ConstSpan<T> x, crd::containers::ConstSpan<crd::u32> y, crd::usize ntr, crd::usize p,
                 crd::usize nc, crd::containers::Array<T>& mean, crd::containers::Array<crd::usize>& cnt)
{
    mean.resize(nc * p);
    cnt.resize(nc);
    for (crd::usize c = 0; c < nc; ++c)
    {
        cnt[c] = 0;
        for (crd::usize j = 0; j < p; ++j)
        {
            mean[c * p + j] = static_cast<T>(0);
        }
    }
    for (crd::usize i = 0; i < ntr; ++i)
    {
        const crd::u32 c = y[i];
        ++cnt[c];
        for (crd::usize j = 0; j < p; ++j)
        {
            mean[c * p + j] += x[i * p + j];
        }
    }
    for (crd::usize c = 0; c < nc; ++c)
    {
        for (crd::usize j = 0; j < p; ++j)
        {
            mean[c * p + j] /= static_cast<T>(cnt[c]);
        }
    }
}
} // namespace detail

// Linear discriminant analysis: shared (pooled) covariance, Bayes rule with empirical priors. sklearn LDA.predict.
template <Real T>
[[nodiscard]] crd::containers::Array<crd::u32> lda_predict(crd::containers::ConstSpan<T> xtr,
                                                           crd::containers::ConstSpan<crd::u32> y, crd::usize ntr,
                                                           crd::usize p, crd::usize nc,
                                                           crd::containers::ConstSpan<T> xte, crd::usize nte,
                                                           crd::memory::IAllocator* alloc)
{
    crd::containers::Array<T> mean(alloc);
    crd::containers::Array<crd::usize> cnt(alloc);
    detail::class_means(xtr, y, ntr, p, nc, mean, cnt);
    crd::containers::Array<T> cov(alloc); // pooled within-class / ntr (sklearn LDA convention)
    cov.resize(p * p);
    for (crd::usize i = 0; i < p * p; ++i)
    {
        cov[i] = static_cast<T>(0);
    }
    for (crd::usize i = 0; i < ntr; ++i)
    {
        const crd::u32 c = y[i];
        for (crd::usize a = 0; a < p; ++a)
        {
            for (crd::usize b = 0; b < p; ++b)
            {
                cov[a * p + b] += (xtr[i * p + a] - mean[c * p + a]) * (xtr[i * p + b] - mean[c * p + b]);
            }
        }
    }
    for (crd::usize i = 0; i < p * p; ++i)
    {
        cov[i] /= static_cast<T>(ntr);
    }
    crd::containers::Array<T> sinv(alloc);
    T logdet = static_cast<T>(0);
    detail::cov_inv_logdet(crd::containers::ConstSpan<T>{cov.data(), p * p}, p, alloc, sinv, logdet);
    crd::containers::Array<crd::u32> pred(alloc);
    pred.resize(nte);
    for (crd::usize t = 0; t < nte; ++t)
    {
        T best = static_cast<T>(0);
        crd::u32 bestc = 0;
        bool first = true;
        for (crd::usize c = 0; c < nc; ++c)
        {
            T xsm = static_cast<T>(0);
            T msm = static_cast<T>(0);
            for (crd::usize a = 0; a < p; ++a)
            {
                for (crd::usize b = 0; b < p; ++b)
                {
                    xsm += xte[t * p + a] * sinv[a * p + b] * mean[c * p + b];
                    msm += mean[c * p + a] * sinv[a * p + b] * mean[c * p + b];
                }
            }
            const T delta = crd::math::log(static_cast<T>(cnt[c]) / static_cast<T>(ntr)) + xsm - static_cast<T>(0.5) * msm;
            if (first || delta > best)
            {
                best = delta;
                bestc = static_cast<crd::u32>(c);
                first = false;
            }
        }
        pred[t] = bestc;
    }
    return pred;
}

// Quadratic discriminant analysis: per-class covariance (sample, ddof=1) + full quadratic discriminant. sklearn QDA.
template <Real T>
[[nodiscard]] crd::containers::Array<crd::u32> qda_predict(crd::containers::ConstSpan<T> xtr,
                                                           crd::containers::ConstSpan<crd::u32> y, crd::usize ntr,
                                                           crd::usize p, crd::usize nc,
                                                           crd::containers::ConstSpan<T> xte, crd::usize nte,
                                                           crd::memory::IAllocator* alloc)
{
    crd::containers::Array<T> mean(alloc);
    crd::containers::Array<crd::usize> cnt(alloc);
    detail::class_means(xtr, y, ntr, p, nc, mean, cnt);
    crd::containers::Array<T> sinv(alloc);
    crd::containers::Array<T> logdet(alloc);
    sinv.resize(nc * p * p);
    logdet.resize(nc);
    crd::containers::Array<T> cov(alloc);
    crd::containers::Array<T> si(alloc);
    cov.resize(p * p);
    for (crd::usize c = 0; c < nc; ++c)
    {
        for (crd::usize i = 0; i < p * p; ++i)
        {
            cov[i] = static_cast<T>(0);
        }
        for (crd::usize i = 0; i < ntr; ++i)
        {
            if (y[i] != c)
            {
                continue;
            }
            for (crd::usize a = 0; a < p; ++a)
            {
                for (crd::usize b = 0; b < p; ++b)
                {
                    cov[a * p + b] += (xtr[i * p + a] - mean[c * p + a]) * (xtr[i * p + b] - mean[c * p + b]);
                }
            }
        }
        for (crd::usize i = 0; i < p * p; ++i)
        {
            cov[i] /= static_cast<T>(cnt[c] - 1);
        }
        T ld = static_cast<T>(0);
        detail::cov_inv_logdet(crd::containers::ConstSpan<T>{cov.data(), p * p}, p, alloc, si, ld);
        logdet[c] = ld;
        for (crd::usize i = 0; i < p * p; ++i)
        {
            sinv[c * p * p + i] = si[i];
        }
    }
    crd::containers::Array<crd::u32> pred(alloc);
    pred.resize(nte);
    for (crd::usize t = 0; t < nte; ++t)
    {
        T best = static_cast<T>(0);
        crd::u32 bestc = 0;
        bool first = true;
        for (crd::usize c = 0; c < nc; ++c)
        {
            T q = static_cast<T>(0);
            for (crd::usize a = 0; a < p; ++a)
            {
                for (crd::usize b = 0; b < p; ++b)
                {
                    q += (xte[t * p + a] - mean[c * p + a]) * sinv[c * p * p + a * p + b] *
                         (xte[t * p + b] - mean[c * p + b]);
                }
            }
            const T delta = crd::math::log(static_cast<T>(cnt[c]) / static_cast<T>(ntr)) -
                            static_cast<T>(0.5) * logdet[c] - static_cast<T>(0.5) * q;
            if (first || delta > best)
            {
                best = delta;
                bestc = static_cast<crd::u32>(c);
                first = false;
            }
        }
        pred[t] = bestc;
    }
    return pred;
}

// Robust regression (Huber M-estimator) via IRLS. Scale = center-0 MAD median(|r|)/0.6745 (statsmodels RLM convention).
// statsmodels.RLM(M=HuberT()).
template <Real T>
[[nodiscard]] crd::containers::Array<T> robust_huber(crd::containers::ConstSpan<T> x, crd::containers::ConstSpan<T> y,
                                                     crd::usize n, crd::usize p, crd::memory::IAllocator* alloc,
                                                     T tuning = static_cast<T>(1.345), crd::usize max_iter = 50,
                                                     T tol = static_cast<T>(1e-9))
{
    crd::containers::Array<T> beta(alloc);
    T rss = static_cast<T>(0);
    detail::ls_solve(x, y, n, p, alloc, beta, rss); // OLS start
    crd::containers::Array<T> w(alloc);
    crd::containers::Array<T> res(alloc);
    crd::containers::Array<T> absr(alloc);
    w.resize(n);
    res.resize(n);
    absr.resize(n);
    for (crd::usize it = 0; it < max_iter; ++it)
    {
        for (crd::usize i = 0; i < n; ++i)
        {
            T pred = static_cast<T>(0);
            for (crd::usize j = 0; j < p; ++j)
            {
                pred += x[i * p + j] * beta[j];
            }
            res[i] = y[i] - pred;
            absr[i] = res[i] < static_cast<T>(0) ? -res[i] : res[i];
        }
        std::sort(absr.data(), absr.data() + n);
        const T med =
            (n % 2 == 1) ? absr[n / 2] : static_cast<T>(0.5) * (absr[n / 2 - 1] + absr[n / 2]);
        const T s = med / static_cast<T>(0.6745);
        if (s <= static_cast<T>(0))
        {
            break;
        }
        for (crd::usize i = 0; i < n; ++i)
        {
            const T u = res[i] / s;
            const T au = u < static_cast<T>(0) ? -u : u;
            w[i] = au <= tuning ? static_cast<T>(1) : tuning / au;
        }
        auto nb = wls(x, y, crd::containers::ConstSpan<T>{w.data(), n}, n, p, alloc);
        T max_change = static_cast<T>(0);
        for (crd::usize j = 0; j < p; ++j)
        {
            const T d = nb[j] - beta[j];
            const T ad = d < static_cast<T>(0) ? -d : d;
            if (ad > max_change)
            {
                max_change = ad;
            }
            beta[j] = nb[j];
        }
        if (max_change < tol)
        {
            break;
        }
    }
    return beta;
}

// Quantile regression at level tau via IRLS on the pinball loss (weights w_i = (tau or 1-tau)/|r_i|). statsmodels.QuantReg.
template <Real T>
[[nodiscard]] crd::containers::Array<T> quantile_regression(crd::containers::ConstSpan<T> x,
                                                            crd::containers::ConstSpan<T> y, crd::usize n, crd::usize p,
                                                            T tau, crd::memory::IAllocator* alloc,
                                                            crd::usize max_iter = 1000, T tol = static_cast<T>(1e-9))
{
    crd::containers::Array<T> beta(alloc);
    T rss = static_cast<T>(0);
    detail::ls_solve(x, y, n, p, alloc, beta, rss);
    crd::containers::Array<T> w(alloc);
    w.resize(n);
    const T eps = static_cast<T>(1e-6);
    for (crd::usize it = 0; it < max_iter; ++it)
    {
        for (crd::usize i = 0; i < n; ++i)
        {
            T pred = static_cast<T>(0);
            for (crd::usize j = 0; j < p; ++j)
            {
                pred += x[i * p + j] * beta[j];
            }
            const T e = y[i] - pred;
            const T ae = e < static_cast<T>(0) ? -e : e;
            const T den = ae < eps ? eps : ae;
            w[i] = (e > static_cast<T>(0) ? tau : static_cast<T>(1) - tau) / den;
        }
        auto nb = wls(x, y, crd::containers::ConstSpan<T>{w.data(), n}, n, p, alloc);
        T max_change = static_cast<T>(0);
        for (crd::usize j = 0; j < p; ++j)
        {
            const T d = nb[j] - beta[j];
            const T ad = d < static_cast<T>(0) ? -d : d;
            if (ad > max_change)
            {
                max_change = ad;
            }
            beta[j] = nb[j];
        }
        if (max_change < tol)
        {
            break;
        }
    }
    return beta;
}

// RANSAC: robust fit by random min-sample consensus, refit on the largest inlier set. Threefry-seeded ⇒ same seed
// bit-identical (the determinism moat). sklearn.RANSACRegressor.
template <Real T>
[[nodiscard]] crd::containers::Array<T> ransac(crd::containers::ConstSpan<T> x, crd::containers::ConstSpan<T> y,
                                               crd::usize n, crd::usize p, T threshold, crd::usize max_trials,
                                               crd::u64 seed, crd::memory::IAllocator* alloc)
{
    ThreefryRng rng(seed, 0);
    crd::containers::Array<T> best_beta(alloc);
    crd::containers::Array<T> sub_x(alloc);
    crd::containers::Array<T> sub_y(alloc);
    crd::containers::Array<crd::usize> idx(alloc);
    crd::containers::Array<T> cand(alloc);
    best_beta.resize(p);
    sub_x.resize(p * p);
    sub_y.resize(p);
    idx.resize(p);
    for (crd::usize j = 0; j < p; ++j)
    {
        best_beta[j] = static_cast<T>(0);
    }
    crd::usize best_inliers = 0;
    for (crd::usize trial = 0; trial < max_trials; ++trial)
    {
        for (crd::usize k = 0; k < p; ++k) // sample p distinct indices
        {
            crd::usize id = 0;
            bool dup = true;
            while (dup)
            {
                id = static_cast<crd::usize>(rng.next_u64() % n);
                dup = false;
                for (crd::usize q = 0; q < k; ++q)
                {
                    if (idx[q] == id)
                    {
                        dup = true;
                    }
                }
            }
            idx[k] = id;
        }
        for (crd::usize k = 0; k < p; ++k)
        {
            for (crd::usize j = 0; j < p; ++j)
            {
                sub_x[k * p + j] = x[idx[k] * p + j];
            }
            sub_y[k] = y[idx[k]];
        }
        T rss = static_cast<T>(0);
        detail::ls_solve(crd::containers::ConstSpan<T>{sub_x.data(), p * p},
                         crd::containers::ConstSpan<T>{sub_y.data(), p}, p, p, alloc, cand, rss);
        crd::usize ni = 0;
        for (crd::usize i = 0; i < n; ++i)
        {
            T pred = static_cast<T>(0);
            for (crd::usize j = 0; j < p; ++j)
            {
                pred += x[i * p + j] * cand[j];
            }
            const T e = y[i] - pred;
            const T ae = e < static_cast<T>(0) ? -e : e;
            if (ae < threshold)
            {
                ++ni;
            }
        }
        if (ni > best_inliers)
        {
            best_inliers = ni;
            for (crd::usize j = 0; j < p; ++j)
            {
                best_beta[j] = cand[j];
            }
        }
    }
    // refit on the inliers of the best model
    crd::usize ni = 0;
    for (crd::usize i = 0; i < n; ++i)
    {
        T pred = static_cast<T>(0);
        for (crd::usize j = 0; j < p; ++j)
        {
            pred += x[i * p + j] * best_beta[j];
        }
        const T e = y[i] - pred;
        if ((e < static_cast<T>(0) ? -e : e) < threshold)
        {
            ++ni;
        }
    }
    crd::containers::Array<T> in_x(alloc);
    crd::containers::Array<T> in_y(alloc);
    in_x.resize(ni * p);
    in_y.resize(ni);
    crd::usize kk = 0;
    for (crd::usize i = 0; i < n; ++i)
    {
        T pred = static_cast<T>(0);
        for (crd::usize j = 0; j < p; ++j)
        {
            pred += x[i * p + j] * best_beta[j];
        }
        const T e = y[i] - pred;
        if ((e < static_cast<T>(0) ? -e : e) < threshold)
        {
            for (crd::usize j = 0; j < p; ++j)
            {
                in_x[kk * p + j] = x[i * p + j];
            }
            in_y[kk] = y[i];
            ++kk;
        }
    }
    crd::containers::Array<T> refit(alloc);
    T rss = static_cast<T>(0);
    detail::ls_solve(crd::containers::ConstSpan<T>{in_x.data(), ni * p}, crd::containers::ConstSpan<T>{in_y.data(), ni},
                     ni, p, alloc, refit, rss);
    return refit;
}

template <Real T> struct FactorAnalysisResult
{
    crd::containers::Array<T> components;     // nc*p row-major: loading k, feature j (sign-fixed max-abs positive)
    crd::containers::Array<T> noise_variance; // p
};

// Factor analysis (X = W F + noise, diagonal noise) via the SVD/EM iteration. Eigendecomposition of the psi-scaled
// covariance replaces the SVD (its eigenvalues == singular values^2). Log-likelihood stop matches sklearn.
// sklearn.decomposition.FactorAnalysis(svd_method='lapack').
template <Real T>
[[nodiscard]] FactorAnalysisResult<T> factor_analysis(crd::containers::ConstSpan<T> x, crd::usize n, crd::usize p,
                                                      crd::usize nc, crd::memory::IAllocator* alloc,
                                                      crd::usize max_iter = 1000, T tol = static_cast<T>(1e-2))
{
    crd::containers::Array<T> mean(alloc);
    crd::containers::Array<T> xc(alloc);
    crd::containers::Array<T> var(alloc);
    crd::containers::Array<T> psi(alloc);
    crd::containers::Array<T> sqrt_psi(alloc);
    crd::containers::Array<T> w(alloc);
    mean.resize(p);
    xc.resize(n * p);
    var.resize(p);
    psi.resize(p);
    sqrt_psi.resize(p);
    w.resize(nc * p);
    for (crd::usize j = 0; j < p; ++j)
    {
        T s = static_cast<T>(0);
        for (crd::usize i = 0; i < n; ++i)
        {
            s += x[i * p + j];
        }
        mean[j] = s / static_cast<T>(n);
    }
    for (crd::usize i = 0; i < n; ++i)
    {
        for (crd::usize j = 0; j < p; ++j)
        {
            xc[i * p + j] = x[i * p + j] - mean[j];
        }
    }
    for (crd::usize j = 0; j < p; ++j)
    {
        T s = static_cast<T>(0);
        for (crd::usize i = 0; i < n; ++i)
        {
            s += xc[i * p + j] * xc[i * p + j];
        }
        var[j] = s / static_cast<T>(n); // ddof=0
        psi[j] = static_cast<T>(1);
    }
    const T llconst = static_cast<T>(p) * crd::math::log(static_cast<T>(6.283185307179586)) + static_cast<T>(nc);
    const T small = static_cast<T>(1e-12);
    bool first = true;
    T old_ll = static_cast<T>(0);
    for (crd::usize it = 0; it < max_iter; ++it)
    {
        for (crd::usize j = 0; j < p; ++j)
        {
            sqrt_psi[j] = crd::math::sqrt(psi[j]) + small;
        }
        crd::hesap::dense::Symmetric<T> m(alloc, p); // Xtilde^T Xtilde
        for (crd::usize a = 0; a < p; ++a)
        {
            for (crd::usize b = 0; b < p; ++b)
            {
                T s = static_cast<T>(0);
                for (crd::usize i = 0; i < n; ++i)
                {
                    s += xc[i * p + a] * xc[i * p + b];
                }
                m.at(a, b) = s / (sqrt_psi[a] * sqrt_psi[b] * static_cast<T>(n));
            }
        }
        auto e = crd::hesap::dense::eig_sym(alloc, m); // ascending eigenvalues = singular values^2
        T unexp = static_cast<T>(0);
        for (crd::usize mm = 0; mm + nc < p + 1 && mm < p - nc; ++mm)
        {
            unexp += e.values.data()[mm]; // smallest p-nc eigenvalues
        }
        T sum_log_s = static_cast<T>(0);
        for (crd::usize k = 0; k < nc; ++k)
        {
            const crd::usize src = p - 1 - k;
            const T lam = e.values.data()[src];
            sum_log_s += crd::math::log(lam);
            const T sq = crd::math::sqrt(lam - static_cast<T>(1) > static_cast<T>(0) ? lam - static_cast<T>(1)
                                                                                     : static_cast<T>(0));
            for (crd::usize j = 0; j < p; ++j)
            {
                w[k * p + j] = sq * e.vectors.at(j, src) * sqrt_psi[j];
            }
        }
        T sum_log_psi = static_cast<T>(0);
        for (crd::usize j = 0; j < p; ++j)
        {
            sum_log_psi += crd::math::log(psi[j]);
        }
        const T ll = (llconst + sum_log_s + unexp + sum_log_psi) * (-static_cast<T>(n) / static_cast<T>(2));
        if (!first && ll - old_ll < tol)
        {
            break;
        }
        first = false;
        old_ll = ll;
        for (crd::usize j = 0; j < p; ++j)
        {
            T sw = static_cast<T>(0);
            for (crd::usize k = 0; k < nc; ++k)
            {
                sw += w[k * p + j] * w[k * p + j];
            }
            psi[j] = var[j] - sw > small ? var[j] - sw : small;
        }
    }
    FactorAnalysisResult<T> r;
    r.components.resize(nc * p);
    r.noise_variance.resize(p);
    for (crd::usize j = 0; j < p; ++j)
    {
        r.noise_variance[j] = psi[j];
    }
    for (crd::usize k = 0; k < nc; ++k)
    {
        crd::usize mi = 0;
        T ma = static_cast<T>(0);
        for (crd::usize j = 0; j < p; ++j)
        {
            const T av = w[k * p + j] < static_cast<T>(0) ? -w[k * p + j] : w[k * p + j];
            if (av > ma)
            {
                ma = av;
                mi = j;
            }
        }
        const T sign = w[k * p + mi] >= static_cast<T>(0) ? static_cast<T>(1) : static_cast<T>(-1);
        for (crd::usize j = 0; j < p; ++j)
        {
            r.components[k * p + j] = sign * w[k * p + j];
        }
    }
    return r;
}

} // namespace crd::hesap::stats
