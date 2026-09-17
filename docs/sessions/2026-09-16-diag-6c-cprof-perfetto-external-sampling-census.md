<!-- doc-role: historical -->
# DIAG.6c census — CPROF interoperability, Perfetto export, verified external sampling/unwind

Row 079 (DIAG.6c). This is the **(a) census** tick: no engine code. It extracts the acceptance clauses verbatim,
surveys what already exists, records the choice points, and enumerates the sub-units with each one's gate lane, so
6c's eventual flip is auditable the way 6b's clause table made 6b's auditable. Row 079 stays **Open**.

## Acceptance clauses (verbatim, `design/runtime-diagnostics.md#diag-6c` + ADR-0133 DG14 / ID-1)

Design goals: version CPROF without silently changing pinned layouts; a **bounded Perfetto-compatible export**
preserving task flows, counters, clock domains, loss and source identity; native viewer stays optional; **verified
WPR/perf sampling** and platform-debugger recipes for uninstrumented work / blocked time / memory pressure; external
tool deps stay optional and exported data must be **usable offline without a service account**; where supported,
include context switches, page faults, cache/branch counters, lock contention, CPU/NUMA affinity, and **record
permissions / multiplexing / missing PMU events rather than treating unavailable counters as zero**; profile frame
pacing / input-to-present **separately** from offline throughput and shader execution time.

Acceptance (the pass/fail gate):
1. **Export/import a real capture**, inspect the **same critical path in native and external views**, and **reject
   schema/endianness/size mismatches**.
2. **Qualify optimized/fiber stack unwinding, or explicitly mark missing stacks.**
3. **A known unscoped CPU hotspot is visible through sampling; scope-only charts do NOT satisfy this case.**

ADR ID-1: schema `cerid-diagnostics/1`, a bounded versioned POD-pinned format like CPROF capture — a layout change
**bumps N and ships a reader for N-1**; an offline export **declares the schema version it wrote**. The unified
crash+symbol+generation+capture bundle is `unsupported` today (DG14), built by DIAG.5d/6c.

## What already exists (survey)

| Area | State | Evidence |
|---|---|---|
| CPROF v1 format | present | `capture.hpp`: FourCC `'CPRO'`, `kCprofVersion=1`, `flags` u64 (6b(b) set a correlation-section bit; version stayed 1 so old readers skip it); `CprofHeader` pins `sample_struct_size`/`frame_record_size`/`counter_count`; `CounterMeta` 64 B on-disk pin; LE-only (documented; BE unsupported) |
| Writer | present | `save_capture_to_buffer(alloc) -> crd::containers::Array<crd::u8>` (owned buffer; the type an export reuses), `save_capture_to_file`, `validate_capture_buffer(ConstSpan<u8>)` |
| Reader | present | `CaptureView` parses once, `is_valid()` false on **any size/magic/version mismatch**; read-only accessors mirror the live profiler's introspection API (same shape the UI panel renders) |
| Mismatch rejection (clause 1, part) | **tested** | `test_capture_roundtrip.cpp`: `validate_capture_buffer rejects bogus inputs` — too-tiny buffer, wrong magic, broken version; header pins struct sizes → **size mismatch** is a struct-size check. **Endianness**: a BE-written buffer byte-swaps the magic → magic mismatch → rejected; covered implicitly by the wrong-magic case, no explicit BE test (BE unsupported by design) |
| Forward compat (N-1 reader) | **mechanism only** | only v1 exists; `CaptureView` rejects an unknown version cleanly (never silently misreads). The ADR's "ship a reader for N-1 on a bump" has nothing to read yet — the discipline is in place, the second-direction reader is future-on-first-bump |
| JSON precedent | present | `diagnostics.hpp/.cpp` `to_json(DiagnosticEvent&, cont::String&)` + `append_json_escaped` — deterministic key order, container-safe, appends into `cont::String`. This is the pattern a Perfetto JSON export follows (NOT itself a capture export) |
| Perfetto / chrome-trace export | **absent** | no `perfetto` / `traceEvents` / `displayTimeUnit` anywhere in `engine/foundation/perf` or `tools/` |
| `.cprof` / export CLI | **absent** | `tools/` = asset_cooker, ceir_capability_matrix, ceir_opgen, ceridc, kir-autotune, shader-cook — none touches captures |
| Symbol side of external unwind | present (5d) | `symbol_index.hpp` (DIAG.5d(c)): per-module binary identity — Windows PDB RSDS GUID+age, Linux ELF GNU build-id — + loaded range + debug-file name, so an offline symbolizer finds the MATCHING debug file and rejects a wrong one. Also `bundle.hpp`/`bundle_import.hpp`/`bundle_manifest.hpp` |
| PDB in shipping | present (observed) | `/Zi` is an explicit shipping flag — `cmake/CrdBuildPerformance.cmake:24` ("Shipping keeps its explicit /Zi PDB and LTCG uncached"); not merely displayName text |
| WPR/perf sampling runbook | **absent** | `scripts/` has only `native_build_profiles.py` / `test-native-build-profiles.py` (build profiling, not runtime sampling) |
| Rich counters (ctx-switch / page-fault / PMU / lock / NUMA) | **absent in-process** | `capture.hpp` has a generic `CounterMeta` + per-frame counters, but no context-switch/page-fault/PMU/affinity fields — these are exactly what **external** sampling (ETW/WPR) adds, not the in-process scope profiler |

## Environment discovery — external samplers ARE on this box

Like the real GPU in 6b, the external-tool proofs are **not** env-gated here:
- `wpr` — `C:\WINDOWS\system32\wpr` (Windows Performance Recorder)
- `xperf`, `wpaexporter` — `C:\Program Files (x86)\Windows Kits\10\Windows Performance Toolkit\`
- `tracerpt` — `C:\WINDOWS\system32\tracerpt`
- `perf` (Linux) / `tracelog` — **not** present.
- Debuggers: **`lldb`** present (`C:\LLVM-20.1.8\bin\lldb`); `cdb`/`windbg`/`gdb` **not** on PATH (WinDbg/cdb need the
  Windows Debugging Tools install — env-gated; gdb is Linux).
- **Elevation caveat:** `wpr -start` (and most `xperf` kernel providers) require an **elevated** shell; the DIAG loop's
  Bash is not necessarily Administrator. Sub-unit (f) MUST detect the access denial and record it as a **permission
  failure**, never as an empty/zero trace — this is the design's own "record permissions … rather than treating
  unavailable as zero" clause applied to our own runbook.

So the WPR/xperf sampling + unwind sub-units get a **real, observed** run on this box (record a trace of a sandbox
run, confirm an unscoped hotspot is visible, unwind optimized frames via PDB) **where elevation permits** — otherwise
a recorded permission failure, not a compile-only artifact and not a silent skip. The Linux `perf` path stays a
documented runbook + CI-lane note (no `perf` here).

## Choice points (record now, decide when the sub-unit lands — not this tick)

- **Perfetto wire format: JSON trace-events vs protobuf.** The design doc does NOT pin it. JSON (chrome `traceEvents`)
  loads directly in `ui.perfetto.dev` offline with **no dependency and no service account** and matches the existing
  `to_json` precedent; protobuf needs a dependency or hand-rolled varints. The "usable offline without a service
  account" clause favors JSON. Do not decide here.
- **Export buffer type.** Reuse `crd::containers::Array<crd::u8>` (writer's return) or `cont::String` (the `to_json`
  precedent) — no `std::string`/`ostringstream`. Whichever, container-safe and bounded.
- **Fiber unwinding.** x64 uses `.pdata`/`.xdata` unwind tables (frame pointers irrelevant), so WPA/xperf can unwind
  optimized native frames via PDB. crd-jobs **fibers** are the hard case the acceptance calls out — qualify them or
  **explicitly mark missing stacks** (never silently drop).

## Sub-unit enumeration (each with its gate lane; order is a plan, re-confirm per tick)

- **(a) census** — this doc. (done)
- **(b) CPROF version discipline** — explicit forward-compat: a test that a v2-tagged buffer is rejected cleanly by
  the v1 reader; an explicit BE-magic rejection test; document the "bump N + ship N-1 reader" procedure. Lane: win-debug + win-asan.
  *(landed — substance reframed from re-testing rejects to field-by-field `offsetof` layout pins, the real "no silent
  layout change" mechanism; see `## (b) landed`.)*
- **(c) Perfetto export** — bounded chrome-trace JSON from a `CaptureView` (scopes→duration events, counters→counter
  events, clock domains + loss + source identity in args), into a container buffer, no service account. Lane: win-debug + win-asan; win-shipping/clang-cl-shipping compile.
- **(d) export/import acceptance** — export a real capture; assert the **same** critical-path scope appears in the
  native `CaptureView` and in the exported Perfetto JSON (parse it back / structural check). Lane: win-debug.
- **(e) export CLI** — a `tools/` command: `.cprof` → Perfetto JSON, offline. Lane: win-debug build + a smoke run.
- **(f) WPR sampling runbook (REAL on this box)** — a `scripts/` recorder + contract doc; record a sandbox run with
  `wpr`/`xperf`, confirm a **known unscoped hotspot** is visible through sampling (scope-only charts fail the case).
  Lane: **observed on this box** (wpr/xperf present) + Linux `perf` documented/CI-noted.
- **(g) optimized/fiber unwind qualification** — WPA/xperf unwinds optimized native frames via PDB (5d symbol
  identity feeds matching); qualify crd-jobs fiber stacks or explicitly mark them missing. Lane: observed on this box.
- **(h) rich counters "where supported"** — ctx-switch / page-fault / (PMU where permitted) / lock / NUMA via ETW,
  recording permissions/multiplexing/missing-PMU rather than zero. Lane: observed-where-permitted + documented.
- **(i) frame pacing / input-to-present** — profiled **separately** from offline throughput + shader execution time.
  Lane: win-debug (may reuse existing present-timing; census-flag overlap in (i)'s own tick).
- **(j) platform debugger recipes** — the design's "platform debugger recipes to inspect uninstrumented work, blocked
  time and memory pressure" goal: WinDbg/cdb (`!runaway`, `!analyze -v`, wait-chain for blocked time; `!address`/`!heap`
  for memory pressure) + gdb/lldb equivalents. Lane: **lldb observed on this box**; WinDbg/cdb documented + observed
  only if the Debugging Tools are installed (check `cdb`/`windbg` on PATH in that tick — do NOT assume); gdb documented.

**Cross-cutting constraint (from "native viewer stays optional"):** the Perfetto export (c) and its CLI (e) must NOT
link `crd-perf-ui` — offline export is independent of the native viewer.

## This tick's gates

Census only — no code, no build. `check-master-plan.py` + `check-repository.py` must PASS (new doc + row-079 note).
Row 079 stays **Open** (DIAG.0-gated batch; a slice flips only when FULLY done).

## (b) landed — CPROF version discipline

The substance of "version CPROF without silently changing pinned layouts": the on-disk layout is now pinned
**field-by-field**, so a same-size reorder (which the existing `sizeof` pins miss) breaks the build before it can
change bytes on disk.

- **Layout pins** — `tests/foundation/perf/test_diag_cprof_version.cpp` carries file-scope, init-free
  `static_assert(offsetof(...))` + `sizeof` for **every field of every on-disk struct**: `CprofHeader` (13 fields),
  `ThreadHeader`, `CounterMeta`, `AllocatorMeta`, `Sample`, `CorrelationRecord`, `FrameRecord` (array offsets
  expressed in the same `kMaxCounters` knob the header `sizeof` pin uses). Teeth: flipping
  `offsetof(CprofHeader, version)` 4→5 produced `error C2338: static assertion failed: 'CprofHeader.version @4'`
  (observed, restored, lane rebuilt).
- **Reader behavior** — four cases, tag `[perf][diag][cprof-version]`, each self-teething (baseline valid → mutate →
  reject → restore → valid): unknown version (v0 **and** v2) rejected by BOTH `validate_capture_buffer` and
  `CaptureView::is_valid`; a byte-swapped (big-endian) magic rejected (documents the LE-only decision — endianness
  falls out of the magic check); an **unknown flag bit tolerated** (only `kCprofFlagCorrelation` is consulted → new
  files stay readable by old readers; the reader must not silently tighten this into a rejection); a pinned
  struct-size mismatch (`sample_struct_size`/`frame_record_size`) rejected.
- **Procedure note** at `kCprofVersion` (`capture.hpp`): the flag-bit trick works ONLY for a purely additive section,
  and 6b(b) already consumed `CprofHeader`'s last spare slot (`correlation_section_offset`, the former `_pad_a`) — a
  SECOND optional section forces a **v2 bump** (or a section-table redesign). On a bump: raise `kCprofVersion`, add a
  version switch, keep a tested N-1 reader (ADR-0133 ID-1); an export declares the version it wrote.

This unit changes **zero bytes** of the format (compile-time pins + a comment + tests only).

**Gates.** win-debug `crd-perf-tests` full **1256/196** (was 1234/192; +22 assertions / +4 cases, no regressions);
`[cprof-version]` 22/4; win-asan `[cprof-version]` 22/4 clean (inside vcvars); win-shipping `/W4 /WX` compiles
`crd-perf-tests` clean; `win-clang-cl-shipping` compiles **my** TU clean (`test_diag_cprof_version.cpp.obj` builds).

**Discovered pre-existing blocker (NOT 6c(b), out of scope — flagged for a separate session).** Building
`crd-perf-tests` under `win-clang-cl-shipping` fails on a dependency, `crash_capture_specimen.cpp` (DIAG.5d):
`error: all paths through this function will call itself [-Werror,-Winfinite-recursion]`. The specimen's intentional
recursion is suppressed via `_MSC_VER` (`#pragma warning(disable:4717)`) / `__GNUC__` pragmas, but clang-cl defines
`_MSC_VER` + `__clang__` (not `__GNUC__`), so it takes the MSVC branch whose 4717 disable does not silence clang's
`-Winfinite-recursion`. Fix shape: a `#if defined(__clang__)` branch first with `#pragma clang diagnostic ignored
"-Winfinite-recursion"`. This is part of `main`'s already-broad RED CI (the user's push/hardening domain); it does not
gate 6c(b) (my code compiles clean on that lane) but WILL need fixing before any crd-perf-tests-dependent clang-cl-shipping flip.

## (c) landed — Perfetto trace-events JSON export

`crd::perf::export_perfetto_json(const CaptureView&, cont::String&, PerfettoExportStats&)` (+ a `_to_file` twin) in
`crd-perf` (`include/crd/perf/capture_export.hpp` + `src/capture_export.cpp`) renders any loaded CPROF capture as a
Chrome/Perfetto **trace-events JSON** document. Not a new module; the native viewer (`crd-perf-ui`) is NOT a
dependency of this path (native viewer stays optional).

**Wire-format decision (closes the (a) choice point): JSON trace-events, not protobuf.** The "usable offline without
a service account" + "external tool deps stay optional" clauses decide it — chrome `traceEvents` JSON loads directly
in `ui.perfetto.dev` with zero dependency and no service account, and reuses the existing `to_json` precedent.
Protobuf would add a dependency or hand-rolled varints for no offline benefit.

**Event mapping:**

| CPROF | trace event | notes |
|---|---|---|
| scope Sample (CPU / calibrated GPU) | `ph:"X"` on `pid 0`, `tid`=thread index | ts/dur in **microseconds** (ns/1000 with a 3-digit fraction) |
| scope Sample (uncalibrated GPU: correlation `clock_domain != CPU`) | `ph:"X"` on `pid 1+device`, `tid`=queue | ts/dur = the **RAW tick values as-is**, never rescaled into the CPU timeline; process name says "raw ticks (uncalibrated)" |
| per-frame counter | `ph:"C"` on `pid 0` at `frame_end` | value typed by `CounterType` (I64/DurationNs -> integer, F64 -> fixed-point) |
| frame | `ph:"X"` on a reserved `tid` ("frames") | frame_begin..frame_end |
| ThreadHeader.dropped_count | `ph:"i"` instant + `dropped` in the thread_name args | loss never hidden |
| every used (pid,tid) | `ph:"M"` process_name / thread_name | — |
| document | top-level `metadata` | `exporter`/`cprof_version`/`captured_at_ns`/`schema:"cerid-diagnostics/1"` (ADR-0133 ID-1: an export declares what it wrote) |

**Locale safety:** every number is formatted by integer arithmetic (`json_writer.hpp`), never `printf`/`%f`. This box
is tr-TR (comma-decimal) Windows; a `%f` would have emitted `,` and produced invalid JSON found only in Perfetto.

**Refactor (zero behavior change):** the anon-namespace JSON helpers (`append_json_escaped`, `append_u64`) were
hoisted from `diagnostics.cpp` to a shared internal `src/json_writer.hpp` (`crd::perf::detail`, inline) so the two
serializers don't duplicate them; the full suite (1280/197, incl. `test_diagnostics`) covers the move.

**Real Perfetto-compatibility evidence (observed, not asserted).** The test writes the document to
`CRD_PERFETTO_EXPORT_OUT` (opt-in; assertions run regardless) and an external `python -c "json.load(...)"` gate on
this box reported, verbatim:
`valid JSON; events= 10 · ph set= ['C', 'M', 'X'] · all ts numeric= True · pids= [0, 8] ·`
`metadata= {'exporter':'crd-perf','cprof_version':1,'captured_at_ns':843288188130300,'schema':'cerid-diagnostics/1'} · displayTimeUnit= ns`
(pid 8 = uncalibrated GPU device 7 on its own track; opening it in the Perfetto UI is (d)). Teeth: forcing raw-GPU
samples onto pid 0 made the `"pid":8` assertion fail (observed, restored, lane rebuilt).

**Env read:** `std::getenv` is C4996 under MSVC `/WX`; used `_dupenv_s` on MSVC / `getenv` elsewhere (local, PCH-safe)
rather than a target-wide `_CRT_SECURE_NO_WARNINGS` (broad + PCH-fragile on this shared test exe).

**Named follow-ups (not (c)):** task **flows** -- the pinned 32 B `Sample` carries no parent link, so `s`/`f` flow
arrows need a producer (fiber_id / begin!=end thread are preserved in args meanwhile); opening in the Perfetto UI and
asserting native==external critical path is **(d)**.

**Gates.** win-debug `crd-perf-tests` full **1280/197** (was 1256/196; +24 assertions / +1 case, no regressions);
`[perfetto]` 24/1 on win-debug + win-asan; win-shipping `/W4 /WX` full link clean (perf on -- the exporter body is
compiled and warning-clean); `win-clang-cl-shipping` **`crd-perf` clean (exit 0)** -- the `#else` stub + the
diagnostics refactor. The test TU cannot be verified under clang-cl-shipping while the specimen blocks the target
(below); perf-off it is an empty TU, and its body is covered warning-clean by MSVC `/W4 /WX`. container check + both
validators PASS.

**Flip-blocking blocker (pre-existing, not 6c).** `crash_capture_specimen.cpp` (DIAG.5d) fails `-Winfinite-recursion`
`/WX` under clang-cl, and CMake makes the specimen exes **order-only prerequisites of every `crd-perf-tests` object**,
so the whole test exe (not just the specimen) cannot build under clang-cl-shipping. **6c cannot flip to Needs CI until
that fix lands** (flagged: `task_f28fe517`; add the `#if defined(__clang__)` `-Winfinite-recursion` suppression) -- a
Needs-CI row with a red clang-cl lane would be a false flip.

## (d) landed — export/import acceptance (native == external)

Acceptance clause 1 ("export/import a real capture, inspect the SAME critical path in native and external views";
the reject-mismatches tail is (b)). The unit is **equality, not presence**: the native view (`CaptureView` values)
and the external view (exported JSON values) must agree to the nanosecond on the same scopes, with nesting preserved.
Test-only — no engine change ((d) exposed no exporter bug on the critical-path scopes, the counter, or the raw path — it does not cross-check the M/i or frame X events against native values, which the "same critical path" clause does not require).

- **New case** `test_diag_perfetto_export.cpp` tag `[perf][diag][perfetto][acceptance]` (15 assertions): a nested
  `outer_scope`→`known_hotspot`, a counter=42, the raw GPU span; save → `CaptureView` → export.
- **Native samples found by interned `name_id`** (scan all capture threads — the capture-side index is dense, NOT the
  live thread index). Nesting checked natively: `outer.begin ≤ hot.begin ≤ hot.end ≤ outer.end`, `hot.depth ==
  outer.depth + 1`.
- **Exact-fragment equality** against an **independent** test-local µs formatter (the Perfetto ts=µs *spec*): the
  contiguous `"ts":<us>,"dur":<us>,"name":"<name>"` built from the native `begin_ns`/`end_ns` must appear verbatim in
  the export, for both scopes; plus the native `depth` in args; the counter C-event `"value":42` at the frame whose
  record actually holds 42 (`frame_end_ns`→µs); the raw path `"ts":500000,"dur":4096,"name":"shadow_depth"` (raw ticks
  as-is). Teeth: switching the exporter's CPU ts from µs to ns made both scope fragments fail (observed, restored,
  lane rebuilt) — the equality catches a real exporter regression, it is not vacuous.
- **Structural validity in-test** (`json_structurally_valid`): a container-free state machine over the document —
  balanced `{}`/`[]` outside strings, correct escape handling, ends at depth 0 not mid-string, no unescaped control
  byte in a string. "Parse-back" without a parser (this box has no JSON lib in-engine).

**External-engine proof — env-gated (absent here).** `trace_processor_shell` / `trace_processor` are NOT on this box
and the `perfetto` pip module is absent (and must NOT be installed on an unattended tick — its first use downloads
~100 MB, a file download needing user permission). When a `trace_processor_shell` binary is present, the gold external
proof is: export via `CRD_PERFETTO_EXPORT_OUT`, then
`trace_processor_shell --query-string "select name, ts, dur, depth from slice where name in ('outer_scope','known_hotspot','shadow_depth')" <file>`
— Perfetto stores `slice.ts` in **ns** (to confirm against the `slice`-table docs when an engine is present), so the round-trip oracle is `slice.ts == native begin_ns` exactly. The
in-repo `python -c json.load` gate (recorded in (c)) already proves the document is well-formed, right-`ph`-set,
numeric-`ts` JSON. Driving `ui.perfetto.dev` is a permission-gated external site — skipped on the unattended tick;
the user procedure is a CORS-enabled local server + `https://ui.perfetto.dev/#!/?url=http://localhost:<port>/x.json`
(data stays local). NOTE for the UI importer: the reserved `frames` tid `0xFFFF0000` and the `M` events without `ts`
are the two things the strict JSON importer could reject — verify when an engine is available.

**Gates.** win-debug `crd-perf-tests` full **1295/198** (was 1280/197; +15 assertions / +1 case, no regressions);
`[acceptance]` 15/1 on win-debug + win-asan; `[perfetto],[cprof-version]` 61/6; win-shipping `/W4 /WX` full link clean.
(d) is test-only, so `crd-perf` is unchanged since (c)'s clang-cl-shipping-clean build. container check + both
validators PASS. **6c remains flip-blocked** on the `crash_capture_specimen.cpp` clang-cl fix (`task_f28fe517`,
still not landed).

## (e) landed — offline export CLI (cprof_export)

`tools/cprof-export/` → `cprof_export <in.cprof> <out.json>`: loads a CPROF file, validates it, and writes the
Perfetto trace-events JSON via the SAME `export_perfetto_json_to_file` path (d) verified. Links `crd-perf` only —
NOT `crd-perf-ui` (native viewer stays optional). Prints the `PerfettoExportStats` incl. **dropped_samples** (loss
never hidden) + the declared schema.

- **New reusable primitive** `crd::perf::load_capture_from_file(path, alloc) -> Array<crd::u8>` (the inverse of
  `save_capture_to_file`; both `#if`/`#else` branches; `#else` and any error return an EMPTY buffer, never a
  truncated one). Keeps the CLI's deps to `crd-perf` (+ core/containers/memory) — no `crd-platform`, no
  `std::filesystem` (its `path` is an owning std container the checker bans).
- **Perf-off refusal (observed, not compile-only).** The CLI checks `crd::perf::kEnabled` FIRST and, when profiling
  is compiled out, prints "profiling compiled out … use win-debug/win-shipping-profile" and exits **3** — never an
  empty/zero-byte json (tools-side of "record unavailable, never treat as zero"). A `tools/` target has no
  crash-specimen dependency, so **clang-cl-shipping (perf off) builds `cprof_export`** and running it produced exactly
  that message + exit 3 — observed.
- **Byte-identity oracle.** The `[perfetto]` test, when `CRD_PERFETTO_EXPORT_OUT` is set, also drops the exact capture
  `buf` bytes as `<out>.cprof` (same buffer → same `captured_at_ns`, not a re-`save`). Running `cprof_export` on that
  `.cprof` produced a json **byte-identical** (`cmp` → IDENTICAL) to the test's own in-process export, and `json.load`
  parsed it (10 events, schema `cerid-diagnostics/1`). Identity proves the CLI is a faithful wrapper over the
  (d)-verified path, not a second implementation. CLI stdout: `3 sample, 1 counter, 1 frame, 5 metadata, 0 instant
  events; 0 samples dropped; schema cerid-diagnostics/1`.
- **Registration triple:** `crd_module(tools/cprof-export NAME cprof_export HOST EXECUTABLES cprof_export DEPENDS
  containers core memory perf)` in root `CMakeLists.txt`; a systems-map row in `docs/systems/README.md`; both
  validators enforce it.
- **Reject paths observed (not asserted).** `cprof_export <nonexistent> x.json` → `cprof_export: cannot read …` +
  exit 1, **`x.json` not created**; `cprof_export <40-byte-truncated.cprof> y.json` → `… is not a valid CPROF v1
  capture (bad magic/version/size)` + exit 1, **`y.json` not created** — the refusal writes no zero-byte output, and
  the truncated case is clause 1's "reject size mismatches" observed through the tool. `load_capture_from_file`'s
  failure contract is also unit-pinned (`[perf][diag][cprof-load]` 3/1): `nullptr` and a nonexistent path each return
  an EMPTY buffer (never truncated), and an empty buffer is not a valid capture.

**Gates.** win-debug: `cprof_export` builds + the byte-identity + `json.load` smoke; full `crd-perf-tests` green,
`[perfetto],[cprof-version]` 62/6. win-shipping: `cprof_export` builds. **clang-cl-shipping: `cprof_export` builds AND
the perf-off refusal runs (exit 3)** — the CLI, unlike `crd-perf-tests`, is verifiable on that lane. container check +
both validators PASS. **6c remains flip-blocked** on the `crash_capture_specimen.cpp` clang-cl fix (`task_f28fe517`,
re-checked not-landed this tick) — `crd-perf-tests` still cannot build under clang-cl-shipping.

## (f) landed (artifacts + observed denial) — external CPU-sampling runbook (WPR/xperf; perf)

Acceptance clause 3: "a known unscoped CPU hotspot is visible through sampling; scope-only charts do not satisfy this
case." Delivered as artifacts + a verdict machine; the HOTSPOT_VISIBLE run is **pending a properly-privileged elevated
shell** (see the denial below), so clause 3 is **not yet Satisfied** and 6c gains a SECOND flip blocker.

- **Specimen** `tests/support/diag/specimens/cpu_hotspot_specimen.cpp` (`crd_diag_cpu_hotspot_specimen`): one
  `__declspec(noinline)` function `crd_diag_unscoped_hotspot_burn` burning a dependent arithmetic chain into a
  `volatile` sink for ~2 s, with **NO `CRD_PERF_SCOPE`** — invisible to the in-process scope profiler, findable only
  by an external sampler. Standalone; **NOT** a `crd-perf-tests` dependency (the runbook launches it). Builds clean on
  **win-debug AND win-clang-cl-shipping** (unlike the DIAG.5d crash specimen — no per-compiler warning pragma needed).
  Symbol resolvability confirmed on this box: `crd_diag_unscoped_hotspot_burn` appears 6× in
  `build/win-debug/tests/diag/crd-diag-cpu-hotspot-specimen.pdb` (which is what `xperf` reads via `_NT_SYMBOL_PATH`);
  `dumpbin /symbols` on the linked `.exe` prints nothing because MSVC keeps debug symbols in the PDB, not a COFF table
  in the image — expected, not a gap. So an elevated `xperf -symbols` run WILL decode the symbol. The specimen's quick
  argument path (`… q`) runs and exits 0.
- **Runbook** `scripts/sample-cpu-wpr.py` — a verdict machine printing exactly one of `HOTSPOT_VISIBLE (0)` /
  `HOTSPOT_NOT_VISIBLE (4)` / `PERMISSION_DENIED (3)` / `TOOL_MISSING (2)`. `wpr -start CPU -filemode` → run specimen →
  `wpr -stop <scratch>.etl` → `xperf -i <etl> -o <txt> -symbols -a profile -detail` → grep the symbol. `try/finally:
  wpr -cancel` so a failure never orphans a kernel session; `_NT_SYMBOL_PATH` = the **local** build dir only (never
  `srv*` — no network symbol download). The `xperf` syntax was checked against the installed `xperf -help
  processing`/`-help symbols` (`-symbols` is a processing flag BEFORE the action; `profile -detail` buckets by
  function when symbols decode) — the earlier recalled `-a profile -symbols` was wrong and is corrected here.
- **Observed denial (first-class result, not a skip).** The two admin probes on this shell **disagree** — `net
  session` returns 0 (rc 0) but `IsUserAnAdmin()` is false — so neither is authoritative; wpr's own privilege check is,
  and **`wpr -start CPU` fails `0xc5585011` "Failed to enable the policy to profile system performance"** — the kernel
  sampler needs `SeSystemProfilePrivilege`, which the desktop-app shell lacks. The runbook
  reports `PERMISSION_DENIED (exit 3)` with wpr's stderr verbatim, and `wpr -status` afterward is idle (**no orphan
  session**). This is the design's own "record unavailable, never treat as zero" applied to our runbook.
- **Verdict-logic tests** `scripts/test-sample-cpu-wpr.py` (6/6, `unittest`, run on this box): TOOL_MISSING (wpr
  absent / specimen absent), PERMISSION_DENIED (start fails, no `-stop`/`-cancel` orphan), **HOTSPOT_VISIBLE** and
  HOTSPOT_NOT_VISIBLE (the symbol-grep happy paths, mocked — the part the denial prevents observing live), and the
  Linux TOOL_MISSING branch. Not auto-run by the validators (scripts tests are standalone here).
- **Linux `perf` recipe** in the same script (`--linux`): `perf record -g <specimen>` → `perf report --stdio` → grep
  the symbol; env-gated (no `perf` on this box), `perf_event_paranoid`/privilege failures surface as PERMISSION_DENIED.

**Pending for clause 3 (user action):** run `python scripts/sample-cpu-wpr.py` from a **real elevated PowerShell**
(SeSystemProfilePrivilege) and confirm `HOTSPOT_VISIBLE`. Follow-up (named, not this tick): the gold contrast demo —
the same specimen also saving a `.cprof` with a trivial scope and no hotspot, shown side-by-side with the sampled
profile, to make "scope-only does not satisfy" visual.

**Gates.** win-debug + win-clang-cl-shipping build the specimen; `scripts/test-sample-cpu-wpr.py` 6/6; the runbook run
observed PERMISSION_DENIED + no orphan session; container check + both validators PASS; scratch `.etl`/CSV removed.
**6c flip blockers (both must clear):** (1) `crash_capture_specimen.cpp` clang-cl `-Winfinite-recursion` fix
(`task_f28fe517`) so `crd-perf-tests` builds under clang-cl-shipping; (2) an elevated `sample-cpu-wpr.py` run
confirming HOTSPOT_VISIBLE (clause 3).

## (g) landed — optimized / fiber stack-unwind qualification

Acceptance clause 2: "WPA/xperf unwinds optimized native frames via PDB (5d symbol identity feeds matching);
qualify crd-jobs fiber stacks or explicitly mark them missing." x64 unwinding is table-driven (`.pdata`/`.xdata`;
`.eh_frame` on SysV), so it is correct through `/O2` frames with no frame pointers — and WPA/xperf and the in-process
`RtlCaptureStackBackTrace` read the **same** tables. So the observable core of clause 2 is provable HERE, on the
shipping lane, with no kernel sampler and no elevation; the external `xperf -a stack` confirmation is the
elevation-gated half (the same gate as (f)).

- **In-process oracle** `tests/foundation/jobs/test_diag_unwind_qualification.cpp` (`[jobs][diag][unwind]`): a chain
  of six `noinline` functions, each recording its own `_ReturnAddress()` (`__builtin_return_address(0)` off-MSVC)
  and folding the callee's result into a `volatile` sink AFTER the call (so `/O2` cannot tail-call-eliminate the
  frame). The innermost captures via `RtlCaptureStackBackTrace` (`backtrace()` on Linux). Oracle is **symbol-free
  and exact**: every recorded return address must appear in the captured backtrace, in **strictly increasing frame
  order** — it depends only on the unwind tables being right, not on PDB symbolication. `REQUIRE(count>0)` is not
  used as an oracle.
- **Two cases.** (1) *Optimized native, thread stack* — the chain atop the full test/Catch2/CRT stack: a deep
  capture, chain present + ordered. (2) *Fiber* — the same chain driven on a real crd fiber via the public low-level
  primitives (`fiber_init_stack` + `fiber_switch`, no scheduler needed): the fiber's OWN frames unwind correctly
  (chain present + ordered) AND the walk **truncates at the fiber-switch boundary**, proven two ways. *Directly:* a
  `noinline` `drive_fiber_and_switch` records a thread-stack return-address marker just before the switch, and the
  test `REQUIRE`s that address is **absent** from the fiber capture — so "the walk cannot cross the hand-rolled
  `fiber_switch` onto the thread stack" is observed, not inferred. *Corroborating:* the fiber capture is strictly
  shallower than the same chain on a full thread stack and bounded near the chain depth (`≤ kChain+6`, no runaway
  past the trampoline). Depth is lane-dependent (a `WARN` prints it each run so the figure is reproducible): fiber 9
  vs thread 24 on win-debug, 8 vs 16 on win-shipping `/O2`, 8 vs 15 on clang-cl. The hand-rolled `fiber_switch` is a
  register swap onto a manually-built stack whose trampoline carries no unwind info; the walk therefore ends within a
  few frames of the fiber entry (the exact terminating frame is not asserted). **That truncation is the qualified,
  documented limitation** the acceptance demands be marked, not a silent drop.
- **Lanes (the point of clause 2).** win-debug proves nothing about optimization; the oracle is built + run on
  **win-shipping (MSVC `/O2`)** and **win-clang-cl-shipping** (thin-LTO) as well: **43 assertions / 2 cases green on
  all three**. The test lives in `crd-jobs-tests` (not `crd-perf-tests`), so it is NOT caught by flip-blocker 1's
  `crash_capture_specimen` clang-cl defect — the LTO lane actually runs it. *Linux lane:* the `#else` path
  (`__builtin_return_address` + `backtrace()`, and the SysV `fiber_switch`) compiles by inspection against the
  `diagnostic_allocator.cpp` `backtrace` precedent but has NOT been built or run here — its run is CI-pending.
- **Teeth (observed on `/O2`).** Force-inlining one chain function collapses its frame; the recorded return address
  then shares an index with its neighbour and the strict-ordering `REQUIRE` fails (`chain frame 4 out of order at
  index 4 (prev 4)`, exit 42) on win-shipping. Restored → rebuilt that lane → 41/41 green again.
- **External half** `scripts/sample-cpu-wpr.py --stacks`: records with the `Profile` stackwalk and processes with
  `-a stack` (both checked against the installed `xperf -help stackwalk`/`-help processing` — `Profile` is the
  sampled-stack flag, `stack` the stack-showing action), then greps the hotspot symbol in a **call-stack** context.
  Adds a `MISSING_STACKS` verdict (exit 5), distinct from `HOTSPOT_NOT_VISIBLE`: a profile captured but carrying no
  decoded `module!symbol` frames (stackwalk absent) — not the same failure as "stacks present, symbol absent".
  `scripts/test-sample-cpu-wpr.py` now **9/9** (adds stacks-VISIBLE / stacks-NOT_VISIBLE-with-frames /
  MISSING_STACKS). Observed on this box: `sample-cpu-wpr.py --stacks` → PERMISSION_DENIED (`0xc5585011`), WPR not
  recording afterward (no orphan) — same elevation gate as (f). Caveat: the `MISSING_STACKS` discriminator
  (`_has_stack_frames`, a `\w+!\w+` module!symbol match) is a **heuristic not yet validated against real
  `xperf -a stack` output** — the elevation-blocked run means the exact text format is unconfirmed; validate it
  against the first elevated `.txt` and tighten if needed.

**Clause 2 verdict.** SATISFIED in-process on the shipping (`/O2`) and clang-cl LTO lanes: optimized native frames
unwind correctly, and crd-jobs fiber stacks are qualified with their truncation-at-the-switch boundary explicitly
documented (never silently dropped). The external WPA/xperf `-a stack` confirmation is elevation-gated and rides the
existing blocker (2) below — it is not a new blocker, and not a partial: the qualifying oracle is live here.

**Gates.** win-debug + win-shipping + win-clang-cl-shipping build `crd-jobs-tests`; `[unwind]` 43/43 on all three;
teeth observed + restored on win-shipping; `scripts/test-sample-cpu-wpr.py` 9/9; `--stacks` runbook observed
PERMISSION_DENIED + no orphan; container check + both validators PASS; no stray root files; scratch removed.
**6c flip blockers (both still stand):** (1) `crash_capture_specimen.cpp` clang-cl `-Winfinite-recursion` fix
(`task_f28fe517`) so `crd-perf-tests` builds under clang-cl-shipping; (2) an elevated `sample-cpu-wpr.py` run
confirming HOTSPOT_VISIBLE for the flat profile (clause 3) and `--stacks` call stacks (clause 2 external half).

## (h) landed — rich counters "where supported" (capability probe + verdict machine)

Design clause (`runtime-diagnostics.md`, "Where supported ..."): include context switches, page faults, cache/branch
counters, lock contention and CPU/NUMA affinity, and **"record permissions, multiplexing and missing PMU events
rather than treating unavailable counters as zero."** Delivered as a capability machine, not a metric collector —
each counter class is recorded as a NAMED state, never a silent zero. (Advisor was overloaded all three attempts this
tick; proceeded per protocol — two real defects were caught by live observation, below.)

- **`scripts/counters-capability.py`** enumerates five counter classes (context_switch, page_fault, cache_branch_pmu,
  lock_contention, cpu_numa_affinity) and records each as `AVAILABLE` / `MULTIPLEXING_REQUIRED` / `PERMISSION_DENIED`
  / `UNAVAILABLE(reason)` / `TOOL_MISSING`. The capability enumeration is **unprivileged** (`xperf -pmcsources`,
  `xperf -providers KF`) and runs fully here; a real capture is elevation-gated (`--capture <class>`), the same gate
  as (f)/(g). All xperf syntax was checked against the installed help (`-pmcsources`, `-providers KF`, `-help start`
  for `-Pmc`/`-PmcProfile`), not recalled.
- **Observed on this box (unprivileged report).** Every class is `AVAILABLE`: `CSWITCH` and `HARD_FAULTS` kernel flags
  are present; lock_contention / cpu_numa_affinity are derived from `CSWITCH`+`DISPATCHER` (WPA wait analysis); and
  the PMU is **real** — 16 cache/branch sources (BranchInstructions, CacheMisses, LLCMisses, BranchMispredictions,
  InstructionRetired, …), **max 8 concurrent counters** (see the multiplexing note). "Missing PMU" is NOT the honest
  result here — the hardware exposes it; only the *capture* is privilege-gated.
- **Observed on this box (capture, elevation-gated).** `--capture context_switch` → `PERMISSION_DENIED` (NT Kernel
  Logger access denied, `0x5`); `--capture cache_branch_pmu` → `PERMISSION_DENIED` (Failed to configure counters,
  rc `0x8000FFFF` = E_UNEXPECTED — **presumed** a privilege failure like the kernel-logger `0x5`, not proven
  E_ACCESSDENIED; confirm on the elevated run); `--capture cache_branch_pmu --pmu-count 12` →
  `MULTIPLEXING_REQUIRED` (12 > 8 concurrent). No kernel session is orphaned (try/finally `xperf -stop`).
- **Two defects caught by live observation (not recall).** (1) `-pmcsources` reports 9 total *profile* slots, but
  `-PmcProfile` rejected a 9-counter request with "Too many counters specified (max=8)" — one slot is the timer, so
  the effective concurrent-counter limit is **N−1 = 8**; `pmu_concurrent_limit()` now models that and the default
  request reserves the timer slot. (2) that count error was first mis-reported as `PERMISSION_DENIED`; a counter-count
  limit is **multiplexing, not a permission failure**, so the `-on` failure classifier now maps "too many counters" /
  "max=" to `MULTIPLEXING_REQUIRED` and only genuine privilege failures to `PERMISSION_DENIED`.
- **Verdict-logic tests** `scripts/test-counters-capability.py` **11/11**: pmcsources parsing, PMU
  AVAILABLE/UNAVAILABLE classification, kernel-flag presence, the multiplexing decision, the N−1 timer-slot
  reservation, a denied capture (no orphan), the count-error→MULTIPLEXING classification, and an unknown class.
- **Linux** noted (no `perf` here): `perf list` (sources) + `perf stat -e ...`; `perf_event_paranoid` gates capture —
  documented runbook, CI-lane note.

**Clause verdict.** SATISFIED as the design words it: this box's counter classes are recorded with their real
permission and multiplexing states (PMU present, 8-concurrent limit, capture needs elevation) — never zeroed. An
elevated run would flip the capture verdicts from `PERMISSION_DENIED` to actual `AVAILABLE` captures; that is the same
elevated-run dependency as (f)/(g), already tracked by flip blocker (2).

**Gates.** `scripts/test-counters-capability.py` 11/11; the capability report + `--capture` verdicts observed on this
box; both validators PASS; no stray root files; scratch removed. No engine C++ touched (scripts only). 6c stays
**Open**; flip blockers unchanged: (1) `crash_capture_specimen.cpp` clang-cl fix (`task_f28fe517`); (2) an elevated
run confirming HOTSPOT_VISIBLE (clause 3) + `--stacks` (clause 2 external) — and now the counter captures (h).

## (i) landed — frame pacing / input-to-present (separate from throughput + shader time)

Design clause: "Profile frame pacing/input-to-present **separately** from offline throughput and shader execution
time." The acceptance point is the separation — three distinct axes, none derived from another. Delivered as a pure,
bounded analyzer proven separate by construction and by oracle.

- **No on-disk change (deliberate).** `FrameRecord` already carries `frame_begin_ns`/`frame_end_ns`, and its layout is
  pinned by 6c(b)'s `offsetof`/`sizeof` static_asserts with no spare header slot — adding a present/input field would
  force `kCprofVersion=2` + an N-1 reader, a slice of its own. So (i) computes pacing from the existing timestamps and
  keeps input-to-present in memory. Present/input timestamps are **not** recorded on-disk today; that on-disk path is
  named future work (a v2 bump), not smuggled into this slice.
- **`engine/foundation/perf/include/crd/perf/frame_pacing.hpp`** — `FramePacingAnalyzer`, header-only, `crd::` only
  (fixed-bucket histograms, no sort/alloc), integer nanoseconds (locale-independent). Three separate accumulators:
  PACING (frame-to-frame interval distribution: mean/min/max/percentile/stutter-count), WORK (per-frame duration
  `end-begin`, mean), and LATENCY (input-to-present `present-input`: mean/max/percentile). It never reads GPU scope
  durations — that is exactly how "separately from shader execution time" is honoured structurally. `feed_frames()`
  drives it from a `CaptureView::frame_records()` span so it is a real capture consumer, not an orphan.
- **The separation oracle** `tests/foundation/perf/test_diag_frame_pacing.cpp` (`[perf][diag][pacing]`, 22 assertions
  / 4 cases): (1) *pacing ≠ throughput* — steady 16 ms vs alternating 8/24 ms have the **identical mean interval**
  (identical throughput) but the alternating p99 is ≥ 5 ms higher and its stutter count (≥20 ms) is 50 vs 0; (2)
  *pacing ≠ shader/work time* — two runs with the **identical begin cadence** but 4 ms vs 12 ms per-frame work have
  identical pacing (mean/max/p99) and different duration; (3) *input-to-present ≠ frame duration* — 16 ms frames with
  a 3-frame pipeline report 48 ms latency; (4) `feed_frames` derives 3 intervals + 4 durations from a `FrameRecord`
  run.
- **Teeth (observed on win-debug).** The real load-bearing part is the histogram binning, so the tooth targets it:
  making `bucket_of` ignore its value (all samples into bucket 0) breaks the percentile/stutter mapping → case 1
  (pacing tail, line 33) AND case 3 (input-to-present percentile, line 75) fail (exit 42, 2/4 cases), while case 2
  (equal pacing — both collapse identically) and case 4 (means, not histogram) still pass. This proves the histogram
  is what distinguishes the distributions, in both the pacing and latency axes — not just that a REQUIRE fires.
  Restored → rebuilt → 22/22. (An earlier weaker tooth — percentile returning the mean — only proved the line-33
  REQUIRE fires when p99==mean and additionally tripped a `/WX` unused-parameter error, so it was replaced by this
  histogram tooth.)
- **Lanes.** win-debug 22/22, **win-asan** 22/22, **win-shipping (`/O2`)** 22/22. The test is in `crd-perf-tests`, so
  the clang-cl **exe** link is blocked by flip-blocker 1 (`crash_capture_specimen`, an order-only prerequisite of
  every perf-test object) — but the TU itself **compiles clean under clang-cl** (`/W4 /WX /permissive- -flto=thin`,
  object produced) by invoking clang-cl directly, exactly the (b) precedent.

**Clause verdict.** SATISFIED: frame pacing and input-to-present are measured on their own axes, provably independent
of throughput and of shader/work time, with no on-disk or version change. The on-disk present/input timestamps and a
perf-ui pacing graph are named follow-ups.

**Gates.** `[pacing]` 22/22 on win-debug + win-asan + win-shipping; clang-cl TU compiles clean (exe link rides
blocker 1); teeth observed + restored; container check PASS; both validators PASS; no stray root files; scratch
removed. 6c stays **Open**; flip blockers unchanged.

## (j) landed — platform-debugger recipes + observed availability (the final 6c sub-unit)

Design goal: "platform debugger recipes to inspect uninstrumented work, blocked time and memory pressure"; external
tool dependencies stay optional. Delivered as a runbook + an observed availability verdict machine, honest about a
box on which no external interactive debugger is runnable.

- **Observed on this box (looked, did not assume).** `where lldb cdb windbg gdb`: **lldb present** at
  `C:\LLVM-20.1.8\bin\lldb.exe` **but UNRUNNABLE** — it exits `0xC0000135` (STATUS_DLL_NOT_FOUND) because
  `liblldb.dll` hard-imports `python310.dll`, absent here (this box has Python 3.14, no 3.10; `dumpbin /dependents`
  confirms the import). Reproduced from a **native PowerShell** (`LASTEXITCODE=-1073741515`), independent of the
  MSYS/bash→cmd shell — a real launch failure, not an environment artifact. **cdb / windbg / gdb ABSENT** (Windows
  Debugging Tools not installed; gdb is Linux). So there
  is **no runnable external interactive debugger** on this box — recorded as a first-class state, never a faked
  transcript. This is the same honesty rule as the (f)/(g)/(h) elevation gate.
- **`scripts/debugger-recipes.py`** carries the recipes in its header (matching the (f)/(h) script-is-runbook
  pattern, so no new `docs/` file to satisfy check-master-plan) and a verdict machine printing exactly one of
  `SYMBOLS_RESOLVED (0)` / `TOOL_MISSING (2)` / `DEBUGGER_UNRUNNABLE (3)` / `SYMBOLS_MISSING (4)`. Run here:
  `--probe` → the availability table + `DEBUGGER_UNRUNNABLE: lldb present but STATUS_DLL_NOT_FOUND (0xC0000135) …`,
  exit 3.
- **Recipes map the clause's three nouns to a command AND an in-engine equivalent** (external deps optional):
  *uninstrumented work* → WinDbg `!runaway` / `~*k`, lldb `break;run;bt` — in-engine: the DIAG.6c(f) WPR sampler +
  hotspot specimen; *blocked time* → WinDbg `!analyze -v -hang` / wait-chain, lldb `thread apply all bt` — with the
  honest cross-ref to (g): a **parked crd fiber is on no OS-thread stack** (register-swap `fiber_switch`), so a
  debugger shows workers idling in the scheduler, not the blocked fibers → the DIAG.4c hang watchdog + wait graph is
  the right tool; *memory pressure* → WinDbg `!address -summary` / `!heap -s`, lldb `memory region` (no `!heap`
  equivalent) — in-engine: the DIAG.3c DiagnosticAllocator report + the (h) `page_fault` counter class. WinDbg/cdb
  commands stay documented-only with the "cdb/windbg absent on this box" line — never a fake session.
- **Verdict-logic tests** `scripts/test-debugger-recipes.py` **6/6**: TOOL_MISSING, DEBUGGER_UNRUNNABLE (with the
  `0xC0000135`→python310 classification), SYMBOLS_RESOLVED / SYMBOLS_MISSING batch outcomes, and the
  version-ok-no-batch path — i.e. the branches the box's own unrunnable lldb cannot exercise live.

**Clause verdict.** SATISFIED as the design words it (recipes for the three inspection targets, external deps kept
optional and mapped to in-engine equivalents), with the box's actual debugger availability recorded honestly. A
verified live lldb session is impossible here (lldb won't launch); that is a recorded finding, and the engine's own
crash/hang/allocator/counter diagnostics are the cross-referenced answer.

**Gates.** `scripts/test-debugger-recipes.py` 6/6; the availability probe observed on this box; both validators PASS;
no stray root files; no C++ touched (scripts + docs only). Memory `this-box-diag-tool-capabilities.md` corrected
(lldb present-but-unrunnable).

## 6c status: (a)–(j) all landed; row 079 stays Open, flip-blocked on two user-gated items

Every DIAG.6c sub-unit (a) census, (b) CPROF version discipline, (c) Perfetto export, (d) export/import acceptance,
(e) export CLI, (f) external CPU-sampling runbook, (g) optimized/fiber unwind qualification, (h) rich-counter
capability, (i) frame pacing / input-to-present, (j) debugger recipes — is landed with observed proofs or honestly
recorded tool-unavailability. **Row 079 stays Open**; there is no agent-doable 6c work left. It flips only when BOTH
user-gated blockers clear: (1) `crash_capture_specimen.cpp` clang-cl `-Winfinite-recursion` fix (`task_f28fe517`) so
`crd-perf-tests` builds under clang-cl-shipping; (2) an elevated run confirming HOTSPOT_VISIBLE (flat profile clause
3), `--stacks` call stacks (clause 2 external half) and the (h) counter captures. Next loop tick has no further 6c
sub-unit to advance → it begins row 080 / DIAG.7a with a census-first tick (a blocked-on-user row does not stall the
loop, same logic as skipping DIAG.0).
