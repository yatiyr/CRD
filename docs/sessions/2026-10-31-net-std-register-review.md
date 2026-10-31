# NET-STD: the scheduled re-research of the standards register

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Date: 2026-10-31. Row: [NET-STD](../ROADMAP.md#slice-net-std) (track C, batch 5; the
[parallel tracks](2026-10-31-parallel-tracks.md)). Contract: the registers'
[snapshot and review](../design/registers.md#snapshot-and-review).

## CI first

No push of this track has a finished hosted run of its own: every GitHub Actions push run since 3bb87c7f, the run of
79f88f6a (41221773970) included, was cancelled by a later push before any job was scheduled. The operator's
checkpoint run 41215667488 (workflow dispatch at c2645814, which holds batches 1 and 2: the standards register, its
guard step and the BenchMachine POSIX tests) has both "Repository checks" jobs (ubuntu-24.04 and
windows-2025-vs2026) green, so the "Standards register is current" step and the tooling regression checks pass on
both hosts; win-vs, win-arm64-debug, linux-gcc-debug-scalar, linux-gcc-relwithdebinfo and the arm64 debug lanes were
green and the rest still running when this batch ended. crd-net and crd-crypto (batches 3 and 4) have no hosted build
yet. No failure is attributed to this track.

## What landed

- **The review schedule is data.** The register's `review` gains `lag_days` (25), `full_every_days` (365), a
  `calendar` of the events that owe a scan (IETF 127, 2026-11-20, `verified: false` until the IETF meeting page
  confirms it) and `runs` (the 2026-10-27 networking research is the first full run). `review_due` in
  `scripts/check-registers.py` computes the due day: the earliest of a scan `lag_days` after each event later than the
  latest run, a full re-research a year after the latest full run, and a full re-research `lag_days` after a light run
  that found a pinned entry's status changed. The guard fails a typed `next` that differs (the register's 2026-12-15
  is the computed day), a calendar with no event after the latest run, malformed or unordered events and runs, a run
  after today, a run citing a missing document or an unknown entry, and a schedule without a full run. The legal
  domain (COMPLY.0) reuses the same fields for its quarterly reviews, platform events and gazette triggers.
- **`scripts/register-review.py`** runs a review in five commands:
  - `due` prints the review owed, why, and the unverified calendar events;
  - `plan` writes the primary sources a refresh reads, for the user to approve: today 63 (the RFC Editor JSON record
    of 43 RFCs, the datatracker record of 16 drafts and four IANA registry files) and 6 items read by hand (the W3C
    trace context page, the NIST publications list for FIPS 204, FIPS 186-5 and IR 8547, the ML-DSA object
    identifiers at NIST CSOR and the IETF meeting calendar);
  - `fetch` reads an approved plan over HTTPS only, at most 16 MiB a source, with a SHA-256 manifest and recorded
    errors;
  - `compare` verifies the digests and judges every record: a draft's new revision and posted day, its status (active,
    IESG processing, RFC Editor queue, expired, replaced, published as an RFC), an obsoleted RFC, and each IANA
    codepoint confirmed, named differently, assigned another value or absent. The report lists register changes,
    proposed ROADMAP amendments, the pinned status changes that trigger a full re-research, unreadable sources and the
    items read by hand. `--apply` writes the register (statuses, revisions, confirmed codepoints lose
    `verified: false`, the snapshot day), records the run with its triggers and moves `next`. It never opens the
    ROADMAP for writing, and a codepoint IANA contradicts stays a proposal, because the generated headers change only
    by review;
  - `record` records a run made by hand and moves `next`.
- The readers' record shapes (the RFC Editor's `obsoleted_by`, the datatracker's `rev`, `state`, `iesg_state`,
  `rfceditor_state`, `aliases` and `rev_history`, IANA's registry XML) were written from knowledge; a record without
  them is reported unreadable and changes nothing, and the first approved refresh confirms them.
- The contract, `scripts/README.md` and the guard's own docstring describe the schedule and the tool.

## Verification

- `test-repository-tools.py` `Registers` (9 cases: the existing seven on the new schedule, the schedule's refusals
  and `review_due`'s four rules) and the new `RegisterReview` (7 cases over a fixture tree with fake transports:
  `due`, `plan` and its refusals, `fetch` with digests, an unreachable source, an oversize body and a plain-HTTP
  refusal, `compare` without and with `--apply` (an obsoleted pinned RFC, a new draft revision in IESG processing, a
  draft published as RFC 9911, confirmed, misnamed, misvalued and absent codepoints, an unreadable record, the run and
  its trigger, the ROADMAP's bytes unchanged, LF endings), a clean refresh that passes the guard, tampering refused,
  `record` and its refusals, and the readers' refusals): passing on Windows, and the whole suite (198 tests, 2
  skipped) on the track machine under Linux Python.
- Teeth: ten mutations each fail the cases: no trigger recorded, no digest check, no size bound, no misvalue
  detection, the RFC Editor queue ignored, plain HTTP accepted, events covered by a run still owed, triggers counted
  after a full run, a typed `next` accepted and a calendar without a future event accepted.
- `check-registers.py` passes on the real register (next review 2026-12-15, after IETF 127); `register-review.py due`
  and `plan` run on it.
- Machine time: 0.10 h (17:08 to 17:14 UTC; TERMINATED). The machine reports itself as n2-standard-8.

## Remaining in NET-STD

- **User action:** approve the first refresh's download: the 63 public metadata records `python
  scripts/register-review.py plan --out <file>` lists (rfc-editor.org JSON records, datatracker.ietf.org document
  records and four iana.org registry XML files, all small text). The run then reads the six items by hand, confirms
  IETF 127's dates, verifies the `iana::unverified` codepoints of both generated headers and records the run.
- Done once [NET.1](../ROADMAP.md#slice-net.1) is Done (it waits on OWN, track B).
