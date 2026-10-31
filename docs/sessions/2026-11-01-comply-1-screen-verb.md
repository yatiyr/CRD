# COMPLY.1: the screening served by `ceridc screen`

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Date: 2026-11-01 (UTC evening of 2026-10-31). Row: [COMPLY.1](../ROADMAP.md#slice-comply.1) (track C, batch 10; the
[parallel tracks](2026-10-31-parallel-tracks.md)). Contract: the registers'
[sanctions and embargo screening](../design/registers.md#screening).

## CI first

Run 41231558128 (d5cc2ec0, batches 3 to 7) is the only run so far that scheduled jobs for this track's code. It was
still running at this batch. Its finished failures are the ones batch 9 fixed, and the jobs that finished since add
nothing new:

- **MSVC C4389** in `test_register_ids.cpp`: win-shipping-profile and win-relwithdebinfo also failed on it. Fixed by
  2782ab9e.
- **The real-binary `ceridc comply` quoting** on win-clang-cl, the same failure as win-clang-cl-shipping. Fixed by
  ee040d0f.
- **Not this track's:** the CPU identity and hardware identity tests on win-clang-cl (the registry's processor name
  keeps trailing spaces, and Hyper-V hides the microcode revision). These belong to HW-RESILIENCE.
- **Passed:** linux-gcc-public-checks, linux-gcc-debug-scalar, win-vs and both Repository checks jobs. linux-gcc-debug,
  linux-gcc-asan and linux-clang-tsan were still running.

Every later push run, the one for 4e75f014 included, was cancelled before its jobs were scheduled. This batch's push
cancels the next one too. Hosted evidence for batches 8 to 10 needs a run that is allowed to finish.

## What landed

`ceridc screen` serves crd-comply's sanctions and embargo screening on the command line, beside `ceridc comply`:

```text
ceridc screen --lists <dir> --authority <id,...> --day <YYYY-MM-DD> [--destination <region>]
              [--kind <individual|entity|vessel|aircraft> --name <n>... [--birth <date>]
              [--country <cc>]... [--identifier <type:value>]...] [--review <n>] [--match <n>]
```

**Lists.** Each authority's list is the file `<dir>/<authority>.sanctions.toml`. A file that cannot be read is a
missing list, so the verdict fails closed (Unavailable). An authority named twice is read and screened once.

**Validation before any file is read.**
- The verb refuses:
  - a missing `--lists`, `--authority` or `--day`;
  - a day that is not a YYYY-MM-DD calendar day;
  - an empty item or more authorities than a store holds;
  - subject fields without `--name`, or neither a subject nor a destination;
  - a threshold that is not one to four digits.
- `--kind` is required with `--name`. A wrong kind discounts the very party that matches, so it is never assumed.
- The rest of the request is judged by the module's own rules, run against an empty store: the authorities' shape
  (which keeps every file name inside `--lists`: `../x` is refused), the policy order and bound, the destination
  region, the names, the birth, the countries and the identifiers. A refusal names the module's error and key.

**Lists refused with their file.** A list that does not decode is refused with its file, error, line and key. A file
that holds another authority's list is refused, so `test.un`'s file can never screen as another authority.

**The answer.**
- Each authority's generation, 0 when absent, and the verdict.
- The gaps: missing, stale or not yet published.
- Every party hit: authority, generation, party, the listed name that scored best, the score, the disposition, and
  whether the birth, a country or an identifier agrees.
- Every destination hit: region, programme, measure and disposition.
- The subject is screened, never echoed.
- `ok` is true for every verdict, Blocked included: the verdict is the answer, and the exit status reports only a
  refused request.

The verbs share their helpers in `tools/ceridc/src/comply.cpp`: the refusals, the day, the comma list and the file
path. `ceridc comply` keeps its answers. The contract's screening section gains a "Served" item, the capability
registry's `compliance.obligations` summary names `ceridc screen`, and the comply module's CMake comment names both
verbs.

## Verification

All on the track machine (n2-standard-8, Ubuntu 24.04, GCC 13.3), linux-gcc-debug.

- **Build:** `ceridc`, `crd-ceridc-tests` and `crd-comply-tests` with 0 warnings.
- **`crd-ceridc-tests "[screen],[comply]"`:** 10 cases, 167 assertions. The 7 `[screen]` cases cover:
  - a blocked person with every hit field;
  - Turkish names in capitals with İ and Ş, matched by name alone (possible), by identifier and by country;
  - kind and birth discounts, and the caller's thresholds;
  - destinations by comprehensive and sectoral embargo, a region, the country holding a region and another region;
  - missing, stale (still blocking) and not yet published lists, an authority named twice and an absent directory;
  - every refusal of the verb and of the module, with its key;
  - malformed and misnamed lists;
  - the real binary answering the same bytes, a blocked answer exiting 0 and a refused one non-zero.
- **CTest `ceridc|comply|^crd-no-|^crd-check`:** 57 of 57, among them `crd-check-registers`,
  `crd-check-capability-owners` and `crd-check-isa-isolation`.
- **Teeth:** each of 10 mutations of the verb is caught by the `[screen]` cases: a kind assumed, the validation
  skipped, a misnamed list accepted, an authority read twice, the threshold length unchecked, subject fields accepted
  without a name, the verdict not written, the gaps dropped, two agreement flags swapped and `present` taken from the
  request.
- **Strict analysis and format:**
  - `scripts/tidy-files.py` with clang-tidy 20 finds the four changed C++ files clean. Its first finding is fixed: the
    function-local constants are lower case.
  - The pinned clang-format 20.1.8 reproduces the new test file and every added line. `main.cpp` and `verbs.hpp`
    have older lines that it would reflow, and they are left as they are.

The track machine ran from 21:35 to 21:46 and from 21:49 to 21:58 UTC, about 0.33 h, and is stopped.

## Found: Windows command-line arguments are not UTF-8

On Windows, `main(int, char**)` receives its arguments in the ANSI code page, so `ceridc` (and every tool `main` in
`tools/`) gets a Turkish name such as `DENİZ` as code-page bytes, not as UTF-8. The fold reads them as separators, so
the name is screened wrong. crd-platform already treats every path as UTF-8 and converts it to UTF-16 for Win32.
`read_environment` reads with `GetEnvironmentVariableA` and has the same gap for non-ASCII values.

The in-process cases cover UTF-8 names on every platform. The real-binary case keeps ASCII arguments until the
arguments are read as UTF-8 on Windows, which is the next item of this track: one owned conversion of the wide
command line in crd-platform, adopted by every tool `main`, the environment read through the wide API, and a Windows
real-binary case with a Turkish name.

## Remaining in COMPLY.1

- **User action: approve the download of the official lists' first read** (OFAC's SDN and consolidated lists, the EU
  financial sanctions file, the UN Security Council consolidated list, the UK sanctions list, Turkey's asset-freeze
  decisions). The importers to `cerid-sanctions/1` and the dated embargo regimes land with that read.
- The user approves the first read of the instruments, and counsel reviews the eight classifications (user actions).
- The screening over MCP after [MCP-ADAPT](../ROADMAP.md#slice-mcp-adapt) (track A).
- The row is Done once [COMPLY.0](../ROADMAP.md#slice-comply.0) is Done.
