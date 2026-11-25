# 2026-11-25: NET-PATH, a learned handover lock refuses the path's background before it keeps

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path). Follows [the LEO claims](2026-11-25-net-path-leo-claims-one-draw.md),
Next 2. User decision on record (2026-11-01): "I WANT NET DONE PROPERLY AND GOLD STANDARD AND WITHOUT ANY DEFERS!"
Ran on this PC (WSL2 linux-gcc-release, -debug, linux-clang-fuzz; MSVC win-debug).

**CI first:** nothing to fix. 45559790834 (65e76ee7) finished green, win-asan shard 2 included; no run containing
02ea3dff or 2cff089a has finished (45601823342, 45613227799 pending).

## The trace

A temporary trace of the learner (removed) over 16 runs of the 1 s stopping case (seeds 0-3, first handovers 3.0,
3.3, 3.6, 3.9 s): after the last handover, every disturbance in the locked window's four bins was an RTT rise of
2,355 us (two 1,472-byte datagrams at 10 Mbit/s, just above the 2 ms step floor, with 8 J near 0) or, in 8 runs,
one congestion loss. Their share, about 0.22 a period, matched the background the lock test measures from the other
bins (0.12 to 0.17 a two-bin window). So the keep rule (3 periods and a third of those observed) counted the queue's
own noise as handovers: no feed from the schedule in force, only a rule with no background test.

## What landed

- `fix(hesap-net)`: the keep also needs the binomial tail against the background at `kKeepAlphaQ62`, the lock's
  10^-3 over the 2 kWindow - 1 windows the keep tries (`static_assert` it is no stricter than the lock's, so a
  fresh lock never fails its first keep). A clean path still releases on the eleventh quiet period.
- A unit case: a random loss a second on a 15 s period (38% of periods disturbed in any window) holds the lock all
  48 periods while handovers go on, and releases it within 4 + 11 periods once they stop.
- The connection case's release is held to a band: 7 to 14 periods after the handovers stop.

## Evidence

- Sweep, release, 40 runs (seeds 0-3, first handovers 3.0-3.9 s by 100 ms): locks by 14 s as before, released
  27.99 to 32.99 s, 8 to 13 quiet periods; the same 16 traced runs released at 12 to 16 before.
- Teeth: on HEAD's learner the unit case never released (held 48 of 48) and the default connection run released at
  34.98 s, past the band's 34 s.
- `crd-net-tests`: 1,208 cases, 1,200 passed, 8 skipped (release and debug); `crd-hesap-net-tests` 193 cases pass
  on release, debug and MSVC win-debug (4,490,975 assertions each). libFuzzer estimators: 2,266,382 runs clean.

## Next

1. CI first: the first finished run containing 02ea3dff, 2cff089a and this fix (MSVC, clang-cl, ARM64, TSan), and
   win-asan's TP-17 gate lines.
2. `has_data` per datagram and the ACK rule on the bulk path.
