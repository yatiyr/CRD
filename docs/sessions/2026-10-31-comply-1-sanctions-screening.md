# COMPLY.1: sanctions and embargo screening of parties and destinations

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Date: 2026-10-31 (into 2026-11-01). Row: [COMPLY.1](../ROADMAP.md#slice-comply.1) (track C, batch 9; the
[parallel tracks](2026-10-31-parallel-tracks.md)). Contract: the registers'
[sanctions and embargo screening](../design/registers.md#screening).

## CI first

This time the track did cause hosted failures. Run 41231558128 (d5cc2ec0, which holds batches 3 to 7) was the first
run whose jobs reached crd-net, crd-crypto and crd-comply on other compilers. Each failure is fixed in this batch:

- **MSVC C4389** in `tests/foundation/net/test_register_ids.cpp` (the GREASE pattern compared a promoted `u16`
  shifted to `int` with an unsigned mask). It failed win-debug, win-debug-scalar, win-asan, win-public-checks,
  win-tidy and win-arm64-debug. The pattern is now computed on the `u32` loop value (`fix(net)`).
- **GCC `-Wstringop-overflow` under LTO** in `crd/containers/wire_bytes.hpp`. With the debug assert compiled out,
  linux-gcc-relwithdebinfo proved a write past the caller's buffer reachable and refused to link
  `crd-fuzz-crypto-policy` and `crd-fuzz-net-negotiation`. The writer now stops the process with `CRD_FATAL` on such a
  write in every build (`fix(containers)`). On the track machine, the old assert reproduces the failure (exit 1, six
  `stringop-overflow` lines) and the fix links both targets.
- **cmd.exe quoting** in the real-binary `ceridc comply` test on win-clang-cl-shipping ("The filename, directory name,
  or volume label syntax is incorrect"). cmd.exe drops the outer quotes of a command line that holds more than two
  quotes, so on Windows the test now wraps the whole command in one more pair (`fix(ceridc)`).

Failures in the same run that belong to other tracks:

- **Fixed here.** `-Wshadow` in `engine/foundation/core/src/cpu_features.cpp` (97a9ba74, HW-RESILIENCE): the ARM64
  vendor and part names shadowed the MIDR field locals. This failed every GCC and Clang ARM64 lane and hid this
  track's ARM64 builds. The file was still unchanged on main and no other checkout had it modified, so this batch
  renames the two locals in a separate commit (`fix(core)`).
- **Already fixed on main.** The `ftruncate` unused-result error in `durable_file.cpp` was fixed by be0adf3f.
- **Not this track's.** The FMA probe in `simd_level_probe.cpp` (win-tidy) and the CPU identity and hardware
  identity tests on win-clang-cl-shipping.

Every later push run is cancelled before its jobs are scheduled, so these fixes get hosted evidence on the next run
that finishes.

## What landed

- **Search folding in crd-core** (`text.search-folding`, a new capability owned by `core`). `utf8_decode` sits beside
  the UTF-8 well-formedness rule, and `fold_code_point` reads a table that
  [gen-text-fold.py](../../scripts/gen-text-fold.py) derives from the Unicode Character Database. The table covers
  Latin through Cyrillic, Armenian, Hebrew, Arabic, Georgian, Latin Extended Additional and Greek Extended, the
  punctuation blocks and the fullwidth forms. A letter folds by the first rule that answers:
  1. A conventional reading (sharp s, thorn, eth, eng, wynn, yogh, ezh, the schwa as a, the oe ligature).
  2. NFKD without marks, case-folded, when that leaves ASCII.
  3. The Latin letter's UCD name (o with stroke is o, dotless i is i).
  4. NFKD case-folded within its own script (Greek and Cyrillic lose case and accents, final sigma is sigma).

  Combining marks, apostrophes, ayn and hamza are elided. Punctuation and spaces separate words.
- **Fixed points and drift.** The generator refuses any fold that is not a fixed point. The table holds 99 ranges and
  1 472 mappings. It is `crd-core-text-fold-drift` and is listed in `generated-sources.json`. Python 3.12 (UCD 15.0,
  on the track machine) and Python 3.14 (UCD 16.0, on this PC) both reproduce it byte for byte. `.inc` files are now
  `eol=lf` so the drift check also holds on Windows checkouts.
- **The name matcher** (`crd/comply/names.hpp`).
  - `fold_name` reads at most 16 words of at most 64 code points. It drops honorifics and legal forms (a legal form
    spelt in single letters, A.Ş. or L.L.C., included, and the Cyrillic ООО forms) unless nothing else is left.
  - `name_score` pairs words best-first. A pair is rated by Jaro-Winkler, by 0.9 for an initial, or by at least 0.9
    for the same consonant skeleton. Pairs below 0.88 are not counted.
  - The score is the highest of three: the length-weighted similarity, the covered share of the shorter name less
    0.05 for each unpaired word, and the Levenshtein similarity of the joined words when the word counts differ.
  - The thresholds were tuned on a prototype against the cases the tests now hold, on both sides of 850.
- **Sanctions lists** (`crd/comply/sanctions.hpp`). Each list is a `cerid-sanctions/1` generation per authority:
  - the authority, jurisdiction, generation, publication day and maximum age;
  - the official file's URL and SHA-256;
  - embargoed destinations: an ISO 3166 region, the programme and the measure;
  - parties: id, kind, names and aliases in UTF-8, programmes, listing day, births for individuals only, countries
    and identifiers in compared form.

  Each list is decoded whole within its bounds. `SanctionsStore` adopts a generation only above the authority's
  current number, by the same rule as the rule generations.
- **Screening.**
  - Hits: a party is **Discounted** when the subject's kind or birth contradicts it. It is a **Match** when an
    identifier agrees, or when the score reaches the match threshold with a birth or country that agrees. Otherwise
    it is **Possible**.
  - Destinations: a request in an embargoed region or inside an embargoed country is a match. A request that names
    only the country an embargoed region belongs to is possible.
  - Freshness: a list is missing, stale or not yet published. A stale list is still screened.
  - The verdict is Blocked, else Unavailable, else Review, else Clear. A malformed request is refused with its key.
- **One decoder, one adoption rule.** The rule decoder and the sanctions decoder share `src/decoder.hpp`: the
  identifier shapes, the bounded and typed TOML reading, `find_held` and `adopt`. `parse_day` and the module's errors
  (`ComplyError`, with `duplicate-party` and `duplicate-destination`) now live in `crd/comply/generation.hpp`.
  `ceridc comply` reads the renamed error names.
- **Register.**
  - `us.sanctions`, `eu.restrictive-measures`, `int.un-sanctions`, `gb.sanctions` and `tr.asset-freeze` now cite the
    screening tests as their active check. Their owner is `comply`, and the STORE rows stay their consumers. They are
    still planned: their texts are not read yet.
  - TR, EU, GB, US and INT were reviewed as new rule generations: TR, EU, US and INT at generation 3, GB at 2.
  - `compliance.obligations` lists the new API and a signature that refuses a second Jaro-Winkler.

## Verification

All on the track machine, which `bench_vm.py status` reports as n2-standard-8, Ubuntu 24.04, GCC 13.3.

**linux-gcc-debug**
- Build: 0 warnings.
- `crd-comply-tests`: 37 cases, 7 514 assertions. These are the rule cases plus 8 `[names]` and 14 `[sanctions]`
  cases, among them a concurrent applier and screener and a list of 20 000 parties.
- `crd-core-tests [text-fold],[utf8]`: 7 cases, 569 assertions. `crd-net-tests`: 17 cases. `crd-containers-tests`:
  131 cases.
- CTest `comply|text-fold|ceridc|wire|^crd-no-|^crd-check`: 73 of 73. These include the two comply corpus replays,
  the rules and fold drift tests, `crd-check-registers`, `crd-check-capability-owners` and `crd-check-isa-isolation`.
  The isa-isolation check passed after rebuilding a geometry test whose object another track's move had left stale.

**linux-gcc-relwithdebinfo**
- `crd-fuzz-crypto-policy` and `crd-fuzz-net-negotiation` link and replay their corpora.
- `crd-net-tests` and `crd-crypto-tests` pass.

**Fuzz**
- `crd-fuzz-comply-sanctions` writes 23 seeds, which replay clean.
- Its oracles are the contract invariants, idempotent folding, self-score 1000 and a model of gaps, brute-force
  scores, dispositions, destinations and the verdict.

**Teeth.** Each of 18 mutations is caught by the unit tests, and the screening ones by the corpus too:
- the matcher: pair floor, skeleton, subset, joined, stop words, initials, word order;
- the screening: stale, early, discount, corroboration, gap precedence, region-possible, duplicate party, an entity's
  birth, repeated authorities, policy order, missing list.

**Strict analysis and format**
- `scripts/tidy-files.py` with clang-tidy 20 passes the 16 new and changed C++ files clean. The findings it raised
  were fixed: a ranges `any_of`, an explicit half-transposition count, conditionals unnested, the forwarding
  reference taken by value, a local constant's case and a raw string.
- The pinned clang-format 20.1.8 reproduces every new file.

**Python**
- `TextFold`, with 4 new cases (the rule order, the classes, ranges and fixed points with a forced violation, and
  `--check` on a fixture and the committed table).
- `TestInstruments`, `CapabilityOwners`, `Registers`, `LegalRegister`, `ComplyRules` and `CryptoInventory` pass on
  Linux and Windows.
- In the full Windows suite, one case needs Ninja on PATH: `test_generated_target_folders`, an environment gap that
  HEAD shows too.

**Validators.** `check-registers.py`, `check-generated.py`, `gen-comply-rules.py --check`,
`check-capability-owners.py`, `check-docs.py` and `check-allman-braces.py` pass.

The track machine ran from 20:11 to 20:52 and from 20:55 to 21:05 UTC, about 0.84 h, and is stopped.

## Remaining in COMPLY.1

- **User action: approve the download of the official lists' first read.** The lists are OFAC's SDN and consolidated
  lists, the EU financial sanctions file, the UN Security Council consolidated list, the UK sanctions list, and
  Turkey's asset-freeze decisions under Laws No. 6415 and 7262. They are public data files, not testing tools.
  Their importers to `cerid-sanctions/1` are written against the files themselves, never from memory, and land with
  that read. The read also dates the embargo regimes each authority applies, so the destination entries are read,
  not recalled.
- The user approves the first read of the instruments (the legal register's plan), and counsel reviews the eight
  classifications. Both are user actions.
- The screening query over `ceridc` beside `ceridc comply`, and over MCP after [MCP-ADAPT](../ROADMAP.md#slice-mcp-adapt).
- The NET.2 providers make algorithms implemented. Each such change re-reviews the exports it reaches.
- The row is Done once [COMPLY.0](../ROADMAP.md#slice-comply.0) is Done.
