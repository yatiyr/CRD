# 2026-11-24: NET-PATH, a full sent-packet storage holds back no path validation

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path). Operator order: fuzz host-check 45296064704 (04440292) first. Its leg
net-quic-connection, seed 8403, input `afb01010`, failed the follow check `follows() || (ended() && !proving(true))`,
and it also failed on HEAD 4be4728e. CI: host-checks 45307860762 (fuzz, 1b6e5bc8) and 45299047960 (udp-ring) passed.
The udp-ring run was on an Azure host (kernel 7.0.0-1017-azure), and all 2,560 ring opens (640 per size) got every
buffer they asked for, 32768 included. CI 45317055228 (4be4728e) is pending. GitHub's main equals GitLab's (4be4728e).

## The defect

The client's moves had left its 72 sent-packet entries full of packets nobody acknowledged (`can_send` false; the
window was open at 2,889 bytes in flight). The server's challenges reached both of the client's sockets, so a
PATH_RESPONSE was owed on each, and its own validations owed challenges. None could be recorded, so nothing was sent.
The probe timeout that frees entries (`make_room`) was due at 5.83 s, but the validations expired at 4.46 s and
4.66 s unsent, and the last one closed the connection silently (`path_lost`). RFC 9000 section 8.2.2 lets only
congestion control delay a PATH_RESPONSE. The engine was wrong; the oracle was right.

## The fix

- `QuicRecovery::evict`/`can_evict`: in a full storage the oldest outstanding packet is declared lost for one entry,
  the way a probe timeout declares packets lost for its probes. The loss detection timer is then armed again.
- `QuicConnection::make_path_room` runs just before a packet of path frames is written, after the held-socket,
  credit, ID and key checks. It feeds the loss to the controller (`end_detection`) and counts `path_evictions`.
  `has_content` lets a full storage through only for a PATH_RESPONSE or due challenge on the active path, and
  `path_sendable` (the other paths and the path timer) uses `can_evict`. Every timer reported due is one send serves.
- OBSERVED_ADDRESS, data and PINGs still wait for room. The freed packet carries the owed PING (`put_answer`).

## Evidence

- `afb01010` replays clean. `-minimize_crash` on the unfixed binary gave `fb997001` (201 bytes), which fails the
  same oracle there and passes fixed. It joins `corpus-quic-connection`, and the 69-input corpus replays clean.
- New case "a storage full of unacknowledged packets holds back no PATH_RESPONSE or challenge": the raw client
  acknowledges nothing. Sixteen challenges are each answered at once, with one eviction per round past the reserve.
  Then a move to `b`: the challenges on `b` and on `a` (section 9.3.3) take one entry each, `b`'s is sent again on its
  timer with nothing arriving, the answer validates it, and there are no failures. Teeth: with `has_content` and
  `path_sendable` back on `can_send`, the case fails (the response is never seen).
- linux-gcc-debug: `crd-net-tests` 1180 cases (1172 passed, 8 skipped); the 30 net fuzz corpus CTests and the
  `crd-no-*`/`crd-check*` guards pass (49 of 49). LLVM 22.1.8 tidy is clean on the two engine sources and the test.
- libFuzzer (linux-clang-fuzz, fixed, seed 8403, two jobs, 541 s): 37,840 runs, no find.

## Next

The 65610837 regressions (a) to (d), starting with (a), crd-net's 64 KiB wall rate against 4fb61ff3's tree.
Then the input's storage: 33 PMTU probes of 1472 bytes left at one instant, one per validated own-socket move.
Each move restarts the search and empties bytes in flight, so the window gate on a probe cannot hold one back. A
case is needed to decide whether one probe per new path at one instant is right (RFC 8899 section 5).
