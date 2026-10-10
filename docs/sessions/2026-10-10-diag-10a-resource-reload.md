# DIAG.10a resource-reload fuzzing, 2026-10-10

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.10a](../ROADMAP.md#slice-diag.10a). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-10a); harness:
> [test instruments](../design/test-instruments.md). Rules: [AGENTS](../../AGENTS.md).
> Preceding: [DIAG.10a command decoding](2026-10-10-diag-10a-command-decoding.md).

## Goal

The fifth slice of DIAG.10a: "resource reload". A cooked pack reaches the engine as a file the `ResourceManager`
mounts, loads from and hot-reloads; the CRDR container reader (`crdr_read`) parses it first. That ingress is now on
the bounded fuzz harness with oracles that are not the manager's own verdict, and what it found is fixed. It also
closes the item queued since DIAG.9b: `do_reload_mount` updated the live table before checking a pack's ranges.
Committed locally, not pushed.

## What changed

- A fuzz target `crd-fuzz-resources-reload` in `tests/assets/resources/fuzz` (the replay executable and its `-corpus`
  CTest on every lane, the libFuzzer executable under `CRD_ENABLE_FUZZER`), registered in `scripts/fuzz.py`
  (`resources-reload`), in the test-instruments table and in `test-repository-tools.py`'s target list; the resources
  module owns `tests/support/fuzz`, and crd-resources-tests excludes the `fuzz/` directory from its sources.
- The production entry points take paths, so each pack an input names is written to a per-process file under the
  platform temp directory and removed after the input. An even first byte makes the rest one pack file: `crdr_read`
  reads it directly, then the file is mounted, its ids (up to 16) loaded, the mount reloaded, everything evicted, the
  ids loaded again and the mount removed. An odd first byte makes it a script over two pack files and one manager:
  write a pack built from a decoded spec through `CrdrWriter` the way the cooker lays one out, then overridden (an
  entry's offset or size replaced, an id duplicated, the tail cut, junk appended, the manifest and string pool
  compressed, a chunk dropped, the container type changed); write raw bytes; corrupt a file in place (cut or flip a
  byte); mount; unmount (also ids that name nothing); `load_sync`; release a handle; `reload_mount_now`; drain (every
  handle released, the budget set to zero and back); pin and unpin. `poll_hot_reload` is not driven: it detects a
  change by mtime at one-second resolution, which is not deterministic within an input, and both reload paths run
  `do_reload_mount`.
- Two test loaders copy their artifact bytes and refuse an empty artifact or one starting with 0xEE; one has a
  placeholder. A third type has no loader.
- Oracles:
  - the CRDR layout restated in the harness (32-byte header, 24-byte chunk headers, payloads padded to 16 bytes of
    their own size, the padding after the last payload and any later bytes outside the container) with zstd decoding
    each compressed chunk: `crdr_read` must accept exactly what it accepts, give each uncompressed payload as a view
    at exactly its offset and each compressed one as exactly the decoded bytes, and allocate at most eight times the
    input plus 64 KiB plus what its accepted claims decode to;
  - the pack rules restated (a PACK container whose first MFST chunk holds whole 48-byte entries, every range inside
    the file, no id twice; names from the first STRP chunk up to a NUL or the pool's end): a mount is accepted exactly
    then, with the next mount id;
  - a model of the manager: the live table serves each id from the newest mount whose manifest names it; every loaded
    block's state, generation, bytes (the file's bytes at the range it was loaded from, as the file is on disk then)
    and resident size; pins; the reload subscribers' events. After every operation `find_entry` (mount, type, flags,
    range and name), `entry_count`, `mount_count`, `current_memory_use()` and every held handle (state, generation,
    payload type, placeholder flag, bytes) must equal it, a reload must return the model's swap count and fire each
    subscriber once with the new generation, and a refused mount or reload changes nothing;
  - each loader requires that only its own type's payloads reach `unload`;
  - the run returns every byte it allocated.
- Inputs whose compressed chunks claim more than 16 MiB in all are skipped as out of scope (decided from the claims,
  before decoding); at most 16 handles are held.
- 34 seeds, written by hand and each checked as written (the raw seeds' verdict is the real manager mounting them):
  seven packs that mount (honest, a compressed manifest and string pool, empty, a name index past the pool, a
  zero-length entry, artifacts both loaders refuse, trailing bytes), six refused whole (torn, a range that wraps, a
  duplicated id, not a PACK, no manifest, a manifest of partial entries), four compressed-chunk claims (past the
  bound, a pair whose sum wraps, a byte past what the frame decodes to, an exact one), a chunk stored past the end, a
  cut header, garbage, and 14 scripts.
- The replay driver (`tests/support/fuzz/src/replay_main.cpp`) installs a Debug CRT report hook on Windows Debug
  builds: a heap overrun the Debug CRT finds at a free now ends the replay with the report, instead of a modal dialog
  that held it until the CTest timeout (observed while checking the wrapping-claims tooth).

## Findings, each fixed and kept as a corpus input

1. `crdr_read` sized its decompression buffer from the chunks' claimed sizes before decoding anything. A 96-byte file
   whose one compressed chunk claimed 1 TiB reserved it (the harness's budget fatal; an out-of-memory abort in the
   engine). Two claims whose sum wraps past 2^64 reserved the wrapped 16 bytes, then the first frame decoded 32 bytes
   into them (a heap overrun). A claim is now bounded by what its stored bytes can decode to, before anything is
   reserved: every zstd block has a 3-byte header and decodes to at most 128 KiB, so `n` stored bytes decode to at most
   `n / 3 * 128 KiB` (`crdr_max_decompressed_size` in `crdr.hpp`). Seeds `chunk-claim-past-bound`,
   `chunk-claims-wrap`.
2. A frame that decoded to fewer bytes than its chunk claimed was accepted with the rest of the payload zero-filled,
   bytes the file never held. The decoded size must equal the claim. Seed `chunk-claim-past-frame`.
3. `do_reload_mount` (both reload paths):
   - it copied a new manifest's offsets into the live table before any range check, so a torn pack picked up by hot
     reload left entries pointing past the end of the file (the item queued since DIAG.9b), and it checked a reloaded
     range as `offset + size > file size`, which wraps: an offset near 2^64 passed and the loader read before the
     buffer. A reload now reads and checks the pack exactly as a mount does (`read_pack`) and changes nothing when it
     is refused. Seeds `script-reload-torn`, `script-reload-range-wraps`.
   - it reloaded every loaded id the new manifest named, even one a newer mount served, from the older pack's bytes.
     Only ids this mount serves are reloaded. Seed `script-reload-shadowed-id`.
   - it took the loader from the new entry's type, so a retyped entry's payload was swapped into a block of the old
     type (a handle's `get()` casts to that type) and the old payload was freed by the new type's loader. A reload now
     keeps the last-good payload of a retyped entry. Seed `script-reload-retyped`.
   - it only rewrote the offsets of ids already live: ids a reload added were never served, and ids it dropped kept
     their old offsets into the new file. The pack now becomes the mount's manifest and every id it named before or
     names now is resolved again. Seed `script-reload-drops-and-adds`.
   - a manifest naming one id twice gave two reload tasks for one block (two swaps, two generations, two callbacks).
     Such a pack is refused (finding 6). Seed `script-reload-duplicate-id`.
   - it never updated a block's resident size or the manager's total, so `current_memory_use()` kept the old size
     after a reload resized an artifact. Seed `script-reload-resizes`.
4. `unmount` erased every id the mount served, although its header said an id leaves the live table only when no
   other mount provides it; an older mount's id disappeared. Each mount now keeps its own manifest (`MountRecord`
   entries with an index), and the live table is derived from them: the newest mount naming an id serves it. Seed
   `script-unmount-falls-back`.
5. A re-issue after eviction took the loader from the live entry's type, so an entry retyped by a reload or a newer
   mount loaded a payload of the new type into a block (and handles) of the old type. A re-issue of another type now
   fails the load and leaves the block evicted, on all three load paths (`load_sync`, `load_async`, `load_streamed`).
   Seed `script-reissue-retyped`.
6. A manifest naming one id twice mounted, the later entry silently winning. It is refused: which artifact serves the
   id would otherwise depend on manifest order. The cooker writes one entry per source and does not check that ids are
   distinct (a follow-up); the demo pack the sandbox mounts has 34 distinct ids. Seeds `pack-duplicate-id`,
   `script-reload-duplicate-id`.

No on-disk format, record or schema changed. The contract these pin is stated in `resource_manager.hpp`; the v1f
description (`docs/archive/systems/resources.md`) was silent on all but newest-mount-wins.

## Teeth

Each break was applied to the restored source, the fuzz target rebuilt on win-debug, the named seeds replayed and
observed failing, then the source written back and the target rebuilt; after the last tooth the full corpus replays
green.

| Break | Seeds that fail |
|---|---|
| T1 the claim bound removed (`crdr_max_decompressed_size` of the maximum) | `chunk-claim-past-bound` (budget fatal), `chunk-claims-wrap` (Debug CRT: written past the heap buffer) |
| T3 the decoded size not compared with the claim | `chunk-claim-past-frame` (verdict) |
| T4 duplicate ids accepted | `pack-duplicate-id` (mount verdict), `script-reload-duplicate-id` (swap count) |
| T5 a reload that proceeds past a torn-pack refusal | `script-reload-torn`, `script-reload-range-wraps` (live table) |
| T6 unmount erasing instead of falling back | `script-unmount-falls-back` (live table) |
| T7 a reload of an id a newer mount serves | `script-reload-shadowed-id` (swap count) |
| T8 a reload across types | `script-reload-retyped` (swap count) |
| T9 a re-issue across types | `script-reissue-retyped` (resident size) |
| T10 a reload that keeps the old resident size | `script-reload-resizes` (resident size) |
| T11a a reload that ignores added ids | `script-reload-drops-and-adds` (live table) |
| T11b a reload that keeps dropped ids | `script-reload-drops-and-adds` (live table) |
| T12 pack ranges never checked (mount and reload) | `pack-torn`, `pack-range-wraps` (mount verdict), `script-reload-torn`, `script-reload-range-wraps` (swap count) |

T1 first held the replay on the Debug CRT's heap-corruption dialog until it was killed; the driver's report hook
(above) turned it into the failure in the table.

## libFuzzer rounds

Single-process rounds on `linux-clang-fuzz` (ASan and UBSan), 600 s each, `-max_len=65536 -timeout=10
-rss_limit_mb=2048 -malloc_limit_mb=256`, the committed corpus read-only beside a scratch working corpus, on the fixed
source (before the last edits, which only renamed the spec flags and rewrapped lines for the strict checks):

| Round | Executions | New units | Outcome |
|---|---|---|---|
| 1 (libFuzzer's own seed) | 152,294 | 2,597 | clean, no artifact, peak RSS 468 MiB |
| 2 (`-seed=2`) | 106,198 | 2,245 | clean, no artifact, peak RSS 481 MiB |

The target runs about 180 to 250 executions a second (each input writes and reads its pack files). Every finding above
came from the written seeds; the rounds found nothing further. The new units were not merged into the committed corpus.
No pack file was left in the temp directory.

## Verification

- win-debug: the whole tree built; the full ctest (inside vcvars) passed 7,533 of 7,533 (one test skipped by its own
  gate, as before). The last edits after it only renamed the harness's spec flags, split a nested conditional, added a
  lint comment to the replay driver and rewrapped three lines; the touched targets (crd-resources-tests, all twelve
  fuzz replay executables, crd-sandbox, ceridc) were rebuilt on the final source and the corpus replay (34 inputs),
  `--seed` reproducing it byte for byte, crd-resources-tests (128 cases) and 23 CTests (the `crd-no-*` and
  `crd-check*` guards, `crd-master-plan`, `crd-repository-*` and every `crd-fuzz-*-corpus`) pass.
- The real sandbox (`crd-sandbox --headless`, win-debug) mounts its cooked demo pack with the new reader (34 entries,
  all ids distinct, every range inside the file) and finds its 7 meshes, 1 skeleton and 3 clips.
- win-shipping, win-clang-cl-shipping and win-asan (inside vcvars), each reconfigured, with the same targets built:
  the corpus replay, the seeds byte-identical, crd-resources-tests (128 cases) and the twelve fuzz corpus CTests
  green; every clang-cl link succeeded first time; no sanitizer report.
- WSL, one lane at a time: linux-gcc-debug, linux-gcc-asan and linux-clang-tsan (`setarch -R`) built the target,
  crd-resources-tests and crd-fuzz-ceir-binary (the shared replay driver); the corpus replay, the seeds identical, 3
  repeats on each gcc lane and 10 under TSan, crd-resources-tests (128 cases), the ceir-binary corpus and the
  `crd-fuzz-resources-reload-corpus` CTest all pass with no sanitizer report. linux-clang-fuzz rebuilt on the final
  source replays the corpus.
- An input that fails ends its process before it removes its pack files, so the teeth runs left 17 of them in the
  Windows temp directory (named by process id); they were deleted. Passing runs leave none.
- Strict tidy is clean on all 6 changed C++ files (the harness's flag enum became constants, a nested conditional was
  split, and the CRT hook's fixed signature carries a lint comment). clang-format differs only in the repository's
  hand alignment, include grouping (the harness include first, as in every fuzz target) and line breaks. The Allman
  check, `test-repository-tools.py` (65 tests), `test-dev-workflow.py` (54), `check-master-plan.py` and
  `check-repository.py` pass.

## Remaining in DIAG.10a

The compiled and reference CEIR comparison; the virtual-memory and streaming allocators; reinvestigating the
unknown-origin CEIR and CKIR artifacts on the strengthened harness; settling the concurrent-load corruption; the
hosted run, which must now also show `crd-fuzz-resources-reload-corpus` (34 inputs) on all six lanes and
crd-resources-tests green.
