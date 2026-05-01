; fiber_switch_win64.asm — Windows x64 fiber context switch
;
; Signature: void fiber_switch(FiberContext* from, FiberContext* to)
;   RCX = from  (pointer to FiberContext.rsp of the current fiber)
;   RDX = to    (pointer to FiberContext.rsp of the fiber to resume)
;
; FiberContext layout: { void* rsp; }  — single pointer at offset 0.
;
; Stack frame built on the CURRENT fiber's stack (from saved_RSP, low to high):
;   [  0..159]  XMM6-XMM15  (10 x 16 bytes, movdqu — unaligned-safe)
;   [160..167]  MXCSR (4) + x87 FCW (2) + padding (2)
;   [168..231]  RBX RBP RDI RSI R12 R13 R14 R15  (8 x 8 bytes, pushed onto stack)
;   [232..N  ]  return address (saved by the original `call fiber_switch`)
;
; The `sub rsp, 8` before saving MXCSR/FCW aligns RSP to 16 bytes so that
; the context block lies within a predictable and debuggable boundary.
; movdqu is used for XMM saves because the initial fiber's saved_RSP is
; 8-byte-misaligned (required to satisfy Windows x64 ABI entry alignment
; for the fiber entry function).

.code

PUBLIC  fiber_switch

fiber_switch PROC
    ; -- Save callee-saved integer registers (push order = restore pop order reversed) --
    push    r15
    push    r14
    push    r13
    push    r12
    push    rsi
    push    rdi
    push    rbp
    push    rbx

    ; -- Reserve 8 bytes for MXCSR + x87 FCW (aligns RSP to 16 for the frame) --
    sub     rsp, 8
    stmxcsr DWORD PTR [rsp]
    fstcw   WORD PTR  [rsp + 4]

    ; -- Save XMM6-XMM15 (Windows x64 ABI: XMM6-XMM15 are callee-saved) --
    ; movdqu = unaligned 128-bit store; correct regardless of RSP alignment.
    sub     rsp, 160
    movdqu  XMMWORD PTR [rsp +   0], xmm6
    movdqu  XMMWORD PTR [rsp +  16], xmm7
    movdqu  XMMWORD PTR [rsp +  32], xmm8
    movdqu  XMMWORD PTR [rsp +  48], xmm9
    movdqu  XMMWORD PTR [rsp +  64], xmm10
    movdqu  XMMWORD PTR [rsp +  80], xmm11
    movdqu  XMMWORD PTR [rsp +  96], xmm12
    movdqu  XMMWORD PTR [rsp + 112], xmm13
    movdqu  XMMWORD PTR [rsp + 128], xmm14
    movdqu  XMMWORD PTR [rsp + 144], xmm15

    ; -- Context switch: save current RSP, load next RSP --
    mov     QWORD PTR [rcx], rsp    ; from->rsp = current RSP
    mov     rsp, QWORD PTR [rdx]    ; RSP = to->rsp

    ; -- Restore XMM6-XMM15 from new stack --
    movdqu  xmm6,  XMMWORD PTR [rsp +   0]
    movdqu  xmm7,  XMMWORD PTR [rsp +  16]
    movdqu  xmm8,  XMMWORD PTR [rsp +  32]
    movdqu  xmm9,  XMMWORD PTR [rsp +  48]
    movdqu  xmm10, XMMWORD PTR [rsp +  64]
    movdqu  xmm11, XMMWORD PTR [rsp +  80]
    movdqu  xmm12, XMMWORD PTR [rsp +  96]
    movdqu  xmm13, XMMWORD PTR [rsp + 112]
    movdqu  xmm14, XMMWORD PTR [rsp + 128]
    movdqu  xmm15, XMMWORD PTR [rsp + 144]
    add     rsp, 160

    ; -- Restore MXCSR and x87 FCW --
    ldmxcsr DWORD PTR [rsp]
    fldcw   WORD PTR  [rsp + 4]
    add     rsp, 8

    ; -- Restore callee-saved integer registers (reverse push order) --
    pop     rbx
    pop     rbp
    pop     rdi
    pop     rsi
    pop     r12
    pop     r13
    pop     r14
    pop     r15

    ; -- Jump to the saved return address at the top of the new stack --
    ; For a fiber's first execution this is the entry function address.
    ; For subsequent switches this is the instruction after the previous
    ; fiber_switch call inside that fiber.
    ret
fiber_switch ENDP

END
