# 2026-11-24: NET-PATH, one send key and tokens of their size

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path). Design: the Key phases bullet of
[packet protection](../design/network-foundation.md). Follows [the controller storage](2026-11-24-net-path-controller-storage.md).
Ran on this PC (WSL2, Windows for clang-tidy).

**CI first:** CI 45466383581 (6e4b4728) failed the GCC LTO link of `crd-net-tests` on linux-gcc-release,
-relwithdebinfo and -shipping: `-Werror=null-dereference` in `set_congestion`'s refusal unwind (01f2610f), which
dereferenced `control_at()` without the first loop's null check. It now skips slots with no control. Reproduced here
without the fix (GCC 13.3, the same error), clean with it. The clang, clang-cl and ARM64 reds there are the `min_u32`
that 80411130 already removed.

**Follow-up 2 (1 MiB bulk):** two interleaved 7-round runs against 8bdfea79: medians 580 against 581 and 567 against
568 MiB per CPU-second, wall 883 against 912 and 887 against 882. Equal; the wall gap flips between runs.

## What landed

- `QuicPacketKey::update` may take the key itself and advances it in place. `QuicPacketProtection` keeps one 1-RTT
  send key instead of two. If the provider fails that step, no key is left and the connection closes with
  INTERNAL_ERROR (`QuicPacketError::Provider`), whether the update was this side's or an answer to the peer's.
- Tokens take storage of exactly their size instead of two 256-byte arrays:
  - a client's Initial token (configured, or a Retry's) goes when its Initial keys go;
  - a Retry whose token is refused storage is dropped, and the next Retry is taken;
  - a refused NEW_TOKEN keeps nothing (client), and `give_new_token` returns false (server);
  - `buffer_allocations()` counts the tokens.
- The three receive keys stay. RFC 9001 section 6.3 allows two, but only with a hold of one PTO instead of three.

## Evidence

- Tests: the new `[memory]` token case; in-place updates match RFC 9001's "ku" vector and a separate update.
  Tooth: keeping the Initial token fails the live-block check (5 == 4).
- Runs:
  - linux-gcc-debug: 1,202 `crd-net-tests` cases and 15 crypto `[quic]` cases;
  - release (LTO): 1,202 cases;
  - ASan: 254 connection, packet, memory and recovery cases, and crypto `[quic]`.
- libFuzzer: net-quic-packet 525,757 runs, crypto-quic 4,684,475, net-quic-connection 7,740, no artifact; the three
  corpora replay.
- Interop with ngtcp2, nghttp3, picoquic and msquic passes. LLVM 22.1.8 clang-tidy is clean on the six sources touched.
- Sizes: `QuicConnection` is 15,200 bytes (was 16,384), `QuicPacketProtection` 3,872 (was 4,560).
- Held memory after each connection moved 4 KiB, 5 rounds against 8bdfea79's engine:
  - server 28.2 to 28.6 KiB (median 28.3) against 32.3 to 32.6;
  - client 28.7 to 29.0 against 32.7 to 33.0;
  - msquic's server end 38.0 to 68.3 in the same rounds.
- Board at this engine against 8bdfea79's, 5 interleaved rounds, medians: bulk 578 against 556 MiB per CPU-second at
  1 MiB windows and 424 against 423 at 64 KiB; handshakes 4,101 against 4,124 per CPU-second (0.6% lower, inside the
  2% rule). `quic_board.py --check` passes; the 19 guard CTests pass on linux-gcc-debug.
- Not proven here: the `Provider` close (no provider failure can be injected), MSVC (not built locally; clang-tidy ran
  on its compile database).

## Next

1. CI: 45471447616 (4ae23931) and the first run containing this batch's commits: MSVC, clang-cl, ARM64, TSan and the
   GCC LTO lanes. Also fuzz host-check 45471477574.
2. A held server end: the TLS state's 1.3 KiB copy of its context's settings (ticket keys included). Also the four
   transport parameter copies (1.5 KiB) and the 0.8 KiB stored configuration.
