# 2026-11-08 — NET.3: snapshot decode at serialize's cost

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET.3](../ROADMAP.md#slice-net.3). Board: [serializers](../bench/2026-11-08-net-3-serializers.md#decode-without-a-temporary-later-the-same-day).

User, 2026-11-08 (relayed with this run): the GPL and LGPL rewrite goes to a new agent, "do not regress anything";
it is that agent's work, and this track does not touch it.

## CI first

Nothing new from this track. 42416903705's two Build failures (linux-gcc-release, -shipping) are older than 8d5c9e63;
42413677756's are older than their fixes; 42423776692, 42428822989 and 42430818018 have no failure yet. No completed
run contains 6b9eca2c (42432298072 was cancelled by a newer push), so the public-checks pre-fetch of the
FlatBuffers and Cap'n Proto pins is still unread.

## What landed

- scripts/gen-wire-schemas.py: a decode no longer starts with `value = Value{}`. Each field is written once on every
  path, by its take or, when absent (an optional value, a newer revision's field), by its declared value; a struct
  whose lists decode into storage keeps `wire_fresh`. The profile had 28% of the entity decode on the reload of that
  temporary (a 16-byte load over a 4-byte store, which store forwarding cannot serve). Generated headers regenerated.
- delta.cpp: `first_difference` inlined into its two callers (its readers were copied through the stack).
- Tests: a list's storage refilled by an older revision's message holds that revision's defaults, never the newer
  message's values (removing one generated default fails it); every vector also decodes over the value the previous
  vector left.

## Evidence

- linux-gcc-debug: crd-containers-tests and crd-net-tests pass; the generator's 30 tests and `--check` pass.
- linux-clang-fuzz: wire-test, wire-delta-test, net-delta-stream and wire-session-control, 24.1 million executions on
  the final code (seed 12) and 57.4 million on an interim build, clean.
- Board, interleaved before and after: snapshot decode 1083.6 and 1075.0 ns against serialize's 1083.6 and 1081.2
  (1.00x, 0.99x; before 1.01x to 1.03x); delta encode 2% to 3% lower. A one-check reader (5% to 8% slower decode) and
  word-at-a-time varints (no change) were measured and dropped.

## Next

- The delta CPU gap is explained, not a clause: S28 keeps one interpreter; per-field dispatch is the cost above the
  floor. NET.3 closes at the audit of the first completed complete-tier run containing 9c7a6717.
