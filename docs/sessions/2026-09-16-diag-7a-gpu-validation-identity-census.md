<!-- doc-role: historical -->
# DIAG.7a census — common GPU validation lifecycle and stable object identity

Row 080 (DIAG.7a). This is the **(a) census** tick: no engine code. It extracts the acceptance clause verbatim,
surveys what already exists across BOTH backends, records the choice points, and enumerates the sub-units with each
one's gate lane — the same shape that made 6c auditable. Row 080 stays **Open** (DIAG.0-gated). Unlike 6c (greenfield
in perf), 7a sits on a real, live two-backend GPU layer that already has validation capture; the census's job is to
map that seam so 7a is unification + identity + tests, not a rewrite.

## Acceptance clause (verbatim, `design/runtime-diagnostics.md#diag-7a` + ADR-0133 DG11/DG12)

> Extend existing provider validation capture through startup, recording, submit, asynchronous completion, present,
> reload and destruction. Attach stable Cerid resource/program/pass IDs and generation to native object names and
> reports; preserve live/recently retired descriptor/range provenance. Define actual activation and unsupported
> reasons for core, synchronization and GPU-assisted modes without importing vendor types into public modules.
>
> Acceptance: intentional lifetime/state/descriptor hazard on a small isolated workload produces a correlated error
> on each claimed route; ordinary valid consumers stay clean. Dropped validation messages are not a clean run.
> RAH-6.b later extends this same service to its new recording contracts; it cannot create a parallel validation
> collector.

DG11/DG12 (ADR-0133): the diagnostics service is one collector; DIAG.7b/7c **extend** the existing
[DX12](../../engine/gpu/gpu-context-dx12/src/dx12_validation_capture.cpp) and
[Vulkan](../../engine/gpu/gpu-context-vulkan/src/vulkan_validation_capture.cpp) validation capture — they may not
create a parallel one. 7a is therefore the **common contract** those two extend, plus the identity attachment.

Downstream (context, not this row): **7b** = DX12 debug layer / GPU-based validation / DRED / device-removal; **7c** =
Vulkan validation + synchronization + GPU-assisted checks + device-fault extensions. Both build on 7a's common vocab.

## What already exists (survey — this row is TWO-backend)

| Area | DX12 | Vulkan | Note |
|---|---|---|---|
| Validation capture class | `dx12_validation_capture.{hpp,cpp}` — `ID3D12InfoQueue1` callback, `EnableDebugLayer`, `D3D12_MESSAGE` | `vulkan_validation_capture.{hpp,cpp}` — `crd::gpu::ValidationCapture`, a `VK_EXT_debug_utils` messenger on the `VkInstance` | present BOTH; ported onto the one graphics layer (RET-4/ADR-0105) |
| Public-header vendor cleanliness | `enum Dx12ValidationSeverity`, `Dx12ValidationIssue`, POD report — no `D3D12_*` in the header | `enum class ValidationSeverity`, crd:: containers — no `Vk*` in the header | GOAL "no vendor types in public modules" already largely met per backend |
| Severity vocabulary | `Dx12ValidationSeverity{Info,Warning,Error}` | `ValidationSeverity{Info,Warning,Error}` | **parallel, NOT unified** — different enum names for the same three levels |
| Dropped-message accounting | `report.dropped` counter; `clean` predicate requires `dropped == 0` (+ `contexts_started==finished`) | `records_dropped` counter | "dropped ≠ clean run" HONORED on DX12's predicate; Vulkan counts drops (census-flag: confirm it in a clean-run predicate) |
| Instrumentation-failure signal | `Dx12ValidationIssue::StartupOverflow` (a first-class failure, not silent) | records_dropped increment | DX12 richer; unify the concept |
| Object naming | (census-flag: check `SetName`/`WKPDID_D3DDebugObjectName` coverage) | `vkSetDebugUtilsObjectNameEXT` — 8 call sites, **SITE+SIZE labels** (e.g. "shadowmap 2048x2048x1") | human labels, NOT stable Cerid **ID + generation** |
| Pass identity | `frame_graph.hpp`: `add_pass(const char* name, …)`, `pass_name(u32)` | same (backend-agnostic frame graph) | passes have STRING names; no stable `PassId`+generation |
| Resource/program identity | resources named by creation site; no `ResourceId`/`ProgramId`+generation surfaced to native names | same | the 5d `symbol_index` "identity feeds matching" is the precedent to mirror |
| Activation / unsupported reasons | `validation_layer_spec_version()`==0 means layer ABSENT (Vulkan); debug-interface query gates DX12 | as noted | per-mode (core/sync/GPU-assisted) activation+reason NOT yet a unified surface |

## Choice points (record now, decide when the sub-unit lands — not this tick)

- **Unified vocabulary vs per-backend.** 7a wants ONE validation contract; today `Dx12ValidationSeverity` and
  `ValidationSeverity` are separate. Option A: a common `crd::gpu` severity/issue vocabulary both backends map onto
  (no vendor types); Option B: leave them separate and unify only the *report* shape. The ADR's "one collector, no
  parallel" pushes toward A. Do not decide here.
- **Identity stability scope.** "Stable … IDs and generation" — stable across *what*? The clause pairs ID with
  **generation**, which implies stable across **reload/recreation** (a resource keeps its Cerid ID; generation bumps
  on recreate — mirroring the DIAG.3e SlotMap/handle model and 5d identity). Confirm against DG12; record as the
  working definition, not a resolved fact.
- **Where identity is attached.** Native object name (debug-utils / SetName) is one channel; the validation *report*
  is another. The clause says "to native object names AND reports" — both. The name string must encode the Cerid
  ID+generation parseably (not just a human site label).
- **Vendor-type boundary.** Keep `Vk*`/`D3D12_*` out of public modules (already the pattern) — the common contract
  lives in `gpu-context` (backend-agnostic), backends translate.
- **Lanes.** This box has a real GPU with BOTH DX12 and Vulkan device gates that RUN (not skip) — so 7a's acceptance
  (intentional hazard → correlated error per route; valid consumers clean) is a **device test observed here**, not
  env-gated. Delete `imgui.ini` at repo root after device suites. GPU-assisted/DRED specifics belong to 7b/7c.

## Sub-unit enumeration (each with its gate lane; order is a plan, re-confirm per tick)

- **(a) census** — this doc. (done)
- **(b) common validation vocabulary** — a backend-agnostic `crd::gpu` severity/issue/report contract both captures
  map onto (no vendor types); DX12 + Vulkan translate their native severities into it; the "dropped ≠ clean" rule and
  the instrumentation-overflow signal become common. Lane: win-debug build both backends; device test both APIs.
- **(c) stable Cerid identity type** — a `ResourceId`/`PassId`/`ProgramId` + generation value type (mirroring the 3e
  handle/generation and 5d identity), with a parseable name-encoding helper. Lane: win-debug + unit test (no device).
- **(d) attach identity to native names** — thread the Cerid ID+generation through `vkSetDebugUtilsObjectNameEXT` /
  DX12 `SetName` and into the validation report, replacing bare site labels. Lane: device test both APIs (assert a
  validation message carries the Cerid ID).
- **(e) lifecycle coverage** — validation capture spanning startup/recording/submit/async-completion/present/reload/
  destruction (the clause's list); assert no phase drops messages silently. Lane: device test both APIs.
- **(f) per-mode activation + unsupported reasons** — a unified surface reporting core / synchronization /
  GPU-assisted activation state + the reason when unsupported (no vendor types). Lane: observed here (report what this
  box's layers actually offer) + documented.
- **(g) acceptance specimens** — an intentional lifetime/state/descriptor-hazard workload that trips a **correlated**
  error on each claimed route, and a valid-consumer control that stays clean; a dropped-message case proving "dropped
  ≠ clean". Lane: device test both APIs (real GPU here). Mirror the DIAG.0 specimen-harness discipline.

## This tick's gates

Census only — no code, no build. `check-master-plan.py` + `check-repository.py` must PASS (new doc + row-080 note).
Row 080 stays **Open** (DIAG.0-gated; a slice flips only when FULLY done). Prior row 079 (DIAG.6c) is (a)–(j) complete
and Open only on two user-gated blockers — no agent-doable 6c work remains, which is why the loop advanced here.

## (b) landed — common validation vocabulary

The two backends had parallel-but-separate validation types; (b) gives them ONE contract to project onto, and fixes a
real acceptance gap on Vulkan. ADR-0133's "one collector, no parallel" — 7b/7c extend this, they do not fork it.

- **Common header** `engine/gpu/gpu-context/include/crd/gpu/validation.hpp` (backend-agnostic, no vendor types):
  `enum class ValidationSeverity {Info,Warning,Error}` (now the CANONICAL home for the name — the Vulkan capture used
  to define it locally), a `to_string`, and `struct ValidationReport {info,warning,error,dropped,truncated,
  instrumentation_failures; bool clean()}`. **`clean()` encodes the acceptance rule "dropped validation messages are
  not a clean run"**: clean requires zero error/warning AND nothing lost (dropped/truncated) AND no instrumentation
  failure.
- **Vulkan** (`vulkan_validation_capture.{hpp,cpp}`): drops its local `ValidationSeverity` and includes the common
  one (one name, one definition — no ODR clash; all existing consumers, incl. the vulkan test suite, keep the same
  `crd::gpu::ValidationSeverity`). **Fix (a real acceptance gap):** `records_dropped` was tracked but had NO accessor
  and was in NO clean-run predicate. Every Vulkan clean-run gate asserts only `error_count()==0 && warning_count()==0`
  (grep: ~40 sites across `test_vulkan_context.cpp` / `test_vulkan_frame_graph.cpp`; NONE also bounds dropped or
  `messages().size()`), so a run that dropped messages — possibly errors — **would read as clean**, contradicting the
  acceptance rule. (Not a claim that a real drop-with-hidden-error was observed; the structural fact is that the gates
  cannot see drops.) Added `dropped_count()` and `report()` (projects counters + records_dropped onto the common
  `ValidationReport`), so `report().clean()` now honours dropped-≠-clean for Vulkan too.
- **DX12** (`dx12_validation_capture.hpp`): keeps its rich native types (`Dx12ValidationReport` carries lifecycle that
  7b needs) and gains inline `to_common(Dx12ValidationSeverity)` + `to_common(const Dx12ValidationReport&)` →
  `ValidationReport` — purely additive, no consumer breakage. DX12's own `complete_and_silent()` already treated
  dropped as non-clean; now that rule is shared vocabulary. **The mapper folds instrument-failure lifecycle into the
  common `instrumentation_failures`**: a capture that never reached `Ready`, or that saw `device_creation_failures` /
  `execution_failures`, maps to a NON-clean common report even when every message counter is zero — otherwise a
  registered-default-empty DX12 report ("the instrument never started") would read as provably-clean through the
  common contract, the exact scar (b) exists to prevent. (Message-count lifecycle — `contexts_started ==
  contexts_finished` — stays DX12-native for now; sub-unit (e) folds it.)
- **Oracle** `tests/gpu/gpu-context/test_diag_validation_vocab.cpp` (`[gpu][diag][validation][vocab]`, device-free, 10
  assertions): `clean()` is true only all-zero, and false for each of dropped / truncated / instrumentation_failures /
  error / warning individually (info alone stays clean); `to_string` stable. Teeth (observed win-debug): removing the
  `dropped == 0` term from `clean()` fails the dropped case (line 26, 1/2 cases) → restored + rebuilt → 10/10.
  DX12 projection tested device-free in `tests/gpu/gpu-context-dx12/test_dx12_validation.cpp` (`[dx12][validation]
  [vocab]`, 13 assertions): a `Ready` native report with dropped/truncated maps to a NON-clean common report; a
  `Ready`, message-silent report maps to clean; a **not-`Ready`** report and one with device-creation/execution
  failures each fold to `instrumentation_failures > 0` and NON-clean (the fold above); severity translation correct.
  Teeth (observed win-debug): dropping the readiness/device/execution fold from the mapper makes the not-`Ready` case
  read as clean → test fails at that line → restored, header touched (mtime scar), rebuilt → 13/13.
- **Lanes.** All three GPU backend libs (`crd-gpu-context`, `-vulkan`, `-dx12`) build on win-debug + win-shipping
  (`/O2 /WX`) + win-clang-cl-shipping (`/W4 /WX -flto=thin`) with the unified headers. `crd-gpu-context-tests`
  `[vocab]` 10/10 on all three lanes. `crd-gpu-context-dx12-tests` `[vocab]` 9/9 run on win-debug; the touched TU
  compiles clean under win-shipping + clang-cl.

**Scope honesty.** (b) unifies the vocabulary and fixes the Vulkan dropped-exposure gap, tested at the contract level
device-free. LIVE per-backend dropped injection (submit past capacity on a real device → `report().dropped > 0`,
`clean()==false`) belongs to the acceptance specimens in sub-unit **(g)** and is a device test on this box's real GPU;
it is not smuggled into (b). Vulkan `report()`'s runtime read of `records_dropped` is trivial + compile-verified here.
For (g): `vkSubmitDebugUtilsMessageEXT` IS declared in this box's Vulkan SDK `vulkan_core.h` (used nowhere in-tree
yet), so a synthetic-message injection path for the Vulkan drop specimen is feasible via that entry point — record it
so (g) does not re-discover the question.

**Gates.** `[vocab]` 10/10 (gpu-context, 3 lanes) + 13/13 (dx12, win-debug; TU also links on win-shipping /O2 /WX +
win-clang-cl-shipping); teeth observed + restored on both the common `dropped` term and the DX12 instrument-failure
fold; libs/exes build on 3 lanes; container check PASS; both validators PASS; no stray root files; no imgui.ini. 7a
stays **Open** (sub-units (c)–(g) remain).

## (c) landed — stable Cerid object identity value type

The clause requires "stable Cerid resource/program/pass IDs and generation" attached to native object names and
reports (design §DIAG.7a). (c) defines ONLY the value type + its name-encoding contract; WHO allocates identities and
bumps generations, and the threading of the encoded name into `vkSetDebugUtilsObjectNameEXT` / `SetName` and the
report, is (d). **Choice point resolved:** the design §DIAG.7a clause + ADR-0133 schema clause 7 ("correlate …
generation and source identity") and the record-schema paragraph (design L58-65: "IDs must survive export without
exposing raw pointers as identity"; "IDs cannot silently alias when a … GPU object is reused; preserve deleted
generation metadata") settle the census's open question as **index + generation, alias-proof on reuse, stable across
reload/recreation (generation bumps)**, exactly the SlotMap/handle model. **Provenance correction to the (a) census
header/L22:** the DG tags are defined in the research doc (`research/2026-09-14-diagnostics-and-instrumentation.md`,
not the ADR); DG11 ("disconnected GPU traces") is owned by DIAG.6b/7a, but **DG12 ("GPU fault depth" — DRED /
device-fault / GPU-assisted wiring) is owned by DIAG.7b/7c**, not 7a. So 7a's identity requirement is NOT a DG12
driver — it rides the §7a clause + schema clause 7 above; the (a) section's loose "DG11/DG12 (ADR-0133)" attribution
is superseded by this note.

- **Value type** `engine/gpu/gpu-context/include/crd/gpu/object_identity.hpp` (header-only, no vendor types):
  `enum class ObjectKind {Resource,Program,Pass}` + `to_string` (res/prog/pass), and `struct ObjectIdentity {kind;
  u32 index; u32 generation; valid(); operator==}`. The (index, generation) layout **mirrors
  `crd::containers::SlotMap::Handle` verbatim** (do not invent a second handle shape): `index==0xFFFFFFFF` is the null
  slot, `generation==0` is reserved so a default identity is invalid, live generations start at 1 and bump on
  recreation. `kind` lives IN the value because (d)'s report stores mixed-kind identities and the name string must
  carry the kind anyway.
- **Encoding** `encode(id, char* out, cap) -> chars written` and `parse(std::string_view, ObjectIdentity&) -> bool`.
  Format is fixed-width ASCII lowercase hex `crd:<kind>:<8 hex index>:g<8 hex generation>`
  (e.g. `crd:res:0000002a:g00000007`), hand-formatted (never snprintf → locale-independent, tr-TR-safe), so (d) can
  widen it char-by-char to `LPCWSTR` for DX12 and embed it in the debug layers' message prose. `kObjectIdentityMaxChars
  =27` / `kObjectIdentityBufferSize=28`. **encode REFUSES an invalid identity** (returns 0, writes nothing) — a name
  that round-trips must denote a real object, never a distinguished "invalid" string. **parse scans for the token
  INSIDE a longer string** (a debug-layer message quotes the name in prose), keeps scanning past a malformed `crd:`,
  and accepts only a token whose parsed identity is `valid()`.
- **Oracle** `tests/gpu/gpu-context/test_diag_object_identity.cpp` (`[gpu][diag][identity]`, device-free, 86 assertions
  / 8 cases): validity contract matches SlotMap (default/gen-0/null-index invalid, index 0 valid); encode/parse
  round-trip across every kind × index∈{0,1,0x2a,0xFFFFFFFE} × generation∈{1,2,7,0xFFFFFFFF}; exact fixed-width hex
  output; encode refuses the invalid identity (canary unwritten); parse finds a token embedded with prefix+suffix
  prose and past an earlier malformed `crd:`; parse rejects wrong prefix / unknown kind / non-hex / missing `:g` /
  truncated index or generation / a well-formed-but-generation-0 token; encode into a too-small buffer returns 0 and
  leaves a one-past-the-end canary intact, exact-fit succeeds. Teeth (observed win-debug): forcing `parse` to discard
  the generation (fix it to 1) fails the round-trip + embedded-parse + reject-gen-0 cases (3/8, exit 42) → restored,
  header touched (mtime scar), rebuilt → 86/86.
- **Lanes.** New TU registered in `tests/gpu/gpu-context/CMakeLists.txt` (explicit list) + auto-reconfigure; the exe
  builds on win-debug + win-shipping (`/O2 /WX`) + win-clang-cl-shipping (`/W4 /WX -flto=thin`); `[identity]` 86/86 on
  win-debug AND win-shipping /O2 (the hand-rolled hex is UB-clean under the optimizer). No device (identity is a pure
  value type). Container check PASS; both validators PASS.

**Scope honesty.** (c) is the value type + name contract ONLY. Nothing is threaded into `vkSetDebugUtilsObjectNameEXT`
or `ID3D12Object::SetName`, and the validation report struct is untouched — identity is attached to native names and
reports in **(d)**, and who owns the SlotMap that mints these identities (resource/program/pass registries) is (d)'s
plumbing. Row 080 stays **Open** (DIAG.0-gated; sub-units (d)–(g) remain).

## (d) split into (d1) + (d2), and (d1) landed — identity on the per-message record

The planned (d) "attach identity to native names AND reports" bundled three decisions (parse-into-report, mint identities,
name native objects) across ~8 Vulkan naming sites + a DX12 `SetName` inventory + two device suites — not ONE unit, and
half is unprovable this tick (there is no `vkGetDebugUtilsObjectName`; Vulkan *naming* can only be proven by a real
message that names the object, which is (g)-shaped). So (d) is split:

- **(d1) — identity on the per-message validation record**, parsed at capture on both backends, proven by synthetic
  message injection on the real GPU. Additive, bounded, provable today. (this tick)
- **(d2) — mint + name**: a resource/program/pass identity registry (see census flags below), the encoded token
  *prefixed* onto the existing debug-utils/`SetName` labels, and DX12 read-back via
  `GetPrivateData(WKPDID_D3DDebugObjectNameW)`. (next tick)

**(d2) inventory captured this tick (grep, not acted on):** DX12 has **zero** `SetName`/`WKPDID` calls in
`engine/gpu/gpu-context-dx12/src/` today — so (d2) *adds* object naming, it does not modify existing calls (resolves the
(a) census "check SetName coverage" flag: coverage is none). And there is **no `SlotMap<` anywhere in `engine/gpu/`** —
so no resource/program/pass registry exists yet; (d2) must introduce the minting scheme (the identity value type from
(c) is ready for it).

- **Per-message field.** Both message records gain `ObjectIdentity identity{}` (DX12 `Dx12ValidationMessage`, Vulkan
  `ValidationMessage`) — NOT the aggregate `ValidationReport`, which is counters; identity is per message. Neither
  record has a pinned on-disk layout (verified: no `static_assert`/`offsetof`/`memcpy` gates them), so the field is a
  safe append.
- **DX12 callback** (`receive_message`): parses the token from the **full** `pDescription` *before* the 1024-byte
  `text[]` truncation, so a token past the cut still resolves. The records buffer is raw (`try_allocate`, not
  constructed), so identity is explicitly set to the default-invalid before the parse attempt. `parse` is pure +
  noexcept + non-allocating — it honours the capture's "callbacks neither allocate nor call D3D" contract.
- **Vulkan callback** (`capture_callback`): iterates `pCallbackData->pObjects[0..objectCount)` (both the array and each
  `pObjectName` are nullable), first `valid()` parse wins — the structured route, since each named object carries its
  debug-utils name; falls back to `parse(pMessage)` if no named object yields a token. First-valid-wins; multi-object
  correlation is (e)/(g).
- **Device tests (real GPU here, both backends).** DX12 (`[dx12][validation][identity]`, appended to
  `test_dx12_validation.cpp`, 14 assertions): a Ready capture + real device → `ID3D12InfoQueue::AddApplicationMessage`
  with a token embedded in prose → the record carries `ObjectIdentity{Resource,0x2a,7}`; a tokenless message → a
  default-invalid identity; the warning-count delta is exactly 2 (the synthetic pair, no layer noise). Vulkan (new
  `test_diag_validation_identity.cpp`, `[gpu-context][vulkan][gpu][validation][identity]`, 16 assertions): drives
  `vkSubmitDebugUtilsMessageEXT` (fetched via `vkGetInstanceProcAddr`, `REQUIRE`d non-null — never skip-as-pass;
  `validation_layer_spec_version()` `REQUIRE`d non-zero) through the SAME callback the live layer uses, with the
  severity/type inside the messenger's masks and a REAL `VK_OBJECT_TYPE_INSTANCE` handle so the layer adds no VUID
  noise — three cases: token in the object name → parsed from the object; token only in the message prose → prose
  fallback; neither → invalid; each submit's `warning_count()` delta is exactly 1.
- **Teeth (combined, observed win-debug on the real GPU).** Short-circuiting the `parse` call in BOTH callbacks
  (`false && parse(...)`, keeps the symbol referenced) makes the identity cases fail on both backends (DX12 2/14 at the
  valid+equality lines; Vulkan 4/16 at the named-object and prose-fallback cases; the tokenless case correctly still
  passes) → restored, both .cpp + both headers touched (mtime scar), rebuilt → DX12 14/14, Vulkan 16/16.
- **Lanes.** Both backend libs + both device-test exes (`crd-gpu-context-dx12-tests`, `crd-gpu-context-vulkan-tests`)
  build on win-debug + win-shipping (`/O2 /WX`) + win-clang-cl-shipping (`/W4 /WX -flto=thin`). `[identity]` runs on
  the real GPU: DX12 14/14 + Vulkan 16/16 on win-debug AND win-shipping /O2. New Vulkan TU registered in the explicit
  CMakeLists list (+ auto-reconfigure). Container check PASS; both validators PASS; no stray root files; no imgui.ini
  was produced (these gates create no window). Note: the DX12 parse runs on BOTH message-delivery routes — the live
  `RegisterMessageCallback` target and the `replay_startup` `GetMessage` path both funnel through `receive_message`,
  so a token in a startup-replayed message resolves identically (each hands a NUL-terminated `LPCSTR`).

**Scope honesty.** (d1) attaches identity to the per-message validation *record* and proves capture-time resolution by
synthetic injection through the real callback. It does NOT name native objects (no `SetName` /
`vkSetDebugUtilsObjectNameEXT` call is added — that, plus the identity registry that mints the IDs and the
`ValidationReport`-level rollup, is (d2)), and it does not provoke a real validation error carrying a Cerid-named
object (that correlated-hazard proof is the (g) acceptance specimen). The synthetic message exercises the exact parse
path a live layer message would hit. Row 080 stays **Open** (DIAG.0-gated; sub-units (d2),(e),(f),(g) remain).

**Working-tree note (not this tick).** The whole DIAG programme is one uncommitted batch under the DIAG.0 gate, so
`git status` carries many untracked/modified files from earlier rows. Five modified tracked test files were observed
this tick and are NOT 7a work — `tests/foundation/core/test_diag_crash_capture.cpp` (+520) / `test_diag_crash_contract.cpp`,
`tests/foundation/jobs/test_diag_hang_dump.cpp` / `test_diag_hang_watchdog.cpp` / `test_diag_rt_sentinel.cpp` (a
`ProgressSample` field addition + crash-capture cases): DIAG.3c/4c/5a/5b collateral already in the batch. This tick's
proposed commit is scoped to the (d1) files only and does not cover them; carried, not shipped here.

## (d2) split into (d2a) + (d2b), and (d2a) landed — identity registry + debug-name builder

"Mint + name" is a registry design + N creation-site wirings per backend + DX12 read-back + a Vulkan prefix — more
than one provable unit, and the wiring depends on greps. Split:

- **(d2a) — the `IdentityRegistry` (minting) + `format_debug_name` (the bracketed native-name builder), device-free.**
  Pure, bounded, fully provable today. (this tick)
- **(d2b) — wire `mint()` at the backend creation sites, DX12 `SetName` + `GetPrivateData(WKPDID_D3DDebugObjectNameW)`
  read-back proof, Vulkan `vkSetDebugUtilsObjectNameEXT` label prefix.** (next; may itself split d2b-dx12 / d2b-vk)

### (d2b) inventory (grep, captured this tick — not acted on)

- **Vulkan naming path (verified, not inferred):** naming goes through ONE helper `name_image(VkDevice, VkImage,
  site, w, h, layers)` (`vulkan_raster_context.cpp:264`, wraps the `vkGetDeviceProcAddr` fetch + the SITE+SIZE label),
  called at **5 sites** (vk-texture / vk-bundle / fg-transient / fg-persist / fg-companion-depth) — **VkImage only**.
  So (d2b)'s Vulkan half is prefixing that single helper (covers all 5 image sites at once); buffers and other object
  types are NOT named today (the (a) census's "8 sites" was wrong — it is 5, images only). Non-image naming is new
  (d2b) surface, not a prefix.
- **Creation sites (where mint() attaches):** DX12 — `dx12_compute_context.cpp` 1, `dx12_context.cpp` 4,
  `dx12_raster_context.cpp` 10. Vulkan — `vulkan_compute_context.cpp` 1, `vulkan_context.cpp` 14,
  `vulkan_raster_context.cpp` 11. (grep -c of create_buffer/texture/program/storage_buffer/color_target/raster_program.)
- **No existing `debug_name`/`set_name`/`label` field** on the public resource interfaces (`gpu-context/include/crd/gpu/
  *.hpp`) — so (d2b) names via the native SetName/debug-utils calls, not through an interface label field.
- **No documented single-thread creation contract** on `raster_context.hpp` — so the registry carries its own mutex
  (done); the caller-serialisation question does not arise.
- **Ownership choice point (decide in d2b):** DX12's validation capture is PROCESS-WIDE (registers on every device via
  a static registry), so two DX12 contexts each minting `res:0` from a *per-context* registry would produce colliding
  identities in the SAME capture — violating "IDs cannot silently alias." Options: per-context registry (collides),
  per-backend-module static, or one shared registry in gpu-context. Not decided here; grep-2's per-backend site
  spread is the input. (d2a) deliberately takes an explicit allocator and NO global singleton so (d2b) is free to
  choose.

### (d2a) landed

- **`engine/gpu/gpu-context/include/crd/gpu/identity_registry.hpp`** (header-only, no vendor types): `IdentityRegistry`
  holds one `crd::containers::SlotMap<crd::u8>` per `ObjectKind` (three independent index spaces). `mint(kind) ->
  ObjectIdentity{kind, handle.index, handle.generation}`, `retire(id) -> bool` (false on stale/invalid — double-retire
  rejected), `alive(id)`, `live_count(kind)`. The alias-proof property (recycled index + bumped generation) is
  **inherited from SlotMap** — no second mechanism, no separate retired-log (SlotMap's generation bump IS the
  retired-provenance primitive; descriptor/range provenance is 7b). Explicit-allocator ctor, no process-global
  singleton (ownership is (d2b)'s call). Internal `std::mutex` — creation and the validation callbacks can run
  concurrently (7a(d1) proved concurrent delivery); a registry that corrupts under two creators is worse than a lock.
- **`format_debug_name`** (added to `object_identity.hpp`, beside encode/parse): builds `"[<encoded id>] <site label>"`
  — the bracketed-prefix form (d1)'s `parse` was built to find — reusing `encode()`; refuses an invalid identity or a
  too-small buffer (0, writes nothing), same contract as `encode`. `kDebugNamePrefixChars` sizes the max prefix. It
  does NOT widen to `LPCWSTR` (that is (d2b)'s DX12 wiring).
- **Oracle** `tests/gpu/gpu-context/test_diag_identity_registry.cpp` (`[gpu][diag][identity][registry]`, device-free,
  50 assertions / 7 cases): mint validity + generation 1 + live_count; retire idempotent + rejects invalid; **alias-
  proof** — mint A, retire A, mint B reuses A's slot with `B.index==A.index`, `B.generation==A.generation+1`,
  `!alive(A)`, `alive(B)`, `A!=B`; per-kind spaces independent (Resource/Program/Pass all index 0, all distinct); a concurrency case (4 threads x 100 mints -> 400 distinct indices, live_count 400) pins the documented mutex;
  `format_debug_name` **exact string equality** (not just "parse finds it" — a dropped bracket would still parse), then
  round-trips via `parse` with the label preserved after "] "; refuses invalid + too-small (canary intact). Teeth
  (win-debug): making `mint` hardcode generation 1 fails the alias-proof case (3/47 at the generation/alive/inequality
  lines) → restored, header touched (mtime scar), rebuilt → 50/50.
- **Lanes.** `crd-gpu-context-tests` builds on win-debug + win-shipping (`/O2 /WX`) + win-clang-cl-shipping
  (`-flto=thin`); `[registry]` 50/50 on win-debug AND win-shipping /O2 (the concurrency case runs under the optimizer too). New TU registered in the explicit CMakeLists
  list. Container check PASS; both validators PASS. No device (pure value machinery).

**Scope honesty.** (d2a) is the minting + name-formatting machinery ONLY. No `mint()` is called at any creation site,
no `SetName`/`vkSetDebugUtilsObjectNameEXT` is invoked, no registry instance is owned by any context — all (d2b). Row
080 stays **Open** (DIAG.0-gated; sub-units (d2b),(e),(f),(g) remain).

## (d2b) split into (d2b-dx12) + (d2b-vk), (d2b-dx12) split into (a)+(b); (d2b-dx12-a) landed

Wiring "mint + name" across every backend creation site is many units, and DX12 alone has **71 real native creation
sites** (`CreateCommittedResource`/`CreatePipelineState`/`CreateRootSignature`/`CreateDescriptorHeap`; `dx12_raster_
context.cpp` holds 58) — a full sweep cannot be done AND proven in one tick alongside the ownership decision. Splits:

- **(d2b-dx12-a) — ownership decided + a DX12 naming helper + ONE class (storage buffer) wired + in-process proof.**
  (this tick)
- **(d2b-dx12-b) — sweep the remaining DX12 native-object classes** (mechanical, once the helper exists).
- **(d2b-vk) — prefix the Vulkan `name_image` helper** (separate tick; no in-process name read-back exists on Vulkan,
  so its correlated proof is a real message → (g)).

**Ownership decision (the choice point from (d2a), now settled).** Identity uniqueness scope must equal the validation
capture's correlation scope. The DX12 capture is PROCESS-WIDE (a static registry hooks every device), so two DX12
contexts each minting `res:0` from a per-context registry would collide in that one capture — violating "IDs cannot
silently alias." Chosen: **one process-wide `crd::gpu::identity_registry()`** (declared in `identity_registry.hpp`,
defined out-of-line in `identity_registry.cpp` as a function-local static, `default_allocator()`, no init parameter —
a first-call-wins allocator arg would be a scar).
One shared index space across both backends is *stronger* than per-backend and is the natural correlation anchor;
`default_allocator()` is itself a static that outlives it, so destruction order is safe. Nothing in AGENTS.md or
ADR-0133 forbids a function-local static (the dx12 capture's own `registry()` is the in-module precedent).

**(d2b-dx12-b) inventory (grep, captured — not acted on):** 71 native creation sites — `dx12_compute_context.cpp` 6,
`dx12_frame_descriptors.cpp` 1, `dx12_raster_context.cpp` 58, `dx12_ray_tracing_context.cpp` 4,
`dx12_work_graph_context.cpp` 2. (d2b-dx12-b) wires the resource/texture/target/heap classes through the same helper.

- **Naming helper** `engine/gpu/gpu-context-dx12/src/dx12_identity_naming.{hpp,cpp}` (src-private — `ID3D12Object`
  stays out of the public gpu-context modules; the `.hpp` forward-declares it and holds declarations only, `<d3d12.h>`
  and the bodies live in the `.cpp`): `detail::dx12_attach_identity(ID3D12Object*, ObjectKind, site) -> ObjectIdentity`
  mints, builds `format_debug_name` into a stack buffer, widens ASCII → UTF-16, `SetName`s it (best-effort: a failed
  debug name never fails creation — counting such failures is (e)), returns the identity. `dx12_detach_identity(id)`
  retires. `dxguid` is already PUBLIC on `crd-gpu-context-dx12`, so `WKPDID_D3DDebugObjectNameW` is available to the
  test transitively. (Both this helper and the registry accessor are out-of-line — a clean single-definition choice;
  it did not, however, resolve the clang-cl-shipping ICE below.)
- **Storage buffer wired:** `Dx12StorageBuffer` gains an `ObjectIdentity m_identity`; `create_storage_buffer` attaches
  it to the native UAV buffer (site "dx12-storage"), the destructor retires it. Retire bumps the slot generation, so a
  later message about the freed native object parses to `alive()==false` — the retired-provenance primitive (e) builds
  on (no separate log).
- **Tests** (`[dx12][validation][identity][naming]`, appended to `test_dx12_validation.cpp`, real GPU, 14 assertions /
  2 cases): (1) attach to a raw `CreateCommittedResource` buffer → read the name back with
  `GetPrivateData(WKPDID_D3DDebugObjectNameW)` → narrow → **exact equality** with `format_debug_name(id,"dx12-storage")`
  → `parse` returns the id → `identity_registry().alive(id)`; `detach` → `!alive`. (2) wiring by delta —
  `create_storage_buffer()` raises `live_count(Resource)` by exactly 1, its destruction returns it to baseline
  (deltas, since the process-wide registry may hold other live objects). Teeth (win-debug, real GPU): skipping the
  `SetName` call makes the round-trip's `GetPrivateData` REQUIRE fail (test 1, line ~1281) → restored, helper touched
  (mtime scar), rebuilt → 14/14.
- **Lanes.** `crd-gpu-context-dx12` lib + `crd-gpu-context-dx12-tests` exe build and the `[naming]` tests pass on
  **win-debug (14/14)** and **win-shipping /O2 /WX (14/14)** on the real GPU; the registry lib + `crd-gpu-context-tests`
  `[registry]` (50/50) build on all three lanes. On **win-clang-cl-shipping** the `dx12-tests` exe links (see the ICE
  note) — the feature is present on all three lanes.
- **CORRECTION (d2b-dx12-b tick): the win-clang-cl-shipping `lld-link` 0xC0000005 is FLAKY, not deterministic, and NOT
  tied to the identity wiring.** The (d2b-dx12-a) tick recorded it as "deterministic, engine-TU-triggered, bisected to
  `dx12_raster_context.cpp`" and filed `task_35b3af62`. That was WRONG — an artifact of unlucky repeated repro under
  load. The (d2b-dx12-b) tick established: re-linked in isolation, **all three** affected exes link clean on
  clang-cl-shipping — `crd-gpu-context-dx12-tests`, `crd-gpu-context-tests`, AND `crd-gpu-context-encoder-gpu-tests`;
  the crash appeared only under concurrent LTO-link load (three heavy links back-to-back plus a concurrent GPU suite),
  and once on `crd-gpu-context-tests`, which contains NONE of the DX12 identity wiring. So it is a flaky LLVM 20.1.8
  thin-LTO lld-link stability issue (memory/parallelism-sensitive), in the toolchain / CI-build-parallelism domain —
  not a code defect. `task_35b3af62` was dismissed (premise falsified). If it recurs in CI, it is a toolchain matter
  (LLVM version, link parallelism), the user's domain — not a fix in `dx12_raster_context.cpp`.

**Scope honesty.** (d2b-dx12-a) wires ONE class (storage buffer) with mint + `SetName` + retire, proven in-process by a
name round-trip on the real GPU; the ownership decision (process-wide registry) is settled. It does NOT sweep the other
70 DX12 native-creation sites (d2b-dx12-b), name any Vulkan object (d2b-vk), or provoke a real hazard message (g). Row
080 stays **Open** (DIAG.0-gated; sub-units (d2b-dx12-b),(d2b-vk),(e),(f),(g) remain).

## (d2b-dx12-b) batch 1 — textures

The mechanical sweep of the 71 DX12 native-creation sites, one class-GROUP per tick (never partial within a group).
Batch 1 = the `Dx12Texture` family only, mirroring the `Dx12StorageBuffer` reference pattern from (d2b-dx12-a).

- **Wired:** `Dx12Texture` gains an `ObjectIdentity m_identity` (ctor param defaulted `= {}`); the four public
  factories — `create_texture` / `create_texture_from_mips` / `create_texture_dim` (sites all labelled "dx12-texture")
  and `create_depth_texture` ("dx12-depth-texture") — call `detail::dx12_attach_identity(tex.Get(),
  ObjectKind::Resource, …)` and the destructor retires. Textures are `ObjectKind::Resource`.
- **Attach-after-last-return rule (stated, applies to every future batch):** the `mint`+`SetName` MUST go AFTER the
  last `return nullptr` between the native create and the object's construction, or a failed creation leaks an identity
  into `live_count`. All four texture factories place `make_unique` after their final `if (!submit_and_wait()) return
  nullptr;`, so attaching immediately before the `make_unique` is safe. Attach is also done BEFORE `std::move(tex)`
  (argument evaluation order is unspecified), in a separate statement.
- **Frame-graph transient `Dx12Texture` (site ~8041, `new Dx12Texture(n.resource, …)`) is deliberately NOT wired** —
  it is a REN-1 frame-graph node; the defaulted identity param leaves it invalid and its destructor's detach is a
  no-op. Frame-graph transient images are a later batch (batch 2 at earliest, possibly their own tick).
- **Batch-2 choice point (recommended, NOT decided):** a raster target is ONE Cerid resource backed by several native
  objects (color / resolve / depth / readback). Recommend one identity per LOGICAL resource, `SetName`'d onto each
  native sub-object with a role suffix in the site label ("dx12-target-color", "dx12-target-depth") — so a message
  about the depth buffer correlates to the Cerid target. Minting per native object would fragment one resource into N
  identities. Heaps are infrastructure, not a Cerid resource — decide their disposition when the sweep reaches them.
- **Test** (`[dx12][validation][identity][naming]`, appended, real GPU): `create_texture` raises `live_count(Resource)`
  by 1 and `create_depth_texture` by another; each destruction restores the baseline (deltas). NO `GetPrivateData`
  round-trip for textures — `Dx12Texture` exposes no test-reachable native handle, and adding an accessor for a test is
  the wrong trade; the (d2b-dx12-a) raw-helper round-trip is the naming proof, the delta is the wiring proof. Teeth
  (win-debug, real GPU): neutralising the color-texture factory's attach fails the `create_texture` delta (3
  assertions) → restored, TU touched (mtime scar), rebuilt → 21/21 (3 cases: storage-buffer round-trip + storage-buffer
  delta + texture delta).

## (d2b-dx12-b) batch 2 — raster & G-buffer targets

Batch 2 = the render-TARGET class group: `Dx12RasterTarget` (colour / visbuffer / MSAA-with-resolve / colour+depth /
colour+D24S8 / VRS factories) and `Dx12GBufferTarget` (N colour planes). Both settle the batch-1 choice point in code.

- **Name-only helper (the refactor this batch needed):** `detail::dx12_attach_identity` MINTS on every call, so
  applying it to colour+resolve+depth+readback+heaps would mint N identities per target — exactly the fragmentation the
  choice point rejects. Added a name-only sibling `detail::dx12_name_object(ID3D12Object*, const ObjectIdentity&,
  site)` (format_debug_name + widen + `SetName`, **no mint**, no-op on null); `dx12_attach_identity` is now `mint` +
  `dx12_name_object`. One mint on the primary, name every sibling with the returned id.
- **Decided rule — one identity per LOGICAL resource, named onto every native sub-object it OWNS:** minting moved INTO
  the target constructors — every standalone factory (colour / visbuffer / MSAA-with-resolve / colour+depth /
  colour+D24S8 / VRS) funnels through the `Dx12RasterTarget` ctor, so the one-mint-per-logical-target guarantee is
  structural, not per-factory-site. `Dx12RasterTarget` mints on `m_tex` ("dx12-target-color") and names `m_resolve` /
  `m_depth` / `m_readback` / `m_rtv_heap` / `m_dsv_heap` (role-suffixed) + `m_vrs` via `set_vrs` ("dx12-target-vrs");
  the dtor retires once. `Dx12GBufferTarget` mints on plane 0 ("dx12-gbuffer-color") and names planes 1..N-1, all
  readbacks ("dx12-gbuffer-readback"), and the rtv heap ("dx12-gbuffer-rtv-heap"); dtor retires once.
- **Opt-out for frame-graph nodes (ctor-mints has no implicit opt-out, so an EXPLICIT one was added):** because minting
  is in the ctor, EVERY `Dx12RasterTarget` construction would otherwise mint — including the three NON-factory,
  frame-graph node sites (`image_with_depth` REN-40, and the depth/colour transient `slice_target` REN-3/39, one of
  which passes a NULL colour tex). A new `Dx12RasterTarget::IdentityMode { Mint, None }` ctor tag (defaulting `Mint`)
  gives those three sites `IdentityMode::None`: they keep the defaulted-invalid identity (dtor detach is a no-op),
  matching batch 1's deferral of the transient `Dx12Texture`. Frame-graph transient/RTT target identity (with realias
  generation semantics) is the frame-graph batch's, not this one's. Verified: the full suite includes
  `test_dx12_frame_graph.cpp` (real transient aliasing) and stays balanced at 10297/184.
- **Heap disposition decided:** a **target-owned** rtv/dsv heap is a sub-object of that logical resource → named with
  the target's identity + a role suffix. A **standalone** heap (the frame-descriptor arena in `dx12_frame_descriptors.
  cpp`) is infrastructure, not a Cerid resource → left unwired. Principle: sub-object of a logical resource → its
  identity + role suffix; standalone → skip.
- **Not wired (deliberate):** frame-graph transient / RTT target nodes (via the `IdentityMode::None` opt-out above); the
  frame-descriptor arena heap; anything in the compute / work-graph contexts (later batches).
- **Tests** (`[dx12][validation][identity][naming]`, appended, real GPU): (1) the discriminating +1-not-N proof —
  `create_gbuffer_target(3)` raises `live_count(Resource)` by **exactly 1** (a per-native-object scheme would show +7),
  and `create_color_target` / `create_color_depth_target` each +1 despite owning 3 / 5 native objects; each destruction
  restores. (2) the name-only mechanism itself — `dx12_attach_identity` on a raw resource then `dx12_name_object` on a
  second raw resource with the same id: `live_count` rises by exactly 1 (one mint, not two), BOTH objects'
  `GetPrivateData(WKPDID_D3DDebugObjectNameW)` read back the exact `format_debug_name` string and `parse` to the SAME
  identity (a message about either sub-object resolves to the one logical target). Teeth (win-debug, real GPU):
  neutralising the G-buffer mint fails the +1 delta (0==1) → restored, TU touched (mtime scar), rebuilt → 5 cases /
  45 assertions `[naming]`. Full `crd-gpu-context-dx12-tests` re-run **10297/184** on win-debug (no regression).
- **Lanes:** win-debug builds + full suite 10297/184; win-shipping (`/O2 /WX`) builds clean AND runs `[naming]` 45/5 on
  the real GPU. win-clang-cl-shipping (thin-LTO): the FLAKY LLVM 20.1.8 `lld-link` 0xC0000005 struck again at the LINK
  step (compile clean) — consistent with the (d2b-dx12-b batch-1) finding that this is a toolchain/concurrency stability
  issue, not a code defect; the fix is to re-link the exe in isolation (an isolated re-confirm this batch was not
  completed — the link runs long and was reaped to avoid a next-tick concurrent-load collision; batch 1 already proved
  all three exes link clean in isolation). Row stays **Open** (DIAG.0-gated; sub-units (d2b-dx12-b batch 3+),(d2b-vk),
  (e),(f),(g) remain). *(SUPERSEDED — see batch 3: the crash reproduces in isolation with a varying signature; an
  isolated re-link does NOT reliably fix it. The "concurrency only / re-link fixes it" reading is wrong.)*

## (d2b-dx12-b) batch 3 — raster programs (ObjectKind::Program)

Batch 3 is defined by KIND, not by file: the Cerid *program* objects. This tick wires the one clean program CLASS with
factory construction — `Dx12RasterProgram` (`IRasterProgram`) — as `ObjectKind::Program`. The compute / RT / work-graph
"programs" are context-embedded pipeline structs (a different ownership model) and are a documented later sub-batch (3b),
so this batch is complete-within-its-group, not partial.

- **Wired (`Dx12RasterProgram`, ObjectKind::Program):** the class gains an `ObjectIdentity m_identity`. Its ctor mints on
  the **root signature** (`m_root`, the always-present primary — `valid()` requires it) with site "dx12-program-rootsig"
  and stamps the id onto the eager `m_pso1`; the dtor retires once. All four factories (`create_raster_program` + the
  three mesh/tess variants at raster_context ~1736/1794/1856/1923) funnel through this ctor → one mint per program by
  construction. There are **no** non-factory / frame-graph `Dx12RasterProgram` construction sites (unlike the targets),
  so no `IdentityMode::None` opt-out is needed.
- **Lazy PSO variants named, never minted:** `pso_for` builds PSOs on first use (the default `m_pso1` slot + a keyed
  `m_cache[]` of up to 8 variants per colour/depth/samples/topology combo). Both build sites now call
  `dx12_name_object(pso, m_identity, "dx12-program-pso")` — a program with N cached PSOs is still ONE Program identity
  (the +1-not-N property, this time proven by the discriminator draw below rather than by a G-buffer count).
- **Test** (`[dx12][raster][program][identity][naming]`, appended to `test_dx12_raster.cpp` — the TU that already owns
  the dxc/DXIL raster-program fixture; same exe, so `[naming]` spans both TUs): creating a program raises
  `live_count(Program)` by exactly 1 **and leaves `live_count(Resource)` unchanged** (this is the only assertion that
  proves `ObjectKind::Program`, not `Resource`); a draw then forces `pso_for` to build the default PSO and the Program
  count STAYS +1 (a PSO build does not mint); destruction retires. 13 assertions (incl. the Resource-delta proof that the draw ran + an MSAA cache-variant that keeps Program at +1). Teeth (win-debug, real GPU):
  neutralising the program mint fails the Program +1 delta (0==1, both the post-create and post-draw checks) → restored,
  TU touched, rebuilt.
- **Classified remaining program-surface inventory (so batch 3b/4 start from a real list, not "71 minus stuff"):**
  - *Not a GPU object → no identity:* `Dx12GpuProgramImpl` (dx12_context.cpp) holds DXIL bytecode only (`m_dxil`), no
    native ID3D12 object; `create_program` returns these and they are correctly unnamed.
  - *Context-owned helper PSOs/root-sigs (infrastructure, skip — no program class wraps them):* in
    dx12_raster_context.cpp — the blit/present PSO (`m_blit`, ~5841/5862) and three internal compute-helper pipelines
    (~5194/5215, 5247/5270, 5312/5333: TLAS / kernel-buffer / sampler compute internals).
    **(PARTLY SUPERSEDED — batch 3c: the three ~5215/5270/5333 pipelines are `kernel_pipeline` / `rt_kernel_pipeline` /
    `sampled_kernel_pipeline`, which dispatch AUTHORED `IGpuProgram` kernels (`dx_prog->dxil()`) — they are Program-kind
    caches, wired in 3c, NOT infrastructure. Only the blit PSO `m_blit` is genuinely infrastructure.)**
  - *Still-open program classes → batch 3b (programs):* compute pipelines in `dx12_compute_context.cpp` (`pl->root` +
    `pl->pso`, ~461/484/490); RT pipelines in `dx12_ray_tracing_context.cpp` (root ~375 + compute PSO ~383) + the DXR
    state object in dx12_raster_context.cpp (`CreateStateObject` ~2815); the work-graph state object in
    `dx12_work_graph_context.cpp` (root ~158 + `CreateStateObject` ~185). Each is a context-embedded struct, its own tick.
  - *Batch 4 (Resource-kind sites in the other contexts):* compute/RT/work-graph buffers + heaps — deferred, still Open.
- **Lanes:** win-debug full suite **10310/185** (+13/+1 = the program test; no regression); win-shipping (`/O2 /WX`)
  builds clean AND runs `[naming]` **58/6** on the real GPU. win-clang-cl-shipping (thin-LTO): `lld-link` crashed at the
  LINK step **twice in isolation this batch** with DIFFERENT signatures (`0xC0000005` on the `test_dx12_rt.cpp.obj` LTO
  Function-Pass-Manager pass, then "Illegal instruction") — a non-deterministic LLVM lld-link/LTO-codegen crash on a TU
  unrelated to this change, NOT a source defect (no diagnostic; correctness is proven on win-debug + win-shipping). The
  earlier "flaky under concurrent load, isolated re-link fixes it" note is corrected: it reproduces in isolation and a
  re-link does not reliably fix it (memory updated). Row stays **Open** (DIAG.0-gated; sub-units batch 3b, batch 4,
  d2b-vk, e, f, g remain).

## (d2b-dx12-b) batch 3b — compute kernels (ObjectKind::Program)

Batch 3b wires the compute-pipeline program group. The three "program" sites the batch-3 census flagged have DIFFERENT
ownership shapes (read this tick), and only one is a persistent, factory-returned program — so 3b = compute kernels, and
the RT / work-graph sites are classified (with function names) below, not wired.

- **Wired (compute, ObjectKind::Program):** `build_dxil_pipeline` (dx12_compute_context.cpp, the shared builder behind
  both `create_pipeline_from_hlsl` and `create_pipeline_from_dxil`) returns a `PipelineImpl` (a `struct : ComputePipeline`,
  owned by `unique_ptr`). It gains an `ObjectIdentity m_identity` + a `~PipelineImpl` that retires (ONE retire site — the
  unique_ptr destruction; `ComputePipeline` has a virtual dtor). The builder mints on `pl->root` (site "dx12-compute-
  rootsig") **after the last `return nullptr`** (once both PSO paths — pipeline-library load at ~484 and plain create at
  ~490 — have set `pl->pso`), then names `pl->pso` once ("dx12-compute-pso"). Struct, not a class with a ctor → the
  batch-1 FACTORY-SITE rule (mint after the last early-return), not the batch-2/3 ctor rule.
- **Pipeline-library hazard avoided:** the D4 warm-start cache keys PSOs by `pName` = an FNV-1a hash of the DXIL+layout
  (deliberately RUN-STABLE, so identical shaders share a cache slot across process runs). `ObjectIdentity{index,gen}` is
  PER-RUN and must never enter that key. It does not: the identity rides only the WKPDID `SetName` debug-name channel
  (`dx12_name_object`), which is separate from `LoadComputePipeline`/`StorePipeline`'s `pName`.
- **No logical-program dedup → +1 per create call:** each `create_pipeline_from_*` builds a fresh `PipelineImpl` (the
  library cache only warms PSO *creation*, it does not return a shared pipeline object), so two identical kernels are
  **+2**, not +1 — the discriminator in the test. PSOs are eager (built in the builder), so no dispatch is needed to
  force one.
- **Test** (`[dx12][compute][program][identity][naming]`, appended to `test_dx12_compute.cpp` — reuses its `kVecAddHlsl`
  fixture; same exe): create raises `live_count(Program)` by exactly 1 AND leaves `live_count(Resource)` unchanged (the
  kind assertion); a second identical kernel is +2 (no dedup); each destruction retires to baseline. 8 assertions (incl. a compute-buffer allocation that leaves Resource flat -- BufferImpl is unwired). Teeth
  (win-debug, real GPU): neutralising the compute mint fails the Program deltas (0==1) → restored, TU touched, rebuilt.
- **Classified NOT wired (with function names, so batch 3c/4 start from a decision):**
  - *Ephemeral per-dispatch — NOT a persistent program (deferred; a transient-identity model, not a program batch):*
    RT `Dx12RayTracingContext::trace_dispatch` (dx12_ray_tracing_context.cpp:345) and work-graph
    `Dx12WorkGraphContext::dispatch_graph` (dx12_work_graph_context.cpp:127) build their root sig + compute PSO / state
    object as LOCAL ComPtrs, use them for one dispatch, and discard them at function return. There is no persistent
    program object to correlate; minting there would churn mint+retire every dispatch. If per-dispatch correlation is
    ever wanted it is the transient model (like the frame-graph target nodes), not this sweep. (Work-graph tests also
    live in a SEPARATE exe, `crd-gpu-context-dx12-work-graph-tests`.)
  - *Persistent, content-cached RT pipeline surface in raster_context → batch 3c candidate (its own tick):* the DXR
    state-object cache (`CreateStateObject` -> `out.state`, keyed by `out.key[]` content hash, ~2815), the cached RT
    root signature `m_rt_root` (~5238), and the cached RT compute PSOs `m_rt_pso[]` (keyed by DXIL, ~5267). These ARE
    persistent, content-keyed (dedup → +1 per unique content), and span several object types — a distinct wiring model
    from a single factory, so its own tick. Its tests are in `test_dx12_rt.cpp` (the TU on which win-clang-cl-shipping's
    lld-link has crashed — expect the flake there, don't attribute it).
- **DX12 persistent-program identity is complete after 3b for the FACTORY-returned programs** (grep
  `create_*program|create_*pipeline|unique_ptr<*Program>|unique_ptr<*Pipeline>` across dx12 src + include: the only
  factory-returned program types are `IRasterProgram` — all 4 factories `create_raster_program`/`create_mesh_program`/
  `create_task_mesh_program`/`create_tess_program` construct `Dx12RasterProgram` (batch 3), `ComputePipeline` — all real
  builders route through `build_dxil_pipeline` (batch 3b; the `create_pipeline(shader_dir,name)` overload is a `nullptr`
  stub), and `IGpuProgram` — `Dx12GpuProgramImpl`, DXIL-only, correctly no identity. No others). Remaining 7a DX12 work:
  batch 3c (the cached RT pipeline surface), batch 4 (Resource-kind sites in the compute/RT/work-graph contexts —
  buffers/heaps), then d2b-vk, (e), (f), (g).
- **Lanes:** win-debug full suite **10318/186** (+8/+1 = the compute test; no regression); win-shipping (`/O2 /WX`)
  builds clean AND runs `[naming]` **66/7** on the real GPU. win-clang-cl-shipping (thin-LTO): not re-attempted this tick
  (the non-deterministic lld-link/LTO-codegen crash characterised in batch 3 is a toolchain issue, ≤2 attempts, never a
  row gate — correctness is on win-debug + win-shipping). Row stays **Open** (DIAG.0-gated).

## (d2b-dx12-b) batch 3c — raster-context compute-PSO caches (ObjectKind::Program)

**Why 3c ≠ "RT" (a scope correction).** The batch-3 census called the ~5215/5270/5333 pipelines "infrastructure". That
was wrong: `kernel_pipeline` (5210), `rt_kernel_pipeline` (5265) and `sampled_kernel_pipeline` (5328) all dispatch
AUTHORED kernels — each takes `dx_prog->dxil()` from an `IGpuProgram&` in `dispatch_kernel` / `dispatch_kernel_indirect`
/ `dispatch_kernel_sampled`. They are three BYTE-IDENTICAL Program-kind PSO caches (`ComPtr[cap] + key[cap] + n`,
append-only, cap → `nullptr`, no eviction). So batch 3c is grouped by PATTERN = these three caches, not by "RT".

- **Wired:** a parallel `ObjectIdentity m_{kernel,rt_pso,sampled}_id[kKernelPsoCap]{}` next to each cache. On the cache
  **miss** path, after `CreateComputePipelineState` succeeds and adjacent to the PSO store (so the parallel arrays cannot
  desync — `m_X_n` is the single source of truth for both), mint ONE identity on the PSO (`ObjectKind::Program`, sites
  "dx12-kernel-pso" / "dx12-rt-kernel-pso" / "dx12-sampled-kernel-pso"). A cache **hit** returns above with NO mint.
- **Shared root sigs are NOT named.** `m_kernel_root` / `m_rt_root` / `m_sampled_kernel_root` are lazily-created
  singletons shared across all kernels of their kind — stamping one program's id onto a shared object is the aliasing
  bug batch 2 rejected for shared heaps. They stay unnamed (infrastructure).
- **Retire in `~Dx12RasterContext`:** three loops over `[0, m_X_n)` calling `dx12_detach_identity`. The caches are
  append-only (cap-full returns `nullptr`, nothing is ever evicted/overwritten), so the context dtor is the SINGLE
  retire site — no per-entry dtor (the `ComPtr` arrays hold PSOs, the identities live in the parallel arrays). Verified:
  a grep for writes to `m_{kernel,rt_pso,sampled}_n` / `m_{...}_pso[...]` finds ONLY the `= 0U` initializers and the
  miss-path stores — no reset / device-loss / recreate path zeroes a count or `.Reset()`s an entry (which would drop a
  PSO without retiring its identity, an unrecoverable `live_count` leak). If such a reset is ever added, it must retire.
- **Pointer-key dedup (encode what the code does):** the caches key by the kernel's DXIL POINTER
  (`m_kernel_key[i] == dxil`, i.e. `dx_prog->dxil().data()`, stable for an `IGpuProgram`'s lifetime), NOT by content. So
  the SAME `IGpuProgram` dispatched twice is **+1** (a hit), while a SECOND program built from identical source is a
  distinct pointer → a new entry → **+2**. This is the discriminator, and it is per-program-object, not content-dedup.
  The +1-on-hit assertion depends on `dx_prog->dxil().data()` being stable for the program's lifetime; verified
  write-once (grep `m_dxil.` in dx12_context.cpp: only `resize` in the ctor + `data()`/`size()` reads, no later mutation).
- **Test** (`[dx12][kernel][program][identity][naming]`, in `test_dx12_validation.cpp` — reuses its frame-graph +
  `enc_dispatch` harness, since `dispatch_kernel` requires `frame_recording()`): a CKIR compute kernel dispatched through
  a one-pass graph raises `live_count(Program)` by exactly 1 on the miss AND leaves `live_count(Resource)` unchanged (a
  PSO is Program-kind); a second dispatch of the same program is still +1 (pointer hit); a second program from identical
  source is +2; destroying the raster context returns Program to baseline (the dtor retire). 16 assertions. Teeth
  (win-debug, real GPU): neutralising the `kernel_pipeline` mint fails the deltas (0==2) → restored, TU touched, rebuilt.
  `m_rt_pso` / `m_sampled_pso` are byte-identical in shape and wiring; their mint lines were reviewed for
  array/index/site correctness (`m_rt_pso_id[m_rt_pso_n]` / `m_sampled_id[m_sampled_pso_n]`, each before its `++`) but
  are NOT exercised by a test this tick (a dedicated RT-query / HZB dispatch fixture is heavier and adds no new
  mechanism) — the mechanism + teeth on `kernel_pipeline` plus the review are the coverage.
- **Lanes:** win-debug full suite **10334/187** (+16/+1 = the kernel test; no regression); win-shipping (`/O2 /WX`)
  builds clean AND runs `[naming]` **82/8** on the real GPU. win-clang-cl-shipping (thin-LTO): not re-attempted (the
  non-deterministic lld-link/LTO crash is a toolchain issue, ≤2 attempts, never a row gate; correctness is on win-debug +
  win-shipping). Row stays **Open** (DIAG.0-gated; remaining: batch 3d = the `m_dxr` DXR state-object cache, batch 4 =
  Resource-kind sites in the compute/RT/work-graph contexts, then d2b-vk, e, f, g).

## (d2b-dx12-b) batch 3d — DXR state-object cache (ObjectKind::Program)

Batch 3d wires the last DX12 program surface: the raster context's DXR ray-tracing-pipeline cache `m_dxr[kKernelPsoCap]`
(struct `DxrPipe` = `key[6]` + `state` (ID3D12StateObject) + `sbt` (ID3D12Resource) + …), reached via `trace_rays` →
`trace_rays_impl` → `dxr_pipeline`.

- **Wired:** `DxrPipe` gains `ObjectIdentity identity{}`. It is COPIED by value into the array (`m_dxr[m_dxr_n] = out`),
  so it gets NO retiring dtor (a dtor would make the local `out` retire on scope exit and double-retire the copy).
  The mint sits on the cache-MISS build, AFTER the last SBT `return nullptr` + `Unmap` + `sbt_va` and immediately BEFORE
  the copy: `out.identity = dx12_attach_identity(out.state.Get(), Program, "dx12-rt-pipeline")` (primary = the state
  object), then `dx12_name_object(out.sbt.Get(), out.identity, "dx12-rt-sbt")`. The copy carries `out.identity` into the
  slot; retire is a loop over `m_dxr[0..m_dxr_n)` in `~Dx12RasterContext` (added to the 3c retire block — same
  append-only, single-site rule). The SBT is NAMED, not minted, so `live_count(Resource)` stays flat.
- **Content-key dedup is REAL here** (unlike the pointer-keyed compute caches): `DxrPipe::key` is `fnv1a_64` over each
  stage's DXIL BYTES (the DxrPipe::key SCAR — a pointer key returned a stale state object when a freed program's DXIL
  buffer was reused at the same address). So a pipeline built from the SAME stage bytes is a genuine content HIT → +1;
  distinct content → +2.
- **Reachability + test (driven, not skipped):** `trace_rays` is NOT on `IRasterContext` — it is recorded via the
  encoder's `TraceDesc` verb inside a frame-graph pass (needs `frame_recording()`), and is DXR-gated
  (`supports_rt_pipeline()` = a real `RaytracingTier >= 1_0` check). This box HAS DXR (the `[dx12][rt]` suite runs, not
  skips), so the test DROVE the real path: three CKIR RT stages (`build_rt_pipeline_{raygen,closesthit,miss}` →
  `emit_rt_stage_hlsl` → `compile_hlsl_to_dxil` → `create_program`), a built `Dx12RtScene` acceleration structure, and
  `encoder->trace_rays(TraceDesc)` in a one-pass graph. Asserts: miss → `Program +1` & `Resource` unchanged; identical
  stages again → still +1 (content hit); destroy context → baseline. `[dx12][rt][program][identity][naming]`, 20
  assertions. Teeth (win-debug, real GPU + real DXR): neutralising the mint fails the deltas (0==1) → restored, TU
  touched, rebuilt. (Test WARN-skips honestly if a future box lacks DXR or dxc.)
- **DX12 program identity is now COMPLETE.** Every factory-returned program (`Dx12RasterProgram` — batch 3; compute
  kernels via `build_dxil_pipeline` — 3b) and every cached PSO/pipeline surface (`m_kernel_pso`/`m_rt_pso`/`m_sampled_pso`
  — 3c; `m_dxr` — 3d) mints an `ObjectKind::Program` identity and retires it. Remaining 7a DX12: **batch 4** = the
  Resource-kind sites in the compute/RT/work-graph contexts (compute `BufferImpl`, RT `make_buffer`/`def[]`/`up[]`/`rb[]`,
  work-graph backing buffers); then `d2b-vk`, (e), (f), (g).
- **Lanes:** win-debug full suite **10354/188** (+20/+1 = the DXR test; no regression); win-shipping (`/O2 /WX`) builds
  clean AND runs `[naming]` **102/9** on the real GPU. win-clang-cl-shipping (thin-LTO): not re-attempted (the
  non-deterministic lld-link/LTO crash is a toolchain issue, ≤2 attempts, never a row gate; the exe compiles
  `test_dx12_rt.cpp`, the TU it has crashed on). Row stays **Open** (DIAG.0-gated).
- **Retire completeness (before-done grep).** The only writes to the cache are the value copy at the single miss site
  (`m_dxr[m_dxr_n] = out`) and the `++m_dxr_n` increment; `m_dxr_device.Reset()` (on a failed `As<ID3D12Device5>`)
  resets the COM device pointer, NOT the array, and there is no `m_dxr_n` reset or entry-overwrite path. So one mint site,
  one retire loop, no leak — same single-site invariant proven for the 3c caches.
- **The `Resource` assertion is a genuine oracle here (not a tautology).** The SBT is an `ID3D12Resource` that batch 3d
  NAMES but does not mint; the `res0`-unchanged assertion would FAIL if `out.sbt` were minted as Resource
  (`live_count(Resource)` would rise by 1). This is the one place in the sweep where the Resource-flat check actively
  guards a wrong classification rather than merely restating "nothing minted Resource." (Batch 4b resolved: the 3d test
  captures `res0` AFTER `build_scene`, so wiring `blas`/`tlas` bakes the scene's +1 into the baseline and the SBT-flat
  oracle across `trace_rays` is unchanged — the 3d test needed no edit.)
- **Finding (recorded, not edited — out of scope).** The comment near `test_dx12_rt.cpp:~309` ("DX12 reports
  RtPipeline=false today") is pre-A16 and stale: `supports_rt_pipeline()` is a real `RaytracingTier >= 1_0` capability
  query and returns true on this box (the `[dx12][rt]` suite runs, not skips). Left as-is (user's RT test); noted so the
  next tick doesn't re-derive it.

## (d2b-dx12) batch 4 — compute buffer (ObjectKind::Resource)

Batch 4 opens the DX12 **Resource-kind** wiring in the compute context. `BufferImpl` (the `ComputeBuffer` impl in
`dx12_compute_context.cpp`) holds one native `ID3D12Resource res` for all three `ComputeMemory` heaps (DEFAULT/UPLOAD/
READBACK take the same single-`res` path in `create_buffer`).

- **Wired:** `BufferImpl` gains `ObjectIdentity m_identity{}` and a retiring dtor `~BufferImpl() override {
  dx12_detach_identity(m_identity); }`. `create_buffer` returns `unique_ptr<ComputeBuffer>` (base has a `virtual`
  dtor), so the dtor is the single retire site. The mint sits AFTER the last early-return (`b->res == nullptr`), on the
  live native resource: `b->m_identity = dx12_attach_identity(b->res.Get(), Resource, "dx12-compute-buffer")`. No
  secondaries (there is only one resource per buffer). No dedup — distinct buffers are distinct objects. The user dtor
  suppresses the implicit move but leaves the implicit copy; grep confirms `BufferImpl` is heap-only via
  `make_unique<BufferImpl>()` and every other reference is `static_cast<BufferImpl&>` (by reference) — never copied by
  value, so no double-retire (the hazard that shaped `DxrPipe`).
- **3b test converted (strengthened, not weakened):** the batch-3b `[dx12][compute][program]` test previously asserted
  `Resource == res_before` across a `create_buffer` to prove the *program* mint was kind-orthogonal *while BufferImpl was
  unwired*. Now BufferImpl mints, so that `+0` would be wrong. The assertion is converted to `res_before + 1` (buffer
  mints one Resource) + `Program == prog_before + 1` (buffer does not touch the Program space) inside a scope, then
  `res_before` after it (buffer retires). This is a stronger oracle: it now proves the buffer mints AND is kind-orthogonal
  AND retires, where before it only proved "nothing minted Resource."
- **New test:** `[dx12][compute][resource][identity][naming]` — `create_buffer` → `Resource +1`, `Program` flat (kind
  oracle); a second byte-identical buffer → `+2` (NO dedup — the discriminator against a false shared identity);
  destroying it → `+1`; UPLOAD + READBACK buffers → `+3`; all out of scope → baseline. Teeth (win-debug, real GPU):
  neutralising the mint fails 2 cases / 5 assertions (this test + the converted 3b test) → restored, TU touched, rebuilt.
- **DX12 Resource completeness grep (`CreateCommittedResource`/`CreatePlacedResource`/`make_buffer`).** The grep lists
  EVERY creation site — the attach/name call sits on a separate line from the create call, so already-wired sites (this
  batch's `b->res`, and the batch-1/2 raster textures/targets) appear too; do not read a listed line as "unwired."
  Classified by owner LIFETIME (verified against each declaration, not by function name):
  - **Persistent owners, UNWIRED → named follow-ups (not "done"):**
    - RT `Dx12RtScene::blas` / `::tlas` (members of the `IAccelerationStructure` object; `dx12_ray_tracing_context.cpp`
      180/223/295/331) → **batch 4b**.
    - compute `Impl::ts_readback` (the persistent GPU-timestamp READBACK buffer; `dx12_compute_context.cpp` 292) →
      **batch 4c** (internal-infra resources).
    - raster `dx12_raster_context.cpp` non-target buffer resources (vertex/index/upload-ring/staging/readback +
      frame-graph `CreatePlacedResource` transients at 7961/7972) — a large mixed group (batches 1/2 wired only textures
      and render targets, not these) → **batch 4d**. FIRST subtract the already-wired batch-1/2 sites
      (`Dx12RasterTarget`/`Dx12GBufferTarget` textures, render targets, heaps) by tracing each ComPtr to its OWNER, not
      by line. What remains needs a per-site read: mint persistent buffer owners; for the frame-graph placed resources
      VERIFY whether `n.resource` outlives the frame (plan-cached) or is rebuilt per frame before classifying —
      batch-2 precedent gave the FG-node `Dx12RasterTarget` sites `IdentityMode::None`.
  - **Transient locals (freed at scope end — grep-verified they are function-locals/`Array`s, NOT struct members) →
    census-only, deliberately NOT minted:** RT build scratch/feed `bscratch`/`tscratch`/`ibuf`/`vbuf`/`abuf` (locals in
    `build_scene`) and per-dispatch `def[16]`/`up[16]`/`rb[16]` (local arrays in `trace_dispatch`); work-graph `backing`
    and `dev_bufs`/`up_bufs`/`rb_bufs` (locals/`Array`s in `dispatch_graph`). Same class as the RT pipeline's ephemerals.
- **Forward note for 4b:** the batch-3d census claim that the SBT `res0`-unchanged check is "the one place Resource-flat
  is a genuine oracle" holds ONLY until 4b — the `[dx12][rt]` test builds a `Dx12RtScene` via `build_scene`, so once 4b
  wires `blas`/`tlas` that test's Resource baseline must be captured AFTER `build_scene` (else the AS mints shift it).
- **Lanes:** win-debug full dx12 exe **10366/189** (+12/+1 = the new resource test; no regression); win-shipping
  (`/O2 /WX`) builds clean AND runs `[naming]` **114/10** on the real GPU. win-clang-cl-shipping (thin-LTO): not
  re-attempted (non-deterministic lld-link/LTO crash, ≤2 attempts, never a row gate). Row stays **Open** (DIAG.0-gated;
  DX12 Resource remaining: 4b RT AS, 4c compute ts_readback, 4d raster buffers; then d2b-vk, e, f, g).

## (d2b-dx12) batch 4b — acceleration structure (ObjectKind::Resource); + 4c classification

Batch 4b wires the RT acceleration structure. A `Dx12RtScene` (`SceneImpl : Dx12RtScene : IAccelerationStructure`,
`dx12_ray_tracing_context.cpp`) owns two native buffers — `blas` and `tlas` — but is ONE logical resource.

- **Wired (one identity per LOGICAL resource — the census line-431 rule, as for `Dx12RasterTarget`):** `SceneImpl`
  gains `ObjectIdentity m_identity{}` and a retiring `~SceneImpl() override`. The mint is on the **TLAS** (the bound
  handle the root SRV reads), the **BLAS is NAMED with the same id** (`dx12_name_object`, "dx12-rt-blas"), NOT minted.
  So `live_count(Resource)` rises by exactly **+1 per scene** despite two native buffers. Both real builders got the
  mint (`build_scene_instanced` and `build_scene_curves`; `build_scene` forwards to the former) — placed AFTER the last
  early-return (`if (!impl.submit_and_wait()) return nullptr;`), i.e. only a fully-built scene mints. `blas`/`tlas` are
  each assigned exactly once per builder (grep: no refit/rebuild reassignment on a live scene), so no overwrite/retire
  dance is needed. Completeness (grep): exactly two `make_unique<SceneImpl>` sites and exactly two `dx12-rt-tlas` mints
  — every scene-constructing path mints (`build_scene` forwards to `build_scene_instanced`). `SceneImpl` is heap-only via
  `make_unique` and never copied by value (the user dtor suppresses the implicit move; grep confirms no copy), so
  retirement is single-site — the same double-retire guard proven for `BufferImpl`. The transient build feeds —
  `bscratch`/`tscratch`/`ibuf`/`vbuf`/`abuf`, all function-locals — are NOT minted.
- **New test:** `[dx12][rt][resource][identity][naming]` — `build_scene` → `Resource +1` (one id, two buffers) &
  `Program` flat (kind oracle); a second scene → `+2` (distinct object, no dedup); destroy inner → `+1`; destroy outer
  → baseline. Teeth (win-debug, real GPU + DXR): neutralising the TLAS mint fails 1 case / 3 assertions → restored, TU
  touched, rebuilt. (There is no AS cache, so the `+2` pins one-identity-per-built-scene — not a dedup-policy choice as
  in the pointer/content-keyed program caches.)
- **The 3d DXR test needed NO edit (feared coupling did not exist).** It captures `res0` AFTER `rt.build_scene(...)`, so
  the scene's +1 is already in the baseline; its `Resource == res0` check only asserts `trace_rays` mints no Resource
  (the SBT is named, not minted) — still a genuine oracle, unaffected by 4b. (The batch-4 "forward note" and the
  batch-3d "(until 4b)" qualifier are corrected above.)
- **(4c) compute `Impl::ts_readback` — CLASSIFIED as context infrastructure, deliberately NOT minted.** It is a
  context-owned GPU-timestamp READBACK scratch buffer (`dx12_compute_context.cpp` ~292), created only if
  `CreateQueryHeap`/timestamp setup succeeds and used purely for internal timing — the SAME class as the batch-3c shared
  compute root signatures (`m_kernel_root`/`m_rt_root`/`m_sampled_kernel_root`), which the sweep also left unminted as
  infrastructure. It is not a Cerid-visible resource object, and there is no public `ts_ok` handle to gate an honest
  `+1` teeth assertion on. Consistent outcome: no wiring; recorded here. (If the design later demands every native
  object carry an identity, this is the one compute site to revisit.)
- **Lanes:** win-debug full dx12 exe **10373/190** (+7/+1 = the AS resource test; no regression); win-shipping
  (`/O2 /WX`) builds clean AND runs `[naming]` **121/11** on the real GPU + DXR. win-clang-cl-shipping (thin-LTO): not
  re-attempted (non-deterministic lld-link/LTO crash, ≤2 attempts, never a row gate). Row stays **Open** (DIAG.0-gated;
  DX12 Resource remaining: **batch 4d** = raster `dx12_raster_context.cpp` non-target buffers — first subtract the
  batch-1/2 texture/target sites by tracing each ComPtr to its OWNER; then `d2b-vk`, e, f, g).

## (d2b-dx12) batch 4d — raster non-target resources: one consistency fix + the completeness proof

Batch 4d traced EVERY remaining `CreateCommittedResource`/`CreatePlacedResource` site (AND every caller of the
`make_readback_buffer`/`make_upload_buffer` helpers — see the caller table below, which the site grep alone missed) in
`dx12_raster_context.cpp` to its owner, classified by the census line-431 rule read from the API side ("is it a
Cerid-visible logical object handed out by a public factory?"). Outcome: **all public-factory objects were already
wired, with ONE consistency gap fixed here — the storage buffer's owned readback sibling was unnamed** (a line-431
inconsistency dating to d2b-dx12-a, which predated `dx12_name_object`). Everything else is infrastructure or FG-transient.

- **Public-factory objects — ALREADY WIRED (owner ctor mints, siblings named):**
  - `IRasterTarget` (`create_color_target`/`_ms`/`_color_depth[_stencil]`/`_vrs`/`_visbuffer`) → `Dx12RasterTarget`
    (sites 1239/1289/1348/1360/1425/1440/1505) — batch 2, mint@418.
  - `IGBufferTarget` (`create_gbuffer_target`) → `Dx12GBufferTarget` (site 5665 creates `tex[]`, handed to the ctor) —
    batch 2, mint@970.
  - `ITexture` (`create_texture*`) → sites 4779/4865/4955/5067 — batch 1, mints@4823/4921/5024/5108.
  - `IStorageBuffer` (`create_storage_buffer`) → primary buf site 3297, mint@3323 (d2b-dx12-a); **batch 4d names its
    owned readback sibling** with the same identity (`dx12_name_object(readback, id, "dx12-storage-readback")`) — parity
    with the target/gbuffer readbacks (previously the lone unnamed owned sibling in the sweep). Named because it is a
    PERSISTENT member of the wrapper (moved in, `Unmap`'d in `~Dx12StorageBuffer`); the `upload` staging at 3304 is a
    transient LOCAL (dropped after `submit_and_wait`) and is correctly NOT named — the owned-vs-local discriminator.
  - `IRasterProgram` (`create_*_program`) → batch 3, mint@741 (+ compute-PSO caches 3c, DXR cache 3d).

- **Completeness proof — `ITexture` factory count reconciles (5 base decls → 4 DX12 overrides → 4 mints):** the DX12
  overrides are `create_texture` (mint@4823), `create_texture_from_mips` (@4921), `create_texture_dim` (@5024),
  `create_depth_texture` (@5108); the remaining base `ITexture` declarations are not overridden and return the base
  `nullptr` (no resource). One mint per constructing override.
- **Completeness proof — `make_readback_buffer`/`make_upload_buffer` caller table** (the helpers the site grep's bodies
  showed but whose callers it did not enumerate): target/gbuffer readbacks 1252/1299/1373/1452/5674 → named sibling of a
  `Dx12RasterTarget`/`Dx12GBufferTarget` (batch 2); texture/target upload staging 1517/4791/4876/4969/5077 → transient
  locals (uploaded then dropped); DXR SBT 2845 → named on the DXR pipeline identity (batch 3d); storage upload/readback
  3304/3311 → the buf is minted and the readback is NOW named (this batch); the upload is a transient local; the upload
  ring 6122/6190 (`b.ring`) → context-internal infrastructure (not minted). Every caller falls into wired / transient /
  infrastructure — no unwired public-factory owner. The single discriminator across the table is OWNED-vs-LOCAL: the
  products that are NAMED (DXR SBT 2845, storage readback 3311) are named because a persistent/cached object OWNS them
  (the `DxrPipe`, the `Dx12StorageBuffer`); the ones left unnamed are locals that die after their copy — one rule
  applied five times, not five judgments.
- **Context-internal infrastructure — NOT minted (the batch-3c shared-root-sig / 4c ts_readback rule):** `m_multi_args`
  + `m_multi_idx_args` (site 3858/3904 — ExecuteIndirect argument buffers, context members); `m_identity_ib` (3959 —
  the cached indexless-draw index buffer, `ensure_identity_index_buffer`); `m_ts_readback` (7078 — the raster GPU-
  timestamp readback, twin of the compute 4c site); the `make_readback_buffer`/`make_upload_buffer` anon-namespace
  helpers (74/93 — called for readback/upload staging). None is a Cerid-visible object; no handle refers to them.
- **Frame-graph-owned — NOT minted (batch-2 `IdentityMode::None` precedent, reinforced):** `p.node.resource` (7299) and
  the `CreatePlacedResource` `n.resource` (7961/7972) + `n.depth_resource` (8050). The placed resources are
  HEAP-SLOT-ALIASED transients (REN-38-B6: the same heap memory backs different logical resources across passes,
  `alias_allowed`) and are `.Reset()` on graph rebuild — a stable per-object identity on aliased memory would be
  semantically WRONG, not merely unnecessary. Confirmed by reading the lifetime, not assumed.
- **`IPresentSurface` (`create_present_surface`) — census-only:** `Dx12PresentSurface` owns NO app-created
  `ID3D12Resource`; its back-buffers are DXGI-owned and obtained by `GetBuffer` (a per-present borrow into a local
  ComPtr), and the swapchain itself is an `IDXGISwapChain3` (a DXGI object, not an `ID3D12Object` the registry can mint).
  It is also HWND-gated (the headless dx12 suite never constructs one). Nothing to attach.

**⇒ DX12 (d2b-dx12) native creation-site wiring is COMPLETE** for every Cerid-visible Resource and Program object.
Remaining d2b: `d2b-vk` (the Vulkan `vkSetDebugUtilsObjectNameEXT` prefix sweep).

### Finding — `ObjectKind::Pass` is defined but NOTHING mints it (a real DIAG.7a gap, NOT a d2b-dx12 gap)

The DIAG.7a acceptance clause names "stable object/**pass**/resource/program identities", and `ObjectKind::Pass` exists
in the (c) value type, but no Pass identity is ever minted. Passes are BACKEND-AGNOSTIC (`frame_graph.hpp`:
`add_pass(const char* name, …)` / `pass_name(u32)` — STRING names, no stable `PassId`+generation — the (a) census flag
at the top of this doc). Because a pass has NO native `ID3D12Object`/`VkObject` to `SetName`, Pass-identity wiring is
NOT a (d2b) creation-site batch (d2b = "mint()+SetName at the native creation sites") and does not block d2b-dx12/d2b-vk.
It is its own unit — mint a `PassId`+generation in the frame-graph `add_pass` and retire it when the graph is
destroyed/rebuilt — and must be scheduled before the (g) acceptance specimens (which correlate a hazard to a named
pass). Proposed sequencing: after `d2b-vk`, a backend-agnostic **(d2c) frame-graph Pass identity** unit, then (e)/(f)/(g).

**Lanes:** one-line naming change (storage readback). win-debug full dx12 exe **10373/190** (name-only, no count
change); teeth — turning the readback `dx12_name_object` into a `dx12_attach_identity` (the minted-sibling bug the
line-431 rule prevents) makes `create_storage_buffer` → `Resource +2` and fails the existing count test
(`test_dx12_validation.cpp:1312`), so the naming is guarded by that oracle → restored, TU touched, rebuilt. win-shipping
(`/O2 /WX`) builds clean AND runs `[naming]` **121/11**. Both validators PASS; row stays **Open** (DIAG.0-gated).

## (d2b-vk) started — the Vulkan identity adapter + the first wired group (storage buffer)

**Vulkan census (before this unit).** The Vulkan backend had ONE naming helper, `name_image` (`vulkan_raster_context.cpp`
264) — a human-readable labeler (`"%s %ux%ux%u"` via `vkSetDebugUtilsObjectNameEXT`, images only). NO identity mint, NO
`format_debug_name`, NO `name_buffer`/pipeline/AS naming, NO identity adapter, and the ctx did not even include
`identity_registry`. The untracked `test_diag_validation_identity.cpp` is the **(d1)** proof (a Cerid identity resolved
FROM a message's named objects), NOT a (d2b) creation-site wire — so d2b-vk was effectively unstarted. Vulkan device IS
present on this box (the `[geo]` device suite runs, 56 assertions, not skipped), so on-device count teeth are observable.

- **Adapter landed — `vulkan_identity_naming.{hpp,cpp}` (src-private, `crd::gpu::detail`), the Vulkan mirror of
  `dx12_identity_naming`.** Thin, because the registry + `format_debug_name` + `ObjectIdentity` are shared/backend-
  agnostic (that machinery was (d2a); Vulkan needs only the `vkSetDebugUtilsObjectNameEXT` adapter, so this is NOT a
  second (d2a)). `vk_attach_identity(VkDevice, VkObjectType, u64 handle, ObjectKind, site) → ObjectIdentity` = mint +
  name; `vk_name_object(...)` = name-only (siblings); `vk_detach_identity(id)` = retire. The mint happens BEFORE the
  debug-utils null guard, so retire-on-destroy holds even when `VK_EXT_debug_utils` is absent (no layer/ICD → PFN null →
  name silently no-ops). `VK_OBJECT_TYPE_UNKNOWN`/null handle are rejected (some ICDs refuse them). Auto-globbed into the
  lib by `crd_collect_sources`; no CMakeLists source-list edit needed.
- **First group wired — `VulkanStorageBuffer` (`create_storage_buffer`), mirroring DX12 d2b-dx12-a + 4d.** Mint on the
  device `VkBuffer` (`VK_OBJECT_TYPE_BUFFER`, "vk-storage") after the last early-return; name the owned host-visible
  readback `VkBuffer` with the SAME identity ("vk-storage-readback") — `+1` per logical buffer despite two natives
  (line-431). Identity stored on the wrapper, retired in `~VulkanStorageBuffer`. **Relocation handled:** the S7 defrag
  pass (`defragment_resources` → `swap_device_bundle`) replaces `m_buf` with a NEW `VkBuffer`; `swap_device_bundle`
  re-stamps the same identity onto the relocated buffer (no mint) so a message about it still resolves — the DxrPipe
  overwrite rule, applied to the reassignment path. Copy/move are deleted (no double-retire). **Honest gap:** the
  re-stamp is name-only and (Vulkan has no read-back) unobservable without a message — same class as the 4d storage
  readback; it is guarded only by the single-retire count contract (a `+1`-across-defrag assertion drives a real
  relocation via the public `vulkan_raster_defragment` and confirms the count/liveness are unchanged, catching a
  relocation that leaks or double-mints, but NOT a silently-dropped re-stamp). `vk_attach_identity` is `[[nodiscard]]`
  (the same discarded-mint guard that caught the DX12 4d teeth); the one caller assigns.
- **Oracles (Vulkan has NO debug-name read-back — `vkSetDebugUtilsObjectNameEXT` is write-only; the DX12 GetPrivateData
  round-trip does NOT port).** Two tests, both `[vulkan][identity][naming]` in `test_vulkan_context.cpp`: (i) ADAPTER
  (device-free) — `vk_attach_identity(VK_NULL_HANDLE, …)` still mints (`+1`, `alive`) with naming a forced no-op (proves
  mint-before-guard, constraint B), `vk_name_object` adds no mint, `vk_detach_identity` retires to baseline; (ii) WIRE
  (device-gated) — `create_storage_buffer` → `Resource +1` (not +2: readback named, not minted), a second buffer → `+2`
  (no dedup), each destroy → baseline. Teeth: neutralising the factory mint (invalid identity) fails the `+1`/`+2`
  deltas (3 assertions) → restored, TU touched, lane rebuilt. (The vulkan test CMakeLists gains the `src` include dir,
  mirroring the DX12 suite, to reach the src-private adapter header.)
- **`name_image` call-site classification (for the NEXT d2b-vk units, left unchanged this tick):** `vk-texture` (5588) →
  the image/`VulkanTexture` group (next: add `vk_attach_identity` beside it); `vk-bundle` (6844) → read owner lifetime
  before classifying; `fg-transient`/`fg-persist`/`fg-companion-depth` (7863/7922/8904) → frame-graph transients, get NO
  identity (the batch-2 `IdentityMode::None` rule). `name_image` stays a human-readable labeler; it is not rewritten.
- **Remaining d2b-vk (named follow-ups):** images/textures, other buffer wrappers, programs (`VkPipeline` +
  `VkPipelineLayout`), acceleration structures (`VkAccelerationStructureKHR` + backing `VkBuffer`) — each one identity
  per logical object, siblings named. Then the backend-agnostic **(d2c) frame-graph Pass identity**, then (e)/(f)/(g).
  Nothing on Vulkan is "complete".
- **Lanes:** win-debug `[identity][naming]` **14/2** (both new tests incl. the `+1`-across-defrag assertion; storage ran
  on-device, not skipped); teeth verified (neutralised factory mint → the count deltas fail). Regression over the paths
  this change touches: `[raster]` **1050/77** (storage/targets/defrag-heavy) and the RET-4 defrag/suballocator/relocation
  tests **160/3** — both green, so the additive mint/retire/re-stamp breaks nothing. win-shipping (`/O2 /WX`) builds clean
  AND runs `[identity][naming]` **14/2**. Container ban PASS (new `.cpp`). NOTE: the WHOLE ~500-test vulkan exe is
  pre-existingly slow (a bare run exceeds minutes with growing memory — observed before this change too), so the targeted
  `[raster]`+RET-4 regression stands in for it; a `[naming]`/`[raster]`-green row note is not gated on the full-exe run.
  Row stays **Open** (DIAG.0-gated).

## (d2b-vk) images group — VulkanTexture (ObjectKind::Resource)

The second d2b-vk group: `VulkanTexture` (a `VkImage` + `VkImageView` over an `ImageBundle`), one logical resource.

- **Wired (structural, batch-1 shape):** the mint is in the `VulkanTexture` CTOR — the single site all FIVE
  `create_texture*` factories (`create_texture`/`_mipped`/`_from_mips`/`_dim` + the `create_texture_dim` kinds) funnel
  through (the line-428 rule: minting is structural, not per-factory). Reconciled by grep: **5** `create_texture*`
  overrides (`create_texture`/`_mipped`/`_from_mips`/`_dim`/`_depth_texture`) → 5 owned `make_unique<VulkanTexture>`
  sites → the one ctor, plus exactly 1 FG `new VulkanTexture` site (opted out, below). Mint on the `VkImage`
  (`VK_OBJECT_TYPE_IMAGE`, site `"vk-texture WxHx1"`), name the `VkImageView` (`VK_OBJECT_TYPE_IMAGE_VIEW`) with the same
  id. **Label note:** `ImageBundle` carries no array-layer count (fields are image/mem/view/owner/alloc/format/width/
  height/mip_levels/samples/usage/aspect — no `layers`), so the human suffix hardcodes `x1`; a cube/array texture's
  label now reads `x1` where the old image-only `name_image` wrote the real layer count. The identity TOKEN is
  unaffected (it does not encode size); only the human tail of the debug name loses the layer number. **Pooled memory is NOT named:** `ImageBundle` uses a
  suballocator (`owner`/`alloc`; RET-4 "48 small images share pooled blocks"), so a shared block carries no single
  image's identity (the 3c shared-infrastructure rule). Retire in `~VulkanTexture`, placed BEFORE the `if (m_borrowed)
  return;` early-return so it runs for every owned texture (no-op on the invalid identity a borrowed one carries).
- **Frame-graph borrowed sampled views opt out.** A `VulkanTexture` is also constructed as an FG sampled transient (a
  view over an FG-owned image, then `set_borrowed()`); that one site passes the new `with_identity=false` ctor flag — an
  FG transient gets NO identity (the batch-2 rule), and since its dtor frees nothing a mint there would also leak. The
  ctor flag is the Vulkan analogue of the DX12 `Dx12RasterTarget` `IdentityMode::None` tag.
- **Redundant `name_image` removed** at the `create_texture_dim` site (it set an image-only human name that the ctor's
  identity name now supersedes). `name_image` itself and its FG callers (`fg-transient`/`fg-persist`/
  `fg-companion-depth`) are LEFT untouched — those are FG transients that stay human-readable with no identity. The
  `name_image` at `vk-bundle` (6858) is also still untouched and UNCLASSIFIED — resolve `out`'s owner when wiring
  targets/gbuffer (it is likely a target-bundle helper, so the next tick collides with it).
- **Relocation re-stamp.** The S7 defrag `m_live_textures` loop recreates the image AND its view; `swap_bundle` now
  re-stamps BOTH with the same identity (no mint), exactly as `swap_device_bundle` does for the storage buffer.
- **Test** `[vulkan][texture][identity][naming]`: `create_texture` → `Resource +1` (two natives, one identity), a second
  → `+2`, destroy inner → `+1`, destroy → baseline; then a mipped texture (TRANSFER_SRC ⇒ relocatable) → `+1`,
  `vulkan_raster_defragment` returns `relocations ≥ 1` (so the re-stamp path actually ran — not vacuous) with the count
  unchanged. Teeth: neutralising the ctor mint fails 5 assertions (the count deltas + the defrag delta) → restored, TU
  touched, lane rebuilt. Same honest gap as storage: the re-stamp is name-only and unobservable without a message; the
  count/liveness contract guards leak/double-mint, not a silently-dropped re-stamp.
- **Recorded for the NEXT tick (targets).** `~VulkanRasterTarget` begins `if (m_borrowed) { return; }` (line ~293) — a
  retire after it is SKIPPED for FG-borrowed targets, so put the retire BEFORE it. The FG-borrowed target construction
  sites are `new VulkanRasterTarget(...)` at ~7774 and ~8892 (both `set_borrowed()` after) — these must NOT mint (the
  same `with_identity=false`/`IdentityMode::None` opt-out). `VulkanRasterTarget` owns color/resolve/depth/readback/vrs
  bundles → ONE identity minted on the colour `VkImage`, the rest named (the batch-2 shape).
- **Lanes:** win-debug `[identity][naming]` **26/3** (adapter + storage + texture; texture ran on-device, defrag
  asserted non-vacuous); teeth verified; RET-4 defrag/relocation (incl. the mipped-texture relocation the re-stamp
  touches) **160/3**; the `[raster]` regression (targets/textures/storage-heavy) **1050/77**; win-shipping (`/O2 /WX`)
  builds clean AND runs `[identity][naming]` **26/3**. Container ban PASS. (The full ~500-test vulkan exe stays
  pre-existingly slow — the targeted `[raster]`+RET-4 subset stands in, as for the storage unit.) Row stays **Open**
  (DIAG.0-gated). Remaining d2b-vk: targets, gbuffer, other buffers, programs (`VkPipeline`), AS; then (d2c) Pass identity.

## (d2b-vk) targets group — VulkanRasterTarget (ObjectKind::Resource)

The third d2b-vk group: `VulkanRasterTarget` (colour + optional resolve/depth/vrs images + a readback buffer), one
logical resource — the batch-2 shape.

- **Wired (structural):** the mint is in the `VulkanRasterTarget` ctor. Reconciled by grep: **6** `create_color*`/
  `create_visbuffer*` factory overrides all funnel through ONE `make_target` helper → ONE `make_unique<VulkanRasterTarget>`
  site (7194), plus 3 FG `new` expressions across 2 sites (opted out). Mint on the colour `VkImage`, or the **depth image
  when colour is null** (the depth-only FG branch and any depth-only target) — site `"vk-target-color"` / `"vk-target-depth"`
  by which won. Every OWNED sibling is named with the same id: colour view, resolve image+view, depth image+view, and
  the readback `VkBuffer` (`VK_OBJECT_TYPE_BUFFER`); each `vk_name_object` no-ops on a null handle (an absent
  resolve/depth bundle). Retire in `~VulkanRasterTarget`, placed BEFORE the `if (m_borrowed) return;` early-return.
- **No cross-object depth collision.** The `make_target` helper CREATES all of colour/resolve/depth/readback itself
  (grep: every bundle is a local `create_image_bundle`/`create_readback`, never an imported `ITexture&`), so naming the
  depth image with the target's identity never overwrites another object's identity. (The FG `shared_depth_target` is a
  borrowed target — it opts out — so it is not a factory import.)
- **VRS sibling.** `m_vrs` is set post-construction via `set_vrs` (from `apply_vrs`), so it is named THERE with
  `m_identity` (image + view), not in the ctor — the DX12 batch-2 `set_vrs` pattern. No-op if the target is borrowed.
- **FG borrowed targets opt out.** The FG `new VulkanRasterTarget` construction — **3 `new` expressions across 2 call
  sites** (~7827, and a ternary at ~8945 whose two branches incl. the depth-only one both `new`) — passes
  `with_identity=false` (+ explicit `has_stencil=false`); they view FG-owned bundles, get no identity, and their dtor
  frees nothing (a mint would leak).
- **No relocation re-stamp.** Targets are NOT in a defrag registry (no `m_live_targets`; grep) — only textures and
  storage buffers relocate. So no `swap_*` re-stamp path for targets.
- **`vk-bundle` classified.** The `name_image(out.image, "vk-bundle", …)` at ~6881 is inside `create_image_bundle`, the
  SHARED image-creation helper every VkImage passes through — a generic fallback human label, OVERWRITTEN by the
  identity name for owned images (textures, now targets) and by the `fg-*` labels for transients. Not an identity, no
  collision; left as the pre-name default.
- **Test** `[vulkan][target][identity][naming]`: `create_color_target` → `Resource +1` (colour+view+readback, one id);
  `create_color_depth_target` → `+2` total (adds depth image+view, still one id for that target); `create_color_target_ms(4x)`
  → still one id (adds a resolve), sub-check skipped if 4x unsupported; `create_visbuffer_target` (the DISTINCT
  `R32_UINT` colour format) → still `+1`, proving the distinct-format factory shares the one make_target→ctor mint path;
  `create_color_vrs_target` → still `+1` (make_target mints, then `set_vrs` names the VRS siblings onto the same id;
  returns a plain colour target when VRS is unsupported, so the count holds either way); each destroy → back down; all
  out of scope → baseline. No defrag assertion (targets don't relocate). The VRS-sibling naming in `set_vrs` is name-only
  and its effect is unobservable (Vulkan has no name read-back), so the +1 is all the count can assert. Teeth:
  neutralising the ctor mint fails 5 assertions → restored, TU touched, lane rebuilt.
- **Lanes:** win-debug `[identity][naming]` **41/4** (adapter+storage+texture+target; 41 assertions, targets ran
  on-device); teeth verified; regression `[raster]` **1050/77** + RET-4 **160/3**; win-shipping (`/O2 /WX`) builds clean
  AND runs `[identity][naming]` **41/4**. Container ban PASS. Row stays **Open** (DIAG.0-gated). Remaining d2b-vk: gbuffer
  (`VulkanGBufferTarget`, `rb[]`/`kMaxGBuffer` arrays — its own tick), other buffers, programs (`VkPipeline`), AS; then
  (d2c) Pass identity.

## (d2b-vk) gbuffer group — VulkanGBufferTarget (ObjectKind::Resource)

The fourth d2b-vk group: `VulkanGBufferTarget` is a deferred MRT — **N colour planes** (`m_img[kMaxGBuffer]`, each
image+view, `kColorFormat`, colour-attachment usage; `2 ≤ N ≤ kMaxGBuffer=8`) plus **N pooled host-visible readback
buffers** (`m_rb[kMaxGBuffer]`). One logical resource.

- **Wired (structural):** the mint is in the `VulkanGBufferTarget` ctor. Reconciled by grep: **one** factory
  `create_gbuffer_target` (5924) → **one** `make_unique<VulkanGBufferTarget>` site (5946); **no `new VulkanGBufferTarget`
  anywhere** (no FG/borrowed construction, so no `with_identity` opt-out needed and no `m_borrowed` early-return in the
  dtor). Mint on plane 0's `VkImage` — site `"vk-gbuffer"`, `ObjectKind::Resource` — then NAME every owned sibling with
  the returned id: planes `1..N-1` image (`"vk-gbuffer-plane"`), all N views (`"vk-gbuffer-view"`), all N readback
  `VkBuffer`s (`"vk-gbuffer-readback"`). Retire in `~VulkanGBufferTarget` (first statement, no early-return to guard).
- **No cross-object / depth collision.** `create_gbuffer_target` CREATES every image bundle + readback itself in a loop
  (`create_image_bundle` + `create_readback`), importing nothing from another target — so naming a plane with the
  gbuffer's identity never overwrites another object's id. **There is NO depth plane** — the gbuffer is pure colour MRT
  (depth lives on a separate `VulkanRasterTarget`); so the depth-import concern that applied to targets does not arise
  here at all.
- **No relocation re-stamp.** `VulkanGBufferTarget` is not in any `m_live_*` defrag registry (grep) and deletes all
  move/copy ctors — it never relocates, so there is no `swap_*` re-stamp path.
- **`create_image_bundle` label overwrite.** `create_image_bundle` (6917) names every image it makes `"vk-bundle"`
  (`name_image` at 6934) — so each plane's *image* carries that generic label until the identity name overwrites it (the
  view gets no prior label, so the identity name is its first). Same classification as the other groups' bundle labels: a
  human debug label, not an identity; the identity name is the authoritative one.
- **Test** `[vulkan][gbuffer][identity][naming]`: `create_gbuffer_target(…, 2)` → `Resource +1` (2 planes + 2 readbacks,
  one id); nested `create_gbuffer_target(…, 4)` → `+2` total (4 planes + 4 readbacks, still ONE more id) — the n=2 vs n=4
  pair pins **per-logical-target, not per-plane**; inner destroy → back to `+1`; both out of scope → baseline. No defrag
  assertion (gbuffers don't relocate). Teeth: disabling the ctor mint block fails 3 of the 4 gbuffer count assertions
  (the +1, +2 and g4-retired-+1 checks; the baseline check trivially holds with no mint) and leaves the other 4 identity
  tests passing (they keep their own mints) → restored, TU touched, lane rebuilt.
- **Lanes:** win-debug `[identity][naming]` **50/5** (adapter+storage+texture+target+gbuffer; 50 assertions, gbuffer ran
  on-device); teeth verified (only the gbuffer test failed with the mint disabled); regression `[raster]` **1050/77** +
  RET-4 **160/3** (both match the prior tick — the ctor mint is additive, no behavioural change to the MRT draws/read_pixel);
  win-shipping (`/O2 /WX`) builds clean AND runs `[identity][naming]` **50/5**. Container ban PASS. Row stays **Open**
  (DIAG.0-gated). Remaining d2b-vk: other buffers, programs (`VkPipeline` + `VkPipelineLayout`), AS
  (`VkAccelerationStructureKHR`); then backend-agnostic (d2c) frame-graph Pass identity.

## (d2b-vk) buffers group — VkBuffer creation-site census + compute buffers (ObjectKind::Resource)

A whole-backend `VkBuffer` creation-site sweep (`grep vkCreateBuffer|create_buffer_bundle|create_readback|BufferBundle`
across all `engine/gpu/gpu-context-vulkan/src/*.cpp`) — every site classified, so the census shows the grep is
exhaustive. **This tick WIRES bucket 2 (compute buffers) only** and defers the rest, mirroring the d2b-dx12-a/4d split:
bucket 2 lives in its own TU (`vulkan_compute_context.cpp`) with its own `[compute]` tag, while the context-field buffers
(bucket 3) live in `vulkan_raster_context.cpp` and want a different (context-teardown) oracle — a clean split point.

| # | Bucket | Sites | Disposition |
|---|--------|-------|-------------|
| 1 | Already covered | storage buffer + readback (raster 1416/1427); target readback (7239/7240); gbuffer readbacks (5952/5957) | minted / named siblings in prior d2b-vk ticks |
| 1 | Geometry via storage | vertex-pull + index go through `IStorageBuffer` (`vk_buffer_of`, "VERTEX for vertex pulling" 1162) | **no separate vertex/index wrapper** — geometry buffers ARE `VulkanStorageBuffer` (already minted) |
| 2 | Wrapper-owned | **compute buffer** `BufferImpl` (`vulkan_compute_context.cpp` 49; one site `create_buffer`→make_unique 406) | **WIRED THIS TICK** — mint in ctor / retire in dtor |
| 3a | Context-field (upload ring) | `UploadBatch::ring` — `m_upload[kUploadBatches=2]`, each **lazily** created in `begin_upload_batch` (7041), grown by realloc in `upload_batched` (7107), retired in the context dtor loop (1290) | **WIRED THIS TICK** — identity in `UploadBatch` next to `ring`; mint on lazy creation, **re-stamp (same id) on grow**, retire in dtor |
| 3b | Context-field (multi-draw rings) | `m_multi_args` (`ensure_multi_args`) + `m_multi_idx_args` (`ensure_multi_idx_args`) — **lazy** (created on first non-indexed / indexed `draw_*_multi` during frame recording, fixed size, **create-once no grow**), retired in dtor | **WIRED THIS TICK** — one `ObjectIdentity` per ring, mint at each `ensure_*` success return, retire in dtor; no re-stamp (never grows) |
| 4 | Transient staging | storage-**upload** staging (1583, local `stg` in `upload_storage`, destroyed after the copy — NOT the named readback of tick 5); texture-upload staging (5410/5459/5567/5706/5772) | **NO identity, DONE** — created/copied/destroyed within one call (same class as DX12 4c `ts_readback`) |
| 5 | AS/RT/SBT | RT-context `make_buffer` (`vulkan_ray_tracing_context.cpp`): AS **backing** buffers (in the `owned` arena) + **scratch** (local, freed after build); SBT buffer (raster 2959, `m_rtp[].sbt`) | AS scenes **WIRED THIS TICK** (see AS section — one Resource id per scene on the TLAS; backing buffers are covered by that scene id, scratch is transient/no-id); **SBT + RT pipeline DEFERRED** to the RT-pipeline tick (raster TU, `ObjectKind::Program`) |
| 6 | FG-owned | FG buffer node (8112) | `fg-*` label, not an identity — leave |
| 7 | DGC per-execute scratch | `vulkan_dgc_context.cpp` `make_buf` (68), reached via `gbuf[8]`/`preproc` **locals in `dispatch_generated`** (Impl holds no persistent `DgcBuf`), freed via `free_buf` at call end | **NO identity, DONE** — per-execute scratch, not a logical app resource (same class as bucket 4) |

- **Wired (structural):** `BufferImpl` (compute) is a single `VkBuffer` wrapper (no readback sibling — one native
  object) with a destroying dtor. Include `"vulkan_identity_naming.hpp"`, add `ObjectIdentity m_identity{}`, mint in the
  ctor on `m_buffer` (`VK_OBJECT_TYPE_BUFFER`, site `"vk-compute-buffer"`, `ObjectKind::Resource`), retire in the dtor.
  One creation site (`create_buffer`→`make_unique<BufferImpl>` 406); no borrowed/FG path, so no opt-out. `noexcept` ctor,
  same shape as the other wrappers.
- **Test** `[vulkan][compute][buffer][identity][naming]`: `create_buffer` (GpuOnly) → `Resource +1` (one VkBuffer, one
  id); a second host-visible buffer → `+2`; inner destroy → `+1`; both out of scope → baseline. Teeth: disabling the ctor
  mint fails 3 of the 4 compute count assertions (the baseline holds with no mint) and leaves the other 5 identity tests
  passing → restored, TU touched, lane rebuilt.
- **Lanes:** win-debug `[identity][naming]` **56/6** (adapter+storage+texture+target+gbuffer+compute; ran on-device);
  teeth verified; regression `[compute]` **879/9** (the touched TU — matmul/MLP kernels still pass, the ctor mint is
  additive); win-shipping (`/O2 /WX`) builds clean AND runs `[identity][naming]` **56/6**. Container ban PASS. Row stays
  **Open** (DIAG.0-gated). Remaining d2b-vk: context-field buffers (bucket 3), programs (`VkPipeline` + `VkPipelineLayout`),
  AS (`VkAccelerationStructureKHR`); then backend-agnostic (d2c) frame-graph Pass identity.

## (d2b-vk) context-field buffers 3a — upload staging ring (ObjectKind::Resource)

Bucket 3 split into 3a (upload ring, THIS tick) and 3b (multi-draw arg rings, next). Reading the creation paths
corrected the earlier guess: **all bucket-3 buffers are lazy, not init-created**, so they need a trigger, not an init
count. The upload ring is cleanly triggerable via the public `begin_upload_batch`/`end_upload_batch`; the multi-draw
rings need a real multi-draw (frame recording), so they split off.

- **Wired (structural):** each `UploadBatch` slot (`m_upload[kUploadBatches=2]`) gains `ObjectIdentity identity{}` next
  to its `ring`. Mint on lazy creation in `begin_upload_batch` (7052, only in the `ring.buffer == VK_NULL_HANDLE`
  branch — so **once per ring**), site `"vk-upload-ring"`, `ObjectKind::Resource`. Retire in the context dtor loop
  (next to `destroy_buffer_bundle(m_device, b.ring)`) — a no-op for a slot that never opened a batch.
- **Grow is a re-stamp, not a remint.** `upload_batched` reallocs the ring bigger on overflow (7107). After the new
  `make_buffer`, the SAME id is `vk_name_object`-ed onto the bigger `VkBuffer` (identity stable across recreation — the
  DG12 contract), so `live_count` stays FLAT across a grow. The grow's failure branch (realloc fails → uploads fall back
  to sync) retires + clears the identity so a later `begin_upload_batch` mints a fresh one instead of leaking.
- **No confound with the existing identity tests.** The ring is lazy and created ONLY in `begin_upload_batch` (7041) or
  its grow (inside `upload_batched`, which requires `m_batch_open`); no other `[identity][naming]` test opens a batch. The
  storage defrag path was read (1470-1530): relocation is a direct `vkCmdCopyBuffer` + `swap_device_bundle` (device→
  device), NOT `upload_batched` — so it never touches the ring. Storage/texture/target/gbuffer baselines are unaffected —
  confirmed by all 7 cases green.
- **Baseline k0 = 0.** Raster init mints NO Resource (its only owned native object is a default sampler, not an
  `ObjectKind::Resource`) — asserted directly (`after init == before`).
- **Test** `[vulkan][buffer][identity][naming]` (context-field): `before` measured BEFORE `create_vulkan_raster_context`;
  after init → `before` (k0=0); begin/end → `+1` (slot 0 ring); begin/end → `+2` (slot 1 ring); a 3rd cycle → still `+2`
  (once-per-ring idempotence); a `> 8 MiB` `upload_storage` inside a batch grows the ring → still `+3` (the `+1` is the
  storage buffer; the grow re-stamps, adds nothing); storage out of scope → `+2`; context out of scope → baseline. Two
  teeth: (a) disabling the ring mint fails 6 assertions in this test, other 6 cases green; (b) turning the grow re-stamp
  into a REMINT fails 3 assertions (the grow `+3`, the storage-destroyed `+2`, and the baseline — the reminted-then-not-
  retired ids leak) — a FAILING run that simultaneously proves the grow path actually ran (the `+3` alone can't tell a
  re-stamp from a grow that never triggered). Both restored, TU touched, lane rebuilt (67/7).
- **Lanes:** win-debug `[identity][naming]` **67/7** (adapter+storage+texture+target+gbuffer+compute+upload-ring; ran
  on-device); teeth verified; regression `[raster]` **1050/77** + RET-4 **160/3** (the touched TU — upload/grow/defrag
  paths intact, the mint is additive); win-shipping (`/O2 /WX`) builds clean AND runs `[identity][naming]` **67/7**.
  Container ban PASS. Row stays **Open** (DIAG.0-gated). Remaining d2b-vk: 3b multi-draw arg rings, programs
  (`VkPipeline` + `VkPipelineLayout`), AS (`VkAccelerationStructureKHR`); then backend-agnostic (d2c) frame-graph Pass identity.

## (d2b-vk) context-field buffers 3b — multi-draw arg rings (ObjectKind::Resource)

The other half of bucket 3: `m_multi_args` (non-indexed indirect ring) and `m_multi_idx_args` (indexed indirect ring) —
two SEPARATE context-owned `VkBuffer`s, each lazily created on the first multi-draw of its kind and retired in the
context dtor. Bucket 3 is now fully wired.

- **Wired (structural):** each ring gains a context `ObjectIdentity` (`m_multi_args_id` / `m_multi_idx_args_id`). Mint at
  the `ensure_multi_args` / `ensure_multi_idx_args` **success `return true`** (after the alloc/bind/map guard — the failure
  branch destroys+nulls and returns before the mint, so it never holds a minted id and needs no retire there), sites
  `"vk-multi-args"` / `"vk-multi-idx-args"`, `ObjectKind::Resource`. Retire in the context dtor next to each
  `vkDestroyBuffer` (unconditional — a no-op if the ring was never created). **Create-once, fixed size** (`ensure_*`
  early-outs when the buffer exists) → NO grow → **no re-stamp** (unlike the 3a upload ring). `VkDeviceMemory` is not
  named (consistent with every group — only buffer/image/view handles carry identities).
- **Gating fact:** the verb that drives the indexed ring is `draw_storage_multi_indexed_depth` (calls
  `ensure_multi_idx_args`, **ungated**); the separate `multi_draw_indirect()`-gated path is a different verb, so the test
  uses the ungated one and needs no `+1`-vs-`+2` device conditional.
- **Test** `[vulkan][buffer][identity][naming]` (in `test_vulkan_frame_graph.cpp`, reusing the `Rig` + `build_vertex_pull_vs`
  + frame-graph multi-draw pattern the REN-38/39 gates already establish — identity tests are split across files by tag).
  `before` measured BEFORE the context; k0=0 asserted; the delta is read **immediately around each `fgraph->execute()`** so
  intervening `create_color_depth_target` (which mints its own +1) never pollutes it. A batched non-indexed multi-draw →
  args ring `+1` with `multi_batch_count()` `+1` (the "path actually ran" probe); a second non-indexed → `+0`
  (create-once); a batched indexed multi-draw → indexed ring `+1` with `multi_indexed_batch_count()` `+1`; context out of
  scope → baseline (both rings + all setup retired). Teeth: disabling BOTH `ensure_*` mints fails exactly the two mint-
  delta assertions (the create-once and baseline correctly still hold with no mint) and leaves the other 7 cases green →
  restored, TU touched, lane rebuilt.
- **Lanes:** win-debug `[identity][naming]` **90/8** (adapter+storage+texture+target+gbuffer+compute+upload-ring+multi-
  draw-rings; ran on-device); teeth verified (only the 3b test failed, 2 mint-delta assertions); regression `[multidraw]`
  **32/2** + `[raster]` **1050/77** + RET-4 **160/3** (the multi-draw functional gates drive the newly-minted rings —
  additive, no behavioural change); win-shipping (`/O2 /WX`) builds clean AND runs `[identity][naming]` **90/8**.
  Container ban PASS. **Bucket 3 fully wired.** Row stays **Open** (DIAG.0-gated). Remaining d2b-vk: programs
  (`VkPipeline` + `VkPipelineLayout`), AS (`VkAccelerationStructureKHR` + bucket-5 SBT/RT buffers); then backend-agnostic
  (d2c) frame-graph Pass identity.

## (d2b-vk) programs group — pipeline/shader-object census + compute pipelines (ObjectKind::Program)

A whole-backend pipeline/shader-object creation-site sweep. `ObjectKind::Program` is a SEPARATE kind from the buffer/
texture `Resource` count, so the oracle switches to `live_count(Program)` and the existing `Resource` baselines are
untouched. Reading each wrapper's DTOR (the ownership oracle) found **three distinct logical program layers** — this tick
WIRES the compute layer and splits the two raster layers off (mirroring the buffer 3a/3b split), and defers RT to the AS
tick.

| # | Layer / site | Owns (destroyed in its dtor) | Disposition |
|---|--------------|------------------------------|-------------|
| P1 | compute `PipelineImpl` (`vulkan_compute_context.cpp`; one site `create_pipeline_from_spirv`→make_unique) | `VkPipeline` + `VkPipelineLayout` + `VkDescriptorSetLayout` + `VkShaderModule` | **WIRED THIS TICK** — mint on the `VkPipeline`, name the 3 siblings |
| P2 | gpu-context `VulkanGpuProgramImpl` (`vulkan_context.cpp`; `create_program`) | ONE `VkShaderModule` (a compiled shader stage; also holds the SPIR-V blob) | **WIRED THIS TICK** — mint on the `VkShaderModule`, no siblings |
| P3 | raster `VulkanRasterProgram` (`vulkan_raster_context.cpp`; `create_raster_program` LINKS two P2 into new shader objects) | `VkShaderEXT` vs/fs (+task/tcs/tes) + `VkPipelineLayout` (`VK_EXT_shader_object`, no `VkPipeline`) | **WIRED THIS TICK** — mint on `m_vs` (the VS, or the **mesh shader object when `is_mesh`**), name fs/task/tcs/tes + layout |
| — | RT pipeline (raster TU `m_rtp[]`: `VkPipeline` + `sbt` + `sbt_mem`, `vkCreateRayTracingPipelinesKHR` ~2811) | RT `VkPipeline` + SBT `VkBuffer` + layout | **DEFERRED to the RT-pipeline tick** — a raster-TU context-field array (retired at context teardown), `ObjectKind::Program`; mint on the `VkPipeline`, name the SBT + layout siblings |
| — | DGC pipelines (`vulkan_dgc_context.cpp`: the `CompPipe` builder ~101-141; `prod`/`cons` are LOCALS in `dispatch_generated`, freed via `free_pipe` at 398-399) | per-execute `VkPipeline`+layout+module, freed at call end | **NO identity** — feature-internal scratch (read, not reasoned; same class as the DGC buffers, bucket 7) |
| — | `VkPipelineCache` (`Impl::pipeline_cache`) | context-lifetime driver ISA cache | **NOT a program** — context machinery, never minted/named |

- **Wired (P1, structural):** `PipelineImpl` is one logical program over four native objects. Include already present;
  add `ObjectIdentity m_identity{}`; mint in the ctor on `m_pipeline` (`VK_OBJECT_TYPE_PIPELINE`, `ObjectKind::Program`,
  site `"vk-compute-pipeline"`); name the pipeline layout / descriptor-set layout / shader module siblings; retire in the
  dtor. One creation site (`create_pipeline_from_spirv`→`make_unique<PipelineImpl>`); no wrapper cache/dedup (the
  `pipeline_cache` is the driver `VkPipelineCache`, ISA reuse only), so `+1` per create is correct. `noexcept` ctor.
- **Test** `[vulkan][compute][program][identity][naming]`: a trivial compute kernel compiled via `compile_glsl_to_spirv`;
  `create_pipeline_from_spirv` → `Program +1` (four natives, one id); a second from the same SPIR-V → `+2` (no wrapper
  cache); inner destroy → `+1`; both out of scope → baseline. Teeth: disabling the ctor mint fails 3 of the 4 program
  count assertions (the baseline holds with no mint) and leaves the other 8 identity cases green → restored, TU touched,
  lane rebuilt.
- **Lanes:** win-debug `[identity][naming]` **97/9** (…+compute-program; ran on-device); teeth verified (only the program
  test failed, 3 of 4 count assertions); regression `[compute]` **886/10** (the touched TU — the kernels still pass, the
  ctor mint is additive); win-shipping (`/O2 /WX`) builds clean AND runs `[identity][naming]` **97/9**. Container ban PASS.
  Row stays **Open** (DIAG.0-gated). Remaining d2b-vk: raster programs (P2 `VulkanGpuProgramImpl` + P3
  `VulkanRasterProgram`), AS (`VkAccelerationStructureKHR` + bucket-5 SBT/RT + RT pipeline); then backend-agnostic (d2c)
  frame-graph Pass identity.

## (d2b-vk) raster programs — P2 (compiled stages) + P3 (linked shader-object programs) (ObjectKind::Program)

The two raster program layers, completing the programs group.

- **P2 `VulkanGpuProgramImpl`** (`vulkan_context.cpp`, one site `create_program`→make_unique): owns one `VkShaderModule`
  (a compiled shader stage; it also keeps the SPIR-V blob). No wrapper cache/memo near `create_program` (grep) → `+1` per
  call. Mint in the ctor (after the SPIR-V copy) on `m_module` (`VK_OBJECT_TYPE_SHADER_MODULE`, `ObjectKind::Program`,
  site `"vk-shader-module"`), no siblings; retire in the dtor. Include `"vulkan_identity_naming.hpp"` added.
- **P3 `VulkanRasterProgram`** (`vulkan_raster_context.cpp`): **four factories** — `create_raster_program`,
  `create_mesh_program`, `create_mesh_task_program`, `create_tess_program` — all funnel through ONE
  `make_unique<VulkanRasterProgram>` (the ctor is the single structural mint site). Mint on `m_vs`
  (`VK_OBJECT_TYPE_SHADER_EXT`; `m_vs` is the VS, or the mesh shader object when `is_mesh` — untested here, the triangle
  program is `is_mesh=false`, so that stays a note), name the siblings `m_fs` / `m_task` / `m_tcs` / `m_tes` (unconditional
  — `vk_name_object` rejects null handles) + `m_layout` (`VK_OBJECT_TYPE_PIPELINE_LAYOUT`); retire in the dtor.
- **P3 does NOT own P2 — read, not reasoned.** `create_raster_program` (1761) reads the two P2 inputs' `vk_spirv()` and
  calls `m_api.create` (`vkCreateShadersEXT`) to make its OWN `VkShaderEXT`s + its own pipeline layout; the P2 objects stay
  the caller's. So a full raster program is **three** logical programs (2 stage modules + 1 linked program), and destroying
  the linked program leaves the stages alive.
- **Test** `[vulkan][program][identity][naming]` (in `test_vulkan_frame_graph.cpp`, reusing the `Rig` + `build_triangle_vs/fs`):
  `before` measured BEFORE the context; k0=0 (init compiles nothing); `create_program(vs)` → `+1`, `create_program(fs)` →
  `+2`, `create_raster_program` → `+3`; then **ordered resets** `prog.reset()` → `+2`, `fs.reset()` → `+1`, `vs.reset()` →
  baseline — which makes "P3 does not own P2" observable. Teeth: disabling BOTH the P2 and P3 mints in one run fails 5
  assertions in this test (the +1/+2/+3 and the two intermediate resets; the final baseline holds with no mints) and
  leaves the other 9 identity cases green → both restored, both TUs touched, lane rebuilt.
- **Lanes:** win-debug `[identity][naming]` **108/10** (…+raster-programs; ran on-device); teeth verified (only the
  raster-programs test failed, 5 assertions). The REAL P2/P3 regression is `[raster]` **1050/77** (every raster draw
  compiles P2 stages via `create_program` and links a P3 program via `create_raster_program`) + RET-4 **160/3**; the
  `create_program(KGraph,KEntry)` functional tests (D-008 C1-c / D-007 B3-c) and the `[identity]` suite also run the P2
  path. `[compute]` **886/10** covers the shared gpu-context init in the touched TU (the compute *kernels* use the P1
  `create_pipeline_from_spirv` path, not P2). win-shipping (`/O2 /WX`) builds clean AND runs `[identity][naming]`
  **108/10**. Container ban PASS. **Programs group fully wired (P1+P2+P3).** Row stays **Open**
  (DIAG.0-gated). Remaining d2b-vk: AS (`VkAccelerationStructureKHR` + bucket-5 SBT/RT + RT pipeline); then backend-
  agnostic (d2c) frame-graph Pass identity.

## (d2b-vk) acceleration structures — RtScene (ObjectKind::Resource)

The AS half of the AS/RT sweep. Reading the dtors corrected the DX12-inherited assumption: **the CONTEXT owns the natives**,
not the scene. `Impl` holds `owned` (every `DevBuffer`) + `owned_as` (every `VkAccelerationStructureKHR`) + `owned_mm`,
all destroyed in the context dtor; `RtSceneImpl` is a thin non-owning `{tlas, blas}` view. Scratch buffers are local and
freed after each build (transient). So this mirrors DX12 tick-3's SHAPE (one id on the TLAS, BLAS named as sibling) even
though the ownership differs: the identity tracks the scene HANDLE's lifetime (retired in `~RtSceneImpl`), while the native
AS lives in the context arena until teardown.

- **Wired (structural):** `RtSceneImpl` gains `ObjectIdentity m_identity{}` + `~RtSceneImpl() { detail::vk_detach_identity(m_identity); }`
  (retire only — it owns no native). A `stamp_scene_identity(device, scene)` helper mints on `scene.tlas`
  (`VK_OBJECT_TYPE_ACCELERATION_STRUCTURE_KHR`, `ObjectKind::Resource`, site `"vk-rt-tlas"`) and names `scene.blas`
  (`"vk-rt-blas"` — skipped for the cluster path where `blas == VK_NULL_HANDLE`, address-referenced). Called before each
  `return scene` in all **4 concrete builders** (`build_scene_instanced` [also the `build_scene` / `build_scene_alpha` /
  `build_scene_scalable` delegates], `build_scene_omm`, `build_scene_curves`, `build_scene_clusters`). One identity per
  logical scene regardless of the TLAS + BLAS + backing buffers it references.
- **Backing / scratch buffers.** AS backing buffers are **un-nameable per-scene by construction**: `make_buffer` COPIES
  each `DevBuffer` into the context `owned` arena and the local `DevBuffer` goes out of scope at the end of the builder, so
  there is no per-scene handle left to name after the build. They are covered by the scene's one identity (the logical AS),
  consistent with DX12 tick-3 (which named only the BLAS as a sibling, not the AS backing resources). Scratch is local,
  freed after the build → transient, no id.
- **Every stamp site is post-TLAS-success** (verified by reading): each builder `return nullptr`s on a failed
  `build_as`/`create_as` for the TLAS before reaching `stamp_scene_identity`, so `scene.tlas` is never null at the mint
  (`vk_attach_identity` rejects a null handle). **No builder takes an external BLAS** — every `blas_addr` is derived from
  the builder's own `scene->blas` (the cluster path from its own in-place cluster BLAS), so naming the BLAS with the
  scene's id never touches another scene's object.
- **Test** `[vulkan][rt][identity][naming]`: gated on `vk->ray_query()` + `rt.valid()` (this box has a real RT device, so it
  RUNS). `build_scene` → `Resource +1` (TLAS+BLAS, one id); a second scene → `+2`; inner handle out of scope → `+1`; both
  out of scope → baseline. Teeth: disabling the scene mint fails 3 of the 4 count assertions (the baseline holds with no
  mint) and leaves the other 10 identity cases green → restored, TU touched, lane rebuilt.
- **Lanes:** win-debug `[identity][naming]` **114/11** (…+AS scenes; ran on-device); teeth verified (only the AS test
  failed, 3 of 4 count assertions); regression `[rt]` **1720/38** (the touched TU — the scene builders now mint, the
  functional RT traces still pass); win-shipping (`/O2 /WX`) builds clean AND runs `[identity][naming]` **114/11**.
  Container ban PASS. Row stays **Open** (DIAG.0-gated). Remaining d2b-vk: the RT PIPELINE (raster-TU `m_rtp[]` +
  SBT, `ObjectKind::Program`) — the last d2b-vk unit; then the d2b-vk completion reconciliation grep. Then backend-
  agnostic (d2c) frame-graph Pass identity.

## (d2b-vk) RT pipeline (ObjectKind::Program) + d2b-vk completion reconciliation

The RT-pipeline unit — the last user-facing native-attach object.

- **Wired (structural):** the raster-TU `m_rtp[]` cache (`RtPipe`, keyed by the trio's SPIR-V content hashes,
  lookup-or-create, cap `kKernelPsoCap`) gains `ObjectIdentity id{}`. On a cache MISS (a new pipeline built), mint on
  `out.pipeline` (`VK_OBJECT_TYPE_PIPELINE`, `ObjectKind::Program`, site `"vk-rt-pipeline"`) and name the SBT `out.sbt`
  (`VK_OBJECT_TYPE_BUFFER`, `"vk-rt-sbt"`) — the SBT is program-owned. The pipeline **layout is shared context-wide**
  (`m_rtp_pipe_layout`, destroyed once after the loop), so it is NOT named per-pipeline. Retire in the context dtor loop
  before `vkDestroyPipeline`. Cached create-once per key → no re-stamp.
- **Test** `[vulkan][rt][program][identity][naming]` (feature-gated on `supports_rt_pipeline()`; RUNS on this box):
  reuses the proven REN-38-A16 framecook trace path (scene + `build_rt_pipeline_trio` + `StateHost` + `kRtPipeGraph`).
  `after_setup` measured after the 3 stage programs mint (P2), so the delta isolates the pipeline; first trace → `Program +1`
  (pipeline created + cached); a second trace with the same trio → `+1` (cache hit, no remint); context teardown → baseline.
  Teeth: disabling the mint fails the 2 mint assertions and leaves the other 11 identity cases green → restored, rebuilt.
- **Lanes:** win-debug `[identity][naming]` **129/12** (ran on-device); teeth verified; regression `[ren38]` **851/43**
  (the RT-pipeline gate) + `[raster]` **1050/77** + RET-4 **160/3**; win-shipping (`/O2 /WX`) **129/12**. Container ban PASS.

### d2b-vk completion reconciliation (every native creation site → a bucket)

A `grep` of every `vkCreate*` native-object site across `engine/gpu/gpu-context-vulkan/src/*.cpp`, each mapped to a bucket:

| symbol | sites | bucket |
|--------|-------|--------|
| `vkCreateImage` | tex 5701 / bundle 6958 | **minted/named** (texture, target, gbuffer wrappers); FG 8002/8061/9041 → **FG-owned `fg-*`** |
| `vkCreateImageView` | tex 5724 / bundle 6995 → **named siblings**; FG 9068/9086/9097 → **FG**; RET-5 swapchain composite view 604 → **presentation machinery** (lazy per-swapchain-image, freed on resize; the swapchain image is presentation-engine-owned, not a Cerid Resource) |
| `vkCreateBuffer` | compute 408, multi-args 7465/7509, storage/readback 7190, RT SBT 2976 → **minted/named**; DGC 68 → **feature scratch**; FG 8161 → **FG**; RT-ctx 93 → **AS backing/scratch** (scene id / transient) |
| `vkCreateSampler` | raster 1262/1266/1280/1287 + cache 3119 | **context machinery — NOT a Resource** (a sampler is a fixed-function state token with NO ObjectKind — not Resource/Program/Pass; the trichotomy IS the rule, mirrors the 4 default samplers & 3a. Contrast the kernel-pipeline caches below, which build a VkComputePipeline over a user shader module → Program) |
| `vkCreateComputePipelines` | compute P1 537 → **minted (Program)**; DGC 140 → **feature scratch**; **raster kernel_pipeline 7409 / rt_kernel_pipeline 6337 / sampled_kernel_pipeline 6409 → minted (Program, P4 — this tick)**; RT-ctx compute helper 843 → **transient one-shot scratch** (created→used→destroyed in the same fn) |
| `vkCreateShadersEXT` | raster PFN 40/72 only (the shader objects it creates are the P3 raster program) → **minted (P3)** |
| `vkCreateShaderModule` | compute P1 465 / gpu-ctx P2 965 → **minted/named**; DGC 132 → **feature scratch**; RT-ctx 828/912 → **feature scratch** (baked into a one-shot pipeline that is destroyed in the same fn — no persistent object, nothing to name) |
| `vkCreateAccelerationStructureKHR` | RT-ctx PFN 51/230 (the AS built via `build_as`/`create_as`) → **minted (AS scene)** |
| `vkCreateRayTracingPipelinesKHR` | raster PFN 2787/2813 → **minted (RT pipeline, this tick)**; RT-ctx PFN 63/252 → the RT-ctx RT-pipeline path (`trace_rays_pipeline`, 898) creates the pipeline, traces via a one-shot command buffer, and destroys it in the SAME call (1020) → **transient one-shot scratch**, NOT minted |

**(P4) context-internal compute-kernel caches — WIRED (this tick); d2b-vk CLOSED.**
`kernel_pipeline` (7387), `rt_kernel_pipeline` (6320) and `sampled_kernel_pipeline` (6390) each build a
`VkComputePipeline` (VK_SHADER_STAGE_COMPUTE_BIT) over `vk_prog->vk_module()` — a **user CKIR shader module** (plain
kernel dispatch, RT ray-query dispatch, HZB-sampled readback). By the ObjectKind trichotomy that decides every other
row, an object that executes a user shader module is a **Program** (identical in kind to P1 `create_pipeline_from_spirv`,
and matching the DX12 backend, which already mints its equivalent `m_rt_pso[]` as Program). Each cache is a keyed
lookup-or-create (`_key[]` = the caller's `VkShaderModule`) with a matching dtor loop and NO mid-life flush, so each was
wired like P1: a parallel `ObjectIdentity _id[kKernelPsoCap]` array, mint on cache-MISS at the store point (after the
`vkCreateComputePipelines` success guard, before the `++n`), retire in the dtor loop. **No siblings** — the module
already carries the P2 identity (re-naming it under the pipeline id would double-identity a native object) and the
pipeline layout is per-cache shared, not a per-pipeline sibling. Test: a Compute frame-graph pass drives
`enc_dispatch` → `dispatch_kernel` → `kernel_pipeline`; baseline AFTER `create_program` (P2 excluded), Program +1 on
first dispatch, +0 on the same kernel (cache hit), +1 on a different kernel, baseline at context teardown; teeth
neutralize the mint → 3 count-delta assertions fail. `rt_kernel_pipeline`/`sampled_kernel_pipeline` are byte-identical
and wired the same way; reached only through the RT ray-query / HZB-readback lowering paths. COVERAGE (observed vs
by-symmetry): `kernel_pipeline` is directly observed (the new compute-dispatch test, +1/+0/+1 deltas + teeth);
`rt_kernel_pipeline` is observed by REN-38-A9 GATE, which ran in the `[ren38]` 851/43 lane (this box has ray_query, so
the ray-query dispatch does not skip); `sampled_kernel_pipeline`'s only driving gate is REN-40-G3 GATE (Vulkan) in the
scene-render suite, which is PRE-EXISTING-RED on this box at `ref_renderer.init_programs` (SceneRenderer shader init,
upstream of any dispatch). This was PROVEN independent of P4, not merely reasoned: neutralizing ALL THREE mints
(`ObjectIdentity{}`) and rebuilding + re-running `[occlusion][vulkan]` reproduced the IDENTICAL init_programs failure at
the same line — so the red is orthogonal to the additive mint. The sampled mint is therefore wired-by-symmetry with the
two exercised caches but not directly exercised this tick (no public/`enc_dispatch_sampled` driver exists to observe it
without replicating the lowering), and is recorded as such rather than claimed green. The sampler cache (3119) is EXCLUDED by the same rule — a sampler runs no user code and has no ObjectKind,
so it stays machinery. Both RT-CONTEXT pipeline paths (the 843 compute helper and `trace_rays_pipeline` at 898/962) are
**transient one-shot scratch** — each creates its pipeline, uses it in a single submission, and destroys it in the same
call — so neither is minted. **Every `vkCreate*Pipeline*` site is now bucketed with no gap: d2b-vk (the Vulkan native-attach identity sweep) is
WIRING-COMPLETE and reconciliation-clean. Coverage: kernel_pipeline + rt_kernel_pipeline directly observed;
sampled_kernel_pipeline wired-by-symmetry (its driving gate pre-existing-red upstream, proven orthogonal to the mint).**

---

## (d2c-vk) Frame-graph PASS identity — Vulkan record identity (this tick)

The backend-agnostic unit after d2b: a frame-graph pass gets a stable `ObjectKind::Pass` identity. Pass is the **third
index space** already present in `IdentityRegistry` (`m_slots[2]`, a `crd::containers::SlotMap<u8>` like Resource and
Program), so no registry change was needed — its SlotMap generation IS the "stable, cannot silently alias" property the
design (runtime-diagnostics.md:353) asks for.

**Reading (a), not (b).** A pass identity is RECORD-lifetime (mint at `add_pass`, retire when the pass record dies), not
a name-keyed id that must survive a graph rebuild. This is forced by two facts, not just preference: (1) the design text
says only "attach stable … pass IDs … to native object names and reload and destruction" — mint/retire, no name→id
cache; (2) `mint()` is a SlotMap **free-list** (retire recycles the slot, bumps the generation), so per-frame
`add_pass`/rebuild churn is bounded by peak live passes, never exhausted — exactly what reading (a) needs. Inventing a
name→id cache the design didn't ask for would be the wrong call.

**Wiring (Vulkan, `VulkanFrameGraph`).** A pass has NO native `VkObject` to `SetName` (its native "name" is a
command-buffer LABEL — see the deferred sub-unit below), so the mint is a PURE `identity_registry().mint(ObjectKind::Pass)`
on the `Pass` record in `add_pass` (no device call). The pass record is destroyed in TWO places, so it is retired in
both (identity goes where the destroy is): `reset()` — which framecook calls EVERY frame, `m_passes.clear()` (the
mid-life-flush that P4's kernel caches did NOT have) — and the graph dtor (passes added since the last reset). (P4's kernel caches had no such flush — nothing was ever
cleared mid-life — so this extra retire site is specific to the pass record, not a gap P4 missed.) A single
`retire_pass_identities()` helper loops `m_passes` and calls `detail::vk_detach_identity` (no-op-safe) at each site.
`build()`/`execute()` neither mint nor retire.

**One identity per record — verified, not assumed.** `Pass` is an aggregate (no user dtor/copy/move/assignment) whose
only members are two `Array<Access>` (themselves plain-value elements) plus the `ObjectIdentity`. So `m_passes.push_back`
reallocation copies the id value into the new storage and destroying the old element runs NO dtor (a discarded `Pass`
never calls `vk_detach_identity`) — the live vector element always carries the minted id, and `live_count` is unaffected
by growth. No pass record lives outside `m_passes` (REN-37.5 persistence is images/targets, never passes), so
`retire_pass_identities()` covers every pass exactly once.

**Oracle + teeth.** `live_count(ObjectKind::Pass)` (a separate index space from Resource/Program). Test
`[vulkan][frame-graph][pass][identity][naming]`: empty graph == before; `add_pass` ×2 → +1 then +2; `build()` +2;
`execute()` +2 (labels are transient — nothing minted in the hot loop); `reset()` → before (BOTH retired via the
flush path); a fresh `add_pass` → +1 (slot recycled); graph dtor → before. Teeth: neutralizing the mint fails exactly
the 5 positive-count assertions (the `== before` cases correctly still hold with no mint), 13 other cases green.

**Lanes:** win-debug + win-shipping `[identity][naming]` **152/14**; container-ban PASS; regression `[ren38]` **851/43**
(every FG test now mints+retires on add_pass/reset — count nets out, no shift). No test asserts a cross-kind live TOTAL
(registry `live_count` always takes a kind), so Pass mints shift nothing.

### (d2c-vk label) native command-buffer pass label — DONE (this tick)

`execute()` has exactly TWO pass-execution sites (the earlier "4+" counted loop heads): the ASYNC pass (records into `m_async_cmd`, the
compute command buffer, ~9281 pre-edit) and the GRAPHICS pass (records into `m_cmd`, the graphics slot buffer, ~9520
pre-edit; line numbers are historical — the wraps added lines). Each `p.fn(*this, p.user)` is now wrapped in a `detail::PassLabelScope` local labelling the SAME command buffer
that pass records into (async → `m_async_cmd`, graphics → `m_cmd`) — never a fixed CB. The CB is STABLE across the labelled region: `frame_rec_new_pass()` only resets per-pass state (sampler / raster state), not the buffer, and `execute()` never reassigns `m_cmd`/`m_async_cmd` inside the loop — so the buffer captured at the guard's ctor is the one recording when End fires (verified by reading, backing the same-CB claim). `vkCmdBegin/EndDebugUtilsLabelEXT`
(VK_EXT_debug_utils, the same extension as object naming) are PFN-loaded ONCE in the `VulkanFrameGraph` ctor (`m_begin_label_fn`
/ `m_end_label_fn`) — never per-pass-per-frame — and are best-effort no-ops when null, exactly like `vk_name_object`. The
label string reuses the shared `format_debug_name`, so a capture shows `[pass:N] <name>` beside the `[res:M] <site>` object
names; a null `p.name` (a raw `add_pass(nullptr)`) formats as `pass` (never dereferenced).

**Balance by construction, not by discipline.** `PassLabelScope` is RAII: the ctor calls Begin (and arms End) only if the
PFNs+CB are present and `format_debug_name` succeeds; the dtor calls End iff Begin ran. A `continue`/early-return inside the
labelled block therefore can never unbalance the label.

**Partial oracle (correction to last tick's "no oracle").** The label STRING has no read-back (write-only). But the
validation layer reports End-without-Begin, so the d2c-vk pass test now runs a `ValidationCapture` across `execute()` and
`CHECK(capture.error_count() == 0U)`. Balance-teeth: neutralizing the Begin call while keeping End armed FIRED the VUID
(the capture check failed) — which proves (a) the End PFN resolves on THIS box (End ran and the layer saw it; Begin
resolving is INFERRED, not teeth-proven — same extension, same `vkGetDeviceProcAddr` loader path, so Begin resolves iff
End does) and (b) the layer validates label balance. Note the RAII makes Begin-without-End impossible by construction
(End fires iff Begin ran, from the dtor), so the ONLY unbalance an oracle must catch is End-without-Begin — and that
direction is teeth-proven. Restored + rebuilt → green. Only the label STRING remains un-oracled (write-only).

**Lanes:** win-debug + win-shipping `[identity][naming]` **153/14** (+1 = the capture check; label mints nothing, so the
`live_count(Pass)` deltas are unchanged); container-ban PASS; `[ren38]` **851/43** (every FG test now emits balanced labels
in the hot loop — validation clean, no regression).

**Remaining d2c sub-unit:** **(d2c-dx12)** the DX12 `Dx12FrameGraph` analog — mint on the DX12 `add_pass` record + retire
on reset/dtor, then the PIX/`BeginEvent` label. Same record-identity shape, parallel to the Vulkan units.

**d2c-vk is COMPLETE** — record identity (mint + both retire paths + `live_count(Pass)` oracle + teeth) AND the native
command-buffer label (RAII-balanced + validation-capture partial oracle + fired balance-teeth). Row 080 stays Open
(DIAG.0-gated).

---

## (d2c-dx12) Frame-graph PASS identity — DX12 record identity (this tick); PIX label deferred

The DX12 mirror of d2c-vk. `Dx12FrameGraph` (dx12_raster_context.cpp) has the SAME shape as `VulkanFrameGraph` — verified,
not assumed: `add_pass` (`m_passes.push_back(p)`), `reset()` → `m_passes.clear()` (the every-frame mid-life flush), a dtor
that does NOT clear `m_passes`, and a `Pass` aggregate (`name` is `const char*`, no user dtor/copy/move) so the minted id
carries through `push_back` reallocation exactly as on Vulkan.

**Wiring (STEP A, record identity).** A pass has no `ID3D12Object` to `SetName`, so `add_pass` does a PURE
`identity_registry().mint(ObjectKind::Pass)` on the `Pass` record (no device call); a `retire_pass_identities()` helper
(`detail::dx12_detach_identity` per record) runs at BOTH `reset()` (before `m_passes.clear()`) and the graph dtor.
Identity goes where the destroy is. `build()` mints nothing (ASSERTED, below); `execute()` cannot mint by CONSTRUCTION — the sole `identity_registry().mint(ObjectKind::Pass)` call is in `add_pass` (grep-verified single site) and `execute()` never calls `add_pass`.

**Oracle + teeth.** `live_count(ObjectKind::Pass)`. Test `[dx12][frame-graph][pass][identity][naming]` (in
test_dx12_frame_graph.cpp, matching where the tests live): empty graph == before; `add_pass` ×2 → +1/+2; `build()` → +2
(build mints nothing — CPU-side ordering, no target/window); `reset()` → before (BOTH retired via the flush); a fresh
`add_pass` → +1 (slot recycled); graph dtor → before. Teeth: neutralizing the mint fails exactly the 4 positive-count
assertions (+1/+2/build/+1), 11 other cases green. The DX12 device IS present on this box —
the test RAN (not skipped, 131 assertions), so the oracle is exercised, not just built.

**Lanes:** win-debug + win-shipping `[identity][naming]` **133/12**; container-ban PASS; regression `[frame-graph]`
**654/34** (every DX12 FG test now mints+retires on add_pass/reset — count nets out, no shift).

**STEP B (PIX label) — BLOCKED on PIX availability (its own tick, once unblocked).** A DX12 pass label would be `ID3D12GraphicsCommandList::BeginEvent`/
`EndEvent`, which need PIX event-encoding metadata constants (`PIX_EVENT_UNICODE_VERSION`/`ANSI_VERSION`). PIX is NOT
vendored anywhere in the tree (`grep pix3.h|WinPixEventRuntime|PIXBeginEvent|PIX_EVENT|::BeginEvent` across engine/tests/
cmake → nothing), so emitting a correct label needs the header on disk or a vendored runtime — deferred rather than
guessing constants from memory. When taken: an RAII guard mirroring `PassLabelScope` over the graphics list the pass
records into. NOTE: the D3D12 debug layer does NOT report unbalanced PIX events the way VVL reports End-without-Begin, so
the Vulkan label's "partial oracle" likely does NOT transfer — the DX12 label's verification is expected to rest on
RAII-by-construction + reading, to be confirmed by running the balance-teeth and reporting what actually fires (not
assumed).

**d2c-dx12 record identity is COMPLETE** (mint + both retire paths + oracle + teeth, exercised on a real device). With
d2c-vk COMPLETE (record + label) and d2c-vk/dx12 record identity done, the only remaining d2c sub-unit is the DX12 PIX
label — BLOCKED on PIX availability (not vendored), not merely deferred: a future tick must not re-attempt it until PIX
is on disk. Row 080 stays Open (DIAG.0-gated).

---

## (e) Identity lifecycle coverage — the retired-provenance end-to-end proof (this tick)

(e) is a PROOF unit, not a wiring unit: the no-alias mechanism already exists (the `crd::containers::SlotMap` generation
bump, which `IdentityRegistry` inherits — `alive()` = `map(kind).contains(handle_of(id))`, and `contains` tests
`m_generation[h.index] == h.generation`, slot_map.hpp:69). The reads found the property already WELL covered by the d2a
tests — `test_diag_identity_registry.cpp`: mint/retire/alive, alias-proof slot reuse with a bumped generation (line 50:
same index, gen+1, `alive(old)==false` while the slot is live again), independent kind index spaces, concurrent-mint
uniqueness; `test_diag_object_identity.cpp`: encode/parse round-trip for every kind+boundary, and parse of a token quoted
INSIDE a longer driver message.

**The one gap: the end-to-end SEAM.** No test combined `parse()` with `registry.alive()` — the two halves were proven
separately, but the operative (e) claim the design states (runtime-diagnostics.md: "IDs cannot silently alias when a …
GPU object is reused"; "Preserve deleted generation metadata") — that a validation/debug-layer message QUOTING a freed
object's native name resolves, through parse → registry, to retired-not-alive — was untested. Added one end-to-end test
(`[gpu][diag][identity][registry][lifecycle]`, in test_diag_identity_registry.cpp, device-free): mint a Resource →
`format_debug_name` it → embed that name in a longer "VALIDATION: object … accessed after free" message → `parse` it
back and `alive`==true while live → retire → `parse` the SAME text again (the token is unchanged) and `alive`==false →
mint a NEW Resource that REUSES the freed slot with a bumped generation → `parse` the OLD message once more: it still
decodes to the old identity but `alive`==false (no silent alias), while the new object is the live one — end to end, not
merely at the value level.

**Teeth (decisive, restorable throwaway).** Dropping the generation term from `SlotMap::contains`
(`&& m_generation[h.index] == h.generation`, slot_map.hpp:69 — index-only match) failed BOTH the pre-existing alias-proof
registry test (line 60) AND the new end-to-end test (line 105), 6 other cases green; restored + rebuilt → green. So the
new test genuinely exercises the generation-bump no-alias primitive through the full parse→registry path, and would catch
an aliasing regression. (SlotMap is a foundation container, not DIAG code — the neutralization was a throwaway: only `crd-gpu-context-tests`
was rebuilt against the neutralized header and only `[registry]` was run against it, no other exe or lane saw it; reverted
immediately, grep-confirmed zero residue, never committed.)

**Lanes:** win-debug + win-shipping `[registry]` **64/8** (+1 case, +14 assertions over the prior 50/7); container-ban
PASS. Device-free (CPU-only) — no device lane needed. runtime-diagnostics.md:353 lists "reload" alongside creation and
destruction as an identity-attach point; at the registry level that is a fresh mint on a recycled slot (same index, bumped
generation), which the alias-proof + end-to-end tests both cover. A BACKEND reload case (`create_program` -> destroy ->
`create_program` yielding the same index with a bumped generation on a real device) would be a (g) acceptance specimen,
not a gap in (e).

**(e) is COMPLETE.** Remaining DIAG.7a sub-units: (f) per-mode activation, (g) acceptance specimens (plus the
PIX-blocked (d2c-dx12) label). Row 080 stays Open (DIAG.0-gated).

---

## (f) Per-mode validation activation — vendor-free surface + Vulkan wiring (this tick); DX12 next

**Framing correction.** Earlier census/ROADMAP notes (and an advisor plan) framed (f) as "gate the native identity
attach by a mode." Reading DIAG.7a in the design settled it otherwise: runtime-diagnostics.md:353-354 says *"Define actual
activation and unsupported reasons for core, synchronization and GPU-assisted modes without importing vendor types into
public modules."* (f) is per-mode VALIDATION activation, not an identity-naming gate. The identity mint/retire stays
unconditional (it is the shipping leak oracle); (f) governs which validation MODES are on and reports why a requested one
is not.

**(f-1) the vendor-free surface (validation.hpp).** `enum class ValidationMode { Core, Synchronization, GpuAssisted }`;
`enum class ValidationUnsupportedReason { None, NotRequested, LayerAbsent, ExtensionAbsent, FeatureAbsent,
BackendHasNoEquivalent, DeviceRejected }` (+ `to_string`); `struct ValidationActivation` (per-mode requested/active/reason
+ `is_active`/`is_requested`/`unsupported_reason` + `consistent()`). `GpuContextConfig` gains `enable_sync_validation` and
`enable_gpu_assisted_validation` — kept SEPARATE from `enable_validation` (which stays "Core"), so every existing caller is
byte-for-byte unchanged and the two costly modes default off. `IGpuContext::validation_activation()` is append-only with a
default (empty report) so a backend that has not wired (f) compiles unchanged. NO `Vk*`/`D3D12_*` in the header. Device-free
test (`[validation][vocab]`): `consistent()` accepts a well-formed mixed report and REJECTS each invariant violated in
isolation (active-but-not-requested / active-with-a-reason / not-requested-with-a-reason) — the surface's own teeth.

**(f-2) Vulkan wiring (vulkan_context.cpp).** Any requested mode enables `VK_LAYER_KHRONOS_validation`; sync/GPU-assisted
ride `VkValidationFeaturesEXT` chained on the instance `pNext` with `..._SYNCHRONIZATION_VALIDATION_EXT` /
`..._GPU_ASSISTED_EXT`. **The trap, found by running:** `VK_EXT_validation_features` is provided by the validation LAYER,
not the loader, so it does NOT appear in the null-layer `vkEnumerateInstanceExtensionProperties` — the first run reported
sync/GPU-assisted `extension-absent` (the device test's CHECK(sync active) FAILED — an inadvertent but genuine teeth); the
fix enumerates `VK_LAYER_KHRONOS_validation` explicitly. Activation is recorded honestly: Core/Sync after instance creation
(layer loaded == vkCreateInstance succeeded); GPU-assisted is confirmed against the device's `fragmentStoresAndAtomics` +
`vertexPipelineStoresAndAtomics` (the layer accepts the enable but only instruments if present *and enabled*) — active from
the *report* does not by itself prove instrumentation (that is the hazard specimen, (g)). On THIS box all three modes ACTIVE.

**(f-2 fix) enable the GPU-AV features on OUR device, not the layer's.** First device run logged
`vkCreateDevice(): Warning ... Forcing vertexPipelineStoresAndAtomics to VK_TRUE` — proof the report said `GpuAssisted
active` while the feature it needs was only *available*, never *enabled* by us: VVL was force-enabling it behind our
`VkDeviceCreateInfo` (and warning). `active[2]` now also **enables** `fragmentStoresAndAtomics` +
`vertexPipelineStoresAndAtomics` in `enabled_feats` when GPU-AV is active, so the report reflects a feature WE turned on.
Verified by experiment: after the fix the "Forcing…" warning is GONE (count 0, win-debug AND win-shipping) and
`[validation]` stays **35/2**. Also simplified `active[0] = enable_validation` (the `&& any_validation` term was always
true when `enable_validation` is).

**Backward-compat (advisor blind spot).** `enable_validation` still means Core-only; sync/GPU-AV are opt-in, so existing
tests that set `enable_validation=true` and assert `clean()` are unaffected — `[ren38]` **851/43** unchanged (no
pre-existing hazard surfaced by a leaked sync-val default).

**Lanes:** win-debug + win-shipping `[validation][vocab]` **32/4** and `[validation]` (device) **35/2**; DX12
`[identity][naming]` **133/12** (the context.hpp change did not regress DX12 — the append-only default virtual); `[ren38]`
**851/43**; device-free `crd-gpu-context-tests` **227/26**; container-ban PASS.

**Toolchain note (corrected by experiment).** A build this tick died with `fatal error C1083: cannot open include file:
'span'`, which first looked like the transient C1001/lld-link ICE family. It was NOT: Visual Studio had been upgraded
2022→"18" (2026), so the hardcoded `...\2022\Community\vcvars64.bat` no longer exists — the `>nul`-masked "path not found"
left `INCLUDE` empty and the STL unreachable. Proven by probe (`cl`→9009, `INCLUDE` empty). Fixed by using
`...\18\Community\...\vcvars64.bat`. This is DISTINCT from last tick's C1001 in `<format>`, which resolved on retry while
builds were otherwise working (still recorded as transient) — a dead vcvars path leaves `INCLUDE` empty on *every* attempt
and no retry fixes it, so the two cannot share a root cause. After the path fix every lane builds clean.

**Remaining:** **(f-3) DX12 wiring** — Core = `EnableDebugLayer` (today's `enable_validation`); GpuAssisted =
`ID3D12Debug1::SetEnableGPUBasedValidation` (before device creation); Synchronization = `BackendHasNoEquivalent` (a CORRECT
report, D3D12 has no sync-validation mode). Then the hazard specimens that prove activation is REAL (a sync WAR/RAW hazard
seen only with Synchronization on; an OOB descriptor seen only with GpuAssisted on) fold into **(g) acceptance specimens**.

**(f-1)+(f-2) COMPLETE.** Row 080 stays Open (DIAG.0-gated).

## (f-3) DX12 per-mode validation activation — this tick

**Structural surprise (recorded before implementing).** DX12 is NOT the Vulkan mirror the plan assumed. Two findings from
orientation: (1) `create_dx12_gpu_context` took **only an allocator — no `GpuContextConfig`** (the whole DX12 context
ignored config; `enable_validation` had ZERO uses in `gpu-context-dx12/src`), unlike `create_vulkan_gpu_context(cfg)`. (2)
The debug layer is not enabled from the context: it is turned on by a **process-global registry** (`registry().enabled`
once-guard) driven by constructing a `Dx12ValidationCapture` before the context. The single `D3D12CreateDevice` seam is
`detail::Dx12DeviceScope::create` (in dx12_validation_capture.cpp, holding `devices_mutex`), called from ~10 sites.

**Design (minimal, append-only).** The factory gained a trailing `const GpuContextConfig& config = {}` — its `alloc` param
already had a default, so all 172 existing callers are source-unchanged. `Dx12DeviceScope` gained
`request_validation(core,sync,gpu_based)` (called by the context BEFORE `create()`) + `activation()`; `create()` applies the
process-global transition under `devices_mutex` and fills a `ValidationActivation`. `CaptureRegistry` gained
`gpu_based_validation` beside `enabled`. GBV is set **explicitly to the requested value** (not "TRUE only if requested"), so
it never leaks into a later context's device — the DX12 debug state is process-global (Catch2 = one process); verified by
`[identity][naming]` holding exactly **133/12** after the GBV test runs.

**Per-mode semantics.** Core: any requested mode ensures the debug layer via the same registry once-guard; **Core active is
OBSERVABLE** post-device — `ID3D12InfoQueue` QI on the device succeeds iff the debug layer is on for it (the post-hoc oracle
Vulkan lacks; the capture already depends on `ID3D12InfoQueue1`). GpuAssisted: `ID3D12Debug`→QI `ID3D12Debug1`→
`SetEnableGPUBasedValidation` pre-device; active == QI ok + enabled pre-device (ENABLED, not proven instrumenting — (g)
proves that). Synchronization: `BackendHasNoEquivalent` when requested (D3D12 has no sync-validation mode;
`SetEnableSynchronizedCommandQueueValidation` is a GBV ordering knob, NOT hazard detection — deliberately not mapped).
Reasons: `D3D12GetDebugInterface` fails→`LayerAbsent`; `ID3D12Debug1` QI fails→`ExtensionAbsent`.

**DX12 caveat.** `EnableDebugLayer` has no inverse and the once-guard is process-global: the report describes THIS context's
request/activation, not the process state. `validation_activation()` on `Dx12GpuContext` is append-only (mirrors Vulkan).

**Teeth (test-catchable).** Neutralized the `ID3D12Debug1` QI (`if (false && debug.As(&debug1))`) → `active[2]` false →
`CHECK(is_active(GpuAssisted))` FAILED (1/19, exit 42), `consistent()` still held (reason `ExtensionAbsent`), Core stayed
active. Restored → rebuilt the lane → **19/1** green. (The "report active without calling Set" teeth is NOT test-catchable —
active is derived from the same path; only a (g) specimen can prove instrumentation.)

**Also this tick:** added the missing hard `CHECK(is_active(GpuAssisted))` to the Vulkan device test (was coherence-only) →
`[validation]` **35→36/2**.

**Lanes (win-debug + win-shipping).** DX12 `[validation][gpu]` (new) **19/1**; DX12 `[validation]` **4102/26** (no
regression); DX12 `[identity][naming]` **133/12** (baseline, no GBV leak); Vulkan `[validation]` **36/2**. On THIS box Core +
GpuAssisted resolve ACTIVE, Synchronization `BackendHasNoEquivalent`. check-master-plan + check-repository + container-ban
PASS.

**(f-3) COMPLETE.** Row 080 stays Open (DIAG.0-gated). Remaining DIAG.7a: **(g) acceptance specimens** (hazard proofs that
activation is REAL on both backends); then the PIX-blocked (d2c-dx12) label (only if pix3.h lands). Then rows 081 (7b),
082 (7c).

## (g-1) Correlated-hazard acceptance — Vulkan Core resource route (this tick)

**Acceptance clause (verbatim, design 7a):** "intentional lifetime/state/descriptor hazard on a small isolated workload
produces a correlated error on each claimed route; ordinary valid consumers stay clean. Dropped validation messages are not
a clean run." The novel property (g) adds over (a)-(f) is **correlation** — the error must tie back to a Cerid
`ObjectIdentity`, not merely fire. The d1 test deliberately used a SYNTHETIC message and left the real-hazard proof to (g).

**g-1 specimen (`test_diag_validation_identity.cpp`, `[hazard]`).** A named Cerid buffer (minted + stamped via the REAL
production path `detail::vk_attach_identity`, `ObjectKind::Resource`) is hit with an oversized `vkCmdFillBuffer` (fill 64
into a 16-byte buffer). This is a **record-time VUID** — no queue submit, so the device is never removed (crash-safe; no
lifetime/TDR risk). The live validation layer fired the real error and the capture resolved it:
`vkCmdFillBuffer(): size (64) is greater...` with `identity.valid()==true`. Assertions: `dropped==0` (correlate over a
complete set), `error_count>=1`, a message whose `.identity == id`, and `alive(id)` still true. **Valid discriminator:** a
correctly-sized fill (16 into 16) on the SAME named buffer under a fresh capture yields NO message correlated to `id` —
"valid consumers stay clean". win-debug + win-shipping `[hazard]` **20/1**; regression `[validation],[identity]` **209/17**.

**Teeth.** Minted the identity but did NOT stamp it on the buffer (`identity_registry().mint` instead of
`vk_attach_identity`): the hazard still fires but the message carries no token → `find_by_identity(id)` returns null →
`REQUIRE(hit != nullptr)` FAILED (13/14, exit 42). Restored → rebuilt → **20/1**. So the test depends on the real
naming→parse→correlate path, not on any error merely being present.

**Hazard-class ledger (the clause enumerates lifetime/state/descriptor).** g-1 is a **usage-bounds** hazard (oversized
fill) — not lifetime/state/descriptor. Ledger: bounds ✓ (g-1); state/sync → (g-2); descriptor → (g-3); **lifetime →
unplanned / open design question.** A lifetime hazard (use after `vkDestroyBuffer`) cannot be correlated as written: once the
buffer is destroyed the layer forgets its debug name, so a dead-handle error carries no token. Correlating it needs
Cerid-side retired-handle provenance, which the (e) decision explicitly did NOT build (generation bump only). Surface before
(g-3); do not solve it in (g-2).

**(g-1) COMPLETE.** Row 080 stays Open (DIAG.0-gated). Remaining (g): **(g-2) Vulkan Sync WAR/RAW pair** (the true
mode-on/off discriminator — sync-val is per-instance, unlike DX12's process-global debug layer); **(g-3) GPU-AV OOB
descriptor** (last; the off-side is unsafe on real HW — design a benign OOB or record the off-side as deliberately not
exercised, with the reason). A DX12 correlated specimen (manual text-scan + `parse_after_prefix`, since DX12 messages retain
text but not a pre-parsed identity) may fold into (g-2)/(g-3) or 7b. Then the PIX-blocked (d2c-dx12) label (only if pix3.h
lands); then rows 081 (7b), 082 (7c).

## (g-2) Correlated-hazard acceptance — Vulkan Synchronization route (this tick)

The mode-ON/OFF discriminator the Core and DX12 routes cannot give: synchronization validation is per-INSTANCE, so both
sides run in one process. `test_diag_validation_identity.cpp` `[sync]` — three legs on a named Cerid buffer (a shared
`HazardRig` builds buffer + pool + cb; reused for the sync-on and sync-off contexts):

- **(a) hazard, sync ON:** two `vkCmdFillBuffer` to the same range with no barrier → syncval fires at RECORD time (no
  submit): `vkCmdFillBuffer(): WRITE_AFTER_WRITE hazard detected` with `identity.valid()==true`. Asserts `!dropped`,
  `mentions_waw`, `find_by_identity(id) != nullptr` (correlated to the buffer), `alive(id)`, AND
  `validation_activation().is_active(Synchronization)` — the REPORT agrees with the observed behaviour.
- **(b) valid, sync ON:** the same two writes with a `TRANSFER_WRITE→TRANSFER_WRITE` `vkCmdPipelineBarrier` between them →
  no WAW, `error_or_warning_count()==0` (clean, not merely "no hazard").
- **(c) mode-OFF discriminator:** a SECOND context, Core only (sync OFF), same unbarriered writes →
  `CHECK_FALSE(is_active(Synchronization))` and NO WAW, `error_or_warning_count()==0`. Sync off ⇒ no synchronization hazard.
  (Leg (c) being clean also confirms no environment-level syncval override on this box — `VK_LAYER_ENABLES`/vkconfig — which
  is what makes the discriminator meaningful; a box where it fires with sync off gets a diagnosis path, not a flaky test.)

**Correlation note.** The stored body carries only the class `WRITE_AFTER_WRITE` (the VUID name `SYNC-HAZARD-*` rides in
`pMessageIdName`, which the capture does not store); the buffer arrived via `pObjects` (the capture resolves objects-first),
the SAME route as g-1 — this VVL's syncval lists the buffer in the object list, so the d1 prose fallback remains unexercised
by a real message.

**Mechanism teeth (on the ENGINE, not the test).** (f-2) computes `active[1]` from `VK_EXT_validation_features` PRESENCE, not
from the chain actually running. Forced `n_vf = 0U` in `vulkan_context.cpp` (dropping the `VkValidationFeaturesEXT` chain) →
rebuilt lib+test → `[sync]` FAILED (`REQUIRE(mentions_waw)` — no hazard, exit 42) while the (f-2) `[validation]` REPORT test
still PASSED **36/2** (report unchanged). That is precisely the report-vs-reality gap (g) exists to close: (f) alone would
claim sync active when it is not running; only the (g-2) behavioural specimen catches it. Restored → rebuilt lane → **43/2**.

**Lanes.** win-debug + win-shipping `[hazard]` **43/2**; regression `[validation],[identity]` **232/18**. container-ban +
check-master-plan + check-repository PASS.

**Hazard-class ledger.** bounds ✓ (g-1); **state/sync ✓ (g-2)**; descriptor → (g-3); lifetime → open (a destroyed buffer
loses its debug name, so a dead-handle error carries no token; correlating it needs retired-handle provenance the (e)
decision did not build — surface before (g-3), do not slip-fix).

**(g-2) COMPLETE.** Row 080 stays Open (DIAG.0-gated). Remaining (g): **(g-3) GPU-AV OOB descriptor** — LAST; the off-side is
UNSAFE on real HW (an OOB read without GPU-AV can fault/TDR), so design a benign OOB (robustBufferAccess) or prove ON
fires+correlates and record the off-side deliberately-not-exercised with the reason (prove by experiment). Then the
lifetime-class open question; a DX12 correlated specimen (text-scan + `parse_after_prefix`) may fold into a later leg or 7b;
then the PIX-blocked (d2c-dx12) label (only if pix3.h lands); then rows 081 (7b), 082 (7c).

## (g-3) Correlated-hazard acceptance — Vulkan GPU-assisted (GPU-AV) descriptor-OOB route (this tick)

The descriptor-class hazard, and the first PROGRAM-route correlation. `test_diag_validation_identity.cpp` `[gpuav]`:

- **Reachability (read #1).** `VulkanComputeContext(VulkanGpuContext&, ...)` ADOPTS the context's instance/device, and GPU-AV
  is set at instance creation — so a compute context built on a `enable_gpu_assisted_validation=true` context runs on the
  instrumented instance. No structural change needed.
- **`robustBufferAccess` is NOT enabled (read #4)** → a buffer OOB is UB, so GPU-AV reports it (with robustness ON, a
  bound-buffer OOB is defined and GPU-AV skips it). It also means there is NO safe in-test OFF-side (see teeth).
- **Specimen.** A GLSL compute kernel writes `outb.data[idxb.idx[0]]` where `idx[0]` = 1,000,000 into a 4-uint buffer — a
  data-driven OOB the compiler cannot fold. Compiled via `compile_glsl_to_spirv`, dispatched through the high-level surface
  (`create_pipeline_from_spirv` → `begin`/`dispatch`/`submit_and_wait`). GPU-AV reports at the fence wait (a SUBMIT is
  required — a different risk class from g-1/g-2's record-time VUIDs). GPU-AV neutralizes the access, so with GPU-AV ON the
  submit is safe. Observed: `vkCmdDispatch(): (set = 0, binding = 0) ...` OOB, severity Error, `identity.valid()==true`.
- **Correlation = PROGRAM route (kind=1).** The identity resolved to Program (`kind=1`) via objects-first. Which
  Program-named object carried it is NOT observed -- the compute context stamps the same Program id on the pipeline, shader
  module, pipeline layout AND descriptor-set layout (vulkan_compute_context.cpp:98-104), and whether the buffer was also
  listed is not observed either (only that a Program-named object won, or the buffer was absent); the stored text is the body
  only. Still the FIRST program-route proof (g-1/g-2 were resource-route). Assert: `!dropped`, `error_or_warning_count>=1`, a
  record with a valid identity that is `alive()`; the compute objects' ids are internal (no accessor), so the assertion is
  "a valid, alive Cerid identity" + the printed `kind` records the route; combined with the teeth this is a complete proof.
- **Benign GPU-AV setting note.** The GPU-AV context logs `WARNING-Setting-Limit-Adjusted`: the layer force-adds
  `VkPhysicalDeviceTimelineSemaphoreFeatures{timelineSemaphore=TRUE}` at device creation -- a GPU-AV-internal need, active
  only in this debug mode (unlike the f-2 `vertexPipelineStoresAndAtomics` case, nothing the compute path itself requires). No action.

**Teeth (ENGINE; the OFF-side dispatch is deliberately NOT run).** Neutralized ONLY the `GPU_ASSISTED_EXT` enable in the
`VkValidationFeaturesEXT` chain (kept sync) → rebuilt lib+test → `[gpuav]` FAILED (`error_or_warning_count>=1` — no GPU-AV
message, exit 42) while the (f-2) `[validation]` report test still PASSED **36/2** (report reads `active[2]` from
`avail_feats`, not from instrumentation running). The report-vs-reality gap, again — (f) alone would claim GPU-AV active when
it is not instrumenting. Restored → rebuilt lane → **11/1**. The mode-OFF dispatch on a second context is a device-OOB with
`robustBufferAccess` off → UB / fault / TDR risk that would take the whole test exe down; it is DELIBERATELY NOT EXERCISED,
and the engine teeth is the mode-off proof instead.

**Lanes.** win-debug + win-shipping `[hazard]` **54/3** (g-1+g-2+g-3); regression `[validation],[identity]` **243/19**.
container-ban + check-master-plan + check-repository PASS.

### (g) ledgers — two axes, so (g) is NOT declared fully done after g-3

**Hazard-class** (the clause enumerates lifetime/state/descriptor): bounds ✓ (g-1) · state/sync ✓ (g-2) · **descriptor ✓
(g-3)** · **lifetime → OPEN** (a destroyed buffer loses its debug name, so a dead-handle error carries no token; correlating
it needs Cerid-side retired-handle provenance the (e) decision did NOT build — an open design question, not a slip-fix).

**Route** ("correlated error on each claimed route" = resource/program/pass × both backends, claimed by d2b/d2c):
resource ✓ (vk, g-1/g-2) · program ✓ (vk, g-3) · **pass → UNADDRESSED** (the capture does not parse `pCmdBufLabels`, so a
hazard inside a `PassLabelScope` cannot correlate to a Pass identity — a real gap) · **DX12 Core resource → UNADDRESSED** (a
DX12 correlated specimen needs a text-scan + `parse_after_prefix`, since `Dx12ValidationMessage` retains text but not a
pre-parsed identity; DX12 GPU-AV/DRED evidence is 7b's, but a DX12 Core correlated specimen is 7a's).

**(g-3) COMPLETE.** Row 080 stays Open (DIAG.0-gated). Remaining for (g)/7a: the pass-route correlation (needs
`pCmdBufLabels` parsing in the capture) and a DX12 Core correlated specimen; the lifetime-class open question; then the
PIX-blocked (d2c-dx12) label (only if pix3.h lands). Decide with the advisor which of these are 7a vs 7b before flipping.

## (g-4) Correlated-hazard acceptance — Vulkan PASS route (this tick)

The last unaddressed Vulkan route. A hazard recorded inside a `PassLabelScope` carries no named object, so d1's
objects-then-prose resolve could not reach the Pass identity. This tick extends the resolver and proves the route.

**Engine extension (`vulkan_validation_capture.cpp`, d1-family).** In the resolve loop, AFTER `pObjects` and BEFORE the
prose fallback, walk `callback_data->pCmdBufLabels` (then `pQueueLabels`) through the SAME `parse()`. **Walked FORWARD (index 0 first)** -- the nested specimen (leg d) FAILED with a backward walk
and PROVED by experiment that this VVL delivers the label stack most-recent-FIRST (index 0 = innermost), so a forward walk makes the innermost pass win over its enclosure. Both guarded on non-null so the d1
no-label path stays a no-op. `pQueueLabels` is wired the same way (four lines) so a future submit-time / GPU-AV message can
correlate to a pass without another engine touch — not exercised this tick. **Resolve precedence is now: objects →
command-buffer labels (innermost=index 0 first) -> queue labels → prose.** Recorded as the d1 contract's extension.

**Specimen (`test_diag_validation_identity.cpp` `[pass]`), one Pass id minted via `identity_registry().mint(Pass)`:**
- **(a) correlate:** an UNNAMED buffer + oversized `vkCmdFillBuffer` recorded INSIDE `PassLabelScope(pass_id)` → the message
  has no object token, so it resolves from the label → `identity.kind == Pass`, `alive(pass_id)`. Confirmed by run:
  `pass msg ident.valid=1 kind=2 text=vkCmdFillBuffer(): size (64) is greater` — VVL DOES populate `pCmdBufLabels` for a
  record-time VUID (the run-to-confirm fact).
- **(b) teeth:** the same fill OUTSIDE any scope → error still fires (`error_count>=1`) but the Pass id does NOT resolve
  (`find_by_identity(pass_id)==nullptr`) — the label is load-bearing (no code neutralization; scope vs no-scope is the
  discriminator).
- **(c) precedence:** a NAMED buffer inside the SAME scope → objects win → resolves to the buffer (`kind==Resource`), and
  the pass id does NOT win — turning "objects precede labels" into an asserted contract.
- **(d) nested precedence:** `PassLabelScope(outer)` enclosing `PassLabelScope(inner)`, unnamed buffer, fill inside the
  inner scope -> resolves to `inner_id`, NOT `outer_id`. This caught the ordering bug above and turns "innermost wins"
  into a tested contract.

**Regression (the resolver runs on every capture).** d1 synthetic `[identity]` unchanged — its object / prose / none cases
resolve identically (the none case stays invalid: an empty label array is a no-op branch). win-debug `[identity],[validation]`
**265/20**; win-debug + win-shipping `[hazard]` **76/4** (g-1..g-4). container-ban + check-master-plan + check-repository PASS.

### (g) route ledger update
resource ✓ (vk, g-1/g-2) · program ✓ (vk, g-3) · **pass ✓ (vk, g-4)** · **DX12 Core resource → UNADDRESSED** (7a's claimed
route on the 2nd backend: `Dx12ValidationMessage` retains text but not a pre-parsed identity → a manual text-scan +
`parse_after_prefix`; the g-1 shape — a Cerid-named buffer + a wrong-before-state barrier under `Dx12ValidationCapture`).
Hazard-class: bounds ✓ · state/sync ✓ · descriptor ✓ · **lifetime → OPEN** (dead handle loses its debug name; needs
retired-handle provenance the (e) decision did not build — the row's one caveat, DIAG.0 decides; not a slip-fix).

**Pass lifecycle seam ((e)/(g), on record).** A Pass id is retired at `reset()`/dtor and re-minted next frame with a
bumped generation (d2c); a message about a since-retired pass resolves to a `valid()` identity that `alive()` rejects --
the (e) no-alias property for the Pass kind. Not exercised here; recorded so the seam is on the books.

**(g-4) COMPLETE.** Row 080 stays Open (DIAG.0-gated). All Vulkan routes (resource/program/pass) now proven. Remaining 7a:
the **DX12 Core resource** correlated specimen (next tick) — after which (g) is complete against the clause as claimed and the
row is flippable pending only DIAG.0 + CI. DX12 Program/Pass + the PIX label are 7b; the lifetime class is the open caveat.

## (g-5) Correlated-hazard acceptance — DX12 Core resource route (this tick)

The second-backend claimed route. `test_dx12_validation.cpp` `[dx12][validation][identity][hazard]`, template = the `:126`
invalid-command-close test (capture-first ordering + `Dx12DeviceScope`).

**Correction to the plan: DX12 pre-parses identity too.** `Dx12ValidationMessage` already carries a pre-parsed `identity`
(d1 did BOTH backends), resolved from the full description BEFORE the `text[1024]` truncation. So correlation is
`msg.identity == id` — NOT the manual text-scan the plan assumed (both captures
pre-parse; the asymmetry that remains is the resolve INPUT -- Vulkan has `pObjects`/labels, DX12 has only the description,
which is why DX12 pass is mechanism-blocked below).

**Specimen (g-1 shape, `detail::dx12_attach_identity` gives a known id).** A Cerid-named committed buffer takes an invalid
`ResourceBarrier` (two transitions of subresource 0 in one call) → a record-time debug-layer state error naming the resource
(D3D12 message id 527; its multi-line body is truncated in the single-line test INFO, so the exact enum name is not asserted
from this run), `kind == Resource`. Three legs, scanning only records at index ≥ the count taken before each leg (the debug layer
is process-global, so creation chatter precedes the hazard): (a) named → a correlated ERROR names the id, `alive(id)`;
(b) an UNNAMED buffer, same hazard → the error still fires but nothing parses (the name is load-bearing — teeth, no code
neutralization); (c) a single valid transition on a named buffer → no correlated ERROR (valid stays clean). The layer is
process-global so there is no in-process off-side; "valid stays clean" is the discriminator (as f-3/g-1).

**Finding: DX12 emits MULTIPLE records naming a resource.** Alongside the 527 error, a non-error record (id 1008) also
carried the resource identity. So "correlated" must match the ERROR specifically (`identity == id && severity == Error`),
not the first record that happens to name the resource — otherwise leg (a)'s severity check picks the wrong record (it did,
first run) and leg (c) would false-fail on benign name-carrying info. Encoded in `dx12_new_error_correlates`.

**Lanes.** win-debug + win-shipping `[dx12][validation][identity][hazard]` **18/1**; regression `[validation]` **4120/27**,
`[identity][naming]` **133/12** (baseline). container-ban + check-master-plan + check-repository PASS.

### (g) route ledger — bucketed for the DIAG.0 judgement (row stays Open; DIAG.0 is the user's gate)
Per route × backend:
- **PROVEN:** resource (vk g-1/g-2, dx12 **g-5**) · program (vk g-3) · pass (vk g-4).
- **FEASIBLE-BUT-DEFERRED (triage, not a mechanism limit):** DX12 program under Core — the same `dx12_attach_identity`
  Program naming + a program-referencing Core error would correlate exactly as g-5 does; deferred to 7b as the DX12
  validation-depth row, not blocked.
- **MECHANISM-BLOCKED:** DX12 pass — no command-list label carries the Pass token without PIX (`pix3.h` absent); the d2c
  DX12 Pass label is already recorded blocked. 7b territory.
- **OPEN (design question, the row's one caveat):** lifetime class — a destroyed object loses its debug name, so a
  dead-handle error carries no token; correlating it needs retired-handle provenance the (e) decision did not build. DIAG.0
  decides whether this blocks; not a slip-fix.

**(g-5) COMPLETE.** With resource/program/pass proven on Vulkan and resource on DX12, every route 7a CLAIMS for the common
service (d2b/d2c) has a correlated-error proof or an explicit bucket above. Row 080 stays Open (DIAG.0-gated). Remaining tail
is 7b (DX12 program/pass depth) + the PIX label + the lifetime caveat — none a 7a correctness gap.
