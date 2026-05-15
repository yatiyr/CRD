# Per-slice verification protocol

> Locked 2026-05-15 per Strategic Execution Plan (`docs/ROADMAP.md` §
> Strategic Execution Plan) + CLAUDE.md DoD #8 + memory
> `feedback_per_slice_run_ctest.md`.

## Why this protocol exists

A slice is NOT closed until `ctest --preset <X>` returns exit 0 on the
four Windows per-slice configs (win-debug + win-asan + win-shipping +
win-tidy). The lesson, learned 2026-05-15 during Phase 3.1.7 v3-close,
is that **the test binary saying "All tests passed" can coexist with a
failing ctest-registered guard test**.

The four guard tests live in `tests/math/CMakeLists.txt` and are
registered via `add_test(NAME …)` — not as Catch2 `TEST_CASE`s, not as
entries in any test binary's `--list-tests` output:

| Guard | What it catches |
|---|---|
| `crd-no-non-ascii-test-names` | Windows ctest mojibakes non-ASCII chars in TEST_CASE names via CP1254 (Turkish Windows). `→` / `—` / `²` / `°` in TEST_CASE names fail to match argv. v3-close exposed 19 instances in v3b/v3c that had shipped past the test-binary-only verification. |
| `crd-simd-emission-check` | The compiler must emit AVX2 `ymm` instructions when the preset asks for them. Catches LTCG fold-back, preprocessor-skipped SIMD intrinsic includes, etc. |
| `crd-no-std-math-check` | Bans `std::sin/cos/exp/log/pow/sqrt/tan/asin/acos/atan/sinh/cosh/tanh` outside `crd::math::deterministic`. Catches accidental drift from ADR-0063 determinism contract. |
| `crd-no-std-sort-check` | Bans `std::sort` (use `crd::containers::stable_sort` instead). Catches cross-platform-sort-order divergence. |
| `crd-no-untagged-physical-numeric` (lands at v0a-3) | Bans bare-`f32` / `f64` for fields named `length`/`mass`/`force`/etc. — the units-system enforcement. |

Per-slice verification that runs ONLY the test binaries (`./crd-foo-tests.exe --reporter compact`) misses ALL of the above.

## The protocol

### Required at every slice close

Run `scripts/per-slice-check.ps1` (Windows) or `scripts/per-slice-check.sh` (Linux):

```powershell
# Windows — full per-slice verification (~5–10 min depending on slice size)
.\scripts\per-slice-check.ps1
```

```bash
# Linux — equivalent
./scripts/per-slice-check.sh
```

Exit code 0 = slice is verifiable. Non-zero = at least one config
failed and the slice is **not** closed.

### When to skip configs

The script supports `-SkipShipping` / `-SkipTidy` / `-SkipAsan` flags
for quick iteration during slice development. **At slice close, run
the full set (no skips).** The `full-sweep.ps1` 17-config sweep is the
v-cluster close gate; per-slice-check is the per-slice gate.

| Slice phase | Recommended invocation |
|---|---|
| In flight (rapid iteration) | `.\scripts\per-slice-check.ps1 -SkipShipping -SkipTidy` (debug + asan only — ~2 min) |
| Slice nearing close | `.\scripts\per-slice-check.ps1` (full 4 configs) |
| Sub-module close (e.g. `-bvh` complete, `-convex` complete, etc.) | `.\scripts\per-slice-check.ps1` + start `.\scripts\full-sweep.ps1` in background |
| V-cluster close (e.g. `v3-close`) | `.\scripts\full-sweep.ps1` (17 configs) |

### Per-sub-module eylem-stub smoke practice (in flight during Phase 3.1.7 geometry)

Eylem v1b shipped 2026-05-11; v1c resumes ~2026-12 per the Strategic
Execution Plan. To prevent code rot over the ~7-month gap, **as each
Phase 3.1.7 geometry sub-module ships**, run a ~30-min integration
smoke against the corresponding eylem v1c+ stub path (per the matrix
in `docs/phases/phase-3.1-eylem.md` § Eylem cold-storage mitigation):

| Geometry sub-module | Eylem v1c+ stub smoke |
|---|---|
| v4 mesh | `eylem::TriangleMeshCollider` builds + minimal raycast |
| v5 spatial | `eylem::SpatialIndex` builds + minimal point-query |
| v7 mesh-processing | `eylem::ColliderCooker::simplify_for_physics` builds + QEM-reduces a test mesh |
| v9 GPU LBVH | `eylem::Broadphase::gpu_path` builds (compile-only) |
| v9c V-HACD | `eylem::ColliderCooker::vhacd_decompose` builds + decomposes a test mesh |

These smokes are NOT formal slices and do NOT block sub-module close.
If a smoke fails, file a short debt entry in `docs/debt.md` and
continue; eylem v1c+ resume will pay it down. The point is to catch
build breakage **early** so the eylem resume isn't blocked by 7
months of compounded API drift.

## Adding a new guard

If you add a new lint / policy guard:

1. Drop the guard script in `scripts/check_<name>.ps1` + `.sh`.
2. Register as a ctest test in `tests/math/CMakeLists.txt` via `add_test(NAME crd-<name>-check COMMAND …)`.
3. **Verify it appears in `ctest --preset win-debug --list-tests`** before declaring the slice that added it closed.
4. Update `feedback_per_slice_run_ctest.md` if the guard category isn't already covered.

## References

- `CLAUDE.md` Definition of Done #8 — the protocol rule.
- `feedback_per_slice_run_ctest.md` (user memory) — the rule, the rationale, the eylem-smoke matrix.
- `feedback_strategic_execution_plan_2026_05_15.md` (user memory) — the strategic context.
- `docs/ROADMAP.md` § Strategic Execution Plan — the source of the protocol decision.
- `docs/sessions/2026-05-15-geometry-v3-close.md` — the v3-close incident that triggered the protocol fix.
- `scripts/full-sweep.ps1` — the v-cluster close gate (17 configs).
