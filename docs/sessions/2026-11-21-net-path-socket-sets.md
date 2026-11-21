# 2026-11-21 — QUIC paths: socket sets and the client host's second socket

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path). Design: [S33](../design/network-foundation.md#sockets) "Socket sets"
and [S75](../design/network-foundation.md#migration) "The client host's sockets".

## CI first

Run 44854356220 (dispatched on d17003bd): both Repository checks, win-vs and linux-gcc-arm64-debug failed only on
`docs/generated/dependency-licenses.md`'s recorded bytes, which b0882ee0 already records (`check-generated.py`
passes at HEAD). Its other jobs were still running.

## What landed

- `SocketSet` (udp.hpp): up to four sockets a loop and its host share; a single socket converts to a set of one.
  The set waits as one (`wait_handle()`, one `poll`/`WSAPoll`, `waited()` passing Linux's error flag back; a set
  holding a simulated socket waits a 1 ms slice). `ReceivedDatagram::socket` names the arrival socket.
- `IoLoop` reads every socket each turn within `batches_per_poll`; a stateless answer leaves by its arrival socket.
- `SendQueue` names each datagram's socket and hands each socket its own in order: a busy socket holds back no
  other. Slots stay put; an order array of slot indices is compacted. A socket the set lacks drops and counts.
- `QuicHost`/`QuicServer`/`QuicClient` take the set; the host routes by path socket, gates ECN and PMTU discovery on
  every socket, and matches a server's held 0-RTT by socket. `QuicClient::probe_path()`/`migrate()` (refusals
  `NoConnection`, `NoSocket`).
- Defect found and fixed: a client whose first network vanished at the move was never followed. The server answers
  a new path only with an unused client ID (RFC 9000 section 9.5), and the NEW_CONNECTION_ID carrying it was in
  flight on the dead path; the validation then failed back onto that path. `migrate()` now announces every ID the
  server has not acknowledged again from the new path.
- `path_too_big(path, ...)`: a report moves the search only from the active path and about a packet sent there
  since the path state last started.

## Evidence

- win-debug (MSVC 14.51): all 985 `crd-net-tests` cases (13 new; 776 under `[net]`).
- Teeth: without the re-announcement the gone-network case fails (the server never follows, the client idles out);
  without the path check in `path_too_big` four checks of `test_quic_pmtu.cpp` fail.
- clang-tidy 22.1.8 (`tidy-files.py`) clean on the 16 changed sources and headers; Allman, 120 columns.
- WSL linux-gcc-debug (GCC 13, -Werror): `crd-net-tests` built; 790 `[net]` cases pass.
- linux-clang-fuzz `crd-fuzz-net-quic-connection` (its own-socket moves, ops 0xFA and 0xF9, now re-announcing IDs):
  481 s, 62355 runs, nothing found (scratch corpus; the committed corpus unchanged).

## Next

preferred_address (section 9.6) on a server's second socket; interop with ngtcp2's migrating client and server
through `QuicClient::migrate()` in `cerid_quic_peer`; LEO handover awareness.
