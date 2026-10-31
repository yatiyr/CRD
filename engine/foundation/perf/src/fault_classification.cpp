// crd-perf -- hardware fault classification (HW-RESILIENCE.2). See fault_classification.hpp.

#include "payload_codec.hpp"

#include <crd/perf/fault_classification.hpp>

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

// Linux signal numbers (x86-64 and AArch64 agree) and SIGBUS's machine-check codes.
constexpr crd::u32 kSigBus = 7U;
constexpr crd::u32 kSigSegv = 11U;
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
    (void)in.u64(); // ExceptionInformation[0]: read, write or execute
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
    }
    e.modules = parse_minidump_modules(dump, alloc);
    for (ModuleIdentity& m : parse_minidump_unloaded_modules(dump, alloc))
    {
        e.modules.push_back(static_cast<ModuleIdentity&&>(m));
    }
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
    // The registers, between "\nregs " and the identities after them: x86-64's rip, AArch64's pc.
    const crd::usize regs = text.find("\nregs ");
    if (regs != cont::StringView::npos)
    {
        crd::usize stop = text.find("\nbuild_id ", regs);
        if (stop == cont::StringView::npos)
        {
            stop = text.find("\nmodules ", regs);
        }
        const cont::StringView block =
            text.substr(regs + 1U, stop == cont::StringView::npos ? text.size() - regs - 1U : stop - regs);
        if (block.starts_with("regs x86_64\n"))
        {
            e.pc_known = record_value(block, "rip", true, e.pc, unused);
        }
        else if (block.starts_with("regs aarch64\n"))
        {
            e.pc_known = record_value(block, "pc", true, e.pc, unused);
        }
    }
    e.modules = parse_crash_record_modules(bytes, alloc);
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
    constexpr cont::StringView names[] = {"machine-check", "code-address-bit-flip", "data-address-bit-flip",
                                          "replay-not-reproduced"};
    bool first = true;
    for (crd::u32 bit = 0U; bit < 4U; ++bit)
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
                                   crd::memory::IAllocator* alloc)
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
