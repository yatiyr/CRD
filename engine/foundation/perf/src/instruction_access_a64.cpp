// crd-perf -- the AArch64 half of decode_instruction_access (HW-RESILIENCE.2): the data accesses of an A64
// instruction. See instruction_access.hpp. Every encoding follows the Arm ARM's A64 tables (DDI 0487 and the 2025
// extensions: LSE128, THE, LRCPC3, LSUI, LSFE, LS64, MTE, GCS); the 2026-11-01 session note records the sweep that
// checked the load and store group against LLVM 20.

#include "instruction_access_detail.hpp"

namespace crd::perf::detail
{
namespace
{
namespace cont = crd::containers;

constexpr crd::u8 kRead = kAccessRead;
constexpr crd::u8 kWrite = kAccessWrite;
constexpr crd::u8 kEither = kAccessRead | kAccessWrite;
constexpr crd::u32 kLength = 4U;
// DC ZVA's block (DCZID_EL0.BS) and a cache maintenance instruction's line (CTR_EL0.CWG) are not in the capture; the
// architecture bounds both at 2 KiB, and the aligned 2 KiB around the address holds either.
constexpr crd::u64 kMaxBlock = 2048U;
constexpr crd::u64 kTagGranule = 16U;

constexpr cont::StringView kUnlisted = "an encoding this decoder does not list";
constexpr cont::StringView kSve = "an SVE instruction, whose vector length and predicates are not in the capture";
constexpr cont::StringView kSme =
    "an SME instruction, whose streaming vector length and ZA storage are not in the capture";
constexpr cont::StringView kMops = "a memory copy or set (FEAT_MOPS), whose registers place only its current step";
constexpr cont::StringView kPac =
    "a pointer-authenticated load, whose address depends on an authentication the capture cannot repeat";
constexpr cont::StringView kGcs = "a call, a return or a Guarded Control Stack operation, whose GCS access (at "
                                  "GCSPR_EL0, not in the capture) cannot be placed";
constexpr cont::StringView kSystem =
    "an exception return or a system instruction, or one only a higher exception level executes";

struct A64
{
    crd::u32 w = 0U;
    const FaultRegisters& regs;
    InstructionAccess& out;
    cont::StringView why;
};

[[nodiscard]] constexpr crd::u32 bits(crd::u32 w, crd::u32 hi, crd::u32 lo) noexcept
{
    return (w >> lo) & ((crd::u32{1} << (hi - lo + 1U)) - 1U);
}

[[nodiscard]] constexpr bool bit(crd::u32 w, crd::u32 at) noexcept
{
    return ((w >> at) & 1U) != 0U;
}

// The field's value as a two's complement number of `width` bits, in 64-bit arithmetic.
[[nodiscard]] constexpr crd::u64 sext(crd::u64 v, crd::u32 width) noexcept
{
    const crd::u64 sign = crd::u64{1} << (width - 1U);
    return (v ^ sign) - sign;
}

// A base register (Rn): 31 names the stack pointer, which FaultRegisters keeps at gpr[31].
[[nodiscard]] crd::u64 base(const A64& d) noexcept
{
    return d.regs.gpr[bits(d.w, 9U, 5U)];
}

// A data or offset register: 31 names the zero register.
[[nodiscard]] crd::u64 xzr(const A64& d, crd::u32 r) noexcept
{
    return r == 31U ? 0U : d.regs.gpr[r];
}

bool access(A64& d, crd::u64 address, crd::u64 size, crd::u8 kinds) noexcept
{
    add_access(d.out, address & kAArch64AddressBits, size, kinds);
    return true;
}

bool refuse(A64& d, cont::StringView why) noexcept
{
    d.why = why;
    return false;
}

[[nodiscard]] crd::u8 load_or_store(const A64& d) noexcept
{
    return bit(d.w, 22U) ? kRead : kWrite;
}

[[nodiscard]] crd::u32 log2(crd::u64 n) noexcept
{
    crd::u32 s = 0U;
    while ((crd::u64{1} << s) < n)
    {
        ++s;
    }
    return s;
}

// ---- the load and store group (op1 = x1x0) -----------------------------------------------------------------------

// LDXR, STXR, LDAXR, STLXR, the pairs, LDAR, STLR, LDLAR, STLLR, CAS and CASP.
bool exclusive_ordered_cas(A64& d) noexcept
{
    const crd::u32 size = bits(d.w, 31U, 30U);
    const bool o2 = bit(d.w, 23U);
    const bool o1 = bit(d.w, 21U);
    if (!o1)
    {
        return access(d, base(d), crd::u64{1} << size, load_or_store(d));
    }
    if (o2)
    {
        return access(d, base(d), crd::u64{1} << size, kEither); // CAS
    }
    if (size >= 2U)
    {
        return access(d, base(d), 2U * (crd::u64{4} << (size & 1U)), load_or_store(d)); // LDXP, STXP
    }
    return access(d, base(d), 2U * (crd::u64{4} << size), kEither); // CASP
}

// FEAT_LSUI's unprivileged exclusives and compare and swaps: LDTXR, STTXR, CAST, CASPT.
bool unprivileged_exclusive(A64& d) noexcept
{
    const crd::u32 size = bits(d.w, 31U, 30U);
    if (bit(d.w, 21U))
    {
        return refuse(d, kUnlisted);
    }
    if (!bit(d.w, 23U))
    {
        return size >= 2U ? access(d, base(d), crd::u64{4} << (size & 1U), load_or_store(d)) : refuse(d, kUnlisted);
    }
    if (size == 3U)
    {
        return access(d, base(d), 8U, kEither); // CAST
    }
    return size == 1U ? access(d, base(d), 16U, kEither) : refuse(d, kUnlisted); // CASPT
}

// LD1 to LD4 and ST1 to ST4, multiple structures: each register's 8 or 16 bytes, contiguous.
bool multiple_structures(A64& d) noexcept
{
    const bool post = bit(d.w, 23U);
    if (bit(d.w, 31U) || bit(d.w, 21U) || (!post && bits(d.w, 20U, 16U) != 0U))
    {
        return refuse(d, kUnlisted);
    }
    const crd::u32 opcode = bits(d.w, 15U, 12U);
    crd::u32 registers = 0U;
    switch (opcode)
    {
        case 0x0U:
        case 0x2U:
            registers = 4U;
            break;
        case 0x4U:
        case 0x6U:
            registers = 3U;
            break;
        case 0x7U:
            registers = 1U;
            break;
        case 0x8U:
        case 0xAU:
            registers = 2U;
            break;
        default:
            return refuse(d, kUnlisted);
    }
    const bool q = bit(d.w, 30U);
    if (!q && bits(d.w, 11U, 10U) == 3U && (opcode == 0x0U || opcode == 0x4U || opcode == 0x8U))
    {
        return refuse(d, kUnlisted); // LD2 to LD4 of one 64-bit element per register
    }
    return access(d, base(d), crd::u64{registers} * (q ? 16U : 8U), load_or_store(d));
}

// LD1 to LD4 and ST1 to ST4 of one lane, LD1R to LD4R, and FEAT_LRCPC3's LDAP1 and STL1.
bool single_structure(A64& d) noexcept
{
    const bool post = bit(d.w, 23U);
    const bool r = bit(d.w, 21U);
    const crd::u32 opcode = bits(d.w, 15U, 13U);
    const bool s = bit(d.w, 12U);
    const crd::u32 size = bits(d.w, 11U, 10U);
    if (bit(d.w, 31U))
    {
        return refuse(d, kUnlisted);
    }
    if (!post && bits(d.w, 20U, 16U) == 1U && !r && opcode == 4U && !s && size == 1U)
    {
        return access(d, base(d), 8U, load_or_store(d)); // LDAP1, STL1 of a 64-bit lane
    }
    if (!post && bits(d.w, 20U, 16U) != 0U)
    {
        return refuse(d, kUnlisted);
    }
    const crd::u64 elements = (((opcode & 1U) << 1U) | (r ? 1U : 0U)) + 1U;
    switch (opcode >> 1U)
    {
        case 0U:
            return access(d, base(d), elements, load_or_store(d));
        case 1U:
            return (size & 1U) == 0U ? access(d, base(d), elements * 2U, load_or_store(d)) : refuse(d, kUnlisted);
        case 2U:
            if (size == 0U)
            {
                return access(d, base(d), elements * 4U, load_or_store(d));
            }
            return size == 1U && !s ? access(d, base(d), elements * 8U, load_or_store(d)) : refuse(d, kUnlisted);
        default:
            break;
    }
    // LD1R to LD4R: one element per register, replicated.
    return bit(d.w, 22U) && !s ? access(d, base(d), elements << size, kRead) : refuse(d, kUnlisted);
}

// LDR (literal), LDRSW (literal), PRFM (literal) and the SIMD and FP literal loads.
bool literal(A64& d) noexcept
{
    const crd::u32 opc = bits(d.w, 31U, 30U);
    const crd::u64 address = d.regs.pc + (sext(bits(d.w, 23U, 5U), 19U) << 2U);
    if (!bit(d.w, 26U))
    {
        constexpr crd::u64 sizes[4] = {4U, 8U, 4U, 0U}; // LDR W, LDR X, LDRSW, PRFM (no access)
        return opc == 3U || access(d, address, sizes[opc], kRead);
    }
    return opc != 3U ? access(d, address, crd::u64{4} << opc, kRead) : refuse(d, kUnlisted);
}

// A single register's load or store in the register classes: its size in bytes and what it does (0 bytes: a
// prefetch, which makes no access). False: unallocated.
struct Operand
{
    crd::u64 size = 0U;
    crd::u8 kinds = 0U;
};

[[nodiscard]] bool register_operand(crd::u32 size, bool simd, crd::u32 opc, Operand& o) noexcept
{
    const crd::u64 bytes = crd::u64{1} << size;
    if (simd)
    {
        if (opc >= 2U && size != 0U)
        {
            return false;
        }
        o.size = opc >= 2U ? 16U : bytes;
        o.kinds = (opc & 1U) != 0U ? kRead : kWrite;
        return true;
    }
    switch (opc)
    {
        case 0U:
            o = Operand{bytes, kWrite};
            return true;
        case 1U:
            o = Operand{bytes, kRead};
            return true;
        case 2U:
            o = Operand{size == 3U ? 0U : bytes, kRead}; // LDRSB, LDRSH, LDRSW to 64 bits; PRFM
            return true;
        default:
            o = Operand{bytes, kRead}; // LDRSB, LDRSH to 32 bits
            return size < 2U;
    }
}

// FEAT_LRCPC2's LDAPUR and STLUR and their signed loads (general registers).
bool rcpc_unscaled(A64& d) noexcept
{
    const crd::u32 size = bits(d.w, 31U, 30U);
    const crd::u32 opc = bits(d.w, 23U, 22U);
    if ((opc == 2U && size == 3U) || (opc == 3U && size >= 2U))
    {
        return refuse(d, kUnlisted);
    }
    return access(d, base(d) + sext(bits(d.w, 20U, 12U), 9U), crd::u64{1} << size, opc == 0U ? kWrite : kRead);
}

// FEAT_LRCPC3's pairs (LDIAPP, STILP) and writeback forms (LDAPR post-indexed, STLR pre-indexed): a store with
// writeback stores below its base.
bool rcpc3(A64& d) noexcept
{
    const crd::u32 size = bits(d.w, 31U, 30U);
    const crd::u32 opc = bits(d.w, 23U, 22U);
    if (size < 2U)
    {
        return refuse(d, kUnlisted);
    }
    const crd::u64 n = crd::u64{4} << (size & 1U);
    if (opc >= 2U)
    {
        if (bits(d.w, 20U, 12U) != 0U)
        {
            return refuse(d, kUnlisted);
        }
        return opc == 2U ? access(d, base(d) - n, n, kWrite) : access(d, base(d), n, kRead);
    }
    const crd::u32 opc2 = bits(d.w, 15U, 12U);
    if (opc2 > 1U)
    {
        return refuse(d, kUnlisted);
    }
    const bool store = opc == 0U;
    const crd::u64 address = store && opc2 == 0U ? base(d) - 2U * n : base(d);
    return access(d, address, 2U * n, store ? kWrite : kRead);
}

// FEAT_MTE's tag loads and stores: STG, STZG, ST2G, STZ2G and LDG act on whole 16-byte granules.
bool memory_tags(A64& d) noexcept
{
    const crd::u32 opc = bits(d.w, 23U, 22U);
    const crd::u32 mode = bits(d.w, 11U, 10U); // 0: LDG and the EL1 forms; 1: post-indexed; 2: offset; 3: pre-indexed
    const crd::u64 offset = sext(bits(d.w, 20U, 12U), 9U) << 4U;
    if (mode == 0U)
    {
        return opc == 1U ? access(d, (base(d) + offset) & ~(kTagGranule - 1U), kTagGranule, kRead)
                         : refuse(d, kSystem); // STZGM, STGM, LDGM
    }
    const crd::u64 address = mode == 1U ? base(d) : base(d) + offset;
    const crd::u64 aligned = address & ~(kTagGranule - 1U);
    const crd::u64 n = opc >= 2U ? 2U * kTagGranule : kTagGranule;
    return access(d, aligned, address - aligned + n, kWrite);
}

// The class at bits 29:24 = 011001: RCpc, LRCPC3, MOPS, GCS stores, LSE128, RCW, LSUI atomics and MTE.
bool class_19(A64& d) noexcept
{
    const crd::u32 size = bits(d.w, 31U, 30U);
    const crd::u32 mode = bits(d.w, 11U, 10U);
    const crd::u32 op = bits(d.w, 15U, 12U);
    if (!bit(d.w, 21U))
    {
        switch (mode)
        {
            case 0U:
                return rcpc_unscaled(d);
            case 1U:
                return refuse(d, size == 0U ? kMops : kUnlisted);
            case 2U:
                return rcpc3(d);
            default:
                break;
        }
        // GCSSTR, GCSSTTR: a store to the Guarded Control Stack's memory at the base.
        const bool gcs_store =
            size == 3U && bits(d.w, 23U, 22U) == 0U && bits(d.w, 20U, 16U) == 31U && bits(d.w, 15U, 13U) == 0U;
        return gcs_store ? access(d, base(d), 8U, kWrite) : refuse(d, kUnlisted);
    }
    if (size == 3U)
    {
        return memory_tags(d);
    }
    if (size == 2U)
    {
        return refuse(d, kUnlisted);
    }
    switch (mode)
    {
        case 0U:
        {
            // LDCLRP, LDSETP, SWPP (size 0) and RCWCLRP, RCWSWPP, RCWSETP and their S forms: 128 bits.
            const bool lse128 = size == 0U && (op == 1U || op == 3U || op == 8U);
            return lse128 || (op >= 9U && op <= 11U) ? access(d, base(d), 16U, kEither) : refuse(d, kUnlisted);
        }
        case 1U: // LDTADD, LDTCLR, LDTSET, SWPT
            return op == 0U || op == 1U || op == 3U || op == 8U ? access(d, base(d), crd::u64{4} << size, kEither)
                                                                : refuse(d, kUnlisted);
        default: // RCWCAS (8 bytes) and RCWCASP (16), with their S forms
            return op == 0U ? access(d, base(d), mode == 2U ? 8U : 16U, kEither) : refuse(d, kUnlisted);
    }
}

// The class at bits 29:24 = 011101: MOPS (CPY, SETG) and FEAT_LRCPC3's SIMD and FP LDAPUR and STLUR.
bool class_1d(A64& d) noexcept
{
    const crd::u32 size = bits(d.w, 31U, 30U);
    const crd::u32 mode = bits(d.w, 11U, 10U);
    if (bit(d.w, 21U) || (mode != 1U && mode != 2U))
    {
        return refuse(d, kUnlisted);
    }
    if (mode == 1U)
    {
        return refuse(d, size == 0U ? kMops : kUnlisted);
    }
    Operand o;
    if (!register_operand(size, true, bits(d.w, 23U, 22U), o))
    {
        return refuse(d, kUnlisted);
    }
    return access(d, base(d) + sext(bits(d.w, 20U, 12U), 9U), o.size, o.kinds);
}

// LDP, STP, LDNP, STNP, LDPSW, STGP and FEAT_LSUI's LDTP, STTP, LDTNP and STTNP, general and SIMD and FP.
bool pair(A64& d) noexcept
{
    const crd::u32 opc = bits(d.w, 31U, 30U);
    const crd::u32 mode = bits(d.w, 24U, 23U); // 0: no-allocate; 1: post-indexed; 2: offset; 3: pre-indexed
    const bool load = bit(d.w, 22U);
    crd::u64 element = 0U;
    if (bit(d.w, 26U))
    {
        constexpr crd::u64 simd_sizes[4] = {4U, 8U, 16U, 16U}; // S, D, Q, and LSUI's Q pairs
        element = simd_sizes[opc];
    }
    else
    {
        constexpr crd::u64 general_sizes[4] = {4U, 4U, 8U, 8U}; // W, LDPSW, X, and LSUI's X pairs
        element = general_sizes[opc];
        if (opc == 1U && mode == 0U)
        {
            return refuse(d, kUnlisted);
        }
        if (opc == 1U && !load)
        {
            // STGP: two registers' 16 bytes and the granule's tag, its offset scaled by the granule.
            const crd::u64 offset = sext(bits(d.w, 21U, 15U), 7U) << 4U;
            return access(d, mode == 1U ? base(d) : base(d) + offset, 16U, kWrite);
        }
    }
    const crd::u64 offset = sext(bits(d.w, 21U, 15U), 7U) * element;
    return access(d, mode == 1U ? base(d) : base(d) + offset, 2U * element, load ? kRead : kWrite);
}

// The atomic memory operations (bits 21 = 1, 11:10 = 00): LSE, RCW, LDAPR, LS64 and FEAT_LSFE's floating point.
bool atomics(A64& d) noexcept
{
    const crd::u32 size = bits(d.w, 31U, 30U);
    const crd::u32 ar = bits(d.w, 23U, 22U);
    const crd::u32 op = bits(d.w, 15U, 12U);
    const bool rs_zero = bits(d.w, 20U, 16U) == 31U;
    if (bit(d.w, 26U))
    {
        // LDFADD, LDFMAX, LDFMIN, LDFMAXNM, LDFMINNM (and STF*): BF16 at size 0, then H, S and D.
        const bool listed = op == 0U || (op >= 4U && op <= 7U);
        return listed ? access(d, base(d), size == 0U ? 2U : crd::u64{1} << size, kEither) : refuse(d, kUnlisted);
    }
    if (op <= 8U)
    {
        return access(d, base(d), crd::u64{1} << size, kEither); // LDADD to LDUMIN, SWP (and ST* aliases)
    }
    if (op <= 11U && size < 2U)
    {
        return access(d, base(d), 8U, kEither); // RCWCLR, RCWSWP, RCWSET and their S forms
    }
    if (op <= 11U && size == 3U && ar == 0U && (op != 9U || rs_zero))
    {
        return access(d, base(d), 64U, kWrite); // ST64B, ST64BV0, ST64BV
    }
    if (op == 12U && ar == 2U && rs_zero)
    {
        return access(d, base(d), crd::u64{1} << size, kRead); // LDAPR
    }
    if (op == 13U && size == 3U && ar == 0U && rs_zero)
    {
        return access(d, base(d), 64U, kRead); // LD64B
    }
    return refuse(d, kUnlisted);
}

// The register classes (bits 29:24 = 111x00): immediate (unscaled, post-indexed, unprivileged, pre-indexed),
// register offset, atomics and the pointer-authenticated loads.
bool register_forms(A64& d) noexcept
{
    const crd::u32 size = bits(d.w, 31U, 30U);
    const bool simd = bit(d.w, 26U);
    const crd::u32 mode = bits(d.w, 11U, 10U);
    Operand o;
    if (!bit(d.w, 21U))
    {
        const bool prefetch_mode = mode != 0U && !simd && size == 3U && bits(d.w, 23U, 22U) == 2U;
        if ((simd && mode == 2U) || prefetch_mode || !register_operand(size, simd, bits(d.w, 23U, 22U), o))
        {
            return refuse(d, kUnlisted); // no SIMD unprivileged forms; PRFUM has no indexed or unprivileged form
        }
        const crd::u64 address = mode == 1U ? base(d) : base(d) + sext(bits(d.w, 20U, 12U), 9U);
        return o.size == 0U || access(d, address, o.size, o.kinds);
    }
    if (mode == 0U)
    {
        return atomics(d);
    }
    if (mode != 2U)
    {
        return !simd && size == 3U ? refuse(d, kPac) : refuse(d, kUnlisted); // LDRAA, LDRAB
    }
    const crd::u32 option = bits(d.w, 15U, 13U);
    if ((option & 2U) == 0U || !register_operand(size, simd, bits(d.w, 23U, 22U), o))
    {
        return refuse(d, kUnlisted);
    }
    if (o.size == 0U)
    {
        return true; // PRFM and RPRFM
    }
    crd::u64 offset = xzr(d, bits(d.w, 20U, 16U));
    switch (option)
    {
        case 2U: // UXTW
            offset &= 0xFFFFFFFFU;
            break;
        case 6U: // SXTW
            offset = sext(offset & 0xFFFFFFFFU, 32U);
            break;
        default: // LSL (UXTX), SXTX
            break;
    }
    const crd::u32 shift = bit(d.w, 12U) ? log2(o.size) : 0U;
    return access(d, base(d) + (offset << shift), o.size, o.kinds);
}

// LDR, STR and their sizes with an unsigned offset scaled by the access size.
bool unsigned_offset(A64& d) noexcept
{
    Operand o;
    if (!register_operand(bits(d.w, 31U, 30U), bit(d.w, 26U), bits(d.w, 23U, 22U), o))
    {
        return refuse(d, kUnlisted);
    }
    if (o.size == 0U)
    {
        return true; // PRFM
    }
    return access(d, base(d) + (crd::u64{bits(d.w, 21U, 10U)} << log2(o.size)), o.size, o.kinds);
}

bool loads_and_stores(A64& d) noexcept
{
    switch (bits(d.w, 29U, 24U))
    {
        case 0x08U:
            return exclusive_ordered_cas(d);
        case 0x09U:
            return unprivileged_exclusive(d);
        case 0x0CU:
            return multiple_structures(d);
        case 0x0DU:
            return single_structure(d);
        case 0x18U:
        case 0x1CU:
            return literal(d);
        case 0x19U:
            return class_19(d);
        case 0x1DU:
            return class_1d(d);
        case 0x28U:
        case 0x29U:
        case 0x2CU:
        case 0x2DU:
            return pair(d);
        case 0x38U:
        case 0x3CU:
            return register_forms(d);
        default: // 0x39, 0x3D
            return unsigned_offset(d);
    }
}

// ---- branches, exception generation and system instructions (op1 = 101x) --------------------------------------------

// SYS and SYSL (op0 = 01): the data cache and instruction cache maintenance an application may execute.
bool sys(A64& d) noexcept
{
    const crd::u32 op1 = bits(d.w, 18U, 16U);
    const crd::u32 crn = bits(d.w, 15U, 12U);
    const crd::u32 crm = bits(d.w, 11U, 8U);
    const crd::u32 op2 = bits(d.w, 7U, 5U);
    if (crn == 7U && crm == 7U && (op1 == 3U || (op1 == 0U && !bit(d.w, 21U))))
    {
        return refuse(d, kGcs); // GCSPUSHM, GCSPOPM, GCSSS1, GCSSS2, GCSPUSHX, GCSPOPX, GCSPOPCX
    }
    if (bit(d.w, 21U) || op1 != 3U || crn != 7U)
    {
        return refuse(d, kSystem);
    }
    const crd::u64 block = xzr(d, bits(d.w, 4U, 0U)) & ~(kMaxBlock - 1U);
    if (crm == 4U && (op2 == 1U || op2 == 3U || op2 == 4U))
    {
        return access(d, block, kMaxBlock, kWrite); // DC ZVA, DC GVA, DC GZVA
    }
    const bool cache = (crm == 5U && op2 == 1U) ||                                 // IC IVAU
                       (crm == 11U && op2 == 1U) ||                                // DC CVAU
                       ((crm == 11U || crm == 15U) && (op2 == 0U || op2 == 7U)) || // DC CVAOC, CIVAOC (OCCMO)
                       ((crm == 10U || crm == 12U || crm == 13U || crm == 14U) &&  // DC CVAC, CVAP, CVADP, CIVAC
                        (op2 == 1U || op2 == 3U || op2 == 5U));                    // and their tag forms
    return cache ? access(d, block, kMaxBlock, kEither) : refuse(d, kSystem);
}

bool system(A64& d) noexcept
{
    const crd::u32 op0 = bits(d.w, 20U, 19U);
    if (op0 == 1U)
    {
        return sys(d);
    }
    if (op0 >= 2U)
    {
        return true; // MRS, MSR (register)
    }
    const crd::u32 op1 = bits(d.w, 18U, 16U);
    const crd::u32 crn = bits(d.w, 15U, 12U);
    const bool rt_zero = bits(d.w, 4U, 0U) == 31U;
    if (bit(d.w, 21U))
    {
        return refuse(d, kUnlisted);
    }
    const bool pstate = crn == 4U && rt_zero;
    const bool hint_or_barrier = op1 == 3U && (crn == 2U || crn == 3U) && rt_zero;
    // STSHH, a hint for the store that follows it.
    const bool store_hint = op1 == 1U && crn == 9U && bits(d.w, 11U, 8U) == 6U && rt_zero;
    const bool wait = op1 == 3U && crn == 1U && bits(d.w, 11U, 8U) == 0U && bits(d.w, 7U, 5U) <= 1U; // WFET, WFIT
    return pstate || hint_or_barrier || store_hint || wait || refuse(d, kUnlisted);
}

bool branch_exception_system(A64& d) noexcept
{
    const crd::u32 w = d.w;
    if (bits(w, 30U, 26U) == 0x05U)
    {
        return !bit(w, 31U) || refuse(d, kGcs); // B; BL pushes to the GCS
    }
    if (bits(w, 30U, 25U) == 0x1AU || bits(w, 30U, 25U) == 0x1BU)
    {
        return true; // CBZ, CBNZ, TBZ, TBNZ
    }
    if (bits(w, 30U, 25U) == 0x3AU)
    {
        // FEAT_CMPBR: CB<cc> with a register (and CBB<cc>, CBH<cc> on W) or with an immediate; conditions 4 and 5
        // are unallocated.
        const crd::u32 cc = bits(w, 23U, 21U);
        const crd::u32 form = bits(w, 15U, 14U);
        const bool listed = bit(w, 24U) ? !bit(w, 14U) : form == 0U || (form >= 2U && !bit(w, 31U));
        return (cc != 4U && cc != 5U && listed) || refuse(d, kUnlisted);
    }
    switch (bits(w, 31U, 24U))
    {
        case 0x54U:
            return true; // B.cond, BC.cond
        case 0x55U:
            return refuse(d, bits(w, 23U, 22U) == 0U && bits(w, 4U, 0U) == 31U ? kGcs : kUnlisted); // RETA?SPPC
        case 0xD4U:
        {
            const crd::u32 opc = bits(w, 23U, 21U);
            const crd::u32 ll = bits(w, 1U, 0U);
            const bool listed =
                bits(w, 4U, 2U) == 0U && ((opc == 0U && ll == 1U) || (opc <= 2U && opc != 0U && ll == 0U));
            return listed || refuse(d, kSystem); // SVC, BRK and HLT make no access
        }
        case 0xD5U:
            return bits(w, 23U, 22U) != 0U ? refuse(d, kSystem) : system(d); // SYSP, MSRR, MRRS and unallocated
        case 0xD6U:
        case 0xD7U:
        {
            // BR, BRAAZ, BRABZ, BRAA, BRAB make no data access; calls and returns use the GCS.
            const crd::u32 opc = bits(w, 24U, 21U);
            const crd::u32 op3 = bits(w, 15U, 10U);
            const crd::u32 op4 = bits(w, 4U, 0U);
            const bool keyed = op3 == 2U || op3 == 3U;
            if (bits(w, 20U, 16U) != 31U)
            {
                return refuse(d, kUnlisted);
            }
            if ((opc == 0U && ((op3 == 0U && op4 == 0U) || (keyed && op4 == 31U))) || (opc == 8U && keyed))
            {
                return true;
            }
            if (opc == 1U || opc == 2U || opc == 9U)
            {
                return refuse(d, kGcs);
            }
            return refuse(d, opc == 4U || opc == 5U ? kSystem : kUnlisted);
        }
        default:
            return refuse(d, kUnlisted);
    }
}

} // namespace

InstructionAccess decode_a64(cont::ConstSpan<crd::u8> code, const FaultRegisters& regs) noexcept
{
    InstructionAccess out;
    if (code.size() < kLength)
    {
        out.reason = "the capture holds fewer bytes than the instruction";
        return out;
    }
    const crd::u32 word =
        crd::u32{code[0]} | (crd::u32{code[1]} << 8U) | (crd::u32{code[2]} << 16U) | (crd::u32{code[3]} << 24U);
    A64 d{word, regs, out, {}};
    const crd::u32 op1 = bits(d.w, 28U, 25U);
    bool decoded = true;
    if (op1 == 0U)
    {
        // SME (op0 = 1) and the reserved space, whose one allocated instruction, UDF, makes no access.
        decoded = !bit(d.w, 31U) && bits(d.w, 31U, 16U) == 0U ? true : refuse(d, bit(d.w, 31U) ? kSme : kUnlisted);
    }
    else if (op1 == 2U)
    {
        decoded = refuse(d, kSve);
    }
    else if (op1 == 1U || op1 == 3U)
    {
        decoded = refuse(d, kUnlisted);
    }
    else if ((op1 & 5U) == 4U)
    {
        decoded = loads_and_stores(d);
    }
    else if ((op1 & 0xEU) == 0xAU)
    {
        decoded = branch_exception_system(d);
    }
    // Data processing (immediate 100x, register x101, SIMD and floating point x111) reaches no memory.
    if (!decoded)
    {
        InstructionAccess refused;
        refused.reason = d.why;
        return refused;
    }
    out.length = kLength;
    out.decoded = true;
    return out;
}

} // namespace crd::perf::detail
