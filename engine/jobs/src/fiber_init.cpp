#include <crd/jobs/detail/fiber_context.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/platform.hpp>
#include <crd/core/types.hpp>

#include <cstdint>
#include <cstring>
#include <cstdlib>

namespace crd::jobs::detail
{

// Called if a fiber entry function returns without switching context.
// Execution must never reach this point; it always indicates a programming error.
[[noreturn]] static void fiber_abort() noexcept
{
    CRD_FATAL("fiber entry function returned without calling fiber_switch â€” all fibers must yield before returning");
    std::abort();
}

void fiber_init_stack(FiberContext& ctx, void* stack_base, crd::usize stack_size,
                      void (*entry_fn)()) noexcept
{
    CRD_ASSERT(stack_base != nullptr);
    CRD_ASSERT(stack_size >= 1024U);
    CRD_ASSERT(entry_fn != nullptr);

    auto* const top = static_cast<crd::u8*>(stack_base) + stack_size;

#if CRD_OS_WINDOWS && CRD_ARCH_X64
    // Windows x64 ABI â€” callee-saved: RBX RBP RDI RSI R12-R15 XMM6-XMM15 MXCSR x87FCW
    //
    // Initial frame layout (from saved_RSP low â†’ high):
    //   [  0..159]  XMM6-XMM15    (10 Ã— 16 = 160 bytes, restored with movdqu)
    //   [160..163]  MXCSR         (4 bytes, default 0x1F80)
    //   [164..165]  x87 FCW       (2 bytes, default 0x027F)
    //   [166..167]  padding       (2 bytes, zero)
    //   [168..175]  RBX           (8 bytes, zero)
    //   [176..183]  RBP           (8 bytes, zero)
    //   [184..191]  RDI           (8 bytes, zero)
    //   [192..199]  RSI           (8 bytes, zero)
    //   [200..207]  R12           (8 bytes, zero)
    //   [208..215]  R13           (8 bytes, zero)
    //   [216..223]  R14           (8 bytes, zero)
    //   [224..231]  R15           (8 bytes, zero)
    //   [232..239]  entry_fn      â† fiber_switch's `ret` jumps here
    //   [240..247]  fiber_abort   â† entry_fn's `ret` lands here (safety net)
    //   Total: 248 bytes
    //
    // Alignment invariant:
    //   p = align16(stack_top), saved_RSP = p - 248
    //   saved_RSP % 16 = 8  (ensures entry_fn sees RSP % 16 = 8 per Windows x64 ABI)
    //   (saved_RSP + 240) % 16 = 8 âœ“   â† RSP when entry_fn begins

    constexpr crd::usize frame_size = 248U;
    CRD_ASSERT(stack_size >= frame_size + 64U);

    auto p = reinterpret_cast<std::uintptr_t>(top);
    p &= ~std::uintptr_t{15U};  // align down to 16 â€” p % 16 = 0

    auto* frame = reinterpret_cast<crd::u8*>(p - frame_size); // NOLINT(performance-no-int-to-ptr)
    std::memset(frame, 0, frame_size);

    // Default FP environment
    *reinterpret_cast<crd::u32*>(frame + 160U) = 0x1F80U;  // MXCSR
    *reinterpret_cast<crd::u16*>(frame + 164U) = 0x027FU;  // x87 FCW

    // Entry point and safety net
    *reinterpret_cast<void**>(frame + 232U) = reinterpret_cast<void*>(entry_fn);
    *reinterpret_cast<void**>(frame + 240U) = reinterpret_cast<void*>(fiber_abort);

    ctx.rsp             = frame;
    ctx.tib_stack_base  = static_cast<crd::u8*>(stack_base) + stack_size; // high end
    ctx.tib_stack_limit = stack_base;                                      // low end (fully committed)

#elif CRD_ARCH_X64
    // Linux x86-64 SysV ABI â€” callee-saved: RBX RBP R12-R15 (XMMs are caller-saved)
    //
    // Initial frame layout (from saved_RSP low â†’ high):
    //   [ 0.. 7]  RBX           (8 bytes, zero)
    //   [ 8..15]  RBP           (8 bytes, zero)
    //   [16..23]  R12           (8 bytes, zero)
    //   [24..31]  R13           (8 bytes, zero)
    //   [32..39]  R14           (8 bytes, zero)
    //   [40..47]  R15           (8 bytes, zero)
    //   [48..55]  entry_fn      â† fiber_switch's `ret` jumps here
    //   [56..63]  fiber_abort   â† entry_fn's `ret` lands here (safety net)
    //   Total: 64 bytes
    //
    // Alignment invariant:
    //   p = align16(stack_top), saved_RSP = p - 64
    //   saved_RSP % 16 = 0  (ensures entry_fn sees RSP % 16 = 8 per SysV ABI)
    //   (saved_RSP + 56) % 16 = 8 âœ“   â† RSP when entry_fn begins

    constexpr crd::usize frame_size = 64U;
    CRD_ASSERT(stack_size >= frame_size + 64U);

    auto p = reinterpret_cast<std::uintptr_t>(top);
    p &= ~std::uintptr_t{15U};  // align down to 16 â€” p % 16 = 0

    auto* frame = reinterpret_cast<crd::u8*>(p - frame_size); // NOLINT(performance-no-int-to-ptr)
    std::memset(frame, 0, frame_size);

    *reinterpret_cast<void**>(frame + 48U) = reinterpret_cast<void*>(entry_fn);
    *reinterpret_cast<void**>(frame + 56U) = reinterpret_cast<void*>(fiber_abort);

    ctx.rsp = frame;

#else
#error "crd-jobs: unsupported platform or architecture"
#endif
}

} // namespace crd::jobs::detail
