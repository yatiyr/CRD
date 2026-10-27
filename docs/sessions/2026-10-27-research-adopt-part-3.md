# RESEARCH-ADOPT Part 3: building with Cerid (2026-10-27)

<!-- doc-role: session -->
> Session record; live work and status belong only to [ROADMAP](../ROADMAP.md). Rules: [AGENTS](../../AGENTS.md).

[RESEARCH-ADOPT](../ROADMAP.md#slice-research-adopt) applied every row of the Part 3
[proposal table](../research/2026-10-26-building-with-cerid-2026-comparison.md#gaps-and-proposed-roadmap-changes),
which the user approved on 2026-10-26. Placement is by anchor, because the table's Order numbers were stale. The table
was renumbered to 1,017 rows. With Parts 1 and 2 already in, RESEARCH-ADOPT is Done.

## New rows (44)

- After DIAG: [MCP-ADAPT.0](../ROADMAP.md#slice-mcp-adapt.0). After CMD.4: [MCP-ADAPT](../ROADMAP.md#slice-mcp-adapt).
- After NETCRASH: [IR-FUZZ](../ROADMAP.md#slice-ir-fuzz), [TEST-METH](../ROADMAP.md#slice-test-meth), then
  [SUPPLY.1](../ROADMAP.md#slice-supply.1), SUPPLY.4, SUPPLY.2 and [SEC.1](../ROADMAP.md#slice-sec.1).
- After GPU-TIER.0: [IMG-ORACLE](../ROADMAP.md#slice-img-oracle), then [FORMAL.1](../ROADMAP.md#slice-formal.1).
- After RAH-7: [CKIR-LANG.1](../ROADMAP.md#slice-ckir-lang.1). After HGP-1: CKIR-LANG.3. After HGP-4: CKIR-LANG.2.
- After I2D-0: [LANG.0](../ROADMAP.md#slice-lang.0). After REFLECT: [LANGTOOL.1](../ROADMAP.md#slice-langtool.1) to .6.
- After CMD.2: [API-STAB.0](../ROADMAP.md#slice-api-stab.0). After SCRIPT: [CEIR-JIT.0](../ROADMAP.md#slice-ceir-jit.0).
- After EXT.3: [SUPPLY.3](../ROADMAP.md#slice-supply.3), [EXT.4](../ROADMAP.md#slice-ext.4), then EXT.5 (Later).
- After AGENT.1: [AGENT-DOCS](../ROADMAP.md#slice-agent-docs). After AGENT.5:
  [AGENT-BENCH](../ROADMAP.md#slice-agent-bench).
- After I2D-6: [I18N.1](../ROADMAP.md#slice-i18n.1). After I2D-8: [A11Y-GAME](../ROADMAP.md#slice-a11y-game).
- After D7E-0: [VCS.1](../ROADMAP.md#slice-vcs.1). After EDITOR.1: VCS.2.
- After NATIVE: CEIR-JIT.1 to .3, then [DX.1](../ROADMAP.md#slice-dx.1) to .4.
- After CHIR-SCI.3: [REPL.0](../ROADMAP.md#slice-repl.0). After CHIR-SCI.4: REPL.1, REPL.2. After NOTEBOOK.1: REPL.3.
  After NOTEBOOK.5: REPL.4.
- After INTEROP.1: [INTEROP.3](../ROADMAP.md#slice-interop.3) (Later).

New rows take the state of their neighbours. Each carries the table cell and the area's gap and principle text: what
is swapped live, which module owns the numerics, the SSE2 or older-GPU fallback, the peers, and which downloads need
the user's approval first.

## Conflicts

- **After OWN.** The table put IR-FUZZ, SUPPLY.1 and their followers after OWN. The later user decision of 2026-10-27
  puts the [NETCRASH](../ROADMAP.md#slice-netcrash) window directly after OWN. They sit after NETCRASH and before AUD-2:
  still after OWN, still before the renderer.
- **CMD.4 and MCP-ADAPT.** The table places MCP-ADAPT after CMD.4 and requires CMD.4, and also amends CMD.4 to require
  MCP-ADAPT, which is a cycle. CMD.4's MCP adapter clause moves to MCP-ADAPT. CMD.4 keeps the CLI and RPC adapters and
  names MCP-ADAPT in its text, without the requirement.
- **AGENT.1 to .3.** The table gives all three the MCP-ADAPT requirement. AGENT.1 carries it, and AGENT.2 and AGENT.3
  reach it through AGENT.1.
- **Part 1 and Part 2 IDs** (GPU-TIER.0, WEB-CEIR, SCIVIZ-PLOT) now exist and are linked, not marked "(P1)" or "(P2)".

## Requirements added beyond the table

Each producer sits earlier in the table and is named by its consumer's text:
- REPL.1: LANGTOOL.1. REPL.3: SCIVIZ-PLOT. NOTEBOOK: REPL.4.
- AGENT-BENCH: IMG-ORACLE. CEIR-JIT.1: IR-FUZZ.
- QUAL.2, 6.0 and 6.1: CEIR-JIT.3 (the CEIR-JIT.3 cell names them).
- MLR-1: CKIR-LANG.3. D7E-8 and D7E-10: LANGTOOL.3.
- DOC.2: REFLECT.2. OPS.2: SUPPLY.4 beside SUPPLY.2. INTEROP: INTEROP.3.

## Amendments

LANG.1 to LANG.5, I2D-9.d, D7E-8, D7E-10, CMD.4, AGENT.1, AGENT.2, AGENT.3 (OAuth profile and content provenance),
AGENT, CHIR-SCI, NOTEBOOK.1, NOTEBOOK, QUAL.2, 6.0, 6.1, OPS.2 (its supply-chain clause is struck and moved), OWN.1,
REFLECT.2, DOC.2, INTEROP.1, INTEROP, I2D-8.e, MLR-1 and NATIVE.2.

Gap text with no table row was carried onto the rows the research names: the isolation tiers (WASI 0.3 components, a
restricted process for native code) onto EXT.3, and the NSAccessibility and ARIA adapters onto MAC.1 and WEB.2.

## ADR and design notes

- [ADR-0065](../decisions/0065-hesap-numerical-substrate.md) §28: REPL.0 decides the §8 and §9 `crd-hesap-repl`
  placement. IMG-ORACLE's metric is recommended for crd-hesap, and OWN.1 decides.
- [ADR-0128](../decisions/0128-chir-0-language-binding-decisions.md) §9: LANG.0 replaces the first-consumer triggers.
- [ADR-0108](../decisions/0108-ceir-owned-language-stack-supersedes-cpp-only-scripting.md) §7a: an external code
  generator below CEIR is CEIR-JIT.0's decision, and the owned option leads.
- [ADR-0081](../decisions/0081-agent-native-engine-cli.md) §6.4: MCP-ADAPT.0 and MCP-ADAPT own spec currency.
- [ADR-0130](../decisions/0130-system-qualification-and-agent-driven-products.md): the AGENT-BENCH board and the
  SUPPLY rows.
- Owner pointers in the [test instruments](../design/test-instruments.md), [CI tiers](../design/ci-tiers.md) and
  [public consumption](../design/public-consumption.md) contracts.

## Downloads

Nothing was downloaded. The rows that name a download needing the user's approval are MCP-ADAPT.0, LANGTOOL.1,
LANGTOOL.5, CEIR-JIT.0, IR-FUZZ, TEST-METH, SUPPLY.1, SUPPLY.2, IMG-ORACLE, FORMAL.1, REPL.3, AGENT-BENCH and I18N.1.
The research's [download table](../research/2026-10-26-building-with-cerid-2026-comparison.md#needs-a-download) lists
them all.

## CI at the start of the batch

Completed runs 40422239096 and 40424055075 were cancelled before any job ran. In-progress run 40409214033 (head
cf4b8a13) failed `simd convert_truncate gives INT32_MIN` on linux-gcc-relwithdebinfo (job 121876231527), -release
(121876231573) and -shipping (121876231690). e9828cc2 already fixed that GCC fold. No complete-tier run was dispatched.
Push runs are pending, and this batch changed only documentation. DIAG.11c and DIAG.12 still wait for the final
complete-tier run.

## Left

Nothing for RESEARCH-ADOPT. A fourth research document,
`docs/research/2026-10-27-networking-and-crash-pipeline-2026-comparison.md`, is still being written in the working
tree. Its proposed rows are not adopted, because the user has not approved them yet.
