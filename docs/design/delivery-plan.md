# Delivery plan: grouped, dependency-ordered master table

<!-- doc-role: reference -->
> Accepted order for every row of [ROADMAP](../ROADMAP.md); status lives only there. Rules: [AGENTS](../../AGENTS.md).
> Applied 2026-10-27 with the ADR-0129 amendment; the Later state is retired and the validator rejects it. New rows
> are placed right after their last prerequisite, inside their family group.

## Summary

The user rejected the order of 2026-10-27: about 330 rows were parked as Later and never re-planned, and backbones
trailed their consumers (the physics engine EYLEM, about 95 rows, sat after science and media). On 2026-10-27 the user
accepted this plan and all twelve decision items, tightening six of them, with the standing order: no easy paths, the
full gold-standard way. The plan places every one of the 1068 rows of commit da0a5812 (RESEARCH-ADOPT Done) and the
16 new real-hardware rows the applier inserts (1084 rows) into 42 groups with no Later state:

1. History and the user-fixed front (current work, the NETCRASH window, security and supply, AUD-2, QUAL.0, the RAH
   redesign and the GEOM block), unchanged row for row.
2. Foundation backbones: allocator, container and jobs hardening with the ARM64 port; the CHIR language, reflection,
   commands, scene/ECS/world, documents, scripting, extensions and agent surface; hesap CPU and GPU, the GPU compute
   programs and the machine-learning core; SDF, B-rep, analysis meshing and the FE/FV cores; animation core.
3. The physics engine EYLEM as one contiguous family with its numeric qualification, before the renderer library.
4. The renderer runtime library, crd-ui, the CR-D007 editor, publishing and the desktop release.
5. Notebook and science (with physics visualization and the PHYS-QUAL close), AI products, media and audio, the macOS
   and web releases with the simulator halves of mobile and consoles.
6. The verticals in priority order, each right after the last backbone it needs.
7. Operations, then the real-hardware halves of every hardware-gated row, then the cross-domain portfolio.

The machine form is `build/ci-final/roadmap-order.json`; `build/ci-final/apply_roadmap_order.py` applies it (see
[Applying the plan](#applying-the-plan)). The dry run on a copy of the live table passes the applier's checks and the
repository validator.

## Ordering rules

1. **Dependencies first.** The order is a stable topological order of the Requires cells after the corrections below.
   A Requires cell that names a parent row carries that parent's whole closure; such cells were corrected where only
   part of the parent is a real prerequisite. Missing real prerequisites were added, so the validator enforces the
   placements instead of table position.
2. **Backbones before consumers.** A system other systems call precedes every row that consumes it. Between two
   backbones the consumer goes second.
3. **Families stay contiguous.** A family splits only when a child needs something that must come later (or earlier)
   than its siblings; every split is listed with its reason. Follow-on rows (DEBT-*, MAINT-*, SANITY-*, CONFIG-*,
   D-00x, phase-numbered 3.1.x, 4.2x, 6.x, 8.x) sit inside the family they belong to.
4. **Verticals after their engines,** in the priority order games and networking, collaboration, content, DAW,
   cinematics, CAD/CAE, simulation and robotics, each right after the last backbone it needs.
5. **Platforms where their prerequisites are met.** ARM64 in the foundation; macOS and web after the desktop product
   they qualify.
6. **Hardware split (user decision).** A row that needs hardware or SDK access the project does not have is split:
   its implementation and its qualification on emulators, simulators, software adapters and public SDKs run in the
   existing row, right after its prerequisites; the real-hardware qualification is a new `-HW` row (or ROCM.2) in
   P40, at the end, so a missing device never halts the strictly sequential table. The `-HW` IDs are not dot-children,
   so no parent is dragged to the end.
7. **Clauses that wait for a later consumer** (a UI adapter, a macOS backend) stay in their row as an explicit clause
   that names the consuming row, and the consuming row carries a cross-linked clause and Requires the source row.
8. **Formal conformance.** Every row that implements a design FORMAL.1 models re-checks the model against the
   implemented design; a design implemented before FORMAL.1 is validated by FORMAL.1 itself.
9. **No Later.** Every Later row becomes Open in its position. Review rows stay Review where their decision is needed.
   Done, Recorded, Partial and Needs CI keep their state; Done stays at the front.
10. **Nothing dropped, merged or shortened.** Row text, anchors and sources are kept byte for byte; the only changes
    are the Order number, Later states, the listed Requires cells and the appended clauses listed below.
11. **Validator gates kept:** every `.runtime` row before RENDERER-RUNTIME, every I2D-/SPR- row after it, every
    `.authoring` row after I2D-9, CR-D007 before NOTEBOOK, parents after their dot-children.

## Groups in order

"Positions now" are positions in today's 1068 rows; the applier inserts the 16 real-hardware rows into P40.

| Group | Positions now | Rows now | Rows with pending | Name |
| --- | --- | --- | --- | --- |
| P00 | 1-92 | 92 | 92 | Completed history (Done and Recorded) |
| P01 | 93-111 | 19 | 19 | Current front (user-fixed): adoption, CPU dispatch, resilience, diagnostics, ownership |
| P02 | 112-130 | 19 | 19 | NETCRASH window: secure network foundation and crash-to-fix intake (user-fixed) |
| P03 | 131-140 | 10 | 10 | Security, supply chain and the audit gate (user-fixed) |
| P04 | 141-195 | 55 | 55 | RAH renderer redesign (user-fixed) |
| P05 | 196-208 | 13 | 13 | Computational geometry backbone (user-fixed) |
| P06 | 209-231 | 23 | 23 | Foundation hardening: allocators, containers, jobs and the ARM64 port |
| P07 | 232-248 | 17 | 17 | Language and reflection backbone (CHIR, reflection, language services) |
| P08 | 249-255 | 7 | 7 | Command, transaction and MCP surface |
| P09 | 256-271 | 16 | 16 | Scene, ECS, scientific meaning and large worlds |
| P10 | 272-276 | 5 | 5 | Documents and shared reload |
| P11 | 277-284 | 8 | 8 | Scripting and native execution tiers |
| P12 | 285-289 | 5 | 5 | Extensions, packages and signing |
| P13 | 290-298 | 9 | 9 | Agent surface and import trust |
| P14 | 299-308 | 10 | 10 | Numerics CPU completion (hesap follow-ons) |
| P15 | 309-333 | 25 | 25 | GPU compute programs and hesap GPU |
| P16 | 334-341 | 8 | 8 | Machine-learning core: model assets, inference and training |
| P17 | 342-359 | 18 | 18 | Geometry extensions and discretization: SDF, B-rep, analysis meshing, FE and FV cores |
| P18 | 360-367 | 8 | 8 | Animation core |
| P19 | 368-462 | 95 | 95 | Physics engine EYLEM, navigation crowds and physics qualification |
| P20 | 463-553 | 91 | 91 | Renderer runtime library and offline rendering (RENDERER-RUNTIME gate) |
| P21 | 554-627 | 74 | 74 | Input, windows and the owned UI (crd-ui) with the CR-D007 shell |
| P22 | 628-646 | 19 | 19 | Scene and prefab document reconciliation for the editor |
| P23 | 647-677 | 31 | 31 | CR-D007 editor, agent control, guarded crash fixing, native authoring, onboarding |
| P24 | 678-693 | 16 | 16 | Publishing, stores, live operations, mods, the extension marketplace and the Cerid store |
| P25 | 694-700 | 7 | 7 | 2D games and world-space UI |
| P26 | 701-814 | 114 | 114 | Visual authoring acceptance of every renderer family |
| P27 | 815-831 | 17 | 17 | Desktop release qualification and CR-D007 |
| P28 | 832-859 | 28 | 28 | Scientific language, REPL, notebook, scientific visualization and physics visualization |
| P29 | 860-869 | 10 | 10 | AI products: applied workflows, game brains and the combined AI release |
| P30 | 870-903 | 34 | 34 | Media and audio engines |
| P31 | 904-929 | 26 | 26 | macOS and web releases |
| P32 | 930-943 | 14 | 14 | Networked games, servers, massive worlds and real-time communication |
| P33 | 944-947 | 4 | 4 | Collaboration host and browser hub |
| P34 | 948-971 | 24 | 24 | Content creation: procedural generation, assisted posing, modeling and interchange |
| P35 | 972-978 | 7 | 7 | DAW and the collaboration release |
| P36 | 979-982 | 4 | 4 | Cinematics and virtual production |
| P37 | 983-1018 | 36 | 36 | CAD, CAE, CAM, EDA and AEC |
| P38 | 1019-1060 | 42 | 42 | Estimation and control, simulation, sensors, robotics, autonomy, aerospace and embedded |
| P39 | 1061-1063 | 3 | 3 | Operations for distributed, collaborative and embedded deployments |
| P40 | 1064-1065 | 2 | 18 | Real-hardware qualification (the hardware halves of every hardware-gated row) |
| P41 | 1066-1068 | 3 | 3 | Cross-domain portfolio |

### P00 Completed history (Done and Recorded)

Rows in order: AUD-0, AUD-1, DOCS.1, DOCS.2, DOCS.3, DOCS.4, DOCS, ORIENT.1, ORIENT.2, ORIENT, REPO.1, REPO.2, REPO.3a,
REPO.3b, REPO.VS, REPO.SYNC.1, REPO.SYNC.2, REPO.SYNC.3, REPO.SYNC.4, REPO.SYNC.5, REPO.SYNC, REPO.DEV.0, REPO.DEV.1,
REPO.3c.1, REPO.3c.2, REPO.3c.3, REPO.3c.4.a, REPO.3c.4, REPO.3c.5.a, REPO.3c.5.b, REPO.3c.5.c, REPO.3c.5.d, REPO.3c.5,
REPO.3c.6, REPO.3c.7, REPO.3c.8, REPO.3c.9, REPO.3c.10, REPO.3c, REPO.DEV.2, REPO.DEV.3a, REPO.DEV.3b.1, REPO.DEV.3b.2,
REPO.DEV.3b.3, REPO.DEV.3b.4, REPO.DEV.3b.5, REPO.DEV.3b, REPO.DEV.3c, REPO.DEV.3, REPO.DEV.4, REPO.DEV.5, REPO.DEV.6,
REPO.DEV.7, REPO.DEV.8, REPO.DEV.9, REPO.DEV.10, REPO.DEV.11, REPO.DEV, DIAG.0, DIAG.1a, DIAG.1b, DIAG.2a, DIAG.2b,
DIAG.3a, DIAG.3b, DIAG.3c, DIAG.3d, DIAG.3e, DIAG.3f, DIAG.4a, DIAG.4b, DIAG.4c, DIAG.5a, DIAG.5b, DIAG.5c, DIAG.5d,
DIAG.6a, DIAG.6b, DIAG.6c, DIAG.7a, DIAG.7b, DIAG.7c, DIAG.8a, DIAG.8b, DIAG.8c, DIAG.9a, DIAG.9b, DIAG.9c, DIAG.10a,
DIAG.10b, DIAG.10c, DIAG.11a.

Done and Recorded history in its current order. Unblocks everything.

### P01 Current front (user-fixed): adoption, CPU dispatch, resilience, diagnostics, ownership

Rows in order: RESEARCH-ADOPT, SIMD-DISPATCH, HW-RESILIENCE.1, HW-RESILIENCE.2, HW-RESILIENCE.3, HW-RESILIENCE.4,
HW-RESILIENCE, DIAG.11b, DIAG.11c, DIAG.12, DIAG, MCP-ADAPT.0, REPO.3d, REPO.3, REPO, HESAP-JOBS-RETUNE, OWN.1, OWN.2,
OWN.

User-fixed. HESAP-JOBS-RETUNE is folded into SIMD-DISPATCH and closes with it (user decision
2026-10-29, [record](../sessions/2026-10-29-user-decision-fold-jobs-retune.md)); it now requires SIMD-DISPATCH, not DIAG.

### P02 NETCRASH window: secure network foundation and crash-to-fix intake (user-fixed)

Rows in order: NET.1, NET-STD, COMPLY.0, COMPLY.1, NET.2, HESAP-NET, NET.3, NET.4, 4.2a, NET-PATH, NET-REACH, 4.2b,
NET-SIM, NET-DEBUG, NET.5, COMPLY.2, CRASH-FIX.1, CRASH-FIX.2, NETCRASH.

User-fixed. The Part 4 rows and the COMPLY children sit where the operator placed them: NET-STD after NET.1, the
register COMPLY.0 and cryptography and export control (COMPLY.1) before NET.2, HESAP-NET after NET.2, NET-PATH and
NET-REACH after 4.2a, privacy (COMPLY.2) before CRASH-FIX.1.

### P03 Security, supply chain and the audit gate (user-fixed)

Rows in order: IR-FUZZ, TEST-METH, COMPLY.3, COMPLY.4, SUPPLY.1, SUPPLY.4, SUPPLY.2, SEC.1, AUD-2, QUAL.0.

User-fixed. Security and vulnerability duties (COMPLY.3) and licences (COMPLY.4) precede SUPPLY.1 and SEC.1, which they
govern (operator placement).

### P04 RAH renderer redesign (user-fixed)

Rows in order: PQP-0.plan, PQP-1.plan, CORE-USE.1, CORE-USE.2, CORE-USE, JOBS-QOS, RAH-0, GPU-TIER.0, IMG-ORACLE,
FORMAL.1, RAH-SYNC, DX12-AGILITY, VK-PROFILE, RAH-1a.1, RAH-1a.2, RAH-1a-close, RAH-1.b, RAH-1.c, RAH-1.d, RAH-1.e,
RAH-1, RAH-2.a, RAH-2.b, RAH-2.c, RAH-2.d, RAH-2, RAH-3.a, RAH-3.b, RAH-3, RAH-4.a, RAH-4.b, RAH-4.c, RAH-4.d, RAH-4,
RAH-5.a, RAH-5.b, RAH-5.c, RAH-5.d, RAH-5, RAH-6.a, RAH-6.b, RAH-6, RAH-7.a, RAH-7.b, RAH-7.c, RAH-7.d, RAH-7.e,
RAH-7.f, RAH-7, CKIR-LANG.1, RAH-8.a, RAH-8.b, RAH-8, GPU-FORENSICS, RAH-DRAW.

User-fixed and unchanged.

### P05 Computational geometry backbone (user-fixed)

Rows in order: FOUNDATION-AUDIT, SCI-CONTRACT.1, GEOM-K.0, GEOM-K.1, GEOM-K.2, GEOM-K.3, GEOM-K.4, GEOM-EXACT.1,
GEOM-EXACT.2, GEOM-QUAL, NAV.0, NAV.3, GEOM.

User-fixed (operator placement of 2026-10-27).

### P06 Foundation hardening: allocators, containers, jobs and the ARM64 port

Rows in order: MAINT-1, DEBT-11, MEM-HARD.1, DEBT-16.1, DEBT-16.2, DEBT-16.3, DEBT-16, MAINT-3, MAINT-7, SANITY-TLSF,
MEM-HARD.2, MEM-HARD.3, MEM-HARD, SANITY-CONTAINERS, MAINT-6, MAINT-2, JOBS-PORT.1, JOBS-HARD.1, JOBS-HARD.2,
JOBS-HARD.3, JOBS-HARD, JOBS-PORT.2, 8.2a.

FOUNDATION-AUDIT has just closed the memory, log, containers and vm cycle and every later system allocates and
schedules. The soak lane (MAINT-1) comes first because MEM-HARD.2 and JOBS-HARD.3 soak on it; DEBT-11 is build hygiene
for the shipping lanes those boards use. The TLSF and allocator MAINT and SANITY rows are MEM-HARD scope; 32-bit
pointers (DEBT-16.2) precede MEM-HARD.2. JOBS-PORT.1 is the guard row the hardening needs and JOBS-PORT.2 the ARM64
release row, which ports the hardened substrate. 8.2a (SE(3) in crd-math) is foundation math. Unblocks the language,
hesap distribution, physics islands, CEIR-JIT.1 and the EYLEM replay matrix.

### P07 Language and reflection backbone (CHIR, reflection, language services)

Rows in order: LANG.0, LANG.1, LANG.2, LANG.3, LANG.4, LANG.5, LANG, REFLECT.1, REFLECT.2, REFLECT.3, REFLECT,
LANGTOOL.1, LANGTOOL.2, LANGTOOL.3, LANGTOOL.4, LANGTOOL.5, LANGTOOL.6.

CHIR is the one runtime-changeable language for every system; FE-CORE weak forms, AI.3 losses, physics scripting,
CHIR-SCI and the REPL consume it. LANG.0 no longer waits for the UI review I2D-0; the REPL still follows the UI (P28).

### P08 Command, transaction and MCP surface

Rows in order: CMD.1, CMD.2, API-STAB.0, CMD.3, CMD.4, MCP-ADAPT, CMD.

One typed command surface for GUI, CLI, MCP and agents; its journal feeds the CRASH-FIX.1 packs and is the design
FORMAL.1 models (CMD.3 and CMD.4 re-check conformance).

### P09 Scene, ECS, scientific meaning and large worlds

Rows in order: ECS-EDIT.1, ECS-EDIT.2, ECS-EDIT, ECS-HARD.1, ECS-HARD.2, DEBT-15.1, DEBT-15.2, DEBT-15.3, DEBT-15.5,
DEBT-15.6, DEBT-15.7, ECS-HARD, SCI-CONTRACT.2, SCI-CONTRACT.3, SCI-CONTRACT, WORLD.1.

ECS-HARD keeps its ECS-EDIT dependency (reflected stable identities); ECS-EDIT follows REFLECT and CMD (its rollback is
CMD.3). The transform follow-ons are ECS-HARD scope; SCI-CONTRACT.2 applies units through the REFLECT descriptors;
WORLD.1 closes the group. Unblocks animation, physics, render origins, large-world navigation and sensors.

### P10 Documents and shared reload

Rows in order: DOC.1, DOC.2, DOC.3, CONFIG-1.6b, DOC.

The config hot-reload hook joins the one shared reload path before DOC closes.

### P11 Scripting and native execution tiers

Rows in order: SCRIPT.1, SCRIPT.2, SCRIPT.3, SCRIPT.4, SCRIPT, CEIR-JIT.0, CEIR-JIT.1, CEIR-JIT.2.

Scripting binds CHIR and native code to the reflected schema; the execution-tier decision and the baseline and
optimizing tiers follow (CEIR-JIT.1 needs the ARM64 port of P06).

### P12 Extensions, packages and signing

Rows in order: EXT.1, EXT.2, EXT.3, SUPPLY.3, EXT.4.

Code signing sits directly before the package format that verifies signatures before load. EXT.5 and the EXT parent move
to the publishing group (P24, user decision).

### P13 Agent surface and import trust

Rows in order: COMPLY.6, AGENT.1, AGENT-DOCS, AGENT.2, AGENT.3, DATA-TRUST.1, DATA-TRUST.2, DATA-TRUST, SHIP.0.

The AI compliance child (COMPLY.6) precedes AGENT.1 and AI.1, which it governs; then agent schemas, workspace and
hostile-input defence, bounded import, and the publishing decision SHIP.0 after DATA-TRUST.

### P14 Numerics CPU completion (hesap follow-ons)

Rows in order: DEBT-3, DEBT-4, DEBT-5, DEBT-6, DEBT-7, DEBT-8, DEBT-9, DEBT-10, MAINT-5, SANITY-WPT.

hesap CPU follow-ons retained from earlier crush work close before the GPU programme builds on the same kernels.

### P15 GPU compute programs and hesap GPU

Rows in order: V17-RECONCILE, CGP-0, CGP-1, CGP-2, CGP-3, CGP-4, NAV.1, NAV.2, NAV.5, NAV.6, AS-4, AS-6, HGP-0, HGP-1,
CKIR-LANG.3, HGP-2, HGP-3, HGP-4, 8.2b, CKIR-LANG.2, HGP-5, HESAP-SYM, VV-UQ, HESAP-DIST, HESAP-AUDIO-DSP.

crd-hesap is the computational base for every solver and matrix need: GPU physics, FE-CORE assembly, the ML core and the
renderer's neural models consume it. NAV.1 and NAV.2 follow CGP-4 (operator); NAV.5 and NAV.6 join them. 8.2b closes
against HGP-4. HESAP-AUDIO-DSP is hesap scope.

### P16 Machine-learning core: model assets, inference and training

Rows in order: MLR-0, MLR-1, MLR-4, AI.1, AI.2, AI.3, AI.4, ROCM.1.

Model and tensor assets, inference primitives, future GPU execution, inference, training and fine-tuning: a backbone the
renderer's neural rows, learned motion matching, facial ML and posing consume. ROCM.1 (the HIP implementation, qualified
on emulation routes) follows its prerequisites here (hardware split); ROCM.2 is real hardware in P40.

### P17 Geometry extensions and discretization: SDF, B-rep, analysis meshing, FE and FV cores

Rows in order: SDF-v0, SDF-v1, SDF-v2, SDF-v3, SDF-v4, SDF-v5, SDF-v6, SDF-v7, SDF-v8, BREP.1, BREP.2, BREP.3, BREP,
MESH-SIM.1, MESH-SIM.2, MESH-SIM.3, FE-CORE, FV-CORE.

crd-sdf feeds physics, the renderer and procgen; the B-rep kernel is the exact solid backbone of CAD/CAM/CAE/AEC and of
MESH-SIM.1; FE-CORE is shared by FEA, real-time FEM and multiphysics.

### P18 Animation core

Rows in order: ANIM.1, ANIM.3, ANIM.5, ANIM.2, ANIM.4, ANIM.6, DEBT-15.4, DEBT-15.

Physics ragdolls (EYLEM-v4b/c) and the renderer's skinning and morph rows (GVA-2/3) need real producers.

### P19 Physics engine EYLEM, navigation crowds and physics qualification

Rows in order: PHYS-QUAL.plan, EYLEM-RESUME, EYLEM-v1c, EYLEM-v1c-sensor, EYLEM-v1d, EYLEM-v1d-manifold,
EYLEM-v1d-filter-a, EYLEM-v1d-filter-b, EYLEM-v1d-filter-c, EYLEM-v1d-callback-a, EYLEM-v1d-callback-b,
EYLEM-v1d-callback-c, EYLEM-v1d-mesh, EYLEM-v1d-hf, EYLEM-v1e, EYLEM-v1f, EYLEM-v1f-articulation-filter,
EYLEM-v1f-fields-a, EYLEM-v1f-fields-b, EYLEM-v1f-fields-c, EYLEM-v1f-fields-d, EYLEM-v1f-fields-e, EYLEM-v1f-fields-f,
EYLEM-v1f-fields-g, EYLEM-v1f-fields-h, EYLEM-v1f-fields-i, EYLEM-v1g, EYLEM-v1g-contactmodify, EYLEM-v1h, EYLEM-v1i,
EYLEM-v1j, EYL-0, EYL-1, EYLEM-v1k, EYLEM-v1l, EYLEM-v1l-test-conservation, EYLEM-v1l-test-closedform, NAV.4, ANIM.7,
NAV.7, EYLEM-v2a, EYLEM-v2b, EYLEM-v2c, EYLEM-v3a, EYLEM-v1l-test-stress, EYLEM-v3b, EYLEM-v3c, EYLEM-v3d, EYLEM-v3e,
EYLEM-v4a, EYLEM-v4b, EYLEM-v4c, EYLEM-v4d-cine-a, EYLEM-v4d-cine-b, EYLEM-v4d-cine-c, EYLEM-v5a, EYLEM-v5b, EYLEM-v5c,
EYLEM-v5d, EYLEM-v6a, EYLEM-v6b, EYLEM-v6c, EYLEM-v6c-urdf, EYLEM-v6c-sdf, EYLEM-v6c-mjcf, EYLEM-v6c-actuators,
EYLEM-v6d-nonsmooth-newton, EYLEM-v6e-sensor-a, EYLEM-v6e-sensor-b, EYLEM-v6e-sensor-c, EYLEM-v6f-aero-a,
EYLEM-v6f-aero-b, EYLEM-v6f-aero-c, EYLEM-v6f-aero-d, EYLEM-v1f-fields-j, EYLEM-v7a, EYLEM-v7b, EYLEM-v7c, EYLEM-v7d,
EYLEM-v8a, EYLEM-v8b, EYLEM-v8c, EYLEM-v8d, EYLEM-v8e, EYLEM-v9a, EYLEM-v9b, EYLEM-v9b-test-cross-engine,
EYLEM-v9b-test-drift, EYLEM-v9b-test-property, EYLEM-v9c, EYLEM-v9d, D-004, D-005, PHYS-QUAL.1, PHYS-QUAL.2.

See [Why EYLEM was late and where it goes now](#why-eylem-was-late-and-where-it-goes-now). PHYS-QUAL.1 and PHYS-QUAL.2
run here; the PHYS-QUAL parent closes in P28 after EYL-3 (user decision).

### P20 Renderer runtime library and offline rendering (RENDERER-RUNTIME gate)

Rows in order: D6, CGP-render, MLR-render.1, MLR-render.2, MLR-render.3, MLR-render, PRESENT.1, PRESENT.2, PRESENT.3,
PRESENT, RPL-0.runtime, TXS-0.runtime, TXS-1.runtime, MAT-0.runtime, MAT-1.runtime, LSH-0.runtime, GVA-0.runtime,
TPR-0.runtime, LSH-1.runtime, LSH-2.runtime, GVA-1.runtime, RPL-1.runtime, RPL-2.runtime, RPL-3.runtime, RTX-0.runtime,
RTX-2.runtime, MAT-2.runtime, MAT-3.runtime, MAT-4.runtime, MAT-5.runtime, GVA-2.runtime, GVA-3.runtime, GVA-4.runtime,
GVA-5.runtime, TXS-2.runtime, TXS-3.runtime, TPR-1.runtime, TPR-2.runtime, TPR-3.runtime, VFX-0.runtime, VFX-1.runtime,
VFX-2.runtime, VFX-3.runtime, LSH-4.runtime, LSH-5.runtime, RTX-1.runtime, ARG-0.runtime, ARG-1.runtime, ARG-2.runtime,
ARG-3.runtime, ARG-4.runtime, ARG-5.runtime, RTX-3.runtime, OFF-1, OFF-2, OFF-3, OFF-4, OFF-5, OFF-6, OFF-7, OFF-8,
OFF-9, RTX-4.runtime, VGE-0.runtime, VGE-1.runtime, VGE-2.runtime, VGE-3.runtime, LSH-3.core, VGE-4.runtime,
VGE-5.runtime, VGE-6.runtime, LSH-3.runtime, ARG-6.runtime, RPL-4.runtime, RPL-5.runtime, RPL-6.runtime, MLR-2.runtime,
REN-AA, DEBT-1, B16, REN-OCEAN, RENDER-COMPAT.1, RENDER-COMPAT.2, RENDER-COMPAT, QUAL.1, PQP-0.renderer, PQP-1.renderer,
PQP-2.renderer, PQP-3.renderer, PQP-4.renderer, RENDERER-RUNTIME.

Internal order unchanged; DEBT-1 joins before B16 and the new Requires name the backbones the families consume. QUAL.1
qualifies on available and software adapters (hardware split; QUAL.1-HW in P40). No UI starts before RENDERER-RUNTIME
(ADR-0129, validator).

### P21 Input, windows and the owned UI (crd-ui) with the CR-D007 shell

Rows in order: I2D-0, INPUT.1, INPUT.2, INPUT.3, INPUT.4, INPUT.5, INPUT, WINDOW.1, WINDOW.2, WINDOW.3, MAINT-4, WINDOW,
I2D-1.a, I2D-1.b, I2D-1.c, I2D-1, I2D-2.a, I2D-2.b, I2D-2.c, I2D-2.d, I2D-2, I2D-3.a, I2D-3.b, I2D-3.c, COMPLY.7,
I2D-3.d, I2D-3.e, I2D-3, I2D-4.a, I2D-4.b, I2D-4.c, I2D-4, I2D-5.a, I2D-5.b, I2D-5.c, I2D-5.d, I2D-5, I2D-6.a, I2D-6.b,
I2D-6.c, I2D-6.d, I2D-6, I18N.1, I2D-7.a, I2D-7.b, I2D-7.c, I2D-7.d, I2D-7, I2D-8.a, I2D-8.b, I2D-8.c, I2D-8.d, I2D-8.e,
I2D-8.f, I2D-8, A11Y-GAME, D7E-0, VCS.1, D7E-1, D7E-2, D7E-3, D7E-4, D7E-5, D7E-6, D7E-7, D7E-8, D7E-9, D7E-10, I2D-9.a,
I2D-9.b, I2D-9.c, I2D-9.d, I2D-9.e, I2D-9.

MAINT-4 belongs to docked and detached UI. 6.3 (OpenXR on simulators) needs all of its prerequisites, now including
CEIR-JIT.3 (Part 5), so it sits after CEIR-JIT.3 in P23. I2D-3.a, I2D-8.a and I2D-8.d carry the UI clauses of EXT.2,
SCI-CONTRACT.2, SCRIPT.4 and CMD.3. The accessibility child (COMPLY.7) sits before I2D-3.d, which it governs (operator
placement).

### P22 Scene and prefab document reconciliation for the editor

Rows in order: DEBT-12.1, DEBT-12.2, DEBT-12.3, DEBT-12, DEBT-13.1, DEBT-13.2, DEBT-13.3, DEBT-13.4, DEBT-13.5,
DEBT-13.6, DEBT-13.7, DEBT-13.8, DEBT-13, DEBT-14.1, DEBT-14.2, DEBT-14.3, DEBT-14.4, DEBT-14.5, DEBT-14.

The Review rows are editor-consumer reconciliations that gate EDITOR.1, so the whole family sits directly before the
editor.

### P23 CR-D007 editor, agent control, guarded crash fixing, native authoring, onboarding

Rows in order: EDITOR.1, VCS.2, EDITOR.2, EDITOR.3, EDITOR.4, EDITOR.5, SHIP.1, SHIP.2, SHIP.3, EDITOR.6, EDITOR.7,
SHIP.4, SHIP.5, DEBT-2, EDITOR, AGENT.4, AGENT.5, AGENT-BENCH, AGENT, CRASH-FIX.3, CRASH-FIX.4, CRASH-FIX, NATIVE.1,
NATIVE.2, NATIVE, CEIR-JIT.3, 6.3, DX.1, DX.2, DX.3, DX.4.

CRASH-FIX.3, .4 and the parent follow AGENT.3 and AGENT as the user set. SHIP.1 to SHIP.3 sit after EDITOR.5 and SHIP.4,
SHIP.5 after EDITOR.7 (Part 5). EDITOR.2 carries the CMD.3 gizmo-drag clause. 6.3 (OpenXR on simulators, hardware split)
sits right after its last prerequisite CEIR-JIT.3; SHIP-XR follows it in P31.

### P24 Publishing, stores, live operations, mods, the extension marketplace and the Cerid store

Rows in order: COMPLY.5, SHIP.6, SHIP.7, COMPLY.8, SHIP.8, SHIP.9, EXT.5, EXT, STORE.0, STORE.1, STORE.2, STORE.3,
STORE.4, STORE.5, STORE.6, SHIP.10.

Online safety, user content and children (COMPLY.5) first, because it governs the store, mods, EXT.5, 4.2d, COLLAB.1 and
NET-LIVE; then store adapters, entitlements and payments, the store compliance kit COMPLY.8, the publisher console, mods
and UGC (SHIP.9), the public extension marketplace EXT.5 and the EXT parent (user decision), the core of the Cerid store
STORE.0 to STORE.6 (built on SHIP.6, SHIP.7, COMPLY.8, SHIP.9 and EXT.5), then `ceridc publish` (SHIP.10, which needs
STORE.5). STORE.7 to STORE.9 follow SHIP in P27; STORE-MAC and STORE-WEB follow their SHIP rows in P31; STORE-SOCIAL,
the STORE parent and STORE-MOBILE follow NET-LIVE in P32.

### P25 2D games and world-space UI

Rows in order: SPR-0, SPR-1, SPR-2, SPR-3, SPR-4, WORLD-UI, FONT-3D.

2D games and world-space UI.

### P26 Visual authoring acceptance of every renderer family

Rows in order: RPL-0.authoring, RPL-0, TXS-0.authoring, TXS-0, TXS-1.authoring, TXS-1, MAT-0.authoring, MAT-0,
MAT-1.authoring, MAT-1, LSH-0.authoring, LSH-0, GVA-0.authoring, GVA-0, TPR-0.authoring, TPR-0, LSH-1.authoring, LSH-1,
LSH-2.authoring, LSH-2, GVA-1.authoring, GVA-1, RPL-1.authoring, RPL-1, RPL-2.authoring, RPL-2, RPL-3.authoring, RPL-3,
RTX-0.authoring, RTX-0, RTX-2.authoring, RTX-2, MAT-2.authoring, MAT-2, MAT-3.authoring, MAT-3, MAT-4.authoring, MAT-4,
MAT-5.authoring, MAT-5, GVA-2.authoring, GVA-2, GVA-3.authoring, GVA-3, GVA-4.authoring, GVA-4, GVA-5.authoring, GVA-5,
TXS-2.authoring, TXS-2, TXS-3.authoring, TXS-3, TPR-1.authoring, TPR-1, TPR-2.authoring, TPR-2, TPR-3.authoring, TPR-3,
VFX-0.authoring, VFX-0, VFX-1.authoring, VFX-1, VFX-2.authoring, VFX-2, VFX-3.authoring, VFX-3, LSH-4.authoring, LSH-4,
LSH-5.authoring, LSH-5, RTX-1.authoring, RTX-1, ARG-0.authoring, ARG-0, ARG-1.authoring, ARG-1, ARG-2.authoring, ARG-2,
ARG-3.authoring, ARG-3, ARG-4.authoring, ARG-4, ARG-5.authoring, ARG-5, RTX-3.authoring, RTX-3, RTX-4.authoring, RTX-4,
VGE-0.authoring, VGE-0, VGE-1.authoring, VGE-1, VGE-2.authoring, VGE-2, VGE-3.authoring, VGE-3, VGE-4.authoring, VGE-4,
VGE-5.authoring, VGE-5, VGE-6.authoring, VGE-6, LSH-3.authoring, LSH-3, ARG-6.authoring, ARG-6, RPL-4.authoring, RPL-4,
RPL-5.authoring, RPL-5, RPL-6.authoring, RPL-6, MLR-2.authoring, MLR-2.

Each renderer family's `.authoring` row and then its parent (validator gate after I2D-9).

### P27 Desktop release qualification and CR-D007

Rows in order: UX-QUAL.1, UX-QUAL.2, UX-QUAL, I2D-PQ, PQP-0, PQP-1, PQP-2, PQP-3, PQP-4, OPS.1, SHIP.11, SHIP, STORE.7,
STORE.8, STORE.9, QUAL.2, CR-D007.

SHIP.11 and SHIP close publishing, and STORE.7 to STORE.9 follow them, before QUAL.2 (which Requires STORE.8). QUAL.2
qualifies on clean virtual machines and hosted runners (hardware split; QUAL.2-HW in P40).

### P28 Scientific language, REPL, notebook, scientific visualization and physics visualization

Rows in order: CHIR-SCI.1, CHIR-SCI.2, SCIVIZ-PLOT, CHIR-SCI.3, REPL.0, CHIR-SCI.4, REPL.1, REPL.2, CHIR-SCI,
NOTEBOOK.1, REPL.3, NOTEBOOK.2, NOTEBOOK.3, NOTEBOOK.4, NOTEBOOK.5, REPL.4, NOTEBOOK, SCIVIZ.1, SCIVIZ.2, SCIVIZ.3,
SCIVIZ.4, SCIVIZ.5, SCIVIZ, EYL-2, EYL-3, PHYS-QUAL, HGP-6.integration, HGP-6.

The notebook is a CR-D007 workspace (validator); the REPL follows the UI (user decision); SCIVIZ-PLOT needs the owned
text stack; EYL-2/3 render physics through it, and the PHYS-QUAL parent closes after EYL-3 (user decision).

### P29 AI products: applied workflows, game brains and the combined AI release

Rows in order: MLR-3, AI.5, AI.6, AI, ML-DOMAIN.1, ML-DOMAIN.2, ML-DOMAIN.3, ML-DOMAIN.4, ML-DOMAIN.5, ML-DOMAIN.

Applied workflows and game brains need AGENT; the combined AI release; the original ML scope reconciled.

### P30 Media and audio engines

Rows in order: MED-REVIEW, MED-GPU-0, MED-GPU-1, MED-1, MED-2, MED-3, MED-8, MED-4, MED-5, MED-6, MED-7, MED-9, MED-10,
MED-11, MED-12, MED-HW, MED-GPU-2, MED-GPU-3, PLG-1, AUDIO-DRV, MIDI-UMP, PLG-2, PLG-3, PLG-4, PLG-5, PLG-6, PLG-7,
AUDIO-CEIR.1, AUDIO-CEIR.2, AUDIO-CEIR.3, AUDIO-CEIR.4, AUDIO-SPATIAL, AUDIO-GAME, AUDIO-CEIR.

The codecs precede the export row that needs them (MED-GPU-3). MED-HW, AUDIO-DRV and MIDI-UMP qualify on this host and
virtual devices (hardware split); their macOS backends are carried by MAC.2.

### P31 macOS and web releases

Rows in order: MAC.1, MAC.2, MAC.DIAG, MAC, SHIP-MAC, STORE-MAC, 6.1, SHIP-MOBILE, 6.0, SHIP-CONSOLE, SHIP-XR,
JOBS-PORT.3, JOBS-PORT, WEB.1, WEB-CEIR, WEB.2, WEB.3, WEB.4, WEB.5, WEB.DIAG, WEB, 6.2, SHIP-WEB, STORE-WEB,
SHIP-ENTERPRISE, HESAP-GPU-ALL.

After the Windows and Linux products they qualify. 6.1 (mobile on emulators and the iOS Simulator) and 6.0
(console-class profile from public information) follow MAC, CEIR-JIT.3 and the audio engine; HESAP-GPU-ALL closes on all
software-reachable backends after WEB (hardware split). The store rows SHIP-MAC, SHIP-MOBILE, SHIP-CONSOLE, SHIP-XR,
SHIP-WEB and SHIP-ENTERPRISE follow their platforms.

### P32 Networked games, servers, massive worlds and real-time communication

Rows in order: NET-TIME, 4.2c, 4.2d, NET-SERVER, ANTICHEAT, NET-SCALE, COMPLY.9, COMPLY, NET-LIVE, STORE-SOCIAL, STORE,
STORE-MOBILE, 4.2e, NET.

NET-TIME sits directly before 4.2c (Part 4); the communications child (COMPLY.9) and the COMPLY parent precede NET-LIVE,
which needs them and the media and audio engines; STORE-SOCIAL (needs 4.2d, COMPLY.9 and NET-LIVE), the STORE parent and
STORE-MOBILE (needs STORE, SHIP-MOBILE, SHIP-XR) follow NET-LIVE; the NET family closes together.

### P33 Collaboration host and browser hub

Rows in order: COLLAB.1, COLLAB.2, COLLAB.3, COLLAB.4.

Collaboration has priority after games; COLLAB.5 and the parent wait for the DAW.

### P34 Content creation: procedural generation, assisted posing, modeling and interchange

Rows in order: PROCGEN.1, PROCGEN.2, PROCGEN.3, 8.4b, PROCGEN.4, 8.4a, PROCGEN.5, PROCGEN.6, 8.4c, PROCGEN, POSE.1,
POSE.2, POSE.3, POSE.4, POSE, ANIM, MODEL.1, MODEL.2, MODEL.3, MODEL, INTEROP.1, INTEROP.3, INTEROP.2, INTEROP.

Procedural generation with its phase-8 rows, assisted posing (AI, editor) and the ANIM parent, modeling, then
interchange (needs MODEL and media).

### P35 DAW and the collaboration release

Rows in order: PLG-8, DAW-APP.1, DAW-APP.2, DAW-APP.3, DAW-APP, COLLAB.5, COLLAB.

PLG-8 (AU/AUv3 with MAC) opens the DAW group; COLLAB.5 and COLLAB close after DAW-APP.

### P36 Cinematics and virtual production

Rows in order: 8.3a, 8.3b, 8.3c, VPROD.

Cinematics and virtual production. VPROD qualifies on a simulated LED wall and recorded tracking (hardware split;
VPROD-HW in P40).

### P37 CAD, CAE, CAM, EDA and AEC

Rows in order: CAD-SOLVE, CAD.1, CAD.2, CAD.3, CAD, FEA.1, FEA.2, FEA.3, FEA.4, FEA.5, FEA.6, FEA, CFD.1, CFD.2, CFD.3,
CFD.4, CFD, CAM.1, CAM.2, CAM.3, CAM.4, CAM.5, CAM.6, CAM.7, CAM.8, CAM.9, CAM, EDA.1, EDA.2, EDA.3, EDA.4, EDA, 3.1.17,
AEC.1, AEC.2, AEC.

AEC.2 and EDA.4 need COLLAB; SIM.1 needs CFD and FEA.

### P38 Estimation and control, simulation, sensors, robotics, autonomy, aerospace and embedded

Rows in order: CONTROL.1, CONTROL.2, CONTROL.3, CONTROL.4, CONTROL.5, CONTROL.6, CONTROL.7, CONTROL.8, CONTROL.9,
CONTROL.10, CONTROL.11, CONTROL.12, CONTROL.13, CONTROL, SIM.1, SIM.2, SIM.3, SIM.4, SIM, SENSOR.1, SENSOR.2, SENSOR.3,
SENSOR, WORLD.2, WORLD, 8.0a, 8.0b, 8.0c, 8.0d, AUTONOMY.1, AUTONOMY.2, AUTONOMY, 8.1a, 8.1b, AUTOPILOT, 3.1.19,
EMBED.1, EMBED.2, EMBED.3, EMBED, 3.1.18, 6.4.

AUTOPILOT (SITL) and EMBED.3 (SIL and PIL on instruction-set simulators) qualify here (hardware split). 6.4 (HPC on
containerized clusters) follows SIM and NET-SERVER.

### P39 Operations for distributed, collaborative and embedded deployments

Rows in order: OPS.2, OPS.3, OPS.

Distributed operations need 6.4, AI, 8.3c, COLLAB and EMBED.

### P40 Real-hardware qualification (the hardware halves of every hardware-gated row)

Rows in order: GPU-FORENSICS-HW (inserted by the applier), QUAL.1-HW (inserted by the applier), QUAL.2-HW (inserted by
the applier), UX-QUAL-HW (inserted by the applier), MED-HW-QUAL (inserted by the applier), AUDIO-DRV-HW (inserted by the
applier), MIDI-UMP-HW (inserted by the applier), NET-TIME-HW (inserted by the applier), ROCM.2, ROCM, HESAP-GPU-ALL-HW
(inserted by the applier), 6.3-HW (inserted by the applier), 6.1-HW (inserted by the applier), 6.0-HW (inserted by the
applier), 6.4-HW (inserted by the applier), VPROD-HW (inserted by the applier), AUTOPILOT-HW (inserted by the applier),
EMBED-HW (inserted by the applier).

The real-hardware halves of every hardware-gated row, at the end so that a missing device cannot halt the strictly
sequential table (user decision). ROCM.2 and the ROCM parent are the AMD hardware half.

### P41 Cross-domain portfolio

Rows in order: PORTFOLIO.1, PORTFOLIO.2, PORTFOLIO.

Last: the cross-domain release review, which requires every real-hardware row (pending Requires fix).

## Why EYLEM was late and where it goes now

**Why it was late.** Not because physics depends on media or science. Three things combined:

1. ADR-0129 (2026-09-12) set the product order renderer, UI and CR-D007, notebook, then "media, audio/plugins, later
   applications and preserved engine/domain work", and explicitly superseded the older eylem-before-UI clause. The
   documentation audit then parked physics, geometry and every domain row as Later and appended them in their old
   phase order behind media and science.
2. Their Requires cells encoded that tail instead of real needs. EYLEM-RESUME needs PHYS-QUAL.plan, which needs WORLD.1
   and SCI-CONTRACT; WORLD.1 needs ECS-HARD, which needs ECS-EDIT (behind REFLECT, LANG and, through LANG.0's I2D-0
   dependency, RENDERER-RUNTIME) and JOBS-HARD; JOBS-HARD.1 named the JOBS-PORT parent, whose JOBS-PORT.3 needs MAC,
   which needs NOTEBOOK and CR-D007. So every physics row was transitively behind the macOS port and the notebook.
3. The EYLEM rows declared only EYLEM-RESUME, so their real prerequisites (geometry queries, jobs, ECS, SDF, FE-CORE,
   scripting, GPU primitives, ARM64) were invisible.

**Where it goes now: P19, one contiguous block of 95 rows directly before the renderer runtime library.** It follows
exactly what it consumes: the geometry kernel (P05; broadphase and narrow phase consume crd-geometry-bvh and
crd-geometry-convex after GEOM-K.3), hardened jobs and memory (P06; island-parallel solve), the ECS, frames, units and
large-world coordinates (P09), scripting (P11; the Script field formula), hesap CPU and GPU and CGP (P14-P15; GPU
broadphase sort, differentiable path), SDF and FE-CORE (P17; SDF fields and cloth collisions, real-time FEM as an
FE-CORE consumer) and the animation core (P18; ragdolls, IK assist, cinematic curves). It precedes the renderer because
GVA-3.runtime qualifies cloth and soft-body input, which must come from a real producer (EYLEM-v3), and because games,
simulation, robotics, VPROD and CAD/CAE all consume it. The cuts that made this possible: JOBS-HARD.1 names JOBS-PORT.1
instead of the JOBS-PORT parent, LANG.0 no longer waits for the UI review, and PHYS-QUAL.1 no longer waits for live
visualization. PHYS-QUAL.1 and PHYS-QUAL.2 (the numeric boards) run in P19; the PHYS-QUAL parent, with EYL-2 and EYL-3,
closes in P28, because the user requires that physics qualification closes only with live visualization and
debugging, and EYL-2/EYL-3 render through SCIVIZ-PLOT, which needs the owned text stack of the UI programme.

The internal order is the dependency chain: rigid 3D core and its v1 close, crowds on the character controller, 2D,
soft bodies (the XPBD stress bench right after v3a), articulation and ragdolls, vehicles, CCD, reduced-coordinate
robotics with importers, sensors and aerospace (J2 gravity lands with the aero cluster), FEM, GPU, differentiable and
determinism matrix, fixed point, replay tooling, the replay sandbox and reload reconciliation, then PHYS-QUAL.1/.2.

## ADR-0129 amendment (accepted; the operator commits it with the apply)

Append to `docs/decisions/0129-renderer-ui-editor-delivery-order.md`, under Consequences, exactly:

> **2026-10-27 amendment (user decision, [delivery plan](../design/delivery-plan.md)):** backbones precede their
> consumers. After the RAH redesign and the computational geometry backbone, the table runs foundation hardening and
> the ARM64 port; the CHIR language, reflection, command, scene/ECS/world, document, scripting, extension and agent
> backbone; hesap CPU and GPU, the GPU compute programs and the machine-learning core; SDF, B-rep, analysis meshing and
> the FE/FV cores; the animation core; and the physics engine EYLEM with its numeric qualification, all before the
> renderer runtime library. The sentence above that supersedes "eylem-before-UI" clauses no longer applies: physics
> now precedes the renderer and the UI because the renderer consumes it (GVA-3.runtime qualifies cloth and soft-body
> input from a real producer). The renderer runtime library still completes before any UI implementation
> (RENDERER-RUNTIME), CR-D007 still precedes the notebook, and the notebook still follows as a CR-D007 workspace.
> Media, audio, the macOS and web releases and the product verticals follow in the plan's order. A row that needs
> hardware or SDK access the project does not have qualifies on emulators, simulators and software adapters at its
> place, and its real-hardware half runs at the end of the table. No row is Later.

## Requires corrections

Each line: group, slice, current prerequisites -> new prerequisites, reason. All are in the JSON's `requires_fixes`
with the current cell in `requires_before`; the applier refuses if a cell changed in the meantime.

- P04 FORMAL.1: RAH-0 -> RAH-0, DIAG.11b, CRASH-FIX.2. decision 13: the models of designs implemented before it (flight
  recorder, CRASH-FIX.2 leases) are validated against those implementations.
- P04 RAH-7.b: RAH-7.a -> RAH-7.a, FORMAL.1. formal conformance: re-checks the FORMAL.1 staged-commit model.
- P04 RAH-7.d: RAH-7.c -> RAH-7.c, FORMAL.1. formal conformance: late-failure runs validated as model traces.
- P04 RAH-7.e: RAH-7.b, JOBS-QOS -> RAH-7.b, JOBS-QOS, FORMAL.1. formal conformance: the fallback path joins the
  staged-commit model.
- P06 DEBT-16.1: none -> MEM-HARD.1. TLSF follow-on folded into MEM-HARD; starts from the consumer map.
- P06 DEBT-16.2: none -> MEM-HARD.1. TLSF 32-bit pointers folded into MEM-HARD; starts from the consumer map.
- P06 DEBT-16.3: none -> MEM-HARD.1. multi-threaded TLSF folded into MEM-HARD; starts from the consumer map.
- P06 MAINT-3: none -> MEM-HARD.1. sharded allocator / deferred free is MEM-HARD scope.
- P06 MAINT-7: none -> MEM-HARD.1. growable TLSF consumer audit is MEM-HARD scope.
- P06 MEM-HARD.2: MEM-HARD.1 -> MEM-HARD.1, DEBT-16. its adversaries run "across target pointer sizes" and concurrency,
  which DEBT-16 adds.
- P06 MEM-HARD: MEM-HARD.3 -> MEM-HARD.3, MAINT-3, MAINT-7, SANITY-TLSF. parent closes with the folded MAINT-3, MAINT-7
  and SANITY-TLSF.
- P06 MAINT-6: none -> SANITY-CONTAINERS. same capacity-edge scope as SANITY-CONTAINERS; runs on its corpus.
- P06 MAINT-2: none -> MEM-HARD. a concurrent hash map needs the qualified allocator ownership.
- P06 JOBS-PORT.1: ROCM -> FOUNDATION-AUDIT. was ROCM (a GPU port); the OS/ISA/ABI guard row needs only the audited
  foundation.
- P06 JOBS-HARD.1: MEM-HARD, JOBS-PORT -> MEM-HARD, JOBS-PORT.1. was JOBS-PORT (parent, behind macOS and web); hardening
  x64 needs only the guard row .1.
- P06 JOBS-PORT.2: JOBS-PORT.1, SHIP.5 -> JOBS-PORT.1, JOBS-HARD. adds JOBS-HARD: the ARM64 port carries the hardened
  substrate; drops the Part 5 SHIP.5 prerequisite, whose direction the rows own text reverses ("after ARM64 qualifies,
  SHIP.4 and SHIP.5 add ARM64 packages"; "ARM64 packages join after JOBS-PORT.2"), so SHIP.4 and SHIP.5 now Require
  JOBS-PORT.2.
- P06 8.2a: none -> SCI-CONTRACT.1. SE(3) conventions follow the frame/handedness contract; EYLEM-v6c consumes it.
- P07 LANG.0: I2D-0 -> JOBS-HARD, OWN. was I2D-0 (UI review); the language reference needs JOBS-HARD (async,
  cancellation) and OWN (the compiler owns no numerics), not the UI architecture.
- P07 LANG.1: I2D-0, LANG.0 -> LANG.0. drops I2D-0 for the same reason as LANG.0.
- P08 CMD.3: CMD.2 -> CMD.2, FORMAL.1. formal conformance: re-checks the FORMAL.1 journal model.
- P08 CMD.4: CMD.3 -> CMD.3, FORMAL.1. formal conformance: replay and dry-run validated against the journal model.
- P09 ECS-EDIT.2: ECS-EDIT.1 -> ECS-EDIT.1, CMD.3. its transaction rollback is the CMD.3 journal.
- P09 DEBT-15.1: none -> ECS-HARD.1. transform follow-on folded into ECS-HARD.
- P09 DEBT-15.2: none -> ECS-HARD.1. TransformF64 folded into ECS-HARD (WORLD.1 consumes it).
- P09 DEBT-15.3: none -> ECS-HARD.1. parallel propagation folded into ECS-HARD.
- P09 DEBT-15.5: none -> ECS-HARD.1. change tracking folded into ECS-HARD.
- P09 DEBT-15.6: none -> ECS-HARD.1. renormalize policy folded into ECS-HARD.
- P09 DEBT-15.7: none -> ECS-HARD.1. look-at convention folded into ECS-HARD.
- P09 ECS-HARD: ECS-HARD.2 -> ECS-HARD.2, DEBT-15.1, DEBT-15.2, DEBT-15.3, DEBT-15.5, DEBT-15.6, DEBT-15.7. parent
  closes with its folded transform follow-ons (DEBT-15.4 waits for bones, see splits).
- P09 SCI-CONTRACT.2: SCI-CONTRACT.1 -> SCI-CONTRACT.1, REFLECT. units on API/ECS/UI adapters use the REFLECT unit
  descriptors.
- P10 CONFIG-1.6b: none -> DOC.3. config hot reload reconciles with the shared reload of DOC.3.
- P10 DOC: DOC.3 -> DOC.3, CONFIG-1.6b. parent closes with the folded config reload hook.
- P11 SCRIPT.3: SCRIPT.2 -> SCRIPT.2, FORMAL.1. decision 13: re-checks the FORMAL.1 hot-reload model.
- P11 CEIR-JIT.1: CEIR-JIT.0, SCRIPT.3, IR-FUZZ -> CEIR-JIT.0, SCRIPT.3, IR-FUZZ, JOBS-PORT.2. its baseline tier is "for
  x64 and ARM64": the ARM64 port row must come first.
- P15 V17-RECONCILE: CR-D007 -> RAH-8, CKIR-LANG.1. was CR-D007; the reconciliation is against shipped CKIR/CEIR
  evidence, final after RAH-8 and CKIR-LANG.1, not against the editor.
- P16 MLR-0: NOTEBOOK -> HGP-4. was NOTEBOOK; model and tensor assets sit on hesap tensors (HGP-4), not on the notebook.
- P16 MLR-1: NOTEBOOK, CKIR-LANG.3 -> MLR-0, HGP-0, CKIR-LANG.3. was NOTEBOOK; inference primitives need MLR-0, GEMM
  (HGP-0) and the tile dialect.
- P16 MLR-4: NOTEBOOK -> CGP-0, MLR-1. was NOTEBOOK; future GPU execution builds on CGP-0 scheduling and MLR-1.
- P16 AI.1: MLR-0, MLR-1, NOTEBOOK, COMPLY.6 -> MLR-0, MLR-1, COMPLY.6. drops NOTEBOOK; model/dataset assets do not need
  the notebook workspace; keeps COMPLY.6.
- P16 ROCM.1: NOTEBOOK -> HGP-5, MLR-1. was NOTEBOOK; the HIP implementation lowers hesap GPU and ML ops and qualifies
  on emulation routes right after them (hardware split).
- P17 SDF-v0: none -> GEOM-QUAL. SDF had no prerequisites; it is a geometry consumer of the closed kernel.
- P17 SDF-v1: none -> SDF-v0. SDF chain order.
- P17 SDF-v2: none -> SDF-v1, GEOM-K.3. mesh-to-SDF baker uses the retargeted BVH.
- P17 SDF-v3: none -> SDF-v2. SDF chain order.
- P17 SDF-v4: none -> SDF-v3. SDF chain order.
- P17 SDF-v5: none -> SDF-v4, RAH-7. GPU path is a CKIR asset under live replacement.
- P17 SDF-v6: none -> SDF-v5. SDF chain order.
- P17 SDF-v7: none -> SDF-v6. SDF chain order.
- P17 SDF-v8: none -> SDF-v7, CGP-3. GPU baker and dual contouring share CGP-3 (one owner).
- P17 BREP.1: none -> GEOM-QUAL. B-rep kernel builds on the geometry kernel (research amendment to BREP.2).
- P17 BREP.2: BREP.1, MODEL, GEOM-QUAL -> BREP.1, GEOM-QUAL. was MODEL (a product workspace); the kernel must not wait
  for its consumer.
- P17 FE-CORE: MESH-SIM.1, HESAP-DIST -> MESH-SIM.1, MESH-SIM.3, HESAP-DIST, HGP-1, LANG. hp-adaptivity through
  MESH-SIM.3, GPU assembly on HGP-1, weak forms authored in CHIR (LANG).
- P17 FV-CORE: MESH-SIM.1, HESAP-DIST -> MESH-SIM.1, HESAP-DIST, HGP-1, LANG. same authored, live-edit rules as FE-CORE:
  CHIR forms and GPU sparse.
- P18 ANIM.1: none -> ECS-HARD. skeletons are ECS entities and relations.
- P18 ANIM.3: none -> ANIM.1, GEOM-K.3. foot IK raycasts use the geometry BVH.
- P18 ANIM.5: none -> ANIM.1. morph targets extend the skeletal asset.
- P18 ANIM.2: none -> ANIM.1, AI.3. learned motion matching trains on AI.3.
- P18 ANIM.4: none -> ANIM.3. procedural look-at/foot/hand IK uses ANIM.3.
- P18 ANIM.6: none -> ANIM.5, AI.2, HESAP-AUDIO-DSP. visemes need audio analysis and ML lip sync.
- P18 DEBT-15.4: none -> ANIM.1. socket propagation needs bones.
- P19 EYLEM-v1c: EYLEM-RESUME -> EYLEM-RESUME, GEOM-K.3. broadphase consumes crd-geometry-bvh after the kernel retarget
  (GEOM-K.3).
- P19 EYLEM-v1c-sensor: EYLEM-RESUME -> EYLEM-v1c. EYLEM intra-family chain (the rows declared only EYLEM-RESUME).
- P19 EYLEM-v1d: EYLEM-RESUME -> EYLEM-v1c, GEOM-K.3. narrow phase consumes crd-geometry-convex after GEOM-K.3.
- P19 EYLEM-v1d-manifold: EYLEM-RESUME -> EYLEM-v1d. EYLEM intra-family chain (the rows declared only EYLEM-RESUME).
- P19 EYLEM-v1d-filter-a: EYLEM-RESUME -> EYLEM-v1d. EYLEM intra-family chain (the rows declared only EYLEM-RESUME).
- P19 EYLEM-v1d-filter-b: EYLEM-RESUME -> EYLEM-v1d-filter-a. EYLEM intra-family chain (the rows declared only
  EYLEM-RESUME).
- P19 EYLEM-v1d-filter-c: EYLEM-RESUME -> EYLEM-v1d-filter-b, ECS-HARD. ECS-native predicate hooks.
- P19 EYLEM-v1d-callback-a: EYLEM-RESUME -> EYLEM-v1d, ECS-HARD. deferred ECS event streams.
- P19 EYLEM-v1d-callback-b: EYLEM-RESUME -> EYLEM-v1d-callback-a. EYLEM intra-family chain (the rows declared only
  EYLEM-RESUME).
- P19 EYLEM-v1d-callback-c: EYLEM-RESUME -> EYLEM-v1d-callback-b. EYLEM intra-family chain (the rows declared only
  EYLEM-RESUME).
- P19 EYLEM-v1d-mesh: EYLEM-RESUME -> EYLEM-v1d, GEOM-K.3. triangle BVH via GEOM-K.3.
- P19 EYLEM-v1d-hf: EYLEM-RESUME -> EYLEM-v1d. EYLEM intra-family chain (the rows declared only EYLEM-RESUME).
- P19 EYLEM-v1e: EYLEM-RESUME -> EYLEM-v1d-manifold. EYLEM intra-family chain (the rows declared only EYLEM-RESUME).
- P19 EYLEM-v1f: EYLEM-RESUME -> EYLEM-v1e. EYLEM intra-family chain (the rows declared only EYLEM-RESUME).
- P19 EYLEM-v1f-articulation-filter: EYLEM-RESUME -> EYLEM-v1f, EYLEM-v1d-filter-c. EYLEM intra-family chain (the rows
  declared only EYLEM-RESUME).
- P19 EYLEM-v1f-fields-b: EYLEM-RESUME -> EYLEM-v1f-fields-a. EYLEM intra-family chain (the rows declared only
  EYLEM-RESUME).
- P19 EYLEM-v1f-fields-c: EYLEM-RESUME -> EYLEM-v1f-fields-a. EYLEM intra-family chain (the rows declared only
  EYLEM-RESUME).
- P19 EYLEM-v1f-fields-d: EYLEM-RESUME -> EYLEM-v1f-fields-a. EYLEM intra-family chain (the rows declared only
  EYLEM-RESUME).
- P19 EYLEM-v1f-fields-e: EYLEM-RESUME -> EYLEM-v1f-fields-d, SDF-v8. Gradient formula consumes crd-sdf ("after Phase
  3.1.5 closes").
- P19 EYLEM-v1f-fields-f: EYLEM-RESUME -> EYLEM-v1f-fields-d. EYLEM intra-family chain (the rows declared only
  EYLEM-RESUME).
- P19 EYLEM-v1f-fields-g: EYLEM-RESUME -> EYLEM-v1f-fields-d, SCRIPT. Script formula needs SCRIPT ("after Phase 4
  scripting ships").
- P19 EYLEM-v1f-fields-h: EYLEM-RESUME -> EYLEM-v1f-fields-a. EYLEM intra-family chain (the rows declared only
  EYLEM-RESUME).
- P19 EYLEM-v1f-fields-i: EYLEM-RESUME -> EYLEM-v1f-fields-h, EYLEM-v1f-fields-g, EYLEM-v1f-fields-e,
  EYLEM-v1f-fields-f. EYLEM intra-family chain (the rows declared only EYLEM-RESUME).
- P19 EYLEM-v1g: EYLEM-RESUME -> EYLEM-v1e, JOBS-HARD. island-parallel solve on crd-jobs.
- P19 EYLEM-v1g-contactmodify: EYLEM-RESUME -> EYLEM-v1g. EYLEM intra-family chain (the rows declared only
  EYLEM-RESUME).
- P19 EYLEM-v1h: EYLEM-RESUME -> EYLEM-v1c, EYLEM-v1d. EYLEM intra-family chain (the rows declared only EYLEM-RESUME).
- P19 EYLEM-v1i: EYLEM-RESUME -> EYLEM-v1h. EYLEM intra-family chain (the rows declared only EYLEM-RESUME).
- P19 EYLEM-v1j: EYLEM-RESUME -> EYLEM-v1g. EYLEM intra-family chain (the rows declared only EYLEM-RESUME).
- P19 EYL-0: none -> EYLEM-v1j, FE-CORE. typed physics resources follow the EYLM snapshot and include FE fields.
- P19 EYL-1: none -> EYL-0, EYLEM-v1j. replay and scrubbing build on the v1j snapshot harness.
- P19 EYLEM-v1k: EYLEM-RESUME -> EYLEM-v1i, EYLEM-v1j, EYLEM-v1f. EYLEM intra-family chain (the rows declared only
  EYLEM-RESUME).
- P19 EYLEM-v1l: EYLEM-RESUME -> EYLEM-v1k, EYLEM-v1c-sensor, EYLEM-v1d-callback-c, EYLEM-v1d-mesh, EYLEM-v1d-hf,
  EYLEM-v1f-articulation-filter, EYLEM-v1f-fields-i, EYLEM-v1g-contactmodify. v1 surface freeze covers every v1 row.
- P19 EYLEM-v1l-test-conservation: EYLEM-RESUME -> EYLEM-v1l. EYLEM intra-family chain (the rows declared only
  EYLEM-RESUME).
- P19 EYLEM-v1l-test-closedform: EYLEM-RESUME -> EYLEM-v1l. EYLEM intra-family chain (the rows declared only
  EYLEM-RESUME).
- P19 ANIM.7: none -> NAV.4, ANIM.2. crowd steering is NAV.4 (one owner).
- P19 EYLEM-v2a: EYLEM-RESUME -> EYLEM-v1l. EYLEM intra-family chain (the rows declared only EYLEM-RESUME).
- P19 EYLEM-v2b: EYLEM-RESUME -> EYLEM-v2a. EYLEM intra-family chain (the rows declared only EYLEM-RESUME).
- P19 EYLEM-v2c: EYLEM-RESUME -> EYLEM-v2b. EYLEM intra-family chain (the rows declared only EYLEM-RESUME).
- P19 EYLEM-v3a: EYLEM-RESUME -> EYLEM-v1l. EYLEM intra-family chain (the rows declared only EYLEM-RESUME).
- P19 EYLEM-v1l-test-stress: EYLEM-RESUME -> EYLEM-v1l, EYLEM-v3a. its XPBD-particle stress bench needs v3a.
- P19 EYLEM-v3b: EYLEM-RESUME -> EYLEM-v3a, SDF-v8. cloth SDF environment collisions via crd-sdf.
- P19 EYLEM-v3c: EYLEM-RESUME -> EYLEM-v3b. EYLEM intra-family chain (the rows declared only EYLEM-RESUME).
- P19 EYLEM-v3d: EYLEM-RESUME -> EYLEM-v3a. EYLEM intra-family chain (the rows declared only EYLEM-RESUME).
- P19 EYLEM-v3e: EYLEM-RESUME -> EYLEM-v3b, EYLEM-v3d. EYLEM intra-family chain (the rows declared only EYLEM-RESUME).
- P19 EYLEM-v4a: EYLEM-RESUME -> EYLEM-v1l. EYLEM intra-family chain (the rows declared only EYLEM-RESUME).
- P19 EYLEM-v4b: EYLEM-RESUME -> EYLEM-v4a, ANIM.1. ragdoll preset needs skeletons.
- P19 EYLEM-v4c: EYLEM-RESUME -> EYLEM-v4b, ANIM.3. IK pose assist uses the ANIM.3 IK owner.
- P19 EYLEM-v4d-cine-a: EYLEM-RESUME -> EYLEM-v1l, ANIM.1. curve evaluation from animation clips.
- P19 EYLEM-v4d-cine-b: EYLEM-RESUME -> EYLEM-v4d-cine-a. EYLEM intra-family chain (the rows declared only
  EYLEM-RESUME).
- P19 EYLEM-v4d-cine-c: EYLEM-RESUME -> EYLEM-v4d-cine-b. EYLEM intra-family chain (the rows declared only
  EYLEM-RESUME).
- P19 EYLEM-v5a: EYLEM-RESUME -> EYLEM-v1l, EYLEM-v1h. EYLEM intra-family chain (the rows declared only EYLEM-RESUME).
- P19 EYLEM-v5b: EYLEM-RESUME -> EYLEM-v5a. EYLEM intra-family chain (the rows declared only EYLEM-RESUME).
- P19 EYLEM-v5c: EYLEM-RESUME -> EYLEM-v5b. EYLEM intra-family chain (the rows declared only EYLEM-RESUME).
- P19 EYLEM-v5d: EYLEM-RESUME -> EYLEM-v5c, EYLEM-v1d-hf. EYLEM intra-family chain (the rows declared only
  EYLEM-RESUME).
- P19 EYLEM-v6a: EYLEM-RESUME -> EYLEM-v1l. EYLEM intra-family chain (the rows declared only EYLEM-RESUME).
- P19 EYLEM-v6b: EYLEM-RESUME -> EYLEM-v6a. EYLEM intra-family chain (the rows declared only EYLEM-RESUME).
- P19 EYLEM-v6c: EYLEM-RESUME -> EYLEM-v4a, 8.2a. Featherstone spatial algebra uses SE(3) (8.2a).
- P19 EYLEM-v6c-urdf: EYLEM-RESUME -> EYLEM-v6c. EYLEM intra-family chain (the rows declared only EYLEM-RESUME).
- P19 EYLEM-v6c-sdf: EYLEM-RESUME -> EYLEM-v6c. EYLEM intra-family chain (the rows declared only EYLEM-RESUME).
- P19 EYLEM-v6c-mjcf: EYLEM-RESUME -> EYLEM-v6c. EYLEM intra-family chain (the rows declared only EYLEM-RESUME).
- P19 EYLEM-v6c-actuators: EYLEM-RESUME -> EYLEM-v6c. EYLEM intra-family chain (the rows declared only EYLEM-RESUME).
- P19 EYLEM-v6d-nonsmooth-newton: EYLEM-RESUME -> EYLEM-v6c. EYLEM intra-family chain (the rows declared only
  EYLEM-RESUME).
- P19 EYLEM-v6e-sensor-a: EYLEM-RESUME -> EYLEM-v1l, SCI-CONTRACT.3. sensor time/frame contract.
- P19 EYLEM-v6e-sensor-b: EYLEM-RESUME -> EYLEM-v6e-sensor-a, EYLEM-v1h. EYLEM intra-family chain (the rows declared
  only EYLEM-RESUME).
- P19 EYLEM-v6e-sensor-c: EYLEM-RESUME -> EYLEM-v6e-sensor-a. EYLEM intra-family chain (the rows declared only
  EYLEM-RESUME).
- P19 EYLEM-v6f-aero-a: EYLEM-RESUME -> EYLEM-v1l. EYLEM intra-family chain (the rows declared only EYLEM-RESUME).
- P19 EYLEM-v6f-aero-b: EYLEM-RESUME -> EYLEM-v6f-aero-a. EYLEM intra-family chain (the rows declared only
  EYLEM-RESUME).
- P19 EYLEM-v6f-aero-c: EYLEM-RESUME -> EYLEM-v6f-aero-b. EYLEM intra-family chain (the rows declared only
  EYLEM-RESUME).
- P19 EYLEM-v6f-aero-d: EYLEM-RESUME -> EYLEM-v6f-aero-c. EYLEM intra-family chain (the rows declared only
  EYLEM-RESUME).
- P19 EYLEM-v1f-fields-j: EYLEM-RESUME -> EYLEM-v1f-fields-a, EYLEM-v6f-aero-d. J2 gravity "lands alongside the v6f
  aerospace cluster".
- P19 EYLEM-v7a: EYLEM-RESUME, OWN.1 -> EYLEM-v3d, FE-CORE, OWN.1. real-time FEM is a consumer of FE-CORE ("shared by
  real-time FEM"); solvers from hesap.
- P19 EYLEM-v7b: EYLEM-RESUME -> EYLEM-v7a. EYLEM intra-family chain (the rows declared only EYLEM-RESUME).
- P19 EYLEM-v7c: EYLEM-RESUME -> EYLEM-v7b. EYLEM intra-family chain (the rows declared only EYLEM-RESUME).
- P19 EYLEM-v7d: EYLEM-RESUME -> EYLEM-v7c. EYLEM intra-family chain (the rows declared only EYLEM-RESUME).
- P19 EYLEM-v8a: EYLEM-RESUME -> EYLEM-v1l, CGP-1. GPU broadphase uses CGP-1 sort and scan.
- P19 EYLEM-v8b: EYLEM-RESUME -> EYLEM-v8a. EYLEM intra-family chain (the rows declared only EYLEM-RESUME).
- P19 EYLEM-v8c: EYLEM-RESUME -> EYLEM-v8a, EYLEM-v3a. EYLEM intra-family chain (the rows declared only EYLEM-RESUME).
- P19 EYLEM-v8d: EYLEM-RESUME -> EYLEM-v8c. EYLEM intra-family chain (the rows declared only EYLEM-RESUME).
- P19 EYLEM-v8e: EYLEM-RESUME -> EYLEM-v8b, EYLEM-v8d. EYLEM intra-family chain (the rows declared only EYLEM-RESUME).
- P19 EYLEM-v9a: EYLEM-RESUME -> EYLEM-v1l, HGP-4. differentiable path on hesap tensors/autodiff.
- P19 EYLEM-v9b: EYLEM-RESUME -> EYLEM-v1j, JOBS-PORT.2. replay-hash matrix includes ARM64.
- P19 EYLEM-v9b-test-cross-engine: EYLEM-RESUME -> EYLEM-v9b. EYLEM intra-family chain (the rows declared only
  EYLEM-RESUME).
- P19 EYLEM-v9b-test-drift: EYLEM-RESUME -> EYLEM-v9b. EYLEM intra-family chain (the rows declared only EYLEM-RESUME).
- P19 EYLEM-v9b-test-property: EYLEM-RESUME -> EYLEM-v9b, TEST-METH. property testing is owned by TEST-METH.
- P19 EYLEM-v9c: EYLEM-RESUME -> EYLEM-v9b, GEOM-K.2. Q-format fixed point is GEOM-K.2 (one owner).
- P19 EYLEM-v9d: EYLEM-RESUME -> EYLEM-v1j, CMD. replay CLI is a ceridc command.
- P19 D-004: EYL-3 -> EYL-1, EYLEM-v9d, CMD. was EYL-3 (live viz); a replay sandbox needs replay (EYL-1, v9d) and the
  command log.
- P19 D-005: D-004 -> D-004, DOC. reload reconciliation through DOC, after the replay sandbox, keeps eylem live edit.
- P19 PHYS-QUAL.1: EYL-3, PHYS-QUAL.plan -> PHYS-QUAL.plan, EYL-1, EYLEM-v2c, EYLEM-v3e, EYLEM-v4d-cine-c, EYLEM-v5d,
  EYLEM-v6f-aero-d, EYLEM-v7d, EYLEM-v8e, EYLEM-v9d. was EYL-3; qualification is numeric boards, so it needs every
  physics family, not live visualization.
- P20 CGP-render: RAH-8 -> RAH-8, CGP-4. packages the renderer subset of the CGP family, which now precedes it.
- P20 MLR-render.2: MLR-render.1 -> MLR-render.1, MLR-1. renderer inference kernels consume MLR-1 primitives (no
  duplicate).
- P20 MLR-render.3: MLR-render.2 -> MLR-render.2, AI.3. renderer model training uses the AI.3 training loop (no
  duplicate).
- P20 RPL-0.runtime: D6, PRESENT, PQP-1.plan, CGP-render -> D6, PRESENT, PQP-1.plan, CGP-render, WORLD.1. every pipeline
  renders through WORLD.1 local render origins.
- P20 GVA-2.runtime: MAT-5.runtime -> MAT-5.runtime, ANIM.1, ANIM.7. skinning and crowds need real skeleton, clip and
  crowd producers.
- P20 GVA-3.runtime: GVA-2.runtime -> GVA-2.runtime, ANIM.5, EYLEM-v3e. morph and cloth/soft-body input need ANIM.5 and
  EYLEM soft bodies (no stub inputs).
- P20 GVA-5.runtime: GVA-4.runtime -> GVA-4.runtime, SDF-v8. SDF special geometry consumes crd-sdf.
- P20 ARG-4.runtime: ARG-3.runtime -> ARG-3.runtime, SDF-v8. mesh/global SDF GI consumes crd-sdf.
- P20 DEBT-1: none -> GVA-4.runtime, VFX-2.runtime. ocean follow-on re-audit after its inputs.
- P20 REN-OCEAN: GVA-4.runtime, VFX-2.runtime -> GVA-4.runtime, VFX-2.runtime, B16, DEBT-1. closes the B16
  reconciliation and the DEBT-1 follow-on file.
- P21 MAINT-4: none -> WINDOW.2. ImGui multi-viewport disposition belongs to detached/docked UI.
- P21 WINDOW: WINDOW.3 -> WINDOW.3, MAINT-4. parent closes with MAINT-4.
- P21 I2D-3.a: I2D-2, REFLECT, CMD, DOC -> I2D-2, REFLECT, CMD, DOC, EXT.2. carries the EXT.2 widget-deregistration
  clause.
- P21 I2D-8.a: I2D-7 -> I2D-7, SCI-CONTRACT.2. carries the SCI-CONTRACT.2 unit-field clause.
- P21 I2D-8.d: I2D-8.c -> I2D-8.c, SCRIPT.4, CMD.3. carries the SCRIPT.4 palette/console clause and the CMD.3 drag
  clause.
- P22 DEBT-12.3: none -> DOC. scene/prefab follow-on reconciled with its siblings.
- P22 DEBT-13.4: none -> DOC. scene/prefab follow-on reconciled with its siblings.
- P22 DEBT-13.7: none -> DOC. scene/prefab follow-on reconciled with its siblings.
- P22 DEBT-13.8: none -> DOC. scene/prefab follow-on reconciled with its siblings.
- P22 DEBT-14.1: none -> DOC, WORLD.1. streaming scene load builds on WORLD.1 partitioning.
- P22 DEBT-14.5: none -> DOC. scene/prefab follow-on reconciled with its siblings.
- P23 EDITOR.2: EDITOR.1 -> EDITOR.1, CMD.3. carries the CMD.3 gizmo-drag clause.
- P23 SHIP.3: SHIP.2, NET.5, CRASH-FIX.2, DOC.2, API-STAB.0 -> SHIP.2, NET.5, CRASH-FIX.2, DOC.2, API-STAB.0, FORMAL.1.
  decision 13: re-checks the FORMAL.1 staged-rollout model.
- P23 SHIP.4: EDITOR.7, SHIP.3, DX12-AGILITY -> EDITOR.7, SHIP.3, DX12-AGILITY, JOBS-PORT.2. Windows ARM64 packages join
  after the ARM64 port (its own text).
- P23 SHIP.5: EDITOR.7, SHIP.3 -> EDITOR.7, SHIP.3, JOBS-PORT.2. Linux ARM64 packages join after the ARM64 port (its own
  text).
- P23 DEBT-2: none -> EDITOR.3, NAV.2. gizmo/mesh/curve/navmesh editors follow the editor gizmos and NAV.2.
- P23 EDITOR: EDITOR.7, SHIP.4, SHIP.5 -> EDITOR.7, SHIP.4, SHIP.5, DEBT-2. keeps the Part 5 SHIP.4 and SHIP.5; the
  parent also closes with the direct-manipulation follow-on.
- P23 6.3: CEIR-JIT.3 -> PQP-3.renderer, INPUT, WINDOW, CEIR-JIT.3. keeps the Part 5 CEIR-JIT.3 (AOT for XR exports);
  OpenXR extends the renderer XR matrix with input and windows.
- P28 SCIVIZ-PLOT: CHIR-SCI.2, OWN.1 -> CHIR-SCI.2, OWN.1, I2D-2. axis labels and SVG/PDF export need the owned text
  stack I2D-2.
- P28 SCIVIZ.1: none -> SCIVIZ-PLOT. field visualization builds on the plotting core.
- P28 SCIVIZ.4: none -> SCIVIZ.1. annotations on SCIVIZ.1 views.
- P28 SCIVIZ.5: none -> SCIVIZ.1. comparison views of SCIVIZ.1.
- P28 SCIVIZ: none -> SCIVIZ.1, SCIVIZ.2, SCIVIZ.3, SCIVIZ.4, SCIVIZ.5. parent names its children.
- P28 EYL-2: SCIVIZ-PLOT -> SCIVIZ-PLOT, EYL-1. its plots come from SCIVIZ-PLOT.
- P28 EYL-3: none -> EYL-2, SCIVIZ.1. live physics fields render through SCIVIZ.1.
- P28 PHYS-QUAL: PHYS-QUAL.2 -> PHYS-QUAL.2, EYL-3. user decision: qualification closes only with live visualization and
  debugging.
- P29 ML-DOMAIN: AI -> AI, ML-DOMAIN.1, ML-DOMAIN.2, ML-DOMAIN.3, ML-DOMAIN.4, ML-DOMAIN.5. parent names its children.
- P30 MED-GPU-3: MED-REVIEW -> MED-REVIEW, MED-12, MED-HW, MED-GPU-2. final export needs the encoders, MED-12 transcode,
  hardware video and MED-GPU-2.
- P31 MAC.1: NOTEBOOK, JOBS-PORT.2 -> NOTEBOOK, JOBS-PORT.2, SUPPLY.3, I2D-8.e. carries Apple notarization (SUPPLY.3),
  NSAccessibility (I2D-8.e) and macOS ARM64.
- P31 MAC.2: MAC.1 -> MAC.1, AUDIO-DRV, MIDI-UMP, PLG-7. carries CoreAudio (AUDIO-DRV), CoreMIDI (MIDI-UMP) and Cocoa
  plugin editors (PLG-7).
- P31 6.1: CEIR-JIT.3 -> CEIR-JIT.3, MAC, JOBS-PORT.2, AUDIO-CEIR. mobile needs AOT, iOS toolchain, ARM64 and audio.
- P31 6.0: CEIR-JIT.3 -> CEIR-JIT.3, AUDIO-CEIR. console packs need AOT exports and audio.
- P31 6.2: none -> WEB. closes against the WEB release.
- P31 HESAP-GPU-ALL: WEB, MAC, ROCM, HGP-5, MLR-1 -> WEB, MAC, ROCM.1, HGP-5, MLR-1. was ROCM (hardware); the
  all-backend close qualifies on ROCM.1 routes, AMD hardware is HESAP-GPU-ALL-HW.
- P32 4.2c: NET.5, NET-TIME -> NET.5, NET-TIME, EYLEM-v1j. keeps the Part 4 NET-TIME dependency and adds the EYLM
  snapshot that rollback re-simulates.
- P32 NET-SCALE: 4.2c, 4.2d, NET-SERVER -> 4.2c, 4.2d, NET-SERVER, WORLD.1, FORMAL.1. sharded worlds use WORLD.1
  partitioning; decision 13: re-checks the FORMAL.1 handoff model.
- P33 COLLAB.1: NET.5, DOC, NET, COMPLY.5 -> NET.5, DOC, NET-SERVER, NET-REACH, COMPLY.2, COMPLY.5, COMPLY.9, FORMAL.1.
  was NET (parent, behind NET-LIVE and media); the project host needs NET.5, DOC, NET-SERVER, NET-REACH (reachability),
  the COMPLY privacy (COMPLY.2), online-safety (COMPLY.5) and communications (COMPLY.9) rows and the FORMAL.1 host
  model.
- P33 COLLAB.2: COLLAB.1 -> COLLAB.1, FORMAL.1. formal conformance: re-checks the FORMAL.1 host model.
- P33 COLLAB.3: COLLAB.2 -> COLLAB.2, FORMAL.1. formal conformance: recovery and failover against the host model.
- P34 8.4b: none -> PROCGEN.2, PROCGEN.3. folded rules row follows PROCGEN.2/.3.
- P34 PROCGEN.4: none -> PROCGEN.1. terrain uses the noise primitives.
- P34 8.4a: none -> PROCGEN.4. folded terrain row follows PROCGEN.4.
- P34 PROCGEN.5: none -> PROCGEN.2, PROCGEN.3. city generation uses WFC and L-systems.
- P34 PROCGEN.6: none -> MAT-5. procedural materials target the closed material family.
- P34 8.4c: none -> SDF-v8, CGP-3. marching cubes, SDF Booleans and dual contouring are crd-sdf and CGP-3 (one owner).
- P34 PROCGEN: none -> PROCGEN.1, PROCGEN.2, PROCGEN.3, PROCGEN.4, PROCGEN.5, PROCGEN.6, 8.4a, 8.4b, 8.4c. parent closes
  with its children and the folded 8.4a-c.
- P34 POSE.4: POSE.3 -> POSE.3, EDITOR. motion editing, timeline and undo run in the editor.
- P34 ANIM: POSE -> POSE, ANIM.1, ANIM.2, ANIM.3, ANIM.4, ANIM.5, ANIM.6, ANIM.7. parent names its children as well as
  POSE.
- P34 MODEL.1: CR-D007, PHYS-QUAL -> CR-D007, GEOM-EXACT.2. was PHYS-QUAL; mesh editing needs the editor and exact mesh
  Booleans, not physics boards.
- P35 PLG-8: PLG-7 -> PLG-7, MAC.1. AU/AUv3 hosting ships "with MAC".
- P36 8.3a: none -> MED-GPU-3. the sequencer edits the media timeline.
- P36 8.3b: none -> 8.3a, RENDER-COMPAT.1. camera and lens model shared with the offline renderer.
- P36 8.3c: none -> 8.3a, OFF-9, JOBS-HARD. render farm dispatches OFF-9 output through crd-jobs.
- P37 CAD.2: none -> CAD-SOLVE. sketch constraint solving is CAD-SOLVE (research amendment).
- P37 CAD.3: none -> CAD.1, CAD.2. drafting annotates features and sketches.
- P37 CAD: CAD-SOLVE -> CAD-SOLVE, CAD.1, CAD.2, CAD.3. parent names its children.
- P37 FEA: none -> FEA.1, FEA.2, FEA.3, FEA.4, FEA.5, FEA.6. parent names its children.
- P37 CFD: none -> CFD.1, CFD.2, CFD.3, CFD.4. parent names its children.
- P37 CAM.1: none -> CAD. milling toolpaths run on CAD parts.
- P37 CAM.2: none -> CAD. turning runs on CAD parts.
- P37 CAM.3: none -> GEOM-EXACT.2. slicing runs on exact meshes.
- P37 CAM.4: none -> CAM.1. post-processing follows toolpaths.
- P37 CAM.5: none -> CAM.1. material removal simulates toolpaths.
- P37 CAM.6: none -> CAD. sheet metal is a CAD feature family.
- P37 CAM.7: none -> CAM.1. PCB machining follows the milling core.
- P37 CAM: CAM.9 -> CAM.1, CAM.2, CAM.3, CAM.4, CAM.5, CAM.6, CAM.7, CAM.8, CAM.9. parent names its children.
- P38 CONTROL.1: none -> SCI-CONTRACT. estimation/control use the frames, units and uncertainty contract.
- P38 CONTROL.2: none -> SCI-CONTRACT. estimation/control use the frames, units and uncertainty contract.
- P38 CONTROL.3: none -> SCI-CONTRACT. estimation/control use the frames, units and uncertainty contract.
- P38 CONTROL.4: none -> SCI-CONTRACT. estimation/control use the frames, units and uncertainty contract.
- P38 CONTROL.5: none -> SCI-CONTRACT. estimation/control use the frames, units and uncertainty contract.
- P38 CONTROL.6: none -> SCI-CONTRACT. estimation/control use the frames, units and uncertainty contract.
- P38 CONTROL.7: none -> SCI-CONTRACT. estimation/control use the frames, units and uncertainty contract.
- P38 CONTROL.8: none -> SCI-CONTRACT. estimation/control use the frames, units and uncertainty contract.
- P38 CONTROL.9: none -> SCI-CONTRACT. estimation/control use the frames, units and uncertainty contract.
- P38 CONTROL: none -> CONTROL.1, CONTROL.2, CONTROL.3, CONTROL.4, CONTROL.5, CONTROL.6, CONTROL.7, CONTROL.8,
  CONTROL.9, CONTROL.10, CONTROL.11, CONTROL.12, CONTROL.13. parent names its children.
- P38 8.0b: none -> CONTROL. PID/MPC primitives are CONTROL (one owner).
- P38 8.0d: none -> 8.0a, SIM. robot sim loop runs on SIM and the kinematics row.
- P38 8.1b: none -> 8.1a, SCI-CONTRACT.3, NET.5. telemetry needs time scales and the network foundation.
- P38 3.1.19: SCI-CONTRACT.3 -> SCI-CONTRACT.3, 8.1a. astrodynamics uses time scales and the flight model.
- P38 6.4: HESAP-DIST -> HESAP-DIST, SIM, NET-SERVER. distributed simulation and cluster deployment run on SIM and the
  server fleet layer.
- P39 OPS.3: OPS.2, 6.4, AI, 8.3c, COLLAB, EMBED -> OPS.2, 6.4, AI, 8.3c, COLLAB, EMBED, FORMAL.1. decision 13:
  re-checks the FORMAL.1 scheduler-lease model.

Pending Requires corrections (they name rows not yet in the table; the JSON's `pending_requires_fixes` applies each
once every row it names exists, and until then the row keeps its current or planned cell):

- PORTFOLIO: -> PORTFOLIO.2, ROCM, GPU-FORENSICS-HW, QUAL.1-HW, QUAL.2-HW, UX-QUAL-HW, MED-HW-QUAL, AUDIO-DRV-HW,
  MIDI-UMP-HW, NET-TIME-HW, HESAP-GPU-ALL-HW, 6.3-HW, 6.1-HW, 6.0-HW, 6.4-HW, VPROD-HW, AUTOPILOT-HW, EMBED-HW (applied
  once GPU-FORENSICS-HW, QUAL.1-HW, QUAL.2-HW, UX-QUAL-HW, MED-HW-QUAL, AUDIO-DRV-HW, MIDI-UMP-HW, NET-TIME-HW,
  HESAP-GPU-ALL-HW, 6.3-HW, 6.1-HW, 6.0-HW, 6.4-HW, VPROD-HW, AUTOPILOT-HW, EMBED-HW exist).

## Family splits

Each family below is not contiguous; every other family is.

- **NET:** NET.1-.5 in the NETCRASH window (P02, user decision); 4.2c-e, NET-SERVER, ANTICHEAT, NET-SCALE, NET-LIVE,
  NET-TIME and the parent in P32 (physics snapshots, media and audio, the web release).
- **4.2x:** 4.2a, 4.2b in the window (user decision); 4.2c-e in P32.
- **CRASH-FIX:** .1 and .2 in the window; .3, .4 and the parent after AGENT in P23 (user decision).
- **SUPPLY:** .1, .4, .2 in the user-fixed security block; SUPPLY.3 directly before EXT.4, the package format that
  verifies signatures (decision 7, accepted).
- **COMPLY:** the operator's merged family places each child directly before the first row it governs, and this
  plan keeps that rule: COMPLY.0 and COMPLY.1 before NET.2, COMPLY.2 before CRASH-FIX.1 (P02); COMPLY.3 and COMPLY.4
  before SUPPLY.1 (P03); COMPLY.6 before AGENT.1 (P13); COMPLY.7 before I2D-3.d (P21); COMPLY.5 and the store kit
  COMPLY.8 with the store rows (P24); COMPLY.9 and the COMPLY parent before NET-LIVE (P32).
- **SHIP:** SHIP.0 after DATA-TRUST, SHIP.1-.5 inside the editor group, SHIP.6-.10 in P24, SHIP.11 and the parent
  before QUAL.2, and the store rows for macOS, mobile, consoles, XR, web and enterprise after their platforms in P31,
  as the research places them.
- **STORE:** STORE.0-.6 in P24, STORE.7-.9 in P27, STORE-MAC and STORE-WEB in P31, STORE-SOCIAL, the parent and
  STORE-MOBILE in P32, each after its last prerequisite (platforms, SHIP rows, NET-LIVE).
- **PQP and QUAL:** plan rows before RAH (P04), `.renderer` rows before RENDERER-RUNTIME (P20), release rows before
  CR-D007 (P27); QUAL.0, QUAL.1, QUAL.2 likewise; QUAL.1-HW and QUAL.2-HW in P40.
- **Renderer families (RPL, TXS, MAT, LSH, GVA, TPR, VFX, RTX, ARG, VGE, MLR-2):** `.runtime` rows in P20, `.authoring`
  rows and the family parents in P26 (validator gate, ADR-0129).
- **MLR:** MLR-render in P20, MLR-0, MLR-1, MLR-4 in the ML core (P16), MLR-3 in P29 after AGENT.
- **AI:** AI.1-.4 in P16; AI.5, AI.6 and the parent in P29.
- **HGP:** HGP-0-.5 in P15; HGP-6.integration and HGP-6 in P28 (NOTEBOOK and EYL-3).
- **CKIR-LANG:** .1 in the RAH block (user-fixed); .2 and .3 in P15.
- **CEIR-JIT:** .0-.2 in P11; .3 (AOT exports) after EDITOR.7 in P23.
- **AGENT:** .1-.3 and AGENT-DOCS in P13; AGENT.4, .5, AGENT-BENCH and the parent in P23.
- **EXT:** .1-.4 in P12; EXT.5 and the EXT parent with SHIP.9 and the STORE family in P24 (user decision).
- **OPS:** OPS.1 before the first desktop release (P27); OPS.2, .3 and the parent in P39.
- **JOBS-PORT:** .1 and .2 (ARM64) in P06; .3 (WASM) and the parent with the web release (P31).
- **SCI-CONTRACT:** .1 in the GEOM block; .2, .3 and the parent in P09.
- **WORLD:** WORLD.1 in P09; WORLD.2 and the parent in P38.
- **NAV:** .0 and .3 in the GEOM block; .1, .2 after CGP-4 (operator), .5 and .6 with them (P15); .4 and .7 in P19.
- **ANIM:** .1-.6 in P18; ANIM.7 next to NAV.4 in P19; the parent after POSE in P34.
- **DEBT-15:** .1-.3 and .5-.7 in ECS-HARD (P09); .4 (bone sockets) and the parent in P18.
- **EYL:** EYL-0 and EYL-1 in P19; EYL-2 and EYL-3 in P28.
- **PHYS-QUAL:** .plan, .1 and .2 in P19; the parent after EYL-3 in P28 (user decision).
- **COLLAB:** .1-.4 in P33; COLLAB.5 and the parent after DAW-APP in P35.
- **PLG:** PLG-1-7 in P30; PLG-8 (AU/AUv3 with MAC) at the start of P35.
- **SCIVIZ:** SCIVIZ-PLOT between CHIR-SCI.2 and CHIR-SCI.3; SCIVIZ.1-.5 and the parent after NOTEBOOK.
- **ROCM:** ROCM.1 (emulation routes) in P16; ROCM.2 and the parent in P40.
- **6.x:** 6.3 in P23, 6.1 and 6.0 in P31, 6.2 with WEB in P31, 6.4 in P38; their `-HW` halves in P40.
- **8.x:** 8.2a in P06, 8.2b in P15, 8.4a-c in PROCGEN (P34), 8.3a-c in P36, 8.0a-d and 8.1a-b in P38.
- **Hardware-split rows:** every row listed under the hardware split keeps its simulator half in place and has its
  `-HW` half in P40.

## Folded rows and optional renames

- DEBT-1 into REN-OCEAN; DEBT-2 into EDITOR; DEBT-3 to DEBT-10, MAINT-5 and SANITY-WPT into the hesap CPU completion
  (P14); DEBT-11 and MAINT-1 at the start of the foundation (P06); DEBT-12, -13, -14 with all their children before
  EDITOR.1 (P22); DEBT-15 into ECS-HARD and the animation core; DEBT-16, MAINT-3, MAINT-7, SANITY-TLSF into MEM-HARD;
  SANITY-CONTAINERS, MAINT-6 and MAINT-2 after MEM-HARD; MAINT-4 into WINDOW; CONFIG-1.6b into DOC; D-004 and D-005
  into physics replay and reload.
- Phase-numbered rows: 3.1.17 into EDA, 3.1.18 into EMBED, 3.1.19 into aerospace, 4.2x into NET, 6.x and 8.x as above.

Optional renames for the user (not applied; IDs are stable anchors and the audit snapshot routes some of them):
DEBT-16.x to MEM-HARD.TLSF-x; DEBT-15.x to ECS-HARD.T-x; DEBT-3 to DEBT-10 to HESAP-CPU.x; MAINT-1 to SOAK-LANE;
4.2a to 4.2e to NET-TRANSPORT, NET-DETSIM, NET-REPL, NET-SESSION, NET-ROS2; 8.2a to MATH-SE3; 8.3a-c to CINE.1-.3;
8.4a-c to PROCGEN.T/.R/.G; 6.0, 6.1, 6.3, 6.4 to PLAT-CONSOLE, PLAT-MOBILE, PLAT-XR, PLAT-HPC.

## Single owners named by the plan

The plan merges no rows. These ownership facts bind the rows that touch them (OWN.1 records, OWN.2 enforces):

- **CRC-32 and CRC32C:** one owner in the foundation, in crd-core's containers and hash family: the dispatched
  checksum family of HW-RESILIENCE.1 (carry-less multiply and portable paths, an SSE2 floor) replaces the copies in
  `perf`, `resources` and `hesap-tensor` (the duplicate record on OWN.1). crd-hesap does not own it; HESAP-NET's
  erasure codes, the crdr containers, the crash bundles and every network and document checksum call the foundation
  owner.
- GPU skinning in GVA-2.runtime; skeletons, clips and the CPU reference in ANIM.1.
- Marching cubes and SDF Booleans in crd-sdf (SDF-v4, SDF-v7) with the GPU forms in CGP-3; 8.4c consumes them.
- Crowd steering in NAV.4; ANIM.7 and GVA-2.runtime consume it. IK in ANIM.3; EYLEM-v4c consumes it.
- Sensor models: EYLEM-v6e owns the physics-attached sensors, SENSOR.1 the typed schemas, calibration and ground truth.
- PCB output in EDA.3; CAM.7 owns PCB machining only. PID/MPC in CONTROL; 8.0b consumes it.
- Q-format fixed point in GEOM-K.2; EYLEM-v9c consumes it. Property testing in TEST-METH.

## UI clauses carried by the consuming UI rows

Decision item 3 (accepted, tightened): each UI-consumer clause stays an explicit clause of its source row, which
qualifies the UI-independent contract, and the consuming UI row carries a cross-linked clause and Requires the source.

| Source row | Clause kept at the source | Receiving row |
| --- | --- | --- |
| SCI-CONTRACT.2 | "UI adapters": units at the reflected property layer | I2D-8.a (unit/expression fields) |
| SCRIPT.4 | "same invocation from GUI/CLI/MCP" | I2D-8.d (command palette, console) |
| EXT.2 | "widget deregistration" | I2D-3.a (UiWorld lifetime and reconciliation) |
| CMD.3 | "one-commit drags" | I2D-8.d (control drags) and EDITOR.2 (gizmo drags) |

Platform clauses already named "with MAC" in their rows are carried the same way: MAC.1 carries Apple notarization
(SUPPLY.3), the NSAccessibility adapter (I2D-8.e) and the macOS half of JOBS-PORT.2; MAC.2 carries the CoreAudio
backend (AUDIO-DRV), the CoreMIDI transport (MIDI-UMP) and Cocoa plugin-editor embedding (PLG-7). The browser clauses
already name WEB.2 and WEB.5. The exact appended texts are in [Planned clause texts](#planned-clause-texts).

## Formal-model conformance

Decision item 12 (accepted, tightened): FORMAL.1 models RAH-7's staged commit, CMD.3's journal, the flight recorder's
global sequence and the COLLAB host. Every row that implements one of those designs Requires FORMAL.1 and re-checks
the model against the implemented design: the model's states and actions are refined to the implementation, and
recorded runs are validated as traces of the specification in CI.

- Staged commit: RAH-7.b (stage, commit, last-good, retirement), RAH-7.d (late-failure runs as traces), RAH-7.e (the
  fallback-while-compiling path joins the model).
- Journal: CMD.3 (the journal), CMD.4 (dry-run, replay and cancellation).
- Flight recorder: implemented before FORMAL.1 (DIAG.11b), so FORMAL.1 itself validates the shipped implementation
  and Requires DIAG.11b.
- Project host: COLLAB.1 (the reviewed design and the model agree), COLLAB.2 (sync and concurrent edits), COLLAB.3
  (recovery, failover, no lost accepted edits).

Decision 13 (adopted 2026-10-27, user decision): FORMAL.1 also models five more protocols of the same class, with the
same conformance rule, recorded as clauses on FORMAL.1 and on each implementing row:

- CRASH-FIX.2 agent leases (claim, renew, expiry, release, takeover): implemented before FORMAL.1, so FORMAL.1 Requires
  CRASH-FIX.2 and validates its recorded runs as traces, as it does for the flight recorder; CRASH-FIX.2 carries the
  clause that a later protocol change re-checks the model.
- OPS.3 scheduler leases with idempotent outputs and checkpoints: OPS.3 Requires FORMAL.1 and re-checks.
- NET-SCALE entity handoff and dynamic re-sharding (no double or missing owner, no lost accepted state): NET-SCALE
  Requires FORMAL.1 and re-checks.
- SCRIPT.3 hot-reload quiescence, state migration and exact rollback: SCRIPT.3 Requires FORMAL.1 and re-checks.
- SHIP.3 staged rollout (rings, halt on regression, rollback, resumable patches): SHIP.3 Requires FORMAL.1 and
  re-checks.

## Hardware-gated rows: simulator halves and real-hardware halves

Decision item 6 (accepted, tightened): every row that needs hardware or SDK access the project does not have is split.
The existing row is the simulator half, placed right after its prerequisites, and keeps its full text plus a
"Hardware split" clause naming its emulators, simulators, software adapters and public SDKs; the real-hardware half is
a new row in P40. Audit result:

| Simulator half (existing row, position group) | Real-hardware half (P40) |
| --- | --- |
| GPU-FORENSICS (P04, user-fixed) | GPU-FORENSICS-HW |
| QUAL.1 (P20) | QUAL.1-HW |
| QUAL.2 (P27) | QUAL.2-HW |
| UX-QUAL.1 (P27) | UX-QUAL-HW |
| MED-HW, AUDIO-DRV, MIDI-UMP (P30) | MED-HW-QUAL, AUDIO-DRV-HW, MIDI-UMP-HW |
| NET-TIME (P32) | NET-TIME-HW |
| ROCM.1 (P16) | ROCM.2 and the ROCM parent (existing) |
| HESAP-GPU-ALL (P31) | HESAP-GPU-ALL-HW |
| 6.3 OpenXR (P23) | 6.3-HW |
| 6.1 mobile, 6.0 consoles (P31) | 6.1-HW, 6.0-HW |
| 6.4 HPC (P38) | 6.4-HW |
| VPROD (P36), AUTOPILOT, EMBED.3 (P38) | VPROD-HW, AUTOPILOT-HW, EMBED-HW |

Not split, with the reason: JOBS-PORT.2 and the MAC rows qualify on hosted ARM64 and Apple Silicon CI runners;
PRESENT.3 and every other GPU row on this host's GPU; SENSOR rows judge declared sensor models and datasets; physical
robot, vehicle and flight deployment is outside the table ("separately authorized"). The Part 5 store-submission rows
(SHIP-MAC, SHIP-MOBILE, SHIP-CONSOLE, SHIP-XR) are placed after their platforms and are reviewed for the same split
when Part 5 lands.

New rows (state, text, Requires). Each row's Sources cell links this section as its Detail and ADR-0130 as its
decision; the JSON's `pending_rows` holds the exact cells, and the applier inserts every one the table does not contain
yet at its planned position, so one apply produces the complete table:

**GPU-FORENSICS-HW** (Open). Requires: GPU-FORENSICS.

Real-hardware half (user decision 2026-10-27; implementation and simulator qualification are in GPU-FORENSICS): Radeon
GPU Detective on AMD GPUs and the Intel GPU crash tools on Intel GPUs, one adversarial page fault per vendor tool on its
own hardware, imported into the DIAG.5d bundle with the same provenance and redaction checks. Every device purchase,
account and SDK access is approved by the user first; the evidence names the exact device, firmware, driver and OS
tuple, and a device that is not available keeps this row open, never waived.

**QUAL.1-HW** (Open). Requires: QUAL.1.

Real-hardware half (user decision 2026-10-27; implementation and simulator qualification are in QUAL.1): the physical
Windows and Linux GPU, driver and CPU matrix: AMD, Intel and NVIDIA discrete GPUs, integrated (UMA) GPUs, a low-VRAM
device, each GPU-TIER.0 tier on a real adapter, AMD and Intel CPUs across SIMD-DISPATCH levels, and the Steam Deck tuple
(native Linux build and Proton run on the device). Every device purchase, account and SDK access is approved by the user
first; the evidence names the exact device, firmware, driver and OS tuple, and a device that is not available keeps this
row open, never waived.

**QUAL.2-HW** (Open). Requires: QUAL.2, QUAL.1-HW.

Real-hardware half (user decision 2026-10-27; implementation and simulator qualification are in QUAL.2): clean-machine
game and editor exports on the representative hardware set of QUAL.1-HW, module exclusion and recovery on it, and rr
qualification (DIAG.9c) on a bare-metal Linux host with hardware performance counters (`perf_event_paranoid` at most 1).
Every device purchase, account and SDK access is approved by the user first; the evidence names the exact device,
firmware, driver and OS tuple, and a device that is not available keeps this row open, never waived.

**UX-QUAL-HW** (Open). Requires: UX-QUAL.

Real-hardware half (user decision 2026-10-27; implementation and simulator qualification are in UX-QUAL.1): pen tablets
with pressure and tilt, a touchscreen with multi-touch gestures, Xbox, PlayStation and generic HID gamepads with
haptics, and the Windows and Linux screen readers driving the native product on that hardware, with the UX-QUAL.2
input-to-present latency measured on each device. Every device purchase, account and SDK access is approved by the user
first; the evidence names the exact device, firmware, driver and OS tuple, and a device that is not available keeps this
row open, never waived.

**MED-HW-QUAL** (Open). Requires: MED-HW.

Real-hardware half (user decision 2026-10-27; implementation and simulator qualification are in MED-HW): hardware decode
and encode on AMD (AMF, Vulkan Video, D3D12 Video, VA-API), Intel (oneVPL, Vulkan Video, VA-API) and NVIDIA
(NVENC/NVDEC, Vulkan Video) devices, per codec, profile and level, with the capability tiers and software fallbacks
proven on each and the FFmpeg, NVENC and AMF peer boards rerun there. Every device purchase, account and SDK access is
approved by the user first; the evidence names the exact device, firmware, driver and OS tuple, and a device that is not
available keeps this row open, never waived.

**AUDIO-DRV-HW** (Open). Requires: AUDIO-DRV.

Real-hardware half (user decision 2026-10-27; implementation and simulator qualification are in AUDIO-DRV): external USB
and Thunderbolt audio interfaces, ASIO hardware drivers, aggregate clocking and drift-corrected duplex across two
physical interfaces, hot-plug of real devices, and the 32- and 64-frame round-trip latency board against Reaper on that
hardware. Every device purchase, account and SDK access is approved by the user first; the evidence names the exact
device, firmware, driver and OS tuple, and a device that is not available keeps this row open, never waived.

**MIDI-UMP-HW** (Open). Requires: MIDI-UMP.

Real-hardware half (user decision 2026-10-27; implementation and simulator qualification are in MIDI-UMP): physical MIDI
2.0 devices: UMP endpoints, function blocks, MIDI-CI discovery and Property Exchange, MPE and MIDI 1.0 translation with
real controllers and synthesizers, and timestamp jitter measured on the wire. Every device purchase, account and SDK
access is approved by the user first; the evidence names the exact device, firmware, driver and OS tuple, and a device
that is not available keeps this row open, never waived.

**NET-TIME-HW** (Open). Requires: NET-TIME.

Real-hardware half (user decision 2026-10-27; implementation and simulator qualification are in NET-TIME): PTP (IEEE
1588-2019) and gPTP (802.1AS) with hardware timestamps on PTP-capable NICs and a gPTP switch, against a reference
grandmaster, `SO_TXTIME` launch-time scheduling on real NICs, and the clock-quality value validated against that
reference. Every device purchase, account and SDK access is approved by the user first; the evidence names the exact
device, firmware, driver and OS tuple, and a device that is not available keeps this row open, never waived.

**HESAP-GPU-ALL-HW** (Open). Requires: HESAP-GPU-ALL, ROCM.

Real-hardware half (user decision 2026-10-27; implementation and simulator qualification are in HESAP-GPU-ALL): AMD
hardware conformance of every hesap GPU op and the claimed determinism certificates on physical AMD, Apple and NVIDIA
devices, with the full peer boards rerun on each. Every device purchase, account and SDK access is approved by the user
first; the evidence names the exact device, firmware, driver and OS tuple, and a device that is not available keeps this
row open, never waived.

**6.3-HW** (Open). Requires: 6.3.

Real-hardware half (user decision 2026-10-27; implementation and simulator qualification are in 6.3): OpenXR on physical
headsets (a Meta Quest through its PC link and standalone, a SteamVR headset, and a Windows Mixed Reality or other
OpenXR runtime), session, input, compositor, reprojection and comfort budgets measured on each, controllers and hand
tracking. Every device purchase, account and SDK access is approved by the user first; the evidence names the exact
device, firmware, driver and OS tuple, and a device that is not available keeps this row open, never waived.

**6.1-HW** (Open). Requires: 6.1.

Real-hardware half (user decision 2026-10-27; implementation and simulator qualification are in 6.1): physical iOS and
Android phones and tablets across GPU vendors (Apple, Adreno, Mali, PowerVR), thermal throttling and battery budgets,
lifecycle and audio on device, and the renderer, UI and input qualification on that hardware. Every device purchase,
account and SDK access is approved by the user first; the evidence names the exact device, firmware, driver and OS
tuple, and a device that is not available keeps this row open, never waived.

**6.0-HW** (Open). Requires: 6.0.

Real-hardware half (user decision 2026-10-27; implementation and simulator qualification are in 6.0): console platform
packs with actual SDK and dev-kit access under the platform holders' agreements, each with its public capability
boundary, the AOT exports run on the dev kit, and the platform certification checklists passed. Every device purchase,
account and SDK access is approved by the user first; the evidence names the exact device, firmware, driver and OS
tuple, and a device that is not available keeps this row open, never waived.

**6.4-HW** (Open). Requires: 6.4.

Real-hardware half (user decision 2026-10-27; implementation and simulator qualification are in 6.4): a physical
multi-node cluster with a high-speed interconnect (InfiniBand or RoCE) and multi-GPU nodes: MPI and HESAP-DIST strong
and weak scaling, failure recovery and job scheduling at matched residuals against PETSc, hypre and MUMPS on that
cluster. Every device purchase, account and SDK access is approved by the user first; the evidence names the exact
device, firmware, driver and OS tuple, and a device that is not available keeps this row open, never waived.

**VPROD-HW** (Open). Requires: VPROD.

Real-hardware half (user decision 2026-10-27; implementation and simulator qualification are in VPROD): an LED volume
with inner and outer frustums, genlock and framelock hardware, an OpenTrackIO camera tracking system and a cinema camera
with lens encoders, with lens and LED colour calibration measured on set. Every device purchase, account and SDK access
is approved by the user first; the evidence names the exact device, firmware, driver and OS tuple, and a device that is
not available keeps this row open, never waived.

**AUTOPILOT-HW** (Open). Requires: AUTOPILOT.

Real-hardware half (user decision 2026-10-27; implementation and simulator qualification are in AUTOPILOT): MAVLink 2
HITL with PX4 and ArduPilot flight controllers, uXRCE-DDS on the controller, and timing and failure injection with the
controller in the loop; physical flight remains separately authorized. Every device purchase, account and SDK access is
approved by the user first; the evidence names the exact device, firmware, driver and OS tuple, and a device that is not
available keeps this row open, never waived.

**EMBED-HW** (Open). Requires: EMBED.3, 3.1.18.

Real-hardware half (user decision 2026-10-27; implementation and simulator qualification are in EMBED.3): HIL on the
declared physical boards, including the 3.1.18 NEON, RTOS and deterministic resource-bound targets: host/target
equivalence, timing, resource and calibration evidence on the boards; physical actuation remains a separate capability.
Every device purchase, account and SDK access is approved by the user first; the evidence names the exact device,
firmware, driver and OS tuple, and a device that is not available keeps this row open, never waived.

## Planned clause texts

The JSON's `text_edits` holds each clause; the applier appends it to the row's text unless the row already contains it.

- **GPU-FORENSICS** appends: **Hardware split** (2026-10-27, user decision): the vendor tools whose hardware is on this
  host qualify here; the other vendors' tools on their own hardware are GPU-FORENSICS-HW.
- **QUAL.1** appends: **Hardware split** (2026-10-27, user decision): this row qualifies on every device and software
  adapter available here: this host's GPU and CPU, the WARP, lavapipe and SwiftShader software adapters, the WSL Mesa
  stack, each GPU-TIER.0 tier forced through the Vulkan profiles layer and DX12 feature-level clamps, low-VRAM and UMA
  budgets imposed by caps, CPU levels through Intel SDE, and the Proton run of the Windows build on the Linux lanes; the
  physical multi-vendor matrix and the Steam Deck device are QUAL.1-HW.
- **QUAL.2** appends: **Hardware split** (2026-10-27, user decision): clean-machine exports, module exclusion and
  recovery qualify here on clean virtual machines and the hosted Windows and Linux runners; the representative hardware
  set and rr on a bare-metal Linux host with performance counters are QUAL.2-HW.
- **UX-QUAL.1** appends: **Hardware split** (2026-10-27, user decision): pen, touch and gamepad tasks run here through
  the operating systems' synthetic input (Windows synthetic pointer injection, Linux uinput virtual devices) and the
  screen readers as software; physical pens, touchscreens, gamepads and haptics are UX-QUAL-HW.
- **MED-HW** appends: **Hardware split** (2026-10-27, user decision): this row qualifies on this host's GPU video
  engines and the owned software fallbacks, with each other vendor adapter built and conformance-tested against recorded
  bitstreams; the AMD, Intel and NVIDIA device matrix is MED-HW-QUAL.
- **AUDIO-DRV** appends: **Hardware split** (2026-10-27, user decision): this row qualifies on this host's audio
  endpoints and virtual loopback devices; external interfaces, ASIO hardware and two-interface clock drift are
  AUDIO-DRV-HW. The CoreAudio backend is carried by MAC.2.
- **MIDI-UMP** appends: **Hardware split** (2026-10-27, user decision): this row qualifies on the Windows MIDI Services
  and ALSA virtual and loopback UMP endpoints with a MIDI-CI responder simulator; physical MIDI 2.0 devices are
  MIDI-UMP-HW. The CoreMIDI transport is carried by MAC.2.
- **NET-TIME** appends: **Hardware split** (2026-10-27, user decision): this row qualifies with software timestamps,
  NET-SIM clock skew, steps and leap smear, and linuxptp in software timestamping mode between virtual hosts;
  hardware-timestamped PTP and gPTP against a reference grandmaster are NET-TIME-HW.
- **ROCM.1** appends: **Hardware split** (2026-10-27, user decision): implemented and qualified at its prerequisites
  (after the hesap GPU and ML core): HIP paths compiled for the AMDGPU targets with offline code-object validation and
  disassembly checks, executed through HIP's NVIDIA platform on this host and through chipStar (HIP on SPIR-V with
  OpenCL or Level Zero); actual AMD hardware stays ROCM.2, at the end of the table.
- **HESAP-GPU-ALL** appends: **Hardware split** (2026-10-27, user decision): every compiler, op and runtime obligation,
  the CLI and the peer boards qualify here on NVIDIA (this host), Apple (hosted macOS runners), WebGPU and AMD through
  ROCM.1's routes; AMD hardware conformance and the determinism certificates on physical devices are HESAP-GPU-ALL-HW.
- **6.3** appends: **Hardware split** (2026-10-27, user decision): implemented and qualified at its prerequisites on
  Monado with its simulated headset and controllers, the Meta XR Simulator and the Khronos OpenXR conformance suite in
  its simulated modes; physical headsets are 6.3-HW.
- **6.1** appends: **Hardware split** (2026-10-27, user decision): implemented and qualified at its prerequisites on the
  Android emulator (x86_64 and ARM64 images with host-GPU and SwiftShader Vulkan) with the NDK, and the iOS Simulator on
  hosted macOS runners, with thermal and lifecycle events injected through the emulators; physical phones and tablets
  are 6.1-HW.
- **6.0** appends: **Hardware split** (2026-10-27, user decision): implemented and qualified at its prerequisites as a
  console-class target profile built from public information: AOT-only CEIR-JIT.3 exports, fixed memory and thread
  budgets, suspend, resume and constrained-storage lifecycles, certification-style checks, and the Microsoft GDK where
  it is publicly available; the NDA SDKs and dev kits are 6.0-HW.
- **6.4** appends: **Hardware split** (2026-10-27, user decision): implemented and qualified at its prerequisites with
  single-node multi-process MPI and a containerized multi-node cluster (containers or virtual machines on a virtual
  network under NET-SIM faults, with a Slurm or Kubernetes job layer), scaling measured on the available cores and GPUs;
  a physical cluster is 6.4-HW.
- **AUTOPILOT** appends: **Hardware split** (2026-10-27, user decision): SITL qualifies here; HITL on flight controllers
  is AUTOPILOT-HW.
- **EMBED.3** appends: **Hardware split** (2026-10-27, user decision): SIL and PIL qualify here on instruction-set
  simulators (QEMU and Renode models of the declared targets) with cycle-level timing where the simulator models it; HIL
  on physical boards is EMBED-HW.
- **VPROD** appends: **Hardware split** (2026-10-27, user decision): this row qualifies with cluster rendering of inner
  and outer frustums across render nodes on this host and virtual machines, software framelock over NET-TIME,
  OpenTrackIO from recorded and simulated tracking streams and an LED wall simulated as a calibrated virtual display;
  the LED volume, genlock and tracking hardware are VPROD-HW.
- **SCI-CONTRACT.2** appends: **UI clause** (2026-10-27, user decision): the UI adapter is qualified here at the
  reflected property layer (REFLECT unit descriptors); the crd-ui unit fields are carried by I2D-8.a.
- **I2D-8.a** appends: **Carried from SCI-CONTRACT.2** (2026-10-27, user decision): unit-aware display and entry of
  quantities through the REFLECT unit descriptors, the strip-compute-retag bridge at the field boundary, unit and
  dimension errors as diagnostics, and the SCI-CONTRACT.2 migration fixtures replayed through the UI.
- **SCRIPT.4** appends: **UI clause** (2026-10-27, user decision): the GUI invocation is qualified here through the
  ImGui debug surface, CLI and MCP; the crd-ui command palette and console are carried by I2D-8.d.
- **EXT.2** appends: **UI clause** (2026-10-27, user decision): widget deregistration is qualified here as an ownership
  and quiescence contract independent of the UI; the crd-ui widgets and panels are carried by I2D-3.a.
- **I2D-3.a** appends: **Carried from EXT.2** (2026-10-27, user decision): widgets and panels an extension registers are
  owned UiWorld nodes, deregistered generation-safely on uninstall or reload with no dangling bindings, callbacks or
  focus.
- **CMD.3** appends: **UI clause** (2026-10-27, user decision): the one-commit drag is qualified here as a transaction
  (begin, update, then one commit or an exact cancel as one journal entry) headless and from CLI and MCP; crd-ui drags
  are carried by I2D-8.d and gizmo drags by EDITOR.2. **Formal conformance** (2026-10-27, user decision): the FORMAL.1
  journal model is re-checked against this implementation: its states and actions are refined to the implemented
  journal, and recorded runs are validated as traces of the specification in CI.
- **I2D-8.d** appends: **Carried from SCRIPT.4** (2026-10-27, user decision): the command palette and the console invoke
  debug and cheat commands through the same metadata, shipping allowlists and capabilities as CLI and MCP, with no
  unguarded remote code execution. **Carried from CMD.3** (2026-10-27, user decision): every crd-ui drag (sliders,
  dials, numeric scrubbing, reordering, drag and drop) is one CMD.3 preview transaction that commits once or cancels
  exactly.
- **EDITOR.2** appends: **Carried from CMD.3** (2026-10-27, user decision): each transform gizmo drag is one CMD.3
  preview transaction, committed once or cancelled exactly, and undo restores the pre-drag state.
- **MAC.1** appends: **Carried clauses** (2026-10-27, user decision): Apple notarization from SUPPLY.3, the
  NSAccessibility adapter from I2D-8.e and the macOS half of JOBS-PORT.2's ARM64 qualification.
- **MAC.2** appends: **Carried clauses** (2026-10-27, user decision): the CoreAudio backend from AUDIO-DRV, the CoreMIDI
  UMP transport from MIDI-UMP and Cocoa plugin-editor embedding from PLG-7.
- **FORMAL.1** appends: **Conformance** (2026-10-27, user decisions): two models describe designs implemented before
  this row, the flight recorder's global sequence (DIAG.11b) and the CRASH-FIX.2 agent-lease protocol (claim, renew,
  expiry, release and takeover); each is checked against its shipped implementation by validating recorded runs as
  traces of the specification. Four more models are added with the same conformance rule: OPS.3 scheduler leases with
  idempotent outputs and checkpoints, NET-SCALE entity handoff and dynamic re-sharding, SCRIPT.3 hot-reload quiescence,
  state migration and exact rollback, and SHIP.3 staged rollout (rings, halts and rollback). RAH-7.b, RAH-7.d, RAH-7.e,
  CMD.3, CMD.4, COLLAB.1 to COLLAB.3, SCRIPT.3, NET-SCALE, OPS.3 and SHIP.3 re-check their models against the
  implemented designs.
- **CRASH-FIX.2** appends: **Formal conformance** (2026-10-27, user decision): the agent-lease protocol is modeled in
  FORMAL.1, which follows this row and validates this implementation's recorded runs (including lease expiry, takeover
  and a lost holder) as traces of the specification; a later change to the protocol re-checks the model.
- **OPS.3** appends: **Formal conformance** (2026-10-27, user decision): the FORMAL.1 scheduler-lease model (leases,
  idempotent outputs and checkpoints, failure recovery) is re-checked against this implementation, and recorded runs
  with injected node loss are validated as traces of the specification in CI.
- **NET-SCALE** appends: **Formal conformance** (2026-10-27, user decision): the FORMAL.1 handoff and re-sharding model
  is re-checked against this implementation: no entity is owned by two servers or none, no accepted state is lost across
  a handoff, a re-shard or a server loss, and recorded NET-SIM runs at scale are validated as traces of the
  specification.
- **SCRIPT.3** appends: **Formal conformance** (2026-10-27, user decision): the FORMAL.1 hot-reload model (quiescence,
  state migration, stale handles, exact rollback on failure) is re-checked against this implementation, and recorded
  reloads with injected failures are validated as traces of the specification.
- **SHIP.3** appends: **Formal conformance** (2026-10-27, user decision): the FORMAL.1 staged-rollout model (rings, halt
  on regression, rollback, resumable patches) is re-checked against this implementation, and recorded rollouts are
  validated as traces of the specification.
- **RAH-7.b** appends: **Formal conformance** (2026-10-27, user decision): the FORMAL.1 staged-commit model is
  re-checked against this implementation: its states and actions are refined to the implemented stage, commit, last-good
  and retirement steps, and recorded runs are validated as traces of the specification in CI.
- **RAH-7.d** appends: **Formal conformance** (2026-10-27, user decision): the late-failure and conservation runs are
  validated as traces of the FORMAL.1 staged-commit model.
- **RAH-7.e** appends: **Formal conformance** (2026-10-27, user decision): the fallback-while-compiling path is added to
  the FORMAL.1 staged-commit model and re-checked against this implementation.
- **CMD.4** appends: **Formal conformance** (2026-10-27, user decision): dry-run, replay and cancellation conform to the
  FORMAL.1 journal model; recorded replays are validated as traces of the specification.
- **COLLAB.1** appends: **Formal conformance** (2026-10-27, user decision): the reviewed host design and the FORMAL.1
  host model are made to agree before COLLAB.2 starts.
- **COLLAB.2** appends: **Formal conformance** (2026-10-27, user decision): durable sync, concurrent edits and offline
  reconnect are re-checked against the FORMAL.1 host model, and recorded sessions are validated as traces of the
  specification.
- **COLLAB.3** appends: **Formal conformance** (2026-10-27, user decision): recovery, failover, revocation and
  stale-client behaviour are re-checked against the FORMAL.1 host model, including no lost accepted edits.
- **PHYS-QUAL** appends: **Close** (2026-10-27, user decision): physics qualification closes only with live
  visualization and debugging (EYL-3); PHYS-QUAL.1 and PHYS-QUAL.2 run before it.

## Part 5 placement

The 20 Part 5 rows landed at commit d47348d6 and are placed at the research's positions: SHIP.0 after DATA-TRUST
(P13); SHIP.1 to SHIP.3 after EDITOR.5 and SHIP.4, SHIP.5 after EDITOR.7 (P23); COMPLY.5, SHIP.6, SHIP.7, COMPLY.8,
SHIP.8, SHIP.9, SHIP.10, EXT.5 and EXT (P24); SHIP.11 and SHIP before QUAL.2 (P27); the store rows after their
platforms (P31). The batch's Requires changes are kept: AGENT.1, EXT and EXT.5 now carry cells that contain the plan's
corrections, so those fixes were dropped; EDITOR keeps SHIP.4 and SHIP.5 and adds DEBT-2; 6.3 keeps CEIR-JIT.3 (so it
moves after CEIR-JIT.3) and adds the plan's prerequisites; SHIP.3 gains FORMAL.1 (decision 13).

One addition is reversed, with the reason: JOBS-PORT.2 was given SHIP.5 as a prerequisite, but both rows' own text
says the opposite ("After ARM64 qualifies, SHIP.4 and SHIP.5 add Windows and Linux ARM64 packages"; "ARM64 packages
join after JOBS-PORT.2"), and keeping it would move the ARM64 port, CEIR-JIT.1, EYLEM-v9b and the macOS release behind
the editor. JOBS-PORT.2 stays in the foundation and SHIP.4 and SHIP.5 Require JOBS-PORT.2.

The STORE family landed at da0a5812 (RESEARCH-ADOPT Done) and is placed by its Requires: STORE.0 to STORE.6 after
EXT in P24, before SHIP.10 (which now Requires STORE.5); STORE.7 to STORE.9 after SHIP in P27, before QUAL.2 (which
Requires STORE.8); STORE-MAC after SHIP-MAC and STORE-WEB after SHIP-WEB in P31; STORE-SOCIAL, the STORE parent and
STORE-MOBILE after NET-LIVE in P32. The batch changed no Requires cell the plan corrects.

## Decisions

All twelve items were accepted on 2026-10-27, items 2, 3, 6, 8, 10 and 12 tightened, and decision 13 adopted; all
are applied:

1. The backbone block before the renderer library: accepted; the ADR-0129 amendment above.
2. PHYS-QUAL.1 runs without EYL-3, but the PHYS-QUAL parent Requires EYL-3 and closes in P28.
3. UI-consumer clauses: carried by I2D-8.a, I2D-8.d, I2D-3.a and EDITOR.2, cross-linked.
4. LANG.0 without I2D-0: accepted; UI-specific language needs enter CHIR through LANG editions.
5. The ML core and AI.1-.4 before the renderer: accepted.
6. Hardware split: every hardware-gated row has a simulator half in place and a real-hardware half in P40.
7. SUPPLY.3 before EXT.4: accepted.
8. EXT.5 and the EXT parent sit with SHIP.9 and the STORE family (P24).
9. Review rows reached earlier: accepted.
10. COLLAB.1 Requires NET.5, DOC, NET-SERVER, NET-REACH, the privacy child COMPLY.2, the online-safety child
    COMPLY.5 (it names COLLAB.1), the communications child COMPLY.9 (voice and video in collaboration) and FORMAL.1.
11. Ownership overlaps: settled as listed under single owners, with CRC-32 in the foundation.
12. Formal conformance on every implementing row, as listed.

13. Decision 13 (adopted): five more formal models with conformance, as listed under formal-model conformance.

## Applying the plan

`python build/ci-final/apply_roadmap_order.py <roadmap> build/ci-final/roadmap-order.json [--check]`

It inserts the 16 real-hardware rows of `pending_rows` that the table lacks, rewrites the table in the JSON's order,
sets Later to Open, applies the Requires fixes (pending ones once their rows exist) and the `requires_add`
prerequisites of pending rows that have landed, appends the planned clauses, renumbers, keeps every other byte of every
existing row and the line endings, and refuses (listing the rows) when a row is unassigned, unknown, placed twice, a
planned Requires cell changed since generation, an inserted row lacks a cell or its Detail and ADR sources, or any order
rule of the validator would break. When a fix is refused for a changed cell, re-base it on the new cell (keep the
operator's additions, add the plan's) and update `requires_fixes` and `requires_before`; after one apply the guard
refuses a second one, by design. The operator owns the validator change that rejects the Later state and commits the
ADR-0129 amendment with the apply.

Final dry run (2026-10-27, on a copy of the live table at commit da0a5812, 1068 rows, implementor paused): the 16
real-hardware rows inserted (1084 rows), 860 positions changed, 328 Later rows set to Open, 242 Requires cells
corrected, 41 clauses appended, zero dependency, parent and gate violations; the slice and source cells of the
existing rows unchanged, their text changed only by the planned clauses, every line outside the table byte-identical;
`scripts/check-master-plan.py` run against the copy: PASS. Nothing is deferred and no row is unassigned.
