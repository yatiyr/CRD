# 2026-10-31 — Three development tracks run the roadmap in parallel

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

## User order

The user, 2026-10-31 about 18:00: "do all hw resilience at once and also close the ... DIAG branch! and I want mcp
adapt to be done and own rows to be done! I want you to come to NET,COMPLY,CRASH-FIX!", then: "DO MCP ADAPT, REPO
HYGENE AND START ... NET,COMPLY AND CRASH FIX ... I WANT YOU TO START THEM and finish them".

## The tracks

Each track works its rows in table order, delivers to `main` by rebase, and never touches another track's rows.

| Track | Where it builds | Rows |
|---|---|---|
| A, the serial workflow | this host | [HW-RESILIENCE.1](../ROADMAP.md#slice-hw-resilience.1) to [HW-RESILIENCE](../ROADMAP.md#slice-hw-resilience) as one unit, then the DIAG close ([DIAG.11b](../ROADMAP.md#slice-diag.11b) to [DIAG](../ROADMAP.md#slice-diag)) |
| B | its cloud machine `cerid-track-b` | [OWN.1](../ROADMAP.md#slice-own.1) to [OWN](../ROADMAP.md#slice-own) first (NET.1 and COMPLY.0 require OWN), then [MCP-ADAPT.0](../ROADMAP.md#slice-mcp-adapt.0), then [REPO.3d](../ROADMAP.md#slice-repo.3d) to [REPO](../ROADMAP.md#slice-repo) |
| C | its cloud machine `cerid-track-c` | [NET.1](../ROADMAP.md#slice-net.1) to [NETCRASH](../ROADMAP.md#slice-netcrash): NET, COMPLY and CRASH-FIX |
| Optimization | its cloud machine `cerid-bench` | [SIMD-PERF](../ROADMAP.md#slice-simd-perf) and [HESAP-JOBS-RETUNE](../ROADMAP.md#slice-hesap-jobs-retune) |

- The cloud tracks' machines are spot `c4-standard-8` instances driven by `ci/bench_vm.py --name`, started for a
  batch's builds and tests and stopped after it. Windows coverage comes from CI.
- OWN.1 required HESAP-JOBS-RETUNE, which moved into the optimization track. Ownership does not depend on the
  retune's tuning, so OWN.1 now requires SIMD-DISPATCH.
- A row whose prerequisite another track still holds stays Partial and names that prerequisite; it closes when the
  prerequisite does (the master-plan check refuses a Done row with an incomplete prerequisite).
- The project's CPU quota is 64 vCPUs. The CI's cloud group drops to four machines, and an increase to 128 is
  requested (`cerid-ci-cpus`).
