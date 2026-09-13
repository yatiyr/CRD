# Memory reference: build and verification

<!-- doc-role: reference -->
> Technical reference; verify dated claims against current contracts/source. Current work: [ROADMAP](../../ROADMAP.md); current rules: [AGENTS](../../../AGENTS.md).

> Reference corpus, consolidated 2026-09-12; not a live tracker. Read [AGENTS](../../../AGENTS.md),
> [MEMORY](../../../MEMORY.md) and [ROADMAP](../../ROADMAP.md) for current rules/status.
> Dated state, loop grants, tool paths and schedules below are historical. Reusable engineering lessons remain
> applicable unless superseded by current instructions. Retrieve one named record; do not load this whole file on entry.

<a id="memory-build_system"></a>
## build_system

---
name: Cerid build system
description: How to build, test, and run smokes — paths, ASan DLL fix, PowerShell env setup
type: reference
originSessionId: 354328a5-f90d-4042-956f-04d12edadcfb
---
Visual Studio is installed at: `C:\Program Files\Microsoft Visual Studio\18\Community\` (VS 2026, version 18).

**PowerShell env setup block** (use at start of each build session — sets PATH/INCLUDE/LIB for cmake + MSVC + SDK):
```powershell
$vsBase   = "C:\Program Files\Microsoft Visual Studio\18\Community"
$ninja    = "$vsBase\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja"
$cmake    = "$vsBase\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
$msvcBase = "$vsBase\VC\Tools\MSVC"
$msvcVer  = (Get-ChildItem $msvcBase | Sort-Object Name | Select-Object -Last 1).Name
$cl       = "$msvcBase\$msvcVer\bin\Hostx64\x64"
$msvcInc  = "$msvcBase\$msvcVer\include"
$msvcLib  = "$msvcBase\$msvcVer\lib\x64"
$sdkBase  = "C:\Program Files (x86)\Windows Kits\10"
$sdkVer   = (Get-ChildItem "$sdkBase\Include" | Sort-Object Name | Select-Object -Last 1).Name
$sdkInc   = "$sdkBase\Include\$sdkVer"
$sdkLib   = "$sdkBase\Lib\$sdkVer"
$sdkBin   = "$sdkBase\bin\$sdkVer\x64"  # rc.exe, mt.exe
$env:PATH    = "$ninja;$cmake;$cl;$sdkBin;$env:PATH"
$env:INCLUDE = "$msvcInc;$sdkInc\ucrt;$sdkInc\shared;$sdkInc\um"
$env:LIB     = "$msvcLib;$sdkLib\ucrt\x64;$sdkLib\um\x64"
Set-Location "D:\Dev\cerid"
```

Then run: `cmake --preset win-debug` then `cmake --build --preset win-debug`

**NOTE:** If the cmake build directory is corrupted (e.g. failed configure from adding C-language source), delete the build dir and reconfigure: `Remove-Item -Recurse -Force "D:\Dev\cerid\build\win-debug"` then `cmake --preset win-debug` with the env block above.

**ASan DLL fix (CRITICAL — must do every session before win-asan ctest):**
CTest for win-asan fails with Exit 0xc0000135 (DLL not found) unless the MSVC tools dir is in PATH. The env setup block above already adds it. The DLL is `clang_rt.asan_dynamic-x86_64.dll` in the `$cl` directory.

**Running executables in PowerShell:**
Always use absolute paths with the `&` call operator:
```powershell
& "D:\Dev\cerid\build\win-debug\runtime\smoke_resources.exe"
```
Relative paths `.\build\...` fail with "not recognized as cmdlet" in some contexts.

Presets: win-debug, win-release, win-asan, win-relwithdebinfo, win-clang-cl, win-tidy

**Headless smoke list** (17 smokes, run for all testable configs as part of DoD):
```powershell
$smokes = @("smoke_config","smoke_containers","smoke_filesystem","smoke_frame_clock",
            "smoke_jobs","smoke_log","smoke_math","smoke_memory","smoke_shader",
            "smoke_resources","smoke_resources_async","smoke_resources_reload",
            "smoke_resources_stream","smoke_resources_render","smoke_texture","smoke_mesh","smoke_material")
$base = "D:\Dev\cerid\build\win-debug\runtime"
foreach ($s in $smokes) { & "$base\$s.exe" 2>&1 | Out-Null; if ($LASTEXITCODE -ne 0) { Write-Host "FAIL: $s" } }
```

**CRITICAL: vcvars must be loaded in the SAME PowerShell call as cmake**
`[System.Environment]::SetEnvironmentVariable` does NOT persist across separate PowerShell tool calls (each call is a new process). Always load vcvars AND run cmake in a single PowerShell command:
```powershell
$vcvars = "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
$tempFile = [System.IO.Path]::GetTempFileName() + ".txt"
cmd /c "`"$vcvars`" && set > `"$tempFile`""
$lines = Get-Content $tempFile
foreach ($line in $lines) { if ($line -match '^([^=]+)=(.*)$') { [System.Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], 'Process') } }
Remove-Item $tempFile -ErrorAction SilentlyContinue
cmake --build --preset win-tidy  # <-- cmake call in same block
```

**Catch2 flag**: `--reporter compact` (NOT `--reporters` — causes parse error)

**`crd-jobs` `/Od /Y-` pin on `worker_pool.cpp` + `fiber_init.cpp` is load-bearing — do NOT remove it.** `engine/jobs/CMakeLists.txt` pins those two TUs to `/Od` on MSVC. At `/O2` the optimizer caches the thread-local segment base across the hand-rolled `fiber_switch` asm, so a post-resume TLS read (after a fiber migrates to another OS thread) returns the wrong thread's slot → SEGFAULT in essentially every job. The `CRD_JOBS_TLS_OPAQUE` source guard helps but is NOT sufficient on the MSVC toolset GitHub `windows-latest` ships (VS 2022). It was dropped in `a63f80d` (2026-05-12) after a *local* VS 2026 sweep looked clean → every `crd-jobs` test SEGFAULT'd in win-release + win-shipping on CI. Restored 2026-05-12 same day. A clean local win-release/win-shipping build on VS 2026 does NOT reproduce the crash — only CI does — so don't "confirm it can be dropped" from a dev-box sweep.

**Stale-`.obj` in secondary build dirs (win-debug-scalar/-sse2/-tidy, etc.) — ninja-CLI sibling of the C4789 issue.** When a class in a widely-included header grows a member (e.g. `Array<T>` gained `m_freeze_depth` in D-002, 2026-05-12), ninja in a long-untouched secondary build dir may re-link a library/exe from a `.obj` compiled against the OLD header without recompiling it → ABI skew (`sizeof` mismatch across TUs) → bizarre symptoms far from the cause (seen: a `crd::containers::Array` freeze-guard assert firing inside `CrdrWriter::add_chunk` during shader cooking, breaking the `win-debug-scalar` build, even though no code is wrong). Diagnose by comparing `.obj` mtimes vs the changed header's mtime (`stat -c '%y' build/<preset>/.../foo.cpp.obj`). Fix: `Remove-Item -Recurse -Force build\<preset>` then reconfigure+rebuild — a plain `cmake --build` will NOT fix it (ninja thinks the `.obj` is current). Prevention: run `scripts/full-sweep.ps1 -Reconfigure` (not the plain form) whenever a core header changed since the secondary dirs were last fully built. Release/shipping configs are immune to this *particular* one because `CRD_ENABLE_ASSERTS` is off so the new member doesn't exist in either ABI.

**clang-cl PATH**: `$vsBase\VC\Tools\Llvm\x64\bin` (add this to PATH for win-clang-cl preset)

## cdb (Console Debugger) — for crash-dump analysis

Installed via `winget install Microsoft.WinDbg` (Microsoft Store WinDbg appx ships cdb as the console-mode engine).

**Path:** `C:\Program Files\WindowsApps\Microsoft.WinDbg_1.2603.20001.0_x64__8wekyb3d8bbwe\amd64\cdb.exe` (other archs at `arm64/`, `x86/` siblings; we use amd64).

**Workflow for any access violation / SEGFAULT** (sandbox or test):

1. Crash dumps land at `./crashes/crash_YYYYMMDD_HHMMSS.dmp` (relative to exe's working dir).
2. Non-interactive analysis:
   ```powershell
   & "C:\Program Files\WindowsApps\Microsoft.WinDbg_1.2603.20001.0_x64__8wekyb3d8bbwe\amd64\cdb.exe" `
       -z .\crashes\crash_YYYYMMDD_HHMMSS.dmp `
       -y "srv*c:\symbols*https://msdl.microsoft.com/download/symbols" `
       -c "!analyze -v; ~*kn 30; q" 2>&1 | Select-Object -Last 80
   ```
3. `-c` script: `!analyze -v` annotated crash report + `~*kn 30` per-thread stack (30 frames) + `q` exit.

**Don't fall back to log-bisecting on a SEGFAULT.** Adding logs perturbs inlining + timing + hides the real frame; the dump file already has the complete stack + register state. Saved as feedback memory `feedback_use_crash_dumps_first.md`.


<!-- end-memory:build_system -->

<a id="memory-feedback_ascii_only_test_names"></a>
## feedback_ascii_only_test_names

---
name: test-case-names-must-be-ascii-only-never-em-dash-never-math-glyphs
description: "Cerid's `crd-no-non-ascii-test-names` CI guard fails any TEST_CASE name containing characters outside 0x00-0x7F. Em-dash, equivalent (≡), degree (°), arrows, etc. are all blocked"
metadata:
  node_type: memory
  type: feedback
  originSessionId: 307daaf4-04ca-4f85-b2f3-6606266d6f97
  modified: 2026-09-06T16:04:18.491Z
---

When writing `TEST_CASE("...")` strings, the name MUST contain only
ASCII (0x00-0x7F). The CI guard `crd-no-non-ascii-test-names`
(`scripts/check_no_non_ascii_test_names.{ps1,sh}`) fails any test
binary whose `TEST_CASE` line in source contains a non-ASCII byte.

**Common offenders Claude defaults to** (and must NOT use):

- `—` (em-dash, U+2014) — use ` -- ` or `: ` instead.
- `≡` (equivalent, U+2261) — use `==` or `equiv` instead.
- `°` (degree sign, U+00B0) — use `deg`.
- `→` (rightarrow, U+2192) — use `->`.
- `≤` / `≥` (U+2264, U+2265) — use `<=` / `>=`.
- `²` / `³` (superscripts) — use `^2` / `^3`.
- `π` (U+03C0) — use `pi`.

**Why this is a hard rule**: Windows `ctest` re-encodes argv via the
Active Code Page (CP-1252, CP-936, etc.). A test name with non-ASCII
arrives at the Catch2 binary mojibaked, so the per-test filter
`ctest -R <name>` MISSES it. CTest then reports the test as
"NotRun" → exit 8 → slice closure blocked. The guard exists to catch
this at slice-close time instead of in some downstream CI hop.

**How to apply**: every TEST_CASE name is ASCII. Comments above /
below the test are not scanned — describe the math freely there
(comment-only Unicode is fine). The test NAME itself stays plain.

**How to verify before slice close**: `ctest --preset win-debug -R
crd-no-non-ascii` should return PASS. Alternatively, the guard runs
as part of the full per-slice DoD.

**⛔ 2026-09-04 (D-007 25b-1): a TAG-filtered exe run HIDES it — the
per-tick win gate must be `run-ctest.bat <dir> <regex>` (by-name),
NEVER `crd-ceir-gpu-tests.exe "[tag]"`.** A 25a-2 TEST_CASE name had
an em-dash; the direct exe with `[autodiff]` reported "All tests
passed" (89/89 FALSE-GREEN — the tag filter matches by tag, never
touches the mojibaked name), so the slice "closed" green. The very
next tick's `ctest -R` (by-name) surfaced it: `No test cases matched
'...gemm(A,A)->reduce(sum) � backward...'` → counted Failed → exit 8.
The bug was invisible to the tool I closed with. Run by-name.

**⛔ 2026-09-06 (D-007 32e): RE-HIT, same mechanism.** A `TEST_CASE`
name had an em-dash (`— a fresh identity`); `crd-chir-tests.exe
"[chir]"` was GREEN (138/19) but `ctest -R "chir 32"` reported #751
`— fail (the name→filter round-trip mojibaked → "No tests ran"). This
memory PREDICTED it and I still wrote the em-dash — the rule is not
sticking at WRITE time. The gate (ctest-by-name) caught it, but only
because I ran ctest and not just the bare `[chir]` exe. Fix at write
time: TEST_CASE names ASCII, ALWAYS.

**Suppression**: if a non-ASCII char is truly load-bearing (rare),
add a `crd-lint-allow-non-ascii-test-name` marker comment on the same
line as the TEST_CASE. Not for general use.

**Why I keep hitting this**: em-dash and `≡` look great in
prose. They are the natural way I write English. They are
**incompatible with Windows test discovery**. Apply this rule at
WRITE time, not at slice-close fix-up time.

Related: [catch_discover_tests_bracket_comma](build-and-verification.md#memory-feedback_catch_discover_tests_bracket_comma) (different special-
char foot-gun in the same `catch_discover_tests` pipeline).


<!-- end-memory:feedback_ascii_only_test_names -->

<a id="memory-feedback_band_close_reclassify_every_ensure_star_builder_vs_loader"></a>
## feedback_band_close_reclassify_every_ensure_star_builder_vs_loader

---
name: feedback_band_close_reclassify_every_ensure_star_builder_vs_loader
description: "At a band CLOSE that claims 'everything converted to authored assets', RE-CLASSIFY every ensure_*/builder candidate (don't inherit the conversion slice's 'all done' claim) — the discriminator is hardcoded output STRUCTURE vs disk-derived; a hand-built FS with authorable choices is a BUILDER even if commented 'fixed pass contract'"
metadata:
  node_type: memory
  type: feedback
  originSessionId: 192f4d9e-b3f7-485b-a775-5391bcd4183c
  modified: 2026-08-16T03:03:23.997Z
---

**CEIR-18z (the renderer-band close, 2026-08-16).** The band-proof close is where the authored-asset invariant gets
RE-VERIFIED, not inherited. CEIR-18p had claimed "author + delete every hand-built ensure_* program/kernel"; the 18z
close audited ALL 26 `ensure_*` in scene_renderer.cpp anyway (the gate-reverifies-rows rule) and FOUND one the
conversion slice had left: `ensure_visbuffer_fs` — a 3-node hand-built KGraph (the visbuffer id → graded-grey
`(primId+1)·0.25`). Its comment argued it was a "fixed contract of the pass KIND, like the empty depth-only FS." That
comment was the 18p-era decision UNDER AUDIT — the defendant, not a witness; recording an exception on its authority is
inheriting the exact row the close exists to re-verify.

**The discriminator (advisor-locked).** A LOADER/cooker-seam: `KGraph g` + `ckir_read`/`cook_stage_named`/`resolve_program_text`,
OR node-creation whose output STRUCTURE derives from a disk asset/declaration (e.g. `cook_post_graph(pdesc)` then
`vec4(out, 1)` output-wiring, or `build_fs_for_pass(disk .crdm)` with the fixed fullscreen inputs wired in — the
FragCoord/vec2 adaptor lines are the sanctioned seam). A BUILDER (the anti-pattern → convert+delete): node-creation whose
output STRUCTURE is HARDCODED in C++. The empty-depth-FS "fixed contract" exemption holds ONLY for genuinely zero-output
contracts; a 3-node FS that makes authorable CHOICES (a `+1`, a `·0.25` scale, a grey replication) is a builder — convert
it to a `.ckir` even though it "just writes an id."

**How to apply.** (1) At any band/slice close that claims "all X are now assets", enumerate + read the first ~15 lines of
EVERY candidate (pattern-matching is not classifying — record a table: name → loader/builder → the disk asset it loads).
(2) A "fixed pass contract" comment on a hand-built graph is a claim to RE-TEST, not accept. (3) FINDING a builder at the
close STRENGTHENS it (the 17z precedent — a close that surfaces a latent gap is stronger than one that finds none); write
it up that way. (4) Convert via the loader template (`resolve_program_text → ckir_read → create_program`, LOUD on missing),
move the builder body into a `[.emitckir]` regen test IN-TREE till the user commits ([feedback_uncommitted_delete_loses_the_source_keep_regen_in_tree](workflow-and-correctness.md#memory-feedback_uncommitted_delete_loses_the_source_keep_regen_in_tree)),
add a committed-asset load gate, and re-run the BEHAVIORAL device gate that renders through it. Related:
[feedback_gate_reverifies_status_matrix_rows_never_inherits_stale_or_unverified_claims](workflow-and-correctness.md#memory-feedback_gate_reverifies_status_matrix_rows_never_inherits_stale_or_unverified_claims),
[feedback_authored_asset_slice_done_only_when_cpp_deleted_and_renders](rendering.md#memory-feedback_authored_asset_slice_done_only_when_cpp_deleted_and_renders), [feedback_everything_is_an_authorable_asset_ceir](execution-ir.md#memory-feedback_everything_is_an_authorable_asset_ceir).


<!-- end-memory:feedback_band_close_reclassify_every_ensure_star_builder_vs_loader -->

<a id="memory-feedback_build_grep_hides_link_errors"></a>
## feedback_build_grep_hides_link_errors

---
name: feedback_build_grep_hides_link_errors
description: "Filtering build output with grep \"error C|Linking\" silently hides linker failures (fatal error LNK…) and reads them as success; check the exit status, and never rebuild a target while a background run holds its executable"
metadata:
  node_type: memory
  type: feedback
  originSessionId: b0138d6a-548b-428b-87b2-fe30c9f36f7c
---

**Two build-tooling scars, 2026-07-20, found together.**

**1. The filter hid the failure.** I had been checking builds with

    cmd //c "scripts\build-target.bat build\win-debug <target>" 2>&1 | grep -E "error C|Linking"

`error C` matches MSVC *compile* diagnostics (`error C2065`) but **not linker ones** (`fatal error LNK1168`). When the
link failed, the filter still printed an earlier `Linking` line and I read it as success — so a new test file was
silently never linked, and the symptom surfaced later as Catch2's `No test cases matched`, which reads like a CMake
registration problem and sends you looking in the wrong place.

**How to apply:** never infer build success from a grep pattern. Either let the exit status decide

    cmd //c "scripts\build-target.bat build\win-debug <target>" > /tmp/b.log 2>&1; echo "exit=$?"; tail -5 /tmp/b.log

or, if filtering, match `error` broadly (`grep -iE "error|warning C4|Linking"`) so `LNK`/`fatal` cannot slip through.
Corollary: `No test cases matched` after adding a test file usually means the binary was NOT rebuilt, not that CMake
missed the file — verify the registration AND the link before debugging CMake.

**2. Why the link failed at all.** `LNK1168: cannot open <target>.exe for writing` — a background full regression was
**running** the very executable being relinked. Launching a long regression and then rebuilding the same target is
self-conflicting. Sequence them: wait for the run to finish (check `tasklist | grep <exe>`) before rebuilding, or build
first and run second. Related: [reference_build_test_workflow](build-and-verification.md#memory-reference_build_test_workflow), [feedback_per_slice_run_ctest](build-and-verification.md#memory-feedback_per_slice_run_ctest).


<!-- end-memory:feedback_build_grep_hides_link_errors -->

<a id="memory-feedback_catch_discover_tests_bracket_comma"></a>
## feedback_catch_discover_tests_bracket_comma

---
name: catch-discover-tests-bracket-comma
description: "TEST_CASE names containing `[xyz)` bracket-comma patterns fuse multiple Catch2 cases into one CTest entry — avoid them"
metadata:
  node_type: memory
  type: feedback
  originSessionId: 4b3c3c8b-65fa-4775-a24e-20d4c6d74b02
---

`catch_discover_tests` (CMake-side Catch2 discoverer) mis-parses `TEST_CASE` names that contain `[…,…)` substrings (open-bracket + content with comma + close-paren). The result is that all test cases listed after the bad one fuse into a single CTest entry whose "name" is a `;`-delimited blob of their Catch2 names. CTest then dispatches that compound string as `--test-spec`, finds zero Catch2 matches in the binary, exits non-zero. The Catch2 test binary itself runs all cases fine — this is a test-NAME bug, not a logic bug.

**Why:** the discoverer's regex apparently treats `[…]` as a tag delimiter; when it sees `[0, N)` the comma confuses parsing and it loses test-boundary tracking from that point through end-of-list. (Verified empirically at Phase 3.1.7 v9a-b1 close 2026-05-18 — test name `"sort_morton_pairs u32 pair integrity: indices form a permutation of [0, N)"` fused 14 downstream cases into one failing CTest entry; renaming to drop the `[0, N)` substring fixed all 4 runtime configs.)

**How to apply:** never write `TEST_CASE` names containing bare `[…,…)` mathematical-interval substrings. Acceptable patterns:
- Plain English: `"…permutation"`, `"…in range 0 to N"`.
- Tags use a separate string arg, not the name: `TEST_CASE("name", "[tag][tag2]")`.
- If you absolutely need interval notation, escape the bracket: `\[0, N\)` — but plain English is simpler.

The win-tidy preset passes (which only does build) while win-debug/asan/shipping/release fail ctest with exit=8 — that combination is the fingerprint of a `catch_discover_tests` registration bug, not a runtime regression.

Related: [per-slice-run-ctest](build-and-verification.md#memory-feedback_per_slice_run_ctest), [v9-gpu-sanity-harness](device-programs.md#memory-feedback_v9_gpu_sanity_harness).


<!-- end-memory:feedback_catch_discover_tests_bracket_comma -->

<a id="memory-feedback_ceir_deserialize_build_raw_graceful_reject_never_factory_assert"></a>
## feedback_ceir_deserialize_build_raw_graceful_reject_never_factory_assert

---
name: feedback_ceir_deserialize_build_raw_graceful_reject_never_factory_assert
description: "CEIR deserialization (binary decoder + text parser) must build the value RAW and check canonicality GRACEFULLY, never route hostile input through a canonicalizing factory whose intern() asserts"
metadata:
  node_type: memory
  type: feedback
  originSessionId: fade8ea4-87ca-470f-83e0-cdfe82a44e7f
  modified: 2026-08-09T13:18:49.325Z
---

CEIR-8b scar. `Context::attr_dict` **canonicalizes** (byte-order sorts keys) but does **not** dedup; `intern_attr`
then `CRD_ASSERT`s `attr_is_canonical` (which rejects duplicate/unsorted keys). So a hostile TEXT input
`{"a":1,"a":2}` routed through `attr_dict → intern_attr` **CRASHES on the assert** instead of failing gracefully —
violating the reject-not-assert triple (a parser/decoder must return a `ParseResult` error on malformed input, never
abort). The advisor caught this pre-close.

**Why:** a factory's canonicalize-then-assert contract is for TRUSTED authoring (the caller promises a repairable
value); a deserialization boundary handles UNTRUSTED bytes and must treat any non-canonical record as an error, not
repair-or-die.

**How to apply:** on EVERY deserialization path (binary decoder arms in `binary.cpp`, text parser arms in
`parse.cpp`), build the value with the RAW factory (`AttrValue::of_dict`/`of_array`/`of_extern`, `Type` struct-fill),
call `attr_is_canonical`/`type_is_canonical` and `fail()` GRACEFULLY, THEN `intern_*` (whose assert is now
guaranteed to pass). NEVER route decoded/parsed input through a canonicalizing convenience factory (`attr_dict`
sorts; the binary/text arms must not). This recurs for every open-world vocabulary in band 8 (8c effect locations,
8d stable-id records, 8e region/interface) — each gets a canonicalizing factory AND a deserialization path.

Companion (same slice): a version range-check added to the binary decoder but not the text parser is a latent
text/binary **asymmetry** — both serial forms must agree on validity. 8b retrofitted the check into 8a's
`parse_extern` (types) too. See [feedback_ceir_oracle_u32_arithmetic_must_wrap_mod32](device-programs.md#memory-feedback_ckir_oracle_u32_arithmetic_must_wrap_mod32) for the sibling
canonicality-guard discipline and [project_ceir_master_spine_locked](project-history.md#memory-project_ceir_master_spine_locked) for the band context.


<!-- end-memory:feedback_ceir_deserialize_build_raw_graceful_reject_never_factory_assert -->

<a id="memory-feedback_clang_tidy_after_every_slice"></a>
## feedback_clang_tidy_after_every_slice

---
name: clang-tidy-after-every-slice
description: "User mandate 2026-05-14 — run clang-tidy (`win-tidy` preset) after every slice as part of the per-slice verification, not just at sub-phase close."
metadata:
  node_type: memory
  type: feedback
  originSessionId: 3c7d76c4-dcca-4d23-a491-f532640abae7
---

Run clang-tidy after every slice. Standard per-slice verification is now: win-debug + win-asan + win-shipping ctest + win-tidy build (treats tidy warnings as build output to scan).

**Why:** User asked 2026-05-14 after the v3b slice. The pre-existing v2 tidy warnings (`test_gjk_distance.cpp`, `test_hull_queries.cpp`, `test_simd_support.cpp` modernize-use-std-numbers, readability-identifier-naming, bugprone-unchecked-optional-access etc.) accumulated invisibly because tidy was only checked at sub-phase close. Catching tidy warnings per-slice prevents this debt accumulation; each slice ships clean.

**How to apply:**
- After implementing each slice's engine + tests, run `cmake --build --preset win-tidy --target <my-test-binary>` (the target that exercises the new code).
- Filter output for `warning:|error:` patterns; tidy emits per-file warnings inline with the compile step.
- New v3+ slice warnings → fix before declaring slice done.
- Pre-existing carry-over warnings (from v2 or earlier) → leave alone unless the slice's session log calls them out as in-scope; they're debt for a dedicated tidy-paydown slice.

**Key tidy rules surfaced so far:**
- `readability-isolate-declaration` — split `f64 a, b;` into `f64 a; f64 b;`
- `modernize-use-std-numbers` — prefer `std::numbers::pi_v<f64>` etc. over numeric literals
- `readability-identifier-naming.LocalConstexprVariable` — local `constexpr` requires `kCamelCase` (the `.clang-tidy` is authoritative; CLAUDE.md says "lower_case" which is outdated documentation)
- `bugprone-unchecked-optional-access` — wrap `optional<X>` reads in `if (opt)` or `opt.value()` after explicit `has_value()` check
- `misc-unused-using-decls` — remove unused `using X = ...;` aliases
- `bugprone-suspicious-memory-comparison` — `memcmp` on types without unique object representation (e.g. `Vec3<f32>` with padding) is unreliable; compare member-by-member


<!-- end-memory:feedback_clang_tidy_after_every_slice -->

<a id="memory-feedback_clang_tidy_ci_local_version_skew"></a>
## feedback_clang_tidy_ci_local_version_skew

---
name: feedback-clang-tidy-ci-local-version-skew
description: "clang-tidy categorisation of identifier kinds (Constant vs LocalConstexprVariable vs StaticConstant) varies by version. CI uses LLVM 17 by default; local uses LLVM 19. Pin both, AND set explicit ConstantCase + StaticConstant + LocalConstant rules in .clang-tidy as defensive duplicates so the config is version-independent."
metadata:
  node_type: memory
  type: feedback
  originSessionId: b24674c3-970b-481c-a127-bf4231bceca3
---

`readability-identifier-naming` in clang-tidy resolves each identifier to
ONE category (Constant, LocalConstant, GlobalConstant,
LocalConstexprVariable, StaticConstant, StaticConstexprVariable, etc.),
and category granularity has EVOLVED across LLVM versions.

**Hit in v8d-3d cluster** (2026-05-17): two function-local identifiers in
`engine/units/src/unit_preferences.cpp` passed local tidy (VS2026 bundled
LLVM 19) but failed CI tidy (`windows-latest` ships VS2022 with LLVM 17):

- `constexpr int kMaxPrec = 17;` — LLVM 19 calls this
  `LocalConstexprVariable` (which our `.clang-tidy` sets to `CamelCase + k`,
  PASS). LLVM 17 calls it generic `Constant` (defaults to `lower_case`,
  FAIL).
- `static constexpr const char* kSuffixes[] = {...};` — same divergence.

**Two-pronged fix**:

1. **Pin LLVM version in CI to LLVM 20.1.8** to match the local dev environment
   (VS2026 ships LLVM 20.1.8 via its bundled clang-tidy). **PIN BOTH the action
   tag AND the full version**:
   ```yaml
   - name: Install LLVM 20.1.8 (pinned)
     uses: KyleMayes/install-llvm-action@v2.0.9   # NOT @v2 — see below
     with:
       version: '20.1.8'                           # NOT '20' — see below
   ```
   The action prepends its bin dir to PATH so `clang-tidy` resolves to the
   pinned version, overriding MSVC's bundled tidy.

   **Two ratcheting lessons (both hit 2026-05-17 across multiple iterations)**:

   - **`@v2` does NOT auto-resolve to the latest v2.0.x.** The action's
     `@v2` tag pointed to an older v2.0.x that pre-dated LLVM 20 support
     (added in v2.0.7, more 20.x versions in v2.0.9). `@v2 + version: '20'`
     silently fell back to LLVM 19.1.7. Confirmed via the `Verify clang-tidy
     version` CI step output: `LLVM version 19.1.7` when we asked for 20.
     **Always pin to a specific minor like `@v2.0.9` for reproducibility.**

   - **`version: '20'` (major-only) is a "minimum version" request** — the
     action picks the highest compatible from its assets.json list. If its
     assets.json doesn't yet have 20.x entries, the highest 19.x is chosen.
     **Always pin to a full semver like `version: '20.1.8'` for determinism.**

   - **First-pass attempt to pin LLVM 19 was wrong** for an unrelated reason:
     LLVM 19 lumps function-local `constexpr T` into the generic
     `LocalConstant` category; LLVM 20 has a separate `LocalConstexprVariable`
     category that the project `.clang-tidy` targets with `CamelCase + k`.
     The project convention `kMaxPrec` requires LLVM 20+ to pass tidy.

   **Verification step**:
   ```yaml
   - name: Verify clang-tidy version
     shell: pwsh
     run: clang-tidy --version
   ```
   Always include this and read its output before debugging clang-tidy
   failures — saves an iteration. The user spotted my LLVM 19 fallback only
   after digging into the CI log; the verify-version step prints it upfront.

2. **Defensive duplicates in `.clang-tidy`** — set BOTH the granular and
   the generic categories to the same rule:
   ```yaml
   - key: readability-identifier-naming.LocalConstexprVariableCase
     value: CamelCase
   - key: readability-identifier-naming.LocalConstexprVariablePrefix
     value: 'k'
   # Defensive: older clang-tidy falls back to generic Constant
   - key: readability-identifier-naming.ConstantCase
     value: CamelCase
   - key: readability-identifier-naming.ConstantPrefix
     value: 'k'
   - key: readability-identifier-naming.StaticConstantCase
     value: CamelCase
   - key: readability-identifier-naming.StaticConstantPrefix
     value: 'k'
   - key: readability-identifier-naming.StaticConstexprVariableCase
     value: CamelCase
   - key: readability-identifier-naming.StaticConstexprVariablePrefix
     value: 'k'
   # Hold function-local non-constexpr `const T n` to lower_case
   - key: readability-identifier-naming.LocalConstantCase
     value: lower_case
   - key: readability-identifier-naming.LocalConstantPrefix
     value: ''
   ```
   Any clang-tidy version's categorisation now resolves to the same rule.

**Belt + suspenders**: do both. The pinned LLVM keeps behavior identical;
the defensive `.clang-tidy` rules make future version drift survivable.

**Also caught in same CI run**: GCC rejects `std::min({a, b, c})` brace-init
in template-deduction contexts even though the standard's
`std::min(initializer_list)` overload exists. Convert to nested binary
form `std::min(std::min(a, b), c)`. The cleanup agent's earlier refactor
used the brace-init form which only worked on MSVC + clang-cl. Helper:
`scripts/.fix-min-max-brace-init.ps1` does the conversion via .NET regex.

**Catch-22 with `modernize-min-max-use-initializer-list`**: that clang-tidy
check STEERS us toward the GCC-rejecting brace-init form. Disabled globally
in `.clang-tidy` with comment. Use nested binary form everywhere for
cross-compiler portability.

Related: [feedback-clang-tidy-warnings-are-errors](build-and-verification.md#memory-feedback_clang_tidy_warnings_are_errors) — the policy that
made these CI failures surface. [feedback-gcc-linux-double-to-float-narrowing](numerics-and-performance.md#memory-feedback_gcc_linux_double_to_float_narrowing)
— same class of MSVC-tolerates / GCC-rejects pattern.


<!-- end-memory:feedback_clang_tidy_ci_local_version_skew -->

<a id="memory-feedback_clang_tidy_drops_slash_flags_and_ooms_under_commit_pressure"></a>
## feedback_clang_tidy_drops_slash_flags_and_ooms_under_commit_pressure

---
name: feedback_clang_tidy_drops_slash_flags_and_ooms_under_commit_pressure
description: "The win-tidy 'AVX-512 clang-tidy crash' is COMMIT EXHAUSTION, not a matcher bug; and clang-tidy silently DROPS every /-spelled MSVC flag, so the gate analysed a config we do not ship"
metadata:
  node_type: memory
  type: feedback
  originSessionId: a3482f73-d858-400b-816d-942216e20052
  modified: 2026-07-25T12:28:35.287Z
---

Two independent win-tidy defects, both diagnosed 2026-07-25. Neither is an upstream clang-tidy matcher
bug, and disabling checks fixes neither.

**1. The "non-deterministic AVX-512 crash" is the HOST RUNNING OUT OF COMMIT.** The sweep log's real
message is `Error running clang-tidy.exe: LLVM ERROR: out of memory` with `Exception Code: 0xC000001D`
(STATUS_ILLEGAL_INSTRUCTION = LLVM's own abort), not 0xC0000005. Measured: a clang-tidy edge peaks at
**~200–300 MB**, and the very files that crashed under the build ran **0/5 crashes standalone** — while
the host sat at **83 GB of a 96 GB commit limit** with Visual Studio (8.7 GB), a DAW and browsers
resident. Whichever process asks for memory when headroom runs out is the one that dies, which is exactly
why it was "a different check on a different file every run". `malloc_allocator.cpp` — a tiny file —
crashing at `<eof>` is the clincher: not TU complexity, ambient pressure.
**It is not only clang-tidy.** `cl.exe` itself took a `0xC0000005` on `/O2` hesap TUs at `-j5`
(`test_validation.cpp`, then `test_feast.cpp` — a different file each time), while the SAME win-shipping
build had completed at `-j10` earlier the same day, before `clangd` started indexing and took 10.5 GB
across three instances. Same mechanism, different victim. So "a random compiler AV on a random file"
during a sweep = **check commit headroom first**, before suspecting an ICE or the code.
*Fix (user chose repo-side clamp only, 2026-07-25 — no page-file change):* `Set-CrdBuildJobs`-style clamp
**per config, re-measured right before each build**, in `scripts/per-slice-check.ps1` and
`scripts/full-sweep.ps1`. Per-edge budgets: tidy 1.5 GB · debug/clang-cl 2.0 · asan 2.5 · relwithdebinfo
3.0 · release/shipping 4.0; `jobs = min(cap, floor((free - 4 GB) / per_edge))`, always printed.
NEVER disable a check for this — two were disabled on the wrong theory and have been reverted.

**2. clang-tidy SILENTLY DROPS every `/`-spelled MSVC flag** that arrives through the compile command
(`cmake -E __run_co_compile --tidy=... -- cl.exe /EHsc /arch:AVX2 /DWIN32 ...`). Verified with an
`#error`-guarded probe through that exact CMake path: `/EHsc`, `/arch:AVX2` and `/D<macro>` never reach
the TU; the same flags passed via `--extra-arg=` do. (CMake spells `-I` and user `-D` with dashes, which
is why most of the build looked fine.) Consequences, both silent:
  * exceptions look DISABLED ⇒ any `try` is a hard `clang-diagnostic-error`
    (`engine/platform/src/filesystem.cpp`), and `bugprone-exception-escape` can never fire;
  * `__AVX2__` is UNDEFINED ⇒ every AVX2-guarded path is preprocessed out and **never analysed** — the
    gate reads green on code it never looked at (the [feedback_tidy_gate_clean_on_unparsed_files](build-and-verification.md#memory-feedback_tidy_gate_clean_on_unparsed_files) disease).
*Fix:* `--extra-arg=/EHsc` + `--extra-arg=${CRD_SIMD_MSVC_ARCH_FLAG}` in the `CRD_ENABLE_CLANG_TIDY` block
(root `CMakeLists.txt`), the ISA flag exported by the same `CrdSimd.cmake` branch that adds the real
compile option so they cannot drift; mirrored in `scripts/tidy-files.ps1`.

**Fallout worth expecting:** correcting the flags unmasked **120 previously invisible findings** (64
identifier-naming, 30 isolate-declaration, 9 bugprone-exception-escape, 7 const-cast, ...) — they had
accumulated because the win-tidy build had been dying at edge ~18/1070 for a long time, so the whole tail
of the tree was UNGATED. `clang-tidy --fix` handles naming + isolate-declaration, but REVIEW THE DIFF: it
renamed a lambda `O`→`o` onto an existing `o` and `ob_`→`ob` onto an existing `ob`, producing compile
errors. Also: an empty `catch (...) {}` trips `bugprone-empty-catch` — give every catch a real statement.

Supersedes the wrong half of [feedback_full_sweep_blockers_emitter_determinism_and_clang_tidy_avx512: original reference absent; current rule](../../BUILDING.md).
Related: [feedback_transient_clang_tidy_crash](build-and-verification.md#memory-feedback_transient_clang_tidy_crash) (also likely this, not an LLVM bug),
[feedback_host_14900k_cap_builds](build-and-verification.md#memory-feedback_host_14900k_cap_builds), [feedback_clang_tidy_must_be_llvm_20_not_22](build-and-verification.md#memory-feedback_clang_tidy_must_be_llvm_20_not_22).


<!-- end-memory:feedback_clang_tidy_drops_slash_flags_and_ooms_under_commit_pressure -->

<a id="memory-feedback_clang_tidy_must_be_llvm_20_not_22"></a>
## feedback_clang_tidy_must_be_llvm_20_not_22

---
name: feedback_clang_tidy_must_be_llvm_20_not_22
description: win-tidy gate is LLVM 20.1.8 (VS2026/CI-pinned) — a stray standalone LLVM 22 on PATH fires spurious failures; never debug tidy with the wrong version
metadata:
  node_type: memory
  type: feedback
  originSessionId: 7b0bb65d-6788-4a80-96e7-82e1072ab242
---

The clang-tidy gate (CI + local) is **LLVM 20.1.8** (VS2026 bundles it: `…\VC\Tools\Llvm\x64\bin\clang-tidy.exe`; CI pins it explicitly). The dev box ALSO has a standalone **LLVM 22.1.1** at `C:\Program Files\LLVM\bin` that is first on PATH, so a naive `find_program(clang-tidy)` (and a bare `clang-tidy` invocation) picks 22 — the WRONG version.

LLVM 22 disagrees with the pinned gate in ways that look like real code bugs but are NOT:
- **`try`/`catch` → "cannot use 'try' with exceptions disabled"** — a clang-cl regression; 22 ignores `/EHsc` for its parse. 20.1.8 accepts try/catch fine. (Engine builds with `/EHsc` — exceptions are ENABLED; "exceptions-free" is a coding *convention*, not a compiler setting.)
- **`bugprone-throwing-static-initialization`** fires under 22, not under 20 (e.g. the hesap CLI `CRD_HESAP_CLI_REGISTER_MODULE` registrars).
- **`readability-identifier-naming` on function-local `constexpr`** (e.g. `kMaxPrec`): 20 categorises it `LocalConstexprVariable` (→ kCamelCase, correct); 22 lumps it into `LocalConstant` (→ lower_case) → false "invalid case style". NO single name satisfies both — do NOT rename; use 20.1.8.

**Why:** debugging win-tidy with LLVM 22 = chasing ghosts (cost ~a full session, 2026-05-28).

**How to apply — fix is LOCAL-ENV, NOT committed CMake:**
1. The gate is 20.1.8 — `clang-tidy --version` must say 20.1.8 before trusting ANY win-tidy failure.
2. **Do NOT add a VS-preferring `find_program` to `CMakeLists.txt`** (I tried; reverted). It would `NO_DEFAULT_PATH`-search the VS-bundled clang-tidy FIRST, and on **CI (windows-2022 / VS2022)** the bundled Clang tools are an OLDER LLVM (17/18) — that would override CI's explicitly-installed-and-pinned 20.1.8 (`KyleMayes/install-llvm-action@v2.0.9 version 20.1.8`, put first on PATH) and REINTRODUCE the very skew the pin kills. CI is already correct via plain `find_program(CLANG_TIDY_EXE NAMES clang-tidy)` + PATH order. Leave it.
3. The broken thing is ONLY the dev box's PATH (stray `C:\Program Files\LLVM` 22 shadows VS2026's 20.1.8). Cleanest fix: **remove LLVM 22 from PATH** (or local-only `-DCLANG_TIDY_EXE=…\VC\Tools\Llvm\x64\bin\clang-tidy.exe`, uncommitted). In VS after changing PATH: Project → Delete Cache and Reconfigure (stale `CLANG_TIDY_EXE` cache won't re-resolve).
4. Standalone clang-tidy scans for verification MUST invoke the 20.1.8 binary explicitly.

See [reference_build_test_workflow](build-and-verification.md#memory-reference_build_test_workflow), [feedback_clang_tidy_ci_local_version_skew](build-and-verification.md#memory-feedback_clang_tidy_ci_local_version_skew).


<!-- end-memory:feedback_clang_tidy_must_be_llvm_20_not_22 -->

<a id="memory-feedback_clang_tidy_warnings_are_errors"></a>
## feedback_clang_tidy_warnings_are_errors

---
name: feedback-clang-tidy-warnings-are-errors
description: "clang-tidy warnings are project-policy build failures. .clang-tidy has WarningsAsErrors '*' set 2026-05-17. Surfaced by user during v8d-2d slice close."
metadata:
  node_type: memory
  type: feedback
  originSessionId: b24674c3-970b-481c-a127-bf4231bceca3
---

clang-tidy warnings under the `win-tidy` preset are BUILD FAILURES, not
advisories. The project's stated policy in CLAUDE.md is "Compile clean —
zero warnings"; flipping `.clang-tidy`'s `WarningsAsErrors` from `''` to
`'*'` 2026-05-17 makes the config match the doc.

**Why:** previously the empty `WarningsAsErrors` let tidy warnings show in
the win-tidy build log but not fail the build. Combined with clang-tidy
only re-running on CHANGED files in incremental builds, this meant new
slices could ship with style warnings (uppercase literal suffix,
GlobalConstant kCamelCase, LocalConstexpr k-prefix, etc.) that the per-
slice gate silently passed. The user explicitly called this out during
v8d-2d close: "we should have no warnings... this should be treated as
errors."

**How to apply:**
- Before declaring a slice closed: per-slice gate must show ZERO clang-tidy
  warnings in `scripts\.per-slice-logs\win-tidy.log` (or wherever the gate
  writes it). `Select-String -Pattern ': warning:' | Measure-Object` must
  return 0.
- A `WarningsAsErrors: '*'` config now ENFORCES this — if tidy fires
  anywhere on changed code, the win-tidy build fails and the per-slice
  gate fails. No more silent slips.
- To temporarily defer a specific finding during a refactor (rare):
  silence at the call site with `// NOLINT(check-name)` and a comment
  explaining the deferral + a follow-up issue. Don't disable globally in
  `.clang-tidy` without a deliberate decision.
- Common style violations in NEW code (always check before commit):
  - integer literal suffixes: `0xABCDu` → `0xABCDU` (`readability-uppercase-literal-suffix`)
  - file/anon-namespace `constexpr` constants: `k_null_foo` →
    `kNullFoo` (`GlobalConstantCase = CamelCase + k` prefix)
  - function-local `const` variables: `const u32 N` → `n` (`Constant` rule
    = `lower_case`)
  - function-local `constexpr` variables: `constexpr f32 S = 1e5F` →
    `constexpr f32 kScale = 1e5F` (`LocalConstexprVariableCase` = CamelCase
    + `k` prefix)
- Pre-existing debt: when this flip ran, full clean rebuild surfaced any
  warnings that incremental builds had silently shipped. Those get fixed
  in the same close cycle.

Related: [feedback-clang-tidy-after-every-slice](build-and-verification.md#memory-feedback_clang_tidy_after_every_slice) — the per-slice mandate.
This one ENFORCES it via config.


<!-- end-memory:feedback_clang_tidy_warnings_are_errors -->

<a id="memory-feedback_commit_verify_is_op_local_module_wide_rules_ride_a_consumer_sweep"></a>
## feedback_commit_verify_is_op_local_module_wide_rules_ride_a_consumer_sweep

---
name: feedback_commit_verify_is_op_local_module_wide_rules_ride_a_consumer_sweep
description: "The 8i transaction's commit-verify is OP-LOCAL: it runs the registered verifier only on the ops the transaction TOUCHED, so a rule that spans ops (a whole-document DRC, a cross-op structural invariant) fires iff its CARRIER op is in the touched-set — editing an upstream op will NOT trip a rule that lives on a downstream one. Whole-MODULE rules ride a find_*_violation-style SWEEP the consumer runs; the correct authoring pattern is sweep-mid-transaction (8i applies edits eagerly, so the authored state is visible before commit) → commit-or-rollback, with commit-verify as the BACKSTOP behind the sweep, never a substitute for it"
metadata:
  node_type: memory
  type: feedback
  originSessionId: fade8ea4-87ca-470f-83e0-cdfe82a44e7f
  modified: 2026-08-09T21:01:03.258Z
---

Recurred across THREE CEIR-9 universality proofs (named at 9e, reinforced by 9f's stale-comment episode, demonstrated
at 9g), so it earns a memory. The 8i `Transaction` ([project_ceir_master_spine_locked](project-history.md#memory-project_ceir_master_spine_locked)) enforces a rule at commit by
running each TOUCHED op's registered verifier (`Context::verify`). That is **op-LOCAL** by construction:

- A rule about op X only fires when X (its carrier) is in the transaction's touched-set. So a CAD design rule must live
  on the board op's verifier to catch a board edit (CEIR-9d); a rule on a downstream `drc`/consumer op would NOT fire
  when the board is edited. An EDA whole-board DRC, an ECS archetype invariant, a notebook cross-cell rule — none of
  these are op-local, so **commit-verify cannot see them**.
- Whole-MODULE rules ride a `find_*_violation`-style **SWEEP** (`find_structure_error`, `find_domain_violation`, the
  token/borrow walks) that the CONSUMER runs — NOT a foundation gap in 8i, a different verification GRANULARITY that
  COMPOSES on top. Op-local is the correct commit-time granularity; module-wide is a separate pass.

**The authoring pattern (the CEIR-9g agent loop, the reusable shape for every editor/agent/CLI consumer):** open a
transaction → author/modify → run the module-wide sweep **MID-TRANSACTION, PRE-COMMIT** (8i applies edits EAGERLY, so
the sweep over the module sees the authored state before you commit) → sweep clean ⇒ commit; sweep defect ⇒ emit a
diagnostic (an [project_ceir_master_spine_locked](project-history.md#memory-project_ceir_master_spine_locked) 8g `DiagnosticEngine` code, read by CODE not string) and roll back
(byte-identical). The op-local commit-verify is the **BACKSTOP** behind the sweep (belt-and-suspenders — it catches
what a lazy consumer that skips the sweep would miss), never a substitute for it.

⛔ Consequence for a proof/slice: a cross-op rule you expect to fire "when the program is edited" will silently NOT fire
if you attach it to the wrong op and rely on commit-verify. If you need module-wide enforcement, run the sweep
yourself; if you attach the rule op-locally, make sure the edit touches that op.

**How to apply:** (a) attaching a validity rule → decide op-local (rides commit-verify, fires on that op's edit) vs
module-wide (a consumer sweep, fires whenever the consumer chooses to run it); never expect a cross-op rule to fire
from an unrelated op's edit. (b) building an editor/agent/CLI over transactions → sweep-mid-tx-then-commit-or-rollback,
diagnostics by code. Sibling of the honesty scars [feedback_source_must_match_honest_scoreboard](workflow-and-correctness.md#memory-feedback_source_must_match_honest_scoreboard) and
[feedback_gate_reverifies_status_matrix_rows_never_inherits_stale_or_unverified_claims](workflow-and-correctness.md#memory-feedback_gate_reverifies_status_matrix_rows_never_inherits_stale_or_unverified_claims) (the 9f stale-comment
correction that reinforced this one).


<!-- end-memory:feedback_commit_verify_is_op_local_module_wide_rules_ride_a_consumer_sweep -->

<a id="memory-feedback_concurrent_tests_use_crd_jobs"></a>
## feedback_concurrent_tests_use_crd_jobs

---
name: feedback_concurrent_tests_use_crd_jobs
description: "Concurrency tests must drive threads through crd-jobs (the fiber job system), never raw std::thread; and Catch2 REQUIRE is not thread-safe"
metadata:
  node_type: memory
  type: feedback
  originSessionId: 7b0bb65d-6788-4a80-96e7-82e1072ab242
---

When a test needs real multi-threaded concurrency (proving a lock-free / thread-safe
structure), drive the producers/consumers through **`crd-jobs`** (`crd::jobs::init()`
→ `parallel_for(count, num_workers(), fn)` → `wait(c)` → `shutdown()`), NOT raw
`std::thread`. Cerid has its own fiber job system; it IS the engine's multi-platform
concurrency substrate, so tests must exercise the same path real code uses.

**Why:** strong user directive (2026-05-27, S4 RingAllocator): "why std::thread? why
not our job system!" Using std::thread bypasses the substrate under test and the
project's portability model (the fiber asm context-switch is the WASM hazard we track
in [project_browser_wasm_deployment_goal](project-history.md#memory-project_browser_wasm_deployment_goal)). Generalizes [feedback_spatial_substrate_thread_safety](workflow-and-correctness.md#memory-feedback_spatial_substrate_thread_safety)
("fiber-jobified concurrent test mandatory") to ALL concurrent tests.

**How to apply:**
- Link the test binary against `crd-jobs` (the production lib under test stays
  jobs-free — only the TEST depends on jobs, preserving module decoupling).
- `parallel_for`'s job lambda must fit the 41-byte SBO + be trivially copyable:
  capture ONE pointer to a small context struct, not many captures.
- **Catch2 `REQUIRE`/`CHECK` are NOT thread-safe** — calling them from a worker
  fiber/thread trips `"redirect is already active"` (catch_output_redirect). Workers
  only RECORD into atomics / pre-sized arrays; the MAIN thread asserts after `wait()`.
- `jobs::init()`/`shutdown()` once per binary — if another test/listener already
  inits jobs for the whole binary (e.g. crd-resources-tests' ResourcesJobsListener),
  do NOT double-init; scope init/shutdown inside the single test only when it's the
  sole jobs user.


<!-- end-memory:feedback_concurrent_tests_use_crd_jobs -->

<a id="memory-feedback_ctest_not_built_sentinel_axis_and_cuda_linux_asan_leak"></a>
## feedback_ctest_not_built_sentinel_axis_and_cuda_linux_asan_leak

---
name: feedback_ctest_not_built_sentinel_axis_and_cuda_linux_asan_leak
description: catch_discover_tests _NOT_BUILT fires in BOTH discovery modes (built-vs-configured is the axis); CUDA under Linux-ASan needs detect_leaks=0 for the WSL libcuda cuInit leak
metadata:
  node_type: memory
  type: feedback
  originSessionId: fade8ea4-87ca-470f-83e0-cdfe82a44e7f
  modified: 2026-08-10T17:57:11.618Z
---

⛔ **`catch_discover_tests` registers a failing `<target>_NOT_BUILT` sentinel test whenever the target is CONFIGURED but its
executable is NOT built — in BOTH `DISCOVERY_MODE PRE_TEST` and the default POST_BUILD.** The axis is **built-vs-configured,
NOT PRE_TEST-vs-POST_BUILD** (I wasted a plan on "switch to POST_BUILD to dodge the sentinel" — the advisor caught it; verify
in seconds: grep `_NOT_BUILT` in `<build>/_deps/catch2-src/extras/Catch.cmake`, it's in both mode branches). A sibling target
(vulkan) doesn't sentinel because the gate BUILDS it; another (dx12) doesn't because it's never CONFIGURED on that platform.

**Why it bit (CEIR-13z CUDA):** WSL2 here has the full CUDA toolkit (`/usr/bin/nvcc`) AND a real GPU-passthrough device
(RTX 4070 Ti SUPER). So `find_package(CUDAToolkit)` succeeds on Linux → `crd-gpu-context-cuda` + the CUDA *test* target
CONFIGURE → their PRE_TEST sentinel fires under `ctest -R ceir` because gate6b wasn't building them. **The honest fix is to
build + run them, not to mask the sentinel** — the CUDA CEIR tests (add/reduce/scan/6-dispatch FFT) BUILD + PASS on the real
Linux device (linux-gcc-debug 5/5). The sentinel was correctly flagging missing CUDA coverage on a CUDA-capable box — that's
how the 2nd-platform CUDA proof was found. **Rule:** when a device-test target is guarded on a toolkit WSL also has, the Linux
gate must BUILD it (add it to the gate's hardcoded target list) or the sentinel reds the gate.

⛔ **CUDA under a Linux ASan build (WSL passthrough):** `cuInit` in the WSL `libcuda.so.1` shim FAILS for ASan-instrumented
processes → the tests correctly SKIP ("no CUDA device available"), but the driver's own cuInit allocations (~50 KB in ~5 objs)
are never freed and trip **LeakSanitizer at exit**, turning a correct skip into exit != 0. A narrow `leak:libcuda` LSan
suppression is **INSUFFICIENT** — only ~half the frames name a module; the rest are `<unknown module>` (JIT/anon regions LSan
can't name). Fix = `PROPERTIES ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0"` on the CUDA test binary, **`if(UNIX)`-guarded** —
ASan's use-after-free/overflow checks stay ON, the tests never reach our code under ASan (they skip), and Windows debug+asan
cover CUDA with the native driver (no suppression, real leak coverage intact). ⛔ Do NOT broaden to skipping/removing the
target — the sentinel must stay so a CUDA-capable box still builds it. Real Linux CUDA coverage is the non-ASan (debug) run on
the physical device. See [feedback_cuda_multipass_fft_broken_single_workgroup_ok](device-programs.md#memory-feedback_cuda_multipass_fft_broken_single_workgroup_ok).


<!-- end-memory:feedback_ctest_not_built_sentinel_axis_and_cuda_linux_asan_leak -->

<a id="memory-feedback_cuda_test_target_missing_crd_repo_dir_is_cwd_luck"></a>
## feedback_cuda_test_target_missing_crd_repo_dir_is_cwd_luck

---
name: feedback_cuda_test_target_missing_crd_repo_dir_is_cwd_luck
description: "A test target that never loaded an asset may lack CRD_REPO_DIR; its #ifndef \".\" fallback works only when CWD==repo root (Windows luck), fails under WSL/CI ctest."
metadata:
  node_type: memory
  type: feedback
  originSessionId: cb9df3b8-2389-479b-9d99-d3d6ce3ba327
  modified: 2026-09-05T20:27:08.840Z
---

A test target that had never loaded an authored asset can be MISSING
`target_compile_definitions(<tgt> PRIVATE CRD_REPO_DIR="${CMAKE_SOURCE_DIR}")`. Asset-path code
falls back to `#ifndef CRD_REPO_DIR ... "." #endif`, which resolves the asset ONLY when the process
CWD is the repo root.

**Why:** running the exe DIRECTLY on Windows (CWD often the repo root) resolves `./assets/...` by
LUCK and passes; WSL/CI `ctest` runs with CWD == the BUILD dir, so `./assets/...` does not exist and
the load fails. The symptom is a Windows-green / Linux-red split on the FIRST asset-loading test in
that target — easy to misdiagnose as a device or driver problem. Hit at CEIR-29b-1: the new
`crd-ceir-gpu-cuda-tests` target loaded `relu.ckir` for the first time; `run_mlp_module` returned
false only under WSL ctest until `CRD_REPO_DIR` was added.

**⛔ The `#ifndef CRD_REPO_DIR / #define "." / #endif` fallback in a TU IS this scar PRE-ARMED (CEIR-31a-1a-ii, 2026-09-05).**
When the CMakeLists already defines `CRD_REPO_DIR` (the house pattern), the fallback is dead on the real build — its
ONLY effect is to convert a MISSING define (a loud, correct compile error) into a silent `./assets/...` cwd-relative read
that Win-greens and WSL-reds. So: **DELETE the fallback, do NOT add it** — a missing define must fail LOUD at compile.
(The audio + dist committed-asset reading gates on `crd-ceir-tests` had inherited the fallback from the 30a-1 mold; both
struck.) The reflex is inverted from the "add CRD_REPO_DIR" fix below: add it to the CMakeLists (once, per target), never
to the TU as a `"."` fallback.

**How to apply:** when a device/asset test is Win-green but WSL/CI-red on an asset path, check the
target's CMakeLists for `CRD_REPO_DIR` BEFORE blaming the device or the driver. Add
`target_compile_definitions(<tgt> PRIVATE CRD_REPO_DIR="${CMAKE_SOURCE_DIR}")` (the house pattern
every other test target already has) — and strike any `#ifndef CRD_REPO_DIR "."` fallback in the TU. Related:
[feedback_cuda_emitter_signature_is_a_contract_shared_by_every_launch_site](device-programs.md#memory-feedback_cuda_emitter_signature_is_a_contract_shared_by_every_launch_site),
[reference_windows_test_run_env_assets_shaderc_asan_dll](build-and-verification.md#memory-reference_windows_test_run_env_assets_shaderc_asan_dll).


<!-- end-memory:feedback_cuda_test_target_missing_crd_repo_dir_is_cwd_luck -->

<a id="memory-feedback_decompose_before_deferring_analytic_core_is_buildable"></a>
## feedback_decompose_before_deferring_analytic_core_is_buildable

---
name: feedback_decompose_before_deferring_analytic_core_is_buildable
description: NEVER defer a feature because ONE leaf needs later infra — decompose; the analytic core is buildable NOW
metadata:
  node_type: memory
  type: feedback
  originSessionId: 40e3ad67-a505-447d-89df-272b48c237f6
---

⛔⛔ User reprimand (2026-07-12, D-007 B8-b): I deferred thin-film iridescence AND transmission/refraction
from the OpenPBR lobe stack, calling them "spectral/renderer-coupled." User: **"NO DEFERRALS!!!! I want
explanation on that!"** Both were implemented in full the same session — bit-exact CPU-oracle + both-backends
observable — with ZERO new infra.

**Why the deferral was wrong:** thin-film is PURE analytic shader math (Belcour-Barla Fresnel: sqrt/cos/exp/
smoothstep/mix + XYZ→sRGB) — there was never a reason to defer it. Transmission LOOKED renderer-coupled
because its final step samples the scene-colour framebuffer, but that is ONE LEAF: the BTDF `(1−F)·base·D·Vis`
on the mirrored light, Beer-Lambert `volume_attenuation`, and the Snell `refraction_ray` are ALL analytic and
buildable today. Only the texture SAMPLE the refraction ray reads is renderer-side (a later bind).

**How to apply:** before deferring ANY feature, DECOMPOSE it. If 90% is analytic/buildable and only one leaf
needs infra that lands later, BUILD the 90% now and leave a named seam for the leaf — do NOT defer the whole
feature. "Rides the renderer / needs system X" is almost always about a single leaf, not the core. This is the
[feedback_never_defer_solve](workflow-and-correctness.md#memory-feedback_never_defer_solve) / [feedback_solve_losses_never_document_and_accept](workflow-and-correctness.md#memory-feedback_solve_losses_never_document_and_accept) principle applied to
mis-classification: the trap is calling analytic work "coupled" and shelving it. Reference for these two lobes:
glTF Sample Renderer `KHR_materials_iridescence` / `_transmission` / `_volume` (transcribed in ckir_lighting.hpp).
Related: [feedback_close_the_slice_never_claim_done_when_partial](workflow-and-correctness.md#memory-feedback_close_the_slice_never_claim_done_when_partial), [feedback_never_defer_fix_dod_failures](workflow-and-correctness.md#memory-feedback_never_defer_fix_dod_failures).


<!-- end-memory:feedback_decompose_before_deferring_analytic_core_is_buildable -->

<a id="memory-feedback_deleting_reference_in_ab_parity_test_degrades_to_can_t_fail"></a>
## feedback_deleting_reference_in_ab_parity_test_degrades_to_can_t_fail

---
name: feedback_deleting_reference_in_ab_parity_test_degrades_to_can_t_fail
description: "When you delete the reference impl an A/B parity test compares against, the test silently degrades to A==A (can't-fail) — convert to ABSOLUTE asserts in the SAME change"
metadata:
  node_type: memory
  type: feedback
  originSessionId: 192f4d9e-b3f7-485b-a775-5391bcd4183c
  modified: 2026-09-05T18:09:08.829Z
---

An A/B parity test proves `new == old` by running BOTH recorders and comparing their
captured output. When you DELETE `old` (e.g. `record_scene_raster` at CEIR-16d-live-4c),
any arm that resolved `old` through the shared registration (`register_builtin_records`)
now resolves `new` too — so `CHECK(a == b)` becomes `CHECK(a == a)`, a vacuous gate that
can never fail. The device-free tests looked green; the teeth were gone. Worse: the very
test that had CAUGHT a real bug (run_mrt_blend_gpu caught the color_slot MRT bug — its teeth
were the legacy arm) is exactly the one that goes toothless.

Two failure sub-modes at a deletion:
1. The "legacy" arm binds NO plan → post-flip it hits the generic replay with a null plan →
   hard `ctx.fail()` (a RED, at least loud). Fixable but noisy.
2. The "legacy" arm binds a plan via `replace_record` → A==A, silently GREEN. The dangerous one.

⭐ **Third sub-mode — the reference is KEPT but the parity assert doesn't ABORT (CEIR-30c-2b, advisor-caught):**
you load a value (e.g. an authored-asset placement vector) AND drive the rest of the test with that same
loaded value, keeping the hand-written reference and asserting `scv_asset == scv`. Correct so far — but if
the parity is a NON-aborting `CHECK`, a DRIFTED asset fires that ONE `CHECK` and then every downstream
falsifiability check (plan_transfers, n_xfer, the oracle compare) SELF-AGREES with whatever the asset
resolved to — so the drift is a single failed assertion buried among ~15 green passes, and the headline
"bit-exact vs oracle" still reads green. **Rule:** when the compared value ALSO drives execution, the parity
must `REQUIRE` (abort the arm) at the ONE place it meets the independent reference — a `CHECK` there decorates
a drifted run instead of failing it. (Catch2 `CHECK` continues; `REQUIRE` aborts.)

**Why:** parity tests are SCAFFOLDING that proves the migration before deletion; after the
reference is gone, "at parity with what?" has no answer. Keeping them as CEIR-vs-CEIR ships a
disguised gate (violates GOLD / never-disguise-failure).

**How to apply:** in the SAME atomic change as the deletion, convert every A/B parity test to
SINGLE-PATH with ABSOLUTE asserts — transcribe the expected values from the surviving impl's
contract (for command streams: read the ported emitter's verb ladder and assert exact command
kind / geometry kind / coalesce factor / DrawIndex row / binding count / slot; for pixels:
assert absolute channel values, e.g. "center of triangle in color1 ≈ 51 (additive), not the
clear"). In-scope test handles (prog_a, buf0, fake_raster()) are absolute, NOT fragile — assert
`==` them exactly. Delete the reference arm; keep the new arm; drop `replace_record` where the
unconditional registration now covers it. Advisor flagged this at 16d-live-4c; the 3 on-device
tests would have gone vacuous had only the 4 device-free ones been converted. Related:
[feedback_gate_reverifies_status_matrix_rows_never_inherits_stale_or_unverified_claims](workflow-and-correctness.md#memory-feedback_gate_reverifies_status_matrix_rows_never_inherits_stale_or_unverified_claims),
[feedback_always_pick_gold_standard_never_disguise_failure](workflow-and-correctness.md#memory-feedback_always_pick_gold_standard_never_disguise_failure),
[feedback_authored_asset_slice_done_only_when_cpp_deleted_and_renders](rendering.md#memory-feedback_authored_asset_slice_done_only_when_cpp_deleted_and_renders).


<!-- end-memory:feedback_deleting_reference_in_ab_parity_test_degrades_to_can_t_fail -->

<a id="memory-feedback_dx12_pso_format_must_match_rt"></a>
## feedback_dx12_pso_format_must_match_rt

---
name: dx12-pso-format-must-match-rt
description: DX12 PSO must carry the actual render target format — defaulting to RGBA8 when the target is R11G11B10F/RGBA16F produces a completely black screen (silent no-op draws)
metadata:
  node_type: memory
  type: feedback
  originSessionId: dd844f83-0821-41ab-8607-fa93ccf37a76
  modified: 2026-08-02T02:12:16.282Z
---

DX12 PSO render-target format MUST match the actual RT format. The `pass_pso()` wrapper defaulted `rt_fmt = kColorFormat` (RGBA8 Unorm) for every draw verb, but frame-graph colour targets can be R11G11B10F or RGBA16F. The mismatch made EVERY draw a silent no-op — a completely black screen with no validation error (debug layer off).

**Why:** DX12 bakes the RT format into the PSO at creation time. A PSO built for RGBA8 cannot draw to an R11G11B10F target — the driver silently discards the draw. Vulkan doesn't have this issue because its dynamic rendering doesn't bake formats.

**How to apply:** `Dx12RasterTarget::color_format()` derives the format from `m_tex->GetDesc().Format`. Every `pass_pso()` call that renders to a colour target must pass `t.color_format()` (or `t0.color_format()` for MRT). The PSO cache entry compares `rt_fmt` explicitly — the old single-bit key flag (`kColorFormat ? 0 : 0x40000`) was insufficient for multiple non-RGBA8 formats.

Related: [dx12-hlsl-svposition-last-register-packing](device-programs.md#memory-feedback_dx12_hlsl_svposition_last_register_packing), [feedback_hlsl_masks_type_bugs_run_vulkan](device-programs.md#memory-feedback_dx12_hlsl_masks_type_bugs_run_vulkan)


<!-- end-memory:feedback_dx12_pso_format_must_match_rt -->

<a id="memory-feedback_full_sweep_after_uncommitted_work_peels_tidy_onion"></a>
## feedback_full_sweep_after_uncommitted_work_peels_tidy_onion

---
name: feedback_full_sweep_after_uncommitted_work_peels_tidy_onion
description: A full per-slice sweep after a long uncommitted stretch + a toolchain bump surfaces a TAIL of pre-existing failures; use ninja -k 0 to see it all at once
metadata:
  node_type: memory
  type: feedback
  originSessionId: 9b330af5-88bb-499e-a92a-1752e160e0ba
---

**Scar (D-008 close, 2026-07-11).** After a long run of uncommitted slices (C0…C2-f) plus a clang-tidy 20.1.8 bump
and an MSVC toolset update (14.50→14.51), the first FULL `scripts/per-slice-check.ps1` sweep surfaced a stack of
failures that targeted builds never caught:
1. **`runtime/examples/smoke_*`** — 8 demo programs are built by the full sweep but NOT by the targets you build
   per-slice. They accumulated every API change (deleted `compile.hpp`, retired `ShaderModule`/`create_device`,
   `create_runtime(compiler)`). Lesson: when you change a widely-used API, grep **`runtime/examples/`** too.
2. **`readability-redundant-casting`** firing inside **MSVC 14.51's `<xutility>`** (STL header, not our code) — a
   clang-tidy-vs-STL false positive. Fix = add `-readability-redundant-casting` to `.clang-tidy`'s disabled list
   (same pattern as the ~17 already-disabled readability sub-checks; `Checks:` is a folded YAML scalar — NO inline
   `#` comments inside it).
3. Then pre-existing **`bugprone-*`** (dx12: branch-clone, casting-through-void) and **`readability-identifier-naming`**
   (hesap-tensor + jobs tests: global consts need `kFoo`, LOCAL constexpr need `lower_case` — see
   [project_local_constexpr_naming_cleanup](project-history.md#memory-project_local_constexpr_naming_cleanup)) — all masked earlier because the build stopped at the first failure.

**How to apply:**
- Don't peel one error per rebuild. Run `cmake --build build/win-tidy -- -k 0` (ninja keep-going) to collect the
  WHOLE tail at once, then fix the batch.
- Most of the tail is PRE-EXISTING toolchain-surfaced debt in files you didn't touch (real, but not your slice).
  Fix it to green (the gate demands it) but report it as collateral, not as the slice's own work.
- Related: [feedback_stale_toolset_path_in_build_dir_wipe_dont_sed](build-and-verification.md#memory-feedback_stale_toolset_path_in_build_dir_wipe_dont_sed), [feedback_clang_tidy_ci_local_version_skew](build-and-verification.md#memory-feedback_clang_tidy_ci_local_version_skew),
  [feedback_msvc_c4127_ci_local_version_skew](build-and-verification.md#memory-feedback_msvc_c4127_ci_local_version_skew), [feedback_transient_clang_tidy_crash](build-and-verification.md#memory-feedback_transient_clang_tidy_crash).


<!-- end-memory:feedback_full_sweep_after_uncommitted_work_peels_tidy_onion -->

<a id="memory-feedback_full_sweep_catches_cross_config_simd"></a>
## feedback_full_sweep_catches_cross_config_simd

---
name: feedback-full-sweep-catches-cross-config-simd
description: "The 5-config per-slice DoD only builds MSVC+AVX2, so SIMD/intrinsic code that breaks on gcc/clang/scalar/SSE2 stays latent until the 18-config full sweep (or CI). Run the full sweep / CI before declaring a SIMD-touching cluster closed."
metadata:
  node_type: memory
  type: feedback
  originSessionId: 8232c613-08b5-412f-9d51-73f8c05a65d9
---

`scripts/per-slice-check.ps1` (the 5-config gate: debug/asan/shipping/release/tidy)
builds **only MSVC with /arch:AVX2**. Any code that compiles there but breaks on
gcc, clang-cl, the scalar backend, or the SSE2 backend is **invisible** to it.

**Why this bites SIMD/intrinsic code specifically:** MSVC `/arch:AVX2` bundles
FMA; gcc/clang gate `_mm256_fmadd_*` behind a separate `-mfma`. MSVC is lax about
unused-lambda-captures / unused-functions; clang `-Werror` is strict. The
scalar/SSE2 backends compile *different* code paths (the `#else` of every
`CRD_SIMD_HAS_AVX2` block + the half-decomposition Vec4f/Vec4d fallbacks) that the
AVX2 build never touches. And the determinism guards differ by platform: Windows
runs `check_no_std_math.ps1`, Linux runs `check_no_std_math.sh` (ctest only reaches
them if the build succeeds first).

**Case study (v0-close, 2026-05-20):** `simd::fma()` shipped in v0d and passed
every per-slice DoD, but the v0-close 18-config full sweep failed 11/18 on SIX
latent bugs introduced back in v0d/v0e — gcc `-mfma`, missing `Vec4f::fma`, two
clang unused-warnings, scalar/SSE2 unused prefetch locals, and a pre-existing
`std::atan2` in `complex.hpp` the Linux `.sh` guard caught (the build had to
succeed before ctest could even run the guard). None were reachable from the
MSVC-AVX2-only per-slice gate.

**How to apply:** when a slice touches SIMD, intrinsics, compile flags, or
`crd-math`/`crd-hesap` kernels, do NOT trust a green 5-config per-slice DoD as
closure evidence. Run `scripts/full-sweep.ps1` (or push to CI) before declaring
the cluster closed. Cheap pre-check before the full sweep: build the four
non-MSVC-AVX2 Windows configs locally (`win-clang-cl`, `win-debug-scalar`,
`win-debug-sse2`) + one `wsl-build.ps1 -Preset linux-gcc-*`. Relates to
[feedback_full_sweep_required](build-and-verification.md#memory-feedback_full_sweep_required) and refines [feedback_per_slice_run_ctest](build-and-verification.md#memory-feedback_per_slice_run_ctest).

**`-mfma` is determinism-safe** (don't re-debate this): `mul_add` stays
two-rounded because it is separate `_mm256_mul_pd`+`_mm256_add_pd` intrinsics and
`-ffp-contract=off` (on `crd-simd-flags`, PUBLIC through `crd-math`) blocks
contraction; only the explicit `simd::fma()` emits hardware FMA. The
SIMD-vs-scalar bit-exact parity tests in every config's ctest are the standing
guard.


<!-- end-memory:feedback_full_sweep_catches_cross_config_simd -->

<a id="memory-feedback_full_sweep_required"></a>
## feedback_full_sweep_required

---
name: A slice is closed only when scripts/full-sweep.ps1 returns PASS
description: Bench-target sweeps and incremental rebuilds are not slice closure; only full-sweep.ps1 across all 18 configs counts as Definition-of-Done verification (count refreshed 2026-05-15 — Win x 11 + Linux x 7)
type: feedback
originSessionId: 44374b19-0728-4302-88da-52db2ebbc4c4
---
A Cerid slice is closed only when `scripts/full-sweep.ps1` returns
`PASS` for every one of the 18 build steps (**Win × 11 + Linux × 7**
as of 2026-05-15; was 14 = 8+6 at memory creation). Bench-target
builds (`cmake --build --preset X --target crd-bench`), incremental
rebuilds, and target-subset sweeps DO NOT substitute.

**Current sweep coverage (2026-05-15, after the v0c-close +
`win-shipping-profile` addition):**
- Win (11): `debug`, `relwithdebinfo`, `release`, `asan`, `clang-cl`,
  `debug-scalar`, `debug-sse2`, `shipping`, `shipping-profile`,
  `clang-cl-shipping` (build + ctest + sandbox-smoke) + `tidy`
  (build-only).
- Linux (7, via WSL `scripts\wsl-build.ps1`): `linux-gcc-debug`,
  `linux-gcc-relwithdebinfo`, `linux-gcc-release`, `linux-gcc-asan`,
  `linux-gcc-debug-scalar`, `linux-gcc-debug-sse2`,
  `linux-gcc-shipping` (build + ctest).

**Phase-close vs sub-slice-close:** prior v0b-close and v0c-close
sweeps ran with `-SkipLinux` (Win-only, 11 configs). That's acceptable
for sub-slice closes per the cadence table below. For **Phase closes**
(the .x → .y close), the full 18-config sweep including Linux is
required — caught at v0d-close 2026-05-15 (Phase 3.1.7.5 close) when
the user flagged "why 11 and not 14".

**Sub-slice verification cadence (refined 2026-05-10 mid-d0d):**

| Change shape | Win sweep | Linux sweep |
|---|---|---|
| New files within one module; no shared API touched | Every sub-slice iteration | At PARENT slice close |
| Touches a shared header (RHI interface, math types, scene component, base class virtual, enum) | Every iteration | **Every iteration** |
| Slice close (full DoD verification) | Always | Always |

Heuristic: ask "did I add or change something that exists in a header
included by N>1 module?" If yes -> Linux too. If no -> defer.

Reasoning: Win sweeps are 3-5 min vs Linux 15-30 min. Always-both at
sub-slice level wastes 25 min of waiting per iteration on changes that
have low cross-platform risk (pure additions to a module). But changes
that hit a shared interface (e.g. v1a-draw-d0c added a pure-virtual to
`crd::rhi::CommandBuffer` -> broke 4 mocked fakes, exactly the kind
of thing GCC catches that MSVC sometimes lets slide). Catching those
at sub-slice level instead of slice close keeps the debugging context
fresh and avoids "Linux debt" carrying across multiple sub-slices.

**Why:** Phase 3.1 v0e was first declared closed based on a Win × 6
bench-target build sweep that completed clean. Hours later a real
Definition-of-Done sweep surfaced LNK1257 in 3 release-class configs
(C4714 LTCG drift the bench-fix had introduced) + a Quatf #151 ctest
mojibake in 4 configs (`°` UTF-8 vs Windows ACP argv). Both real
regressions; both would have shipped if the user hadn't asked "are
all configs OK right now?" Full story:
`docs/sessions/2026-05-10-v0-postmortem-c4714-and-utf8-argv.md`.

**How to apply:** When proposing a slice as closed (or claiming "12-config
sweep clean"), the only acceptable evidence is the SUMMARY block from
`scripts/full-sweep.ps1` showing PASS for every config. Bench builds,
incremental builds, single-target verifications, or recollected prior
sweep results are NOT slice-closure evidence — they're partial signals.
If the full-sweep wasn't run as part of the slice, the slice is not
closed yet. Run it before declaring done.

This rule is permanent and applies to every Phase 3.1+ slice.


<!-- end-memory:feedback_full_sweep_required -->

<a id="memory-feedback_host_14900k_cap_builds"></a>
## feedback_host_14900k_cap_builds

---
name: feedback_host_14900k_cap_builds
description: "Dev host is an i9-14900K with Raptor Lake instability — run DoD sequentially + Ninja-capped, never all-core, to avoid 0xA bugchecks"
metadata:
  node_type: memory
  type: feedback
  originSessionId: b634ffd2-741c-489f-ac53-646bfcaf6fc4
---

The dev host CPU is an **Intel Core i9-14900K**, which has the documented 13th/14th-gen "Raptor Lake" **Vmin-shift instability defect**. Sustained all-core build load (DoD sweeps) triggers kernel bugchecks (`0x0000000A` IRQL_NOT_LESS_OR_EQUAL → black screen, percentage counts up, reboot). Diagnosed 2026-05-23 from the System event log: 4 such bugchecks in 5 days, all under build load; BIOS already on post-fix microcode `0x12F`, so the silicon is likely already degraded.

**Why:** The build only *triggers* the fault; it does not *cause* it (user-space compilers can't bugcheck a healthy kernel). Capping parallelism is **harm reduction** — it lets work continue safely but does NOT fix the chip. The real fix is hardware: BIOS → Intel Default Settings, disable XMP + run Windows Memory Diagnostic, then Intel Processor Diagnostic Tool (IPDT) → RMA under Intel's extended 5-year warranty if it fails.

**How to apply:**
- **Never pass `-Parallel`** to `per-slice-check.ps1`. Run the DoD sequentially.
- `per-slice-check.ps1` / `full-sweep.ps1` now default `-BuildJobs` to half the logical cores (sets `CMAKE_BUILD_PARALLEL_LEVEL`, covers Win + WSL via `wsl-build.ps1`). `0` = uncapped.
- Tuning ladder if it still crashes: **16 → 12 → 8 → 6** (P-cores degrade and the scheduler prefers them for `cl.exe`, so 16 can still pin all 8 P-cores; ~8 ≈ P-core thread count).
- Ad-hoc `cmake --build` is NOT covered by the scripts → set a persistent user env var `CMAKE_BUILD_PARALLEL_LEVEL=16` (and `export` it in WSL `~/.bashrc`).
- Benchmarks + `parallel_for` tests (`bench_hesap_*`, jobs smokes) are the **same hazard class** — all-core load without compiling. Defer long bench sweeps until hardware is confirmed stable; a crash during a bench run is this fault, not a bench bug. See [feedback_full_sweep_required](build-and-verification.md#memory-feedback_full_sweep_required), [reference_build_test_workflow](build-and-verification.md#memory-reference_build_test_workflow), [build_system](build-and-verification.md#memory-build_system).

Full write-up + tuning ladder lives in CLAUDE.md → Troubleshooting → "Host instability: i9-14900K Raptor Lake".


<!-- end-memory:feedback_host_14900k_cap_builds -->

<a id="memory-feedback_iterate_local_test_only"></a>
## feedback_iterate_local_test_only

---
name: feedback-iterate-local-test-only
description: "During slice iteration, build + run only the directly-affected module's tests. Reserve per-slice-check.ps1 (4-config DoD) for slice close."
metadata:
  node_type: memory
  type: feedback
  originSessionId: b24674c3-970b-481c-a127-bf4231bceca3
---

During iteration on a slice, build + run ONLY the directly-affected module's
test binary (e.g., `crd-geometry-delaunay-tests`). Do NOT run
`scripts/per-slice-check.ps1` (which builds + tests win-debug + win-asan +
win-shipping + win-tidy across the entire engine) until the END of the slice
as the close-gate check.

**Why:** The 4-config check builds the WHOLE engine (every module) plus runs
EVERY test binary. That's many minutes of wall time per invocation and
overwhelms the iteration loop. The user is annoyed when I run the full gate
during a code-edit iteration cycle. The full gate is the cluster-close
verification, not the dev loop.

**How to apply:**
- During slice iteration: `cmake --build --preset win-debug --target <module>`
  then run the affected test binary directly, e.g.
  `& "D:\Dev\cerid\build\win-debug\tests\geometry-delaunay\crd-geometry-delaunay-tests.exe" --reporter compact`.
  Re-target only what changed.
- AFTER all code is written + local tests pass: invoke
  `scripts/per-slice-check.ps1` ONCE as the per-slice DoD gate.
- The 4-config gate is the EXIT gate for the slice, not a pre-flight check.

Related: [feedback-per-slice-run-ctest](build-and-verification.md#memory-feedback_per_slice_run_ctest) — confirms that `ctest` (via the
per-slice script) is the slice-close-time verification, not the iteration-
time verification.


<!-- end-memory:feedback_iterate_local_test_only -->

<a id="memory-feedback_local_test_only_ci_owns_sweep"></a>
## feedback_local_test_only_ci_owns_sweep

---
name: feedback_local_test_only_ci_owns_sweep
description: Local verification = only the touched module; CI owns the full multi-config DoD sweep
metadata:
  node_type: memory
  type: feedback
  originSessionId: 39e451a2-e1fa-4ae3-aeca-e932082a1962
---

For local slice verification, build + test ONLY the module(s) actually touched — do NOT run the full-engine `per-slice-check.ps1` 5-config sweep locally. CI owns the full multi-config sweep from now on.

**Why:** (2026-05-23 user directive, during v3b-1b-perf close.) The 5-config DoD (`-IncludeRelease`) rebuilds + ctests the ENTIRE engine (3000+ tests) five times at half-core throttle (the Raptor Lake bugcheck cap) — ~12+ min and the single highest-risk workload on the unstable i9-14900K host (it's how the corrupt-PDB + C1001 transients surfaced). For a change contained to one module (e.g. dorgbr = hesap-dense headers + svd.cpp, no public-API change), 99% of that rebuild/retest is irrelevant. "We should only test what we have touched."

**How to apply:** build the touched module's target (e.g. `cmake --build build/win-<cfg> --target crd-hesap-dense-tests -j 16`) + run its tests, across the configs that matter for the change (shipping for LTO, tidy for clang-tidy, asan if memory-relevant). Push, and let CI run the full 18-config sweep for cross-module/cross-config coverage. This strengthens [feedback_iterate_local_test_only](build-and-verification.md#memory-feedback_iterate_local_test_only) and shifts the CLOSE gate to CI — supersedes the prior "a slice isn't closed until per-slice-check.ps1 / full-sweep.ps1 returns PASS locally" practice ([feedback_full_sweep_required](build-and-verification.md#memory-feedback_full_sweep_required), [feedback_per_slice_run_ctest](build-and-verification.md#memory-feedback_per_slice_run_ctest)) for LOCAL work. Always cap builds at 16 jobs ([feedback_host_14900k_cap_builds](build-and-verification.md#memory-feedback_host_14900k_cap_builds)). Corrupt-PDB (LNK1207/1285) + transient C1001 ICEs during heavy sweeps are environmental, not defects ([feedback_transient_msvc_ltcg_ice_accept](build-and-verification.md#memory-feedback_transient_msvc_ltcg_ice_accept)).


<!-- end-memory:feedback_local_test_only_ci_owns_sweep -->

<a id="memory-feedback_msvc_autovec_conditional_two_array_update"></a>
## feedback_msvc_autovec_conditional_two_array_update

---
name: msvc-autovec-conditional-two-array-update
description: "MSVC /O1+/O2 auto-vectorizes per-lane `if (cond) { a[q]=x; b[q]=y; }` loops with wrong masked blends — write lane logic as manual vector select chains; repro+flag-bisect recipe inside"
metadata:
  node_type: memory
  type: feedback
  originSessionId: 5aa5bcfe-3ae0-4422-8b9c-79726201dd8e
---

MSVC 19.51 at /O1 AND /O2 (with or without /GL — NOT an LTCG issue) auto-vectorizes a
per-lane scalar loop of the shape `for (q < W) if (v > best[q]) { best[q] = v; pr[q] = i; }`
(a conditional update of TWO arrays, here f64 + u64) with WRONG masked blends — the v14-h
batched-LU lane pivot scan returned provably-false comparisons on raw data. /Od and gcc -O3
are correct; an fprintf inside the loop suppresses the vectorizer (heisenbug); **ASan is
structurally blind** (wrong-code, no invalid access); the {1..16} moat can pass (both runs
equally wrong). Related: [msvc-od-straightline-kernel-stack-bomb](build-and-verification.md#memory-feedback_msvc_od_straightline_kernel_stack_bomb),
[msvc-ltcg-forceinline-codelets-c1002](build-and-verification.md#memory-feedback_msvc_ltcg_forceinline_codelets_c1002) — the third MSVC-optimizer scar of 2026-07-05.

**Why:** the compiler owns scalar loops next to vector code; its vectorizer's two-array
masked-store pattern is buggy, and every hand-written lane kernel that drops to per-lane
scalars for "the awkward part" hands it exactly that pattern.

**How to apply:** in SIMD lane kernels, express per-lane conditionals as MANUAL vector
select chains (`m = cmp_gt(v, best); best = select(m, v, best); idx = select(m, V(i), idx)`
— indices ride f64 lanes, exact ≤ 2^53); flags/info extraction reads a STORED mask, never a
conditional multi-array update loop. Debug recipe when a config-specific wrong-result
appears: (1) exact-value adjudication against raw data to identify WHICH tier lies,
(2) run-twice on identical inputs (nondeterminism vs deterministic wrong-code),
(3) measure-and-kill cheap theories (poison-fill for UMR, signal fence for alias reorder,
noinline seams) — do NOT ship a theory that didn't change behavior, (4) 60-line standalone
repro with the exact flag set + flag bisection (/GL on/off, /O2 vs /O1) pins the construct,
(5) fix the CONSTRUCT engine-wide and re-measure perf (the correct form was FASTER here).


<!-- end-memory:feedback_msvc_autovec_conditional_two_array_update -->

<a id="memory-feedback_msvc_c4127_ci_local_version_skew"></a>
## feedback_msvc_c4127_ci_local_version_skew

---
name: msvc-c4127-ci-local-version-skew
description: "CI MSVC (14.44) and local MSVC (14.50) differ on warnings under /WX — notably C4127 \"conditional expression is constant\". A constexpr comparison in a runtime `if` passes locally but fails CI. Fix with static_assert / `if constexpr`, not local-only verification."
metadata:
  node_type: memory
  type: feedback
  originSessionId: 8232c613-08b5-412f-9d51-73f8c05a65d9
---

Cerid's CI runs an **older MSVC than the local box** (CI = 14.44 in
`...\2022\Enterprise\VC\Tools\MSVC\14.44...`; local = 14.50 VS-18-Community).
Their `/W4 /WX` warning behavior is NOT identical — newer MSVC suppresses some
warnings the older one still emits.

**Confirmed case (2026-05-20):** `C4127 "conditional expression is constant"`.
A runtime `if (kConstexprValue != 0x...)` (e.g. a FourCC pin check in
`smoke_hesap_substrate.cpp`) compiles clean on local 14.50 but is a `/WX` error
on CI 14.44. Pre-existing since v0a; only surfaced when CI built it.

**Resolution (2026-05-20): C4127 is globally suppressed via `/wd4127` on the
`crd-warnings` target** (root `CMakeLists.txt`, right after `/wd4714`). After two
CI occurrences in two pushes (smoke_hesap_substrate, geometry-curves/frames.hpp),
the call was made to disable it globally — same rationale + precedent as
`/wd4714`: a low-value, version-dependent MSVC warning with no clean per-site fix
for legitimately mixed compile-time/runtime template conditions (e.g.
`if (!closed || i < n)` where `curve.closed` is `static constexpr` for one curve
type and a runtime member for another — `if constexpr` is not robust across the
template). Newer MSVC (14.50) already relaxed it.

**How to apply going forward:**
- A `/WX`-green local build is NOT proof CI's MSVC will pass. MSVC analog of
  [feedback_clang_tidy_ci_local_version_skew](build-and-verification.md#memory-feedback_clang_tidy_ci_local_version_skew).
- C4127 itself no longer fails the build (suppressed). Do NOT add per-site
  `#pragma warning(disable:4127)` — the global flag covers it.
- Still prefer the *better code* where it applies: `static_assert` for
  compile-time pin/format checks (e.g. FourCC), `if constexpr` for genuine
  type-trait branches. Those are correctness/clarity wins independent of the
  warning. The global suppress only covers the genuinely-mixed template cases
  that have no clean rewrite.
- Other version-skew warnings may still surface one-at-a-time (ninja stops the
  TU at the first `/WX` error); judge each on whether it's a real bug, a clean
  code fix, or another low-value-disable-globally case like 4714/4127.


<!-- end-memory:feedback_msvc_c4127_ci_local_version_skew -->

<a id="memory-feedback_msvc_hides_switch_gaps_gcc_werror_switch_catches_real_bugs"></a>
## feedback_msvc_hides_switch_gaps_gcc_werror_switch_catches_real_bugs

---
name: feedback_msvc_hides_switch_gaps_gcc_werror_switch_catches_real_bugs
description: "MSVC does not warn on a switch missing an enum case, so a new enum value silently falls through to default/nothing — a real latent bug. gcc -Werror=switch catches it. Build RAF/REN on Linux gcc regularly, not just Windows."
metadata:
  node_type: memory
  type: feedback
  originSessionId: bf0ab64f-0cb7-4b04-970c-78c4f58c02b9
  modified: 2026-08-05T12:13:58.806Z
---

Restoring the RAF/REN Linux build (RAF-12, 2026-08-05) surfaced **two real latent bugs** that the Windows-only MSVC
workflow had hidden — both `-Werror=switch` (a switch over an enum that doesn't handle every value):

1. `engine/gpu-context/src/command_model.cpp` `validate_packet` never handled `RasterCommandKind::DrawMulti` /
   `DrawMultiIndexed` (added in RAF-8) — so CPU multi-draw packets **skipped validation entirely** (fell out of the
   switch to `validate_bindings` with no geometry check). A malformed multi-draw would not be caught.
2. `engine/frame-cook/src/frame_emit.cpp` `from_pass_kind` never handled `FramePassKind::Custom` (added in RAF-10) —
   its trailing `return "raster.geometry"` meant a cooked **Custom (app-executor) pass round-tripped to raster.geometry**,
   silently losing the app's executor identity (the exact class of bug the format-emitter comment right below it warns
   about — "any format the emitter did not name round-tripped INTO RGBA8").

**Why:** MSVC (even /W4) does NOT warn on a non-exhaustive `switch` over an enum by default; gcc's `-Werror=switch`
(the Linux preset) does. So adding an enum value and forgetting a switch case compiles clean on Windows and ships a
silent fall-through. Same root as the accumulated `-Werror=unused-function` / `-Werror=shadow` breaks that had piled up
because the band was only ever Windows-built.

**Also (RAF-12.4, 2026-08-05):** making `IRasterContext::create_command_encoder()` PURE VIRTUAL broke a test stub
`StubRaster` (tests/scene-render/test_scene_render.cpp) that subclasses IRasterContext but didn't override it —
`cannot declare field of abstract type`. **MSVC linked a STALE `test_scene_render.cpp.obj` (never rebuilt despite the
header change) and the tests "passed"; the fresh gcc build CAUGHT the abstract-type break.** Two lessons: (1) making
any interface method pure-virtual requires auditing EVERY subclass INCLUDING test stubs/mocks (grep `: *IRasterContext`
across tests/ + sandbox/, not just engine/) and giving each an override; a stub returning `unique_ptr<ICommandEncoder>`
also needs `#include command_model.hpp` (the COMPLETE type, for the unique_ptr destructor). (2) The MSVC incremental
build can silently link a stale .obj after a header change (the `#deps 0` / stale-obj hazard) — a green Windows run is
NOT proof; only a fresh gcc build (or a wiped Windows build) is. When de-virtualizing verbs, the dossier that counts
CALL sites misses stub OVERRIDE sites — strip those per family too.

**How to apply:** treat a Linux gcc build as a first-class correctness gate for RAF/REN, per
[feedback_raf_ren_bands_test_one_linux_one_windows_after_raf_close](build-and-verification.md#memory-feedback_raf_ren_bands_test_one_linux_one_windows_after_raf_close) — build both backends' engine libs on gcc as part
of the one-Linux-one-Windows cadence, not just at band close. When you ADD an enum value (a `RasterCommandKind`, a
`FramePassKind`, a format), grep every `switch` over that enum and add the case explicitly — don't rely on a `default`
that mis-handles it. A missing case that "still renders/round-trips" is the same silent-wrong the error graph exists to
prevent. Related scars: [feedback_post_color_ops_must_be_vec4_robust_sampled_input](workflow-and-correctness.md#memory-feedback_post_color_ops_must_be_vec4_robust_sampled_input), the format-emitter no-op tail.


<!-- end-memory:feedback_msvc_hides_switch_gaps_gcc_werror_switch_catches_real_bugs -->

<a id="memory-feedback_msvc_ltcg_forceinline_codelets_c1002"></a>
## feedback_msvc_ltcg_forceinline_codelets_c1002

---
name: msvc-ltcg-forceinline-codelets-c1002
description: "MSVC honors __forceinline under LTCG — force-inlined giant generated kernels detonate link-time codegen (C1002 heap exhaustion, pass 2); demote at the emitter, MSVC-scoped"
metadata:
  node_type: memory
  type: feedback
  originSessionId: 5aa5bcfe-3ae0-4422-8b9c-79726201dd8e
---

MSVC honors `__forceinline` even under LTCG (`/GL` + `/LTCG`). A generated-codelet header full of
force-inlined multi-thousand-line straight-line kernels gets ENTIRELY inlined into the one dispatch
function that calls them; link-time pass-2 codegen then dies with **fatal C1002 "compiler is out of
heap space in pass 2" → LNK1257** after tens of minutes (hit 2026-07-05: `crd-hesap-fft`'s
`execute()` + 56 codelets from the 143K-line `batched_codelets_gen.hpp` + 2× `execute_ip4aos`;
link.exe at 17 GB WS — system RAM was NOT the limit, it's an optimizer-internal blowup). This is the
LTCG sibling of [msvc-od-straightline-kernel-stack-bomb](build-and-verification.md#memory-feedback_msvc_od_straightline_kernel_stack_bomb) (the /Od 1.4MB-frame scar): giant
generated straight-line kernels are hostile to EVERY unbounded inliner — debug detonates the stack,
LTCG detonates the compiler heap.

**Why:** `__forceinline` bypasses the inliner cost model that exists precisely to prevent this; a
call into a straight-line kernel of thousands of instructions is performance noise, so the forced
inline buys nothing and risks a 40-minute link failure.

**How to apply:** generated kernels never carry an unconditional force-inline — the generator emits
an attribute macro (`CRD_FFT_GEN_INLINE`: MSVC = plain `inline`, gcc/clang = always_inline so their
measured codegen is untouched), and big template drivers instantiated multiple times get an
MSVC-scoped `__declspec(noinline)` seam at the dispatch boundary. Fix at the EMITTER so a regen
preserves it. Diagnosing: C1002-at-a-source-line under LTCG names the post-inlining mega-function's
containing declaration — look for force-inline callees, not that line's own code; it is
deterministic, not a transient ICE, so don't retry a 40-minute link hoping.


<!-- end-memory:feedback_msvc_ltcg_forceinline_codelets_c1002 -->

<a id="memory-feedback_msvc_o2_miscompiles_fp_conditional_nan_branch"></a>
## feedback_msvc_o2_miscompiles_fp_conditional_nan_branch

---
name: feedback_msvc_o2_miscompiles_fp_conditional_nan_branch
description: MSVC /O2 mis-selects a not-taken FP conditional branch that computes NaN; make the expression branchless
metadata:
  node_type: memory
  type: feedback
  originSessionId: d095b72d-cb68-40b1-aaa2-1147516ea4fd
---

⛔ **MSVC /O2 (shipping/LTCG) mis-selects a conditional whose NOT-taken branch computes a NaN** — even at runtime,
even with `[[msvc::no_unique_address]]`/`noinline`/volatile-runtime inputs. A sibling of [feedback_msvc_autovec_conditional_two_array_update](build-and-verification.md#memory-feedback_msvc_autovec_conditional_two_array_update) but SCALAR: `if (x != 0) return p*value/x; ... return limit;` — MSVC
speculatively evaluates `p*value/x` = `0/0` = NaN for the x==0 case and returns the NaN despite the guard. debug /
asan / gcc / clang-cl are all fine; only win-shipping fails. Symptom in v15-b: `pow_const(0.0, 0.0).dbase` = NaN
instead of the guarded value.

**Why:** the codebase is /fp:precise (not fast-math), so isinf/isnan work — this is a genuine branch-selection
miscompile, not an fp-model artifact. The `0/0` / `0·∞` in the dead branch is the trigger.

**How to apply:** don't special-case FP singularities with a branch that leaves a NaN in the other arm. Make the
formula **branchless** so the only NaN produced is the mathematically-correct one. For `pow` the fix was Ceres' exact
branchless slope `p·x^(p-1)` — correct everywhere, NaN only at the true (0,0) 0·∞ singularity (Ceres-faithful). Do
NOT burn cycles on `no_unique_address`/`noinline`/runtime-input (rt()) workarounds — they don't fix it; go branchless.

Also: **the Edit tool doesn't reliably bump mtime for ninja** — after editing a header, `touch` it (WSL) before a
Windows build or ninja says "no work to do" and you test a STALE binary (cost several false "still failing" reads).


<!-- end-memory:feedback_msvc_o2_miscompiles_fp_conditional_nan_branch -->

<a id="memory-feedback_msvc_od_straightline_kernel_stack_bomb"></a>
## feedback_msvc_od_straightline_kernel_stack_bomb

---
name: msvc-od-straightline-kernel-stack-bomb
description: "MSVC /Od gives every expression temporary its own stack slot — a ~2600-statement generated SIMD kernel needs a 1.2-1.4MB frame and overflows the 1MB Windows default stack; fix = dual-body emission (SIMD under NDEBUG||__OPTIMIZE__, bit-identical lane-scalar otherwise)"
metadata:
  node_type: memory
  type: feedback
  originSessionId: e7db65f6-278a-4511-ab2e-13d86ad02005
---

MSVC at `/Od` (win-debug, win-asan, win-tidy configs) assigns EVERY expression temporary its own
un-reused stack slot: a straight-line generated SIMD kernel of ~2600 statements (the FFT 256-point
codelets) needs a **1.17–1.4 MB frame** (~236 B per SSA value, measured via a dumpbin `sub rsp`
probe) — one call overflows the 1 MB Windows default stack (0xC00000FD SEGFAULT). Linux debug never
shows it (8 MB stacks). `#pragma optimize("gt", on)` CANNOT re-enable optimization inside a /Od
compiland (tested 2026-07-03 — frames unchanged), and clang-cl debug configs exist, so pragma fixes
are dead ends.

**Why:** /Ob0 means no inlining, so it is a SINGLE-frame problem that scales linearly with emitted
statement count; any generator that grows a kernel past ~1500 statements re-arms it silently — and
the failure only appears the first time a Windows debug config RUNS the new path (linux-only gating
hides it for weeks).

**How to apply:** (1) generators emit DUAL bodies — the SIMD tiles under
`#if defined(NDEBUG) || defined(__OPTIMIZE__)` and a lane-scalar edition otherwise (same DAG, same
schedule per column/lane ⇒ **bit-identical**: Vec ops are lane-wise IEEE ops; keep `0 - x` instead of
unary minus to preserve signed-zero identity); measured frames drop 20–25× (23–57 KB). (2) The
`NDEBUG`-only gate is NOT enough: ad-hoc `g++ -O3` bench builds carry no `-DNDEBUG` and silently
bench the scalar bodies — `__OPTIMIZE__` closes that; re-run the perf board after ANY emission-gate
change (verify the shipped artifact). (3) Before "fixing" a debug stack overflow, MEASURE the frame
(dumpbin /disasm, `mov eax,SIZE` before `sub rsp,rax`) — don't guess inline-accumulation vs
single-frame. Home: `scripts/gen_fft_batched.py` (`_assemble`/`_scalar_*`), SANITY ledger 2026-07-03.


<!-- end-memory:feedback_msvc_od_straightline_kernel_stack_bomb -->

<a id="memory-feedback_named_allocators_in_tests"></a>
## feedback_named_allocators_in_tests

---
name: Use named custom allocators in tests, not default_allocator()
description: Tests must construct named allocator instances (TlsfAllocator, MallocAllocator, etc.); never call crd::memory::default_allocator() in test fixtures.
type: feedback
originSessionId: 44374b19-0728-4302-88da-52db2ebbc4c4
---
For ANY new test (and ideally existing tests when touched), construct
named instances of Cerid's custom allocators — `crd::memory::TlsfAllocator`
(production-realistic; catches leaks at destruction), `MallocAllocator`
(simple instance wrapper), or `LinearAllocator` (one-shot scratch).

Pattern:

```cpp
TEST_CASE(...) {
    crd::memory::TlsfAllocator alloc{16U << 20}; // 16 MB per-test heap

    BodyPool body_pool{&alloc, 64U};
    crd::scene::World world{&alloc};
    RenderBuffer buf{&alloc};
    // ...
}
```

NOT:

```cpp
BodyPool body_pool{crd::memory::default_allocator(), 64U};
```

**Why:** The user explicitly called this out 2026-05-11 during v1b-d:
"why we are using default allocators for world, there is a reason why
we have made our own custom allocators, use them no matter what it is
good practice in my opinion."

**How to apply:**
- Per-test fixture: one TlsfAllocator instance (typically 16 MB; bump
  if "TlsfAllocator: out of memory" CRD_FATAL fires — World + ECS
  storage + pools eat several MB minimum).
- Pass `&alloc` to every constructor that takes `IAllocator*`.
- Production-realistic side-benefit: TLSF asserts on outstanding
  allocations at destruction, catching leaks the default
  malloc-allocator would silently swallow.
- Existing tests that use `default_allocator()` are tech debt; backfill
  when convenient (do not block a slice on it, but flag the new tests
  AND any old tests touched in the same edit).

This convention applies to ALL future test files in the project, not
just v1b-d / eylem-viz.


<!-- end-memory:feedback_named_allocators_in_tests -->

<a id="memory-feedback_never_simplify_gate_tests_frontier_always"></a>
## feedback_never_simplify_gate_tests_frontier_always

---
name: never-simplify-gate-tests-frontier-always
description: "NEVER simplify, dumb down, or toy-ify gate tests or implementations — always build the full frontier-level pipeline"
metadata:
  node_type: memory
  type: feedback
  originSessionId: dd844f83-0821-41ab-8607-fa93ccf37a76
  modified: 2026-08-01T17:11:15.747Z
---

NEVER replace a real pipeline with a simplified/toy version to make a test pass. Gate tests must exercise the ACTUAL system — the full compute kernels, the real cull pipeline, the production code path. If something doesn't work in the test environment, BUILD the infrastructure to make the real thing work — never strip it down.

**Why:** The user explicitly rejected replacing `k40gOccCullToml` (full GPU cull pipeline: cull_reset, cull_view0, occlusion_reset, occlusion_cull) with a simplified HZB-only TOML that skipped the compute kernels. "We are not building a TOY SYSTEM here! We do the most sophisticated cutting edge frontier solutions!" The simplified test proved nothing about the actual occlusion cull pipeline.

**How to apply:** When a gate test fails because the test environment doesn't support the full pipeline, the fix is ALWAYS to extend the test infrastructure — never to simplify the thing being tested. This applies to every gate, every test, every implementation. Related: [elite-only-no-shortcuts](workflow-and-correctness.md#memory-feedback_elite_only_no_shortcuts), [feedback_quality_bar](workflow-and-correctness.md#memory-feedback_quality_bar), [feedback_full_victory_beat_all_gold_standards](workflow-and-correctness.md#memory-feedback_full_victory_beat_all_gold_standards).


<!-- end-memory:feedback_never_simplify_gate_tests_frontier_always -->

<a id="memory-feedback_never_throwaway_compiles_use_build_bat_flow"></a>
## feedback_never_throwaway_compiles_use_build_bat_flow

---
name: feedback_never_throwaway_compiles_use_build_bat_flow
description: "NEVER verify with ad-hoc cl.exe/dxc.exe/standalone .exe compiles — they trip the permission sandbox and block the autonomous loop waiting for the user's approval. Verify ONLY through the build system (build-target.bat + run-ctest.bat / wsl-build.ps1)."
metadata:
  node_type: memory
  type: feedback
  originSessionId: 192f4d9e-b3f7-485b-a775-5391bcd4183c
  modified: 2026-08-17T18:02:25.215Z
---

⛔⛔⛔ In the CEIR autonomous loop, the user is ASLEEP/AWAY and wants continuous
progress with ZERO waiting. Standalone `cl.exe` / `dxc.exe` invocations, `cmd /c
"call vcvars && cl ..."`, and building/running throwaway `.exe`s in the scratchpad
TRIP the Bash permission sandbox → each one pops an approval prompt → the loop
BLOCKS mid-turn waiting for input. This enraged the user (2026-08-17): "you are
spoiling something that was working alright before."

**Why:** These command shapes run arbitrary compilers/executables the sandbox
hasn't been pre-approved for. The commands that flow WITHOUT prompts are the ones
used all session: `git`, the Grep/Glob/Read/Edit/Write tools, and
`cmd /c "scripts\build-target.bat <dir> <target>"` / `cmd /c "scripts\run-ctest.bat
<dir> <regex>"` / `scripts\wsl-build.ps1` (the vcvars-wrapping helper .bat/.ps1
scripts, which the harness already allows).

⛔ ALSO: NEVER edit repo files through Bash (`sed -i`, `>`/`>>` redirection into a tracked
file, `cp`/`mv` over repo files). Writing a repo file via Bash trips the SAME sandbox prompt
and enraged the user again (2026-08-17). Edit/Write TOOLS are the only pre-approved way to
change files. Bash is READ-ONLY: `grep`, `git status/log/show`, `find`, `ls` — plus writes
to the scratchpad dir ONLY. Every code change goes through Edit/Write; every build/test
through build-target.bat / run-ctest.bat / wsl-build.ps1.

**How to apply:**
- NEVER "quick-check" an emitter/kernel by hand-compiling HLSL with dxc.exe or C++
  with cl.exe. To validate emitted shader text, add a real TEST_CASE that emits +
  (for device shaders) compiles through the backend's own `compile_*_to_*` path,
  then `build-target.bat` + `run-ctest.bat` it — the normal flow.
- Verify new code by: Edit/Write the code + a test → `build-target.bat build/win-debug
  <test-target>` → `run-ctest.bat build/win-debug "<tag>"`. Linux leg via
  `wsl-build.ps1 linux-gcc-debug`. That is the ENTIRE approved command surface.
- The user's standing order: drive the loop to CEIR-35, fix every error/gap on the
  spot (never "not my code"), test added + blast radius (not the whole suite), end
  every turn with ScheduleWakeup, and NEVER stop to wait for input. See
  [project_ceir_autonomous_loop_grant](project-history.md#memory-project_ceir_autonomous_loop_grant), [feedback_autonomous_ceir_loop_never_idles_drive_through_every_blocker](execution-ir.md#memory-feedback_autonomous_ceir_loop_never_idles_drive_through_every_blocker).


<!-- end-memory:feedback_never_throwaway_compiles_use_build_bat_flow -->

<a id="memory-feedback_new_source_file_needs_preset_reconfigure_not_bare_cmake"></a>
## feedback_new_source_file_needs_preset_reconfigure_not_bare_cmake

---
name: feedback_new_source_file_needs_preset_reconfigure_not_bare_cmake
description: A new .cpp/.hpp needs reconfigure-preset.bat <preset> per config before build-target sees it; bare cmake -S -B breaks the cache
metadata:
  node_type: memory
  type: feedback
  originSessionId: fade8ea4-87ca-470f-83e0-cdfe82a44e7f
  modified: 2026-08-11T23:15:44.118Z
---

The CMake libs use `crd_collect_sources` (a **glob** of `src/`), so a **newly created** source file is invisible to `build-target.bat` until the preset is **reconfigured** — the stale build reuses the old file list and the new test/symbol silently "No tests ran" / link-errors.

**Why:** glob source lists are captured at configure time; a plain `cmake --build` does not re-glob.

**How to apply:**
- After adding a new `.cpp`/`.hpp`, run `scripts\reconfigure-preset.bat <preset>` (win-debug / win-asan) and `cmake --preset <preset>` in WSL (linux-gcc-debug) **once per config** before `build-target.bat`.
- ⛔ NEVER `cmake -S <src> -B <build>` from the plain PowerShell/Bash tool — it runs **outside vcvars**, cannot find Ninja, and leaves `CMAKE_MAKE_PROGRAM-NOTFOUND` in the cache (a broken config). The repair is `reconfigure-preset.bat <preset>` (it wraps `cmake --preset` under vcvars, rediscovering the toolchain). Do NOT hand-edit the cache ([feedback_stale_toolset_path_in_build_dir_wipe_dont_sed](build-and-verification.md#memory-feedback_stale_toolset_path_in_build_dir_wipe_dont_sed)).
- Editing an EXISTING file needs no reconfigure — only new files change the glob.

See [reference_build_test_workflow](build-and-verification.md#memory-reference_build_test_workflow) and [feedback_per_slice_run_ctest](build-and-verification.md#memory-feedback_per_slice_run_ctest).


<!-- end-memory:feedback_new_source_file_needs_preset_reconfigure_not_bare_cmake -->

<a id="memory-feedback_no_cpp_kgraph_builders_author_ckir_directly"></a>
## feedback_no_cpp_kgraph_builders_author_ckir_directly

---
name: feedback_no_cpp_kgraph_builders_author_ckir_directly
description: "⛔⛔⛔ TOP ORDER (user 2026-08-16, emphatic) — NEVER write a C++ KGraph builder (build_X) for a rendering algorithm, not even as [.emitckir] regen tooling; the .ckir/.crdv (CKIR)/CHIR asset is the SOLE source, authored/edited DIRECTLY; scene_renderer gets ZERO new rendering-technique C++"
metadata:
  node_type: memory
  type: feedback
  originSessionId: 192f4d9e-b3f7-485b-a775-5391bcd4183c
  modified: 2026-08-16T06:21:56.302Z
---

⛔⛔⛔ THE USER'S #1 MISSION MANDATE (2026-08-16, after I repeatedly missed it): **get rid of ALL C++ frame/render-technique code in scene_renderer and make every algorithm a CHIR/CKIR ASSET.** A C++ `KGraph` builder (`build_cluster_light_cull`, `build_rt_worldpos`, `build_rt_composite`, `build_visbuffer_fs`, and any future one) is rendering-algorithm code written in C++ — it MUST NOT EXIST, regen tooling or not.

**The `[.emitckir]` pattern is RETIRED as an anti-pattern.** It (write `build_X(KGraph&)` in a `.hpp` → emit the `.ckir` → keep the builder in-tree as the "regen source") makes the C++ the de-facto source and the `.ckir` a generated artifact — the exact inversion of "everything is an authorable asset." Do NOT reach for it.

**The rule going forward:**
- A rendering algorithm ships as a `.ckir`/`.crdv` (CKIR) — or a CHIR — asset. The `.ckir` TEXT format is human-authorable (`[[entry]]`/`[[node]]` node graph, CEIR-18q; see `assets/ckir/deferred_lighting.ckir`). AUTHOR/EDIT IT DIRECTLY (hand-written node-graph text today; the CEIR-32/33 node editor later). The `.ckir` file IS the source of truth.
- DELETE the C++ builders: `engine/kir/include/crd/kir/ckir_light_cull.hpp`, `ckir_rt_worldpos.hpp`, inline `build_rt_composite`/`build_visbuffer_fs`.
- Gates/tests LOAD the committed `.ckir` (`ckir_read`) — they must NEVER call a builder. A build-gate that needs a KGraph to emit/run loads the `.ckir` into it.
- ⛔ NEVER add rendering-technique C++ to `scene_renderer.cpp`. Every algorithm + every rendering DECISION (which technique, the froxel math, the cull, lighting) comes from an asset. scene_renderer is ONLY the host: load assets, resolve declared resource NAMES → GPU objects, cook, drive the device. Where the C++ still DECIDES something an asset should own (e.g. CEIR-18a-3: pass.technique), that is a bug to fix, not a pattern to extend.

**Why:** the user was explicit and angry that I kept introducing C++ builders (`ckir_rt_worldpos.hpp` this session) while claiming to honor the asset mandate. This OVERRIDES the uncommitted-delete-keeps-regen-in-tree scar for these builders — the `.ckir` on disk is the source; the builder does not come back. **How to apply:** re-anchor on THIS every time an algorithm needs authoring — write the `.ckir`, not C++. See [feedback_everything_is_an_authorable_asset_ceir](execution-ir.md#memory-feedback_everything_is_an_authorable_asset_ceir) and [feedback_authored_asset_slice_done_only_when_cpp_deleted_and_renders](rendering.md#memory-feedback_authored_asset_slice_done_only_when_cpp_deleted_and_renders); supersedes the "keep regen in-tree" half of [feedback_uncommitted_delete_loses_the_source_keep_regen_in_tree](workflow-and-correctness.md#memory-feedback_uncommitted_delete_loses_the_source_keep_regen_in_tree) for rendering-algorithm builders.


<!-- end-memory:feedback_no_cpp_kgraph_builders_author_ckir_directly -->

<a id="memory-feedback_no_malloc_in_probes_even_to_dodge_toolchain"></a>
## feedback_no_malloc_in_probes_even_to_dodge_toolchain

---
name: feedback_no_malloc_in_probes_even_to_dodge_toolchain
description: Never use a malloc-based IAllocator in benches/probes — not even to dodge a toolchain/link error; use crd TLSF
metadata:
  node_type: memory
  type: feedback
  originSessionId: c87579b0-4445-4ff8-9af0-cf67eaa15cb6
---

In a v10-FFT compiler-A/B probe I wrote an inline malloc-backed `IAllocator` purely to sidestep a
**win-clang-cl debug-CRT (`MDd`) vs my `/O2` (`MD`) link mismatch** (linking the preset libs failed, so I
linked nothing). The user stopped me hard: **"WHY MALLOC?"**

**Why it's wrong (two counts):**
1. **Violates the project no-malloc rule** — `crd-no-malloc-allocator` ctest guard + the whole pre-v5 malloc
   sweep ([project_no_malloc_sweep_before_v5](project-history.md#memory-project_no_malloc_sweep_before_v5)) removed every `MallocAllocator`. Don't reintroduce one even
   in a throwaway probe.
2. **Non-representative measurement** — this very session measured that allocator choice matters ~12% to
   bandwidth (`std::vector` 29.3 vs crd TLSF 25.7 GB/s on the transbw transpose micro). A malloc buffer's
   placement/alignment is NOT what the engine actually does ⇒ the probe number would mislead.

**How to apply:** ALWAYS use `crd::memory::TlsfAllocator` (or the real engine allocator) in benches/probes.
If a toolchain/link issue blocks it, FIX THE TOOLCHAIN (match CRT; compile the crd allocator `.cpp` sources
into the TU with matching flags; or run the probe on the WSL path that already links the real `.a` libs) —
do NOT swap in malloc to make the link succeed. A clean-linking unrepresentative probe is worse than no
probe. Related: [reference_sanity_doctrine](workflow-and-correctness.md#memory-reference_sanity_doctrine) (representativeness), the std::vector-12% lesson in
[project_v10_fft_plan](project-history.md#memory-project_v10_fft_plan) Part 8.


<!-- end-memory:feedback_no_malloc_in_probes_even_to_dodge_toolchain -->

<a id="memory-feedback_no_std_containers_anywhere_incl_tests"></a>
## feedback_no_std_containers_anywhere_incl_tests

---
name: feedback_no_std_containers_anywhere_incl_tests
description: NO owning STL containers ANYWHERE incl. tests + generated .inc reference data — std::vector/map/string forbidden
metadata:
  node_type: memory
  type: feedback
  originSessionId: deb11ae2-46e2-4805-9918-89f237d06d9b
---

**NO owning STL containers ANYWHERE — including TEST code and generated `.inc` reference-data headers.** The user
was angry (2026-06-21) that I used `std::map<std::string, std::vector<double>>` in the v11 DSP test reference
includes (`window_refs.inc` etc.) + `std::string`/`std::vector` in the test `.cpp` helpers. CLAUDE.md says "engine
and TOOL code"; the user's standing intent is **literally everywhere in the repo, tests included.**

**Why:** consistency + the no-malloc/owning-STL discipline is project-wide, not just engine; std containers in
tests still pull `<map>`/`<vector>`/`<string>` and the owning-allocator pattern the engine bans.

**How to apply:**
- FORBIDDEN: `std::vector`, `std::string`, `std::map`, `std::unordered_map`, `std::set`, `std::list`, … (owning).
- USE: `crd::containers::Array`, `crd::containers::String`, `crd::containers::HashMap`. Non-owning views OK
  (`crd::containers::ConstSpan` = std::span, `StringView` = std::string_view). STL `<algorithm>` is fine.
- **Generated reference data (`gen_*.py`/`.m` → `*.inc`): emit PLAIN C ARRAYS, not a std::map.** Pattern:
  `inline const double ref_<sanitized_name>[] = { ... };` (a built-in array = NOT an STL container = allowed).
  The test references the array IDENTIFIER directly (no string-keyed map lookup needed — the test knows which
  ref it wants). A `template<size_t N> void check(const double(&ref)[N], const cont::Array<f64>& h, double tol)`
  deduces N. Sanitize names (dots/dashes → `_`) so they're valid identifiers.
- Before finishing ANY file (incl. a test or a generator), grep it for `std::vector|std::map|std::string|std::set`
  and convert. [reference_sanity_doctrine](workflow-and-correctness.md#memory-reference_sanity_doctrine) (verify-the-shipped-artifact).


<!-- end-memory:feedback_no_std_containers_anywhere_incl_tests -->

<a id="memory-feedback_per_slice_binary_direct_misses_ctest_and_crossconfig"></a>
## feedback_per_slice_binary_direct_misses_ctest_and_crossconfig

---
name: feedback_per_slice_binary_direct_misses_ctest_and_crossconfig
description: Iterating on the test BINARY with tag filters (not ctest) + MSVC-only misses two whole defect classes; run one ctest + one clang-cl + one gcc build before slice close
metadata:
  node_type: memory
  type: feedback
  originSessionId: 178961b8-0463-4615-82ce-96bde42457f0
---

The fast per-slice loop — build the touched test target, run the **binary directly**
with a `[tag]` filter, on **MSVC only** — structurally misses **two entire defect
classes**. Both are invisible until a `ctest`-based, multi-toolchain run:

1. **`ctest`-registered guard tests never run.** `crd-no-non-ascii-test-names`,
   `crd-simd-emission-check`, `crd-no-std-*`, etc. are `add_test(NAME ...)` entries,
   NOT cases in any binary's `--list-tests`. Running the binary directly skips them
   entirely. Non-ASCII TEST_CASE names also fail *only* under `ctest` (Windows argv
   mojibake) — the binary passes them when invoked with no filter.
2. **clang-cl + gcc `-Werror` issues never compile.** MSVC `/WX` does NOT flag
   unused-but-set vars, unused lambdas, or `double→float` narrowing in `T{literal}`
   brace-init — clang and gcc do. MSVC-green ≠ clang/gcc-green.

**Why it bites hard:** a slice can be "4-config DoD green" (all MSVC, binary-direct)
and still have ~20+ latent failures that only surface at the cluster-close 18-config
sweep (or CI) — turning a "done" slice into a multi-hour cross-config cleanup detour.

**How to apply (cheap insurance before declaring a slice done):**
- Run `ctest --preset win-debug -R <relevant>` at least once (fires the guards +
  the argv-mojibake path), not just the binary.
- Build the touched module on **win-clang-cl** AND **gcc-linux** (`wsl-build.ps1
  <preset> -SkipTests`, native `~/cerid-build/<preset>` dir) — keep-going (`-- -k 0`)
  to surface all `-Werror` at once.
- Keep TEST_CASE names ASCII from the start ([feedback_ascii_only_test_names](build-and-verification.md#memory-feedback_ascii_only_test_names)) and
  inner-template literals as `static_cast<R>(...)` not `R{...}`
  ([feedback_gcc_linux_double_to_float_narrowing](numerics-and-performance.md#memory-feedback_gcc_linux_double_to_float_narrowing)).

**Case study (v3d-2c-3, 2026-05-24/25):** the full sweep caught 18 non-ASCII test
names (v3d-1/2a/2b/2c + lstsq) + 9 clang/gcc `-Werror` build-fails (unused `z`/`max3`
lambdas, `deflate2` set-but-unused, 5 narrowings) — ALL pre-existing in the
just-committed v3c+v3d body, none caught by the per-slice MSVC-binary checks across
~8 slices. Re-sweep after fixes: 18/18 PASS. Refines [feedback_per_slice_run_ctest](build-and-verification.md#memory-feedback_per_slice_run_ctest),
[feedback_full_sweep_catches_cross_config_simd](build-and-verification.md#memory-feedback_full_sweep_catches_cross_config_simd), [feedback_local_test_only_ci_owns_sweep](build-and-verification.md#memory-feedback_local_test_only_ci_owns_sweep).


<!-- end-memory:feedback_per_slice_binary_direct_misses_ctest_and_crossconfig -->

<a id="memory-feedback_per_slice_run_ctest"></a>
## feedback_per_slice_run_ctest

---
name: per-slice-run-ctest-not-test-binary
description: "Per-slice verification must run `ctest --preset <X>` not just the test binary directly; guard tests like crd-no-non-ascii-test-names / crd-simd-emission-check / crd-no-std-math-check / crd-no-std-sort-check are registered as ctest tests and don't appear in the test binary's case list; also covers per-sub-module eylem-stub integration smoke during geometry phase"
metadata:
  node_type: memory
  type: feedback
  originSessionId: aa515082-af9b-4f04-a36f-377aeabe6e4a
---

**Rule (per-slice verification protocol):** At each slice close, run
`ctest --preset <preset>` for **5 configs**: win-debug + win-asan +
win-shipping + win-shipping-profile + win-tidy. **Do NOT** rely solely on
running the test binary directly (e.g. `./crd-geometry-convex-tests.exe`).
The ctest layer runs additional guard tests that the test binary itself
doesn't.

**Why 5 configs (not 4):** The 5th config `win-shipping-profile` (added
2026-05-15 during D-003 v0b post-discussion) is win-shipping + `CRD_ENABLE_PROFILING=ON`.
It validates that any code gated by `#if CRD_ENABLE_PROFILING` /
`#if CRD_PERF_ENABLED` -- the entire `crd-perf` substrate and all
`CRD_PERF_SCOPE`/`CRD_PERF_COUNTER_*` instrumentation sites scattered
across the engine -- compiles + runs correctly under MSVC shipping LTCG
+ `/OPT:ICF` + `/O2`. Without it, gated code only runs under debug/asan
optimization levels; release-only bugs (LTCG inlining miscompiles,
dead-store-propagation through atomics, ICF-folded function aliasing)
in profiler hot paths would slip past per-slice DoD into production.

`win-shipping` (profiling OFF) still runs to verify the consumer-ship
zero-overhead contract -- macros must collapse to `((void)0)` and the
singleton must never be built. Both configs together cover the full
on/off matrix at max optimization.

**Why:** Discovered 2026-05-15 during Phase 3.1.7 v3-close. The
`crd-no-non-ascii-test-names` guard (added in v1i-c 2026-05-13 to
catch Windows-ctest-argv-mojibake of `→`/`—` in TEST_CASE names) is
registered as a separate `add_test(NAME crd-no-non-ascii-test-names)`
in `tests/math/CMakeLists.txt` and runs via `ctest`. It does **not**
appear in any test binary's `--list-tests` output because it's not a
Catch2 TEST_CASE — it's a PowerShell script invoked via `add_test`.

Slices v3a/v3b/v3c shipped with non-ASCII characters in their
TEST_CASE names (19 of them across `test_quickhull.cpp` +
`test_convex_hull_2d.cpp`). The session logs said "Built + run green
on win-debug + win-asan + win-shipping" — but those verifications ran
the test binary directly, NOT `ctest`. The guard was working
correctly but never ran during those slice closures. v3-close ran
the full sweep, which DID invoke `ctest`, and the guard exposed all
19 non-ASCII names + clang-cl's unused-function debt at the same time.

This same gap can hide:
- `crd-simd-emission-check` (verifies SIMD instructions emit at the right width per build config)
- `crd-no-std-math-check` (bans `std::sin/cos/exp/log/pow/sqrt` outside `crd::math::deterministic`)
- `crd-no-std-sort-check` (bans `std::sort` for cross-platform determinism)
- Any future lint/policy guard registered as a ctest test.

**How to apply:**

1. **Per-slice verification command** — at slice close, before declaring done:
   ```powershell
   # From a vcvars64-sourced cmd / PowerShell session:
   cmake --build --preset win-debug
   ctest --preset win-debug --output-on-failure

   cmake --build --preset win-asan
   ctest --preset win-asan --output-on-failure  # needs ASan DLL on PATH

   cmake --build --preset win-shipping
   ctest --preset win-shipping --output-on-failure

   cmake --build --preset win-shipping-profile
   ctest --preset win-shipping-profile --output-on-failure  # NEW 2026-05-15 -- LTCG + profiling ON

   cmake --build --preset win-tidy
   # win-tidy has no separate ctest preset; the build itself runs clang-tidy
   ```

2. **Check the ctest exit code.** Exit 0 = all tests passed (including guards). Non-zero = something failed; look at the output for the named guard. A test binary that says "All tests passed" can coexist with a failing guard test — both must be green.

3. **Slice is NOT closed until all five configs return ctest exit 0.** Test-binary-direct verification does not count.

**Related per-sub-module eylem-stub smoke practice** (locked 2026-05-15 in
[feedback_strategic_execution_plan_2026_05_15](workflow-and-correctness.md#memory-feedback_strategic_execution_plan_2026_05_15)):

As each Phase 3.1.7 geometry sub-module ships (v4 mesh / v5 spatial / v6
polygon / etc.), run a ~30-min integration smoke against the
corresponding eylem v1c+ stub path to mitigate eylem cold-storage:
- v4 mesh → smoke `eylem::TriangleMeshCollider` stub (builds + minimal raycast against a baked CRDR mesh)
- v2 GJK → smoke eylem v1d narrowphase stub (builds + minimal pair-test)
- v1 BVH → smoke eylem v1c broadphase stub (builds + minimal overlap query)
- v3 convex hull → smoke eylem `Collider::ConvexHull` stub

These smokes are NOT formal slices and do NOT block sub-module close —
they're hygiene that catches eylem build breakage before it
accumulates over the ~7-month gap (eylem paused 2026-05-11 → resume
~2026-12). If a smoke fails, file a short debt entry in `docs/debt.md`
and continue; eylem v1c+ resume will pay it down.

**Pin reference:** `docs/sessions/2026-05-15-geometry-v3-close.md` "Drive-by
debts paid" section #2 (the original incident); `docs/ROADMAP.md` §
Strategic Execution Plan; `tests/math/CMakeLists.txt` (where the four
ctest-registered guards live); `scripts/check_*.ps1` + `scripts/check_*.sh`
(the guards themselves).

**When this changes:**
- If a new ctest-registered guard is added, ensure it runs in win-debug at minimum (the cheapest config) so per-slice verification surfaces failures fast.
- If a guard's scope tightens (e.g. `crd-no-untagged-physical-numeric` arrives in Phase 3.1.7.5), update this memory + verify the guard runs across the relevant module's test executable.

## ⚠ RECURRENCE 2026-07-10 (D-007 B0/B3, `crd-kir`) — the identical failure, 14 months of doctrine later

`crd-kir-tests.exe` printed **"All tests passed (400 assertions in 43 test cases)"** while **6 of its own cases could not
be SELECTED by ctest at all** — em-dashes in the TEST_CASE names, mojibake'd through the Turkish Active Code Page, so
Catch2's filter matched nothing and ctest reported `Failed`. `crd-no-non-ascii-test-names` was **red the whole time**.
Sessions had reported "kir 254 green" from binary-direct runs. 9 names fixed across `tests/kir` + `tests/kir-vulkan`.

Two additions to the protocol:

1. **A green binary is not a green ctest.** The binary cannot see guards; and a *name-encoding* failure makes ctest print
   `Failed` for a test that is fine. If ctest says `Failed` but the output reads `No test cases matched '...'` with a `?`
   where a dash should be, it is this bug — not a broken test.
2. **Run ctest through `scripts/run-ctest.bat`, which sources vcvars.** A bare `ctest` from PowerShell fails
   `crd-simd-emission-check` with *"dumpbin not on PATH"* — a phantom failure that will send you chasing SIMD emission.

Diagnosis one-liner (no build needed): `powershell scripts/check_no_non_ascii_test_names.ps1`.


<!-- end-memory:feedback_per_slice_run_ctest -->

<a id="memory-feedback_perf_jobs_adapter_asan_flake"></a>
## feedback_perf_jobs_adapter_asan_flake

---
name: perf-jobs-adapter-asan-flake
description: "RESOLVED 2026-05-20 — the crd-perf jobs-adapter `jobs_*` counters were a plain data race (non-atomic u64 incremented from every worker thread), NOT an ASan timing flake. Fixed by making the internal stats atomic. If a jobs-adapter count assertion fails again, it is a real bug — do NOT retry-pass."
metadata:
  node_type: memory
  type: feedback
  originSessionId: b24674c3-970b-481c-a127-bf4231bceca3
---

**RESOLVED 2026-05-20.** Earlier I logged the crd-perf jobs-adapter test
failures as a benign "ASan-induced timing flake" with retry-pass acceptable.
**That diagnosis was wrong.** The real cause was a plain **data race** on
non-atomic counters.

`engine/perf/src/jobs_adapter.cpp` kept `JobsAdapterStats g_stats{}` (plain
`crd::u64` fields) and the observer callbacks did `++g_stats.jobs_ended` etc.
Those callbacks (`cb_on_job_begin` / `cb_on_job_end`) fire **concurrently on
every worker thread** — a `parallel_for`'s jobs end in parallel. Concurrent
non-atomic read-modify-write loses updates → `jobs_ended == 7` for an 8-job
batch. Surfaced deterministically enough on **linux-gcc** (the
`parallel_for captures one sample per job` test, `tests/perf/test_jobs_adapter.cpp:111`)
to fail CI; rarer on the single-job test because only one callback ran at a time.

**Discriminator that proved it was the counter, not an ordering race:** the
same test's `count_job_samples() == 8` (samples written inside `on_job_end`
via `pop_region`) **passed** while `jobs_ended == 8` failed — so all 8
`on_job_end` callbacks demonstrably ran; only the tally raced away.

**Fix:** internal `AtomicStats` struct of `std::atomic<crd::u64>`; callbacks
use `.fetch_add(1, std::memory_order_relaxed)` (relaxed is correct — pure
tallies, no happens-before with other reads); `jobs_adapter_stats()` snapshots
each atomic with `.load(relaxed)` into the public POD `JobsAdapterStats`
(unchanged public API); install/uninstall reset via per-field `.store(0)`.
Verified: 50× loop of `[adapter]` + ctest #295 green.

**How to apply:** A jobs-adapter (or any perf-counter) count assertion failing
is now a **real bug**, not a flake — root-cause it, do NOT retry-pass. The
general lesson: counters touched by multi-threaded callbacks must be atomic.
This is the [feedback_never_defer_solve](workflow-and-correctness.md#memory-feedback_never_defer_solve) case study — "known flake, retry"
hid genuine UB for days.

**Note on the separate ordering race (NOT fixed, did NOT fire here):** the
trampoline decrements the wait-counter (`worker_pool.cpp:158`) BEFORE
`on_job_end` fires (`:284`), so `wait()` can in principle return before the
last job's `on_job_end` runs. The CI failure was NOT this — the sample-count
discriminator above ruled it out. If a future test asserts an exact count of
something written in `on_job_end` *immediately after `wait()`* and flakes,
revisit with a localized fix (move `counter_decrement` to the scheduler,
post-`on_job_end`).


<!-- end-memory:feedback_perf_jobs_adapter_asan_flake -->

<a id="memory-feedback_plan_table_must_rebuild_at_every_frame_install_site"></a>
## feedback_plan_table_must_rebuild_at_every_frame_install_site

---
name: feedback_plan_table_must_rebuild_at_every_frame_install_site
description: "A plan-driven executor's per-pass plan table must be rebuilt at EVERY frame-install site, not just the obvious one; the flag-ON sweep of the REAL renderer finds the null-plan holes that execute_frame A/B tests miss."
metadata:
  node_type: memory
  type: feedback
  originSessionId: fade8ea4-87ca-470f-83e0-cdfe82a44e7f
  modified: 2026-08-12T02:49:02.246Z
---

CEIR-16b-3d-3 (fullscreen executor migration to `record_fullscreen_ceir`, a plan-driven replay): the per-pass CEIR
replay plans (`FramePlans`, keyed by pass name_hash) are built by `build_frame_plans` at frame LOAD. But a frame is
installed at **multiple** sites, and EACH must rebuild the table or the newly-installed frame's fullscreen passes get a
null plan → `record_fullscreen_ceir` → `ctx.fail()` → the whole frame record fails (or, per-pass, renders nothing).

Sites in scene_renderer.cpp (all must call `rebuild_frame_plans()`): `set_frame_graph_toml`, the default-root reparse,
the RAF-11 reload `frame_commit` (missed — hot-reload replayed the PREVIOUS frame's plan shape), and
`set_soft_shadows` (missed — the forward_csm ⇄ forward_csm_moment tier swap; the moment tier adds 3 fullscreen passes
convert/blur_x/blur_y that the prior table has no entry for → every moment shadow black). Resolved **fallback** frames
(forward_agx/forward_srgb carry a fullscreen `post` pass) also need their OWN plans — built in `resolve_frame_asset`,
index-parallel with the fallback-desc cache; `plans_for(authored)` resolves the desc ACTUALLY recorded (main OR
fallback) to the right table. And a FAILED plan build must be FATAL (`frame_ok=false`), not a WARN that silently
step-backs to a C++ path that no longer exists after deletion.

**Why:** the device A/B tests (16b-3d-1/2) ran via `execute_frame` and passed pixel-identical — but the LIVE renderer
records via `run_authored_cb`/AuthoredPass, a path `execute_frame` does not exercise (for_each expansion, layered
[$index] targets, tier swaps). The bug lived entirely in the install/rebuild plumbing that A/B never touched.

**How to apply:** when migrating any executor to a load-time-built plan table, grep EVERY assignment to the installed
desc (`frame =`, reparse, reload commit, tier/quality swaps) and confirm each rebuilds the table. Before deleting the
legacy path, run the **flag-ON sweep of the REAL renderer** (scene-render device tests + sandbox `--smoke-test 2` with
CRD_ASSETS_DIR) — it drives `run_authored_cb` over shipped frames and surfaces null-plan holes while the legacy net
still catches them. The same plan-rebuild discipline applies to [project_ceir_master_spine_locked](project-history.md#memory-project_ceir_master_spine_locked) 16d scene.raster.
Related: [feedback_new_execution_path_must_run_the_full_suite_on_a_real_device_not_just_a_cook_roundtrip](numerics-and-performance.md#memory-feedback_new_execution_path_must_run_the_full_suite_on_a_real_device_not_just_a_cook_roundtrip),
[feedback_widening_a_closed_enum_audit_every_consumer_not_just_total_switches](workflow-and-correctness.md#memory-feedback_widening_a_closed_enum_audit_every_consumer_not_just_total_switches).


<!-- end-memory:feedback_plan_table_must_rebuild_at_every_frame_install_site -->

<a id="memory-feedback_powershell_file_on_a_bat_is_a_silent_noop_use_cmd_c"></a>
## feedback_powershell_file_on_a_bat_is_a_silent_noop_use_cmd_c

---
name: feedback_powershell_file_on_a_bat_is_a_silent_noop_use_cmd_c
description: "`powershell -File script.bat` does NOT run a .bat — it errors \"does not have a .ps1 extension\" yet the OUTER powershell exits 0, so the build silently never runs and you gate a STALE binary"
metadata:
  node_type: memory
  type: feedback
  originSessionId: 192f4d9e-b3f7-485b-a775-5391bcd4183c
  modified: 2026-08-15T14:40:03.212Z
---

Running a `.bat` helper via `powershell -File scripts/build-target.bat <args>` is a SILENT NO-OP.
`-File` only accepts `.ps1`; with a `.bat` it prints "Processing -File '...bat' failed because the
file does not have a '.ps1' extension" — but the **outer powershell process still exits 0**, and the
background-task summary reports "completed (exit code 0)". So the target is NEVER rebuilt and you go on
to gate a STALE binary.

**Why it bit hard (CEIR-18p TAA, 2026-08-15):** every scene-render "rebuild" this session was a no-op.
My sgn-refactor "verification" (velocity gate 22/1 both backends) actually ran the OLD binary — it
passed only because the refactor is bit-identical. The one-shot `.ckir` emit "never fired" for three
runs because the emit code was never compiled in. Only when I finally read the build's OUTPUT FILE
(not the exit code) did I see the `-File` error.

**How to apply:**
- Run `.bat` helpers with `cmd /c "scripts\build-target.bat build/win-debug <target> 2>&1"` and CHECK
  `$LASTEXITCODE` (cmd's real exit), OR the PowerShell call operator `& .\scripts\build-target.bat ...`.
  NEVER `powershell -File *.bat`. (`.ps1` helpers like `tidy-files.ps1` are fine with `-File`.)
- A clean `cmd /c` build prints `[N/M] Building CXX object ...` + `Linking CXX executable ...`. If you
  see NO compile/link lines, the build did not run — do not trust it.
- ⛔ VERIFY-THE-INSTRUMENT: a green gate proves nothing if the binary is stale. After ANY engine edit,
  confirm the rebuild actually compiled the changed .cpp (watch for its `Building CXX object` line)
  BEFORE trusting the gate. Exit code 0 from the wrapper is NOT proof the compiler ran.
- Harmless noise: `cmd /c` runs of build-target.bat emit two `'m' is not recognized ...` lines before
  the build; the build still compiles + links correctly. Related: [reference_bat_helpers_need_powershell_tool_not_bash](build-and-verification.md#memory-reference_bat_helpers_need_powershell_tool_not_bash).


<!-- end-memory:feedback_powershell_file_on_a_bat_is_a_silent_noop_use_cmd_c -->

<a id="memory-feedback_powershell_select_first_kills_native_exe_exit_code"></a>
## feedback_powershell_select_first_kills_native_exe_exit_code

---
name: feedback_powershell_select_first_kills_native_exe_exit_code
description: "PowerShell `native.exe | ... | Select-Object -First N` closes the pipe early → kills the exe mid-teardown → $LASTEXITCODE reads 255/-1 even when the program passed; use -Last / collect-then-filter / Out-Null to read the true exit code."
metadata:
  node_type: memory
  type: feedback
  originSessionId: 192f4d9e-b3f7-485b-a775-5391bcd4183c
  modified: 2026-08-15T05:11:57.748Z
---

⛔ In PowerShell, piping a native executable through `Select-Object -First N` (or any cmdlet that stops the pipeline
early, e.g. `... | select -First 1`) sends a `StopUpstreamCommandsException` that **terminates the upstream native
process** as soon as N objects arrive. The exe is killed mid-run (often during device/GPU teardown), so `$LASTEXITCODE`
becomes **255 / -1** — a PHANTOM failure — **even though the program's own assertions all passed** and it would have
exited 0 if left to finish.

**Scar (CEIR-18e, 2026-08-15).** A Catch2 GPU test binary (`crd-scene-render-tests`) printed `All tests passed` then
showed `exit=255` whenever I filtered its output with `... | Select-String X | Select-Object -First 1`. I nearly
misdiagnosed it as a Vulkan+DX12 device-teardown crash and started hunting a non-existent bug. The tell: the SAME
invocation piped to `| Out-Null` or `| Select-String ... | Select-Object -Last N` (both consume the whole stream) exited
**0**, and the FULL suite + `ctest` (which run the exe to completion) exited 0. There was no crash — `-First` was killing
the exe.

**How to apply:** to read a native exe's TRUE exit code in PowerShell, never let a `-First`/early-stopping cmdlet sit
downstream of it. Instead: (1) `$out = & exe args 2>&1; $LASTEXITCODE` then filter `$out` afterwards (collect-then-
filter — the exe finishes first); or (2) `& exe args 2>&1 | Out-Null; $LASTEXITCODE`; or (3) `| Select-Object -Last N`
(must consume everything). `ctest`/`catch_discover_tests` run each test to completion, so a `-First`-induced 255 in a
manual harness does NOT mean the gate will fail — verify the real exit before treating a 255 as a device/teardown crash.
Relates to [feedback_timeout_is_not_a_hang_proof](workflow-and-correctness.md#memory-feedback_timeout_is_not_a_hang_proof) and [feedback_bat_helpers_need_powershell_tool_not_bash](build-and-verification.md#memory-reference_bat_helpers_need_powershell_tool_not_bash).


<!-- end-memory:feedback_powershell_select_first_kills_native_exe_exit_code -->

<a id="memory-feedback_project_is_hand_formatted_never_run_clang_format"></a>
## feedback_project_is_hand_formatted_never_run_clang_format

---
name: feedback_project_is_hand_formatted_never_run_clang_format
description: "The cerid repo is HAND-formatted, NOT clang-format-clean — running clang-format -i reflows ~2000 lines on a pristine file (strips declaration alignment, expands single-line blocks). Never run it to \"fix\" formatting; there is no CI format gate."
metadata:
  node_type: memory
  type: feedback
  originSessionId: 192f4d9e-b3f7-485b-a775-5391bcd4183c
  modified: 2026-08-17T14:01:56.726Z
---

The cerid codebase is **hand-formatted**. A `.clang-format` exists (ColumnLimit 120)
but the committed code does **not** conform to it: running `clang-format -i` (even the
repo style, LLVM-20 — the pinned tidy version) on a **pristine committed file** churns
**~2000 lines** (e.g. context.cpp: 1984+/876-). clang-format strips the project's
consecutive-declaration alignment (`bool         reads    = false;` → `bool reads = false;`)
and expands single-line blocks (`if (x) { return true; }` → 4 lines). There is no CI
clang-format gate (the code would fail it repo-wide).

**Why:** In the CEIR-20b malloc sweep I ran `clang-format -i` on ~119 files to fix the
declaration-alignment drift a token-width-changing sweep (MallocAllocator ->
GrowableTlsfAllocator, 15→21 chars) introduced. It reflowed every file — a 20k-line
whitespace explosion mixed into the semantic change.

**How to apply:**
- **NEVER** run `clang-format -i` on repo files to tidy formatting. Match surrounding
  hand-style by hand. For a mechanical sweep that shifts alignment columns, either leave
  the minor misalignment (build/tidy/checks don't care — alignment is not a tidy check)
  or hand-fix the touched lines only.
- The pristine-file test must run the file **in place under the repo** so clang-format
  finds the repo `.clang-format` — a file copied to /tmp gets LLVM *default* style and a
  meaningless diff.
- **Recovery if you already ran it:** back up all modified+untracked files; `git checkout
  HEAD -- <the .cpp/.hpp/.h files>`; re-run the *semantic* sweep (scripted changes recover
  perfectly → minimal 1:1 diffs); then re-splice each hand-written addition from the backup
  into the pristine HEAD file (pre-existing code stays hand-formatted → additions-only
  diffs). New files (no HEAD) keep the backup. Verify additions-only via `git diff --numstat`.
- Related: [feedback_large_exact_code_moves_use_a_verified_script_not_a_giant_edit](workflow-and-correctness.md#memory-feedback_large_exact_code_moves_use_a_verified_script_not_a_giant_edit),
  [feedback_no_malloc_allocator_malloc: original reference absent; current rule](../../CODING.md).


<!-- end-memory:feedback_project_is_hand_formatted_never_run_clang_format -->

<a id="memory-feedback_raf_ren_bands_test_one_linux_one_windows_after_raf_close"></a>
## feedback_raf_ren_bands_test_one_linux_one_windows_after_raf_close

---
name: feedback_raf_ren_bands_test_one_linux_one_windows_after_raf_close
description: "After the RAF band (D-007) is finished, test the RAF and REN bands in just ONE Linux + ONE Windows config — not the full multi-config per-slice sweep. The user does not want to lose time to full sweeps every slice."
metadata:
  node_type: memory
  type: feedback
  originSessionId: bf0ab64f-0cb7-4b04-970c-78c4f58c02b9
  modified: 2026-08-05T09:35:00.157Z
---

User directive (2026-08-05): "after raf band is finished I just want RAF and REN bands to be tested
in one linux and one windows config" — and, in the same breath, "I don't want to lose time with
sweeps all the time."

**Why:** the full 5-config Windows per-slice sweep (`per-slice-check.ps1 -IncludeRelease`) plus the
18-config full sweep runs the WHOLE repo's ~5700 tests per config — for a RAF/REN rendering slice
that is minutes of grind (and, right now, it also keeps hitting unrelated pre-existing aborts). The
signal a rendering slice actually needs is: its own gates pass on BOTH GPU backends + one optimized
build + tidy, cross-checked once on Linux.

**How to apply (for RAF/REN-band slices, once RAF/D-007 closes):**
- Verify the slice's OWN gates (the `[raf..]`/`[ren..]`/`[scene-render]` tags) on **one Windows
  config** (win-debug is the default; add win-asan only when the slice touches allocation/lifetime,
  as RAF-11's deferred-release queue did) and **one Linux config** (a `linux-gcc-*` via
  `scripts/wsl-build.ps1`), rather than the full `per-slice-check.ps1` / `full-sweep.ps1` matrix.
- Keep LLVM-20 tidy per changed file (`scripts/tidy-files.ps1`) and the sandbox smoke on both GPU
  backends — those are cheap and catch the rendering-specific regressions.
- The full multi-config sweep stays the tool for toolchain-sensitive work (SIMD/LTCG/ABI) and for a
  BAND close, not every slice. Do not launch a long full sweep when a targeted per-config run gives
  the same signal — it wastes the user's time.

This narrows [feedback_per_slice_run_ctest](build-and-verification.md#memory-feedback_per_slice_run_ctest) / [feedback_full_sweep_required](build-and-verification.md#memory-feedback_full_sweep_required) for the RAF/REN
rendering bands specifically; the broad per-slice/full-sweep discipline still holds for engine-core,
hesap, and toolchain-sensitive slices.


<!-- end-memory:feedback_raf_ren_bands_test_one_linux_one_windows_after_raf_close -->

<a id="memory-feedback_reflect_needs_unoptimized_spirv_and_compiler_injection"></a>
## feedback_reflect_needs_unoptimized_spirv_and_compiler_injection

---
name: feedback_reflect_needs_unoptimized_spirv_and_compiler_injection
description: SPIR-V you plan to REFLECT must be compiled unoptimized (level_zero); performance passes strip OpNames + dead bindings
metadata:
  node_type: memory
  type: feedback
  originSessionId: 9b330af5-88bb-499e-a92a-1752e160e0ba
---

**Scar (D-008 C2-e):** `crd::gpu::compile_glsl_to_spirv` defaults to shaderc PERFORMANCE opt passes (the ~30× GLSL-vs-CUDA
GEMM fix). But the crd-shader Effect frontend REFLECTS its SPIR-V via spirv-reflect to recover descriptor-binding names,
push-constant names, vertex-attribute names, and unused bindings. Performance passes STRIP `OpName` decorations and
dead-code-eliminate unused bindings → reflection returns empty/renamed metadata and the Effect's parameter/binding
tables come out wrong. The original inline shaderc used `optimization_level_zero` for exactly this reason.

**Rule:** any "compile GLSL → reflect the SPIR-V" path must compile UNOPTIMIZED. `compile_glsl_to_spirv(..., optimize=false)`
leaves shaderc at level_zero so `OpName`s + dead bindings survive. Runnable kernels/pipelines keep `optimize=true`.
**Smell:** reflection metadata (names, counts) that's empty or off-by-N after a compiler change → check the opt level.

**Companion pattern (ADR-0103 I1 — the language seam):** `crd-shader` names NO shading language. The Effect Runtime takes
an injected `crd::shader::ISpirvCompiler`; the bridge module **`crd-shader-vulkan`** (`create_vulkan_spirv_compiler`)
implements it over `compile_glsl_to_spirv`, linking BOTH crd-shader + crd-gpu-context-vulkan so **neither depends on the
other** — that's what keeps gpu-context rhi-free (compute/rendering decoupling). The injected compiler must OUTLIVE the
Runtime (borrowed): declare the compiler member/local BEFORE the runtime, or hold it in a process-lifetime static in
tests. See [project_gpu_context_owns_every_gpu_program](project-history.md#memory-project_gpu_context_owns_every_gpu_program), [project_shader_entry_point_normalize_to_main](project-history.md#memory-project_shader_entry_point_normalize_to_main),
[project_compute_rendering_separation](project-history.md#memory-project_compute_rendering_separation), [feedback_oracle_must_round_every_elementary_op](numerics-and-performance.md#memory-feedback_oracle_must_round_every_elementary_op).


<!-- end-memory:feedback_reflect_needs_unoptimized_spirv_and_compiler_injection -->

<a id="memory-feedback_run_only_the_tests_you_added"></a>
## feedback_run_only_the_tests_you_added

---
name: feedback_run_only_the_tests_you_added
description: "Run ONLY the specific tests just added; a full-suite sweep is at most ONE run at the end, never a per-iteration habit"
metadata:
  node_type: memory
  type: feedback
  originSessionId: a3482f73-d858-400b-816d-942216e20052
  modified: 2026-08-11T06:57:08.437Z
---

While iterating, run **only the test(s) you just added** (`<binary> "REN-38-A16 GATE*"`). If you suspect a
regression, run the full suite **once** — not after every edit.

**Why:** the user watched me re-run whole GPU suites repeatedly while debugging one gate, and called it out
sharply ("WHY YOU ARE RE RUNNING ALL THE TESTS ALL THE TIME"). It is minutes of wall-clock per run and it buries
the one failure that matters in thousands of passing assertions.

**How to apply:**
- Filter by the exact TEST_CASE name, not a prefix that also matches the cook-time twin.
- ⛔ CONFIRM a NEW test file actually COMPILED + RAN before trusting a green — check `<binary> --list-tests | grep <name>`
  or the assertion-count delta, NOT just that a `[tag]` filter passed. `tests/ceir/CMakeLists.txt` lists sources
  EXPLICITLY (not a glob), so a new `test_<dialect>.cpp` is silently NOT compiled until you add it to that list — and a
  `[frame]`-style tag run FALSE-GREENS on the opgen-GENERATED `..._gen_smoke` tests (which self-register from the dialect),
  so "3 cases passed" looked like a real gate while the real verifier tests never built (CEIR-15a). Every future CEIR
  dialect test (15b, 16, …) has this trap: add the file to the CMakeLists, then verify `--list-tests` shows YOUR case.
- ⛔⛔ CONSUMER-EDITION (a shared STATIC LIB change): when you edit `crd-ceir` (a `.lib`/`.a`) and rebuild ITS OWN test
  binary (`crd-ceir-tests`), a SEPARATELY-linked CONSUMER binary (`crd-frame-cook-tests`) still holds the OLD lib — its
  passing tests prove NOTHING about your change until YOU rebuild that consumer (a relink pulls the new lib). And a
  by-NAME filter silently drops coverage: filtering `'15a'` EXCLUDED `'ceir 15c'` — the exact guard-3 polarity CANARY the
  change had to keep green (CEIR-15c-1a; the advisor caught the stale-consumer-exe + filter-excludes-canary trap). A
  logical trace of "the canary would stay None" is NOT a run. How to apply: after a shared-lib edit, rebuild EVERY
  consumer test binary that links it AND run a filter that INCLUDES the polarity canary (a positive that must stay clean),
  not just the negatives you added; then a `frame`-wide blast-radius sweep since the symbol is now consumed in >1 module.
- When a filtered run produces no output and the binary is still resident, it is **HUNG, not slow** — say so and
  debug it, never paper over it with a polling loop. (An unsignalled `VkFence` waited with `UINT64_MAX` did
  exactly this; see [feedback_timeout_is_not_a_hang_proof](workflow-and-correctness.md#memory-feedback_timeout_is_not_a_hang_proof).)
- Narrate what a long-running command is actually doing. A silent poll loop reads as a black box.
- `test_vulkan_frame_graph.cpp` is now ~4k lines, so every edit recompiles a large TU — one more reason not to
  rebuild-and-run-everything per iteration.

Related: [feedback_iterate_local_test_only](build-and-verification.md#memory-feedback_iterate_local_test_only), [feedback_local_test_only_ci_owns_sweep](build-and-verification.md#memory-feedback_local_test_only_ci_owns_sweep),
[feedback_build_grep_hides_link_errors](build-and-verification.md#memory-feedback_build_grep_hides_link_errors) (grep for `': error'` also hides `LNK1168`; check the exit code).


<!-- end-memory:feedback_run_only_the_tests_you_added -->

<a id="memory-feedback_run_tidy_per_slice_never_accumulate"></a>
## feedback_run_tidy_per_slice_never_accumulate

---
name: feedback_run_tidy_per_slice_never_accumulate
description: "Run clang-tidy on each new file DURING the slice (scripts/tidy-files.ps1), never accumulate; win-tidy-local breaks when the VS-bundled CMake rewrites CMAKE_COMMAND."
metadata:
  node_type: memory
  type: feedback
  originSessionId: 934ee96e-34fa-4239-87ad-44921a7d5a19
---

⛔ **Run the clang-tidy gate INCREMENTALLY, per file, as part of testing EACH slice — never let it accumulate.**

**Why:** 2026-07-07, closing v16-h, I discovered the `win-tidy-local` gate had silently broken and the ENTIRE
autodiff cluster (v16-c…h, ~11 test files) had been written UNGATED, accumulating **200+ tidy violations**
(`readability-isolate-declaration` = multi-declarations, `readability-identifier-naming` = uppercase math names like
`T`/`N`/`Q`/`A`/`M` that collide with `Tape`/`Newton`/`WithinAbs`/`Matchers`). Fixing 200 at once is miserable and
risky; fixing 1-2 per file as you write them is trivial.

**How the gate broke (the root cause):** `build/win-tidy-local/CMakeCache.txt`'s `CMAKE_COMMAND` got rewritten to the
**VS-bundled** CMake (`…/Microsoft Visual Studio/18/…/CMake/CMake/bin/cmake.exe`) — the VS fork self-rearms this on any
regenerate and, on this Turkish-locale host, also re-arms the ninja `#deps 0` landmine (docs/BUILDING.md). Its ABI
test then FALSELY fails (`link.exe` exits 0 but CMake flags failure). **Fix:** wipe the dir + reconfigure with the
STANDALONE cmake via `scripts/configure-preset.bat win-tidy-local` (bakes in `C:/Program Files/CMake/bin/cmake.exe`).
Audit: `grep CMAKE_COMMAND build/<dir>/CMakeCache.txt` must be the standalone path.

**How to apply:**
- After adding/editing any test or header, run `powershell -File scripts/tidy-files.ps1 <touched .cpp/.hpp>` (pinned
  LLVM-20.1.8, `--warnings-as-errors=*`, CI-faithful) right beside the module test run; fix before moving on.
- Pass BOTH the `.cpp` and any new `.hpp` (each checked as its own TU; no `--header-filter` = no transitive noise).
- `clang-tidy --fix --checks='-*,readability-isolate-declaration'` safely auto-splits multi-declarations (bulk win);
  do NAMING renames by hand ([feedback_clang_tidy_ci_local_version_skew](build-and-verification.md#memory-feedback_clang_tidy_ci_local_version_skew) / the v11 scar: `--fix` naming CORRUPTS).
- For deliberate math notation (CNN tests: X/W/Z/A/P, conv weights), `// NOLINTBEGIN(readability-identifier-naming)` …
  `// NOLINTEND` is the SANCTIONED fix (precedent: tests/hesap-wavelet/test_dwt.cpp) — not a cop-out.
- If the tidy gate ever passes trivially or errors on configure, VERIFY it is actually running — a broken gate is a
  DoD failure. Codified in AGENTS.md §"Definition of Done" item 2.

Related: [feedback_clang_tidy_after_every_slice](build-and-verification.md#memory-feedback_clang_tidy_after_every_slice), [feedback_clang_tidy_must_be_llvm_20_not_22](build-and-verification.md#memory-feedback_clang_tidy_must_be_llvm_20_not_22), [build_system](build-and-verification.md#memory-build_system).


<!-- end-memory:feedback_run_tidy_per_slice_never_accumulate -->

<a id="memory-feedback_search_engine_before_building"></a>
## feedback_search_engine_before_building

---
name: feedback_search_engine_before_building
description: "STANDING RULE — before building any solver/kernel/utility, FIRST grep the engine for an existing one; reuse > reimplement"
metadata:
  node_type: memory
  type: feedback
  originSessionId: dbe6c571-c4b1-4769-8980-fa6818c253e7
---

**Before implementing ANY solver, kernel, or utility, FIRST check whether the engine already has it.** Reuse > reimplement. The user made this a hard standing rule after catching redundancy in v12.

**Why:** v12 reimplemented things the engine already had because I didn't grep first:
- **erf/erfc/lgamma** — already in `crd::math::deterministic` (Cephes, f32+f64); hesap-special even *links crd-math*. Reimplemented in hesap-special.
- **f64 SIMD log/exp** — MISPLACED into `hesap-special/detail/simd_{log,exp}.hpp`; crd-math is their home (it already has f32 `log/exp(Vec4f/Vec8f)` + a `Vec4d` type). A reusable SIMD math primitive belongs in crd-math, not a consumer's `detail/`.
- **tridiagonal QL** — was about to be hand-written for Golub-Welsch when hesap-dense already ships gold-standard Sturm/dqds/**MRRR** eigensolvers (`eig_sym`, `eig_sym_mrrr`, `detail/tridiag_eigenvalues`, `mrrr_single_rrr_vectors`). User caught it: "we have eigensolvers, why don't you use them?"
- (Bessel I0 also exists in hesap-dsp `bessel_i0` for the Kaiser window — minor.)

**How to apply:**
1. BEFORE writing the first line, `grep engine/*/include` for the capability AND its synonyms (`eig`/`eigen`/`tridiag`/`steqr`; `log`/`exp`/`Vec4d`; `bessel`/`i0`; `gamma`/`erf`; `fft`; `rng`…).
2. If it exists → reuse. If close-but-not-quite → extend it **in its home module**. Only build new when nothing fits — and say so explicitly.
3. New reusable code lands in the module that OWNS that capability (SIMD math primitive → crd-math). Don't bury a reusable thing in a consumer's `detail/`.
4. Cross-module dependency worry? confirm acyclicity (does the provider depend on you?) and prefer the edge (or a shared module) over a duplicate. A self-contained reimplementation is justified ONLY after the search comes up empty.

Written into `docs/SANITY.md` rule 8 + `CLAUDE.md` Hard rules (first bullet) so no agent forgets. [reference_sanity_doctrine](workflow-and-correctness.md#memory-reference_sanity_doctrine)

RESOLUTION 2026-06-23: (1) f64 SIMD log/exp MOVED to `crd/math/simd/transcendental.hpp` (namespace `crd::math`, names crd_log1/crd_log4/crd_exp1/crd_exp4); hesap-special's simd_lanczos.hpp consumes via `using crd::math::...`; dead Stirling `simd_lgamma.hpp` deleted; suite 401837/30 GREEN. (2) erf/erfc/lgamma overlap = LEGITIMATE, kept: crd-math's are the deterministic-FP-contract versions (eylem), hesap-special's are the gold family coherent around `calerf` + erfcx/erfinv; crd-math CAN'T reuse hesap-special's (cycle). (3) Golub-Welsch → uses hesap-dense eigensolver in a NEW `crd-hesap-quadrature` module (keeps hesap-special a leaf). The test for the SIMD primitive (test_simd_log.cpp) still lives in the hesap-special binary — could move to tests/math (minor).


<!-- end-memory:feedback_search_engine_before_building -->

<a id="memory-feedback_semantics_preserving_pass_differential_test_is_bit_exact_vs_unoptimized_program"></a>
## feedback_semantics_preserving_pass_differential_test_is_bit_exact_vs_unoptimized_program

---
name: feedback_semantics_preserving_pass_differential_test_is_bit_exact_vs_unoptimized_program
description: "A semantics-preserving pass (DCE/CSE/canonicalize/...) is differential-tested BIT-EXACT vs the unoptimized program's own device output, never tolerance-vs-oracle"
metadata:
  node_type: memory
  type: feedback
  originSessionId: cb9df3b8-2389-479b-9d99-d3d6ce3ba327
  modified: 2026-09-04T14:08:56.166Z
---

For a SEMANTICS-PRESERVING compiler pass (DCE, CSE, canonicalize/fold, specialize, ...) the differential
test's reference is **the unoptimized program's OWN output on the SAME device, BIT-EXACT** — NOT a
tolerance vs an analytic/hesap oracle.

**Why:** a tolerance compare (`abs(opt - ref) < 1e-5` vs hesap) would PASS a pass that perturbed a value
by 1e-5 — it cannot catch a subtle miscompile. `opt(P) == raw(P)` bit-exact, same device / same kernels /
same inputs, proves the pass changed nothing observable. The oracle compare still has a place (it proves
the RAW program correct), but it is not the pass's differential test.

**How to apply:** run the raw program, capture its device output; run the pass; re-plan + re-execute the
SAME corpus / device / inputs; `CHECK(opt[i] == raw[i])` EXACT (f32 `==` is a tidy-clean established
pattern here for deterministic GPU output). Also assert a structural identity that proves the pass ACTED
(e.g. `plan.stages.size()` dropped), so a no-op pass can't pass silently. If bit-exact FAILS on a corpus
with no cross-thread reduction (sequential dot, elementwise, copy-transpose), that is a device-determinism
FINDING, not a tolerance to loosen. Proven device-INDEPENDENT (real RTX 4070 Ti + llvmpipe) at CEIR-26a-3.

This is the CEIR-26 standard for 26b–26f, established at 26a-3 (DCE). Pin readback outputs BEFORE the pass
or it deletes them: [feedback_plan_output_by_traversal_is_not_ssa_liveness_pin_readback_before_any_pass](execution-ir.md#memory-feedback_plan_output_by_traversal_is_not_ssa_liveness_pin_readback_before_any_pass).


<!-- end-memory:feedback_semantics_preserving_pass_differential_test_is_bit_exact_vs_unoptimized_program -->

<a id="memory-feedback_shared_test_helper_inherits_first_callers_fixed_buffers"></a>
## feedback_shared_test_helper_inherits_first_callers_fixed_buffers

---
name: feedback_shared_test_helper_inherits_first_callers_fixed_buffers
description: "When hoisting a test helper to a shared header, guard or template its fixed-size stack buffers — they carry the first caller's assumption"
metadata:
  node_type: memory
  type: feedback
  originSessionId: cb9df3b8-2389-479b-9d99-d3d6ce3ba327
  modified: 2026-09-04T11:43:59.537Z
---

When a test helper is HOISTED from a single-caller TU into a shared header (e.g. tests/gpu-shared/), any fixed-size stack
buffer sized for that first caller's corpus becomes a SILENT trap advertised as reusable. Example (2026-09-04, CEIR-25c-2b):
`MlpLoss` FD functor had `double z1[64]` etc. sized for the 25c-2 corpus (m*d1 == 32); once shared+named-reusable, a caller
with `m*d1 > 64` overflows the stack silently — the baked-32 scar one level up ([feedback_ckir_fft_batched_radix_dispatch_breaks_fixed_twiddle_contract](device-programs.md#memory-feedback_ckir_fft_batched_radix_dispatch_breaks_fixed_twiddle_contract) family).

**Why:** a "temporary" fixed size is invisible-but-correct in its origin TU; hoisting changes the contract from "used once,
here" to "reusable, anywhere" without changing the buffer.

**How to apply:** at hoist time, GUARD or template the fixed size. A runtime cap can't `static_assert`, so make it
deliberately-wrong-and-loud: `static constexpr int kCap = 64; if (m*d1 > kCap || m*d2 > kCap) return 0.0;` (the FD gate then
fails HARD, never a silent overflow — the "typed reject, never silent" doctrine [feedback_ceir_deserialize_build_raw_graceful_reject_never_factory_assert](build-and-verification.md#memory-feedback_ceir_deserialize_build_raw_graceful_reject_never_factory_assert)),
and name the cap in the header comment. Or template the bound (`MlpLoss<Cap=64>`) so a bigger corpus opts in.


<!-- end-memory:feedback_shared_test_helper_inherits_first_callers_fixed_buffers -->

<a id="memory-feedback_stale_toolset_path_in_build_dir_wipe_dont_sed"></a>
## feedback_stale_toolset_path_in_build_dir_wipe_dont_sed

---
name: feedback_stale_toolset_path_in_build_dir_wipe_dont_sed
description: "A cross-config build dir can hardcode an OLD MSVC toolset ml64 path; reconfigure won't refresh it — wipe, never sed"
metadata:
  node_type: memory
  type: feedback
  originSessionId: 9b330af5-88bb-499e-a92a-1752e160e0ba
---

**Symptom:** a secondary build dir (e.g. `build/win-clang-cl`) fails at `crd-jobs`'s
`fiber_switch_win64.asm` with `ml64.exe ... CreateProcess failed: The system cannot find the file
specified`, pointing at an MSVC toolset version (e.g. `14.50.35717`) that no longer exists — while
`build/win-debug` (same vcvars) builds fine because its cache has the CURRENT toolset (`14.51.36231`).

**Root cause:** MSVC toolset was updated. CMake caches the detected `CMAKE_ASM_MASM_COMPILER` path in
`CMakeFiles/<ver>/CMakeASM_MASMCompiler.cmake` (+ `CMakeCache.txt` + `rules.ninja`). Re-running
`cmake --preset` does **NOT** refresh an already-cached compiler path — the stale ml64 path persists.

**How to apply:**
- Fix = **wipe** the dir and reconfigure fresh: `rm -rf build/win-clang-cl && cmake --preset win-clang-cl`.
  Fresh detection picks up the current toolset. `git status` unaffected (build dirs are gitignored).
- **NEVER `sed` a build dir to swap the version string** — the same string is embedded in `.obj`/`.lib`/
  `.pch`/`.ninja_deps` binaries; even a same-length replace mangles artifacts and ninja's dep DB (2026-07-11
  scar). Wipe is the only safe fix.
- This is ENVIRONMENTAL, unrelated to any code slice; it blocks `crd-jobs` (untouched) before reaching your
  code. Don't chase it as a code defect. Related: [reference_build_test_workflow](build-and-verification.md#memory-reference_build_test_workflow), [build_system](build-and-verification.md#memory-build_system),
  [feedback_per_slice_binary_direct_misses_ctest_and_crossconfig](build-and-verification.md#memory-feedback_per_slice_binary_direct_misses_ctest_and_crossconfig).


<!-- end-memory:feedback_stale_toolset_path_in_build_dir_wipe_dont_sed -->

<a id="memory-feedback_targeted_fix_skip_resweep"></a>
## feedback_targeted_fix_skip_resweep

---
name: skip-re-sweep-when-a-targeted-fix-is-locally-verified-on-the-failing-config
description: "When the full-sweep is 17/18 PASS, the remaining failure has been root-caused + locally verified on the failing config, and the fix is small/contained, deferring the full re-sweep to CI is acceptable"
metadata:
  node_type: memory
  type: feedback
  originSessionId: 307daaf4-04ca-4f85-b2f3-6606266d6f97
---

When `scripts/full-sweep.ps1` returns N-1/N PASS and the single
remaining failure has been:

1. **Root-caused** to a specific identified bug, AND
2. **Locally fixed** in code, AND
3. **Verified by running the failing preset alone** end-to-end
   (`scripts/wsl-build.ps1 -Preset <X>` for Linux, or
   `cmake --build --preset <X>` + `ctest --preset <X>` for Windows),

THEN re-running the full 25-minute sweep is **not** required to close
the slice. CI will catch any cross-config regression the local fix
might have introduced.

**Why:** User directive 2026-05-19 during the v9-close re-sweep when
the targeted u64-cast fix on `tests/geometry-bvh-gpu/test_morton_sort.cpp`
was verified locally on `linux-gcc-relwithdebinfo` (the only failing
config). Quote: "I think we don't need to restart the sweep nothings
gonna break in my opinion, if breaks I wanna see that in CI and then
we take action, we should move on on my command".

The re-sweep would have been ~25 min of waiting for a known-targeted
fix. The user prefers shipping fast + letting CI act as the safety net
for the residual cross-config risk over paying that wait cost.

**How to apply:**

- Always run the full sweep on the FIRST attempt of slice close — that
  surfaces the failures.
- When the sweep returns N-1/N PASS, **don't auto-trigger** a re-sweep
  after fixing the one failing config. Instead:
  1. Verify the fix locally on the failing preset alone.
  2. Report status + ask the user if they want a full re-sweep or to
     move on with CI as the safety net.
- This **refines** but does NOT revoke [full_sweep_required](build-and-verification.md#memory-feedback_full_sweep_required) — the
  first full-sweep is still mandatory; only the *re-sweep after a
  targeted fix* is deferrable.
- Does NOT apply when:
  - Multiple configs failed (need to re-verify cross-config consistency).
  - The fix touches a shared header / cross-cutting interface (then
    every config is at risk again).
  - This is a phase close (`.x → .y`) — phase closes need a clean
    pre-close sweep, no exceptions.

Links: [full_sweep_required](build-and-verification.md#memory-feedback_full_sweep_required) (the strict version this refines).


<!-- end-memory:feedback_targeted_fix_skip_resweep -->

<a id="memory-feedback_test_eigensolvers_on_random_not_smooth"></a>
## feedback_test_eigensolvers_on_random_not_smooth

---
name: feedback_test_eigensolvers_on_random_not_smooth
description: "Eigensolver/Schur unit tests must use GENERIC random matrices, not smooth sin/cos — smooth spectra deflate without exercising the hard paths"
metadata:
  node_type: memory
  type: feedback
  originSessionId: 178961b8-0463-4615-82ce-96bde42457f0
---

When unit-testing eigensolvers / Schur / deflation kernels, the test matrix corpus
MUST include generic **random** (PRNG) matrices, not just smooth analytic ones
(`sin(i*..)`/`cos(i*..)`). Smooth-spectrum matrices converge/deflate in ways that
**skip the hard code paths**, so a kernel can pass every smooth-matrix test at
~1e-13 and still be badly wrong (recon ~1) on a generic matrix.

**Why:** smooth analytic matrices tend to have clustered/structured spectra that
either deflate trivially or not at all — they don't hit the *partial-deflation*
branch, long bulge-chases, or the spike-reflection path. A random matrix exercises
all of them.

**How to apply:** every Schur/eig gate gets at least one PRNG-random-matrix case
(use the LCG `s = s*1664525u + 1013904223u` pattern the benches use), AND a
head-to-head recon vs a known-good reference path (e.g. AED vs single-shift
`complex_schur`) on that random matrix. If a smooth test passes but a random/bench
matrix fails, the bug is in a path the smooth spectrum didn't reach.

**Case studies (this has now bitten 3×):** v3d-2c-2b-3 complex AED — `complex_aed_deflate`
spike-reflection path (zlaqr2 spike-row conjugation) was wrong but only ran on
PARTIAL deflation, which the small smooth-matrix 2b-2 tests never triggered; the
bench's random matrices exposed it (recon ~1) and the fix was one line
(`work[k] = conj(v.at(0,k))`). The 2b-2 "4-config DoD green" was a false-positive
because its tests didn't cover partial deflation. See
[feedback_when_advisor_asks_verify_recheck_on_failure](workflow-and-correctness.md#memory-feedback_when_advisor_asks_verify_recheck_on_failure) and
[feedback_always_bench_both_eigen_and_lapack](numerics-and-performance.md#memory-feedback_always_bench_both_eigen_and_lapack).


<!-- end-memory:feedback_test_eigensolvers_on_random_not_smooth -->

<a id="memory-feedback_tidy_gate_clean_on_unparsed_files"></a>
## feedback_tidy_gate_clean_on_unparsed_files

---
name: feedback_tidy_gate_clean_on_unparsed_files
description: "scripts/tidy-files.ps1 reported \"clean\" for files it never PARSED (missing -I ⇒ include not found ⇒ 0 checks ran ⇒ looked identical to clean); hard-fail on unresolved includes."
metadata:
  node_type: memory
  type: feedback
  originSessionId: 9b330af5-88bb-499e-a92a-1752e160e0ba
---

⛔⛔ **A gate that cannot parse a file must FAIL, never report "clean."** `scripts/tidy-files.ps1` hand-maintained its
`-I` list, which omitted whole modules (`engine/kir/include` was never in it). So `#include <crd/kir/ckir.hpp>` failed →
the TU never parsed → **zero check diagnostics** → and the script *filtered out* `"file not found"` lines, so a blind
file was byte-identical in output to a genuinely clean one. `engine/kir-vulkan/src/backend_vulkan.cpp` passed the DoD
gate without a single line ever being analysed. **89 real violations across `crd-kir` were invisible** (eval 17 · glsl 35
· hlsl 34 · cuda 3), plus 1 each in the vulkan/dx12 backends.

**Why:** this is SANITY #2 (verify the *shipped* artifact, not a green you remember) compounded with SANITY #4 (know
what your diagnostic CAN'T see). A clean run from a tool that never looked at the code proves nothing — and it is worse
than a red, because it actively certifies the opposite. It is the same shape as the `#deps 0` ninja scar and the
`win-tidy-local` `CMAKE_COMMAND` rewrite: **the instrument broke silently and kept printing PASS.** AGENTS.md DoD §2
already warned "if the tidy gate ever appears to pass trivially, VERIFY it is actually running" — this is that scar's
second face, and the warning wasn't enough because nothing *enforced* it.

**The gate had THREE silent-pass holes, not one** (each found by tripping it, not by reading it):
1. unresolved include ⇒ TU never parsed ⇒ 0 diagnostics ⇒ printed `clean`;
2. `.cpp` with no compile-DB entry (Metal/HIP targets aren't configured on Windows) ⇒ bare fallback command ⇒ same;
3. a path that doesn't exist ⇒ `SKIP (not found)` ⇒ **exit 0**. A splatting bug that collapsed 27 paths into one
   string made the whole sweep print "All passed" while gating nothing.

**How to apply:**
- Fixed (2026-07-09/10, D-007 B0): the script now (1) treats ANY unresolved include as **`UNGATED`** — a loud, non-zero
  exit, never folded into "clean"; (2) derives the header `-I` set by **globbing `engine/*/include`** so a new module
  can never silently fall out of the gate; (3) drives `.cpp` files from the real **compile database** (`-p`), mirrored
  to a scratch copy with MSVC's PCH flags (`/Yu`, `/Fp`, `/FI…cmake_pch`) stripped — clang cannot read an MSVC `.pch`,
  and un-stripped they masquerade as `clang-diagnostic-error`; (4) falls back to synthesized flags for a `.cpp` absent
  from the DB; (5) treats a **MISSING** path as a failure, and prints the file COUNT it actually gated.
- **A gate must report what it DID, not only what it found.** "All passed" without a count is unfalsifiable.
- **Before trusting any "clean" from a gate, prove the gate can SEE the file.** Cheap check: run it once with a
  deliberate violation inserted, or compare diagnostic counts with/without the include path. A gate that yields the same
  output for parsed and unparsed input is not a gate.
- Generalize: any tool that filters its own error channel (`| Where-Object { $_ -notmatch "file not found" }`,
  `2>/dev/null`, `|| true`) can convert a hard failure into a silent pass. Audit the filter before trusting the verdict.

Related: [feedback_run_tidy_per_slice_never_accumulate](build-and-verification.md#memory-feedback_run_tidy_per_slice_never_accumulate) · [feedback_clang_tidy_must_be_llvm_20_not_22](build-and-verification.md#memory-feedback_clang_tidy_must_be_llvm_20_not_22) ·
[reference_sanity_doctrine](workflow-and-correctness.md#memory-reference_sanity_doctrine) · [feedback_source_must_match_honest_scoreboard](workflow-and-correctness.md#memory-feedback_source_must_match_honest_scoreboard)


<!-- end-memory:feedback_tidy_gate_clean_on_unparsed_files -->

<a id="memory-feedback_transient_aliaser_must_check_slot_size_and_borrowed_bundle_format"></a>
## feedback_transient_aliaser_must_check_slot_size_and_borrowed_bundle_format

---
name: feedback_transient_aliaser_must_check_slot_size_and_borrowed_bundle_format
description: "Two latent frame-graph defects the moment atlas exposed — the aliaser chose freed slots by lifetime alone (heap cannot grow), and the borrowed transient bundle carried no format (is_depth answered \"colour\" for the depth atlas)"
metadata:
  node_type: memory
  type: feedback
  originSessionId: dd844f83-0821-41ab-8607-fa93ccf37a76
  modified: 2026-07-31T18:25:10.140Z
---

Adding the REN-40-D moment atlas (three RGBA16F array transients beside the R32_TYPELESS depth atlas)
exposed two latent defects that had survived every earlier graph because every earlier graph was
accidentally immune:

1. **The transient aliaser chose freed slots by LIFETIME alone.** Both backends' `alias_class` picked
   the first slot with `free_after < first_pass` — no size check. A D3D12 heap (and a Vulkan buffer
   slot, which allocates at creation) cannot grow; the DX12 code even had an
   `else if (sz > slot.size) { slot.size = sz; }` branch whose own comment said "won't happen for
   equal-size transients" — it updated bookkeeping for a heap that stayed small. The first graph with
   UNEQUAL same-class transient sizes made `CreatePlacedResource` fail at offset 0 → **the whole frame
   graph refused to build** → `0 passes` → the canvas showed the PREVIOUS arm's pixels — which made a
   DX12 A/B gate read "moment == hard, pixel-identical" (the canvas-kept-previous-contents scar).
   **Fix: the chooser requires `slot.size >= sz` as well as free.** (Vulkan's IMAGE path was safe — it
   sizes slots to the max occupant before allocating; its BUFFER path had the hole.)
2. **The borrowed transient texture carried no format.** Vulkan's frame graph wraps a sampled transient
   in a borrowed `VulkanTexture{ImageBundle{image, view}}` — `format` stayed `VK_FORMAT_UNDEFINED`. When
   the atlas sampler became format-keyed (`is_depth()` → comparison vs LINEAR/CLAMP), the DEPTH atlas
   answered "colour" and was sampled through the non-comparison sampler: `tex_sample_cmp` then returns
   the RAW STORED DEPTH as "visibility" → **the whole frame at ~half brightness with a shallow shadow**
   — plausible pixels, wrong physics, invisible to every draws>0 check. Found by dumping a scanline AS
   NUMBERS and seeing the lit plateau had moved (the same instrument that cracked PCSS).
   ⛔ On DX12 `is_depth` must be an EXPLICIT construction flag — depth SRVs are `R32_FLOAT` over
   `R32_TYPELESS`, so the format cannot answer.

**Why:** both defects render *plausible* frames (previous contents / uniformly dimmer) — nothing
crashes, `draws > 0`, logs clean unless the executor's build-failure report is enabled. A gate that
compares two arms into the SAME target must fail loudly when an arm records nothing.

**How to apply:** any new transient with a new size/format combination exercises the aliaser — check the
chooser fits before blaming the new pass. When a shadow/atlas image suddenly renders "soft" or dimmer
everywhere, suspect the SAMPLER identity (comparison vs plain) before the filter maths, and dump a
scanline as numbers first. Test binaries do not init logging — the frame executor's named rejections
are invisible until you add a `ConsoleSink` in the failing test.

Related: [feedback_pcss_three_defects_unbound_sampler_ring_search_receiver_plane](workflow-and-correctness.md#memory-feedback_pcss_three_defects_unbound_sampler_ring_search_receiver_plane),
[feedback_a_perf_flag_that_can_measure_an_empty_frame_must_exit](rendering.md#memory-feedback_a_perf_flag_that_can_measure_an_empty_frame_must_exit),
[feedback_multi_pass_scene_draws_must_load_not_clear_smoke_is_pixel_blind](workflow-and-correctness.md#memory-feedback_multi_pass_scene_draws_must_load_not_clear_smoke_is_pixel_blind)


<!-- end-memory:feedback_transient_aliaser_must_check_slot_size_and_borrowed_bundle_format -->

<a id="memory-feedback_transient_clang_tidy_crash"></a>
## feedback_transient_clang_tidy_crash

---
name: feedback-transient-clang-tidy-crash
description: "Transient clang-tidy access-violation crashes (most often in `bugprone-reserved-identifier` matcher) on a single per-slice DoD run that build clean on retry are an upstream LLVM bug, not a code-side DoD failure — close on retry-PASS, do not re-sweep"
metadata:
  node_type: memory
  type: feedback
  originSessionId: b24674c3-970b-481c-a127-bf4231bceca3
---

When `scripts/per-slice-check.ps1` win-tidy build aborts with an
upstream clang-tidy stack trace ("PLEASE submit a bug report to
https://github.com/llvm/llvm-project/issues/" followed by a
`Stack dump` referencing an `ASTMatcher: Processing
'bugprone-reserved-identifier'` matcher and `Exception Code:
0xC0000005` / access violation), and the build resumes cleanly on
retry, this is an **upstream LLVM clang-tidy bug**, not a code-side
DoD failure.

**Why:** Direct evidence — same source files build clean on retry
(`cmake --build --preset win-tidy --target <target>` returns exit 0
with no errors and no warnings); the crash backtrace is entirely
inside `clang-tidy.exe` virtual address space; clang-tidy itself
acknowledges the crash as a defect that should be reported upstream.

**How to apply:**
1. On a single config FAIL in the per-slice sweep, check the failure
   mode. If it's a clang-tidy access violation (not a clang-tidy
   *warning-as-error* — those ARE code-side), retry `win-tidy`
   alone first: `cmake --build --preset win-tidy --target <slice-target>`.
2. If retry builds clean (exit 0, no diagnostics), re-run the
   `scripts/per-slice-check.ps1` full sweep. If full sweep is now
   PASS, **the slice is closed** — file the crash as a new
   `docs/debt.md` upstream-bug entry, do not re-sweep, do not chase
   ghost code-side bugs.
3. If retry crashes AGAIN, this is reproducible — bisect on the
   slice's new code to find the construct that crashes the matcher
   (often unusual template / `constexpr` / lambda nesting) and
   refactor to sidestep it. File the construct upstream too.

**Same policy class** as
[feedback_transient_msvc_ltcg_ice_accept](build-and-verification.md#memory-feedback_transient_msvc_ltcg_ice_accept) (MSVC LTCG ICE in
`link!DllGetObjHandler` during shipping config). Both are upstream
toolchain bugs that the project's DoD policy accepts on retry-PASS
rather than blocking the slice or chasing a non-existent code-side
defect.

**Don't:**
- Skip the win-tidy config to "work around it" — the rule is
  per-slice DoD = 4 configs, all green.
- Add the `bugprone-reserved-identifier` rule to a
  `.clang-tidy` disable list in the slice — the rule is fine; only
  the specific compile-time invocation crashed.
- Re-sweep the full 18 configs at v-close time as if the slice
  hasn't been verified — the per-slice 4-config PASS counts. The
  18-config cluster sweep happens at cluster close (v7-close in
  this case), which is independent verification.

**First instance**: 2026-05-17 v7d isotropic remeshing first DoD
attempt at win-tidy build; retry PASS in elapsed 03:13.


<!-- end-memory:feedback_transient_clang_tidy_crash -->

<a id="memory-feedback_transient_msvc_ltcg_ice_accept"></a>
## feedback_transient_msvc_ltcg_ice_accept

---
name: feedback-transient-msvc-ltcg-ice-accept
description: "When full-sweep fails only on a transient MSVC LTCG ICE that retries clean, do not re-run the full sweep — close the slice on retry-success evidence and file the ICE as new debt"
metadata:
  node_type: memory
  type: feedback
  originSessionId: 72f1d11d-1798-4b4b-9943-ba63ab54bb2e
---

When `scripts/full-sweep.ps1` fails on exactly one config with an **MSVC LTCG internal compiler error** (C1001, "İç derleyici hatası" / "Access violation" inside `link!DllGetObjHandler` during the `/LTCG` codegen phase), and a plain rebuild of that config's failed target succeeds without code changes, treat the slice as **PASS** for DoD purposes. Do not re-run the full sweep to chase the green log.

**Why:** A transient MSVC LTCG ICE is an upstream compiler bug, not a code defect. The retry-clean rebuild plus the matching `win-clang-cl-shipping` (same flags, different compiler) PASS prove the code is correct. Re-running the entire ~13-minute sweep just hoping the dice land differently is treadmill work — the user explicitly waved it off ("we don't need a full sweep now") when this came up paying v1 debts on 2026-05-13.

**How to apply:**
1. When the sweep summary shows `1 failed` and the failure is an MSVC ICE (C1001 with `link!DllGetObjHandler` in the stack trace) on a `*-shipping` LTCG config:
2. Re-run the build of just that failed target (`cmake --build --preset <cfg> --target <target>`). No CMake reconfigure, no code change.
3. If it links clean — slice closes. Cite the retry-success in the session log instead of a second sweep log.
4. Always file the ICE as a new entry in `docs/debt.md` ("MSVC LTCG transient ICE under shipping config — investigate if it recurs"). One incident is upstream noise; a pattern of recurrence is worth a workaround (`CRD_NOINLINE` on a suspect function, or splitting a TU).
5. If the second sweep would have been free (no human cost, plenty of time), the rule still applies — burning context window + ~13 min for a green log of evidence that already exists is not slice-closure work.

This is the SAME family as `feedback_full_sweep_required.md` — that rule says "sweep PASS required for DoD". This one carves out the specific exception: a transient compiler ICE that retries clean is not a code-side failure of the DoD, and the retry-clean rebuild is the verification.


<!-- end-memory:feedback_transient_msvc_ltcg_ice_accept -->

<a id="memory-feedback_vtable_stability_append_at_end"></a>
## feedback_vtable_stability_append_at_end

---
name: feedback-vtable-stability-append-at-end
description: "When extending a virtual interface (Device/Queue/CommandBuffer/any polymorphic base), ALWAYS append new pure-virtuals at the END of the class declaration. Inserting in the middle shifts every subsequent vtable slot and silently mis-dispatches downstream callers in win-release."
metadata:
  node_type: memory
  type: feedback
  originSessionId: b24674c3-970b-481c-a127-bf4231bceca3
---

**Rule:** When adding new `virtual` methods to an existing polymorphic
interface class, ALL new methods go **at the END** of the class body.
NEVER insert in the middle, even when the new method is conceptually
adjacent to existing ones.

**Why:** Vtable slot ordering matches declaration order. Inserting in
the middle shifts every subsequent method's vtable index. Symptoms in
win-release with LTCG:
- `dynamic_cast<DerivedT*>(base_ptr)` returns null for a genuine
  `DerivedT` (because virtual dispatch landed on a wrong-slot method
  that returned a different type entirely).
- "GraphicsPipelineDesc::pipeline_layout is not a VulkanPipelineLayout"
  or similar type-mismatch errors at sites that previously worked.
- Replacing `dynamic_cast` with `static_cast` as a "fix" makes things
  worse — wrong-type pointer → UB → SEGV.
- win-debug + win-asan + win-shipping may pass (less aggressive opt).

**How to apply:**

- New `virtual` methods go at the END of the class declaration,
  AFTER any existing virtual.
- Order of pre-existing methods STAYS unchanged — never re-sort.
- Add an in-class comment block before the "new additions" section
  documenting the convention so the next agent reads it.
- Run `crd-sandbox.exe --headless --smoke-test 3` against win-release
  whenever extending an RHI interface — per-slice DoD's win-debug +
  win-asan + win-shipping + win-tidy does NOT exercise win-release.

**Case study (2026-05-17 Phase 3.1.7.6 v0-close):** v0a inserted
`Device::create_compute_pipeline` between `create_graphics_pipeline`
and `create_command_buffer`; v0c inserted `CommandBuffer::buffer_barrier`
between `transition_image` and `push_constants`; v0d inserted
`Device::compute_queue` + `has_dedicated_compute_queue` between
`graphics_queue` and `wait_idle`. All subsequent vtable slots shifted.
engine/draw/src/renderer.cpp's `device->create_pipeline_layout(...)`
dispatched through a wrong slot in win-release and returned something
that wasn't a VulkanPipelineLayout. User caught it: "this crash
started happening after we added compute stuff to our rhi. keep that
in mind!!!!!" Fix: moved all v0a-v0e additions to the END of each
interface, documented vtable-stability discipline IN the class.

**Related:**
- [feedback_never_defer_solve](workflow-and-correctness.md#memory-feedback_never_defer_solve) — solve, never defer.
- [feedback_quality_bar](workflow-and-correctness.md#memory-feedback_quality_bar) — elite, no shortcuts.


<!-- end-memory:feedback_vtable_stability_append_at_end -->

<a id="memory-feedback_whole_repo_build_and_test_is_cis_job_not_local"></a>
## feedback_whole_repo_build_and_test_is_cis_job_not_local

---
name: feedback_whole_repo_build_and_test_is_cis_job_not_local
description: "⛔⛔⛔ USER DIRECTIVE: NEVER run whole-repo builds/test sweeps locally (per-slice-check across the whole repo, whole-repo win-asan/win-tidy) — that is CI's job. Locally build+run ONLY the module(s) you actually touched."
metadata:
  node_type: memory
  type: feedback
  originSessionId: 192f4d9e-b3f7-485b-a775-5391bcd4183c
  modified: 2026-08-15T07:56:56.041Z
---

⛔⛔⛔ **USER DIRECTIVE (2026-08-15, emphatic).** Do NOT run whole-repo builds or whole-repo test sweeps on the local
machine. Running `scripts/per-slice-check.ps1` across the ENTIRE repo (whole-repo win-debug + win-asan + win-tidy) is
**CI's job**, not the local agent's. A whole-repo local sweep is slow, memory-pressures the host (clang-tidy/cl 0xC0000005
crashes), and — the concrete harm that triggered this — a single flaky GPU test wedged `ctest` for 2+ HOURS and stalled the
whole session while the user was away.

**Why:** the local loop is for FAST iteration on the slice at hand; the exhaustive cross-module / cross-config regression
sweep is exactly what CI exists to do (in parallel, on dedicated hardware, with its own flake handling). Spending local
wall-clock re-proving untouched modules is pure waste and blocks the agent behind hang-prone GPU tests.

**⛔ NUANCE (the user was explicit): "test only what you changed + blast radius" does NOT mean ignore bugs you see.**
You STILL fix every bug you encounter, anywhere — the scope rule is about not spending local wall-clock re-BUILDING and
re-RUNNING untouched modules, NOT about looking away from defects. See a bug → fix it (or record it so it isn't lost);
just don't run the whole-repo sweep locally to go find bugs CI will find.

**How to apply:** to verify a slice locally, build+run ONLY the module(s) the change actually touches + its blast radius
(e.g. a test-only edit to `tests/scene-render/…` → build `crd-scene-render-tests` and run the specific `[tags]`/named
tests you added, across the configs that matter — win-debug + the WSL linux legs for GPU code). NEVER `per-slice-check.ps1`
over the whole repo locally; NEVER a whole-repo `ctest --preset win-asan` / whole-repo tidy. Let CI carry the global sweep
+ the re-verifies-rows regression pass. If a local GPU test must run, bound it (`ctest --timeout N`) so it can never hang
the session. This SUPERSEDES the older "close globally with per-slice-check" habit in CLAUDE.md /
[feedback_full_sweep_required](build-and-verification.md#memory-feedback_full_sweep_required) for LOCAL work — the full sweep still happens, but in CI.

**Concrete harm (2026-08-15) — corrected, because I first MISDIAGNOSED it.** A whole-repo `per-slice-check` win-asan
`ctest` sweep is **~3 hours** — 6384 AddressSanitizer-instrumented tests at ~1-4 s each. Over a change that only touched
`tests/scene-render`, I launched that whole-repo sweep locally, then — while it was healthily in progress at ~4175/6384,
zero failures, tests completing every few seconds — I MISREAD "16 s ctest-process CPU over 115 min" (normal for an
orchestrator; the child test processes burn the CPU) and a wrong-binary `Get-Process` query (I looked for
`crd-scene-render-tests`; the running binary was `crd-gpu-context-vulkan-tests`) as a HANG, and **killed a healthy run**.
No test hung; no test failed (the live ctest log showed 4175 "Test Passed." + 0 failure markers, last completions stamped
seconds before I killed it). The real lesson is NOT a flaky test — it is that a whole-repo local sweep is a MULTI-HOUR job
that belongs in CI, and that "slow ≠ hung": before declaring a hang, compute the EXPECTED duration (tests × per-test time)
and query the ACTUALLY-running binary. This was one of THREE self-inflicted phantom "defects" in one session (this + the
`Select-Object -First` fake-255 + a wrong-process phantom) — my own harness produced every one. Related:
[feedback_powershell_select_first_kills_native_exe_exit_code](build-and-verification.md#memory-feedback_powershell_select_first_kills_native_exe_exit_code), [feedback_timeout_is_not_a_hang_proof](workflow-and-correctness.md#memory-feedback_timeout_is_not_a_hang_proof),
[feedback_run_only_the_tests_you_added](build-and-verification.md#memory-feedback_run_only_the_tests_you_added), [feedback_host_14900k_cap_builds](build-and-verification.md#memory-feedback_host_14900k_cap_builds).


<!-- end-memory:feedback_whole_repo_build_and_test_is_cis_job_not_local -->

<a id="memory-feedback_wsl_ninja_pipe_masks_build_exit_stale_exe_false_green"></a>
## feedback_wsl_ninja_pipe_masks_build_exit_stale_exe_false_green

---
name: feedback_wsl_ninja_pipe_masks_build_exit_stale_exe_false_green
description: "WSL `ninja … | tail && ctest` masks the build failure (pipe returns tail's exit 0) so ctest runs a STALE exe and false-greens; capture the build rc BEFORE any pipe. The WSL twin of the grep/Select-First exit-code scars."
metadata:
  node_type: memory
  type: feedback
  originSessionId: cb9df3b8-2389-479b-9d99-d3d6ce3ba327
  modified: 2026-09-06T01:55:15.262Z
---

⛔⛔ **`ninja … 2>&1 | tail -N && ctest …` on WSL MASKS a build failure and false-greens on a STALE exe (CEIR-31b-3-a-i-2, 2026-09-06).** The `&&` sees the PIPELINE's exit code, which is `tail`'s (0), NOT ninja's — so when the gcc build FAILED (a `-Werror=overloaded-virtual` error), ctest ran anyway against the previously-built (stale) exe and reported "55 tests passed." Only the WRONG COUNT (a new TEST_CASE was missing → the `[ceir31b]` run showed "1 test case" not 2, and a name-filtered run matched 0) exposed it; a slice whose test count happened to match would have false-greened a broken build. The `> tail -3` window also HID the actual error (it was above the last 4 lines).

**Why:** a pipe's `$?` is the LAST command's; `PIPESTATUS[0]` holds the first's — but `bash -lc "… | grep …; echo \$?"` (or PIPESTATUS through another pipe) can still lose it (printed `BUILD_EXIT=` empty this tick).

**How to apply:** capture the build exit code BEFORE any pipe, and gate ctest on it:
```
wsl bash -lc "cd <build> && ninja <targets> > /tmp/b.log 2>&1; rc=\$?; tail -20 /tmp/b.log; test \$rc -eq 0 && ctest -R '<re>' --output-on-failure"
```
Or run the build and the test as SEPARATE tool calls, reading the build's real result (Linking line + exit) first. NEVER trust "tests passed" as proof both toolchains are green unless the BUILD exit code was checked — "true-by-run" requires the build rc, not just that ctest produced output. When a rebuild is needed, remember catch_discover_tests only re-runs when the exe actually relinks: a stale exe keeps a stale test list, so a `ctest -R <newtestname>` matching 0 tests means the exe never rebuilt (chase the build failure, don't conclude the test is missing). Family: [feedback_build_grep_hides_link_errors](build-and-verification.md#memory-feedback_build_grep_hides_link_errors) (the LNK/link-error grep-hiding), [feedback_powershell_select_first_kills_native_exe_exit_code](build-and-verification.md#memory-feedback_powershell_select_first_kills_native_exe_exit_code) (PS pipe kills the exe rc), [feedback_stale_exe_sibling_false_green](workflow-and-correctness.md#memory-feedback_stale_exe_sibling_false_green) (a sibling false-greens on a stale static lib). Same root: an exit code lost to a pipe/filter turns a failure into a false green.


<!-- end-memory:feedback_wsl_ninja_pipe_masks_build_exit_stale_exe_false_green -->

<a id="memory-reference_bat_helpers_need_powershell_tool_not_bash"></a>
## reference_bat_helpers_need_powershell_tool_not_bash

---
name: reference_bat_helpers_need_powershell_tool_not_bash
description: "The Windows .bat build helpers must run via the PowerShell tool, NOT the Bash tool (silent no-op)"
metadata:
  node_type: memory
  type: reference
  originSessionId: fade8ea4-87ca-470f-83e0-cdfe82a44e7f
  modified: 2026-09-05T20:07:14.279Z
---

The repo's Windows build helpers — `scripts\configure-preset.bat`, `scripts\build-target.bat`,
`scripts\run-ctest.bat`, `scripts\tidy-files.ps1` — MUST be invoked through the **PowerShell tool**
(`cmd /c "scripts\configure-preset.bat win-debug"`), NOT the **Bash tool**.

⛔ **`tidy-files.ps1` arg form (2026-09-05):** the files are **POSITIONAL** (`ValueFromRemainingArguments`):
`powershell -File scripts\tidy-files.ps1 <f1.cpp> <f2.hpp> <f3.cpp>` — one space-separated file per arg. NOT
`-Files a,b,c` (a comma-list is ONE path → "file not found") and NOT `-Files a b c` (→ `PositionalParameterNotFound`).
Pass the changed .cpp TUs AND any new/edited .hpp headers. The shell-profile parse-error banner at the top of the
output (`Microsoft.PowerShell_profile.ps1 … missing terminator`) is HARMLESS noise; the real verdict is the
`All N file(s) gated and tidy-clean` line.

Calling `cmd /c "scripts\build-target.bat ..."` from the **Bash tool** (Git Bash) is a **silent no-op**:
Git Bash (MSYS) mangles the `/c` flag (and the `scripts\...` backslash path) before `cmd` sees it, so `cmd`
just starts INTERACTIVELY — it prints its banner (`Microsoft Windows [Version ...]` + the `D:\Dev\cerid>`
prompt) and runs nothing. No error — the build simply doesn't happen (the `.exe`/artifacts keep their old mtime).

⛔⛔ **The trap is a FALSE `EXIT=0`** (2026-09-04, CEIR-25c-1b): the mangled `cmd /c` returns exit code 0
having run NOTHING, so `echo "EXIT=$?"` prints `EXIT=0` and a redirected log file is never even created.
Any "green" test/build number that came through **Bash** `cmd /c` this way is a **stale binary** — the
rebuild never ran. (In Git Bash the escape hatch is `cmd //c "..."` — the double slash blocks MSYS mangling —
but the RULE is still: use the PowerShell tool.) This cost a full debugging detour once (kept grepping a
stale generated file, thinking a fix "didn't work" when the rebuild never ran).

**Rule:** any `.bat`/`.ps1` helper → PowerShell tool. The Bash tool is fine for `grep`/`find`/`sed`/`ls`/
`rm`/`touch`/`git` and reading files, just not for invoking the Windows batch build scripts.

⛔⛔ **COMPOUNDING TRAP — a WRONG TARGET NAME (2026-09-05, CEIR-30c-1):** even from the PowerShell tool,
the reliable way to SEE ninja/ctest output + the real pass count is to call `ctest.exe`/`cmake.exe`
**directly** (PATH-prepend the MSVC bin, `Push-Location build\win-debug`, `& "…\ctest.exe" -R … | Out-String`)
— the `.bat` helpers' stdout still doesn't round-trip cleanly. And **verify the target name against the
`add_executable(...)` in the CMakeLists** before trusting a build: `crd-ceir-gpu-tests` is real, the
plausible-looking `crd-tests-ceir-gpu` is UNKNOWN (`cmake --build --target <bad>` → `ninja: error: unknown
target` + EXIT=1). A no-op build leaves the prior-tick `.exe`; a re-run of a test whose NAME is unchanged
then **false-greens on the STALE exe** (the old assertions pass, the new code was never compiled). Prove the
rebuild: watch for the `Building CXX object …/<file>.o` + `Linking …` lines, or that EXIT≠1 on the CORRECT
target name. See [feedback_stale_exe_sibling_false_green](workflow-and-correctness.md#memory-feedback_stale_exe_sibling_false_green).
See [reference_build_test_workflow](build-and-verification.md#memory-reference_build_test_workflow), [project_ceir_autonomous_loop_grant](project-history.md#memory-project_ceir_autonomous_loop_grant).


<!-- end-memory:reference_bat_helpers_need_powershell_tool_not_bash -->

<a id="memory-reference_build_test_workflow"></a>
## reference_build_test_workflow

---
name: reference-build-test-workflow
description: "Cerid build + test workflow — vcvars sourcing, single-target build, single-test-binary run, per-slice DoD gate, parallel-mode hooks."
metadata:
  node_type: memory
  type: reference
  originSessionId: b24674c3-970b-481c-a127-bf4231bceca3
  modified: 2026-09-06T11:20:16.312Z
---

## Iteration loop (DURING slice work)

Build + run only the module you're editing. NOT the full per-slice gate.

### 1. Source vcvars + build one target

Claude Code's PowerShell tool has NEITHER ninja NOR cl on PATH at startup.
You MUST source vcvars64.bat before any cmake build. The reliable pattern
(works in one PS line, no temp files):

```powershell
cmd /c 'call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1 && cd /d D:\Dev\cerid && cmake --build --preset win-debug --target <module-or-test-target>'
```

Targets:
- engine module: `crd-<module>` (e.g., `crd-geometry-delaunay`)
- test exe: `crd-<module>-tests` (e.g., `crd-geometry-delaunay-tests`)
- single test binary build is much faster than `--target all`

### 2. Run the test binary directly

```powershell
& "D:\Dev\cerid\build\win-debug\tests\<module>\crd-<module>-tests.exe" --reporter compact
```

Filter by tag:
```powershell
& "D:\Dev\cerid\build\win-debug\tests\<module>\crd-<module>-tests.exe" "[tag]" --reporter compact
```

List cases:
```powershell
& "D:\Dev\cerid\build\win-debug\tests\<module>\crd-<module>-tests.exe" --list-tests
```

**Flag is `--reporter` (singular). `--reporters` errors out as "Unrecognised
token".**

### 3. After adding a new source file to a CMakeLists.txt

Reconfigure before re-building or ninja will say "no work to do" without
picking the new file up:

```powershell
cmd /c 'call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1 && cd /d D:\Dev\cerid && cmake --preset win-debug'
```

(For files added to a `crd_collect_sources(...)`-globbed directory, just
re-running the build re-globs automatically.)

## Slice CLOSE (only at slice close — see [feedback-iterate-local-test-only](build-and-verification.md#memory-feedback_iterate_local_test_only))

```powershell
D:\Dev\cerid\scripts\per-slice-check.ps1
```

Runs serially across win-debug + win-asan + win-shipping + win-tidy.
Typical wall time: 3:00–4:00 minutes for a small module change.

Skip slow configs during partial iteration if needed:
- `-SkipShipping` (skips LTO config — biggest single time saver)
- `-SkipTidy` (skips clang-tidy)
- `-SkipAsan`
- `-Reconfigure` to force `cmake --preset` before each build

**Cluster CLOSE** (after all slices in a sub-module are done):
`scripts/full-sweep.ps1` for the 11 Windows + 7 Linux full 18-config sweep.

## Background execution (saves context tokens)

`PowerShell` and `Bash` tools accept `run_in_background: true`. The system
notifies on completion — no need to sleep, poll, or re-check. **Use this
for any command that takes > ~30s and whose output you don't need
immediately.**

When to background:
- `scripts/per-slice-check.ps1` at slice close (~3:30, sometimes longer):
  background it, work on docs/session-log/MEMORY/context updates while it
  runs, then read the result notification.
- `scripts/full-sweep.ps1` at cluster close (10–20 min): always background.
- A single `cmake --build` that touches a large module: background if you
  have other independent work to do.

When NOT to background:
- Single-test-binary runs (sub-second).
- Single-module `cmake --build` when you'll just immediately read the result.
- Any command whose output you NEED to decide the next step.

Pattern:
```powershell
# Foreground: blocks until done, you see output inline.
D:\Dev\cerid\scripts\per-slice-check.ps1

# Background: returns immediately, system notifies on completion.
# (Use `run_in_background: true` on the Bash/PowerShell tool call.)
```

If output is huge, the tool also persists full output to disk and only
shows you a preview. Background mode is the most context-efficient option.

## Parallelization notes

The 4 per-slice configs are INDEPENDENT (separate `build/<preset>/` dirs).
They could run in parallel via PowerShell jobs / `Start-ThreadJob`. Caveats:
- Total memory: 4× ninja + 4× linker is ~16–24 GB peak. Set `ninja -j N/4` per
  job to keep CPU under saturation.
- win-shipping is link-bound (LTO single-threaded link); the most-parallelism-
  friendly config — pair it with shorter configs.
- Each parallel branch must source vcvars independently.
- Stdout interleaving makes "PASS/FAIL summary" the key output, not the log.

If `-Parallel` flag is added to per-slice-check.ps1, suggested mode:
- Two pairs: (win-debug + win-tidy), (win-asan + win-shipping) — balanced.
- OR all 4 at once with `ninja -j 4` per job on a 16-core machine.

## ASan DLL — required for win-asan ctest

```powershell
$env:PATH = 'C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\MSVC\14.51.36231\bin\Hostx64\x64;' + $env:PATH
```

The per-slice-check.ps1 sets this before its ASan ctest invocation.

## Common gotchas

- `Bash` tool: cannot run PowerShell cmdlets (`Select-Object`, `Get-Content`,
  etc.). Use `PowerShell` tool for PS pipelines.
- `where.exe` failures from PS with `2>&1` print red error noise; use
  `Get-Command -ErrorAction SilentlyContinue` instead.
- `cmake --build` without vcvars sourced fails with cryptic
  `"" -v crd-<target>` + `no such file or directory` because
  CMAKE_MAKE_PROGRAM is missing from the environment.
- Visual Studio Installer dir on PATH for vswhere.exe:
  `C:\Program Files (x86)\Microsoft Visual Studio\Installer`. vcvars64.bat
  needs vswhere.exe; the per-slice-check.ps1 prepends it before sourcing.

Related: [feedback-iterate-local-test-only](build-and-verification.md#memory-feedback_iterate_local_test_only), [feedback-per-slice-run-ctest](build-and-verification.md#memory-feedback_per_slice_run_ctest)


<!-- end-memory:reference_build_test_workflow -->

<a id="memory-reference_build_toolchain_vs18_vcvarsall_path"></a>
## reference_build_toolchain_vs18_vcvarsall_path

---
name: reference_build_toolchain_vs18_vcvarsall_path
description: "The correct vcvarsall.bat path for building this repo (VS 18, Ninja generator)"
metadata:
  node_type: memory
  type: reference
  originSessionId: 5c1488ab-fa28-4925-ae3b-29d69f5ca31c
  modified: 2026-08-02T20:28:10.772Z
---

The host's MSVC moved to **Visual Studio 18 Community**. The Ninja-generated `build/win-release` needs the
MSVC environment sourced first. The correct incantation (the old `...\2022\Community\...` path no longer exists
and silently fails — `&&` short-circuits, cmake never runs, and `>nul 2>&1` hides it, so the build returns exit 1
with ZERO output, which looks like a mystery, not a bad path):

```powershell
& cmd /c "`"C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat`" x64 >nul 2>&1 && cmake --build D:\Dev\cerid\build\win-release --target <TARGET> 2>&1"
```

Discover it robustly with vswhere: `& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath` → append `\VC\Auxiliary\Build\vcvarsall.bat`. Generator is **Ninja**
(`CMAKE_GENERATOR:INTERNAL=Ninja` in CMakeCache), so the env MUST be sourced — there is no MSBuild toolset
auto-detection. If a build returns exit 1 with empty output, suspect a stale vcvarsall path FIRST. See
[reference_build_test_workflow](build-and-verification.md#memory-reference_build_test_workflow).


<!-- end-memory:reference_build_toolchain_vs18_vcvarsall_path -->

<a id="memory-reference_cholmod_oracle_and_wsl_build"></a>
## reference_cholmod_oracle_and_wsl_build

---
name: cholmod-oracle-and-wsl-build
description: "How to build+run the CHOLMOD supernodal oracle bench (WSL), and the WSL2 v9fs build-dir gotcha (never fresh-build on /mnt/d)."
metadata:
  node_type: memory
  type: reference
  originSessionId: a9e2b042-249d-4290-abf9-3c707236fa70
---

**CHOLMOD oracle** (the real supernodal peer for hesap sparse-direct; [cholmod-factor-gap](project-history.md#memory-project_cholmod_factor_gap)).

Setup (WSL, sudo password is the user's — ask if needed): `bash scripts/setup-cholmod-ref.sh` — apt-installs `libsuitesparse-dev` (CHOLMOD 5.2.0) + `libopenblas-dev`, and **switches the BLAS/LAPACK update-alternative to OpenBLAS** (Ubuntu defaults to single-threaded reference netlib, which would make CHOLMOD artificially slow — a fake crush). Verify `readlink -f /usr/lib/x86_64-linux-gnu/libblas.so.3` shows `openblas-pthread`.

Bench: `runtime/examples/bench_hesap_cholesky_vs_cholmod.cpp`, gated by `-DCRD_BUILD_HESAP_VS_CHOLMOD=ON` (Linux/WSL only; CHOLMOD's Supernodal module is GPL ⇒ dev-only, NEVER shipped/CI-release). Forces `CHOLMOD_SUPERNODAL` + `CHOLMOD_NATURAL` on the same Cerid-AMD-permuted matrix (fair: identical elimination order). Decoupled from `CRD_BUILD_HESAP_VS_REFERENCE` — pass `-DCRD_HESAP_MATRIX_DIR=<suitesparse-mm dir>` to reuse already-downloaded matrices. `CRD_BENCH_THREADS=N` caps Cerid workers; set `OMP_NUM_THREADS=OPENBLAS_NUM_THREADS=N` to match for a fair fight.

**Fast build recipe (avoids the v9fs trap below):** reuse the existing `build/linux-gcc-release` dir (deps already cached, crd libs compiled, spirv-reflect resolved via cache at `~/cerid-deps/vulkan-sdk`):
```
export VULKAN_SDK=/home/yatiyr/cerid-deps/vulkan-sdk
cmake -S /mnt/d/Dev/cerid -B /mnt/d/Dev/cerid/build/linux-gcc-release \
  -DCRD_BUILD_HESAP_VS_CHOLMOD=ON -DCRD_HESAP_MATRIX_DIR=/mnt/d/Dev/cerid/build/win-relwithdebinfo/_deps/suitesparse-mm
cmake --build .../linux-gcc-release --target bench_hesap_cholesky_vs_cholmod
```
This also serves as the **gcc `-Werror` cross-config check** on whatever hesap-direct code changed.

**WSL2 v9fs gotcha (cost me ~20 min):** a FRESH CMake build dir on `/mnt/d` (the Windows drive, mounted v9fs) is pathologically slow — git clones / FetchContent write thousands of small files across the 9p boundary; an Eigen+OpenBLAS fetch stalls for 15+ min in `D` (uninterruptible I/O). `CRD_BUILD_HESAP_VS_REFERENCE=ON` triggers exactly that (Eigen + OpenBLAS source build) and the CHOLMOD bench does NOT need it (it uses SYSTEM cholmod+OpenBLAS). Also: a fresh dir re-fetches ALL project CPM deps AND fails configure on crd-shader unless `$VULKAN_SDK/Source/SPIRV-Reflect` exists. ⇒ Either reuse a configured dir (above), or build on a NATIVE ext4 path (`~/...`) where clones are fast — but then you still hit the spirv-reflect/VULKAN_SDK requirement. Reusing `linux-gcc-release` sidesteps all of it.


<!-- end-memory:reference_cholmod_oracle_and_wsl_build -->

<a id="memory-reference_ctest_regex_pipe_is_a_cmd_pipe"></a>
## reference_ctest_regex_pipe_is_a_cmd_pipe

---
name: reference_ctest_regex_pipe_is_a_cmd_pipe
description: "A `|` in a ctest -R regex passed through cmd /c is a CMD PIPE, not alternation — use two single-term calls"
metadata:
  node_type: memory
  type: reference
  originSessionId: cb9df3b8-2389-479b-9d99-d3d6ce3ba327
  modified: 2026-09-04T11:43:47.763Z
---

Running `cmd /c "scripts\run-ctest.bat <dir> A|B"` (the helper does `ctest -R "%~2"`) does NOT run ctest with the
regex alternation `A|B`. The **`|` is interpreted by CMD as a shell PIPE** BEFORE the batch file sees it — so `A` is
run and its output piped to a nonexistent command `B` → `'B' is not recognized as an internal or external command` and
ctest exits non-zero having matched nothing. Hit 2026-09-04 (CEIR-25c-2b) trying `-R "D3D12|DX12"`.

**Rule:** to regress two name-patterns, make TWO single-term calls (`... DX12` then `... D3D12`) — a single term has no
pipe and works (as `-R "25c-2"` does). A quoted `"%~2"` inside the .bat does NOT protect the `|`, because CMD splits the
outer `cmd /c "..."` command line on `|` first. Same shell-parsing family as [reference_bat_helpers_need_powershell_tool_not_bash](build-and-verification.md#memory-reference_bat_helpers_need_powershell_tool_not_bash)
(the `cmd /c` false-EXIT=0). See also [feedback_per_slice_run_ctest](build-and-verification.md#memory-feedback_per_slice_run_ctest).


<!-- end-memory:reference_ctest_regex_pipe_is_a_cmd_pipe -->

<a id="memory-reference_cuda133_cub_cublas_bench_build_flags"></a>
## reference_cuda133_cub_cublas_bench_build_flags

---
name: reference_cuda133_cub_cublas_bench_build_flags
description: CUDA 13.3 CUB/cuBLAS gold benches need -arch=sm_89 (driver rejects 13.3 PTX JIT) + -std=c++17 -Xcompiler /Zc:preprocessor
metadata:
  node_type: memory
  type: reference
  originSessionId: 1487a581-3392-44fb-bc9e-ebeaffd19da5
---

**Building a CUB / CCCL / cuBLAS gold bench with the local CUDA 13.3 (`nvcc`) for the B-cmp compute-primitive crush campaign:**

```
nvcc -O3 -std=c++17 -arch=sm_89 -allow-unsupported-compiler -Xcompiler "/Zc:preprocessor" \
     -I"C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.3\include\cccl" foo.cu -o foo.exe
```

- **`-arch=sm_89` is MANDATORY** (RTX 4070 Ti SUPER = Ada = sm_89). Without it nvcc emits forward-compat PTX that the installed
  driver JIT-rejects at runtime: *"the provided PTX was compiled with an unsupported toolchain"* — the kernel silently does
  NOT launch, `cudaGetErrorString` on the CUB call returns that string, `temp_bytes` comes back 0, and the result buffer is
  untouched. **The bench then prints an absurd bandwidth (100s of TB/s) because the empty launch is ~instant.** Always verify a
  vendor bench with a KNOWN result (e.g. reduce of all-1.0 ⇒ sum == N) before trusting its timing. The cuFFT DLL benches
  worked without this because the DLL ships precompiled multi-arch SASS.
- CCCL headers live under `include/cccl/` (not `include/cub/`) in 13.3 — add `-I…\include\cccl`.
- CCCL requires `-std=c++17` AND the conforming preprocessor `-Xcompiler /Zc:preprocessor` (else `#error` at compile).
- `cudaDeviceProp::memoryClockRate` was REMOVED in CUDA 13 — don't reference it.
- Timing: per-call `cudaEvent` brackets race the async launch (gave garbage); use **wall-clock over a 200-call batch with
  `cudaDeviceSynchronize` before/after**, divide by the count, min-of-N.
- Run the `.exe` with `…\CUDA\v13.3\bin\x64` (and `…\bin`) on PATH for the runtime DLLs.
- ⛔ **`nvcc` COMPILE needs the host `cl.exe`** — a bare `nvcc foo.cu` fails `nvcc fatal: Cannot find compiler 'cl.exe' in PATH`.
  Build UNDER vcvars: PowerShell `cmd /c '"…\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1 && nvcc …'` (the same
  vcvars the cmake builds use; `-arch=sm_89 -std=c++17 -Xcompiler /Zc:preprocessor`). CUDA-bin-on-PATH is for RUN, not compile.
- ⛔⛔ **A STALE prior-session `.exe` runs and prints PLAUSIBLE numbers even when the fresh compile FAILED** — I ran `nvcc … && ./x.exe`
  under Bash (where cl was absent so the compile silently failed) and it executed an OLD `x.exe`, printing sane-looking GB/s. Always
  confirm the compile produced a FRESH exe (delete first, or check the timestamp / `EXIT=0`) before trusting a bench's output — the
  Bash `PIPESTATUS`/grep-exit can hide the nvcc failure. (Companion to the "verify a vendor bench with a KNOWN result" rule above.)
- 2026-07 toolchain path moved: Visual Studio is now `C:\Program Files\Microsoft Visual Studio\18\Community\` (was "2026 Insiders").

Board: `docs/bench/2026-07-13-gpu-compute-primitives.md`. First result: CKIR `build_reduce` BEAT CUB `DeviceReduce` (1.42×
L2-resident, 1.05× DRAM-bound). See [feedback_bit_exact_fft_crushes_only_when_dram_bound](numerics-and-performance.md#memory-feedback_bit_exact_fft_crushes_only_when_dram_bound) (same memory-bound doctrine).


<!-- end-memory:reference_cuda133_cub_cublas_bench_build_flags -->

<a id="memory-reference_manual_header_filter_tidy_stricter_than_gate_on_shared_test_headers"></a>
## reference_manual_header_filter_tidy_stricter_than_gate_on_shared_test_headers

---
name: reference_manual_header_filter_tidy_stricter_than_gate_on_shared_test_headers
description: "A manual `clang-tidy --header-filter=<x>` run on a SHARED TEST header (tests/gpu-shared/*.hpp) reports naming/style errors the REAL gate does NOT enforce — verify against a shipped sibling before 'fixing' them."
metadata:
  node_type: memory
  type: reference
  originSessionId: 192f4d9e-b3f7-485b-a775-5391bcd4183c
  modified: 2026-08-16T01:37:37.443Z
---

⛔ Running `clang-tidy -p build/win-tidy --header-filter='<myheader>' <some_test.cpp>` to check a **shared test-harness
header** (`tests/gpu-shared/*.hpp`) is **STRICTER than the real CI gate**. The manual `--header-filter` FORCES diagnostics
on that header; the gate's own header-filter (set via `CMAKE_CXX_CLANG_TIDY` in the root CMakeLists) does **not** surface
naming/style violations for these shared test headers — so a manual run will report `readability-identifier-naming`
("invalid case style for global constant") on `static constexpr int` members and similar that the gate silently accepts.

**Proof (CEIR-18a-1, 2026-08-15):** my new `ckir_light_cull_test.hpp` got 9 `readability-identifier-naming` errors on its
`static constexpr int` scene members (`tiles_x`, `num_clusters`, …). The SHIPPED sibling `ckir_visbuffer_test.hpp` trips the
**identical** errors on `n_vert`/`n_tri`/`n_clusters` (verified by the same manual run) — yet it ships and the repo tidy gate
is GREEN. So those "errors" are a manual-run artifact, not gate failures; lowercase `static constexpr` matches the sibling
convention and passes. (`tests/.clang-tidy` only disables 3 bugprone checks, NOT naming — [reference_tests_clang_tidy_exclusions](build-and-verification.md#memory-reference_tests_clang_tidy_exclusions)
— so the exclusion isn't the naming silence; the header-filter scope is.)

**How to apply:** when a manual `--header-filter` tidy on a shared test header reports naming/style errors, VERIFY against a
shipped sibling harness (run the same manual check on it) before renaming — if the sibling trips identical errors and ships
green, they are NOT gate failures; match the sibling convention, don't diverge. This does NOT apply to ENGINE headers
(`engine/*/include`) — those ARE in the gate scope and must be tidy-clean (the 18a-1 engine header `ckir_light_cull.hpp` was
verified clean). Still fix genuinely-bugprone findings (e.g. `bugprone-misplaced-widening-cast` on `static_cast<usize>(a*b)`
— pre-compute the product or cast a single operand; the siblings do this via an `npix`-style local). Related:
[feedback_tidy_gate_clean_on_unparsed_files](build-and-verification.md#memory-feedback_tidy_gate_clean_on_unparsed_files), [feedback_whole_repo_build_and_test_is_cis_job_not_local](build-and-verification.md#memory-feedback_whole_repo_build_and_test_is_cis_job_not_local).

**The EXACT mechanism (re-confirmed CEIR-18b, 2026-08-16 — I templatized `ckir_light_cull_test.hpp` and re-hit this):**
`.clang-tidy` line ~100 sets `HeaderFilterRegex: '.*\/crd\/.*'`. The gate tidies a `.cpp` as its TU and this regex decides
which INCLUDED headers get diagnostics — it matches only paths containing `/crd/`. A `tests/gpu-shared/*.hpp` header is NOT
under `crd/`, so the gate NEVER checks it. But `scripts/tidy-files.ps1` runs each PASSED file as its own main-file TU with
NO `--header-filter` (its comment says so), so passing a shared test header DIRECTLY to it checks the header in full → the
false positives. ⛔ RULE: do NOT pass `tests/gpu-shared/*.hpp` (or any non-`crd/` shared header) to tidy-files.ps1 — it is
gate-invisible; the `readability-identifier-naming` errors on its struct constants are artifacts. The `.cpp` files that
include it ARE gate-checked (their own TU) — so real findings THERE (isolate-declaration, widening-cast) must be fixed,
including latent debt in a file your slice touched ([feedback_full_sweep_after_uncommitted_work_peels_tidy_onion](build-and-verification.md#memory-feedback_full_sweep_after_uncommitted_work_peels_tidy_onion)).


<!-- end-memory:reference_manual_header_filter_tidy_stricter_than_gate_on_shared_test_headers -->

<a id="memory-reference_tests_clang_tidy_exclusions"></a>
## reference_tests_clang_tidy_exclusions

---
name: reference-tests-clang-tidy-exclusions
description: "Tests have a dedicated .clang-tidy with 3 check exclusions for Catch2/test patterns that don't apply to production code. Created 2026-05-17 during the policy-flip cleanup."
metadata:
  node_type: memory
  type: reference
  originSessionId: b24674c3-970b-481c-a127-bf4231bceca3
---

`D:\Dev\cerid\tests\.clang-tidy` exists as a per-directory override of the
repo-wide `.clang-tidy`. It inherits the root file and disables exactly
three checks **for test code only**:

1. **`bugprone-unchecked-optional-access`** — clang-tidy 17's flow-sensitive
   analyzer doesn't see through Catch2's `REQUIRE(opt.has_value())` macro
   abort path. Tests legitimately do `REQUIRE(opt.has_value()); REQUIRE(opt->field == ...)`
   and tidy fires a false positive on the second deref. The pattern scales
   with test count, not with risk.
2. **`bugprone-suspicious-memory-comparison`** — tests deliberately use
   `std::memcmp` on `Vec3<float>` / `f64` for bit-exact replay assertions
   (Shewchuk predicate determinism, deterministic-FP regression catches).
   In production code this check stays active.
3. **`bugprone-random-generator-seed`** — tests use constant PCG-style
   seeds (`state = 0xC0FFEE`) for reproducible CI replay. Production code
   keeps the check active.

**Why per-directory, not global:** all three checks are valuable for engine
code; only the Catch2/test patterns trip them as false positives. Local
override avoids weakening the engine-side policy.

**How to apply:** when adding new test code that hits one of these, no
extra `// NOLINT` needed — the tests dir already silences them. When
adding test patterns that trip a DIFFERENT check, either: (a) use a
per-line `// NOLINT(check-name)` if it's local, or (b) discuss adding the
check to the test exclusion list if it's a common pattern.

Related: [feedback-clang-tidy-warnings-are-errors](build-and-verification.md#memory-feedback_clang_tidy_warnings_are_errors) — the project-wide
zero-warnings mandate.


<!-- end-memory:reference_tests_clang_tidy_exclusions -->

<a id="memory-reference_windows_test_run_env_assets_shaderc_asan_dll"></a>
## reference_windows_test_run_env_assets_shaderc_asan_dll

---
name: reference_windows_test_run_env_assets_shaderc_asan_dll
description: "Running a Windows crd test .exe DIRECTLY (not via ctest/per-slice-check) needs three run-env pieces set or it FALSELY reds: CRD_ASSETS_DIR (authored .ckir load; without it scene-render init_programs()==false → REN-2/REN-38 gates hard-fail, CEIR gates SKIP 'shader backend unavailable'), the Vulkan-SDK Bin on PATH (shaderc_shared.dll, GLSL→SPIR-V), and the VS18 MSVC ASan runtime dir on PATH for win-asan builds (else the exe won't launch → EMPTY test output). ctest sets CRD_ASSETS_DIR from the CMake test ENVIRONMENT; per-slice-check sets the ASan PATH — a raw `& $bin` run sets neither."
metadata:
  node_type: memory
  type: reference
  originSessionId: 192f4d9e-b3f7-485b-a775-5391bcd4183c
  metadata:
    type: reference
  modified: 2026-08-16T19:59:11.087Z
---

⛔ **A Windows crd test binary run DIRECTLY (`& build\<cfg>\tests\<d>\<t>.exe`, e.g. a hand-rolled matrix script) FALSELY REDS unless three run-env pieces are set — a green ctest/per-slice-check run masks all three because it sets them for you.** Cost a full debug cycle at the CEIR-19 band close (looked like a scene-render regression; was pure run-env).

Symptom → cause map (check BEFORE suspecting code):
1. **scene-render: many gates fail `REQUIRE(renderer.init_programs(*vk))` == false** (and the CEIR gates SKIP "shader backend unavailable") ⇒ **`CRD_ASSETS_DIR` unset**. init_programs loads the authored `.ckir` programs from disk; no assets dir → no program → false. Set `CRD_ASSETS_DIR=D:\Dev\cerid\assets`. (The older REN-2/REN-38 gates HARD-REQUIRE it; the newer CEIR gates SKIP-guard it — same root.)
2. **create_program / a compute+RT kernel fails to compile** ⇒ **`shaderc_shared.dll` not on PATH**. The Vulkan backend loads shaderc dynamically (GLSL→SPIR-V, ADR-0103). Add `%VULKAN_SDK%\Bin` (e.g. `C:\VulkanSDK\1.4.341.1\Bin`).
3. **win-asan: the exe produces EMPTY output / won't start** ⇒ **the MSVC ASan runtime DLL not on PATH**. The VS18 build links `clang_rt.asan_dynamic-x86_64.dll`, in `C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\MSVC\<ver>\bin\Hostx64\x64` (VS **18**, not 2022 — the vs18 toolchain, per [reference_build_toolchain_vs18_vcvarsall_path](build-and-verification.md#memory-reference_build_toolchain_vs18_vcvarsall_path)). Prepend that dir to PATH before running an asan exe. `per-slice-check.ps1` does this (its `$AsanRuntimeDir`); a raw run does not.

⛔ These are RUN-env, distinct from BUILD-env: `build-target.bat`/vcvars fix the BUILD (Ninja + cl.exe + `<memory>`/`<span>`/d3d12.lib); they do NOT set the three above for the subsequent `.exe` RUN. The clean path for a full Windows matrix is `ctest`/`per-slice-check.ps1` (env handled). If you MUST run exes directly, set all three. Related: [feedback_sandbox_smoke_overlay_only_false_green_needs_crd_assets_dir](workflow-and-correctness.md#memory-feedback_sandbox_smoke_overlay_only_false_green_needs_crd_assets_dir) (the assets-dir half).


<!-- end-memory:reference_windows_test_run_env_assets_shaderc_asan_dll -->

<a id="memory-reference_wsl2_perf_unavailable_use_objdump"></a>
## reference_wsl2_perf_unavailable_use_objdump

---
name: reference_wsl2_perf_unavailable_use_objdump
description: perf hardware counters are unavailable in this WSL2 (vPMU not virtualized — all events <not supported>); use objdump static instruction-mix instead for kernel analysis
metadata:
  node_type: memory
  type: reference
  originSessionId: d8c20658-f5fe-496c-941d-3a18ffad3cdd
---

On this dev box's **WSL2 (Ubuntu, 6.6.x-microsoft-standard-WSL2 kernel)**, `perf` **hardware counters do not
work** — every event (cycles, instructions, cache-misses, L1-dcache-load-misses) reads **`<not supported>`**
even after `sudo apt install linux-tools-generic linux-tools-common` AND `sysctl -w
kernel.perf_event_paranoid=-1`. The Hyper-V **vPMU is not virtualized** for WSL2; this is a platform limitation,
not a config fix (no `.wslconfig` switch enables it reliably). The perf binary lands at
`/usr/lib/linux-tools-<ver>/perf` (the `/usr/bin/perf` wrapper version-mismatches the WSL kernel).

**Substitute toolchain that DOES work in WSL2 (no PMU needed — installed + validated 2026-06-15):**
- **`llvm-mca -mcpu=raptorlake`** (apt `llvm`; binary `/usr/bin/llvm-mca`) — STATIC pipeline model: per-port
  pressure + Block RThroughput + IPC of a hot kernel. Mark the region in C++ with
  `asm volatile("# LLVM-MCA-BEGIN name" ::: "memory");` … `asm volatile("# LLVM-MCA-END" ::: "memory");`, then
  `g++ -O3 -march=native -S` and `llvm-mca file.s` (reports each named region). This pinned the FFT AoS butterfly
  as **port-5(shuffle)-bound** (RThroughput 16 vs SoA 3.3) — the deep "which port" answer perf would give.
  ⚠ It models ONLY the L1-resident kernel — NOT memory passes / cache traffic. Pair with cachegrind + timing.
- **`valgrind --tool=cachegrind --cache-sim=yes <bin>`** (apt `valgrind`) — SIMULATES D1/LL cache misses (no PMU).
  Proved the FFT sub-FFT is L1-streaming-bound (D1 8.5%, LLd 0% = fits L2, streams L1→L2 per pass). ~50× slowdown
  ⇒ run on small inputs/reps with a `timeout`.
- **`objdump -d --no-show-raw-insn <bin>`** + grep/count mnemonics — STATIC instruction mix (shuffles
  `vperm2f128|vpermpd|vpermilpd|vunpck*|vshuf|vblend` vs FMA `vfmadd|vfnmadd` vs arith `v(mul|add|sub)pd` vs mem
  `vmov(u|a)pd`). ⚠ **whole-binary counts OVERCLAIM** — restrict to the exact hot symbols (awk the symbol range)
  AND validate with timing (the FFT static count said shuffle-bound, but timing+cachegrind proved memory-pass-bound).
- **cyc/elem via bracketed timing** (rdtsc or steady_clock × pinned core) — ratios are throttle-robust.
- Compile probes with `-g`; build to `build/` (NOT `/tmp` — WSL clears it between sessions).
- ⭐ **Evidence ladder (do not overclaim from one tool):** source-path → symbol-restricted objdump → llvm-mca
  port model → cachegrind cache sim → full-FFT timing. Static counts are supporting, timing is decisive.

**Invocation gotcha:** the Bash tool is Git Bash and **MSYS-mangles leading `/usr/...` paths** passed to
`wsl.exe` (turns them into `C:/Program...`). Drive WSL from the **PowerShell tool** (`wsl bash -lc '...'`,
single-quoted) or write the script to a `build/*.sh` file and run `wsl bash /mnt/d/.../build/x.sh`.

If real hardware counters are ever needed: run natively on Windows (VTune / AMD uProf) against the MSVC build,
or measure on a bare-metal Linux box. Context: [project_v10_fft_plan](project-history.md#memory-project_v10_fft_plan), dossier `docs/research/fft-mkl-crush.md`.


<!-- end-memory:reference_wsl2_perf_unavailable_use_objdump -->

<a id="memory-reference_wsl_lc_loop_var_expands_empty_use_literal_paths"></a>
## reference_wsl_lc_loop_var_expands_empty_use_literal_paths

---
name: reference_wsl_lc_loop_var_expands_empty_use_literal_paths
description: "`wsl bash -lc '...'` from the Bash tool expands SHELL/LOOP variables ($p, $x) to EMPTY (env vars like $HOME survive) — a for-loop no-ops and exits 0, a FALSE GREEN; spell out literal paths, echo a variable-bearing sentinel to verify"
metadata:
  node_type: memory
  type: reference
  originSessionId: 192f4d9e-b3f7-485b-a775-5391bcd4183c
  modified: 2026-08-17T23:04:05.458Z
---

Invoking `wsl bash -lc '<script>'` from the Bash tool (Git Bash → wsl.exe → Linux bash) does NOT reliably
pass shell/loop variables through. Symptom seen at the CEIR-21z Linux band-close:

```
wsl bash -lc 'set -e; for p in linux-gcc-debug linux-gcc-asan; do echo LEG_$p; cd ~/cerid-build/$p && ninja ...; done'
```

printed `LEG_` (empty `$p`) TWICE, then exited 0 — the loop iterated the right number of times but `$p` was
EMPTY every iteration, so `cd ~/cerid-build/` (empty) + `cmake .`/`ninja` failed into a `2>&1 | tail` pipe
(which `set -e` doesn't trip on a non-final pipe stage) and the whole thing NO-OP'd to a green exit. A
**FALSE GREEN** — it looked like a passing build+test sweep and built/tested NOTHING.

Diagnostic that pinned it: `wsl bash -lc 'for x in a b; do echo ITER_$x; done; echo HOME_IS_$HOME'` →
`ITER_` `ITER_` `HOME_IS_/home/yatiyr`. So **environment variables ($HOME) expand, but variables SET DURING
the script (loop vars, locals) expand to empty** — the signature of a double-evaluation: wsl.exe routes the
command line through a shell that expands `$x` (unset at that point → "") once BEFORE the quoted `bash -lc`
runs it. Single-quoting the outer arg in Git Bash does NOT prevent it (tried both `"..."` and `'...'`).

**Rule:** through `wsl bash -lc`, use **literal paths and env-vars ($HOME/`~`) only — never a loop/local
variable**. Spell each preset out explicitly:

```
wsl bash -lc 'cd ~/cerid-build/linux-gcc-debug && cmake . >/dev/null 2>&1 && ninja crd-ceir-tests && ctest -R "..." ; \
              cd ~/cerid-build/linux-gcc-asan  && cmake . >/dev/null 2>&1 && ninja crd-ceir-tests && ctest -R "..."'
```

This ran REAL: `74/74` link + `60/60` tests each leg. **Check:** always `echo` a variable-bearing sentinel
(e.g. `echo LEG_<literal>`) and CONFIRM the value printed before trusting a green — an empty sentinel means
the no-op trap fired. Probable clean fix to verify ONCE before relying on it: `wsl -e bash -lc '...'` (the
`-e` may stop wsl.exe pre-expanding). Also: `scripts/wsl-build.ps1 <preset>` avoids the trap entirely (it
builds the command Windows-side). Related: [feedback_powershell_file_on_a_bat_is_a_silent_noop_use_cmd_c](build-and-verification.md#memory-feedback_powershell_file_on_a_bat_is_a_silent_noop_use_cmd_c),
[feedback_powershell_select_first_kills_native_exe_exit_code](build-and-verification.md#memory-feedback_powershell_select_first_kills_native_exe_exit_code), [reference_wsl_linux_sweep_and_llvmpipe_exposure](build-and-verification.md#memory-reference_wsl_linux_sweep_and_llvmpipe_exposure).


<!-- end-memory:reference_wsl_lc_loop_var_expands_empty_use_literal_paths -->

<a id="memory-reference_wsl_linux_sweep_and_llvmpipe_exposure"></a>
## reference_wsl_linux_sweep_and_llvmpipe_exposure

---
name: reference_wsl_linux_sweep_and_llvmpipe_exposure
description: "WSL Linux runs MORE than CI (llvmpipe = a real Vulkan device; CI has loader only, no ICD — GPU tests skip there). CI-mirror locally with VK_DRIVER_FILES=/nonexistent. Vulkan headers PINNED via FetchContent (distro 1.3.27x lacks the 2025 NV extensions). the B19 SIGSEGVs turned out to be REAL kernel defects — FIXED, see feedback_llvmpipe_campaign_three_kernel_defects."
metadata:
  node_type: memory
  type: reference
  originSessionId: 31e31376-4d57-4a00-b30c-77365444ac88
  modified: 2026-07-27T13:55:14.459Z
---

**The three Linux environments differ, and each catches a different class** (first mapped 2026-07-27, REN-38 close):

- **GitHub CI Linux**: installs `libvulkan-dev` (LOADER only — no `mesa-vulkan-drivers`, no ICD) → `create_vulkan_gpu_context`
  finds no device → every GPU-dispatch test SKIPS. CI Linux verifies gcc compile strictness + CPU tests only. No dxc
  either → `CRD_HAS_DXC=0`; HLSL-validation tests soft-skip on the stub's `CRD_HAS_DXC=0` marker (wired into
  `test_ckir_glsl_compile.cpp` — the older `test_hlsl_conformance.cpp` always had the probe idiom).
- **WSL Ubuntu 24.04** (`wsl-build.ps1 <preset>`, builds on native ext4 `~/cerid-build/<preset>`): HAS
  `mesa-vulkan-drivers` → **llvmpipe runs the GPU tests on the CPU** — strictly more coverage than CI. To mirror CI
  exactly: `VK_DRIVER_FILES=/nonexistent VK_ICD_FILENAMES=/nonexistent ctest` (loader present, no device → GPU skips).
- **Windows**: real GPU + pinned LunarG SDK 1.4.341 (CI caches it).

**Vulkan headers are PINNED on Linux** (`engine/gpu-context-vulkan/CMakeLists.txt`): distro headers are 1.3.27x and
the frontier NV extensions (cluster-AS FA-3, LSS B18-f) don't exist there → FetchContent Khronos Vulkan-Headers at
the SAME SDK tag as Windows (`vulkan-sdk-1.4.341.0`), `BEFORE PUBLIC` include (tests compile raw Vulkan too),
`SOURCE_SUBDIR include` so their CMakeLists (which re-creates `Vulkan::Headers`) is never added. Loader version is
independent — every extension entry point loads via `vkGetDeviceProcAddr`.

**RESOLVED (same day):** the llvmpipe failures were triaged to four root causes and ALL FIXED — three real
kernel defects (phantom-lane ballot complement, unguarded tail threads, eager-Select sentinel loads), NV-
calibrated tolerances, a dxc-absence assert, and environment-stale autotune rows. Full detail:
[feedback_llvmpipe_campaign_three_kernel_defects](workflow-and-correctness.md#memory-feedback_llvmpipe_campaign_three_kernel_defects). The WSL llvmpipe run is now a FIRST-CLASS gate — it
catches what no other environment can (subgroup-width generality + real OOB), run it on GPU-kernel slices.

**⚠ llvmpipe THREAD OVERSUBSCRIPTION under a parallel ctest.** llvmpipe spawns ~nproc worker threads PER
PROCESS, so `ctest -j N` puts N x nproc of them (plus N JIT compilers) on the same cores. The heaviest shader in
the suite (IB-1 path tracer: 4 unrolled bounces x many-lights NEE+MIS) SEGFAULTed ONCE under `-j 4` while passing
3/3 alone, 44/44 in its own family at -j 4, and 5/5 stress runs afterwards. `scripts/wsl-build.ps1` now exports
`LP_NUM_THREADS=4` before ctest so a parallel run cannot manufacture phantom failures. ⚠ The sanctioned harness
runs ctest SERIALLY anyway — if you hand-run `-j N`, set that cap yourself or expect load-dependent noise.


<!-- end-memory:reference_wsl_linux_sweep_and_llvmpipe_exposure -->


<a id="memory-feedback_clang_tidy_local_constexpr_is_local_constant"></a>
## feedback_clang_tidy_local_constexpr_is_local_constant

---
name: feedback_clang_tidy_local_constexpr_is_local_constant
description: "Pinned LLVM 20.1.8 readability-identifier-naming has no LocalConstexprVariable category: a non-static function-local constexpr is a LocalConstant (lower_case); static locals, namespace and class constants stay kCamelCase. Verified 2026-09-13 against the binary and hosted job 103641498236."
metadata:
  node_type: memory
  type: feedback
  recorded: 2026-09-13
---

**Rule.** With the repository `.clang-tidy`, LLVM 20.1.8 reports `constexpr int kX = 1;` inside a function as
`invalid case style for local constant`. The option names `LocalConstexprVariable` and `StaticConstexprVariable` do not
exist in that binary (zero string matches); the two keys were dead and were removed on 2026-09-13. Resolution order in
the check: `ConstexprVariable` (deliberately unset) → `ClassConstant` → `GlobalConstant` → `StaticConstant` →
`LocalConstant`. Probe with the repository config: namespace `kA`, class `static constexpr kB`, function-local
`static constexpr kC` and `static const kD` all pass; function-local `constexpr kE` and `const kF` fail; lower_case
locals pass.

**Why.** The earlier records claimed LLVM 20 had a granular local-constexpr category and that `kMaxPrec`-style locals
must not be renamed. Hosted job 103641498236 (revision 0b858a6) and the local helper both contradicted that, and the
previous CI run had already forced a rename in `test_work_smoke_vulkan.cpp`.

**How to apply.** Name non-static function-local constants lower_case whether `const` or `constexpr`
(`constexpr u32 expected_count = 5U;`). Keep `kCamelCase` for namespace/class-scope constexpr and `static` locals.
Run `scripts/tidy-files.ps1` on changed TUs; CI stops at the first failing TU, so tidy every TU a hosted job never
reached before publishing. CODING carries the same table.

<!-- end-memory:feedback_clang_tidy_local_constexpr_is_local_constant -->

<a id="memory-feedback_repository_guard_parity_windows_linux"></a>
## feedback_repository_guard_parity_windows_linux

---
name: feedback_repository_guard_parity_windows_linux
description: "Repository guard scripts run as .ps1 on Windows and .sh on Linux; both must resolve the repo root in the body, use the same field regex and give the same verdict on either host. On 2026-09-13 three Windows guards had scanned nothing since May and one bash regex could not match a field named exactly like its token."
metadata:
  node_type: memory
  type: feedback
  recorded: 2026-09-13
---

**Rule.** Every CTest guard registered in `tests/foundation/math/CMakeLists.txt` has a PowerShell and a bash body.
Resolve the repository root in the script body (`if ([string]::IsNullOrEmpty($RepoRoot)) { ... }`), never in a
`param` default: PowerShell evaluates that default where `$PSScriptRoot` is empty, `Resolve-Path "/.."` becomes the
drive root, every scope is skipped and the guard prints PASS having scanned nothing. Keep the regexes identical:
the untagged-numeric field test is `\w*TOKEN\w*`, so a field named exactly `depth` matches on both hosts. Strip an
optional drive letter before the line number when a bash guard post-processes `grep -n` output, so a Windows host
run gives the Linux verdict.

**Why.** Hosted run 34726528230 failed `crd-no-malloc-allocator` on all six Linux lanes while every Windows lane
passed it, and failed `crd-no-untagged-physical-numeric` on every Windows lane while Linux passed. Both were
script-parity defects exposed by new code: `check_no_malloc_allocator.ps1`, `check_no_std_math.ps1` and
`check_no_std_sort.ps1` carried the `param` default since 2026-05-28 (the non-ASCII guard had documented the same
bug on 2026-05-13), and `check_no_untagged_physical_numeric.sh` required one character before the token since
2026-05-15.

**How to apply.** After touching a guard, run it through CTest on Windows with no arguments from a build directory
and run the bash sibling with an explicit root; both must report the same files. A guard that passes suspiciously
fast or prints PASS for a known violation has not scanned. Prove a fixed regex with a probe copy that removes the
allow marker.

<!-- end-memory:feedback_repository_guard_parity_windows_linux -->
