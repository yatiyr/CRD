#include <crd/hesap/net/link_models.hpp>
#include <crd/hesap/special/erf.hpp>
#include <crd/math/cmath.hpp>

namespace crd::hesap::net
{

namespace
{

constexpr crd::f64 kChanceOne = 4294967295.0;
constexpr crd::f64 kSqrtHalf = 0.70710678118654752440;
constexpr crd::f64 kSqrtThree = 1.73205080756887729353;
constexpr crd::f64 kInverseSqrtTwoPi = 0.39894228040143267794;
// iproute2's pareto.c scales a Pareto of tail 3 by 4/3, which is this times its standard form; pareto_normal keeps that
// weight, so its law at tail 3 is netem's paretonormal.dist up to the one scale that gives it a deviation of one.
constexpr crd::f64 kNetemParetoWeight = 0.75 * 1.15470053837925152902;

// The chance a standard normal lies above `gaussian`.
[[nodiscard]] crd::f64 survival(crd::f64 gaussian) noexcept
{
    return 0.5 * crd::hesap::special::erfc(kSqrtHalf * gaussian);
}

// Covariance of a standard normal and the standard Pareto of the same rank, by Simpson's rule: the integrand decays as
// exp(-z^2 (1 - 1/tail) / 2), so with the tail above 2 the range and step hold it to far better than a delay needs.
[[nodiscard]] crd::f64 normal_pareto_covariance(crd::f64 tail, crd::f64 mean, crd::f64 deviation) noexcept
{
    constexpr crd::u32 intervals = 2048U;
    constexpr crd::f64 range = 12.0;
    const crd::f64 step = 2.0 * range / static_cast<crd::f64>(intervals);
    crd::f64 sum = 0.0;
    for (crd::u32 i = 0U; i <= intervals; ++i)
    {
        const crd::f64 z = -range + (step * static_cast<crd::f64>(i));
        const crd::f64 pareto = (crd::math::pow(survival(z), -1.0 / tail) - mean) / deviation;
        const crd::f64 inner_weight = (i % 2U) != 0U ? 4.0 : 2.0;
        const crd::f64 weight = (i == 0U || i == intervals) ? 1.0 : inner_weight;
        sum += weight * z * pareto * kInverseSqrtTwoPi * crd::math::exp(-0.5 * z * z);
    }
    return sum * step / 3.0;
}

[[nodiscard]] bool within_delay_range(crd::i64 value) noexcept
{
    return value >= -kMaxLinkDelayNs && value <= kMaxLinkDelayNs;
}

} // namespace

crd::u32 link_chance(crd::f64 fraction) noexcept
{
    if (!(fraction > 0.0))
    {
        return 0U;
    }
    if (fraction >= 1.0)
    {
        return 0xFFFFFFFFU;
    }
    return static_cast<crd::u32>(crd::math::nearbyint(fraction * kChanceOne));
}

bool gilbert_from_rate_and_burst(crd::f64 loss, crd::f64 mean_burst, GilbertElliottChances& out) noexcept
{
    if (!(loss >= 0.0 && loss < 1.0 && mean_burst >= 1.0))
    {
        return false;
    }
    const crd::f64 r = 1.0 / mean_burst;
    const crd::f64 p = loss * r / (1.0 - loss);
    if (!(p <= 1.0))
    {
        return false;
    }
    out = GilbertElliottChances{link_chance(p), link_chance(r), 0U, 0U};
    return true;
}

bool gilbert_from_rate_and_correlation(crd::f64 loss, crd::f64 correlation, GilbertElliottChances& out) noexcept
{
    if (!(loss >= 0.0 && loss < 1.0 && correlation >= 0.0 && correlation < 1.0))
    {
        return false;
    }
    out = GilbertElliottChances{link_chance(loss * (1.0 - correlation)),
                                link_chance((1.0 - loss) * (1.0 - correlation)), 0U, 0U};
    return true;
}

bool DelayProcess::init(const DelayOptions& options) noexcept
{
    const bool has_deviation = options.deviation_ns != 0;
    const bool pareto_shape = options.shape == DelayShape::Pareto || options.shape == DelayShape::ParetoNormal;
    if (!within_delay_range(options.mean_ns) || !within_delay_range(options.deviation_ns) || options.deviation_ns < 0 ||
        !(options.correlation >= 0.0 && options.correlation < 1.0) || options.floor_ns > options.ceiling_ns ||
        (options.shape == DelayShape::LogNormal && has_deviation && options.mean_ns <= 0) ||
        (pareto_shape && !(options.pareto_tail > 2.0 && options.pareto_tail <= 1e6)))
    {
        return false;
    }
    m_options = options;
    m_started = false;
    m_innovation = crd::math::sqrt(1.0 - (options.correlation * options.correlation));
    if (options.shape == DelayShape::LogNormal && has_deviation)
    {
        const crd::f64 ratio = static_cast<crd::f64>(options.deviation_ns) / static_cast<crd::f64>(options.mean_ns);
        const crd::f64 variance = crd::math::log1p(ratio * ratio);
        m_shape_a = crd::math::sqrt(variance);
        m_shape_b = -0.5 * variance;
    }
    if (pareto_shape)
    {
        const crd::f64 tail = options.pareto_tail;
        m_shape_a = tail / (tail - 1.0);
        m_shape_b = crd::math::sqrt(tail / (tail - 2.0)) / (tail - 1.0);
        if (options.shape == DelayShape::ParetoNormal)
        {
            // The mix's own deviation, so `deviation_ns` means what it does in every other shape.
            const crd::f64 covariance = normal_pareto_covariance(tail, m_shape_a, m_shape_b);
            m_shape_c = crd::math::sqrt(
                (0.0625 + (kNetemParetoWeight * kNetemParetoWeight) + (2.0 * 0.25 * kNetemParetoWeight * covariance)));
        }
    }
    return true;
}

crd::i64 DelayProcess::next() noexcept
{
    const crd::f64 fresh = m_normal.next();
    m_gaussian = m_started ? (m_options.correlation * m_gaussian) + (m_innovation * fresh) : fresh;
    m_started = true;

    const crd::f64 mean = static_cast<crd::f64>(m_options.mean_ns);
    const crd::f64 deviation = static_cast<crd::f64>(m_options.deviation_ns);
    crd::f64 delay = mean;
    if (m_options.deviation_ns != 0)
    {
        switch (m_options.shape)
        {
            case DelayShape::Uniform:
                delay = mean + (deviation * kSqrtThree * (1.0 - (2.0 * survival(m_gaussian))));
                break;
            case DelayShape::Normal:
                delay = mean + (deviation * m_gaussian);
                break;
            case DelayShape::LogNormal:
                delay = mean * crd::math::exp((m_shape_a * m_gaussian) + m_shape_b);
                break;
            case DelayShape::Pareto:
            case DelayShape::ParetoNormal:
            {
                const crd::f64 tail_rank = crd::math::pow(survival(m_gaussian), -1.0 / m_options.pareto_tail);
                const crd::f64 pareto = (tail_rank - m_shape_a) / m_shape_b;
                delay = mean + (deviation * (m_options.shape == DelayShape::Pareto
                                                 ? pareto
                                                 : ((0.25 * m_gaussian) + (kNetemParetoWeight * pareto)) / m_shape_c));
                break;
            }
        }
    }
    if (!(delay < static_cast<crd::f64>(m_options.ceiling_ns)))
    {
        return m_options.ceiling_ns;
    }
    if (delay <= static_cast<crd::f64>(m_options.floor_ns))
    {
        return m_options.floor_ns;
    }
    return static_cast<crd::i64>(crd::math::nearbyint(delay));
}

} // namespace crd::hesap::net
