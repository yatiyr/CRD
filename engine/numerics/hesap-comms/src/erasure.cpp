// RFC 5510 §8's systematic Reed-Solomon code over GF(2^8) (crd/hesap/comms/erasure.hpp). The Lagrange coefficients are
// formed in the logarithm domain: every factor is a difference of two distinct points alpha^a - alpha^b, never zero,
// so a product is the exponent of a sum of logarithms modulo 255.

#include <crd/core/assert.hpp>
#include <crd/hesap/comms/erasure.hpp>
#include <crd/hesap/comms/erasure_dispatch.hpp>
#include <crd/hesap/comms/gf256.hpp>

namespace crd::hesap::comms
{
namespace
{

using crd::u32;
using crd::u64;
using crd::u8;
using crd::usize;

// One dot call covers at most four outputs (one register group of the shuffle levels) and 32 sources, so its
// expanded coefficients take 5 KiB of stack; longer source lists accumulate chunk by chunk.
constexpr usize kOutputChunk = 4U;
constexpr usize kSourceChunk = 32U;

// Every coefficient expanded at compile time, so a block's encode or decode only copies them.
struct ExpandedCoefficients
{
    GfCoefficient of[256];
};

constexpr ExpandedCoefficients make_expanded_coefficients() noexcept
{
    ExpandedCoefficients expanded{};
    for (u32 c = 0U; c < 256U; ++c)
    {
        expanded.of[c] = gf256_expand(static_cast<u8>(c));
    }
    return expanded;
}

constexpr ExpandedCoefficients kExpanded = make_expanded_coefficients();

// The point of encoding symbol `index` < 255.
u8 point(u32 index) noexcept
{
    return gf256_alpha_pow(index);
}

// log(alpha^a - alpha^b) for a != b.
u32 difference_log(u32 a, u32 b) noexcept
{
    return kGf256.log[point(a) ^ point(b)];
}

// The logarithm of the barycentric weight 1 / prod_{m != t} (x_t - x_m) of each of `count` points.
void weight_logs(const u32* indices, usize count, u8* out) noexcept
{
    for (usize t = 0U; t < count; ++t)
    {
        u64 sum = 0U;
        for (usize m = 0U; m < count; ++m)
        {
            if (m != t)
            {
                sum += difference_log(indices[t], indices[m]);
            }
        }
        out[t] = static_cast<u8>((kGf256Order - (sum % kGf256Order)) % kGf256Order);
    }
}

// log prod_t (x - x_t) over the `count` points, x not among them.
u32 node_polynomial_log(u32 x, const u32* indices, usize count) noexcept
{
    u64 sum = 0U;
    for (usize t = 0U; t < count; ++t)
    {
        sum += difference_log(x, indices[t]);
    }
    return static_cast<u32>(sum % kGf256Order);
}

// Lagrange coefficient t of the points at x: exp(log l(x) + log w_t - log(x - x_t)).
u8 lagrange(u32 node_log, u32 weight_log, u32 x, u32 index) noexcept
{
    return gf256_alpha_pow(static_cast<u64>(node_log) + weight_log + kGf256Order - difference_log(x, index));
}

// Bit set over the 255 symbol indices.
struct IndexSet
{
    u64 words[4] = {};

    // Adds `index`; false when it was already present.
    bool insert(u32 index) noexcept
    {
        const u64 bit = u64{1U} << (index & 63U);
        u64& word = words[index >> 6U];
        const bool fresh = (word & bit) == 0U;
        word |= bit;
        return fresh;
    }

    [[nodiscard]] bool contains(u32 index) const noexcept
    {
        return (words[index >> 6U] & (u64{1U} << (index & 63U))) != 0U;
    }
};

// Each repair index in [k, n), none twice.
ErasureStatus check_repair(std::span<const u32> repair, u32 k, u32 n) noexcept
{
    IndexSet seen;
    for (const u32 j : repair)
    {
        if (j < k || j >= n)
        {
            return ErasureStatus::IndexOutOfRange;
        }
        if (!seen.insert(j))
        {
            return ErasureStatus::DuplicateIndex;
        }
    }
    return ErasureStatus::Ok;
}

// outputs[q] = sum_t coefficient(q, t)·inputs[t] over `count` inputs, for `targets` outputs at the points
// x_q = alpha^targets[q], with inputs at the points alpha^indices[t] whose weight logarithms are `weights`.
void interpolate(const u8* const* inputs, const u32* indices, const u8* weights, usize count, const u32* targets,
                 u8* const* outputs, usize target_count, usize bytes) noexcept
{
    const ErasureKernels& kernels = *erasure_kernels().table;
    GfCoefficient rows[kOutputChunk * kSourceChunk];
    for (usize q0 = 0U; q0 < target_count; q0 += kOutputChunk)
    {
        const usize nq = target_count - q0 < kOutputChunk ? target_count - q0 : kOutputChunk;
        u32 node_logs[kOutputChunk];
        for (usize q = 0U; q < nq; ++q)
        {
            node_logs[q] = node_polynomial_log(targets[q0 + q], indices, count);
        }
        for (usize s0 = 0U; s0 < count; s0 += kSourceChunk)
        {
            const usize ns = count - s0 < kSourceChunk ? count - s0 : kSourceChunk;
            for (usize q = 0U; q < nq; ++q)
            {
                for (usize s = 0U; s < ns; ++s)
                {
                    rows[(q * ns) + s] =
                        kExpanded.of[lagrange(node_logs[q], weights[s0 + s], targets[q0 + q], indices[s0 + s])];
                }
            }
            kernels.dot(rows, inputs + s0, ns, outputs + q0, nq, bytes, s0 != 0U);
        }
    }
}

} // namespace

ReedSolomonCode::ReedSolomonCode(u32 k, u32 n) noexcept : m_k(k), m_n(n), m_weight_logs{}
{
    CRD_ASSERT(valid(k, n));
    u32 indices[kMaxSymbols];
    for (u32 i = 0U; i < k; ++i)
    {
        indices[i] = i;
    }
    weight_logs(indices, k, m_weight_logs);
}

u8 ReedSolomonCode::coefficient(u32 i, u32 j) const noexcept
{
    CRD_ASSERT(i < m_k && j < m_n);
    if (j < m_k)
    {
        return i == j ? u8{1U} : u8{0U};
    }
    u64 node = 0U;
    for (u32 m = 0U; m < m_k; ++m)
    {
        node += difference_log(j, m);
    }
    return lagrange(static_cast<u32>(node % kGf256Order), m_weight_logs[i], j, i);
}

ErasureStatus ReedSolomonCode::encode(std::span<const u8* const> sources, std::span<const u32> repair,
                                      std::span<u8* const> outputs, usize bytes) const noexcept
{
    if (sources.size() != m_k || repair.size() != outputs.size())
    {
        return ErasureStatus::WrongCount;
    }
    const ErasureStatus status = check_repair(repair, m_k, m_n);
    if (status != ErasureStatus::Ok)
    {
        return status;
    }
    u32 indices[kMaxSymbols];
    for (u32 i = 0U; i < m_k; ++i)
    {
        indices[i] = i;
    }
    interpolate(sources.data(), indices, m_weight_logs, m_k, repair.data(), outputs.data(), repair.size(), bytes);
    return ErasureStatus::Ok;
}

ErasureStatus ReedSolomonCode::prepare_encode(std::span<const u32> repair,
                                              std::span<GfCoefficient> rows) const noexcept
{
    if (rows.size() != repair.size() * m_k)
    {
        return ErasureStatus::WrongCount;
    }
    const ErasureStatus status = check_repair(repair, m_k, m_n);
    if (status != ErasureStatus::Ok)
    {
        return status;
    }
    u32 indices[kMaxSymbols];
    for (u32 i = 0U; i < m_k; ++i)
    {
        indices[i] = i;
    }
    for (usize q = 0U; q < repair.size(); ++q)
    {
        const u32 node_log = node_polynomial_log(repair[q], indices, m_k);
        for (u32 s = 0U; s < m_k; ++s)
        {
            rows[(q * m_k) + s] = kExpanded.of[lagrange(node_log, m_weight_logs[s], repair[q], s)];
        }
    }
    return ErasureStatus::Ok;
}

ErasureStatus ReedSolomonCode::encode_prepared(std::span<const GfCoefficient> rows, std::span<const u8* const> sources,
                                               std::span<u8* const> outputs, usize bytes) const noexcept
{
    if (sources.size() != m_k || rows.size() != outputs.size() * m_k)
    {
        return ErasureStatus::WrongCount;
    }
    erasure_kernels().table->dot(rows.data(), sources.data(), m_k, outputs.data(), outputs.size(), bytes, false);
    return ErasureStatus::Ok;
}

ErasureStatus ReedSolomonCode::decode(std::span<const ErasureSymbol> received, std::span<const u32> lost,
                                      std::span<u8* const> outputs, usize bytes) const noexcept
{
    if (received.size() != m_k || lost.size() != outputs.size())
    {
        return ErasureStatus::WrongCount;
    }
    IndexSet have;
    u32 indices[kMaxSymbols];
    const u8* inputs[kMaxSymbols];
    for (usize t = 0U; t < received.size(); ++t)
    {
        if (received[t].index >= m_n)
        {
            return ErasureStatus::IndexOutOfRange;
        }
        if (!have.insert(received[t].index))
        {
            return ErasureStatus::DuplicateIndex;
        }
        indices[t] = received[t].index;
        inputs[t] = received[t].data;
    }
    IndexSet asked;
    for (const u32 i : lost)
    {
        if (i >= m_k)
        {
            return ErasureStatus::IndexOutOfRange;
        }
        if (!asked.insert(i))
        {
            return ErasureStatus::DuplicateIndex;
        }
        if (have.contains(i))
        {
            return ErasureStatus::NotLost;
        }
    }
    if (lost.empty())
    {
        return ErasureStatus::Ok;
    }
    u8 weights[kMaxSymbols];
    weight_logs(indices, m_k, weights);
    interpolate(inputs, indices, weights, m_k, lost.data(), outputs.data(), lost.size(), bytes);
    return ErasureStatus::Ok;
}

} // namespace crd::hesap::comms
