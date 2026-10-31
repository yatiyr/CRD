// HW-RESILIENCE.2: hardware fault classification (crd/perf/fault_classification.hpp) -- reading a minidump's and a
// Linux crash record's fault, each impossible-state signature and its guards, the processor advisory table, the
// verdict and its sentence, and bundle.inspect's judgement of a capture and of a bundle that carries one.

#include <crd/containers/array.hpp>
#include <crd/containers/string.hpp>
#include <crd/perf/bundle.hpp>
#include <crd/perf/diag_commands.hpp>
#include <crd/perf/fault_classification.hpp>
#include <crd/perf/hardware_identity.hpp>

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

// A minidump of an exception stream (type 6), a module list (type 4: the game at kExeBase) and an unloaded module
// list (type 14: a plugin at `unloaded_base`).
cont::Array<u8> minidump(u32 code, u64 pc, u32 parameters, u64 access, u64 address, u64 unloaded_base)
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

    cont::Array<u8> out;
    put32(out, 0x504D444DU); // 'MDMP'
    put32(out, 0xA793U);
    put32(out, 3U);  // streams
    put32(out, 32U); // the directory follows the header
    for (u32 i = 0U; i < 4U; ++i)
    {
        put32(out, 0U);
    }
    const u32 first = 32U + 3U * 12U;
    const u32 sizes[] = {static_cast<u32>(exception.size()), static_cast<u32>(modules.size()),
                         static_cast<u32>(unloaded.size())};
    const u32 types[] = {6U, 4U, 14U};
    u32 at = first;
    for (u32 i = 0U; i < 3U; ++i)
    {
        put32(out, types[i]);
        put32(out, sizes[i]);
        put32(out, at);
        at += sizes[i];
    }
    for (const cont::Array<u8>* stream : {&exception, &modules, &unloaded})
    {
        for (const u8 b : *stream)
        {
            out.push_back(b);
        }
    }
    return out;
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
