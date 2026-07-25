# 2026-07-25 — CI determinism: the cook content-hash, and the win-tidy gate

> **Goal (user-directed):** make CI **completely green, deterministically** — no "retry until it passes",
> no disabled checks standing in for a fix. Targeted verification only (re-run the problematic parts of
> the sweep, not the whole matrix).
>
> Two blockers had been handed over as "pre-existing, not quick-fixable". **Both diagnoses were wrong**,
> and each turned out to be two defects rather than one. All four are root-caused and fixed below.

---

## 1. Parallel-cook dedup non-determinism — it was **struct padding in the content hash**

**Reported as:** "address-dependent ordering in the KIR→GLSL emitter; ASan randomizes the heap and exposes
it." A glslang serialization mutex had been added on the theory that shaderc carried process-global state.

**Actually:** `serialize_graph` blasted the POD pools raw
(`wbytes(arr.data(), n * sizeof(KNode))`), so every **indeterminate padding byte** of
`KNode`/`KStmt`/`KType`/`KEntry` landed in the content hash. The builders default-initialize (`KNode n;`),
so padding holds whatever the stack held — the "content" hash was a function of stack history.

* **Reproduced** with a purpose-built gate (`tests/kir/test_ckir_serialize_determinism.cpp`): build the
  same graph twice with a `noinline dirty_stack(0xAA)` / `dirty_stack(0x55)` between them, compare bytes.
  Green in win-debug (MSVC `/RTC1` 0xCC-fills locals deterministically — which is exactly why win-debug
  masked the whole bug), **red in win-asan at byte 33** = `KNode+1`, the hole between `KOp op` and the
  2-aligned `KType type`.
* **Fix (root):** `ckir_serialize.hpp` now writes a **canonical, packed, padding-free record** per element,
  field by field, little-endian, with the f64 as its bit pattern. Format version bumped to 2; the manifest
  now carries canonical record widths, and `static_assert`s pin the three array extents the widths derive
  from. Bonus: the cooked artifact is now **ABI-independent** (a blob cooked by MSVC loads under gcc/clang
  instead of being rejected by a `sizeof` manifest).

**Second, independent instance in the same slice.** After the graph hash was fixed, D10 (parallel ==
serial) and D12 (recook identity) still failed. `reflect()`'s `ShaderReflection` is written **raw** into the
bundle's `REFL` chunk, so *its* padding is part of the cooked bytes. Diffing the two cache files pinned the
divergence at file offsets **1881..1883** — exactly the 3-byte hole after `KStage stage`.
`ShaderReflection r{}` is **not** sufficient under MSVC (it implements value-init of a class with member
initializers as "run the implicit default ctor", writing members only); an explicit `std::memset` is,
plus field-by-field element writes so a brace-built temporary's padding is never copied in.

**The glslang mutex is removed** — it was never the cause and it serialized the parallel cook for nothing.

**Audited the rest of the tree for the same class** (every other `reinterpret_cast<const u8*>(&pod)` chunk
write): `PresetInfo` · `ProfileFileInfo` · `ObekInfo` · `SceneInfo` · `PbrmParams` · `PbrmTextures` are all
uniformly-aligned and each carries a `static_assert(sizeof(T) == <field sum>)` pinning it **padding-free** —
which is exactly the discipline the KIR pools lacked, because `KNode`/`KStmt`/`KEntry` were never designed
as an on-disk layout; they got serialized opportunistically. No other site is affected.

**Verified (cold cache, 3× each):** `D3 / D5 / D6 / D8 / D10 / D12` — **8/8 green in win-asan and 8/8 green
in win-shipping**, three consecutive runs per config.

## 2. `build/win-shipping` had the `#deps 0` landmine live

Found while verifying: win-shipping's `msvc_deps_prefix` was the **English** `"Note: including file: "` while
this Turkish-locale host's `cl.exe` emits `"Not: eklenen dosya:"` ⇒ ninja recorded **zero header
dependencies**, so header edits never recompiled there. A build of the vulkan tests after a header change
reported `0 Building CXX` and relinked stale objects. Repaired per `docs/BUILDING.md`: **wiped** the dir
(a reconfigure alone leaves the stale `#deps 0` objects marked VALID) and reconfigured with the standalone
CMake; prefix verified Turkish, full rebuild green. All other Windows build dirs audited — only
win-shipping was affected (the clang-cl dirs legitimately use the English prefix).

## 3. The win-tidy "non-deterministic AVX-512 clang-tidy crash" is **commit exhaustion**

**Reported as:** an upstream LLVM 20.1.8 matcher bug on AVX-512 intrinsic headers, "not deterministically
fixable"; two checks had been disabled in `.clang-tidy` to dodge it.

**Actually:** the sweep log's real message is `Error running clang-tidy.exe: LLVM ERROR: out of memory`
with `Exception Code: 0xC000001D` (STATUS_ILLEGAL_INSTRUCTION — LLVM's own abort), **not** `0xC0000005`.
Evidence:

| measurement | result |
|---|---|
| peak WS of a clang-tidy edge (5 heavy TUs) | **200–300 MB** |
| the crashed files, run standalone ×5 | **0/5 crashes** |
| the crashed file via the **exact** `__run_co_compile` command ×6 | **0/6 crashes**, 289 MB peak |
| host commit during the sweep | **83.3 GB used of a 96.2 GB limit** (devenv 8.7 GB, DAW 3.2 GB, …) |

`malloc_allocator.cpp` — a tiny file — crashing at `<eof>` is the clincher: not TU complexity, ambient
pressure. Whichever process asks for memory when headroom runs out is the one that dies, which is exactly
why it was "a different check on a different file every run" and why disabling checks never converged.
(Both signatures are the same cause: a heap request fails ⇒ `LLVM ERROR: out of memory`; a stack growth
that cannot commit ⇒ `0xC0000005`.)

**Fixes:** the two speculative `.clang-tidy` disables are **reverted**, and both sweep scripts gained a
**commit-headroom preflight** that measures free commit, prints it with the top consumers, and clamps
`-BuildJobs` to what fits — so a random OOM can never again masquerade as a code failure.

## 4. clang-tidy silently **drops every `/`-spelled MSVC flag** — the gate analysed a config we don't ship

Verified with an `#error`-guarded probe through the exact `cmake -E __run_co_compile` path: `/EHsc`,
`/arch:AVX2` and `/D<macro>` **never reach the TU**, while the same flags passed via `--extra-arg=` do.
(CMake spells `-I` and user `-D` with dashes, which is why most of the build looked fine.) Two silent
consequences:

1. **Exceptions look disabled** ⇒ any `try` is a hard `clang-diagnostic-error`. This — not the crash — is
   what actually failed `engine/platform/src/filesystem.cpp`, and it also meant
   `bugprone-exception-escape` could never fire anywhere.
2. **`__AVX2__` is undefined** ⇒ every AVX2-guarded path is preprocessed out and **never analysed**. The
   gate read green on code it never looked at — the same disease as the "tidy gate clean on unparsed
   files" scar.

**Fix:** `--extra-arg=/EHsc` + `--extra-arg=${CRD_SIMD_MSVC_ARCH_FLAG}` in the `CRD_ENABLE_CLANG_TIDY`
block of the root `CMakeLists.txt`. The ISA flag is exported by the **same** `CrdSimd.cmake` branch that
adds the real compile option, so the two cannot drift apart. `scripts/tidy-files.ps1` mirrors it.

### Fallout: 120 previously-invisible findings, all fixed

The win-tidy build had been dying at edge ~18/1070 for a long time, so the whole tail of the tree was
**ungated**. With the flags corrected, a keep-going build enumerated 120 findings:

| check | count |
|---|---|
| `readability-identifier-naming` | 64 |
| `readability-isolate-declaration` | 30 |
| `bugprone-exception-escape` | 9 |
| `cppcoreguidelines-pro-type-const-cast` | 7 |
| `readability-avoid-nested-conditional-operator` | 6 |
| `bugprone-misplaced-widening-cast` | 3 |
| `bugprone-unhandled-exception-at-new` | 1 |

Handled as: `clang-tidy --fix` for the two mechanical checks (**diff reviewed** — it renamed a lambda
`O`→`o` onto an existing `o`, and `ob_`→`ob` onto an existing `ob`, both of which had to be repaired by
hand), and by-hand fixes for the rest — `noexcept` bodies given real catch handlers that *record* the
loss (`dropped_count()` for a lost log sink; new `record_failures` / `capture_alloc_failures` counters in
the test doubles) rather than swallowing, `new (std::nothrow)` + a null check in the profiler's ring
allocation, `const_cast` removed by making the eval-buffer parameters non-const, nested ternaries replaced
by `crd::math::clamp` / explicit `if`, and widening casts moved onto the operands.

An empty `catch (...) {}` trips `bugprone-empty-catch`, so **every** catch added in this pass carries a
meaningful statement.

---

## Verification (targeted — no full 18-config sweep)

* **win-asan** — `D3/D5/D6/D8/D10/D12` **8/8 green ×3** (cold cache each run) + the new D1 determinism
  gate 5/5.
* **win-shipping** (freshly rebuilt after the `#deps` repair) — `D3/D5/D6/D8/D10/D12` **8/8 green ×3**.
* **win-debug** — full `all` build clean; `log|perf|kir|serialize` **174/174 green**.
* **win-tidy** — full build, **0 findings**.
* Every file touched in this session re-gated with `scripts/tidy-files.ps1`: **clean**.

## Files

`engine/kir/include/crd/kir/ckir_serialize.hpp` (canonical encoding + `reflect()` memset) ·
`engine/gpu-context-vulkan/src/vulkan_glsl_compile.cpp` (mutex removed) ·
`tests/kir/test_ckir_serialize_determinism.cpp` (**new** gate) · `CMakeLists.txt` + `cmake/CrdSimd.cmake`
(tidy extra-args) · `scripts/{per-slice-check,full-sweep,tidy-files}.ps1` · `.clang-tidy` (disables
reverted) · `engine/log/{logger.cpp,sinks/file_sink.cpp}` · `engine/perf/src/profiler.cpp` ·
`engine/platform/src/dynamic_library.cpp` · `runtime/src/main.cpp` · `runtime/examples/smoke_math.cpp` ·
`tests/{log,perf,kir,gpu-context-vulkan,gpu-context-dx12}/…` (the 120 findings).

## Standing host note — the sweep is bounded by COMMIT, not CPU

On this host the commit limit is 96.2 GB and resident desktop tooling holds ~83 GB of it: `devenv` 9.4 GB,
**three `clangd` instances totalling 10.5 GB**, Studio One 3.2 GB, explorer 3.1 GB, copilot-language-server
1.4 GB, Opera ~3.7 GB, Defender 1.8 GB. That leaves ~12 GB for the whole build. It is not only clang-tidy
that dies there — **`cl.exe` itself took a `0xC0000005` on `/O2` hesap TUs at `-j5`** (a different file each
time: `test_validation.cpp`, then `test_feast.cpp`), while the *same* win-shipping build had completed at
`-j10` earlier in the session, before clangd started indexing. Same mechanism, different victim.

**User decision (2026-07-25): repo-side clamp only** — the host stays as it is, no page-file change. So the
clamp is **per config and re-measured immediately before each build**, not a one-shot preflight:

| config | budget per concurrent edge |
|---|---|
| win-tidy | 1.5 GB (clang-tidy ~0.3 GB + a `/Od` cl.exe) |
| win-debug / win-debug-scalar / win-debug-sse2 / win-clang-cl | 2.0 GB |
| win-asan | 2.5 GB |
| win-relwithdebinfo | 3.0 GB |
| win-release / win-shipping / win-shipping-profile / win-clang-cl-shipping | 4.0 GB |

**The budgets are MEASURED, not estimated** (instrumented build, file-captured):

| what | peak |
|---|---|
| clang-tidy edge | 0.20–0.30 GB |
| `/Od` cl.exe edge | 0.36 GB |
| ⛔ **LTCG `link.exe`** | **5.6 GB** — links, not compiles, are the shipping/release constraint |

win-shipping **crashed** cl.exe at `-j5` with ~14.5 GB free; it **succeeded** at `-j3` while bottoming out at
**0.26 GB free commit** — 260 MB from the limit. So the boundary on this desktop is ~4 GB/edge effective for
shipping, which is what the table encodes.

`jobs = min(-BuildJobs, floor((free_commit - 2 GB reserve) / per_edge))`, printed every time, in both
`scripts/per-slice-check.ps1` and `scripts/full-sweep.ps1` (the latter via `Set-CrdBuildJobs` inside the
generated inner script, so each of the 11 Windows configs is sized on its own). When it clamps it says
**"a random clang-tidy/cl crash under low headroom is NOT a code defect"**, so the next reader does not
spend a session chasing a ghost the way this one started.

Closing Visual Studio / clangd / the DAW before a sweep remains the fastest way to make it both fast *and*
crash-free — the clamp keeps it correct either way, just slower when the desktop is full.
