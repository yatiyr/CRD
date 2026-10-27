# RESEARCH-ADOPT Part 2: beyond games (2026-10-27)

<!-- doc-role: session -->
> Session record; live work and status belong only to [ROADMAP](../ROADMAP.md). Rules: [AGENTS](../../AGENTS.md).

[RESEARCH-ADOPT](../ROADMAP.md#slice-research-adopt) applied every row of the Part 2
[proposal table](../research/2026-10-26-beyond-games-2026-comparison.md#gaps-and-proposed-roadmap-changes), which
the user approved on 2026-10-26, together with the operator note on the
[network foundation window](2026-10-27-user-decision-network-foundation-window.md). Placement is by anchor, because
the table's Order numbers were stale. The table was renumbered to 973 rows.

## New rows (24)

- After HGP-5: [HESAP-SYM](../ROADMAP.md#slice-hesap-sym). After CHIR-SCI.2:
  [SCIVIZ-PLOT](../ROADMAP.md#slice-sciviz-plot).
- After MED-REVIEW: [HESAP-AUDIO-DSP](../ROADMAP.md#slice-hesap-audio-dsp). After MED-GPU-1:
  [MED-HW](../ROADMAP.md#slice-med-hw).
- After PLG-1: [AUDIO-DRV](../ROADMAP.md#slice-audio-drv), then [MIDI-UMP](../ROADMAP.md#slice-midi-ump). After
  PLG-6: [PLG-7](../ROADMAP.md#slice-plg-7), then [PLG-8](../ROADMAP.md#slice-plg-8).
- After AUDIO-CEIR.4: [AUDIO-SPATIAL](../ROADMAP.md#slice-audio-spatial), then
  [AUDIO-GAME](../ROADMAP.md#slice-audio-game).
- After WEB.1: [WEB-CEIR](../ROADMAP.md#slice-web-ceir). After SCI-CONTRACT: [VV-UQ](../ROADMAP.md#slice-vv-uq).
- After HGP-6: [HESAP-DIST](../ROADMAP.md#slice-hesap-dist). After BREP: MESH-SIM.1 to .3,
  [FE-CORE](../ROADMAP.md#slice-fe-core) and [FV-CORE](../ROADMAP.md#slice-fv-core). After CAD.2:
  [CAD-SOLVE](../ROADMAP.md#slice-cad-solve).
- After 4.2d: [NET-SERVER](../ROADMAP.md#slice-net-server), then [ANTICHEAT](../ROADMAP.md#slice-anticheat).
- After SIM.3: [SIM.4](../ROADMAP.md#slice-sim.4). The SIM parent now closes on it.
- After 8.1b: [AUTOPILOT](../ROADMAP.md#slice-autopilot). After 8.3b: [VPROD](../ROADMAP.md#slice-vprod).

New rows take the state of their neighbours (Open or Later). Each carries the full proposal text and the area's gap
and principle text: what is authored and swapped live, which crd-hesap module solves, the SSE2 or older-GPU fallback
and the peers. A row also states when a download needs the user's approval first.

## Conflicts and folds

- **ANTICHEAT.** The table placed it after NET.4. The user's window decision puts the server and anti-cheat rows
  with the application layers, after NET.5, and server-authoritative validation needs 4.2c. It sits after NET-SERVER
  and requires NET.4 (the table's requirement), 4.2c and NET-SERVER. NET.5 now names both rows, and the NET parent
  requires both.
- **Networking overlaps** are cross-linked, not merged:
  - 4.2d keeps the backend-neutral session API and its local stubs; NET-SERVER owns production matchmaking, backends,
    fleets and regions.
  - NET-SCALE runs on NET-SERVER's fleets and scales 4.2c's interest management and priority budgets across shards.
  - NET-LIVE deploys its forwarding servers through NET-SERVER and takes hardware codecs from MED-HW.
  - ANTICHEAT detects from NET-DEBUG's multi-peer recordings, and NET.4's adversarial suite runs against it.
- **Kept requirements.** 4.2c keeps NET.5: the table's 4.2b was the old requirement, which the window decision
  replaced. COLLAB.1 keeps NET.5 and DOC and adds NET, as the table says.
- **CONTROL.11 to .13.** Their Detail links point at the archived phase's reference reading lists (estimation, control,
  path planning), not at scope sections. They were reconciled as the reference lists of CONTROL.1 to .4, CONTROL.5 to .9
  and CONTROL.10, and close with them. CONTROL.13's link pointed at the first "Path planning" heading; it now points at
  the reading list (`#path-planning-1`).
- **Folded rows keep their IDs:** 8.2b closes against `hesap-autodiff` and HGP-4, 8.0c against 4.2e and CONTROL.10
  against NAV.3 and CONTROL.8.

## Requirements added beyond the table

Each consumer that the proposal text names gets the requirement, because each producer sits earlier in the table:
- SCIVIZ-PLOT: CHIR-SCI.3, NOTEBOOK.2, EYL-2, SCIVIZ.2, SCIVIZ.3.
- HESAP-DIST: 6.4.
- The EYLEM-v6c importers: 8.0a. EYLEM-v6f-aero-a to -d: 8.1a.
- DAW-APP.1: MIDI-UMP, PLG-8, HESAP-AUDIO-DSP and AUDIO-SPATIAL.
- WEB.1: SIMD-DISPATCH and GPU-TIER.0.
- WEB requires WEB-CEIR, CAD requires CAD-SOLVE, and CAD.1 requires BREP.2.
- NET-LIVE: MED-HW.

## Amendments

OWN.1, 8.2b, 8.0c, 8.0a, 8.1a, CONTROL.10 to .13, EYLEM-v7a, CFD.1 to .4, FEA.1 to .6 (FEA.6 owns the multiphysics
coupling), PLG-4, AUDIO-CEIR.1, MED-REVIEW, MED-2, CHIR-SCI.1, CHIR-SCI.2 (the `vmap` transform), WEB.1, WEB.4,
EXT.3, CAD.1, BREP.3, SIM.2, 4.2c, COLLAB.1, 8.3a to 8.3c, INTEROP.1, DAW-APP.1, 4.2e, SENSOR.2, 3.1.19 and EMBED.2.
Rows that only point at a new owner: CHIR-SCI.3, NOTEBOOK.2, EYL-2, SCIVIZ.2, SCIVIZ.3, 6.4, CAD.2, CAD, WEB, 4.2d,
NET-SCALE, NET-LIVE, NET.5 and NET.

The area 5 gap text with no table row was carried onto the rows the research names for it: the reactive execution
model and Jupyter interop on NOTEBOOK.1, and the format subsets on NOTEBOOK.3 (the download table names NOTEBOOK.3).

## ADR notes

- [ADR-0035](../decisions/0035-networking-architecture.md) is amended in place. The thin-socket Layer 1, the two-mode
  Layer 3 and the 8.0c and 8.0-module dependency clauses are struck, each with a forward link. A new amendment
  section records:
  - the NETCRASH window;
  - QUIC with datagrams as the primary transport;
  - the security baseline of NET.1 to NET.4;
  - NET-SIM as the gate for every layer, and NET-DEBUG inside `crd-net`;
  - the authored replication policies;
  - the layers above the session API: NET-SERVER, ANTICHEAT, NET-SCALE, NET-LIVE, COLLAB and the one ROS 2 bridge.
- [ADR-0065](../decisions/0065-hesap-numerical-substrate.md) §27 adds HESAP-DIST and HESAP-SYM to crd-hesap.
  [ADR-0093](../decisions/0093-hesap-dsp-cluster.md) adds the production audio DSP. The
  [ADR-0077](../decisions/0077-multi-domain-expansion-vision.md) ownership amendment records the domain folds.

## Downloads

Nothing was downloaded. These rows name a download that needs the user's approval before it is fetched: AUDIO-DRV,
MIDI-UMP, PLG-4, PLG-8, AUDIO-SPATIAL, HESAP-AUDIO-DSP, MED-2, NOTEBOOK.3, EXT.3, BREP.3, SIM.4 and AUTOPILOT. The
research's [download table](../research/2026-10-26-beyond-games-2026-comparison.md#needs-a-download) lists the rest.

## Gap text with no proposal row

No row was invented for:
- CAM and EDA reuse of GEOM-K.4 and GEOM-EXACT.1, with KiCad 10 as the EDA interop peer (area 4);
- the web peers, Unity and Godot web exports (area 6);
- embedded targets without an FPU (area 8, already EYLEM-v9c).

## CI at the start of the batch

Every completed run from 40388755720 to 40421965175 was cancelled with no failed job. In-progress run 40409214033
(head cf4b8a13) failed `simd convert_truncate gives INT32_MIN` on linux-gcc-relwithdebinfo, -release and -shipping;
e9828cc2 already fixed that GCC fold. No complete-tier run was dispatched: push runs are pending, and this batch
changed only documentation.

## Left

[Part 3](../research/2026-10-26-building-with-cerid-2026-comparison.md#gaps-and-proposed-roadmap-changes): every new
row and amendment, plus the design references and ADR notes they cite.
