#pragma once
// crd-perf -- hardware fault classification (HW-RESILIENCE.2). A native crash capture is read for the evidence it
// holds (the fault, its code and data addresses, the loaded modules), its "impossible state" signatures are found,
// and the machine is checked against a table of processors with a known instability. The verdict says how likely a
// hardware fault is, names each signature with its evidence and gives one plain sentence for the user that names
// the likely cause and its fix. Blame needs evidence: a known processor alone is never a verdict, and a sent signal's
// address (a process's id, not a fault's) is never read as one.
//
// Signatures:
//   machine-check          the kernel reported an uncorrectable memory error (Linux SIGBUS BUS_MCEERR_AR/AO)
//   code-address-bit-flip  execution went to an address outside every module that one flipped bit puts inside a
//                          loaded one: a code pointer holding a value no store wrote
//   data-address-bit-flip  an access faulted at an address outside every module that one flipped bit puts inside a
//                          loaded one
//   replay-not-reproduced  the capture's replay recipe ran on the same build and did not reproduce the failure
//   code-bit-flip          the faulting instruction's bytes differ from the module's image in exactly one bit: code
//                          no store wrote (compare_code_with_image; more than one differing bit is a patch, such as a
//                          hook or a breakpoint, and never judged)
//   access-not-made        an access fault on an address, or of a kind, that the faulting instruction (decoded with
//                          its registers, instruction_access.hpp) does not access: code that cannot fault there
//   divide-without-cause   an x86-64 divide error on an instruction that does not divide, or on a DIV or IDIV whose
//                          register divisor is nonzero and whose quotient fits
// The last two are judged only on code proven to be its module's (its image comparison matches), so code that changed
// after the fault (JIT code, a hook) is never read for them.
// Addresses below kNullRegionBytes (a null pointer plus an offset, a common software bug) are never judged.
//
// Verdict: likely on a machine check, on two signatures, or on one with a listed processor; possible on one
// signature; otherwise none.

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/containers/string.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/core/crash.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/perf/hardware_identity.hpp>
#include <crd/perf/instruction_access.hpp>
#include <crd/perf/symbol_index.hpp>

namespace crd::perf
{

// The lowest addresses no process maps (Windows' first 64 KiB, Linux's default vm.mmap_min_addr).
inline constexpr crd::u64 kNullRegionBytes = 0x10000U;

enum class CrashCaptureKind : crd::u8
{
    None = 0U,
    Minidump = 1U, // a Windows minidump: its exception stream and module list
    Record = 2U,   // a Linux crash record (crd::crash): its signal line, registers and module lines
};

// "none", "minidump", "record".
[[nodiscard]] crd::containers::StringView crash_capture_kind_name(CrashCaptureKind k) noexcept;

// The faulting access's kind, as the capture states it (a Windows access violation's first parameter; on x86-64 Linux
// the page fault's error code).
enum class FaultAccess : crd::u8
{
    Unknown = 0U,
    Read = 1U,
    Write = 2U,
    Execute = 3U,
};

// "unknown", "read", "write", "execute".
[[nodiscard]] crd::containers::StringView fault_access_name(FaultAccess a) noexcept;

// What a native crash capture says about its fault.
struct CrashEvidence
{
    CrashEvidence() = default;
    explicit CrashEvidence(crd::memory::IAllocator* alloc) : modules(alloc) {}

    CrashCaptureKind kind = CrashCaptureKind::None;
    bool ok = false;
    crd::containers::StringView error; // why the capture was refused (static text)
    crd::u32 code = 0U;                // exception code (Windows) or signal number (Linux)
    crd::i32 signal_code = 0;          // Linux si_code; 0 in a minidump
    bool sent = false;                 // sent by a process (si_code <= 0): its address is not evidence
    bool pc_known = false;
    crd::u64 pc = 0U; // the faulting instruction's address
    bool address_known = false;
    crd::u64 address = 0U;                          // the data address an access fault names
    crd::containers::Array<ModuleIdentity> modules; // loaded, then unloaded (a minidump's), as the capture lists them
    crd::u32 code_size = 0U;                        // bytes of `code_bytes` the capture holds from pc (0: none)
    crd::u8 code_bytes[crd::crash::kCodeWindowBytes] = {};
    InstructionSet instruction_set = InstructionSet::Unknown;
    FaultRegisters registers; // the faulting thread's; known only when they name the capture's pc
    FaultAccess access = FaultAccess::Unknown;
    bool trap_known = false;
    crd::u32 trap = 0U; // x86-64 Linux: the processor's exception vector (14 page fault, 13 protection, 0 divide)
};

// Reads a minidump (Windows' exception stream, type 6, with its thread context, the processor architecture of its
// system information, type 7, its module lists and the range of its memory lists, types 5 and 9, that holds the pc) or
// a Linux crash record (its registers, with x86-64's trap number and error code, and its `code` line). Untrusted input
// on any platform: nothing is read outside `capture`; a capture that is neither, or whose fault cannot be read, is
// refused with `error`.
[[nodiscard]] CrashEvidence read_crash_evidence(crd::containers::ConstSpan<crd::u8> capture,
                                                crd::memory::IAllocator* alloc = crd::memory::default_allocator());

// Where a module image's bytes come from: `read` copies `size` bytes at file `offset` into `out`, false when the
// file is shorter or cannot be read (a span, a file or a symbol server's download).
struct ImageReader
{
    void* context = nullptr;
    bool (*read)(void* context, crd::u64 offset, void* out, crd::u32 size) = nullptr;
};

enum class CodeImageResult : crd::u8
{
    NotCompared = 0U,   // `reason` says why
    Matches = 1U,       // every captured byte the image holds is the image's
    OneBitFlipped = 2U, // exactly one bit differs: the code-bit-flip signature
    Modified = 3U,      // more than one bit differs: patched code (a hook, a breakpoint), never judged
};

// "not-compared", "matches", "one-bit-flipped", "modified".
[[nodiscard]] crd::containers::StringView code_image_result_name(CodeImageResult r) noexcept;

// The faulting instruction's bytes against the module's image.
struct CodeImageCheck
{
    CodeImageCheck() = default;
    explicit CodeImageCheck(crd::memory::IAllocator* alloc) : module(alloc) {}

    CodeImageResult result = CodeImageResult::NotCompared;
    crd::containers::StringView reason; // why not compared (static text)
    crd::u64 pc = 0U;                   // the capture's pc
    crd::u32 compared = 0U;             // bytes compared, from pc
    crd::u32 differing_bits = 0U;
    crd::u32 offset = 0U;  // the first differing byte, from pc
    crd::u32 bit = 0U;     // OneBitFlipped: the flipped bit of that byte
    crd::u8 recorded = 0U; // that byte as the capture holds it
    crd::u8 image = 0U;    // and as the image does
    crd::containers::String module;
};

// The loaded module holding the capture's pc, or null.
[[nodiscard]] const ModuleIdentity* code_module(const CrashEvidence& e) noexcept;

// Compares the capture's code bytes with `image`, the file of the loaded module holding the pc: a PE file (its
// TimeDateStamp, SizeOfImage and CheckSum must be the module's; the pc's section maps it to a file offset, and the
// base relocations inside the window are applied for the module's load address) or an ELF file (its GNU build-id and
// PT_LOAD span must be the module's; the PT_LOAD holding the pc maps it, and a module with text relocations is not
// compared). Bytes past the section's or segment's file data are not compared. Untrusted input on both sides:
// nothing is read outside what `image` returns, and anything this cannot be sure of is NotCompared, never a verdict.
[[nodiscard]] CodeImageCheck compare_code_with_image(const CrashEvidence& e, const ImageReader& image,
                                                     crd::memory::IAllocator* alloc = crd::memory::default_allocator());

// Whether the capture's replay recipe was run on the same build, and what it did.
enum class ReplayOutcome : crd::u8
{
    NotRun = 0U,
    Reproduced = 1U,
    NotReproduced = 2U,
};

// "not-run", "reproduced", "not-reproduced".
[[nodiscard]] crd::containers::StringView replay_outcome_name(ReplayOutcome r) noexcept;

// Parses a replay_outcome_name; false (leaving `out`) for any other text.
[[nodiscard]] bool parse_replay_outcome(crd::containers::StringView text, ReplayOutcome& out) noexcept;

// Signature bits of FaultClassification::signals.
inline constexpr crd::u32 kFaultSignalMachineCheck = 1U << 0;
inline constexpr crd::u32 kFaultSignalCodeAddressBitFlip = 1U << 1;
inline constexpr crd::u32 kFaultSignalDataAddressBitFlip = 1U << 2;
inline constexpr crd::u32 kFaultSignalReplayNotReproduced = 1U << 3;
inline constexpr crd::u32 kFaultSignalCodeBitFlip = 1U << 4;
inline constexpr crd::u32 kFaultSignalAccessNotMade = 1U << 5;
inline constexpr crd::u32 kFaultSignalDivideWithoutCause = 1U << 6;

// Appends the names of `signals` ("machine-check", "code-address-bit-flip", "data-address-bit-flip",
// "replay-not-reproduced", "code-bit-flip", "access-not-made", "divide-without-cause"), comma separated, in bit order.
void append_fault_signal_names(crd::u32 signals, crd::containers::String& out);

enum class HardwareVerdict : crd::u8
{
    None = 0U,
    Possible = 1U,
    Likely = 2U,
};

// "none", "possible", "likely".
[[nodiscard]] crd::containers::StringView hardware_verdict_name(HardwareVerdict v) noexcept;

// A processor with a known instability, as its maker published it.
struct CpuAdvisory
{
    crd::containers::StringView id;     // a stable key ("intel-raptor-lake-vmin-shift")
    crd::containers::StringView name;   // the instability, as its maker names it
    crd::containers::StringView vendor; // the CPUID vendor
    crd::u32 signature;                 // CPUID 1 EAX
    crd::u32 fixed_microcode;           // the first revision with every published mitigation
    crd::containers::StringView source; // the maker's statement
};

// Every listed advisory.
[[nodiscard]] crd::containers::ConstSpan<CpuAdvisory> cpu_advisories() noexcept;

// The advisory `machine`'s processor is listed under, or null: the vendor and signature, and the model the advisory
// covers (Raptor Lake: a 13th or 14th Gen desktop Core i5, i7 or i9 of 65 W or more, so no HX, H, P, U or T part).
// Unknown CPU identity (kHardwareCpuKnown clear) is never listed.
[[nodiscard]] const CpuAdvisory* find_cpu_advisory(const HardwareIdentity& machine) noexcept;

enum class MicrocodeState : crd::u8
{
    NotListed = 0U, // the processor has no advisory
    Unknown = 1U,   // listed, but the microcode was not read (a hypervisor hides it)
    BeforeFix = 2U, // listed, and older than the fix
    Fixed = 3U,     // listed, and at or after the fix (which stops further damage, never repairs it)
};

// "not-listed", "unknown", "before-fix", "fixed".
[[nodiscard]] crd::containers::StringView microcode_state_name(MicrocodeState s) noexcept;

// One flipped bit: the address as recorded, the same address with the bit restored and the module that holds it.
struct BitFlipEvidence
{
    BitFlipEvidence() = default;
    explicit BitFlipEvidence(crd::memory::IAllocator* alloc) : module(alloc) {}

    bool found = false;
    crd::u64 recorded = 0U;
    crd::u64 restored = 0U;
    crd::u32 bit = 0U;
    crd::containers::String module;
};

enum class InstructionCheckResult : crd::u8
{
    NotJudged = 0U,          // `reason` says why
    Consistent = 1U,         // the instruction makes the faulting access, or its operands raise its divide error
    AccessNotMade = 2U,      // the access-not-made signature
    DivideWithoutCause = 3U, // the divide-without-cause signature
};

// "not-judged", "consistent", "access-not-made", "divide-without-cause".
[[nodiscard]] crd::containers::StringView instruction_check_result_name(InstructionCheckResult r) noexcept;

// The faulting instruction, decoded with the capture's registers, against the fault the operating system reported.
struct InstructionCheck
{
    InstructionCheckResult result = InstructionCheckResult::NotJudged;
    crd::containers::StringView reason; // why not judged, or what the instruction does not do (static text)
    crd::u64 pc = 0U;
    crd::u32 length = 0U;   // the decoded instruction's bytes
    crd::u32 accesses = 0U; // its data accesses
};

struct FaultClassification
{
    FaultClassification() = default;
    explicit FaultClassification(crd::memory::IAllocator* alloc) : code_flip(alloc), data_flip(alloc), code_image(alloc)
    {
    }

    HardwareVerdict verdict = HardwareVerdict::None;
    crd::u32 signals = 0U; // kFaultSignal*
    bool crash = false;    // a crash capture was judged
    bool sent = false;     // its signal was sent: the address was not judged
    BitFlipEvidence code_flip;
    BitFlipEvidence data_flip;
    CodeImageCheck code_image; // the comparison judged (NotCompared when none was given)
    InstructionCheck instruction;
    const CpuAdvisory* advisory = nullptr;
    MicrocodeState microcode = MicrocodeState::NotListed;
    ReplayOutcome replay = ReplayOutcome::NotRun;
};

// Judges a crash (null: none, as for a flight bundle without one) on `machine` (null: not recorded) with what its
// replay did and its code's comparison with the module's image (null: not compared, so the instruction is not judged).
// Pure: reads nothing but its arguments.
[[nodiscard]] FaultClassification classify_fault(const CrashEvidence* crash, const HardwareIdentity* machine,
                                                 ReplayOutcome replay, const CodeImageCheck* code_image = nullptr,
                                                 crd::memory::IAllocator* alloc = crd::memory::default_allocator());

// One plain sentence for the user: whether the failure looks like a hardware fault, what points to it, and the fix
// (a BIOS update for microcode before the fix, a warranty replacement for a processor that has the fix and still
// faults, a memory test for a memory error). `machine` is the one classify_fault judged.
[[nodiscard]] crd::containers::String fault_message(const FaultClassification& c, const HardwareIdentity* machine,
                                                    crd::memory::IAllocator* alloc = crd::memory::default_allocator());

} // namespace crd::perf
