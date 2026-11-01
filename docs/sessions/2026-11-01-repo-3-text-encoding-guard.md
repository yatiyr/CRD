# 2026-11-01 — REPO.3: authored text is UTF-8 without BOM and without double encoding

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [REPO.3](../ROADMAP.md#slice-repo.3), two of the items the
[NLopt route](2026-11-01-repo-3-nlopt-difftest-route.md#remaining-on-repo3) left: the mojibake with a guard, and the
pinned actions' Node deprecation lines. Built and tested on `cerid-track-b` (spot c4-standard-8, Ubuntu 24.04,
GCC 13.3) in the `linux-gcc-debug` tree. [MCP-ADAPT.0](../ROADMAP.md#slice-mcp-adapt.0) still waits on hosted CI and
[REPO.3d](../ROADMAP.md#slice-repo.3d)'s flip on DIAG.

## CI first

No GitHub Actions run has finished for any commit since 03571fa3. Run 41269105413 (b0b1b245) is still running;
its eight failed jobs all stopped at the build on two causes already repaired on main: the `min` and `max` macros of
windows.h in `filesystem.cpp` (win-debug, win-debug-scalar, win-asan shard 2, win-public-checks, clang-cl,
clang-cl shipping and win-vs; fixed by 9723ac75, after b0b1b245, and every windows.h include under
`engine/foundation/platform` now has NOMINMAX before it) and MSVC's C4324 on `CheckStream` (win-arm64-debug; fixed
by 921e19cc, track A). The push run at de5e10b0 was cancelled when a newer push arrived; a `workflow_dispatch` run at
de5e10b0 (41274380605, its own concurrency group) is queued for runners, and the push group's pending run is now
41277265371 at 7ed2c63e. The own CI store
(`ci/ops.py build list`) holds no builds.

## The repair

`tests/execution/ceir/test_plan.cpp` entered the repository on 2026-08-10 (ea622e3d, then `tests/ceir/`) already
double-encoded: its UTF-8 had been read as Windows-1252 and saved as UTF-8 again, so an em dash (E2 80 94) became
the three characters U+00E2 U+20AC U+201D, and a star (E2 AD 90) became U+00E2 U+00AD U+0090 (0x90 has no
Windows-1252 character, so it survived as the C1 control). It also began with a byte order mark. Lines from 1277 on
were added later in clean UTF-8, so the file was mixed and no version in history is clean to compare against.
`tests/gpu/gpu-context-vulkan/test_vulkan_gsplat.cpp` had one such em dash (line 381, from aabbc97a).

A one-shot script, not kept, replaced each sequence that maps back (through Windows-1252, its unassigned bytes as
C1 controls) to the bytes of one UTF-8 character by that character, bytes in and bytes out. Its proof per file:
272 sequences in `test_plan.cpp` and one in the gsplat test; a second pass finds none; re-corrupting each repaired
character at its recorded position reproduces the original bytes exactly; and the files' ASCII projections (every
non-ASCII character removed) are unchanged, so only comment glyphs moved (section signs, middle dots, em dashes,
arrows, the stop and star signs, box-drawing rules). Twelve files carried a byte order mark (`test_plan.cpp` and
eleven others: `rational_time.hpp`/`.cpp`, `audio_graph.cpp`, `flac_decode.cpp`, `flac_encode.cpp` and six test
sources and test CMake lists); it is removed. MSVC and clang-cl compile every engine and test unit with `/utf-8`
(the `crd-warnings` interface), and 1,063 installed public headers already carry non-ASCII without one, so no
consumer meets a new case.

## The guard

`scripts/check-repository.py` (the `crd-repository-hygiene` CTest and the repository step of the workflow, the
pipeline and the CI-4 parity table, so no new wiring) now checks every tracked file Git treats as text, from
`git ls-files --eol`: not unset by `.gitattributes`, not detected as binary. Each must be valid UTF-8, must not start
with a byte order mark, and must hold no double-encoded UTF-8.

- **The rule.** A lead character, followed by characters that are Windows-1252's forms of bytes 0x80 to 0xBF, whose
  bytes decode to one UTF-8 character. Leads of three- and four-byte characters (U+00E0 to U+00F4) always count;
  two-byte leads only from Latin-1 and Latin Extended-A (U+00C2 to U+00C5, the letters European text loses this
  way). Other two-byte leads are ordinary text in this tree: the multiplication sign before an en dash in the
  boards' "1.2x-3x" ranges (D7 96 would decode to a Hebrew letter), and a capital U umlaut before an apostrophe in
  the Turkish research note. Offending sequences are printed `ascii()`-escaped, since the Windows repository job
  runs the check.
- **Exceptions.** `MOJIBAKE_EXCEPTIONS` names legitimate matches per file with the reason: in `spai.hpp` and the
  2026-05-26 SPAI session note, a capital A circumflex (A-hat, the column block A(I, J_k)) followed by a middle dot
  is the product with m-hat, not a double-encoded middle dot. Narrowing the rule to characters above U+00FF would
  have exempted it without a table, but would also stop catching corrupted section signs and accented letters.
- **Binary data.** Six `.bin` tensor fixtures hold no NUL byte, so Git's detection read them as text.
  `.gitattributes` now declares the tensor and model data formats (`.bin`, `.npy`, `.npz`, `.safetensors`,
  `.onnx`, `.q8`, `.q8s`) and `.glb` and `.3mf` `binary`; none of them changes in the index.
- **Tests.** `test_text_encoding` in `scripts/test-repository-tools.py`: clean text with the legitimate look-alikes
  passes; a byte order mark and invalid UTF-8 are named; an em dash, a star (the C1 byte), a section sign, e acute,
  g breve and a four-byte character, each corrupted the Windows way, are caught and named with the character they
  stand for; a lead with too few tail characters or with an overlong decode is left alone; an exception applies to
  its own sequence only. `test_text_names_follow_git_text_detection` parses the listing (unset, binary, paths with
  spaces). Teeth: dropping the two-byte leads, or taking every two-byte lead, fails the test; the guard against
  HEAD's two files reports the byte order mark and 272 and 1 sequences; emptying the exceptions fails the two SPAI
  files.
- `docs/design/repository-layout.md` states the contract for all authored text.

## Node deprecation lines

The pinned actions' `DEP0040` (punycode) and `DEP0169` (`url.parse`) lines, recorded from run 38640651640, were
printed by every job until 30c243bd (2026-10-26, "run every workflow action on Node 24"), which moved checkout,
setup-python, cache, upload-artifact and github-script to their Node 24 releases; job logs of 2026-10-22 still show
them. Eleven jobs of run 41269105413 (both repository-check jobs, linux-gcc-arm64-debug, linux-gcc-public-checks
and seven Windows jobs, each through its post-job cleanup) print neither. The clause is closed. The LunarG installer's
Qt `setPixelSize` line stays open: the Windows lanes restore the SDK from cache (`vulkan-sdk-1.4.341.1-windows`), so
no lane of this run ran the installer; it needs a cold Windows lane.

## Evidence

- `linux-gcc-debug`: the touched targets build (185 steps); `crd-ceir-tests` 18,784 assertions in 580 cases,
  `crd-time-tests` 73 cases, `crd-audio-tests` 31, `crd-hesap-opt-tests` 154, `crd-hesap-ode-tests` 67,
  `crd-ceridc-tests` 35, `crd-timeline-tests` 6, all pass; `crd-gpu-context-vulkan-tests` on lavapipe 313 passed and
  1 skipped with the pinned validation layer (`scripts/install-vulkan-validation.py`; without it the six
  validation-identity cases fail, as they would on any host without the layer). CTest: the guards
  (`^crd-check|^crd-no-|^crd-diag-coverage$|^crd-diag-seeded-faults$`), `crd-repository-hygiene`,
  `crd-repository-tools` and `crd-master-plan`, 24/24 pass.
- Local validators: `check-repository.py`, `check-allman-braces.py`, `check-generated.py`, `check-master-plan.py`,
  `check-capability-owners.py`, `check-docs.py`.

## Remaining on REPO.3

- Routes or retirements for the 27 other `unrouted` sources (the CUDA and VkFFT programs under `bench/`, the Shewchuk
  predicates reference, `scripts/bench_gemm_v0d.cpp`, `scripts/v14m_ggml_q8_bench.c`, the MKL batched-GEMM bench and
  the two liquid-dsp peers).
- The LunarG Qt line, from a cold Windows lane.
- Then REPO.3d (scoped configuration and consumer checks, then full remote CI evidence on the published revision once
  DIAG is Done) and REPO.3's close.
