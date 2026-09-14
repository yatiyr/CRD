# DIAG.2a/2b/3a — typed events, lifecycle recording, checked allocator math

<!-- doc-role: historical -->

Dated evidence for [DIAG.2a](../ROADMAP.md#slice-diag.2a), [DIAG.2b](../ROADMAP.md#slice-diag.2b)
and [DIAG.3a](../ROADMAP.md#slice-diag.3a). Contract:
[runtime-diagnostics](../design/runtime-diagnostics.md); direction:
[ADR-0133](../decisions/0133-runtime-diagnostics-and-instrumentation.md). One autonomous DIAG-loop
iteration; all proof is `win-debug`.

## DIAG.2a — typed events, identities, policy (`crd/perf/diagnostics.{hpp,cpp}`)

- **Schema + taxonomy.** `kDiagnosticSchemaVersion`; `Severity` separates Info / Warning /
  RecoverableError / DeveloperAssertion / InstrumentFailure / FatalInvariant (DG13's "don't
  collapse into one error bucket"). `is_fatal` isolates the required-validation class.
- **Identities + clock.** `SourceIdentity` (borrowed file/symbol/line), `GenerationKey` (u64,
  wrap made explicit), monotonic `diagnostic_now_ns` (steady clock, never wall-clock).
- **Authorable bounded policy (DG18).** `DiagnosticPolicyStore::load` validates a candidate
  against hard bounds and the schema version; only a valid candidate commits and advances the
  generation. A malformed / oversized / unknown-version / zero-bound candidate is rejected with
  an explicit `PolicyLoadStatus` and leaves **both** the active policy and the generation
  untouched — malformed input can never move the active generation (last-known-good).
- **Shared context.** `to_json` emits a deterministic, escaped, single-line object; `to_log_line`
  the human form. Message truncation to the policy's byte bound sets `message_truncated` — never
  silent.
- **Cross-module bridge without an upward dependency.** `EventCode` is a `u32` with a documented
  foundation range `[0, 0xFFFF]`; other modules call `register_code_range(name, lo, hi)` from
  their own translation unit, so crd-perf includes no CEIR header. `owning_module` resolves the
  tag; overlapping / inverted / into-foundation registrations are refused. A CEIR-coded and a
  foundation-coded event serialise through the identical path, differing only in the module tag
  (the "same structured context through CLI/JSON" acceptance).
- **Always-on validation.** `check_invariant` / `CRD_DIAG_INVARIANT` build a `FatalInvariant`
  event and route it to a settable handler (tests install a non-terminating one; the default
  reports via `CRD_FATAL` and aborts). It is present in every build — not a `CRD_ASSERT` that
  vanishes in release.

70 assertions across 8 cases (`[diag][events|policy|json|bridge|fatal]`).

## DIAG.2b — lifecycle recording + emergency record (`crd/perf/diagnostic_recorder.{hpp,cpp}`)

- **Preallocated bounded ring.** `init(capacity)` reserves the ring once; `record` never
  allocates and overwrites the oldest slot when full while keeping every retained record valid
  (owned message strings intact). `snapshot` copies oldest-first; `count`/`total_recorded` expose
  live vs monotonic totals.
- **Reader lifetime tokens.** `register_reader` hands back a generation-tagged token;
  `deregister_reader` retires the slot and bumps its generation, so a double-deregister or a
  token for a since-reused slot is rejected — no double-free, no dangling reader.
- **Independent emergency record.** `capture_emergency` writes a fixed-size record under its own
  lock, never the ring mutex, so it succeeds even while another thread holds the ring/log lock
  (the "crash while the log mutex is held still yields a record" case). Async-signal safety from
  a real handler remains DIAG.5b's job.
- **Lifecycle.** Every entry point is a benign no-op before `init` and after `shutdown`;
  re-init after shutdown is legal. DG08's concern is met by reading and writing the registry under
  the same lock rather than trusting a writer mutex to protect readers.

56 assertions across 6 cases, including a 6-thread × 4000 concurrent-record stress that stays
bounded to capacity with every record accounted for. The TSan proof rides DIAG.1b's lane.

## DIAG.3a (core) — checked allocator arithmetic (`crd/memory/checked_math.hpp`)

Constexpr, allocation-free overflow-checked helpers: `checked_add`, `checked_mul`,
`checked_array_size` (count×stride), `checked_align_up` (refuses the `(value + align - 1)` wrap
the naive form suffers within `align-1` of `SIZE_MAX`, and non-power-of-two alignments),
`checked_page_count`, `checked_padded_array_size`. Each returns success/overflow rather than
wrapping to a too-small size (the classic allocator integer-overflow bug), and adds no
per-allocation heap cost. 31 assertions incl. compile-time `static_assert`s and exact
near-address-width boundary cases.

**Not yet done for 3a:** wiring the helpers through every allocator's size path and the full
per-allocator ownership / zero-size / exhaustion / realloc-failure contract sweep — later
iterations of the loop.

## Regression

`crd-perf-tests`, `crd-memory-tests` and `crd-jobs-tests` build clean under `/W4 /WX
/permissive-`; the repository and master-plan validators pass.
