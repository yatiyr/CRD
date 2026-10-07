# DIAG.8a cook and hot-reload provenance, 2026-10-07

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.8a](../ROADMAP.md#slice-diag.8a). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-8a). Rules: [AGENTS](../../AGENTS.md).
> Preceding: [DIAG.8a CEIR core](2026-10-07-diag-8a-ceir-provenance.md).

## Goal

The CEIR core batch made provenance survive passes, the binary and the compiled plan. This batch takes it through the
cook and the hot-reload supervisor: a program cooked from a named source file navigates to that file after it is
loaded, a reload that does not cook says where it failed, and the installed generation keeps its own positions.

## What was missing (measured before coding)

- `cook_program_text` parsed with file id 0 and reported `ParseFailed` with no position at all; the parser's
  `error_line:error_col` was dropped.
- A verifier rejection (`CookResult::op`) pointed into the cook's Context. `ReloadSet::cook_source` cooks in a
  transient Context and destroys it before returning, so a failed `reload_source` reported only the error kind.
- `add_source` and `reload_source` took no file name.
- A reformat-only reload has the same content hash, so it is `NoChange` and its candidate is destroyed. The installed
  generation then kept the positions of the old text.

## What landed

- **`CookSite`** (`crd/ceir/cook/program_cook.hpp`): plain values (cook-Context file id, line, column, offender
  stable id, `ProvenanceGap`) that outlive the cook's Context. `CookResult::site` carries it on failure.
  `cook_program` fills it from `resolve_provenance` for every verifier rejection that names an op; a builder op with
  no position reports `NoSourceLocation` with line 0, never a zero line passed off as a position.
- **`cook_program_text(ctx, source, file, asset_id, ...)`**: registers `file` and parses under it, so every op's
  line:col rides the cooked blob's `ORIG` chunk; a parse failure's site is the parser's line, column and file. The
  unnamed overload is unchanged (file id 0) and both cook the same content and interface hashes.
- **`ReloadSet::add_source` / `reload_source(id, source, file = {})`**: the cook site is copied out of the transient
  Context into `AddResult::cook_site` / `ReloadResult::cook_site` before the Context dies. Both structs stay aggregates
  (the new member is POD and last).
- **Installed metadata.** The installed generation loads its origins from the blob, so its Context resolves every op
  (and a compiled plan's fault) to the authored file while later reloads fail. A `NoChange` reload whose candidate
  carries origins copies them op-for-op (pre-order; equal content implies equal structure) onto the installed ops,
  re-registering file paths in the installed Context; the generation and its handles stay current. An origin-free
  candidate (a builder blob) leaves the installed positions alone. Plans compiled before a refresh own their sites and
  keep the old positions; a recompile picks up the new ones.

## Test

`tests/execution/ceir-cook/test_reload_provenance.cpp` (new, 3 cases, 138 assertions, `[ceir][diag]` and
`[ceir][reload][diag]`). Expected positions come from scanning the text.
- **Cook.** A named-file cook and an unnamed cook have equal content and interface hashes; the blob read into a fresh
  Context runs a zero-step `core.for` to `BadForStep`, whose plan provenance names the file, line and column. A broken
  line reports `ParseFailed` at that line, column 5 and the file. An op of an unregistered dialect parses, fails the
  registration check, and its site is that op's line and column. The same module built in code reports
  `NoSourceLocation` with line 0.
- **Failed reload.** After `add_source` under file A, a parse-broken and a registration-failing `reload_source` under
  file B each report the line and column in the new text and install nothing; the installed generation, its handle and
  its runtime fault still name file A's `core.for` line. A failing first `add_source` reports the same site.
- **Reformat.** A reload of the reformatted text under file C is `NoChange`, keeps the generation and handle, and the
  runtime fault now names file C at the moved line. A builder blob with the same content is `NoChange` and leaves file
  C's positions. A body edit under file B hot-swaps to a generation that names file B.

Teeth (win-debug; each restored, touched and the lane rebuilt green):
- The file overload not registering the file: 10 assertions fail across all 3 cases.
- `fill_site` skipped: the verifier, builder, reload and add registration sites fail (9 assertions, 2 cases).
- The `NoChange` refresh skipped: the reformat case fails (9 assertions).
- `cook_source` not copying the site out: the failed-reload case's parse, registration and add sites fail (8
  assertions).

## Evidence

- **win-debug:** `crd-ceir-cook-tests` passes 40 cases (1,864 assertions); `crd-ceir-gpu-vulkan-tests`, the other
  includer of the cook headers, builds and passes 57 cases on the RTX 4070 Ti SUPER.
- **win-shipping, win-clang-cl-shipping, win-asan:** `crd-ceir-cook-tests` builds and passes 40 cases on each (the
  clang-cl thin-LTO link was clean; no ASan report).
- **WSL:** linux-gcc-debug, linux-gcc-asan and linux-clang-tsan each build and pass `crd-ceir-cook-tests` (40 cases)
  with no sanitizer report. These builds came before the final comment rewraps and the removed using-declaration,
  which change no behaviour; the four Windows lanes were rebuilt and rerun after them.
- **Checks:** strict tidy is clean on the three changed sources (one unused using-declaration removed). `clang-format
  --dry-run` on the changed lines reports only the repository's hand-aligned declarations; over-width lines (including
  multibyte comment arrows) were rewrapped. The Allman check, the ctest guards, check-master-plan and
  check-repository pass.

## Remaining on DIAG.8a

- CHIR lowering records ChirNode origins (node span + CHIR id) on the CEIR ops it creates.
- CKIR node identity and a GPU validation error navigate to the CEIR dispatch's origin.
- A host-provider (native intrinsic) runtime error names its op; script/intrinsic entry points and asset generation.
