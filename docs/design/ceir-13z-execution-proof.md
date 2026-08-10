# CEIR-13z — the §129 execution proof (design note, stage 1)

**Slice:** CEIR-13z (D-007 master spine, band-13 CLOSE) · **Status:** ◧ DESIGN-LOCKED (advisor-consulted 2026-08-10;
implementation starts 13z-1 next tick — the 11a/11b stage-1 precedent) · **ADR:** 0126 (at 13z-1).

## The contract (tracker row + §129 / §170)

`add`·`reduce`·`scan`·FFT as CEIR program **ASSETS** — authored **text AND builder** — lowered via 13d's `lower_region`,
**dispatched on a live Vulkan AND DX12 device**, ValidationCapture-silent, **bit-exact vs the existing CPU oracles**,
**hot-reload live-swaps**, and the **reference executor validates host-visible semantics**. ⚠ On close (13z-z): execute
ADR-0108's deferred **cornerstone flip** (PRINCIPLES/AGENTS/README/ROADMAP C++-only → CEIR/CHIR + the ADR-0081 §9 in-file
strike) as ONE commit — THIS is the "first CEIR vertical slice" (§5 gate 2).

## What already exists (13z WIRES, does not build)

- **The kernels + CPU oracles:** `engine/kir/include/crd/kir/ckir_{reduce,scan,fft}.hpp` — the CKIR imperative compute
  kernels AND their CPU oracles (`eval_cpu_kernel` / the direct-DFT reference). `add` is the trivial element-wise kernel.
- **The both-backend dispatch harness:** `tests/gpu-shared/ckir_kernel_dispatch.hpp` — uploads host buffers → GpuOnly device
  buffers, dispatches over the portable `crd::gpu::IComputeContext` / `ComputeRecorder` surface, reads back. Multi-pass FFT
  driver (`dispatch_fft2d`) with the explicit upload→pass-0 barrier already handled. This is the harness 13z mirrors.
- **The device suites + skip idiom:** `tests/gpu-context-vulkan/*`, `tests/gpu-context-dx12/test_dx12_compute.cpp` — the
  `if (ctx == nullptr) { WARN("no ... device; skipping"); return; }` soft-skip.
- **The lowering seam:** 13d `lower_region(ctx, block) → Array<LoweredCommand>` — the unresolved kernel-ref + binding Value*s
  ride each command's `op` back-pointer; `validate_dispatch` rejects a null program at execute (the 13d-1b pin).

## The forks (advisor-confirmed)

- **A — execute seam I/O model → A1 (standalone function, NOT IExecutionProvider).** `IExecutionProvider::execute` returns
  host-visible `i64` scalars; GPU compute I/O is BUFFERS, and nothing consumes `advertises()` for GPU until the partitioner.
  Ship **`execute_lowered(ctx, commands, IComputeContext&, resolver, bindings)`** in crd-ceir-gpu (buffer-level). The
  `IExecutionProvider` wrapping is a **21/26 named-forward** (record in the ADR). This is the natural continuation of 13d's
  compile≠run split — the lowering made the list; 13z binds + runs it.
- **B+C COLLAPSE — no crd-kir edge; a caller-supplied resolver.** The kernel resolver is a **fn-ptr returning
  `IGpuProgram*`** (a gpu-context type the bridge already links) — the **test target links crd-kir** to compile the CKIR
  kernels and hands the resolver in (the 13c cook `KernelResolveFn` precedent; ADR-0125 §2.1 amendment-by-narrowing). ⛔ Do
  NOT add the ADR-0109 §4.2 kir edge until a PRODUCTION resolver lives in the bridge. Resolution failure → null program →
  `validate_dispatch`'s existing `NullProgram` rejection (an always-runs, device-free test).
- **D — decomposition (see below).** The executor is backend-AGNOSTIC (`IComputeContext&`); `ckir_kernel_dispatch.hpp`
  already abstracts both backends, so "add on Vulkan" + "add on DX12" is likely ONE checkpoint. ⭐ FFT is not merely the 4th
  kernel — it is the **multi-dispatch chain with inter-pass barriers**, the first real stress of 13d's barrier derivation →
  order it LAST (after the seam is proven on single-dispatch `add` and simple chains `reduce`/`scan`).
- **E — the cornerstone flip is ALREADY authorized.** ADR-0108 was user-approved INCLUDING the deferred action ("on close,
  execute the flip as ONE commit"). Agents never commit anyway → at 13z-z make the edits, propose the ONE commit
  prominently, the user commits. The commit IS the ratification gate; do NOT re-ask permission ticks ahead.
- **F — the honest per-kernel comparison (verified, and it STRENGTHENS the claim).** The existing `ckir_fft` device tests
  compare vs a direct DFT at **f32 tolerance** (`CHECK(bad == 0)` over an f32-tol count), with bit-exact SUB-cases (unit
  impulse → all-ones; DC bin = exact integer sum; transpose). So the row's "bit-exact vs oracle" is honest for int, f32-tol
  for the float FFT math. ⭐ The stronger, always-legitimate claim 13z pins: **the CEIR-asset path output is BYTE-IDENTICAL
  to the direct-CKIR-path output** for ALL FOUR kernels (same kernel, same device, same data — the CEIR authoring/lowering/
  binding indirection must not perturb a single byte). Then each kernel's MATH correctness keeps its established convention:
  | kernel | CEIR-path vs direct-CKIR-path | math vs oracle |
  |---|---|---|
  | add | `==` byte-identical | `==` (integer) |
  | reduce | `==` byte-identical | `==` (integer) |
  | scan | `==` byte-identical | `==` (integer) |
  | FFT | `==` byte-identical | f32-tol vs DFT + bit-exact impulse/DC sub-cases (the ckir_fft convention) |
  This is the source=scoreboard framing — the "bit-exact" wording is honored where it's true (the path equivalence + int
  math) and the FFT-math f32-tol is surfaced, not silently weakened.
- **G — the reference-executor leg (the fork the advisor surfaced).** Dispatch/transfer/resource ops are typed
  `NoSemantics` in the core Interpreter (the 12a named-forward: reference semantics land "when the resource runtime +
  provider binding land, 13+/§150"). "Reference executor validates host-visible semantics" therefore needs a decision.
  **Shape pinned (confirm at the checkpoint):** the reference is a **bridge-side CPU execution** of the SAME lowered list
  against the existing CKIR CPU oracle (`eval_cpu_kernel`) — the device output is compared to that CPU reference (the §118
  oracle). ⛔ The crd-ceir CORE Interpreter STAYS `NoSemantics` for dispatch (the §150 forward preserved, I5 intact — the
  core never learns about kernels/assets). The host-visible SCALAR skeleton (a CEIR program that dispatches a reduce and
  returns the reduced scalar via `func.return`) is what the core reference validates; the buffer-level oracle is the
  bridge's. This may be its own checkpoint (13z-4).

## Decomposition (checkpoints — each gates; the row flips at 13z-z)

1. **13z-1** — ADR-0126 + `execute_lowered` (bind LoweredCommand → `DispatchDesc`/`TransferDesc` → `IComputeContext`;
   resolver fn-ptr; barriers → command_model barriers) + **`add`** on the portable surface (both backends via
   `ckir_kernel_dispatch.hpp`), device-skip-guarded, + the **always-runs** device-free tests (resolver-fails → typed error;
   null-program rejection). The seam proven on a single dispatch.
2. **13z-2** — **`reduce`** + **`scan`** (simple multi-dispatch / single-pass) as CEIR assets, **authored text AND builder**
   (the §121 text≡builder discipline), byte-identical CEIR-path-vs-direct-CKIR + int `==` math.
3. **13z-3** — **FFT** — the multi-dispatch chain: 13d's inter-pass barrier derivation is STRESSED here (the ping-pong /
   transpose passes); byte-identical vs `dispatch_fft2d`, f32-tol vs DFT + the bit-exact impulse/DC sub-cases.
4. **13z-4** — the reference-executor leg (fork G) + **hot-reload live-swap** (a kernel-ref rebinds to a new CKIR program via
   the 10a ReloadSet; the CEIR asset re-lowers + re-executes, output tracks the swap) — the two remaining §129 DoD legs.
5. **13z-z** — **BAND-13 GATE** (the §129 proof composes: all 4 kernels, both backends, text+builder, oracle-exact,
   hot-reload) + **the ADR-0108 cornerstone flip** (PRINCIPLES/AGENTS/README/ROADMAP + ADR-0081 §9 strike) proposed as ONE
   commit, user-surfaced. Band-13 header ◧ → ✅.

## Mechanical gate items (before checkpoint 1 — the sandbox-false-green guards)

- **New device-test target** → add to `gate6b.sh`'s HARDCODED build list (else the Linux gate silently never builds it — the
  1a scar) AND list its sources explicitly in the tests CMake (tests/ceir-gpu lists sources explicitly).
- **Guard the all-skip false-green:** a device suite that skips every case reads as PASS. Every checkpoint MUST include
  always-runs, device-free tests (resolver-fails → typed error; null-program → `NullProgram`; the lowered-list shape) so the
  target has real assertions even with no GPU. (The 4-config gate's win-debug/win-asan likely have a real GPU; linux-gcc via
  WSL is llvmpipe — int kernels stay `==` there, FFT keeps f32-tol; a device-absent config still runs the device-free half.)
- **crd-ceir CORE stays host-only/jobs-free/asset-free** (I3/I4/I5) — `execute_lowered` + the resolver + the CPU-oracle
  reference all live in the crd-ceir-gpu BRIDGE (or the test target), never the core. `crd-ceir-invariants` re-verifies.

## Open items for 13z-1's advisor consult

- The exact `execute_lowered` signature (does it own buffer creation, or take a caller binding-table mapping CEIR Value* →
  `ComputeBuffer*`? — lean: caller-supplied binding-table, mirroring `ckir_kernel_dispatch.hpp`'s explicit buffers).
- Whether ADR-0126 is warranted or 13z rides ADR-0125 (lean: a thin ADR-0126 — a new public bridge entry point + the
  provider-wrapping named-forward + the resolver contract earn a decision record, like 13d took ADR-0125). → RESOLVED:
  ADR-0126 (13z-1a).

## DoD-leg ownership (each §129 requirement → its closing checkpoint)

The §129 row lists six requirements; a gate is surprised by an UNPLACED leg, so each is pinned to a checkpoint here:

| §129 DoD leg | owned by | how |
|---|---|---|
| dispatched Vulkan AND DX12 | **13z-1b** (`add`) → all kernels by 13z-z | `tests/ceir-gpu-vulkan` + `tests/ceir-gpu-dx12` (per-backend, mirroring kir-vulkan/dx12; shared harness in `tests/gpu-shared/`; only the Vulkan target → gate6b.sh, DX12 Windows-only) |
| bit-exact vs the CPU oracles | **13z-1b+** | CEIR-path output byte-identical to the direct-CKIR-path (`dispatch_kernel_1wg`) on the SAME KGraph+buffers+device; each kernel's math keeps its convention (int/f32 add `==`; FFT f32-tol) |
| authored text AND builder | **13z-2** (reduce/scan) | the §121 text≡builder discipline for the CEIR asset |
| **ValidationCapture-silent** | **13z-1b (Vulkan)** | `crd::gpu::ValidationCapture capture(*vk); … CHECK(capture.error_count() == 0U)` under `cfg.enable_validation=true` — the RET-4 idiom; the DX12 info-queue equivalent when that backend lands. ⛔ was UNPLACED — pinned here (advisor). |
| hot-reload live-swaps | **13z-4** | a kernel-ref rebinds via the 10a ReloadSet; the CEIR asset re-lowers + re-executes, output tracks the swap |
| reference executor validates host-visible | **13z-4** (fork G) | a bridge-side CPU-oracle run of the same lowered list; core Interpreter stays `NoSemantics` (§150 fwd) |

## 13z-1b implementation notes (advisor-settled structure)

- **Per-backend targets** `tests/ceir-gpu-vulkan` + `tests/ceir-gpu-dx12` (NOT one both-backend target — a dx12-linked
  target cannot build on Linux, forfeiting the WSL llvmpipe Vulkan run, a standing project value). Copy the conditional dx12
  wiring from `tests/CMakeLists.txt`. Only the Vulkan target → gate6b.sh. Prove **Vulkan first, then DX12** (the DX12 scar
  density). 1b closes only when BOTH pass on a real device; Vulkan-green / DX12-pending is fine tick pacing.
- **The harness = clone the bracket, swap one line.** A `dispatch_ceir_1wg` in `tests/gpu-shared/` mirrors `dispatch_kernel_1wg`
  EXACTLY (buffer creation, upload copies, `TransferDst→ShaderRead` barriers, readback) with `execute_lowered(...)` replacing
  ONLY the `rec.dispatch(...)` line. SAME pipeline object on both paths (the resolver returns the direct path's pipeline);
  separate output buffers; byte-compare. ⛔ CONTRACT: CEIR binding-operand order = KGraph `buffer_decl` binding indices
  0..n-1 (the 13a positional-slot rule) — wrong order = a wrong-but-plausible dispatch, not an error.
- **Generic emit** (verified): `crd::kir::emit_compute_kernel_glsl(g, entry, scratch, GlslKernel&)` → `compile_glsl_to_spirv`
  → `create_pipeline_from_spirv(spirv, n_bindings, 0)` (Vulkan); `emit_compute_kernel_hlsl(...)` → `create_pipeline_from_hlsl`
  (DX12). The `add` kernel: `build_add_kernel` (KGraph: `c[lid] = a[lid] + b[lid]`, 3 F32 buffers at bindings 0/1/2).
- **Always-runs half INSIDE the new targets** (not a separate device-free checkpoint — solves the all-skip false-green for
  the NEW targets): the `add` CEIR asset lowers to a 1-Dispatch/3-binding list, `validate_lowered` == None, and the device
  test soft-skips (`if (ctx == nullptr) WARN skip`) so a no-GPU config still runs the lower-shape asserts.
