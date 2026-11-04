// The block and convolutional interleavers (crd/hesap/comms/interleave.hpp).

#include <crd/core/assert.hpp>
#include <crd/hesap/comms/interleave.hpp>

#include <cstdint>
#include <cstring>
#include <limits>

namespace crd::hesap::comms
{
namespace
{

using crd::u32;
using crd::u64;
using crd::u8;
using crd::usize;

bool disjoint(std::span<const u8> a, std::span<const u8> b) noexcept
{
    const auto a0 = reinterpret_cast<std::uintptr_t>(a.data());
    const auto b0 = reinterpret_cast<std::uintptr_t>(b.data());
    return a.empty() || b.empty() || a0 + a.size() <= b0 || b0 + b.size() <= a0;
}

bool block_shape(std::span<const u8> in, std::span<u8> out, u32 rows, u32 columns, usize symbol_bytes) noexcept
{
    if (rows == 0U || columns == 0U || symbol_bytes == 0U || in.size() % symbol_bytes != 0U ||
        in.size() / symbol_bytes != u64{rows} * columns || out.size() < in.size())
    {
        return false;
    }
    return disjoint(in, out);
}

// Moves symbol i of `in` to place map(i) of `out`.
template <typename Map>
void permute(std::span<const u8> in, std::span<u8> out, u64 count, usize symbol_bytes, Map map) noexcept
{
    for (u64 i = 0U; i < count; ++i)
    {
        std::memcpy(out.data() + (map(i) * symbol_bytes), in.data() + (i * symbol_bytes), symbol_bytes);
    }
}

// The first symbol of branch b's ring among the M·B·(B − 1)/2 a side stores (b = 0 gives 0), and its length.
u64 ring_base(ConvolutionalInterleaver::Role role, u32 branches, u32 depth, u32 b) noexcept
{
    const u64 earlier = ((u64{b} * b) - b) / 2U; // 0 + 1 + .. + (b − 1)
    const u64 before =
        role == ConvolutionalInterleaver::Role::Interleave ? earlier : (u64{b} * (branches - 1U)) - earlier;
    return before * depth;
}

u64 ring_length(ConvolutionalInterleaver::Role role, u32 branches, u32 depth, u32 b) noexcept
{
    return u64{role == ConvolutionalInterleaver::Role::Interleave ? b : branches - 1U - b} * depth;
}

// The bytes one side stores, false when the parameters are outside init()'s range or the size outside usize.
bool sized(u32 branches, u32 depth, usize symbol_bytes, usize& bytes) noexcept
{
    if (branches < 1U || branches > ConvolutionalInterleaver::kMaxBranches || depth < 1U ||
        depth > ConvolutionalInterleaver::kMaxDepth || symbol_bytes == 0U)
    {
        return false;
    }
    const u64 symbols = (u64{branches} * (branches - 1U) / 2U) * depth;
    if (symbols != 0U && symbol_bytes > std::numeric_limits<usize>::max() / symbols)
    {
        return false;
    }
    bytes = static_cast<usize>(symbols) * symbol_bytes;
    return true;
}

} // namespace

bool block_interleave(std::span<const u8> in, std::span<u8> out, u32 rows, u32 columns, usize symbol_bytes) noexcept
{
    if (!block_shape(in, out, rows, columns, symbol_bytes))
    {
        return false;
    }
    permute(in, out, u64{rows} * columns, symbol_bytes,
            [rows, columns](u64 i) noexcept { return block_interleave_position(rows, columns, i); });
    return true;
}

bool block_deinterleave(std::span<const u8> in, std::span<u8> out, u32 rows, u32 columns, usize symbol_bytes) noexcept
{
    if (!block_shape(in, out, rows, columns, symbol_bytes))
    {
        return false;
    }
    permute(in, out, u64{rows} * columns, symbol_bytes,
            [rows, columns](u64 p) noexcept { return block_interleave_index(rows, columns, p); });
    return true;
}

usize ConvolutionalInterleaver::storage_bytes(u32 branches, u32 depth, usize symbol_bytes) noexcept
{
    usize bytes = 0U;
    return sized(branches, depth, symbol_bytes, bytes) ? bytes : 0U;
}

bool ConvolutionalInterleaver::init(Role role, u32 branches, u32 depth, usize symbol_bytes,
                                    std::span<u8> storage) noexcept
{
    usize bytes = 0U;
    if (!sized(branches, depth, symbol_bytes, bytes) || storage.size() != bytes)
    {
        return false;
    }
    if (!storage.empty())
    {
        std::memset(storage.data(), 0, storage.size());
    }
    m_storage = storage.data();
    m_symbol = symbol_bytes;
    m_turn = 0U;
    m_branches = branches;
    m_depth = depth;
    m_branch = 0U;
    m_role = role;
    return true;
}

void ConvolutionalInterleaver::push(std::span<const u8> in, std::span<u8> out) noexcept
{
    CRD_ASSERT(m_symbol != 0U && in.size() == m_symbol && out.size() == m_symbol);
    CRD_ASSERT(in.data() == out.data() || disjoint(in, out));
    const u64 length = ring_length(m_role, m_branches, m_depth, m_branch);
    if (length == 0U)
    {
        if (in.data() != out.data())
        {
            std::memcpy(out.data(), in.data(), m_symbol);
        }
    }
    else
    {
        const u64 slot = ring_base(m_role, m_branches, m_depth, m_branch) + (m_turn % length);
        u8* const held = m_storage + (slot * m_symbol);
        if (in.data() == out.data())
        {
            for (usize i = 0U; i < m_symbol; ++i)
            {
                const u8 leaving = held[i];
                held[i] = out[i];
                out[i] = leaving;
            }
        }
        else
        {
            std::memcpy(out.data(), held, m_symbol);
            std::memcpy(held, in.data(), m_symbol);
        }
    }
    if (++m_branch == m_branches)
    {
        m_branch = 0U;
        ++m_turn;
    }
}

} // namespace crd::hesap::comms
