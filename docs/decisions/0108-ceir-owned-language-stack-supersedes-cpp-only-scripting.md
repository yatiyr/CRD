# ADR-0108 — A Cerid-owned executable-program language stack (CEIR/CHIR); C++ is no longer the *only* authorable program

**Status:** **ACCEPTED** (2026-08-07, user-approved at the CEIR-0b gate) — the D-007 **CEIR band**. This ADR
captures a **user-directed change of direction** (the CEIR universal-programming roadmap,
`docs/research/2026-08-07-ceir-universal-programming-master-roadmap.md` §5). The **decision is now locked**; the
user-facing cornerstone flip (PRINCIPLES/AGENTS/README/ROADMAP) and the in-file ADR-0081 §9 strike are **DEFERRED to
the first CEIR vertical slice** — §5's *second* gate ("accepted AND a real vertical slice exists"), so the repo
never claims a capability before it is real. It is a *surgical* supersession — see §7.
**Supersedes (ON ACCEPTANCE):** ADR-0081 §9 "Note on scripting language" — the clause "**C++ hot-reload is the ONLY
scripting path**" and its "other scripting paths are explicitly rejected." Everything else in ADR-0081 (the
agent-native CLI/RPC/MCP architecture, capability security, replay, the command schema) is **REAFFIRMED, not
touched**. ADR-0081 itself invited this: *"Future revisitation would require a new ADR."*
**Phase:** D-007 (CEIR programme). North star: mission §1.
**Tags:** `[scripting]` `[lang]` `[ceir]` `[chir]` `[ir]` `[agent]` `[architecture]` `[substrate]` `[north-star]`

---

## 1. Context — why the direction changed

ADR-0081 (2026-05-19) locked **C++ hot-reload as the *only* scripting path**: Cerid programs are `.crds.cpp` files
compiled to hot-reloadable DLLs; no embedded interpreter. Its stated reasons — one language for engine + scripts +
tools (no marshaling), the full type system + debugger, and deterministic FP — **still hold, and this ADR preserves
every one of them** (§3's non-negotiables: deterministic time/RNG, C++ FFI, no third-party VM). What changed is not
the rationale but the **requirement set**: CEIR adds requirements a compiled `.crds.cpp` DLL structurally cannot
meet.

**CEIR changes the premise.** The mission's north star (§1) is that **every reusable algorithm expressible from
capabilities Cerid already understands is a versioned, inspectable, serializable, hot-reloadable program ASSET** —
rendering, compute, RT, ML, media, geometry, UI, physics, animation, audio, *and* application logic. An algorithm-
as-asset must be:

- **authorable as data** (text, a CR-D007 visual graph, a domain frontend, a C++ builder — all four projections of
  one semantic model, §9 §121),
- **inspectable + serializable + diffable** (semantic IR, not opaque compiled code, §10 §123),
- **hot-reloadable + agent-authorable + machine-inspectable** (§108 §161),
- **lowerable to the best legal CPU/GPU/NPU/provider strategy** by a compiler (§69 §144).

**A `.crds.cpp` DLL cannot be any of those.** Compiled C++ is not an inspectable semantic graph; it cannot be
visually authored, semantically diffed, verified by a Cerid verifier, partially evaluated per evaluation domain, or
lowered to a GPU/NPU provider. "C++ is the only authorable program" therefore blocks the entire CEIR mission: it
would mean every renderer architecture, GI algorithm, tensor graph, and UI effect must remain bespoke C++ — the
exact `ForwardPlusExecutor`-as-native-code outcome §100/§178 forbid.

So the change is not "add a scripting language" — it is: **the engine gains a Cerid-owned executable-program
representation (CEIR) and, above it, a Cerid-owned high-level language layer (CHIR), because algorithms-as-assets
require a representation C++ compiled to a DLL cannot provide.**

## 2. Decision (the precise clauses — mission §5)

1. **C++ remains a first-class native extension + programmatic-authoring language.** Application programs, engine
   modules, and providers may still be written in C++. The C++ builder API emits ordinary canonical CEIR (§121).
2. **C++ hot-reload remains useful and supported** (ADR-0081's `crd-script` DLL supervisor is not deleted).
3. **Cerid gains a Cerid-OWNED textual + visual programmable language stack:** CEIR (the execution IR, this
   detour) and CHIR (the high-level language layer, design-only until CEIR-29). Both are **Cerid-owned** — no
   external IR is the internal architecture (§124 §183).
4. **No Lua / Python / JavaScript / GDScript / Wren runtime dependency for the core language.** The rejection of
   *embedded third-party interpreters* from ADR-0081 **stands** — CHIR is Cerid's own language, not a wrapped VM.
5. **C++ is no longer the *only* authorable executable-program representation.** This is the single clause of
   ADR-0081 §9 that is superseded. Textual CEIR/CHIR and CR-D007 visual programs are equal, first-class authoring
   surfaces (§9 §180).
6. **CLI / RPC / MCP remains the source-of-truth automation surface** (ADR-0081 §1-§8, fully reaffirmed). Program
   assets are **agent-authorable and machine-inspectable** through it (§122 §161): agents discover dialects/ops,
   query signatures, build graphs, verify, compile, and save assets semantically — not only by patching text.
7. **Program assets get canonical asset identity** (`engine://` · `app://` · `plugin://` · `runtime://`, §105) and
   participate in the cook/hash/hot-reload lifecycle (§106-§110).

## 3. The language non-negotiables — pinned NOW (mission §98 §99), so nothing built in the interim precludes them

Even though CHIR implementation is far (CEIR-29), its load-bearing constraints are fixed now so CEIR is built as a
valid target for them:

- **No mandatory tracing GC in real-time hot paths** (§19 §98). Deterministic lifetimes: value semantics /
  ownership-borrowing / arenas / handles / region lifetimes / explicit state stores. (Editor/offline tiers may be
  richer; the shipping hot path is allocation-disciplined, §153.)
- **Deterministic time + RNG APIs** (§27 §58). Randomness and time are explicit, counter-based, replay-recordable —
  the ADR-0063 determinism contract survives into the language.
- **Capability-based security** (§99). A program asset declares requested capabilities (`capability.scene.read`,
  `capability.gpu.compute`, `capability.file.write`, `capability.audio.rt`, `capability.external.native`, …); the
  host grants them; untrusted/agent-authored programs are sandboxed. This *is* ADR-0081 §4's capability set,
  generalized from CLI sessions to program assets.
- **Native FFI / intrinsics with versioning** (§100). C++ is reachable as versioned native intrinsics (the escape
  hatch), each declaring effects/domain/determinism/capabilities — never arbitrary pointer/OS access through a
  generic op.
- **Structured concurrency** (§30), **Result/Option typed failures** (§29), **unit-aware quantities** (§17 — the
  ADR-0078 two-layer discipline survives), **hot reload with state migration** (§108 §109) — all language-roadmap
  requirements the IR must not block.

## 4. What this is NOT

- **Not "scripting next month."** CHIR is a real language project (parser, semantic analysis, trait solving,
  ownership, diagnostics, tooling) — Rust-frontend-class work, sequenced at **CEIR-29, after the CEIR corpus
  exists** to teach the language its ergonomics (mission §4). This ADR reverses one clause; it does not schedule a
  language for the near term. The near-term deliverable is CEIR (the execution IR), on which asset authoring, the
  visual editor, and provider orchestration all run without CHIR.
- **Not a third-party runtime.** CEIR/CHIR are Cerid-owned; importers (ONNX/StableHLO/MaterialX) are compatibility
  paths, never the internal semantics (§124).
- **Not a repudiation of C++.** C++ stays first-class for native extension, providers, and programmatic authoring.
  The change is that C++ is no longer the *sole* way to express a new *algorithm*.

## 5. Relationship to CEIR / CHIR / CKIR (the layer contract — detailed in ADR CEIR-0c)

```
authoring: text · CR-D007 visual · domain frontends · C++ builder · importers   (four projections, one semantics)
        ↓
CHIR — Cerid high-level language layer (generics/ADTs/closures/ownership/…)      (this ADR authorizes it; built CEIR-29)
        ↓
CEIR — Cerid Execution IR (typed SSA + regions + effects + capabilities)         (this detour)
        ↓
CKIR (per-invocation kernels, UNCHANGED) ∥ execution providers                   (ADR-0101/0103, referenced by identity)
```

CKIR (ADR-0101/0103) is untouched — it remains the per-invocation kernel IR. CEIR references CKIR programs by
content-hash identity and may generate CKIR during lowering (§85).

## 6. Consequences

**Positive:** algorithms become inspectable, diffable, hot-reloadable, agent-authorable assets; renderer
architectures / GI / tensor / ML / UI-effect / media programs stop being bespoke C++ (§178); one compiler chooses
CPU/GPU/NPU lowerings; the CR-D007 visual language and a future textual language share one semantic model; the
agent-native surface (ADR-0081) gains a semantic program substrate to author, not just text to patch.

**Negative / risk:** this authorizes a large multi-year language effort (CHIR); the risk is scope-creep pulling
CHIR forward before the CEIR corpus justifies it (mitigation: CEIR-29 sequencing + §4's "not scripting next month");
and a compiler/IR is a different engineering discipline than the kernel work Cerid has excelled at (mitigation: the
§167-§172 test matrices — round-trip fuzz + reference-executor differentials — are built from CEIR-1, not bolted on).

## 7. Supersession mechanics — surgical, and deferred to acceptance (mission §5, house SUPERSEDED rule)

- **Scope of supersession:** ONLY ADR-0081 §9's "C++ is the ONLY scripting path" + "other scripting paths are
  explicitly rejected" clause. ADR-0081 §1-§8 (agent-native CLI/RPC/MCP, capability security, command schema,
  replay, phasing) is **reaffirmed verbatim**.
- **Acceptance gate ✅ MET (2026-08-07, user-approved).** The four edits below now await ONLY §5's *second* gate —
  the first CEIR vertical slice (a working CEIR program asset, ≈ CEIR-10z / CEIR-13). Until then the decision is
  recorded (this ADR = Accepted; the `docs/decisions/README.md` rows) but the operative rule in PRINCIPLES/AGENTS/
  README/ROADMAP still reads C++-only, because C++-only is still what is *operationally* true (no other authoring
  path exists yet). This is deliberate — the mission author chose it so docs describe reality, not aspiration.
- **⛔ DEFERRED to the first CEIR vertical slice (§5 gate 2), then executed as ONE coordinated commit:**
  1. Editing `docs/PRINCIPLES.md` — the "C++ hot-reload is the ONLY scripting language" cornerstone (~lines 149-160).
  2. Editing `AGENTS.md` and root `README.md` where they state the C++-only rule.
  3. Editing `docs/ROADMAP.md` — the Strategic Execution Plan's "locked 2026-05-19; revisiting requires a new ADR"
     note (now marked historical, but it carries a third live copy of the reversed rule).
  4. Striking ADR-0081 §9 in place (the house SUPERSEDED-clause-struck-in-place rule) + adding a `Superseded-by:
     ADR-0108` line to ADR-0081's header + updating its decisions-README row.
  Until the vertical slice, PRINCIPLES/AGENTS/README/ROADMAP correctly still state the C++-only rule (it is what is
  operationally true), and ADR-0081 §9 stays un-struck-in-place — a coherent record because THIS ADR (Accepted) +
  the decisions-README rows already point forward, so a reader tracing the decision finds the reversal.
- **On the first CEIR vertical slice:** execute the four deferred edits above as one coordinated commit; no further
  ADR status change (this ADR is already Accepted).

## 8. References

- `docs/research/2026-08-07-ceir-universal-programming-master-roadmap.md` — §5 (this ADR's mandate), §98 (language
  feature set), §99 (capability security), §4 (why CHIR is above CEIR), §100 (native intrinsics), §19 (ownership).
- ADR-0081 — Agent-native engine (the decision this surgically amends; its §9 is superseded on acceptance, §1-§8
  reaffirmed).
- ADR-0101 / ADR-0103 — the IR-is-source-of-truth + gpu-context-owns-every-program decisions CKIR rests on.
- ADR-0063 — determinism contract (survives into the language's deterministic time/RNG).
- ADR-0078 — units substrate (survives as §17 unit-aware quantities).
- The forthcoming CEIR-0c (CEIR/CHIR/CKIR ownership) + CEIR-0d (native-intrinsic schema) + CEIR-0e (CHIR-0 language
  design note) ADRs/notes refine this direction.
