# 2026-10-31 — HW-RESILIENCE.1: CRDR integrity and the one re-read

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [HW-RESILIENCE.1](../ROADMAP.md#slice-hw-resilience.1), the cooked-asset and compressed-block paths: checksums
checked after the read and after decompression, one re-read from the source, a typed error and a clear message.

## CI first

Push run 41204961252 (06f099ce) failed four ways. Three were the stale generated-source manifest that 8332981c had
already refreshed. The fourth was new: `crd-check-isa-isolation` on linux-gcc-arm64-debug. The scalar kernel table
(a5e21b25) was filled with namespace-scope lambdas, and GCC emits each lambda's static thunk as a weak symbol named
under the table (`crd::math::kScalarMathKernelsPortable::{lambda(double)#2}::_FUN`), outside the level namespace.
x86 GCC does the same (reproduced with WSL g++ and `nm`). bcaed0e1 fills the table with `&lv::exp` and so on, so
every symbol stays in its level namespace: 72 in `portable::`, none outside.

## The format: container version 3

[ADR-0038](../decisions/0038-cooked-binary-container-format.md#amendment-2026-10-31-container-version-3-checksums)
is amended.

- The header and each chunk header grow to 40 bytes and end with the CRC-32C of their first 36 bytes. A chunk header
  also holds the CRC-32C of its stored bytes and, when compressed, the XXH3-64 of its decoded bytes.
- The version is 3 because 2 already names the DX12 device-removal bundle, which shares the `CRDR` magic. Other
  versions are refused.
- `crdr_read` checks each header before trusting any field of it, then each stored payload, then each decoded
  payload. The new errors are `HeaderCorrupt`, `ChunkCorrupt` and `DecodedCorrupt`, and `CrdrFile` names the chunk
  that failed. `crdr_error_text` replaces the cooker's own copy of the error strings.
- `CrdrWriter::add_chunk` loses its unused flags argument: a caller passing the compressed bit with raw bytes would
  have written a broken file.

## The re-read

- `CrdrFaultScope` records the first failing `crdr_read` on its thread. The resource manager opens one around each
  loader call, so it sees a parse failure however deep in the loader it happened (`hdr_from_crdr`, scene and obek
  copies, matrix and tensor readers).
- `load_checked` runs the sync, async, streamed and hot-reload paths. When the loader fails and the scope holds a
  failure, the artifact is read once more from its source and loaded again. A second failure leaves the handle
  Failed (or Placeholder) with `integrity_error()` set and an error naming the chunk and the fix (re-cook or
  re-install).
- A loader that fails on bytes that passed every check (its own allocation, or bad content) fails as before,
  without a re-read. The DIAG.9b allocation-failure reload case relies on this.
- The mount and reload pack reads (`read_pack`) read the pack once more when the container check fails.
- The cooker's cache check (`products_intact`) also requires `crdr_is_current`. A cache cooked under an older
  container version is cooked again instead of shipped.
- Other compressed readers already check their own format's checksums (ZIP members after inflate, PNG chunks, the
  diagnostic bundle), so they are unchanged.

## A fault seam for flipped bits

The I/O fault seam gains `IoFaultAction::Flip`: the read succeeds and delivers every byte, with bit `value` inverted.
FaultInjector gains the `flipped-read` site for it. Every crd-platform read and the diagnostic asset read apply it.
This is the storage or transfer fault a read cannot see, and the tests use it to drive the real recovery paths.

## Verification (win-debug)

- `crd-resources-tests`, all 141 cases. New cases:
  - Every bit of a two-chunk container flipped in turn: each flip is the expected typed error with the right chunk,
    except flips in the padding, which the reader never looks at.
  - Forged decoded hashes and stored-chunk fields, an old version with a valid checksum, and the fault scope's
    nesting.
  - Flipped reads on the sync, async and streamed loads at three places each, on the mount and on both kinds of
    reload: each loads after exactly one re-read.
  - Corruption on disk, stored and decoded: Failed with the typed error after exactly two reads and two loader
    calls.
  - An old-version cache artifact whose hash the database still records is cooked again, and the pack is
    byte-identical.
- `crd-platform-tests [diag9b]` (Flip on the binary, range, text and async reads) and `crd-perf-tests` (flipped-read
  ranking and counting).
- Also run: the anim, audio, CEIR cook, cooker, hesap resources, preset, profile, scene and timeline programs, and the
  anim and tensor io CTests.
- The fuzz target's CRDR restatement now checks the checksums, and its 19 raw seeds are regenerated
  (`--seed`; the 15 script seeds are unchanged). The corpus replays with 34 inputs.
- Teeth, each restored, rebuilt and rerun green:
  - Skipping the stored-bytes check failed 5 of the integrity cases (287 assertions).
  - Disabling the re-read failed the 3 manager cases.
  - Dropping `crdr_is_current` from the cache check failed the cooker case.
- The ctest guards, tidy, the format oracle on added lines and the Allman check pass.

## Still left on the row

- Documents: atomic saves with checksums and generations, autosave and journaling.
- Network: the plaintext integrity hash and the lockstep and rollback state hashes. These are owed to the NET rows,
  which track C owns.
- Repair of installer, patch and update chunks.
- The hosted ARM64 runs of the CRC variants.
