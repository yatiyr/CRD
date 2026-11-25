# 2026-11-25: NET.2, layout and shadow arms for the threaded ML-DSA signing refusal

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Rows: [NET-PATH](../ROADMAP.md#slice-net-path) batch; the entry is [NET.2](../ROADMAP.md#slice-net.2)'s provider.
Follows [the cross](2026-11-25-crypto-threaded-signing-cross.md), whose three next arms this batch builds.

**CI first:** no run containing dc6705c1 or 8f1e45aa has finished (CI 45559790834, 65e76ee7, is running); nothing to fix.

## What landed (94e5783e)

- `threads_harness.hpp` parses its own arguments for both programs: `gate`, `touch`, `touch-stack` (the same 512 KiB
  touch from a function AddressSanitizer does not instrument) and `shift=N` (each thread's work N bytes lower on its
  stack, below an untouched `alloca`; 1 to 65536, anything else a SETUP refusal).
- `crd-mldsa-threads-control slim`: `copy`'s storage in a session with no in-object signature array.
- `crd-mldsa-threads-cerid pad`: a crd-crypto session holding a raw session's 3,309 zeroed in-object bytes.
- `provider-host-check.py` (unit-tested): `control-copy-slim`, `control-copy-shift-1k`, `-2k`, `-3k`, `cerid-pad`,
  `cerid-touch-stack`.
- Fixed on the way: both touch frames were set but never read, which GCC's `-Werror=unused-but-set-variable` refuses;
  only MSVC had ever built these `EXCLUDE_FROM_ALL` programs. They now read their byte back.

## Evidence

- Built on win-asan (MSVC 19.51), win-clang-cl and linux-gcc-asan (GCC 13.3); clang-tidy 22 clean on both programs.
- `cdb uf` on the final win-asan exe: `touch_stack_unchecked` has no `__asan_*` call, is not inlined, and has a
  0x4048-byte frame against `touch_stack`'s 0x40E8 with its shadow calls (32 frames each, past 512 KiB). `shifted`
  poisons only its allocation's redzones (`_asan_alloca_poison`).
- Local driver round on the i9-14900K: all nine dispatch arms clean (this CPU has never failed). `shift=0`, `=65537`,
  `=12x`, `=`, `bogus` and `raw pad` print their SETUP line and exit 2 (win-asan; `shift=99999` on GCC).

## Dispatched

Host checks 45562483404 and 45562487761 on 94e5783e: the six new arms with `control-copy`, `cerid` and `cerid-touch`
as baselines on the same CPUs. They were queued behind CI 45559790834's 20 jobs when this note was written.

## Next

Read both runs: first which legs drew an affected CPU and whether `cerid` still fails there (this commit reshaped the
thread lambda, so 52 of 80 is no control). Then: a failing control arm is the Cerid-free reproduction (register once,
gate the case); a clean `cerid-pad` beside a failing `cerid` puts the cause in the layout, a failing one rules the
in-object bytes out; `cerid-touch-stack` clean means stack pages, failing means their shadow.
