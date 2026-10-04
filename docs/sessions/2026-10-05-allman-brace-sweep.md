# Allman braces on every control statement: repository sweep and guard

<!-- doc-role: historical -->
> Dated evidence. Live owner: [REPO.DEV.10](../ROADMAP.md#slice-repo.dev.10); contribution rule:
> [CONTRIBUTING](../CONTRIBUTING.md). Rules: [AGENTS](../../AGENTS.md).

## User direction

The user restated the convention: a body of one statement after `if`, `while` and the like still takes curly braces,
Allman style. They asked first how often the agent had broken it (58 sites, fixed in the DIAG.7b batch), then to fix
the format everywhere: "fix all of them".

## The rule and why nothing enforced it

[.clang-format](../../.clang-format) has said it all along: `BreakBeforeBraces: Allman`,
`AllowShortIfStatementsOnASingleLine: Never`, `AllowShortBlocksOnASingleLine: Empty`,
`AllowShortLoopsOnASingleLine: false`. Nothing held it:
- `clang-format -i` is never run, because hand formatting is kept.
- `.clang-tidy` disables `readability-braces-around-statements`.
- The tree had about 19,200 sites, and the three weeks of DIAG commits added 705 more.

## Census (hand-written C/C++, before the sweep)

The census covered 2,750 tracked C/C++ files, excluding the 85 generated outputs named in
`scripts/generated-sources.json`.

| Kind | Sites |
|---|---|
| One-line block `if (x) { y; }` | 17,168 |
| Brace-less body | 1,548 |
| `if (x) {` (block opened on the head line) | 184 |
| `} else` | 31 |
| Control statement starting mid-line (crammed lines, one-line lambdas and inline functions, case labels) | about 430 |

Generated: 464 one-line blocks in the CEIR outputs of `tools/ceir_opgen/ceir_opgen.py`, 438 in the dialect sources
and 26 in the smoke tests. The other generated headers (FFT codelets, ODE tableaus, wavelet coefficients) were
already clean.

Excluded as vendored:
- `engine/numerics/hesap-tensor/include/crd/hesap/tensor/detail/dlpack.h` (upstream DLPack).
- `bench/reference/shewchuk-predicates.c` (Shewchuk's public-domain predicates, kept verbatim).

The sweep's first pass rewrote that file; the change was reverted.

## Method

The fix is mechanical and was not done with clang-format. `scripts/check-allman-braces.py` masks each file:
- String and character contents, raw strings, comments and preprocessor lines are blanked, at the same length.
- Brackets are matched on the mask, and edits are line-range rewrites, applied bottom-up and repeated to a fixpoint.

It rewrites:
- one-line blocks, including one-line `switch` blocks, with case labels on their own lines (`IndentCaseLabels`);
- brace-less bodies, including nested statements and their `else` chains;
- `if (x) {` openings and `} else` lines;
- control statements that start mid-line;
- whatever follows on the same line: more statements, or closing braces re-indented to their block.

A body that sits under its head without being indented gets four extra columns of indentation; a plain statement in
that position is refused. Two forms are kept as they are:
- An `else` / `#endif` / `if (...)` chain split by the preprocessor is accepted.
- An `else` that exists only under an `#if` gets its braces after the `#endif`. When the macro is off, the braces form a
  plain block. This applies to two sites in `fft.hpp`.

**Token oracle (the check that replaces a local build).** For every one of the 2,748 hand-written files, the original
was formatted to stdout only with the repository style plus:
- `InsertBraces: true`
- `SortIncludes: Never`
- `SortUsingDeclarations: Never`
- `BreakStringLiterals: false`

Comments and whitespace were then stripped from that output and from the rewritten file, and the token streams
compared. They are identical for 2,747 files. The one difference is the deliberate `fft.hpp` braces after `#endif`,
which clang-format cannot see across. So every brace landed where clang's own parse puts it, and nothing else changed
token-wise. No source file was formatted in place.

## Result

- **Edits:** 21,331 rewrites in 879 files, plus the generator. Per kind:
  - 18,892 one-line blocks;
  - 1,591 brace-less bodies;
  - 583 mid-line splits;
  - 234 `if (x) {` openings;
  - 31 `} else` lines.
- **Long lines:**
  - Twelve rewritten lines would have exceeded 120 columns.
  - Ten were conditions whose trailing comment had followed the one-line body. The comment now sits on the line
    above; the one that still exceeded 120 columns there was wrapped.
  - The other two were wrapped by hand.
- **Mixed line endings:** six files had them and now use their majority ending. The repository stores C/C++ as LF
  (`.gitattributes`), so the commit is unaffected.
- **Generator:** `ceir_opgen.py` emits its verifier checks through one `_reject_if` helper and its smoke-test check as
  Allman blocks. The 52 changed outputs were regenerated.
  - `ceir_opgen.py --check` passes.
  - The 62 generator tests pass.
  - The manifest hashes were refreshed with `check-generated.py --refresh`; the reason is this note.
  - `check-generated.py` passes (160 sources).
- **Guard:** `scripts/check-allman-braces.py` checks every tracked C/C++ source except manifest entries and the
  vendored list, in about six seconds. It is a hosted lane step after the generated-sources step and has four
  regression tests in `test-repository-tools.py` (55 tests pass). The rule is stated in
  [CONTRIBUTING](../CONTRIBUTING.md#route-4-a-build-system-change).
- **Checks:** `check-repository.py`, `check-ci-tiers.py`, `check-generated.py`, `check-master-plan.py` and
  `test-dev-workflow.py` pass.
- **Strict-gate sample:** the 17 most rewritten files.
  - 16 are clean.
  - `fft.hpp`, analysed as a header, reports nested conditionals, local-constant naming and multi-declaration
    findings. All of them are present at the pre-sweep revision and untouched by this change.
  - The full set is the hosted tidy lane's.

## Not in scope

- `.cu` CUDA benchmark sources and shader files are not C++ translation units of the engine build.
- `case` labels followed by a statement on the same line are a separate clang-format option
  (`AllowShortCaseLabelsOnASingleLine: false`). The sweep split them only where a control statement followed the
  label.
