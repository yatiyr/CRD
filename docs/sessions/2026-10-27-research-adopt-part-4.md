# RESEARCH-ADOPT Part 4: networking and the crash-to-fix pipeline (2026-10-27)

<!-- doc-role: session -->
> Session record; live work and status belong only to [ROADMAP](../ROADMAP.md). Rules: [AGENTS](../../AGENTS.md).

[RESEARCH-ADOPT](../ROADMAP.md#slice-research-adopt) applied every row of the proposal table ("Gaps and proposed
roadmap changes") of the [Part 4 research](../research/2026-10-27-networking-and-crash-pipeline-2026-comparison.md),
which the user approved on 2026-10-27 together with a legal and regulatory register (COMPLY). Placement is by
anchor, because the table's numbers were stale. The table was renumbered to 1,024 rows. RESEARCH-ADOPT stays Partial:
the COMPLY children are left.

## New rows (6)

- After NET.1: [NET-STD](../ROADMAP.md#slice-net-std), then [COMPLY.0](../ROADMAP.md#slice-comply.0).
- After NET.2: [HESAP-NET](../ROADMAP.md#slice-hesap-net).
- After 4.2a: [NET-PATH](../ROADMAP.md#slice-net-path), then [NET-REACH](../ROADMAP.md#slice-net-reach).
- Directly before 4.2c: [NET-TIME](../ROADMAP.md#slice-net-time) (Later, like 4.2c).

New rows take the state of their neighbours. Each carries its proposal text and the area's gap text: what is swapped
live (congestion controllers, crypto policy generations), which hesap module owns the numerics, the portable or SSE2
fallback, the peers and budgets, and which downloads need the user's approval first.

## COMPLY.0

The user's order is that networking, the crash pipeline and everything that touches data are secured against
malicious attacks, robust, and obey every applicable law, with no shortcuts, Turkish law always included. COMPLY.0 is
the register and its checks: one entry per obligation with its primary source, the date read, the owning row, a check
and a review cadence, and a counsel-review flag for interpretation. It lists every domain and jurisdiction the user
named, Turkish law (KVKK 6698 and 7499, 7545, 5651, 5809 and BTK, 6563, 6502, RTÜK, 5070, 5846, 5378, the Penal
Code's data and IT offences) as a first-class block. It sits after NET-STD and before NET.2, and NET.2 and
CRASH-FIX.1 require it, so the order is binding: the register exists before any row sends data off a machine.

Left for the next batch: the per-domain children at the rows they govern, each instrument read from its primary
source (EUR-Lex, mevzuat.gov.tr, kvkk.gov.tr, btk.gov.tr and the other regulators) with the date recorded, and the
COMPLY parent after the last child (the checker requires a parent after its children).

## Amendments

P-6 to P-32 were applied in place to NET.1, NET.2, NET.3, NET.4, 4.2a, NET-SIM, NET-DEBUG, NET.5, NETCRASH,
HW-RESILIENCE.1, CRASH-FIX.1 to .4, AGENT.3, OPS.1, WEB.5, 4.2c, 4.2d, NET-SCALE, NET-LIVE, 4.2e, NET, COLLAB.1,
COLLAB.2, NET-SERVER and ANTICHEAT. Requirements added: NET.3 HESAP-NET, 4.2a NET-STD, NET.5 NET-PATH and NET-REACH,
4.2c, NET-LIVE and NET NET-TIME, and NET.2 and CRASH-FIX.1 COMPLY.0.

Gap text with no table row went onto the rows the research names: YubiKey-class keys, rotation rehearsed in NET-SIM
and a runtime refusal of built-in capture keys (NET.2); the cross-lane ADR-0063 float corpus (NET-SIM);
favour-the-shooter limits and the HW-RESILIENCE.3 state hashes (4.2c); static and dynamic partitioning (NET-SCALE);
captions and the Dante licence as a user decision (NET-LIVE); service SLOs and cost per active user (OPS.1); Apple
`.ips` and Android tombstones (CRASH-FIX.1); the freestanding transport core for embedded targets (EMBED.2); the N-1
readers for long-lived artefacts and crypto agility by signed policy (NET-STD).

## Checksums and the CRC-32 duplicates

The research counts CRC-32 in "five modules" once and in three modules in P-15. The tree has three copies, all
CRC-32/IEEE (polynomial 0xEDB88320): `engine/foundation/perf/src/bundle.cpp`,
`engine/assets/resources/src/png_image.cpp` (`png_crc32`, also used by `png_encode.cpp` and `zip_archive.cpp`) and
`engine/numerics/hesap-tensor/include/crd/hesap/tensor/detail/io_zip.hpp`. OWN.1 records them.

P-15 names CRC32C with SSE4.2 paths, but PNG, ZIP and gzip mandate CRC-32/IEEE, a different polynomial that the
SSE4.2 instruction does not compute. The owned family therefore holds both: CRC-32/IEEE with carry-less multiply and
portable paths, CRC32C with SSE4.2, carry-less multiply and portable paths, and a 64-bit fast hash, each built per
SIMD-DISPATCH level.

The operator note asked for "one crd-hesap owner". P-15 says foundation-owned, OWN.1 already assigns hashing to the
foundation, and crd-perf (a foundation module that links only crd-core, crd-time, crd-jobs, crd-memory,
crd-containers and crd-toml) needs the checksum and may not depend on crd-hesap. The owner is the foundation hashing
module beside `containers/hash.hpp`; [ADR-0065](../decisions/0065-hesap-numerical-substrate.md) §29 records it.

## ADR and design notes

- [ADR-0035](../decisions/0035-networking-architecture.md): the Part 4 amendment (window additions, security pins,
  integrity under encryption, NET-TIME and the layers above).
- [ADR-0065](../decisions/0065-hesap-numerical-substrate.md) §29: HESAP-NET's placements; it amends §27's
  "hesap-comms stays modulation and OFDM".
- [System quality contract](../design/system-quality-contract.md#data-security-and-agent-access): integrity under
  encryption, honest security wording and the COMPLY register.
- [Runtime diagnostics](../design/runtime-diagnostics.md#existing-programme-ownership-after-diag): the crash rows'
  network context, grouping and CRA workflow, and OPS.1's OTLP export.

## Downloads

Nothing was downloaded. Every peer, tool, trace and dataset in the research's
[download list](../research/2026-10-27-networking-and-crash-pipeline-2026-comparison.md#needs-a-download) is named on
its row as needing the user's approval first: NET.2, NET.4, NET-SIM, NET-DEBUG, NET-PATH, NET-REACH, HESAP-NET,
NET-TIME (linuxptp, and a PTP-capable NIC as a purchase), CRASH-FIX.2, OPS.1, 4.2c, NET-LIVE, 4.2e, COLLAB.1 and
NET-SERVER.

## CI read

Every completed run from 40415857302 to 40431210227 was cancelled with no failed job. The in-progress run
40409214033 (head cf4b8a13) failed `simd convert_truncate gives INT32_MIN for NaN and out-of-range lanes` on
linux-gcc-relwithdebinfo, linux-gcc-release and linux-gcc-shipping; e9828cc2 already fixed that case. No new
complete-tier run is green, so DIAG.11c and DIAG.12 stay Needs CI.
