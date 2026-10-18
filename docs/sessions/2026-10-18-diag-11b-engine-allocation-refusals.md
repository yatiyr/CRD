# DIAG.11b: allocation refusals in the rest of the engine, 2026-10-18

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.11b](../ROADMAP.md#slice-diag.11b). Contract:
> [DIAG.3a partial construction](../design/runtime-diagnostics.md#diag-3a). Previous batch:
> [func.func body lookups](2026-10-17-diag-11b-func-body-lookups.md); the sweep this finishes started in
> [allocation refusals in the scene and the resource manager](2026-10-17-diag-11b-unchecked-allocations.md#queued),
> which queued it as follow-up (m).

## Goal

Follow-up (m). A sub-budget allocator (linear, stack, pool, growable linear) refuses a request by returning nullptr
([allocator.hpp](../../engine/foundation/memory/include/crd/memory/allocator.hpp)). Code that takes memory from a
caller's `IAllocator` and writes through the result unchecked follows null in every build without asserts. The
previous batches repaired the containers, the CEIR context, the scene and the resource manager; this batch repairs
every other site in the engine, the tools and the runtime examples, and adds a guard so the shape cannot come back.
Each module keeps the convention it already has: a status where it reports one, otherwise the named out-of-memory
fatal of DIAG.3a. Committed locally, not pushed.

## The sweep

Every `allocate`/`try_allocate`/`reallocate` call with arguments and every `construct<T>`, `construct_array<T>` and
`allocate_array<T>` under `engine/`, `tools/`, `runtime/` and `sandbox/` was read, outside the allocator
implementations themselves. The queued list held 66 sites; the sweep found six more: the optimization model's three
functions, the scene renderer's staged frame graph and the scene and öbek cookers' registries.

| Module | Sites | Convention | Now |
| --- | --- | --- | --- |
| ceir-cook | `ReloadSet::cook_source`'s transient Context, `alloc_generation`'s generation and its Context; `PlanCache::put`'s artifact and dependency copies (the dependency size was an unchecked product) | no out-of-memory status (`CookError`, `AddError` are closed enums with records and decoders) | `construct_or_fatal`/`allocate_array_or_fatal`; five messages in `cook::detail` |
| ceir-host | `HostProvider::pool_launch`'s token (the launch op's result is its handle) | `ExecError` has none | `construct_or_fatal`; `detail::pooled_token_size()`/`_alignment()` expose the private token's request shape to the specimen |
| ceir | `DiagnosticEngine::emit`'s arena copies: the code name, the message, the note and fix-it arrays (unchecked products) and each note's and fix-it's text | none | `allocate_array_or_fatal`, `detail::kDiagnosticArenaOutOfMemory` |
| kir | `eval_cpu`'s materialization buffer and an Attention node's row of scores; `reverse_ad`'s cotangent table | `eval_cpu` is `void noexcept` | `allocate_array_or_fatal`; a negative node total reads as a count no allocation holds |
| hesap-fft | `FftPlan`'s 30 raw tables (32- and 64-byte aligned) and its two four-step sub-plans; the inner plans of `BluesteinPlan`, `NdFftPlan` (both kinds of axis) and `SparseFftPlan` | constructors, no status | one private `plan_table<U>(count, alignment)` and `sub_plan(n)`; `construct_or_fatal` with one message per plan class |
| hesap-dense | `Matrix` (its `rows * cols` was unchecked), `Vector`, `Permutation`, the structured storage of `Symmetric`, `Hermitian`, `Triangular` and `Banded` (`n * n` and the band height `kl + ku + 1` unchecked), and the packing buffers of `gemm`, the small direct `gemm_parallel` path, the packed `gemm_parallel` path and `syrk_lower_minus` | none | checked sizes and `allocate_array_or_fatal` at the original alignment; `allocate_zeroed` takes rows and cols |
| hesap-dsp | `FftConvolver`'s plan; `segment_periodograms`' (Welch, spectrogram) and `stft`'s per-job plans | none | `construct_or_fatal` |
| hesap-wavelet | `cwt`'s per-job scale plans | none | `construct_or_fatal` |
| hesap-autodiff | `VTape`'s node and scratch arrays (`max_nodes * 6 * n` unchecked) | none | the pool size is checked before anything is allocated (its own message), then `allocate_array_or_fatal` |
| hesap-opt | `Model::minimize`, `subject_to_eq`, `subject_to_ge`: the type-erased function | none (`OptStatus` is the solver's result) | `construct_or_fatal` |
| hesap-tensor | `sparsedetail::ScratchBlock` | `TensorStatus::AllocFailed`: every one of its seven callers tests `m_ptr` | kept; the byte size is now checked, so a count that overflows is refused (null, `AllocFailed`) instead of wrapping to a short block |
| scene-render | the reload adapter's staged `FrameGraphDesc` (`frame_stage`) | `DiagCode` has none | `construct_or_fatal` |
| asset cooker | `SceneCooker`'s and `ObekCooker`'s registries | none | `construct_or_fatal` |

`construct.hpp` gains the explicit-alignment forms `allocate_array<T>(alloc, count, alignment)` and
`allocate_array_or_fatal<T>(alloc, count, alignment, message)` (an alignment below `alignof(T)` is raised to it), so
the 64- and 32-byte tables, the 32-byte packing buffers and the 16-byte dense storage keep the alignment their SIMD
loads and streaming stores rely on. The element-alignment forms now forward to them.

Read and left as they are: the allocator implementations and adapters, which pass a parent's result up
(`flight_triggers.cpp`, `fault_injection.cpp`, `cg.hpp`'s arena-then-parent fallback, `thread_safe_allocator.hpp`);
the sites on a heap allocator, fatal on exhaustion by contract (`default_allocator()` in `platform/input.hpp` and
`vulkan_gpu_allocator.cpp`, ceridc's `GrowableTlsfAllocator`, the runtime examples' TLSF and virtual-memory
allocators); calls that are already checked (`tensor.hpp`, `dlpack.hpp`, the resource loaders, the Vulkan GPU
allocator's bool and `.valid()` results, `os_resource_probe.cpp`); and `dx12_validation_capture.cpp`, whose record
count is bounded by `kMaxMessages`.

## Two defects found on the way

- **An Attention node's scores were never returned.** `eval_cpu` allocated one row of scores per evaluated Attention
  node and never deallocated it, so a heap scratch leaked a row per evaluation (an arena scratch hid it). It is
  returned at the end of the node now; "AS-4: eval_cpu returns every scratch block it takes, an attention node's
  scores included" counts the live blocks.
- **`VTape::make` checked its capacity only in an assert.** A build without asserts (shipping, release) wrote a node
  past both arrays once a tape outgrew `max_nodes`; the `[[maybe_unused]]` on `m_cap` and a clang-cl comment had
  recorded that the guard was missing there. The capacity is checked in every build now, fatal
  (`VTape: node capacity exceeded`).

## The guard

[check-allocation-results.py](../../scripts/check-allocation-results.py), CTest `crd-check-allocation-results` on
every lane: every raw allocation result under `engine/`, `tools/`, `runtime/` and `sandbox/` must be compared with
nullptr (or negated, or tested with `.valid()`) within six lines and inside the block that made it, sit in an
`if`/`while` condition, be returned to a caller, go through an `*_or_fatal` helper, be a `default_allocator()` call,
or be one of the counted heap sites in its `ALLOWED` table (10 files, 17 calls, each with its reason; a count, so a
new call in the same file fails). The block limit matters: the DSP convolver's constructor result was followed by the
destructor's `if (m_plan != nullptr)`, which a plain window read as a check.

- On the tree: 58 allocation calls, every result checked or passed up.
- Teeth: on HEAD's sources (`git archive HEAD`) it reports 71 sites in 24 files, exactly the sites this batch
  repaired; the 72nd, the sparse scratch block, is the counted status site and stays allowed.
- Unit cases (`scripts/test-repository-tools.py`, `AllocationResults`): unchecked results reported by line; the
  checked, passed-up, conditional, heap and `_or_fatal` forms pass; a check outside the window does not count; the
  tree check honours and counts allowances and fails an empty tree.

## Tests and teeth

`tests/support/diag/specimens/refusing_allocator.hpp` is the specimens' shared refusing allocator: it grants from the
default allocator until armed, then refuses by rule (every request, one request shape, every request at or above an
alignment, everything after N grants). Each mode arms the rule that makes its allocation the run's first refused one,
so a refusal that lands elsewhere first reaches another fatal (`CRD_DIAG_FATAL_OTHER`) or a crash and the mode fails.

New child-process modes (`crd_diag_fatal_tests`, each owned by its module's suite), 44 in all:

| Specimen | Modes |
| --- | --- |
| `crd-ceir-cook-fatal-specimen` | `reload-cook-context-refused`, `reload-generation-refused`, `reload-generation-context-refused`, `plan-artifact-refused`, `plan-deps-refused` |
| `crd-ceir-host-fatal-specimen` | `pool-token-refused` |
| `crd-ceir-fatal-specimen` (3 more) | `diagnostic-text-refused`, `diagnostic-notes-refused`, `diagnostic-fixits-refused` |
| `crd-kir-fatal-specimen` | `eval-scratch-refused`, `eval-attention-scratch-refused`, `reverse-ad-scratch-refused` |
| `crd-hesap-fft-fatal-specimen` | `plan-hier-table-refused`, `plan-stockham-table-refused`, `plan-four-step-table-refused`, `plan-four-step-subplan-refused`, `bluestein-plan-refused`, `nd-axis-plan-refused`, `nd-bluestein-axis-refused`, `sparse-plan-refused` |
| `crd-hesap-dense-fatal-specimen` | `matrix-elements-refused`, `matrix-size-overflow`, `vector-elements-refused`, `permutation-refused`, `structured-storage-refused`, `banded-height-overflow`, `gemm-pack-refused`, `gemm-small-pack-refused`, `gemm-parallel-pack-refused`, `syrk-pack-refused` |
| `crd-hesap-dsp-fatal-specimen` | `convolver-plan-refused`, `welch-plan-refused`, `stft-plan-refused` |
| `crd-hesap-wavelet-fatal-specimen` | `cwt-plan-refused` |
| `crd-hesap-autodiff-fatal-specimen` | `vtape-nodes-refused`, `vtape-pool-refused`, `vtape-pool-overflow`, `vtape-capacity-exceeded` |
| `crd-hesap-opt-fatal-specimen` | `model-objective-refused`, `model-eq-refused`, `model-ineq-refused` |
| `crd-scene-cooker-fatal-specimen` | `scene-cooker-refused`, `obek-cooker-refused` |
| `crd-scene-render-fatal-specimen` | `frame-reload-staging-refused` (device-free, the RAF-11 live reload path; its frame source lives next to the specimen) |

Two more alloc-fatal modes in crd-memory-tests' or_fatal harness case: `allocate-aligned-array-or-fatal` and
`allocate-array-or-fatal-overflow`. Unit cases: the explicit-alignment form asks for the alignment it is given and
raises a smaller one (crd-memory-tests), the attention scratch balance (crd-kir-tests), the sparse scratch overflow
(crd-hesap-tensor-sparse-tests).

Teeth (win-debug), each restored byte for byte, touched, rebuilt and rerun green:

- **Tooth A**, the four `*_or_fatal` helpers' `CRD_FATAL` replaced by a no-op: all 40 refusal modes fail, 30 crashing on
  the refused pointer and 10 returning without the fatal. The four overflow and capacity modes, which do not go through
  the helpers, keep passing.
- **Tooth B**, the explicit checks removed (the `Matrix` element-count check, the band-height check, the `VTape` pool
  size and capacity checks, the scratch block's checked size, the alignment of the explicit-alignment form, the
  attention row's deallocation): `matrix-size-overflow` and `banded-height-overflow` return without the fatal (the
  sizes wrap to 0 and 1), `vtape-capacity-exceeded` returns after writing past the arrays, `vtape-pool-overflow` runs
  into its 60 s timeout (it attempts a 137 GB node array), and the three unit cases fail (`8 == 64`, `live == 0`
  failing with one block live, and a non-null wrapped scratch block).

After the restore all 103 CTests matching `fatal` pass on win-debug (the 39 of earlier batches, the 44 new modes and
the harness cases) and the unit cases pass. Both teeth ran before the tidy repairs, the `pick_of_four` form and the
`nd-bluestein-axis-refused` dims change below; none of those touches a helper, a check or a mode's target, and the
final tree passes every mode on every lane in the table.

## Tidy findings the edited headers brought into the gate

Strict tidy gates a header only when it is named (its `HeaderFilterRegex` is spelled with `/` and never matches the
Windows paths, so no lane gates headers through a translation unit), so naming the edited headers brought their older
findings into the gate, and they were repaired. `fft.hpp` had 216 (clang stops at 20 errors, so they surfaced in two
rounds): 177 declarations of several variables in one statement, split one per statement by a script that read the
reported lines and kept each initializer and trailing comment (the four `const T* const a = ..., *const b = ...`
statements it split into pointer-to-pointer types were restored by hand), 14 nested conditionals rewritten as `if`
chains with the same values (`h_n2`, `m_ds_b`, `m_ds_c`, `m_sh_n1`, `msets` and `m_rmax_bits`, all at plan time) and,
in `execute_ip4aos`, a reference-returning `pick_of_four` for the len-4 block's output pair, 24 local constants
renamed to the local-constant case (`kW` to `lanes`, `F32` to `is_f32`, `C` to `per_vec`, `kRe` to `period`,
`kBudget` to `scratch_budget`, `w1rA` to `w1r_a` and the like) and `m_log2` initialized at its declaration. The FFT
suite's bit-exact checks pass unchanged, and the hot code is the same code: a probe that instantiates
`execute_ip4aos` (f32 and f64, both directions) and the batched radix-2, -4 and -8 butterflies, compiled with
win-shipping's flags against HEAD's `fft.hpp` and against the new one (whole-program optimization off so the objects
hold machine code), disassembles to identical instructions in all ten functions, and so does `syrk_packed_inner_lower` (f64 and f32),
whose lower-triangle start became an `if`. A first form of the pair selection,
an `if` chain of stores, compiled to 74 fewer instructions in the f64 driver and was replaced, so the kernel did not
change behind an unmeasured edit. The other headers had 24: nested conditionals in `syrk_microkernel.hpp`, `model.hpp`, `sparse.hpp` and `cwt.hpp` became `if` chains; multiple declarations per
statement in `convolution.hpp` and `cwt.hpp` were split; the overlap-add block size `L` became `block`, the SIMD lane
width `W` became `lanes`; `cwt`'s prefix test uses `starts_with`; `lsd_sort` takes its key function by `const&` (it
never forwarded it); `MatrixView`'s converting constructor uses a `requires` clause instead of `enable_if`; the
private trait `is_complex_value` became `IsComplexValue`. Two public names changed to the constant convention:
`Matrix::layout`/`MatrixView::layout` are `kLayout`, `Triangular::side`/`diag` are `kSide`/`kDiag`; their only users
were `test_matrix_catalog.cpp`'s static checks, updated.

## Verification

Suites below: crd-memory-, -containers-, -ceir-, -ceir-cook-, -ceir-host-, -kir-, the hesap fft, dense, dsp, wavelet,
autodiff, opt, tensor and tensor-sparse suites, crd-scene-cooker- and crd-cooker-tests. Every CTest matching `fatal`,
`^crd-no-` or `^crd-check` (the contract-fatal modes of every batch, the alloc-fatal harness cases and the repository
guards, `crd-check-allocation-results` among them) is counted as "fatal and guard".

| Lane | Built | Ran |
| --- | --- | --- |
| win-debug | every target, no warning | the suites (kir 314 cases, 1084 s; dense 350; ceir 580), crd-scene-render-tests (114, on the GPU with `CRD_ASSETS_DIR`), crd-kir-vulkan- (31), -dx12- (22) and -cuda-tests (19); fatal and guard 113 of 113 |
| win-shipping | every target (1,719 steps), no warning | the suites and crd-scene-render-tests (114); fatal and guard 109 of 109 |
| win-clang-cl-shipping | the touched suites and every specimen, every link on the first try | the suites (kir 314) and crd-scene-render-tests (114); fatal and guard 109 of 109 |
| win-asan (under vcvars) | the touched suites and specimens | the suites (kir: the attention and grad cases); the only ASan reports are the allocator-poison specimens' deliberate ones in crd-memory-tests; fatal and guard 113 of 113 |
| linux-gcc-debug | the touched suites and specimens (873 steps), no warning | the suites (kir 314), crd-scene-render-tests' `[raf11]` reload cases; fatal and guard 107 of 107 |
| linux-gcc-asan (UBSan included) | the same, no warning | the suites (kir: attention, grad and module cases); reports only from the poison specimens; fatal and guard 107 of 107 |
| linux-clang-tsan | the same, no warning | the suites (kir: attention and grad), no TSan report; fatal and guard 107 of 107 |
| linux-gcc-shipping | every target (1,699 steps) under the link-time warning set with `-Werror`, `-k 0`: no `warning:` line, no `lto-wrapper` line, no failed step | the suites; fatal and guard 106 of 106 |
| linux-gcc-release | every target (1,704 steps) under the same set: no `warning:` line, no `lto-wrapper` line, no failed step | the suites; fatal and guard 106 of 106 |
| linux-gcc-relwithdebinfo (asserts compiled in) | every target (1,704 steps) under the same set: no `warning:` line, no `lto-wrapper` line, no failed step | the suites; fatal and guard 107 of 107 |

Static checks: strict tidy (LLVM 20.1.8) clean on all 51 changed C++ files; clang-format `--dry-run` reports nothing on
an added line that was not reported on the line it replaced (the hand-aligned blocks in `ckir_eval.hpp`, `vhvp.hpp`
and `cwt.hpp`'s unindented case labels); `check-allman-braces.py`, `check-master-plan.py`, `check-repository.py` and
`test-repository-tools.py` (79 tests) pass.

Host faults, rerun once and not chased (host-misexecutes): two win-debug links failed with `LNK1163: invalid selection
for COMDAT section` on different objects (crd-ceir-gpu-dx12-tests, then crd-audio-tests); the next build linked both.

Found while verifying: on win-shipping `nd-bluestein-axis-refused` first reached the Array fatal, because with a
largest axis of 6 the N-d plan's line buffer asked for exactly a shipping `BluesteinPlan<f64>`'s 128 bytes; the mode
now gives the plan a second axis of 1000, so the buffer cannot share a plan object's shape on any layout. The fft fatal
modes were rebuilt and rerun with it on linux-gcc-debug, linux-gcc-asan and linux-clang-tsan (8 of 8 each), and the
two later builds above include it.

## Hosted evidence needed once pushed

- The ten new specimens build on every lane that builds their owners, and their 44 modes, the three new
  `crd-ceir-fatal-diagnostic-*` included, pass by their `CRD_DIAG_FATAL_OK` lines, as do the two new alloc-fatal modes
  in crd-memory-tests' harness case.
- `crd-check-allocation-results` passes on every lane.
- The touched suites pass: crd-memory-, -containers-, -ceir-, -ceir-cook-, -ceir-host-, -kir- (with the attention
  balance case), the hesap fft, dense (with the renamed `kLayout`/`kSide`/`kDiag` checks), dsp, wavelet, autodiff,
  opt and tensor suites, crd-scene-cooker-, crd-cooker- and crd-scene-render-tests; the kir device suites on the device
  lanes.
- linux-gcc-shipping, linux-gcc-release and linux-gcc-relwithdebinfo link every changed consumer with no `warning:`
  line.

## Next

Follow-up (n), the recurrence guard against test sources no target compiles; (l) waits for the user's approval of the
zstd 1.5.7 download. Then DIAG.11b clause (1).
