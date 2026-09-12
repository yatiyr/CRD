# Developer selection foundation

<!-- doc-role: evidence -->
> Dated evidence, 2026-09-12. Only live work: [ROADMAP](../ROADMAP.md). Rules: [AGENTS](../../AGENTS.md).

## Scope and ownership

Continued the approved repository loop while exact published-revision GPU evidence remains open. REPO.DEV.3 is
split into traceable children: conservative planning, complete executing frontend, and integrated qualification.
The full research contract is preserved in [the design](../design/developer-workflow.md); this increment does not
claim that doctor/check/evidence or tiered scheduling have shipped. No advisor capability is available. No commits,
pushes, engine algorithm changes or renderer implementation were made in this increment.

## Implemented

`python scripts/dev.py plan --build <directory> [--config <configuration>]` is read-only planning. It includes local
net tracked/untracked changes, or a clean actual CI base/head pair. Explicit path scenarios are marked separately.
Plans carry changed-content/revision and codemodel hashes, ownership/reverse consumers, CTest/fixture/guard selection,
disabled counts, preserved properties and risk gates. Unknown/stale inputs broaden the requirement with a reason.
It never invokes an automatic local build or test sweep. The Windows/Linux repository job now runs its portable
regression suite; existing full CI gates remain unchanged.

The implementation resolves opaque CMake IDs and propagates compiler include-directory ownership for header-only
interfaces. Generated sources remain represented. It retains authored utility requirements in the graph but excludes
arbitrary utilities from automatic build-target lists. Explicit CMake-generated aggregates are excluded entirely:
the actual native graph contains six distinct `ALL_BUILD` IDs, which initially exposed an over-strict uniqueness
check and an unwanted aggregate reverse edge. Both mechanisms have discriminating regressions now.

The [recipe](../recipes/2026-09-12-affected-build-selection.md) teaches the selection and its limits. The frontend
reuses the existing synchronizer query writer in fixtures; it neither regenerates the real IDE model nor modifies
the synchronizer's journals, saved buffers or membership manifest.

## Verification

- Python suite: **17/17** on Windows Python 3.14.4 and WSL Ubuntu Python 3.12.3. Cases include opaque IDs, private
  sources, propagated interface includes, generated sources, configuration differences, stale/foreign/corrupt replies,
  rename/deletion/untracked handling, CI checkout identity, byte/deletion hashes, fixtures, disabled tests, utility
  safety and empty/guard-only rejection.
- Tiny real CMake 4.3.2 / native VS 2026 Debug fixture: build with two workers, then **1/1 selected CTest** passes.
  A header-only interface selects exactly its library and executable consumer, excluding an unrelated executable.
  The consumer includes a generated header. An initial sandboxed compiler-discovery attempt was unavailable;
  the identical fixture resolves the installed toolchain and passes in the normal host context.
- Same real fixture with WSL GCC/Ninja: **1/1 selected CTest** passes. These are tiny fixtures, not full Cerid builds.
- Actual Cerid VS Debug graph: `work_build.cpp` resolves to `crd-ceir-gpu`, with **30 graph targets / 29 buildable
  targets** in its reverse closure. The graph retains `cook-demo-assets` as a utility obligation, without selecting
  `ALL_BUILD`, cleanup or installation as build commands. This inspection is not a new runtime qualification of
  those 29 targets.
- Actual native CTest discovery contains `crd-test-helpers-tests_NOT_BUILT-b12d07c` without a command. The planner
  therefore reports incomplete discovery and broad CI requirements, not zero passing tests. REPO.DEV.3b/3c own
  scoped build/rediscovery and real-consumer qualification without forcing unrelated local compilation.
- A documentation scenario works with an unconfigured build, selects documentation/hygiene guards and no engine
  compilation. Machine-readable plans state explicitly that no checks executed.
- Final CLI JSON is also exercised inside each real fixture. Six existing repository-tool tests, repository hygiene,
  Actionlint and whitespace checks pass. The documentation validator passes **851 rows, 1,005 documents and 8,528
  local links**, including orientation size budgets. No existing full CI obligation was removed.

Raw disposable logs live under ignored `build/research-dev-workflow-20260912/`; this session is the tracked evidence.
Earlier repairs and the adapter probe are recorded in [the preceding session](2026-09-12-repository-ci-environment.md).
Full CI comparison/publication remains REPO.DEV.11/REPO.3d. The 17 portable cases have been added to the existing
repository CI job; remote confirmation requires the user's next publication.

## CI census update

At published revision `9045eebb5c072b6025343c67343a57a57c79bc85`, newly completed
[MSVC Debug](https://github.com/yatiyr/CRD/actions/runs/34694952926/job/103556726241) and
[MSVC SSE2](https://github.com/yatiyr/CRD/actions/runs/34694952926/job/103556726276) each report the same seven DX12
failures already owned by REPO.3c.5–7. Their inventories are 6,762 and 6,759 tests respectively. Neither lane adds
the clang-cl FFT stack failures. Debug's A-buffer exception occurs after 117.26 seconds; SSE2's after 119.74 seconds.
These are observed exceptions, not a claim of a timeout or a root cause. The selected-adapter probe and prior repairs
are still unpublished. The run was still active when inspected; no whole-run success is claimed.

## Next discriminating work

Complete REPO.DEV.3b's doctor/check/evidence commands, including approved scoped utility handling, synchronization
checks, toolchain/runtime setup, post-build CTest rediscovery, real exit codes and source-identity stability. Then
qualify real consumer execution under REPO.DEV.3c. Continue remaining independent repository children during the
human publication wait. Keep RAH-0/ADR-0107 renderer review gates intact.
