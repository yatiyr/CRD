# 2026-10-31 — HW-RESILIENCE.1: close-out of the local work

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [HW-RESILIENCE.1](../ROADMAP.md#slice-hw-resilience.1), the three items left after the
[durable saves](2026-10-31-hw-resilience-durable-saves.md).

## CI first

No completed run has a failed job. The push runs since the last batch were cancelled before their jobs started.
Complete-tier run 41215667488 (c2645814) is in progress; its finished jobs are green, both repository jobs included
(`check-generated.py` passes there and at HEAD: the stale FFT manifest of 4859c057 is fixed).

## Installer, patch and update chunk repair

No installer, patch or update code and no chunk manifest exist: [SHIP.2](../ROADMAP.md#slice-ship.2) defines the
chunk manifest (an [ADR-0038](../decisions/0038-cooked-binary-container-format.md) amendment) and
[SHIP.3](../ROADMAP.md#slice-ship.3) the updater and its repair. A verify-and-repair API with no format and no caller
would be dead code. The clause moves verbatim to both rows as their platform acceptance, linked both ways: installers,
patches and update payloads use this row's checksum family per chunk, verified after read and after decompression,
and repair re-fetches only the bad chunks.

## The network part

The plaintext integrity hash belongs to the wire serialisation, [NET.3](../ROADMAP.md#slice-net.3); the lockstep and
rollback state hashes belong to the replication policies, [4.2c](../ROADMAP.md#slice-4.2c). Both rows are track C's,
which this track does not edit, so the row names them as the owners and the operator relays the clause to track C to
record as their acceptance. [HW-RESILIENCE.3](../ROADMAP.md#slice-hw-resilience.3) keeps the divergence check that
compares those hashes.

## The hosted ARM64 runs

Complete-tier run 41215667488 ran the checksum cases on three ARM64 jobs, each green with every test passing:
`windows-arm64 (win-arm64-debug)` (job 124579596150, Cobalt 100), `linux-arm64 (linux-gcc-arm64-debug)` (job
124579596374) and `linux-arm64 (linux-clang-arm64-debug)` (job 124579596450), each in the plain run and the
`crd-containers-kernels-at-portable` and `-at-neon` arms. The logs do not say which crc variant ran: a passing case
prints nothing. `checksum kernels: the process runs the levels its CPU selection resolves` now prints it on every run
(`checksum kernels ran: crc avx2 +pclmulqdq +vpclmulqdq, hash avx2` on this host; CTest's log and JUnit keep it), so
the next ARM64 runs record whether the `+crc32` and `+pmull` variants were the ones tested.

## Status

The row is Needs CI: a complete-tier run at or after this commit whose `win-arm64-debug`, `linux-gcc-arm64-debug`
and `linux-clang-arm64-debug` jobs pass and whose LastTest logs show `crc neon +crc32 +pmull` in the unforced run.
