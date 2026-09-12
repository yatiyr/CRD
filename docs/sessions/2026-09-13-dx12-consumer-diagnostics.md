# DX12 actual-consumer validation and repair

<!-- doc-role: historical -->
> Dated evidence. Live owner: [REPO.3c.5.d](../ROADMAP.md#slice-repo.3c.5.d); rules: [AGENTS](../../AGENTS.md).

## Scope and findings

Continued the authorized repository loop under the [DX12 repair contract](../design/dx12-workload-repair.md).
Added a shared test helper around the actual four B18, RT-4 and impostor workloads. Capture starts before device
creation and is inspected after context/resource teardown, including assertion/skip exceptions. Existing inputs,
authored assets, numerical bars and pixel/cull oracles are unchanged. Unavailable devices now produce actual SKIP
in the touched cases. No geometry/physics algorithms, renderer design gate, staging, commit or push was involved.
Advisor unavailable. Desktop automation was stopped with Escape; no further app input was issued. The synchronizer
completed its native CMake regeneration, and guarded command-line qualification continued.

Evidence root: ignored `build/research-dev-workflow-20260912/`. MSVC 19.51.36246, Windows Debug, two serialized build
workers, NVIDIA 4070 Ti SUPER 10de:2705 / 32.0.15.9579. Source snapshots under `before-consumer-validation/` preserve
the original affected tests. Wrappers preserve synchronizer readiness, source/model identities, bounded commands,
native exits, exact test inventories, JUnit and sealed manifests.

`dx12-full-consumers-011743`: five affected targets rebuilt; **6 selected/reported/executed, 6 failed, zero skipped
or disabled**. All existing numerical/pixel oracles passed on hardware, but every new silence assertion failed:

- B18: pipeline-library cold lookup warning 971 (one per shader, three in the scattering case).
- RT-4: two warning 1328 messages from creating scratch buffers in ignored initial UAV state.
- Impostor: four missing depth clear hints (821), four color clear hints/mismatches (820), three stage-linkage
  errors (660), one explicit failed PSO-creation HRESULT, and five descriptor-heap errors (554).

The descriptor finding reopened the earlier .c owner before continuing .d. Its focused reproducer, repair, hardware/
WARP qualification and six-file clean LLVM-20 run are recorded in the [descriptor session](2026-09-13-dx12-descriptor-allocation.md).
No warning/error was filtered or reclassified. Correct pixels alone did not qualify the native command stream.

## Implemented consumer repairs

The velocity vertex cook appends flat fade at location 4 after its smooth clip outputs at locations 5/6. Fragment
inputs already sort by location; HLSL VS/DS/MS output declarations previously used append order. The implementation
now sorts output declarations through one bounded helper without changing graph arithmetic or authored locations.
A native regression supplies noncontiguous, mixed-interpolation outputs in that same order and asserts exact pixels.
The four RT scratch allocations now start COMMON and explicitly transition to UAV before acceleration-structure build;
AS result buffers retain their required immutable acceleration-structure state.

The compute pipeline library now maintains a bounded index of successful stores, avoiding native probes for known
cold misses. A versioned little-endian envelope preserves that index with native data; malformed/duplicate/truncated/
oversized/checksum-invalid inputs preserve the live cache. Replacement and destruction respect the native library's
borrowed-blob lifetime. Repeated reload, a newly introduced shader after warming, malformed inputs and explicit reset
all execute real exact-output kernels. Existing B18 tolerances and RT-4 oracles are unchanged.

Resource creation now receives the first known clear from the actual CEIR attachments, using the same pure decoder
as execution. The load-time plan handles color/depth slots, shared depth, Load/DontCare and multiplicative MRT clears;
the recorder forwards optimization-only metadata to transient/persistent native images. No hint initializes pixels
or invalidates existing persistent history. An adjacent authoring gap was verified in source: fullscreen plans dropped
both the pass's `clear_color` parameter and the existing build descriptor's clear. Both links now carry the authored
value. A cooked-asset regression proves it reaches exact uncovered pixels, alongside an actual drawn triangle.
The existing persistent-history regression also changes only the creation hint and retains the old image's pixels.

The [implementation recipe](../recipes/2026-09-13-dx12-consumer-contracts.md) records parameters, native references,
assembly, boundaries and source ownership. It also records a separate source-only unchecked fixed compute-heap cursor
under RAH-6.b; the observed raster frame descriptor failures remain repaired under .c. No performance victory is claimed.

## Incremental qualification

| Evidence directory | Actual result |
|---|---|
| `dx12-full-consumers-013407` | Seven hardware cases: stage-interface and RT-4 passed; four B18 cases still emitted cache warnings; impostor retained nine clear-hint warnings and **zero native errors** |
| `dx12-full-consumers-013925` | Expanded cache test plus the preceding set: **7/8 passed**, only impostor clear warnings remained; D4 exercised 176 assertions with native silence |
| `warp-014200-3c6b66` | Independent WARP census plus **7/8 passed**; impostor's 18 original assertions passed and only its capture-silence assertion failed; all original D3DConfig settings/app registrations restored and verified |
| `dx12-full-consumers-015559` | Seven affected targets rebuilt; **9/9 selected/reported/executed/pass**, zero skips/disabled; actual impostor capture had zero warnings/errors through teardown |
| `dx12-full-consumers-015935` | Added cooked fullscreen clear-to-pixels regression: **10/10 selected/reported/executed/pass**, zero skips/disabled; transient and persistent cases each executed three frames; exact clear `0xcc996633`, center `0xff0000ff`, one submission per frame |
| `dx12-consumer-capture-tidy-020113` | LLVM-20 parsed all 18 changed headers/TUs; 17 clean, new frame-graph test had two isolate-declaration diagnostics. Declarations split; final analysis and affected-consumer/WARP qualification follow |
| `dx12-consumer-capture-tidy-020752` | Both final changed files parsed clean, including the declaration repair, persistent-hint history regression and cache ownership comment; all 18 changed C++ files now have clean incremental LLVM-20 evidence |
| `dx12-final-consumers-020951` | Inventory guard stopped before CTest: the broad CEIR prefix selected 108 cases, including additional Vulkan and render-graph consumers. Reviewed the full inventory and added their executable owners |
| `dx12-final-consumers-021131` | **108/108 reported/executed/pass**, zero skips/disabled; final command-owner audit found ten cases in the separate `crd-ceir-gpu-dx12-tests` executable had not been rebuilt. Those ten are not accepted as current-source qualification; its owner rebuild and focused rerun are required below. The other 98 cases' owners were rebuilt |
| `dx12-ceir-owner-021431` | Rebuilt the separate CEIR/DX12 owner; **10/10 selected/reported/executed/pass**, zero skips/disabled. Together with the 98 current-owner cases above, all 108 reviewed consumers have current-source qualification; do not sum overlapping runs |
| `warp-021633-743acb` | Independent software-device census passed, then **25/25 selected/reported/executed/pass**, zero skips/disabled. Includes validation negative/positive cases, states, descriptors, stage linkage, authored clears, cache, four B18 cases, RT-4 and actual impostor. Production captures silent; intentional negative diagnostics asserted. All original app/device settings restored and verified |

All successful numerical/native tests retain separate oracles. The clear-plan test has 34 assertions; the cooked
pixel test has 62; the actual impostor has 19. These are correctness observations, not benchmark measurements.
Later final qualification is recorded below rather than rewriting the failed intermediate evidence.

## Published CI

Human HEAD remains `7201a7b818ee6b363aac6cfbf97f94fa96d8fb85` in
[run 34714148010](https://github.com/yatiyr/CRD/actions/runs/34714148010). All six Linux lanes and both repository
checks passed. Shipping job 103608115021 completed with **six failures / 6,681 tests**: B18 hair/fur/scattering,
inner coverage, RT-4 and actual impostor. Atomic A-buffer passed. Its software adapter is the previously recorded
flagless 1414:008c / 10.0.26100.33296 tuple; the published revision predates the local classification repair.
The downloaded log is `ci-103608115021-7201.log`. Release job 103608114996 is failed, but its direct log endpoint
still returns 404 and CLI run-log fallback timed out after 100 seconds. Needs CI output; its cause is not inferred.
Other Windows/ASan/tidy findings retain their owning rows and previous dated evidence. Unpublished fixes still need
qualification after the user commits/pushes.

At the later refresh, no newer publication existed and run 34714148010 was complete. The Release job's structured
steps showed Configure succeeded but Build/Test had no conclusion and no CTest artifact. Its check annotation
explicitly reported **the hosted runner lost communication with GitHub**. Saved `release-job-7201.json` and
`release-annotations-7201.json` establish an infrastructure failure, not a known compiler/test failure. Retried only
that existing hosted job with `gh run rerun --job 103608114996`; command exit 0 is recorded in
`release-rerun-7201.txt`. The rerun still targets the human's existing revision, not unpublished local repairs.
The replacement Release job is **103635907447**, observed In progress at the next refresh.

## Local close and continuation

REPO.3c.5.d and its parent now retain only published-CI qualification, together with their earlier children. The local
hardware tuple is NVIDIA 10de:2705 / 32.0.15.9579; WARP is 1414:008c / 10.0.26100.8972, wave width 4, RT 1.1,
classified Software. Hosted 10.0.26100.33296 remains a distinct provider. All envelopes retain real exits, source/model
identities, inventories and bounded execution. No performance, extra platform or new renderer programme is claimed.

Documentation validation first found MEMORY 68 bytes over budget; compacted its routing prose while retaining every
link. Subsequent validation passed **863 rows, 1,023 documents, 8,782 local links, 210 D-007 and 16 v17 routes**.
Repository guard passed 96 module layouts; `git diff --check` passed. Orientation rules were inspected and retained;
MEMORY, system/recipe navigation, the sole owner rows and this evidence were updated. Current work advances to
REPO.3c.6 under the user's rule to retain CI waits while continuing available work. No staging, commit or push occurred.
