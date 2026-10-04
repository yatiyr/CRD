# User decisions 2026-10-05: closure policy and the open DIAG gates

<!-- doc-role: historical -->
> Dated evidence. Live owners: [DIAG.1b](../ROADMAP.md#slice-diag.1b), [DIAG.6c](../ROADMAP.md#slice-diag.6c),
> [DIAG.7a](../ROADMAP.md#slice-diag.7a), [DIAG.7b](../ROADMAP.md#slice-diag.7b),
> [DIAG.7c](../ROADMAP.md#slice-diag.7c), [REPO.DEV](../ROADMAP.md#slice-repo.dev). Rules: [AGENTS](../../AGENTS.md).

The user asked which decisions were open and for a recommendation on each. They then accepted every recommendation:
"go, accept all recommendations". The decisions below are therefore user decisions, and each owning row cites this
note.

## 1. Closure policy (all rows)

A row reaches Done only when every acceptance clause is proven, or the user has explicitly moved the clause into a
named, visible gate row. There are no silent waivers. A gate the user can clear, such as an elevated run, is cleared
rather than waived.

## 2. DIAG.1b: TSan

A hosted clang ThreadSanitizer lane on Linux runs in the nightly/complete tier only. It first needs the 40 clang++
`-Werror` sites counted by the REPO.DEV.11 audit fixed; that work also gives the tree a second compiler. Meanwhile one
recorded WSL2 TSan run may serve as interim evidence. It never replaces the lane.

## 3. DIAG.6c: external sampling

The user performs one elevated `python scripts/sample-cpu-wpr.py --stacks` run (Administrator) to prove the hotspot is
visible (clause 3) and to confirm external stacks (clause 2). Granting "Profile system performance" to the account is
optional. There is no hosted sampling lane: the runbook is covered by its script tests. The other flip blocker, the
clang-cl `crash_capture_specimen` fix, was committed in `660a0857` and waits for CI confirmation only.

## 4. DIAG.7a closure

- **DX12 program route:** DIAG.7b(g) is the error-severity proof. Its GPU-based-validation
  `DESCRIPTOR_UNINITIALIZED` error carries the pipeline's Program identity. The g-6 warning-severity correlation stays as
  supporting evidence.
- **Lifetime class:** built, not waived. A bounded Cerid-side table maps a destroyed object's native handle to its
  retired identity, so a use-after-destroy error correlates. It is proven by a use-after-destroy specimen on both APIs.
- **DX12 pass label:** emitted through the core `ID3D12GraphicsCommandList::BeginEvent`/`EndEvent` string marker,
  without PIX. This must be measured first. Pass correlation is proven through DRED breadcrumb context strings. The DX12
  debug layer's lack of command-list context in its messages is recorded as a measured platform limit, not as "blocked".

## 5. DIAG.7b(h) and DIAG.7c(h): a real device fault

- **(h1)** A real page-fault device removal on WARP, in a child process. This is software-provider evidence, labelled as
  such, CI-capable and never a desktop risk. Whether WARP reports DRED page-fault data is measured first.
- **(h2)** One opt-in hardware run: a page fault, not a hang, in an isolated child process, behind an explicit opt-in.
  The user starts it on an idle machine; it is never in CI or the default suite. The result is recorded as evidence for
  this GPU only.

## 6. REPO.DEV: a software Vulkan device on the hosted Linux lanes

Install Mesa's lavapipe and the Khronos validation layers, pinned, on the hosted Linux lanes. The roughly 380 Vulkan
device tests then run on every push instead of skipping. Any test that cannot hold on a software device states why,
explicitly.

## Order of work

1. Lavapipe in CI (6).
2. The lifetime table and the DX12 core pass label (4).
3. The TSan lane (2).
4. WARP real-fault evidence (5, h1).

DIAG.7c(d) (`VK_EXT_device_fault`) proceeds alongside, independent of these. The elevated WPR run (3) and the opt-in
hardware fault run (5, h2) belong to the user.

## Executed (2026-10-05)

- **(6) Lavapipe in CI.** Both hosted Linux jobs (`linux-gcc`, `linux-gcc-shipping`) install `mesa-vulkan-drivers`
  (lavapipe), and its version joins the `dpkg-query` record in every lane's evidence. Under the
  [pinned-inputs](../design/pinned-inputs.md) contract, apt packages of the pinned Ubuntu 24.04 image are "unpinned:
  recorded", not version-locked. So "pinned" for this decision means:
  - the package is named in `cmake/pins.json` → `unpinned.apt`;
  - its version is recorded in every lane;
  - the regenerated [license manifest](../generated/dependency-licenses.md) lists it;
  - the validation layer stays the SHA-pinned download it already was (`install-vulkan-validation.py`).

  The first hosted run is the qualification: tests that skipped for want of a device now execute, and any that cannot
  hold on a software device are fixed or made to say why.
