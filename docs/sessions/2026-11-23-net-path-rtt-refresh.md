# 2026-11-23: NET-PATH, an idle path's RTT kept fresh

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path). Design: [S78](../design/network-foundation.md#multipath),
Scheduling and Proof bullets. RFC 10069 sections 3.3, 5.4, 5.5 and 5.9 read again for it.

## What landed

- Under `LowestRtt` a path that yields its data was never measured again, so a path that became the faster kept its
  old, slower estimate and never took data back from a trickle. `QuicConnectionConfig::path_refresh_ns` (1 s by
  default, 0 for none, at least `kQuicGranularityNs`; live through `set_path_refresh()`): a STREAM or DATAGRAM frame
  leaving one slot makes every other `refreshable()` slot whose last ack-eliciting packet is that old owe two PINGs
  in two packets (`owe_refreshes()`; a data path still sending, not `backup_held()`, nothing in flight, no probe
  timeout pending).
- The second PING draws an immediate acknowledgement, so the sample has no ACK delay (one PING measured 65 ms for a
  40 ms path: a sample under the path's minimum keeps its delay). It renews the estimate (`QuicSentPacket::refresh`,
  `QuicRtt::sample`'s `renew`): one sample a second moves an exponential average too slowly to rank by.
- Section 5.9 asks for no keep-alives: only this side's data starts one, so an end that only acknowledges sends none
  and an idle connection sends nothing more. A refresh lost on a dead path becomes its probe timeout, so the path is
  held dark before data goes there. A connection with one path is unchanged (no other slot is refreshable).
- `QuicConnectionCounters::path_refreshes`; the connection fuzzer's 0xF4 sets the refresh (bits 6 and 7).

## CI fix

CI 45100344604 (b8422b3b): linux-gcc-release, -relwithdebinfo and -shipping failed to link
`crd-fuzz-net-quic-connection` on GCC 13's LTO `-Warray-bounds` in `begin_abandon` (slot 4 of `m_controls`, which its
guard excludes). Reproduced on linux-gcc-release; the stopped control is now cleared by pointer, and the release and
relwithdebinfo lanes link every network target (the release lane's only other errors are HESAP benchmarks).

## Evidence

- Two `[multipath]` cases in `test_quic_connection.cpp` (the refresh; an idle path gone dark found by its refresh's
  probe timeout, no data there); `QuicRtt` renewal checks in `test_quic_recovery.cpp`.
- linux-gcc-debug: crd-net-tests all cases pass, the recovery, connection, packet and cids corpora replay, the 19
  `crd-no-|crd-check` guards pass; LLVM 22 tidy clean on the changed sources; libFuzzer on the connection target.
- Teeth, each caught: refreshing a backup path; any ack-eliciting packet as the trigger (the server then refreshes 9
  times); no renewal (the trickle never moves back); one PING (65 ms, the trickle stays). `DataWatch` charged a
  datagram a socket down lost to the next one; the fixture's `Filter::sent()` now sees every datagram.

## Next

- libFuzzer found `crash-bb4b5a5c` (in WSL `~/fuzz-scratch/crash1`; base64
  `wPMAAAAOAAAAJMAMAAL+878A9/gCBvoADoAAAAMAAAAAAPd+wCT+AgAM8wIC+gAOgAD4+PgAAwAARgAAAAAAAAACAAzzv/f4AAIG+gAAAD0AAAAAAAAAAC4AAAAuAAAgAPoHAOwHAPr6Bw==`):
  the follow oracle's `counters.path_failures != 0U` (fuzz_quic_connection.cpp, clean path). It replays with refreshes
  off, so it predates this slice: reproduce, fix, commit the input.
- CI first: fuzz host-check 45111483680 and the rest of CI 45100344604. Then PATH_ACK placement.
