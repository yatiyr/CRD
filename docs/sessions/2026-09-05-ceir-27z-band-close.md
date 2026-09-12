# CEIR-27 band close — optimization strategy is an authorable asset (`ceir.transform` + `ceir.rewrite`, §71/§72/§146)

<!-- doc-role: evidence -->
> Dated evidence; counts, results and Next paragraphs are historical. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

The AUTHORITATIVE CEIR-27 close record. Optimization STRATEGY — both an execution SCHEDULE and a peephole REWRITE — is now an
authorable `.ceir` asset a runtime loader/driver applies, instead of hard-coded C++. Two proof lines: the §146 SCHEDULE proof (two
authored schedules optimize ONE program to the same numbers, different plans) and the §72 PLUGIN-RULE proof (an authored rewrite
reproduces canonicalize's fold WITHOUT editing the central C++ pattern array).

## What the band proves

- **`ceir.transform` — the SCHEDULE as an asset** (§71): two program-global directive ops (`transform.fuse{enable}` +
  `transform.share_storage{enable}`), authored as a `.ceir` module a loader (`plan_options_from_transform`) reads into `PlanOptions`
  at the existing plan seam (no signature change — the seam is test-only, flags read at 3 in-seam sites). Directives are NOT Pure
  (rule metadata, not a value computation); a `find_transform_misuse` rejects a duplicated directive (last-write-wins otherwise).
- **the §146 two-schedule DIFFERENTIAL**: the SAME fp32-MLP payload planned with two committed schedules (`schedule_fuse` [fuse+share]
  vs `schedule_nofuse` [neither]) → different plan shape (fuse = 2 stages incl. GemmRelu; no-fuse = 3) → BIT-EXACT device output on
  **Vulkan + DX12 + llvmpipe** (the §146 load-bearing claim: two schedules, one semantics). Share is program-global-inert on the MLP
  (asserted `tenants==0` both), so the observable difference is purely the fuse directive's.
- **`ceir.rewrite` — the PEEPHOLE as an asset** (§72): one op `rewrite.rule` (root/constraint/action/result/operand + optional
  inner_operand), closed constraint/action vocabularies enforced by `find_rewrite_misuse`. A data-driven `greedy_rewrite_rules`
  interpreter (mirroring the C++ `greedy_rewrite`: collect-per-round fixpoint, monotone-cap→FATAL, RAUW-and-leave) applies authored
  rules to a payload — so a plugin ships a canonicalization WITHOUT editing `canonicalize.hpp`'s `kCanonPatterns` (the array's own
  header notes it has no plugin boundary; §72's DSL IS that boundary).
- **the §72 PLUGIN-BOUNDARY proof**: the TWO authored rules reproduce canonicalize's COMPLETE `tensor.reshape` pattern set by
  DIFFERENTIAL — `greedy_rewrite_rules([rules])` print-equal to `canonicalize_run` — WITHOUT the rules being in `kCanonPatterns`.
  Rule block order IS pattern priority (proven, not asserted, on an outer-identity chain where both rules match the pivot op).

## Row-per-slice

| Slice | Claim | Proof |
|---|---|---|
| **27-0** | band-open census (advisor-verified) | knobs exist+named (`PlanOptions{fuse_gemm_relu, share_intermediate_storage}`); seam is test-only, flags at 3 in-seam sites; canonicalize = 2 C++ patterns; TWO proof lines (§146 schedule ≠ §72 plugin-rule); two deferrals LOCKED (per-op targeting, e-graph) |
| **27a** | `ceir.transform` dialect + asset→PlanOptions loader + dup-guard | 2 directive ops (opgen, NOT Pure = category-error not DCE, advisor-corrected); `plan_options_from_transform` (absent directive keeps base, no signature change); `find_transform_misuse` DuplicateDirective; device-free gate #827 + opgen-drift + smoke |
| **27b** | the §146 two-schedule BIT-EXACT differential | 2 committed `.ceir` schedules; device-free asset canonicality #828 (anti-drift + roundtrip + walk); device differential **Vk #4763 + DX12 #4852 + llvmpipe #4565** — plan-shape identity + BIT-EXACT output + first-mismatch capture; hand-authored `.ceir` byte-exact first try |
| **27c** | `ceir.rewrite` dialect + rule model/driver + identity_reshape asset | `rewrite.rule` (5 required attrs, closed vocabs, NOT Pure) + `RewriteRule`/`greedy_rewrite_rules`/`find_rewrite_misuse` (passes/rewrite_rules.hpp) + `rule_identity_reshape.ceir`; PLUGIN-BOUNDARY differential #543 (authored rule == `canonicalize_run` print-equal, NOT in kCanonPatterns) + discriminating idempotence (the has_uses guard proof); CRD_REPO_DIR added to crd-ceir-tests (first asset-reading device-free ceir test) |
| **27d** | the 2nd rule reshape_of_reshape (BUILD-a-new-op) | vocab +3 (`operand_defined_by_root` constraint + `build_op_from_inner_operand` action via generic `ctx.create_operation` + optional `inner_operand` attr, absent⇒0, build_rule stays 5 params per the resource.import precedent); ⛔ UNIVERSAL has_uses guard hoist (27c(5)(ii) SUPERSEDED — canonicalize.hpp:51 carries it); `ActionNeedsConstraint` cross-pairing guard; gate #544 — differential + FIRE-COUNT identity + COMPOSE (both rules collapse a net-identity chain) + ORDER-PRIORITY proven + cross-pairing negative |
| **27z** | band close | 27b llvmpipe #4565 BIT-EXACT (vulkaninfo confirms deviceName=llvmpipe LLVM 20.1.2); e-graph §72 deferral (recorded at 27-0); 2 scars; `canonicalize.hpp` "no plugin boundary" clause amended in place (27c/d superseded it); this log + commit message |

## Row-per-config

| Config | CEIR-27 result | Notes |
|---|---|---|
| **win-debug** (MSVC) | ✅ device-free family (27a #827, 27b #828, 27c #543, 27d #544, opgen-drift #711, rewrite gen-smoke) + the 3 device legs | the primary dev config; all hand-written files tidy-clean |
| **RTX 4070 Ti** (Vulkan + DX12) | ✅ 27b BIT-EXACT #4763 (Vk) + #4852 (DX12) | the §146 device differential, both backends |
| **lavapipe** (linux-gcc-debug, software Vulkan / llvmpipe LLVM 20.1.2) | ✅ 27b BIT-EXACT #4565 | ⭐ two authored schedules optimize the MLP bit-identically on CPU Vulkan; no llvmpipe defect (unlike the B19 campaign) |
| **WSL linux-gcc** (`-Werror`) — `crd-ceir-tests` | ✅ 27c #543 + 27d #544 + opgen-drift #711 | ⭐ the §72 core `rewrite_rules.hpp` is header-only (only `test_rewrite_driver.cpp` includes it) and widened THREE enums (`RewriteConstraint`/`RewriteAction`/`RewriteMisuseKind`) — gcc `-Werror` compiled it CLEAN (no switch-gap / sign-conv from the widen, the exposure the widen-enum/gcc-switch scars warn about); the §72 core is GREEN on BOTH toolchains |
| **win-release / win-asan** | CI (whole-repo) | the full release/asan sweep is CI's job (local = changed module + blast-radius); the 4 new files ride the next CI whole-repo run |

## Deferral ledger (all chartered name-forwards — never a silent subset)

1. **27e — pass-time rule wiring** (the biggest): with both canonicalize patterns now expressible as authored rules, "where does
   `canonicalize_run` get its rules at pass time" (asset path / registered default / embedded) is a DESIGN slice, its own census.
   The C++ `kCanonPatterns` retirement WAITS on 27e — 27c/27d proved equivalence by DIFFERENTIAL (that IS the retirement evidence),
   but the wiring is a choice, not a deletion this band.
2. **per-op targeting** (§71 fine-grained "fuse THESE ops" via handles/matchers — the MLIR-transform payload-handle shape): NO live
   consumer (both knobs are program-GLOBAL by construction); lands when a corpus wants two fusable sites decided differently.
3. **equality-saturation / e-graph** (§72 "Long-term optional frontier"): a VISION bullet, not a specified feature — documented and
   skipped (the no-follow-ons rule forbids deferring a SPECIFIED feature, not converting a vision bullet into a spec).
4. **general operand-PATH** (variable hop count): 27d's `inner_operand` is a single fixed inner index; a variable-length path
   language lands when a 3rd rule needs 3 hops.
5. **built op gets no attrs**: `build_op_from_inner_operand` builds via `create_operation` with no attr set — a root whose builder
   needs required attrs is not buildable this way (name-forward, recorded in the toml doc).

## Scars this band (memories written)

- ⛔ **A data-driven rule interpreter must read the C++ MATCH for EVERY precondition, not the rewrite body.** Building
  `greedy_rewrite_rules`, I claimed `build_op_from_inner_operand` "has no result-use guard" — reasoning from the action shape —
  but `canonicalize.hpp:51`'s `match_reshape_of_reshape` carries `!has_uses()` in plain sight; the guard is UNIVERSAL. Advisor-caught
  at the 27d review; hoisted to one universal check. → `feedback_data_driven_driver_read_the_match_not_just_the_rewrite`.
- ⛔ **A ⛔ invariant asserted in a SOURCE/doc comment needs a discriminating gate, or state it unproven.** The rewrite.ceirop.toml
  said "block order IS priority", but the only order-sensitive test used a chain where one rule could ever match the pivot — the
  claim shipped unproven until 27d(e) built the outer-identity corpus where both rules match. Folded (not a new file) into
  `feedback_gate_reverifies_status_matrix_rows_never_inherits_stale_or_unverified_claims` (the gate-reverify doctrine, extended to
  ⛔-comments in `.toml`/`.hpp`).

## Uncommitted batch (proposed — user commits; NO AI co-author trailer)

⛔ This is the band's largest uncommitted batch and the risk surface (the "uncommitted delete loses the source" scar). NEW this band:
`engine/ceir/ops/{transform,rewrite}.ceirop.toml` + the regenerated `engine/ceir/generated/crd/ceir/gen/{transform,rewrite}_ops.{cpp,hpp}`
+ `{transform,rewrite}.ops.{json,md}` + `tests/ceir/generated/test_{transform,rewrite}_gen_smoke.cpp` +
`engine/ceir/{include/crd/ceir/transform.hpp, src/transform.cpp}` + `engine/ceir/include/crd/ceir/gpu/tensor_pipeline.hpp` +
`engine/ceir-gpu/src/tensor_pipeline.cpp` (the loader) + `engine/ceir/include/crd/ceir/passes/rewrite_rules.hpp` +
`assets/ceir/{schedule_fuse,schedule_nofuse,rule_identity_reshape,rule_reshape_of_reshape}.ceir` + `tests/ceir/CMakeLists.txt`
(CRD_REPO_DIR) + edits to `tests/ceir/test_rewrite_driver.cpp`, `tests/ceir-gpu/test_tensor_pipeline.cpp`,
`tests/ceir-gpu-vulkan/test_ceir_pipeline_vulkan.cpp`, `tests/ceir-gpu-dx12/test_ceir_pipeline_dx12.cpp`,
`engine/ceir/include/crd/ceir/passes/canonicalize.hpp` (comment amendment).

⛔ RIDING as pre-existing (prior-band, NOT this batch — the user decides the boundary): `engine/ceir/generated/crd/ceir/gen/resource_ops.{cpp,json,md}`
(the 26c declare→Allocate + 26z import→Allocate regen, `M` since before 27) + the `??` files `tests/ceir-gpu-{vulkan,dx12}/test_ceir_bcast_perm_*.cpp`
and `test_ceir_elementwise_*.cpp` (prior-band uncommitted work).

```
feat(ceir-27): optimization strategy as an authorable asset — ceir.transform schedules + ceir.rewrite peepholes (§71/§72/§146)

Make the optimizer's STRATEGY authorable instead of hard-coded C++. Add ceir.transform (transform.fuse /
transform.share_storage program-global directives) + a plan_options_from_transform loader that reads a .ceir
schedule into PlanOptions at the existing plan seam (no signature change); find_transform_misuse rejects a
duplicated directive. Prove §146: two committed schedules (schedule_fuse vs schedule_nofuse) optimize one MLP
to a different plan but BIT-EXACT device output on Vulkan + DX12 + llvmpipe. Add ceir.rewrite (rewrite.rule +
a data-driven greedy_rewrite_rules interpreter + find_rewrite_misuse) so a plugin ships a canonicalization
without editing canonicalize.hpp's kCanonPatterns: two authored rules (identity_reshape RAUW-to-operand +
reshape_of_reshape build-a-new-op, via a build_op action over a 2-hop operand path) reproduce canonicalize's
complete tensor.reshape set by differential — print-equal to canonicalize_run, NOT in kCanonPatterns. The
RAUW-and-leave re-match guard is universal (driver contract); a path-walking action is typed-rejected
(ActionNeedsConstraint) unless paired with the constraint that validates its path; rule block order is
pattern priority (proven). Wiring the authored rules into canonicalize_run so kCanonPatterns can retire is
27e (named-forward). Author schedule_fuse/schedule_nofuse/rule_identity_reshape/rule_reshape_of_reshape.ceir.
```
