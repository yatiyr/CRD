# 2026-11-22 — NET-PATH: path ID 0 abandoned while others live

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path). Design: S78's Lifecycle bullet in
[network-foundation.md](../design/network-foundation.md). The PC had shut down after batch 12; the tree was clean at
415124ad, level with origin/main, so nothing was lost.

## CI first

CI 45013880738 (e446680a, still running on its Windows legs): the GCC LTO links, the ASan `[cids]` case and its
arm64-asan twin are the defects 905fa518 and batch 11 fixed. Its linux-clang-tsan red is the renderer's REN-8 timing
gate (236.7 ms against 200 ms on lavapipe under TSan), outside this row. Fuzz host-checks 45021997098 and 45017650858
are still queued.

## What landed

- RFC 10069 section 3.4 and Figure 5: `abandon_path(0)` and the peer's PATH_ABANDON of path ID 0 stop its slot as any
  other once another path sends; the frames, the answer and the ACK frames for its stragglers go on another path.
- `lead_slot()`: the handshake slot while it sends, else the lowest slot still sending. `path()`, `paths()`,
  `path_control()`, the handover setters, the close and its answers, datagrams no slot claims, the PMTU report check
  and the validation timeout follow it. The handshake slot keeps its 4-tuples and is never reclaimed (Remaining).
- The last path still sending: `abandon_path()` answers `LastPath` (replacing `HandshakePath`), the peer's PATH_ABANDON
  of it closes with NO_ERROR, and its failed validation discards the state silently; the handshake slot's failure with
  no fallback now abandons path ID 0 while others send. `probe_path()`/`migrate()` answer `Abandoned` once path ID 0 is
  gone, and the preferred address is no longer followed.
- Defects found on the way: `has_content` asked slot 0's recovery whether any slot could send, and stream frames took
  slot 0's RTT; both now use the sending slot. `QuicRecovery::stop_path` stops slot 0 once the handshake spaces are
  gone and never the last slot sending (recovery fuzz model updated).

## Evidence

- linux-gcc-debug (WSL): `crd-net-tests` 1111 cases (1104 passed, 7 skipped); four new `[multipath]` cases: either end
  abandons path ID 0 under 40% loss, 30% duplication and jitter over four seeds (data finishes on path ID 1, both close
  path ID 0, the close crosses on path ID 1); both ends abandoning each other's last path close with NO_ERROR; Figure 5
  with the frame sent on path ID 0 itself; the last path's failed validation ends silently. Guards and the recovery,
  cids and connection corpora pass.
- Teeth: the close on slot 0 crashes the pair case; straggler ACKs from slot 1 only fail Figure 5's ACK checks; no
  silent close for the last path crashes its case.
- linux-clang-fuzz: quic-recovery seed 8901, 180 s, 80804 runs; quic-connection seed 8902, 240 s, 22693 runs; clean.

## Next

NET-PATH: MAX_PATH_ID raised as path IDs close, with the handshake slot reclaimed; concurrent validations paced.
