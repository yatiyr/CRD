// crd-fuzz-hesap-raptorq: hesap-net's RaptorQ code (RFC 6330; HESAP-NET; crd/hesap/net/raptorq.hpp) under the bounded
// fuzz harness (REPO.DEV.9; docs/design/test-instruments.md).
//
// Input. The first byte picks the mode, the rest is its data.
//   0  a decoder: k, the symbol size, a memory fault, then the ESIs as 32-bit words;
//   1  wire: four octets of a payload ID, then twelve of an object transmission information;
//   2  an object: F, T in units of Al, Z, N and Al, then its octets;
//   3  §4.3's derivation: F, WS, Al, SS and P'.
//
// Oracles, none of them the solver's own verdict:
//   * the refusal order restated from raptorq.hpp decides every refused request, and writes nothing to the solver;
//   * the rank of the literal matrix A of Figure 6, by plain elimination, decides Ok against Singular;
//   * an accepted decoder's intermediate symbols are the encoder's bytes and reproduce every symbol it was given;
//   * a reader restated bit by bit from Figures 1 to 3 decides the wire forms, and a valid form writes back;
//   * an object's blocks cover it contiguously, and its symbols read and write each octet exactly once, zero past its
//   end;
//   * §4.3's definition, restated over every n, decides the derivation.

#include "raptorq_literal.hpp"

#include <crd/containers/array.hpp>
#include <crd/fuzz/harness.hpp>
#include <crd/hesap/net/raptorq.hpp>

#include <cstring>
#include <initializer_list>
#include <span>

namespace
{

using crd::u32;
using crd::u64;
using crd::u8;
using crd::usize;
using namespace crd::hesap::net;

template <typename T> using Array = crd::containers::Array<T>;
using Bytes = Array<u8>;

struct Reader
{
    const u8* data;
    usize size;

    u32 next()
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
        u32 value = 0U;
        for (u32 i = 0U; i < 4U; ++i)
        {
            value = (value << 8U) | next();
        }
        return value;
    }

    u64 next64()
    {
        const u64 high = next32();
        return (high << 32U) | next32();
    }
};

Bytes zeroed(usize size)
{
    Bytes bytes;
    bytes.resize(size);
    if (size != 0U)
    {
        std::memset(bytes.data(), 0, size);
    }
    return bytes;
}

// A block of symbols that depends on nothing but k and the position, enough to tell any two symbols apart.
Bytes known_block(u32 k, usize t)
{
    Bytes block = zeroed(usize{k} * t);
    u64 state = 0x6330ULL + k;
    for (usize i = 0U; i < block.size(); ++i)
    {
        state = (state * 0x5851F42D4C957F2DULL) + 1U;
        block[i] = static_cast<u8>(state >> 56U);
    }
    return block;
}

void decoder_mode(Reader r)
{
    const u32 raw_k = (r.next() << 8U) | r.next();
    const u32 k = (raw_k & 0x8000U) != 0U ? raw_k * 4U : 1U + (raw_k % 48U); // now and then out of range, or large
    const usize t = 1U + (r.next() % 16U);
    const u32 fault = r.next();

    Array<u32> esis;
    while (r.size >= 4U && esis.size() < 96U)
    {
        const u32 word = r.next32();
        esis.push_back((word >> 24U) == 0xFFU ? word : word & 0x00FFFFFFU);
    }

    // The refusals of raptorq.hpp, in their order, from the definitions.
    RaptorqParameters p;
    RaptorqStatus want = RaptorqStatus::Ok;
    if (k < 1U || k > kRaptorqMaxSourceSymbols)
    {
        want = RaptorqStatus::BadBlockSize;
    }
    else
    {
        CRD_FUZZ_REQUIRE(raptorq_parameters(k, p));
        for (const u32 esi : esis)
        {
            if (want == RaptorqStatus::Ok && esi >= kRaptorqEsiLimit)
            {
                want = RaptorqStatus::EsiOutOfRange;
            }
        }
        if (want == RaptorqStatus::Ok && esis.size() < k)
        {
            want = RaptorqStatus::TooFewSymbols;
        }
    }
    const usize structure_bytes = RaptorqSolver::structure_bytes(k, esis);
    CRD_FUZZ_REQUIRE((structure_bytes == 0U) == (want != RaptorqStatus::Ok));

    if (want == RaptorqStatus::Ok && k > 48U)
    {
        return; // a large k with its few ESIs is refused above; one with many is not this target's to pay for
    }

    Bytes structure = zeroed(structure_bytes + 8U);
    const usize offset = fault & 7U;
    if (want == RaptorqStatus::Ok && (fault & 8U) != 0U)
    {
        RaptorqSolver short_memory;
        CRD_FUZZ_REQUIRE(
            short_memory.analyze(k, esis, std::span<u8>(structure.data() + offset, structure_bytes - 1U)) ==
            RaptorqStatus::WorkspaceTooSmall);
        CRD_FUZZ_REQUIRE(short_memory.factor(structure) == RaptorqStatus::BadSequence);
    }
    if (want == RaptorqStatus::Ok)
    {
        for (usize i = 0U; i < esis.size(); ++i)
        {
            for (usize j = 0U; j < i; ++j)
            {
                if (esis[i] == esis[j])
                {
                    want = RaptorqStatus::DuplicateEsi;
                }
            }
        }
    }

    RaptorqSolver solver;
    const RaptorqStatus analyzed = solver.analyze(k, esis, std::span<u8>(structure.data() + offset, structure_bytes));
    CRD_FUZZ_REQUIRE(analyzed == want);
    Bytes small = zeroed(16U);
    if (want != RaptorqStatus::Ok)
    {
        CRD_FUZZ_REQUIRE(solver.factor(small) == RaptorqStatus::BadSequence);
        return;
    }
    CRD_FUZZ_REQUIRE(solver.inactivated() >= p.p);
    CRD_FUZZ_REQUIRE(solver.inactivated() <= p.l);
    CRD_FUZZ_REQUIRE(solver.schur_rows() >= solver.inactivated());
    if ((fault & 16U) != 0U)
    {
        CRD_FUZZ_REQUIRE(solver.factor(std::span<u8>(small.data(), 1U)) == RaptorqStatus::WorkspaceTooSmall);
    }

    // The literal matrix over the padded rows.
    Array<u32> isis;
    for (const u32 esi : esis)
    {
        isis.push_back(raptorq_isi(p, k, esi));
    }
    for (u32 x = k; x < p.k_prime; ++x)
    {
        isis.push_back(x);
    }
    const Bytes a = raptorq_literal::matrix(p, isis);
    const bool full_rank = raptorq_literal::rank(a, a.size() / p.l, p.l) == p.l;

    Bytes factors = zeroed(solver.factor_bytes() + 8U);
    const RaptorqStatus factored = solver.factor(std::span<u8>(factors.data() + offset, solver.factor_bytes()));
    CRD_FUZZ_REQUIRE(factored == (full_rank ? RaptorqStatus::Ok : RaptorqStatus::Singular));
    Bytes c = zeroed(usize{p.l} * t);
    if (!full_rank)
    {
        CRD_FUZZ_REQUIRE(solver.solve(std::span<const u8* const>(), t, c, c) == RaptorqStatus::BadSequence);
        return;
    }

    // The encoder of the same block has one answer, which the decoder must find.
    const Bytes block = known_block(k, t);
    Array<const u8*> sources;
    for (u32 i = 0U; i < k; ++i)
    {
        sources.push_back(block.data() + (usize{i} * t));
    }
    RaptorqSolver encoder;
    Bytes encoder_structure = zeroed(RaptorqSolver::structure_bytes(k));
    CRD_FUZZ_REQUIRE(encoder.analyze(k, encoder_structure) == RaptorqStatus::Ok);
    Bytes encoder_factors = zeroed(encoder.factor_bytes());
    CRD_FUZZ_REQUIRE(encoder.factor(encoder_factors) == RaptorqStatus::Ok);
    Bytes expected = zeroed(usize{p.l} * t);
    Bytes scratch = zeroed(encoder.solve_scratch_bytes(t));
    CRD_FUZZ_REQUIRE(encoder.solve(sources, t, expected, scratch) == RaptorqStatus::Ok);

    Bytes symbols = zeroed(esis.size() * t);
    Array<const u8*> received;
    for (usize i = 0U; i < esis.size(); ++i)
    {
        u8* const symbol = symbols.data() + (i * t);
        raptorq_symbol(p, expected.data(), t, raptorq_isi(p, k, esis[i]), symbol);
        received.push_back(symbol);
        if (esis[i] < k)
        {
            CRD_FUZZ_REQUIRE(std::memcmp(symbol, sources[esis[i]], t) == 0);
        }
    }
    scratch = zeroed(solver.solve_scratch_bytes(t));
    if (!received.empty() && (fault & 32U) != 0U)
    {
        CRD_FUZZ_REQUIRE(solver.solve(std::span<const u8* const>(received.data(), received.size() - 1U), t, c,
                                      scratch) == RaptorqStatus::WrongCount);
    }
    CRD_FUZZ_REQUIRE(solver.solve(received, t, c, scratch) == RaptorqStatus::Ok);
    CRD_FUZZ_REQUIRE(std::memcmp(c.data(), expected.data(), c.size()) == 0);
}

void wire_mode(Reader r)
{
    u8 id[4];
    u8 oti_bytes[12];
    for (u8& b : id)
    {
        b = static_cast<u8>(r.next());
    }
    for (u8& b : oti_bytes)
    {
        b = static_cast<u8>(r.next());
    }

    u8 sbn = 0U;
    u32 esi = 0U;
    CRD_FUZZ_REQUIRE(raptorq_read_payload_id(id, sbn, esi));
    CRD_FUZZ_REQUIRE(sbn == id[0]);
    CRD_FUZZ_REQUIRE(esi == ((u32{id[1]} << 16U) | (u32{id[2]} << 8U) | id[3]));
    u8 back[4] = {};
    CRD_FUZZ_REQUIRE(raptorq_write_payload_id(sbn, esi, back));
    CRD_FUZZ_REQUIRE(std::memcmp(back, id, 4U) == 0);

    u64 f = 0U;
    for (u32 i = 0U; i < 5U; ++i)
    {
        f = (f << 8U) | oti_bytes[i];
    }
    const u32 t = (u32{oti_bytes[6]} << 8U) | oti_bytes[7];
    const u32 z = oti_bytes[8];
    const u32 n = (u32{oti_bytes[9]} << 8U) | oti_bytes[10];
    const u32 al = oti_bytes[11];
    bool valid = f <= 942574504275ULL && t != 0U && al != 0U && z != 0U && n != 0U;
    if (valid)
    {
        valid = t % al == 0U && n <= t / al;
        const u64 kt = (f / t) + ((f % t) != 0U ? 1U : 0U);
        const u64 per_block = (kt / z) + ((kt % z) != 0U ? 1U : 0U);
        valid = valid && per_block <= 56403U;
    }
    RaptorqOti oti;
    CRD_FUZZ_REQUIRE(raptorq_read_oti(oti_bytes, oti) == valid);
    if (valid)
    {
        CRD_FUZZ_REQUIRE(oti.transfer_length == f && oti.symbol_size == t && oti.source_blocks == z);
        CRD_FUZZ_REQUIRE(oti.sub_blocks == n && oti.alignment == al);
        u8 written[12] = {};
        CRD_FUZZ_REQUIRE(raptorq_write_oti(oti, written));
        oti_bytes[5] = 0U; // the reserved octet
        CRD_FUZZ_REQUIRE(std::memcmp(written, oti_bytes, 12U) == 0);
    }
}

void object_mode(Reader r)
{
    const u32 al = 1U + (r.next() % 8U);
    const u32 units = 1U + (r.next() % 16U);
    const u32 t = units * al;
    const u32 z = 1U + (r.next() % 8U);
    const u32 n = 1U + (r.next() % 20U);
    const usize f = r.size < 2048U ? r.size : 2048U;
    const RaptorqOti oti{f, t, z, n, al};
    if (n > units)
    {
        CRD_FUZZ_REQUIRE(!raptorq_oti_valid(oti));
        return;
    }
    CRD_FUZZ_REQUIRE(raptorq_oti_valid(oti));

    Bytes image = zeroed(f);
    u64 next_offset = 0U;
    u32 total_symbols = 0U;
    Bytes symbol = zeroed(t);
    for (u32 sbn = 0U; sbn < z; ++sbn)
    {
        RaptorqBlock block;
        CRD_FUZZ_REQUIRE(raptorq_block(oti, sbn, block));
        CRD_FUZZ_REQUIRE(block.offset == next_offset || block.bytes == 0U);
        CRD_FUZZ_REQUIRE(block.bytes <= u64{block.k} * t);
        CRD_FUZZ_REQUIRE(block.offset + block.bytes <= f);
        total_symbols += block.k;
        next_offset = block.offset + block.bytes;

        const std::span<const u8> in(r.data + block.offset, static_cast<usize>(block.bytes));
        const std::span<u8> out(image.data() + block.offset, static_cast<usize>(block.bytes));
        for (u32 m = 0U; m < block.k; ++m)
        {
            CRD_FUZZ_REQUIRE(raptorq_read_symbol(oti, block.k, in, m, symbol));
            CRD_FUZZ_REQUIRE(raptorq_write_symbol(oti, block.k, out, m, symbol));
        }
        CRD_FUZZ_REQUIRE(!raptorq_read_symbol(oti, block.k, in, block.k, symbol));
    }
    RaptorqBlock past;
    CRD_FUZZ_REQUIRE(!raptorq_block(oti, z, past));
    CRD_FUZZ_REQUIRE(next_offset == f);
    CRD_FUZZ_REQUIRE(total_symbols == (f / t) + ((f % t) != 0U ? 1U : 0U));
    if (f != 0U)
    {
        CRD_FUZZ_REQUIRE(std::memcmp(image.data(), r.data, f) == 0);
    }
}

// KL(n) of §4.3, restated over the whole table.
u32 restated_kl(u64 ws, u64 al, u64 t, u64 n)
{
    const u64 limit = ws / (al * (((t - 1U) / (al * n)) + 1U));
    u32 best = 0U;
    for (const RaptorqSystematicRow& row : raptorq_systematic_rows())
    {
        if (row.k_prime <= limit)
        {
            best = row.k_prime;
        }
    }
    return best;
}

void derive_mode(Reader r)
{
    const u64 f = r.next64() >> (r.next() % 64U);
    const u64 ws = r.next64() >> (r.next() % 64U);
    const u32 al = r.next();
    const u32 ss = r.next();
    const u32 payload = ((r.next() << 8U) | r.next()) & 0x0FFFU; // N_max stays in reach of the restatement

    RaptorqOti oti;
    const bool derived = raptorq_derive_oti(f, ws, al, ss, payload, oti);
    bool possible = f <= kRaptorqMaxTransferLength && al != 0U && ss != 0U && payload != 0U && payload % al == 0U;
    if (!possible)
    {
        CRD_FUZZ_REQUIRE(!derived);
        return;
    }
    const u64 n_max = payload / (u64{ss} * al);
    if (n_max == 0U || restated_kl(ws, al, payload, n_max) == 0U)
    {
        CRD_FUZZ_REQUIRE(!derived);
        return;
    }
    const u64 smallest = restated_kl(ws, al, payload, n_max);
    const u64 kt = (f / payload) + ((f % payload) != 0U ? 1U : 0U);
    const u64 z = kt == 0U ? 1U : (kt / smallest) + ((kt % smallest) != 0U ? 1U : 0U);
    possible = z <= 255U;
    CRD_FUZZ_REQUIRE(derived == possible);
    if (!derived)
    {
        return;
    }
    const u64 per_block = (kt / z) + ((kt % z) != 0U ? 1U : 0U);
    u64 n = 1U;
    while (per_block > restated_kl(ws, al, payload, n))
    {
        ++n;
    }
    CRD_FUZZ_REQUIRE(oti.transfer_length == f && oti.symbol_size == payload && oti.alignment == al);
    CRD_FUZZ_REQUIRE(oti.source_blocks == z && oti.sub_blocks == n);
    CRD_FUZZ_REQUIRE(raptorq_oti_valid(oti));
}

void add(crd::fuzz::SeedSink& sink, const char* name, std::initializer_list<u8> bytes)
{
    Bytes seed;
    for (const u8 b : bytes)
    {
        seed.push_back(b);
    }
    sink.add(name, seed.data(), seed.size());
}

// A decoder's seed: the mode, k = 10, 4-octet symbols, the fault bits, then `count` ESIs from `first`.
void add_decoder(crd::fuzz::SeedSink& sink, const char* name, u32 k, u32 fault, u32 first, u32 count)
{
    Bytes seed;
    seed.push_back(0U);
    seed.push_back(0U);
    seed.push_back(static_cast<u8>(k - 1U));
    seed.push_back(3U);
    seed.push_back(static_cast<u8>(fault));
    for (u32 i = 0U; i < count; ++i)
    {
        const u32 esi = first + i;
        seed.push_back(static_cast<u8>(esi >> 24U));
        seed.push_back(static_cast<u8>(esi >> 16U));
        seed.push_back(static_cast<u8>(esi >> 8U));
        seed.push_back(static_cast<u8>(esi));
    }
    sink.add(name, seed.data(), seed.size());
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    if (size == 0U || size > crd::fuzz::kMaxInputBytes)
    {
        return 0;
    }
    Reader r{data + 1U, size - 1U};
    switch (data[0] % 4U)
    {
        case 0U:
            decoder_mode(r);
            break;
        case 1U:
            wire_mode(r);
            break;
        case 2U:
            object_mode(r);
            break;
        default:
            derive_mode(r);
            break;
    }
    return 0;
}

void crd_fuzz_seeds(crd::fuzz::SeedSink& sink)
{
    // Decoders: every source, every repair symbol, a mix over many K, too few, short memory, each fault bit, the last
    // ESIs, an ESI out of range, a repeat, a k out of range.
    add_decoder(sink, "decoder-sources", 10U, 0U, 0U, 10U);
    add_decoder(sink, "decoder-repair", 10U, 0U, 10U, 10U);
    add_decoder(sink, "decoder-mixed", 18U, 0U, 7U, 18U);
    add_decoder(sink, "decoder-padded", 11U, 0U, 3U, 11U);
    add_decoder(sink, "decoder-overhead", 26U, 0U, 20U, 40U);
    add_decoder(sink, "decoder-too-few", 12U, 0U, 0U, 11U);
    add_decoder(sink, "decoder-short-memory", 13U, 8U + 5U, 0U, 14U);
    add_decoder(sink, "decoder-short-factors", 13U, 16U + 3U, 0U, 14U);
    add_decoder(sink, "decoder-wrong-count", 13U, 32U, 0U, 14U);
    add_decoder(sink, "decoder-last-esis", 12U, 0U, kRaptorqEsiLimit - 13U, 13U);
    add_decoder(sink, "decoder-out-of-range", 12U, 0U, 0xFF000000U, 13U);
    add(sink, "decoder-repeat", {0U, 0U, 0U, 1U, 0U, 0U, 0U, 0U, 5U, 0U, 0U, 0U, 5U});
    add(sink, "decoder-k-zero", {0U, 0x80U, 0x00U, 1U, 0U, 0U, 0U, 0U, 5U});
    add(sink, "decoder-k-large", {0U, 0xFFU, 0xFFU, 1U, 0U, 0U, 0U, 0U, 5U});

    // Wire: §3's figures, a valid form, each field out of its range.
    add(sink, "wire-valid", {1U, 0xABU, 0x12U, 0x34U, 0x56U, 0U, 0U, 0U, 1U, 0U, 0xFFU, 4U, 0U, 1U, 0U, 1U, 4U});
    add(sink, "wire-reserved", {1U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 1U, 0U, 0xA5U, 4U, 0U, 1U, 0U, 1U, 4U});
    add(sink, "wire-largest",
        {1U, 0U, 0U, 0U, 0U, 0xDBU, 0x75U, 0xFEU, 0xFAU, 0x53U, 0U, 0xFFU, 0xFFU, 0xFFU, 0U, 1U, 1U});
    add(sink, "wire-past-largest",
        {1U, 0U, 0U, 0U, 0U, 0xDBU, 0x75U, 0xFEU, 0xFAU, 0x54U, 0U, 0xFFU, 0xFFU, 0xFFU, 0U, 1U, 1U});
    add(sink, "wire-z-zero", {1U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 1U, 0U, 0U, 4U, 0U, 0U, 0U, 1U, 4U});
    add(sink, "wire-n-zero", {1U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 1U, 0U, 0U, 4U, 1U, 0U, 0U, 0U, 4U});
    add(sink, "wire-al-misfit", {1U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 1U, 0U, 0U, 6U, 1U, 0U, 1U, 4U});

    // Objects: one block, many, more blocks than symbols, sub-blocks, a short last symbol, an empty object.
    add(sink, "object-one-block", {2U, 3U, 3U, 0U, 0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 9U, 10U, 11U, 12U});
    add(sink, "object-blocks", {2U, 0U, 3U, 2U, 0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 9U, 10U, 11U, 12U});
    add(sink, "object-sub-blocks", {2U, 1U, 7U, 1U, 3U, 1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 9U, 10U, 11U, 12U, 13U, 14U});
    add(sink, "object-thin", {2U, 3U, 15U, 7U, 0U, 1U, 2U, 3U});
    add(sink, "object-empty", {2U, 3U, 3U, 2U, 1U});
    add(sink, "object-too-many-subs", {2U, 0U, 1U, 0U, 9U, 1U, 2U});

    // Derivations: a block that fits, many blocks, sub-blocks, each refusal.
    add(sink, "derive-fits",
        {3U, 0U, 0U, 0U, 0U, 0U, 0x10U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0x80U, 0U, 0U, 0U, 0U, 4U, 8U, 4U, 0U});
    add(sink, "derive-sub-blocks",
        {3U, 0U, 0U, 0U, 0U, 0U, 0x10U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0x04U, 0U, 0U, 0U, 0U, 4U, 8U, 4U, 0U});
    add(sink, "derive-blocks",
        {3U, 0U, 0U, 0U, 1U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0x10U, 0U, 0U, 0U, 0U, 4U, 8U, 4U, 0U});
    add(sink, "derive-no-memory",
        {3U, 0U, 0U, 0U, 0U, 0U, 0x10U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 8U, 0U, 0U, 4U, 8U, 4U, 0U});
    add(sink, "derive-misfit",
        {3U, 0U, 0U, 0U, 0U, 0U, 0x10U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0x80U, 0U, 0U, 0U, 0U, 4U, 8U, 4U, 2U});
}
