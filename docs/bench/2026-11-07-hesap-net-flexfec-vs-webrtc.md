# 2026-11-07 — HESAP-NET: FlexFEC and RED against libwebrtc

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [HESAP-NET](../ROADMAP.md#slice-hesap-net). Session: [FlexFEC and RED against libwebrtc](../sessions/2026-11-07-hesap-net-flexfec-webrtc.md).

## Machine and arms

Intel Core i9-14900K, WSL2 Ubuntu 24.04, one thread, GCC 13.3.0, the `linux-gcc-release` tree with
`-DCRD_BUILD_ESTIMATORS_VS_WEBRTC=ON`. libwebrtc is branch-heads/8147 (84b14960): `forward_error_correction.cc`, the RFC 8627
reader and writer and NetEq's `red_payload_splitter.cc` verbatim with only their plumbing shimmed, without link-time
optimisation. hesap runs on AVX2 with GFNI, the host's level. `crd-hesap-flexfec-board --rounds 25`, run twice: the best of 25
interleaved rounds, on 24 RTP packets of 1200 bytes and libwebrtc's random packet masks at a 25% rate. The board fails if the
two sides' outputs differ.

## Results

The first run; the second agreed within 1.5% on every row.

| Operation | hesap ns | libwebrtc ns | libwebrtc / hesap | hesap heap | libwebrtc heap, per operation |
|---|---|---|---|---|---|
| read one header | 6.9 | 11.6 | 1.67 | none | none |
| write six repairs | 666.6 | 1199.0 | 1.80 | none | 6 blocks, 144 bytes |
| one repair over 24 packets | 421.8 | 662.0 | 1.57 | none | none |
| recover three of 24 | 1217.8 | 4385.0 | 3.60 | none | 204 blocks, 41,772 bytes |
| split RED, two redundant blocks | 4.4 | 115.2 | 26.21 | none | 11 blocks, 1,401 bytes |

## Reading

Each read-header figure includes copying 32 bytes in first, because libwebrtc's reader rewrites the header in place, and reading
the masks' bits afterwards, so link-time optimisation cannot drop hesap's work (the first version of this arm read 1.0 ns that way).

The first board found a real gap: `flexfec_parse` took 141 ns against 4.5 ns, from one loop per mask bit, a zeroed header of 15
blocks and a copy of the whole of it. It now reads the mask as one bit string and keeps only the blocks the packet names.

The two write rows are the dispatched XOR against a byte loop GCC vectorises. EncodeFec's six repairs also build their masks from
its tables, which hesap's arm does not do; the one-repair row has no table lookup.

The two rows with the large ratios are mostly copies. libwebrtc's interface takes each received packet as an allocated, copied
`Packet`, as its FlexfecReceiver does, where hesap's window reads the receive buffers in place; that was not measured separately.
RED is the extreme case: hesap's blocks are views of the payload and NetEq's are copies in a list.
