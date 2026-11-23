# 2026-11-23: NET-PATH, an idle path's RTT kept fresh

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path). Design: [S78](../design/network-foundation.md#multipath),
Scheduling and Proof bullets. RFC 10069 sections 3.3, 5.4, 5.5 and 5.9 read again for it.

## What landed

- Under `LowestRtt` a path that yields its data was never measured again, so a path that became the faster kept its
  old, slower estimate and never took data back from a trickle. `QuicConnectionConfig::path_refresh_ns` (1 s by
  default, 0 for none, at least `kQuicGranularityNs`; live through `set_path_refresh()`): a STREAM or DATAGRAM frame
  leaving one slot makes every other `refreshable()` slot whose last ack-eliciting packet is that old owe one PING
  (`owe_refreshes()`; a data path still sending, not `backup_held()`, nothing in flight, no probe timeout pending).
- The PING's sample renews the estimate (`QuicSentPacket::refresh`, `QuicRtt::sample`'s `renew`): one sample a second
  would move the exponential average an eighth at a time, too slowly to rank by (seen: without it the trickle stayed
  on the stale path for three seconds).
- Section 5.9 asks for no keep-alives: only this side's data starts one, so an end that only acknowledges sends none
  and an idle connection sends nothing more. A refresh lost on a dead path becomes its probe timeout, so the path is
  held dark before data goes there. A connection with one path is unchanged (no other slot is refreshable).
- `QuicConnectionCounters::path_refreshes`; the connection fuzzer's 0xF4 sets the refresh (bits 6 and 7).

## CI fix

CI 45100344604 (b8422b3b): linux-gcc-release, -relwithdebinfo and -shipping failed to link `crd-fuzz-net-quic-connection` on GCC 13's LTO `-Warray-bounds` in `begin_abandon` (slot 4 of `m_controls`, a path its guard excludes). Reproduced on linux-gcc-release; the stopped control is now cleared by pointer, and the whole lane then links every network target (its only other errors are HESAP benchmarks outside this track).

## Evidence

- New `[multipath]` case in `test_quic_connection.cpp`; `QuicRtt` renewal checks in `test_quic_recovery.cpp`.
- linux-gcc-debug: crd-net-tests all cases pass (`[multipath],[net-path],[recovery]` 357 cases; the rest 791), the
  recovery, connection, packet and cids corpora replay, the 19 `crd-no-|crd-check` guards pass; LLVM 22 tidy clean on
  the five changed sources; libFuzzer on the connection target 41594 runs (420 s), no finding.
- Teeth, each caught: refreshing a backup path; any ack-eliciting packet as the trigger (the server then refreshes 9
  times); no renewal (the trickle never moves back).

## Next

CI first: audit fuzz host-check 45111483680 and CI runs 45100344604 and 45111430486. Then PATH_ACK placement (the
lowest-RTT path or the path it acknowledges), with each moved acknowledgement's ranges retired by path ID.
