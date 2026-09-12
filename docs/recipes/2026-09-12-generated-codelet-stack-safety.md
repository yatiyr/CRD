# Generated codelet stack safety

<!-- doc-role: reference -->
> Current implementation reference. Work owner: [REPO.3c.8](../ROADMAP.md#slice-repo.3c.8); rules: [AGENTS](../../AGENTS.md).

## Parameters and evidence

| Input | Meaning / units | Contract |
|---|---|---|
| `NDEBUG`, `__OPTIMIZE__` | Build/preprocessor flags, dimensionless | Existing optimized Cerid configurations define at least one; ordinary Debug defines neither. Do not infer optimization from the build directory name. |
| `CRD_FFT_CODELET_INLINE` | Hierarchical leaf call policy | Ordinary `inline` in unoptimized builds; existing force-inline policy in optimized builds. |
| `CRD_FFT_GEN_INLINE` | Large generated batched leaf policy | Ordinary `inline` in unoptimized builds and native MSVC; existing force-inline policy in optimized Clang/GCC. |
| Executable stack reserve | Virtual-address reserve, bytes | Inspect the actual PE header. The reproduced clang-cl test executable has 1,048,576 bytes; the repair does not enlarge it. |
| `FftPlan` size, direction, batch | Transform elements, forward/inverse, independent columns | Unchanged algorithm selection, scratch ownership, schedule and numerical oracles. |

## Why a small input can overflow

Inlining a family of generated straight-line leaves merges their expression temporaries into the caller. In an
unoptimized build those temporaries can retain separate stack slots, even when only one branch executes. A small
transform can therefore fail at function entry before it reaches its chosen leaf. This is distinct from recursive
FFT depth or the heap arrays holding samples.

Clang's force-inline attribute attempts inlining independently of the optimization level; ordinary `inline` does
not impose that requirement. See [Clang's attribute contract](https://clang.llvm.org/docs/AttributeReference.html#always-inline-force-inline).
The Windows linker reserves 1 MB by default on x64, but the artifact's PE header is the authoritative value for a
specific run. See [Microsoft's stack allocation contract](https://learn.microsoft.com/en-us/cpp/build/reference/stack-stack-allocations?view=msvc-170).

For a call chain, compare the available stack with the sum of simultaneously live caller/callee frames plus ABI,
runtime and exception-handler overhead. The largest individual frame alone is not a proof that a chain fits.
Fiber stacks are separate allocations and need their own measured budgets; a main-thread pass does not qualify them.

## Rebuildable diagnosis and repair

1. Reproduce the published named CTests with the same compiler/profile. Retain the command, exit and full failure
   log. The six affected cases cover large f32/f64, hierarchical, scheduled and batched forward/inverse paths.
2. Inspect the executable with `llvm-readobj --file-headers <exe>` and the compiled TU with
   `llvm-readobj --unwind <object>`. Match `StartAddress` symbols to `ALLOC_LARGE` allocations. A debugger is useful
   for the call chain, but a broken debugger installation is not evidence about the engine.
3. Inspect real compile flags and generated-source provenance. Preserve the arithmetic, SIMD/scalar selection and
   optimized inlining policy. Use ordinary calls for the large unoptimized leaves; do not change the global engine
   force-inline macro or raise all application stacks to disguise the problem.
4. Update both tracked generator and emitted policy, check regeneration, rebuild affected consumers, then rerun
   the full affected numerical tests and incremental tidy. Verify the resulting frame sizes as well as test results.
5. Keep native/Linux/compiler diversity in CI. Source-equivalent optimized output supports a preservation claim;
   performance superiority still needs a separately measured benchmark board.

## Traps and limits

- A scalar fallback can still overflow when many scalar leaves are forced into one caller. Check both batched and
  hierarchical families; repairing only one does not remove the other family's stack use.
- Debug flags and compiler inlining behaviour matter even when optimized CI passes the same inputs.
- Keep the native MSVC large-codelet exception: historical LTCG compiler exhaustion is a separate constraint.
- Generated declaration/name cleanup belongs in the generator and must preserve expression order. The
  [FFT style helper](../../scripts/fft_codegen_style.py) handles a deliberately limited grammar and has portable
  regression fixtures. Fused generated DAGs carry per-function statement-count annotations; other tidy checks
  still parse and check the entire body. Do not use a blanket generated-file lint exclusion.
- Generated comments can name lost scratch generators. The hierarchical header names `build/gen_subfft_m3.py`,
  which is absent in this checkout; this repair changes only its include/call annotations. Restoring authoritative
  generation/provenance is owned by [REPO.DEV.10](../ROADMAP.md#slice-repo.dev.10), not falsely certified here.
- A new header can trigger the active native synchronizer. Let its generation finish before a second CMake
  configuration; preserve its membership transaction. Do not bypass the guard to win a configure race.

## Results and source

Before repair, LLVM-20.1.8 clang-cl `/Od` produces `execute_batched` frames of 2,149,688 bytes (f32) and 2,033,048
bytes (f64), exceeding the executable's reserve individually. Six scoped CTests reproduce stack overflow. These
are diagnostic artifact measurements, not benchmark results or a speed claim. Final validation belongs in the
[repair session](../sessions/2026-09-12-repository-ci-environment.md#fft-stack-repair).
After the repair and declaration cleanup, those caller frames are 736 and 880 bytes respectively. The final
rebuild passes 56 FFT/consumer runtime cases plus six relevant guards; this does not qualify arbitrary fiber
stack sizes or replace the exact published CI gate.

Implementation: [call policy](../../engine/numerics/hesap-fft/include/crd/hesap/fft/detail/codelet_policy.hpp),
[hierarchical leaves](../../engine/numerics/hesap-fft/include/crd/hesap/fft/detail/hier_codelets.hpp),
[batched generator](../../scripts/gen_fft_batched.py), [emitted batched leaves](../../engine/numerics/hesap-fft/include/crd/hesap/fft/detail/batched_codelets_gen.hpp),
[consumer tests](../../tests/numerics/hesap-fft/test_fft.cpp).
