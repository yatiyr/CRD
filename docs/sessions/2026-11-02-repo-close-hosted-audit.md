# 2026-11-02 — REPO close: REPO.3d, REPO.3 and the REPO parent

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md#slice-repo).

Closed at the user's order of 2026-11-01 ([record](2026-11-01-user-order-close-waiting-rows.md#user-order)) on
complete-tier run [41409186119](https://github.com/yatiyr/CRD/actions/runs/41409186119) at `fb8452af`, read in
[the DIAG close audit](2026-11-02-diag-close-hosted-audit.md).

- **REPO.3d:** DIAG closed in `8264a9d9`, a documentation-only commit after `fb8452af`, so the run is green on every
  lane for the same code (no unexpected failure on any of the 29 test lanes; the Required result job passes).
  win-release's wild-write case, repaired in `79d0c0c0`, passes there.
- **REPO.3:** on that run, the first complete run at or after `a0c9d534`: the six `[shewchuk]` cases pass on all four
  ASan lanes (each sharded lane in one shard); the cold `win-debug-scalar` install passes `--set-temp-repository` and
  prints no "Retrieving meta information from remote repository" line; no lane prints `-Wdeprecated-non-prototype`
  (the oracle builds under the tree's warnings on MSVC, clang-cl and the Linux Clang lanes); both repository-checks
  jobs pass over `525c8417` and later. The Linux repository-checks job of run 41298463221 passed. The Qt line stays
  registered as TP-8 and is let be (third-party policy).
- **REPO:** every child is Done or Recorded; publication and remote qualification stay the user's.
