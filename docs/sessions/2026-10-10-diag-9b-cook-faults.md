# DIAG.9b fault injection during a cook, 2026-10-10

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.9b](../ROADMAP.md#slice-diag.9b). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-9b). Rules: [AGENTS](../../AGENTS.md).
> Preceding: [DIAG.9b bundle identity](2026-10-10-diag-9b-flight-bundle-identity.md).

## Goal

The sixth slice of DIAG.9b: inject faults during a cook and show that the previous working assets survive. Load and
reload were shown in the [fault injection](2026-10-10-diag-9b-fault-injection.md) slice. Still open after this batch:
injection during install, execute and teardown, refusal of arbitrary external I/O replay as a shipping check, a real
host (crd-sandbox), and the hosted run. This batch is committed locally and not pushed.

## What was there (checked before coding)

- The cook stage is the asset processor, crd-cooker's `cmd_cook` (tools/asset_cooker): it scans a source tree, decides
  each job's freshness from the cook database's recorded inputs, runs the handler through `CookIO` (the declared-input
  seam), publishes cached artifacts with write-then-rename and writes the pack last. Any failure returns before the
  pack and database are written, and a job that had begun leaves a dangling journal `begin`.
- Its source and input reads (`read_file_binary`) were already on the I/O fault seam, but `read_file_text` was not, and
  that is how the processor reads each asset's `.meta` (the asset's id), the id sidecars of sub-products, the cook
  database and journal. So were configuration and scene-render's authored assets.
- Reading the code against the "previous working assets survive" clause found three defects, each reachable by a
  transient read failure of a file that is there (a sharing violation, a failed or short read):
  - an unreadable `.meta` took the "malformed" path: the asset was given a new random id and its `.meta` rewritten, so
    the next pack named the asset by an id no reference knew;
  - `CookIO::stable_id` did the same for a sub-product's id sidecar;
  - `CookIO` recorded any failed read as absence. For an optional input (one a handler uses when present) the handler
    succeeded without it and the processor published a product cooked without a file that exists.
- The crd-cook path of CEIR programs (`ReloadSet::cook_source`) takes source text from its caller and allocates only
  through `allocate` (non-null or fatal), so it has no recoverable read or allocation seam of its own; the asset
  processor is the cook a host's packs come from.

## What changed

- **crd-platform:** `read_file_text` consults the I/O fault seam (`IoFaultOp::ReadWhole`, as `read_file_binary`): a
  failed, short or cancelled answer returns false with the output cleared, a delay is slept. With no hook it is one
  atomic load; a shipping build compiles the seam out.
- **crd-perf:** an I/O fault's flight event names the file it failed by its last path component only ("injected
  failed-read at hit 4 (seed 11) on a.bin"), the causal input the capture was missing. A host's directories never
  reach the evidence; a name past the 111-byte event bound is clipped and flagged.
- **crd-cooker:**
  - `meta_read` returns Ok, Unreadable or Malformed. Unreadable stops the cook ("cannot read the .meta of ...; its id
    is kept and nothing is published"); only Malformed regenerates.
  - `CookIO` keeps the first input that exists but could not be read (`unreadable()`, `unreadable_path()`); absence is
    still recorded as a dependency. `cmd_cook` publishes nothing for a run whose job read such an input, whatever the
    handler returned.
  - `stable_id` returns false with a null id for a sidecar that exists but cannot be read, and never mints over it; an
    absent sidecar is minted and persisted as before.

## Tests

- **crd-resources-tests**, 4 new `[resources][diag9b][fault][cook]` cases in `tests/assets/resources/test_fault_injection.cpp`
  (it already links the cooker and crd-perf), each on a temp tree cooked by the real `cmd_cook`:
  - *a cook interrupted by a failed or short read publishes nothing and keeps the asset's id*: after a clean cook the
    source is edited, then four plans fail the cook's source read, cut it short, fail the `.meta` read that yields the
    id, and fail the `.meta` read as a recorded input. Each run returns nonzero; the pack, the cached artifact, the cook
    database and the `.meta` are byte-identical to the clean cook's; the journal holds `begin a.bin` exactly when the
    job had begun; the capture's InjectedFault trigger has this run's fault event just before it, naming the file
    (`on a.bin`, `on a.bin.meta`); the recipe's one-hit plan, armed alone from the same state, interrupts the same read.
    A last failed cook leaves the `begin` dangling, and the next clean cook recooks the distrusted job, removes the
    journal and publishes the new content under the same id.
  - *a read fault in the cook's up-to-date check recooks the same product*: on an unchanged tree a failed read of the
    source (it looks absent) or of the cached artifact (it looks torn) recooks the job and writes a byte-identical pack
    and database, logged `cooked`; a failed read of the cached artifact the pack is assembled from publishes nothing.
  - *an input that exists but cannot be read fails the cook*: a test handler (`.optin`) whose `extra.dat` is optional;
    with `extra.dat` present and its read failed, the run returns nonzero and nothing changes; clean, the product holds
    the new source and `extra.dat`.
  - *an id sidecar that exists but cannot be read is never minted over*: through `CookIO` directly, with every read
    failing, `stable_id` refuses the existing sidecar (null id, `unreadable()` naming it, bytes unchanged) and still
    mints an absent one; disarmed it returns the kept id.
  - The read ordinals each plan names are stated in the case and checked through the event's file name, so a change in
    the processor's read order fails the case loudly rather than faulting a different file.
- **crd-perf-tests:** new `[fault]` case *a fired read's event names its file, never its directory*: forward-slash and
  backslash paths name only `hero.bin.meta` and `0123.crdr`, no directory text appears, and a 150-character name is
  clipped with the clip flagged.
- **crd-platform-tests:** the synchronous-read case also covers `read_file_text`: failed, short and cancelled answers
  return false with the output cleared and report `ReadWhole`; a delay is waited out; on a shipping lane the read runs
  plain.
- Every case asserts the refusal branch on a shipping lane (arming Unavailable, the hook not installed, the cook
  publishing the new content plainly).

Teeth on win-debug, each observed failing, then restored by writing the saved sources back and rebuilding:
- an unreadable `.meta` treated as malformed: the interrupted-cook case fails (pack, database, artifact and id change);
- `stable_id` falling through to mint over an unreadable sidecar: the sidecar case fails;
- the processor ignoring `CookIO::unreadable()`: the optional-input case publishes a product without `extra.dat`, and
  the `.meta`-as-input plan of the interrupted-cook case publishes;
- `read_file_text` consulting the seam but ignoring its answer: the platform case and the interrupted-cook case fail;
- the event built without the file name: the perf case and the cook cases' file-name checks fail.

## Verification

- win-debug: the whole tree built; crd-resources-tests 123 cases, crd-cooker-tests 30, crd-perf-tests 255,
  crd-platform-tests 49.
- win-shipping and win-clang-cl-shipping (every touched executable rebuilt): crd-platform-tests 49, crd-perf-tests
  122, crd-resources-tests 123 and crd-cooker-tests 30 green; the four cook cases ran their refusal branches.
- win-asan (inside vcvars): the same four suites green; the only sanitizer reports are the existing deliberate
  specimens' child processes.
- WSL, one lane at a time, from a scratch directory under the home directory: linux-gcc-debug, linux-gcc-asan and
  linux-clang-tsan (`setarch -R`) each built the four suites and passed crd-platform-tests 49, crd-perf-tests
  `[fault],[diag9b]` 38, crd-resources-tests 123 and crd-cooker-tests 30; the `[cook]` and `[fault]` cases then
  passed 3 more runs on each gcc lane and 10 under TSan. No ASan, LeakSanitizer, UBSan or TSan report.
- Strict tidy clean on all 10 changed C++ files (one raw-string finding in the new perf case fixed). clang-format
  differs only on the repository's hand alignment. The Allman check, the 8 ctest guards, `check-master-plan` and
  `check-repository` pass.

## What remains in DIAG.9b

- Injection during install, execute and teardown with the previous assets surviving.
- Refusal of arbitrary external I/O replay as a shipping capability check.
- A real host (crd-sandbox) installing the recorder, its triggers, a `FlightCommandHost` sink (with
  `refresh_modules` at start and after module loads) and both command sets.
- The hosted run: in addition to the earlier list, crd-resources-tests with the four `[cook]` cases and
  crd-cooker-tests green on all six lanes.
