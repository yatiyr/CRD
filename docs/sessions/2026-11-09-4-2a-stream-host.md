# 2026-11-09 — 4.2a: the stream path's host behind a poller

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [4.2a](../ROADMAP.md#slice-4.2a). Design: [S53](../design/network-foundation.md#stream-host).

CI first: nothing from this track failed. Host check 42543967150 (the `net-qmux` legs) is still queued; its read stays owed.

## What landed

- `StreamPoller`, the set's seam, and `TcpPoller` (epoll on Linux, a dense `WSAPoll` or `poll` set elsewhere), level-triggered.
- `QmuxHost`: accept and connect behind one thread with a fixed pool, `QuicHandle` generations, service once per turn, fair shares
  per turn, and every end reported with its `SocketError`.
- `DeadlineHeap` taken out of `QuicHost`, so both hosts have one heap; `host_handle.hpp` holds the handle both use.
- `TlsSession::outgoing` and `QmuxConnection::outgoing`: output shown in place and only what the socket took removed, so a partial write
  needs no staging buffer per connection.
- Decisions recorded in S53: accepted peers are charged as validated; the handshake timeout covers the TCP connection; a reset ends
  only what could not finish (unsent bytes, a handshake that timed out, a failed transport).

## Evidence

- `crd-net-tests` `[qmux-host]`, `[poller]`, `[deadline-heap]`: 44 cases on linux-gcc-debug; on win-debug too (`WSAPoll`), with `[qmux]`, `[tcp]`.
  Over pipes: every refusal and close path, back-pressure at 512-byte sockets with no busy turn, turn shares, deadlines, no allocation in an
  established turn. Over real TCP: 8 clients, 512 KiB each way, 16 KiB socket buffers.
- 42 source mutations killed; two gaps they found (a partial write's extra `write`, a report before a connection stands) now have tests, one
  redundant `set_interest` removed. Two survivors are equivalent or Linux-only: a zero `ready_per_turn` (no array of none can be allocated), and a
  closed listener (`epoll_ctl` refuses it, the poll set needs the check).
- `crd-fuzz-net-qmux-host`: corpus of 5 seeds; three libFuzzer runs on linux-clang-fuzz, 28,863 executions, clean. Hosted legs (seeds 8911, 8912) added to Host check.

## Next

The stream path's 0-RTT and interop against the Rust `qmux` crate; then the raw UDP path of ADR-0035 and the browser endpoints.
