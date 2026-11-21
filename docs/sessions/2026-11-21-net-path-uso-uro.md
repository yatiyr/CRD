# 2026-11-21: NET-PATH, Windows UDP segmentation and coalescing (USO, URO)

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path) (Partial). Design: [S33](../design/network-foundation.md#sockets).

## CI first

Nothing of this track failed: run 44887080529 (70f94328) was still running with no failed job, and 44889979894
(c512fdcc) had not started.

## What landed

- Windows USO: a socket segments where `UDP_SEND_MSG_SIZE` reads back (never set on the socket, which would segment
  PMTU probes too). Each train (`segment_train`) is one `WSASendMsg` gathering its datagrams in place, one `WSABUF`
  each, with a `UDP_SEND_MSG_SIZE` control message beside the codepoint's. A refused train goes again a datagram a
  call; `WSAEOPNOTSUPP`, or `WSAEINVAL` for an unmarked train, turns segmentation off for the socket.
- Windows URO: `coalesce_receive` sets `UDP_RECV_MAX_COALESCED_SIZE` to the IPv6 UDP payload; `UDP_COALESCED_INFO`
  is read by the control reader Linux's `UDP_GRO` now shares (`read_control`), and trains are counted the same way.
- One probe (`segments_trains`) and one train-counting helper serve both hosts.

## Evidence

- win-debug: `[udp],[io-loop],[quic-client]` pass (60 cases). Bursts over IPv4, IPv6 and dual-stack leave in
  exactly `segment_train`'s 5 trains and arrive identical; the QUIC hosts over real UDP send trains both ways.
- New, both hosts: a CE train (Windows refuses CE: refused once, then sent a datagram a call, unmarked, segmenting and
  marking kept; Linux sends it as one train), and a train of 9500-byte datagrams toward TEST-NET-1 refused as each
  datagram would be (MessageTooLarge, one refusal, segmenting kept). WSL linux-gcc-debug passes the same 60 cases.
- Teeth on Windows, each caught: no refused-train branch, any refusal turning segmentation off, no segment-size
  control message (the burst arrives as one datagram).
- clang-tidy 22.1.8 (`tidy-files.py`, win-tidy): udp.cpp, test_udp.cpp and test_quic_client.cpp clean.
- Not reachable on this PC: URO coalescing. Windows' loopback hands even a segmented send on a datagram at a time
  (12 runs, 0 trains), and a WSL `UDP_SEGMENT` sender through the Hyper-V switch arrived as 40 single datagrams
  with the option set. The parse is the shared `read_control` that Linux GRO proves; a real coalesced train needs a
  peer across a physical NIC with UDP receive offload.

## Next

io_uring (with zero-copy receive where the NIC splits headers) and RIO behind the same seam.
