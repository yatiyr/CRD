# 2026-11-09 — 4.2a: QPACK compression against six encoders on the qifs corpus

<!-- doc-role: evidence -->
> Dated evidence; counts and results are historical. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).
> Owner row: [4.2a](../ROADMAP.md#slice-4.2a); session: [2026-11-09](../sessions/2026-11-09-4-2a-qpack.md); design: [S56](../design/network-foundation.md#qpack).

- **Measure:** total bytes on the wire (field sections plus encoder stream, the corpus's framing excluded) over each QIF file, at the corpus's parameters: table capacity 0, 256, 512 and 4096, blocked streams 0 and 100, acknowledgements immediate (1) or never (0). Sizes are deterministic; the machine does not matter.
- **Harness:** `crd-qpack-replay encode` runs crd-net's encoder and proves every section through crd-net's decoder; `qpack_interop.py` compares it with each corpus output, over exactly the runs that encoder has, and first decodes all 528 outputs back to their QIF text. Pinned corpus: `qifs` (da52cd93), `encoded/qpack-06`. Commit: this slice.

| crd-net / peer | all runs | capacity 256 | capacity 512 | capacity 4096 | blocked 0, ack 1 | blocked 100, ack 1 | blocked 100, ack 0 |
|---|---|---|---|---|---|---|---|
| ls-qpack (96 runs) | 1.003 | 0.973 | 0.980 | 1.091 | 0.991 | 1.019 | 1.002 |
| f5 (72) | 0.958 | 0.929 | 0.910 | 1.081 | 0.769 | 0.869 | 1.266 |
| proxygen (72) | 0.951 | 0.970 | 0.937 | 0.943 | 0.811 | 1.003 | 0.997 |
| qthingey (96) | 0.969 | 0.960 | 0.934 | 0.983 | 0.897 | 0.997 | 0.977 |
| quinn (96) | 0.939 | 0.968 | 0.937 | 0.827 | 0.801 | 0.805 | 1.193 |
| nghttp3 (96) | 0.892 | 0.799 | 0.877 | 0.918 | 0.834 | 1.021 | 1.007 |

Below 1 means crd-net's output is smaller.

- **Result.** Smaller than five of the six overall, and 0.3% above ls-qpack, the best, which it beats at table sizes 256 and 512.
- **Losses, explained.** At 4096 bytes ls-qpack and f5 lead by 8 to 9% (their heuristics were not analysed). crd-net's are three small rules (a 64-slot second-sight filter, static and dynamic names reused, a duplicate for the oldest quarter), and two widenings tried here and measured made things worse (inserting at first sight: +1.4%; a 512-slot filter: +1.0%): more entries displace better ones. f5 and quinn at `blocked 100, ack 0` lead by 19 to 27%; with no acknowledgement ever arriving, RFC 9204 section 2.1.1 forbids crd-net from evicting anything, so its table stops once full; whether those two encoders evicted unacknowledged entries in that mode was not inspected. The mode does not occur on a live connection, where acknowledgements arrive.
- **What moved it:** the duplicate of fields about to fall out took the overall ratio against ls-qpack from 1.0065 to 1.0028 (1.0006 before the guard that keeps the original for a section that may not block: that version left a section referencing an entry it had just evicted).
