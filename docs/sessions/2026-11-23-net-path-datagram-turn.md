# 2026-11-23: NET-PATH, an I/O turn bounded in datagrams, and a paced ngtcp2 peer

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md). Design: the "Two seams" bullet of [Ingress](../design/network-foundation.md#ingress).
Follows [the challenge answer](2026-11-23-net-path-challenge-answer.md), whose next work was the ngtcp2 client's move
that ended back on its first socket.

## What landed

- **Engine (`IoLoop::poll`).** A turn read each socket with at most `batches_per_poll` receive calls. A coalescing
  socket (`UDP_GRO`) receives into `train_slots` (4) slots, and a peer that does not segment fills each with one
  datagram, so its turn fell from 128 datagrams to 16. The bound is now in datagrams: each socket is read until it has
  handed on `batch * batches_per_poll`, a train counted by its datagrams (the receive reaching the bound may end
  inside a train). A plain socket's turn is unchanged.
- **Test peer (`ngtcp2_quic_peer.c`).** It called `ngtcp2_conn_update_pkt_tx_time` only when the window blocked, so
  ngtcp2 never paced and a write round sent the whole window at once. A round now sends one send quantum, as ngtcp2's
  own examples do, and the pacer's time inside `ngtcp2_conn_get_expiry2()` wakes the next.
- `quic_interop.py`: the path check's failure text carries both ends' stderr.

## Evidence

- The cause: in the failing runs the ngtcp2 client sent both 1200-byte PATH_CHALLENGEs from its second socket and the
  Cerid server never read a datagram from that port. ngtcp2 retries a challenge after about an initial PTO (1 s);
  the client closed 35 ms after the move. Per-socket drops (`/proc/net/udp`), 4x512 KiB echo, linux-gcc-public-checks,
  three runs each: ngtcp2 to ngtcp2 server 79-338 before the pacing fix, 0 after; ngtcp2 client to Cerid server
  64-162 before either fix, 68-162 with pacing alone, 0-12 with the turn fix alone, 0 with both. A Cerid client
  (segmented sends) caused none throughout.
- The two plain "ngtcp2 client ... moves at 524288" cases: 10 of 80 runs failed before, plus one "ngtcp2 did not
  validate ECN" in about 150 (the same drops hitting ngtcp2's ECN test on the new path); 0 of 600 after.
- New [io-loop] case: single datagrams on a coalescing socket fill the turn's 32 as plain slots would; a train counts
  each of its datagrams. With the old turn it fails six checks.
- linux-gcc-public-checks: the ten net interop CTests pass, `crd-net-quic-interop-ngtcp2` and
  `crd-net-http3-interop-nghttp3` twice more; the `crd-net-*` CTests, the `crd-no-*`/`crd-check*` guards and the net
  fuzz corpora (56) pass. LLVM 22 tidy is clean on `io_loop.cpp` and `test_io_loop.cpp`.

## Next

- picoquic multipath past the first path.
