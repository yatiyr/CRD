# Large C++ research and repository loop planning

<!-- doc-role: evidence -->
> Dated evidence, 2026-09-12. Live work: [ROADMAP](../ROADMAP.md); rules: [AGENTS](../../AGENTS.md).

## Purpose and boundary

Prepare the complete repository/verification repair programme, research large C++ development practices, and
present material decisions before activating implementation. The request explicitly orders research and questions
first. No engine, workflow, synchronizer or compiler configuration implementation changed in this planning session.
Human-only commits/pushes and the single-roadmap rule remain absolute. No advisor capability was available;
no external review or delegated work is claimed.

Research: [large C++ development and CI](../research/2026-09-12-large-cpp-development-and-ci.md).
It compares primary material from Chromium, LLVM, Firefox, Qt, Blender, Unreal and Bazel and translates it into
Cerid-specific source findings, proposed acceptance tests and a finite unattended contract. Blender handbook
retrieval failed; its official CMake implementation supplied the relevant evidence instead. No benchmark ran.

## Source and CI evidence

Entry: `main`, clean, at published `9045eebb5c072b6025343c67343a57a57c79bc85` (`repository cleanups.`).
`git status`, source reads and GitHub read-only APIs establish this evidence. Main was already pushed by the human.
Current workflow [34694952926](https://github.com/yatiyr/CRD/actions/runs/34694952926) was still running at the sample;
its completed jobs establish the failures below without waiting for unrelated long jobs. No run was cancelled.

- [Windows repository job 103556726280](https://github.com/yatiyr/CRD/actions/runs/34694952926/job/103556726280):
  native profile fixture requests `Visual Studio 17 2022`; hosted image is `windows-2025-vs2026`, version
  `20260907.229.1`. CMake cannot find VS 2022. Seven portable profile tests pass before the native configure fails.
- [Linux Debug job 103556726277](https://github.com/yatiyr/CRD/actions/runs/34694952926/job/103556726277) and
  [Linux Shipping job 103556726103](https://github.com/yatiyr/CRD/actions/runs/34694952926/job/103556726103):
  pinned Vulkan validation download gets HTTP 403 in `urllib.request.urlopen`. All six Linux compiler job metadata
  entries show setup failure; these two full logs were sampled. No new engine-test result follows from setup failure.
- [Strict tidy job 103556726265](https://github.com/yatiyr/CRD/actions/runs/34694952926/job/103556726265):
  `work_build.cpp:152` simplify-subscript and `:157` avoid-nested-conditional diagnostics, warnings as errors.
  The same current source expressions were read locally. They also occur in the preceding run's tidy log.
- Linux repository tooling completed successfully. This is not proof of Linux engine/runtime correctness.

Preceding [Windows Debug job 103538355050](https://github.com/yatiyr/CRD/actions/runs/34688030261/job/103538355050),
revision `2a82134759c25e5f7af5e9aaf0f53c10d3721955`, reports seven failures among 6,760 CTests:

1. B18-a hair BCSDF on DX12 versus CPU oracle.
2. B18-b fur BCSDF on DX12 versus CPU oracle.
3. B18-c hair multiple-scattering tiers on DX12.
4. B17-c atomic A-buffer on DX12, including a segmentation exception reported after 115.42 seconds.
5. B1-f inner coverage on DX12.
6. RT-4 NEE/MIS area-light path tracing on DX12.
7. CEIR-18p impostor engagement on DX12.

The earlier job's controller census says Microsoft Hyper-V Video, driver `10.0.26100.1150`; it does not establish
each test's selected D3D12 adapter. Retain all seven cases and obtain actual adapter/feature/oracle evidence before
assigning a root cause. No failure is dismissed as runner variance. The existing tests contain software-adapter
precision branches and missing-device early returns that require explicit qualification accounting.

Read commands used `gh run view <id> --repo yatiyr/CRD --json ...` and
`gh api repos/yatiyr/CRD/actions/jobs/<job-id>/logs`. The runner inventory endpoint returned `total_count: 0`.
Full logs and metadata were saved under ignored `build/research-dev-workflow-20260912/`. Remote links and diagnostic
summaries above are durable; local logs may be removed with build outputs and contain no independent live status.

## Source findings and reuse

- 96 `engine/<family>/<module>/CMakeLists.txt` files; 20 visible configure presets; native eight-profile contract retained.
- Existing `project_sync/model.py` already queries CMake File API and target dependencies. An affected planner
  should reuse low-level reading/path checks, while correcting opaque-ID resolution and accounting for interface,
  generated and conditional dependencies. Structural synchronization filters are not a complete build graph.
- Existing GPU `RESOURCE_LOCK crd_gpu_device` and tooling/numerical timeouts must be reused. The finding is incomplete
  orchestration/coverage, not an absence of locks or all timeouts.
- `ci.yml` already has read-only contents permission. Proposed security work concerns mutable action references,
  checkout persistence, acquisition identity and trust separation; it does not claim a writable token was observed.
- Four ordinary configurations lack direct workflow preset invocation; native fixture overlap is partial evidence.
  `win-tidy-local` is diagnostic and `win-vs` is the native integration route. Preserve every preset and classify it.
- Root CMake writes an ImGui patch into downloaded source. Concurrent cache corruption is an unverified concern;
  reproducible/isolated patching needs its own proof. No package-export helper was found in the scoped build census.
- Existing soft CI performance budgets are not hard performance qualification. Future speed claims require measured
  boards, including cache regressions and resource costs, written to `docs/bench/` at measurement time.

## Planning changes and next action

Add REPO.DEV children and REPO.3c repair children to the existing master table, preserving every prior ID and renderer
prerequisite. Research is Recorded; the decision row is Review; fixes and remote qualification remain open.
Update context and research navigation. Operational rules/BUILDING changes are an explicit implementation child;
the new local policy is not falsely described as shipped by this session.

Initial decision review covers CI cadence, human publication while unattended, hardware resources and the finite
repository programme boundary. All answers are now recorded below and the recurring continuation has been activated in this task, preserving
the no-commit/no-push rule. Planning did not change engine/workflow implementation. The next entry is
[REPO.3c.1](../ROADMAP.md#slice-repo.3c.1). No renderer ADR approval is inferred.

Verification for these documentation changes: run the documentation/master-plan validator, repository hygiene
guard and scoped diff/whitespace checks. Results are recorded below after execution; no engine rebuild is implied.

## Verification and staged continuation

- `python -X utf8 scripts/check-master-plan.py`: PASS, 847 rows, 1,000 documents, 8,468 local links,
  210 retained D-007 source-ID routes and 16 retained v17 routes.
- `python -X utf8 scripts/check-repository.py`: PASS, root/ignore contracts and 96 module registrations.
- `git diff --check`: PASS. Existing roadmap rows were mechanically renumbered after adding 20 rows; stable IDs
  and all retained contracts remain. Only planning/reference/pointer files changed.
- A single thread heartbeat, `cerid-repository-hardening-loop`, was created PAUSED with a 15-minute continuation
  interval. The first creation request lacked the thread destination and failed validation; the corrected request
  returned the ID and PAUSED state. No implementation run was activated by that operation.
- Initial answers received: tiered CI; finish independent work while human-publication gates remain open and
  resume CI after the human pushes; existing hosted CI/workstation only, with missing hardware gates retained.
  The final answer confirmed stopping at repository closure before renderer review. All four initial decisions
  are explicit; REPO.DEV.1 is complete. The same heartbeat was then activated, with a finite goal, no commits/pushes,
  quiet publication monitoring and no new resources. No implementation result is attributed to the planning phase.
