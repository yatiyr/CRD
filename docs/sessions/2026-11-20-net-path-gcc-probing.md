# 2026-11-20 — NET-PATH: GCC's probing, and three CI fixes

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path). Design: [S74](../design/network-foundation.md#gcc-probing).

## CI first (scheduled runs 43003055646 to 44723313119, all on 6f4cfed5)

- win-relwithdebinfo (44723313119): the runner's MSVC moved from 14.51 to 14.52.37025, whose LTCG raises C4723
  (potential divide by 0) under /WX in `clock.cpp`'s line sums and `mat_types.hpp`'s Mat2 `try_solve`. Both divisions
  are now guarded as `otg.hpp`'s are: `line_sums` divides only by a non-zero weight (every caller has a point of
  positive weight), and `try_inverse`/`try_solve` refuse an exact zero determinant, which only a bound of 0 or below
  let divide (new cases). This PC's 14.51 never warned; only the next hosted run confirms it.
- linux-gcc-public-checks, picoquic Prague (3 of 9 runs): Cerid's client direction ran 0.775 to 0.781 busy, under the
  0.8 floor, whenever picoquic's echo was held by the same AQM about as often (577 to 720 marks) and itself ran 0.676 to
  0.73; unheld runs reach 0.96. The floor is now peer-relative in that case (`busy_floor`).
- linux-gcc-public-checks, qmux crate over h3 with 5% loss (3 of 9 runs): the Quinn client's one CONNECTION_CLOSE was
  lost and the Cerid server lingered out (`h3_close=local`). A lingering server now PINGs every 50 ms, so a closing
  client answers with its CONNECTION_CLOSE again (RFC 9000 section 10.2.1) within its 300 ms drain.
- Open: webtransport-go `closer client window 65536` (1 of 9): the Go peer saw the CONNECT stream's STOP_SENDING
  (WT_SESSION_GONE) before Cerid's WT_CLOSE_SESSION capsule, which the 64 KiB window held back. Next batch.
- Not this track's: linux-clang-tsan `test_fault_classification.cpp:929` (44723313119).

## What landed

- GCC's probing (interface version 6): libwebrtc's ProbeController, BitrateProber, ProbeBitrateEstimator and
  AlrDetector, as S74 describes; `probe`, `probe_alr`, `probe_initial`; `probe_bytes()`.
- QUIC: 1-RTT packets pad to their datagram while a probe wants bytes, only when the window and the pacer let data go;
  a probe wanting bytes arms the pacer's timer (`probe_fills`).
- Fuzzing: the switches in byte 1 and the probe's bounds; seeds `gcc-probe-alr`, `gcc-probe-initial` from the writer.

## Evidence

- win-debug (MSVC 14.51): all 943 `crd-net-tests` cases, `crd-math-tests` and `crd-hesap-net-tests` pass, every
  `crd-check-*`/`crd-no-*` guard passes (capability-owners on a rerun: this host's misexecution), win-relwithdebinfo
  links both; clang-tidy 20 is clean on the 12 changed sources (one local constant renamed).
- WSL linux-gcc-debug and linux-gcc-release (LTO): the 147 GCC, probe, congestion, QUIC connection, receive-timestamp,
  BBR, Prague and SCReAM cases pass; math and hesap-net pass. linux-gcc-public-checks: the picoquic (24 cases) and
  qmux crate (214 cases) interop runs pass.
- Fuzzing: linux-clang-fuzz found in 0.2 s a GCC swapped in at the 2^32 window, where a probe's room cannot add
  (the oracle's bound, not the controller); the oracle now allows it and the unit joins the corpus
  (`0f63ef8c...`). Then 300 s, 952288 executions, 6873 new units, no finding; ASan replays the corpus clean.
- Measured: 10 Mbit/s, 40 ms, 500 ms buffer: half the link in 0.19 s, 85% in 6.6 s (39.5 s and 46.7 s without
  probes), 18000 bytes of padding, nothing dropped. QUIC, 4 Mbit/s: 20 padded packets, estimate 3.8 Mbit/s against
  668 kbit/s without, and 3.3 Mbit/s while 1 Mbit/s comes back.

## Next

The webtransport-go close order; then migration, NAT rebinding and LEO handover.
