// crd-perf -- the memory accesses a faulting instruction makes (HW-RESILIENCE.2). See instruction_access.hpp.

#include "instruction_access_detail.hpp"

namespace crd::perf
{
namespace
{
namespace cont = crd::containers;

constexpr crd::u32 kRax = 0U;
constexpr crd::u32 kRcx = 1U;
constexpr crd::u32 kRdx = 2U;
constexpr crd::u32 kRbx = 3U;
constexpr crd::u32 kRsp = 4U;
constexpr crd::u32 kRbp = 5U;
constexpr crd::u32 kRsi = 6U;
constexpr crd::u32 kRdi = 7U;
constexpr crd::u64 kDirectionFlag = crd::u64{1} << 10U;
constexpr crd::u32 kMaxX86Length = 15U;
constexpr crd::u8 kRead = kAccessRead;
constexpr crd::u8 kWrite = kAccessWrite;
constexpr crd::u8 kEither = kAccessRead | kAccessWrite;

constexpr cont::StringView kUnlisted = "an opcode this decoder does not list";
constexpr cont::StringView kShadowStack = "a near call or return, whose shadow-stack access under CET (at SSP, not in "
                                          "the capture) cannot be placed";
constexpr cont::StringView kSystem = "a far transfer, interrupt, I/O, system or privileged instruction";
constexpr cont::StringView kVendor = "a 66 prefix on a near branch, whose length depends on the vendor";

// How an instruction reaches memory beyond its r/m operand.
enum class Implicit : crd::u8
{
    None,
    Push,      // writes the stack below rsp
    Pop,       // reads the stack at rsp
    PushRm,    // reads the r/m operand, then writes the stack (FF /6)
    PopRm,     // reads the stack, then writes the r/m operand (8F /0), whose rsp base counts after the pop
    Leave,     // reads the stack at rbp
    String,    // MOVS, CMPS, STOS, LODS, SCAS through rsi and rdi, repeated rcx times under REP
    Xlat,      // reads [rbx + al]
    Moffs,     // A0 to A3: an absolute address in place of a ModRM
    MaskMove,  // MASKMOVQ, (V)MASKMOVDQU: writes the bytes the mask selects from [rdi]
    BitOffset, // BT, BTS, BTR, BTC with a register bit offset: the bit's byte, at any distance from the r/m address
    CacheLine, // CLFLUSH, CLFLUSHOPT, CLWB: the line holding the r/m address
    Divide,    // DIV, IDIV: reads the r/m divisor
};

// One opcode's form: its immediate and what its r/m operand does when it names memory.
struct Form
{
    cont::StringView refuse; // why it is not decoded (empty: decoded)
    crd::u32 imm = 0U;
    crd::u8 kinds = 0U; // the r/m memory operand's access (0: none, as LEA, prefetches and hint NOPs)
    crd::u32 size = 0U; // its bound in bytes
    Implicit implicit = Implicit::None;
    crd::u32 implicit_size = 0U;
};

[[nodiscard]] Form refused(cont::StringView why) noexcept
{
    Form f;
    f.refuse = why;
    return f;
}

[[nodiscard]] Form operand(crd::u8 kinds, crd::u32 size, crd::u32 imm = 0U) noexcept
{
    Form f;
    f.kinds = kinds;
    f.size = size;
    f.imm = imm;
    return f;
}

[[nodiscard]] Form no_access(crd::u32 imm = 0U) noexcept
{
    return operand(0U, 0U, imm);
}

[[nodiscard]] Form implicit(Implicit what, crd::u32 size, crd::u32 imm = 0U) noexcept
{
    Form f;
    f.implicit = what;
    f.implicit_size = size;
    f.imm = imm;
    return f;
}

[[nodiscard]] Form bit_offset(crd::u8 kinds) noexcept
{
    Form f = implicit(Implicit::BitOffset, 0U);
    f.kinds = kinds;
    return f;
}

struct X86
{
    cont::ConstSpan<crd::u8> code;
    const FaultRegisters& regs;
    crd::u32 at = 0U;
    bool cut = false;
    bool too_long = false;
    // Prefixes.
    bool opsize = false;
    bool addrsize = false;
    bool rep = false;   // F3
    bool repne = false; // F2
    bool lock = false;
    bool segment = false; // FS or GS: a base the capture does not hold
    crd::u8 rex = 0U;     // REX, or the bits VEX and EVEX carry for it
    bool evex = false;
    crd::u32 vector_bytes = 16U; // a vector memory operand's bound
    // The ModRM operand.
    bool has_modrm = false;
    crd::u32 mod = 0U;
    crd::u32 reg = 0U;
    crd::u32 rm = 0U;
    crd::u8 modrm = 0U;
    bool memory = false;
    bool rip = false;
    bool has_base = false;
    bool has_index = false;
    crd::u32 base = 0U;
    crd::u32 index = 0U;
    crd::u32 scale = 0U;
    crd::u64 disp = 0U;
    bool disp8 = false;

    crd::u8 byte() noexcept
    {
        if (at >= kMaxX86Length)
        {
            too_long = true;
            return 0U;
        }
        if (at >= code.size())
        {
            cut = true;
            return 0U;
        }
        return code[at++];
    }

    crd::u64 bytes(crd::u32 n) noexcept // little endian
    {
        crd::u64 v = 0U;
        for (crd::u32 i = 0U; i < n; ++i)
        {
            v |= crd::u64{byte()} << (8U * i);
        }
        return v;
    }

    [[nodiscard]] bool w() const noexcept { return (rex & 8U) != 0U; }
    [[nodiscard]] crd::u32 iz() const noexcept { return opsize ? 2U : 4U; }
    [[nodiscard]] crd::u32 osz() const noexcept { return w() ? 8U : iz(); }
    [[nodiscard]] crd::u32 stack_size() const noexcept { return opsize ? 2U : 8U; } // push and pop default to 64 bits
    [[nodiscard]] crd::u64 gpr(crd::u32 r) const noexcept { return regs.gpr[r & 15U]; }
    [[nodiscard]] crd::u64 address(crd::u64 a) const noexcept { return addrsize ? (a & 0xFFFFFFFFU) : a; }

    void read_modrm() noexcept
    {
        has_modrm = true;
        modrm = byte();
        mod = modrm >> 6U;
        reg = (modrm >> 3U) & 7U;
        rm = modrm & 7U;
        if (mod == 3U)
        {
            return;
        }
        memory = true;
        if (rm == 4U)
        {
            const crd::u8 sib = byte();
            scale = sib >> 6U;
            index = ((sib >> 3U) & 7U) | ((rex & 2U) != 0U ? 8U : 0U);
            has_index = index != kRsp; // index 4 without REX.X is none; r12 is an index
            if ((sib & 7U) == 5U && mod == 0U)
            {
                disp = static_cast<crd::u64>(static_cast<crd::i64>(static_cast<crd::i32>(bytes(4U))));
            }
            else
            {
                has_base = true;
                base = (sib & 7U) | ((rex & 1U) != 0U ? 8U : 0U);
            }
        }
        else if (rm == 5U && mod == 0U)
        {
            rip = true;
            disp = static_cast<crd::u64>(static_cast<crd::i64>(static_cast<crd::i32>(bytes(4U))));
        }
        else
        {
            has_base = true;
            base = rm | ((rex & 1U) != 0U ? 8U : 0U);
        }
        if (mod == 1U)
        {
            disp = static_cast<crd::u64>(static_cast<crd::i64>(static_cast<crd::i8>(byte())));
            disp8 = true;
        }
        else if (mod == 2U)
        {
            disp = static_cast<crd::u64>(static_cast<crd::i64>(static_cast<crd::i32>(bytes(4U))));
        }
    }

    // The r/m operand's address with `displacement`, once the whole instruction is read (RIP counts from its end).
    [[nodiscard]] crd::u64 effective(crd::u64 displacement, crd::u64 rsp_bias) const noexcept
    {
        crd::u64 ea = rip ? regs.pc + at : 0U;
        if (has_base)
        {
            ea += gpr(base) + (base == kRsp ? rsp_bias : 0U);
        }
        if (has_index)
        {
            ea += gpr(index) << scale;
        }
        return address(ea + displacement);
    }
};

[[nodiscard]] cont::StringView cut_reason(const X86& d) noexcept
{
    return d.too_long ? cont::StringView{"longer than 15 bytes, which is invalid"}
                      : cont::StringView{"the capture holds fewer bytes than the instruction"};
}

using detail::add_access;

// ---- forms ----------------------------------------------------------------------------------------------------------

[[nodiscard]] bool one_byte_has_modrm(crd::u8 op) noexcept
{
    if (op < 0x40U)
    {
        return (op & 7U) < 4U;
    }
    return op == 0x63U || op == 0x69U || op == 0x6BU || (op >= 0x80U && op <= 0x8FU) || op == 0xC0U || op == 0xC1U ||
           op == 0xC6U || op == 0xC7U || (op >= 0xD0U && op <= 0xD3U) || (op >= 0xD8U && op <= 0xDFU) || op == 0xF6U ||
           op == 0xF7U || op == 0xFEU || op == 0xFFU;
}

[[nodiscard]] bool two_byte_has_modrm(crd::u8 op) noexcept
{
    return !((op >= 0x04U && op <= 0x0CU) || op == 0x0EU || (op >= 0x30U && op <= 0x37U) || op == 0x77U ||
             (op >= 0x80U && op <= 0x8FU) || (op >= 0xA0U && op <= 0xA2U) || (op >= 0xA8U && op <= 0xAAU) ||
             (op >= 0xC8U && op <= 0xCFU));
}

struct X87Row
{
    crd::u8 kinds; // 0: not a valid form
    crd::u32 size; // 0: the environment (28 or 14 bytes); 1: the whole state (108 or 94)
};

// D8 to DF's memory forms by opcode and ModRM.reg. Register forms work on the FPU stack and touch no memory.
[[nodiscard]] Form x87(const X86& d, crd::u8 op) noexcept
{
    if (!d.memory)
    {
        return no_access();
    }
    static constexpr X87Row kD9[8] = {{kRead, 4U}, {0U, 0U},    {kWrite, 4U}, {kWrite, 4U},
                                      {kRead, 0U}, {kRead, 2U}, {kWrite, 0U}, {kWrite, 2U}};
    static constexpr X87Row kDB[8] = {{kRead, 4U}, {kWrite, 4U}, {kWrite, 4U}, {kWrite, 4U},
                                      {0U, 0U},    {kRead, 10U}, {0U, 0U},     {kWrite, 10U}};
    static constexpr X87Row kDD[8] = {{kRead, 8U}, {kWrite, 8U}, {kWrite, 8U}, {kWrite, 8U},
                                      {kRead, 1U}, {0U, 0U},     {kWrite, 1U}, {kWrite, 2U}};
    static constexpr X87Row kDF[8] = {{kRead, 2U},  {kWrite, 2U}, {kWrite, 2U},  {kWrite, 2U},
                                      {kRead, 10U}, {kRead, 8U},  {kWrite, 10U}, {kWrite, 8U}};
    X87Row row{kRead, 4U}; // D8 and DA: 32-bit real and integer operands
    switch (static_cast<crd::u32>(op))
    {
        case 0xD9U:
            row = kD9[d.reg];
            break;
        case 0xDBU:
            row = kDB[d.reg];
            break;
        case 0xDCU:
            row = X87Row{kRead, 8U};
            break;
        case 0xDDU:
            row = kDD[d.reg];
            break;
        case 0xDEU:
            row = X87Row{kRead, 2U};
            break;
        case 0xDFU:
            row = kDF[d.reg];
            break;
        default:
            break;
    }
    if (row.kinds == 0U)
    {
        return refused(kUnlisted);
    }
    crd::u32 size = row.size;
    if (size == 0U)
    {
        size = d.opsize ? 14U : 28U; // FLDENV, FNSTENV
    }
    else if (size == 1U)
    {
        size = d.opsize ? 94U : 108U; // FRSTOR, FNSAVE
    }
    return operand(row.kinds, size);
}

[[nodiscard]] Form one_byte(X86& d, crd::u8 op) noexcept
{
    const crd::u32 osz = d.osz();
    if (op < 0x40U)
    {
        const crd::u32 low = op & 7U;
        const bool compare = (op & 0x38U) == 0x38U; // CMP reads; ADD, OR, ADC, SBB, AND, SUB, XOR read and write
        switch (low)
        {
            case 0U:
                return operand(compare ? kRead : kEither, 1U);
            case 1U:
                return operand(compare ? kRead : kEither, osz);
            case 2U:
                return operand(kRead, 1U);
            case 3U:
                return operand(kRead, osz);
            case 4U:
                return no_access(1U);
            case 5U:
                return no_access(d.iz());
            default:
                return refused(kUnlisted); // PUSH and POP of segments, DAA and the like: invalid in 64-bit mode
        }
    }
    if (op >= 0x50U && op <= 0x57U)
    {
        return implicit(Implicit::Push, d.stack_size());
    }
    if (op >= 0x58U && op <= 0x5FU)
    {
        return implicit(Implicit::Pop, d.stack_size());
    }
    if ((op >= 0x70U && op <= 0x7FU) || (op >= 0xB0U && op <= 0xB7U) || (op >= 0xE0U && op <= 0xE3U) || op == 0xEBU ||
        op == 0xA8U)
    {
        return no_access(1U); // short branches, LOOP, JRCXZ, MOV r8 and TEST AL with an 8-bit immediate
    }
    if (op >= 0x90U && op <= 0x99U)
    {
        return no_access(); // NOP, PAUSE, XCHG with rax, CBW and CWD
    }
    if (op >= 0xB8U && op <= 0xBFU)
    {
        return no_access(d.w() ? 8U : d.iz());
    }
    if (op >= 0xD8U && op <= 0xDFU)
    {
        return x87(d, op);
    }
    const crd::u8 group = d.reg == 7U ? kRead : kEither; // group 1's CMP reads
    switch (static_cast<crd::u32>(op))
    {
        case 0x63U:
            return operand(kRead, d.opsize ? 2U : 4U); // MOVSXD
        case 0x68U:
            return implicit(Implicit::Push, d.stack_size(), d.iz());
        case 0x69U:
            return operand(kRead, osz, d.iz());
        case 0x6AU:
            return implicit(Implicit::Push, d.stack_size(), 1U);
        case 0x6BU:
            return operand(kRead, osz, 1U);
        case 0x80U:
            return operand(group, 1U, 1U);
        case 0x81U:
            return operand(group, osz, d.iz());
        case 0x83U:
            return operand(group, osz, 1U);
        case 0x84U:
        case 0x8AU:
            return operand(kRead, 1U);
        case 0x85U:
        case 0x8BU:
            return operand(kRead, osz);
        case 0x86U:
            return operand(kEither, 1U);
        case 0x87U:
            return operand(kEither, osz);
        case 0x88U:
            return operand(kWrite, 1U);
        case 0x89U:
            return operand(kWrite, osz);
        case 0x8CU:
            return operand(kWrite, 2U);
        case 0x8DU:
            return no_access(); // LEA computes the address and reads nothing
        case 0x8FU:
            if (d.reg != 0U)
            {
                return refused("an AMD XOP instruction");
            }
            return implicit(d.memory ? Implicit::PopRm : Implicit::Pop, d.stack_size());
        case 0x9BU:
        case 0x9EU:
        case 0x9FU:
        case 0xF5U:
        case 0xF8U:
        case 0xF9U:
        case 0xFCU:
        case 0xFDU:
            return no_access(); // FWAIT, SAHF, LAHF, CMC, CLC, STC, CLD, STD
        case 0x9CU:
            return implicit(Implicit::Push, d.stack_size()); // PUSHF
        case 0x9DU:
            return implicit(Implicit::Pop, d.stack_size()); // POPF
        case 0xA0U:
            return implicit(Implicit::Moffs, 1U);
        case 0xA1U:
            return implicit(Implicit::Moffs, osz);
        case 0xA2U:
            return implicit(Implicit::Moffs, 1U);
        case 0xA3U:
            return implicit(Implicit::Moffs, osz);
        case 0xA4U:
        case 0xA6U:
        case 0xAAU:
        case 0xACU:
        case 0xAEU:
            return implicit(Implicit::String, 1U);
        case 0xA5U:
        case 0xA7U:
        case 0xABU:
        case 0xADU:
        case 0xAFU:
            return implicit(Implicit::String, osz);
        case 0xA9U:
            return no_access(d.iz());
        case 0xC0U:
            return operand(kEither, 1U, 1U);
        case 0xC1U:
            return operand(kEither, osz, 1U);
        case 0xC6U:
            if (d.reg == 0U)
            {
                return operand(kWrite, 1U, 1U);
            }
            return d.modrm == 0xF8U ? no_access(1U) : refused(kUnlisted); // XABORT
        case 0xC7U:
            return d.reg == 0U ? operand(kWrite, osz, d.iz()) : refused(kUnlisted); // XBEGIN is not decoded
        case 0xC9U:
            return implicit(Implicit::Leave, d.stack_size());
        case 0xD0U:
        case 0xD2U:
            return operand(kEither, 1U);
        case 0xD1U:
        case 0xD3U:
            return operand(kEither, osz);
        case 0xD7U:
            return implicit(Implicit::Xlat, 1U);
        case 0xE9U:
            return d.opsize ? refused(kVendor) : no_access(4U);
        case 0xC2U:
        case 0xC3U:
        case 0xE8U:
            return refused(kShadowStack);
        case 0xF6U:
        case 0xF7U:
        {
            const crd::u32 size = op == 0xF6U ? 1U : osz;
            crd::u32 imm = 0U; // TEST's immediate: 8 bits, or 16 or 32 (never 64)
            if (d.reg < 2U)
            {
                imm = op == 0xF6U ? 1U : d.iz();
            }
            if (d.reg >= 6U)
            {
                return implicit(Implicit::Divide, size);
            }
            return operand(d.reg == 2U || d.reg == 3U ? kEither : kRead, size, imm); // NOT and NEG write
        }
        case 0xFEU:
            return d.reg < 2U ? operand(kEither, 1U) : refused(kUnlisted);
        case 0xFFU:
            switch (d.reg)
            {
                case 0U:
                case 1U:
                    return operand(kEither, osz);
                case 2U:
                    return refused(kShadowStack);
                case 4U:
                    return d.opsize ? refused(kVendor) : operand(kRead, 8U); // JMP r/m64
                case 6U:
                    return implicit(d.memory ? Implicit::PushRm : Implicit::Push, d.stack_size());
                default:
                    return refused(kSystem); // far CALL and JMP
            }
        case 0x6CU:
        case 0x6DU:
        case 0x6EU:
        case 0x6FU:
        case 0xC8U:
        case 0xCAU:
        case 0xCBU:
        case 0xCCU:
        case 0xCDU:
        case 0xCFU:
        case 0xE4U:
        case 0xE5U:
        case 0xE6U:
        case 0xE7U:
        case 0xECU:
        case 0xEDU:
        case 0xEEU:
        case 0xEFU:
        case 0xF1U:
        case 0xF4U:
        case 0xFAU:
        case 0xFBU:
            return refused(kSystem);
        case 0x8EU:
            return refused("a segment register load");
        default:
            return refused(kUnlisted);
    }
}

[[nodiscard]] Form vector(const X86& d, crd::u32 imm = 0U) noexcept
{
    return operand(kEither, d.vector_bytes, imm);
}

// 0F AE: FXSAVE, FXRSTOR, LDMXCSR, STMXCSR, the cache-line flushes and the fences.
[[nodiscard]] Form group_ae(const X86& d) noexcept
{
    if (!d.memory)
    {
        if (!d.rep && !d.repne && !d.opsize && d.reg >= 5U)
        {
            return no_access(); // LFENCE, MFENCE, SFENCE
        }
        return d.rep && !d.opsize && d.reg <= 3U ? no_access() : refused(kSystem); // RDFSBASE to WRGSBASE
    }
    if (d.rep || d.repne)
    {
        return refused(kUnlisted);
    }
    if (d.opsize)
    {
        return d.reg >= 6U ? implicit(Implicit::CacheLine, 64U) : refused(kUnlisted); // CLWB, CLFLUSHOPT
    }
    switch (d.reg)
    {
        case 0U:
            return operand(kWrite, 512U); // FXSAVE
        case 1U:
            return operand(kRead, 512U); // FXRSTOR
        case 2U:
            return operand(kRead, 4U); // LDMXCSR
        case 3U:
            return operand(kWrite, 4U); // STMXCSR
        case 7U:
            return implicit(Implicit::CacheLine, 64U); // CLFLUSH
        default:
            return refused("an XSAVE area, whose size the capture does not hold");
    }
}

[[nodiscard]] Form two_byte(X86& d, crd::u8 op) noexcept
{
    const crd::u32 osz = d.osz();
    if ((op >= 0x10U && op <= 0x17U) || (op >= 0x28U && op <= 0x2FU) || (op >= 0x50U && op <= 0x6FU) ||
        (op >= 0x74U && op <= 0x76U) || (op >= 0x7CU && op <= 0x7FU) || (op >= 0xD0U && op <= 0xFEU && op != 0xF7U))
    {
        return vector(d); // SSE and MMX: at most 16 bytes at the r/m address
    }
    if ((op >= 0x70U && op <= 0x73U) || op == 0xC2U || (op >= 0xC4U && op <= 0xC6U))
    {
        return vector(d, 1U);
    }
    if (op >= 0x40U && op <= 0x4FU)
    {
        return operand(kRead, osz); // CMOVcc reads its source whatever the condition
    }
    if (op >= 0x80U && op <= 0x8FU)
    {
        return d.opsize ? refused(kVendor) : no_access(4U);
    }
    if (op >= 0x90U && op <= 0x9FU)
    {
        return operand(kWrite, 1U); // SETcc
    }
    if (op >= 0xC8U && op <= 0xCFU)
    {
        return no_access(); // BSWAP
    }
    switch (static_cast<crd::u32>(op))
    {
        case 0x01U:
        {
            // XGETBV, XEND, XTEST, RDPKRU, WRPKRU, RDTSCP; every other 0F 01 form is a system instruction.
            const crd::u8 m = d.modrm;
            return m == 0xD0U || m == 0xD5U || m == 0xD6U || m == 0xEEU || m == 0xEFU || m == 0xF9U ? no_access()
                                                                                                    : refused(kSystem);
        }
        case 0x0BU:
        case 0x0EU:
        case 0x31U:
        case 0x77U:
        case 0xA2U:
        case 0xB9U:
        case 0xFFU:
        case 0x0DU:
        case 0x18U:
        case 0x1FU:
            return no_access(); // UD2, FEMMS, RDTSC, EMMS, CPUID, UD1, UD0, prefetches (never faulting), hint NOPs
        case 0x1EU:
            return d.rep && d.mod == 3U && d.reg == 7U && (d.rm == 2U || d.rm == 3U) ? no_access()
                                                                                     : refused(kSystem); // ENDBR
        case 0xA3U:
            return bit_offset(kRead);
        case 0xABU:
        case 0xB3U:
        case 0xBBU:
            return bit_offset(kEither);
        case 0xA4U:
        case 0xACU:
            return operand(kEither, osz, 1U); // SHLD, SHRD
        case 0xA5U:
        case 0xADU:
            return operand(kEither, osz);
        case 0xAEU:
            return group_ae(d);
        case 0xAFU:
        case 0xBCU:
        case 0xBDU:
            return operand(kRead, osz); // IMUL, BSF/TZCNT, BSR/LZCNT
        case 0xB0U:
        case 0xC0U:
            return operand(kEither, 1U); // CMPXCHG, XADD
        case 0xB1U:
        case 0xC1U:
            return operand(kEither, osz);
        case 0xB6U:
        case 0xBEU:
            return operand(kRead, 1U); // MOVZX, MOVSX
        case 0xB7U:
        case 0xBFU:
            return operand(kRead, 2U);
        case 0xB8U:
            return d.rep ? operand(kRead, osz) : refused(kSystem); // POPCNT; JMPE otherwise
        case 0xBAU:
            if (d.reg == 4U)
            {
                return operand(kRead, osz, 1U); // BT with an immediate offset stays inside the operand
            }
            return d.reg > 4U ? operand(kEither, osz, 1U) : refused(kUnlisted);
        case 0xC3U:
            return operand(kWrite, d.w() ? 8U : 4U); // MOVNTI
        case 0xC7U:
            if (d.memory)
            {
                return d.reg == 1U ? operand(kEither, d.w() ? 16U : 8U)
                                   : refused("an XSAVE area, whose size the capture does not hold, or VMX");
            }
            return (d.reg == 6U || d.reg == 7U) && !d.repne ? no_access() : refused(kSystem); // RDRAND, RDSEED, RDPID
        case 0xF7U:
            return implicit(Implicit::MaskMove, d.opsize ? 16U : 8U);
        default:
            return refused(op == 0x0FU ? cont::StringView{"an AMD 3DNow! instruction"} : kUnlisted);
    }
}

[[nodiscard]] Form three_byte_38(X86& d, crd::u8 op) noexcept
{
    if (op <= 0x0BU || op == 0x10U || op == 0x14U || op == 0x15U || op == 0x17U || (op >= 0x1CU && op <= 0x1EU) ||
        (op >= 0x20U && op <= 0x25U) || (op >= 0x28U && op <= 0x2BU) || (op >= 0x30U && op <= 0x35U) ||
        (op >= 0x37U && op <= 0x41U) || (op >= 0xC8U && op <= 0xCDU) || op == 0xCFU || (op >= 0xDBU && op <= 0xDFU))
    {
        return vector(d); // SSSE3, SSE4, SHA, GFNI, AES
    }
    switch (static_cast<crd::u32>(op))
    {
        case 0xF0U:
            return d.repne ? operand(kRead, 1U) : operand(kRead, d.osz()); // CRC32 r, r/m8; MOVBE load
        case 0xF1U:
            return d.repne ? operand(kRead, d.osz()) : operand(kWrite, d.osz()); // CRC32 r, r/m; MOVBE store
        case 0xF6U:
            return d.opsize != d.rep ? operand(kRead, d.w() ? 8U : 4U) : refused(kSystem); // ADCX, ADOX; WRSS
        case 0xF9U:
            return !d.opsize && !d.rep && !d.repne ? operand(kWrite, d.w() ? 8U : 4U) : refused(kUnlisted); // MOVDIRI
        default:
            return refused(kUnlisted);
    }
}

[[nodiscard]] Form three_byte_3a(const X86& d, crd::u8 op) noexcept
{
    if ((op >= 0x08U && op <= 0x0FU) || (op >= 0x14U && op <= 0x17U) || (op >= 0x20U && op <= 0x22U) ||
        (op >= 0x40U && op <= 0x42U) || op == 0x44U || (op >= 0x60U && op <= 0x63U) || op == 0xCCU || op == 0xCEU ||
        op == 0xCFU || op == 0xDFU)
    {
        return vector(d, 1U);
    }
    return refused(kUnlisted);
}

// VEX and EVEX: every operand is bounded by the vector length, but for the refused forms.
[[nodiscard]] Form extended(const X86& d, crd::u32 map, crd::u8 op) noexcept
{
    if (map == 2U && ((op >= 0x90U && op <= 0x93U) || (op >= 0xA0U && op <= 0xA3U) || op == 0xC6U || op == 0xC7U))
    {
        return refused("a gather or scatter (VSIB)");
    }
    if (map == 2U && !d.evex &&
        ((op >= 0x48U && op <= 0x4FU) || (op >= 0x5CU && op <= 0x5FU) || (op >= 0x6BU && op <= 0x6FU)))
    {
        return refused("an AMX tile configuration or tile load");
    }
    if (map == 1U && op == 0xF7U && !d.evex)
    {
        return implicit(Implicit::MaskMove, 16U); // VMASKMOVDQU
    }
    const bool imm =
        map == 3U || (map == 1U && ((op >= 0x70U && op <= 0x73U) || op == 0xC2U || (op >= 0xC4U && op <= 0xC6U)));
    return vector(d, imm ? 1U : 0U);
}

// ---- implicit accesses ----------------------------------------------------------------------------------------------

// A string instruction's operand at `pointer`, `size` bytes an element, `count` elements (DF orders them downward).
void string_operand(InstructionAccess& out, const X86& d, crd::u64 pointer, crd::u64 size, crd::u64 count,
                    crd::u8 kinds) noexcept
{
    if (count == 0U)
    {
        return; // REP with rcx zero touches nothing
    }
    const crd::u64 span = count > ~crd::u64{0} / size ? ~crd::u64{0} : count * size;
    const bool down = (d.regs.flags & kDirectionFlag) != 0U;
    add_access(out, down ? pointer - (span - size) : pointer, span, kinds);
}

// Adds the accesses of `f` (decoded) to `out`; false, with `why`, when one cannot be placed.
bool place(X86& d, const Form& f, crd::u8 op, InstructionAccess& out, cont::StringView& why) noexcept
{
    const bool uses_rm = d.memory && (f.kinds != 0U || f.implicit == Implicit::PushRm ||
                                      f.implicit == Implicit::PopRm || f.implicit == Implicit::BitOffset ||
                                      f.implicit == Implicit::CacheLine || f.implicit == Implicit::Divide);
    const bool uses_segment = uses_rm || f.implicit == Implicit::Xlat || f.implicit == Implicit::Moffs ||
                              f.implicit == Implicit::MaskMove ||
                              (f.implicit == Implicit::String && op != 0xAAU && op != 0xABU && op != 0xAEU &&
                               op != 0xAFU); // STOS and SCAS address ES only
    if (d.segment && uses_segment)
    {
        why = "an FS or GS segment base, which the capture does not hold";
        return false;
    }
    const crd::u64 rsp = d.gpr(kRsp);
    const crd::u64 n = f.implicit_size;
    switch (f.implicit)
    {
        case Implicit::None:
            break;
        case Implicit::Push:
            add_access(out, rsp - n, n, kWrite);
            break;
        case Implicit::Pop:
            add_access(out, rsp, n, kRead);
            break;
        case Implicit::PushRm:
            add_access(out, d.effective(d.disp, 0U), n, kRead);
            add_access(out, rsp - n, n, kWrite);
            return true;
        case Implicit::PopRm:
            add_access(out, rsp, n, kRead);
            add_access(out, d.effective(d.disp, n), n, kWrite);
            return true;
        case Implicit::Leave:
            add_access(out, d.gpr(kRbp), n, kRead);
            break;
        case Implicit::String:
        {
            if (d.addrsize)
            {
                why = "a string instruction with a 32-bit address size";
                return false;
            }
            const bool compare = op == 0xA6U || op == 0xA7U || op == 0xAEU || op == 0xAFU;
            if (d.repne && !compare)
            {
                why = "REPNE on a string instruction other than CMPS and SCAS";
                return false;
            }
            const crd::u64 count = d.rep || d.repne ? d.gpr(kRcx) : 1U;
            const crd::u32 kind = (op & 0xFEU);
            if (kind == 0xA4U || kind == 0xA6U || kind == 0xACU) // MOVS, CMPS, LODS read [rsi]
            {
                string_operand(out, d, d.gpr(kRsi), n, count, kRead);
            }
            if (kind == 0xA4U || kind == 0xAAU) // MOVS, STOS write [rdi]
            {
                string_operand(out, d, d.gpr(kRdi), n, count, kWrite);
            }
            else if (kind == 0xA6U || kind == 0xAEU) // CMPS, SCAS read [rdi]
            {
                string_operand(out, d, d.gpr(kRdi), n, count, kRead);
            }
            break;
        }
        case Implicit::Xlat:
            add_access(out, d.address(d.gpr(kRbx) + (d.gpr(kRax) & 0xFFU)), 1U, kRead);
            break;
        case Implicit::Moffs:
        {
            const crd::u32 width = d.addrsize ? 4U : 8U;
            crd::u64 at = 0U;
            for (crd::u32 i = 0U; i < width; ++i)
            {
                at |= crd::u64{d.code[d.at - width + i]} << (8U * i);
            }
            add_access(out, at, n, op <= 0xA1U ? kRead : kWrite);
            break;
        }
        case Implicit::MaskMove:
            if (d.memory)
            {
                why = "a masked move with a memory operand, which is invalid";
                return false;
            }
            add_access(out, d.address(d.gpr(kRdi)), n, kWrite);
            break;
        case Implicit::BitOffset:
        {
            if (!d.memory)
            {
                break;
            }
            if (d.addrsize)
            {
                why = "a bit offset with a 32-bit address size";
                return false;
            }
            // The bit's byte lies the offset's signed byte count from the address; the processor reads an operand-sized
            // unit holding it.
            const crd::u32 bits = d.osz() * 8U;
            crd::u64 offset = d.gpr(d.reg | ((d.rex & 4U) != 0U ? 8U : 0U));
            if (bits < 64U)
            {
                const crd::u64 sign = crd::u64{1} << (bits - 1U);
                offset &= (crd::u64{1} << bits) - 1U;
                offset = (offset ^ sign) - sign;
            }
            const crd::u64 byte = d.effective(d.disp, 0U) + static_cast<crd::u64>(static_cast<crd::i64>(offset) >> 3);
            add_access(out, byte - (d.osz() - 1U), 2U * d.osz() - 1U, f.kinds);
            return true;
        }
        case Implicit::CacheLine:
            add_access(out, d.effective(d.disp, 0U) & ~crd::u64{63}, 64U, kEither);
            return true;
        case Implicit::Divide:
        {
            out.divides = true;
            if (d.memory)
            {
                add_access(out, d.effective(d.disp, 0U), n, kRead);
                return true;
            }
            const crd::u64 rax = d.gpr(kRax);
            const crd::u64 rdx = d.gpr(kRdx);
            crd::u64 divisor = 0U;
            if (n == 1U && d.rex == 0U && d.rm >= 4U)
            {
                divisor = (d.gpr(d.rm - 4U) >> 8U) & 0xFFU; // AH, CH, DH, BH
            }
            else
            {
                divisor = d.gpr(d.rm | ((d.rex & 1U) != 0U ? 8U : 0U));
            }
            const crd::u32 bits = static_cast<crd::u32>(n * 8U);
            out.divisor_known = true;
            out.divide_faults = x86_divide_faults(d.reg == 7U, bits, bits == 8U ? rax >> 8U : rdx, rax, divisor);
            return true;
        }
    }
    if (d.memory && f.kinds != 0U)
    {
        if (d.evex && d.disp8)
        {
            // The 8-bit displacement is scaled by the tuple's factor, a power of two up to the vector length.
            const crd::i64 disp8 = static_cast<crd::i64>(d.disp);
            for (crd::u64 factor = 1U; factor <= d.vector_bytes; factor <<= 1U)
            {
                add_access(out, d.effective(static_cast<crd::u64>(disp8 * static_cast<crd::i64>(factor)), 0U), f.size,
                           f.kinds);
            }
        }
        else
        {
            add_access(out, d.effective(d.disp, 0U), f.size, f.kinds);
        }
    }
    return true;
}

[[nodiscard]] InstructionAccess decode_x86(cont::ConstSpan<crd::u8> code, const FaultRegisters& regs) noexcept
{
    InstructionAccess out;
    X86 d{code, regs};
    // Legacy prefixes, then REX (a legacy prefix after a REX voids it).
    crd::u8 op = 0U;
    while (true)
    {
        op = d.byte();
        if (d.cut || d.too_long)
        {
            break;
        }
        if (op == 0x66U)
        {
            d.opsize = true;
        }
        else if (op == 0x67U)
        {
            d.addrsize = true;
        }
        else if (op == 0xF3U)
        {
            d.rep = true;
        }
        else if (op == 0xF2U)
        {
            d.repne = true;
        }
        else if (op == 0xF0U)
        {
            d.lock = true;
        }
        else if (op == 0x64U || op == 0x65U)
        {
            d.segment = true;
        }
        else if (op != 0x26U && op != 0x2EU && op != 0x36U && op != 0x3EU) // ES, CS, SS, DS: no base in 64-bit mode
        {
            if (op < 0x40U || op > 0x4FU)
            {
                break;
            }
            d.rex = op;
            continue;
        }
        d.rex = 0U;
    }
    Form f;
    crd::u32 map = 0U;
    if (op == 0xC4U || op == 0xC5U || op == 0x62U)
    {
        if (d.opsize || d.rep || d.repne || d.lock || d.rex != 0U)
        {
            out.reason = "a VEX or EVEX prefix after a 66, F2, F3, LOCK or REX prefix, which is invalid";
            return out;
        }
        crd::u32 pp = 0U;
        if (op == 0xC5U)
        {
            const crd::u8 b1 = d.byte();
            d.rex = static_cast<crd::u8>((b1 & 0x80U) != 0U ? 0U : 4U);
            d.vector_bytes = (b1 & 4U) != 0U ? 32U : 16U;
            pp = b1 & 3U;
            map = 1U;
        }
        else if (op == 0xC4U)
        {
            const crd::u8 b1 = d.byte();
            const crd::u8 b2 = d.byte();
            d.rex = static_cast<crd::u8>((~b1 >> 5U) & 7U) | static_cast<crd::u8>((b2 & 0x80U) != 0U ? 8U : 0U);
            d.vector_bytes = (b2 & 4U) != 0U ? 32U : 16U;
            pp = b2 & 3U;
            map = b1 & 0x1FU;
        }
        else
        {
            const crd::u8 p0 = d.byte();
            const crd::u8 p1 = d.byte();
            const crd::u8 p2 = d.byte();
            if (d.cut || d.too_long)
            {
                out.reason = cut_reason(d);
                return out;
            }
            if ((p0 & 8U) != 0U || (p1 & 4U) == 0U)
            {
                out.reason = "APX's extended registers";
                return out;
            }
            d.evex = true;
            d.rex = static_cast<crd::u8>((~p0 >> 5U) & 7U) | static_cast<crd::u8>((p1 & 0x80U) != 0U ? 8U : 0U);
            const crd::u32 length = (p2 >> 5U) & 3U;
            d.vector_bytes = 16U << length;
            pp = p1 & 3U;
            map = p0 & 7U;
            if (length == 3U)
            {
                d.vector_bytes = 64U; // reserved for memory forms; rounding control for register forms
            }
        }
        d.opsize = pp == 1U;
        d.rep = pp == 2U;
        d.repne = pp == 3U;
        if (map != 1U && map != 2U && map != 3U && !(d.evex && (map == 5U || map == 6U)))
        {
            out.reason = "a VEX or EVEX map this decoder does not list";
            return out;
        }
        op = d.byte();
        if (map == 1U && op == 0x77U && !d.evex)
        {
            f = no_access(); // VZEROUPPER, VZEROALL
        }
        else
        {
            d.read_modrm();
            f = extended(d, map, op);
        }
        map += 0x10U; // extended maps
    }
    else if (op == 0x0FU)
    {
        op = d.byte();
        map = 1U;
        if (op == 0x38U || op == 0x3AU)
        {
            map = op == 0x38U ? 2U : 3U;
            op = d.byte();
            d.read_modrm();
            f = map == 2U ? three_byte_38(d, op) : three_byte_3a(d, op);
        }
        else
        {
            if (two_byte_has_modrm(op))
            {
                d.read_modrm();
            }
            f = two_byte(d, op);
        }
    }
    else
    {
        if (one_byte_has_modrm(op))
        {
            d.read_modrm();
        }
        f = one_byte(d, op);
    }
    if (d.cut || d.too_long)
    {
        out.reason = cut_reason(d);
        return out;
    }
    if (!f.refuse.empty())
    {
        out.reason = f.refuse;
        return out;
    }
    for (crd::u32 i = 0U; i < f.imm; ++i)
    {
        (void)d.byte();
    }
    if (f.implicit == Implicit::Moffs)
    {
        (void)d.bytes(d.addrsize ? 4U : 8U); // the absolute address is the last field; place() reads it back
    }
    if (d.cut || d.too_long)
    {
        out.reason = cut_reason(d);
        return out;
    }
    cont::StringView why;
    if (!place(d, f, map == 0U ? op : 0U, out, why))
    {
        out = InstructionAccess{};
        out.reason = why;
        return out;
    }
    out.length = d.at;
    out.decoded = true;
    return out;
}

struct U128
{
    crd::u64 hi;
    crd::u64 lo;
};

[[nodiscard]] bool less(U128 a, U128 b) noexcept
{
    return a.hi < b.hi || (a.hi == b.hi && a.lo < b.lo);
}

} // namespace

cont::StringView instruction_set_name(InstructionSet s) noexcept
{
    switch (s)
    {
        case InstructionSet::Unknown:
            return "unknown";
        case InstructionSet::X64:
            return "x86-64";
        case InstructionSet::AArch64:
            return "aarch64";
    }
    return "unknown";
}

bool x86_divide_faults(bool is_signed, crd::u32 bits, crd::u64 high, crd::u64 low, crd::u64 divisor) noexcept
{
    const crd::u64 mask = bits >= 64U ? ~crd::u64{0} : (crd::u64{1} << bits) - 1U;
    high &= mask;
    low &= mask;
    divisor &= mask;
    if (divisor == 0U)
    {
        return true;
    }
    if (!is_signed)
    {
        return high >= divisor; // the quotient fits exactly when the dividend's high half is below the divisor
    }
    // IDIV truncates toward zero, so |quotient| = floor(|dividend| / |divisor|), which fits when it is below 2^(n-1)
    // for a positive quotient and at most 2^(n-1) for a negative one: |dividend| < (2^(n-1) + negative) * |divisor|.
    const crd::u64 sign = crd::u64{1} << (bits - 1U);
    const bool dividend_negative = (high & sign) != 0U;
    const bool divisor_negative = (divisor & sign) != 0U;
    U128 dividend = bits == 64U ? U128{high, low} : U128{0U, (high << bits) | low};
    if (dividend_negative)
    {
        dividend.hi = ~dividend.hi;
        dividend.lo = ~dividend.lo + 1U;
        dividend.hi += dividend.lo == 0U ? 1U : 0U;
        if (bits < 64U)
        {
            const crd::u64 wide = 2U * bits == 64U ? ~crd::u64{0} : (crd::u64{1} << (2U * bits)) - 1U;
            dividend = U128{0U, dividend.lo & wide};
        }
    }
    const crd::u64 magnitude = divisor_negative ? (~divisor + 1U) & mask : divisor;
    const crd::u32 shift = bits - 1U;
    U128 limit{shift == 0U ? 0U : magnitude >> (64U - shift), magnitude << shift};
    if (dividend_negative != divisor_negative)
    {
        limit.lo += magnitude;
        limit.hi += limit.lo < magnitude ? 1U : 0U;
    }
    return !less(dividend, limit);
}

InstructionAccess decode_instruction_access(InstructionSet set, cont::ConstSpan<crd::u8> code,
                                            const FaultRegisters& regs) noexcept
{
    if (!regs.known)
    {
        InstructionAccess out;
        out.reason = "the capture holds no registers";
        return out;
    }
    if (set == InstructionSet::X64)
    {
        return decode_x86(code, regs);
    }
    if (set == InstructionSet::AArch64)
    {
        return detail::decode_a64(code, regs);
    }
    InstructionAccess out;
    out.reason = "the capture names no instruction set";
    return out;
}

} // namespace crd::perf
