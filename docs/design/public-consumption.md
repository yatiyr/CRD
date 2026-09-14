# Public consumption: self-contained public headers and a relocatable package

<!-- doc-role: reference -->
> Contract for REPO.DEV.8. Status lives only in [ROADMAP](../ROADMAP.md#slice-repo.dev.8); the accepted research is
> [public consumption and stronger instruments](../research/2026-09-12-large-cpp-development-and-ci.md#public-consumption-and-stronger-instruments);
> the modules are [CrdPublicChecks.cmake](../../cmake/CrdPublicChecks.cmake) and
> [CrdPackage.cmake](../../cmake/CrdPackage.cmake); the instruments are
> [check-headers.py](../../scripts/check-headers.py) and [test-package-consumer.py](../../scripts/test-package-consumer.py).

Purpose: every public header of every module compiles on its own through the view a consumer has of that module;
the public/private include and dependency boundaries the [module registry](module-registry.md) declares are what
a public header actually needs; and a real downstream project can install a Cerid package, move it, and build and
run against it with the profile it was packaged from, on the toolchain of the lane that qualified it.

## Consumer view

A module's consumer view is the set of usage requirements a target receives by linking the module's library targets
`PRIVATE`: their `PUBLIC` and `INTERFACE` include directories, definitions, options, features and link edges, and
nothing else. No private include directory, no precompiled header, no translation unit that included the right
header first. A public header that compiles alone in that view is self-contained and inside the boundary; one that
does not is either missing an include (a header defect) or reaching a module the registry does not give its
consumers (a boundary defect), and both are fixed at the header or at the declaration, never by widening the check.

`CRD_PUBLIC_CHECKS=ON` (default OFF; the default graph is unchanged) makes [CrdPublicChecks.cmake](../../cmake/CrdPublicChecks.cmake)
generate, after `crd_add_modules()`, one shim translation unit per public header (`include/**/*.hpp` and `*.h`;
`*.in` templates and `*.inc` fragments are not headers) into an OBJECT library per module,
`crd-<module>-header-check`, which links only that module's non-imported library targets. The shims live under
`<build>/public-checks/<module>/` and are rewritten only when their content changes; the aggregate target is
`crd-header-checks`. A module that builds no library target on the host (`kir-hip`, `kir-metal` everywhere;
`gpu-context-dx12`, `kir-dx12`, `kir-webgpu` on Linux) has no consumer view there and is listed as unchecked in the
configure output, never counted as passed.

A documented fragment that is not a self-contained header (an X-macro table, a platform-only header without its own
guard) is excluded by `crd_public_header_exclude(<module> <header> REASON <text>)`; a stale exclusion (unknown
module, header gone) fails the configure. The first complete run needed none: the 21 failures MSVC found on the
original tree were all header or boundary defects and were fixed as such (below); GCC, run on the tree after the
first fifteen fixes, reported the six hesap-tensor headers until the include-only edge carried `hesap` too, then none.

Include-only edges are part of the view. ADR-0096 keeps `crd-hesap-tensor` free of a link edge to `hesap-dense`
(the `smoke_hesap_tensor` isolation gate) while `batched.hpp`, `einsum_exec.hpp`, `tt.hpp`, `decomp.hpp`, `nn.hpp`
and `sparse_cp.hpp` are public header-only surfaces over the dense GEMM: the module therefore publishes the
`hesap-dense` and `hesap` include directories as include-only `PUBLIC` edges (the Philox precedent of the same ADR),
the consumer view sees the headers, and the link obligation stays with the consumer, as the module documents. The
registry does not record include-only edges; that is a listed gap, not a hidden one.

## What the first run found and fixed

| Class | Headers | Fix |
|---|---|---|
| Missing include in a public header | `math/mat_simd_f32.hpp` (`Mat4`/`Vec4` from `mat.hpp`, `vec.hpp`), `geometry/convex/sat.hpp` (`<optional>`), `geometry/curves/queries.hpp` (`detail::floor_mod` from `arclength.hpp`), `hesap/dense/detail/gemm_pack.hpp` and through it `syrk_microkernel.hpp` (`crd::hesap::conj` from `hesap/complex.hpp`), `hesap/quadrature/cubature.hpp` and `qng.hpp` (`detail::qmax` from `gauss_kronrod.hpp`), `hesap/opt/sqp_equality.hpp` (`detail::chol_solve` from `levenberg_marquardt.hpp`), `hesap/sparse/structural.hpp` (`ConstSpan` from `containers/span.hpp`) | the include the header always needed |
| Public header over a private link edge | `ceir/host/host_provider.hpp` uses `crd::jobs::Priority` while `crd-jobs` was `PRIVATE` | the edge is `PUBLIC`; the registry already declared `jobs` |
| Public header over an undeclared module | `hesap/resources/tensor_artifact.hpp` (hesap-tensor), `hesap/autodiff/einsum_reverse.hpp` (hesap-tensor), `hesap/opt/newton_sparse.hpp` and `levenberg_marquardt_sparse.hpp` (hesap-direct, formerly a test-only edge) | `PUBLIC` link edges and `DEPENDS` entries; every edge acyclic (no module depended on resources, autodiff or opt) |
| Public header over a private include-only edge | the six `hesap/tensor` header-only surfaces over `hesap-dense` (and its `hesap/complex.hpp`) | include-only `PUBLIC` edges (ADR-0096) |
| Public header over a private third-party include | `imgui/unit_preferences_inspector.hpp` draws with the ImGui API | the ImGui include directories are `SYSTEM PUBLIC` on `crd-imgui` |

The default `win-debug` graph after the fixes differs from the REPO.DEV.7 snapshot only in added `-I` flags of the
modules with new edges and their consumers (160 of 1,814 compile commands; nothing removed; the same 47,323 Ninja
lines) and in the CMake re-run rule listing the new modules.

## Hosted lanes and the local check

Two complete-tier presets carry the option: `win-public-checks` and `linux-gcc-public-checks` (Debug bases, tests
on, mapped in [ci-tiers.json](../../.github/ci-tiers.json) to the `windows` and `linux-gcc` jobs). Each builds the
whole preset plus the shims and runs the suite plus the consumer test; the change tier is untouched. The evidence
bundle's build section records the added compile edges.

Locally, `python scripts/check-headers.py --build build/win-debug --changed` (or named headers) compiles the changed
public headers standalone with the flags of a sibling translation unit of the owning module, taken from the build
directory's compile database through the tidy gate's synthesis (PCH inputs stripped, header as the main file), with
every output the command names (object, program database, dependency file) redirected into a scratch directory, so
the build tree is read and never written. This is the module's own view, not the consumer's: it catches a missing
include before a push; the boundary proof is the lane's.

## The relocatable package

[CrdPackage.cmake](../../cmake/CrdPackage.cmake) adds install rules only (no build statement), so every
configuration carries them and `cmake --install <build>` produces a prefix with `include/` (the public headers,
`*.in` excluded, plus the generated `crd/core/build_config.hpp`), `lib/` (the archive) and
`lib/cmake/Cerid/` (`CeridConfig.cmake`, its version file, the exported `Cerid::` targets). The exported surface is
`core` (`Cerid::core`); extending it to a family is the documented next step, one `crd_package_library()` call per
library whose usage requirements are export-clean: build-tree paths and engine-only interface targets wrapped in
`$<BUILD_INTERFACE:...>`, so a consumer does not inherit `crd-warnings` (`/W4 /WX` or `-Wall -Werror`) and the
installed `CeridTargets.cmake` names no source, build or prefix path.

One installed prefix is one profile. `build_config.hpp` differs per configuration (asserts, profiling,
`CRD_DEBUG`/`CRD_RELEASE`, the log level), so `CeridConfig.cmake` records the build type and the switches it was
installed from, refuses a single-configuration consumer that configures another build type
(`Cerid_ALLOW_PROFILE_MISMATCH=ON` overrides), and warns under a multi-configuration generator, where the installed
header belongs to whichever configuration ran the install: the Visual Studio solution install is a declared gap,
not a claimed capability. Cerid is static-only: `BUILD_SHARED_LIBS=ON` fails the configure, and the package exports
static archives.

`scripts/test-package-consumer.py` is the proof, registered as the `crd-package-consumer` CTest under the option
with the lane's own generator, make program, compiler, build type and switches (locally `--preset <name>` resolves
the same from CMakePresets.json): configure a core-only build (`CRD_MODULES=core`, tests, sandbox and benchmarks
off), build, install, verify the layout (every public header, no template), move the prefix, grep every installed
`.cmake` file for the source, build and pre-move paths, write a consumer that only calls
`find_package(Cerid CONFIG REQUIRED)` and links `Cerid::core`, configure it against the moved prefix, build it,
prove its compile commands carry no engine warning flag, run it and compare the profile values it prints from the
installed header with the packaging switches, then prove the two refusals (mismatching build type, shared
libraries). It passed on MSVC 19.51 (Ninja, `win-debug` profile) and GCC 13.3 (`linux-gcc-debug` profile).

## Not in scope

Exporting families beyond `core`, an include-only edge notion in the registry, shared-library builds, a
multi-configuration install contract, and the fuzz and sanitizer instruments (REPO.DEV.9) are separate rows.
