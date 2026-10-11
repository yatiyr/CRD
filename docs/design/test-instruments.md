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
serializer defect, never a harness opinion. Since DIAG.10a each loader target also crosses to the other form: an
accepted CEIR text module must serialize, deserialize and print back to the same text, an accepted CEIR blob must
print text the parser accepts and prints identically, and an accepted CKIR blob must write `.ckir` text that reads
back to the same blob; the CKIR text target already compared the blobs of the two reads.

| Target | Loader | Corpus (seeds + adopted findings) |
|---|---|---|
| `crd-fuzz-ceir-text` | `crd::ceir::parse` | 27: the rich graph's text, the hand malformed table of `test_malformed.cpp`, eight adopted findings and one input adopted after it crashed only inside a libFuzzer process (DIAG.10a) |
| `crd-fuzz-ceir-binary` | `crd::ceir::deserialize` | 30: the rich graph's blob, truncations, a flipped count, garbage, ten blobs holding a name or value the text form cannot express (DIAG.10a, each checked refused as it is written), ten adopted findings |
| `crd-fuzz-ckir-text` | `crd::kir::ckir_read` | 13: the D1 scale kernel and the raster fragment entry as written, the malformed table of `test_ckir_asset.cpp`, six adopted findings |
| `crd-fuzz-ckir-binary` | `crd::kir::deserialize_graph` | 30: the two graphs' blobs, truncations, a flipped byte, garbage, seven non-canonical records (DIAG.10a: a dim past the rank, a rank past 8, out-of-range op, dtype, tier and stage, a stray stage output; each checked refused), nine adopted findings and one input adopted after it crashed only inside a libFuzzer process |
| `crd-fuzz-perf-bundle` | `crd::perf::import_bundle` (DIAG.5d) | 12: the golden bundle, truncations, traversal, a lying length, a decompression bomb, a wrong schema, a hostile section count, build identities |
| `crd-fuzz-memory-allocators` | every production allocator and the `SlotMap`, as operation sequences, the `VirtualMemoryAllocator` and `StreamingAllocator` on small per-input reservations (DIAG.10a) | 37: 31 written streams (one or more per allocator, malformed lengths and alignments, a stream cut mid-operation, one per finding of the written seeds; for the arena exact exhaustion, commit blocks, purge and recommit, reallocation paths; for the streaming layer the budget charge, unservable requests, policy pressure, a lying policy, staging and the category view) and 6 adopted libFuzzer inputs |
| `crd-fuzz-perf-flight` | `crd::perf::decode_flight_record` (DIAG.10a) | 30: seven captures a real `FlightRecorder` wrote (every end reason, a Basic policy, a suppressed trigger, a replay recipe, a clipped message, an empty post window, the fatal path without the ring) with fixed clock readings, ten records that contradict themselves in one way each, truncations, a trailing byte, a disagreeing count, a text length at its bound, an unknown detail level, garbage |
| `crd-fuzz-perf-assets` | `parse_flight_policy_asset`, `parse_fault_plan_asset`, `parse_fault_plan` and crd-toml beneath them (DIAG.10a) | 41: the two committed assets, documents for every refusal the readers name, crd-toml's nesting, integer-range and number-length limits, plan texts at their bounds |
| `crd-fuzz-perf-capture` | `crd::perf::validate_capture_buffer` and `CaptureView` (DIAG.10a) | 33: three captures in the writer's exact layout (one with frames and a correlation section, one whose frame span overflows a 64-bit difference) and one buffer per reader defect DIAG.10a fixed, truncations, garbage, and one adopted libFuzzer finding (a sample span the Perfetto export overflowed) |
| `crd-fuzz-ceir-record` | `crd::ceir::cook::decode_record`, the run-record reader (DIAG.10a) | 57: eight records in the three recorders' shapes (plan, host and device; a bounded trace, an unavailable read, a reloading host's asset and generation, an ulp and an exact envelope) with a fixed build, 30 records that contradict a recorder in one way each, truncations as given and resealed, a trailing byte, a wrong checksum, an older schema, three counts past the payload, garbage |
| `crd-fuzz-ceridc-diag` | command decoding: ceridc's MCP transport (`mcp_handle` over `crd::assetio::json::parse`), its diag tool and `DiagCommandService::execute` with every registered command's argument check (DIAG.10a) | 52: the JSON-RPC lifecycle, every id form (string, integer, null, fraction, past 2^53, object), batch arrays and bare values, names with a NUL, the parser's number, UTF-8, escape and nesting boundaries (15 refused), the diag tool's fields and refusal order, cursors, a 65-byte command, native requests of raw bytes (a stale cursor before any snapshot, a cursor past 2^53), and the flight, fault and CEIR commands under hosts with and without a root |
| `crd-fuzz-resources-reload` | `crd::resources::crdr_read` and the `ResourceManager` pack ingress: `mount_manifest`, `unmount`, `load_sync`, `reload_mount_now` and eviction (DIAG.10a) | 34: seven packs that mount (honest, compressed manifest, empty, a name past the pool, a zero-length entry, artifacts the loaders refuse, trailing bytes), six refused whole (torn, a range that wraps, a duplicated id, not a PACK, no manifest, a manifest of partial entries), four compressed-chunk claims (past the bound, a sum that wraps, a byte past the frame, exact), three structural, and 14 scripts (reload swaps, a torn and a wrapping reload, a shadowed id, a retyped id reloaded and re-issued, unmount falling back, a reload that drops and adds, a resized reload, a duplicated id on reload, pins and drains, placeholders and last-good, in-place corruption, invalid mounts) |
| `crd-fuzz-ceir-differential` | the compiled execution tier (`crd::ceir::plan::compile` and `run`) against the reference interpreter (`crd::ceir::exec::Interpreter`) on generated programs, with their printed and serialized twins (DIAG.10a) | 35: 32 seeds: straight-line arithmetic over i64 edges, if, for, while, switch and match, the three scope kinds, the three cell kinds with feedback and depths, calls, tokens (launch, spawn, worker, main thread, join, await, race, fiber wait, continuation), host inputs (with a script, exhausted, without a source), map_reduce in index order, and one per finding (a while condition at a small budget, recursion at and past the nesting bound directly and through a map_reduce body, a re-entered map_reduce, a 2^31-index and a full-i64 parallel range, a for step past INT64_MAX, a fold that runs out of steps, a loop that launches tokens and runs a parallel op) and three adopted libFuzzer inputs (a parallel op inside a parallel body; a token store that grew quadratically; a parallel body re-entering the program, its work growing with each nesting level) |

Corpora are raw bytes under `tests/<owner>/fuzz/corpus/<target>/`, `-text -diff -merge` in `.gitattributes` so no
normalization ever rewrites the bytes that reproduced a finding; seeds carry a content-hash name from
`<replay> --seed <dir>`, merged units carry libFuzzer's SHA-1 name. The harness directory `tests/support/fuzz` is a
registered test directory owned by the `ceir`, `ceir-cook`, `ceridc`, `kir`, `perf`, `memory` and `resources` rows,
so a fuzz executable's link set is the target's own module plus core, containers and memory. Every `crd_fuzz_target()` has exactly one
`fuzz.py` entry naming its corpus and build-relative executable, and no entry is stale (`test-repository-tools.py`,
DIAG.10a). The replay driver declares itself headless before the first input (`set_assert_headless`, and on Windows
no abort dialog), so an engine assertion inside a target ends the replay with its evidence instead of waiting on a
modal dialog until the CTest timeout.

The stateful target `crd-fuzz-memory-allocators` (DIAG.10a) is the same harness with a different input shape: the
first byte picks an allocator and its configuration, the rest is an operation stream replayed against the allocator
and an independent reference model. Only contract-valid operations reach a raw allocator (allocation through
`try_allocate`, frees of live blocks, a TLSF reallocate only when a probe shows its fallback cannot reach the fatal
out-of-memory path, strictly increasing ring epochs within K in flight), so a failure is an allocator defect. The
malformed requests the contract covers (zero, near-`SIZE_MAX` and past-the-arena sizes, top-bit and
non-power-of-two alignments) are generated on purpose and must be refused without change, and the checked layers
take the hostile operations they promise to diagnose (double, interior and foreign frees, redzone overruns and
writes into quarantine for the `DiagnosticAllocator`; stale, double-erased and forged handles for the `SlotMap`).
The oracles are the model's: no overlap, alignment, ownership, intact payloads, exact success and failure where the
allocator's capacity model is exact, `validate_structure()`, statistics that return to zero, an offset span that
coalesces back to one region, a parent that gets every byte back, and the same outcome trace on a second run. The
parent is page aligned so placement does not depend on where the system allocator put the arena. The address-space
allocators (2026-10-11) reserve at most 16 MiB per input and release it with the allocator: the
`VirtualMemoryAllocator` is held to an exact model of its bump offset and committed high-water mark (with recommitted
bytes reading zero and the committed tail poisoned under ASan), and the `StreamingAllocator` runs under a residency
policy the input scripts, with every category's charge tracked exactly through `used()`, the budget held on every
success, the retry bound and no policy consultation for a request nothing can serve. Kinds are appended to the enum
so that every committed input keeps the kind its first byte selected.

The evidence-reader targets (DIAG.10a) hold each reader to what its writer can produce. `crd-fuzz-perf-flight`
restates, independently of the decoder, what `FlightRecorder::trigger()`, `build_capture()` and `finish()` guarantee
(the trigger and every suppressed kind armed by the recorded policy, one contiguous run of events across the
trigger, the end reason and truncation derived from the post window, a capture without the ring fatal and empty), and
an accepted record must re-encode to exactly its own bytes. `crd-fuzz-perf-capture` computes the CPROF layout from
the header itself and requires every `CaptureView` accessor to answer from exactly where that layout says, with every
name terminated inside its own field or the blob's strings. `crd-fuzz-perf-assets` offers every input to all three
policy readers: a refusal names its reason and leaves the output untouched, and an accepted policy or plan writes a
canonical document or text that reads back to the same value. Their seed functions check each seed's verdict while
writing it (`VerdictSink`), so a regenerated corpus cannot carry an honest seed the reader now refuses, or a forged
one it now accepts. The perf corpora live in `tests/foundation/perf/fuzz/corpus/<reader>/` (the bundle corpus moved
to `corpus/bundle/`). `crd-fuzz-ceir-record` decodes each input as given and again with its header's payload size and
checksum recomputed, so a mutated payload reaches the payload reader instead of stopping at the checksum. It restates
what `replay.record`, `record_host_run` and the device recorder write (each executor's own fields and errors, never a
cancelled run, each input's state derived from its need, every kept read of an input the program needs, each kernel's
text with its hash), requires an accepted record to re-encode to its own bytes, to name the missing inputs its states
name and to show no divergence from a trace that repeats it, and bounds the decoder's allocations to eight times the
input plus 64 KiB, so a count is checked against the bytes left before anything is reserved for it. It carries the
program blob without reading it: the CRDR reader is `crd-fuzz-ceir-binary`'s. `crd-fuzz-ceridc-diag` reads an input
as a host line (the grant, and an optional file root that does not exist) and request lines: JSON-RPC lines fed to
`mcp_handle` as ceridc's stdio loop would, the diag tool's `arguments` spliced into a fixed envelope (`@`), and native
requests of raw bytes (`!`). A line that could reach one of ceridc's file verbs is not fed: they read and write paths
the input names. An independent iterative RFC 8259 validator (UTF-8 decoded to code points) must agree with the parser
within its documented limits, every reply must be one line of valid UTF-8 JSON that follows the JSON-RPC rules with
the request's id echoed exactly, the diag tool's report must equal the native answer for the page it names byte for
byte, the service must refuse in its documented order without running a handler, and every snapshot is walked to its
end with repeatable cursors. Its seed function lists the seeds the parser refuses and checks each verdict as it writes
it.

`crd-fuzz-resources-reload` (DIAG.10a) drives the cooked-pack ingress through real files: the production entry points
take paths, so each pack an input names is written under the platform temp directory with a per-process name and
removed after the input. An even first byte makes the rest one pack file (read by `crdr_read` directly, then mounted,
its ids loaded, reloaded, evicted, loaded again and unmounted); an odd one makes it a script over two pack files and
one manager. The CRDR layout and the pack rules are restated in the harness (zstd decodes the compressed chunks), and
a model of the manager (live table by newest mount, loaded blocks with state, generation, bytes and resident size,
pins, reload events) is compared with `find_entry`, `entry_count`, `mount_count`, `current_memory_use` and every held
handle after each operation. Two test loaders (one with a placeholder) copy their artifact and refuse one leading byte,
and each requires that only its own type's payloads reach it. Inputs whose compressed chunks claim more than 16 MiB in
all are skipped as out of scope; `poll_hot_reload` is not driven (one-second mtimes), both reload paths sharing
`do_reload_mount`. On Windows Debug builds the replay driver turns a Debug CRT report (heap corruption found at a free)
into an immediate failure instead of a modal dialog.

`crd-fuzz-ceir-differential` (DIAG.10a) compares the two CEIR execution tiers. Every input decodes to a program, never
to a parse failure: a header (one to four functions with their parameter and result counts, a step budget of 64 to
131,072, a host input script or no source at all, the entry arguments) and then each function's body as a stream of op
choices over arith, core control flow and the three cell kinds, `func.call` (recursion included), async and task
tokens, data-parallel bodies and the input ops. The generator stays inside the module-validity set a cook enforces
(the structure, domain, token and borrow verifiers and the recursion policy), so a refused program is out of contract
and not run. Both tiers run each program at the same budget with the same scripted input: they must agree on the error
class (the documented ExecError and RunError pairs), the values, every cell, every map output on success and, on an
error, the stable id of the op it blames. A compile refusal is accepted only for the documented tier difference (a
parallel body that holds a cell or reads host input, refused whole); a reference error with no compiled counterpart is
a finding. The plan run twice, a fresh reference session, and the printed-and-parsed and serialized-and-loaded twins
must give the same outcome. On a failure the program's text is printed with the two outcomes.

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
ordinary route. No `fuzz/pending/` directory remains. Exception (2026-10-11,
[container-overflow control](../sessions/2026-10-11-diag-10a-container-overflow-control.md)): the development host
now faults in sanitized and unsanitized builds, with and without libFuzzer and independent of the input (a
committed seed failed in-process, and an input handled correctly thousands of times produced a wrong result once).
Until a run on another machine settles whether the host is the cause, findings from this host that do not replay
alone are counted and recorded in the session, not adopted: such an input carries no regression value.

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
atomic links are a candidate requiring a memory-order/reclamation proof. A clean suite reports no detected race on
its instrumented executions; it cannot prove absence of races or ordering bugs.

**The hosted lane (DIAG.1b, 2026-10-05).** `linux-clang-tsan` runs in the complete tier, four tests at a time, with
`halt_on_error=1` and a committed suppression file
([tsan-suppressions.txt](../../tests/support/diag/tsan-suppressions.txt)). The file may name only third-party
libraries that are not built with `-fsanitize=thread`: today Mesa lavapipe and the Khronos validation layer, whose
internal ordering TSan cannot see. A report in Cerid code is fixed, never suppressed. The first full run found and
fixed two engine races (the diagnostic snapshots' fiber and counter fields; the frame-history seqlock payload), a
32-bit fiber tag that could be 0, a test that skipped the pool's sanitizer switch protocol, and specimens that assumed
ASan; see the [inventory and triage](../sessions/2026-10-05-clang-werror-and-tsan-inventory.md). The WSL2
address-space note above did not reproduce with clang 18 on the 2026-10 reference host.

## Seeded detector failures (DIAG.10b)

[diag-seeded-faults.py](../../scripts/diag-seeded-faults.py) breaks one instrument at a time and requires the
diagnostic suite to notice. The seeds live in [seeded-faults.json](../../tests/support/diag/seeded-faults.json): each
is a set of exact source edits (each seeded text must occur once in the tree) or extra environment for the test run,
with the lane facts it applies to (`os`, `asan`, `tsan`, `msvc`, ... read from the build tree) and the exact CTest
names that must fail, each with the diagnostic text its failure must print. Per seed the runner builds the targets and
runs the tests unseeded (all must pass), applies the seed, rebuilds and reruns with JUnit output, then restores the
files byte for byte with a newer timestamp, rebuilds and reruns. The verdict comes from the JUnit and the test
output, never from CTest's exit status: a seed that leaves a test passing SURVIVED, one whose test fails without the
named text is a WRONG_FAILURE, a mutant that does not compile is BUILD_FAILED and never a detection, and no test is
rerun. After the restore git must report each edited file exactly as before (an uncommitted change survives). Every
seed names the targets holding its detecting tests, so even an environment seed tests the current tree. Contract and
the classes it must cover: [DIAG.10b](runtime-diagnostics.md#diag-10b). Run it inside the MSVC environment on
Windows, one lane at a time: a seed edits the shared checkout, so a second lane building from it would compile the
seed.

The committed sanitizer suppression files may name only third-party shared libraries (`lib*.so*`): the verdict tests
(`test-diag-seeded-faults.py`, the `crd-diag-seeded-faults` CTest and a repository CI step) refuse any other entry,
so the rule above about Cerid code is enforced, not only stated. A seed may add such an entry for its own run.

## Diagnostic coverage and the nonreproduced-failure register (DIAG.10b)

The coverage configuration is `linux-clang-coverage` (diagnostic tier, never hosted): clang++ with
`CRD_ENABLE_COVERAGE` (`-fprofile-instr-generate -fcoverage-mapping` on every TU, exclusive with the sanitizers and
libFuzzer). [diag-coverage.py](../../scripts/diag-coverage.py) runs the test set named by
[coverage-scope.json](../../tests/support/diag/coverage-scope.json) once (selected CTests plus the Catch2 programs
CTest does not register), requires every selected test to pass in the JUnit, merges the raw profiles and reads
llvm-cov's export of the scope files. It reports regions, branches, functions and lines per scope file with the
denominator stated (scope files, files this lane instrumented, files excluded by path). Percentages are never a
gate. The gate is the outcome sets: every value of a registered result enum must be produced at a site that ran,
and every transition site of a registered state machine must have run, unless an exclusion owns it with a class
(`dying-child`, `windows-only`, `other-configuration`, `resource-failure`, `no-producer`, `default-value`,
`owned-gap`) and a reason. An exclusion that names something that now runs, no longer exists, or calls a site
compiled on this lane `windows-only` is stale and fails the run. A child that dies writes no profile, so paths
reached only inside dying specimens are owned as `dying-child`, never counted as untested. Closing an owned gap
removes its exclusion in the same change; on 2026-10-11 twelve were closed by cases
([session](../sessions/2026-10-11-diag-10b-owned-gaps-closed.md)) and three remain, each waiting for an engine seam.
The harness's own Timeout verdict is exercised with the CPU-hotspot specimen under a 200 ms bound.

[nonreproduced-failures.json](../../tests/support/diag/nonreproduced-failures.json) registers flaky and
nonreproduced failures: first seen, lane, target, signature, reruns, status (`nonreproduced`, `explained`,
`toolchain`, `host`), explanation and session. It is evidence only. The verdict tests (`test-diag-coverage.py`, the
`crd-diag-coverage` CTest and a repository CI step) hold every entry complete and fail if any build file, CI step or
script reads it, so it can never select, skip or retry a test.

## Not in scope

REPO.DEV.9 remains the bounded ingestion baseline. DIAG now owns the immediate diagnostic extensions above;
clang++ as a full Linux compiler and not-yet-existing product consumers retain their respective qualification rows.
The original templates/routes contribution work remains REPO.DEV.10.
