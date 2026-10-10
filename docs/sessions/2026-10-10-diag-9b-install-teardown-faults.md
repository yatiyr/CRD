# DIAG.9b fault injection at install, execute and teardown, 2026-10-10

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.9b](../ROADMAP.md#slice-diag.9b). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-9b). Rules: [AGENTS](../../AGENTS.md).
> Preceding: [DIAG.9b cook faults](2026-10-10-diag-9b-cook-faults.md).

## Goal

The seventh slice of DIAG.9b: the remaining lifecycle stages of "inject during load/cook/install/execute/reload/
teardown; previous working assets survive recoverable failure". Load and reload were shown in the
[fault injection](2026-10-10-diag-9b-fault-injection.md) slice, cook in the [cook faults](2026-10-10-diag-9b-cook-faults.md)
slice. Still open after this batch: refusal of arbitrary external I/O replay as a shipping check, a real host
(crd-sandbox), and the hosted run. This batch is committed locally and not pushed.

## What was there (checked before coding)

- **Install** has two forms in the engine, both already reading through the I/O fault seam:
  - `ResourceManager::mount_manifest` installs a cooked pack into the running manager (a patch or DLC pack over the
    mounted ones; the newest mount serves an id). It reads the pack once with `read_file_binary`, parses it, and only
    then touches the live table.
  - `SceneRenderer::reload` installs a render asset into the renderer's live slot (RAF-11): the resolver re-reads the
    frame's source with `read_file_text` (on the seam since the cook slice), the reloader stages the re-cooked graph,
    and the commit swaps it in at a frame boundary and bumps the generation. The frame kind needs no device.
- Reading `mount_manifest` against "previous working assets survive" found one defect: it checked the manifest
  container but not the entries' byte ranges. A pack torn on disk inside its artifacts (its manifest whole) mounted;
  its entries took over the ids an earlier mount served, and every later load of those ids failed reading past the
  end. The hot-reload path (`do_reload_mount`) already skipped an out-of-range entry.
- **Execute**: the only fault seam on an executing path is the GPU one. A device loss injected at a live queue
  submission (the [device loss](2026-10-10-diag-9b-device-loss.md) slice) is injection while work executes, with
  recovery on a fresh device. No CPU execution path (the CEIR plan executor, crd-jobs) has a recoverable read or
  allocation of its own; the DX12 frame descriptor arena's `try_allocate` (`Dx12FrameDescriptors::reserve`) is the
  only execute-time recoverable allocation, and it is left as a candidate rather than claimed.
- **Teardown** (`~ResourceManager`, dropping handles) performs no read and no `try_allocate`; it unloads payloads,
  including the ones a reload retired and still holds for a one-frame grace period. The manager requires every async
  load to be waited before it dies.
- An injected short read makes `read_file_binary` and `read_file_text` fail outright, so a torn file on disk is the
  only way a truncated pack reaches the parser; the torn-pack case therefore writes the cut file itself.

## What changed

- **crd-resources:** `mount_manifest` refuses a pack whose manifest names bytes past its own end (overflow-safe:
  `blob_size > size || blob_offset > size - blob_size`), before the live table is touched, with an error naming the
  range. The header comment states the whole-refusal contract.
- **Tests only otherwise.** crd-scene-render-tests now links crd-perf for the injector and recorder.

## Tests

- **crd-resources-tests** (`tests/assets/resources/test_fault_injection.cpp`), 3 new cases:
  - *a pack install interrupted by a failed or short read keeps the mounted assets* (`[install]`): a base pack is
    mounted and its mesh loaded; a patch pack with the same id and new content is installed under a failed read, a
    short read cut inside the container header (16 bytes) and one cut four bytes into the artifact. Each install
    returns an invalid id; the mount and entry counts, the id's serving mount, and the loaded payload's pointer,
    generation and bytes are unchanged; the capture's fault event names the patch file; the recipe's one-hit plan
    interrupts the same install alone. Disarmed, the patch mounts, serves the id, and a reload of it swaps the new
    content in.
  - *a torn pack is refused at install and the mounted asset keeps serving* (`[install]`, every build): the patch on
    disk is cut halfway through its artifact, one byte short, and inside its manifest container; none mounts, and a
    first load afterwards reads the base mount's artifact. The whole patch then mounts.
  - *teardown after injected faults releases everything the run allocated* (`[teardown]`): the manager and the MESH
    loader's payloads allocate through a counting allocator; three interrupted reloads (allocation failure, failed and
    short read), a refused install and two failed streamed loads (cancelled, failed) fire; a streamed load delayed
    50 ms is in flight when teardown starts and the required drain waits it out (Ready, its bytes right); a clean
    reload leaves a retired payload in its grace period. With a plan that fails every read and every `try_allocate`
    armed and hooked, dropping the handles and destroying the manager consults no site; afterwards no allocation is
    live. The recorder's first window (post window 200 events) is delivered at shutdown as `shutdown`, truncated,
    with the six later faults suppressed.
- **crd-scene-render-tests** (`tests/rendering/scene-render/test_diag_install_faults.cpp`), 1 new case: *a frame
  install interrupted by a failed or short read keeps the live generation* (`[install]`): a frame installed by id
  from a private asset root, its source edited, then reloaded under a failed read, a read cut inside the first line
  and one cut a byte short of the end. Each reload reports refused, the generation stays 0, the event names
  `install_gate.frame.toml` (the install's first read is the frame's source), and the recipe's one-hit plan
  interrupts the same install alone. Disarmed, the old source back on disk reloads as a no-op (the reloader's
  installed content is still the old graph) and the edited source installs generation 1.
- Every case asserts the refusal branch on a shipping lane (arming Unavailable, the hook not installed, the install
  running plain).

Teeth on win-debug, each observed failing, then restored by writing the saved source back and rebuilding:
- the range check removed from `mount_manifest`: the torn-pack case fails (the cut packs mount and take the id);
- a failed manifest read mounted as an empty pack: the injected install case fails (a valid id, the mount count
  growing);
- `~ResourceManager` not unloading the payloads in their grace period: the teardown case fails (4 allocations live);
- `SceneRenderer::reload` reporting success when the reloader refused: the renderer case fails on every faulted
  install;
- the reloader forgetting the installed content hash when a stage fails: the renderer case fails (the old source
  reinstalls as generation 1, the edited one as 2).

## Verification

- win-debug: the whole tree built; crd-resources-tests 126 cases and crd-scene-render-tests 114 cases (its GPU
  cases on this machine's device) green.
- win-shipping and win-clang-cl-shipping (both touched executables rebuilt): crd-resources-tests 126 and
  crd-scene-render-tests 114 green; the injected cases ran their refusal branches and the torn-pack case ran whole.
- win-asan (inside vcvars): crd-resources-tests 126 and crd-scene-render-tests 114 green, no sanitizer report.
- WSL, one lane at a time, reconfigured, from a scratch directory under the home directory: linux-gcc-debug
  (crd-resources-tests 126, crd-scene-render-tests 81 on lavapipe), linux-gcc-asan and linux-clang-tsan
  (`setarch -R`; crd-resources-tests 126, crd-scene-render-tests `[install],[raf11]` 6). The `[install]` and
  `[teardown]` cases then passed 3 more runs on each gcc lane and 10 under TSan. No ASan, LeakSanitizer, UBSan or
  TSan report.
- Strict tidy clean on the 3 changed C++ sources. clang-format differs only on the repository's hand alignment and
  include grouping. The Allman check, the 8 ctest guards, `check-master-plan` and `check-repository` pass.

## What remains in DIAG.9b

- Refusal of arbitrary external I/O replay as a shipping capability check.
- A real host (crd-sandbox) installing the recorder, its triggers, a `FlightCommandHost` sink (with
  `refresh_modules` at start and after module loads) and both command sets.
- The hosted run: in addition to the earlier list, crd-resources-tests with the `[install]` and `[teardown]` cases
  and crd-scene-render-tests with the `[install]` case green on all six lanes.
