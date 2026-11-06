// crd-fuzz-hesap-erasure: hesap-comms' Reed-Solomon erasure code and its dispatched GF(2^8) kernels (HESAP-NET;
// crd/hesap/comms/erasure.hpp) under the bounded fuzz harness (REPO.DEV.9; docs/design/test-instruments.md).
//
// Input. The first byte picks the mode, the rest is its data.
//   0  a decode request: k, n, the symbol length, the received indices and the lost indices are read from the bytes,
//      and the received symbols are the true encoding symbols of generated sources;
//   1  an encode request: k, n, the symbol length, the number of sources and the repair indices;
//   2  a kernel request: coefficients, source regions at any alignment, prior outputs and the accumulate flag, run
//      through every kernel level this CPU can execute.
//
// Oracles, none of them the codec's own verdict:
//   * the refusal order restated from erasure.hpp decides every request's exact status;
//   * an encoded block lies in RFC 5510's code: its n symbols satisfy the n - k checks of the dual code,
//     sum_j u_j x_j^m c_j = 0 for m < n - k with x_j = alpha^j and u_j = 1 / prod_{l != j} (x_j - x_l), computed with
//     a shift-and-add multiply; with the k systematic symbols fixed that identifies the codeword;
//   * a decoded symbol equals the source it replaces;
//   * a refused request writes no byte, and no request writes past its symbol length;
//   * every level's dot product equals a shift-and-add evaluation of its definition.
#include <crd/fuzz/harness.hpp>

#include <crd/core/cpu_features.hpp>
#include <crd/core/kernel_dispatch.hpp>
#include <crd/hesap/comms/erasure.hpp>
#include <crd/hesap/comms/erasure_dispatch.hpp>
#include <crd/hesap/comms/gf256.hpp>

#include <cstring>
#include <initializer_list>

namespace
{
using crd::u32;
using crd::u64;
using crd::u8;
using crd::usize;
using namespace crd::hesap::comms;

constexpr u32 kMaxN = ReedSolomonCode::kMaxSymbols;
constexpr usize kMaxSymbolBytes = 24U;
constexpr usize kGuard = 16U;
constexpr u8 kSentinel = 0xA5U;

// ---- The field, restated -----------------------------------------------------------------------------------------

u8 slow_mul(u8 a, u8 b)
{
    u32 x = a;
    u32 product = 0U;
    for (u32 bit = 0U; bit < 8U; ++bit)
    {
        if ((b & (1U << bit)) != 0U)
        {
            product ^= x;
        }
        x <<= 1U;
        if ((x & 0x100U) != 0U)
        {
            x ^= 0x11DU;
        }
    }
    return static_cast<u8>(product);
}

u8 slow_pow(u8 a, u32 e)
{
    u8 result = 1U;
    for (u32 i = 0U; i < e; ++i)
    {
        result = slow_mul(result, a);
    }
    return result;
}

u8 slow_inverse(u8 a)
{
    return slow_pow(a, 254U);
}

// ---- Input reading and generated data -----------------------------------------------------------------------------

struct Reader
{
    const u8* data;
    usize size;

    u8 next()
    {
        if (size == 0U)
        {
            return 0U;
        }
        --size;
        return *data++;
    }

    u32 next32()
    {
        u32 v = 0U;
        for (u32 i = 0U; i < 4U; ++i)
        {
            v = (v << 8U) | next();
        }
        return v;
    }

    // An index: one byte, or a full u32 when `wide`.
    u32 index(bool wide) { return wide ? next32() : next(); }
};

struct Lcg
{
    u64 state;

    u8 next()
    {
        state = (state * 6364136223846793005ULL) + 1442695040888963407ULL;
        return static_cast<u8>(state >> 56U);
    }
};

// The block's n symbols of up to kMaxSymbolBytes, then a guard each; sources first.
u8 g_codeword[kMaxN][kMaxSymbolBytes + kGuard];
u8 g_out[kMaxN][kMaxSymbolBytes + kGuard];

void fill_sources(u32 k, usize bytes, u64 seed)
{
    Lcg lcg{seed};
    for (u32 i = 0U; i < k; ++i)
    {
        for (usize b = 0U; b < bytes; ++b)
        {
            g_codeword[i][b] = lcg.next();
        }
    }
}

void fill_outputs(usize count)
{
    for (usize q = 0U; q < count; ++q)
    {
        std::memset(g_out[q], kSentinel, sizeof(g_out[q]));
    }
}

bool untouched(usize count, usize from)
{
    for (usize q = 0U; q < count; ++q)
    {
        for (usize b = from; b < sizeof(g_out[q]); ++b)
        {
            if (g_out[q][b] != kSentinel)
            {
                return false;
            }
        }
    }
    return true;
}

// The dual code's checks over the block's n symbols in g_codeword.
void require_codeword(u32 k, u32 n, usize bytes)
{
    u8 points[kMaxN];
    for (u32 j = 0U; j < n; ++j)
    {
        points[j] = slow_pow(2U, j);
    }
    u8 u[kMaxN];
    for (u32 j = 0U; j < n; ++j)
    {
        u8 product = 1U;
        for (u32 l = 0U; l < n; ++l)
        {
            if (l != j)
            {
                product = slow_mul(product, static_cast<u8>(points[j] ^ points[l]));
            }
        }
        u[j] = slow_inverse(product);
    }
    for (usize b = 0U; b < bytes; ++b)
    {
        u8 term[kMaxN];
        for (u32 j = 0U; j < n; ++j)
        {
            term[j] = slow_mul(u[j], g_codeword[j][b]);
        }
        for (u32 m = 0U; m < n - k; ++m)
        {
            u8 sum = 0U;
            for (u32 j = 0U; j < n; ++j)
            {
                sum = static_cast<u8>(sum ^ term[j]);
                term[j] = slow_mul(term[j], points[j]);
            }
            CRD_FUZZ_REQUIRE(sum == 0U);
        }
    }
}

// Encodes every repair symbol of g_codeword's sources into place, checked against the dual code.
void encode_block(const ReedSolomonCode& code, usize bytes)
{
    const u32 k = code.source_count();
    const u32 n = code.symbol_count();
    const u8* sources[kMaxN];
    u32 repair[kMaxN];
    u8* outputs[kMaxN];
    for (u32 i = 0U; i < k; ++i)
    {
        sources[i] = g_codeword[i];
    }
    for (u32 j = k; j < n; ++j)
    {
        repair[j - k] = j;
        outputs[j - k] = g_codeword[j];
        std::memset(g_codeword[j], kSentinel, sizeof(g_codeword[j]));
    }
    CRD_FUZZ_REQUIRE(code.encode({sources, k}, {repair, n - k}, {outputs, n - k}, bytes) == ErasureStatus::Ok);
    for (u32 j = k; j < n; ++j)
    {
        for (usize b = bytes; b < sizeof(g_codeword[j]); ++b)
        {
            CRD_FUZZ_REQUIRE(g_codeword[j][b] == kSentinel);
        }
    }
    require_codeword(k, n, bytes);
}

// ---- The refusal order, restated ----------------------------------------------------------------------------------

ErasureStatus restated_decode(u32 k, u32 n, const u32* received, usize received_count, const u32* lost,
                              usize lost_count, usize output_count)
{
    if (received_count != k || lost_count != output_count)
    {
        return ErasureStatus::WrongCount;
    }
    for (usize t = 0U; t < received_count; ++t)
    {
        if (received[t] >= n)
        {
            return ErasureStatus::IndexOutOfRange;
        }
        for (usize s = 0U; s < t; ++s)
        {
            if (received[s] == received[t])
            {
                return ErasureStatus::DuplicateIndex;
            }
        }
    }
    for (usize q = 0U; q < lost_count; ++q)
    {
        if (lost[q] >= k)
        {
            return ErasureStatus::IndexOutOfRange;
        }
        for (usize s = 0U; s < q; ++s)
        {
            if (lost[s] == lost[q])
            {
                return ErasureStatus::DuplicateIndex;
            }
        }
        for (usize t = 0U; t < received_count; ++t)
        {
            if (received[t] == lost[q])
            {
                return ErasureStatus::NotLost;
            }
        }
    }
    return ErasureStatus::Ok;
}

ErasureStatus restated_encode(u32 k, u32 n, usize source_count, const u32* repair, usize repair_count,
                              usize output_count)
{
    if (source_count != k || repair_count != output_count)
    {
        return ErasureStatus::WrongCount;
    }
    for (usize q = 0U; q < repair_count; ++q)
    {
        if (repair[q] < k || repair[q] >= n)
        {
            return ErasureStatus::IndexOutOfRange;
        }
        for (usize s = 0U; s < q; ++s)
        {
            if (repair[s] == repair[q])
            {
                return ErasureStatus::DuplicateIndex;
            }
        }
    }
    return ErasureStatus::Ok;
}

// k, n and the symbol length from the request's first three bytes.
struct Shape
{
    u32 k;
    u32 n;
    usize bytes;
};

Shape read_shape(Reader& r)
{
    const u32 k = 1U + (r.next() % kMaxN);
    const u32 n = k + (r.next() % (kMaxN - k + 1U));
    const usize bytes = r.next() % (kMaxSymbolBytes + 1U);
    return {k, n, bytes};
}

// A list of at most kMaxN + 1 indices: its length is k plus a signed byte's offset when `near` is set, else a byte.
usize read_count(Reader& r, u32 k)
{
    const u8 raw = r.next();
    if ((raw & 0x80U) == 0U)
    {
        return raw % (kMaxN + 2U);
    }
    const u32 delta = raw & 3U;
    if ((raw & 0x40U) == 0U)
    {
        return k + delta;
    }
    return k > delta ? k - delta : 0U;
}

// ---- Modes --------------------------------------------------------------------------------------------------------

void decode_request(Reader r)
{
    const Shape shape = read_shape(r);
    const u8 flags = r.next();
    const bool wide = (flags & 1U) != 0U;
    const ReedSolomonCode code(shape.k, shape.n);
    fill_sources(shape.k, shape.bytes, (u64{flags} << 32U) | r.next32());
    encode_block(code, shape.bytes);

    u32 received[kMaxN + 4U];
    u32 lost[kMaxN + 4U];
    usize received_count = read_count(r, shape.k);
    usize lost_count = 0U;
    if ((flags & 0x10U) != 0U)
    {
        // A generated pattern: the first indices of a seeded permutation of [0, n) are received, and every source
        // among the rest is asked for.
        u32 order[kMaxN];
        for (u32 j = 0U; j < shape.n; ++j)
        {
            order[j] = j;
        }
        Lcg lcg{r.next32()};
        for (u32 j = shape.n - 1U; j > 0U; --j)
        {
            const u32 pick = ((u32{lcg.next()} << 8U) | lcg.next()) % (j + 1U);
            const u32 held = order[j];
            order[j] = order[pick];
            order[pick] = held;
        }
        received_count = received_count < shape.n ? received_count : shape.n;
        for (usize t = 0U; t < received_count; ++t)
        {
            received[t] = order[t];
        }
        for (usize t = received_count; t < shape.n; ++t)
        {
            if (order[t] < shape.k)
            {
                lost[lost_count++] = order[t];
            }
        }
    }
    else
    {
        for (usize t = 0U; t < received_count; ++t)
        {
            received[t] = r.index(wide);
        }
        // Keep a request mostly well formed: unless flag bit 1 is set, a received index is folded below n, and
        // unless bit 2 is, a lost one below k.
        if ((flags & 2U) == 0U)
        {
            for (usize t = 0U; t < received_count; ++t)
            {
                received[t] %= shape.n;
            }
        }
        lost_count = read_count(r, 0U) % (kMaxN + 2U);
        for (usize q = 0U; q < lost_count; ++q)
        {
            lost[q] = r.index(wide);
        }
        if ((flags & 4U) == 0U)
        {
            for (usize q = 0U; q < lost_count; ++q)
            {
                lost[q] %= shape.k;
            }
        }
    }
    const usize output_count = (flags & 8U) != 0U ? lost_count + 1U : lost_count;

    ErasureSymbol symbols[kMaxN + 4U];
    for (usize t = 0U; t < received_count; ++t)
    {
        symbols[t].index = received[t];
        symbols[t].data = received[t] < shape.n ? g_codeword[received[t]] : g_codeword[0];
    }
    u8* outputs[kMaxN + 4U];
    for (usize q = 0U; q < output_count; ++q)
    {
        outputs[q] = g_out[q % kMaxN];
    }
    fill_outputs(kMaxN);
    const ErasureStatus expected =
        restated_decode(shape.k, shape.n, received, received_count, lost, lost_count, output_count);
    const ErasureStatus status =
        code.decode({symbols, received_count}, {lost, lost_count}, {outputs, output_count}, shape.bytes);
    CRD_FUZZ_REQUIRE(status == expected);
    if (status != ErasureStatus::Ok)
    {
        CRD_FUZZ_REQUIRE(untouched(kMaxN, 0U));
        return;
    }
    CRD_FUZZ_REQUIRE(untouched(output_count, shape.bytes));
    for (usize q = 0U; q < lost_count; ++q)
    {
        CRD_FUZZ_REQUIRE(std::memcmp(g_out[q], g_codeword[lost[q]], shape.bytes) == 0);
    }
}

void encode_request(Reader r)
{
    const Shape shape = read_shape(r);
    const u8 flags = r.next();
    const bool wide = (flags & 1U) != 0U;
    const ReedSolomonCode code(shape.k, shape.n);
    fill_sources(shape.k, shape.bytes, (u64{flags} << 32U) | r.next32());
    const usize source_count = read_count(r, shape.k);
    u32 repair[kMaxN + 4U];
    const usize repair_count = read_count(r, 0U) % (kMaxN + 2U);
    for (usize q = 0U; q < repair_count; ++q)
    {
        repair[q] = r.index(wide);
    }
    if ((flags & 2U) == 0U && shape.n > shape.k)
    {
        for (usize q = 0U; q < repair_count; ++q)
        {
            repair[q] = shape.k + (repair[q] % (shape.n - shape.k));
        }
    }
    const usize output_count = (flags & 8U) != 0U ? repair_count + 1U : repair_count;
    const u8* sources[kMaxN + 4U];
    for (usize i = 0U; i < source_count; ++i)
    {
        sources[i] = g_codeword[i % kMaxN];
    }
    u8* outputs[kMaxN + 4U];
    for (usize q = 0U; q < output_count; ++q)
    {
        outputs[q] = g_out[q % kMaxN];
    }
    fill_outputs(kMaxN);
    const ErasureStatus expected =
        restated_encode(shape.k, shape.n, source_count, repair, repair_count, output_count);
    const ErasureStatus status =
        code.encode({sources, source_count}, {repair, repair_count}, {outputs, output_count}, shape.bytes);
    CRD_FUZZ_REQUIRE(status == expected);
    if (status != ErasureStatus::Ok)
    {
        CRD_FUZZ_REQUIRE(untouched(kMaxN, 0U));
        return;
    }
    CRD_FUZZ_REQUIRE(untouched(repair_count, shape.bytes));
    encode_block(code, shape.bytes);
    for (usize q = 0U; q < repair_count; ++q)
    {
        CRD_FUZZ_REQUIRE(std::memcmp(g_out[q], g_codeword[repair[q]], shape.bytes) == 0);
    }
    // The prepared path gives the same bytes and refuses rows that do not fit.
    static GfCoefficient rows[usize{kMaxN} * kMaxN];
    const usize row_count = repair_count * shape.k;
    CRD_FUZZ_REQUIRE(code.prepare_encode({repair, repair_count}, {rows, row_count + 1U}) == ErasureStatus::WrongCount);
    CRD_FUZZ_REQUIRE(code.prepare_encode({repair, repair_count}, {rows, row_count}) == ErasureStatus::Ok);
    fill_outputs(kMaxN);
    CRD_FUZZ_REQUIRE(code.encode_prepared({rows, row_count}, {sources, source_count}, {outputs, repair_count},
                                          shape.bytes) == ErasureStatus::Ok);
    CRD_FUZZ_REQUIRE(untouched(repair_count, shape.bytes));
    for (usize q = 0U; q < repair_count; ++q)
    {
        CRD_FUZZ_REQUIRE(std::memcmp(g_out[q], g_codeword[repair[q]], shape.bytes) == 0);
    }
}

// Kernel requests: at most 40 sources and 9 outputs of up to 600 bytes, each region at its own offset in [0, 64).
constexpr usize kKernelSources = 40U;
constexpr usize kKernelOutputs = 9U;
constexpr usize kKernelBytes = 600U;
constexpr usize kKernelStride = kKernelBytes + 64U + kGuard;

u8 g_kernel_sources[kKernelSources][kKernelStride];
u8 g_kernel_prior[kKernelOutputs][kKernelBytes];
u8 g_kernel_out[kKernelOutputs][kKernelStride];
u8 g_kernel_expected[kKernelOutputs][kKernelBytes];

void kernel_request(Reader r)
{
    const usize count = r.next() % (kKernelSources + 1U);
    const usize output_count = 1U + (r.next() % kKernelOutputs);
    const usize bytes = ((usize{r.next()} << 8U) | r.next()) % (kKernelBytes + 1U);
    const u8 flags = r.next();
    const bool accumulate = (flags & 1U) != 0U;
    // Flag bit 1: the coefficients and offsets come from the seeded generator instead of the bytes.
    const bool generated = (flags & 2U) != 0U;
    Lcg lcg{r.next32()};
    const auto pick = [&r, &lcg, generated]()
    {
        return generated ? lcg.next() : r.next();
    };
    u8 coefficients[kKernelOutputs * kKernelSources];
    GfCoefficient expanded[kKernelOutputs * kKernelSources];
    for (usize c = 0U; c < output_count * count; ++c)
    {
        coefficients[c] = pick();
        expanded[c] = gf256_expand(coefficients[c]);
    }
    usize source_offset[kKernelSources];
    usize output_offset[kKernelOutputs];
    for (usize j = 0U; j < count; ++j)
    {
        source_offset[j] = pick() & 63U;
    }
    for (usize g = 0U; g < output_count; ++g)
    {
        output_offset[g] = pick() & 63U;
    }
    const u8* sources[kKernelSources];
    for (usize j = 0U; j < count; ++j)
    {
        for (usize b = 0U; b < bytes; ++b)
        {
            g_kernel_sources[j][source_offset[j] + b] = lcg.next();
        }
        sources[j] = g_kernel_sources[j] + source_offset[j];
    }
    for (usize g = 0U; g < output_count; ++g)
    {
        for (usize b = 0U; b < bytes; ++b)
        {
            g_kernel_prior[g][b] = lcg.next();
            u8 acc = accumulate ? g_kernel_prior[g][b] : u8{0U};
            for (usize j = 0U; j < count; ++j)
            {
                acc = static_cast<u8>(acc ^ slow_mul(coefficients[(g * count) + j], sources[j][b]));
            }
            g_kernel_expected[g][b] = acc;
        }
    }
    const crd::CpuSelection& selection = crd::cpu_selection();
    const crd::CpuLevel cap = crd::kernel_level_cap();
    for (const crd::KernelLevel<ErasureKernels>& level : erasure_kernel_levels())
    {
        if (!crd::kernel_level_eligible(level, selection.selected, selection.kernel_features, cap))
        {
            continue;
        }
        u8* outputs[kKernelOutputs];
        for (usize g = 0U; g < output_count; ++g)
        {
            std::memset(g_kernel_out[g], kSentinel, sizeof(g_kernel_out[g]));
            outputs[g] = g_kernel_out[g] + output_offset[g];
            std::memcpy(outputs[g], g_kernel_prior[g], bytes);
        }
        level.table->dot(expanded, sources, count, outputs, output_count, bytes, accumulate);
        for (usize g = 0U; g < output_count; ++g)
        {
            CRD_FUZZ_REQUIRE(std::memcmp(outputs[g], g_kernel_expected[g], bytes) == 0);
            for (usize b = 0U; b < sizeof(g_kernel_out[g]); ++b)
            {
                const bool inside = b >= output_offset[g] && b < output_offset[g] + bytes;
                CRD_FUZZ_REQUIRE(inside || g_kernel_out[g][b] == kSentinel);
            }
        }
    }
}

void add(crd::fuzz::SeedSink& sink, const char* name, std::initializer_list<u8> bytes)
{
    u8 buffer[64];
    usize size = 0U;
    for (const u8 b : bytes)
    {
        buffer[size++] = b;
    }
    sink.add(name, buffer, size);
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    if (size == 0U || size > crd::fuzz::kMaxInputBytes)
    {
        return 0;
    }
    const Reader rest{data + 1U, size - 1U};
    switch (data[0] % 3U)
    {
    case 0U:
        decode_request(rest);
        break;
    case 1U:
        encode_request(rest);
        break;
    default:
        kernel_request(rest);
        break;
    }
    return 0;
}

void crd_fuzz_seeds(crd::fuzz::SeedSink& sink)
{
    // Shapes are read as k = 1 + byte, n = k + byte and the length as a byte, then the flags and a 32-bit seed;
    // counts with bit 7 are k plus or minus the low bits (bit 6 subtracts).
    // Decodes: (3, 7) losing sources 0 and 2 from symbols 1, 4, 6.
    add(sink, "decode-3-7", {0U, 2U, 4U, 13U, 0U, 1U, 2U, 3U, 4U, 0x80U, 1U, 4U, 6U, 2U, 0U, 2U});
    // (1, 1) and (1, 255): a lone source, and one repair symbol standing for it.
    add(sink, "decode-1-1", {0U, 0U, 0U, 5U, 0U, 9U, 9U, 9U, 9U, 0x80U, 0U, 0U});
    add(sink, "decode-1-255", {0U, 0U, 254U, 24U, 0U, 7U, 7U, 7U, 7U, 0x80U, 200U, 1U, 0U});
    // Generated patterns (flag 0x10, then the permutation's seed): k of a seeded permutation of (200, 255), (16, 24)
    // and (255, 255).
    add(sink, "decode-200-255", {0U, 199U, 55U, 17U, 0x10U, 1U, 2U, 3U, 4U, 0x80U, 5U, 6U, 7U, 8U});
    add(sink, "decode-16-24", {0U, 15U, 8U, 24U, 0x10U, 4U, 3U, 2U, 1U, 0x80U, 8U, 7U, 6U, 5U});
    add(sink, "decode-255-255", {0U, 254U, 0U, 3U, 0x10U, 9U, 8U, 7U, 6U, 0x80U, 1U, 1U, 1U, 1U});
    // Refusals: one received short, a received index past n, a repeated received index, a source asked for that was
    // received, a lost index repeated, an output too many, a wide index, too few symbols of a generated pattern.
    add(sink, "decode-wrong-count", {0U, 2U, 4U, 8U, 0U, 1U, 1U, 1U, 1U, 0xC1U, 4U, 5U, 1U, 0U});
    add(sink, "decode-out-of-range", {0U, 2U, 4U, 8U, 2U, 1U, 1U, 1U, 1U, 0x80U, 4U, 5U, 9U, 1U, 0U});
    add(sink, "decode-duplicate", {0U, 2U, 4U, 8U, 0U, 1U, 1U, 1U, 1U, 0x80U, 4U, 4U, 5U, 1U, 0U});
    add(sink, "decode-not-lost", {0U, 2U, 4U, 8U, 0U, 1U, 1U, 1U, 1U, 0x80U, 0U, 4U, 5U, 1U, 0U});
    add(sink, "decode-lost-twice", {0U, 2U, 4U, 8U, 0U, 1U, 1U, 1U, 1U, 0x80U, 4U, 5U, 6U, 2U, 1U, 1U});
    add(sink, "decode-output-extra", {0U, 2U, 4U, 8U, 8U, 1U, 1U, 1U, 1U, 0x80U, 4U, 5U, 6U, 1U, 1U});
    add(sink, "decode-wide-index", {0U, 0U, 3U, 8U, 3U, 1U, 1U, 1U, 1U, 0x80U, 0x7FU, 0xFFU, 0xFFU, 0xFFU});
    add(sink, "decode-generated-short", {0U, 15U, 8U, 24U, 0x10U, 4U, 3U, 2U, 1U, 0xC2U, 8U, 7U, 6U, 5U});
    // Encodes: every repair of (4, 8), one repair of (100, 160), and the refusals.
    add(sink, "encode-4-8", {1U, 3U, 4U, 11U, 0U, 3U, 3U, 3U, 3U, 0x80U, 4U, 0U, 1U, 2U, 3U});
    add(sink, "encode-100-160", {1U, 99U, 60U, 24U, 0U, 5U, 5U, 5U, 5U, 0x80U, 1U, 59U});
    add(sink, "encode-wrong-count", {1U, 3U, 4U, 11U, 0U, 3U, 3U, 3U, 3U, 0xC1U, 1U, 0U});
    add(sink, "encode-source-index", {1U, 3U, 4U, 11U, 2U, 3U, 3U, 3U, 3U, 0x80U, 1U, 2U});
    add(sink, "encode-duplicate", {1U, 3U, 4U, 11U, 0U, 3U, 3U, 3U, 3U, 0x80U, 2U, 1U, 1U});
    add(sink, "encode-output-extra", {1U, 3U, 4U, 11U, 8U, 3U, 3U, 3U, 3U, 0x80U, 1U, 1U});
    // Kernels (sources, outputs - 1, a 16-bit length, the flags, a seed): the self-check's five outputs of three
    // sources over 141 bytes, generated long misaligned regions (flag 2), accumulation (flag 1), no sources at all.
    add(sink, "kernel-self-check-shape", {2U, 3U, 4U, 0U, 141U, 0U, 1U, 2U, 3U, 4U, 0x01U, 0x02U, 0x80U, 0xFFU, 0U,
                                          0x1DU, 0x53U, 0xCAU, 0x8EU, 0U, 0U, 0U, 0x02U, 0xFFU, 0x01U});
    add(sink, "kernel-misaligned", {2U, 40U, 8U, 2U, 0x57U, 2U, 9U, 8U, 7U, 6U});
    add(sink, "kernel-accumulate", {2U, 37U, 3U, 1U, 0x2CU, 3U, 5U, 5U, 5U, 5U});
    add(sink, "kernel-no-sources", {2U, 0U, 2U, 0U, 33U, 1U, 1U, 2U, 3U, 4U, 5U, 6U, 7U});
}
