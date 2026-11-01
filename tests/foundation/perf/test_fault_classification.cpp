// HW-RESILIENCE.2: hardware fault classification (crd/perf/fault_classification.hpp) -- reading a minidump's and a
// Linux crash record's fault and code bytes, each impossible-state signature and its guards, the code's comparison
// with PE and ELF images, the faulting instruction's registers and its check against the fault, the processor
// advisory table, the verdict and its sentence, and bundle.inspect's judgement of a capture and of a bundle that
// carries one.

#include "module_file_identity.hpp"

#include <crd/containers/array.hpp>
#include <crd/containers/string.hpp>
#include <crd/perf/bundle.hpp>
#include <crd/perf/diag_commands.hpp>
#include <crd/perf/fault_classification.hpp>
#include <crd/perf/hardware_identity.hpp>
#include <crd/perf/symbol_index.hpp>
#include <crd/platform/filesystem.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cstdio>
#include <cstring>

namespace cont = crd::containers;
namespace cp = crd::perf;
using crd::u32;
using crd::u64;
using crd::u8;
using crd::usize;

namespace
{

cont::StringView view(const cont::String& s)
{
    return cont::StringView{s.data(), s.size()};
}

void append_hex(cont::String& out, u64 v)
{
    char text[24];
    (void)std::snprintf(text, sizeof(text), "0x%llx", static_cast<unsigned long long>(v));
    out.append(text);
}

constexpr u64 kExeBase = 0x00005555'55554000ULL;
constexpr u64 kExeSize = 0x00000000'00200000ULL;
constexpr u64 kLibBase = 0x00007f12'34000000ULL;
constexpr u64 kLibSize = 0x00000000'00100000ULL;
constexpr u64 kHeapAddr = 0x00005555'5a000010ULL; // outside both modules, and no single bit away from either

// A Linux crash record as crd::crash writes it: the signal line, x86-64 registers (rip among them) and two modules.
cont::String record(cont::StringView signal_line, u64 rip)
{
    cont::String out;
    out.append("crd crash record\npid 7 tid 7\n");
    out.append(signal_line);
    out.append("\nexe /opt/game/game\nregs x86_64\nr8 0x0 r9 0x0 r10 0x0 r11 0x0\nrdx 0x0 rax 0x0 rcx 0x0 rsp 0x7ffc0\n"
               "rip ");
    append_hex(out, rip);
    out.append(" efl 0x246 csgsfs 0x33 err 0x4\nbuild_id 0102\nmodules 2\nmodule ");
    append_hex(out, kExeBase);
    out.push_back(' ');
    append_hex(out, kExeSize);
    out.append(" 0102 (exe)\nmodule ");
    append_hex(out, kLibBase);
    out.push_back(' ');
    append_hex(out, kLibSize);
    out.append(" 0304 libcerid.so\n");
    return out;
}

cont::String signal_line(u32 signal, crd::i32 code, u64 addr)
{
    char text[96];
    (void)std::snprintf(text, sizeof(text), "signal %u code %d addr 0x%llx", signal, code,
                        static_cast<unsigned long long>(addr));
    return cont::String{cont::StringView{text}};
}

cp::CrashEvidence read(const cont::String& capture)
{
    return cp::read_crash_evidence({reinterpret_cast<const u8*>(capture.data()), capture.size()});
}

void put32(cont::Array<u8>& out, u32 v)
{
    for (u32 i = 0U; i < 4U; ++i)
    {
        out.push_back(static_cast<u8>((v >> (8U * i)) & 0xFFU));
    }
}

void put64(cont::Array<u8>& out, u64 v)
{
    put32(out, static_cast<u32>(v));
    put32(out, static_cast<u32>(v >> 32U));
}

// The bytes of `n` streams of `types` after a minidump header and directory.
cont::Array<u8> assemble(const u32* types, const cont::Array<u8>* const* streams, u32 n)
{
    cont::Array<u8> out;
    put32(out, 0x504D444DU); // 'MDMP'
    put32(out, 0xA793U);
    put32(out, n);   // streams
    put32(out, 32U); // the directory follows the header
    for (u32 i = 0U; i < 4U; ++i)
    {
        put32(out, 0U);
    }
    u32 at = 32U + n * 12U;
    for (u32 i = 0U; i < n; ++i)
    {
        put32(out, types[i]);
        put32(out, static_cast<u32>(streams[i]->size()));
        put32(out, at);
        at += static_cast<u32>(streams[i]->size());
    }
    for (u32 i = 0U; i < n; ++i)
    {
        for (const u8 b : *streams[i])
        {
            out.push_back(b);
        }
    }
    return out;
}

// A minidump of an exception stream (type 6), a module list (type 4: the game at kExeBase) and an unloaded module
// list (type 14: a plugin at `unloaded_base`); with `code`, a memory list of `list_type` (5 or 9) last, holding those
// bytes at `code_start`.
cont::Array<u8> minidump(u32 code, u64 pc, u32 parameters, u64 access, u64 address, u64 unloaded_base,
                         cont::ConstSpan<u8> code_bytes = {}, u64 code_start = 0U, u32 list_type = 5U)
{
    cont::Array<u8> exception;
    put32(exception, 1U); // thread id
    put32(exception, 0U);
    put32(exception, code);
    put32(exception, 0U); // flags
    put64(exception, 0U); // nested record
    put64(exception, pc);
    put32(exception, parameters);
    put32(exception, 0U);
    put64(exception, access);
    put64(exception, address);
    for (u32 i = 2U; i < 15U; ++i)
    {
        put64(exception, 0U);
    }
    put64(exception, 0U); // thread context location
    cont::Array<u8> modules;
    put32(modules, 1U);
    put64(modules, kExeBase);
    put32(modules, static_cast<u32>(kExeSize));
    for (u32 i = 0U; i < 24U; ++i) // checksum .. Reserved1: 96 more bytes, all zero
    {
        put32(modules, 0U);
    }
    cont::Array<u8> unloaded;
    put32(unloaded, 12U); // header size
    put32(unloaded, 24U); // entry size
    put32(unloaded, 1U);
    put64(unloaded, unloaded_base);
    put32(unloaded, 0x10000U);
    put32(unloaded, 0U);
    put32(unloaded, 0U);
    put32(unloaded, 0U);

    const u32 types[] = {6U, 4U, 14U, list_type};
    const cont::Array<u8>* streams[] = {&exception, &modules, &unloaded, nullptr};
    if (code_bytes.empty())
    {
        return assemble(types, streams, 3U);
    }
    // The memory list's own position: after the header, a four-entry directory and the three streams before it.
    const u32 at = static_cast<u32>(32U + 4U * 12U + exception.size() + modules.size() + unloaded.size());
    cont::Array<u8> memory;
    if (list_type == 5U)
    {
        put32(memory, 1U);
        put64(memory, code_start);
        put32(memory, static_cast<u32>(code_bytes.size()));
        put32(memory, at + 20U);
    }
    else
    {
        put64(memory, 1U);
        put64(memory, at + 32U);
        put64(memory, code_start);
        put64(memory, code_bytes.size());
    }
    for (const u8 b : code_bytes)
    {
        memory.push_back(b);
    }
    streams[3] = &memory;
    return assemble(types, streams, 4U);
}

cp::HardwareIdentity machine(cont::StringView vendor, u32 signature, cont::StringView brand, u32 microcode,
                             bool microcode_known)
{
    cp::HardwareIdentity h;
    h.flags = cp::kHardwareCpuKnown | (microcode_known ? cp::kHardwareMicrocodeKnown : 0U);
    h.cpu_signature = signature;
    h.microcode = microcode;
    h.cpu_vendor.append(vendor);
    h.cpu_model.append(brand);
    return h;
}

cp::HardwareIdentity raptor(u32 microcode)
{
    return machine("GenuineIntel", 0x000B0671U, "Intel(R) Core(TM) i9-14900K", microcode, true);
}

} // namespace

TEST_CASE("fault classification: a crash record's fault, registers and modules read back",
          "[perf][diag][hw-resilience]")
{
    const u64 fault = kLibBase + 0x1234U;
    const cp::CrashEvidence e = read(record(view(signal_line(11U, 1, fault)), kExeBase + 0x10U));
    REQUIRE(e.ok);
    CHECK(e.kind == cp::CrashCaptureKind::Record);
    CHECK(e.code == 11U);
    CHECK(e.signal_code == 1);
    CHECK_FALSE(e.sent);
    CHECK(e.pc_known);
    CHECK(e.pc == kExeBase + 0x10U);
    CHECK(e.address_known);
    CHECK(e.address == fault);
    REQUIRE(e.modules.size() == 2U);
    CHECK(view(e.modules[1].name) == "libcerid.so");

    const cp::CrashEvidence sent = read(record(view(signal_line(6U, -6, 0x3e800000a89ULL)), kExeBase));
    REQUIRE(sent.ok);
    CHECK(sent.sent);
    CHECK(sent.signal_code == -6);
    CHECK_FALSE(sent.address_known); // the {pid, uid} word, never an address

    const cp::CrashEvidence ill = read(record(view(signal_line(4U, 2, kExeBase + 0x40U)), kExeBase + 0x40U));
    REQUIRE(ill.ok);
    CHECK_FALSE(ill.address_known); // SIGILL's si_addr is the instruction, not a data access
}

TEST_CASE("fault classification: damaged and foreign captures are refused, and every cut reads safely",
          "[perf][diag][hw-resilience]")
{
    CHECK_FALSE(read(cont::String{"MZ not a capture"}).ok);
    CHECK(read(cont::String{"MZ not a capture"}).kind == cp::CrashCaptureKind::None);
    for (const char* bad : {"signal 11 code 1\n", "signal x code 1 addr 0x10\n", "signal 11 code 1 addr 10\n",
                            "signal 11 code 1 addr 0xZZ\n", "signal 11 code 99999999999 addr 0x10\n",
                            "signal 11 code 1 addr 0x10000000000000000\n"})
    {
        cont::String text{"crd crash record\npid 1 tid 1\n"};
        text.append(bad);
        const cp::CrashEvidence e = read(text);
        CHECK_FALSE(e.ok);
        CHECK(e.kind == cp::CrashCaptureKind::Record);
        CHECK_FALSE(e.error.empty());
    }
    const cont::String whole = record(view(signal_line(11U, 1, kHeapAddr)), kExeBase);
    for (usize cut = 0U; cut < whole.size(); ++cut)
    {
        const cp::CrashEvidence e = read(cont::String{cont::StringView{whole.data(), cut}});
        CHECK((!e.ok || e.address == kHeapAddr)); // a prefix reads the whole line or nothing of it
    }
    const cont::Array<u8> dump = minidump(0xC0000005U, kExeBase, 2U, 0U, kHeapAddr, 0x20000000ULL);
    REQUIRE(cp::read_crash_evidence({dump.data(), dump.size()}).ok);
    for (usize cut = 0U; cut < dump.size(); ++cut)
    {
        const cp::CrashEvidence e = cp::read_crash_evidence({dump.data(), cut});
        CHECK((!e.ok || e.address == kHeapAddr));
    }
}

TEST_CASE("fault classification: a machine check is likely hardware, and a sent signal is never judged",
          "[perf][diag][hw-resilience]")
{
    const cp::CrashEvidence mce = read(record(view(signal_line(7U, 4, kHeapAddr)), kExeBase));
    const cp::FaultClassification c = cp::classify_fault(&mce, nullptr, cp::ReplayOutcome::NotRun);
    CHECK(c.verdict == cp::HardwareVerdict::Likely);
    CHECK(c.signals == cp::kFaultSignalMachineCheck);
    CHECK(view(cp::fault_message(c, nullptr)) ==
          "This failure is likely a hardware fault: the operating system reported a memory error. Test the memory "
          "and remove any memory overclock (such as XMP or EXPO).");
    const cp::CrashEvidence ao = read(record(view(signal_line(7U, 5, kHeapAddr)), kExeBase));
    CHECK(cp::classify_fault(&ao, nullptr, cp::ReplayOutcome::NotRun).signals == cp::kFaultSignalMachineCheck);
    const cp::CrashEvidence plain_bus = read(record(view(signal_line(7U, 2, kHeapAddr)), kExeBase)); // BUS_ADRERR
    CHECK(cp::classify_fault(&plain_bus, nullptr, cp::ReplayOutcome::NotRun).verdict == cp::HardwareVerdict::None);

    // A raise(SIGSEGV)'s address is the sender's pid and uid, here one bit away from the library: never evidence.
    const u64 near_lib = (kLibBase + 0x40U) ^ (u64{1} << 40U);
    const cp::CrashEvidence sent = read(record(view(signal_line(11U, -6, near_lib)), near_lib));
    const cp::FaultClassification s = cp::classify_fault(&sent, nullptr, cp::ReplayOutcome::NotRun);
    CHECK(s.verdict == cp::HardwareVerdict::None);
    CHECK(s.sent);
    CHECK(s.signals == 0U);
    CHECK(view(cp::fault_message(s, nullptr)) == "Nothing in this failure points to a hardware fault (it ended on a "
                                                 "signal another process or the program itself sent).");
}

TEST_CASE("fault classification: a code or data address one bit from a loaded module is a flip, with its guards",
          "[perf][diag][hw-resilience]")
{
    // Execution jumped to the library's address with bit 44 flipped: the fetch faulted at the pc itself.
    const u64 jump = (kLibBase + 0x5000U) ^ (u64{1} << 44U);
    const cp::CrashEvidence fetch = read(record(view(signal_line(11U, 1, jump)), jump));
    const cp::FaultClassification c = cp::classify_fault(&fetch, nullptr, cp::ReplayOutcome::NotRun);
    CHECK(c.verdict == cp::HardwareVerdict::Possible);
    CHECK(c.signals == cp::kFaultSignalCodeAddressBitFlip);
    REQUIRE(c.code_flip.found);
    CHECK(c.code_flip.recorded == jump);
    CHECK(c.code_flip.restored == kLibBase + 0x5000U);
    CHECK(c.code_flip.bit == 44U);
    CHECK(view(c.code_flip.module) == "libcerid.so");
    CHECK_FALSE(c.data_flip.found);
    CHECK(view(cp::fault_message(c, nullptr)) ==
          "This failure shows one sign of a hardware fault, which alone is not proof: the code address 0x6f1234005000 "
          "is one bit away from 0x7f1234005000 in libcerid.so. If failures like it repeat in different places, test "
          "the machine's memory and processor.");

    // A load from the executable's data with bit 30 flipped, while the pc is in the library.
    const u64 load = (kExeBase + kExeSize - 0x10U) ^ (u64{1} << 30U);
    const cp::CrashEvidence data = read(record(view(signal_line(11U, 2, load)), kLibBase + 0x20U));
    const cp::FaultClassification d = cp::classify_fault(&data, nullptr, cp::ReplayOutcome::NotRun);
    CHECK(d.signals == cp::kFaultSignalDataAddressBitFlip);
    REQUIRE(d.data_flip.found);
    CHECK(d.data_flip.bit == 30U);
    CHECK(view(d.data_flip.module) == "game");

    // Guards: a heap address no bit puts in a module, an address inside a module, and a null pointer plus an offset
    // (0x10 is one bit from 0x10010, which a module here could hold) are never flips.
    for (const u64 address : {kHeapAddr, kLibBase + 0x30U, u64{0x10U}})
    {
        const cp::CrashEvidence e = read(record(view(signal_line(11U, 1, address)), kLibBase + 0x20U));
        CHECK(cp::classify_fault(&e, nullptr, cp::ReplayOutcome::NotRun).signals == 0U);
    }
    cont::String low_module = record(view(signal_line(11U, 1, 0x10U)), kLibBase);
    low_module.append("module 0x10000 0x10000 05 low.so\n");
    const cp::CrashEvidence low = read(low_module);
    REQUIRE(low.modules.size() == 3U);
    CHECK(cp::classify_fault(&low, nullptr, cp::ReplayOutcome::NotRun).signals == 0U);

    // A minidump: an access violation on a data address one bit from the game, and one inside an unloaded plugin
    // one bit from the game too (a dangling pointer into the plugin, not a flip).
    const u64 flipped = (kExeBase + 0x800U) ^ (u64{1} << 36U);
    const cont::Array<u8> dump = minidump(0xC0000005U, kExeBase + 0x100U, 2U, 0U, flipped, 0x20000000ULL);
    const cp::CrashEvidence e = cp::read_crash_evidence({dump.data(), dump.size()});
    REQUIRE(e.ok);
    CHECK(e.kind == cp::CrashCaptureKind::Minidump);
    CHECK(e.pc == kExeBase + 0x100U);
    CHECK(e.address == flipped);
    REQUIRE(e.modules.size() == 2U);
    CHECK(e.modules[1].unloaded);
    CHECK(cp::classify_fault(&e, nullptr, cp::ReplayOutcome::NotRun).signals == cp::kFaultSignalDataAddressBitFlip);
    const cont::Array<u8> dangling = minidump(0xC0000005U, kExeBase + 0x100U, 2U, 0U, flipped, flipped & ~u64{0xFFFF});
    const cp::CrashEvidence de = cp::read_crash_evidence({dangling.data(), dangling.size()});
    CHECK(cp::classify_fault(&de, nullptr, cp::ReplayOutcome::NotRun).signals == 0U);
    // An execute fault at the pc (ExceptionInformation[0] = 8): the code pointer is judged.
    const cont::Array<u8> exec = minidump(0xC0000005U, flipped, 2U, 8U, flipped, 0x20000000ULL);
    const cp::CrashEvidence ee = cp::read_crash_evidence({exec.data(), exec.size()});
    CHECK(cp::classify_fault(&ee, nullptr, cp::ReplayOutcome::NotRun).signals == cp::kFaultSignalCodeAddressBitFlip);
    // A fault that names no data address (a breakpoint) is not judged.
    const cont::Array<u8> trap = minidump(0x80000003U, flipped, 0U, 0U, flipped, 0x20000000ULL);
    const cp::CrashEvidence te = cp::read_crash_evidence({trap.data(), trap.size()});
    REQUIRE(te.ok);
    CHECK_FALSE(te.address_known);
    CHECK(cp::classify_fault(&te, nullptr, cp::ReplayOutcome::NotRun).signals == 0U);
}

TEST_CASE("fault classification: the advisory table lists Raptor Lake desktop parts and reads their microcode",
          "[perf][diag][hw-resilience]")
{
    REQUIRE(cp::cpu_advisories().size() == 1U);
    const cp::CpuAdvisory* listed = cp::cpu_advisories().data();
    CHECK(cp::find_cpu_advisory(raptor(0x133U)) == listed);
    for (const char* brand :
         {"13th Gen Intel(R) Core(TM) i9-13900K", "13th Gen Intel(R) Core(TM) i5-13600KF", "Intel(R) Core(TM) i7-14700",
          "Intel(R) Core(TM) i9-14900KS", "13th Gen Intel(R) Core(TM) i5-13400F"})
    {
        CHECK(cp::find_cpu_advisory(machine("GenuineIntel", 0x000B0671U, brand, 0x12FU, true)) == listed);
    }
    // Mobile (HX), 35 W (T) and 60 W (i3) parts, another signature, another vendor, an unread CPU.
    for (const char* brand :
         {"13th Gen Intel(R) Core(TM) i9-13980HX", "13th Gen Intel(R) Core(TM) i9-13900T",
          "13th Gen Intel(R) Core(TM) i3-13100", "Intel(R) Core(TM) i9-12900K", "Intel(R) Xeon(R) E-2488"})
    {
        CHECK(cp::find_cpu_advisory(machine("GenuineIntel", 0x000B0671U, brand, 0x12FU, true)) == nullptr);
    }
    CHECK(cp::find_cpu_advisory(machine("GenuineIntel", 0x000B06F2U, "Intel(R) Core(TM) i5-14400", 0x3AU, true)) ==
          nullptr);
    CHECK(cp::find_cpu_advisory(machine("AuthenticAMD", 0x000B0671U, "Intel(R) Core(TM) i9-14900K", 0U, true)) ==
          nullptr);
    cp::HardwareIdentity unread = raptor(0x133U);
    unread.flags = 0U;
    CHECK(cp::find_cpu_advisory(unread) == nullptr);

    const u64 jump = (kLibBase + 0x5000U) ^ (u64{1} << 44U);
    const cp::CrashEvidence flip = read(record(view(signal_line(11U, 1, jump)), jump));
    const cp::HardwareIdentity before = raptor(0x12EU);
    const cp::FaultClassification b = cp::classify_fault(&flip, &before, cp::ReplayOutcome::NotRun);
    CHECK(b.verdict == cp::HardwareVerdict::Likely);
    CHECK(b.microcode == cp::MicrocodeState::BeforeFix);
    CHECK(view(cp::fault_message(b, &before))
              .ends_with("This processor (Intel(R) Core(TM) i9-14900K) has a known instability, Vmin Shift Instability "
                         "of Intel Core "
                         "13th and 14th Gen desktop processors, and its microcode 0x12e is older than the fix 0x12f: "
                         "update the BIOS "
                         "from the board's maker, which installs it."));
    const cp::HardwareIdentity fixed = raptor(0x133U);
    const cp::FaultClassification f = cp::classify_fault(&flip, &fixed, cp::ReplayOutcome::NotRun);
    CHECK(f.verdict == cp::HardwareVerdict::Likely);
    CHECK(f.microcode == cp::MicrocodeState::Fixed);
    CHECK(view(cp::fault_message(f, &fixed)).find("about a replacement under warranty") != cont::StringView::npos);
    cp::HardwareIdentity hidden = raptor(0xFFFFFFFFU);
    hidden.flags = cp::kHardwareCpuKnown;
    const cp::FaultClassification h = cp::classify_fault(&flip, &hidden, cp::ReplayOutcome::NotRun);
    CHECK(h.microcode == cp::MicrocodeState::Unknown);
    CHECK(view(cp::fault_message(h, &hidden)).find("fixed by microcode 0x12f or later") != cont::StringView::npos);

    // A listed processor without a signature is no verdict; old microcode is still worth saying.
    const cp::CrashEvidence clean = read(record(view(signal_line(11U, 1, kHeapAddr)), kExeBase));
    const cp::FaultClassification n = cp::classify_fault(&clean, &before, cp::ReplayOutcome::NotRun);
    CHECK(n.verdict == cp::HardwareVerdict::None);
    CHECK(view(cp::fault_message(n, &before))
              .starts_with("Nothing in this failure points to a hardware fault, but "
                           "this processor"));
    CHECK(view(cp::fault_message(cp::classify_fault(&clean, &fixed, cp::ReplayOutcome::NotRun), &fixed)) ==
          "Nothing in this failure points to a hardware fault.");
}

TEST_CASE("fault classification: a replay that does not reproduce is a signature, and two are likely",
          "[perf][diag][hw-resilience]")
{
    cp::ReplayOutcome r = cp::ReplayOutcome::NotRun;
    CHECK(cp::parse_replay_outcome("not-reproduced", r));
    CHECK(r == cp::ReplayOutcome::NotReproduced);
    CHECK_FALSE(cp::parse_replay_outcome("maybe", r));
    CHECK(r == cp::ReplayOutcome::NotReproduced);

    const cp::FaultClassification alone = cp::classify_fault(nullptr, nullptr, cp::ReplayOutcome::NotReproduced);
    CHECK(alone.verdict == cp::HardwareVerdict::Possible);
    CHECK_FALSE(alone.crash);
    CHECK(cp::classify_fault(nullptr, nullptr, cp::ReplayOutcome::Reproduced).verdict == cp::HardwareVerdict::None);
    const u64 jump = (kLibBase + 0x5000U) ^ (u64{1} << 44U);
    const cp::CrashEvidence flip = read(record(view(signal_line(11U, 1, jump)), jump));
    const cp::FaultClassification both = cp::classify_fault(&flip, nullptr, cp::ReplayOutcome::NotReproduced);
    CHECK(both.verdict == cp::HardwareVerdict::Likely);
    cont::String names;
    cp::append_fault_signal_names(both.signals, names);
    CHECK(view(names) == "code-address-bit-flip,replay-not-reproduced");
    CHECK(view(cp::fault_message(both, nullptr))
              .ends_with("; replaying it on the same build did not reproduce it. Test the machine's memory and "
                         "processor, and check "
                         "that the BIOS is current."));
}

TEST_CASE("fault classification: bundle.inspect judges a crash capture and a bundle that carries one",
          "[perf][diag][hw-resilience]")
{
    const u64 jump = (kLibBase + 0x5000U) ^ (u64{1} << 44U);
    cont::String capture = record(view(signal_line(11U, 1, jump)), jump);
    char path[512];
    (void)std::snprintf(path, sizeof(path), "%s/fault_classification_record.log", CRD_DIAG_COMMANDS_SCRATCH);
    REQUIRE(cp::write_bundle_atomic(path,
                                    cont::ConstSpan<u8>{reinterpret_cast<const u8*>(capture.data()), capture.size()}));
    cp::BundleWriter writer;
    REQUIRE(writer.add_section(cp::BundleSectionTag::CrashRecord,
                               cont::ConstSpan<u8>{reinterpret_cast<const u8*>(capture.data()), capture.size()}) ==
            cp::BundleWriter::AddStatus::Ok);
    const cont::Array<u8> bundle = writer.finish();
    char bundle_path[512];
    (void)std::snprintf(bundle_path, sizeof(bundle_path), "%s/fault_classification.cdb", CRD_DIAG_COMMANDS_SCRATCH);
    REQUIRE(cp::write_bundle_atomic(bundle_path, cont::ConstSpan<u8>{bundle.data(), bundle.size()}));

    cp::DiagServiceConfig config;
    config.root = cont::StringView{CRD_DIAG_COMMANDS_SCRATCH};
    cp::DiagCommandService svc{cp::authority_bit(cp::DiagAuthority::Read), config, crd::memory::default_allocator()};
    cp::DiagRequest req;
    req.command = "bundle.inspect";
    req.page_items = 64U;
    const cp::DiagArg not_reproduced[] = {{"replay", "not-reproduced"}};
    for (const char* file : {"fault_classification_record.log", "fault_classification.cdb"})
    {
        req.path = file;
        req.args = {};
        const cp::DiagResult result = svc.execute(req);
        REQUIRE(result.status == cp::DiagStatus::Ok);
        CHECK(std::strstr(result.json.c_str(), R"("kind":"crash","capture":"record","read":true)") != nullptr);
        CHECK(std::strstr(result.json.c_str(),
                          R"("kind":"classification","verdict":"possible","signals":"code-address-bit-flip")") !=
              nullptr);
        CHECK(std::strstr(result.json.c_str(), R"("code_bit":44,"code_module":"libcerid.so")") != nullptr);
        req.args = {not_reproduced, 1U};
        const cp::DiagResult replayed = svc.execute(req);
        REQUIRE(replayed.status == cp::DiagStatus::Ok);
        CHECK(std::strstr(replayed.json.c_str(), R"("verdict":"likely","signals":"code-address-bit-flip,replay-not-)"
                                                 R"(reproduced")") != nullptr);
    }
    const cp::DiagArg bad[] = {{"replay", "sometimes"}};
    req.args = {bad, 1U};
    CHECK(svc.execute(req).status == cp::DiagStatus::BadArgument);
    (void)std::remove(path);
    (void)std::remove(bundle_path);
}

namespace
{

using crd::test::module_file::image_reader;

void put_at(cont::Array<u8>& out, usize at, u64 v, u32 width)
{
    for (u32 i = 0U; i < width; ++i)
    {
        out[at + i] = static_cast<u8>(v >> (8U * i));
    }
}

// A PE32+ image: .text at RVA 0x1000 (file 0x400, 0x200 bytes of int3) holding `mov rax, imm64` at +0x10 whose
// immediate (the image base + 0x1100) a DIR64 base relocation in .reloc (RVA 0x2000) names, then `ret`.
constexpr u64 kPeImageBase = 0x00000001'40000000ULL;
constexpr u64 kPeBase = 0x00007ff6'12340000ULL;
constexpr u32 kPeStamp = 0x6543210FU;
constexpr u32 kPeSize = 0x3000U;
constexpr u32 kRelocDir64 = 10U;

cont::Array<u8> pe_image(u32 relocation_type)
{
    cont::Array<u8> img;
    img.resize(0x800U);
    std::memset(img.data(), 0, img.size());
    img[0] = 'M';
    img[1] = 'Z';
    put_at(img, 0x3CU, 0x40U, 4U);
    img[0x40] = 'P';
    img[0x41] = 'E';
    put_at(img, 0x44U, 0x8664U, 2U); // machine
    put_at(img, 0x46U, 2U, 2U);      // sections
    put_at(img, 0x48U, kPeStamp, 4U);
    put_at(img, 0x54U, 240U, 2U); // optional header size
    const usize opt = 0x58U;
    put_at(img, opt, 0x20BU, 2U);
    put_at(img, opt + 24U, kPeImageBase, 8U);
    put_at(img, opt + 56U, kPeSize, 4U);
    put_at(img, opt + 108U, 16U, 4U);
    put_at(img, opt + 112U + 8U * 5U, 0x2000U, 4U); // the base relocation directory
    put_at(img, opt + 112U + 8U * 5U + 4U, 12U, 4U);
    const usize sections = opt + 240U;
    put_at(img, sections + 8U, 0x200U, 4U); // .text: virtual size, address, raw size, raw offset
    put_at(img, sections + 12U, 0x1000U, 4U);
    put_at(img, sections + 16U, 0x200U, 4U);
    put_at(img, sections + 20U, 0x400U, 4U);
    put_at(img, sections + 40U + 8U, 12U, 4U); // .reloc
    put_at(img, sections + 40U + 12U, 0x2000U, 4U);
    put_at(img, sections + 40U + 16U, 0x200U, 4U);
    put_at(img, sections + 40U + 20U, 0x600U, 4U);
    std::memset(img.data() + 0x400U, 0xCC, 0x200U);
    img[0x410] = 0x48U;
    img[0x411] = 0xB8U;
    put_at(img, 0x412U, kPeImageBase + 0x1100U, 8U);
    img[0x41A] = 0xC3U;
    put_at(img, 0x600U, 0x1000U, 4U); // one block: page 0x1000, 12 bytes, the entry and a pad
    put_at(img, 0x604U, 12U, 4U);
    put_at(img, 0x608U, (relocation_type << 12U) | 0x012U, 2U);
    return img;
}

cp::ModuleIdentity module(u64 base, u64 size, cp::SymbolIdKind kind, cont::StringView name)
{
    cp::ModuleIdentity m;
    m.base = base;
    m.size = size;
    m.id_kind = kind;
    m.name.append(name);
    return m;
}

// A capture of a fault at `pc` in `m` holding `code`.
cp::CrashEvidence at_pc(cp::ModuleIdentity m, u64 pc, const u8* code, u32 n)
{
    cp::CrashEvidence e;
    e.ok = true;
    e.kind = cp::CrashCaptureKind::Minidump;
    e.pc_known = true;
    e.pc = pc;
    e.modules.push_back(static_cast<cp::ModuleIdentity&&>(m));
    e.code_size = n;
    std::memcpy(e.code_bytes, code, n);
    return e;
}

cp::CrashEvidence pe_capture(const u8* code, u32 n = crd::crash::kCodeWindowBytes)
{
    cp::ModuleIdentity m = module(kPeBase, kPeSize, cp::SymbolIdKind::PeImage, "game.exe");
    m.timestamp = kPeStamp;
    return at_pc(static_cast<cp::ModuleIdentity&&>(m), kPeBase + 0x1010U, code, n);
}

// The 16 bytes at RVA 0x1010 as the loader leaves them at kPeBase.
void pe_loaded(u8 (&out)[16])
{
    std::memset(out, 0xCC, sizeof(out));
    out[0] = 0x48U;
    out[1] = 0xB8U;
    const u64 imm = kPeBase + 0x1100U;
    for (u32 i = 0U; i < 8U; ++i)
    {
        out[2U + i] = static_cast<u8>(imm >> (8U * i));
    }
    out[10] = 0xC3U;
}

// A 64-bit ELF file: one PT_LOAD (file 0 to 0x1100, memory to `span`), a PT_NOTE with the GNU build-id `id` and a
// PT_DYNAMIC whose DT_FLAGS carries DF_TEXTREL when `textrel`. The code at 0x1010 is `mov rax, [rdi]; ret`, then
// zeros.
cont::Array<u8> elf_image(cont::ConstSpan<u8> id, u64 span, bool textrel)
{
    cont::Array<u8> img;
    img.resize(0x1100U);
    std::memset(img.data(), 0, img.size());
    const u8 ident[] = {0x7FU, 'E', 'L', 'F', 2U, 1U, 1U};
    std::memcpy(img.data(), ident, sizeof(ident));
    put_at(img, 16U, 3U, 2U);  // ET_DYN
    put_at(img, 18U, 62U, 2U); // EM_X86_64
    put_at(img, 32U, 64U, 8U); // e_phoff
    put_at(img, 52U, 64U, 2U);
    put_at(img, 54U, 56U, 2U);
    put_at(img, 56U, 3U, 2U);
    const u64 load[] = {1U, 0U, 0U, 0x1100U, span};    // type, offset, vaddr, filesz, memsz
    const u64 note[] = {4U, 0x100U, 0x100U, 36U, 36U}; // the build-id note
    const u64 dyn[] = {2U, 0x140U, 0x140U, 32U, 32U};  // DT_FLAGS, DT_NULL
    const u64* headers[] = {load, note, dyn};
    for (u32 i = 0U; i < 3U; ++i)
    {
        const usize at = 64U + 56U * i;
        put_at(img, at, headers[i][0], 4U);
        put_at(img, at + 8U, headers[i][1], 8U);
        put_at(img, at + 16U, headers[i][2], 8U);
        put_at(img, at + 32U, headers[i][3], 8U);
        put_at(img, at + 40U, headers[i][4], 8U);
    }
    put_at(img, 0x100U, 4U, 4U);
    put_at(img, 0x104U, id.size(), 4U);
    put_at(img, 0x108U, 3U, 4U); // NT_GNU_BUILD_ID
    std::memcpy(img.data() + 0x10CU, "GNU", 4U);
    std::memcpy(img.data() + 0x110U, id.data(), id.size());
    put_at(img, 0x140U, 30U, 8U);
    put_at(img, 0x148U, textrel ? 4U : 0U, 8U);
    const u8 code[] = {0x48U, 0x8BU, 0x07U, 0xC3U};
    std::memcpy(img.data() + 0x1010U, code, sizeof(code));
    return img;
}

constexpr u8 kElfId[] = {0x5AU, 0x11U, 0xC0U, 0xDEU, 0x01U, 0x02U, 0x03U, 0x04U, 0x05U, 0x06U,
                         0x07U, 0x08U, 0x09U, 0x0AU, 0x0BU, 0x0CU, 0x0DU, 0x0EU, 0x0FU, 0x10U};

cp::CrashEvidence elf_capture(cont::ConstSpan<u8> id, u64 pc_offset, const u8* code, u32 n)
{
    cp::ModuleIdentity m = module(kLibBase, 0x2000U, cp::SymbolIdKind::GnuBuildId, "libcerid.so");
    m.id_len = static_cast<u8>(id.size());
    std::memcpy(m.id, id.data(), id.size());
    cp::CrashEvidence e = at_pc(static_cast<cp::ModuleIdentity&&>(m), kLibBase + pc_offset, code, n);
    e.kind = cp::CrashCaptureKind::Record;
    return e;
}

} // namespace

TEST_CASE("fault classification: the faulting instruction's bytes read back from a dump's memory lists and a record",
          "[perf][diag][hw-resilience]")
{
    const u8 window[] = {0x90U, 0x48U, 0x8BU, 0x07U, 0xC3U, 0xCCU, 0xCCU, 0xCCU, 0xCCU, 0xCCU,
                         0xCCU, 0xCCU, 0xCCU, 0xCCU, 0xCCU, 0xCCU, 0xCCU, 0xCCU, 0xCCU, 0xCCU};
    const u64 pc = kExeBase + 0x101U; // one byte into the range: the 16 from it, of the 20 the range holds
    for (const u32 list_type : {5U, 9U})
    {
        const cont::Array<u8> dump = minidump(0xC0000005U, pc, 2U, 0U, kHeapAddr, 0x20000000ULL,
                                              cont::ConstSpan<u8>{window, sizeof(window)}, pc - 1U, list_type);
        const cp::CrashEvidence e = cp::read_crash_evidence({dump.data(), dump.size()});
        REQUIRE(e.ok);
        REQUIRE(e.code_size == crd::crash::kCodeWindowBytes);
        CHECK(std::memcmp(e.code_bytes, window + 1U, e.code_size) == 0);
        for (usize cut = 0U; cut < dump.size(); ++cut)
        {
            const cp::CrashEvidence part = cp::read_crash_evidence({dump.data(), cut});
            CHECK((part.code_size == 0U || std::memcmp(part.code_bytes, window + 1U, part.code_size) == 0));
        }
    }
    // A range that ends three bytes after the pc gives three; one that does not hold the pc gives none.
    const cont::Array<u8> tail =
        minidump(0xC0000005U, pc, 2U, 0U, kHeapAddr, 0x20000000ULL, cont::ConstSpan<u8>{window, 4U}, pc - 1U);
    CHECK(cp::read_crash_evidence({tail.data(), tail.size()}).code_size == 3U);
    const cont::Array<u8> elsewhere = minidump(0xC0000005U, pc, 2U, 0U, kHeapAddr, 0x20000000ULL,
                                               cont::ConstSpan<u8>{window, sizeof(window)}, pc + 1U);
    CHECK(cp::read_crash_evidence({elsewhere.data(), elsewhere.size()}).code_size == 0U);

    // The record's code line, after its registers: read for its own pc only, and only when well formed.
    const auto with_code = [](u64 rip, const char* line)
    {
        cont::String text = record(view(signal_line(11U, 1, kHeapAddr)), rip);
        const usize at = view(text).find("build_id ");
        cont::String out{cont::StringView{text.data(), at}};
        out.append(line);
        out.append(cont::StringView{text.data() + at, text.size() - at});
        return read(out);
    };
    const cp::CrashEvidence r = with_code(kExeBase + 0x10U, "code 0x555555554010 488b07c3\n");
    REQUIRE(r.ok);
    CHECK(r.pc == kExeBase + 0x10U); // the registers still end before the code line
    REQUIRE(r.code_size == 4U);
    CHECK(r.code_bytes[0] == 0x48U);
    CHECK(r.code_bytes[3] == 0xC3U);
    for (const char* bad :
         {"code 0x555555554011 488b07c3\n", "code 0x555555554010 488b07c\n", "code 0x555555554010 488B07C3\n",
          "code 0x555555554010 \n", "code 0x555555554010 000102030405060708090a0b0c0d0e0f10\n"})
    {
        const cp::CrashEvidence b = with_code(kExeBase + 0x10U, bad);
        REQUIRE(b.ok);
        CHECK(b.code_size == 0U);
    }
}

TEST_CASE("fault classification: the code matches its PE image through the base relocations, and one flipped bit "
          "is the signature",
          "[perf][diag][hw-resilience]")
{
    const cont::Array<u8> image = pe_image(kRelocDir64);
    u8 loaded[16];
    pe_loaded(loaded);
    const cp::CodeImageCheck same = cp::compare_code_with_image(pe_capture(loaded), image_reader(image));
    INFO("reason: " << cont::String{same.reason}.c_str());
    CHECK(same.result == cp::CodeImageResult::Matches);
    CHECK(same.compared == 16U);
    CHECK(view(same.module) == "game.exe");
    // A build with no PDB: its dump's module has no CodeView record, and its PE identity still decides.
    cp::CrashEvidence no_pdb = pe_capture(loaded);
    no_pdb.modules[0].id_kind = cp::SymbolIdKind::None;
    const cp::CodeImageCheck bare = cp::compare_code_with_image(no_pdb, image_reader(image));
    INFO("reason: " << cont::String{bare.reason}.c_str());
    CHECK(bare.result == cp::CodeImageResult::Matches);
    CHECK(bare.compared == 16U);
    no_pdb.modules[0].timestamp = kPeStamp + 1U;
    CHECK(cp::compare_code_with_image(no_pdb, image_reader(image)).result == cp::CodeImageResult::NotCompared);
    no_pdb.modules[0].id_kind = cp::SymbolIdKind::GnuBuildId; // a build-id names an ELF module, never this image
    no_pdb.modules[0].timestamp = kPeStamp;
    CHECK(cp::compare_code_with_image(no_pdb, image_reader(image)).result == cp::CodeImageResult::NotCompared);

    u8 flipped[16];
    std::memcpy(flipped, loaded, sizeof(flipped));
    flipped[5] ^= 0x20U; // inside the relocated immediate
    const cp::CodeImageCheck one = cp::compare_code_with_image(pe_capture(flipped), image_reader(image));
    CHECK(one.result == cp::CodeImageResult::OneBitFlipped);
    CHECK(one.offset == 5U);
    CHECK(one.bit == 5U);
    CHECK(one.recorded == flipped[5]);
    CHECK(one.image == loaded[5]);
    flipped[0] = 0xE9U; // a hook's jump over the first byte as well: patched code, never judged
    CHECK(cp::compare_code_with_image(pe_capture(flipped), image_reader(image)).result ==
          cp::CodeImageResult::Modified);

    // Without the relocation the immediate holds the preferred base's value, so the same bytes differ widely.
    cont::Array<u8> unrelocated = pe_image(0U);
    CHECK(cp::compare_code_with_image(pe_capture(loaded), image_reader(unrelocated)).result ==
          cp::CodeImageResult::Modified);
    // A relocation type this reader does not apply, reaching the window, is never compared.
    const cp::CodeImageCheck odd = cp::compare_code_with_image(pe_capture(loaded), image_reader(pe_image(7U)));
    CHECK(odd.result == cp::CodeImageResult::NotCompared);
    CHECK_FALSE(odd.reason.empty());

    // Another build (its TimeDateStamp), a capture without code, a pc outside every module and a foreign file.
    cont::Array<u8> other = pe_image(kRelocDir64);
    put_at(other, 0x48U, kPeStamp + 1U, 4U);
    CHECK(cp::compare_code_with_image(pe_capture(loaded), image_reader(other)).result ==
          cp::CodeImageResult::NotCompared);
    CHECK(cp::compare_code_with_image(pe_capture(loaded, 0U), image_reader(image)).result ==
          cp::CodeImageResult::NotCompared);
    cp::CrashEvidence outside = pe_capture(loaded);
    outside.pc = kPeBase + kPeSize;
    CHECK(cp::compare_code_with_image(outside, image_reader(image)).result == cp::CodeImageResult::NotCompared);
    const cont::Array<u8> text = elf_image({kElfId, sizeof(kElfId)}, 0x2000U, false);
    CHECK(cp::compare_code_with_image(pe_capture(loaded), image_reader(text)).result ==
          cp::CodeImageResult::NotCompared);
    // Every cut of the image is read safely and never yields a verdict it cannot back.
    for (usize cut = 0U; cut < image.size(); cut += 7U)
    {
        cont::Array<u8> part;
        for (usize i = 0U; i < cut; ++i)
        {
            part.push_back(image[i]);
        }
        const cp::CodeImageResult got = cp::compare_code_with_image(pe_capture(loaded), image_reader(part)).result;
        CHECK((got == cp::CodeImageResult::NotCompared || got == cp::CodeImageResult::Matches));
    }
}

TEST_CASE("fault classification: the code matches its ELF image by build-id, and text relocations are not compared",
          "[perf][diag][hw-resilience]")
{
    const cont::ConstSpan<u8> id{kElfId, sizeof(kElfId)};
    const cont::Array<u8> image = elf_image(id, 0x2000U, false);
    u8 code[16] = {0x48U, 0x8BU, 0x07U, 0xC3U};
    const cp::CodeImageCheck same =
        cp::compare_code_with_image(elf_capture(id, 0x1010U, code, 16U), image_reader(image));
    INFO("reason: " << cont::String{same.reason}.c_str());
    CHECK(same.result == cp::CodeImageResult::Matches);
    CHECK(same.compared == 16U);
    code[2] ^= 0x01U;
    const cp::CodeImageCheck one =
        cp::compare_code_with_image(elf_capture(id, 0x1010U, code, 16U), image_reader(image));
    CHECK(one.result == cp::CodeImageResult::OneBitFlipped);
    CHECK(one.offset == 2U);
    CHECK(one.bit == 0U);
    code[2] ^= 0x01U;
    // Only the file's bytes are compared: four from 0x10FC, none in the zero-filled memory after the file.
    CHECK(cp::compare_code_with_image(elf_capture(id, 0x10FCU, code, 16U), image_reader(image)).compared == 4U);
    CHECK(cp::compare_code_with_image(elf_capture(id, 0x1200U, code, 16U), image_reader(image)).result ==
          cp::CodeImageResult::NotCompared);

    u8 other_id[sizeof(kElfId)];
    std::memcpy(other_id, kElfId, sizeof(kElfId));
    other_id[19] ^= 0xFFU;
    CHECK(
        cp::compare_code_with_image(elf_capture({other_id, sizeof(other_id)}, 0x1010U, code, 16U), image_reader(image))
            .result == cp::CodeImageResult::NotCompared);
    CHECK(cp::compare_code_with_image(elf_capture(id, 0x1010U, code, 16U), image_reader(elf_image(id, 0x3000U, false)))
              .result == cp::CodeImageResult::NotCompared); // another PT_LOAD span
    const cp::CodeImageCheck textrel =
        cp::compare_code_with_image(elf_capture(id, 0x1010U, code, 16U), image_reader(elf_image(id, 0x2000U, true)));
    CHECK(textrel.result == cp::CodeImageResult::NotCompared);
    CHECK(cont::StringView{textrel.reason}.find("text relocations") != cont::StringView::npos);
    for (usize cut = 0U; cut < 0x200U; ++cut)
    {
        cont::Array<u8> part;
        for (usize i = 0U; i < cut; ++i)
        {
            part.push_back(image[i]);
        }
        CHECK(cp::compare_code_with_image(elf_capture(id, 0x1010U, code, 16U), image_reader(part)).result ==
              cp::CodeImageResult::NotCompared);
    }
}

TEST_CASE("fault classification: a one-bit code difference is a signature with its sentence, and a patch is none",
          "[perf][diag][hw-resilience]")
{
    const cont::Array<u8> image = pe_image(kRelocDir64);
    u8 loaded[16];
    pe_loaded(loaded);
    u8 flipped[16];
    std::memcpy(flipped, loaded, sizeof(flipped));
    flipped[1] ^= 0x04U;
    const cp::CrashEvidence e = pe_capture(flipped);
    const cp::CodeImageCheck one = cp::compare_code_with_image(e, image_reader(image));
    const cp::FaultClassification c = cp::classify_fault(&e, nullptr, cp::ReplayOutcome::NotRun, &one);
    CHECK(c.signals == cp::kFaultSignalCodeBitFlip);
    CHECK(c.verdict == cp::HardwareVerdict::Possible);
    cont::String names;
    cp::append_fault_signal_names(c.signals, names);
    CHECK(view(names) == "code-bit-flip");
    CHECK(view(cp::fault_message(c, nullptr)) ==
          "This failure shows one sign of a hardware fault, which alone is not proof: the instruction byte at "
          "0x7ff612341011 reads 0xbc where game.exe holds 0xb8, one bit apart. If failures like it repeat in different "
          "places, test the machine's memory and processor.");
    const cp::HardwareIdentity before = raptor(0x12EU);
    const cp::FaultClassification raptor_flip = cp::classify_fault(&e, &before, cp::ReplayOutcome::NotRun, &one);
    CHECK(raptor_flip.verdict == cp::HardwareVerdict::Likely);

    flipped[0] = 0xE9U;
    const cp::CrashEvidence p = pe_capture(flipped);
    const cp::CodeImageCheck patched = cp::compare_code_with_image(p, image_reader(image));
    CHECK(cp::classify_fault(&p, nullptr, cp::ReplayOutcome::NotRun, &patched).signals == 0U);
    const cp::CodeImageCheck same = cp::compare_code_with_image(pe_capture(loaded), image_reader(image));
    CHECK(cp::classify_fault(&e, nullptr, cp::ReplayOutcome::NotRun, &same).signals == 0U);
}

TEST_CASE("fault classification: image files are found by identity, and bundle.inspect compares a capture's code",
          "[perf][diag][hw-resilience]")
{
    cp::ModuleIdentity pe = module(kPeBase, kPeSize, cp::SymbolIdKind::Rsds, "C:\\game\\game.exe");
    pe.timestamp = kPeStamp;
    CHECK(view(cp::image_file_path(pe, "store")) == "store/game.exe/6543210F3000/game.exe");
    pe.id_kind = cp::SymbolIdKind::None; // no CodeView record: the same binary key
    CHECK(view(cp::image_file_path(pe, "store")) == "store/game.exe/6543210F3000/game.exe");
    pe.timestamp = 0U;
    CHECK(cp::image_file_path(pe, "store").empty());
    cp::ModuleIdentity elf = module(kLibBase, kLibSize, cp::SymbolIdKind::GnuBuildId, "libcerid.so");
    elf.id_len = 3U;
    elf.id[0] = 0xABU;
    elf.id[1] = 0x01U;
    elf.id[2] = 0xFFU;
    CHECK(view(cp::image_file_path(elf, "store")) == "store/.build-id/ab/01ff");
    CHECK(cp::image_file_path(module(0U, 0U, cp::SymbolIdKind::None, "x"), "store").empty());

    // A record whose library code differs from its image in one bit, the image in the root's store.
    const u8 id[] = {0x03U, 0x04U};
    const cont::Array<u8> image = elf_image({id, sizeof(id)}, kLibSize, false);
    char dir[512];
    (void)std::snprintf(dir, sizeof(dir), "%s/images/.build-id/03", CRD_DIAG_COMMANDS_SCRATCH);
    REQUIRE(crd::platform::fs::create_directories(crd::platform::fs::Path{cont::StringView{dir}}));
    char image_path[600];
    (void)std::snprintf(image_path, sizeof(image_path), "%s/04", dir);
    REQUIRE(cp::write_bundle_atomic(image_path, cont::ConstSpan<u8>{image.data(), image.size()}));
    const u64 pc = kLibBase + 0x1010U;
    cont::String text = record(view(signal_line(11U, 1, kHeapAddr)), pc);
    const usize at = view(text).find("build_id ");
    cont::String capture{cont::StringView{text.data(), at}};
    capture.append("code ");
    append_hex(capture, pc);
    capture.append(" 488b03c3\n"); // 0x07 read as 0x03
    capture.append(cont::StringView{text.data() + at, text.size() - at});
    char path[512];
    (void)std::snprintf(path, sizeof(path), "%s/fault_classification_code.log", CRD_DIAG_COMMANDS_SCRATCH);
    REQUIRE(cp::write_bundle_atomic(path,
                                    cont::ConstSpan<u8>{reinterpret_cast<const u8*>(capture.data()), capture.size()}));

    cp::DiagServiceConfig config;
    config.root = cont::StringView{CRD_DIAG_COMMANDS_SCRATCH};
    cp::DiagCommandService svc{cp::authority_bit(cp::DiagAuthority::Read), config, crd::memory::default_allocator()};
    cp::DiagRequest req;
    req.command = "bundle.inspect";
    req.page_items = 64U;
    req.path = "fault_classification_code.log";
    const cp::DiagResult result = svc.execute(req);
    REQUIRE(result.status == cp::DiagStatus::Ok);
    INFO(result.json.c_str());
    CHECK(std::strstr(result.json.c_str(), R"("code_bytes":4)") != nullptr);
    CHECK(std::strstr(result.json.c_str(), R"("verdict":"possible","signals":"code-bit-flip")") != nullptr);
    CHECK(std::strstr(result.json.c_str(), R"("code_image":"one-bit-flipped","code_image_reason":"","code_compared":4,)"
                                           R"("code_differing_bits":1,"code_offset":2,"code_recorded":3,)"
                                           R"("code_image_byte":7)") != nullptr);
    (void)std::remove(image_path);
    // Without the image in the store the code is not compared, and the reason says so.
    const cp::DiagResult missing = svc.execute(req);
    REQUIRE(missing.status == cp::DiagStatus::Ok);
    CHECK(std::strstr(missing.json.c_str(), R"("code_image":"not-compared","code_image_reason":"the module's image is )"
                                            R"(not in the image store")") != nullptr);
    CHECK(std::strstr(missing.json.c_str(), R"("verdict":"none")") != nullptr);
    (void)std::remove(path);
}

namespace
{

// A crash record with every register crd::crash writes for x86-64: rdi and the page fault's trap number and error
// code as given, `code` (hex, or empty) on its code line; its modules are those of record().
cont::String full_record(cont::StringView signal, u64 rip, u64 rdi, u32 trapno, u64 err, const char* code)
{
    cont::String out;
    out.append("crd crash record\npid 7 tid 7\n");
    out.append(signal);
    out.append("\nexe /opt/game/game\nregs x86_64\nr8 0x8 r9 0x9 r10 0xa r11 0xb\nr12 0xc r13 0xd r14 0xe r15 0xf\n"
               "rdi ");
    append_hex(out, rdi);
    out.append(" rsi 0x6000 rbp 0x5000 rbx 0x4000\nrdx 0x0 rax 0x1000 rcx 0x2000 rsp 0x7ff000\nrip ");
    append_hex(out, rip);
    out.append(" efl 0x246 csgsfs 0x33 err ");
    append_hex(out, err);
    out.append("\ntrapno ");
    append_hex(out, trapno);
    out.append(" oldmask 0x0 cr2 0x0\n\n");
    if (code[0] != '\0')
    {
        out.append("code ");
        append_hex(out, rip);
        out.push_back(' ');
        out.append(code);
        out.push_back('\n');
    }
    out.append("build_id 0102\nmodules 2\nmodule ");
    append_hex(out, kExeBase);
    out.push_back(' ');
    append_hex(out, kExeSize);
    out.append(" 0102 (exe)\nmodule ");
    append_hex(out, kLibBase);
    out.push_back(' ');
    append_hex(out, kLibSize);
    out.append(" 0304 libcerid.so\n");
    return out;
}

// A comparison that matched over the whole window.
cp::CodeImageCheck matching()
{
    cp::CodeImageCheck k;
    k.result = cp::CodeImageResult::Matches;
    k.compared = crd::crash::kCodeWindowBytes;
    return k;
}

// A minidump of an exception stream with a thread context and a system information stream naming `architecture`.
cont::Array<u8> context_dump(u32 code, u64 pc, u64 access, u64 address, crd::u16 architecture,
                             const cont::Array<u8>& context)
{
    cont::Array<u8> exception;
    put32(exception, 1U);
    put32(exception, 0U);
    put32(exception, code);
    put32(exception, 0U);
    put64(exception, 0U);
    put64(exception, pc);
    put32(exception, 2U);
    put32(exception, 0U);
    put64(exception, access);
    put64(exception, address);
    for (u32 i = 2U; i < 15U; ++i)
    {
        put64(exception, 0U);
    }
    cont::Array<u8> system;
    system.push_back(static_cast<u8>(architecture));
    system.push_back(static_cast<u8>(architecture >> 8U));
    for (u32 i = 2U; i < 56U; ++i)
    {
        system.push_back(0U);
    }
    // The context follows the header, a three-entry directory and both streams (the exception's 8 more bytes).
    const u32 at = static_cast<u32>(32U + 3U * 12U + exception.size() + 8U + system.size());
    put32(exception, static_cast<u32>(context.size()));
    put32(exception, at);
    const u32 types[] = {6U, 7U, 0xFFFFU};
    const cont::Array<u8>* streams[] = {&exception, &system, &context};
    return assemble(types, streams, 3U);
}

// An AMD64 CONTEXT: rax to r15 at 0x1000 times their number plus one, `rcx` and `rip` as given.
cont::Array<u8> amd64_context(u32 flags, u64 rip, u64 rcx)
{
    cont::Array<u8> c;
    c.resize(0x4D0U);
    std::memset(c.data(), 0, c.size());
    put_at(c, 0x30U, flags, 4U);
    put_at(c, 0x44U, 0x246U, 4U);
    for (u32 i = 0U; i < 16U; ++i)
    {
        put_at(c, 0x78U + 8U * i, 0x1000U * (i + 1U), 8U);
    }
    put_at(c, 0x80U, rcx, 8U);
    put_at(c, 0xF8U, rip, 8U);
    return c;
}

cp::CrashEvidence read_dump(const cont::Array<u8>& dump)
{
    return cp::read_crash_evidence({dump.data(), dump.size()});
}

constexpr u32 kAmd64Full = 0x0010001FU;

} // namespace

TEST_CASE("fault classification: a minidump's thread context and a record's registers read back",
          "[perf][diag][hw-resilience]")
{
    const u64 pc = kExeBase + 0x10U;
    const cont::Array<u8> dump = context_dump(0xC0000005U, pc, 1U, 0x2000U, 9U, amd64_context(kAmd64Full, pc, 0x2000U));
    const cp::CrashEvidence e = read_dump(dump);
    REQUIRE(e.ok);
    CHECK(e.instruction_set == cp::InstructionSet::X64);
    CHECK(e.access == cp::FaultAccess::Write);
    REQUIRE(e.registers.known);
    CHECK(e.registers.gpr[0] == 0x1000U); // rax
    CHECK(e.registers.gpr[1] == 0x2000U); // rcx
    CHECK(e.registers.gpr[15] == 0x10000U);
    CHECK(e.registers.flags == 0x246U);
    // A context without the integer registers, or one for another pc, is no register state.
    CHECK_FALSE(
        read_dump(context_dump(0xC0000005U, pc, 0U, 0x2000U, 9U, amd64_context(0x00100001U, pc, 0U))).registers.known);
    CHECK_FALSE(read_dump(context_dump(0xC0000005U, pc, 0U, 0x2000U, 9U, amd64_context(kAmd64Full, pc + 1U, 0U)))
                    .registers.known);
    // ARM64: X0 to X30, SP and PC.
    cont::Array<u8> arm;
    arm.resize(0x390U);
    std::memset(arm.data(), 0, arm.size());
    put_at(arm, 0U, 0x00400003U, 4U);
    for (u32 i = 0U; i < 31U; ++i)
    {
        put_at(arm, 8U + 8U * i, 0x100U + i, 8U);
    }
    put_at(arm, 0x100U, 0x7FF0000U, 8U);
    put_at(arm, 0x108U, pc, 8U);
    const cp::CrashEvidence a = read_dump(context_dump(0xC0000005U, pc, 8U, pc, 12U, arm));
    CHECK(a.instruction_set == cp::InstructionSet::AArch64);
    CHECK(a.access == cp::FaultAccess::Execute);
    REQUIRE(a.registers.known);
    CHECK(a.registers.gpr[30] == 0x100U + 30U);
    CHECK(a.registers.gpr[31] == 0x7FF0000U);
    // Every cut of the dump reads safely.
    for (usize n = 0U; n < dump.size(); ++n)
    {
        (void)cp::read_crash_evidence({dump.data(), n});
    }

    // A record: every register, the trap number and the page fault's error code (W: a write, I/D: an execution).
    const cp::CrashEvidence r = read(full_record(view(signal_line(11U, 1, 0x1234U)), pc, 0x1234U, 14U, 0x6U, ""));
    REQUIRE(r.ok);
    CHECK(r.instruction_set == cp::InstructionSet::X64);
    REQUIRE(r.registers.known);
    CHECK(r.registers.gpr[7] == 0x1234U); // rdi
    CHECK(r.registers.gpr[4] == 0x7FF000U);
    CHECK(r.registers.gpr[12] == 0xCU);
    CHECK(r.trap_known);
    CHECK(r.trap == 14U);
    CHECK(r.access == cp::FaultAccess::Write);
    CHECK(read(full_record(view(signal_line(11U, 1, 0x1234U)), pc, 0U, 14U, 0x14U, "")).access ==
          cp::FaultAccess::Execute);
    CHECK(read(full_record(view(signal_line(11U, 1, 0x1234U)), pc, 0U, 14U, 0x4U, "")).access == cp::FaultAccess::Read);
    CHECK(read(full_record(view(signal_line(11U, 1, 0x1234U)), pc, 0U, 13U, 0x0U, "")).access ==
          cp::FaultAccess::Unknown);
    // The short record of the tests above names some registers only: no register state.
    CHECK_FALSE(read(record(view(signal_line(11U, 1, 0x1234U)), pc)).registers.known);
}

TEST_CASE("fault classification: a fault on an access the instruction does not make is a signature, never on code "
          "not proven to be its module's",
          "[perf][diag][hw-resilience]")
{
    const u64 pc = kExeBase + 0x10U;
    const u64 rdi = 0x00007f00'12345000ULL;
    const auto judge = [&](u64 address, u32 trapno, u64 err, crd::i32 si_code, const cp::CodeImageCheck* image)
    {
        const cp::CrashEvidence e =
            read(full_record(view(signal_line(11U, si_code, address)), pc, rdi, trapno, err, "488b07c3"));
        CHECK(e.registers.known);
        return cp::classify_fault(&e, nullptr, cp::ReplayOutcome::NotRun, image);
    };
    const cp::CodeImageCheck same = matching();
    // mov rax, [rdi]: a read of [rdi, rdi + 8).
    const cp::FaultClassification made = judge(rdi + 7U, 14U, 0x4U, 1, &same);
    CHECK(made.instruction.result == cp::InstructionCheckResult::Consistent);
    CHECK(made.instruction.length == 3U);
    CHECK(made.instruction.accesses == 1U);
    CHECK(made.signals == 0U);
    CHECK(judge(pc + 2U, 14U, 0x14U, 2, &same).instruction.result == cp::InstructionCheckResult::Consistent);

    const cp::FaultClassification elsewhere = judge(rdi + 8U, 14U, 0x4U, 1, &same);
    CHECK(elsewhere.instruction.result == cp::InstructionCheckResult::AccessNotMade);
    CHECK(elsewhere.instruction.pc == pc);
    CHECK(elsewhere.signals == cp::kFaultSignalAccessNotMade);
    CHECK(elsewhere.verdict == cp::HardwareVerdict::Possible);
    cont::String names;
    cp::append_fault_signal_names(elsewhere.signals, names);
    CHECK(view(names) == "access-not-made");
    CHECK(view(cp::fault_message(elsewhere, nullptr)) ==
          "This failure shows one sign of a hardware fault, which alone is not proof: the instruction at "
          "0x555555554010 faulted on an address it does not access. If failures like it repeat in different places, "
          "test the machine's memory and processor.");
    CHECK(judge(rdi, 14U, 0x6U, 2, &same).instruction.reason == cont::StringView{"a write it does not make"});
    CHECK(judge(rdi + 0x100U, 14U, 0x14U, 2, &same).instruction.reason ==
          cont::StringView{"an execution outside its own bytes"});
    // Two signatures are likely.
    const cp::CrashEvidence twice =
        read(full_record(view(signal_line(11U, 1, rdi + 8U)), pc, rdi, 14U, 0x4U, "488b07c3"));
    CHECK(cp::classify_fault(&twice, nullptr, cp::ReplayOutcome::NotReproduced, &same).verdict ==
          cp::HardwareVerdict::Likely);

    // Never judged: a protection fault (no address), a kernel-sent code, code not proven to be its image's.
    CHECK(judge(rdi + 8U, 13U, 0x0U, 1, &same).instruction.result == cp::InstructionCheckResult::NotJudged);
    CHECK(judge(rdi + 8U, 14U, 0x4U, 128, &same).instruction.result == cp::InstructionCheckResult::NotJudged);
    CHECK(judge(rdi + 8U, 14U, 0x4U, 1, nullptr).signals == 0U);
    // A match that ends inside the instruction (the image's file data stops there) does not prove its bytes.
    cp::CodeImageCheck short_match = matching();
    short_match.compared = 2U;
    const cp::FaultClassification part = judge(rdi + 8U, 14U, 0x4U, 1, &short_match);
    CHECK(part.instruction.result == cp::InstructionCheckResult::NotJudged);
    CHECK(part.instruction.reason == cont::StringView{"the image comparison covers fewer bytes than the instruction"});
    short_match.compared = 3U;
    CHECK(judge(rdi + 8U, 14U, 0x4U, 1, &short_match).instruction.result == cp::InstructionCheckResult::AccessNotMade);
    for (const cp::CodeImageResult r :
         {cp::CodeImageResult::NotCompared, cp::CodeImageResult::Modified, cp::CodeImageResult::OneBitFlipped})
    {
        cp::CodeImageCheck k;
        k.result = r;
        const cp::FaultClassification c = judge(rdi + 8U, 14U, 0x4U, 1, &k);
        CHECK(c.instruction.result == cp::InstructionCheckResult::NotJudged);
        CHECK((c.signals & cp::kFaultSignalAccessNotMade) == 0U);
    }
    // A Windows dump: its access kind, and its protection faults' all-ones address.
    cp::CrashEvidence w =
        read_dump(context_dump(0xC0000005U, pc, 1U, 0x2000U, 9U, amd64_context(kAmd64Full, pc, 0x2000U)));
    const u8 store[] = {0x89U, 0x01U}; // mov [rcx], eax
    std::memcpy(w.code_bytes, store, sizeof(store));
    w.code_size = sizeof(store);
    CHECK(cp::classify_fault(&w, nullptr, cp::ReplayOutcome::NotRun, &same).instruction.result ==
          cp::InstructionCheckResult::Consistent);
    w.access = cp::FaultAccess::Read;
    CHECK(cp::classify_fault(&w, nullptr, cp::ReplayOutcome::NotRun, &same).instruction.reason ==
          cont::StringView{"a read it does not make"});
    w.address = ~u64{0};
    CHECK(cp::classify_fault(&w, nullptr, cp::ReplayOutcome::NotRun, &same).instruction.result ==
          cp::InstructionCheckResult::NotJudged);

    // AArch64 ignores an address's top byte: a fault reported with x7's tag (as Windows may) or without it (as Linux
    // does) is the load's own access, and one byte past it is not.
    cont::Array<u8> arm;
    arm.resize(0x390U);
    std::memset(arm.data(), 0, arm.size());
    put_at(arm, 0U, 0x00400003U, 4U);
    put_at(arm, 8U + 8U * 7U, 0x3400'0000'0000'8000ULL, 8U); // x7
    put_at(arm, 0x108U, pc, 8U);
    const auto arm_judge = [&](u64 address)
    {
        cp::CrashEvidence e = read_dump(context_dump(0xC0000005U, pc, 0U, address, 12U, arm));
        const u8 load[] = {0xE0U, 0x00U, 0x40U, 0xF9U}; // ldr x0, [x7]
        std::memcpy(e.code_bytes, load, sizeof(load));
        e.code_size = sizeof(load);
        CHECK(e.instruction_set == cp::InstructionSet::AArch64);
        return cp::classify_fault(&e, nullptr, cp::ReplayOutcome::NotRun, &same).instruction;
    };
    CHECK(arm_judge(0x3400'0000'0000'8007ULL).result == cp::InstructionCheckResult::Consistent);
    CHECK(arm_judge(0x8007U).result == cp::InstructionCheckResult::Consistent);
    CHECK(arm_judge(0x8008U).result == cp::InstructionCheckResult::AccessNotMade);
    CHECK(arm_judge(0x3400'0000'0000'8008ULL).reason == cont::StringView{"an address it does not access"});
    CHECK(arm_judge(pc + 3U).result == cp::InstructionCheckResult::Consistent); // its own four bytes
}

TEST_CASE("fault classification: a divide error its operands cannot raise is a signature",
          "[perf][diag][hw-resilience]")
{
    const u64 pc = kExeBase + 0x10U;
    const cp::CodeImageCheck same = matching();
    const auto judge = [&](u32 code, u64 rcx, const u8* bytes, u32 n)
    {
        cp::CrashEvidence e = read_dump(context_dump(code, pc, 0U, 0U, 9U, amd64_context(kAmd64Full, pc, rcx)));
        std::memcpy(e.code_bytes, bytes, n);
        e.code_size = n;
        return cp::classify_fault(&e, nullptr, cp::ReplayOutcome::NotRun, &same);
    };
    const u8 idiv[] = {0x48U, 0xF7U, 0xF9U};   // idiv rcx; rdx:rax = 0x3000:0x1000
    const u8 idiv_m[] = {0x48U, 0xF7U, 0x39U}; // idiv qword [rcx]
    const u8 load[] = {0x8BU, 0x01U};          // mov eax, [rcx]
    CHECK(judge(0xC0000094U, 0U, idiv, 3U).instruction.result == cp::InstructionCheckResult::Consistent);
    CHECK(judge(0xC0000095U, 0x2000U, idiv, 3U).instruction.result == cp::InstructionCheckResult::Consistent);
    const cp::FaultClassification fits = judge(0xC0000094U, ~u64{0} >> 1U, idiv, 3U); // by 2^63 - 1
    CHECK(fits.instruction.result == cp::InstructionCheckResult::DivideWithoutCause);
    CHECK(fits.signals == cp::kFaultSignalDivideWithoutCause);
    CHECK(
        view(cp::fault_message(fits, nullptr)) ==
        "This failure shows one sign of a hardware fault, which alone is not proof: the instruction at "
        "0x555555554010 raised a divide error, but its divisor is not zero and its quotient fits. If failures like it "
        "repeat in different places, test the machine's memory and processor.");
    const cp::FaultClassification not_divide = judge(0xC0000094U, 5U, load, 2U);
    CHECK(not_divide.instruction.result == cp::InstructionCheckResult::DivideWithoutCause);
    CHECK(not_divide.instruction.reason == cont::StringView{"it does not divide"});
    const cp::FaultClassification memory = judge(0xC0000094U, 0x2000U, idiv_m, 3U);
    CHECK(memory.instruction.result == cp::InstructionCheckResult::NotJudged);
    CHECK(memory.signals == 0U);
    // A Linux record's divide error: SIGFPE FPE_INTDIV with the divide error's trap number (rdx:rax 0:0x1000 by rcx
    // 0x2000 fits); a sent SIGFPE is never judged.
    const u64 rip = kExeBase + 0x20U;
    const cp::CrashEvidence r = read(full_record(view(signal_line(8U, 1, rip)), rip, 0U, 0U, 0U, "48f7f9"));
    CHECK(cp::classify_fault(&r, nullptr, cp::ReplayOutcome::NotRun, &same).instruction.result ==
          cp::InstructionCheckResult::DivideWithoutCause);
    const cp::CrashEvidence sent = read(full_record(view(signal_line(8U, -6, rip)), rip, 0U, 0U, 0U, "48f7f9"));
    CHECK(cp::classify_fault(&sent, nullptr, cp::ReplayOutcome::NotRun, &same).instruction.result ==
          cp::InstructionCheckResult::NotJudged);
}

TEST_CASE("fault classification: bundle.inspect reports the instruction check of a capture whose code is its image's",
          "[perf][diag][hw-resilience]")
{
    const u8 id[] = {0x03U, 0x04U};
    const cont::Array<u8> image = elf_image({id, sizeof(id)}, kLibSize, false);
    char dir[512];
    (void)std::snprintf(dir, sizeof(dir), "%s/images/.build-id/03", CRD_DIAG_COMMANDS_SCRATCH);
    REQUIRE(crd::platform::fs::create_directories(crd::platform::fs::Path{cont::StringView{dir}}));
    char image_path[600];
    (void)std::snprintf(image_path, sizeof(image_path), "%s/04", dir);
    REQUIRE(cp::write_bundle_atomic(image_path, cont::ConstSpan<u8>{image.data(), image.size()}));
    const u64 pc = kLibBase + 0x1010U;
    const u64 rdi = 0x00007f00'12345000ULL;
    const cont::String capture =
        full_record(view(signal_line(11U, 1, rdi + 0x40U)), pc, rdi, 14U, 0x4U, "488b07c3"); // reads [rdi]
    char path[512];
    (void)std::snprintf(path, sizeof(path), "%s/fault_classification_access.log", CRD_DIAG_COMMANDS_SCRATCH);
    REQUIRE(cp::write_bundle_atomic(path,
                                    cont::ConstSpan<u8>{reinterpret_cast<const u8*>(capture.data()), capture.size()}));
    cp::DiagServiceConfig config;
    config.root = cont::StringView{CRD_DIAG_COMMANDS_SCRATCH};
    cp::DiagCommandService svc{cp::authority_bit(cp::DiagAuthority::Read), config, crd::memory::default_allocator()};
    cp::DiagRequest req;
    req.command = "bundle.inspect";
    req.page_items = 64U;
    req.path = "fault_classification_access.log";
    const cp::DiagResult result = svc.execute(req);
    REQUIRE(result.status == cp::DiagStatus::Ok);
    INFO(result.json.c_str());
    CHECK(std::strstr(result.json.c_str(), R"("instruction_set":"x86-64","registers":true,"access":"read")") !=
          nullptr);
    CHECK(std::strstr(result.json.c_str(), R"("verdict":"possible","signals":"access-not-made")") != nullptr);
    CHECK(std::strstr(result.json.c_str(),
                      R"("instruction":"access-not-made","instruction_reason":"an address it )"
                      R"(does not access","instruction_length":3,"instruction_accesses":1)") != nullptr);
    (void)std::remove(image_path);
    const cp::DiagResult missing = svc.execute(req);
    REQUIRE(missing.status == cp::DiagStatus::Ok);
    CHECK(std::strstr(missing.json.c_str(), R"("instruction":"not-judged")") != nullptr);
    CHECK(std::strstr(missing.json.c_str(), R"("verdict":"none")") != nullptr);
    (void)std::remove(path);
}
