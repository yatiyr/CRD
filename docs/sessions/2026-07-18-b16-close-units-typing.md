# 2026-07-18 — B16 close-out: the full-sweep onion + a gold-standard units typing of the CKIR configs (D-007)

Detour D-007 (GPU program system / CKIR). This session's job was narrow on paper — **close B16 properly (no debts,
no problems)**: tidy the touched files + run the per-slice sweep on the displaced-ocean + mesh work from 2026-07-15/16.
Running the FULL sweep for the first time on the accumulated (uncommitted) B14/B15/B16 tree **peeled a large tidy onion**
of pre-existing issues that prior sessions had missed by verifying with test *binaries* instead of full `ctest` + shipping.
Every one was root-caused and fixed at the source (SANITY doctrine, no debt). Then, at the user's direction, the last
issue (untagged physical fields) was resolved by a **full gold-standard units typing** of the CKIR simulation/GI configs.

## The onion (five issue classes, all pre-existing, all fixed)

1. **Tidy — 36 touched/new files.** Naming: the B4 mesh-emitter locals (`V`/`P`/`LS` → `n_verts`/`n_prims`/`local_size`
   in `ckir_glsl.hpp`/`ckir_hlsl.hpp`), the promoted `ocean_grid` namespace constants → **`k`-prefixed CamelCase**
   (`kEyeH`/`kFovx`/… — GlobalConstant is `CamelCase` **with** a `k` prefix; the earlier grep had hidden the prefix line),
   `kVsFs`→`vs_fs`, `dhS/dhC`→`dh_s/dh_c`, `NC/foamA/foamB/gridN`→lower_case, and one dead `mx` lambda removed. All via
   the CI-faithful LLVM-20 `tidy-files.ps1` fatal gate.

2. **12 non-ASCII TEST_CASE names** (`à-trous`, `α`, `→`, `×`, em-dash) across `test_ckir_svgf/ddgi/ocean/restir.cpp` +
   `test_vulkan_context.cpp` + `test_dx12_compute.cpp`. This both failed the `crd-no-non-ascii-test-names` guard AND made
   ctest report those 12 as *Failed* (CP1254 argv mojibake can't select the name) — the exact `feedback_ascii_only_test_names`
   scar. Fixed to ASCII on the flagged lines only (a Python pass — the files legitimately use non-ASCII in comments).

3. **`std::pow`×3 in `ckir_ocean.hpp`** (host-side JONSWAP constants) → `crd::math::pow` (Math Mandate). Bit-safe: the
   results are stored as F32 IR constants, so the sub-ULP f64 difference rounds away.

4. **win-shipping build failure — traced to the `#deps 0` landmine, not a code bug.** It first showed as `C4789`
   (compile-time buffer-overrun) at `ckir.hpp(836)` during LTCG: MSVC's model of `KGraph` was **136 bytes = the OLD
   pre-`m_stmts` layout**, while the current code writes `m_stmts`/`m_ninput` past it. Root cause: `build/win-shipping`
   carried the **English `msvc_deps_prefix`** ("Note: including file:") + a **VS-bundled `CMAKE_COMMAND`** (it re-armed
   itself during the earlier GLOB reconfigure) on this Turkish-locale host, so header changes never recompiled → a stale
   obj built against the old KGraph. **All other config dirs had the correct Turkish prefix** (which is exactly why only
   shipping failed). Fixed per doctrine: **wiped `build/win-shipping` + reconfigured with the standalone CMake**
   (`configure-preset.bat`) — never `sed` the prefix ([[feedback_stale_toolset_path_in_build_dir_wipe_dont_sed]],
   [[feedback_header_struct_layout_change_stale_obj_config_specific_fail]]).

5. **Untagged-physical guard** — resolved by full units typing (below).

## The units decision — gold standard, not ceremony (user-directed)

The `crd-no-untagged-physical-numeric` guard flagged 11 bare-`double` physical fields in the CKIR sim/GI configs. Digging
in revealed the guard is a **name-heuristic** (it flags `_radius`/`_height`/`wind_speed`/`depth_*` but *misses* `ozone_center`,
`ap_km_max`, the `patch_length[]` array, and the 1/km coefficients — all physical), and that ~half the "physical" fields are
actually **dimensionless tuning knobs** (SVGF sigmas, ocean foam/choppiness, DDGI hysteresis, a cosine-power `depth_sharpness`).
The user (after two rounds) asked for the **gold-standard call**. The principle applied:

> A units type earns its keep by encoding one real invariant — the physical dimension. Type where a dimension exists (real
> Mars-Orbiter safety); a genuinely dimensionless number's honest type IS a raw scalar (wrapping it in `Dimensionless<f64>`
> catches nothing — pure ceremony). ADR-0078 says exactly this: units at the boundary, raw scalars in the kernel/GPU interior.

So — every field with a **real dimension** is typed, across **all four configs**:
- `ckir_atmosphere.hpp`: 8 `Length` (km) + **8 `InverseLength`** (1/km extinction/scatter — a custom
  `Quantity<DimInv<dim::Length>, f64>`, no stock alias). Stored in SI, converted at the builder boundary via file-local
  `km()/in_km()/per_km()/in_per_km()` helpers. **Every value round-trips bit-exact** (×1000⇄÷1000 verified for the whole
  Earth-reference set), so the baked IR constants — and GPU==oracle bit-exactness — are unchanged.
- `ckir_ocean.hpp`: `patch_length`/`fetch`/`depth`/`small_wave` → `Length`, `wind_speed` → `Velocity`, `gravity` →
  `Acceleration`, `wind_dir` → `Angle` (+ the cascade `patch_length[]` array). SI-native ⇒ `.value` is identical.
- `ckir_ddgi.hpp`: `origin_x/y/z`/`spacing` → `Length`.
- Dimensionless tuning knobs stay raw `f64`; the two heuristic **false-positives** (`depth_sharpness` cosine power,
  `depth_reject` normalized threshold) get an honest `crd-lint-allow-untagged-physical` rationale marker.

Builder read sites (~90) use `.value` / `value_in` at the boundary; the ~20 test-side assignments (incl.
`test_vulkan_context.cpp`) were wrapped in the right Quantity type. **Guard green; bit-exactness held** — `[atmos]` 1562,
`[ocean]/[ddgi]/[svgf]/[restir]` 4246 assertions, and the GPU-dispatch-vs-oracle suites, all pass unchanged.

## Verification (user-optimized)

win-debug had already run the **full 5061-test suite green** (complete functional coverage). Per the user's call — CI owns
the full-matrix regression sweep; the other configs only add *config-specific* risk that can only come from the touched
code — the rest was a **focused build+test of the 7 touched targets** (`crd-kir-tests`, `crd-kir-{vulkan,dx12,webgpu}-tests`,
`crd-gpu-context-{vulkan,dx12}-tests`, `crd-rhi-vulkan-tests`) on **win-shipping (clean LTCG build = the C4789 validation),
win-asan (UAF/OOB), win-release (LTCG)**, plus a win-tidy build. All green. Byte-exact HELD (kir 34725 · vk 33023 · dx12
30821). Guards all green.

⚠ **Transient MSVC LTCG `C1001`** at `ckir_restir.hpp:162` in the win-release link — a compiler ICE, not our code (untouched
file; win-shipping's identical LTCG codegen linked it fine) — **cleared on a retry-clean**, exactly the known upstream flake
BUILDING.md documents ("close on retry-PASS, do not re-sweep").

## Status
- **B16 ◧ DoD-CLOSED** — displaced-ocean core + mesh path green all configs; SSR/refraction/underwater/caustics
  (B16-a-1/B16-b) **deferred** per the move to B4→B17.
- The whole uncommitted B14/B15/B16 + B4 batch is now clean and ready to commit (the user commits).

## Next
- **B4 — DX12 device mesh render.** Full ready-to-apply design authored (scratchpad `b4-dx12-mesh-render-draft.md`):
  IR→DXIL mesh on-ramp (`ms_6_5`, 3 `dx12_context.cpp` edits), `OPTIONS7 MeshShaderTier` gate + `Device2`/`CmdList6`, a
  hand-rolled mesh PSO stream (no CD3DX12 in the backend), `create_mesh_program` + `draw_mesh`, then the bindless-depth
  ocean-meshlet variant + a DX12 mesh test (debug-layer clean; new-capability ⇒ run both Vulkan validation + DX12 debug
  layer). Then B4 remaining (TASK stage, B4-vis visibility buffer, B4-tess, WGSL `texture_2d_array` cascade) → **B17 OIT**.

## Lessons (durable)
- **Run the full ctest+shipping at a real close — a green test binary is not a green slice.** Prior sessions shipped
  non-ASCII names, `std::pow`, and untagged fields because they verified with binaries; the close-out full sweep is what
  surfaced all of it (the "onion" — [[feedback_full_sweep_after_uncommitted_work_peels_tidy_onion]]).
- **The `#deps 0` landmine recurs** whenever a VS-bundled-CMake reconfigure touches a build dir on this locale — a
  config-specific stale-obj (here a C4789 from a header struct-layout change) is its fingerprint; audit the prefix, wipe,
  reconfigure with standalone CMake.
- **Gold standard = the type tells the truth**, not "wrap everything." Units where a dimension exists; raw where it
  doesn't. Typing dimensionless shader knobs in `Dimensionless<f64>` is ceremony that catches nothing.
