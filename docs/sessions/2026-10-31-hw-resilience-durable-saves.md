# 2026-10-31 — HW-RESILIENCE.1: durable saves, generations, the journal and autosave

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [HW-RESILIENCE.1](../ROADMAP.md#slice-hw-resilience.1), the save paths: atomic saves with checksums that keep
previous generations, autosave and journaling, with bit-flip tests.

## CI first

No new failure. The jobs of push run 41204961252 (06f099ce) that finished after the last batch failed only on the
three known items (`crd-generated-sources`, `crd-repository-tools` on the stale FFT manifest, and
`crd-check-isa-isolation` on the GCC lanes), all fixed on main. `check-generated.py` passes at HEAD. The push runs
since then were cancelled before any job started. Complete-tier run 41215667488 (c2645814, newer than every
HW-RESILIENCE.1 commit) is queued; it holds the hosted evidence for the ARM64 CRC cases and for DIAG.11c and DIAG.12.

## DIAG.11b (8): the recording's preconditions

`crd-diag-overhead` is built on win-shipping-profile and win-shipping at one commit, and `record`'s own check
(`tree_current`) finds both trees current with the checkout. The user's command, from an elevated prompt with
Visual Studio closed: `scripts\elevated-captures.bat sampling`. The manifest records `modified: true` while another
session's ci/ edits are uncommitted; that is recorded, not refused.

## The primitives (crd-platform, `crd/platform/durable_file.hpp`)

- `write_file_atomic`: `<path>.tmp`, flushed to stable storage (`FlushFileBuffers`; `fsync`; macOS `F_FULLFSYNC`),
  renamed over the file (`MoveFileExW` with write-through), then the directory synced on POSIX.
- `save_generation` / `load_generation`: a 40-byte envelope (`CRDS`, version, generation, payload size and XXH3-64,
  the header's CRC-32C). The newest save is the file; generation g of the `kept` older ones sits in `<path>.(g mod
  kept + 1)`. A save writes the temporary, moves the verified main file into the ring (a damaged one to
  `<path>.damaged`), then renames the temporary: every crash point leaves a verifying generation, and a complete
  temporary left by a crash loads as the newest and is completed by the next save. A load reads every header, then
  the files newest first; a failed check is read once more, then the file is skipped. Ok, Recovered (with the newest
  file's fault), NotFound or Corrupt, and `save_load_message` says it in one sentence.
- `Journal`: checksummed records (sequence, size, XXH3-64, header CRC-32C) after a header naming the snapshot
  generation they apply to; `commit` flushes. `replay_journal` delivers the records before the first failing one and
  counts the bytes after it; `open` cuts that tail. One read whether or not the journal exists.
- `Autosave`: commit within `commit_interval_ns` of the first unsaved change (2 s by default), snapshot by age or
  journal size.

Ownership: a new `io.durable-saves` capability (owner platform) in `cmake/capability-owners.json`, signature
`MOVEFILE_REPLACE_EXISTING` or `F_FULLFSYNC`. crd-perf's `write_bundle_atomic` is the one other copy and is listed
pending under OWN.1: crd-perf is in the relocatable package and crd-platform is not, so crd-perf cannot link it yet.

## The cooker moves onto them, and a defect fixed

The asset cooker had its own temp-and-rename writer and a text journal. Its header promised a durable `begin`, but
the append only called `fflush`, so a power loss could lose the record while the artifacts it guards were half
written. The cooker's own writer is deleted: artifacts, pack and database go through `write_file_atomic`, and the
journal is an `fs::Journal` opened from the replay `load()` already read (the cook's read sequence is unchanged, so
the DIAG.9b cook fault plans still hit the same reads). A journal that cannot be read now distrusts every job.

## Documents

No document system exists yet. The clause "every saved document" moves to [DOC.2](../ROADMAP.md#slice-doc.2) as a
platform-acceptance clause, linked both ways: each document format saves through these primitives on every platform.

## Verification (win-debug)

- `crd-platform-tests`, all 65 cases. New `[durable]` cases: every bit of the newest save flipped on disk and every
  shorter length (each falls back to the previous generation with the typed fault, after one re-read); flipped reads
  through the seam (one flip is read again, every read flipped falls back); all generations damaged (Corrupt, empty
  payload); an interrupted save loaded and completed; every torn length of a save in flight ignored; a damaged main
  file kept out of the ring; every bit and every torn length of a journal (a prefix of the records, never a changed
  one, and appends continue after the cut); journal reset and base; the autosave cadence.
- `crd-resources-tests [cook]`, 18 cases, with a new one: a torn last journal record keeps the records before it,
  a damaged journal header recooks every job.
- Teeth, each restored, rebuilt and rerun green: skipping the payload hash failed 2 cases; removing the re-read failed
  the transfer-flip case; rotating a damaged main file into the ring failed its case; keeping the jobs of a damaged
  cook journal failed the new cook case.
- Tidy on the changed sources, the format oracle on added lines, the Allman check and the ownership guard pass.

## Still left on the row

- Installer, patch and update chunk repair.
- Network: the plaintext integrity hash and the lockstep and rollback state hashes, with the NET rows (track C).
- The hosted ARM64 runs of the CRC variants (complete-tier run 41215667488).
