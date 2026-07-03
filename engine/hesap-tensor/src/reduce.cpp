#include <crd/hesap/tensor/reduce.hpp>
#include <crd/math/cmath.hpp>

namespace crd::hesap::tensor
{

// log(sum(exp(x - max))) + max — the numerically safe form, on the engine's
// deterministic transcendentals (crd::math, never std::). The shifted sum
// runs a fixed left-to-right scalar order (documented: logsumexp's order is
// pinned but scalar; it is a value-gated op, not a bandwidth kernel).
crd::f64 reduce_logsumexp(const TensorView<const crd::f64>& v) noexcept
{
    CRD_ASSERT_MSG(v.size() > 0U && v.is_contiguous(), "reduce_logsumexp: non-empty contiguous view required");
    const crd::f64 m = reduce_max(v);
    if (detail::f64_nan_or_inf(m))
    {
        return m; // inf/nan dominates (matches scipy.special.logsumexp)
    }
    const crd::f64* p = v.data();
    crd::f64 acc = 0.0;
    for (crd::u64 i = 0; i < v.size(); ++i)
    {
        acc += crd::math::exp(p[i] - m);
    }
    return crd::math::log(acc) + m;
}

} // namespace crd::hesap::tensor
