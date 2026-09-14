# Test instruments: bounded ingestion fuzzing and the sanitizer model of the fiber scheduler

<!-- doc-role: reference -->
> Contract for REPO.DEV.9. Status lives only in [ROADMAP](../ROADMAP.md#slice-repo.dev.9); the accepted research is
> [public consumption and stronger instruments](../research/2026-09-12-large-cpp-development-and-ci.md#public-consumption-and-stronger-instruments);
> the harness is [tests/support/fuzz](../../tests/support/fuzz/include/crd/fuzz/harness.hpp) with
> [CrdFuzz.cmake](../../cmake/CrdFuzz.cmake) and [fuzz.py](../../scripts/fuzz.py); the sanitizer model is
> [sanitizer_fibers.hpp](../../engine/foundation/jobs/src/sanitizer_fibers.hpp).

**2026-09-14 extension:** [DIAG](runtime-diagnostics.md) owns the next instrument programme after REPO.DEV.
The measurements below are dated evidence. TSan switch flags and the instrument-only fence substitution require
negative-control/model qualification in DIAG.1b; a clean probe does not prove annotations cannot conceal races.
DG05's immediate free-list repair transfers from CORE-USE.2 to DIAG.1a. DIAG.3f owns MSVC UAR and initialization/leak
coverage; DIAG.10a/DIAG.11b own expanded fuzzing/CI. Existing REPO.DEV evidence is preserved.

Purpose: the two existing ingestion surfaces (CEIR text and binary modules, CKIR text and binary graphs) have
reusable, bounded fuzz targets whose corpus is a permanent regression set on every lane; every sanitizer the tree
offers requires qualification rather than assumption, so a lane cannot print a report and stay green; fiber stack
coverage and concurrency annotations require explicit controls before any lane claims
concurrency coverage. The row qualifies instruments and the current consumers; a defect it finds in an allocator or
the scheduler gets an owner row, not a repair here.

## The bounded fuzz harness

A fuzz target is one translation unit defining `LLVMFuzzerTestOneInput` and `crd_fuzz_seeds`. `crd_fuzz_target()`
builds it twice: the replay executable (the target plus `replay_main.cpp`, which walks corpus directories, feeds
every file to the target, reports the count, the bytes, the inputs the target rejected and the slowest input, and
exits non-zero on any failure or an empty corpus) is registered as the `<target>-corpus` CTest of every
configuration; under `CRD_ENABLE_FUZZER` the same source also links libFuzzer as `<target>-libfuzzer`.

Bounds: a target returns early above `kMaxInputBytes` (64 KiB); each input gets a fresh `BudgetAllocator` (64 MiB)
whose allocation beyond the budget ends in a fatal that names the budget, a reported finding with a small footprint
instead of an RSS kill; the fuzzing side adds libFuzzer's `-max_len`, `-timeout`, `-rss_limit_mb` and
`-malloc_limit_mb` through `scripts/fuzz.py`. Oracles: a rejected input must leave nothing behind; an accepted input
must satisfy the round-trip the unit tests already hold (`print(parse(print(x))) == print(x)`, byte-exact binary
serialization, `serialize_graph(ckir_read(ckir_write(g))) == serialize_graph(g)`), so a fuzz finding is a loader or
serializer defect, never a harness opinion.

| Target | Loader | Corpus (seeds + adopted findings) |
|---|---|---|
| `crd-fuzz-ceir-text` | `crd::ceir::parse` | 24: the rich graph's text, the hand malformed table of `test_malformed.cpp`, six adopted findings |
| `crd-fuzz-ceir-binary` | `crd::ceir::deserialize` | 16: the rich graph's blob, truncations, a flipped count, garbage, six adopted findings |
| `crd-fuzz-ckir-text` | `crd::kir::ckir_read` | 11: the D1 scale kernel and the raster fragment entry as written, the malformed table of `test_ckir_asset.cpp`, four adopted findings |
| `crd-fuzz-ckir-binary` | `crd::kir::deserialize_graph` | 18: the two graphs' blobs, truncations, a flipped byte, garbage, five adopted findings |

Corpora are raw bytes under `tests/<owner>/fuzz/corpus/<target>/`, `-text -diff -merge` in `.gitattributes` so no
normalization ever rewrites the bytes that reproduced a finding; seeds carry a content-hash name from
`<replay> --seed <dir>`, merged units carry libFuzzer's SHA-1 name. The harness directory `tests/support/fuzz` is a
registered test directory owned by the `ceir` and `kir` rows, so a fuzz executable's link set is the target's own
module plus core, containers and memory.

`scripts/fuzz.py` never builds: `run` fuzzes a working copy of the committed corpus for `-max_total_time` seconds
with the limits above, artifacts (`crash-*`, `timeout-*`, `oom-*`, `leak-*`) under scratch, and writes the final
statistics as a board (executions, executions per second, new units, peak RSS, artifacts); `merge` adds to the
committed corpus only the units that add coverage (`-merge=1`); `minimize` shrinks an artifact
(`-minimize_crash=1`); `adopt` copies a minimized artifact into the committed corpus under its SHA-1 name; `replay`
runs the replay executable verbosely. Findings policy: a crasher fixed in the loader is adopted as a passing
regression; one whose repair is out of scope is committed as raw bytes under the owner's `fuzz/pending/<target>/`
directory, which no replay test reads, and is listed with its symptom and owner in the session record, so it is
never a committed red test and never a silent skip. The harness ends every failure with an abort that carries no
Debug CRT dialog on Windows, so a failing replay CTest fails instead of hanging the lane (proved on MSVC 19.51:
the round-four CKIR text inputs, before their fix, ended `crd-fuzz-ckir-text.exe` with exit code 3 in under a
second); the CEIR text and
CKIR text targets print the parser's reason and offset before the abort when the printer's own output is rejected.

The fuzz configuration is `linux-clang-fuzz` (diagnostic tier, never hosted): clang++ with ASan and UBSan,
`CRD_ENABLE_FUZZER` adding `-fsanitize=fuzzer-no-link` to the tree, and `CRD_WARNINGS_AS_ERRORS=OFF`, because
clang++ 18 under the GCC warning set fails 139 translation units of the ceir and kir closure (323
`-Wsign-conversion` sites, almost all in `ckir_kernel_eval.hpp`, `ckir_glsl.hpp`, `ckir_hlsl.hpp` and `ckir_rt.hpp`;
`-Wnested-anon-types` on the `String` small-buffer union in every unit; 13 `-Wdouble-promotion`). The diagnostic
build shows those warnings instead of failing on them, the way `win-tidy-local` previews clang-tidy; every qualified
configuration keeps warnings as errors, and the default graphs are unchanged (the flag is the same string). clang++
on Linux is not a qualified compiler of this tree; that audit belongs to the platform/preset matrix row
(REPO.DEV.11). clang-cl on Windows was tried first and dropped: its ASan has no debug-CRT runtime and, linked
through lld-link, its objects and runtime disagree on the runtime library.

## What the first bounded runs found

Four rounds on the `linux-clang-fuzz` build (clang 18, ASan+UBSan, two workers per target, 600 s then 300 s,
300 s and 120 s per target; every round ended early on the first artifacts of a worker):

| Round | ceir-text | ceir-binary | ckir-text | ckir-binary |
|---|---|---|---|---|
| 1 | 1.29 M executions at 64 k/s, 2 artifacts in 26 s | 13 k, 2 artifacts in 0.3 s | 182 k at 182 k/s, 1 artifact in 1.3 s | (interrupted by the rebuild) |
| 2 | 1.56 M at 55 k/s, 2 in 36 s | 676 k at 78 k/s, 2 in 8 s | 1.58 M at 124 k/s, 2 in 12 s | 668, 2 in 0.1 s |
| 3 | 1.98 M at 61 k/s, 2 in 46 s | 108 k, 2 in 1.7 s | 6.83 M at 96 k/s, 1 in 288 s | 2.90 M at 143 k/s, 2 in 41 s |
| 4 | 153 k, 2 in 3 s | 1.41 M at 94 k/s, 2 in 23 s | 7.96 M at 77 k/s, 2 in 111 s | 440 k at 168 k/s, 2 in 3 s |

Rounds 1 to 3 produced 21 artifacts, all adopted into the corpora. Twelve were root-caused to nine loader defects,
each fixed as a rejection: a signed overflow in the CEIR exponent parser; an operation name longer than the interning buffer (an assert
instead of a rejection, guarded in both CEIR loaders through `Context::kMaxOpNameBytes`); an empty attribute
name in the CEIR binary decoder; an attribute dict whose first name is quoted, which the printer emits and the
parser's brace lookahead took for a region; region nesting without a depth cap in both CEIR loaders; a CKIR
text entry whose `n_out` disagreed with its `[[out]]` blocks; a CKIR blob whose input count exceeded its node
array; the CKIR blob reader validating none of the bounds the text reader validates (now the same checks); and
unbounded integer accumulation in the CKIR token and node-ref readers. The other nine have no established cause.
Six never reproduced in isolation (single-input and whole-corpus replays on both hosts): three round-two CEIR
reports (an unknown-crash in the text parser's keyword comparison, a stack-buffer-underflow and a `string_view`
with insufficient space in the binary decoder) and three round-three CKIR reports (a misaligned `string_view` in
the text reader, a misaligned cursor and a stack-buffer-underflow in the blob reader), the CKIR three found by a
build that already carried every CKIR fix. Three round-three CEIR reports (an unknown-crash under `parse_type`, a
span offset overflow and a jump to address zero in the binary decoder) replay clean after the region caps and
the lookahead fix without a traced cause. An adopted artifact with an unknown cause is still a regression input:
it replays clean on both hosts and a future failure names the input. Round 4's eight artifacts were resolved by
the [REPO.DEV.11 audit](../sessions/2026-09-14-infrastructure-audit.md): the two CKIR text reports were
deterministic writer/reader identity gaps and are fixed (the writer elided a `-0.0` constant through a `!= 0.0`
test, so the sign bit died in the text form while the blob kept it; a `[[out]]` block without `node =` left a
stage output at `-1`, which the reader accepted and the writer emitted as `"n-1"`; both the text and the blob
reader now reject an output that names no node). The other six (two CEIR text reports shaped like stack
overflows, two CEIR binary dangling-view reports, two CKIR blob cursor reports) did not reproduce on a fresh
clang 18 ASan+UBSan build, alone or over 5,000 in-process repetitions each, and were adopted as corpus inputs.
The policy that follows: a libFuzzer-process finding that does not replay in isolation is adopted, counted and
recorded with its repetition evidence; a recurrence with a reproducing artifact gets an owner through the
ordinary route. No `fuzz/pending/` directory remains.

Every adopted artifact replays clean on MSVC 19.51 (`win-debug`), clang 18 (ASan+UBSan) and GCC 13.3
(`linux-gcc-asan`, UBSan without recovery, `-Werror`), and the CEIR and CKIR unit suites (534 and 311 cases) pass
with the fixes on all three; one whole-suite GCC ASan run ended in an unrelated evaluator `SEGV` that four further
runs did not repeat, recorded in the session record and closed by the
[REPO.DEV.11 audit](../sessions/2026-09-14-infrastructure-audit.md) as not reproduced (one observation, five
clean whole-suite runs, no defect established; the hosted ASan lane runs every case in its own process).

## The sanitizer audit

**UBSan recovered.** `-fsanitize=undefined` alone prints a report and continues, so the Linux ASan+UBSan lane could
report undefined behaviour and stay green. The hosted `linux-gcc-asan` job log of run 34780682504 (15,918 lines)
carries no `runtime error:` line, so the lane now compiles and links with `-fno-sanitize-recover=undefined` and a
report fails the run. In the same policy an instrument the toolchain cannot provide fails the configure instead of
warning: `CRD_ENABLE_UBSAN` on MSVC, `CRD_ENABLE_TSAN` on MSVC or together with ASan, `CRD_ENABLE_FUZZER` without
clang.

**ASan and the fiber stacks.** The scheduler switches stacks in hand-written assembly at three sites (a fiber
parking in `counter_wait`, a fiber completing in `job_fiber_trampoline`, the scheduler dispatching or resuming a
fiber in `run_job_in_fiber`). `sanitizer_fibers.hpp` gives each site `__sanitizer_start_switch_fiber` before and
`__sanitizer_finish_switch_fiber` after the switch and the trampoline's first instruction a finish call, so ASan's
notion of the current stack, its bounds and the fake stack of `detect_stack_use_after_return` follow the fiber; the
scheduler side keeps its state in the dispatching frame, the fiber keeps its own (the fake stack it left, the
scheduler stack it returns to) in the `Fiber` record because it may resume on another thread. The helpers are empty
unless the compiler defines the sanitizer, and `-DCRD_JOBS_SANITIZER_FIBERS=0` reproduces the unannotated model for
the audit. Measured on `crd-jobs-tests` and a scratch probe, annotated and unannotated, under `win-asan`
(MSVC 19.51) and `linux-gcc-asan` (GCC 13.3, WSL2):

| Probe | MSVC annotated / unannotated | GCC annotated / unannotated |
|---|---|---|
| `crd-jobs-tests` | 22 of 22 / 22 of 22 | 22 of 22 / 22 of 22 |
| one byte past a stack buffer on a fiber (through a pointer) | stack-buffer-overflow naming the fiber frame / same | same / same (an array subscript is caught by UBSan's bounds check first) |
| use after return on a fiber (`detect_stack_use_after_return=1`) | no report / no report | stack-use-after-return / same |
| 2,000+ nested waits across four threads | clean / clean | clean / clean |

The probes show no behavioural difference between the annotated and the unannotated model on these two compilers;
the annotations stay because they are the documented contract that makes the bounds and the fake-stack ownership
explicit per fiber, and they cost nothing uninstrumented. MSVC's use-after-return detection needs its own compile
switch (`/fsanitize-address-use-after-return`), which no preset sets: on `win-asan` that class is not exercised,
and the table says so rather than claiming it.

**ThreadSanitizer and the fiber model.** `CRD_ENABLE_TSAN` (GCC or clang, exclusive with ASan) builds with
`-fsanitize=thread`; every fiber stack gets a TSan context at pool init (`__tsan_create_fiber`), every switch
announces its target immediately before it (`__tsan_switch_to_fiber`, flags 0: the switch is a synchronization
point on the switching thread, cross-thread ordering being the work queues' acquire/release, which the switch
composes with), the contexts are destroyed at shutdown, and the work-stealing deque's standalone seq_cst fence,
which GCC's TSan cannot model (`-Wtsan`), becomes a seq_cst read-modify-write on the same location under the
instrument only. On this WSL2 kernel TSan aborts with "unexpected memory mapping" unless address-space
randomization is off for the process (`setarch x86_64 -R`, no root needed; the sysctl is unreadable there).

The recorded probe: the switch path itself produced no report in 22 tests and a 2,048-wait probe. This does not prove
the model preserves all required race visibility; DIAG.1b supplies that qualification. The recorded reports are
outside the switch path: two lock-free free-lists read the `next_free` link of the head element outside the
compare-exchange that validates it (`fiber_pool.cpp` `release_to`:296 against `acquire_from`:239; `counter.cpp`
`CounterPool::acquire`:66 and :75 against its release), a plain read racing a plain write, benign in practice by the
code's own argument and a data race by the language. Five of the 22 jobs tests fail on those reports (16 in the
suite, 68 in the probe, no other site). The immediate repair now belongs to [DIAG.1a](../ROADMAP.md#slice-diag.1a);
atomic links are a candidate requiring a memory-order/reclamation proof. No TSan preset is qualified yet. A clean
suite reports no detected race on its instrumented executions; it cannot prove absence of races or ordering bugs.

## Not in scope

REPO.DEV.9 remains the bounded ingestion baseline. DIAG now owns the immediate diagnostic extensions above;
clang++ as a full Linux compiler and not-yet-existing product consumers retain their respective qualification rows.
The original templates/routes contribution work remains REPO.DEV.10.
