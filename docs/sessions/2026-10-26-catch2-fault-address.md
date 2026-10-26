# The {pid, uid} fault address was Catch2's re-raise

<!-- doc-role: historical -->
> Dated evidence. Live owners: [DIAG.11b](../ROADMAP.md#slice-diag.11b) (the C2 batch's SEGV) and
> [DIAG.12](../ROADMAP.md#slice-diag.12) (detector output). Finding:
> [C2](2026-10-26-flight-recorder-c2.md#verification). Patch:
> [catch2-posix-signal-refault](../../cmake/patches/catch2-posix-signal-refault.cmake). Rules:
> [AGENTS](../../AGENTS.md).

## The task

One full linux-gcc-asan run of crd-perf-tests in the C2 batch stopped with a SEGV in the module-identity case at
`0x03e800000a89`. That is the `{pid, uid}` word (uid 1000, pid 0xa89) that the 2026-10-25 TSan investigation also
saw. Settle it before SIMD-DISPATCH. Confirm the host first, audit every alternate-stack path, and reproduce under
ASan beside busy loops.

## The host

**This host still misexecutes after the microcode update** (details under Reproduction):

- Before the work, `scripts/tsan-host-check.py` (60 rounds pinned to CPUs 0 and 1 beside two busy loops) found 0
  events in all four arms (jobs 66 s, control-fiber 183 s, control-plain 333 s, unsanitized 223 s).
- During it, three events in code that cannot produce them: two in Cerid's identity cases and one in a Cerid-free
  control.
- After it, the host check again: 0 events in all four arms (jobs 75 s, control-fiber 209 s, control-plain 382 s,
  unsanitized 245 s).

The TSan check does not expose the fault that this ASan loop does. As the host lesson says, the CPU (i9-14900K) is
likely degraded (Intel's extended 13th/14th-gen warranty). Hosted lanes remain the evidence.

## The cause

The word is `si_addr` read from a sent signal's siginfo. On x86-64 `si_addr` shares its slot with `si_pid` and
`si_uid`, so a SIGSEGV sent by `raise` reads as `(uid << 32) | pid`. Catch2 3.7.1's POSIX fatal-condition handler
reports the case, restores the previous handlers and calls `raise(sig)`. The signal is blocked while the handler
runs, so it is delivered when the handler returns, with the faulting context but the sent siginfo. The next handler
(ASan's, TSan's) therefore reports the right stack, "a READ memory access", and the sender's pid and uid as the
address. During a test case Catch2's handler is innermost, so **every** fault in any case on a sanitizer lane was
reported this way.

A probe proved it: a wild read of `0x5a5a00001000` in a Catch2 case on the linux-gcc-asan tree. ASan reported
`SEGV on unknown address 0x03e80000091d` from `==2333==`, i.e. pid 0x91d, the process's own. So the C2 SEGV was a
real fault in the identity case (the stack was right). Its address was lost, and the word says nothing about the
host or the recorder. This also explains the `{pid, uid}` addresses of the 2026-10-25 TSan host faults, whose cause
was the host's impossible reads.

## The fix

- **Patch `catch2-posix-signal-refault`.** The handler takes SA_SIGINFO. A SIGSEGV, SIGILL or SIGFPE the kernel
  raised (`si_code > 0`) returns after the report, so the instruction faults again under the restored handler with
  its own siginfo and context; this is the rule crd's crash handler has followed since 2026-10-25. A sent signal is
  still re-raised. The library compiles the build-owned copy in place of the upstream source, and the configure fails
  unless exactly one source is replaced.
- **ThreadSanitizer trees compile no Catch2 POSIX handler** (`CATCH_CONFIG_NO_POSIX_SIGNALS` follows
  `CRD_ENABLE_TSAN`). Catch2's report allocates inside the handler, and with `halt_on_error=1` TSan ended the process
  at that signal-unsafe call: exit 66 with no fault report at all. Without the report, a Catch2 handler in the chain
  still cost TSan's fault report its stack ("nested bug in the same thread"). A plain TSan program reports a full
  stack. With no Catch2 handler, TSan reports the address and the faulting function.
- **Alternate-stack audit.** Three runtimes register alternate stacks:
  - crd's `thread_local` buffer: its registration ends with its thread, so it cannot outlive its memory.
  - Catch2's per-case `new[]` block on the main thread: restored at the end of every case, freed only at session end.
  - The sanitizer runtime's per-thread mapping.
  No path leaves a registration over freed memory. One defect: `guard_current_thread_stack` cached "this thread has
  a stack" when it borrowed one. Catch2 takes its stack away at the end of the case, so a later guard on that thread
  left SA_ONSTACK with no stack, and a stack overflow there could not be recorded. The guard and `uninstall()` now
  read the registration from the kernel each time; the two flags are gone. `uninstall()` disables the stack only
  when the registered one is crd's.
- Signals delivered while a fiber stack is current: every handler in play (crd, Catch2, ASan, TSan) is SA_ONSTACK,
  and each thread has an alternate stack, so no handler frame lands on a fiber stack.

## Proof

`tests/foundation/core/test_diag_catch2_fault.cpp` (crd-core-tests, `[catch2-fault]`) re-enters its own executable on
hidden cases through the DIAG.0 specimen harness and reads the whole output:

- *A wild read in a case:* a sanitizer reports `unknown address 0x5a5a00001000` and `read_wild_address`; without one
  the child dies by SIGSEGV (Linux 139) or `0xC0000005` (Windows); Catch2 names the case where it sees the fault (not
  on TSan trees, and not on win-asan, whose vectored handler runs first).
- *A sent SIGSEGV:* the case still ends by it (nothing after `raise` runs).

`crash contract: a borrowed alternate stack its owner took away is replaced by the thread's own` (crd-core-tests)
borrows a stack, takes it away and calls the guard again.

Teeth (the first two on linux-gcc-asan, the third on it and linux-clang-tsan):

- Re-raise kept for faults: the wild-read case fails (the address is `0x03e8...`).
- Return for every signal: the sent case fails (`CHILD CONTINUED`).
- The guard's old cached flag: the contract case fails on both lanes (`2 == 0`, no stack).
- TSan with Catch2's handler compiled in failed the wild-read case before the fix, both with the report (exit 66,
  no fault report) and without it (no frame).

After the restore, the 26 `catch2 fault|crash contract|crash capture|crash record` CTests pass on linux-gcc-asan,
linux-clang-tsan and linux-gcc-debug. `[catch2-fault]` passes on win-debug and win-asan.

## Reproduction

All on linux-gcc-asan, each process pinned to CPUs 0 and 1 beside a busy loop on each, unless noted:

- **The identity cases**, 150 runs: 2 stopped. Both were UBSan `member access within misaligned address
  0x000001892479` (then `0x0000004da864`) for the clamp lambda's closure in `asan_annotate_live_range`
  (`asan_poison.hpp:119`). At -O0 that pointer is formed from the frame pointer just before the call, yet it held a
  small integer, the shape of the 2026-10-11 and 2026-10-25 faults. Catch2's stdout was block-buffered into the log
  file and lost at the abort, so only UBSan's line remains. The cases' hot loop is `read_whole_file`'s byte-by-byte
  `push_back` (86 MB, one annotation per byte, about 20 s a run): the same loop as the C2 SEGV.
- **The crash cases** (`[crash-capture],[crash],[catch2-fault]` of crd-core-tests), 40 runs: 0 events.
- **Whole programs** beside eight busy loops (unpinned): crd-perf-tests 3 runs (310 cases each) and crd-core-tests
  1 run, all passed.
- **Interleaved with a Cerid-free control**, 105 rounds. The control is a single-file, -O0 ASan and UBSan build of the
  same loop: a byte-by-byte `push_back` whose every step moves `__sanitizer_annotate_contiguous_container` through
  the same capturing clamp. Result: control 1 event (`*** stack smashing detected ***` in a program whose only stack
  objects are scalars and the lambda), identity 0 events (2,469 s of identity runs, 1,020 s of control).

So the identity case's fault is not Cerid's. The same misexecution hits the Cerid-free control, and per second of
run time no less often: 1 event in 1,020 s for the control, 2 in 5,557 s for the identity runs.

## Also seen

One linux-clang-tsan rebuild of Catch2's unity batch 4 crashed clang 18.1.3 (exit 139, in "Fast Register Allocator"
on `std::vector<Catch::TagInfo>::_M_realloc_insert`). The immediate rebuild of the same source compiled. A compiler
crash that does not reproduce is counted with the host's faults above.
