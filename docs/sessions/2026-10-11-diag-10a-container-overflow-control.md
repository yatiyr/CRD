# DIAG.10a container-overflow control, 2026-10-11

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.10a](../ROADMAP.md#slice-diag.10a). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-10a); harness:
> [test instruments](../design/test-instruments.md). Rules: [AGENTS](../../AGENTS.md).
> Preceding: [DIAG.10a unknown-origin CEIR and CKIR artifacts](2026-10-11-diag-10a-unknown-origin-artifacts.md).

## Goal

The last local clause of DIAG.10a: the corruption of unknown origin. The preceding batch saw three events whose sites
were all in `crd::containers::Array`'s container-annotation path (`annotate_size`, `asan_annotate_live_range`, a byte
`push_back` right after its annotation) and named the next local step: the same rounds with
`ASAN_OPTIONS=detect_container_overflow=0` against the default. This note runs that control and the two bounded
follow-up slots its result called for. No source, script or workflow changed. Committed locally, not pushed.

## Decision rules, fixed before each run

- Control (arm A `detect_container_overflow=1`, arm B `=0`): faults in arm B at a comparable rate mean the
  annotation path is where corruption is noticed, not where it is made; faults only in arm A across several pairs
  are consistent with an annotation cause or with the annotation being the detector, and both are said; zero in
  both is inconclusive at the executions run. Six slots were planned, six more only if the six gave fewer than
  three events.
- Replay-driver slot: faults in the replay driver mean libFuzzer's driver (its timer, malloc hooks, RSS thread and
  growing heap) is not needed; a clean replay driver after at least 1.5 M executions means it is.
- Build slot: the same 790 inputs at the same moment on five builds. A fault in an unsanitized build means the
  sanitizer runtimes are not needed; faults in WSL with native Windows clean at comparable exposure would point at
  the WSL guest rather than the host.

## The flag disables the annotation in this runtime

`crd-memory-tests` was built on the `linux-clang-fuzz` tree (clang 18.1.3, the runtime the fuzz executables link)
and the case "an Array marks its unused capacity as a container live range" run twice: with the flag on it passes
(80 assertions); with `detect_container_overflow=0` it fails at its first `live_exactly` check and every later one
(exit 42), because `__sanitizer_annotate_contiguous_container` returns before writing shadow. Arm B therefore ran
without the annotation, not merely without its reports. The whole `[memory][diag][asan]` tag passes with the
default (9 cases, 428 assertions).

## The control

`linux-clang-fuzz` at 9842b0b4 (no rebuild needed), six slots. Each slot ran four libFuzzer processes at once:
`crd-fuzz-ceir-text-libfuzzer` and `crd-fuzz-ckir-binary-libfuzzer`, each in both arms, with the same `-seed`
(7001 to 7006) and the same `-runs` per target (450,000 and 3,000,000, the earlier rounds' sizes), an empty working
directory, the committed corpus as read-only seed, `-max_len=65536 -timeout=10 -rss_limit_mb=2048
-malloc_limit_mb=256`, `UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1` and every other ASan option at its
default. The arms of a pair ran at the same moment, so drift in time or temperature hits both.

| Arm | Processes | Faulted | Executions | Executions per event |
| --- | --- | --- | --- | --- |
| A, annotation on | 12 | 8 | 7,806,138 | about 1.0 M |
| B, annotation off | 12 | 7 | 10,835,291 and two runs of 10 to 11 s not counted | about 1.5 M |

By target: ceir-text 7 events in about 2.8 M executions, ckir-binary 8 in about 15.8 M. Thirteen faults wrote an
artifact; two arm-B processes died with SIGSEGV inside ASan's own handler before libFuzzer could write one (a WRITE
to address 4; a jump to pc `0x3f8` with `bp` 0). Fifteen events is far past the three that would have called for
six more slots, so none were run.

Arm B faulted at the same order of rate, and one of its events is a SEGV inside `asan_annotate_live_range` itself,
whose runtime call returns early in that arm. **The container-annotation path is where the corruption is noticed,
not where it is made.**

Each artifact was replayed alone three times with the replay executable under its own arm's options: all 39 replays
pass. One (`574c6ef4`, arm B) is byte-identical to the committed seed `seed-9c664232f137faae`, which every lane
replays; inside the process it failed the cross-format oracle. Two are byte-identical to units the same process had
already run clean; ten are new mutations. The reported sites, recorded and not explained:

- `is_pow2(alignment)` asserted in `MallocAllocator::try_allocate` (`malloc_allocator.cpp:89`) called from
  `BudgetAllocator::try_allocate` (`harness.hpp:125`), twice (arm A, ckir-binary). The caller passes the constant
  `alignof(Header)`: the call site is `mov $0x8,%edx; call`, the callee stores `%rdx` to its own frame slot
  `-0x38(%rbp)` and reloads it for the check, and only coverage callbacks run in between. In correct execution that
  check cannot fail. Three things can change such a value: the CPU, the kernel or hypervisor's save and restore of
  register state, or a foreign write into the live native stack. Nothing in our code is one of them.
- The `BudgetAllocator` header magic overwritten when `~Context` freed an `Array<AttrValue>` (`harness.hpp:154`).
- UBSan "addition of unsigned offset to 0x7ffc50ef10b0 overflowed" in `Array<u8>::push_back` (`array.hpp:342`).
- The budget refusing "135030 live + 64 requested > 67108864 bytes" (arm A, ceir-text): its own message prints
  numbers for which the refusing comparison is false.
- `check_mutable` in `Array<u32>` (`array.hpp:527`); ASan SEGV in `sv_eq` (`parse.cpp`); ASan
  stack-buffer-overflow in `sv_eq` and in `kir::Shape::Shape()` (`ckir.hpp:820`); ASan SEGV in
  `asan_annotate_live_range` (arm B); two ASan SEGVs whose pc is a stack-like address with no frame in our code;
  one cross-format oracle failure (the committed seed above).

## Does it need libFuzzer?

One slot of 540 s, four processes of `crd-fuzz-ceir-text` at once: two replay-driver loops (the non-libFuzzer
executable of the same build, same runtime and options) over 790 diverse inputs (the 764 units a control process
had grown plus the 26 committed seeds), and two libFuzzer loops restarted after each fault.

| Driver | Executions | Events | Executions per event |
| --- | --- | --- | --- |
| Replay driver | 2,923 passes, about 2.31 M | 130 | about 18 k |
| libFuzzer | 6,110,605 counted (the SIGSEGV runs are not) | 209 | about 29 k |

The replay driver faults too, so **libFuzzer's driver is not needed**. The first report of each event, both
drivers together, is from one family: ASan's own handler faulting ("nested bug in the same thread", 35), ASan
stack-overflow (31), UBSan member calls and accesses on null, misaligned or undersized objects (24), ASan
stack-buffer-underflow (18), unknown-crash (15), budget refusals (14, some naming impossible sizes such as
11,754,707,382,688,512 bytes, some numbers the comparison accepts), `is_pow2(alignment)` again (10), ASan "bad
parameters to `__sanitizer_annotate_contiguous_container`" with an `old_mid` far outside the buffer (6),
container-overflow (4), stack-use-after-return (3), `CRD_FATAL` (3), stack-buffer-overflow (2), the budget header
magic (1), heap-use-after-free (1), the frozen-context and type-id asserts (one each), and a few with no first line.
libFuzzer wrote 178 artifacts (177 crash, 1 out-of-memory).

The rate is not stationary: ceir-text went from about one event per 400 k executions in the control to one per 18 k
to 29 k twenty minutes later, on the same build, inputs and load shape.

## Is it the sanitizer, or WSL?

One slot of 300 s, the same 790 inputs, five loops at once: three in WSL, two native on Windows (the Windows
executables built at 9842b0b4 by the preceding batch).

| Build | Passes | Executions | Events |
| --- | --- | --- | --- |
| `linux-clang-fuzz` (clang 18 ASan+UBSan), positive control | 842 | about 665 k | 10 |
| `linux-gcc-debug` (gcc 13.3, no sanitizer) | 5,923 | about 4.68 M | 2 |
| `linux-gcc-asan` (gcc's ASan+UBSan) | 810 | about 640 k | 0 |
| `win-asan` (MSVC ASan), native | 298 | about 235 k | 0 |
| `win-debug` (MSVC, no sanitizer), native | 298 | about 235 k | 0 |

The unsanitized gcc build faulted twice: one SIGSEGV, and one **wrong result**. The serialize output of an input
that passes on the other 5,922 passes was rejected by the same build's own deserializer ("result type refs overrun
the chunk at byte 432") and the cross-format oracle failed. **The sanitizer runtimes are not needed.** The rate differs
by build: the clang-built executable faults far more often per execution than the others, and the two native Windows
loops were clean. The Windows exposure is small and its executables are different code, so this slot does not
separate the host from the WSL guest.

## Probes that stayed clean

- Compile determinism: four concurrent workers compiled `engine/execution/ceir/src/parse.cpp` with the
  linux-gcc-debug command plus `-frandom-seed=1` for 480 s. All 716 compiles exited 0 with one object hash.
- The preceding batch's known-answer CPU probe (integer hashing, a buffer shuffle, `memcpy`, double arithmetic) was
  clean in WSL and on Windows.
- WSL's kernel log shows each fault as the process's own fatal signal (and `timeout` re-raising it); no other
  message. WSL cannot see machine-check records. The Windows Application log has one other process crash in two
  days (`cl.exe`, `c2.dll`, `c0000005`, 2026-10-10 03:34); the other entries are the repository's deliberate crash
  specimens. WHEA has logged nothing.

The fault does not reach every workload, so a generic stress test that passes says nothing here: the discriminating
run must be these executables.

## What this settles and what it does not

Settled locally:

- The container-annotation path is not the cause: the fault rate is the same with the annotation disabled.
- libFuzzer is not needed, ASan and UBSan are not needed, and no input is needed: every artifact replays clean, one
  equals a committed seed, and an unsanitized build computed a wrong result on an input it handles correctly
  thousands of times.
- The constant-argument assertion shows a value changing between a register and a stack slot of one frame with no
  code of ours in between.

No defect in the engine, the harness or the instruments is indicated. What remains is which part of the machine's
execution environment misbehaves: the CPU (an Intel Core i9-14900K, microcode `0x12F`, BIOS 2001) or the WSL2
guest's kernel and memory path. That needs a run outside this host or outside WSL on this host, which is the
user's. The faults are also now frequent (an event every few seconds in some loops), so local results on this host
are less trustworthy than they were: a fault shows as a failure far more often than as a wrong pass, but the gcc
wrong result shows a silent error is possible. The hosted lanes run on other machines and are unaffected.

## Not adopted

The REPO.DEV.11 rule adopts a libFuzzer finding that does not replay alone, with its repetition evidence. None of
this batch's 191 artifacts is adopted: each replays clean, one is already a committed seed, and the run shows the
fault does not depend on the input, so such an input adds no regression value. The rule now carries a dated
exception in [test instruments](../design/test-instruments.md) for this host until the host question is settled.

## For the user: the other-machine run

On a Linux machine (native, or WSL on a machine other than this one), configure the `linux-clang-fuzz` preset and build
`crd-fuzz-ceir-text` and `crd-fuzz-ceir-text-libfuzzer`. Then run, from the repository root, two of each
loop at once for about nine minutes:

    while true; do build/linux-clang-fuzz/tests/ceir/fuzz/crd-fuzz-ceir-text \
        tests/execution/ceir/fuzz/corpus/text || echo FAULT; done
    while true; do mkdir -p w a; build/linux-clang-fuzz/tests/ceir/fuzz/crd-fuzz-ceir-text-libfuzzer w \
        tests/execution/ceir/fuzz/corpus/text -max_total_time=540 -max_len=65536 -timeout=10 \
        -rss_limit_mb=2048 -malloc_limit_mb=256 -artifact_prefix=a/ || echo FAULT; done

Each loop needs its own `w` and `a` directories. Count the FAULT lines and the executions (`stat::number_of_executed_units`).
At the lowest rate seen here (one event per 400 k ceir-text executions), six million clean executions would happen with
probability below one in a million if the other machine had this host's fault. A clean result there points at
this host; the same faults there point back at the software. Running the same loops on this machine booted into a
native Linux would separate the CPU from the WSL guest.

## Verification

No C++ source, script or workflow changed, so no lane build was needed. From `build/win-debug`: `ctest -R
"^crd-no-|^crd-check"` passed. `check-master-plan.py` and `check-repository.py` pass. All scratch is under
`~/b12` in WSL and the session scratchpad; nothing was left running.

## Not done here

- The other-machine run (user).
- The hosted run of DIAG.10a.
