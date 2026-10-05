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

- **(4) DIAG.7a lifetime class: proven on both APIs, with a measured correction to the premise.** The 7a caveat
  assumed a destroyed object loses its debug name, so a dead-handle error could not correlate. Measured on VVL 1.4.341,
  that is false. The layer keeps the names of destroyed handles: a use-after-destroy error lists the destroyed
  `VkBuffer` with its full Cerid name. The one nameless record seen was the parameter-validation message for passing a
  stale handle as a call argument, which carries the handle only as hex in its text. That form is not safe to exercise
  in-process: the probe of `vkCmdFillBuffer` on a destroyed buffer segfaulted the test process. So it is not the
  specimen, and the Cerid-side handle table is not built, because no proven route needs it.
  - **Vulkan specimen:** record a fill on a Cerid-named buffer into a secondary command buffer, destroy the
    `VkBuffer`, then reference the secondary from a primary with `vkCmdExecuteCommands`. Nothing is submitted. An
    earlier version submitted the invalidated buffer and segfaulted lavapipe, which dereferences the destroyed buffer
    object (see the 7c census).
    - The record-time invalid-command-buffer error correlates to the buffer's retired identity.
    - The control without the destroy is clean.
    - Teeth: the identity minted but not used as the debug name fails the correlation.
  - **DX12 specimen:** a Cerid-named placed buffer, in a heap the test owns, is the destination of a copy held behind a
    queue-side wait on a CPU-signalled gate fence. It is final-released while provably in flight, then the device is
    removed (the engine's failure response), so the held copy is discarded and never executed.
    - Error 921 `OBJECT_DELETED_WHILE_STILL_IN_USE` correlates to the retired identity.
    - The control, released after completion, is clean.
    - Same teeth.
  - **Results:** each passed three runs. The full DX12 suite passes (201 cases), as does Vulkan `[validation]` on the RTX and on lavapipe.
- **(4) DX12 pass label: emission landed, proof gated on (h1).** `detail::Dx12PassEventScope` (in
  `dx12_identity_naming`) brackets every frame-graph pass in `Dx12FrameGraph::execute` with a balanced core
  `BeginEvent`/`EndEvent`, without PIX. The marker uses metadata 0, the legacy unicode encoding, and carries
  `format_debug_name(pass identity)`, the same token as Vulkan's `PassLabelScope`.
  - **Measured: no local oracle.** With the debug layer on and no capture tool attached, recording an event produces no
    record: no `BEGIN_EVENT` (1014), and even a deliberately unbalanced `BeginEvent` raises no
    `BEGIN_END_EVENT_MISMATCH` (955) at `Close`. The only in-engine observer is a DRED breadcrumb context, which is
    populated only on a real fault (a forced `RemoveDevice` leaves the list empty, DIAG.7b(e)).
  - **Gated:** the pass-to-fault correlation proof is therefore gated on the WARP real-fault work (5, h1). It is not
    claimed here.
  - **Checks:** the wiring keeps the whole DX12 suite green (201 cases, twice) and `[frame-graph]` (34 cases). The
    strict gate is clean on the three files.
