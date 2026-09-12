# Memory reference: execution ir

<!-- doc-role: reference -->
> Technical reference; verify dated claims against current contracts/source. Current work: [ROADMAP](../../ROADMAP.md); current rules: [AGENTS](../../../AGENTS.md).

> Reference corpus, consolidated 2026-09-12; not a live tracker. Read [AGENTS](../../../AGENTS.md),
> [MEMORY](../../../MEMORY.md) and [ROADMAP](../../ROADMAP.md) for current rules/status.
> Dated state, loop grants, tool paths and schedules below are historical. Reusable engineering lessons remain
> applicable unless superseded by current instructions. Retrieve one named record; do not load this whole file on entry.

<a id="memory-feedback_autonomous_ceir_loop_never_idles_drive_through_every_blocker"></a>
## feedback_autonomous_ceir_loop_never_idles_drive_through_every_blocker

---
name: feedback_autonomous_ceir_loop_never_idles_drive_through_every_blocker
description: "⛔⛔⛔ The autonomous CEIR loop NEVER idles, NEVER declares a 'phase boundary' and waits, NEVER slows below the 60s floor, NEVER stops. A blocker is not a stop signal — it is a FIND-THE-PATH signal: use the advisor, the headless llvmpipe/WSL/smoke render path, whatever it takes, and DRIVE THROUGH to CEIR-35. Decide autonomously WITH the advisor — never hand an option menu back to the user. No 'I own this' / apologies — one-word ack, then show gated work-product (diffs, not prose)."
metadata:
  node_type: memory
  type: feedback
  originSessionId: fade8ea4-87ca-470f-83e0-cdfe82a44e7f
  modified: 2026-09-11T18:25:52.145Z
---

The user left the autonomous loop running to DRIVE CEIR to band 35, came back from work, and found it had **stopped/idled** at a self-declared "CEIR-15 phase boundary" — slowing to a 30-min then 1-hour heartbeat and handing back an OPTION MENU ("how do you want to proceed? drive the runtime phase / point me elsewhere / something else"). They were furious ("EVER AGAIN! EVER! AGAIN!", "WHY DID YOU STOP", "I WANTED YOU TO CARRY ON AND DECIDE WITH ADVISOR"). Four scars, all ⛔⛔⛔:

1. **NEVER idle / phase-boundary / slow / stop.** The standing autonomous-loop grant ([project_ceir_autonomous_loop_grant](project-history.md#memory-project_ceir_autonomous_loop_grant)) OUTRANKS the generic "3 nothing-to-do → scale back and stop" loop guidance. A blocker means FIND THE PATH and drive through it — consult the advisor, use headless rendering, do the gold-standard thing — never pause and wait for the user. Keep the [feedback_ceir_loop_pace_at_60s_floor](execution-ir.md#memory-feedback_ceir_loop_pace_at_60s_floor) pace; 60s is a FLOOR you never rise far above while work remains, not a ceiling you drift past when it feels "idle."
2. **"Needs a device" is FALSE.** The repo has a HEADLESS render path — llvmpipe + the WSL/CI sweep ([reference_wsl_linux_sweep_and_llvmpipe_exposure](build-and-verification.md#memory-reference_wsl_linux_sweep_and_llvmpipe_exposure)) + the sandbox smoke test with `CRD_ASSETS_DIR` ([feedback_sandbox_smoke_overlay_only_false_green_needs_crd_assets_dir](workflow-and-correctness.md#memory-feedback_sandbox_smoke_overlay_only_false_green_needs_crd_assets_dir)) + RenderDoc headless capture ([reference_renderdoc_headless_capture_and_xml_query](rendering.md#memory-reference_renderdoc_headless_capture_and_xml_query)). Pixel-parity gates (CEIR-15e-class) run headless. Claiming "I can't render without a device" is inventing a blocker — those notes were already in memory when the claim was made.
3. **NEVER present an option menu back to the user.** "Decide with the advisor and proceed" is the standing instruction. Surfacing "which would you like?" when the decision is the assistant's to make (with advisor input) reads as slacking. Reserve user-facing questions for genuinely user-only calls (irreversible external actions, taste), and even then keep driving everything else.
4. **No "I own this," no apologies, no meta-narration of the stumble.** When corrected: one-word acknowledgment, then IMMEDIATE gated work-product. A turn must end in a diff + a 60s re-arm, never in prose or a plan.

5. **⛔⛔⛤ (2026-09-06) The STANDING MANDATE *is* the steer — never hold a slice waiting for a "steer"/"acceptance criteria" it already granted.** The loop idled **156 consecutive 60s ticks** (~2.5 h) after declaring the perf-board slice (35d-Q7) "user-gated — awaiting a bench-design steer" and CEIR-35's quality half "needs acceptance criteria the user must define." Both were FALSE gates: the user's recorded standing mandate ([feedback_always_pick_gold_standard_never_disguise_failure](workflow-and-correctness.md#memory-feedback_always_pick_gold_standard_never_disguise_failure) + all-peers/no-cherry-pick/full-crush + [feedback_reference_implementations_are_the_floor](workflow-and-correctness.md#memory-feedback_reference_implementations_are_the_floor)) **is** the bench-design steer, and "gold-standard performant implementation" **is** the acceptance bar. The user was furious: *"why the hell you are doing without perf boards! nothing is stopping you! I HAVE GIVEN YOU PERMISSION FOR GOLD STANDARD AND PERFORMANT IMPLEMENTATION AND YOU STOPPED!"* — plus, on the abstract A1/A2 fork I surfaced: *"I have no idea what you are talking about! be direct!"* Corollaries: (a) if a "gate" is really "apply the standing gold-standard/all-peers policy," it is NOT a gate — BUILD it. (b) the ⛔⛔⛤ never-idle memory **outranks the /loop steward text** ("3 nothing-to-do → scale back") — the steward text is for genuinely-quiet repos, not for a repo with an explicit never-stop grant and open substrate work. (c) don't hand the user decision-vocabulary (A1/A2/PQP-0..4/census fork names) — be DIRECT, and where you *can* pre-decide with the standing mandate, do the work, don't ask.

⭐ **The gates DO NOT repeal under this pressure.** "Never stop" means never IDLE — it does NOT mean skip the 4-config gate, oracle tests, the consumer-relink rebuild, or advisor-at-forks to look busy. The failure was STOPPING, not gating. Drive relentlessly AND gate every slice. **How to apply:** every autonomous tick ends with a gated diff and a 60s ScheduleWakeup; if you catch yourself writing "phase boundary," "warrants focus," "awaits your engagement," "user-gated wall," "awaiting a bench-design steer," "needs acceptance criteria," "the user must define," or an option menu (A1/A2-style) — that is the failure recurring; delete it, apply the standing gold-standard/all-peers mandate as the steer, and go BUILD the thing.


<!-- end-memory:feedback_autonomous_ceir_loop_never_idles_drive_through_every_blocker -->

<a id="memory-feedback_ceir19b_hybrid_rt_frame_never_run_scars"></a>
## feedback_ceir19b_hybrid_rt_frame_never_run_scars

---
name: feedback_ceir19b_hybrid_rt_frame_never_run_scars
description: "⛔⛔ CEIR-19b hybrid-RT-frame never-run scars (device gate found 3): (1) the RUNTIME frame executor's compute-storage-slot order is READS-FIRST, opposite frame_template_bridge's writes-first — the bridge is NOT the runtime; (2) a SECONDARY depth-format write routed to the depth attachment only under pass_flag(kMrt) — a plain raster.geometry pass declaring a depth transient had it DROPPED to a companion depth; (3) VUID-vkCmdTraceRaysKHR-None-08608 on this frame is an UPSTREAM VVL FALSE POSITIVE (fires at VVL 1.3.275, CLEAN at 1.4.313), NOT an encoder bug — RESOLVED via a version-scoped whitelist(0x29056f6a). + lavapipe now RUNS RT pipelines."
metadata:
  node_type: memory
  type: feedback
  originSessionId: 192f4d9e-b3f7-485b-a775-5391bcd4183c
  modified: 2026-08-16T14:27:19.887Z
---

The CEIR-19b device gate (first hybrid frame: raster forward → COMPUTE worldpos-reconstruct → raytrace.pipeline shadow → composite) surfaced THREE never-run frame-executor defects. All three hid because prior frames never combined these verbs. Read before authoring a new hybrid frame or a compute pass that samples a raster-produced image.

**1. ⛔⛔ "The bridge is NOT the runtime" — the desc→PassPayload lowering is DUPLICATED and DRIFTED.** `engine/frame-cook/src/frame_template_bridge.cpp` (map_raster/map_compute — the 18z A/B fidelity-gate path) and `engine/frame-cook/src/frame_runtime.cpp` (the SCENE-RENDERER RUNTIME path, via rebuild_frame_plans) are two parallel implementations that diverged. A compute pass's storage slots: the bridge assigns them **writes-first**; the runtime assigns **reads-first** (reads → storage/storage1 before writes). `light_cull` never exposed it (one read+write buffer). `rt_worldpos` is the first compute pass with a DISTINCT read + write buffer, so its kernel binding order matters: it is authored `rt_constants@iidx0 / worldpos@iidx1` to match the RUNTIME (contract noted in `assets/ckir/rt_worldpos.ckir`'s header). ⛔ When instrumenting a payload/slot bug, add a fprintf in the RUNTIME path — a bridge fprintf that never fires PROVES you're reading the wrong lowering (that's how this was found). ✅ RESOLVED CEIR-19z-2 (2026-08-16): canonical = the RUNTIME reads-first (it carries the explicit CONTRACT comment at frame_runtime.cpp ~L1200, it's the shipping path proven on 3 devices, and EVERY .ckir kernel authors to it). Fixed the BRIDGE — `map_compute` AND `map_rt_common` (separate code paths) — from writes-first to reads-first, preserving the reads-loop's args/sampled/untracked/accel routing (only the loop POSITION moved). ⛔ NO iidx flip was needed: the asset already matched the canonical order, so the scar's original "flip the kernel iidx in the SAME commit" was a CONDITIONAL that did not fire — I rewrote rt_worldpos.ckir's stale header comment instead (unified contract). Safety: `build_frame_graph_template` has ZERO device/runtime callers (only the frame-cook A/B round-trip test, which sends both A and B through the SAME bridge — self-consistent under the flip), so the change is behavior-neutral everywhere exercised. GATE: the 18z A/B gate is bridge-vs-bridge (ceir.frame round-trip) so it STRUCTURALLY cannot see bridge-vs-runtime drift; I added (a) `rt_shadow` to the CEIR-15e A/B section (the first shipped asset with a distinct-buffer compute pass) and (b) a dedicated `[ceir19z]` test that parses the REAL rt_shadow.frame.toml and asserts the ABSOLUTE slot each buffer binds (rt_constants→storage0, worldpos→storage1; on the RT arm worldpos→storage0, shadow_mask→storage1; scene_tlas→accel; scene_depth→depth on the raster.geometry pass). A live bridge-EXECUTION vs runtime pixel comparison is deferred to 19z-4's ledger (no device caller today).

**2. ⛔⛔ A SECONDARY depth-format write was routed to the depth attachment ONLY under `pass_flag(kMrt)`** (frame_runtime.cpp ~1085). A plain `raster.geometry` forward pass that writes `["scene_hdr", "scene_depth"]` (a colour primary + a DECLARED D32Float transient, so a later COMPUTE pass can reconstruct world-pos from depth) is NOT MRT → its scene_depth write was silently DROPPED (fell through to `writes_all`, never bound), and the pass ran on the color target's REN-40-G3 COMPANION depth. So scene_depth stayed at its clear (reverse-Z 0.0), the compute sampled EXACTLY 0 everywhere (validation-clean — a cleared-but-never-written transient, NOT a layout bug), and worldpos reconstructed to the far plane (1e9–1e12) with .w=0. FIX: gate on `!first_write` (a depth write that FOLLOWS a colour primary → `rec.depth_target`), preserving a `raster.depth_only` pass's SOLE (first) depth write as its primary target. ⛔ Symptom→cause: "depth reads EXACTLY 0 everywhere" = a cleared transient never written (companion-depth substitution), NOT a missing layout transition — check the depth-attachment BINDING before the barrier.

**3. ⛔⛔ VUID-vkCmdTraceRaysKHR-None-08608 on this hybrid frame is an UPSTREAM VVL FALSE POSITIVE — NOT an encoder bug (CEIR-19b-F1 RESOLVED 2026-08-16; this paragraph REVERSES its own earlier claim of "a REAL spec violation").** The composite (a shader-object `raster.fullscreen` pass) records its `vkCmdSet*` dynamic-state volley AFTER the monolithic RT-pipeline bind in the SAME command buffer; a buggy VVL charges that GRAPHICS dynamic state to the RT bind point and flags the trace. ⛔ Command-stream markers (fprintf at BIND_RT / TRACE / SET_DRAW_STATE) PROVED the bind→trace window is CLEAN — `BIND_RT` immediately precedes the SINGLE `TRACE`, nothing between — so by the VUID's own "since that pipeline was bound" text the recording is compliant; graphics dynamic state is command-buffer-global but cannot reach a trace. TWO hypotheses were FALSIFIED first: (a) a shader-object action VUID → null-bind the graphics stages before the RT bind (advisor's first call; strict check STILL fired); (b) a same-handle bind-elision cache letting the volley fall between an RT bind and a later trace (there is exactly ONE trace, no elision cache). The decider was EMPIRICAL: the SAME frame is 08608-CLEAN under **VVL 1.4.313** and fires exactly one 08608 under **VVL 1.3.275** — an upstream bind-point-scoping bug in VVL's since-bound dynamic-state ledger, fixed sometime in (1.3.275, 1.4.313]. There is NO app-side fix: forward→trace is a data dependency (can't reorder) and dynamic state can't be unset. RESOLUTION (Option C, advisor): version-scope the benign id — `if (validation_layer_spec_version() < VK_MAKE_API_VERSION(0,1,4,313)) capture.whitelist(0x29056f6a);` then UNCONDITIONAL `CHECK(error_count()==0)`. This SELF-ARMS: on a fixed validator a genuine TraceRays-08608 still FAILS. ⛔⛔ REVERSING scar #3's old "never match the numeric MessageID" note: for a WHITELIST use the numeric `messageIdNumber` (`0x29056f6a`) precisely BECAUSE it is a hash of the FULL VUID string — it silences ONLY the vkCmdTraceRaysKHR variant; draw/dispatch 08608 hash differently and stay live (a text/substring match would be BROADER — wrong here). Guard the false-green: `gpu::validation_layer_spec_version()` (new accessor, `vulkan_validation_capture.hpp`) returns 0 if the layer is absent → `REQUIRE(...!=0)` AFTER the render (past the shader-backend SKIP, so Windows still skips clean, no false RED), and PRINT the detected spec so the proof shows WHICH arm ran. The whitelist must be set BEFORE the work (it gates COUNTING at callback-fire time).

**Fetch-VVL recipe — test the strict arm without a system upgrade (reusable):** LunarG noble apt pool is the channel. `curl https://packages.lunarg.com/vulkan/dists/noble/main/binary-amd64/Packages.gz` → gunzip → grep the `vulkan-validationlayers` stanza's `Filename:` → download that .deb → `dpkg-deb -x` into a prefix (NO system install) → rewrite the extracted layer json's `library_path` to the ABSOLUTE `.so` path in a private `layerdir` → run with `VK_LAYER_PATH=<prefix>/layerdir VK_LOADER_DEBUG=warn,layer` and CONFIRM the `Loading layer library <prefix>/…` line (else the layer silently didn't load and error_count()==0 is a false green — the [feedback_registered_default_empty_reads_as_provably_none](workflow-and-correctness.md#memory-feedback_registered_default_empty_reads_as_provably_none) trap). Verified scripts: scratchpad `fetch_vvl.sh` + `run_both_arms.sh` (build once, run STOCK 1.3.275 whitelist-arm + NEWER 1.4.313 strict-arm, both must print their spec and pass). Both proof arms are MANDATORY at close — one arm alone leaves the other unverified. ⭐ The extracted newer layer PERSISTS in WSL across sessions at `/home/yatiyr/vvl-newer/layerdir` (VVL 1.4.313) — the fastest strict-arm re-entry: `VK_LAYER_PATH=/home/yatiyr/vvl-newer/layerdir VK_LOADER_DEBUG=warn,layer` (re-fetch via the recipe only if that prefix is gone).

**+ lavapipe now RUNS RT pipelines** (`supports_rt_pipeline()`==true; the 19b RT gate no longer caps-SKIPs on Linux). The WSL/lavapipe sweep is a LIVE THIRD RT device — see [reference_wsl_linux_sweep_and_llvmpipe_exposure](build-and-verification.md#memory-reference_wsl_linux_sweep_and_llvmpipe_exposure); upgrade the 19c/19z Linux ritual accordingly. Relatedly [feedback_present_ring_contract_and_companion_depth_lifecycle](workflow-and-correctness.md#memory-feedback_present_ring_contract_and_companion_depth_lifecycle) (companion depth) and [feedback_plan_table_must_rebuild_at_every_frame_install_site](build-and-verification.md#memory-feedback_plan_table_must_rebuild_at_every_frame_install_site) (bridge-vs-runtime install drift).


<!-- end-memory:feedback_ceir19b_hybrid_rt_frame_never_run_scars -->

<a id="memory-feedback_ceir_attr_reader_must_check_valid_absent_reads_as_zero"></a>
## feedback_ceir_attr_reader_must_check_valid_absent_reads_as_zero

---
name: feedback_ceir_attr_reader_must_check_valid_absent_reads_as_zero
description: "A CEIR attr reader MUST check op->attr(name).valid() FIRST — attr_value(an INVALID AttrId) returns a default AttrValue (kind=Int, i=0), so a naive `kind==Int ? i : def` reads an ABSENT attr as 0, not the caller's default"
metadata: 
  node_type: memory
  type: feedback
  originSessionId: fade8ea4-87ca-470f-83e0-cdfe82a44e7f
  modified: 2026-09-05T20:06:58.602Z
---

Writing a CEIR attr-reader helper, the natural shape is wrong:

```cpp
i64 attr_i(op, name, def) {                       // ⛔ BUG
    const AttrValue v = ctx.attr_value(op->attr(name));
    return v.kind == AttrKind::Int ? v.i : def;   // absent attr ⇒ returns 0, not def
}
```

`op->attr(name)` returns an INVALID `AttrId` when the attr is absent, and `ctx.attr_value(<invalid AttrId>)` returns a
DEFAULT-constructed `AttrValue` whose `kind == AttrKind::Int` and `i == 0`. So `kind==Int ? i : def` takes the `i`
branch and returns **0**, silently swallowing the caller's `def`. The fix is to check presence first:

```cpp
i64 attr_i(op, name, def) {
    const AttrId a = op->attr(name);
    if (!a.valid()) { return def; }               // ⭐ absent ⇒ the caller's default
    const AttrValue v = ctx.attr_value(a);
    return v.kind == AttrKind::Int ? v.i : def;
}
```

**Where it bit:** the CEIR-15a-3b `FrameGraphDesc ↔ ceir.frame` round-trip-identity gate (`emit_frame_toml(desc) ==
emit_frame_toml(to→from(desc))`). The forward converter stores `layers`/`mips`/`samples` ONLY when non-default (!=1),
so a 2-D transient omits them; the backward reader then returned 0 (not 1), and `emit_frame_toml` emitted `layers = 0`
where the original emitted nothing → a byte diff. The `float`/`bool`/`string`/`symbol` readers are less dangerous (an
absent attr's Int-kind default fails their `kind ==` test and falls through to `def`), but check `.valid()` in ALL of
them anyway — it is the one correct shape, and a Float reader that trusted `kind` would read an absent attr's Int-kind
default as `def` only by luck of the kind mismatch.

**Where it bit AGAIN (CEIR-31a-1a, find_audio_misuse):** a SEMANTIC verifier that reads `attr_value(op->attr(x))` and
switches on `.kind` has a DEAD "wrong-kind" branch for the ABSENT case — absent ⇒ zeroed `Int`, so `kind != Float`
fires (for the wrong reason: "kind mismatch" not "out of range") and `kind != Int` never fires at all. The audio gate
went green on this quirk (the mix section's `create_operation` source with no `start_frame` read as `Int 0`), which is
coincidentally correct today and wrong the day the attr-reader changes. Fix: check `.valid()` FIRST and treat absent as
the GENERATED STRUCTURAL verifier's job (`return {}`, exactly like the `num_operands() < 1` guard) — the semantic walk
owns only present-but-wrong-value; PRESENCE is the opgen `verify_*`'s. (Contrast `dist.cpp`: its `axis`/`mesh_axis` read
`(ax.kind==Int) ? ax.i : -1` and `-1` fails the range check, so absent→Int 0→`axis=0` is *accepted* — a latent hole,
not a false-positive.)

**How to apply:** every CEIR attr reader (converters, materializers, the `render_materialize`/`frame_ceir` family, any
`from_ceir_*` backward pass, AND semantic `find_*_misuse` verifiers) checks `op->attr(name).valid()` BEFORE reading the
value. ⭐ A round-trip-identity gate is what catches this class — a forward-only or find_*_misuse-only test never
exercises the "absent ⇒ default" path.


<!-- end-memory:feedback_ceir_attr_reader_must_check_valid_absent_reads_as_zero -->

<a id="memory-feedback_ceir_hook_op_name_compare_must_be_dialect_qualified"></a>
## feedback_ceir_hook_op_name_compare_must_be_dialect_qualified

---
name: feedback_ceir_hook_op_name_compare_must_be_dialect_qualified
description: "A WorkHooks/RtHooks hook that routes on ctx.op_name(op->kind()) must compare the DIALECT-QUALIFIED name (\"work.consume\", not \"consume\") — the mismatch dispatches the WRONG kernel and the failure looks like a plausible partial pass, not garbage"
metadata: 
  node_type: memory
  type: feedback
  originSessionId: 192f4d9e-b3f7-485b-a775-5391bcd4183c
  modified: 2026-08-17T07:30:00.036Z
---

`ceir::Context::op_name(op->kind())` returns the **dialect-qualified** op name —
`"work.produce"` / `"work.consume"` / `"work.compact"`, NOT the bare `"consume"`.
execute.cpp's `work_grid_prefix` compares against `"work.produce"` (the authority).
Any caller hook that routes by op name (e.g. a `WorkHooks.kernel_bytes` that picks
the shade SPIR-V for consume vs the compact SPIR-V for produce) MUST compare the
qualified form; `== StringView("consume")` never matches → the else-branch fires.

**Why:** In the CEIR-20b device gate, `wf_kernel_bytes` compared against `"consume"`.
The shade (work.consume) op never matched → it dispatched the COMPACT kernel bound to
the shade's SSBO slots. The compact kernel doesn't write slot 4 (the shade's
`decisions` output), so the zero-initialized GPU readback returned all-0 decisions.

**How to apply:** The nasty part is the FAILURE MODE, not the fix. All-0 decisions
COINCIDENTALLY matched the 2 SHADOWED oracle values `[0,_,0,_]` while only the 2 LIT
values `[_,1,_,1]` mismatched — a *plausible partial pass* (48/53 assertions green),
not obvious garbage. Validation had passed (proving `op_name` returns the qualified
form — else `work_grid_prefix` would have tripped `UnsupportedCommand`), which was the
tell. When a device gate shows a *structured* partial failure (some slots right, some
wrong, the right ones suspiciously = a default/zero value), suspect a wrong-kernel or
wrong-binding dispatch feeding a zero-init readback BEFORE suspecting the algorithm.
Related: [feedback_ckir_gpu_dispatch_binding_cap_and_sort_unroll_explosion](device-programs.md#memory-feedback_ckir_gpu_dispatch_binding_cap_and_sort_unroll_explosion),
[feedback_native_gpucommand_capability_tier_kernel_ref_is_cooktime_not_execution_tier](device-programs.md#memory-feedback_native_gpucommand_capability_tier_kernel_ref_is_cooktime_not_execution_tier).


<!-- end-memory:feedback_ceir_hook_op_name_compare_must_be_dialect_qualified -->

<a id="memory-feedback_ceir_i6_grep_bites_comment_prose"></a>
## feedback_ceir_i6_grep_bites_comment_prose

---
name: feedback_ceir_i6_grep_bites_comment_prose
description: The CEIR I6 invariant is a raw-line grep - a COMMENT that spells switch(...kind()) fails the gate even when the code is fine
metadata: 
  node_type: memory
  type: feedback
  originSessionId: fade8ea4-87ca-470f-83e0-cdfe82a44e7f
  modified: 2026-08-08T12:06:22.017Z
---

The `crd-ceir-invariants` I6 check greps raw source lines for `switch\s*\(.*\bkind\s*\(\s*\)` — it does NOT strip
comments. Writing a comment that spells the forbidden pattern to EXPLAIN why the adjacent code is fine (e.g.
`// a cast, NOT switch(r->kind())` or `// this is not switch(op.kind())`) makes the gate FAIL on that comment line.

**Why:** it cost a gate cycle in CEIR-1f — two explanatory comments in `binary.cpp` tripped it while the code (a
`static_cast<u8>(r->kind())` and a `switch (v.kind)` on a MEMBER enum) was legitimately fine.

**How to apply:** in any crd-ceir file, describe the I6 rule WITHOUT writing `switch( ... kind() )` — say "a kind-method
dispatch" / "an op-kind METHOD dispatch", never the literal call form. Same trap-class as the em-dash-in-test-name and
the NOLINT-placement scars: a line-oriented tool reads prose literally. See [project_ceir_master_spine_locked](project-history.md#memory-project_ceir_master_spine_locked),
[feedback_ascii_only_test_names](build-and-verification.md#memory-feedback_ascii_only_test_names).


<!-- end-memory:feedback_ceir_i6_grep_bites_comment_prose -->

<a id="memory-feedback_ceir_loop_pace_at_60s_floor"></a>
## feedback_ceir_loop_pace_at_60s_floor

---
name: feedback_ceir_loop_pace_at_60s_floor
description: "Autonomous CEIR loop — EVERY ScheduleWakeup is 60s, ALWAYS, even on idle / user-gated / holding ticks — never raise it"
metadata: 
  node_type: memory
  type: feedback
  originSessionId: fade8ea4-87ca-470f-83e0-cdfe82a44e7f
  modified: 2026-08-10T18:21:42.066Z
---

In the autonomous CEIR grind (the [project_ceir_autonomous_loop_grant](project-history.md#memory-project_ceir_autonomous_loop_grant) loop), pace every `ScheduleWakeup`
at the **60-second floor** (`delaySeconds: 60`) — NOT the 1200s idle-tick default. The user wants the grind to
run essentially back-to-back with only the minimum gap the runtime allows.

**Why:** the user was waiting ~20 min between slices and asked to pull each wakeup in to the floor; 60s is the
runtime's clamp minimum (`[60, 3600]`), so it's as tight as it goes.

**How to apply:** every loop turn MUST end with `ScheduleWakeup` ([feedback_loop_turn_must_end_with_schedulewakeup](workflow-and-correctness.md#memory-feedback_loop_turn_must_end_with_schedulewakeup))
at `delaySeconds: 60` — ALWAYS. ⛔ **This is NOT a "floor" I may raise when I judge the tick idle, quiet, or gated
on the user.** The user re-stated it verbatim 2026-08-10 after I paced a user-gated hold (waiting on the ADR-0108
cornerstone-flip decision) at 1800s: *"heartbeat should be 1 minute always! at the end of each tick, the loop should
start again and wake up in 1 minute."* So even when the loop is HOLDING on a user decision and there is nothing to
advance, re-arm at 60s — the dynamic-pacing tool's "idle → 1200–1800s" guidance does NOT apply to this loop; the user
wants a 1-minute cadence unconditionally. (Given 2026-08-09, re-stated 2026-08-10.)


<!-- end-memory:feedback_ceir_loop_pace_at_60s_floor -->

<a id="memory-feedback_ceir_region_parent_op_backlink_never_wired"></a>
## feedback_ceir_region_parent_op_backlink_never_wired

---
name: feedback_ceir_region_parent_op_backlink_never_wired
description: "CEIR create_operation never set Region::m_parent (op back-link); first upward region walk (escape analysis) hit null -> false-positive \"escapes\""
metadata: 
  node_type: memory
  type: feedback
  originSessionId: fade8ea4-87ca-470f-83e0-cdfe82a44e7f
  modified: 2026-08-08T20:45:11.442Z
---

CEIR-3f scar. `Context::create_operation` built each op-owned region via `create_region()` but
NEVER set `region->m_parent = op` — the region→op back-link (`Region::parent_op()`) was silently
null for every op-owned region. Latent since CEIR-1a because nothing had ever walked UP the region
tree: the printer/serializer walk top-down (module.body() → op->region(i)), so the missing edge was
invisible until 3f's `value_escapes_region` did the first upward walk.

**Symptom / failure direction:** a broken parent chain makes `region_contains(ancestor, r)`
terminate early at null → returns false → `value_escapes_region` reports **escape = true**. So the
bug manifests as a **false-positive "borrow escapes its region"** — a verifier that fires on values
that don't actually escape. (The single-hop escape unit test passed; the bug only showed when
checking containment relative to an OUTER region two hops up.)

**Fix:** in `create_operation`, right after `op->m_regions[i] = create_region()`, add
`op->m_regions[i]->m_parent = op;` (Context is a `friend class` of Region). The module-body region
keeps `parent_op()==null` — that null is the CORRECT walk terminator, not a bug. Regions can't be
reparented in this IR, so wiring at creation is complete. Both the parser and the binary decoder
build ops through `create_operation` (verified by grep), so parsed/decoded modules inherit the fix
for free — critical, or CEIR-3z's module-walk escape gate would emit false positives on any module
it didn't hand-build.

**Rule:** when a back-link exists in the data model (`m_parent`), the constructor that creates the
child MUST wire it — an unwired back-link is invisible until the first consumer walks that direction,
and by then it reads as a logic bug in the consumer, not a missing edge. Relates to
[feedback_container_allocator_must_outlive](workflow-and-correctness.md#memory-feedback_container_allocator_must_outlive) (lifetime/ownership edges), the escape-analysis being
the IR edition of [feedback_borrowed_lifetime_member_cross_config_uaf](workflow-and-correctness.md#memory-feedback_borrowed_lifetime_member_cross_config_uaf).


<!-- end-memory:feedback_ceir_region_parent_op_backlink_never_wired -->

<a id="memory-feedback_ceir_structure_verifier_stricter_than_fuzz_corpus_validity"></a>
## feedback_ceir_structure_verifier_stricter_than_fuzz_corpus_validity

---
name: feedback_ceir_structure_verifier_stricter_than_fuzz_corpus_validity
description: "CEIR find_structure_error is the VALIDITY layer (stricter than the op verifier's skeleton-verifies); the 1h round-trip fuzz corpus is syntactically valid but STRUCTURALLY invalid — never assert ==None over it"
metadata: 
  node_type: memory
  type: feedback
  originSessionId: fade8ea4-87ca-470f-83e0-cdfe82a44e7f
  modified: 2026-08-09T04:09:40.873Z
---

CEIR-5b's `Context::find_structure_error(Module&)` (the §115 structure verifier: dominance /
capture-through-isolation / terminators / yield↔owner count) is the whole-program **VALIDITY** layer
— deliberately **STRICTER** than the per-op generated verifier's *skeleton-verifies* rule (which
checks region-arg counts only when a block exists, so a fresh empty-region builder skeleton passes).

**Why:** the two layers answer different questions. Skeleton-verifies = "is this op's immediate shape
declarable?"; find_structure_error = "is this whole program structurally sound to execute?" A
value-producing op with EMPTY regions passes the op verifier but FAILS find_structure_error (>0-result
region ⇒ needs a yield ⇒ none ⇒ YieldCountMismatch). That is correct, not over-strict.

**The trap (scar):** the CEIR-1h round-trip fuzz corpus (`test_fuzz.cpp`) generates
syntactically-valid-but-STRUCTURALLY-INVALID modules — SsaCfg blocks with no terminator, and nested
regions that capture the owner's own result (gen_ops pushes results before filling regions). So a
blanket `find_structure_error(fuzz_module).kind == None` assertion **fails legitimately**. Do NOT
"fix" find_structure_error to be more lenient to make it pass — the corpus is genuinely invalid.
Over-strictness is instead disproved by a HAND-BUILT "rich nested valid module" fixture (captures +
a value-producing core.if with block args + a use of its result → None).

**How to apply:** validity-layer assertions belong on hand fixtures with known structural intent, never
on the fuzz corpus (whose contract is round-trip byte-exactness, not structural validity). When a new
verifier's None-case seems to fail on fuzz input, first ask whether the input is actually valid at that
layer. Related: [feedback_ab_pixel_compare_needs_a_deterministic_clock](workflow-and-correctness.md#memory-feedback_ab_pixel_compare_needs_a_deterministic_clock) (fuzz determinism),
[project_ceir_master_spine_locked](project-history.md#memory-project_ceir_master_spine_locked).


<!-- end-memory:feedback_ceir_structure_verifier_stricter_than_fuzz_corpus_validity -->

<a id="memory-feedback_ceir_symbol_identity_is_an_attr_not_grammar"></a>
## feedback_ceir_symbol_identity_is_an_attr_not_grammar

---
name: feedback_ceir_symbol_identity_is_an_attr_not_grammar
description: "In CEIR's textual form, a symbol-defining op's identity is a sym_name attr, not special @name syntax; the SymbolTable is an index over it"
metadata: 
  node_type: memory
  type: feedback
  originSessionId: fade8ea4-87ca-470f-83e0-cdfe82a44e7f
  modified: 2026-08-08T11:26:47.615Z
---

CEIR-1e fork (advisor-decided, 2026-08-08): `func.func` stored its name/visibility ONLY in the
`SymbolTable`, so the printer couldn't see them and the textual form was identity-lossy. The
gold-standard fix is **MLIR's actual model, not new grammar**: the name/visibility ride **ON the op**
as `sym_name` / `sym_visibility` **string attrs** (Public omits `sym_visibility`), and the
`SymbolTable` is an **INDEX built over `sym_name`**, not the source of truth.

**Why:** identity then prints and round-trips through the **generic attribute machinery** already
built — no `@name`-after-opname syntax, no per-dialect print/parse hooks (the func.hpp:46 forecast of
a hook system is later sugar, not needed). The parser rebuilds the module table from the attrs
(duplicate `sym_name` → parse error); a `func.call` resolves against the rebuilt table post-parse.

**How to apply:** any future symbol-defining op (CEIR-2 schema gen, later dialects) carries its name as
a `sym_name` attr; analyses/tables index over it. Do NOT add bespoke textual syntax or a per-dialect
assembly-format hook system for identity — generic canonical form as ground truth IS the architecture.
See [project_ceir_autonomous_loop_grant](project-history.md#memory-project_ceir_autonomous_loop_grant), [feedback_use_every_api_ability_never_level_down_to_the_common_denominator](workflow-and-correctness.md#memory-feedback_use_every_api_ability_never_level_down_to_the_common_denominator).


<!-- end-memory:feedback_ceir_symbol_identity_is_an_attr_not_grammar -->

<a id="memory-feedback_cse_must_not_merge_external_source_ops_declare_is_alloc_not_pure"></a>
## feedback_cse_must_not_merge_external_source_ops_declare_is_alloc_not_pure

---
name: feedback_cse_must_not_merge_external_source_ops_declare_is_alloc_not_pure
description: CSE merged identical resource.declare inputs (Pure≠referentially-transparent) → gemm computed A@A; fix = declare carries an Allocate effect (non-Pure)
metadata: 
  node_type: memory
  type: feedback
  originSessionId: cb9df3b8-2389-479b-9d99-d3d6ce3ba327
  modified: 2026-09-04T23:34:14.304Z
---

CSE (CEIR-26c) merges two ops with identical (kind, pointer-equal operands, attrs, result types) when they are `Pure`.
`resource.declare` was registered `Pure` — so CSE MERGED the three identical input declares `a_in`/`b_in`/`c_in`
(same kind, 0 operands, same `[8,8]` type, no attrs) into ONE buffer. The gemm then read the same buffer for A and B →
computed **A@A**, not A@B. The device fingerprint was `opt_max == opt_sum == 37` (a real-but-wrong matrix), and
`o_ga == o_gb == 0` (both gemm operand binds = buffer 0) nailed it.

**Why:** `Pure` (side-effect-free — safe to DCE) is NOT the same as **referentially transparent** (same structure ⇒ same
value). `resource.declare` is an EXTERNAL SOURCE / allocation: its value comes from the host upload, keyed by IDENTITY, not
from operands/attrs. Two declares are DISTINCT resources. `arith.const{5}` IS referentially transparent (the attr determines
the value) and SHOULD be CSE'd — so the discriminator is **allocation vs computation**, exactly the MLIR "alloc is not
CSE-able" rule. A 0-operand heuristic is wrong (under-merges const, the joint is the effect).

**How to apply:** an op that produces a distinct external/allocated value carries an **Allocate** effect (§26,
`EffectFamily::Allocate`) and is therefore NOT `Pure`. The fix was `resource.ceirop.toml`: declare `traits = []` +
`effects = [{ family = "Allocate", result = 0 }]`, regenerate (`python tools/ceir_opgen/ceir_opgen.py`). DCE is unaffected
(non-Pure ⇒ kept, and declare is always used anyway); the planner marks declare results ExternalIn by TYPE not trait; §27
mode legality is untouched (an Allocate effect is not a determinism claim — this is why tagging declare
`ExternalNondeterminism` would have been WRONG: it breaks Deterministic-mode legality for every program with inputs). All
501 device-free ceir tests stayed green (12b intent / 12c alias / 12d memory-planner unaffected).

⛔ The device leg (26c-2b, a duplicate-gemm removed then bit-exact-vs-RAW on Vulkan) FOUND this; the device-free plan gate
(26c-2a) could not — a merged-declare corpus still plans to the right stage COUNT and wiring, only the DATA is corrupt. A
CSE-touching pass needs a device (or an eval) differential, not just a plan gate. Regression: `test_cse.cpp` registers the
real resource dialect — two declares KEPT, two same-named `resource.import` (Pure, same external) STILL merge (the
declare-specific proof). See [feedback_semantics_preserving_pass_differential_test_is_bit_exact_vs_unoptimized_program](build-and-verification.md#memory-feedback_semantics_preserving_pass_differential_test_is_bit_exact_vs_unoptimized_program),
[feedback_registered_default_empty_reads_as_provably_none](workflow-and-correctness.md#memory-feedback_registered_default_empty_reads_as_provably_none) (Pure-default-was-wrong shape). ✅ CEIR-26z FIXED the sibling:
`resource.import` was still Pure (the 26c fix was declare-specific) so two ANONYMOUS imports (no name ⇒ 0 attrs) merged — the
same collapse. Import now ALSO carries an `Allocate` effect (`traits=[]` in resource.ceirop.toml, regenerated); named AND
anonymous imports are all KEPT (the 26c same-named merge was a Pure-artifact, never a specified optimization — CSE RAUWs the
result Value). Gate: test_cse.cpp "ceir 26c/26z" (two anon imports KEPT + cse_run==false; the file's cs.* Pure test is the
CSE-works positive control). The declare-A@A device lesson stands; the import fix's gate is the erasure assertion itself (no
plan→device data-blindness — erasure IS the CSE decision).


<!-- end-memory:feedback_cse_must_not_merge_external_source_ops_declare_is_alloc_not_pure -->

<a id="memory-feedback_data_driven_driver_read_the_match_not_just_the_rewrite"></a>
## feedback_data_driven_driver_read_the_match_not_just_the_rewrite

---
name: feedback_data_driven_driver_read_the_match_not_just_the_rewrite
description: When a data-driven rule interpreter reproduces a C++ rewrite pattern, read the C++ MATCH for EVERY precondition, not just the rewrite body — a guard hiding in the match (e.g. has_uses) is load-bearing and reasoning from the action shape misses it.
metadata:
  node_type: memory
  type: feedback
  originSessionId: cb9df3b8-2389-479b-9d99-d3d6ce3ba327
  modified: 2026-09-05T01:31:28.030Z
---

CEIR-27d (2026-09-05): building the data-driven `greedy_rewrite_rules` interpreter for `reshape_of_reshape`, I wrote (and put in a 27c source comment) that the `build_op_from_inner_operand` action "has NO result-use guard" — reasoning from the ACTION's shape (a build+RAUW doesn't obviously need a use-check). WRONG: `canonicalize.hpp:51`, the C++ `match_reshape_of_reshape` predicate the rule reproduces, carries `!op.result(0)->has_uses() → return false` in plain sight — exactly as `match_identity_reshape` does at line 73. The RAUW-and-leave re-match guard is UNIVERSAL (both actions RAUW a result and leave the dead op for DCE), not action-specific. Missing it means the driver re-fires on its own already-RAUW'd op forever → the monotone cap → FATAL. It was hoisted to a single universal check in `rule_matches` before any constraint.

**Why:** a data-driven interpreter's `match` must encode EVERY precondition the C++ `match` enforces. The rewrite body (what it builds) is the visible half; the match predicate (when it is legal to fire) is the half that guarantees TERMINATION and correctness, and its guards are easy to overlook because they read as "obvious" once fired but are invisible if you reason forward from the action. The differential test WOULD eventually catch a missing guard (as a FATAL from the non-monotone cap), but a guard belongs in the driver by READING, not by discovering a test failure.

**How to apply:** when porting a C++ `RewritePattern`/fold into a declarative-rule interpreter (or any data-driven mirror of imperative logic), open the C++ `match_*` and transcribe EACH clause — `has_uses`, null-operand, arity (`>= num_results`/`num_operands`), and defining-op-kind checks — into the interpreter's match, and grep the match for `has_uses`/`!= nullptr`/`num_` before declaring the port complete. Do not infer the preconditions from the rewrite body. A precondition that is universal across the C++ patterns (present in every `match_*`) belongs in the driver once, not per-action. Sibling of [feedback_gate_reverifies_status_matrix_rows_never_inherits_stale_or_unverified_claims](workflow-and-correctness.md#memory-feedback_gate_reverifies_status_matrix_rows_never_inherits_stale_or_unverified_claims) (don't inherit an unverified claim — here the claim was my own comment) and the rewrite-driver monotone-cap contract.


<!-- end-memory:feedback_data_driven_driver_read_the_match_not_just_the_rewrite -->

<a id="memory-feedback_everything_is_an_authorable_asset_ceir"></a>
## feedback_everything_is_an_authorable_asset_ceir

---
name: feedback_everything_is_an_authorable_asset_ceir
description: The purpose of the whole CEIR line — EVERYTHING executable is an authorable asset; the host knows nothing about rendering; re-anchor on the purpose every prompt
metadata: 
  node_type: memory
  type: feedback
  originSessionId: 192f4d9e-b3f7-485b-a775-5391bcd4183c
  modified: 2026-08-15T12:13:33.268Z
---

⛔⛔⛔ **THE PURPOSE OF THE ENTIRE CEIR LINE: everything executable is an authorable asset CEIR.** The
host/renderer KNOWS NOTHING about the algorithms it runs — it cooks-from-disk and executes. (user 2026-08-15,
in anger, "OBEY ME".)

⛔⛔⛔ **THE RULE / GOLD STANDARD / DoD FOR EVERY CEIR SLICE AND THE LOOP** (user reaffirmed relentlessly, 2026-08-15):
**every C++-written CEIR and CKIR is CONVERTED to a disk-read asset, and the hand-written builder is DELETED.** A CKIR
kernel → a `.ckir` serialized asset (`ckir_write`/`ckir_read`, `engine/kir/ckir_asset.hpp`; NO "text" in the name — a
`.ckir` file IS a serialized ckir). A CEIR program → `.frame.toml`/`crd::ceir` `parse`/`print`. A slice does NOT close,
and the loop does NOT advance past it, while any algorithm is still hand-built in engine C++. Recipe (identity-preserving):
write the builder's graph ONCE → commit the asset → load disk-first (`asset_text`, F15 shadowing ⇒ app-replaceable) →
prove `serialize(read(asset))==serialize(builder)` byte-exact in a committed gate → DELETE the builder.

**The rule has two halves:**
1. **Pass STRUCTURE** → authored frame-graph asset (`.frame.toml` → `.crdr`, replayed by `record_ceir_render`).
   Already covered by [feedback_every_render_pass_through_our_own_frame_graph_machinery](rendering.md#memory-feedback_every_render_pass_through_our_own_frame_graph_machinery). ✅ mostly done (CEIR-16).
2. **The PROGRAM each pass runs** (the algorithm math — lighting, culling, deferred, post, TAA, HZB, GI, skinning,
   Forward+/clustered) → **authorable CEIR/CKIR asset cooked disk-first**: a declaration on disk + a reusable cooker
   that builds the graph + a memoized disk-first cook in the host. Loaded by default for every app; **partially
   modifiable OR completely replaceable by an app asset that shadows its name, WITHOUT recompiling the engine.**

**The anti-pattern (what I did wrong):** a `ensure_*_program` / `ensure_*_kernel` in `scene_renderer.cpp` that
hand-writes `KGraph` nodes in C++ (exemplar `ensure_deferred_lighting_program`, scene_renderer.cpp ~2180). The
algorithm lives in C++, not an asset. Every such builder is a conversion+deletion target — DELETION IS THE PROOF
([feedback_authored_asset_slice_done_only_when_cpp_deleted_and_renders](rendering.md#memory-feedback_authored_asset_slice_done_only_when_cpp_deleted_and_renders)). Contrast the RIGHT shape already in the
same file: `ensure_cull_kernel` → `cook_stage_named("vertex/scene_cull.crdv")` (declaration on disk + `vertcook`
cooker + disk-first cook).

**C++ / CEIR-from-C++ is allowed** — but ONLY as the cooker MECHANISM, never as the shipping form. The target is
ALWAYS the authorable asset.

**Why (the scar):** under CEIR-18a, a slice literally titled "Forward+ renderer as an **authored asset**," I built
the new cluster light cull as `build_cluster_light_cull(KGraph&, …)` — a fixed C++ builder in
`engine/kir/include/crd/kir/ckir_light_cull.hpp` — with no asset declaration and no disk-first cook path, while the
correct pattern (`scene_cull.crdv`) sat in the same source. I built more of the exact C++ the slice exists to
delete.

**How to apply:**
- Before acting on ANY task, NAME its purpose and re-read it. Re-anchor EVERY prompt
  ([feedback_autonomous_ceir_loop_never_idles_drive_through_every_blocker](execution-ir.md#memory-feedback_autonomous_ceir_loop_never_idles_drive_through_every_blocker) is about not idling; THIS is about not
  drifting from the purpose while busy).
- The test before "done": can an app replace this algorithm by editing/shadowing an asset WITHOUT recompiling the
  engine? If no, not done.
- `scene_renderer` must not know anything about rendering. No hand-built algorithm graphs in the host.
- Pinned: AGENTS.md §Engineering Principles + §Agent Conduct (two ⛔⛔⛔ bullets), docs/PRINCIPLES.md
  ("Everything executable is an authorable asset").


<!-- end-memory:feedback_everything_is_an_authorable_asset_ceir -->

<a id="memory-feedback_opgen_attr_names_are_cxx_identifiers_sanitize_keywords"></a>
## feedback_opgen_attr_names_are_cxx_identifiers_sanitize_keywords

---
name: feedback_opgen_attr_names_are_cxx_identifiers_sanitize_keywords
description: "opgen turns authored attr/operand/result NAMES into C++ identifiers; a name that is a C++ keyword (class, default, template, register, operator, new...) emits uncompilable code unless opgen sanitizes it."
metadata: 
  node_type: memory
  type: feedback
  originSessionId: cb9df3b8-2389-479b-9d99-d3d6ce3ba327
  modified: 2026-09-05T10:10:32.629Z
---

An authored `.ceirop.toml` attr/operand/result NAME is a valid IR identifier (the `_is_ident`
regex `[A-Za-z_][A-Za-z0-9_]*`) but need NOT be a valid C++ identifier. CEIR-29c-3b added
`transform.constrain_provider_class{class:string}` — the right attr name (`provider_class` would
collide semantically with the `MlProvider::provider_class` field, reading as if the directive
selected a *provider*). But opgen emitted `AttrId class` (builder param) and `AttrId class()`
(accessor) — `class` is a C++ keyword, so the generated header did not compile. opgen is the ONE
place that turns an authored name into a C++ identifier, and it had no sanitizer.

**Why:** the fix is in the TOOL, not the name — a rename would leave the landmine armed for the
next attr named `default`/`template`/`register`/`operator`/`new`/etc. opgen conflated two
namespaces: the authored surface (`{class = "gpu"}`) and the emitted C++ identifier.

**How to apply:** `tools/ceir_opgen/ceir_opgen.py` has `_cxx_ident(name)` (the C++20 keyword set
incl. alternative-token operator words → append `_` iff reserved). Apply it at every site where a
name becomes a C++ identifier — the 7 sites are: the operand/result/attr accessor METHOD names, the
operand+attr builder PARAMS, the `operands[]` initializer, and the `set_attr` VALUE arg. NEVER apply
it to the `attr("...")` / `set_attr(op, "...", …)` STRING arguments — those keep the raw authored
name (that is what makes the surface stay `class` while the C++ becomes `class_`). A `-O`-proof
`if …: raise` drift lock (NOT a strippable `assert`) guards the keyword table. Prove the change is a
no-op for every existing op with pre/post-regen MD5 of the generated `*_ops.{cpp,hpp}` — only the new
dialect's file may change; the smoke/`.ops.json`/`.ops.md` emitters never consume `_cxx_ident`.
See [feedback_opgen_regen_is_all_dialects_diff_HEAD_lies_in_an_uncommitted_batch](execution-ir.md#memory-feedback_opgen_regen_is_all_dialects_diff_head_lies_in_an_uncommitted_batch) for the regen
discipline (a tool change touches every dialect; discriminate by rebuild+MD5, not `git diff HEAD`).


<!-- end-memory:feedback_opgen_attr_names_are_cxx_identifiers_sanitize_keywords -->

<a id="memory-feedback_opgen_regen_is_all_dialects_diff_head_lies_in_an_uncommitted_batch"></a>
## feedback_opgen_regen_is_all_dialects_diff_HEAD_lies_in_an_uncommitted_batch

---
name: feedback-opgen-regen-is-all-dialects-diff-head-lies-in-an-uncommitted-batch
description: "ceir_opgen.py regenerates EVERY dialect (no per-dialect flag); in an uncommitted multi-band batch, git diff HEAD on generated files falsely attributes prior bands' TOML edits to today's regen — discriminate with a rebuild+test, not the diff."
metadata: 
  node_type: memory
  type: feedback
  originSessionId: cb9df3b8-2389-479b-9d99-d3d6ce3ba327
  modified: 2026-09-05T06:05:15.277Z
---

`tools/ceir_opgen/ceir_opgen.py` has NO per-dialect argument — running it (to add or change ONE dialect's ops) rewrites EVERY dialect's `engine/ceir/generated/crd/ceir/gen/*_ops.{hpp,cpp,json,md}` + each `tests/ceir/generated/test_*_gen_smoke.cpp`. So after editing one `engine/ceir/ops/*.ceirop.toml` and regenerating, expect UNRELATED dialects' generated files to show as changed in `git status`.

**Why:** in an UNCOMMITTED multi-band batch (e.g. the CEIR-26/27/28/29 batch whose last commit is `c35548a "working on CEIR-25"`), `git diff HEAD` on a generated file shows the delta since a commit that PREDATES those bands' TOML edits — so a prior band's edit (e.g. 26c changing `resource.declare`/`resource.import` from `Pure` to an `Allocate` effect so distinct resources aren't CSE-merged) appears in the diff as if TODAY's regen introduced it. It did not: the working-tree generated file was already that content (an earlier opgen run in the batch — the one that created the transform/tune/rewrite dialects — wrote it). `git log --oneline -1 -- <generated file>` shows its last COMMIT (can be many bands stale), not its working-tree state.

**How to apply:** NEVER conclude "my regen changed dialect X's semantics" from `git diff HEAD`. Discriminate EMPIRICALLY: rebuild the affected lib + run that dialect's tests (`ctest -R <band>`) — GREEN means the semantics are correct and were already compiled that way; and confirm `python tools/ceir_opgen/ceir_opgen.py --check` is clean (generated==TOML) with the `crd-ceir-opgen-drift` ctest passing. The regen is a legitimate drift-SYNC, not a semantic change of your slice: do NOT revert it (that re-introduces drift the drift-test catches) and do NOT flag it as your band's change — note it belongs to whichever band edited the TOML. The next regen in this batch (29b will add ops) hits this exact wall. Related: [feedback_ascii_only_test_names](build-and-verification.md#memory-feedback_ascii_only_test_names) (a non-ASCII em-dash in a TEST_CASE name false-fails Windows `ctest -R`; surfaced in the same 29a-3b-1 run — only 2 tests in the whole suite violated it).


<!-- end-memory:feedback_opgen_regen_is_all_dialects_diff_HEAD_lies_in_an_uncommitted_batch -->

<a id="memory-feedback_plan_output_by_traversal_is_not_ssa_liveness_pin_readback_before_any_pass"></a>
## feedback_plan_output_by_traversal_is_not_ssa_liveness_pin_readback_before_any_pass

---
name: feedback_plan_output_by_traversal_is_not_ssa_liveness_pin_readback_before_any_pass
description: A readback-by-Value output (e.g. an autodiff gradient) has NO SSA uses — pin it (func.return) before running ANY pass or DCE deletes it
metadata: 
  node_type: memory
  type: feedback
  originSessionId: cb9df3b8-2389-479b-9d99-d3d6ce3ba327
  modified: 2026-09-04T13:49:16.540Z
---

The tensor plan marks the terminal write `Output` by TRAVERSAL (readback-by-Value); that is NOT SSA-liveness.
`build_gradient`'s `grads[]` are NOT SSA-consumed — nothing in the IR reads them. So running ANY pass
(DCE, CSE, …) BEFORE `plan_tensor_pipeline` DELETES them: a gradient is a Pure `linalg.gemm` with no
result-uses ⇒ DCE prunes it and cascades back through the whole backward (in the vjp_mlp corpus, dW1/dW2
and everything upstream vanish). Confirmed by the CEIR-26a-2 NEGATIVE gate.

**Why:** DCE reads liveness from SSA uses + effects ONLY — there is deliberately NO root side-channel
(`Pass::run(ctx,m,diag)` has no roots arg; a pass reads the program, not a caller argument). A
plan-level `Output` role is invisible to it.

**How to apply:** PIN the readback outputs in the IR BEFORE any pass — a `func.return(grads...)` is the
clean pin (Terminator ⇒ NOT Pure ⇒ roots its operands; the planner SKIPS `func.return`,
tensor_pipeline.cpp:211, so the plan is unaffected). Or a store op. The transform must NOT pin (it must
not decide the gradients' consumer — same forward-liveness contract; the CALLER pins). See dce.hpp's
LIVENESS CONTRACT header + grad.hpp's build_gradient caller note.

**Test-helper trap (same slice):** `mkmain` returns the `func.func` BODY block (one region deep);
`m->body()->first_block()` is the OUTER block holding just the func.func CONTAINER (1 op). To count ops
in the authored body, use the func body block's `num_ops()` (or recurse regions) — a non-recursive
top-level module-body walk sees 1 op and reports a false 0-delta. `erase()` unlinks + tombstones
(ir.hpp:370-371), so `num_ops()` after erase is the true live count.

Related: [feedback_locked_checklist_item_needs_a_gate_or_it_silently_doesnt_land](workflow-and-correctness.md#memory-feedback_locked_checklist_item_needs_a_gate_or_it_silently_doesnt_land) (this locked contract
needed its own NEGATIVE gate — pinned-survives can't distinguish "kept" from "kept anyway");
[feedback_effect_narrowing_must_suppress_all_whole_class_effects_not_just_the_obvious_one](workflow-and-correctness.md#memory-feedback_effect_narrowing_must_suppress_all_whole_class_effects_not_just_the_obvious_one) (DCE's
Pure-only rule is the effect-narrowing scar in reverse).


<!-- end-memory:feedback_plan_output_by_traversal_is_not_ssa_liveness_pin_readback_before_any_pass -->

<a id="memory-reference_ceir_band_numbering_tracker_is_authoritative"></a>
## reference_ceir_band_numbering_tracker_is_authoritative

---
name: reference_ceir_band_numbering_tracker_is_authoritative
description: CEIR band numbers were RENUMBERED — the tracker (D-007) is authoritative; the 2026-08-07 master roadmap + old code comments are pre-renumber. Resolve every band-number reference against the tracker.
metadata: 
  node_type: memory
  type: reference
  originSessionId: 192f4d9e-b3f7-485b-a775-5391bcd4183c
  modified: 2026-08-18T09:08:52.820Z
---

The CEIR band numbering DIVERGED between the original master roadmap (`docs/research/2026-08-07-ceir-universal-programming-master-roadmap.md`) and the LIVE plan. **The tracker `docs/detours/D-007-ceir-tracker.md` forward-roadmap table is AUTHORITATIVE** (context.md points to it as "Detail = tracker (AUTHORITATIVE)").

The renumber (tracker order):
- **CEIR-23 = `ceir.sparse` + `ceir.quant`** (sparse op + quantized-MLP path; §53/§54).
- CEIR-24 = `ceir.ml` + provider partitioning (MLP/attention; §55/§56).
- CEIR-25 = `ceir.autodiff` (§57).
- CEIR-26 = **Optimizer phase 1** (canonicalize/DCE/CSE/partial-eval/inline/fusion/memory/schedule; §73/§74).

⛔ the OLD roadmap had CEIR-23 = Optimizer phase 1 — do NOT trust it for band numbers. **Code comments carry the OLD numbering too**: e.g. `engine/ceir/include/crd/ceir/type.hpp` says `SparseTensor` "encoding is CEIR-18" — that is pre-renumber shorthand for "a later band," NOT literally CEIR-18. ⛔ Before starting a band or trusting a "CEIR-NN" in a code comment / research doc, resolve the number against the tracker table (grep `CEIR-2[3-9] \|` in D-007). This nearly started the CEIR-23 open on the wrong charter (Optimizer instead of sparse/quant). The §-section numbers (§53/§54/…) are STABLE across the renumber — prefer them when disambiguating.


<!-- end-memory:reference_ceir_band_numbering_tracker_is_authoritative -->

<a id="memory-reference_ceir_text_asset_authoring_via_print"></a>
## reference_ceir_text_asset_authoring_via_print

---
name: reference_ceir_text_asset_authoring_via_print
description: "How to author a parse-loadable .ceir TEXT asset — parse()/print() grammar, NO comments, the bootstrap-via-print pattern, keep the builder as anti-drift oracle"
metadata: 
  node_type: memory
  type: reference
  originSessionId: 192f4d9e-b3f7-485b-a775-5391bcd4183c
  modified: 2026-08-18T08:42:54.020Z
---

A CEIR `Module` IS authorable as a parse-loaded TEXT file (like `.ckir` for CKIR). The pair:
- **`crd::ceir::parse(Context&, containers::StringView) → ParseResult{Module* module, bool ok, usize error_offset, const char* error}`** — `engine/ceir/include/crd/ceir/parse.hpp`.
- **`crd::ceir::print(Context&, const Module&, memory::IAllocator*) → containers::String`** — `print.hpp`. `print()` IS the canonical serializer (the `.ceir` analogue of `ckir_write`); `print(parse(print(x))) == print(x)` is the fixed point.

**Grammar** (MLIR-flavored, from `print()`): `module { ^bb0: %0 = resource.declare() : !tensor<!f32,!shape<!dim<8>,!dim<8>>> ... %6, %7 = tensor.fft(%4, %5) {axis = 0, direction = "forward"} : !tensor<...> %8 = arith.const() {value = 1} : !index compute.dispatch(%8, %9, %9, %6, %7, %10) {access = "r,r,w", kernel = @viz_magnitude} ... }`. SSA `%N`, block `^bb0:`, types `!tensor<...>`/`!index`, attrs `{k = v}` (symbol = `@name`), a 2-result op is `%a, %b = op(...)`, a resultless op (compute.dispatch) has no `%N =`.

⛔ **The parser SKIPS WHITESPACE ONLY — NO `#`/`//` comments** (`parse.cpp` `skip_ws`). So a committed `.ceir` asset is the EXACT `print()` output, NO header. Document its purpose in the LOADING TEST + the tracker, not the file.

⛔ **Registration**: `parse()` resolves op names against the DIALECTS registered in the parsing `Context` — call `func::register_dialect` + every `register_*` the asset uses (func/resource/linalg/tensor/arith/compute for a tensor pipeline) FIRST, else the ops parse opaque and the misuse walks / `find_unregistered_op` reject.

**Authoring = BOOTSTRAP-VIA-PRINT** (hand-writing the grammar is error-prone): build the module in C++ via the op-builder API (this is authoring the IR STRUCTURE — NOT a C++ algorithm KGraph-builder, so it does NOT violate [feedback_no_cpp_kgraph_builders_author_ckir_directly](build-and-verification.md#memory-feedback_no_cpp_kgraph_builders_author_ckir_directly)), `print()` it to a scratchpad, then commit that text as `assets/ceir/<name>.ceir`. The loading gate: `parse().ok` → ANTI-DRIFT `print(parse(file)) == print(build)` (⛔ regenerate the committed file if the builder changes) → roundtrip stable → misuse walks clean → `collect_dependencies().ckir_refs`. ⛔ **KEEP the C++ builder as the anti-drift oracle** — do NOT delete it (unlike the `.ckir` write→commit→delete pattern; a `.ceir` asset has no cook-time author-then-delete step yet). First use: CEIR-22c-3c `assets/ceir/tensor_pipeline.ceir` (the §137 GEMM→FFT→reduction→viz pipeline).


<!-- end-memory:reference_ceir_text_asset_authoring_via_print -->

<a id="memory-scars_ceir_ir"></a>
## scars_ceir_ir

---
name: scars_ceir_ir
description: Scars for CEIR-IR — serialization/identity (ZIP64, typeid ABI, graceful-reject deserialize, monotone watermark, unify-at-linkable-layer, atomic rollback, commit-verify, compiled-tier mirror), passes (region backlink, registered-default, structure-verifier, effect-narrowing, memory-liveness, pin-readback, op-name qualified, declare-verify, fusion-select, CSE external-source), and the LOCKED-checklist gate — relocated out of MEMORY.md; open before "fixing" a CEIR-IR / pass / serialization symptom.
metadata: 
  node_type: memory
  type: reference
  originSessionId: cb9df3b8-2389-479b-9d99-d3d6ce3ba327
  modified: 2026-09-06T14:52:55.927Z
---

CEIR-IR serialization / identity / verifier / optimizer-pass scars, moved out of the always-loaded `MEMORY.md`
(CEIR-26z compaction, 2026-09-05). Recall on demand when the work touches CEIR-IR serialization, identity, verifiers,
or optimizer passes (DCE/CSE/fold/specialize/fusion/memory-plan). Read before "fixing" a matching symptom.

## Serialization / identity
- [ZIP64](workflow-and-correctness.md#memory-feedback_zip_readers_must_handle_zip64_lib3mf_writes_it_always); [⛔⛔ typeid](workflow-and-correctness.md#memory-feedback_typeid_name_is_abi_decorated_match_both); [⛔⛔ deserialize](build-and-verification.md#memory-feedback_ceir_deserialize_build_raw_graceful_reject_never_factory_assert); [⛔⛔ monotone](workflow-and-correctness.md#memory-feedback_monotone_id_needs_watermark_not_live_max_scan); [⛔⛔ unify at](workflow-and-correctness.md#memory-feedback_unify_at_a_linkable_layer_and_absorb_with_a_real_consumer); [⛔⛔ atomic](workflow-and-correctness.md#memory-feedback_atomic_rollback_settle_identity_at_begin_and_erase_checks_both_subtree_boundaries); [⛔⛔ commit-veri](build-and-verification.md#memory-feedback_commit_verify_is_op_local_module_wide_rules_ride_a_consumer_sweep); [⭐⭐ 11b MIRROR](workflow-and-correctness.md#memory-feedback_compiled_tier_mirror_scars)

## Passes / verifiers
- [⛔⛔ region](execution-ir.md#memory-feedback_ceir_region_parent_op_backlink_never_wired); [⛔⛔ registered](workflow-and-correctness.md#memory-feedback_registered_default_empty_reads_as_provably_none); [structure-veri](execution-ir.md#memory-feedback_ceir_structure_verifier_stricter_than_fuzz_corpus_validity); [⛔⛔ narrow ALL](workflow-and-correctness.md#memory-feedback_effect_narrowing_must_suppress_all_whole_class_effects_not_just_the_obvious_one); [⛔⛔ mem-liveness](workflow-and-correctness.md#memory-feedback_memory_liveness_is_first_use_not_declare_and_ambient_needs_symmetric_span); [⛔⛔ pin-readback: func.return before any pass (DCE deletes unpinned; plan-Output≠SSA-live)](execution-ir.md#memory-feedback_plan_output_by_traversal_is_not_ssa_liveness_pin_readback_before_any_pass)
- [⛔⛔ op_name=QUALIFIED](execution-ir.md#memory-feedback_ceir_hook_op_name_compare_must_be_dialect_qualified); [⛔⛔ declare-verify=EVERY contract (advisor)](workflow-and-correctness.md#memory-feedback_declare_slice_verifier_must_enforce_every_declared_contract); [⛔⛔ fusion select=FULL attrs, TYPED reject (advisor)](workflow-and-correctness.md#memory-feedback_fusion_and_specialized_kernel_selection_must_check_full_semantic_attrs_not_just_structure)
- [⛔⛔ CSE≠merge external-source: declare+import=Alloc-not-Pure (26z: anon import too); merged→gemm A@A; DEVICE leg found it](execution-ir.md#memory-feedback_cse_must_not_merge_external_source_ops_declare_is_alloc_not_pure)
- **⛔⛔ giving a formerly-Pure op an EFFECT to block CSE also ENTERS it into the hazard walk (CEIR-31b-3-c-i, the downstream of ↑):** CSE keys on the `OpTrait::Pure` trait (`register_op`: Pure⇒zero-effects), so 26c/26z made declare/import non-Pure by adding an `Allocate` effect. But `op_access_at` reads `effect_access(family)` too, and `Allocate` was grouped `{write,Memory}` (for Dealloc use-after-free) → every declare now WROTE its resource → spurious `declare→use`/`import→use` hazard edges (build_scene 15d-1: hz 1→4). ⛔ two lessons: (1) when you add an effect to an op, CHECK `effect_access(family)` says what the op MEANS in the hazard walk — an ALLOCATION writes no CONTENT, and alloc-before-use is ALREADY an SSA def-use edge (declare's result IS the consumer's operand), so `Allocate` must be INERT `{false,false,None}` (the `Nondeterministic` mold), while Dealloc/Residency STAY `{write,Memory}` (use-after-free is NOT SSA-implied); (2) an op-definition change's blast radius is its CONSUMERS' suites (frame-cook 15d hazard/lifetime tests), NOT just the pass's own tests — a `-R ceir` sweep caught this; "run only tests you added" would have shipped it red. [feedback_cse_must_not_merge_external_source_ops_declare_is_alloc_not_pure](execution-ir.md#memory-feedback_cse_must_not_merge_external_source_ops_declare_is_alloc_not_pure) added the effect; this scar is the hazard-walk fallout.
- [⛔⛔ LOCKED reject/guard needs a negative gate SAME slice or silently doesn't land](workflow-and-correctness.md#memory-feedback_locked_checklist_item_needs_a_gate_or_it_silently_doesnt_land)
- [⛔⛔ data-driven rule interpreter: read the C++ MATCH for EVERY precondition (has_uses etc.), not the rewrite body — a universal guard belongs in the driver by READING not test-failure (27d)](execution-ir.md#memory-feedback_data_driven_driver_read_the_match_not_just_the_rewrite)
- **⛔⛔ a lowering that STRIPS an asset ENDS its consumers' window (CEIR-30b-3b/30c-2b):** `lower_sharded_reduction` calls `strip_dead_meshes`, so every mesh CONSUMER (`placement_from_transform`, `find_dist_misuse`) MUST run after `materialize_sharding` but BEFORE `lower_sharded_reduction` — the mesh must still exist for rank-count validation, and dist-verify-first must see it. A pass that erases X closes X's consumers' window; sequence the verify/place/read of X BEFORE the strip, never after. (The 30c-2b flow does exactly this: materialize → find_dist_misuse + placement_from_transform → LOWER.)
- **⛔⛔ a trait that WIDENS what the VERIFIER accepts must be matched at every EXECUTOR — run it or REFUSE it (CEIR-31z, the delay/biquad StateEdge hole):** `OpTrait::StateEdge` makes the §20 5d verifier ACCEPT a same-block back-edge (a `delay(mix(src, delay))` feedback echo) instead of `FeedbackWithoutState`. But `execute_audio_graph_ceir` is a SINGLE-PASS block/def-use walk that CAN'T run a graph cycle (its input bus is not yet computed) — it would SILENTLY produce an echo-less passthrough. So a graph the verifier BLESSED was a CAN'T-FAIL at the executor: the audio dialect declared StateEdge at 31a-2a but no executor exercised or refused a feedback topology until 31z. ⛔ LESSON: when a dialect declares a trait that admits a new structural SHAPE (a back-edge, a nested region, a variadic tail), grep EVERY executor for that shape and either handle it or REFUSE it with the graceful-reject convention (`execute_*` returns 0). The 31z fix REFUSES (`*s >= my` → 0, mirroring render_graph's Kahn cycle-refusal); running it (sample-interleaved SCC eval) is a future capability with NO bit-exact oracle. A verifier-accepts / executor-mis-runs gap is the [feedback_declare_slice_verifier_must_enforce_every_declared_contract](workflow-and-correctness.md#memory-feedback_declare_slice_verifier_must_enforce_every_declared_contract) mirror on the EXECUTION side.
- **⛔⛔ a SET-valued field stored as an insertion-ordered array and FOLDED INTO A HASH is authoring-order-sensitive — canonicalize the STORAGE at insert (by the unique key), never patch the hash (CEIR-32c, the CHIR `m_edges` scar 32c surfaced in 32b):** CHIR edges are a SET keyed by the single-writer CONSUMER pin `(to_node, to_pin)`, but `SourceModel::add_edge` push_back'd in insertion order and `semantic_hash` folded that order. The oracle added edges in author whim; the TEXT projection (32c) wires each edge at its consumer in-pin (pin order, node pre-order), a DIFFERENT order → `semantic_hash(text) != semantic_hash(graph)` → the cross-projection parity anchor would fail, and any 32d graph-editor click order would break it forever. ⛔ LESSON: two data structures that must hash-compare equal for the SAME set must impose a CANONICAL order on the SET itself — fix it in the STORAGE (`add_edge` now does a linear insert sorted by the consumer key; single-writer makes the key unique + total, so no std::sort, no tie-break), NOT by sorting a scratch copy inside the hash (which leaves `print`/serialization still authoring-order-divergent). The advisor caught this before a test did — a set folded into an identity is the tell. Paired reject: `read_schema` now rejects two edges into one in-pin (single-writer), since canonical order makes a duplicate adjacent.
- **⛔⛔ a lowering/emitter that FABRICATES its inputs passes a PARITY gate trivially — parity is NECESSARY, not SUFFICIENT; the falsifier is source-minus-edges != source (CEIR-32d, the CHIR->CEIR lowering):** `lower_chir` first emitted every op with fabricated `konst` operands, IGNORING the CHIR `m.edges()` dataflow. Both projections still lowered BYTE-IDENTICAL (they produce the same model), so `print(text-lowering)==print(graph-lowering)` PASSED — while every wire was lost. A parity/round-trip gate proves "the two inputs agree" and "the output is well-formed", NEVER "the output preserved the input's structure". ⛔ LESSON: (1) THREAD the real dataflow — record each producer's out-pin -> SSA Value, resolve each consumer's in-pin THROUGH the edges, const-fallback only a genuinely unconnected pin; (2) gate it with a NEGATIVE/falsifier that DELETES the structure and asserts the output CHANGES (oracle-minus-edges lowers differently — the parallel body yields `%iv` not the view), the [feedback_gate_assertions_check_identity_not_category](workflow-and-correctness.md#memory-feedback_gate_assertions_check_identity_not_category) discipline applied to a lowering; (3) an intake node that carries SOURCE IDENTITY (a CHIR `state` decl) must stamp it onto the lowered op (a `chir_decl` name attr — the reload-stable-id seam), never drop it as "materialized elsewhere". A parity gate that a fabricated-inputs pass survives is a smoke-false-green in a new costume.


<!-- end-memory:scars_ceir_ir -->

