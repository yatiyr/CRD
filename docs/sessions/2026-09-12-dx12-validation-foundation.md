# DX12 validation foundation and published CI review

<!-- doc-role: historical -->
> Dated evidence. Live state: [ROADMAP](../ROADMAP.md); rules: [AGENTS](../../AGENTS.md).

## User direction and scope

The user explicitly requested: "check CI and carry on from REPO.3c.5.a" and required continued work. Implement its full
[validation contract](../design/dx12-workload-repair.md), preserve earlier uncommitted repairs and human-only publication.
No advisor capability was available; no review is claimed. No geometry/physics algorithm or new renderer feature was
implemented. After the requested foundation, newly actionable earlier CI defects take priority under the sequence rule.

## Implementation

The [recipe](../recipes/2026-09-12-dx12-validation-and-command-lifetimes.md) documents the complete API and native contract.
Added allocator-owned bounded concurrent validation, startup replay, explicit readiness/failure accounting and device
scope registration across every gpu-context-dx12 factory. Added shared checked Reset/Close/Signal/completion paths to
compute, raster, RT and work graphs. Invalid close never submits; removed-device sentinel never counts as completion;
finite waits verify actual progress, and failed post-submit retirement makes resource destruction safe. Raster frame
ring, upload, readback, present and resize paths propagate or latch failure instead of returning successful work.

Eight new native tests cover invalid capacity/allocation, intentional command error, late enablement, device loss,
timeout/early wakeup, concurrent callback capacity/truncation, observer/native-device lifetimes and real public compute
copy through context teardown. Actual copy value is 0xC3D007. The test's five context lifetimes are not a claim of actual
work-graph dispatch. Standalone kir-dx12 remains outside this instrument; RAH-6.b owns its integration boundary.

Source registration used the existing CMake collector/synchronizer. Visual Studio's native regeneration produced a
File Modification Detected modal; the observed Reload All action accepted regenerated projects. No source buffer was
edited through the UI. Guarded wrappers then observed sync readiness and stable source/model identities. Two workers,
one Windows Debug configuration, serialized heavy builds. No commit or push.

## Local qualification

Human HEAD: `7201a7b818ee6b363aac6cfbf97f94fa96d8fb85`; this implementation remains an unpublished working-tree change.
Windows/MSVC 19.51.36246, CMake 4.3.2, Python 3.14.4, LLVM 20.1.8. Available NVIDIA RTX 4070 Ti SUPER 10de:2705,
driver 32.0.15.9579; WARP 1414:008c, driver/D3D12Core 10.0.26100.8972. Evidence below lives under ignored
`build/research-dev-workflow-20260912/`, with commands/logs, source/model identity, JUnit counts and sealed records.

- `run-dx12-validation-consumers.py` → `dx12-validation-consumers-234543`: all five affected targets rebuilt
  (`crd-gpu-context-dx12-tests`, scene-render tests, census, work-graph tests, sandbox), build exit 0. **22 selected,
  reported, executed and passed**, zero failures/skips/disabled, CTest exit 0. Includes both actual impostor consumers,
  compute copy, graphics draw, frame submission/timing, authored clear/copy/blit/present, resize and RT-1/3/6.
  The existing impostor cases do not yet use the new capture; their output passes do not establish validation silence.
- `CERID_DIAGNOSTIC_BUILD=build/win-debug`, `run-warp-diagnostics.py '^DX12 validation'` →
  `warp-234706-3a21ce`: native census **1/1 pass**, validation **8/8 pass**, zero skips/failures, exit 0. Positive copy:
  82 information messages, zero warnings/errors/instrument failures. Original app list and all DirectX device settings
  restored and independently read back; valid before/after XML and durable recovery journal retained.
- Incremental `scripts/tidy-files.ps1` qualification: all eleven changed/new C++ files parsed clean across
  `dx12-validation-tidy-232608`, `233238`, `233924`; later invocations supersede only their earlier diagnosed files.
  SDK min/max and small macros, a bad void return, native informational filter interpretation and two tidy diagnostics
  were fixed before final passes. Earlier failing envelopes remain intact. No numerical tolerance or diagnostic filter
  was weakened. The final public-header wording change is documentation only.

These are correctness checks, not a determinism or performance benchmark. No new ASan/platform claim is made; published
sanitizer/compiler/hosted tuple qualification is still required. The new callback registers before startup replay,
and both final runs above include that ordering repair.

## Published CI observation

[Run 34714148010](https://github.com/yatiyr/CRD/actions/runs/34714148010) tests the human HEAD above, not this new code.
At 23:48 local time both repository jobs and five Linux lanes passed; Linux ASan, Windows Release/Shipping/ASan and
strict tidy were still running. Four completed Windows lanes fail: Debug job 103608115003, SSE2 103608114995,
clang Debug 103608115018, clang Shipping 103608114901. Raw logs: `ci-<job-id>-7201.log` in the evidence directory.

All four execute and pass the previously failing DX12 atomic A-buffer case. Each retains six failures: B18-a hair,
B18-b fur, B18-c scattering, inner coverage, RT-4 and the actual impostor consumer. Debug/clang Debug selected 6,768
tests, SSE2 6,765, clang Shipping 6,681. Whole-lane skip lists remain in the logs; those totals are not all-runtime claims.

The independent census reports BasicRender 1414:008c, driver 10.0.26100.33296, DXGI software flag 0, kernel query
success with flags 0x10a, SoftwareDevice 0 **and RenderSupported 0**. The published classifier nevertheless returns
Hardware. Thus negative SoftwareDevice is insufficient hardware execution evidence for this display-backed provider;
the prior repair is not qualified. REPO.3c.4.a must reopen with this exact observation. Microsoft documents both
[flagless primary BasicRender](https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/d3d10-graphics-programming-guide-dxgi)
and [BasicDisplay/BasicRender's WARP role](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/microsoft-basic-display-driver).
This is an adapter-identity defect separate from the new command validation and remaining renderer failures.

The remaining local classification repair must be completed before advancing further. REPO.3c.5.a needs CI output
after human publication; current CI cannot qualify its unpublished implementation. All pending gates retain owners.

## Classification follow-up and ordered continuation

REPO.3c.4.a now handles the documented exact BasicRender vendor/device pair from the selected device's successful
DXGI query. Other hardware classifications require positive kernel RenderSupported evidence. Counterfactual tests
include the actual 0x10a hosted observation, unavailable metadata, adjacent vendor/device IDs and contradictory flags.
The independent census prints the documented-provider match and refuses unsupported hardware classification. This
corrects the earlier kernel-only assumption; the [recipe](../recipes/2026-09-12-dx12-adapter-classification.md) is updated.
No adapter is selected differently and no workload oracle, precision limit or software-specific tolerance changed.

`run-dx12-classification-followup.py` → `dx12-classification-followup-235334`: five affected targets rebuilt, **18/18
selected/reported/executed/pass**, zero skip/fail/disabled, exit 0. Covers all eight validation cases, three identity
cases, the native census and existing B18/inner-coverage/RT-4 oracles. `run-dx12-classification-tidy.py` →
`dx12-classification-tidy-235455`: all six changed headers/TUs parsed clean by LLVM 20, exit 0.

`run-warp-diagnostics.py` with validation/classification/B18/inner-coverage/RT-4 selection → `warp-235614-50e022`:
**17/17 tests and 1/1 census pass**, zero skips/failures, exit 0; all original DirectX settings restored and verified.
This is the local 10.0.26100.8972 provider, not the hosted 10.0.26100.33296 provider. Synthetic flagless-identity tests
exercise the exact published evidence policy; they cannot qualify execution on that unavailable hosted revision.

At the subsequent CI refresh the five remaining compiler/runtime lanes and strict tidy still ran. REPO.3c.4.a/4 and
REPO.3c.5.a retain Needs CI with the real run link; all available local work for those gates is recorded above. The
loop advances to REPO.3c.5.b's resource-state/alias repair. This does not close their remote gates or authorize a commit.
The documentation validator passed 863 rows, 1,017 documents and 8,723 local links before this final evidence append.
