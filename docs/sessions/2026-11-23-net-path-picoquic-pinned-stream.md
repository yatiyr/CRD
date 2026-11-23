# 2026-11-23: NET-PATH, a stream pinned to path ID 1 against picoquic

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path). Follow-up of
[the storage-window note](2026-11-23-net-path-storage-window.md): the picoquic multipath check asked only for some
bytes on path ID 1, which a validation alone would meet.

## What landed

- **`--pin-added 1` on crd-quic-peer** (with `--add-path-at`): the client holds its last stream back until add_path()
  answers. It then pins it with `QuicStreamSet::pin` before its first byte: to the path ID it opened, or to path ID 0
  if it answered otherwise, so a non-negotiated case still completes. The report adds `pinned` (pin() took it) and
  `pin_kept` (still pinned on every turn until every byte and the FIN were acknowledged; a stopped path unpins).
- **The echo harness** (`interop_echo.hpp`) gains `EchoOptions::pin_last` and `EchoTurn::pin_path` (`kPinWait` until
  known). It is off by default, so crd-qmux-peer and the other interop scripts run as before.
- **`picoquic_interop.py`**: each Cerid client offering multipath passes `--pin-added 1`. With both ends offering,
  the check needs `pinned=1`, `pin_kept=1` and picoquic's `other_path_received` of at least the stream's 262,144
  bytes.

## Evidence

- `picoquic_interop.py --only multipath`: 12 of 12. Over six repeated both-offer runs (v1 and v2), picoquic read
  273,941 to 331,926 bytes on path ID 1. All of them had `pinned=1` and `pin_kept=1`.
- Mutant: the pin() call skipped while it still reports success. `pin_kept` is 0 in all six runs, and the check
  fails. With the stream held but unpinned, picoquic read 15,360 to 216,099 bytes on path ID 1, all under the
  stream's size, so the picoquic-side bound also tells the two apart. Restored.
- linux-gcc-public-checks: `crd-net-quic-interop-ngtcp2` (259 s), `-picoquic` (149 s, twice) and
  `crd-net-qmux-interop-crate` (206 s) pass, the last two on the final source.
- clang-format is clean on the added lines; the repository validators pass.

## Next

- CI first: host-check 45194420211 and the first finished CI run containing this batch.
- A picoquic client's own path (`picoquic_probe_new_path_ex` from a second local address), so that Cerid's server
  admits a peer-opened path ID. Then abandoning a path (`picoquic_abandon_path` and Cerid's abandon) and PATH_STATUS
  both ways (`picoquic_set_path_status`).
