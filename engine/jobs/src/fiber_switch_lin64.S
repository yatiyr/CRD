/* fiber_switch_lin64.S — Linux x86-64 SysV ABI fiber context switch
 *
 * Signature: void fiber_switch(FiberContext* from, FiberContext* to)
 *   rdi = from  (pointer to FiberContext.rsp of the current fiber)
 *   rsi = to    (pointer to FiberContext.rsp of the fiber to resume)
 *
 * FiberContext layout: { void* rsp; }  — single pointer at offset 0.
 *
 * SysV callee-saved: RBX RBP R12 R13 R14 R15 RSP
 * XMM registers are caller-saved in SysV — no save/restore required.
 * No MXCSR / x87 FCW save (they are caller-saved in SysV).
 *
 * Stack frame built on the CURRENT fiber's stack (from saved_RSP, low to high):
 *   [ 0.. 7]  RBX
 *   [ 8..15]  RBP
 *   [16..23]  R12
 *   [24..31]  R13
 *   [32..39]  R14
 *   [40..47]  R15
 *   [48..N ]  return address (saved by the original `call fiber_switch`)
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

    /* Save current RSP into from->rsp ([rdi + 0]) */
    mov     %rsp, (%rdi)

    /* Load next RSP from to->rsp ([rsi + 0]) */
    mov     (%rsi), %rsp

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
