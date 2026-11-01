# 2026-11-01 — MCP-ADAPT.0: the MCP Inspector drives `ceridc mcp`

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [MCP-ADAPT.0](../ROADMAP.md#slice-mcp-adapt.0), its remaining item (1); the protocol work is in the
[protocol currency session](2026-11-01-mcp-adapt-0-protocol-currency.md). Run on `cerid-track-b` (spot
c4-standard-8, Ubuntu 24.04, GCC 13.3, no GPU) in `linux-gcc-debug`. The contract is
[agent MCP server](../design/agent-mcp.md#conformance).

## CI first

No GitHub Actions run covers this track's commits 03571fa3 to 77a60960 yet: 41272209476 (65b68c3c) and
41272584943 (77a60960) were cancelled by newer pushes before any job started, and 41273574588 (3e33fc5d) is
pending. Nothing from CI can be traced to this track; the hosted evidence stays item (2) of the row.

## The Inspector

`@modelcontextprotocol/inspector` 2.13.0 (released 2026-10-28; its client is `@modelcontextprotocol/client` 2.4.0)
is a testing tool, a pre-approved download. It is pinned as the dashboard publisher's wrangler is:
`tests/tools/ceridc/mcp/inspector/package.json` names the exact version and the committed `package-lock.json`
(lockfile 3, 157 packages, generated with the pinned Node's npm 11.19.1) addresses every transitive package by its
registry URL and SHA-512 integrity, which `npm ci` checks; the Inspector's own entry matches the registry's published
integrity. The package's only install script (`install-clients.mjs`) exits at once when it is installed as a
dependency, so `npm ci --ignore-scripts` leaves nothing out. Its 2.x command line client has a `--protocol-era`
option (`legacy`, `auto`, `modern`), so it drives both revisions of the row, not only the handshake.

`tests/tools/ceridc/mcp_inspector.py install` installs it outside the checkout
(`~/.cerid/tools/mcp-inspector/<version>`, a stamp naming the lockfile's digest); `run --ceridc <ceridc>` drives it.
In each era the Inspector connects, lists the tools under `--strict` (its schema portability check) and makes seven
calls: import, cook, query, a query with an unknown argument (refused: the Inspector exits 5, `tool_is_error`), an
`instantiate` dry run with a number and a boolean typed by the Inspector from the input schema, `diag gpu.resources`
(headless: its `unavailable` items), and a `diag` with a smuggled `grant` (refused).

The Inspector starts the script's `relay` as its server: it runs `ceridc mcp`, copies every line both ways unchanged
and records it. Each transcript is checked against the pinned schemas exactly as the conformance run checks its own
traffic (one JSON message per line, LF alone; each request in the revision its era must use; every request,
notification and reply against its method's definition; each structured result against the tool's output schema),
and what the Inspector printed must equal the payload ceridc sent. A method the run does not know fails it, so a new
Inspector behaviour is read before it counts.

## The recorded run

`cerid-mcp-inspector-run/1`, 2026-11-01: Inspector 2.13.0, lockfile SHA-256 `146ac0fe…c63c2`, Node v24.22.0, ceridc
built from 77a60960 (this batch changes no C++). 27 Inspector runs, 153 recorded messages, all checked;
`--strict` reported no finding in any era; the Inspector's home stayed empty.

| Era | Opens with | Then | Revision of every request | Messages per call run |
|---|---|---|---|---|
| `legacy` | `initialize`, `notifications/initialized` | tools/list, tools/call | 2025-11-25 | 7 |
| `modern` | `server/discover` (per-request `_meta`, no handshake) | tools/list, tools/call | 2026-07-28 | 6 |
| `auto` | `server/discover`, which names 2026-07-28 first | tools/list, tools/call | 2026-07-28 | 6 |

The two refused calls in each era exit 5 with the refusal report as the tool error; the five others exit 0. Teeth,
each restored and rebuilt:

| Teeth (in `tools/ceridc/src/mcp.cpp`) | Caught by the Inspector run |
|---|---|
| no `resultType` on 2026-07-28 results | `the server/discover result does not validate in 2026-07-28: ... 'resultType' is required` |
| an unknown argument skipped instead of refused | `legacy tools/call query: the Inspector exited 0, expected 5` |

## Defects fixed on the way

- **The MCP fixtures' `.meta` id was malformed.** The conformance run (and the first Inspector run) wrote
  `uuid = "00112233445566770011223344556677"`; `ResourceId::parse` takes the 36-character form, so every cook
  printed `malformed .meta ..., regenerating` and gave the mesh a random id. Both now write the source root through
  one `write_sources` in `mcp_conformance.py` with `00112233-4455-6677-8899-aabbccddeeff`, and both require the
  query to list exactly that id (the conformance CTest passes with it).
- **The dashboard publisher extracted Node by hand.** `ci/dashboard/generate.py` fetched the Node pin into a temporary
  directory and extracted the whole archive itself. `scripts/install-node.py` is now the one installer: it goes
  through `pins.install_tree`, which `install-sde.py`'s whole-kit extraction moved into (member root check, the
  `data` filter, Windows hard links for links, staging and stamp), and adds `bin/node`'s own digest and the reported
  version. The node pin gained its `root`; `check-pins.py` lists the helper.

## Tests

- `scripts/test-repository-tools.py`: `McpInspector` (4 cases: a valid connection of each era; twelve ways a
  transcript fails, among them CR, a missing LF, two content blocks and `isError` against the report; the relay's
  byte-exact copy and record with the server's exit code; the lock's exact version, registry URLs and SHA-512
  integrity) and the Node installer (layout, the npm link, reuse by stamp, the binary digest, the member naming the
  binary, the reported version); the SDE installer's case still passes through the moved code. 262 cases run on
  Windows; the one failure, `test_generated_target_folders`, needs Ninja on the PATH of the host that runs it and
  fails the same way on the unchanged checkout. On the machine: `McpInspector`, `JsonSchemaSubset` and both
  installers pass.
- `scripts/test-ci-platform.py`: the publisher runs `npm ci` through the installed npm and the locked wrangler on the
  installed Node (99 cases pass).
- `crd-ceridc-mcp-conformance` passes with the corrected fixture.

## Remaining on the row

Hosted CI evidence of the ceridc, platform and conformance tests on the Windows (MSVC, clang-cl), ARM64 and scalar
(SSE2 floor) lanes, read from the first complete GitHub Actions run that covers 03571fa3 through this batch.
