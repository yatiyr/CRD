# NET.2: anti-replay windows and the single-use filter

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Date: 2026-11-02. Row: [NET.2](../ROADMAP.md#slice-net.2) (track C, batch 47; the
[parallel tracks](2026-10-31-parallel-tracks.md)). Contract: [anti-replay, S16](../design/network-foundation.md#anti-replay).

## The user's decisions this batch acts on

- 2026-11-01, evening: "I WANT NETWORK PARTS TO BE COMPLETELY DONE AND TESTED! I WANT FULL AND SECURE AND GOLD
  STANDARD NETWORK MODULE!" This batch takes the first half of NET.2's first Remaining clause, the anti-replay
  windows; 0-RTT is the next slice.
- 2026-11-01, night: "DO EVERYTHING TO REDUCE THE COSTS ... SAVE MY MONEY!" Everything ran on this PC: the track's
  WSL lane (`~/cerid-src-track-c`, `~/cerid-track-c-build`) and the win-debug profile in the worktree. No cloud machine
  ran (0 hours, 0 USD).

## CI first

- **Push run 41358455047 (33ac38d4), linux-gcc-shipping**: GCC 13's link-time `-Wnull-dereference` in
  `QuicPacketKey::update` (quic.cpp), which dereferenced `parameters_of()` without a check. A ready key's version is
  always one `init` accepted, so the branch is unreachable; the lookup is now checked and an unknown version is
  `Unsupported` (b5160de6, pushed first). Reproduced on the track's new linux-gcc-shipping tree: the old source fails
  the `crd-crypto-tests` link with the same diagnostic, the fixed one links and its 96 QUIC, TLS and capability cases
  pass. No other `call()->member` dereference is left in crd-crypto's or crd-net's sources and headers.
- **Complete-tier run 41342960677 (59a3347e)**, still running: its new failures are all fixed on main already.
  win-shipping and win-shipping-profile fail `crd-check-isa-isolation` (AWS-LC's `ghash-x86_64.asm` and
  `aesni-gcm-x86_64.asm` in MSVC link-time images) and `crd-check-compile-commands` (jitterentropy's `/Od`), both
  fixed by b37a4177, whose guard is `if(MSVC)` (cl.exe and clang-cl) and whose map judge names the same `crypto:`
  members; linux-gcc-debug's ECDSA certificate compression case is fixed by 6e5dbb36; win-tidy's
  `test_fft_kernels.cpp` finding by d178d59b.
- Push run 41368354840 (43a07273) is pending.

## What landed

`replay.hpp` holds both anti-replay records, one owner for every channel (decision S16):

- `ReplayWindow<Bits>`: the largest accepted number and RFC 6479's ring of words. The `Bits - 64` numbers ending at
  the largest are exact; older ones are Stale. `check` records nothing, `accept` records a Fresh number only; raising
  the largest clears only the words passed (at most the ring). `QuicPacketWindow` is 256 bits (48 bytes, span 192) per
  packet number space; the QUIC receive path is unprotect, decode against the window, check, open, accept.
- `ReplayFilter`: the NEW_TOKEN filter, generalized unchanged in its algorithm (identifiers instead of token salts,
  `kReplayIdBytes` 16). Its period's minimum moved to its user: `QuicTokenKeys::validate` refuses a filter shorter
  than the NEW_TOKEN lifetime or `kQuicTokenClockSkew` (`NotReady`), so short-lived tokens can no longer pass the skew.

## Evidence (this PC)

- linux-gcc-debug: `crd-crypto-tests "[replay]"` 6 cases, 481,584 assertions; all 145 crypto CTests (the suite and
  the five fuzz corpora); `crd-fuzz-crypto-quic --seed` writes 26 seeds (three new window seeds, the 23 old ones
  byte-identical) and replays them clean.
- win-debug (MSVC): `crd-crypto-tests "[replay],[quic],[quic-token]"` 26 cases, 670,889 assertions, no warning; the
  QUIC corpus replays clean.
- `tidy-files.py` (LLVM 20.1.8) clean on every changed C++ file after two local constants were renamed.
- The CTest guards on a fresh linux-gcc-debug configure: 18 of 19 pass (include budget, source routes, capability
  owners, peer headers, the container and allocator bans among them); `crd-check-diag-runbooks` fails only because
  this tree builds the crypto targets alone, so the runbooks' tests of other modules are not listed in it.
- Five source mutations, each caught: the Stale bound off by one, passed words left set, a Duplicate accepted, the
  clearing one word short (all four by the unit tests and by the fuzz corpus), and the filter period not covering the
  skew (by `test_quic_token.cpp`).

## Next

NET.2's Remaining continues with 0-RTT over TLS and QUIC: single-use tickets through `ReplayFilter` with the ticket's
authenticated issue time, AWS-LC's 60-second freshness check, early bytes delivered only marked replayable, a client's
rejection and resend, and the QUIC early level with its transport-parameter context.
