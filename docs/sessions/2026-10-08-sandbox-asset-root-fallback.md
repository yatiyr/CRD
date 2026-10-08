# Sandbox engine asset root falls back to the source tree, 2026-10-08

<!-- doc-role: historical -->
> Dated evidence. Live owner: [REPO.VS](../ROADMAP.md#slice-repo.vs) (the sandbox's developer launch). Layout:
> [repository layout](../design/repository-layout.md#native-visual-studio-solution). Rules: [AGENTS](../../AGENTS.md).

## Goal

A user-requested batch outside the current row (DIAG.7a). crd-sandbox reads its engine assets through
`CRD_ASSETS_DIR`. The Visual Studio debugger (`VS_DEBUGGER_ENVIRONMENT` in `sandbox/CMakeLists.txt`) and ctest set it.
A direct launch from a terminal, a script or a smoke run did not. Then `scene_programs.manifest` was not found and
the log said "Scene renderer unavailable — falling back to overlay-only". The window showed only the clear colour
and the ImGui panels. With `CRD_ASSETS_DIR=<repo>/assets` the same exe rendered the full scene (10,000 instances,
shadows). The fix makes a direct launch of a developer build render the scene too, and leaves shipping unchanged.

The change is tracked under REPO.VS. That row made the Debug sandbox ready to run from the IDE, and its layout
document is the one place that describes `CRD_ASSETS_DIR`. The row stays Done; this note adds evidence.

## Change

- `sandbox/src/asset_root.{hpp,cpp}` (new). `choose_asset_root(env_value, source_assets_dir, shipping, usable)` is a
  pure function, and its probe is its only file-system access. The rule:
  1. `CRD_ASSETS_DIR`, set and non-empty, always wins. It is not probed, so an explicit root the renderer rejects
     still logs REJECTED. It never switches silently to another tree.
  2. A developer build falls back to `<CRD_SOURCE_DIR>/assets` when the probe accepts it.
  3. Otherwise there is no root. `SourceTreeUnusable` is returned when the source tree was probed and refused, and
     `None` when it was never considered (a shipping build, or no candidate).
  `is_usable_asset_root` is the real probe: the path is a directory that holds `scene_programs.manifest`.
- `sandbox/src/main.cpp`. The asset root installs from the choice, still before `init_programs`, and the INFO line
  now names its source: `asset root '<dir>' from CRD_ASSETS_DIR -> installed` or
  `... from the build's source tree (CRD_ASSETS_DIR unset) -> installed`. An unusable source tree logs one INFO line
  that names the probed tree. The overlay-only WARN and the three "set CRD_ASSETS_DIR" ERROR lines are unchanged.
  The CLI comment at the top of the file now documents `CRD_ASSETS_DIR`.
- `sandbox/CMakeLists.txt`. `asset_root.cpp` joins `crd-sandbox`. `$<${_crd_shipping}:CRD_SANDBOX_SHIPPING=1>` gives
  the sandbox its shipping flag from the root's existing shipping condition, so a native multi-config solution gets
  it per configuration. `CRD_SHIPPING` was a CMake option only; no C++ header carried it. The flag goes to the
  function as an argument, never as an `if` on a constant.
- `tests/applications/sandbox-inspect`. `crd-sandbox-inspect-tests` compiles `asset_root.cpp`, the same way it already
  compiles `inspect_panel.cpp`, and adds `test_asset_root.cpp` (6 test cases, tag `[asset-root]`).
- `docs/design/repository-layout.md` describes the fallback.

## Verification

All runs are on this box (Windows 11, real GPU) and on WSL (the Linux reference host).

| Check | Result |
|---|---|
| win-debug `crd-sandbox-inspect-tests` | 484 assertions / 17 test cases pass; `[asset-root]` 46 / 6 |
| win-shipping `crd-sandbox-inspect-tests` | 484 / 17 pass |
| win-clang-cl-shipping `crd-sandbox-inspect-tests` | 484 / 17 pass (thin-LTO linked first time) |
| `crd-sandbox` build | win-debug, win-shipping, win-clang-cl-shipping, linux-gcc-debug: clean |
| WSL `crd-sandbox-inspect-tests` | linux-gcc-debug, linux-gcc-asan, linux-clang-tsan: 484 / 17 pass, no sanitizer report |
| ctest `-R "asset root"` (win-debug) | 6 / 6 discovered and pass |
| `ctest -R "^crd-no-\|^crd-check" -j 8` (win-debug) | 8 / 8 pass |
| `tidy-files.py` (4 files, LLVM 20.1.8) | all clean |
| `clang-format --dry-run --Werror` | new files clean; the touched `main.cpp` hunks match the formatter |
| `check-allman-braces.py` | PASS |

The tests cover:
- a set variable wins over a usable source tree and is not probed, in developer and shipping builds;
- with the variable unset, a usable source tree is chosen, and the probe sees `<source>/assets`;
- an unusable source tree gives no root and reports `SourceTreeUnusable`, and a missing candidate or probe is not
  probed;
- a shipping build never falls back and never probes;
- an empty variable counts as unset in both builds;
- the real probe accepts the repository's `assets/` tree and a scratch tree that holds the manifest. It refuses the
  same scratch tree without the manifest, the manifest file itself, a missing directory, and an empty or null path.
  Scratch trees live under `fs::temp_directory()`.

**Teeth.** Two breaks were tried on win-debug, each rebuilt and run:
- A: the source-tree branch returned no root. 3 of 6 cases failed (7 assertions).
- B: the shipping guard was disabled. 2 of 6 cases failed (7 assertions).

Each edit was restored with a write, so its mtime changed. win-debug was rebuilt and rerun: 484 / 17 pass.

**The real exe.** Smoke runs used `--smoke-test 2 --screenshot-at 1.5 --screenshot <scratch>`. The default
screenshot time (2.5 s) comes after a 2 s smoke run ends, so it needs `--screenshot-at`. Every run exited 0.
- win-debug, `CRD_ASSETS_DIR` unset: `asset root 'D:/Dev/cerid/assets' from the build's source tree (CRD_ASSETS_DIR
  unset) -> installed`. No overlay-only line, no manifest error. The screenshot shows the full instanced scene with
  shadows; 5,381 instances were drawn on the last frame.
- win-debug, `CRD_ASSETS_DIR` set to an empty scratch directory: `... from CRD_ASSETS_DIR -> REJECTED`, then the
  manifest errors and the overlay-only WARN. The variable won even though the source tree was usable.
- win-debug, `CRD_ASSETS_DIR` set to a scratch copy of `assets/` (without `source/`): `... from CRD_ASSETS_DIR ->
  installed`, and the scene rendered from the copy.
- win-shipping, `CRD_ASSETS_DIR` unset: no asset root line, the manifest errors, and the overlay-only WARN, with
  0 instances drawn. This matches the behaviour before the change.

## Limitations

- `crd-gizmo-probe` (`sandbox/src/gizmo_probe.cpp`) reads `CRD_ASSETS_DIR` itself and was not changed. It can reuse
  `choose_asset_root` if it needs the same fallback.
- `linux-gcc-shipping` was not built locally. The shipping flag was checked on win-shipping and
  win-clang-cl-shipping, and CI builds the Linux shipping lane.
