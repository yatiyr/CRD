# DIAG.8b sandbox consumer, 2026-10-07

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.8b](../ROADMAP.md#slice-diag.8b). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-8b). Rules: [AGENTS](../../AGENTS.md).
> Preceding: [DIAG.8b inspect host and headless consumer](2026-10-07-diag-8b-inspect-host-and-headless-consumer.md).

## Goal

Fifth and last local DIAG.8b batch: the sandbox half of "inspect/step an authored program in headless and sandbox
consumers". crd-sandbox runs an authored CEIR program through the shared `InspectHost`, loaded through the app-first
program convention, with a frame loop that controls it without ever waiting on it.

The DX12 hardware fault output pasted again at the start of this step (`submit=00000000 wait=00000000
reason=00000000`, an empty removal record, exit 30) is the second run already recorded in
[the user-items note](2026-10-07-user-items-wpr-and-vulkan-loss.md). It adds nothing new; the user's decision stands
and the case was not run again.

## What was there (checked before coding)

- The app-first program read existed only as a private helper inside the renderer (`resolve_program_text`, used for
  the `.ckir` loads): `app://<id>` first, then `engine://<id>`, through the RAF-9 mount table. Nothing outside
  scene-render could reach it, and `ceir` was not a registered folder, so `engine://ceir/inspect_demo` had no on-disk
  shape and could not resolve.
- crd-sandbox ran no CEIR program. Its device-free precedent for hosted coverage is the showcase tests, which compile
  sandbox sources into a test executable.
- `Session::cancel` raises the flag but leaves the session on the stop until the executing thread leaves it (a resume
  moves it to running at once). A controller that polls for stops can therefore see the same stop again after a
  cancel.

## What changed

- **render-asset-core:** `ceir` is a program folder (`infer_type` Program, extension `.ceir`), beside `ckir`.
- **scene-render:** `SceneRenderer::resolve_program_text(rel, out)` is the public form of the renderer's own app-first
  program read and returns which mount won (`ProgramSource`: `App`, `Engine`, `NotFound`); the private `.ckir` sites
  call the same code.
- **sandbox/src/inspect_panel (`crd::sandbox::InspectPanel`):** loads (or hot-reloads) a program by its canonical
  folder/name through that read into an `InspectHost`, adds line breakpoints and watched lines, starts the entry, and
  exposes `tick()` for the frame loop plus commands (continue, step into/over/out, cancel, pause request) that name
  the generation the caller saw. `tick()` polls with a zero timeout; on a new stop (by sequence) it records the
  authored line and column and takes each watched value once (a bounded snapshot the paused thread answers); on the
  end it joins only an already-finished thread. An optional script applies one action per stop (then continue).
  - **Identity decision:** the asset id and the breakpoint file name are the canonical folder/name
    (`ceir/inspect_demo`), not the mount that won. With the mount in the name, an app file appearing or disappearing
    would change the asset id (`InspectHost` refuses another id) and turn every breakpoint `UnknownFile`.
- **crd-sandbox:** `--inspect [id]` (default `ceir/inspect_demo`), `--inspect-break N`, `--inspect-watch N` (repeatable,
  1-based, validated before anything runs), `--inspect-step a,b`, `--inspect-arg N`, `--app-assets <dir>`. Asked for
  and unavailable, it refuses to run (the `--lod` rule). Each frame ticks the panel and logs every stop, value and
  outcome; a window offers continue, the three steps, pause, cancel, run again and reload. The panel is destroyed
  before the scene renderer. The sandbox module declares `ceir`, `ceir-cook` and `render-asset-core`.
- **Tests:** `crd-sandbox-inspect-tests` (new `tests/applications/sandbox-inspect`, owned by scene-render with
  `TEST_DEPENDS ceir ceir-cook`), and the folder map case in `crd-render-asset-core-tests` covers `ckir` and `ceir`.

## Tests

`tests/applications/sandbox-inspect/test_inspect_panel.cpp` (5 cases, `[sandbox][inspect][diag]`); the test thread
plays the frame loop, the renderer is constructed only for its mounts (no device), and expected lines are scanned from
the committed text:

- **App first:** with no app root the panel loads the engine program (`Engine`), stops at the call line and returns
  36; an app file with one constant changed then shadows it (`App`), and reloading the same panel installs a newer
  generation of the same asset whose breakpoint rebinds to the same line and returns 144. A fresh panel resolves the
  app file; an unknown name and an unregistered folder resolve nothing and the panel cannot start.
- **Held:** a breakpoint stop at the call line, depth 0, with the values taken at the stop (3 as `!i32`, the length
  with its unit, the call `NotYetComputed`, the loop body `OutOfScope`); 50 more frames tick with nothing changing;
  step into stops in the callee (depth 1, the caller's value `OutOfScope`), step out at the loop with the call's value
  refreshed to 36; continue finishes with 36 and frames keep ticking. A scripted run (`into, out`) takes the same three
  stops by itself and finishes.
- **Running:** with a loop of 2^31-1 iterations, 20 frames tick while the program runs, each under the 1 s bound; a
  pause request lands in the loop (`PauseRequest`, the call's value available) and a cancel ends the run `Cancelled`.
- **Generations:** an edit saved while held is `Busy` and the run finishes with the old value; after the run it
  installs as a new generation, whose stop reads the edited constant; continue, pause and cancel naming the old
  generation are refused `StaleGeneration`, and the new one finishes with 144.
- **Destruction:** destroying the panel while held returns.

`tests/rendering/render-asset-core/test_asset_paths.cpp`: `engine://ckir/...`, `engine://ceir/...` and
`app://ceir/...` map to their files, and `ceir` infers as a program.

## Teeth (win-debug)

Each break was applied, the test target rebuilt and run; the file was then written back (mtime moved), rebuilt and
run green twice (5 cases, 164 assertions).

| Break | Result |
|---|---|
| `tick` waits up to 2 s for a stop instead of polling | the running case fails its per-frame bound |
| engine tried before app | 2 cases, 15 assertions fail (the shadow and the app-root reload) |
| `ceir` folder not registered | all 5 cases fail (nothing resolves) |
| watched values taken only at the first stop | 2 cases, 4 assertions fail (stale values at later stops) |
| sequence check removed | the cancel case fails in 2 of 3 runs (the held stop is read again and the panel never ends) |
| asset id taken from the winning mount | the mount-swapping reload fails |

The sequence check was not planned: the running case failed on the first build, which is how the cancel behaviour in
"What was there" was found.

## Evidence

- **win-debug:** the whole tree builds. `crd-sandbox-inspect-tests` 5 cases (164 assertions), five runs in a row
  before the teeth and two after; `crd-render-asset-core-tests` 19 (228); `crd-scene-render-tests` 113 (2,890, on this
  machine's GPU); `crd-ceir-cook-tests [inspect]` 4 (130).
- **crd-sandbox on this machine's GPU (win-debug, `CRD_ASSETS_DIR` set):**
  - `--inspect --inspect-break 16 --inspect-watch 14 --inspect-watch 13 --inspect-watch 16 --inspect-watch 19
    --inspect-step into,out --smoke-test 4`: loaded from `engine://`, breakpoint bound (1 site); stop 1 at
    `ceir/inspect_demo:16:9` with `3`, the length `7` with its unit, the call `not-yet-computed` and the loop body
    `out-of-scope`; stop 2 at `5:9` depth 1 with all four out of scope; stop 3 at `17:9` with the call `36`; finished
    with 36. Smoke PASS, 243 frames (238 on the final build, same stops and values).
  - `--inspect --app-assets <dir> --inspect-break 16 --inspect-watch 14 --smoke-test 3` with the constant changed in
    the app file: loaded from `app://`, stopped at `16:9` with `6`, and stayed held while the smoke run presented
    167 frames (`paused after 1 stops; 167 frames ticked the panel`); smoke PASS, and the window's teardown cancelled
    and joined the held program.
  - `--inspect-break 0` and `--inspect ceir/no_such` both refuse to run (exit 1) with the reason.
- **win-shipping, win-clang-cl-shipping (clean thin-LTO links), win-asan (inside vcvars, no ASan report):**
  `crd-sandbox-inspect-tests` 5 (164), `crd-render-asset-core-tests` 19 (228) and `crd-scene-render-tests` 113
  (2,890, this machine's GPU) pass, and `crd-sandbox` builds. The win-asan `crd-scene-render-tests` run printed its
  all-passed summary with no sanitizer line; a second run to read its exit code ran past 20 minutes under ASan and was
  ended by the machine's orphan watchdog, so that exit code is not recorded.
- **Linux (WSL):** linux-gcc-debug, linux-gcc-asan and linux-clang-tsan each configure and build
  `crd-sandbox-inspect-tests`, `crd-render-asset-core-tests`, `crd-ceir-cook-tests`, `crd-scene-render-tests` and
  `crd-sandbox`, and pass `crd-sandbox-inspect-tests` twice (5, 164), `crd-render-asset-core-tests` (19, 228),
  `crd-ceir-cook-tests [inspect]` (4, 130) and `crd-scene-render-tests "[raf11],[raf9],[csm]"` (20 cases, 609
  assertions). No ASan or UBSan report. Under linux-clang-tsan the panel's two threads are clean; the
  `crd-scene-render-tests` subset passes every assertion but exits 66 with 11 to 13 TSan reports (it varies by run),
  every one with the Vulkan validation layer or lavapipe in the stack, in `test_scene_render_gpu.cpp` device cases
  (frame-graph destruction and recording). That is the layer teardown class the DIAG.8a GPU batch recorded; the
  previous commit was not rebuilt under TSan to prove it pre-exists. No hosted lane runs TSan.
- **Checks:** strict tidy is clean on `inspect_panel.cpp`, `main.cpp`, the test, `identity.cpp`, `scene_renderer.cpp`
  and the two changed public headers. The tidy gate maps `inspect_panel.hpp` to another sandbox target and cannot
  resolve its includes, so it was analysed through `inspect_panel.cpp` with a header filter on the mirrored database:
  no finding. clang-format `--dry-run` shows only the repository's hand alignment and one-line `case` style on new code
  (its wraps were applied by hand). The Allman check, the 8 ctest guards (the container guard first caught a
  `std::string` in a sandbox log line, now removed), check-master-plan and check-repository pass.

## Hosted CI must show

`crd-sandbox-inspect-tests` (5 cases, 164 assertions) green on all six lanes, `crd-render-asset-core-tests` and
`crd-scene-render-tests` still green, `crd-sandbox` building on every lane, and the earlier DIAG.8b suites listed in the
row.

## Not covered

- The sandbox window's buttons are drawn in `main.cpp` and are not exercised by a test (they call the same panel
  commands the test drives). The two sandbox runs above are local-only; no hosted lane runs the windowed sandbox.
- A program whose entry takes no argument or several is not supported by the panel's `--inspect-arg` (one `i64`).
