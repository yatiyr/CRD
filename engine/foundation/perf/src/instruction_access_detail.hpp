#pragma once
// crd-perf -- internal: what the x86-64 decoder (instruction_access.cpp) and the AArch64 decoder
// (instruction_access_a64.cpp) share behind decode_instruction_access.

#include <crd/perf/instruction_access.hpp>

namespace crd::perf::detail
{

inline void add_access(InstructionAccess& out, crd::u64 address, crd::u64 size, crd::u8 kinds) noexcept
{
    if (out.count < kMaxInstructionAccesses && size != 0U)
    {
        out.accesses[out.count++] = MemoryAccess{address, size, kinds};
    }
}

[[nodiscard]] InstructionAccess decode_a64(crd::containers::ConstSpan<crd::u8> code,
                                           const FaultRegisters& regs) noexcept;

} // namespace crd::perf::detail
