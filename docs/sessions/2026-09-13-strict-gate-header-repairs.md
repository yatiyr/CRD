# Strict-gate header repairs: the four pre-existing findings of the portable sample

<!-- doc-role: historical -->
> Dated evidence. Live owner: [REPO.DEV.3b.3](../ROADMAP.md#slice-repo.dev.3b.3); contract:
> [strict analysis](../design/developer-workflow.md#strict-analysis). Rules: [AGENTS](../../AGENTS.md).
> Preceding batch: [portable strict analysis](2026-09-13-portable-strict-analysis.md).

## User direction

The portable LLVM-20 gate's header sample left four headers with findings the hosted clang-tidy lane never surfaced
(`ckir_lighting.hpp`, `smolyak.hpp`, `bessel.hpp`, `heavy_tail.hpp`). The user asked for each finding to be repaired
without changing numerical results: renames and structural rewrites first, the exact existing rounding semantics kept
and documented, no switch to `lround` where it changes results, the gate rerun until clean, the owning test executables
run through the scoped check with bounded jobs, and a dated session record linked from the owning row. No commit or push
by the agent.

## Baseline

`python scripts/tidy-files.py` on the four headers, LLVM 20.1.8 against `build/win-debug` (1,814 entries): 19
diagnostics, every file analysed as the main file of a translation unit of its own module.

| Header | Findings |
|---|---|
| `engine/gpu/kir/include/crd/kir/ckir_lighting.hpp` | readability-identifier-naming on local constants `V1`, `V2`, `caseA`, `caseB`, `sA`, `sB`, `sC` and static constants `px`, `py` |
| `engine/numerics/hesap-quadrature/include/crd/hesap/quadrature/smolyak.hpp` | cppcoreguidelines-missing-std-forward on `F&& f` of `integrate_smolyak` |
| `engine/numerics/hesap-special/include/crd/hesap/special/bessel.hpp` | naming on static constants `c1`, `c2`; bugprone-incorrect-roundings twice; readability-avoid-nested-conditional-operator twice; misc-confusable-identifiers `rjp1`/`rjpl` and `rip1`/`ripl` |
| `engine/numerics/hesap-stats/include/crd/hesap/stats/heavy_tail.hpp` | naming on the function-local `constexpr int kPanels` |

## Repairs

- **ckir_lighting.hpp** — `V1`/`V2` → `v1`/`v2` (the Minv-transformed radius vectors of `ltc_evaluate_disk`), the
  function-local static Poisson tables `px`/`py` → `kPoissonX`/`kPoissonY`, and the Peters-Klein three-case switch
  `caseA`/`caseB`/`sA`/`sB`/`sC` → `case_a`/`case_b`/`s_a`/`s_b`/`s_c`. Renames only; the graph the builder emits is
  unchanged, and the kir tests compare every evaluated node with its CPU oracle by exact equality.
- **smolyak.hpp** — `integrate_smolyak` now takes `const F& f`. The callable is invoked once per grid node and never
  forwarded, so a forwarding reference was the wrong parameter kind (Core Guidelines F.19); `const F&` is the
  Boost.Math quadrature convention and rejects a mutable callable loudly rather than silently copying it. The only
  callers are the three cubature tests, which pass stateless lambdas.
- **bessel.hpp** — `c1`/`c2` → `kGam1Coeff`/`kGam2Coeff` (rewrapped inside 120 columns). The two `(int)(xnu + 0.5)`
  casts become one documented helper, `bes_base_steps`, computing `static_cast<int>(crd::math::trunc(xnu + 0.5))`: the
  same binary64 sum, then the same truncation toward zero the C cast performs, so the value is identical for every
  finite in-range input. `lround` was not substituted because it rounds half away from zero and ν = 0.49999999999999994
  gives ν + 0.5 == 1.0 in binary64 (cast → 1, `lround` → 0), and the `cyl_*_prime` entry points pass a negative ν
  through unreflected, where truncation and half-away-from-zero disagree; either change moves the base order μ and every
  downstream bit. The nested conditional of `bessjy` moved into `bessjy_steps`, which keeps the untouched
  `(int)(xnu − x + 1.5)` shift. `rjl1`/`rjp1` → `rjl_nu`/`rjpl_nu` and `ril1`/`rip1` → `ril_nu`/`ripl_nu` (the values at
  order ν saved before the downward recurrence; the `1`/`l` pair was the confusable). The `besselik_asymp` nested
  ternary became a single condition, `(kind < 0.0 || (k & 1) == 0) ? ak : -ak`, with the same operands.
- **heavy_tail.hpp** — `kPanels` → `panels` (function-local `constexpr` is lower_case per CODING).

## Bit-exact proof

A scratch harness (not committed) compiled with `cl.exe` and the `crd-hesap-special-tests` translation unit's own
flags (`/std:c++20 /EHsc /permissive- /utf-8 /arch:AVX2 /fp:precise /Od /MDd`, `CRD_DETERMINISTIC_FP=1`, PCH inputs
stripped) against the built `win-debug` libraries prints the IEEE bit pattern of every public Bessel entry point and of
the `detail` quartets over a grid that hits every changed path: orders at exact halves, at 0.49999999999999994 and
0.5000000000000001, integers, generic reals and negatives through the four `_prime` functions; arguments on both sides
of `XMIN = 2.0` (both `nl` branches), at the J fast-path boundaries 9.0 and 17.5 and beyond; `sph_bessel` with n > x
and n ≤ x; complex I at |z| ≥ 17.5 and complex K at |z| ≥ 9 so `besselik_asymp` runs with both signs of `kind`; and
f32 instantiations. Before and after the edit: 17,070 lines each, SHA-256 `9F82BAB8…9841BB` both times, byte-identical.

## Verification

- Strict gate after the repairs: all four headers **clean**, exit 0 (LLVM 20.1.8, same database).
- Scoped check, `dev.py check --build build/win-debug --path <the four headers> --target crd-kir-tests --target
  crd-hesap-quadrature-tests --target crd-hesap-special-tests --target crd-hesap-stats-tests --jobs 2`
  (`20260913T201249-85220ead352e`): **passed**, `diagnostic subset passed`. Build 61 steps in 27.7 s, relinking
  `crd-hesap-special`, `crd-hesap-quadrature`, `crd-hesap-stats` and the four executables; **539 of 539** selected
  CTests passed (311 kir, 136 stats, 38 special, 35 quadrature and the 19 global guards, among them the
  no-std-math/sort/transcendental checks), zero failed, skipped or disabled,
  serial with the 180 s per-test budget in 578.5 s; both repository guards passed; tidy phase 18.6 s with each header
  analysed through its owning target's unit (`owner:crd-kir`, `owner:crd-hesap-quadrature`, `owner:crd-hesap-special`,
  `owner:crd-hesap-stats`) and clean; evidence integrity verified over 25 artifacts.
- Not built locally: the other consumers of `ckir_lighting.hpp` (`crd-light-cook`, `crd-gpu-context-vulkan`,
  `crd-scene-render` and their tests; `crd-kir` itself has no translation unit including it) and the comms/opt/tensor
  executables that own `heavy_tail.hpp`. Every change there is a function-local or function-local-static rename, so no
  consumer's source can observe it; the one signature change (`integrate_smolyak`) has no caller outside the built
  cubature tests. CI's affected Windows/Linux lanes remain the cover for those consumers. Later the same evening the
  six consumer test executables (`crd-light-cook-tests`, `crd-gpu-context-vulkan-tests`, `crd-scene-render-tests`,
  `crd-hesap-comms-tests`, `crd-hesap-opt-tests`, `crd-hesap-tensor-tests`) were built in `build/win-debug` with two
  workers through `scripts/build-target.bat`: every one linked with zero compiler diagnostics, so the hosted lanes are
  not the first compile of the repaired headers.
- Documentation validator after the ROADMAP, context and session edits: **PASS** (863 rows, 1,034 documents, 8,923
  local links). Its first run failed only on the `context.md` 2,500-byte orientation budget, which the new handoff link
  had pushed to 2,540 bytes; the rolling "Latest handoffs" list rotated out the oldest entry (the guard/tidy repairs
  note, already evidence on rows 037 and 039), leaving 2,455 bytes.

## Recorded, not repaired (outside the user's scope)

Running the same gate on three sibling quadrature headers to choose a consistent callable convention showed the same
pre-existing pattern: `cppcoreguidelines-missing-std-forward` on `F&& f` in `integrate.hpp` (`apply_rule` line 59,
`integrate_symmetric` line 96), `gauss_kronrod.hpp` (`gauss_kronrod_21` line 71) and `lebedev.hpp` (`integrate_lebedev`
line 848), plus `misc-redundant-expression` at `integrate.hpp:54`, where `(v == v) && (v - v == T{0})` is the intended
NaN/infinity test. The hosted lane reports none of them. They keep REPO.DEV.3b.3 as their owning row until a
hesap-quadrature row takes them; the fix pattern chosen here (`const F&`) applies to each.

## State

REPO.DEV.3b.3 stays Partial: the Linux positive arm with clang-tidy 20 in WSL is still the remaining item. The four
sampled headers are no longer a caveat of the portable gate's evidence. No commit or push by the agent.
