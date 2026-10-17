# DIAG.11b: the warning set at GCC's link-time step, 2026-10-17

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.11b](../ROADMAP.md#slice-diag.11b). Rules: [AGENTS](../../AGENTS.md) ("keep
> warnings zero"). Previous batch: [warning gates](2026-10-17-diag-11b-warning-gates.md), which queued follow-up (f)
> of this note.

## Goal

The previous batch put `-Werror` on GCC and Clang link lines, so a link-time warning fails the build. Its probe
suggested a gap behind that: GCC's link-time compile did not seem to apply the compile line's `-Wall` set, so a
`-Wall`-class defect that only cross-file inlining exposes would still link green. Follow-up (f): confirm that with a
probe that separates the two kinds of warning, decide whether the GCC/Clang link lines get the warning set, and if
they do, keep linux-gcc-shipping, linux-gcc-release and linux-gcc-relwithdebinfo at zero. Committed locally, not
pushed.

## The probes

GCC 13.3.0 under WSL (the hosted ubuntu-24.04 compiler), the shipping flags (`-O3 -DNDEBUG -g -ffunction-sections
-fdata-sections -funroll-loops`, `-flto=auto -fno-fat-lto-objects`). Each program is two to three files that compile
clean on their own under the whole compile set with `-Werror`; the defect is visible only after link-time
optimization inlines one file into another (`~/logs/ltowall`).

| Program | One translation unit, no LTO (the compile set) | Link line without warnings | Link line `-Wall` | Link line `-Wall -Wextra -Wnull-dereference` | Link line: the compile set, `-Werror` |
| --- | --- | --- | --- | --- | --- |
| out-of-bounds read `a[6]` of `int[4]` | `-Warray-bounds` | clean | clean | clean | clean, exit 0 |
| conditionally set local read | `-Wmaybe-uninitialized` | clean | clean | `-Wmaybe-uninitialized` | error |
| possibly null pointer dereferenced | `-Wnull-dereference` | clean | clean | `-Wnull-dereference` | error |
| store `dst[12]` into `char[8]` (the default-on control) | `-Warray-bounds` | `-Wstringop-overflow` | `-Wstringop-overflow` | `-Wstringop-overflow` | error |

So `-Wall` on the link line does nothing at all, while `-Wextra` does. `g++ -v` shows why: of the compile set, lto1
receives `-Wextra -Wpedantic -Wshadow -Wnull-dereference -Werror` and nothing else. GCC streams no diagnostic option
into the LTO objects (an empty set reaches lto1 from the objects alone), and `lto-wrapper` passes on from the link line
only the options lto1 itself knows (common, target and driver options). `-Wall`, `-Wformat=2`, `-Wconversion`,
`-Wdouble-promotion`, `-Wnon-virtual-dtor` and `-Woverloaded-virtual` are C-family options, dropped silently: no
diagnostic, also not from the `gcc` C driver linking C++ objects, so they cannot break a link either. Without link-time
optimization the options on a link line are inert (a plain-object link with the whole set and `-Werror`: exit 0, no
output), and clang++ 18 accepts them on its link line without a word.

Naming every option the compile set turns on (97 beyond GCC's defaults, from `g++ -Q --help=warnings`) on one link
line and reading lto1's command line: 26 reach it. They are `-Warray-bounds=1 -Wextra -Wimplicit-fallthrough=3
-Winfinite-recursion -Wmaybe-uninitialized -Wmisleading-indentation -Wnull-dereference -Wpedantic -Wshadow` (and its
`=local`/`=compatible-local` forms) `-Wstrict-aliasing=3 -Wstrict-overflow=1 -Wstring-compare -Wstringop-truncation
-Wtype-limits -Wuninitialized -Wunused` (and its members) `-Wuse-after-free=2`. A second probe, with `-Wall`'s members
named one by one:

| Program | One translation unit | Link line: the compile set | ... plus `-Wall`'s members that reach lto1 | ... plus `-Wrestrict -Wmismatched-new-delete -Wmismatched-dealloc -Wdangling-pointer=2` |
| --- | --- | --- | --- | --- |
| out-of-bounds read | `-Warray-bounds` | clean | `-Warray-bounds` | `-Warray-bounds` |
| conditionally set local | `-Wmaybe-uninitialized` | `-Wmaybe-uninitialized` | the same | the same |
| possibly null pointer | `-Wnull-dereference` | `-Wnull-dereference` | the same | the same |
| `malloc` in one file, `delete[]` in another | `-Wmismatched-new-delete` | clean | clean | clean |
| `free` in one file, a read through the pointer in another | `-Wuse-after-free` | clean | clean | clean |

The last column is the limit: those four are C-family options too and never reach lto1 even when named, and
`-Wuse-after-free=2` reaches it but does not report the cross-file case. On GCC 13.3, a defect of the classes
`-Wrestrict`, `-Wdangling-pointer`, `-Wmismatched-new-delete`, `-Wmismatched-dealloc`, `-Wformat-overflow`,
`-Wformat-truncation` and `-Wclobbered` that only cross-file inlining exposes is therefore not judged at all; nothing
in the build can change that, so it is recorded here and in the CMake comment rather than claimed covered.

## The decision

The GCC link line now names what the compile set turns on that lto1 reads (root `CMakeLists.txt`, after the `-Werror`
link option; `$<LINK_LANGUAGE:CXX,C>` and `$<CXX_COMPILER_ID:GNU>`): `-Wextra -Wpedantic -Wshadow` and `-Wall`'s
members `-Warray-bounds=1 -Wuninitialized -Wmaybe-uninitialized -Wstringop-truncation -Wuse-after-free=2 -Wunused
-Wstrict-aliasing=3 -Wstrict-overflow=1 -Winfinite-recursion -Wmisleading-indentation`. It is not gated on
`CRD_WARNINGS_AS_ERRORS`, as the compile set is not; `-Werror` keeps its own gate. Clang is left alone: no Linux Clang
lane uses link-time optimization (linux-clang-tsan links plain objects; the fuzz and coverage presets turn warnings
off), and lld's own link-time step reads no `-W` option from the clang driver.

`-Wnull-dereference` reaches lto1 too, but it is not on the line yet. The first full collection (below) showed it at 70
sites, 1,974 lines, mostly paths through the scene and CEIR containers' lookups; reshaping hot ECS and IR headers for a
warning class is a batch of its own, queued as follow-up (g). Leaving it off is not a weakening: it was never judged at
the link before.

**Third-party code.** lto1 compiles every LTO object of an executable under that executable's link line, so a
third-party target built with interprocedural optimization meets the engine's warning set at the link although its own
compile never does. GLFW did: `-Wstringop-truncation` in `input.c` (two sites) and `-Wnull-dereference` in four.
GLFW now builds without interprocedural optimization on GCC, as zstd already does: its objects are ordinary objects, and
windowing and input gain nothing from cross-file inlining. That is not a suppression; it is the link-time half of the
existing rule that third-party code never sees `crd-warnings`. The census on linux-gcc-shipping after the change:

| Third-party code | Objects with `-flto` | Seen at the link under the new set |
| --- | --- | --- |
| Catch2 (`_deps/catch2-build`) | 106 of 106 | nothing |
| GLFW (`_deps/glfw-build`) | 0 of 22 (22 of 22 before) | was 2 + 4 sites, now not compiled at the link |
| zstd (`_deps/zstd-build`) | 0 of 30 | not compiled at the link |
| Dear ImGui (`crd-imgui-vendor`, sources from `_deps/imgui-src`) | 0 of 7 | not compiled at the link |

Catch2 is the one third-party target still compiled at the link, and it is clean under the set; the next third-party
hit there needs the same call.

The first try tested `CMAKE_C_COMPILER_ID`, which is empty at the root (the project enables only C++; C is enabled in
GLFW's own directory), so the property never applied; the guard now names the C++ compiler, whose link line it is.

## The sites

`cmake --build -- -k 0` on `~/cerid-build/linux-gcc-shipping` after the reconfigure relinked every executable: 64 of
184 links failed. lto-wrapper stops an executable's link at its first failing partition, so later collections found a
few more; three rounds brought the shipping tree to zero. linux-gcc-release (`-O3`, benchmarks on) then linked clean
on its first round, and linux-gcc-relwithdebinfo (`-O2`, asserts on) found the last two sites below. Each site was read
as a possible defect before it was changed; nothing is suppressed.

| Site | Class | Verdict | Change |
| --- | --- | --- | --- |
| `crd/kir/ckir_scan.hpp` `build_scan_block` / `build_scan_single_pass` (`rr[0]`) | `-Wmaybe-uninitialized` | real: a thread's chunk `pt = elems / threads` of 0 read an unset register, `threads == 0` divided by zero, and a chunk above 64 wrote past `rr[64]` | `detail::scan_per_thread` refuses a chunk outside 1..`kScanMaxPerThread` (64) in every build; the first freeze is peeled, so the graph is node for node the same |
| `crd/hesap/tensor/einsum.hpp` `einsum_plan_build` (`plan.n_steps`) | `-Wmaybe-uninitialized` | real: when all three greedy sweeps report the saturated cost `~0`, no sweep was taken and the plan's step count stayed unset | the first sweep always fills the plan, and `n_steps` starts at 0 |
| `crd/containers/hash_map.hpp` `operator[]` (`idx`) | `-Wmaybe-uninitialized` | latent: after an insertion the slot is looked up again, and without asserts a corrupt table read an unset index | a miss there is `CRD_FATAL` in every build |
| `tests/assets/resources/test_resource_manager.cpp` `TestArtifact` | `-Wuninitialized` | real: a temporary was copied with `type_fourcc` unset | the member is initialized |
| `memory/src/allocators/tlsf_allocator.cpp` `init_pool` (end sentinel) | `-Warray-bounds` | object model: the stores stayed inside the pool, but through a 32-byte `BlockHeader` lvalue whose last 16 bytes lie past it (the sentinel is the pool's last 16 bytes) | the two words are written with the word accessors every other header write uses |
| `crd/hesap/interp/keyframe.hpp` `sample_track` | `-Warray-bounds` | false: index 6 of the test's `f32[6]` on a path that pairs the CubicHermite arm with the Step span | the value offset is computed once beside the span; the lookup has no arm |
| `vulkan_raster_context.cpp` per-pass timing accessors | `-Warray-bounds` | false: `pass_gpu_ms(99)` and friends behind `i < m_timed_passes`, which never exceeds the 64-entry arrays (only passes below 64 are stamped) | `is_timed()` states both bounds |
| `tests/numerics/hesap-dsp/test_measurements.cpp` `check_idx` | `-Warray-bounds` | false: a reference-to-array template parameter whose two instantiations differ only in their type | a pointer and the count |
| `hesap-dense/src/eig_nonsym.cpp` `dlaexc` (`dloc`) | `-Wmaybe-uninitialized` | false: every index read is below nd², all written | value-initialized |
| `crd/hesap/stats/multivariate.hpp` `dlogpdf_dx` (two) | `-Wmaybe-uninitialized` | false for k ≥ 1; for k = 0 an unwritten array reached the solve, which reads none of it | an empty gradient returns first |
| `crd/hesap/quadrature/cubature.hpp` `gm_estimate` (`pt`) | `-Wmaybe-uninitialized` | false: the test's 3-D integrand reads `x[2]` and the compiler cannot see `d == 3` | the point array is value-initialized, so the integrand never sees an unset coordinate |
| `tests/numerics/hesap-autodiff/test_implicit_diff.cpp` `newton_solve` (`fx`) | `-Wuninitialized` | false: the residual was stored only on the first column's sweep | stored on every sweep (the value part of the forward dual does not depend on the seeded column) |
| `tests/gpu/kir/test_ckir_fft.cpp` `dft2_ref` (`cr`, `ci`; linux-gcc-relwithdebinfo only, `-O2`) | `-Wmaybe-uninitialized` | false: the column buffers handed to the reference DFT are written for the `rows` values it reads | value-initialized |
| `tests/geometry/geometry-delaunay/test_delaunay_2d_hilbert.cpp` `canonicalize` (relwithdebinfo only) | `-Warray-bounds` | false: `std::sort` of a three-element `FixedArray`, whose final insertion stage forms `first + 16` on a branch three elements never take | three compare-exchanges |

`hash_map.hpp` and `cubature.hpp` also carried strict-tidy findings older than this batch (`[[nodiscard]]` on six
`HashMap` accessors, the iterator class name, three combined declarations, a lowercase literal suffix, and forwarding
references that were never forwarded); they are fixed, so both headers parse clean under the LLVM 20 gate.
`HashMap::const_iterator` stays as an alias of the renamed `ConstIterator`. The cubature entry points now take the
integrand by value, as the standard algorithms do; the internal estimators take it by reference.

One link in the third round, `crd-hesap-comms-tests`, ended in `internal compiler error: in drop_overlapping_mem_locs,
at var-tracking.cc:2298`, in GCC's variable tracking at an untouched line (`fft.hpp:1365`); the next build linked the
same target cleanly. It did not reproduce, so it is recorded as one of this host's faults
([host faults](2026-10-11-diag-10a-container-overflow-control.md)), not chased.

## Teeth

The tooth is a cross-file out-of-bounds read inside crd-perf-ui-tests: `crd_teeth_read_at(const int*, int)` appended to
`test_panel_helpers.cpp` and a `[teeth]` case in `test_profiler_sources.cpp` reading index 6 of an `int[4]` through it.
Both files compile clean under the compile set; only link-time inlining shows the defect.

| Step | Link line | Result |
| --- | --- | --- |
| (a) HEAD's `CMakeLists.txt` | `-Werror` only | links, exit 0, no warning; the `[teeth]` case even passes on the value it read past the array |
| (b) the new set, with `-Wnull-dereference` still in it | the new set | `test_panel_helpers.cpp:155:24: error: array subscript 6 is outside array bounds of 'int[4]' [-Werror=array-bounds=]`, link fails |
| (c) the final set | the final set | the same error, link fails |
| (d) the byte-exact backups restored and touched, relinked | the final set | links; 14 cases, 603 assertions pass |

## Verification

The touched suites below are containers, memory, hesap-interp, hesap-dsp, hesap-quadrature, hesap-stats,
hesap-autodiff, resources, hesap-tensor, hesap-tensor-einsum, perf-ui, hesap-dense (349 cases), kir (313 cases),
geometry-delaunay (112 cases) and gpu-context-vulkan (312 cases).

| Lane | Built | Ran |
| --- | --- | --- |
| linux-gcc-shipping | every executable relinked under the set, then the changed sources: 189 link lines, 180 with the set (the nine `tests/diag` specimens carry no engine warning flags), no `warning:` line, `ninja: no work to do` afterwards | every touched suite passes; gpu-context-vulkan on lavapipe with the pinned validation layer (`VK_LAYER_PATH`, as the hosted lane sets it): 311 passed, 1 skipped (a first run without that environment failed only the GPU-assisted validation assertion, which needs the pinned layer) |
| linux-gcc-release (`-O3`, IPO, benchmarks) | the full build, 759 steps: 181 executables relinked under the set (190 link lines, 181 with it), no `warning:` and no `lto-wrapper` line; then the last two test edits | (link evidence only) |
| linux-gcc-relwithdebinfo (`-O2`, IPO, asserts) | the full build found the two sites above; after them 183 links over the two rounds, 190 link lines, 181 with the set, no `warning:` line | geometry-delaunay, kir, hesap-tensor-einsum, containers, memory, hesap-interp: pass |
| linux-gcc-debug (no IPO: the set is inert on its link lines) | the 15 touched test targets, no warning | the touched suites except Vulkan and the full kir suite; kir's `[scan],[fft]` cases (27): pass |
| linux-gcc-asan | the same targets, no warning | containers, memory, hesap-interp, hesap-quadrature, hesap-stats, hesap-tensor-einsum, hesap-tensor, resources, hesap-autodiff, geometry-delaunay, perf-ui, hesap-dsp, hesap-dense and kir's `[scan],[fft]` cases: pass |
| linux-clang-tsan | the same targets, no warning; none of its link lines carries the GCC set | containers, memory, hesap-interp, hesap-quadrature, hesap-tensor-einsum, resources, geometry-delaunay, perf-ui, hesap-autodiff, hesap-dsp and kir's `[scan],[fft]` cases with the lane's `TSAN_OPTIONS`: pass, no report |
| win-debug | `all` (761 steps), no warning | every touched suite, the Vulkan and kir suites on the real GPU; the nine `crd-no-*`/`crd-check*` guards |
| win-shipping | the 15 touched test executables, no warning | every touched suite passes |
| win-clang-cl-shipping | the same, every thin-LTO link on the first attempt | every touched suite passes |
| win-asan | containers, memory, hesap-tensor-einsum, kir | containers, memory and hesap-tensor-einsum under vcvars; kir's `[scan]` cases. The whole kir suite under ASan outlived the loop watchdog's 20-minute limit (it killed the batch file at 23 minutes, `crd-loop-watchdog.log`), which is not a test result |

Hosted evidence this batch needs: linux-gcc-shipping, linux-gcc-release and linux-gcc-relwithdebinfo link every
executable with the named set (`-Warray-bounds=1` on their link lines) and print no `warning:` line; GLFW's objects
compile without `-flto` there; the GCC lanes without link-time optimization stay green with the set on their link lines.

## Follow-up for an extra batch

(g) Link-time `-Wnull-dereference`: 66 of the engine's sites and 4 of GLFW's (GLFW now out of the link-time compile)
on linux-gcc-shipping, 1,974 lines in 64 failed executables, a lower bound because lto-wrapper stops each link at its
first failing partition. The hot spots are `crd/scene/world.hpp:863` (764 lines), `world.hpp:308` (232),
`ceir/src/context.cpp:4611` (130), `crd/math/vec.hpp:752` (87), `world.hpp:297` (58), `ceir/src/print.cpp:145` (54) and
`:178` (39), `world.hpp:871` (51), `crd/ceir/ir.hpp:255` (40) and `crd/containers/array.hpp:193` (36); the complete
list is `~/logs/nullderef-sites.txt` on the WSL host, from `~/logs/lw-ship-1.log`. Read first, as possibly real: the
definite ("null pointer dereference", not "potential") hits at `ceir/src/context.cpp:4611`,
`crd/scene/component.hpp:52`, `scene/src/world.cpp:954`,
`tests/geometry/geometry-mesh-processing/test_cluster_group.cpp:203` and `tests/numerics/hesap-stats/test_mcmc.cpp:73`.
`tests/support/diag/specimens/crash_capture_specimen.cpp:72` is a deliberate null dereference: it must stay a crash
specimen, for example by keeping it off the `crd-warnings` link edge like the `tests/diag` specimens, never by "fixing"
it. Known shapes among the rest: `DiagCommandService::command_at` callers that dereference without the null check
(`tests/ui/perf-ui/test_diag_panel.cpp:242`, `sandbox/src/main.cpp:349`,
`tests/tools/ceridc/fuzz/fuzz_diag_commands.cpp:1068`), and `ResourceManager`'s
`*m_handles.find(id)` after the earlier lookup that set `re_issue` (`resource_manager.cpp:856`, `:1058`, `:1296`),
which can keep the block found the first time instead of looking it up again. Done means `-Wnull-dereference` added to
`_crd_lto1_warnings` and the three IPO trees at zero.
