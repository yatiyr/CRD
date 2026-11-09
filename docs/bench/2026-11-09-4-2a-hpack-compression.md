# 2026-11-09 — 4.2a: HPACK compression against the hpack-test-case encoders

<!-- doc-role: evidence -->
> Dated evidence; counts and results are historical. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).
> Owner row: [4.2a](../ROADMAP.md#slice-4.2a); session: [2026-11-09](../sessions/2026-11-09-4-2a-hpack.md); design: [S62](../design/network-foundation.md#hpack).

- **Measure:** total bytes of the field blocks over the corpus's stories (pinned commit 8a1406e7), each story one compression context at the default 4096-byte table.
- **Harness:** `crd-hpack-replay encode` runs crd-net's encoder over the raw stories and proves every block through crd-net's decoder; `hpack_interop.py` sets it beside each corpus output over the same stories (`crd-net-hpack-interop-corpus`).

| crd-net / peer | never-indexed cookies (shipped) | cookies indexed (experiment) |
|---|---|---|
| nghttp2 | 1.0111 | 0.9953 |
| node-http2-hpack | 1.0111 | 0.9953 |
| python-hpack | 1.0085 | 0.9927 |
| swift-nio-hpack (Huffman) | 1.0085 | 0.9927 |
| haskell-http2-linear-huffman | 0.9895 | 0.9740 |
| swift-nio-hpack (plain text) | 0.8000 | |
| haskell-http2-linear (no Huffman) | 0.7864 | |
| haskell-http2-static-huffman | 0.4847 | |
| haskell-http2-static | 0.3834 | |
| go-hpack | 0.3666 | |
| haskell-http2-naive-huffman | 0.3666 | |
| haskell-http2-naive | 0.2844 | |

Below 1 means crd-net's output is smaller. The total is 364,316 bytes shipped, 358,616 with cookies indexed.

- **Result.** Within 1.1% of the best encoders, and 0.5 to 0.7% smaller than them on equal terms.
- **Gap, explained.** All of it is the security rule: `cookie` and `set-cookie` never enter the table (RFC 7541 section 7.1: no attacker-driven request can measure them through it), and the corpus's stories hold 54,868 bytes of cookie text out of 1,162,372. Dropping the rule for one build took the ratio against nghttp2 from 1.0111 to 0.9953; it was not kept.
- **What moved it:** leaving the fields that rarely repeat out of the table (`:path`, `age`, `content-length`, `etag`, `if-modified-since`, `if-none-match`, `location`) took the ratio against nghttp2 from 1.0249 to 1.0111.
