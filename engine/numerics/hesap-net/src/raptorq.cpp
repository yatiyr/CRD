// RFC 6330's RaptorQ code (crd/hesap/net/raptorq.hpp): the tables and generators of §5.3.5, the solver of §5.4.2 in
// block form, and the object layer of §3 and §4.

#include <crd/containers/wire_bytes.hpp>
#include <crd/core/assert.hpp>
#include <crd/hesap/net/erasure_dispatch.hpp>
#include <crd/hesap/net/gf256.hpp>
#include <crd/hesap/net/raptorq.hpp>

#include <cstdint>
#include <cstring>

namespace crd::hesap::net
{
namespace
{

using crd::u16;
using crd::u32;
using crd::u64;
using crd::u8;
using crd::usize;

#include "raptorq_tables.inc"

constexpr u32 kNoRow = 0xFFFFFFFFU;
constexpr u32 kColumnActive = 0xFFFFFFFFU;
constexpr u32 kColumnInactivated = 0x80000000U; // with the column's u index below it; otherwise its step
constexpr u32 kRowChosen = 0xFFFFFFFFU;         // as a row's weight
constexpr u32 kMaxHdpcRows = 32U;               // Table 2's largest H is 16; the unit tests hold it
constexpr usize kDotBatch = 16U;
constexpr u32 kBuckets = 64U;
constexpr u8 kAlpha = 2U;

struct ByteSpread
{
    u8 of[256][8]; // byte b of a bit row as eight octets of zero and one
};

constexpr ByteSpread make_byte_spread() noexcept
{
    ByteSpread spread{};
    for (u32 b = 0U; b < 256U; ++b)
    {
        for (u32 i = 0U; i < 8U; ++i)
        {
            spread.of[b][i] = static_cast<u8>((b >> i) & 1U);
        }
    }
    return spread;
}

constexpr ByteSpread kByteSpread = make_byte_spread();

[[nodiscard]] bool is_prime(u32 n) noexcept
{
    if (n < 2U)
    {
        return false;
    }
    for (u32 f = 2U; f * f <= n; ++f)
    {
        if (n % f == 0U)
        {
            return false;
        }
    }
    return true;
}

// `first` moved up to the next multiple of 8, which every sizing call allows for.
[[nodiscard]] u8* aligned_to_words(u8* first) noexcept
{
    const usize misaligned = reinterpret_cast<std::uintptr_t>(first) % 8U;
    return misaligned == 0U ? first : first + (8U - misaligned);
}

// Hands out arrays of a size already checked, widest first so each is aligned.
struct Carver
{
    u8* at;

    template <typename T> [[nodiscard]] T* take(usize count) noexcept
    {
        T* first = reinterpret_cast<T*>(at);
        at += count * sizeof(T);
        return first;
    }
};

// output = the sum of sources, in one pass of the kernel per batch, each vector stored once a pass. A copy or zeroing
// before additions costs far more than its bytes: the wide load of what it just wrote cannot forward from its smaller
// stores, and that stall held RaptorQ's small blocks behind nanorq (docs/bench/2026-11-07-hesap-net-raptorq-stalls.md).
class SumBatch
{
public:
    SumBatch(const ErasureKernels& kernels, u8* output, usize bytes) noexcept
        : m_kernels(kernels), m_output(output), m_bytes(bytes)
    {
    }

    void add(const u8* source) noexcept
    {
        m_sources[m_count] = source;
        ++m_count;
        if (m_count == kDotBatch)
        {
            flush();
        }
    }

    void flush() noexcept
    {
        if (m_count != 0U)
        {
            m_kernels.sum(m_output, m_sources, m_count, m_bytes, m_written);
            m_written = true;
            m_count = 0U;
        }
    }

    // The sum of what was added, zero when nothing was.
    void finish() noexcept
    {
        flush();
        if (!m_written)
        {
            std::memset(m_output, 0, m_bytes);
        }
    }

private:
    const ErasureKernels& m_kernels;
    u8* m_output;
    usize m_bytes;
    bool m_written = false;
    usize m_count = 0U;
    const u8* m_sources[kDotBatch];
};

// output = (output when accumulating) + the sum of coefficient[i]*source[i], in batches of the kernel's rows. A
// coefficient of one, all a binary row's are, is an addition, summed last.
class DotBatch
{
public:
    DotBatch(const ErasureKernels& kernels, u8* output, usize bytes, bool accumulate) noexcept
        : m_kernels(kernels), m_output(output), m_bytes(bytes), m_written(accumulate)
    {
    }

    void add(u8 coefficient, const u8* source) noexcept
    {
        if (coefficient == 1U)
        {
            m_ones[m_one_count] = source;
            ++m_one_count;
            if (m_one_count == kDotBatch)
            {
                flush_ones();
            }
            return;
        }
        m_row[m_count] = gf256_expanded(coefficient);
        m_sources[m_count] = source;
        ++m_count;
        if (m_count == kDotBatch)
        {
            flush_products();
        }
    }

    void flush() noexcept
    {
        flush_products();
        flush_ones();
    }

private:
    void flush_products() noexcept
    {
        if (m_count != 0U)
        {
            m_kernels.dot(m_row, m_sources, m_count, &m_output, 1U, m_bytes, m_written);
            m_written = true;
            m_count = 0U;
        }
    }

    void flush_ones() noexcept
    {
        if (m_one_count != 0U)
        {
            m_kernels.sum(m_output, m_ones, m_one_count, m_bytes, m_written);
            m_written = true;
            m_one_count = 0U;
        }
    }

    const ErasureKernels& m_kernels;
    u8* m_output;
    usize m_bytes;
    bool m_written; // whether the output holds a value to add to
    usize m_count = 0U;
    usize m_one_count = 0U;
    GfCoefficient m_row[kDotBatch];
    const u8* m_sources[kDotBatch];
    const u8* m_ones[kDotBatch];
};

// f(column) for every intermediate symbol of a tuple, the LT symbols then the PI symbols (§5.3.5.3).
template <typename F> void for_each_column(const RaptorqParameters& p, const RaptorqTuple& tuple, const F& f) noexcept
{
    u32 b = tuple.b;
    f(b);
    for (u32 j = 1U; j < tuple.d; ++j)
    {
        b = (b + tuple.a) % p.w;
        f(b);
    }
    u32 b1 = tuple.b1;
    while (b1 >= p.p)
    {
        b1 = (b1 + tuple.a1) % p.p1;
    }
    f(p.w + b1);
    for (u32 j = 1U; j < tuple.d1; ++j)
    {
        b1 = (b1 + tuple.a1) % p.p1;
        while (b1 >= p.p)
        {
            b1 = (b1 + tuple.a1) % p.p1;
        }
        f(p.w + b1);
    }
}

// The rows, entries and entries in V of a matrix.
struct MatrixShape
{
    u32 binary_rows = 0U;
    u64 entries = 0U;
    u64 entries_in_v = 0U;
};

// row r >= S's ISI: an encoding symbol's, then the padding's.
[[nodiscard]] u32 row_isi(const RaptorqParameters& p, u32 k, const u32* esis, u32 received, u32 index) noexcept
{
    if (index >= received)
    {
        return k + (index - received);
    }
    return esis != nullptr ? raptorq_isi(p, k, esis[index]) : index;
}

[[nodiscard]] MatrixShape matrix_shape(const RaptorqParameters& p, u32 k, const u32* esis, u32 received) noexcept
{
    const u32 padding = p.k_prime - k;
    MatrixShape shape;
    shape.binary_rows = p.s + received + padding;
    shape.entries_in_v = (u64{3U} * p.b) + p.s;
    shape.entries = shape.entries_in_v + (u64{2U} * p.s);
    for (u32 index = 0U; index < received + padding; ++index)
    {
        const RaptorqTuple tuple = raptorq_tuple(p, row_isi(p, k, esis, received, index));
        shape.entries_in_v += tuple.d;
        shape.entries += u64{tuple.d} + tuple.d1;
    }
    return shape;
}

[[nodiscard]] usize shape_bytes(const RaptorqParameters& p, const MatrixShape& shape) noexcept
{
    const usize rows = shape.binary_rows;
    // row_start, split, end, the row arrays of the first phase and the buckets; steps; columns; MT.
    const usize words = (rows + 1U) + (rows * 8U) + (usize{p.w} * 2U) + (usize{p.l} * 2U) + (usize{p.w} + 1U) +
                        (usize{p.w} * 2U) + static_cast<usize>(shape.entries) + static_cast<usize>(shape.entries_in_v) +
                        64U;
    return 8U + (words * sizeof(u32)) + (usize{p.k_prime + p.s} * 2U);
}

// The bucket of a row by its nonzeros in V: the last holds every larger number.
[[nodiscard]] u32 bucket_of(u32 weight) noexcept
{
    return weight < kBuckets ? weight : kBuckets - 1U;
}

// Refuses the ESIs a request can be refused for without memory.
[[nodiscard]] RaptorqStatus check_rows(u32 k, std::span<const u32> esis, RaptorqParameters& p) noexcept
{
    if (!raptorq_parameters(k, p))
    {
        return RaptorqStatus::BadBlockSize;
    }
    for (const u32 esi : esis)
    {
        if (esi >= kRaptorqEsiLimit)
        {
            return RaptorqStatus::EsiOutOfRange;
        }
    }
    if (esis.size() + (p.k_prime - k) < p.k_prime)
    {
        return RaptorqStatus::TooFewSymbols;
    }
    if (esis.size() > kRaptorqEsiLimit)
    {
        return RaptorqStatus::DuplicateEsi; // more symbols than there are ESIs
    }
    return RaptorqStatus::Ok;
}

// Whether two of `esis` are equal: three byte passes of an LSD radix sort, whatever an attacker chose.
[[nodiscard]] bool has_duplicate(std::span<const u32> esis, u32* first, u32* second) noexcept
{
    const u32* in = esis.data();
    u32* out = first;
    u32* spare = second;
    for (u32 pass = 0U; pass < 3U; ++pass)
    {
        const u32 shift = pass * 8U;
        u32 counts[256] = {};
        for (usize i = 0U; i < esis.size(); ++i)
        {
            ++counts[(in[i] >> shift) & 255U];
        }
        u32 total = 0U;
        for (u32& count : counts)
        {
            const u32 here = count;
            count = total;
            total += here;
        }
        for (usize i = 0U; i < esis.size(); ++i)
        {
            out[counts[(in[i] >> shift) & 255U]++] = in[i];
        }
        in = out;
        u32* const next = spare;
        spare = out;
        out = next;
    }
    for (usize i = 1U; i < esis.size(); ++i)
    {
        if (in[i] == in[i - 1U])
        {
            return true;
        }
    }
    return false;
}

} // namespace

std::span<const RaptorqSystematicRow> raptorq_systematic_rows() noexcept
{
    return kSystematicRows;
}

bool raptorq_parameters(u32 k, RaptorqParameters& out) noexcept
{
    if (k < 1U || k > kRaptorqMaxSourceSymbols)
    {
        return false;
    }
    usize low = 0U;
    usize high = (sizeof(kSystematicRows) / sizeof(kSystematicRows[0])) - 1U;
    while (low < high)
    {
        const usize middle = low + ((high - low) / 2U);
        if (kSystematicRows[middle].k_prime < k)
        {
            low = middle + 1U;
        }
        else
        {
            high = middle;
        }
    }
    const RaptorqSystematicRow& row = kSystematicRows[low];
    out.k_prime = row.k_prime;
    out.j = row.j;
    out.s = row.s;
    out.h = row.h;
    out.w = row.w;
    out.l = out.k_prime + out.s + out.h;
    out.p = out.l - out.w;
    out.u = out.p - out.h;
    out.b = out.w - out.s;
    out.p1 = out.p;
    while (!is_prime(out.p1))
    {
        ++out.p1;
    }
    return true;
}

u32 raptorq_rand(u32 y, u32 i, u32 m) noexcept
{
    const u32 x0 = (y + i) & 255U;
    const u32 x1 = ((y >> 8U) + i) & 255U;
    const u32 x2 = ((y >> 16U) + i) & 255U;
    const u32 x3 = ((y >> 24U) + i) & 255U;
    return (kRandomArrays[0][x0] ^ kRandomArrays[1][x1] ^ kRandomArrays[2][x2] ^ kRandomArrays[3][x3]) % m;
}

u32 raptorq_degree(u32 v, u32 w) noexcept
{
    u32 d = 1U;
    while (kDegreeBounds[d] <= v)
    {
        ++d;
    }
    return d < w - 2U ? d : w - 2U;
}

RaptorqTuple raptorq_tuple(const RaptorqParameters& p, u32 isi) noexcept
{
    u32 a = 53591U + (p.j * 997U);
    if (a % 2U == 0U)
    {
        a += 1U;
    }
    const u32 b = 10267U * (p.j + 1U);
    const u32 y = b + (isi * a); // modulo 2^32
    RaptorqTuple tuple;
    tuple.d = raptorq_degree(raptorq_rand(y, 0U, 1U << 20U), p.w);
    tuple.a = 1U + raptorq_rand(y, 1U, p.w - 1U);
    tuple.b = raptorq_rand(y, 2U, p.w);
    tuple.d1 = tuple.d < 4U ? 2U + raptorq_rand(isi, 3U, 2U) : 2U;
    tuple.a1 = 1U + raptorq_rand(isi, 4U, p.p1 - 1U);
    tuple.b1 = raptorq_rand(isi, 5U, p.p1);
    return tuple;
}

void raptorq_symbol(const RaptorqParameters& p, const u8* intermediate, usize bytes, u32 isi, u8* output) noexcept
{
    if (bytes != 0U)
    {
        SumBatch batch(*erasure_kernels().table, output, bytes);
        for_each_column(p, raptorq_tuple(p, isi),
                        [&](u32 column) noexcept { batch.add(intermediate + (usize{column} * bytes)); });
        batch.finish();
    }
}

usize RaptorqSolver::structure_bytes(u32 k) noexcept
{
    RaptorqParameters p;
    if (!raptorq_parameters(k, p))
    {
        return 0U;
    }
    return shape_bytes(p, matrix_shape(p, k, nullptr, k));
}

usize RaptorqSolver::structure_bytes(u32 k, std::span<const u32> esis) noexcept
{
    RaptorqParameters p;
    if (check_rows(k, esis, p) != RaptorqStatus::Ok)
    {
        return 0U;
    }
    return shape_bytes(p, matrix_shape(p, k, esis.data(), static_cast<u32>(esis.size())));
}

RaptorqStatus RaptorqSolver::analyze(u32 k, std::span<u8> structure) noexcept
{
    m_analyzed = false;
    m_factored = false;
    RaptorqParameters p;
    if (!raptorq_parameters(k, p))
    {
        return RaptorqStatus::BadBlockSize;
    }
    m_parameters = p;
    return analyze_rows(k, nullptr, k, structure);
}

RaptorqStatus RaptorqSolver::analyze(u32 k, std::span<const u32> esis, std::span<u8> structure) noexcept
{
    m_analyzed = false;
    m_factored = false;
    RaptorqParameters p;
    const RaptorqStatus status = check_rows(k, esis, p);
    if (status != RaptorqStatus::Ok)
    {
        return status;
    }
    m_parameters = p;
    return analyze_rows(k, esis.data(), static_cast<u32>(esis.size()), structure);
}

RaptorqStatus RaptorqSolver::analyze_rows(u32 k, const u32* esis, u32 received, std::span<u8> structure) noexcept
{
    const RaptorqParameters& p = m_parameters;
    const MatrixShape shape = matrix_shape(p, k, esis, received);
    if (structure.size() < shape_bytes(p, shape))
    {
        return RaptorqStatus::WorkspaceTooSmall;
    }

    const u32 rows = shape.binary_rows;
    Carver carver{aligned_to_words(structure.data())};
    m_row_start = carver.take<u32>(rows + 1U);
    m_row_split = carver.take<u32>(rows);
    m_row_end = carver.take<u32>(rows);
    m_row_columns = carver.take<u32>(static_cast<usize>(shape.entries));
    m_chosen_row = carver.take<u32>(p.w);
    m_chosen_column = carver.take<u32>(p.w);
    m_column_state = carver.take<u32>(p.l);
    m_u_column = carver.take<u32>(p.l);
    m_schur_row = carver.take<u32>(rows);
    m_column_start = carver.take<u32>(usize{p.w} + 1U);
    m_column_rows = carver.take<u32>(static_cast<usize>(shape.entries_in_v));
    m_row_weight = carver.take<u32>(rows);
    m_row_degree = carver.take<u32>(rows);
    m_row_xor = carver.take<u32>(rows);
    m_row_next = carver.take<u32>(rows);
    m_row_prev = carver.take<u32>(rows);
    m_bucket_head = carver.take<u32>(kBuckets);
    m_set_parent = carver.take<u32>(p.w);
    m_set_size = carver.take<u32>(p.w);
    m_hdpc_first = carver.take<u8>(usize{p.k_prime} + p.s);
    m_hdpc_second = carver.take<u8>(usize{p.k_prime} + p.s);

    if (esis != nullptr && has_duplicate(std::span<const u32>(esis, received), m_row_next, m_row_prev))
    {
        return RaptorqStatus::DuplicateEsi;
    }

    m_k = k;
    m_received = received;
    m_binary_rows = rows;

    // The LDPC rows of §5.3.3.3: three entries of each of the B columns, its identity and two PI symbols per row.
    for (u32 r = 0U; r < p.s; ++r)
    {
        m_row_end[r] = 3U;
    }
    for (u32 i = 0U; i < p.b; ++i)
    {
        const u32 a = 1U + (i / p.s);
        u32 b = i % p.s;
        ++m_row_end[b];
        b = (b + a) % p.s;
        ++m_row_end[b];
        b = (b + a) % p.s;
        ++m_row_end[b];
    }
    u32 cursor = 0U;
    for (u32 r = 0U; r < p.s; ++r)
    {
        const u32 length = m_row_end[r];
        m_row_start[r] = cursor;
        m_row_end[r] = cursor;
        cursor += length;
    }
    for (u32 i = 0U; i < p.b; ++i)
    {
        const u32 a = 1U + (i / p.s);
        u32 b = i % p.s;
        m_row_columns[m_row_end[b]++] = i;
        b = (b + a) % p.s;
        m_row_columns[m_row_end[b]++] = i;
        b = (b + a) % p.s;
        m_row_columns[m_row_end[b]++] = i;
    }
    for (u32 i = 0U; i < p.s; ++i)
    {
        m_row_columns[m_row_end[i]++] = p.b + i;
        m_row_columns[m_row_end[i]++] = p.w + (i % p.p);
        m_row_columns[m_row_end[i]++] = p.w + ((i + 1U) % p.p);
    }

    // One row per encoding symbol and padding symbol.
    for (u32 index = 0U; index < rows - p.s; ++index)
    {
        const u32 r = p.s + index;
        m_row_start[r] = cursor;
        for_each_column(p, raptorq_tuple(p, row_isi(p, k, esis, received, index)),
                        [&](u32 column) noexcept { m_row_columns[cursor++] = column; });
        m_row_end[r] = cursor;
    }
    m_row_start[rows] = cursor;

    // MT's two rows of every column but the last.
    for (u32 j = 0U; j + 1U < p.k_prime + p.s; ++j)
    {
        const u32 h1 = raptorq_rand(j + 1U, 6U, p.h);
        m_hdpc_first[j] = static_cast<u8>(h1);
        m_hdpc_second[j] = static_cast<u8>((h1 + raptorq_rand(j + 1U, 7U, p.h - 1U) + 1U) % p.h);
    }

    select_rows();
    partition_entries();
    m_analyzed = true;
    return RaptorqStatus::Ok;
}

void RaptorqSolver::unlink_row(u32 row) noexcept
{
    const u32 weight = m_row_weight[row];
    if (weight == 0U)
    {
        return;
    }
    const u32 next = m_row_next[row];
    const u32 prev = m_row_prev[row];
    if (prev == kNoRow)
    {
        m_bucket_head[bucket_of(weight)] = next;
    }
    else
    {
        m_row_next[prev] = next;
    }
    if (next != kNoRow)
    {
        m_row_prev[next] = prev;
    }
}

void RaptorqSolver::link_row(u32 row) noexcept
{
    const u32 weight = m_row_weight[row];
    if (weight == 0U)
    {
        return;
    }
    u32& head = m_bucket_head[bucket_of(weight)];
    m_row_next[row] = head;
    m_row_prev[row] = kNoRow;
    if (head != kNoRow)
    {
        m_row_prev[head] = row;
    }
    head = row;
}

// `column` leaves V: every row still in play has one nonzero fewer there.
void RaptorqSolver::retire_column(u32 column) noexcept
{
    --m_active_columns;
    for (u32 e = m_column_start[column]; e < m_column_start[column + 1U]; ++e)
    {
        const u32 row = m_column_rows[e];
        if (m_row_weight[row] != kRowChosen)
        {
            unlink_row(row);
            --m_row_weight[row];
            m_row_xor[row] ^= column;
            link_row(row);
        }
    }
}

// The step that makes `row`, already out of its bucket and marked, solve `diagonal`.
void RaptorqSolver::choose_row(u32 row, u32 diagonal) noexcept
{
    m_chosen_row[m_chosen] = row;
    m_chosen_column[m_chosen] = diagonal;
    m_column_state[diagonal] = m_chosen;
    ++m_chosen;
    retire_column(diagonal);
}

// §5.4.2.2 for r = 2: a row of the largest component of the graph whose vertices are the columns of V and whose edges
// are the rows with two nonzeros there.
u32 RaptorqSolver::pick_degree_two_row() noexcept
{
    const u32 w = m_parameters.w;
    const auto first_column = [&](u32 row) noexcept
    {
        u32 e = m_row_start[row];
        while (m_row_columns[e] >= w || m_column_state[m_row_columns[e]] != kColumnActive)
        {
            ++e;
        }
        return m_row_columns[e];
    };
    const auto find = [&](u32 x) noexcept
    {
        while (m_set_parent[x] != x)
        {
            m_set_parent[x] = m_set_parent[m_set_parent[x]];
            x = m_set_parent[x];
        }
        return x;
    };
    for (u32 row = m_bucket_head[2]; row != kNoRow; row = m_row_next[row])
    {
        const u32 a = first_column(row);
        const u32 b = m_row_xor[row] ^ a;
        m_set_parent[a] = a;
        m_set_parent[b] = b;
        m_set_size[a] = 1U;
        m_set_size[b] = 1U;
    }
    u32 best_size = 0U;
    for (u32 row = m_bucket_head[2]; row != kNoRow; row = m_row_next[row])
    {
        const u32 a0 = first_column(row);
        u32 a = find(a0);
        u32 b = find(m_row_xor[row] ^ a0);
        if (a != b)
        {
            if (m_set_size[a] < m_set_size[b])
            {
                const u32 swap = a;
                a = b;
                b = swap;
            }
            m_set_parent[b] = a;
            m_set_size[a] += m_set_size[b];
        }
        if (m_set_size[a] > best_size)
        {
            best_size = m_set_size[a];
        }
    }
    for (u32 row = m_bucket_head[2]; row != kNoRow; row = m_row_next[row])
    {
        if (m_set_size[find(first_column(row))] == best_size)
        {
            return row;
        }
    }
    return kNoRow; // not reached: some row's component has the size just found
}

// §5.4.2.2 for r > 2: the fewest nonzeros in V, then the lowest original degree. The last bucket holds every larger r.
u32 RaptorqSolver::pick_sparsest_row(u32 bucket) const noexcept
{
    u32 best = kNoRow;
    for (u32 row = m_bucket_head[bucket]; row != kNoRow; row = m_row_next[row])
    {
        if (best == kNoRow || m_row_weight[row] < m_row_weight[best] ||
            (m_row_weight[row] == m_row_weight[best] && m_row_degree[row] < m_row_degree[best]))
        {
            best = row;
        }
    }
    return best;
}

void RaptorqSolver::select_rows() noexcept
{
    const RaptorqParameters& p = m_parameters;

    for (u32 c = 0U; c <= p.w; ++c)
    {
        m_column_start[c] = 0U;
    }
    for (u32 c = 0U; c < p.l; ++c)
    {
        m_column_state[c] = kColumnActive;
    }
    m_u = 0U;
    for (u32 pi = 0U; pi < p.p; ++pi)
    {
        m_column_state[p.w + pi] = kColumnInactivated | m_u;
        m_u_column[m_u] = p.w + pi;
        ++m_u;
    }
    for (u32 b = 0U; b < kBuckets; ++b)
    {
        m_bucket_head[b] = kNoRow;
    }

    for (u32 r = 0U; r < m_binary_rows; ++r)
    {
        u32 weight = 0U;
        u32 sum = 0U;
        for (u32 e = m_row_start[r]; e < m_row_end[r]; ++e)
        {
            const u32 column = m_row_columns[e];
            if (column < p.w)
            {
                ++weight;
                sum ^= column;
                ++m_column_start[column + 1U];
            }
        }
        m_row_weight[r] = weight;
        m_row_degree[r] = weight;
        m_row_xor[r] = sum;
        link_row(r);
    }
    for (u32 c = 0U; c < p.w; ++c)
    {
        m_column_start[c + 1U] += m_column_start[c];
    }
    for (u32 c = 0U; c < p.w; ++c)
    {
        m_set_size[c] = 0U; // here the fill of each column
    }
    for (u32 r = 0U; r < m_binary_rows; ++r)
    {
        for (u32 e = m_row_start[r]; e < m_row_end[r]; ++e)
        {
            const u32 column = m_row_columns[e];
            if (column < p.w)
            {
                m_column_rows[m_column_start[column] + m_set_size[column]] = r;
                ++m_set_size[column];
            }
        }
    }

    m_active_columns = p.w;
    m_chosen = 0U;
    while (m_active_columns != 0U)
    {
        u32 row = m_bucket_head[1];
        if (row != kNoRow)
        {
            const u32 diagonal = m_row_xor[row];
            unlink_row(row);
            m_row_weight[row] = kRowChosen;
            choose_row(row, diagonal);
            continue;
        }
        if (m_bucket_head[2] != kNoRow)
        {
            row = pick_degree_two_row();
        }
        else
        {
            for (u32 b = 3U; b < kBuckets && row == kNoRow; ++b)
            {
                row = pick_sparsest_row(b);
            }
        }
        if (row == kNoRow)
        {
            break; // no binary row has a nonzero left in V
        }
        unlink_row(row);
        m_row_weight[row] = kRowChosen;

        // The column in the fewest rows keeps the diagonal; the others, §5.4.2.2's r - 1, join U.
        u32 diagonal = kColumnActive;
        u32 lowest = 0xFFFFFFFFU;
        for (u32 e = m_row_start[row]; e < m_row_end[row]; ++e)
        {
            const u32 column = m_row_columns[e];
            if (column < p.w && m_column_state[column] == kColumnActive)
            {
                const u32 rows_of_column = m_column_start[column + 1U] - m_column_start[column];
                if (rows_of_column < lowest)
                {
                    lowest = rows_of_column;
                    diagonal = column;
                }
            }
        }
        for (u32 e = m_row_start[row]; e < m_row_end[row]; ++e)
        {
            const u32 column = m_row_columns[e];
            if (column < p.w && column != diagonal && m_column_state[column] == kColumnActive)
            {
                m_column_state[column] = kColumnInactivated | m_u;
                m_u_column[m_u] = column;
                ++m_u;
                retire_column(column);
            }
        }
        choose_row(row, diagonal);
    }

    // Columns no row can reach join U too: the HDPC rows pin them, or S is singular.
    for (u32 c = 0U; c < p.w; ++c)
    {
        if (m_column_state[c] == kColumnActive)
        {
            m_column_state[c] = kColumnInactivated | m_u;
            m_u_column[m_u] = c;
            ++m_u;
        }
    }

    u32 left = 0U;
    for (u32 r = 0U; r < m_binary_rows; ++r)
    {
        if (m_row_weight[r] != kRowChosen)
        {
            m_schur_row[left] = r;
            ++left;
        }
    }
    m_schur_rows = left + p.h;
}

// Each row's entries in the order of the header's comment, so no later pass looks at a column's state to classify one.
void RaptorqSolver::partition_entries() noexcept
{
    const auto partition = [&](u32 row, u32 diagonal) noexcept
    {
        u32 lower = m_row_start[row];
        u32 end = m_row_end[row];
        u32 e = lower;
        while (e < end)
        {
            const u32 column = m_row_columns[e];
            if (column == diagonal)
            {
                --end;
                m_row_columns[e] = m_row_columns[end];
                m_row_columns[end] = column;
            }
            else
            {
                if ((m_column_state[column] & kColumnInactivated) == 0U)
                {
                    m_row_columns[e] = m_row_columns[lower];
                    m_row_columns[lower] = column;
                    ++lower;
                }
                ++e;
            }
        }
        m_row_split[row] = lower;
        m_row_end[row] = end;
    };
    for (u32 t = 0U; t < m_chosen; ++t)
    {
        partition(m_chosen_row[t], m_chosen_column[t]);
    }
    for (u32 s = 0U; s + m_parameters.h < m_schur_rows; ++s)
    {
        partition(m_schur_row[s], kColumnActive);
    }
}

usize RaptorqSolver::factor_bytes() const noexcept
{
    if (!m_analyzed)
    {
        return 0U;
    }
    const usize words_per_row = (usize{m_u} + 63U) / 64U;
    const usize spread_bytes = words_per_row * 64U; // an octet per bit, in whole words
    return 8U + (usize{m_chosen} * words_per_row * sizeof(u64)) + (words_per_row * sizeof(u64)) +
           (usize{m_schur_rows} * sizeof(u32)) + (usize{m_schur_rows} * m_u) + (spread_bytes * 2U);
}

RaptorqStatus RaptorqSolver::factor(std::span<u8> memory) noexcept
{
    if (!m_analyzed)
    {
        return RaptorqStatus::BadSequence;
    }
    m_factored = false;
    if (memory.size() < factor_bytes())
    {
        return RaptorqStatus::WorkspaceTooSmall;
    }
    const RaptorqParameters& p = m_parameters;
    const ErasureKernels& kernels = *erasure_kernels().table;
    const usize words_per_row = (usize{m_u} + 63U) / 64U;
    const usize spread_bytes = words_per_row * 64U;
    const u32 u = m_u;
    const u32 binary_left = m_schur_rows - p.h;

    Carver carver{aligned_to_words(memory.data())};
    u64* const forward = carver.take<u64>(usize{m_chosen} * words_per_row); // V1^-1·U1 by rows
    u64* const row_bits = carver.take<u64>(words_per_row);
    m_pivot_row = carver.take<u32>(m_schur_rows);
    m_schur = carver.take<u8>(usize{m_schur_rows} * u);
    u8* z = carver.take<u8>(spread_bytes);
    u8* z_next = carver.take<u8>(spread_bytes);

    const auto accumulate_row = [&](u32 row, u64* bits) noexcept
    {
        for (u32 e = m_row_start[row]; e < m_row_split[row]; ++e)
        {
            const u64* const earlier = forward + (usize{m_column_state[m_row_columns[e]]} * words_per_row);
            for (usize w = 0U; w < words_per_row; ++w)
            {
                bits[w] ^= earlier[w];
            }
        }
        for (u32 e = m_row_split[row]; e < m_row_end[row]; ++e)
        {
            const u32 index = m_column_state[m_row_columns[e]] & ~kColumnInactivated;
            bits[index / 64U] ^= u64{1U} << (index % 64U);
        }
    };
    const auto spread_into = [&](const u64* bits, u8* octets) noexcept
    {
        for (usize w = 0U; w < words_per_row; ++w)
        {
            for (usize b = 0U; b < 8U; ++b)
            {
                const u8* const spread = kByteSpread.of[(bits[w] >> (b * 8U)) & 255U];
                u8* const out = octets + (((w * 8U) + b) * 8U);
                u64 target = 0U;
                u64 source = 0U;
                std::memcpy(&target, out, sizeof(target));
                std::memcpy(&source, spread, sizeof(source));
                target ^= source;
                std::memcpy(out, &target, sizeof(target));
            }
        }
    };

    for (u32 t = 0U; t < m_chosen; ++t)
    {
        u64* const bits = forward + (usize{t} * words_per_row);
        std::memset(bits, 0, words_per_row * sizeof(u64));
        accumulate_row(m_chosen_row[t], bits);
    }

    std::memset(m_schur, 0, usize{m_schur_rows} * u);
    for (u32 s = 0U; s < binary_left; ++s)
    {
        std::memset(row_bits, 0, words_per_row * sizeof(u64));
        accumulate_row(m_schur_row[s], row_bits);
        std::memset(z, 0, spread_bytes);
        spread_into(row_bits, z);
        std::memcpy(m_schur + (usize{s} * u), z, u);
    }

    // The HDPC rows: for each column c, z = alpha·z + (V1^-1·U1 of c, or the unit vector of an inactivated c), and MT's
    // rows of c add z, which is MT·GAMMA·v without a table of G_HDPC (§5.3.3.3).
    u8* const hdpc = m_schur + (usize{binary_left} * u);
    const u32 lt_and_ldpc = p.k_prime + p.s;
    std::memset(z, 0, spread_bytes);
    std::memset(z_next, 0, spread_bytes); // past u bytes they stay zero, as the bit rows do
    const GfCoefficient& alpha = gf256_expanded(kAlpha);
    for (u32 c = 0U; c < lt_and_ldpc; ++c)
    {
        const u8* source = z;
        kernels.dot(&alpha, &source, 1U, &z_next, 1U, u, false);
        u8* const swap = z;
        z = z_next;
        z_next = swap;

        const u32 state = m_column_state[c];
        if ((state & kColumnInactivated) != 0U)
        {
            z[state & ~kColumnInactivated] ^= 1U;
        }
        else
        {
            spread_into(forward + (usize{state} * words_per_row), z);
        }
        if (c + 1U < lt_and_ldpc)
        {
            kernels.add(hdpc + (usize{m_hdpc_first[c]} * u), z, u);
            kernels.add(hdpc + (usize{m_hdpc_second[c]} * u), z, u);
        }
        else
        {
            for (u32 h = 0U; h < p.h; ++h)
            {
                const GfCoefficient& power = gf256_expanded(gf256_alpha_pow(h));
                const u8* last = z;
                u8* out = hdpc + (usize{h} * u);
                kernels.dot(&power, &last, 1U, &out, 1U, u, true);
            }
        }
    }
    for (u32 h = 0U; h < p.h; ++h)
    {
        hdpc[(usize{h} * u) + (m_column_state[p.l - p.h + h] & ~kColumnInactivated)] ^= 1U;
    }

    // Gaussian elimination, the second phase. No row is swapped in memory: m_pivot_row orders them.
    for (u32 s = 0U; s < m_schur_rows; ++s)
    {
        m_pivot_row[s] = s;
    }
    for (u32 j = 0U; j < u; ++j)
    {
        u32 pivot = j;
        while (pivot < m_schur_rows && m_schur[(usize{m_pivot_row[pivot]} * u) + j] == 0U)
        {
            ++pivot;
        }
        if (pivot == m_schur_rows)
        {
            return RaptorqStatus::Singular;
        }
        const u32 chosen = m_pivot_row[pivot];
        m_pivot_row[pivot] = m_pivot_row[j];
        m_pivot_row[j] = chosen;

        u8* const row = m_schur + (usize{chosen} * u);
        const usize rest = usize{u} - j - 1U;
        const u8* const row_rest = row + j + 1U;
        if (row[j] != 1U)
        {
            const u8 inverse = gf256_inv(row[j]);
            row[j] = inverse;
            if (rest != 0U)
            {
                const GfCoefficient& scale = gf256_expanded(inverse);
                kernels.dot(&scale, &row_rest, 1U, &z, 1U, rest, false);
                std::memcpy(row + j + 1U, z, rest);
            }
        }
        for (u32 s = j + 1U; s < m_schur_rows; ++s)
        {
            u8* const other = m_schur + (usize{m_pivot_row[s]} * u);
            if (other[j] != 0U)
            {
                const GfCoefficient& multiplier = gf256_expanded(other[j]);
                u8* const out = other + j + 1U;
                kernels.dot(&multiplier, &row_rest, 1U, &out, 1U, rest, true);
            }
        }
    }
    m_factored = true;
    return RaptorqStatus::Ok;
}

usize RaptorqSolver::solve_scratch_bytes(usize bytes) const noexcept
{
    return m_factored ? (usize{m_u} + 2U) * bytes : 0U;
}

RaptorqStatus RaptorqSolver::solve(std::span<const u8* const> symbols, usize bytes, std::span<u8> intermediate,
                                   std::span<u8> scratch) const noexcept
{
    if (!m_factored)
    {
        return RaptorqStatus::BadSequence;
    }
    const RaptorqParameters& p = m_parameters;
    if (symbols.size() != m_received)
    {
        return RaptorqStatus::WrongCount;
    }
    if (intermediate.size() < usize{p.l} * bytes || scratch.size() < solve_scratch_bytes(bytes))
    {
        return RaptorqStatus::WorkspaceTooSmall;
    }
    if (bytes == 0U)
    {
        return RaptorqStatus::Ok;
    }

    const ErasureKernels& kernels = *erasure_kernels().table;
    u8* const c = intermediate.data();
    u8* const right_side = scratch.data(); // S·C_u's right side, by pivot
    u8* z = right_side + (usize{m_u} * bytes);
    u8* z_next = z + bytes;
    const u32 binary_left = m_schur_rows - p.h;

    const auto symbol_of = [&](u32 row) noexcept -> const u8*
    {
        const u32 index = row - p.s;
        return row >= p.s && index < m_received ? symbols[index] : nullptr;
    };
    // out = the row's symbol, zero when it is a constraint's, plus the intermediate symbols of its entries up to end.
    const auto row_sum = [&](u8* out, u32 row, u32 end) noexcept
    {
        SumBatch batch(kernels, out, bytes);
        const u8* const source = symbol_of(row);
        if (source != nullptr)
        {
            batch.add(source);
        }
        for (u32 e = m_row_start[row]; e < end; ++e)
        {
            batch.add(c + (usize{m_row_columns[e]} * bytes));
        }
        batch.finish();
    };

    // y = V1^-1·D1, in the diagonal columns' symbols.
    for (u32 t = 0U; t < m_chosen; ++t)
    {
        const u32 row = m_chosen_row[t];
        row_sum(c + (usize{m_chosen_column[t]} * bytes), row, m_row_split[row]);
    }

    // D2 - V2·y: the binary pivots directly, the HDPC pivots through MT·GAMMA's recurrence over y.
    u32 hdpc_slot[kMaxHdpcRows];
    bool hdpc_written[kMaxHdpcRows];
    for (u32 h = 0U; h < p.h; ++h)
    {
        hdpc_slot[h] = kNoRow;
        hdpc_written[h] = false;
    }
    for (u32 j = 0U; j < m_u; ++j)
    {
        const u32 schur = m_pivot_row[j];
        u8* const out = right_side + (usize{j} * bytes);
        if (schur >= binary_left)
        {
            hdpc_slot[schur - binary_left] = j;
        }
        else
        {
            const u32 row = m_schur_row[schur];
            row_sum(out, row, m_row_split[row]);
        }
    }
    const u32 lt_and_ldpc = p.k_prime + p.s;
    const GfCoefficient& alpha = gf256_expanded(kAlpha);
    std::memset(z, 0, bytes);
    for (u32 column = 0U; column < lt_and_ldpc; ++column)
    {
        const u8* source = z;
        kernels.dot(&alpha, &source, 1U, &z_next, 1U, bytes, false);
        u8* const swap = z;
        z = z_next;
        z_next = swap;
        if ((m_column_state[column] & kColumnInactivated) == 0U)
        {
            kernels.add(z, c + (usize{column} * bytes), bytes);
        }
        if (column + 1U < lt_and_ldpc)
        {
            const u32 rows[2] = {m_hdpc_first[column], m_hdpc_second[column]};
            for (const u32 h : rows)
            {
                if (hdpc_slot[h] != kNoRow)
                {
                    const u8* last = z;
                    kernels.sum(right_side + (usize{hdpc_slot[h]} * bytes), &last, 1U, bytes, hdpc_written[h]);
                    hdpc_written[h] = true;
                }
            }
        }
        else
        {
            for (u32 h = 0U; h < p.h; ++h)
            {
                if (hdpc_slot[h] != kNoRow)
                {
                    const GfCoefficient& power = gf256_expanded(gf256_alpha_pow(h));
                    const u8* last = z;
                    u8* out = right_side + (usize{hdpc_slot[h]} * bytes);
                    kernels.dot(&power, &last, 1U, &out, 1U, bytes, hdpc_written[h]);
                }
            }
        }
    }

    // S·C_u = the right side: forward through the multipliers, then back through the unit upper triangle.
    const auto inactivated = [&](u32 j) noexcept
    {
        return c + (usize{m_u_column[j]} * bytes);
    };
    for (u32 j = 0U; j < m_u; ++j)
    {
        const u8* const row = m_schur + (usize{m_pivot_row[j]} * m_u);
        DotBatch batch(kernels, inactivated(j), bytes, false);
        batch.add(row[j], right_side + (usize{j} * bytes));
        for (u32 k = 0U; k < j; ++k)
        {
            if (row[k] != 0U)
            {
                batch.add(gf256_mul(row[k], row[j]), inactivated(k));
            }
        }
        batch.flush();
    }
    for (u32 j = m_u; j-- > 0U;)
    {
        const u8* const row = m_schur + (usize{m_pivot_row[j]} * m_u);
        DotBatch batch(kernels, inactivated(j), bytes, true);
        for (u32 k = j + 1U; k < m_u; ++k)
        {
            if (row[k] != 0U)
            {
                batch.add(row[k], inactivated(k));
            }
        }
        batch.flush();
    }

    // C_v = V1^-1·(D1 - U1·C_u).
    for (u32 t = 0U; t < m_chosen; ++t)
    {
        const u32 row = m_chosen_row[t];
        row_sum(c + (usize{m_chosen_column[t]} * bytes), row, m_row_end[row]);
    }
    return RaptorqStatus::Ok;
}

bool raptorq_write_payload_id(u8 sbn, u32 esi, std::span<u8> out) noexcept
{
    if (out.size() != kRaptorqPayloadIdBytes || esi >= kRaptorqEsiLimit)
    {
        return false;
    }
    crd::containers::wire_store<u32>(out, 0U, (u32{sbn} << 24U) | esi);
    return true;
}

bool raptorq_read_payload_id(std::span<const u8> in, u8& sbn, u32& esi) noexcept
{
    if (in.size() != kRaptorqPayloadIdBytes)
    {
        return false;
    }
    const u32 word = crd::containers::wire_load<u32>(in, 0U);
    sbn = static_cast<u8>(word >> 24U);
    esi = word & (kRaptorqEsiLimit - 1U);
    return true;
}

bool raptorq_oti_valid(const RaptorqOti& oti) noexcept
{
    if (oti.transfer_length > kRaptorqMaxTransferLength || oti.symbol_size == 0U || oti.symbol_size > 65535U ||
        oti.alignment == 0U || oti.alignment > 255U || oti.symbol_size % oti.alignment != 0U ||
        oti.source_blocks == 0U || oti.source_blocks > kRaptorqMaxSourceBlocks || oti.sub_blocks == 0U ||
        oti.sub_blocks > kRaptorqMaxSubBlocks || oti.sub_blocks > oti.symbol_size / oti.alignment)
    {
        return false;
    }
    const u64 symbols = (oti.transfer_length + oti.symbol_size - 1U) / oti.symbol_size;
    return (symbols + oti.source_blocks - 1U) / oti.source_blocks <= kRaptorqMaxSourceSymbols;
}

bool raptorq_write_oti(const RaptorqOti& oti, std::span<u8> out) noexcept
{
    if (out.size() != kRaptorqOtiBytes || !raptorq_oti_valid(oti))
    {
        return false;
    }
    using crd::containers::wire_store;
    wire_store<u32>(out, 0U, static_cast<u32>(oti.transfer_length >> 8U));
    out[4] = static_cast<u8>(oti.transfer_length);
    out[5] = 0U;
    wire_store<u16>(out, 6U, static_cast<u16>(oti.symbol_size));
    out[8] = static_cast<u8>(oti.source_blocks);
    wire_store<u16>(out, 9U, static_cast<u16>(oti.sub_blocks));
    out[11] = static_cast<u8>(oti.alignment);
    return true;
}

bool raptorq_read_oti(std::span<const u8> in, RaptorqOti& oti) noexcept
{
    if (in.size() != kRaptorqOtiBytes)
    {
        return false;
    }
    using crd::containers::wire_load;
    RaptorqOti read;
    read.transfer_length = (u64{wire_load<u32>(in, 0U)} << 8U) | in[4];
    read.symbol_size = wire_load<u16>(in, 6U);
    read.source_blocks = in[8];
    read.sub_blocks = wire_load<u16>(in, 9U);
    read.alignment = in[11];
    if (!raptorq_oti_valid(read))
    {
        return false;
    }
    oti = read;
    return true;
}

namespace
{

// KL(n) of §4.3: the largest K' <= limit, or 0.
[[nodiscard]] u32 largest_k_prime(u64 limit) noexcept
{
    u32 best = 0U;
    usize low = 0U;
    usize high = sizeof(kSystematicRows) / sizeof(kSystematicRows[0]);
    while (low < high)
    {
        const usize middle = low + ((high - low) / 2U);
        if (kSystematicRows[middle].k_prime <= limit)
        {
            best = kSystematicRows[middle].k_prime;
            low = middle + 1U;
        }
        else
        {
            high = middle;
        }
    }
    return best;
}

} // namespace

bool raptorq_derive_oti(u64 transfer_length, u64 working_memory, u32 alignment, u32 sub_symbol_units, u32 payload_size,
                        RaptorqOti& oti) noexcept
{
    if (transfer_length > kRaptorqMaxTransferLength || alignment == 0U || alignment > 255U || sub_symbol_units == 0U ||
        payload_size == 0U || payload_size > 65535U || payload_size % alignment != 0U)
    {
        return false;
    }
    const u64 t = payload_size;
    const u64 kt = (transfer_length + t - 1U) / t;
    const u64 n_max = t / (u64{sub_symbol_units} * alignment);
    if (n_max == 0U)
    {
        return false;
    }
    const auto kl = [&](u64 n) noexcept
    {
        const u64 units = (t + (alignment * n) - 1U) / (alignment * n);
        return largest_k_prime(working_memory / (alignment * units));
    };
    const u32 smallest_block = kl(n_max);
    if (smallest_block == 0U)
    {
        return false;
    }
    const u64 z = kt == 0U ? 1U : (kt + smallest_block - 1U) / smallest_block;
    if (z > kRaptorqMaxSourceBlocks)
    {
        return false;
    }
    const u64 per_block = (kt + z - 1U) / z;
    u64 n = 1U;
    while (per_block > kl(n))
    {
        ++n;
    }
    oti.transfer_length = transfer_length;
    oti.symbol_size = payload_size;
    oti.source_blocks = static_cast<u32>(z);
    oti.sub_blocks = static_cast<u32>(n);
    oti.alignment = alignment;
    return true;
}

bool raptorq_block(const RaptorqOti& oti, u32 sbn, RaptorqBlock& block) noexcept
{
    if (sbn >= oti.source_blocks)
    {
        return false;
    }
    const u64 t = oti.symbol_size;
    const u64 kt = (oti.transfer_length + t - 1U) / t;
    const RaptorqPartition blocks = raptorq_partition(kt, oti.source_blocks);
    const u64 before = sbn < blocks.large_count
                           ? sbn * blocks.large
                           : (blocks.large_count * blocks.large) + ((sbn - blocks.large_count) * blocks.small);
    block.k = static_cast<u32>(sbn < blocks.large_count ? blocks.large : blocks.small);
    const u64 start = before * t;
    block.offset = start < oti.transfer_length ? start : oti.transfer_length;
    const u64 whole = block.k * t;
    const u64 present = oti.transfer_length - block.offset;
    block.bytes = whole < present ? whole : present;
    return true;
}

namespace
{

// f(position in the symbol, position in the block, length) for each sub-symbol of symbol m.
template <typename F> void for_each_sub_symbol(const RaptorqOti& oti, u32 k, u32 m, const F& f) noexcept
{
    const RaptorqPartition subs = raptorq_partition(oti.symbol_size / oti.alignment, oti.sub_blocks);
    u64 in_block = 0U;
    usize in_symbol = 0U;
    for (u32 n = 0U; n < oti.sub_blocks; ++n)
    {
        const u64 length = (n < subs.large_count ? subs.large : subs.small) * oti.alignment;
        f(in_symbol, in_block + (m * length), static_cast<usize>(length));
        in_symbol += static_cast<usize>(length);
        in_block += k * length;
    }
}

// The octets of a sub-symbol of `length` at `at` that a block of `size` octets holds.
[[nodiscard]] usize octets_present(usize size, u64 at, usize length) noexcept
{
    if (at >= size)
    {
        return 0U;
    }
    const usize left = size - static_cast<usize>(at);
    return left < length ? left : length;
}

} // namespace

bool raptorq_read_symbol(const RaptorqOti& oti, u32 k, std::span<const u8> block, u32 m, std::span<u8> symbol) noexcept
{
    if (!raptorq_oti_valid(oti) || m >= k || k > kRaptorqMaxSourceSymbols || symbol.size() != oti.symbol_size)
    {
        return false;
    }
    for_each_sub_symbol(oti, k, m,
                        [&](usize to, u64 from, usize length) noexcept
                        {
                            const usize present = octets_present(block.size(), from, length);
                            if (present != 0U)
                            {
                                std::memcpy(symbol.data() + to, block.data() + static_cast<usize>(from), present);
                            }
                            if (present != length)
                            {
                                std::memset(symbol.data() + to + present, 0, length - present);
                            }
                        });
    return true;
}

bool raptorq_write_symbol(const RaptorqOti& oti, u32 k, std::span<u8> block, u32 m, std::span<const u8> symbol) noexcept
{
    if (!raptorq_oti_valid(oti) || m >= k || k > kRaptorqMaxSourceSymbols || symbol.size() != oti.symbol_size)
    {
        return false;
    }
    for_each_sub_symbol(oti, k, m,
                        [&](usize from, u64 to, usize length) noexcept
                        {
                            const usize present = octets_present(block.size(), to, length);
                            if (present != 0U)
                            {
                                std::memcpy(block.data() + static_cast<usize>(to), symbol.data() + from, present);
                            }
                        });
    return true;
}

} // namespace crd::hesap::net
