# CEIR-13z — the §129 execution proof (session log)

**Date:** 2026-08-10 · **Slice:** CEIR-13z (D-007 master spine, band-13 CLOSE) · **Status:** ◧ IN PROGRESS — design-locked
+ **13z-1a (the execution seam + ADR-0126) DONE + gated (485/485 × 4)**; 13z-1b (`add` on both backends) NEXT · **ADR:** 0126.

## Contract

`add`·`reduce`·`scan`·FFT as CEIR program ASSETS (text AND builder), dispatched Vulkan AND DX12, bit-exact vs the existing
CKIR CPU oracles, hot-reload live-swaps, reference validates host-visible. On close (13z-z): the ADR-0108 cornerstone flip.
Design note: `docs/design/ceir-13z-execution-proof.md` (the 7 forks + the 5-checkpoint decomposition).

## The load-bearing find (13z-1 recon) — two GPU surfaces, two roles

The engine has TWO compute-facing surfaces with incompatible resource types:
- **ADR-0100 `IComputeContext`** (`ComputePipeline` by-name · `ComputeBuffer` · `ComputeRecorder`) — the standalone
  compute-KERNEL execution surface. The CKIR kernels + their CPU oracles + `ckir_kernel_dispatch.hpp` + every device CKIR
  compute test (Vulkan + DX12) live here.
- **RAF-2 `command_model.hpp` `DispatchDesc`/`ICommandEncoder`** (`IGpuProgram` · `IStorageBuffer`) — the raster/frame-graph
  declarative model (`render-graph::record_compute_dispatch`; both raster contexts' `create_command_encoder()`). ⛔ NOT dead
  code (a false "unwired" claim would be its own source≠scoreboard bug — verified the render graph records it).

⭐ **Primary-source divergence SURFACED (not silently switched):** ADR-0125 §2.3 said the lowered list "binds to a
`DispatchDesc` at execute" and `validate_dispatch` runs on it. The evidence (nobody dispatches a `DispatchDesc` for the CKIR
kernel path; every device CKIR test uses `create_pipeline*`→`ComputePipeline`→`ComputeRecorder::dispatch`) contradicts that.
Reconciled with the advisor: `execute_lowered` targets **IComputeContext**; the resolver returns **`ComputePipeline*`** (NOT
`IGpuProgram*` — my fork-C guess corrected); `validate_dispatch`'s semantics move into a typed `ExecuteError`. The ADR-0125
§2.3 + lower.hpp "DispatchDesc" language STRUCK-in-place (source=scoreboard). Both surfaces are legitimate; the partitioner
(21/26) picks per region — 13z picks the kernel surface because that is where the §129 proof kernels + oracles are.

## 13z-1a — what landed (the seam, device-free)

- **ADR-0126** — the two-surface reality; `execute_lowered` on IComputeContext; the resolver contract; the re-homed
  validation; the Barrier-mapping FORK flagged (option a: record the conflicting root on the Barrier at lowering vs option b:
  barrier every `before`-written buffer; + the HazardKind→ComputeAccess map + the fft2d `TransferDst→ShaderRead` scar) →
  resolved at 13z-3; the dispatch-only + `resource_root`-normalize scope pins.
- **`crd/ceir/gpu/execute.{hpp,cpp}`** — `KernelResolveFn` (op→`ComputePipeline*`, null→`UnresolvedKernel`), `ResolvedBinding`
  (Value*→`ComputeBuffer*`), `ExecuteError` (5 kinds), **`validate_lowered`** (PURE device-free) + **`execute_lowered`**
  (records Dispatch into a caller-owned `ComputeRecorder`; Barrier no-op at 13z-1; Transfer/dynamic-grid/Indirect →
  `UnsupportedCommand`, named-forward). Bindings gathered in operand order (grid prefix 3), each `resource_root`-normalized
  then looked up. crd-ceir CORE untouched (I3/I4/I5 hold — bridge only).
- **`tests/ceir-gpu/test_execute.cpp`** (+4) — DEVICE-FREE always-runs (fake resolver returns `user`; fake `ComputeBuffer`;
  fake `ComputeRecorder` COUNTS dispatches): a good dispatch validates + records (2 buffers, grid 1); a `resource.view`
  binding normalizes to its buffer root; the 5 typed error paths; `execute_lowered` refuses a Transfer program +
  records-NOTHING. ⛔ the guard against a device suite that skips every case reading as PASS. Added to the existing
  `crd-ceir-gpu-tests` (already in gate6b.sh — no wiring gap).

**Gate (13z-1a, GREEN):** win-debug **485** · win-asan **485** · linux-gcc-debug **485** · linux-gcc-asan **485** (was 481;
+4). LLVM-20 tidy clean (execute.hpp, execute.cpp, lower.hpp, test_execute.cpp); `-Werror=switch` clean (the `ExecuteError`
switch is total); opgen drift/validator OK; invariants OK (crd-ceir CORE stays host-only). NO recook/fuzz/version-bump.

## NEXT — 13z-1b (`add` on both backends)

A DEVICE-linked test target (links crd-kir + a backend to compile the CKIR `add` kernel to a `ComputePipeline`, a real
`IComputeContext`) authors an `add` CEIR program asset (a single `ceir.compute.dispatch`), lowers it, runs `execute_lowered`
against the device, reads back, and asserts byte-identical to the direct-CKIR path + `==` integer math — device-skip-guarded
(`if (ctx == nullptr) WARN skip`).

## 13z-1b — `add` on device: VULKAN DONE + gated (DX12 next)

The FIRST real on-device execution of a CEIR asset through the 13d→13z pipeline. Advisor-settled structure (per-backend
targets, clone-the-bracket harness, always-runs INSIDE the new target, generic emit read first).

- **`tests/gpu-shared/ceir_execute_1wg.hpp`** (new, both-backend): `build_add_kernel` (a KGraph `c[lid]=a[lid]+b[lid]`, 3 F32
  buffers at bindings 0/1/2) · `build_add_ceir_asset` (a CEIR block: grid const 1 + 3 `resource.declare` + one
  `compute.dispatch(grid×3, a,b,c)` kernel="add" access="r,r,w") · `resolve_single_pipeline` · **`dispatch_ceir_1wg`** — a
  byte-for-byte clone of `dispatch_kernel_1wg` (buffer create / upload copies / `TransferDst→ShaderRead` barriers / readback)
  with `execute_lowered(...)` replacing ONLY the `rec.dispatch(...)` line. ⛔ CONTRACT: CEIR binding-operand order == KGraph
  `buffer_decl` indices 0..n-1 (the 13a positional-slot rule).
- **`tests/ceir-gpu-vulkan/test_ceir_add_vulkan.cpp`** (new target, links `crd-kir-vulkan`): the device test — emit GLSL via
  `emit_compute_kernel_glsl` → `compile_glsl_to_spirv` → `create_pipeline_from_spirv(3,0)`; run the DIRECT path
  (`dispatch_kernel_1wg`) AND the CEIR path (`dispatch_ceir_1wg`) on the SAME pipeline + identical inputs, separate outputs;
  assert **byte-identical** (`memcmp==0`), **`== a+b`** (the CPU oracle; f32 add is one IEEE op → bit-exact everywhere incl
  llvmpipe), and **validation-SILENT** (`ValidationCapture.error_count()==0` under `enable_validation=true`). Plus a
  device-FREE always-runs case (the asset lowers to one Dispatch / 3 bindings / grid 1) — the all-skip false-green guard for
  the NEW target. Ran on a REAL Vulkan device (win-debug/win-asan, 0.5–0.9s) AND llvmpipe (Linux).
- **Wiring:** `tests/ceir-gpu-vulkan/CMakeLists.txt` (mirrors kir-vulkan + crd-ceir/crd-ceir-gpu; `RESOURCE_LOCK
  crd_gpu_device`); `add_subdirectory(ceir-gpu-vulkan)` in tests/CMakeLists.txt (unconditional, self-guarding like the other
  device suites); **only** the Vulkan target added to `gate6b.sh` (DX12 is Windows-only).
- ⛔ **tidy-gate fix (durable, whole-repo):** `scripts/tidy-files.ps1` now also globs `engine/*/generated` — a header that
  includes an opgen-generated engine header (`crd/ceir/gen/*.hpp`) was parsing to **UNGATED** (the 2026-07-09 blind-file
  scar's shape). Fixed at the include-set derivation, matching the scar's "never hand-list" principle.

**Gate (13z-1b Vulkan, GREEN):** win-debug **487** · win-asan **487** (real Vulkan device under ASan — no flake, no leaks) ·
linux-gcc-debug **487** · linux-gcc-asan **487** (llvmpipe). LLVM-20 tidy clean (the harness + the test); `-Werror=switch`;
opgen; invariants (crd-ceir CORE untouched — the execution lives in the test target + the bridge). NO recook/fuzz.

## 13z-1b — DX12 mirror: DONE → 13z-1b CLOSED (both backends)

The DX12 mirror of the Vulkan proof, reusing the SAME `add` KGraph + `build_add_ceir_asset` + the backend-agnostic
`dispatch_ceir_1wg` (it takes an `IComputeContext&`, so DX12 is a drop-in):

- **`tests/ceir-gpu-dx12/test_ceir_add_dx12.cpp`** (new) — `Dx12ComputeContext ctx(&alloc)` (direct ctor; `if (!valid())
  WARN skip`), `emit_compute_kernel_hlsl` → `create_pipeline_from_hlsl(source, 3, 0)`; DIRECT `dispatch_kernel_1wg` vs CEIR
  `dispatch_ceir_1wg` on the SAME pipe → **byte-identical** (`memcmp==0`) + `== a+b`. Ran on a REAL D3D12 device
  (win-debug/win-asan, 0.3–0.5s). Plus the device-free lower-shape always-runs case.
- **`tests/ceir-gpu-dx12/CMakeLists.txt`** — ⛔ guarded `if(NOT TARGET crd-kir-dx12) return()` (the kir-dx12 idiom — Windows-
  only; Linux has no D3D12 backend, so the target is never created there). Links `crd-gpu-context-dx12` EXPLICITLY (its
  include dir is not re-exported by crd-kir-dx12 — the one build fix this tick). `DISCOVERY_MODE PRE_TEST` + `RESOURCE_LOCK
  crd_gpu_device`. `add_subdirectory(ceir-gpu-dx12)` in tests/CMakeLists.txt; ⛔ NOT in gate6b.sh (Windows-only).
- ⛔ **ValidationCapture-silent stays VULKAN-ONLY:** gpu-context-dx12 has no info-queue capture mechanism (test_dx12_compute
  asserts none either) — the DX12 equivalent is a named-forward, exactly as the design-note DoD table pins it. The DX12 test
  asserts byte-identity + oracle, not validation-silence.

**Gate (13z-1b DX12 + close, GREEN):** win-debug **489** · win-asan **489** (Windows carries the +2 DX12 tests; real D3D12
device under ASan — no flake) · linux-gcc-debug **487** · linux-gcc-asan **487** (DX12 guarded out on Linux — honest
per-platform counts). Tidy clean (the DX12 test); `-Werror=switch`; opgen; invariants. ⭐ **CEIR-13z-1b CLOSED — the `add`
CEIR asset executes on BOTH Vulkan AND DX12 real devices, byte-identical to the direct CKIR dispatch.**

## 13z-2a — reduce on both backends + the text-round-trip probe (DONE + gated)

Advisor-decomposed (four verdicts + two landmines). ⛔ **Single-workgroup variants ONLY, and the reason is load-bearing, not
convenience:** `execute_lowered` treats a `Barrier` as a NO-OP until 13z-3, so a multi-dispatch CEIR asset would execute with
NO inter-pass barriers and could **pass by luck** on some hardware (the pixel-blind false-green shape, worse than a failure).
So the full device-wide multi-pass reduce/scan wait for the 13z-3 Barrier→ComputeAccess map; 13z-2 uses the single-workgroup
`build_reduce_block` / `build_scan_block`.

- **The D-PROBE (fork D, answered FIRST):** a `compute.dispatch` module whose `kernel` attr is a **dangling `@add`** (defined
  nowhere) round-trips through the CEIR text form byte-exact (`print`→`parse`→`print`), and the parsed module lowers to the
  same one-dispatch command list. So the §121 text-authoring path works for a pure symbol-REFERENCE attr (parse.hpp's
  re-registration covers symbol-DEFINING ops; this was the unverified case). `test_execute.cpp` +1.
- **Harness generalized (module-wrapped):** `build_ceir_dispatch_asset(c, kernel, access, nbufs)` → a `CeirDispatchAsset`
  {`module` (for print/parse), `block` (for lower), `binds[]`}. ⛔ parse/print operate on MODULES, so the asset wraps
  `create_module()` + body-append (the block stays transparent to `lower_region`). A back-compat `build_add_ceir_asset`
  wrapper keeps the 13z-1b add device tests unchanged.
- **reduce on BOTH backends** (`build_reduce_block(g, 64, 64, KOp::Add)`, 2 buffers `r,w`): the `reduce` CEIR asset lowers →
  runs on real Vulkan (0.33s) + DX12 (0.27s) + llvmpipe, **byte-identical to the direct CKIR dispatch** + `== the sum
  oracle` + validation-silent (Vulkan). ⛔ **landmine 1 (association order):** a workgroup tree-reduction sums in a different
  order than a sequential CPU loop, so with fractional data `==` would fail by design — the inputs are **integer-valued f32**
  (`in[i]=i`, sum 2016 < 2²⁴) so EVERY association order is exact and the naive `==` oracle is legitimate (no tolerance —
  the fork-F table holds). The add's `i*0.5` data does NOT carry to reduce.

**Gate (13z-2a, GREEN):** win-debug **492** · win-asan **492** (real Vulkan+DX12 under ASan) · linux-gcc-debug **489** ·
linux-gcc-asan **489** (reduce-vulkan on llvmpipe; DX12 guarded out — honest per-platform counts). Tidy clean (harness +
3 tests); `-Werror=switch`; opgen; invariants. NO recook/fuzz.

## 13z-2b — scan + the full §121 text-authoring: DONE → 13z-2 CLOSED

- **scan on both backends** (`build_scan_block(g, 64, 64, inclusive=true, write_blocksum=TRUE)`, 3 buffers `r,w,w`): the
  `scan` CEIR asset lowers → runs on real Vulkan + DX12 + llvmpipe, **byte-identical to the direct CKIR dispatch** + `== the
  inclusive-prefix oracle` (integer inputs → the prefix sums k(k+1)/2 are exact) + validation-silent (Vulkan). ⛔ **landmine 2
  handled:** write_blocksum=TRUE (not false) so all 3 declared buffers are LIVE (an unreferenced buffer decl on device is
  unverified) — and `bsum[0] == total` is a free extra oracle assert.
- **The full §121 text-authoring, proven end-to-end:** an EMBEDDED hand-authored text literal (first draft captured from
  `print(builder)` — a temporary `WARN`, removed) is now authored source. Device-free (crd-ceir-gpu-tests, all 4 configs):
  `print(parse(text)) == print(builder)` byte-exact + both lower to the same command list — the §121 text≡builder discipline
  across the two authoring paths (the assert is on the CANONICAL print, so the literal need only PARSE). ⭐ Device (Vulkan):
  a **parsed-from-text reduce asset EXECUTES on a real GPU == oracle** — the text-authoring path reaching real execution.
  Text-on-both-backends is ceremony: byte-equal canonical modules + deterministic 13d lowering ⇒ the builder's DX12 run
  carries the text asset (written down, not silently skipped).
- ⛔ **ASCII-only test-name scar re-hit:** a `§` in a TEST_CASE name broke ctest's Windows name-matching (`--filter` encoding
  mismatch → "no tests ran" → reported as a failure). Renamed to ASCII; the mandate's rule confirmed the hard way.

**Gate (13z-2b + close, GREEN):** win-debug **496** · win-asan **496** (real Vulkan+DX12 under ASan) · linux-gcc-debug **492**
· linux-gcc-asan **492** (reduce/scan/text-leg on llvmpipe; DX12 guarded out — honest per-platform counts). Tidy clean
(harness + 3 tests); `-Werror=switch`; opgen; invariants. NO recook/fuzz. ⭐ **CEIR-13z-2 CLOSED — reduce + scan run as CEIR
assets on both real GPU backends, byte-identical to the direct CKIR path, and the text-authoring path executes on device.**

## 13z-3 part 1 — the 13d BARRIER COMPLETION (DONE + gated)

FFT is the multi-dispatch chain, so 13d's barrier derivation comes due. Advisor-reviewed decomposition (4 forks + a WAR
wrinkle); this tick landed the load-bearing lowering change (the FFT itself is part 3).

- **Fork A → option (a), per-resource (NOT a transition list — that would nest a container in the flat value entry):**
  `LoweredCommand::Barrier` gains ONE `const Value* resource` field (appended). `lower_region` now emits, before each
  dispatch, **one Barrier per conflicting root resource** the dispatch touches, in `after`'s binding-operand order — each
  `{hazard, before=the nearest earlier writer, after, resource}`. ⭐ **a 13d CORRECTNESS COMPLETION, not just execution
  convenience:** the part-2 "one barrier per dispatch, nearest-strongest" DROPPED conflicts — a dispatch reading N buffers
  written by N prior passes recorded only ONE, and the multi-dispatch FFT is exactly where the second conflict is a REAL
  data race. The whole-op `precise_hazard` was retired; the per-pair `conflict`/`pair_hazard`/`hazard_rank` primitives ARE
  the per-resource core (two-hazard-notions design intact — this bridge scan stays narrowed, core `ops_hazard` conservative).
- **`test_lower.cpp` +1 discriminating test:** A writes bufA · B writes bufB · C reads BOTH → C gets **two** RAW barriers
  (bufA from A, bufB from B, in binding order) where the old model emitted one and dropped a conflict. All 5 existing 13d
  tests pass unchanged (single-buffer → same count, now with a resource).
- **ADR-0126 §3.3 RESOLVED** (option a, per-resource; the correctness-completion framing). The `HazardKind→ComputeAccess`
  map + `rec.barrier` emission are part 2 (dispatch→dispatch only; the fft2d `TransferDst→ShaderRead` upload barrier is the
  HARNESS's by construction — a dispatch-only asset has no transfer, so `execute_lowered` never sees the transfer producer).

**Gate (13z-3 part 1, GREEN):** win-debug **497** · win-asan **497** · linux-gcc-debug **493** · linux-gcc-asan **493**
(+1 test). Tidy clean (lower.hpp, lower.cpp, test_lower.cpp); `-Werror=switch`; opgen; invariants (crd-ceir CORE untouched —
the change is bridge-only). NO recook/fuzz.

## 13z-3 part 2 — execute_lowered REPLAYS the barriers (DONE + gated)

- **`execute_lowered` replays** each Barrier as `rec.barrier(root_buffer, from, to)` via a `hazard_access` map:
  RAW→`ShaderWrite→ShaderRead`, WAW→`ShaderWrite→ShaderWrite`, WAR→`ShaderRead→ShaderWrite` (no `default` → -Werror=switch
  forces a case per HazardKind). `cmd.resource` (the lowering's root) is looked up in the binding table; a nullptr (ambient)
  resource barriers EVERY bound buffer.
- ⭐ **WAR wrinkle VERIFIED on both backends** (the advisor's Fork-B): Vulkan's `barrier` issues `vkCmdPipelineBarrier`
  with COMPUTE→COMPUTE stages + access masks → a real execution dependency (the write waits for the read); DX12 **ignores
  `from`** and issues a `to`=ShaderWrite→`UNORDERED_ACCESS` UAV barrier (direction-agnostic serialization). So the map is
  safe as written — no conservatism needed. Pinned in code + ADR-0126 §3.3.
- **FakeRec extended** to RECORD barrier calls (count + last buffer + last from/to). +2 device-free tests (all 4 configs —
  no GPU): a write-then-read chain lowers to one RAW barrier on the shared root, and `execute_lowered` fires exactly one
  `rec.barrier` on the mapped buffer with `ShaderWrite→ShaderRead`; a 2-buffers-from-2-writers program fires **two**
  per-resource barriers (the completion, proven at execute). ⛔ dispatch→dispatch only (the harness owns the upload barrier).

**Gate (13z-3 part 2, GREEN):** win-debug **499** · win-asan **499** · linux-gcc-debug **495** · linux-gcc-asan **495**
(+2 device-free tests). Tidy clean (execute.hpp, execute.cpp, test_execute.cpp); `-Werror=switch`; opgen; invariants
(bridge-only). NO recook/fuzz.

## 13z-3 part 3 — the 6-dispatch 2D FFT (VULKAN DONE + gated; DX12 next)

⭐ The on-device barrier stress — a MULTI-DISPATCH CEIR asset executes bit-exact on real hardware, validating the whole
barrier machinery (part-1 per-resource lowering + part-2 execute replay) on a real transform.

- **The multi-dispatch harness** (`ceir_execute_1wg.hpp`): `build_ceir_multi_asset(nbuffers, MultiPass[], npasses)` → a
  MODULE of N `resource.declare` + M `compute.dispatch` (each pass its own grid=(nwg,1,1) + access + binds + kernel symbol);
  `MultiResolve`/`resolve_multi` (a dispatch op → its pipeline BY IDENTITY — a flat parallel array, NO registry, stateless
  across validate/execute); `dispatch_ceir_multi` — a byte-for-byte clone of `dispatch_fft2d` with `execute_lowered`
  REPLACING the manual per-pass dispatch+barrier loop (persistent buffers, the HARNESS owns the upload `TransferDst→
  ShaderRead` barrier — the asset is dispatch-only).
- **The FFT asset** mirrors `build_fft2d_c2c` (rr=cc=64, tile=16 → 6 passes, 14 buffers, 4 unique kernels: row-FFT · transpose
  R×C · col-FFT · transpose C×R). Honest per-pass access: FFT passes `r,r,r,r,w,w` (4 in, 2 out), transpose `r,w`. ⭐ the CEIR
  lowering derives the inter-pass PER-RESOURCE barriers — **pass 3 (col FFT) gets TWO**: RAW on `b_tr_re` (written by pass 1)
  AND `b_tr_im` (written by pass 2) — the part-1 completion is LOAD-BEARING here (one barrier would drop a real dependency).
  execute_lowered replays them. **BIT-EXACT vs the CPU oracle** (`run_fft2d_cpu` — the same gate the direct `dispatch_fft2d`
  test uses) on real Vulkan (0.99s) + llvmpipe (Linux) + validation-silent. (All FFT hazards are RAW — a forward pipeline,
  no WAR/WAW — as the advisor noted.)

**Gate (13z-3 part 3 Vulkan, GREEN):** win-debug **500** · win-asan **500** (multi-pass FFT under ASan) · linux-gcc-debug
**496** · linux-gcc-asan **496** (FFT on llvmpipe). Tidy clean; `-Werror=switch`; opgen; invariants. NO recook/fuzz.

## 13z-3 part 3 DX12 — the FFT on D3D12: DONE → 13z-3 CLOSED (both backends)

The DX12 mirror, reusing the SAME `build_ceir_multi_asset` + `dispatch_ceir_multi` (backend-agnostic — only the context +
emitter differ): `tests/ceir-gpu-dx12` gains the 6-dispatch 2D FFT test with `emit_compute_kernel_hlsl` →
`create_pipeline_from_hlsl`. The FFT CEIR asset runs **bit-exact vs the CPU oracle** on a REAL D3D12 device (0.54s) — the
per-resource inter-pass barriers (pass 3's two) carry correctly through DX12's UAV-barrier model too. ⛔ ValidationCapture
stays Vulkan-only (gpu-context-dx12 has no capture — the named gap). A near-mechanical drop-in (compiled + passed first try).

**Gate (13z-3 part 3 DX12 + close, GREEN):** win-debug **501** · win-asan **501** (real DX12 FFT under ASan) · linux-gcc-debug
**496** · linux-gcc-asan **496** (DX12 guarded out — honest per-platform counts). Tidy clean; `-Werror=switch`; opgen;
invariants. NO recook/fuzz. ⭐ **CEIR-13z-3 CLOSED — the 6-dispatch 2D FFT runs as a CEIR asset, bit-exact, on real Vulkan +
llvmpipe + real DX12; the whole barrier machinery (part-1 per-resource lowering + part-2 execute replay) validated on a real
multi-dispatch transform, both backends.**

## NEXT — 13z-4 (the reference leg + hot-reload), then 13z-z (band gate + the flip)

- **13z-4:** the two remaining §129 DoD legs. (1) The reference-executor leg (fork G): a BRIDGE-side CPU-oracle run of the
  lowered list validates host-visible semantics — the core Interpreter STAYS `NoSemantics` for dispatch (the §150 forward, I5
  intact). (2) Hot-reload live-swap: a kernel-ref rebinds to a new CKIR program via the 10a ReloadSet; the CEIR asset
  re-lowers + re-executes, the output tracks the swap.
- **13z-z:** the BAND-13 GATE (the §129 proof composes: all 4 kernels — add/reduce/scan/FFT — both backends, text+builder,
  oracle-exact, hot-reload) + ⚠ the ADR-0108 CORNERSTONE FLIP (PRINCIPLES/AGENTS/README/ROADMAP C++-only → CEIR/CHIR + the
  ADR-0081 §9 in-file strike, as ONE user-committed commit — §5 gate 2, ALREADY user-authorized). Band-13 header ◧ → ✅.
