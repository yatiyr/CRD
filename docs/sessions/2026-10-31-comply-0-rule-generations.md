# COMPLY.0: reviewed rule generations, the live store and `ceridc comply`

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Date: 2026-10-31. Row: [COMPLY.0](../ROADMAP.md#slice-comply.0) (track C, batch 7; the
[parallel tracks](2026-10-31-parallel-tracks.md)). Contract: the registers'
[rule generations](../design/registers.md#rule-generations).

## CI first

No failure is attributed to this track. The GitHub Actions push run for 88624a49 (41225876291) was still pending with
no job scheduled when this batch began, and every earlier push run of the track was cancelled by the next push to main
before its jobs were scheduled. The operator's checkpoint run 41215667488 on c2645814 had no failed job: both
"Repository checks" jobs, win-vs, win-shipping, clang-cl, win-arm64-debug, linux-gcc-shipping, linux-gcc-public-checks,
linux-gcc-debug-scalar, linux-gcc-relwithdebinfo and the arm64 lanes (gcc and clang debug, clang asan) were green and
the rest still running. Batch 6's follow-up (the renamed "Standards and legal registers are current" step on a hosted
run of e842f476 or later) therefore stays open: no hosted run has reached it.

## What landed

- **The `comply` owner.** `crd-comply` (`engine/foundation/comply`, depends on crd-core, crd-containers and, privately,
  crd-toml) lands `compliance.obligations` with its API `crd/comply/rules.hpp`; the capability is no longer planned.
- **Rule generations.** `scripts/gen-comply-rules.py` writes one `cerid-rules/1` document per jurisdiction holding a
  rule (a planned or implemented obligation or user action; watched and deprecated entries are not rules):
  `assets/comply/<code>.rules.toml` for AU, BR, CA, EU, GB, INT, JP, PLATFORM, TR, US and US-CA, in the TOML subset
  crd-toml reads, canonical UTF-8 with LF (scopes the rules name in register order, then the rules in register order,
  every string escaped). `--check` is the `crd-comply-rules-drift` CTest and refuses a stray file; the eleven files are
  in `scripts/generated-sources.json`.
- **Reviewed.** `rule_generations` in `cmake/legal-register.json` records each jurisdiction's generation, review day
  and file SHA-256 (all eleven reviewed as generation 1 on 2026-10-31). `scripts/check-registers.py` fails a file whose
  bytes differ from its record, a jurisdiction with rules and no record, a record without rules, a missing file, a
  malformed record and a review day after today. `gen-comply-rules.py --review <code> [--day]` is the review: it
  refuses unchanged rules, otherwise increments the generation and records the day and digest.
- **Decoded whole.** `decode_rules` validates everything before a generation exists: bounded text (1 MiB), counts
  (4096 rules, 256 scopes, 64 list items) and strings (4096 bytes); every key known and typed; ids, topics, rows and
  jurisdiction codes in the register's forms; https primary sources; closed enumerations; real calendar days (YYYY and
  YYYY-MM count from their first day, as the guard counts them) with sunsets after effective days and counsel days
  exactly when counsel answered; unique scopes and rules, every `applies_to` naming a listed scope once; and no
  implemented rule without an active check, a read text and answered counsel (a generation cannot claim a proof the
  register would refuse). A failure names its error, line and key.
- **Swapped live.** `RuleStore::apply` decodes outside the lock, then adopts a generation only above its
  jurisdiction's number: a lower one is a rollback, the same number with other bytes (XXH3-64) a conflict, the same
  bytes already current, a 65th jurisdiction a full store; each refusal leaves the store unchanged.
  `generation(code)` is what a caller persists as its floor.
- **Queried.** `RuleStore::query` answers, under the store's lock, every rule of the requested jurisdictions (request
  order, repeats and unknown codes answering nothing more) that applies to the scope and is in force on the day, with
  the jurisdiction, generation, scope state and proof: `Proven` only for an implemented rule with an active check.
  Today every rule is planned, so every answer is unproven.
- **`ceridc comply`** `--rules <dir> --scope <id> --jurisdiction <code,...> --day YYYY-MM-DD` loads
  `<dir>/<code>.rules.toml` per requested code, reports each code present or absent with its generation, and writes
  the bindings (rule, instrument, primary source, read flag, obligation, check, effective and sunset days, counsel,
  rows, proof) as JSON; a malformed request or a refused generation (with file, error, line and key) is `ok:false`.
- **Contract.** `docs/design/registers.md#rule-generations` records the file form, the review, the swap, the query and
  what stays open: authenticity on delivery is NET.2's signed envelope through crd-crypto, the MCP tool follows
  MCP-ADAPT, and the CHIR projection follows LANG.2 (CHIR-0 has no structured values: its node kinds are event
  handlers, state, queries, parallel updates and awaits, and ADR-0128 rules out extending them here).

## Verification (track machine, linux-gcc-debug, n2-standard-8)

- Build of `crd-comply-tests`, `crd-fuzz-comply-rules`, `ceridc` and `crd-ceridc-tests`: 0 warnings.
- `crd-comply-tests`: 14 cases, 2495 assertions, all passing (days and their periods, the whole decode, format,
  syntax and every size bound at and past it, every required key, unknown keys at three levels, every type, closed
  values and identifiers, unique and named scopes and rules, days, sunsets and counsel days, claimed proofs, the store's
  rollback, conflict, already-current, upgrade, independence of jurisdictions and its full bound, the query's in-force
  edges, scopes, order, repeats and appending, `in_force` and `proof_of` edges, a concurrent applier and reader that
  never sees half a swap over 40 generations, error names, and all eleven committed generations decoded and loaded
  with KVKK binding the crash pool in Turkey).
- `crd-ceridc-tests`: 26 of 26 (the three new comply cases: the answer for TR, EU and an absent code with KVKK's
  binding and its day edge, every malformed request and a refused generation with its error, line and key, and the real
  binary's bytes equal to the verb's).
- `crd-fuzz-comply-rules`: 14 seeds written by `--seed` (the committed corpus is that output) and replayed. Oracles:
  the contract's invariants and a canonical reprint restated from the generator for every accepted text; a model of
  monotonic adoption comparing whole texts for every apply; a brute-force query for every answer.
- CTest `comply|crd-toml|^crd-no-|^crd-check`: 38 of 38, among them the drift test, the corpus replay, the registers
  guard, capability owners, source routes, the include budget, the container ban and ISA isolation.
- Teeth: fourteen mutations of `rules.cpp` (no rollback check, digests always equal, sunset and effective edges off by
  a day, proof without implementation, implemented accepted with a planned check only, unknown scopes, duplicate scopes,
  unknown keys accepted, no leap-year check, a sunset on its effective day, repeated jurisdictions answered twice, the
  text bound off by one, no store bound) each fail the unit tests; the corpus replay catches eight of them. The other
  six are refusals its oracles do not restate (an unknown key, a duplicate scope, whose seed is also refused for its
  unknown scope, a leap day, the text and store bounds) or lie outside its drawn generations (a proof without an
  implementation, which a drawn rule never has).
- Strict analysis: `scripts/tidy-files.py` with the pinned clang-tidy 20 parsed all eight changed C++ files clean after
  the findings were fixed (a loop became `std::ranges::all_of`, two local constants were renamed, four literals became
  raw strings); the new files are formatted with clang-format 20 and the repository's `.clang-format`.
- Python: `test-repository-tools.py` gains `ComplyRules` (5 cases: the canonical generation and its escapes, drift
  and stray files, the review's next generation and its refusals, the guard's digest and missing file, and every
  record rule); the register fixture now reviews its generations, so `Registers`, `LegalRegister`, `RegisterReview`,
  `RegisterIds` and `CapabilityOwners` (42 cases) pass on Windows and Linux. Nine mutations of the generator and the
  guard each fail `ComplyRules`.
- `check-registers.py`, `check-capability-owners.py`, `check-repository.py`, `check-master-plan.py`,
  `check-allman-braces.py` and `check-generated.py` pass.

## Remaining in COMPLY.0

- **User action:** approve the download of the legal register's first read (the 80 official texts
  `python scripts/register-review.py --register cmake/legal-register.json plan --out <file>` lists, and the gazette
  reading; the light scan is due on 2026-11-26). The read records every consolidated version and replaces the
  home-page URLs; the changed entries then go through `gen-comply-rules.py --review`.
- The MCP tool of the same query once [MCP-ADAPT](../ROADMAP.md#slice-mcp-adapt) (track A) gives commands one
  declaration for every surface.
- The CHIR projection of the rule generations once [LANG.2](../ROADMAP.md#slice-lang.2) gives CHIR structured values.
  The table orders the LANG rows after COMPLY.0, so this clause cannot close in the row's position; it is a proposal for
  the user to move it to the COMPLY parent or a COMPLY child after LANG.2.
- Done when [OWN](../ROADMAP.md#slice-own) (track B) and [NET-STD](../ROADMAP.md#slice-net-std) are Done.
