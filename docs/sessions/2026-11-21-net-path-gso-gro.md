# 2026-11-21: NET-PATH, Linux UDP segmentation and coalescing

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path) (Partial). Design: [S33](../design/network-foundation.md#sockets).

## CI first

Nothing of this track failed. Run 44887080529 (70f94328) had not started; in 44861565940 win-release fails only
`crd-hesap-tensor-kernels-at-avx512-sde-skx` and `-spr` (MSVC 14.52, not net), as win-shipping-profile does.

## What landed

- `segment_train` (udp.hpp): the train rule, a first datagram whose next is shorter going alone so a PMTU probe
  never leads one. `UdpSocket::send` on Linux hands each train to `sendmmsg` as one `UDP_SEGMENT` message, one iovec
  per datagram where it lies; a refused train goes again a datagram a message, EIO also turns segmentation off, and
  ENOBUFS loses a train whole. `UdpOptions::segment_send` (on) and `coalesce_receive` (off) choose; `features()`
  reports `segmented_send` and `coalesced_receive`, `offload_counters()` what they did.
- `UDP_GRO` receive: a train in a `kMaxTrainBytes` slot with `ReceivedDatagram::segment`. `IoLoop` reads a
  coalescing socket into `train_slots` and hands each datagram on in place (no copy), truncating as a plain slot
  would, a lone datagram in a train slot included.
- The interop peer opts into coalesced receive.

## Evidence

- WSL linux-gcc-debug: the train model test (20,000 random runs against a direct reading of the rule); loopback
  bursts over IPv4, IPv6 and dual-stack, segmenting on and off, arriving identical (5 trains, 109 of 112 datagrams);
  GSO into a GRO socket arriving as exactly those 5 trains; the IoLoop train test (in place, truncation, mixed set,
  allocation refusals, a lone datagram past `slot_bytes`). A real-UDP QUIC client and server: both hosts' bursts
  leave as trains and each loop splits exactly the trains its socket joined. `[.netns]` under `unshare -Urn`: EIO
  with transmit checksum offload off on the near link (both families; segmentation off, marking kept, all 10
  datagrams arrive), a train past the link MTU refused as each datagram would be, a train across the 1280-byte hop
  lost whole with one report and nothing fragmented, a full queue (pfifo limit 0) losing two trains whole and
  counting them once. The whole crd-net-tests (1014 cases), the 32
  net corpus and drift CTests, and both interop suites (ngtcp2, picoquic) on linux-gcc-public-checks pass;
  linux-gcc-asan passes `[udp],[io-loop],[quic-client]` and `[.netns]`.
- Windows win-debug: crd-net-tests builds and `[udp],[io-loop],[quic-client]` pass (offloads reported off).
- clang-tidy 22.1.8 (`tidy-files.py`): udp.cpp, io_loop.cpp and the three test files clean.
- Teeth, each caught: no train counted on ENOBUFS (full-queue test), no plain retry of a refused train (EIO and MTU
  tests), a probe leading a train (model and burst tests), a cut last datagram left whole (IoLoop test).

## Next

Windows USO (`UDP_SEND_MSG_SIZE`) and URO (`UDP_RECV_MAX_COALESCED_SIZE`) behind the same seam, then io_uring and RIO.

## CI follow-up (2026-11-21, 6ce80283)

Run 44913028609: since Linux 6.11 the host segments past a device without checksum offload in software (v6.10 and
v6.11 `udp_send_skb`), so the `[.netns]` EIO case now sends through an IPsec route (`RouterPath::ipsec`), which still
returns EIO in mainline; passes on WSL 6.6, hosted proof pending. win-tidy: two unused usings moved under their guard.
