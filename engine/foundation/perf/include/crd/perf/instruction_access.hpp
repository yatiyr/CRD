#pragma once
// crd-perf -- the memory accesses a faulting instruction makes (HW-RESILIENCE.2). The instruction's bytes, as a crash
// capture holds them, are decoded with the faulting thread's registers into every range of memory the instruction can
// touch and, for an x86-64 divide, whether its divisor and dividend can raise the divide error. Fault classification
// compares them with the fault the operating system reported: a fault on an access the instruction does not make, or
// a divide error that its operands cannot cause, is code that cannot fault.
//
// Every range is an upper bound (a masked or a repeated string access lies inside it), so an address outside all of
// them is never touched by the instruction. Anything this cannot be sure of is not decoded, with its reason, and
// never judged: an FS or GS segment base, gathers and scatters (VSIB), APX's extended registers, the shadow stack
// that near calls and returns write under CET (SSP is not in the capture), system and I/O instructions, XSAVE's
// variable areas, encodings whose length depends on the vendor (a 66 prefix on a near branch), AMD's XOP and 3DNow!,
// and any opcode not listed. An EVEX instruction's 8-bit displacement is scaled by a factor its tuple type sets; each
// factor (1 to 64) gives one candidate range.
//
// AArch64: every A64 load and store (pairs, exclusives, compare and swaps, atomics including LSE128, RCW, LSUI and
// LSFE, SIMD structures, literal, register-offset and RCpc forms, LS64, MTE tag stores), DC and IC maintenance (the
// 2 KiB bound around the address, since DCZID_EL0 and CTR_EL0 are not in the capture), and the branches, hints and
// data processing that make none. Refused: SVE and SME, FEAT_MOPS copies and sets, pointer-authenticated loads,
// calls, returns and GCS operations (their Guarded Control Stack access at GCSPR_EL0 is not in the capture), system
// instructions and anything not listed. Top Byte Ignore: an AArch64 range's address keeps bits 55:0 only, and a fault
// address is compared in the same 56 bits (`kAArch64AddressBits`). AArch64 has no divide trap (a zero divisor gives
// zero; Windows' INT_DIVIDE_BY_ZERO on ARM64 comes from a `brk` the compiler inserts).

#include <crd/containers/span.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>

namespace crd::perf
{

enum class InstructionSet : crd::u8
{
    Unknown = 0U,
    X64 = 1U, // x86-64
    AArch64 = 2U,
};

// "unknown", "x86-64", "aarch64".
[[nodiscard]] crd::containers::StringView instruction_set_name(InstructionSet s) noexcept;

// The faulting thread's registers, as a minidump's exception context or a crash record holds them.
struct FaultRegisters
{
    bool known = false;
    // x86-64: rax, rcx, rdx, rbx, rsp, rbp, rsi, rdi, r8 to r15 (the encoding's order); AArch64: x0 to x30, then sp.
    crd::u64 gpr[32] = {};
    crd::u64 pc = 0U;
    crd::u64 flags = 0U; // x86-64 RFLAGS (the direction flag orders string accesses)
};

inline constexpr crd::u8 kAccessRead = 1U;
inline constexpr crd::u8 kAccessWrite = 2U;

// [address, address + size), read or written as `kinds` says (both bits: either or both).
struct MemoryAccess
{
    crd::u64 address = 0U;
    crd::u64 size = 0U;
    crd::u8 kinds = 0U;
};

inline constexpr crd::u32 kMaxInstructionAccesses = 8U;

// The address bits an AArch64 access uses: its top byte is ignored.
inline constexpr crd::u64 kAArch64AddressBits = (crd::u64{1} << 56U) - 1U;

struct InstructionAccess
{
    bool decoded = false;
    crd::containers::StringView reason; // why not decoded (static text)
    crd::u32 length = 0U;               // the instruction's bytes (its own fetch is [pc, pc + length))
    crd::u32 count = 0U;                // data accesses in `accesses`; none for an instruction that makes none
    MemoryAccess accesses[kMaxInstructionAccesses] = {};
    bool divides = false;       // x86-64 DIV or IDIV: in 64-bit mode the only instructions that raise a divide error
    bool divisor_known = false; // its divisor is a register
    bool divide_faults = false; // the divisor is zero or the quotient does not fit its register
};

// Decodes the instruction at `regs.pc` from `code` (the capture's bytes from the pc; an instruction longer than them is
// not decoded). Untrusted input: nothing is read outside `code`.
[[nodiscard]] InstructionAccess decode_instruction_access(InstructionSet set, crd::containers::ConstSpan<crd::u8> code,
                                                          const FaultRegisters& regs) noexcept;

// Whether an x86-64 DIV (`is_signed` false) or IDIV of `bits` (8, 16, 32 or 64) raises the divide error: a zero
// divisor or a quotient that does not fit. `high` and `low` hold the dividend's halves as the instruction reads them
// (AH and AL, DX and AX, EDX and EAX, RDX and RAX), `divisor` its divisor; bits above `bits` are ignored. Decided by
// comparison, never by dividing.
[[nodiscard]] bool x86_divide_faults(bool is_signed, crd::u32 bits, crd::u64 high, crd::u64 low,
                                     crd::u64 divisor) noexcept;

} // namespace crd::perf
