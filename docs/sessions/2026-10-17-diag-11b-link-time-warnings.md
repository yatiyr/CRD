# DIAG.11b: the shipping lane's link-time warnings, 2026-10-17

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.11b](../ROADMAP.md#slice-diag.11b). Rules: [AGENTS](../../AGENTS.md) ("keep
> warnings zero"). Previous batch: [installed sites](2026-10-16-diag-11b-installed-sites.md), which queued this.

## Goal

The follow-up the installed-sites batch queued. The hosted `linux-gcc-shipping` lane of complete run 38640651640 (job
116199358033, revision 56748127) printed 79 `warning:` lines, none of which fails it because the link line carries no
`-Werror`: 44 GCC link-time (LTO) diagnostics at 13 sites in eight groups, and 35 `lto-wrapper` lines. The target is
zero on that lane, with nothing suppressed and every site shown before and after by a relink. Committed locally, not
pushed.

## Reproduction

WSL `~/cerid-build/linux-gcc-shipping`, GCC 13.3.0 (`13.3.0-6ubuntu2~24.04.1`, the compiler of the hosted
`ubuntu-24.04` image). Deleting and relinking the fifteen executables whose hosted links warned
(`~/logs/lto-before.log`) printed the same 44 diagnostics, site for site and count for count, and the `lto-wrapper` line
on the three of them that link zstd (`ceridc`, `crd-ceridc-tests`, `crd-ceir-gpu-vulkan-tests`).

## Causes and repairs

| Diagnostic (hosted count) | Cause | Repair |
| --- | --- | --- |
| `crd/kir/ckir_grad.hpp:25` `-Wstringop-overflow` (16, `crd-kir-tests`) | `swap_last2` wrote `perm[k]` for `k < sh.rank` into the eight-entry arrays `reverse_ad` passes; `rank` is an `int` GCC cannot bound (the 16-byte form is the vectorized fill) | `swap_last2` takes `u8 (&)[kMaxRank]` and bounds the rank by `kMaxRank` as well |
| `string_fortified.h` memset/memcpy bound near 2^64 (15, `crd-ceridc-tests`) | seven `read_out` loops of `test_ceridc_diag.cpp` stripped a trailing line break with `out.resize(out.size() - 1U)`; on `resize`'s growth branch GCC sees `size() - 1` wrapped | `out.pop_back()` (all eight such loops of the file); `pop_back` already carries the empty-string guard |
| `crd/containers/array.hpp:276` memset bound near 2^64 (5, the kir and ceir-gpu test executables) | `eval_cpu` resized two arrays by `static_cast<usize>(nn)` of an `int` node count; a negative `int` reads as a near-2^64 size | one `node_count` (0 for a non-positive count) for the reserve and both resizes |
| `hesap/dense/detail/gemm_pack.hpp:53` `-Waggressive-loop-optimizations` (3, `crd-hesap-dense-tests`) | `pack_b`'s four-row interleave ran while `p + 4 <= kc`; after it GCC loses `p <= kc`, so the remainder loop's trip count looks unbounded | the interleave ends at the closed-form row `kc - kc % 4` (the same rows, bit-identical packed bytes) |
| `string_fortified.h` memcpy `-Wstringop-overread` (2, quadrature and ODE tests) | `CommandRegistry::find` built a `String` key from the view on every lookup; in a constant-name clone GCC followed `init_from`'s heap branch with a 23- or 20-byte literal | `m_index.find(name)`: the documented heterogeneous lookup of `HashMap<String, V>`; no key is copied and the `noexcept` lookup no longer allocates |
| `<span>:289` `-Waggressive-loop-optimizations` (1, `smoke_hesap_tensor`) | the same loss as `pack_b` in `convert_f32_to_f16`'s eight-wide F16C loop, on the smoke's 24-element call | the vector part ends at the closed-form index `n - n % 8` |
| `perf-ui/src/diag_panel.cpp:323` `-Wstringop-overflow` (1, `crd-perf-ui-tests`) | **a false positive.** The test's 512-byte path is refused before the store (its `CHECK_FALSE(panel.set_path(...))` and `path().empty()` pass on every lane), but GCC keeps the terminator store `m_path[512]` on that path | `set_path` clears the field and copies the view, so no store is indexed by the view's size |
| `string_fortified.h` memcpy "into a region of size 0" (1, `crd-perf-tests`) | **a false positive.** `test_diag_host.cpp:888` appends to a 44-byte `String`, which lives on the heap; after the out-of-line `reserve` GCC follows `data()`'s inline branch with `old` = 44 | `String::append` states its contract after `reserve`: a string longer than the inline buffer is on the heap (`CRD_ASSERT` plus an unreachable early return, the form `pop_back` already uses) |
| `lto-wrapper: Extra option to -Xassembler: --noexecstack` (35, every link of zstd) | `CMakeLists.txt` turned interprocedural optimization off for `zstd_static`, but zstd 1.5.5's target is `libzstd_static` (`zstd_static` is only its output name), so the guard never applied: zstd's objects were LTO objects (`/GL` on MSVC, `-flto` on GCC) on every IPO lane and its `-Wa,--noexecstack` met the engine's objects in `lto-wrapper` | the guard names `libzstd_static`. This is also the 2026-05-17 MSVC LTCG workaround the comment describes, which had not been in effect |
| `zstd_v01.c` `-Wmaybe-uninitialized` (4, new with the previous repair) | without LTO, GCC compiles zstd optimized at compile time and warns in the legacy v0.1 decoder | `ZSTD_LEGACY_SUPPORT OFF`: Cerid decodes only frames its own cooker wrote with this zstd (`crdr.cpp` and the resources fuzz target), and no tracked file holds a v0.1 to v0.7 frame magic (checked byte-wise over `git ls-files`; five resources fuzz seeds hold the current magic) |

Nothing is suppressed: no pragma, no `-Wno-`, no warning option changed.

## Experiments on the way

| Run (log under WSL `~/logs`) | Change | Result |
| --- | --- | --- |
| `lto-panelA` | `set_path` with a local `size` and the same indexed store | the diagnostic stayed (`diag_panel.cpp:324`) |
| `lto-panelB` | clear the field, then copy | gone |
| `lto-f16a` | closed-form vector end in `convert_f32_to_f16` | gone |
| `lto-appendA` | the guarded `String::append` | gone |
| `lto-after1-interrupted` | block extents as `std::min` in `blas3.cpp` (`gemm` and `gemm_parallel`) instead of touching `pack_b` | four new diagnostics in `pack_b` (`gemm_pack.hpp:184` and `:53`) on `gemm_parallel`'s path in `runtime/bench_hesap_svds`, which the hosted lane links clean; the build was stopped, `blas3.cpp` restored and the closed form put in `pack_b` instead. The same run showed the four `zstd_v01.c` diagnostics |
| `lto-packA` | the closed form in `pack_b`, `blas3.cpp` as committed before | `crd-hesap-dense-tests` and `bench_hesap_svds` link clean |

## Evidence

- **Before** (`lto-before.log`, the committed sources): the 44 diagnostics and three `lto-wrapper` lines above.
- **After** (`lto-after.log`): every executable of the tree deleted, then one `ninja` of the whole configuration: 189
  executables linked (the hosted lane's 184, plus `crd-diag-overhead`, added after 56748127, and four CUDA targets this
  host configures), zstd's 30 objects compiled without `-flto` and without the legacy decoders: **0 `warning:` lines,
  0 `lto-wrapper` lines**, `NINJA_EXIT=0`.
- Each repair is local to its diagnostic's inlining chain, and the experiments above show the two false-positive sites
  and the two loop forms going from present to absent with nothing else changed.

The two DIAG sites are confirmed false positives on the lane: the refused path never reaches the store (the perf-ui
case passes on linux-gcc-shipping) and the appended string is in heap mode by construction (44 > 23 characters).

## Verification

| Lane | Built | Ran |
| --- | --- | --- |
| linux-gcc-shipping (WSL) | every target of the configuration, every executable relinked: 0 warnings | containers, perf, perf-ui, kir, ceir-gpu, hesap-dense, hesap, hesap-quadrature, hesap-ode, ceridc, math, hesap-tensor, resources, `smoke_hesap_tensor`, `bench_hesap_svds`, the lavapipe kir-vulkan and ceir-gpu-vulkan suites and the `crd-fuzz-resources-reload-corpus` replay: all pass |
| linux-gcc-debug (WSL) | the touched test targets and `ceridc`: 0 warnings | the eleven touched suites: all pass |
| win-debug | `all` (the String change reaches nearly every target): no compiler warning | the thirteen touched suites, `smoke_hesap_tensor` and the two Vulkan suites on the real GPU: all pass |
| win-shipping | the touched targets; zstd's objects now compile without `/GL`: no warning, `crd-resources-tests` (a zstd consumer) links | the same thirteen suites and `smoke_hesap_tensor`: all pass |
| linux-gcc-asan (WSL) | the touched test targets and `ceridc`: 0 warnings | containers, perf, perf-ui, ceir-gpu, hesap, ceridc, math, hesap-tensor, resources; kir's `[grad]` and `[opt]` cases (`reverse_ad` through `swap_last2`) and two of its four shards (157 cases); hesap-dense's 25 `[blas3]` cases and one of its four shards: all pass. The other kir shards hold the multi-minute rendering oracles (the hosted asan lane times the cloud ray-march alone at 450 s) and were left to the hosted lane |
| win-asan | containers, perf, perf-ui | the same three: all pass |
| win-clang-cl-shipping | the touched targets (thin LTO; every link succeeded on the first attempt); zstd's objects now compile without `-flto` | the same thirteen suites and `smoke_hesap_tensor`: all pass |
| linux-clang-tsan (WSL) | the touched test targets and `ceridc`: 0 warnings | with the lane's `TSAN_OPTIONS`: containers, perf, perf-ui, ceir-gpu, hesap, ceridc, math, hesap-tensor, resources, kir's `[grad]`/`[opt]` cases and one of its four shards, hesap-dense's `[blas3]` cases: all pass, no report |

- Strict tidy (LLVM 20.1.8, `tidy-files.py`) on the eight changed C++ files: clean. clang-format `--dry-run`: no
  diagnostic on an added or changed line, and no file's count grew. The Allman check, the `crd-no-*`/`crd-check*`
  guards (8 of 8, win-debug), `check-master-plan.py` and `check-repository.py` pass. No script or workflow changed.
- The first linux-gcc-asan run was stopped from outside: the `CRD-Loop-Watchdog` scheduled task reaps any
  `wsl.exe`/`bash.exe` whose command line names the scratchpad once it is 20 minutes old, and killed the run at 26
  minutes, in kir's cloud ray-march case (SIGTERM); the idle WSL VM then stopped. The sanitizer suites were rerun in
  pieces that each finish inside the gate. Not a test failure.

## The other hosted lanes at 56748127

The same run's other GCC IPO lanes print the same class: `linux-gcc-release` exactly the 79 lines of
`linux-gcc-shipping`; `linux-gcc-relwithdebinfo` 45 (35 `lto-wrapper`, six `eval_cpu` resizes in the ceir-gpu,
technique-cook and ceir-gpu-vulkan tests, the two `CommandRegistry::find` overreads, the `convert_f32_to_f16` tail and
`set_path`). Every one is a site repaired here; neither preset was rebuilt locally. `linux-clang-tsan` and
`win-clang-cl` print two `-Wstrict-prototypes` in the legacy `zstd_v04.c`, which is no longer compiled.

Still printed after this batch, outside its lane: `win-clang-cl-shipping`'s two zstd `-Wunused-function`
(`HUF_isSorted` in `huf_compress.c`, `ZSTD_isAligned` in `zstd_lazy.c`; hosted job 116199386927 and the local build
alike, zstd's own code under its own `-Wall`), and `win-shipping`/`win-shipping-profile`'s two
`D9025 overriding '/O2' with '/Od'` from the load-bearing `/Od` pin on crd-jobs' `worker_pool.cpp` and
`fiber_init.cpp` (`engine/foundation/jobs/CMakeLists.txt`). And nothing yet holds the lanes at zero: a link line
without `-Werror` lets the next link-time diagnostic land green.

## Hosted evidence this batch needs

`linux-gcc-shipping` and `linux-gcc-release` print no `warning:` line, link-time or `lto-wrapper`;
`linux-gcc-relwithdebinfo` prints none of the sites above; `linux-clang-tsan` and `win-clang-cl` print no zstd legacy
warning; every lane builds and passes with zstd's legacy decoders gone (the `crd-fuzz-resources-reload-corpus` replay
included).
