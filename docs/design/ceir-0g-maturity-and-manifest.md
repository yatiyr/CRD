# CEIR-0g — One maturity model + the §174 machine-readable manifest

> **Band:** D-007 · CEIR-0 · slice 0g. **Tracker row:** `docs/detours/D-007-ceir-tracker.md` → CEIR-0g.
> **Gate:** one maturity model; a registry-migration plan. **Law:** mission §173 (CEIR L0–L8), §174 (manifest).
> **Reconciles:** the post-RAF L0–L7 model (D-007 §PR-3, live in `docs/capabilities/gpu-platform-capabilities.toml`)
> with the CEIR L0–L8 model (§173). **Status:** ✅ ACCEPTED 2026-08-08. This is a design/plan note — it does
> NOT rewrite the registry (that is the mechanical migration it plans; executed when the generator lands).

---

## 1. The finding — the two scales measure DIFFERENT axes (so "merge" ≠ "renumber")

Left column verified against §PR-3's own ladder (`docs/detours/D-007-gpu-program-system.md:530`), not the TOML's
paraphrase — they match.

| | post-RAF L0–L7 (§PR-3, verified) | CEIR L0–L8 (§173) | relationship |
|---|---|---|---|
| **measures** | how mature a RENDER FEATURE is **as a RAF asset** (`.frame.toml`/`.crdm`/…) | how mature a CAPABILITY is **as a CEIR program** | different axes |
| L0 | catalogued | listed / research target | aligned |
| L1 | math/oracle exists | semantics / ADR defined | **redefined**, same ordinal |
| L2 | mechanic (command/executor) exists + structural tests | IR op + types + verifier landed | **redefined**, same ordinal |
| L3 | one-backend end-to-end | reference / one-provider execution | aligned |
| L4 | cross-backend proven | **optimized on primary backend** | ⬅ CEIR **INSERTION** (the one new rung) |
| L5 | shipped `engine://` asset | Vulkan + D3D12 / multi-provider proof | RAF-L4 → CEIR-L5 (offset begins) |
| L6 | live-authorable (needs CR-D007) | shipped `engine://` program asset | RAF-L5 → CEIR-L6 |
| L7 | production-qualified | CR-D007 authoring + hot reload + diagnostics | RAF-L6 → CEIR-L7 |
| L8 | — | production stress/perf/determinism qualification | RAF-L7 → CEIR-L8 |

**They are not the same ruler.** Precisely: **CEIR L4 (optimized-on-primary-backend) is the ONE genuine insertion** —
it creates a +1 offset from L4 upward (RAF-L4 cross-backend → CEIR-L5, and shipped-asset/authorable/qualified each
shift +1). L1 and L2 are **same-ordinal redefinitions** (RAF's math-oracle + mechanic rungs become CEIR's
semantics + IR-op rungs — a CEIR feature's "exists in code" milestone is an IR op with a verifier, not a command).
L0/L3 align directly. And the axes themselves differ: a shipped RAF renderer (`forward_basic`, RAF-**L5**) is, as a
CEIR program, **CEIR-L0** — CEIR doesn't exist yet, so nothing is a CEIR program. A feature legitimately holds
**both** levels at once during the transition.

## 2. Decision — ONE forward model (CEIR L0–L8), a TWO-AXIS transition, convergence at CEIR-13

- **The one unified maturity model going forward is CEIR L0–L8** (§173, verbatim) — the mission's model, and the
  more granular of the two (the RAF scale is a strict coarsening).
- **During the transition (now → CEIR-13z), a registry feature carries TWO levels:**
  - `raf_level` (0–7) — *current reality*: how the feature exists TODAY as a RAF asset / shipped render capability.
    This is the honest "what runs right now" claim (the registry's existing `level`, renamed).
  - `ceir_level` (0–8) — *the forward track*: how far the feature has climbed as a CEIR program. **Every existing
    feature starts at `ceir_level = 0`** (nothing is a CEIR program yet) and climbs as CEIR-10…13 migrate it.
- **Convergence:** when CEIR-12f/13z delete the RAF frame/executor paths (ADR-0106 superseded), the RAF asset IS a
  CEIR program, `raf_level` is retired, and `ceir_level` (renamed back to `level`) is the sole scale. The registry
  header's "nothing exceeds L5 / L6 needs CR-D007" honesty note maps forward to CEIR-L6/L7.

This preserves the registry's cardinal rule — **the level is the honest claim, prose never exceeds it** — while
adding the CEIR track without falsely promoting shipped-RAF features to a CEIR maturity they haven't earned.

### 2a. How `ceir_level` reads per classification (§PR-4: A / A+R / A+E / B / T)

The registry classifies every feature; the CEIR axis means different things per class, and **not every class
converges**:

| Class | What `ceir_level` measures | Converges? (does `raf_level` retire at 13z?) |
|---|---|---|
| **A** pure-asset · **A+R** asset+runtime · **A+E** asset+executor | how mature the feature is as a CEIR program asset (the standard L0–L8) | **Yes** — the RAF asset becomes a CEIR program; `raf_level` retires |
| **B** canonical/backend (a command family / backend capability) | the `ceir.render`/`ceir.compute`/… **op** representing this command family has landed (the CEIR-11-era rung) — it is IR vocabulary, not a program | Partly — the op lands, but the backend capability itself stays a provider fact; keep both axes |
| **T** tooling (matrix generator, capture, profiling) | **`ceir_level = "n/a"`** — tooling is never a CEIR *program*; it has only a RAF/functional maturity | **No** — `raf_level` (or a functional-maturity field) stays; there is no CEIR track |

So the §2 "at CEIR-13z `raf_level` retires" claim is scoped to the **converging classes (A/A+R/A+E)**; B keeps both
axes, T keeps only its functional maturity. The migration (§4) must not blank-fill `ceir_level = 0` on T-class rows —
they get `"n/a"`.

## 3. The §174 manifest field set — the current TOML + two additions

The existing `gpu-platform-capabilities.toml` already carries most of §174 (id · category · definition · level ·
classification · dependencies · ckir_capabilities · command_families · executors · runtime_systems ·
backend_capabilities · assets · tests_vulkan · tests_dx12 · tests_cross_backend · hot_reload_tests · editor_support ·
quality_perf_gates · fallback · band · status · references · limitations). Against §174's required list
(capability · maturity · assets · tests · providers · fallbacks · editor support · **determinism tier** · perf
board), **two fields are missing and are added by the migration:**

| Added field | Why | Source |
|---|---|---|
| `providers` | §174 + §69 — which CEIR **execution provider(s)** run this capability (host / gpu / npu / media / …). The RAF registry tracked `executors`/`runtime_systems`; CEIR needs the provider axis. | ADR-0109 §4 (the bridge/provider model) |
| `determinism_tier` | §174 + §27 — stores §27's **five determinism classes** (BitExact / DeterministicWithinTarget / DeterministicWithinBackend / Nondeterministic / ExternalNondeterminism). A first-class Cerid product property that must be machine-readable. **CEIR-4b owns the alignment** of these five classes to ADR-0098's T1/T2/T3 tiers (they are NOT 1:1 — five classes vs three tiers); this note does not fix that mapping. | §27 classes; CEIR-4b aligns |

Plus the level-field change from §2: `level` → `{ raf_level, ceir_level }` during transition, reconverging to
`level` post-CEIR-13. `schema` bumps `1 → 2` (§104 versioning — old readers see the bump).

## 4. Registry-migration plan (mechanical; executed later, NOT in this slice)

1. **Rename** `level` → `raf_level` on every `[feature.*]`; **add** `ceir_level` — `0` for A/A+R/A+E/B features
   (honest — no CEIR programs exist yet), **`"n/a"` for T-class** (§2a). `schema = 2`.
2. **Add** `providers = []` + `determinism_tier = ""` to every feature (empty until CEIR classifies them).
3. **Extend the field contract** comment block + the honesty header (map the L5/L6 notes onto the two axes per §2).
4. **The generator** (`docs/generated/gpu-platform-capability-matrix.md`, not yet built — noted in the TOML header)
   emits the matrix from the manifest, flagging: missing backend coverage · assets-without-tests · claimed-CEIR-L6+-
   without-hot-reload/editor · missing `providers`/`determinism_tier` on a CEIR-L2+ feature · stale refs.
5. **As CEIR bands land**, converging features (A/A+R/A+E) gain `ceir_level` + `providers` + `determinism_tier`; at
   CEIR-13z, `raf_level` retires **for the converging classes only** and their `ceir_level` → `level` (a second
   schema bump, `schema = 3`, per §104). B keeps both axes; T keeps its functional maturity.

**Canonical home (one-home-per-fact):** after this note, three docs describe maturity ladders. The resolution:
- **§173 stays THE LAW** (the CEIR L0–L8 definition);
- **the registry header** (`gpu-platform-capabilities.toml`) becomes the **operational home** of the two-axis
  definitions + the honesty rule (it is already the machine-readable source of truth);
- **D-007 §PR-3 gets a superseded-pointer** to the unified model — ⚠ **this is a CEIR-0f action** (0f edits D-007):
  strike §PR-3's standalone ladder in place, point it at §173 + the registry header, so a third live ladder does not
  drift. (Recorded here so 0f's checklist includes it.)

**When:** the field additions (steps 1–3) can land any time (a small, mechanical, additive TOML edit — a good
low-risk task); the generator (step 4) is its own tooling slice (T-class), reasonably deferred until the manifest
has CEIR entries to matrix. This note is the plan; it does not perform the edit.

## 5. Open questions

1. Does any *shipped* RAF feature deserve a `ceir_level > 0` before CEIR exists? **No** — by §2's definition CEIR
   maturity requires a CEIR program; pre-CEIR everything is `ceir_level = 0`. Stated so no one is tempted to
   back-date it (the aspirational-docs trap).
2. Should `raf_level` and `ceir_level` ever be a single "max" number for a quick-glance matrix? **No** — they are
   different axes (§1); collapsing them re-introduces exactly the false-promotion this note prevents. The generated
   matrix shows both columns.
