# DIAG.10a command-decoding fuzzing, 2026-10-10

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.10a](../ROADMAP.md#slice-diag.10a). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-10a); harness:
> [test instruments](../design/test-instruments.md). Rules: [AGENTS](../../AGENTS.md).
> Preceding: [DIAG.10a run records](2026-10-10-diag-10a-run-records.md).

## Goal

The fourth slice of DIAG.10a: "current command decoding". The bytes a diagnostic command arrives as are a JSON-RPC
line on ceridc's stdio MCP transport, read by the owned JSON parser, dispatched by `mcp_handle`, decoded by the diag
tool into a `DiagRequest` and checked by `DiagCommandService::execute` and the command's own argument check. That path
is now on the bounded fuzz harness with oracles that are not the decoder's own verdict, and what it found is fixed.
Committed locally, not pushed.

## What changed

- A fuzz target `crd-fuzz-ceridc-diag` in `tests/tools/ceridc/fuzz` (the replay executable and its `-corpus` CTest on
  every lane, the libFuzzer executable under `CRD_ENABLE_FUZZER`), registered in `scripts/fuzz.py` (`ceridc-diag`), in
  the test-instruments table and in `test-repository-tools.py`'s target list; the ceridc module owns
  `tests/support/fuzz`.
- An input is an optional host line (`#host <grant>[ root]`) and up to 64 request lines. One service per input: the
  host's grant, an optional file root, every command ceridc binds (`bind_diag_commands`: program.provenance,
  program.inspect, replay.prepare, replay.record, replay.run, gpu.resources) and the sandbox's flight and fault
  commands on a small recorder and an injector, so all five registered argument checks are reachable. The root is a
  directory that does not exist: a path command passes the service's path checks, reaches its argument check and its
  handler, and every file it opens fails. Request lines are JSON-RPC lines fed to `mcp_handle` as the stdio loop
  feeds them, the diag tool's `arguments` spliced into a fixed envelope (`@`), or native requests of raw bytes (`!`,
  '|'-separated fields) that no JSON transport can carry (a command name that is not UTF-8, page bounds past u32).
- A line that could reach one of ceridc's file verbs (import, cook, query, instantiate, sequence, render, export) is
  not fed: those verbs read and write paths the input names. The gate reads the names whole and, conservatively, as
  the transport used to (cut at a NUL), so it never lets through a line the old or the new transport routes to a
  verb. Their argument decoding is covered by a new crd-ceridc-tests case instead.
- Oracles:
  - an independent RFC 8259 validator, iterative (a 64 KiB run of `[` must not exhaust the harness's stack), that
    decodes UTF-8 to code points and checks their ranges (crd-core's helper uses a table), must agree with the parser
    within its documented limits: 64 levels of nesting and numbers a double holds;
  - every reply is one line of valid UTF-8 JSON: -32700 with a null id for a line that does not parse, -32600 with a
    null id for a request that is not an object or whose id cannot be echoed, -32600 with the id for a method that is
    not a string, no reply (and nothing run) for a notification, otherwise the request's own id (a string as the same
    string, an integer as the same integer) and the method's documented answer;
  - the diag tool's decoding restated (command a string, path a string, cursor, page_items, page_bytes and schema
    whole numbers up to 2^53 - 1 clamped to u32, args an object of strings) or -32602 with nothing run; the report
    must say `isError` exactly when it refuses, and the same request sent natively for the page the report names must
    return the report byte for byte (a refusal that ran nothing is sent again as it was);
  - the service's refusal order restated from its header (schema, command, authority, request bounds, page bytes,
    path, root, argument names, the command's check, cursor): a refusal runs no handler, a command's own check answers
    only bad-argument or unavailable, and a handler run either opens a new generation or drops the retained snapshot;
  - every document read back: valid UTF-8 JSON on one line, its fields equal to the typed result, the command echo
    clipped at a sequence boundary and sanitized, the grant named, a reason exactly when it refuses, every item within
    1000 bytes, the page within its item and byte bounds, `byte_bounded` exactly when the page stopped early;
  - every Ok snapshot is walked to its end: contiguous cursors, every page makes progress, each page fetched twice is
    the same bytes, a byte-bounded page could not have taken the next page's first item, and the walk covers the
    snapshot exactly once;
  - the run returns every byte it allocated (the per-input `BudgetAllocator` ends at zero).
- 52 seeds, each written by hand: the JSON-RPC lifecycle and every id form, batch arrays and bare values, method and
  tool names with a NUL, 15 lines the parser refuses (numbers outside the grammar or the double range, five kinds of
  ill-formed UTF-8, a lone low surrogate escape, 65 and 20,000 levels of nesting, a cut request), the accepted
  boundaries (64 levels, a 70-digit number, every number form, four-byte UTF-8), the diag tool's fields and the
  service's refusal order, cursors (stale, past the snapshot, another generation), a 65-byte command whose last
  character is two bytes, native requests of raw bytes (a stale cursor before any snapshot, a cursor past 2^53), and
  the flight, fault and CEIR commands with every argument check under hosts with and without a root. The seed function lists the seeds the parser refuses and checks each
  verdict as `--seed` writes it.

## Findings, each fixed and kept as a corpus input or a unit case

1. The JSON parser's numbers (crd-asset-io; glTF, OTIO and every MCP line read through it). `parse_number` copied at
   most 63 characters of `[0-9+-.eE]` to `strtod`: `+1`, `01`, `.5` and `1.` were accepted (the header promised false
   on any RFC violation), a longer token was silently read as its first 63 characters (a 70-digit integer read as
   1e62), and an overflow became infinity. A number now has to match the RFC grammar and is read whole by
   `std::from_chars` (locale-independent; the CEIR parser already uses it on every lane); one a double cannot hold is
   refused. Seeds `number-plus`, `number-leading-zero`, `number-bare-fraction`, `number-trailing-dot`,
   `number-exponent-no-digit`, `number-overflow`, `number-long-token`, `number-forms`; crd-asset-io-tests case
   "JSON numbers follow the RFC grammar exactly and are read whole".
2. The parser never checked a string's raw bytes, so overlong forms, encoded surrogates, code points past U+10FFFF,
   cut sequences and stray continuation bytes reached every consumer (and their writers echoed them). They are
   refused; well-formedness is the new header `crd/core/utf8.hpp`. Five `utf8-*` seeds; the crd-asset-io-tests UTF-8
   case.
3. `as_i64` cast any double to i64: undefined past the i64 range, reachable from an MCP id of `1e300` and from every
   glTF index and offset. It answers the caller's default there and still truncates in range.
4. The MCP envelope (`tools/ceridc/src/mcp.cpp`):
   - a string id was answered as `0`, a fractional id truncated and one past 2^53 rounded, so a client could not match
     the reply; ids now echo exactly (a string, or an integer within 2^53 - 1), and any other id is -32600 with a null
     id;
   - a batch array or a bare value got no reply at all, leaving a client waiting; it is -32600;
   - a method that is not a string was -32601; it is -32600;
   - method and tool names were copied into 64-byte buffers and compared as C strings, so `"ping\u0000x"` was ping and
     `"diag\u0000"` was diag; they compare whole;
   - a string argument longer than 511 bytes was cut to another path, and one holding a NUL was cut at it; it is
     used whole or the call is -32602;
   - a `dry_run` of another type read as false, so `"dry_run":"true"` wrote the file; frame counts and translations
     were cast from any double (undefined past the target range); each is checked, or the call is -32602;
   - the report was copied into the reply as a C string; it is copied by length.
   Seeds `id-*`, `batch-array`, `bare-value`, `method-*`, `tool-*`; crd-ceridc-tests case "the MCP envelope".
5. Both JSON writers (crd-perf's `append_json_escaped`, used by every diagnostic document, event and export, and
   crd-asset-io's `JsonWriter`, used by every ceridc report and the MCP envelope) passed every byte at or above 0x80
   through, so a request's raw bytes (a CLI argument, a value a command's check echoes in its reason) or evidence
   bytes (a bundle's module names, an OBJ's names in an import report) produced a document that is not UTF-8, and a
   strict client rejects the whole reply. Ill-formed bytes are written as `\ufffd`. The service also cut the command
   echo at 64 bytes inside a sequence; it backs off to a boundary as item fields already did. Seeds
   `native-requests`, `native-args`, `diag-command-utf8`; crd-perf-tests case "every document is valid UTF-8" and the
   crd-asset-io-tests writer check.

6. Found by libFuzzer (linux-clang-fuzz, 373 executions into the first round): a refused request's typed
   `DiagResult` kept `cursor` at 0 while its document named the request's cursor, so a native caller and a transport
   read different answers for the same refusal (a stale cursor sent before any snapshot). `refuse()` now sets every
   typed field the document states. Seed `native-stale-cursor`.

No on-disk format, record or schema changed. Duplicate member names keep the parser's first occurrence (RFC 8259
leaves them to the implementation); the transport, the service and the target read them alike.

## Not observed

The commands' own file readers (`bundle.inspect`'s `open_file`, `read_diag_asset_file`, crd-ceir-cook's
`bounded_file.cpp`) call `fopen` directly, outside crd-core's I/O fault seam. A first version of the target installed
an I/O hook to require every opened path to lie under the root on a safe path; the hook was never consulted, because
no command reads through the seam, so the oracle was removed rather than kept as one that cannot fire. The root does
not exist, so nothing is opened either way. Routing those readers through the seam is a follow-up.

## Teeth

Each break was applied, the lane rebuilt, the check observed failing, then the file restored from its backup (its
time stamp touched) and rebuilt; at the end every restored file matched its backup byte for byte.

| Break | Observed |
|---|---|
| T1 the old `strtod` number reader | corpus replay: parser and validator disagree; the asset-io number case; `--seed` refuses `number-plus` |
| T2 raw string bytes not checked | corpus replay: parser and validator disagree; the asset-io UTF-8 case |
| T3 `as_i64` without its range check | the asset-io number case (three values) |
| T4 a string id answered as a number | corpus replay: the echoed id's type; the ceridc envelope case |
| T5 a non-object request dropped | corpus replay: an empty reply; the ceridc envelope case |
| T6 the method compared as a cut C string | corpus replay: ping answered for `ping\u0000x`; the ceridc envelope case |
| T7 the tool compared as a cut C string | corpus replay: the diag tool ran for `diag\u0000`; the ceridc envelope case |
| T8 string arguments cut, NUL kept | the ceridc envelope case (the 512-byte path and the NUL path) |
| T9 a non-boolean `dry_run` read as false | the ceridc envelope case |
| T10 whole-number arguments unchecked | the ceridc envelope case (and the verb wrote its files) |
| T11 crd-perf's escaper passes ill-formed bytes | corpus replay: a document that is not UTF-8; the perf UTF-8 case |
| T12 the command echo cut inside a sequence | corpus replay: the command echo; the perf UTF-8 case |
| T13 crd-asset-io's writer passes ill-formed bytes | the asset-io UTF-8 case |
| T14 duplicate argument names accepted | corpus replay: the refusal order (program.inspect ran its handler) |
| T15 a bounds refusal before authority | corpus replay: the refusal order (oversized where unauthorized) |
| T16 a cursor of another generation accepted | corpus replay: the cursor model |
| T17 `byte_bounded` never set | corpus replay: the page document |
| T18 a non-string `args` member skipped | corpus replay: -32602 expected |
| T19 the diag tool's `page_items` off by one | corpus replay: the page bound of the decoded request |
| T20 a refusal's typed cursor left at 0 | corpus replay: the typed result against its document |

## libFuzzer rounds

Single-process rounds on `linux-clang-fuzz` (ASan and UBSan), 600 s each, `-max_len=65536 -timeout=10
-rss_limit_mb=2048 -malloc_limit_mb=256`, the committed corpus read-only beside a scratch working corpus:

| Round | Executions | New units | Outcome |
|---|---|---|---|
| 1 (`-seed=1`) | 373 | 73 | finding 6, fixed |
| 2 (`-seed=2`) | 20,336 | 1,164 | the harness's own: a refusal echoes a native caller's cursor past 2^53, which the reader required exact in a double; only the cursors the service issues are promised exact, so the reader now compares that echo as a double (seed `native-cursor-past-exact`) |
| 3 (`-seed=3`) | 123,357 | 3,028 | clean, no artifact, peak RSS 498 MB |

The target runs about 200 executions a second (each input builds a service, a flight recorder and every command). The
new units were not merged into the committed corpus.

## Verification

- win-debug: the whole tree built twice; the full ctest (inside vcvars) passed 7,531 of 7,532 on the first build, the
  one failure `crd-repository-hygiene` on a `Testing/` directory a mistyped ctest invocation of mine had created at
  the repository root (removed; the test passes). On the final source, after the `refuse()` fix: the corpus replay (52
  inputs) and `--seed` reproducing it byte for byte, crd-asset-io-tests (54 cases, with `CRD_ASSETIO_DATA`),
  crd-perf-tests (266), crd-sandbox-inspect-tests (31), crd-ceridc-tests (21, with `CRD_CERIDC_EXE`) and 22 CTests
  (`crd-no-*`, `crd-check*`, every `crd-fuzz-*-corpus`, `crd-repository-*`, `crd-master-plan`).
- win-shipping, win-clang-cl-shipping and win-asan (inside vcvars), each reconfigured and rebuilt on the final source:
  the same five suites green (perf 133 cases on the shipping lanes), the seeds byte-identical; clang-cl linked first
  time; the only ASan reports are the three deliberate specimens'.
- WSL, one lane at a time, each reconfigured: linux-gcc-debug (corpus 52, seeds identical, 3 repeats, perf 264,
  asset-io 54, sandbox-inspect 30, ceridc 21, no sanitizer report); linux-gcc-asan (corpus 52, seeds identical, 3
  repeats, perf 264, sandbox-inspect 30, ceridc 21; the only reports are the four deliberate specimens' (heap
  overflow, use after free, use after return, leak)); linux-clang-tsan with `setarch -R` (corpus 52, seeds identical,
  10 repeats, the `[commands]` and `[diag10a]` perf cases (14), asset-io 54, sandbox-inspect 30, ceridc 21, no TSan
  report).
- crd-asset-io-tests on linux-gcc-asan stops at the mikktspace oracle's registered third-party defect TP-5 (a shift
  by 32 inside `mikktspace.c`, docs/third-party-defects.md) when run as one process; its JSON, glTF, OTIO, OBJ, PLY,
  STL and XML cases (39) pass run by tag.
- Strict tidy is clean on all 13 changed C++ files (two loops became `std::ranges` calls, a static constant was
  renamed and two literals became raw strings). clang-format differs only in alignment, include grouping (the harness
  include first, as in every fuzz target) and the reflow of existing comments and one existing string. The Allman
  check, `test-repository-tools.py` (65 tests), `check-master-plan.py` and `check-repository.py` pass.

## Remaining in DIAG.10a

Resource reload (pack manifests, torn packs, `do_reload_mount`); the compiled and reference CEIR comparison; the
virtual-memory and streaming allocators; reinvestigating the unknown-origin CEIR and CKIR artifacts on the
strengthened harness; settling the concurrent-load corruption; the hosted run, which must now also show
`crd-fuzz-ceridc-diag-corpus` (52 inputs) on all six lanes and crd-asset-io-tests, crd-ceridc-tests and
crd-perf-tests green with their `[diag10a]` cases.
