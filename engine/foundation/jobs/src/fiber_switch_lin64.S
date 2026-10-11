/* fiber_switch_lin64.S — Linux x86-64 SysV ABI fiber context switch
 *
 * Signature: void fiber_switch(FiberContext* from, FiberContext* to)
 *   rdi = from  (pointer to FiberContext.rsp of the current fiber)
 *   rsi = to    (pointer to FiberContext.rsp of the fiber to resume)
 *
 * FiberContext layout: { void* rsp; }  — single pointer at offset 0.
 *
 * SysV callee-saved: RBX RBP R12 R13 R14 R15 RSP, the MXCSR control bits and the x87 control word.
 * XMM registers are caller-saved in SysV — no save/restore required.
 *
 * DIAG.10c: MXCSR and the x87 control word are saved and restored like Windows does. The psABI makes their control
 * bits callee-saved (only the MXCSR status bits and the x87 status word are caller-saved). Without the save, a job
 * that set flush-to-zero or a rounding mode passed it to the worker's scheduler stack and to every job that ran next
 * on that thread, and a job that waited resumed in whatever mode the resuming thread had: results changed without
 * any crash. The whole MXCSR is kept, so a job's sticky exception flags stay its own as well.
 *
 * Stack frame built on the CURRENT fiber's stack (from saved_RSP, low to high):
 *   [ 0.. 3]  MXCSR
 *   [ 4.. 5]  x87 FCW
 *   [ 6.. 7]  padding
 *   [ 8..15]  RBX
 *   [16..23]  RBP
 *   [24..31]  R12
 *   [32..39]  R13
 *   [40..47]  R14
 *   [48..55]  R15
 *   [56..N ]  return address (saved by the original `call fiber_switch`)
 */

.text
.globl  fiber_switch
.type   fiber_switch, @function

fiber_switch:
    /* Save callee-saved integer registers */
    push    %r15
    push    %r14
    push    %r13
    push    %r12
    push    %rbp
    push    %rbx

    /* Save the floating-point control state (DIAG.10c) */
    sub     $8, %rsp
    stmxcsr (%rsp)
    fnstcw  4(%rsp)

    /* Save current RSP into from->rsp ([rdi + 0]) */
    mov     %rsp, (%rdi)

    /* Load next RSP from to->rsp ([rsi + 0]) */
    mov     (%rsi), %rsp

    /* Restore the floating-point control state of the fiber being resumed */
    ldmxcsr (%rsp)
    fldcw   4(%rsp)
    add     $8, %rsp

    /* Restore callee-saved integer registers from new stack */
    pop     %rbx
    pop     %rbp
    pop     %r12
    pop     %r13
    pop     %r14
    pop     %r15

    /* Jump to the saved return address at the top of the new stack */
    ret

.size fiber_switch, .-fiber_switch

/* crd_fiber_start — the first code a new fiber runs: fiber_switch's `ret` lands here on a fresh stack built by
 * fiber_init_stack, with R12 = the fiber entry function and R13 = the never-return safety net (fiber_abort).
 *
 * It exists for unwinders. `.cfi_undefined rip` declares this frame the outermost one, so libgcc's _Unwind_Backtrace
 * (glibc backtrace(), crash and sampling profilers) stops here. Without it the entry function's return address was the
 * safety net's address, the unwinder looked up the CFI of whatever precedes it in memory, computed a frame past the
 * stack top and faulted (hosted linux-gcc-debug-sse2, 2026-10-04, test_diag_unwind_qualification). RBP is cleared
 * so frame-pointer walkers stop here as well.
 *
 * Alignment: entered at RSP % 16 == 8 (the ret consumed the 16-aligned frame's return slot), so one 8-byte adjust
 * makes RSP % 16 == 0 before each call, and the callee starts at RSP % 16 == 8 as the SysV ABI requires.
 */
.globl  crd_fiber_start
.type   crd_fiber_start, @function

crd_fiber_start:
    .cfi_startproc
    .cfi_undefined rip
    xor     %ebp, %ebp
    sub     $8, %rsp
    .cfi_adjust_cfa_offset 8
    call    *%r12            /* the fiber entry function; it must switch away and never return */
    call    *%r13            /* safety net: fiber_abort is [[noreturn]] and fatals */
    ud2
    .cfi_endproc

.size crd_fiber_start, .-crd_fiber_start

/*
 * Mark the stack as non-executable. Without this section, GNU ld emits
 *   "missing .note.GNU-stack section implies executable stack ... deprecated
 *    and will be removed in a future version of the linker"
 * and the resulting binary is linked with an executable stack. Hand-written
 * assembly (.S files) does NOT get this section auto-emitted by the
 * assembler the way .c files do, so we add it explicitly.
 */
#if defined(__linux__) && defined(__ELF__)
.section .note.GNU-stack,"",@progbits
#endif
