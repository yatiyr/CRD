# 2026-11-01 — MCP-ADAPT.0: protocol currency for `ceridc mcp`

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [MCP-ADAPT.0](../ROADMAP.md#slice-mcp-adapt.0); its prerequisite [DIAG.8c](../ROADMAP.md#slice-diag.8c) is Done.
Built and tested on `cerid-track-b` (spot c4-standard-8, Ubuntu 24.04, GCC 13.3, no GPU) in `linux-gcc-debug`. The
contract is [agent MCP server](../design/agent-mcp.md); [ADR-0081](../decisions/0081-agent-native-engine-cli.md) has
the amendment.

## CI first

No complete GitHub Actions run covers this track's commits: every run since 0362b538 was cancelled by a newer push
before its jobs started, 41267272693 (9290a029, the commit under the OWN close) is still pending, and none had started
for 60e5ee51. One
finished job failed in run 41262267004 (8a243a34, the FFT track's docs commit): Repository checks
(windows-2025-vs2026), `scripts/test-ci-orchestrator-e2e.py` `test_1_webhook_build_runs_end_to_end`. Its build read
`passed` while the only job it listed, `plan`, was still `scheduled`: the build reached a terminal state before its
dynamic jobs were added. That is a state-ordering defect in the own CI orchestrator (`ci/coordinator`, last changed by
c2645814, add840d2 and 45a9c03e on 2026-10-31), not a timeout, and not from this track; it is named for the track that
owns `ci/` (the serial track) in the batch report. The `MemoryError` above it in the same job is a subprocess probe
inside `scripts/test-ci-platform.py`, whose 98 cases passed.

## The protocol

The 2026-07-28 revision is past the knowledge the earlier research used, so it was read from the specification pages
(listed in the contract) and the published schemas. What `ceridc mcp` does now, from `tools/ceridc/src/mcp.cpp`:

- **Dual era.** `McpServer` replaces the stateless `mcp_handle`. A request whose `params._meta` names `2026-07-28` and
  carries the client's capabilities is served statelessly; `2025-11-25` in `_meta` is `-32602` (that revision belongs
  to the handshake); any other version is `-32022` with `data.supported` and `data.requested`; `initialize` (with its
  three fields) answers `2025-11-25` and selects it for the connection; before it, only `ping` is served without
  `_meta`. A 2026-07-28 request after the handshake is still served by 2026-07-28, as the versioning page allows.
- **Results.** `resultType` and `_meta` server info on every 2026-07-28 result; `server/discover` and tools/list carry
  `ttlMs` 3 600 000 and `cacheScope` public (the tool set is fixed at start and the same for every caller); the server
  version is `CRD_VERSION_STRING` (it said `1.0.0`). `ping` is gone from 2026-07-28 (`-32601`). A tools/list cursor
  is `-32602`: the list is one page.
- **Envelope.** `jsonrpc` must be `"2.0"` (it was not checked); an error whose id cannot be read leaves `id` out, as
  MCP's schema defines the error response (the old `"id":null` fails both published schemas).
- **Tools.** One declaration per tool (name, title, description, arguments, report schema, hints) writes the input
  schema and drives the argument check. Names are in strict name order and follow the 2025-11-25 guidance (a
  `static_assert`). Input schemas are closed (`additionalProperties: false`); output schemas are `anyOf` the verb's
  success shapes and the refusal shape. Each report is the `structuredContent` and its text content; `isError` is read
  from the parsed report's `ok` (it was a substring search).
- **Validation errors are tool errors.** An unknown tool or non-object `arguments` stays `-32602`. Every argument fault
  (absent, unknown, given twice, mistyped, out of range, too long, NUL) is now the tool's refusal report as a tool error
  naming the argument, and nothing runs. Unknown arguments were silently ignored before: a misspelt `dry_run` wrote the
  file, and a smuggled `grant` reached the service (which refused it on its own grant); now neither reaches anything.
- **Render** reports why it stopped early (it answered `ok:false` without a reason).

## The stdio transport

- **Lines.** The loop read with `fgets` into a fixed 1 MiB buffer: a longer line was cut into pieces, each answered as
  its own (malformed) message, and `strlen` cut a line at a NUL byte, so `{"..."method":"ping"}\0x` was answered as a
  ping. Lines are now read whole with their length; a line over 1 MiB is answered once (`-32600`) and dropped; a last
  line without a newline is still served.
- **The protocol owns stdout.** The conformance run found the cooker's `cook: wrote N artifacts` line on stdout ahead
  of the cook reply: a corrupt message to any MCP client, and stray text before the JSON report of `ceridc cook`.
  `crd::platform::claim_protocol_streams` (new, `crd/platform/standard_streams.hpp`) puts stdin in binary mode and moves
  the protocol to a binary stream on a duplicate of the original stdout, pointing stdout at stderr; the cooker's summary
  moved to stderr with its other notes. On Windows the streams were text mode: replies went out as CRLF and a 0x1A byte
  ended the input.

## Conformance

The published `schema.json` of both revisions, pinned at their tag commits (2026-07-28: 5f5440bb, 181 474 bytes;
2025-11-25: 38c84e9f, 174 246 bytes) with fetch records and the specification repository's licence, are in
`tests/tools/ceridc/mcp/` (test oracles, pre-approved downloads). The repository has no JSON Schema validator and CI
installs no Python packages, so `scripts/json-schema-subset.py` implements the 2020-12 keywords the schemas use and
refuses every other keyword and any network `$ref`; 7 cases in `scripts/test-repository-tools.py` (`JsonSchemaSubset`)
hold its type rules (a boolean is never a number, 1.0 is an integer), bounds, composition, references, cycles, refusals
and command line, and that the pinned schemas match their fetch records.

`crd-ceridc-mcp-conformance` (`tests/tools/ceridc/mcp_conformance.py`) drives the real binary in both eras: 60 replies
validated, every tool run and refused, each call's structured result against its tool's output schema and each call's
arguments against the input schema exactly when the server ran the verb. Teeth, each restored and rebuilt:

| Teeth | Caught by |
|---|---|
| no `resultType` on 2026-07-28 results | conformance: `DiscoverResult does not validate: the member 'resultType' is required` |
| unknown arguments skipped instead of refused | conformance: `query: the input schema refuses {... 'extra': 1} but the server ran it` |
| stdout left on the protocol stream | `crd-platform-tests` `[streams]`: 2 of 9 assertions fail (the conformance run no longer has a stray printer, the cooker's line having moved) |

## Tests

- `tests/tools/ceridc/test_ceridc.cpp`: the agent gate now runs through 2026-07-28 (discovery, then the tools) and
  checks the structured results; the stdio case drives the real binary through both eras with a NUL line, an overlong
  line, a 0x1A line and a last line without its newline, ten replies byte-checked and no CR; new cases hold discovery,
  the version error, nine malformed `_meta` forms, the deterministic list, the handshake and both eras on one
  connection; the envelope case holds every argument refusal by its reason. 35 cases, 1 207 assertions.
- `test_ceridc_diag.cpp`: the diag tool's refusals are tool errors naming the argument; a smuggled `grant` is an
  unknown argument and the service runs nothing; the stdio scripts send 2026-07-28 requests.
- `crd-fuzz-ceridc-diag`: the oracle models the dual-era protocol (handshake state per input, `_meta` validity, the
  version error's data, the framing per era, the refusal report); three seeds added (2026-07-28 requests, version
  forms, the `jsonrpc` member) and two changed; the corpus is regenerated from the seed function (56 seeds, replayed
  clean, as is the old corpus).
- `crd-platform-tests` `[streams]`: the claimed stream holds exactly the protocol's bytes (newline and 0x1A
  unchanged), the stray line lands on stderr.
- Also run: `crd-asset-io-tests` (JsonWriter gained `value_raw`), `crd-cooker-tests`, the `crd-check-*`/`crd-no-*`
  guards and diag coverage (21 of 21 after `check-allocation-results` dropped ceridc main's allowance: the loop no
  longer allocates its line buffer by hand), and the LLVM-20 tidy helper on every changed C++ file (clean).

## Remaining on the row

1. The MCP Inspector (a pre-approved testing tool; Node is already pinned) driving `ceridc mcp` in both eras, pinned
   with its npm lockfile and recorded.
2. Hosted CI evidence for the ceridc, platform and conformance tests on the Windows (MSVC, clang-cl, the binary
   streams), ARM64 and scalar (SSE2 floor) lanes.

The parent [MCP-ADAPT](../ROADMAP.md#slice-mcp-adapt) keeps its clauses (generated tools, resources, prompts, tasks,
MRTR, Streamable HTTP, `subscriptions/listen`, OpenTelemetry, MCP Apps, the registry `server.json`), and tracking the
current revision with N-1 support there is the place for an MCP entry in the standards register, whose spec types
today cover IETF, W3C, NIST and Cerid documents only.
