# Building, Testing & Troubleshooting

The canonical build/verification reference for Cerid. Coding standards, engineering principles,
and contributor conduct live in [`AGENTS.md`](../AGENTS.md); the doc map in [`docs/README.md`](README.md).

## Requirements

- **C++20** (no compiler extensions) · **CMake ≥ 3.25** + **Ninja** (`CMakePresets.json`)
- **MSVC 2022/2026** (primary; VS path `C:\Program Files\Microsoft Visual Studio\18\Community\`),
  clang-cl (verified in CI), GCC (Linux in CI)
- **Vulkan SDK 1.3+** (`$env:VULKAN_SDK` must be set for shader compilation)
- Test framework: **Catch2 v3** (via CPM.cmake) · Format: clang-format · Lint: clang-tidy
- MSVC `/Zc:preprocessor` required (for `__VA_OPT__` in log macros)
- Config: toml++ · Graphics: GLFW 3.4, Vulkan 1.3, shaderc, spirv-reflect · Debug UI: Dear ImGui (docking)

## Quick start

```powershell
# Windows with MSVC (primary)
cmake --preset win-debug      # Debug + asserts + profiling
cmake --build --preset win-debug
ctest --preset win-debug

# Other key presets: win-release (LTO) · win-relwithdebinfo · win-clang-cl ·
# win-asan (see the ASan DLL note below) · win-shipping · win-tidy (tidy runs during build)
# Linux (CI mirrors): linux-gcc-{debug,release,relwithdebinfo,asan,shipping,debug-sse2}
```

```powershell
# Format / lint a single file
clang-format -i <file>
clang-tidy -p build/win-debug <file>

# Single test binary (note: --reporter, not --reporters)
& "D:\Dev\cerid\build\win-debug\tests\<module>\crd-test-<module>.exe" --reporter compact
& "...\crd-<module>-tests.exe" "[tag]"     # filter by tag
```

## Per-slice verification (Definition of Done)

```powershell
# 4-config DoD (debug + asan + shipping + tidy) — default for CPU-only slices.
# Run SEQUENTIALLY (no -Parallel) and Ninja-capped — see "Host instability" below.
.\scripts\per-slice-check.ps1

# 5-config (adds win-release) — GPU / LTCG-sensitive slices
.\scripts\per-slice-check.ps1 -IncludeRelease

# Cluster-close: the 18-config full sweep (11 Windows + 7 Linux)
.\scripts\full-sweep.ps1

# Linux presets from Windows (WSL2; builds to ~/cerid-build/<preset> on native ext4)
.\scripts\wsl-build.ps1 linux-gcc-release
```

**Verification runs `ctest --preset <X>`, NOT the test binary directly.** Guard tests
(`crd-no-non-ascii-test-names`, `crd-simd-emission-check`, `crd-no-std-math-check`,
`crd-no-std-sort-check`, `crd-no-untagged-physical-numeric`, `crd-no-std-transcendental-check`,
`crd-hesap-v13-no-exceptions`, …) are registered only with CTest and never appear in a test binary's
`--list-tests`. A binary saying "All tests passed" can coexist with a failing guard — both must be green.

**Smoke tests** are standalone executables in `build/<preset>/runtime/` and are NOT registered with
CTest. Headless set (no GPU/window): `smoke_config smoke_containers smoke_filesystem smoke_frame_clock
smoke_jobs smoke_log smoke_math smoke_memory smoke_shader smoke_resources smoke_resources_async
smoke_resources_reload smoke_resources_stream smoke_resources_render smoke_texture smoke_mesh
smoke_material smoke_hesap_substrate smoke_hesap_blas1 smoke_hesap_sparse smoke_hesap_matrix_resource
smoke_hesap_tensor smoke_virtual_memory smoke_virtual_memory_allocator`. GPU/window set (run manually):
`smoke_app smoke_imgui_overlay smoke_renderer smoke_rhi_api smoke_rhi_vulkan_bootstrap smoke_window
smoke_asset_import`.

**Benchmarks:** every measured board is written to [`docs/bench/`](bench/) at measurement time
(convention + naming in `docs/bench/README.md`).

## Adding a module / test

1. `engine/<name>/include/crd/<name>/<name>.hpp` (umbrella) + `src/` + `CMakeLists.txt` (copy an
   existing module's pattern); `add_subdirectory(engine/<name>)` in the root CMakeLists.
2. `tests/<name>/CMakeLists.txt` + at least one Catch2 test; auto-discovered by CTest.
3. Smoke in `runtime/examples/smoke_<name>.cpp` where relevant.
4. Document in `docs/systems/<name>.md` once shipped.

## Platform notes (PowerShell)

- Use PowerShell-compatible commands (`Get-Content`, `Select-String`, `Remove-Item`), not POSIX tools.
- Run executables with absolute paths + the call operator: `& "D:\Dev\cerid\build\...\smoke_foo.exe"`
  (relative `.\build\...` fails in some invocation contexts).
- **PowerShell 5.1 text I/O mangles UTF-8** (`Get-Content -Raw`/`Set-Content -Encoding utf8` reads
  ANSI + writes BOM ⇒ mojibake). For scripted edits use
  `[IO.File]::ReadAllText($f, [Text.Encoding]::UTF8)` + `WriteAllText` with
  `[Text.UTF8Encoding]::new($false)`.

## Troubleshooting — known issues and permanent fixes

### Host instability: i9-14900K Raptor Lake — cap builds, never run all-core

The dev host's 14900K carries the documented Vmin-shift instability defect; sustained all-core builds
trigger `0x0000000A` bugchecks. **Software cap = harm reduction; the hardware fix is BIOS Intel Default
Settings + IPDT + (if failing) Intel's 5-year RMA.** Mandated workflow: run the DoD **sequentially**
(never `-Parallel`); builds are Ninja-capped (`per-slice-check.ps1` / `full-sweep.ps1` default
`-BuildJobs` to half the logical cores; tuning ladder 16 → 12 → 8 → 6); set a persistent user env var so
ad-hoc builds are capped too: `[Environment]::SetEnvironmentVariable('CMAKE_BUILD_PARALLEL_LEVEL','16','User')`
(WSL: `export CMAKE_BUILD_PARALLEL_LEVEL=16` in `~/.bashrc`). Benchmarks + `parallel_for`-saturating test
runs are the same hazard class. Inspect dumps with WinDbg `!analyze -v`.

### win-asan: CTest fails with exit 0xc0000135 (DLL not found)

Add the MSVC tools dir to PATH before ctest (every session):

```powershell
$asanDir = "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\MSVC\14.50.35717\bin\Hostx64\x64"
$env:PATH = "$asanDir;$env:PATH"
ctest --preset win-asan
```

### win-release: LTCG interprocedural bugs

- MSVC LTCG can mis-propagate a `nullptr` store across inlined TUs (the ResourceManager eviction case):
  keep `CRD_NOINLINE` on `evict_block_locked` / `try_evict_to_budget`.
- **Append new pure-virtuals at the END of an interface** — inserting mid-interface shifts vtable slots
  and surfaces as win-release-only crashes. If you must reorder: full clean rebuild + `-IncludeRelease`.
- MSVC 19.51 LTCG miscompiled in-loop `crd::usize{1} << loop_var` (the `wpt` CI SegFault) — avoid the
  in-loop variable-shift pattern in hot loops (iterate by id / doubling counter).
- Visual Studio IDE: after header changes that grow a class, use **Rebuild** (stale-`.obj` C4789).

### Ninja `#deps 0`: header changes silently never recompile (NEVER use the VS-bundled CMake)

On this Turkish-locale host, the VS-bundled CMake fork stores an English `msvc_deps_prefix` while cl.exe
emits localized include-notes ⇒ **zero header deps recorded** ⇒ stale objects, phantom greens, mixed-layout
SIGSEGVs. It also RE-ARMS itself by rewriting `CMAKE_COMMAND` in `CMakeCache.txt` on any regenerate it runs.
**Policy:** always invoke the standalone CMake explicitly — use the helper scripts, which bake it in:
`scripts/configure-preset.bat <preset>` · `scripts/build-target.bat <build-dir> <targets...>` ·
`scripts/check-deps.bat <build-dir> <obj>` · `scripts/run-ctest.bat <build-dir> <regex>`.
Diagnose: `ninja -t deps <obj>` → `#deps 0` = broken; audit `CMakeCache.txt` `CMAKE_COMMAND` must be
`C:/Program Files/CMake/bin/cmake.exe`. A broken dir must be wiped + reconfigured.

### Assorted permanent fixes

- **ResourceManager destructor**: two-pass unload-then-free (payloads holding `ResourceHandle`s) — do not
  collapse to one pass (use-after-free).
- **`crd::containers::String`**: `set_heap_capacity` stores usable chars, not allocation size (the
  `capacity == allocation size` heap-overflow class).
- **jobs::init() in test binaries**: one owner per binary (double-init crashes).
- **`String` in log macros**: pass `.c_str()`/`StringView` (no `std::formatter` for `String`).
- Catch2 flag is `--reporter` (not `--reporters`); a `[` in a TEST_CASE *name* breaks `catch_discover`
  registration (tags only).
- Transient MSVC LTCG `C1001` / clang-tidy AV crashes that clear on a retry-clean are known upstream
  bugs: close on retry-PASS, file as debt, do not re-sweep.
