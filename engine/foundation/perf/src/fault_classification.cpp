// crd-perf -- hardware fault classification (HW-RESILIENCE.2). See fault_classification.hpp.

#include "payload_codec.hpp"

#include <crd/perf/fault_classification.hpp>

#include <cstring>

namespace crd::perf
{
namespace
{
namespace cont = crd::containers;

// Windows minidump: the exception stream and the access-fault codes whose parameters name the data address.
constexpr crd::u32 kExceptionStream = 6U;
constexpr crd::u32 kStatusAccessViolation = 0xC0000005U;
constexpr crd::u32 kStatusInPageError = 0xC0000006U;
constexpr crd::u32 kExceptionParametersRead = 2U; // the access kind, then the address
constexpr crd::u32 kStatusIntegerDivideByZero = 0xC0000094U;
constexpr crd::u32 kStatusIntegerOverflow = 0xC0000095U; // how x86-64 Windows reports a divide error's overflow
// The exception stream's thread context (a MINIDUMP_LOCATION_DESCRIPTOR after the 152-byte MINIDUMP_EXCEPTION), the
// system information's processor architecture and the CONTEXT layouts it names (AMD64 and ARM64, winnt.h; the Windows
// tests pin each offset against the SDK's own structure).
constexpr crd::usize kExceptionContextOffset = 160U;
constexpr crd::u32 kSystemInfoStream = 7U;
constexpr crd::u16 kArchitectureAmd64 = 9U;
constexpr crd::u16 kArchitectureArm64 = 12U;
constexpr crd::u32 kAmd64Flags = 0x30U;  // ContextFlags
constexpr crd::u32 kAmd64EFlags = 0x44U; // EFlags (32 bits)
constexpr crd::u32 kAmd64Rax = 0x78U;    // Rax to R15 in the encoding's order
constexpr crd::u32 kAmd64Rip = 0xF8U;
constexpr crd::u32 kAmd64Needed = 0x100U;
constexpr crd::u32 kAmd64Integer = 0x00100003U; // CONTEXT_AMD64 | CONTEXT_CONTROL | CONTEXT_INTEGER
constexpr crd::u32 kArm64X0 = 0x08U;            // X0 to X28, Fp (X29) and Lr (X30)
constexpr crd::u32 kArm64Sp = 0x100U;
constexpr crd::u32 kArm64Pc = 0x108U;
constexpr crd::u32 kArm64Needed = 0x110U;
constexpr crd::u32 kArm64Integer = 0x00400003U; // CONTEXT_ARM64 | CONTEXT_CONTROL | CONTEXT_INTEGER
// Its memory lists: MINIDUMP_MEMORY_LIST (a count, then {start, size, rva} per range) and MINIDUMP_MEMORY64_LIST (a
// count and the base rva, then {start, size} per range, their bytes back to back from the base).
constexpr crd::u32 kMemoryListStream = 5U;
constexpr crd::u32 kMemory64ListStream = 9U;

// Linux signal numbers (x86-64 and AArch64 agree) and SIGBUS's machine-check codes.
constexpr crd::u32 kSigBus = 7U;
constexpr crd::u32 kSigFpe = 8U;
constexpr crd::u32 kSigSegv = 11U;
constexpr crd::i32 kSegvMapErr = 1; // SEGV_MAPERR, SEGV_ACCERR and SEGV_PKUERR: a page fault on an address
constexpr crd::i32 kSegvAccErr = 2;
constexpr crd::i32 kSegvPkuErr = 4;
constexpr crd::i32 kBusAdrAln = 1; // BUS_ADRALN: an AArch64 alignment fault on an address
constexpr crd::i32 kBusAdrErr = 2; // BUS_ADRERR: a page fault past a mapped file's end
constexpr crd::i32 kFpeIntDiv = 1; // FPE_INTDIV and FPE_INTOVF
constexpr crd::i32 kFpeIntOvf = 2;
constexpr crd::u32 kTrapPageFault = 14U; // x86-64 exception vectors
constexpr crd::u32 kTrapDivideError = 0U;
constexpr crd::u64 kPageFaultWrite = 1U << 1U; // the page fault error code's W and I/D bits
constexpr crd::u64 kPageFaultFetch = 1U << 4U;
constexpr crd::i32 kBusMceErrAr = 4; // BUS_MCEERR_AR: memory error consumed by the faulting access
constexpr crd::i32 kBusMceErrAo = 5; // BUS_MCEERR_AO: memory error found in the process, not yet consumed

[[nodiscard]] cont::StringView view(const cont::String& s) noexcept
{
    return cont::StringView{s.data(), s.size()};
}

void append_hex(cont::String& out, crd::u64 v)
{
    constexpr char hex_digits[] = "0123456789abcdef";
    char digits[16];
    crd::u32 n = 0U;
    do
    {
        digits[n++] = hex_digits[v & 0xFU];
        v >>= 4U;
    } while (v != 0U);
    out.append("0x");
    while (n != 0U)
    {
        out.push_back(digits[--n]);
    }
}

// ---- reading a capture ----------------------------------------------------------------------------------------------

CrashEvidence refuse(CrashEvidence& e, cont::StringView why)
{
    e.ok = false;
    e.error = why;
    return static_cast<CrashEvidence&&>(e);
}

// The bytes at the pc (up to kCodeWindowBytes) from a dump's memory range [start, start + size) stored at `rva`.
void copy_code(cont::ConstSpan<crd::u8> dump, crd::u64 start, crd::u64 size, crd::u64 rva, CrashEvidence& e) noexcept
{
    if (e.code_size != 0U || e.pc < start || e.pc - start >= size || rva > dump.size() || size > dump.size() - rva)
    {
        return;
    }
    const crd::u64 skip = e.pc - start;
    const crd::u64 left = size - skip;
    e.code_size = static_cast<crd::u32>(left < crd::crash::kCodeWindowBytes ? left : crd::crash::kCodeWindowBytes);
    std::memcpy(e.code_bytes, dump.data() + rva + skip, e.code_size);
}

void read_minidump_code(cont::ConstSpan<crd::u8> dump, CrashEvidence& e) noexcept
{
    detail::Cursor list{find_minidump_stream(dump, kMemoryListStream)};
    const crd::u32 count = list.u32();
    for (crd::u32 i = 0U; list.ok && i < count; ++i)
    {
        const crd::u64 start = list.u64();
        const crd::u32 size = list.u32();
        const crd::u32 rva = list.u32();
        if (list.ok)
        {
            copy_code(dump, start, size, rva, e);
        }
    }
    detail::Cursor list64{find_minidump_stream(dump, kMemory64ListStream)};
    const crd::u64 count64 = list64.u64();
    crd::u64 rva = list64.u64();
    for (crd::u64 i = 0U; list64.ok && i < count64 && rva <= dump.size(); ++i)
    {
        const crd::u64 start = list64.u64();
        const crd::u64 size = list64.u64();
        if (!list64.ok || size > dump.size() - rva)
        {
            break;
        }
        copy_code(dump, start, size, rva, e);
        rva += size;
    }
}

// An access violation's first parameter: read (0), write (1) or execute (8, a DEP fault).
[[nodiscard]] FaultAccess fault_access_of(crd::u64 kind) noexcept
{
    switch (kind)
    {
        case 0U:
            return FaultAccess::Read;
        case 1U:
            return FaultAccess::Write;
        case 8U:
            return FaultAccess::Execute;
        default:
            return FaultAccess::Unknown;
    }
}

[[nodiscard]] crd::u64 load(cont::ConstSpan<crd::u8> bytes, crd::usize at, crd::u32 width) noexcept
{
    crd::u64 v = 0U;
    for (crd::u32 i = 0U; i < width; ++i)
    {
        v |= crd::u64{bytes[at + i]} << (8U * i);
    }
    return v;
}

// The processor architecture and the faulting thread's registers, from the exception stream's thread context.
void read_minidump_context(cont::ConstSpan<crd::u8> dump, cont::ConstSpan<crd::u8> exception, CrashEvidence& e) noexcept
{
    detail::Cursor system{find_minidump_stream(dump, kSystemInfoStream)};
    const crd::u16 architecture = system.u16();
    if (!system.ok)
    {
        return;
    }
    if (architecture == kArchitectureAmd64)
    {
        e.instruction_set = InstructionSet::X64;
    }
    else if (architecture == kArchitectureArm64)
    {
        e.instruction_set = InstructionSet::AArch64;
    }
    if (exception.size() < kExceptionContextOffset + 8U)
    {
        return;
    }
    const crd::u64 size = load(exception, kExceptionContextOffset, 4U);
    const crd::u64 rva = load(exception, kExceptionContextOffset + 4U, 4U);
    if (rva > dump.size() || size > dump.size() - rva)
    {
        return;
    }
    const cont::ConstSpan<crd::u8> context = dump.subspan(static_cast<crd::usize>(rva), static_cast<crd::usize>(size));
    FaultRegisters& r = e.registers;
    if (e.instruction_set == InstructionSet::X64 && context.size() >= kAmd64Needed &&
        (load(context, kAmd64Flags, 4U) & kAmd64Integer) == kAmd64Integer)
    {
        for (crd::u32 i = 0U; i < 16U; ++i)
        {
            r.gpr[i] = load(context, kAmd64Rax + 8U * i, 8U);
        }
        r.pc = load(context, kAmd64Rip, 8U);
        r.flags = load(context, kAmd64EFlags, 4U);
        r.known = e.pc_known && r.pc == e.pc;
    }
    else if (e.instruction_set == InstructionSet::AArch64 && context.size() >= kArm64Needed &&
             (load(context, 0U, 4U) & kArm64Integer) == kArm64Integer)
    {
        for (crd::u32 i = 0U; i < 31U; ++i)
        {
            r.gpr[i] = load(context, kArm64X0 + 8U * i, 8U);
        }
        r.gpr[31] = load(context, kArm64Sp, 8U);
        r.pc = load(context, kArm64Pc, 8U);
        r.flags = load(context, 4U, 4U);
        r.known = e.pc_known && r.pc == e.pc;
    }
}

CrashEvidence read_minidump(cont::ConstSpan<crd::u8> dump, crd::memory::IAllocator* alloc)
{
    CrashEvidence e(alloc);
    e.kind = CrashCaptureKind::Minidump;
    // MINIDUMP_EXCEPTION_STREAM: ThreadId, alignment, then MINIDUMP_EXCEPTION (code, flags, record, address, count,
    // alignment, ExceptionInformation[15]).
    detail::Cursor in{find_minidump_stream(dump, kExceptionStream)};
    (void)in.u32();
    (void)in.u32();
    e.code = in.u32();
    (void)in.u32();
    (void)in.u64();
    e.pc = in.u64();
    const crd::u32 count = in.u32();
    (void)in.u32();
    const crd::u64 kind = in.u64(); // ExceptionInformation[0]: read (0), write (1) or execute (8)
    const crd::u64 address = in.u64();
    if (!in.ok)
    {
        return refuse(e, "the minidump has no exception stream, or it is cut short");
    }
    e.pc_known = true;
    if ((e.code == kStatusAccessViolation || e.code == kStatusInPageError) && count >= kExceptionParametersRead)
    {
        e.address_known = true;
        e.address = address;
        e.access = fault_access_of(kind);
    }
    read_minidump_context(dump, find_minidump_stream(dump, kExceptionStream), e);
    e.modules = parse_minidump_modules(dump, alloc);
    for (ModuleIdentity& m : parse_minidump_unloaded_modules(dump, alloc))
    {
        e.modules.push_back(static_cast<ModuleIdentity&&>(m));
    }
    read_minidump_code(dump, e);
    e.ok = true;
    return static_cast<CrashEvidence&&>(e);
}

// A record's value after `key` (a token at a line start or after a space): a decimal (with an optional '-') or a
// 0x-prefixed lowercase hex number ending at a space or a newline. False when the key is absent or the value is not
// a number.
bool record_value(cont::StringView text, cont::StringView key, bool hex, crd::u64& value, bool& negative)
{
    crd::usize from = 0U;
    while (true)
    {
        const crd::usize at = text.find(key, from);
        if (at == cont::StringView::npos)
        {
            return false;
        }
        from = at + 1U;
        const crd::usize begin = at + key.size();
        if ((at != 0U && text[at - 1U] != ' ' && text[at - 1U] != '\n') || begin >= text.size() || text[begin] != ' ')
        {
            continue;
        }
        crd::usize i = begin + 1U;
        negative = !hex && i < text.size() && text[i] == '-';
        if (negative)
        {
            ++i;
        }
        if (hex)
        {
            if (text.size() - i < 3U || text[i] != '0' || text[i + 1U] != 'x')
            {
                return false;
            }
            i += 2U;
        }
        crd::u64 v = 0U;
        crd::usize digits = 0U;
        for (; i < text.size() && text[i] != ' ' && text[i] != '\n'; ++i, ++digits)
        {
            const char c = text[i];
            crd::u32 d = 0U;
            if (c >= '0' && c <= '9')
            {
                d = static_cast<crd::u32>(c - '0');
            }
            else if (hex && c >= 'a' && c <= 'f')
            {
                d = static_cast<crd::u32>(c - 'a' + 10);
            }
            else
            {
                return false;
            }
            const crd::u32 base = hex ? 16U : 10U;
            if (v > (~crd::u64{0} - d) / base)
            {
                return false;
            }
            v = v * base + d;
        }
        if (digits == 0U || i >= text.size())
        {
            return false; // empty, or not newline-terminated
        }
        value = v;
        return true;
    }
}

[[nodiscard]] int hex_digit(char c) noexcept
{
    if (c >= '0' && c <= '9')
    {
        return c - '0';
    }
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

// The record's code line, "code <pc> <hex bytes>", when it names the record's own pc.
void read_record_code(cont::StringView text, CrashEvidence& e) noexcept
{
    const crd::usize at = text.find("\ncode ");
    const crd::usize end = at == cont::StringView::npos ? at : text.find('\n', at + 1U);
    if (!e.pc_known || end == cont::StringView::npos)
    {
        return;
    }
    const cont::StringView line = text.substr(at + 1U, end - at); // newline-terminated, as record_value needs
    crd::u64 pc = 0U;
    bool negative = false;
    const crd::usize space = line.find(' ', 5U);
    if (!record_value(line, "code", true, pc, negative) || pc != e.pc || space == cont::StringView::npos)
    {
        return;
    }
    const cont::StringView hex = line.substr(space + 1U, line.size() - space - 2U);
    if (hex.empty() || hex.size() % 2U != 0U || hex.size() / 2U > crd::crash::kCodeWindowBytes)
    {
        return;
    }
    crd::u8 code[crd::crash::kCodeWindowBytes] = {};
    for (crd::usize i = 0U; i < hex.size(); i += 2U)
    {
        const int hi = hex_digit(hex[i]);
        const int lo = hex_digit(hex[i + 1U]);
        if (hi < 0 || lo < 0)
        {
            return;
        }
        code[i / 2U] = static_cast<crd::u8>((hi << 4) | lo);
    }
    e.code_size = static_cast<crd::u32>(hex.size() / 2U);
    std::memcpy(e.code_bytes, code, e.code_size);
}

// The record's registers (each a named hex value); x86-64's also carry the trap number and the page fault's error code.
void read_record_registers(cont::StringView block, CrashEvidence& e) noexcept
{
    constexpr cont::StringView x86[] = {"rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
                                        "r8",  "r9",  "r10", "r11", "r12", "r13", "r14", "r15"};
    constexpr cont::StringView arm[] = {"x0",  "x1",  "x2",  "x3",  "x4",  "x5",  "x6",  "x7",  "x8",  "x9",  "x10",
                                        "x11", "x12", "x13", "x14", "x15", "x16", "x17", "x18", "x19", "x20", "x21",
                                        "x22", "x23", "x24", "x25", "x26", "x27", "x28", "x29", "x30", "sp"};
    const bool is_x86 = e.instruction_set == InstructionSet::X64;
    const cont::StringView* names = is_x86 ? x86 : arm;
    const crd::u32 count = is_x86 ? 16U : 32U;
    FaultRegisters& r = e.registers;
    bool negative = false;
    bool all = e.pc_known;
    for (crd::u32 i = 0U; all && i < count; ++i)
    {
        all = record_value(block, names[i], true, r.gpr[i], negative);
    }
    r.pc = e.pc;
    if (is_x86)
    {
        crd::u64 trap = 0U;
        crd::u64 error = 0U;
        all = all && record_value(block, "efl", true, r.flags, negative);
        e.trap_known = record_value(block, "trapno", true, trap, negative) && trap <= 0xFFU;
        e.trap = static_cast<crd::u32>(trap);
        if (e.trap_known && e.trap == kTrapPageFault && record_value(block, "err", true, error, negative))
        {
            e.access = (error & kPageFaultWrite) != 0U ? FaultAccess::Write : FaultAccess::Read;
            e.access = (error & kPageFaultFetch) != 0U ? FaultAccess::Execute : e.access;
        }
    }
    r.known = all;
}

CrashEvidence read_record(cont::StringView text, cont::ConstSpan<crd::u8> bytes, crd::memory::IAllocator* alloc)
{
    CrashEvidence e(alloc);
    e.kind = CrashCaptureKind::Record;
    // The signal line: "signal <n> code <si_code> addr <hex>".
    const crd::usize line = text.find("\nsignal ");
    const crd::usize end = line == cont::StringView::npos ? line : text.find('\n', line + 1U);
    if (end == cont::StringView::npos)
    {
        return refuse(e, "the crash record has no complete signal line");
    }
    const cont::StringView signal_line = text.substr(line + 1U, end - line);
    crd::u64 signal = 0U;
    crd::u64 code = 0U;
    crd::u64 addr = 0U;
    bool negative = false;
    bool unused = false;
    if (!record_value(signal_line, "signal", false, signal, unused) || unused || signal > 64U ||
        !record_value(signal_line, "code", false, code, negative) || code > 0x7FFFFFFFU ||
        !record_value(signal_line, "addr", true, addr, unused))
    {
        return refuse(e, "the crash record's signal line is not 'signal <n> code <n> addr <hex>'");
    }
    e.code = static_cast<crd::u32>(signal);
    e.signal_code = negative ? -static_cast<crd::i32>(code) : static_cast<crd::i32>(code);
    e.sent = e.signal_code <= 0;
    if (!e.sent && (e.code == kSigSegv || e.code == kSigBus))
    {
        e.address_known = true;
        e.address = addr;
    }
    // The registers, between "\nregs " and the code line or the identities after them: x86-64's rip, AArch64's pc.
    const crd::usize regs = text.find("\nregs ");
    if (regs != cont::StringView::npos)
    {
        crd::usize stop = cont::StringView::npos;
        for (const cont::StringView next :
             {cont::StringView{"\ncode "}, cont::StringView{"\nbuild_id "}, cont::StringView{"\nmodules "}})
        {
            const crd::usize found = text.find(next, regs);
            stop = found < stop ? found : stop;
        }
        const cont::StringView block =
            text.substr(regs + 1U, stop == cont::StringView::npos ? text.size() - regs - 1U : stop - regs);
        if (block.starts_with("regs x86_64\n"))
        {
            e.instruction_set = InstructionSet::X64;
            e.pc_known = record_value(block, "rip", true, e.pc, unused);
            read_record_registers(block, e);
        }
        else if (block.starts_with("regs aarch64\n"))
        {
            e.instruction_set = InstructionSet::AArch64;
            e.pc_known = record_value(block, "pc", true, e.pc, unused);
            read_record_registers(block, e);
        }
    }
    e.modules = parse_crash_record_modules(bytes, alloc);
    read_record_code(text, e);
    e.ok = true;
    return static_cast<CrashEvidence&&>(e);
}

// ---- judging --------------------------------------------------------------------------------------------------------

[[nodiscard]] bool inside(const ModuleIdentity& m, crd::u64 address) noexcept
{
    return m.size != 0U && address >= m.base && address - m.base < m.size;
}

// One flipped bit of `address` (outside every module the capture lists, loaded or unloaded) that puts it inside a
// loaded module; the lowest such bit.
void find_bit_flip(const CrashEvidence& e, crd::u64 address, BitFlipEvidence& out)
{
    if (address < kNullRegionBytes)
    {
        return;
    }
    for (const ModuleIdentity& m : e.modules)
    {
        if (inside(m, address))
        {
            return; // a pointer into a module, or a dangling one into an unloaded module: no flip is needed
        }
    }
    for (crd::u32 bit = 0U; bit < 64U; ++bit)
    {
        const crd::u64 restored = address ^ (crd::u64{1} << bit);
        for (const ModuleIdentity& m : e.modules)
        {
            if (!m.unloaded && inside(m, restored))
            {
                out.found = true;
                out.recorded = address;
                out.restored = restored;
                out.bit = bit;
                out.module.clear();
                out.module.append(view(m.name));
                return;
            }
        }
    }
}

[[nodiscard]] bool is_digit(char c) noexcept
{
    return c >= '0' && c <= '9';
}

// The Raptor Lake Vmin Shift advisory covers 13th and 14th Gen desktop Core i5, i7 and i9 parts of 65 W or more:
// "Core(TM) i<5|7|9>-<13|14>NNN" with no suffix or F, K, KF or KS (so no HX, H, P, U or T part).
[[nodiscard]] bool raptor_lake_desktop(cont::StringView brand) noexcept
{
    const crd::usize at = brand.find("Core(TM) i");
    if (at == cont::StringView::npos)
    {
        return false;
    }
    cont::StringView sku = brand.substr(at + 10U);
    const crd::usize space = sku.find(' ');
    if (space != cont::StringView::npos)
    {
        sku = sku.substr(0U, space);
    }
    if (sku.size() < 7U || (sku[0] != '5' && sku[0] != '7' && sku[0] != '9') || sku[1] != '-' || sku[2] != '1' ||
        (sku[3] != '3' && sku[3] != '4'))
    {
        return false;
    }
    for (crd::usize i = 2U; i < 7U; ++i)
    {
        if (!is_digit(sku[i]))
        {
            return false;
        }
    }
    const cont::StringView suffix = sku.substr(7U);
    return suffix.empty() || suffix == cont::StringView{"F"} || suffix == cont::StringView{"K"} ||
           suffix == cont::StringView{"KF"} || suffix == cont::StringView{"KS"};
}

constexpr CpuAdvisory kAdvisories[] = {
    {"intel-raptor-lake-vmin-shift", "Vmin Shift Instability of Intel Core 13th and 14th Gen desktop processors",
     "GenuineIntel", 0x000B0671U, 0x12FU,
     "Intel, 'Intel Core 13th and 14th Gen Desktop Instability Root Cause Update' (2024-09-25: the cause, microcode "
     "0x125, 0x129 and 0x12B, and that microcode does not repair a processor already unstable; "
     "https://community.intel.com/t5/Mobile-and-Desktop-Processors/Intel-Core-13th-and-14th-Gen-Desktop-Instability-"
     "Root-Cause/m-p/1639535), and microcode 0x12F (2025-05) for the rare light-load case"},
};

void append_flip(cont::String& out, cont::StringView what, const BitFlipEvidence& f)
{
    out.append(what);
    out.append(" ");
    append_hex(out, f.recorded);
    out.append(" is one bit away from ");
    append_hex(out, f.restored);
    out.append(" in ");
    out.append(f.module.empty() ? cont::StringView{"a loaded module"} : view(f.module));
}

// What points to a hardware fault, in words: "the code address 0x.. is one bit away from 0x.. in app.exe; ...".
void append_evidence(cont::String& out, const FaultClassification& c)
{
    const crd::usize start = out.size();
    const auto next = [&]()
    {
        if (out.size() != start)
        {
            out.append("; ");
        }
    };
    if ((c.signals & kFaultSignalMachineCheck) != 0U)
    {
        next();
        out.append("the operating system reported a memory error");
    }
    if (c.code_flip.found)
    {
        next();
        append_flip(out, "the code address", c.code_flip);
    }
    if (c.data_flip.found)
    {
        next();
        append_flip(out, "the data address", c.data_flip);
    }
    if ((c.signals & kFaultSignalReplayNotReproduced) != 0U)
    {
        next();
        out.append("replaying it on the same build did not reproduce it");
    }
    if ((c.signals & (kFaultSignalAccessNotMade | kFaultSignalDivideWithoutCause)) != 0U)
    {
        next();
        out.append("the instruction at ");
        append_hex(out, c.instruction.pc);
        out.append((c.signals & kFaultSignalAccessNotMade) != 0U ? cont::StringView{" faulted on "}
                                                                 : cont::StringView{" raised a divide error, but "});
        out.append(c.instruction.reason);
    }
    if ((c.signals & kFaultSignalCodeBitFlip) != 0U)
    {
        next();
        out.append("the instruction byte at ");
        append_hex(out, c.code_image.pc + c.code_image.offset);
        out.append(" reads ");
        append_hex(out, c.code_image.recorded);
        out.append(" where ");
        out.append(c.code_image.module.empty() ? cont::StringView{"its module"} : view(c.code_image.module));
        out.append(" holds ");
        append_hex(out, c.code_image.image);
        out.append(", one bit apart");
    }
}

void append_processor(cont::String& out, const FaultClassification& c, const HardwareIdentity* machine, bool first)
{
    out.append(first ? "This processor" : "this processor");
    if (machine != nullptr && !machine->cpu_model.empty())
    {
        out.append(" (");
        out.append(view(machine->cpu_model));
        out.append(")");
    }
    out.append(" has a known instability, ");
    out.append(c.advisory->name);
}

// Whether the reported fault is one the instruction check judges: a page fault on an address, or an x86-64 divide
// error. A sent signal, a protection fault (Windows reports its address as all ones, Linux sends no address) and the
// rest are never judged.
void judged_fault(const CrashEvidence& e, bool& access, bool& divide) noexcept
{
    const bool x86 = e.instruction_set == InstructionSet::X64;
    access = false;
    divide = false;
    if (e.kind == CrashCaptureKind::Minidump)
    {
        access = (e.code == kStatusAccessViolation || e.code == kStatusInPageError) && e.address_known &&
                 e.address != ~crd::u64{0};
        divide = x86 && (e.code == kStatusIntegerDivideByZero || e.code == kStatusIntegerOverflow);
    }
    else if (e.kind == CrashCaptureKind::Record && !e.sent)
    {
        const crd::i32 sc = e.signal_code;
        const bool page_fault = !x86 || (e.trap_known && e.trap == kTrapPageFault);
        access = e.address_known && page_fault &&
                 ((e.code == kSigSegv && (sc == kSegvMapErr || sc == kSegvAccErr || sc == kSegvPkuErr)) ||
                  (e.code == kSigBus && (sc == kBusAdrErr || (!x86 && sc == kBusAdrAln))));
        divide = x86 && e.code == kSigFpe && (sc == kFpeIntDiv || sc == kFpeIntOvf) && e.trap_known &&
                 e.trap == kTrapDivideError;
    }
}

[[nodiscard]] bool kind_made(crd::u8 kinds, FaultAccess access) noexcept
{
    switch (access)
    {
        case FaultAccess::Read:
            return (kinds & kAccessRead) != 0U;
        case FaultAccess::Write:
            return (kinds & kAccessWrite) != 0U;
        case FaultAccess::Execute:
            return false; // only the instruction's own bytes are executed
        case FaultAccess::Unknown:
            break;
    }
    return true;
}

InstructionCheck check_instruction(const CrashEvidence& e, const CodeImageCheck* code_image) noexcept
{
    InstructionCheck k;
    k.pc = e.pc;
    bool access = false;
    bool divide = false;
    judged_fault(e, access, divide);
    if (!access && !divide)
    {
        k.reason = "the fault is neither a page fault on an address nor a divide error";
        return k;
    }
    if (!e.registers.known)
    {
        k.reason = "the capture holds no registers for its pc";
        return k;
    }
    if (code_image == nullptr || code_image->result != CodeImageResult::Matches)
    {
        k.reason = "the instruction's bytes are not proven to be its module's (its image comparison does not match)";
        return k;
    }
    const InstructionAccess a = decode_instruction_access(e.instruction_set, {e.code_bytes, e.code_size}, e.registers);
    if (!a.decoded)
    {
        k.reason = a.reason;
        return k;
    }
    k.length = a.length;
    k.accesses = a.count;
    k.result = InstructionCheckResult::Consistent;
    if (divide)
    {
        if (!a.divides)
        {
            k.result = InstructionCheckResult::DivideWithoutCause;
            k.reason = "it does not divide";
        }
        else if (!a.divisor_known)
        {
            k.result = InstructionCheckResult::NotJudged;
            k.reason = "its divisor is in memory, which the capture does not hold";
        }
        else if (!a.divide_faults)
        {
            k.result = InstructionCheckResult::DivideWithoutCause;
            k.reason = "its divisor is not zero and its quotient fits";
        }
        return k;
    }
    if (e.address - e.pc < a.length)
    {
        return k; // fetching its own bytes
    }
    bool address_made = false;
    for (crd::u32 i = 0U; i < a.count; ++i)
    {
        const MemoryAccess& m = a.accesses[i];
        if (e.address - m.address < m.size)
        {
            address_made = true;
            if (kind_made(m.kinds, e.access))
            {
                return k;
            }
        }
    }
    k.result = InstructionCheckResult::AccessNotMade;
    if (!address_made)
    {
        k.reason = e.access == FaultAccess::Execute ? cont::StringView{"an execution outside its own bytes"}
                                                    : cont::StringView{"an address it does not access"};
    }
    else
    {
        k.reason = e.access == FaultAccess::Write ? cont::StringView{"a write it does not make"}
                                                  : cont::StringView{"a read it does not make"};
    }
    return k;
}

// ---- the code against the module's image ----------------------------------------------------------------------------

constexpr crd::u32 kWindow = crd::crash::kCodeWindowBytes;

[[nodiscard]] crd::u64 le(const crd::u8* p, crd::u32 width) noexcept
{
    crd::u64 v = 0U;
    for (crd::u32 i = 0U; i < width; ++i)
    {
        v |= static_cast<crd::u64>(p[i]) << (8U * i);
    }
    return v;
}

[[nodiscard]] bool image_read(const ImageReader& image, crd::u64 offset, void* out, crd::u32 size) noexcept
{
    return image.read != nullptr && image.read(image.context, offset, out, size);
}

// The image's bytes at the pc as the loader placed them: `out` holds `n` (at most the capture's) of them, or `why`
// says why they cannot be known.
struct LoadedBytes
{
    crd::u8 out[kWindow] = {};
    crd::u32 n = 0U;
    cont::StringView why;
};

// A PE section: where an RVA lies in the file. `limit` is how many bytes from the RVA the file holds for it.
struct PeSection
{
    crd::u32 va = 0U;
    crd::u32 virtual_size = 0U;
    crd::u32 raw_size = 0U;
    crd::u32 raw_offset = 0U;
};

[[nodiscard]] bool pe_file_offset(cont::ConstSpan<PeSection> sections, crd::u64 rva, crd::u64& offset,
                                  crd::u64& limit) noexcept
{
    for (const PeSection& sec : sections)
    {
        const crd::u64 held =
            sec.virtual_size != 0U && sec.virtual_size < sec.raw_size ? sec.virtual_size : sec.raw_size;
        if (rva >= sec.va && rva - sec.va < held)
        {
            offset = sec.raw_offset + (rva - sec.va);
            limit = held - (rva - sec.va);
            return true;
        }
    }
    return false;
}

// IMAGE_REL_BASED_*: the fields a base relocation adjusts, by type (0: none, ABSOLUTE pads a block).
constexpr crd::u32 kRelHigh = 1U;
constexpr crd::u32 kRelLow = 2U;
constexpr crd::u32 kRelHighLow = 3U;
constexpr crd::u32 kRelHighAdj = 4U;
constexpr crd::u32 kRelDir64 = 10U;
constexpr crd::u32 kBaseRelocationDirectory = 5U; // IMAGE_DIRECTORY_ENTRY_BASERELOC

// Applies the base relocations of `dir` that touch [rva, rva + loaded.n) for a load `delta` from the image base.
void pe_relocate(const ImageReader& image, cont::ConstSpan<PeSection> sections, crd::u64 dir_rva, crd::u64 dir_size,
                 crd::u64 delta, crd::u64 rva, LoadedBytes& loaded) noexcept
{
    crd::u64 dir_offset = 0U;
    crd::u64 dir_limit = 0U;
    if (!pe_file_offset(sections, dir_rva, dir_offset, dir_limit) || dir_size > dir_limit)
    {
        loaded.why = "the image's base relocations lie outside its sections";
        return;
    }
    for (crd::u64 pos = 0U; pos + 8U <= dir_size;)
    {
        crd::u8 head[8];
        if (!image_read(image, dir_offset + pos, head, 8U))
        {
            loaded.why = "the image's base relocations cannot be read";
            return;
        }
        const crd::u64 page = le(head, 4U);
        const crd::u64 block = le(head + 4U, 4U);
        if (block < 8U || block > dir_size - pos)
        {
            loaded.why = "the image's base relocations are malformed";
            return;
        }
        // An entry adjusts at most 8 bytes from its page offset (at most 0xFFF), so only these blocks can reach in.
        if (page < rva + loaded.n && page + 0x1000U + 8U > rva)
        {
            bool skip_next = false;
            for (crd::u64 at = 8U; at + 2U <= block; at += 2U)
            {
                crd::u8 entry[2];
                if (!image_read(image, dir_offset + pos + at, entry, 2U))
                {
                    loaded.why = "the image's base relocations cannot be read";
                    return;
                }
                if (skip_next)
                {
                    skip_next = false; // HIGHADJ's second slot holds its low half, not an entry
                    continue;
                }
                const crd::u32 type = static_cast<crd::u32>(le(entry, 2U)) >> 12U;
                const crd::u64 target = page + (le(entry, 2U) & 0xFFFU);
                crd::u32 width = 0U;
                crd::u64 add = 0U;
                bool applied = true;
                switch (type)
                {
                    case 0U:
                        continue;
                    case kRelHigh:
                        width = 2U;
                        add = (delta >> 16U) & 0xFFFFU;
                        break;
                    case kRelLow:
                        width = 2U;
                        add = delta & 0xFFFFU;
                        break;
                    case kRelHighLow:
                        width = 4U;
                        add = delta & 0xFFFFFFFFU;
                        break;
                    case kRelDir64:
                        width = 8U;
                        add = delta;
                        break;
                    default:
                        width = 8U; // a type this reader does not apply (at most 8 bytes): refused if it reaches in
                        applied = false;
                        skip_next = type == kRelHighAdj;
                        break;
                }
                if (target >= rva + loaded.n || target + width <= rva)
                {
                    continue;
                }
                if (!applied)
                {
                    loaded.why = "a base relocation this reader does not apply reaches the faulting instruction";
                    return;
                }
                crd::u8 field[8];
                crd::u64 offset = 0U;
                crd::u64 limit = 0U;
                if (!pe_file_offset(sections, target, offset, limit) || limit < width ||
                    !image_read(image, offset, field, width))
                {
                    loaded.why = "a base relocation at the faulting instruction cannot be read";
                    return;
                }
                const crd::u64 value = le(field, width) + add;
                for (crd::u32 i = 0U; i < width; ++i)
                {
                    if (target + i >= rva && target + i < rva + loaded.n)
                    {
                        loaded.out[target + i - rva] = static_cast<crd::u8>(value >> (8U * i));
                    }
                }
            }
        }
        pos += block;
    }
}

// The PE file's bytes at the pc: its identity is the module's, and the pc's section maps it.
void pe_loaded_bytes(const CrashEvidence& e, const ModuleIdentity& m, const ImageReader& image, LoadedBytes& loaded)
{
    if (m.id_kind != SymbolIdKind::Rsds && m.id_kind != SymbolIdKind::PeImage)
    {
        loaded.why = "the module is not a Windows image";
        return;
    }
    crd::u8 dos[64];
    crd::u8 nt[24];
    if (!image_read(image, 0U, dos, sizeof(dos)) || !image_read(image, le(dos + 0x3CU, 4U), nt, sizeof(nt)) ||
        std::memcmp(nt, "PE\0\0", 4U) != 0)
    {
        loaded.why = "the image has no PE header";
        return;
    }
    const crd::u64 nt_at = le(dos + 0x3CU, 4U);
    const crd::u32 section_count = static_cast<crd::u32>(le(nt + 6U, 2U));
    const crd::u32 stamp = static_cast<crd::u32>(le(nt + 8U, 4U));
    const crd::u32 optional_size = static_cast<crd::u32>(le(nt + 20U, 2U));
    crd::u8 opt[240] = {};
    if (optional_size < 96U || optional_size > sizeof(opt) || section_count > 96U ||
        !image_read(image, nt_at + 24U, opt, optional_size))
    {
        loaded.why = "the image's PE optional header is malformed";
        return;
    }
    const crd::u32 magic = static_cast<crd::u32>(le(opt, 2U));
    const bool plus = magic == 0x20BU;
    if (!plus && magic != 0x10BU)
    {
        loaded.why = "the image's PE optional header is malformed";
        return;
    }
    const crd::u64 image_base = plus ? le(opt + 24U, 8U) : le(opt + 28U, 4U);
    const crd::u32 size_of_image = static_cast<crd::u32>(le(opt + 56U, 4U));
    const crd::u32 checksum = static_cast<crd::u32>(le(opt + 64U, 4U));
    const crd::u32 dirs_at = plus ? 112U : 96U;
    const crd::u64 dir_count = le(opt + dirs_at - 4U, 4U);
    if (m.timestamp == 0U || stamp != m.timestamp || size_of_image != m.size ||
        (m.checksum != 0U && checksum != m.checksum))
    {
        loaded.why = "the image is not the module's (its TimeDateStamp, SizeOfImage or CheckSum differ)";
        return;
    }
    PeSection sections[96];
    for (crd::u32 i = 0U; i < section_count; ++i)
    {
        crd::u8 h[40];
        if (!image_read(image, nt_at + 24U + optional_size + 40U * i, h, sizeof(h)))
        {
            loaded.why = "the image's section table cannot be read";
            return;
        }
        sections[i] = PeSection{static_cast<crd::u32>(le(h + 12U, 4U)), static_cast<crd::u32>(le(h + 8U, 4U)),
                                static_cast<crd::u32>(le(h + 16U, 4U)), static_cast<crd::u32>(le(h + 20U, 4U))};
    }
    const cont::ConstSpan<PeSection> table{sections, section_count};
    const crd::u64 rva = e.pc - m.base;
    crd::u64 offset = 0U;
    crd::u64 limit = 0U;
    if (!pe_file_offset(table, rva, offset, limit))
    {
        loaded.why = "the pc is in none of the image's file-backed sections";
        return;
    }
    loaded.n = static_cast<crd::u32>(limit < e.code_size ? limit : e.code_size);
    if (!image_read(image, offset, loaded.out, loaded.n))
    {
        loaded.why = "the image's bytes at the pc cannot be read";
        return;
    }
    const crd::u64 delta = m.base - image_base;
    if (delta != 0U && dir_count > kBaseRelocationDirectory &&
        dirs_at + 8U * (kBaseRelocationDirectory + 1U) <= optional_size)
    {
        const crd::u8* dir = opt + dirs_at + 8U * kBaseRelocationDirectory;
        if (le(dir + 4U, 4U) != 0U)
        {
            pe_relocate(image, table, le(dir, 4U), le(dir + 4U, 4U), delta, rva, loaded);
        }
    }
}

// ELF program header and dynamic tags this reads.
constexpr crd::u32 kPtLoad = 1U;
constexpr crd::u32 kPtDynamic = 2U;
constexpr crd::u32 kPtNote = 4U;
constexpr crd::u64 kDtTextRel = 22U;
constexpr crd::u64 kDtFlags = 30U;
constexpr crd::u64 kDfTextRel = 4U;
constexpr crd::u32 kNtGnuBuildId = 3U;
constexpr crd::u32 kMaxProgramHeaders = 256U;

struct ElfSegment
{
    crd::u32 type = 0U;
    crd::u64 offset = 0U;
    crd::u64 vaddr = 0U;
    crd::u64 filesz = 0U;
    crd::u64 memsz = 0U;
};

// Whether a PT_NOTE segment holds the GNU build-id `m` names (a longer id is compared on the bytes `m` kept).
[[nodiscard]] bool elf_note_has_build_id(const ImageReader& image, const ElfSegment& note, const ModuleIdentity& m)
{
    for (crd::u64 at = 0U; at + 12U <= note.filesz && at < 0x10000U;)
    {
        crd::u8 head[12];
        if (!image_read(image, note.offset + at, head, sizeof(head)))
        {
            return false;
        }
        const crd::u64 name_size = le(head, 4U);
        const crd::u64 desc_size = le(head + 4U, 4U);
        const crd::u64 name_padded = (name_size + 3U) & ~crd::u64{3};
        const crd::u64 desc_padded = (desc_size + 3U) & ~crd::u64{3};
        if (name_padded > note.filesz || desc_padded > note.filesz - name_padded ||
            12U + name_padded + desc_padded > note.filesz - at)
        {
            return false;
        }
        crd::u8 name[4];
        if (le(head + 8U, 4U) == kNtGnuBuildId && name_size == 4U &&
            image_read(image, note.offset + at + 12U, name, 4U) && std::memcmp(name, "GNU\0", 4U) == 0)
        {
            crd::u8 id[kMaxSymbolIdBytes];
            const crd::u32 kept = static_cast<crd::u32>(desc_size < kMaxSymbolIdBytes ? desc_size : kMaxSymbolIdBytes);
            return kept == m.id_len && image_read(image, note.offset + at + 12U + name_padded, id, kept) &&
                   std::memcmp(id, m.id, kept) == 0;
        }
        at += 12U + name_padded + desc_padded;
    }
    return false;
}

// Whether the dynamic segment asks for text relocations (the loader then writes the code).
[[nodiscard]] bool elf_text_relocations(const ImageReader& image, const ElfSegment& dynamic, bool is64, bool& textrel)
{
    const crd::u32 entry = is64 ? 16U : 8U;
    const crd::u32 word = is64 ? 8U : 4U;
    for (crd::u64 at = 0U; at + entry <= dynamic.filesz && at < 0x10000U; at += entry)
    {
        crd::u8 d[16];
        if (!image_read(image, dynamic.offset + at, d, entry))
        {
            return false;
        }
        const crd::u64 tag = le(d, word);
        const crd::u64 value = le(d + word, word);
        if (tag == 0U)
        {
            break;
        }
        textrel = textrel || tag == kDtTextRel || (tag == kDtFlags && (value & kDfTextRel) != 0U);
    }
    return true;
}

// The ELF file's bytes at the pc: its build-id and PT_LOAD span are the module's, and the PT_LOAD holding the pc
// maps it.
void elf_loaded_bytes(const CrashEvidence& e, const ModuleIdentity& m, const ImageReader& image, LoadedBytes& loaded)
{
    if (m.id_kind != SymbolIdKind::GnuBuildId || m.id_len == 0U)
    {
        loaded.why = "the module has no GNU build-id to check the image against";
        return;
    }
    crd::u8 eh[64];
    if (!image_read(image, 0U, eh, 52U) || (eh[4] != 1U && eh[4] != 2U) || eh[5] != 1U)
    {
        loaded.why = "the image is not a little-endian ELF file";
        return;
    }
    const bool is64 = eh[4] == 2U;
    if (is64 && !image_read(image, 0U, eh, 64U))
    {
        loaded.why = "the image's ELF header cannot be read";
        return;
    }
    const crd::u64 phoff = is64 ? le(eh + 32U, 8U) : le(eh + 28U, 4U);
    const crd::u32 phentsize = static_cast<crd::u32>(le(eh + (is64 ? 54U : 42U), 2U));
    const crd::u32 phnum = static_cast<crd::u32>(le(eh + (is64 ? 56U : 44U), 2U));
    if (phentsize != (is64 ? 56U : 32U) || phnum > kMaxProgramHeaders)
    {
        loaded.why = "the image's ELF program headers are malformed";
        return;
    }
    ElfSegment segments[kMaxProgramHeaders];
    crd::u64 low = ~crd::u64{0};
    crd::u64 high = 0U;
    bool identified = false;
    bool textrel = false;
    for (crd::u32 i = 0U; i < phnum; ++i)
    {
        crd::u8 ph[56];
        if (!image_read(image, phoff + static_cast<crd::u64>(phentsize) * i, ph, phentsize))
        {
            loaded.why = "the image's ELF program headers cannot be read";
            return;
        }
        ElfSegment& seg = segments[i];
        seg.type = static_cast<crd::u32>(le(ph, 4U));
        seg.offset = is64 ? le(ph + 8U, 8U) : le(ph + 4U, 4U);
        seg.vaddr = is64 ? le(ph + 16U, 8U) : le(ph + 8U, 4U);
        seg.filesz = is64 ? le(ph + 32U, 8U) : le(ph + 16U, 4U);
        seg.memsz = is64 ? le(ph + 40U, 8U) : le(ph + 20U, 4U);
        if (seg.type == kPtLoad)
        {
            low = seg.vaddr < low ? seg.vaddr : low;
            high = seg.vaddr + seg.memsz > high ? seg.vaddr + seg.memsz : high;
        }
        identified = identified || (seg.type == kPtNote && elf_note_has_build_id(image, seg, m));
        if (seg.type == kPtDynamic && !elf_text_relocations(image, seg, is64, textrel))
        {
            loaded.why = "the image's dynamic segment cannot be read";
            return;
        }
    }
    if (!identified || low > high || high - low != m.size)
    {
        loaded.why = "the image is not the module's (its GNU build-id or PT_LOAD span differ)";
        return;
    }
    if (textrel)
    {
        loaded.why = "the image has text relocations, so the loader wrote its code";
        return;
    }
    const crd::u64 vaddr = e.pc - m.base + low;
    for (crd::u32 i = 0U; i < phnum; ++i)
    {
        const ElfSegment& seg = segments[i];
        if (seg.type != kPtLoad || vaddr < seg.vaddr || vaddr - seg.vaddr >= seg.filesz)
        {
            continue;
        }
        const crd::u64 limit = seg.filesz - (vaddr - seg.vaddr);
        loaded.n = static_cast<crd::u32>(limit < e.code_size ? limit : e.code_size);
        if (!image_read(image, seg.offset + (vaddr - seg.vaddr), loaded.out, loaded.n))
        {
            loaded.why = "the image's bytes at the pc cannot be read";
        }
        return;
    }
    loaded.why = "the pc is in none of the image's file-backed segments";
}

[[nodiscard]] crd::u32 popcount8(crd::u32 v) noexcept
{
    crd::u32 n = 0U;
    for (; v != 0U; v &= v - 1U)
    {
        ++n;
    }
    return n;
}

} // namespace

cont::StringView crash_capture_kind_name(CrashCaptureKind k) noexcept
{
    switch (k)
    {
        case CrashCaptureKind::None:
            return "none";
        case CrashCaptureKind::Minidump:
            return "minidump";
        case CrashCaptureKind::Record:
            return "record";
    }
    return "unknown";
}

CrashEvidence read_crash_evidence(cont::ConstSpan<crd::u8> capture, crd::memory::IAllocator* alloc)
{
    const cont::StringView text{reinterpret_cast<const char*>(capture.data()), capture.size()};
    if (text.starts_with("MDMP"))
    {
        return read_minidump(capture, alloc);
    }
    if (text.starts_with("crd crash record\n"))
    {
        return read_record(text, capture, alloc);
    }
    CrashEvidence e(alloc);
    return refuse(e, "not a crash capture (neither a minidump nor a crash record)");
}

cont::StringView code_image_result_name(CodeImageResult r) noexcept
{
    switch (r)
    {
        case CodeImageResult::NotCompared:
            return "not-compared";
        case CodeImageResult::Matches:
            return "matches";
        case CodeImageResult::OneBitFlipped:
            return "one-bit-flipped";
        case CodeImageResult::Modified:
            return "modified";
    }
    return "unknown";
}

const ModuleIdentity* code_module(const CrashEvidence& e) noexcept
{
    if (!e.ok || !e.pc_known)
    {
        return nullptr;
    }
    for (const ModuleIdentity& m : e.modules)
    {
        if (!m.unloaded && inside(m, e.pc))
        {
            return &m;
        }
    }
    return nullptr;
}

CodeImageCheck compare_code_with_image(const CrashEvidence& e, const ImageReader& image, crd::memory::IAllocator* alloc)
{
    CodeImageCheck c(alloc);
    c.pc = e.pc;
    const ModuleIdentity* m = code_module(e);
    if (m == nullptr)
    {
        c.reason = "the pc is in no loaded module";
        return c;
    }
    c.module.append(view(m->name));
    if (e.code_size == 0U)
    {
        c.reason = "the capture holds no code bytes";
        return c;
    }
    crd::u8 magic[4];
    if (!image_read(image, 0U, magic, sizeof(magic)))
    {
        c.reason = "the image cannot be read";
        return c;
    }
    LoadedBytes loaded;
    if (magic[0] == 'M' && magic[1] == 'Z')
    {
        pe_loaded_bytes(e, *m, image, loaded);
    }
    else if (std::memcmp(magic,
                         "\x7f"
                         "ELF",
                         4U) == 0)
    {
        elf_loaded_bytes(e, *m, image, loaded);
    }
    else
    {
        loaded.why = "the image is neither a PE nor an ELF file";
    }
    if (!loaded.why.empty() || loaded.n == 0U)
    {
        c.reason = loaded.why.empty() ? cont::StringView{"the image holds no bytes at the pc"} : loaded.why;
        return c;
    }
    c.compared = loaded.n;
    for (crd::u32 i = 0U; i < loaded.n; ++i)
    {
        const crd::u32 bits = popcount8(static_cast<crd::u32>(e.code_bytes[i] ^ loaded.out[i]));
        if (bits != 0U && c.differing_bits == 0U)
        {
            c.offset = i;
            c.recorded = e.code_bytes[i];
            c.image = loaded.out[i];
            for (crd::u32 b = 0U; b < 8U; ++b)
            {
                if (((e.code_bytes[i] ^ loaded.out[i]) >> b) == 1U)
                {
                    c.bit = b; // the highest differing bit; with one bit the only one
                }
            }
        }
        c.differing_bits += bits;
    }
    c.result = CodeImageResult::Modified;
    if (c.differing_bits <= 1U)
    {
        c.result = c.differing_bits == 0U ? CodeImageResult::Matches : CodeImageResult::OneBitFlipped;
    }
    return c;
}

cont::StringView replay_outcome_name(ReplayOutcome r) noexcept
{
    switch (r)
    {
        case ReplayOutcome::NotRun:
            return "not-run";
        case ReplayOutcome::Reproduced:
            return "reproduced";
        case ReplayOutcome::NotReproduced:
            return "not-reproduced";
    }
    return "unknown";
}

bool parse_replay_outcome(cont::StringView text, ReplayOutcome& out) noexcept
{
    for (const ReplayOutcome r : {ReplayOutcome::NotRun, ReplayOutcome::Reproduced, ReplayOutcome::NotReproduced})
    {
        if (text == replay_outcome_name(r))
        {
            out = r;
            return true;
        }
    }
    return false;
}

void append_fault_signal_names(crd::u32 signals, cont::String& out)
{
    constexpr cont::StringView names[] = {"machine-check",         "code-address-bit-flip", "data-address-bit-flip",
                                          "replay-not-reproduced", "code-bit-flip",         "access-not-made",
                                          "divide-without-cause"};
    bool first = true;
    for (crd::u32 bit = 0U; bit < sizeof(names) / sizeof(names[0]); ++bit)
    {
        if ((signals & (1U << bit)) != 0U)
        {
            if (!first)
            {
                out.push_back(',');
            }
            out.append(names[bit]);
            first = false;
        }
    }
}

cont::StringView fault_access_name(FaultAccess a) noexcept
{
    switch (a)
    {
        case FaultAccess::Unknown:
            return "unknown";
        case FaultAccess::Read:
            return "read";
        case FaultAccess::Write:
            return "write";
        case FaultAccess::Execute:
            return "execute";
    }
    return "unknown";
}

cont::StringView instruction_check_result_name(InstructionCheckResult r) noexcept
{
    switch (r)
    {
        case InstructionCheckResult::NotJudged:
            return "not-judged";
        case InstructionCheckResult::Consistent:
            return "consistent";
        case InstructionCheckResult::AccessNotMade:
            return "access-not-made";
        case InstructionCheckResult::DivideWithoutCause:
            return "divide-without-cause";
    }
    return "not-judged";
}

cont::StringView hardware_verdict_name(HardwareVerdict v) noexcept
{
    switch (v)
    {
        case HardwareVerdict::None:
            return "none";
        case HardwareVerdict::Possible:
            return "possible";
        case HardwareVerdict::Likely:
            return "likely";
    }
    return "unknown";
}

cont::StringView microcode_state_name(MicrocodeState s) noexcept
{
    switch (s)
    {
        case MicrocodeState::NotListed:
            return "not-listed";
        case MicrocodeState::Unknown:
            return "unknown";
        case MicrocodeState::BeforeFix:
            return "before-fix";
        case MicrocodeState::Fixed:
            return "fixed";
    }
    return "unknown";
}

cont::ConstSpan<CpuAdvisory> cpu_advisories() noexcept
{
    return {kAdvisories, sizeof(kAdvisories) / sizeof(kAdvisories[0])};
}

const CpuAdvisory* find_cpu_advisory(const HardwareIdentity& machine) noexcept
{
    if ((machine.flags & kHardwareCpuKnown) == 0U)
    {
        return nullptr;
    }
    for (const CpuAdvisory& a : kAdvisories)
    {
        if (view(machine.cpu_vendor) == a.vendor && machine.cpu_signature == a.signature &&
            raptor_lake_desktop(view(machine.cpu_model)))
        {
            return &a;
        }
    }
    return nullptr;
}

FaultClassification classify_fault(const CrashEvidence* crash, const HardwareIdentity* machine, ReplayOutcome replay,
                                   const CodeImageCheck* code_image, crd::memory::IAllocator* alloc)
{
    FaultClassification c(alloc);
    c.replay = replay;
    if (crash != nullptr && crash->ok)
    {
        c.crash = true;
        c.sent = crash->sent;
        if (crash->kind == CrashCaptureKind::Record && !crash->sent && crash->code == kSigBus &&
            (crash->signal_code == kBusMceErrAr || crash->signal_code == kBusMceErrAo))
        {
            c.signals |= kFaultSignalMachineCheck;
        }
        if (!crash->sent && crash->address_known)
        {
            // Execution at the faulting address is a fetch from it: judge the code pointer, else the data pointer.
            const bool fetch = crash->pc_known && crash->pc == crash->address;
            find_bit_flip(*crash, crash->address, fetch ? c.code_flip : c.data_flip);
            c.signals |= c.code_flip.found ? kFaultSignalCodeAddressBitFlip : 0U;
            c.signals |= c.data_flip.found ? kFaultSignalDataAddressBitFlip : 0U;
        }
        if (code_image != nullptr)
        {
            c.code_image.result = code_image->result;
            c.code_image.reason = code_image->reason;
            c.code_image.pc = code_image->pc;
            c.code_image.compared = code_image->compared;
            c.code_image.differing_bits = code_image->differing_bits;
            c.code_image.offset = code_image->offset;
            c.code_image.bit = code_image->bit;
            c.code_image.recorded = code_image->recorded;
            c.code_image.image = code_image->image;
            c.code_image.module.append(view(code_image->module));
            c.signals |= code_image->result == CodeImageResult::OneBitFlipped ? kFaultSignalCodeBitFlip : 0U;
        }
        c.instruction = check_instruction(*crash, code_image);
        c.signals |= c.instruction.result == InstructionCheckResult::AccessNotMade ? kFaultSignalAccessNotMade : 0U;
        c.signals |=
            c.instruction.result == InstructionCheckResult::DivideWithoutCause ? kFaultSignalDivideWithoutCause : 0U;
    }
    if (replay == ReplayOutcome::NotReproduced)
    {
        c.signals |= kFaultSignalReplayNotReproduced;
    }
    if (machine != nullptr)
    {
        c.advisory = find_cpu_advisory(*machine);
        if (c.advisory != nullptr)
        {
            if ((machine->flags & kHardwareMicrocodeKnown) == 0U)
            {
                c.microcode = MicrocodeState::Unknown;
            }
            else
            {
                c.microcode = machine->microcode < c.advisory->fixed_microcode ? MicrocodeState::BeforeFix
                                                                               : MicrocodeState::Fixed;
            }
        }
    }
    crd::u32 count = 0U;
    for (crd::u32 s = c.signals; s != 0U; s &= s - 1U)
    {
        ++count;
    }
    if ((c.signals & kFaultSignalMachineCheck) != 0U || count >= 2U || (count == 1U && c.advisory != nullptr))
    {
        c.verdict = HardwareVerdict::Likely;
    }
    else if (count == 1U)
    {
        c.verdict = HardwareVerdict::Possible;
    }
    return c;
}

cont::String fault_message(const FaultClassification& c, const HardwareIdentity* machine,
                           crd::memory::IAllocator* alloc)
{
    cont::String out(alloc);
    if (c.verdict == HardwareVerdict::None)
    {
        out.append("Nothing in this failure points to a hardware fault");
        if (c.sent)
        {
            out.append(" (it ended on a signal another process or the program itself sent)");
        }
        if (c.microcode == MicrocodeState::BeforeFix)
        {
            out.append(", but ");
            append_processor(out, c, machine, false);
            out.append(", and its microcode ");
            append_hex(out, machine->microcode);
            out.append(" is older than the fix ");
            append_hex(out, c.advisory->fixed_microcode);
            out.append("; a BIOS update from the board's maker installs it");
        }
        out.append(".");
        return out;
    }
    out.append(c.verdict == HardwareVerdict::Likely ? "This failure is likely a hardware fault: "
                                                    : "This failure shows one sign of a hardware fault, which alone "
                                                      "is not proof: ");
    append_evidence(out, c);
    if (c.verdict == HardwareVerdict::Possible)
    {
        out.append(". If failures like it repeat in different places, test the machine's memory and processor.");
        return out;
    }
    out.append(". ");
    if (c.advisory == nullptr)
    {
        out.append((c.signals & kFaultSignalMachineCheck) != 0U
                       ? "Test the memory and remove any memory overclock (such as XMP or EXPO)."
                       : "Test the machine's memory and processor, and check that the BIOS is current.");
        return out;
    }
    append_processor(out, c, machine, true);
    switch (c.microcode)
    {
        case MicrocodeState::BeforeFix:
            out.append(", and its microcode ");
            append_hex(out, machine->microcode);
            out.append(" is older than the fix ");
            append_hex(out, c.advisory->fixed_microcode);
            out.append(": update the BIOS from the board's maker, which installs it.");
            break;
        case MicrocodeState::Fixed:
            out.append(", and its microcode ");
            append_hex(out, machine->microcode);
            out.append(" already has the fix, which stops further damage but cannot repair a processor that has "
                       "already degraded: contact the processor's maker or the system's seller about a replacement "
                       "under warranty.");
            break;
        case MicrocodeState::Unknown:
        case MicrocodeState::NotListed:
            out.append(", fixed by microcode ");
            append_hex(out, c.advisory->fixed_microcode);
            out.append(" or later: make sure the BIOS is current, and if failures continue, contact the processor's "
                       "maker or the system's seller about a replacement under warranty.");
            break;
    }
    return out;
}

} // namespace crd::perf
