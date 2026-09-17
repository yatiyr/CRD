# DIAG.5d — symbols, bundles and trustworthy import (DG14): census and plan

<!-- doc-role: historical -->

Owner slice: [DIAG.5d](../ROADMAP.md#slice-diag.5d). Contract: [runtime-diagnostics
design](../design/runtime-diagnostics.md#diag-5d); [ADR-0133](../decisions/0133-runtime-diagnostics-and-instrumentation.md).
DG14 (a unified crash + symbol + generation + capture bundle and consumer path). Running slice doc: the sections
below are appended per sub-unit; this opening **(a)** part is the census.

DIAG.5d (row 076) is the first Open DIAG row after 5c landed. Like 5c it packs four concerns into one row
("versioned bounded crash/failure bundles, exact symbol identities, unloaded generations, adversarial import"), so
this is the census sub-unit (a): scope, defects vs acceptance, verification regime, decomposition. **No engine code
this tick.**

## Acceptance (verbatim from the design)

From [runtime-diagnostics.md#diag-5d](../design/runtime-diagnostics.md#diag-5d) ("symbols, bundles and trustworthy
import"):

> Implement the bundle schema and symbol lookup by actual binary/module identity: PDB GUID/age where applicable, ELF
> build ID and matching debug files, later dSYM/WASM identities. Retain unloaded module generations and source
> mapping. Package artifacts atomically with partial-section recovery, quota/retention and restricted raw-data
> handling. CLI import/inspect is bounded and does not execute content.
>
> Acceptance: symbolize an optimized fresh-machine crash, reject wrong symbols instead of giving plausible lines,
> recover truncated bundles, reject traversal/oversized/decompression-bomb inputs, retain old DLL symbols after
> reload. Golden manifests preserve schema migration and explicit absent sections. Raw dumps are never declared
> fully redacted.

DG14 ([research](../research/2026-09-14-diagnostics-and-instrumentation.md) row): crash dumps, native symbols,
program generations and profiler captures "lack a demonstrated unified bundle/consumer path"; 5d builds it (with 6c).

## Design vs ADR — the scope that governs the build

- **Post-mortem, not in-handler.** The bundle *packages* artifacts 5a/5b already produce (the minidump / OS core, the
  5b async-signal-safe record, a log tail, symbol identities, a manifest) into one atomic archive, and a bounded CLI
  imports/inspects it. Nothing here runs in a crash/signal handler, so — unlike 5b — **almost nothing needs to be
  async-signal-safe**. The one exception is symbol-identity *capture* (below): reading it must happen at a safe time,
  not in the handler. 5b already deferred symbolization and the stack walk to "offline (5d)"
  ([5b session](2026-09-15-diag-5b-linux-crash-capture.md)) — this slice is that offline half.
- **Schema.** ADR ID-1: the event/capture/bundle schema is **`cerid-diagnostics/1`**
  ([ADR-0133](../decisions/0133-runtime-diagnostics-and-instrumentation.md) line 81). The unified
  crash+symbol+generation+capture bundle "remains `unsupported` today (DG14) and is built by DIAG.5d/6c" (ADR line
  86–87) — so, like every diag slice, the artifact is DIAG.0-gated; 5d implements it and reports, the user's DIAG.0
  gate flips support.
- **Bounded quota (concrete).** ADR line 134: bundle quota = **≤256 MiB and ≤10 bundles** (excluding separately
  authorized full dumps). "Bounded" = hard per-section + total byte caps with honest truncation markers, not
  grow-until-OOM.
- **Restricted raw-data / redaction.** ADR ID-8 (lines 50–52): "A raw dump is not sanitized merely because the
  accompanying manifest is redacted." → the acceptance's "raw dumps are never declared fully redacted": the manifest
  may redact its own fields, but the bundle must never *claim* the embedded raw dump is redacted/safe when it is not.
- **Importer does not execute content** and is bounded — a reader/inspector, never an evaluator.

## Current state and defects vs the acceptance

Paths: [`crash.cpp`](../../engine/foundation/core/src/crash.cpp) / [`crash.hpp`](../../engine/foundation/core/include/crd/core/crash.hpp)
(5a/5b capture + readback), and greenfield bundle/importer code to be added.

1. **No bundle format or writer exists.** 5a/5b produce a dump/core + a record as *separate* artifacts; there is no
   `cerid-diagnostics/1` container that packages them atomically with a versioned header, per-section tags/lengths,
   partial-section recovery, or quota/retention. Acceptance: "package artifacts atomically with partial-section
   recovery", "recover truncated bundles", "golden manifests preserve schema migration and explicit absent
   sections." **(b)**
2. **Symbol identities — Windows present, Linux absent.** Windows: the minidump's `ModuleListStream` carries the RSDS
   **GUID/age + PDB name** per module; 5a read it back (`read_dump_stream`) for the crashing specimen, so Windows
   "exact symbol identity" is a *readback into the bundle* — (c) confirms it per-module (every loaded module, not just
   the faulting binary), not new capture. The acceptance's "matching debug files" also needs the offline symbolizer's
   file-lookup convention (PDB by GUID/age; on Linux `.gnu_debuglink` / `.build-id/<nn>/<rest>.debug`) — identity
   plus *how the matching file is found*, which (c) must specify, not just the identity bytes. Linux: **measured — the 5b record carries signal,
   `si_code`/`si_addr`, registers, thread identity and the exe path, but NO ELF build-id**
   ([5b record fields](2026-09-15-diag-5b-linux-crash-capture.md)). `dl_iterate_phdr`/`getauxval(AT_...)` are **not
   async-signal-safe**, so the build-id must be captured at `install()` into a static buffer and copied by the
   handler (or written by the offline bundler from the still-present binary). Design constraint, named here. **(c)**
3. **Unloaded module generations — not captured.** **Measured — `capture_dump`'s MINIDUMP_TYPE
   (`crash.cpp:265`) is `MiniDumpWithDataSegs | MiniDumpWithHandleData | MiniDumpWithFullMemoryInfo |
   MiniDumpWithThreadInfo` — it does NOT set `MiniDumpWithUnloadedModules`.** Windows has a native
   `UnloadedModuleListStream`; retaining "old DLL symbols after reload" is a one-flag add + a readback + a test, not
   new capture machinery. Linux has **no** OS-native unloaded-module list — 5d must either track `dlclose` ranges
   itself (needs a hook the engine does not currently have) or declare Linux unloaded attribution a named
   limitation; (d) picks one with the ADR in hand. **(d)**
4. **No importer / no adversarial defense.** There is no bounded reader that rejects path traversal, oversized
   sections, or decompression bombs, or that version-checks and skips unknown sections. Prior art exists to mirror:
   [`zip_archive`](../../engine/assets/resources/include/crd/resources/zip_archive.hpp) /
   [`crdr`](../../engine/assets/resources/src/crdr.cpp) already defend hostile archives (decompression/traversal),
   and the [toml parser](../../engine/foundation/toml/) is the result-typed rejection pattern (5c defect 8).
   Acceptance: "reject traversal/oversized/decompression-bomb inputs", "CLI import/inspect … does not execute
   content", "reject wrong symbols instead of giving plausible lines." **(e)**
5. **Fresh-machine symbolization is unproven.** "Symbolize an optimized fresh-machine crash" needs the bundle to
   carry enough identity (RSDS/build-id + matching-debug-file references) that an *offline* symbolizer on a different
   machine resolves frames exactly, and rejects mismatched symbols rather than emitting plausible-but-wrong lines.
   The symbolizer itself is offline (5b's rule); 5d's deliverable is the identity + the matching/rejection logic,
   demonstrated end-to-end. **(c)/(e)**

## Verification regime

- **win-debug + linux-gcc-debug** for the writer/reader/importer library (crd:: containers only; the importer's
  output structures too).
- **win-asan IS in scope for the importer** — "adversarial import" (hostile length/offset/count → OOB reads) is
  exactly a sanitizer slice, so protocol step 3's win-asan requirement applies here (unlike 5c, which was "no
  sanitizer lane by design"). The win-asan run gotcha applies: run the exe inside the vcvars
  environment (so the ASan runtime DLL is on PATH) or it exits `0xC0000135` (DLL not found), not a test failure.
- **Fuzz lane exists.** `CMakePresets.json` defines **`linux-clang-fuzz`** (ASan + UBSan + libFuzzer, `scripts/fuzz.py`).
  Adversarial import gets a libFuzzer target there, plus hand-authored corpus cases (truncated bundle, traversal
  path, oversized section length, decompression bomb, wrong-schema-version) runnable on win-asan/win-debug.
  `linux-clang-fuzz` is a visible-but-not-owned preset in the user's CI mapping (see the 5c CI-triage note), so the
  fuzz target is authored + locally runnable, not gated on the standard push lanes — state that honestly at the flip.
- **Golden manifests** (schema migration + explicit absent sections) are checked-in fixtures the importer round-trips.
- Both validators (`check-master-plan.py`, `check-repository.py`) each sub-unit.

## Gotchas (desk-checked)

- **Every field from a bundle is untrusted.** Bounds-check each size/offset/count against the actual buffer, cap
  counts and total, reject on version/magic mismatch, no recursion, and **never allocate sized by an untrusted field
  before the cap check**. Rejection is a runtime `parse_result`/`optional` (the toml pattern), **never `CRD_ASSERT`**
  (5c defect 8's rule, now for code being written).
- **Build-id capture timing.** `dl_iterate_phdr` in a signal handler is UB — capture at `install()`.
- **Windows symbol identity is free from the dump; Linux is not.** Don't write a Windows module-enumerator; read the
  minidump. Do write the Linux build-id path.
- **Atomic package = temp + rename**, mirroring 5a's collision-safe `CREATE_NEW` / Linux `O_EXCL` write discipline;
  a partial write must be recoverable (partial-section markers) or discarded, never a half-bundle read as whole.
- **"Never declared fully redacted"** is an honesty invariant on the manifest, not a redaction feature — do not add a
  claim the raw dump cannot back.
- **crd:: containers throughout — this is 5d's largest "crd:: only" surface.** The writer's and importer's output
  structures are `crd::containers::Array<ModuleIdentity>` / `String` / `StringView`, never `std::vector`/`std::string`/
  `std::span`; the reader takes `(const u8*, usize)` or a `crd::` byte view, not `std::span`. `check_no_std_containers`
  fails CI on any owning STL in `engine/` (already red at 70 on the user's tree — adding to it is the one way 5d could
  make the user's lane worse, so audit the diff each sub-unit). The reusable `inflate_raw`/`deflate_raw` codec that
  `zip_archive` uses is itself crd-native — safe to build on.

## Decomposition (a)–(f)

- **(a) [this doc]** census + defects-vs-acceptance + verification regime + decomposition. No engine code; row 076 Open.
- **(b)** bundle format + writer: the `cerid-diagnostics/1` container (magic + version + per-section tag/length),
  atomic temp+rename write, per-section + total byte caps with truncation markers, partial-section recovery, and
  quota/retention (≤256 MiB / ≤10). crd:: containers only. **(b)'s first decision:** whether sections are compressed
  (a declared uncompressed size + cap, reusing the crd-native `deflate_raw`/`inflate_raw` codec that `zip_archive`
  uses) or uncompressed (then decompression-bomb rejection = reject any section carrying a compressed flag).
- **(c)** exact symbol identities: Windows RSDS GUID/age/PDB readback from the dump into the bundle; Linux ELF
  build-id captured at `install()` and carried in the record/bundle; the matching-debug-file reference + the
  wrong-symbol rejection ("plausible lines" guard). Fresh-machine symbolization demonstrated offline.
- **(d)** unloaded generations: Windows `MiniDumpWithUnloadedModules` flag + `UnloadedModuleListStream` readback +
  "old DLL symbols after reload" test; Linux decision (dlclose-range tracking vs named limitation).
- **(e)** trustworthy importer: a bounded, non-executing reader that rejects traversal / oversized / decompression
  bomb / version-mismatch, recovers truncated bundles, and round-trips golden manifests (migration + explicit absent
  sections). Adversarial corpus + a `linux-clang-fuzz` libFuzzer target; importer tests on **win-asan**. This is a
  large sub-unit — it may split into **(e1)** reader + golden manifests and **(e2)** adversarial corpus + fuzz target
  + win-asan, decided when (e) starts (partial-with-clean-notes over a half-audited claim).
- **(f)** clause-by-clause acceptance review (the `clause → test → evidence` table, 5c(g) shape) + flip row 076
  Open→Needs CI with the Session link and the latest actions/runs URL.

Row 076 stays **Open** (this is (a); the slice is not done until (f)). Next tick: **(b)** — bundle format + writer;
begins with the protocol's advisor + CI check.

## (b) landed — bundle format + writer

New: [`bundle.hpp`](../../engine/foundation/perf/include/crd/perf/bundle.hpp) /
[`bundle.cpp`](../../engine/foundation/perf/src/bundle.cpp) in **crd-perf** (the module ADR-0133 line 134 already
names for the bundle quota; it links crd-core/time/jobs/memory/containers but **not crd-log**, so the container has no
logging edge — the log tail arrives as caller bytes, a later composer's job). Tests:
[`test_diag_bundle.cpp`](../../tests/foundation/perf/test_diag_bundle.cpp) (10 cases / 106 assertions).

- **Format — `cerid-diagnostics/1`, sequential TLV (not a trailing directory).** `BundleHeader` (40 B, pinned:
  magic `'CDB1'` + schema_version + format_version + created_at_ns + total_len + section_count + header crc32) then
  `section_count × { BundleSectionHeader (32 B: tag, flags, payload_length, original_length, payload crc32) + payload }`.
  Little-endian only, explicit padding, `static_assert(sizeof)` on both PODs — the CPROF-capture discipline. A trailing
  directory would lose everything on truncation; sequential TLV recovers the complete prefix **by construction**.
- **(b)'s first decision — resolved: uncompressed in v1.** `kSectionFlagCompressed` is *defined* but never produced (the
  `deflate_raw`/`inflate_raw` codec lives downstream in crd-resources, which crd-perf must not depend on). The reader
  **rejects** a compressed-flagged section (`UnsupportedFeature`) — so (e)'s decompression-bomb defence is already
  inherited: a compressed section is refused until a codec edge is deliberately added.
- **Writer (`BundleWriter`).** Per-section + total caps (defaults 64 MiB / 256 MiB / 32 sections) with **honest
  truncation** — an over-cap section is stored cut to the cap, `kSectionFlagTruncated` set, `original_length` = pre-cut
  size (never silently dropped or grown). Explicit **absent** sections (`kSectionFlagAbsent`, payload 0). `finish()`
  fixes up total_len/section_count/CRCs. crd:: containers only (`Array<u8>`/`String`); `check_no_std_containers`
  is **unchanged by this diff** — 74 owning-STL uses locally (CI's 70 on HEAD `b726b204` + 4 from the still-uncommitted
  diag batch), of which many are earlier DIAG-tick test outputs (5a `minidump_probe.hpp` + crash tests, the jobs diag
  tests, `jobs.cpp`) alongside older jobs test infra — loop/hardening debt for a dedicated sweep tick, not this slice;
  **none in perf/, and (b) adds 0** (its files carry no banned token).
- **Reader (`read_bundle`) — round-trip + truncation recovery.** Borrows views into the input (`ConstSpan<u8>`, never
  copies/executes payload). Stops at the first structurally-invalid section (short header, short/oversized payload, crc
  mismatch, compressed flag) and returns the recovered valid prefix (`RecoveredTruncated`) with `stop_offset` rather
  than failing the whole read. Header states: `BadMagic`, `HeaderTooSmall` (short or header-crc mismatch),
  `UnsupportedVersion`. This is (b)'s *minimal* reader; the bounded, non-executing **adversarial** importer
  (traversal/oversized/bomb/version-mismatch + golden manifests + fuzz) is **(e)** and builds on these types.
- **Atomic publish (`write_bundle_atomic`).** `<path>.tmp` → flush to stable storage (`FlushFileBuffers` / `fsync`)
  → replace-rename (`MoveFileExA` / `rename(2)`); a crash mid-write leaves only the `.tmp`, never a half-final. Mirrors
  5a's temp-then-publish discipline.
- **Retention (`enforce_bundle_retention`).** Enumerates a directory (`FindFirstFileA` / `readdir`+`stat`), keeps the
  newest `max_bundles` (default 10, ADR line 134) matching `prefix`…`suffix`, deletes the rest oldest-first by
  last-write time (ties broken by name so eviction is deterministic under coarse-mtime filesystems).
- **Verified:** win-debug + win-shipping (`/W4 /WX`, NDEBUG) + linux-gcc-debug (the POSIX `dirent`/`stat`/`fsync`/
  `st_mtim` branch compiles and passes there) — 106 assertions / 10 cases green on all three. check-master-plan +
  check-repository PASS. Row 076 stays **Open** (slice not done until (f)).
- **Known limitation for (e)/(f):** publish + retention use narrow ANSI Win32 (`MoveFileExA` / `FindFirstFileA`),
  matching `capture.cpp`'s narrow-`fopen` precedent — but a non-ANSI-codepage path (e.g. a Turkish-locale directory)
  is a gap vs 5a's wide (`CreateFileW`) capture. No code change this slice; noted so the acceptance review accounts it.

Next tick: **(c)** — exact symbol identities (Windows RSDS readback into a `SymbolIndex` section; Linux build-id at
`install()`); begins with the protocol's advisor + CI check.

## (c1) landed — exact symbol identities (Windows RSDS + matching/rejection); (c2) = Linux build-id capture

(c) is bigger than one unit, so it splits: **(c1)** [this tick] = the SymbolIndex section + the crd-native minidump
byte parser + serialize/read round-trip + the matching-debug-file lookup + the wrong-symbol rejection — the
acceptance's *"reject wrong symbols instead of giving plausible lines"* and *"matching debug files"* clauses, for the
Windows/PDB identity and the generic machinery. **(c2)** [next tick] = the Linux ELF build-id *captured at
`install()`* (touches crash.cpp's async-signal-safe POSIX path + likely a `--build-id` linker-flag check) + a record
field + self-consistency test. The `GnuBuildId` kind and its `.build-id` path convention already exist in (c1), so
(c2) only fills in the capture.

New: [`symbol_index.hpp`](../../engine/foundation/perf/include/crd/perf/symbol_index.hpp) /
[`symbol_index.cpp`](../../engine/foundation/perf/src/symbol_index.cpp) in **crd-perf**; tests
[`test_diag_symbol_index.cpp`](../../tests/foundation/perf/test_diag_symbol_index.cpp) (6 cases / 68 assertions).

- **SymbolIndex = the tag-5 bundle section.** Pinned POD payload (`SymbolIndexHeader` 16 B + `SymbolRecord` 72 B ×
  count + blob-packed UTF-8 names, the CPROF NameBlob discipline; `static_assert(sizeof)` on both). `ModuleIdentity`
  is the decoded in-memory form (base/size/checksum/timestamp/age + `SymbolIdKind {None,Rsds,GnuBuildId}` + raw id
  bytes + name/`debug_file` `String`s). `serialize_symbol_index` / `read_symbol_index` round-trip byte-exact; the
  reader bounds-checks every blob offset (an out-of-range name yields "" not an overrun).
- **Minidump parser is crd-native and platform-neutral.** The `MINIDUMP_HEADER/DIRECTORY/MODULE/LOCATION` structs are
  defined in the .cpp (**not** from `<DbgHelp.h>`), so `parse_minidump_modules` compiles and is fuzzable on every lane
  and treats the dump as **untrusted**: every RVA/length is checked against the buffer, and a truncated dump yields the
  modules parsed so far — never a read past end. It reads **every** module in the ModuleListStream (not only the
  faulting one), extracting the RSDS GUID(16)+age+PDB path from each CV record; a module without a CV record gets
  `id_kind None` (refuse, don't guess). UTF-16LE module names → UTF-8 (surrogate-aware).
  - **Gotcha (pinned):** the SDK wraps its minidump structs in `<pshpack4.h>` (4-byte packing), so `MINIDUMP_MODULE`
    is **108 B**, not the 112 a natural-aligned copy would be (the trailing `ULONG64 Reserved` fields get no pad). A
    112-byte stride reads every module after the first at the wrong offset — the "every module" claim silently false,
    and a self-consistent synthetic test would not catch it. Fixed with `#pragma pack(4)` + the 108-B stride
    (`kMinidumpModuleRecordBytes`), and **pinned to the real layout** by Windows-only `static_assert`s on the actual
    `<DbgHelp.h>` `MINIDUMP_MODULE` (size 108, `offsetof(CvRecord)==76`, `offsetof(ModuleNameRva)==20`) — a permanent
    compile-time guard on the lane that ships the type. End-to-end evidence against a **real** multi-module dump
    (`install()` + `capture_dump`) is deferred to **(f)** (install() sets a process-global unhandled-exception filter
    + handler thread, too invasive to run inside the Catch2 host); the SDK static_asserts + the multi-module synthetic
    test carry (c1)'s correctness.
- **Matching debug files + wrong-symbol rejection (the acceptance core).** `identity_matches` requires an exact GUID
  **and** age (RSDS) or exact build-id bytes (GnuBuildId); `None` never matches (even itself), and differing kinds
  never match — this is the "reject wrong symbols" guard a symbolizer must call before trusting a found file.
  `debug_file_path` builds the symsrv layout `<root>/<pdb>/<GUID32-UPPER><AGE>/<pdb>` (GUID = Data1/2/3 little-endian
  + Data4 big-endian, verified byte-exact against a hand-computed key) and the `<root>/.build-id/<nn>/<rest>.debug`
  layout; `None` yields "" (no plausible guess). Hardened against corrupt input: an `id_len==0` or malformed-RSDS
  (`id_len!=16`) record never matches, and `read_symbol_index` clamps an out-of-range `id_kind` to `None` so a corrupt
  payload cannot forge a matchable kind (both tested).
- **Round-trip through a bundle:** `build_symbol_index_from_minidump` → `add_section(SymbolIndex)` → `read_bundle` →
  `read_symbol_index` recovers the identities byte-exact — (c1) is wired to (b)'s container.
- **Verified:** win-debug + win-shipping (`/W4 /WX`, NDEBUG) + linux-gcc-debug — 68 assertions / 6 cases green on all
  three (the parser needs no Windows headers to run; `<DbgHelp.h>` is types-only, Windows-only, for the layout pin).
  `check_no_std_containers` unchanged (still 74; (c1) adds 0). check-master-plan + check-repository PASS. No win-asan
  needed ((c) is not the importer). Row 076 stays **Open**.

Next tick: **(c2)** — Linux ELF build-id captured at `install()` (crash.cpp POSIX) + record field + `/proc/self/exe`
`.note.gnu.build-id` self-consistency; begins with the protocol's advisor + CI check.

## (c2) landed — Linux ELF build-id captured at install() and carried in the crash record

Completes (c): the Linux binary identity, the analogue of (c1)'s Windows RSDS GUID. Changed
[`crash.cpp`](../../engine/foundation/core/src/crash.cpp) (POSIX block only — Windows path untouched); test
[`test_diag_build_id.cpp`](../../tests/foundation/core/test_diag_build_id.cpp) (1 case / 25 assertions, Linux-only).

- **Captured at `install()`, never in the handler.** `dl_iterate_phdr` takes the loader lock and may allocate — both
  forbidden in the async-signal handler (crash.cpp's own comment) — so `capture_module_notes()` runs once at install
  (beside the existing `readlink("/proc/self/exe")` cache) and fills a **static** table `s_modules[64]` of
  `{base, id[20], id_len, path[128]}` plus `s_exe_build_id`. The callback walks each object's `PT_NOTE` segments for
  `NT_GNU_BUILD_ID` (name "GNU", type 3), bounds-checking every note against `p_memsz` so a malformed note can't
  overrun; `dlpi_name==""` is the main executable. The handler only **reads** this table.
- **Record lines (async-signal-safe, hand-formatted).** After the existing 5b fields (so 5b's parsers are unaffected —
  verified) the record gains `build_id <hex>` (the exe, load-bearing), `modules <n> [truncated 1]`, and one
  `module <base-hex> <id-hex|-> <basename>` per entry until a record-buffer headroom guard stops it
  (`modules_record_truncated 1`). New `ap_hex_bytes` appender (2 hex digits/byte, no `0x`). Budget: the record is the
  existing 8 KiB alt-stack buffer; a realistic process (exe + libc + a few `.so`) fits with room to spare.
- **Independent verification (the real check).** The test crashes the capture specimen, reads the `crash_*.log`,
  decodes its `build_id`, and compares it byte-for-byte against the specimen binary's `.note.gnu.build-id` parsed
  **directly from the ELF file** (`Elf64_Ehdr → Phdr[] → PT_NOTE → Nhdr` walk) — in-memory-at-install vs on-disk-ELF,
  the independence that makes it a real test, not a tautology. Also asserts `modules >= 2` (exe + libc) and that every
  `module` id column is exact 2-hex-per-byte or `-`. The full `[crash-capture]` suite (14 cases / 77 assertions) still
  passes — the grown record broke no 5b parser.
- **Scope fence:** (c2) ends at "the record carries it." Turning the record into a `SymbolIndex` section (the (c1)
  format) is the composer's job in a later slice — noted, not built.
- **Verified:** linux-gcc-debug — `[build-id]` 25 assertions / 1 case + `[crash-capture]` 77 / 14 green; crd-core-tests
  compiles on win-debug and crd-core on win-shipping (the change is entirely in the `#elif __linux__` block; the new
  test file is `#if defined(__linux__)`-guarded, empty on Windows). `check_no_std_containers` unchanged (74; (c2) adds
  0). check-master-plan + check-repository PASS.
- **What (c) now covers:** the identity **capture** (Windows RSDS readback (c1), Linux build-id at install (c2)) and
  the matching/rejection machinery ((c1)) are complete. Two census items are explicitly **routed, not dropped**: the
  record→`SymbolIndex` **composition** belongs to the composer slice, and the fresh-machine **symbolization demo**
  against a real dump is (f)'s (the acceptance's "symbolize an optimized fresh-machine crash").

Next tick: **(d)** — unloaded generations (Windows `MiniDumpWithUnloadedModules` + `UnloadedModuleListStream` readback,
"old DLL symbols after reload"; Linux dlclose-range decision); begins with the protocol's advisor + CI check.

## (d) landed — retain unloaded module generations

The acceptance's *"retain old DLL symbols after reload."* Windows has an OS-maintained unload trace the dump can carry;
Linux does not. Changed [`crash.cpp`](../../engine/foundation/core/src/crash.cpp) (+`MiniDumpWithUnloadedModules`),
[`symbol_index.hpp`](../../engine/foundation/perf/include/crd/perf/symbol_index.hpp)/[`.cpp`](../../engine/foundation/perf/src/symbol_index.cpp)
(stream-14 parser + `PeImage` identity), the crash specimen (new `unload_av` mode); tests
[`test_diag_symbol_index.cpp`](../../tests/foundation/perf/test_diag_symbol_index.cpp) (now 12 cases / 103 assertions)
and [`test_diag_unloaded_modules.cpp`](../../tests/foundation/core/test_diag_unloaded_modules.cpp) (1 case /
7 assertions, Windows-only).

- **Windows capture — one flag.** `MiniDumpWriteDump`'s type gains `MiniDumpWithUnloadedModules` (the single writer,
  fatal path + `capture_dump`). OS limits, documented: the trace is ntdll's ~64-entry ring and each entry's name is a
  **truncated basename** — so (d) retains the last ~64 unloads by basename, the honest ceiling.
- **crd-native stream-14 parser** (`parse_minidump_unloaded_modules`). The `UnloadedModuleListStream` (14) header
  `{size_of_header, size_of_entry, number_of_entries}` is **self-describing** — the entry stride is read from the file
  (`size_of_entry`), never `sizeof` — the inverse of the (c1) 108-vs-112 trap: here the format hands you the stride, so
  a future larger entry still parses; a `size_of_entry` below the v1 record is refused. Bounds-checked against the
  stream's declared size and the buffer; unloaded entries carry **no CV record**, so their identity is `TimeDateStamp +
  SizeOfImage`. Structs pinned by Windows `static_assert`s on the real `::MINIDUMP_UNLOADED_MODULE_LIST` (12 B) /
  `::MINIDUMP_UNLOADED_MODULE` (24 B). `build_symbol_index_from_minidump` now emits loaded **then** unloaded.
- **`SymbolIdKind::PeImage` — what makes it "retain old *symbols*", not just ranges.** `identity_matches` for PeImage
  requires equal `TimeDateStamp` **and** `SizeOfImage` with a non-zero stamp (a zero stamp is reproducible/stripped →
  refuse); `debug_file_path` builds the symsrv image key `<root>/<name>/<TimeDateStamp:08X><SizeOfImage:X>/<name>`
  (how a DLL's own debug info is fetched by image identity after unload). Two guards this forced, both landed + tested:
  the `id_len==0 → false` rule now applies only to Rsds/GnuBuildId (PeImage legitimately has no id bytes), and the
  `read_symbol_index` clamp bound moved to a `kMaxSymbolIdKind` constant (else every PeImage record would decode as
  `None`). Loaded-without-CV stays `None` — unchanged (c1) semantics. The unloaded flag rides in
  `SymbolRecord::flags` (reclaimed `_pad_a`; size still 72, pins hold) and round-trips through the section + bundle.
  - **ADR ID-1 (no schema bump):** reclaiming the pad as `flags` does not bump `kSymbolIndexVersion`, and that is
    compatible both directions — bit 0 of the former `_pad_a` was always 0 in every v1 payload (`{}`-zeroed), so an old
    payload decodes `unloaded=false` and an old (c1) reader ignores the new bit. It is moot in practice too: no v1
    SymbolIndex payload has left the working tree ((c1) and (d) are in the same still-uncommitted batch), so there is
    no on-disk N-1 to migrate. A real layout change (a size/offset shift) would still bump N per ID-1.
- **Real two-generation proof (Windows, out-of-process).** The `unload_av` specimen `LoadLibraryA("winhttp.dll")` →
  `FreeLibrary` (refcount 0 → traced unload) → `LoadLibraryA` again → fault. At crash time the dump names winhttp in
  **both** the UnloadedModuleListStream (old) **and** the ModuleListStream (live) — the core test reads both back via
  `read_dump_stream` (its own minimal walk; crd-core-tests does not link crd-perf) and asserts winhttp in each. This
  real dump is also exactly the artifact **(f)** can point `parse_minidump_modules` at for the (c1) "every module"
  evidence routed there.
- **Linux — a named limitation, not code.** glibc has no unload trace and no public post-unload hook: `dl_iterate_phdr`
  is install-time only (loader lock). So Linux retains the **install()-time generation** ((c2)'s table); `dlclose`'d
  ranges are not tracked, and a host that reloads `.so`s re-`install()`s to refresh. Alternatives rejected: link-time
  `--wrap dlclose` pushes a build-graph change into every consumer; `r_debug`/`_dl_debug_state` is a debugger-only
  interface, not a supported runtime hook.
- **Verified:** win-debug + win-shipping (`/W4 /WX`) + linux-gcc-debug — `[symbols]` 103/12 on all three, `[unloaded]`
  7/1 (Windows real dump), `[crash-capture]` unchanged (win 46/11, linux 77/14 — the new dump type broke no 5a/5b
  parser), `[build-id]` 25/1 unchanged. `check_no_std_containers` unchanged (74; (d) adds 0). check-master-plan +
  check-repository PASS. Row 076 stays **Open**.

Next tick: **(e)** — the trustworthy importer (bounded, non-executing reader: reject traversal / oversized /
decompression-bomb / version-mismatch, recover truncated bundles, round-trip golden manifests; adversarial corpus +
`linux-clang-fuzz` target; importer tests on **win-asan**). Likely splits **(e1)** reader + golden manifests /
**(e2)** adversarial corpus + fuzz + win-asan. Begins with the protocol's advisor + CI check.

## (e1) landed — manifest format + trustworthy importer + traversal fix + golden v1

(e) splits: **(e1)** [this tick] = the Manifest payload format + `import_bundle` (bounded, non-executing) + the
traversal fix + golden bundles + rejection unit tests + the **win-asan** run. **(e2)** [next tick] = the on-disk
adversarial corpus files + a libFuzzer target over `import_bundle` + `linux-clang-fuzz` registration + a win-asan
corpus replay. New: [`bundle_manifest.hpp`](../../engine/foundation/perf/include/crd/perf/bundle_manifest.hpp)/[`.cpp`](../../engine/foundation/perf/src/bundle_manifest.cpp),
[`bundle_import.hpp`](../../engine/foundation/perf/include/crd/perf/bundle_import.hpp)/[`.cpp`](../../engine/foundation/perf/src/bundle_import.cpp)
in crd-perf; tests [`test_diag_bundle_import.cpp`](../../tests/foundation/perf/test_diag_bundle_import.cpp) (9 cases /
114 assertions) plus traversal cases added to `test_diag_symbol_index.cpp`.

- **Manifest (tag 1) format.** Pinned `ManifestHeader{manifest_version, schema_version, flags, absent_count}` (16 B)
  + `u32 absent_tags[]`. **ID-8 enforced structurally:** there is no field, bit, or API that can declare the raw dump
  redacted — `kManifestFlagFieldsRedacted` refers only to the manifest's own strings, `ManifestFlags` is a closed
  known-mask, and `read_manifest` **reports** unknown bits (`unknown_flags`) without ever interpreting them.
  `absent_count` is bounded by `kMaxManifestAbsentTags` and the payload size (a hostile count is refused, not trusted).
- **`import_bundle` — bounded, non-executing.** Wraps `read_bundle` and adds, in order: oversized rejection (before
  reading a byte), `BadMagic`/`BadHeader`/`UnsupportedVersion` (container) → `SchemaMismatch` (cerid-diagnostics
  schema, which `read_bundle` does not check) → `TooManySections`. The declared `section_count` is **peeked from the
  raw header and rejected before the structural walk**, so a hostile count can never make `read_bundle` allocate a
  section view per declared entry (the post-walk check on the validated header stays as belt-and-braces); the walk
  itself is bounded by the buffer, which `max_total_bytes` already caps. Then per-section a byte cap
  (`kImportSectionOversized`, section not decoded), duplicate-tag detection (first wins), a refused compressed section
  (`compressed_unsupported`, prefix kept — the v1 decompression-bomb defence), and decode of Manifest + SymbolIndex.
  Reported-not-fatal: `total_len_mismatch`, `manifest_absent_mismatches`, `unsafe_names`. Borrows views into the input;
  decodes POD and bounds-checks; **never executes anything**; `noexcept` throughout.
- **The reported fields are exercised, not just declared.** Unit cases now cover `kImportSectionDuplicate` (two
  `CrashRecord` sections → second flagged, first section's bytes retained), `unsafe_names` (a `SymbolIndex` whose one
  module has `debug_file == ".."` → counted 1, never resolved — the importer's end of the traversal clause), and
  `manifest_absent_mismatches > 0` (a manifest that declares `LogTail` absent while a *present* `LogTail` section is in
  the bundle → mismatch 1, status still `Ok` — report, never reject). Note: truncation naturally produces mismatches
  for the sections it cut away; that is intended (the manifest still lists them absent-or-present as authored).
- **Traversal fix (the acceptance's "reject traversal").** `debug_file_path` returns "" for an empty / `.` / `..` /
  embedded-NUL basename (an embedded NUL would silently truncate the built `c_str` path); a directory-qualified name
  reduces to its safe basename. Exposed as `is_safe_lookup_name`, which the importer uses to count `unsafe_names`.
- **Golden v1 + migration guard.** Added `BundleWriter::finish(u64 created_at_ns)` so a bundle from fixed inputs is
  byte-reproducible; `finish()` now delegates with `diagnostic_now_ns()`. A frozen 208-byte golden (Manifest section 0
  declaring LogTail+SymbolIndex absent + a known CrashRecord + the two absent sections, `finish(0)`) is: imported →
  `Ok`, schema + `absent_tags=={LogTail,SymbolIndex}` + the two `Absent` sections recovered, 0 mismatches; **rebuilt →
  byte-identical to the frozen array** (the format has not drifted — that *is* the "schema migration" guard); cut after
  the manifest → `RecoveredTruncated` with the manifest intact; and with `format_version` bumped (crc recomputed) →
  `UnsupportedVersion`. (No N-1 reader yet — v1 is the first version; the whole batch is uncommitted.)
- **Verified:** win-debug + win-shipping (`/W4 /WX`) + linux-gcc-debug + **win-asan** (run inside vcvars) — 331
  assertions / 31 cases (`[symbols]`+`[bundle]`+`[import]`; `[import]` alone is 114/9) green, no ASan reports over the
  golden / truncated / corrupt / oversized / compressed / duplicate / traversal / manifest-mismatch inputs.
  `check_no_std_containers` unchanged (74; (e1) adds 0).
  check-master-plan + check-repository PASS. Row 076 stays **Open**.
- **Routing:** the design's "CLI import/inspect" is the library `import_bundle` here; the CLI executable itself is
  routed to a later slice (ADR: the bundle is "built by DIAG.5d/6c"). The real Windows unload dump from (d) remains
  the artifact (f) can point `parse_minidump_modules` at for the (c1) "every module" evidence.

Next tick: **(e2)** — on-disk adversarial corpus (truncated / traversal / oversized-length / decompression-bomb /
wrong-schema) + a libFuzzer target over `import_bundle` on `linux-clang-fuzz` + a win-asan corpus replay. Begins with
the protocol's advisor + CI check.

## (e2) landed — adversarial corpus + libFuzzer target over import_bundle

(e2) is the fuzzed, corpus-replayed proof of the DIAG.5d acceptance clause **"reject traversal / oversized /
decompression-bomb inputs"** (and "recover truncated bundles"). New: a fuzz target
[`fuzz_bundle_import.cpp`](../../tests/foundation/perf/fuzz/fuzz_bundle_import.cpp) registered by
[`tests/foundation/perf/fuzz/CMakeLists.txt`](../../tests/foundation/perf/fuzz/CMakeLists.txt) via the existing
`crd_fuzz_target()` (REPO.DEV.9; `cmake/CrdFuzz.cmake`), plus a committed 10-file corpus under
`tests/foundation/perf/fuzz/corpus/`. The perf test module's `crd_module()` in the root `CMakeLists.txt` now lists
`support/fuzz` in TESTS (the same declaration `execution/ceir` carries), which the build-graph verifier requires
before the target may link `crd-fuzz-harness`.

- **The target follows the established convention exactly.** `LLVMFuzzerTestOneInput` feeds the bytes to
  `import_bundle` with default limits, then again with tight limits (`max_sections=2`, `max_section_bytes=64`) so the
  `TooManySections` / per-section-cap reject paths get coverage on every input; `crd_fuzz_seeds` emits the corpus. The
  same source is built twice: the **replay** executable (`crd-fuzz-perf-bundle`, replay driver `replay_main.cpp`,
  registered as the `crd-fuzz-perf-bundle-corpus` CTest on **every** lane) and, under `CRD_ENABLE_FUZZER` on
  `linux-clang-fuzz`, the **libFuzzer** executable (`crd-fuzz-perf-bundle-libfuzzer`) from the same TU.
- **Oracle — self-consistency, not a round-trip** (the importer is a bounded, non-executing *reader*): it never
  crashes; every non-empty section payload is a borrowed view that lies strictly inside `[data, data+size)`; a reject
  code is set iff the status is `Rejected`; a non-rejected import carries the cerid-diagnostics schema; `unsafe_names`
  and `manifest_absent_mismatches` are bounded by what the input declared; and importing the same bytes twice yields
  the identical verdict (determinism). A violation aborts with its site (`CRD_FUZZ_REQUIRE`).
- **Corpus (10 files, each <1 KiB, generated by `--seed` — deterministic, content-hash names).** `golden-v1` (the
  frozen valid bundle), `truncated-after-manifest`, `truncated-mid-section`, `empty`, `garbage`, `traversal-dotdot`
  (a `SymbolIndex` whose one module's `debug_file` is `".."`), `wrong-schema` (schema+7, header crc recomputed),
  `too-many-sections` (`section_count = 0xFFFFFFFF`, crc recomputed — the (e1) pre-walk peek path), `oversized-length`
  (section-0 `payload_length` = 4 GiB, a lying length), `bomb-compressed` (the compressed flag with a ~4 GiB
  `original_length` and a 16-byte stored payload). crc32s are recomputed only where a case must reach a deep check
  rather than being rejected earlier as `BadHeader`. The specific reject/recover *outcomes* for these shapes are
  asserted with explicit expected values in the unit suite (`test_diag_bundle_import.cpp`, 114/9); the corpus adds the
  no-crash + determinism + in-bounds proof over the same shapes and is the regression set the replay CTest enforces.
- **Verified.** Replay (`crd-fuzz-perf-bundle-corpus`) green on **win-debug** and **win-asan** (10 inputs, 0 crashes,
  no ASan report). The real fuzzing lane ran too: on **linux-clang-fuzz** the libFuzzer build
  (`crd-fuzz-perf-bundle-libfuzzer`, tree-wide coverage + `-fsanitize=fuzzer`, clang 18 in WSL) executed
  **1,659,856 inputs in 21 s (~79k exec/s), 0 crashes, peak RSS 515 MB** — no crash, no oracle failure, no OOM over
  `import_bundle`. `check_no_std_containers` unchanged (74; (e2) adds 0 — the target uses `crd::` containers, the
  replay driver is pre-existing). check-repository + check-master-plan PASS. libFuzzer writes discovered inputs into
  its first positional dir; those artifacts were removed and are not committed -- the committed corpus is exactly the
  `--seed` output, which is deterministic (re-seeding yields byte-identical content-hash names).
- **Routing.** This is the DIAG.5d acceptance's adversarial-input proof; it also pre-stages **DIAG.10a** ("extend the
  bounded fuzz framework to … capture/bundle/policy readers … register every new ingress surface with a fuzz owner").
  Row 076 stays **Open** — (f) is the clause-by-clause acceptance review that flips it.

Next tick: **(f)** — the clause-by-clause DIAG.5d acceptance review (symbolize a fresh-machine crash from identity;
reject-wrong-symbols; recover-truncated; reject traversal/oversized/bomb; retain unloaded DLL symbols; golden schema
migration + explicit absent; raw dumps never declared redacted), assembling the evidence already landed across
(b)–(e2), then flip row 076 Open→Needs CI with a Session link + latest actions/runs URL. Begins with the protocol's
advisor + CI check.

## (f) acceptance review — clause-by-clause, then row 076 → Needs CI

The final sub-unit: verify every DIAG.5d acceptance clause (and the governing design-body clauses) against the
evidence landed across (b)–(e2), close the one gap (real-dump fresh-machine symbolization), and flip the row. **This
tick added one test** — [`test_diag_symbolize_e2e.cpp`](../../tests/foundation/perf/test_diag_symbolize_e2e.cpp)
(`[perf][diag][symbols][e2e]`) — which ties (c1)+(d)+(e1) end to end on a **real** minidump: the `unload_av` capture
specimen → a real dump with two winhttp generations → `build_symbol_index_from_minidump` → a SymbolIndex bundle
section → `import_bundle`, then asserts every identity-bearing module resolves to a safe non-empty `debug_file_path`
(a `None`-identity module resolves to `""` — refuse, don't guess), at least one module is an unloaded generation, and
a wrong identity (`age+1`) is rejected by `identity_matches`. The perf test module now carries the
`crd-diag-crash-capture-specimen` dependency + `CRD_DIAG_CRASH_CAPTURE_SPECIMEN` define (mirroring `crd-core-tests`).

### Acceptance clauses (verbatim from the design)

| Clause | Evidence | Verdict |
|---|---|---|
| symbolize an optimized fresh-machine crash | `[e2e]` real dump: every-module identity → `debug_file_path(m, "S:/sym")` non-empty + safe; `[symbols]` PDB-GUID/age + `.build-id` conventions. The frame→line symbolizer is the **offline consumer** (5b's rule: symbolization runs offline, never in the handler) — this proves the identity + matching-file path it consumes. | **Proven** (frame resolver routed to the offline consumer) |
| reject wrong symbols instead of giving plausible lines | `identity_matches` rejects wrong GUID/age/build-id (`[symbols]`); `[e2e]` `age+1` on a real module fails to match; a `None`-identity module returns `""` rather than a guess. | **Proven** |
| recover truncated bundles | `read_bundle`/`import_bundle` return `RecoveredTruncated` with the intact prefix (`[bundle]`, `[import]` truncated-after-manifest / mid-section). | **Proven** |
| reject traversal / oversized / decompression-bomb inputs | `[import]` rejects oversized/too-many/schema-mismatch, flags oversized sections, refuses compressed (bomb defence), counts traversal names; the (e2) committed corpus + `crd-fuzz-perf-bundle-corpus` replay (every lane) + libFuzzer (1,659,856 execs, 0 crashes). | **Proven** |
| retain old DLL symbols after reload | Real two-generation winhttp dump: core `[unloaded]` (7/1, capture side) + `[e2e]` `unloaded>0` through the crd-perf bundle. | **Proven** |
| golden manifests preserve schema migration + explicit absent sections | `[import]` golden v1 drift guard (rebuild == frozen bytes) + explicit `Absent` sections + manifest `absent_tags`. | **Proven** |
| raw dumps are never declared fully redacted (ID-8) | Structural: no field/bit/API in `bundle_manifest.*` can claim the raw dump redacted; `read_manifest` reports unknown flags without interpreting (`[import]`). | **Proven (structural)** |

### Design-body clauses

| Clause | Evidence | Verdict |
|---|---|---|
| package artifacts atomically + partial-section recovery | `write_bundle_atomic` (tmp → flush → rename, no leftover `.tmp`; `[bundle]`) + `RecoveredTruncated`. | **Proven** |
| quota / retention (≤256 MiB, ≤10 bundles) | `enforce_bundle_retention` evicts oldest-first (`[bundle]`); per-section + total byte caps in the writer and importer. | **Proven** |
| PDB GUID/age; ELF build ID + matching debug files | `[symbols]` RSDS parse + symstore path; (c2) POSIX build-id capture (core, Linux) + `debug_file_path` `.build-id/<nn>/<rest>.debug` branch. | **Proven** |
| source mapping (identity → matching debug/source file) | `debug_file_path` maps each module identity to its matching debug-file lookup path; frame→line mapping is the offline consumer's step. | **Proven** (frame mapping routed) |
| CLI import/inspect is bounded and does not execute content | `import_bundle` is the bounded, non-executing **library** (borrows views, decodes POD, `noexcept`); the CLI **executable** is routed to **DIAG.6c** per the ADR ("built by DIAG.5d/6c"). | **Library proven; CLI exe routed to 6c** |
| dSYM / WASM identities | Design says "later"; not in scope for v1. | **Routed (later)** |
| Linux unloaded-module attribution | No OS-native unloaded list on Linux; (d) named it a limitation (would need a `dlclose` hook the engine lacks). | **Named limitation** |
| N-1 schema reader | v1 is the first version; a bump ships an N-1 reader (ADR ID-1). | **Routed (v1 is first)** |

### Verification (fresh totals this tick)

- **perf** `[symbols],[bundle],[import],[e2e]` — **win-debug 405/32**, **win-asan 332/32** (the `[e2e]` case takes the
  ASan early-return: under win-asan the specimen's AV is owned by ASan, no crd dump — `exit≠0` asserted, never
  skip-as-pass; the importer/`[symbols]`/`[bundle]`/`[import]` run clean under ASan).
- **fuzz** `crd-fuzz-perf-bundle-corpus` — win-debug + win-asan replay green (10 inputs, 0 crashes); libFuzzer on
  linux-clang-fuzz (1,659,856 execs, 0 crashes) from (e2).
- **core** `[unloaded]` 7/1 (Windows real dump, (d)); (c2) POSIX build-id + (b)/(c)/(c1)/(c2)/(d)/(e1)/(e2) as recorded
  in the sections above.
- `check_no_std_containers` unchanged (**74**; (f) adds 0). check-master-plan + check-repository **PASS**.

Row 076 (DIAG.5d) → **Needs CI**: every acceptance clause is proven or explicitly routed/named; the batch is
uncommitted (the DIAG.0 gate and the user's push remain), so CI is cited at the latest run and flips to Done when a
newer green run lands.
