#include <crd/hesap/net/link_models.hpp>
#include <crd/hesap/special/erf.hpp>
#include <crd/math/cmath.hpp>

#include <limits>

namespace crd::hesap::net
{

namespace
{

constexpr crd::f64 kChanceOne = 4294967295.0;
constexpr crd::f64 kSqrtHalf = 0.70710678118654752440;
constexpr crd::f64 kSqrtThree = 1.73205080756887729353;
constexpr crd::f64 kInverseSqrtTwoPi = 0.39894228040143267794;
// The tc mix takes a share (three quarters) of a Pareto that iproute2's pareto.c scales by 4/3 at tail 3; in standard
// units (mean zero, deviation one) that is the share times this scale times the standard Pareto. Keeping it makes the
// mix at the default share and tail 3 the law of the `paretonormal` table, up to the one scale that gives it a
// deviation of one.
constexpr crd::f64 kMixParetoScale = 1.15470053837925152902;

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

[[nodiscard]] bool usable_table(crd::containers::ConstSpan<crd::i64> table) noexcept
{
    if (table.empty())
    {
        return false;
    }
    for (crd::usize i = 0U; i < table.size(); ++i)
    {
        if (!within_delay_range(table[i]) || (i != 0U && table[i] < table[i - 1U]))
        {
            return false;
        }
    }
    return true;
}

// The delay of an ascending table of equally spaced ranks at a rank in [0, 1], rounded to the nanosecond and held
// between the two entries the rank lies between. The interpolation runs in f64, whose 53 bits do not hold every entry
// of a table that reaches kMaxLinkDelayNs (2^62): converted, an entry can move by up to 2^8 ns past itself, so the
// rounded value is clamped to the segment's ends, first as doubles (so the conversion back is in range) and then as
// the exact integers. Every step is non-decreasing in the rank (the rounded products and sums, nearbyint and both
// clamps), and a segment ends at most at its upper entry, where the next one starts at least, so the delay stays
// non-decreasing in the rank across the whole table. Below 2^53 the entries are exact and neither clamp acts.
[[nodiscard]] crd::i64 table_at(crd::containers::ConstSpan<crd::i64> table, crd::f64 rank) noexcept
{
    const crd::f64 position = rank * static_cast<crd::f64>(table.size() - 1U);
    const crd::usize below = static_cast<crd::usize>(position);
    if (below + 1U >= table.size())
    {
        return table[table.size() - 1U];
    }
    const crd::i64 low_ns = table[below];
    const crd::i64 high_ns = table[below + 1U];
    const crd::f64 low = static_cast<crd::f64>(low_ns);
    const crd::f64 high = static_cast<crd::f64>(high_ns);
    const crd::f64 rounded = crd::math::nearbyint(low + ((position - static_cast<crd::f64>(below)) * (high - low)));
    crd::f64 held = rounded;
    if (held < low)
    {
        held = low;
    }
    else if (held > high)
    {
        held = high;
    }
    const crd::i64 value = static_cast<crd::i64>(held);
    if (value < low_ns)
    {
        return low_ns;
    }
    if (value > high_ns)
    {
        return high_ns;
    }
    return value;
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
        (pareto_shape && !(options.pareto_tail > 2.0 && options.pareto_tail <= 1e6)) ||
        (options.shape == DelayShape::ParetoNormal && !(options.pareto_share >= 0.0 && options.pareto_share <= 1.0)) ||
        (options.shape == DelayShape::Table && !usable_table(options.table)))
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
            m_mix_normal = 1.0 - options.pareto_share;
            m_mix_pareto = options.pareto_share * kMixParetoScale;
            m_shape_c = crd::math::sqrt((m_mix_normal * m_mix_normal) + (m_mix_pareto * m_mix_pareto) +
                                        (2.0 * m_mix_normal * m_mix_pareto * covariance));
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
    if (m_options.shape == DelayShape::Table)
    {
        // Held in integers: the f64 comparisons below would round a floor, a ceiling or an entry past 2^53. For a
        // value x and integer ends this equals their result on the rounded x everywhere the ends are exact.
        const crd::i64 value = table_at(m_options.table, 1.0 - survival(m_gaussian));
        if (value < m_options.floor_ns)
        {
            return m_options.floor_ns;
        }
        return value > m_options.ceiling_ns ? m_options.ceiling_ns : value;
    }
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
                delay =
                    mean + (deviation * (m_options.shape == DelayShape::Pareto
                                             ? pareto
                                             : ((m_mix_normal * m_gaussian) + (m_mix_pareto * pareto)) / m_shape_c));
                break;
            }
            case DelayShape::Table:
                break;
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

bool MediumGrants::init(const MediumGrantOptions& options, crd::i64 now_ns) noexcept
{
    if (options.hold_ns < 0 || options.gap.floor_ns < 0 || !m_gap.init(options.gap))
    {
        return false;
    }
    constexpr crd::u64 unlimited = std::numeric_limits<crd::u64>::max();
    m_hold_ns = options.hold_ns;
    m_packet_budget = options.packets == 0U ? unlimited : options.packets;
    m_byte_budget = options.bytes == 0U ? unlimited : options.bytes;
    m_packets_left = m_packet_budget;
    m_bytes_left = m_byte_budget;
    m_grant_ns = now_ns;
    return true;
}

void MediumGrants::open_after(crd::i64 from_ns) noexcept
{
    const crd::i64 gap = m_gap.next();
    constexpr crd::i64 far = std::numeric_limits<crd::i64>::max();
    m_grant_ns = from_ns > far - gap ? far : from_ns + gap;
    m_packets_left = m_packet_budget;
    m_bytes_left = m_byte_budget;
}

crd::i64 MediumGrants::depart(crd::i64 ready_ns, crd::u32 bytes) noexcept
{
    crd::i64 leaves = ready_ns > m_grant_ns ? ready_ns : m_grant_ns;
    const crd::u64 into_grant = static_cast<crd::u64>(leaves) - static_cast<crd::u64>(m_grant_ns);
    if (m_hold_ns != 0 && into_grant >= static_cast<crd::u64>(m_hold_ns))
    {
        open_after(ready_ns);
        leaves = m_grant_ns;
    }
    --m_packets_left;
    m_bytes_left = bytes < m_bytes_left ? m_bytes_left - bytes : 0U;
    if (m_packets_left == 0U || m_bytes_left == 0U)
    {
        open_after(leaves);
    }
    return leaves;
}

} // namespace crd::hesap::net
