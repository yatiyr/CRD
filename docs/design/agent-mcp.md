# Agent MCP server

<!-- doc-role: contract -->
> Design contract of [MCP-ADAPT.0](../ROADMAP.md#slice-mcp-adapt.0); status lives only in the ROADMAP. The later
> adapter module is [MCP-ADAPT](../ROADMAP.md#slice-mcp-adapt). Read
> [ADR-0081](../decisions/0081-agent-native-engine-cli.md) and the
> [research](../research/2026-10-26-building-with-cerid-2026-comparison.md#gaps-in-area-3) first.

Purpose: `ceridc mcp` speaks the current Model Context Protocol revision, `2026-07-28`, and keeps serving clients that
open with the `2025-11-25` handshake, over the same verbs the command line runs. Every tool states what it takes and
what it answers as JSON Schema, every answer is structured, and an agent that sends a bad argument reads why and can
correct the call. The diagnostic host's grant stays the process's start-up decision.

## Sources

The specification pages read on 2026-11-01: the
[2026-07-28 changelog](https://modelcontextprotocol.io/specification/2026-07-28/changelog),
[versioning and compatibility](https://modelcontextprotocol.io/specification/2026-07-28/basic/lifecycle),
[base protocol and `_meta`](https://modelcontextprotocol.io/specification/2026-07-28/basic/index),
[discovery](https://modelcontextprotocol.io/specification/2026-07-28/server/discover),
[tools](https://modelcontextprotocol.io/specification/2026-07-28/server/tools),
[caching](https://modelcontextprotocol.io/specification/2026-07-28/server/utilities/caching) and the
[stdio transport](https://modelcontextprotocol.io/specification/2026-07-28/basic/transports/stdio). The published
JSON schemas of both revisions are pinned in `tests/tools/ceridc/mcp/` at their tag commits, each with a fetch record
(URL, commit, bytes, SHA-256, licence) and the specification repository's licence (Apache-2.0; MIT for contributions
not yet relicensed).

## The server

`crd::ceridc::McpServer` (`tools/ceridc/include/crd/ceridc/verbs.hpp`) is the server of one connection. ceridc's stdio
loop owns one per process, reads a line, hands it to `handle` and writes the reply line. It is a dual-era server:

| A request | Is served as |
|---|---|
| `params._meta` carries `io.modelcontextprotocol/protocolVersion` `2026-07-28` and `clientCapabilities` | revision 2026-07-28, statelessly: nothing from an earlier request is assumed |
| `params._meta` names `2025-11-25` | refused `-32602`: that revision is served after the handshake, not by per-request metadata |
| `params._meta` names any other version | refused `-32022` UnsupportedProtocolVersion, `data` naming the supported versions and the requested one |
| `initialize` with `protocolVersion`, `capabilities` and `clientInfo` | the 2025-11-25 handshake: the answer names `2025-11-25` whatever was proposed, and that revision serves the connection's requests without `_meta` from then on |
| no `_meta` version before the handshake | refused `-32602` (a malformed 2026-07-28 request), except `ping`, which 2025-11-25 serves before the handshake |

Both eras may share one connection: a 2026-07-28 request after the handshake is still served by 2026-07-28. A
2026-07-28 request is malformed (`-32602`) when its `_meta` is not an object, its version not a string, its client
capabilities not an object, a `clientInfo` it sends lacks a string `name` or `version`, or a `logLevel` it sends is not
one of the eight logging levels. The server never emits `notifications/message`, so a requested log level asks for
nothing it would send.

**Methods.** 2026-07-28: `server/discover`, tools/list, tools/call. 2025-11-25: `initialize`, `ping`, tools/list,
tools/call. Any other method is `-32601`; notifications (`notifications/initialized`, `notifications/cancelled`) are
never answered. `subscriptions/listen`, resources, prompts, the tasks extension and multi round-trip requests belong to
[MCP-ADAPT](../ROADMAP.md#slice-mcp-adapt): the tool set never changes while the process runs, so the server declares
`tools.listChanged: false` and has nothing to notify, and no tool asks the client for input.

**Results.** In 2026-07-28 every result carries `resultType: "complete"` and `_meta["io.modelcontextprotocol/serverInfo"]`
(name `ceridc`, title, the engine's `CRD_VERSION_STRING`); `server/discover` and tools/list carry `ttlMs` 3 600 000
and `cacheScope: "public"`: the tool set is fixed when the process starts (the `diag` tool is listed exactly when the
host bound a diagnostic service) and is the same for every caller. Discovery names `supportedVersions`
`["2026-07-28", "2025-11-25"]`, the capabilities and short instructions. 2025-11-25 results carry none of
`resultType`, `_meta` or the caching hints.

**Envelope (DIAG.10a, kept).** A message is a JSON object with `jsonrpc` `"2.0"`, a string `method` and an `id` that
is a string or an integer within ±(2^53 − 1), echoed exactly; anything else is `-32600`, and an error whose id cannot
be read leaves `id` out, as the MCP schema defines the error response (JSON-RPC 2.0 would write `null`, which the
schema refuses). Batches are invalid requests. `params`, when present, is an object (`-32602`). Method and tool names
compare whole: a name holding a NUL names nothing. On stdio a line is read with its length (a NUL byte is the parser's
to refuse), a line over 1 MiB is answered once as `-32600` and dropped, and a last line without its newline is still a
message.

**The protocol owns stdout.** `crd::platform::claim_protocol_streams` (`crd/platform/standard_streams.hpp`) puts stdin
in binary mode and moves the protocol onto a binary stream on a duplicate of the original standard output, pointing
standard output itself at standard error. A reply is its bytes on every platform (no CRLF; a 0x1A byte does not end
Windows input), and a library's stray print (the cooker printed its summary to stdout until this row) reaches the
client's log, never its protocol.

## Tools

Each tool is one verb, declared once in `tools/ceridc/src/mcp.cpp`; the published input schema and the call's
argument check are both written from that declaration, so they cannot disagree. tools/list answers the tools in name
order (`cook`, `diag`, `export`, `import`, `instantiate`, `query`, `render`, `sequence`), the same bytes every time; a
`static_assert` holds the order and the 2025-11-25 naming guidance (1 to 128 of `A-Z a-z 0-9 _ - .`). The current
names already follow it. Every tool has a `title`, a `description`, an `inputSchema` (`additionalProperties: false`),
an `outputSchema` and `annotations` (`readOnlyHint`, `destructiveHint` for writers, `openWorldHint: false`: the host's
own files). The command-line verbs `comply`, `screen` and `inspect` are not tools: comply and screening reach MCP with
MCP-ADAPT (the capability registry's `compliance.obligations` entry), and `inspect`'s agent form is the `diag` tool's
`program.inspect` under the Execute grant.

| Argument kind | Schema | Checked |
|---|---|---|
| a path or name | `string`, `minLength` 1, `maxLength` (code points) | 1 to 511 UTF-8 bytes (255 for a clip name), no NUL |
| a number | `number` within ±`FLT_MAX` | a JSON number a f32 holds |
| a frame count | `integer` within ±(2^53 − 1) | a whole number |
| a diag count or cursor | `integer` from 0 to 2^53 − 1 | a whole number; above u32 it reaches the service as the largest u32 and is refused there as oversized |
| a flag | `boolean` | a JSON boolean (`"true"` is not one) |
| the diag `args` | `object` of strings | every member a string |

**Errors.** An unknown tool, a `name` that is not a string and `arguments` that are not an object are protocol errors
(`-32602`). Every argument is checked before anything runs; a refused one (absent when required, unknown, given twice,
of the wrong type, out of range, too long, holding a NUL) is a tool execution error, as 2025-11-25 and 2026-07-28
require, so the model reads it: the result is the tool's refusal report `{ok: false, verb, reason}` with `isError:
true`, the reason naming the argument and what it must be. A verb that fails answers its own `{ok: false, verb,
reason}`; a render that stops early says why. An unknown argument is refused rather than ignored: a misspelt `dry_run`
must not write the file, and a smuggled `grant` cannot raise the host's authority (the service never sees the call).

**Results.** A call's report is the result's `structuredContent` and, serialized, its one text content block;
`isError` is the report's `ok` negated, read from the parsed report. Each output schema is `anyOf` the verb's success
shapes and the refusal shape, closed (`additionalProperties: false`), in the keywords the conformance oracle implements.
The `diag` tool's success shape is the diagnostic service's response document (schema, command, ok, status, reason,
grant, generation, cursor, next cursor, total, page items, dropped, complete, byte bounded, summary, items), byte for
byte the native answer.

## Headless and SSE2

The server needs no GPU and no ISA level: ceridc opens no GPU context, and `gpu.resources` over the `diag` tool says
which GPU evidence it cannot have (`unavailable` items) instead of failing. The conformance run on the Linux debug lane
shows it on a host without a GPU; CI's scalar lanes (`win-debug-scalar`, `linux-gcc-debug-scalar`) build and run the
same tests on the SSE2 floor.

## Conformance

`tests/tools/ceridc/mcp_conformance.py` (the `crd-ceridc-mcp-conformance` CTest) drives the real binary over pipes:
one process in 2026-07-28, one that opens with the handshake, runs in 2025-11-25 and then sends a 2026-07-28 request.
Every request it sends validates against its revision's request definition and every reply against the result or
error definition of its method in the pinned schema (checked against its fetch record first). Every tool's input and
output schema must use only keywords the oracle implements; every call's structured result must fit its tool's output
schema, equal its text content and be an error exactly when `ok` is false; and each call's arguments must fit the
input schema exactly when the server ran the verb, so the published schema and the server's check agree on every
case. Every tool runs and is refused at least once. The oracle is `scripts/json-schema-subset.py`: JSON Schema 2020-12
for the keywords the published schemas and the tool schemas use (`type`, `enum`, `const`, `properties`, `required`,
`additionalProperties`, `items`, `anyOf`, `allOf`, `oneOf`, `not`, the numeric, length and item bounds, local `$ref`),
refusing any other keyword and any network reference, so nothing it does not understand reads as valid. Its own cases
are in `scripts/test-repository-tools.py` (`JsonSchemaSubset`).

The in-process cases in `tests/tools/ceridc/test_ceridc.cpp` hold the protocol byte for byte (discovery, the version
error, every malformed `_meta`, the handshake, both eras on one connection, the envelope and every argument refusal),
`test_ceridc_diag.cpp` the `diag` tool, and `crd-fuzz-ceridc-diag` models the dual-era protocol as its oracle.
