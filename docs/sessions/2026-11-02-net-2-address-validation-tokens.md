# NET.2: QUIC address-validation tokens

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Date: 2026-11-02. Row: [NET.2](../ROADMAP.md#slice-net.2) (track C, batch 43; the
[parallel tracks](2026-10-31-parallel-tracks.md)). Contract:
[address validation tokens](../design/network-foundation.md#address-validation), decision S13.

## The user's decisions this batch acts on

- 2026-11-01, about 01:25: "approve the downloads we need to do everything properly, I will contect a lawyer later!
  DO NOT BLOCK ANYTHING CONTACTING A LAWYER IS ... HARD! I WANT NETWORK PART TO BE COMPLETE AND PROPERLY DONE AND GOLD
  STANDARD AND ELITE". RFC 9000 was fetched from the RFC Editor for this slice (below).
- 2026-11-01, about 14:50: "I WANT NET DONE PROPERLY AND GOLD STANDARD AND WITHOUT ANY DEFERS!"
- 2026-11-01, evening: "I WANT NETWORK PARTS TO BE COMPLETELY DONE AND TESTED! I WANT FULL AND SECURE AND GOLD
  STANDARD NETWORK MODULE!" This batch takes NET.2's first Remaining clause: the address-validation tokens.
- 2026-11-01, night: "DO EVERYTHING TO REDUCE THE COSTS ... SAVE MY MONEY!" Everything ran on this PC: the track's
  WSL lane (`~/cerid-src-track-c`, `~/cerid-track-c-build`) and a win-debug tree in the worktree. No cloud machine ran
  (0 hours, 0 USD).

## CI first

Nothing from this track to fix. Push run 41333397427 (59f64219) finished with 13 failed jobs, each already fixed on
main: tls.cpp's `_Find_vectorized` static assertion on clang-cl (59a3347e), test_tls.cpp C4389 on win-asan,
win-debug-scalar and win-public-checks (9ef40e7e), `-Wdouble-promotion` in test_ckir_device_check.cpp on both clang
ARM64 lanes (3cf07ddc), `crd-module-selection` and the module registry contracts expecting the aws-lc and wycheproof
packages (47474b0b) and `crd-ci-platform` (df0d359f). Push runs since then were cancelled before they started; push run
41348210126 (d6b18176) and complete-tier run 41342960677 (59a3347e) are pending. Complete-tier run 41311293265, which
the row named, was cancelled before it started; the row now says so.

<a id="rfc-9000"></a>
## Read for this batch

Read 2026-11-02 from the RFC Editor: RFC 9000 (`rfc9000.txt`, 403,442 bytes, SHA-256
f88aae47f8b18e102024916e975e919201d8dde689cba79b01079eaedd402e22): section 8.1 and 8.1.1 to 8.1.4 (the amplification
limit, the two kinds told apart, Retry tokens bound to address and port for a short time, NEW_TOKEN tokens bound to the
address only, unique, unlinkable, expiring and accepted once, 128 bits of entropy, integrity protection), 17.2.5 (the
Retry packet, its Source Connection ID never the client's, the client's Source Connection ID kept) and 19.7 (NEW_TOKEN,
never empty).

## What landed

- **`QuicTokenKeys`** (quic.hpp, quic.cpp): `mint_retry`, `mint_new_token` and `validate` over a sealing key and the
  previous one; `quic_token_kind` reads the clear kind. `CryptoError::Expired` and `CryptoError::Replayed` are new.
- **`QuicTokenReplay`**: a split-block Bloom filter in four issue-period slots, allocated once at init, checked with
  atomic word operations on several threads.
- **One sealing-key helper**: tls.cpp's ticket key name and per-ticket AEAD moved into `provider_internal`
  (`sealing_key_name`, `salted_aead`, `label_of`); tickets and tokens now share it.
- **Registers**: `quic.transport` gains NET.2, the token evidence and its note; `quic.tls`'s note hands address
  validation to it.

## Evidence (this PC)

- linux-gcc-debug: `crd-crypto-tests "[quic-token]"` 12 cases, 462,620 assertions; `[quic-handshake]` 16 cases (the
  round-trip case among them); the whole `[crypto]` suite 109 cases, 579,846 assertions; `crd-crypto-tls-interop-openssl`
  clean after the ticket helper moved; all four crypto fuzz corpora replay clean. win-debug (MSVC): the same suite and
  the QUIC corpus.
- Measured round trips (X25519MLKEM768; the server's flight packed into Initial and Handshake packets in 1,200-byte
  datagrams): client first flight 2,400 bytes with or without a token; Retry 93 bytes (54-byte token); server flights
  2,400 bytes (classical), 7,963, 10,484 and 13,910 (ML-DSA-44, -65, -87). Unvalidated / after a Retry / with a
  NEW_TOKEN token: classical 1 / 2 / 1, ML-DSA-44 and -65 2 / 2 / 1, ML-DSA-87 2 / 3 / 2 (its flight passes the
  12,000-byte initial congestion window, the transport's to size in 4.2a).
- The replay filter at 2^18 tokens: 55 and 44 false positives in 52,428 probes on two runs (about 0.1%).
- `crd-fuzz-crypto-quic`'s token mode: every input's verdict against the format restated over the provider's HKDF and
  AEAD, minted tokens for each input's client checked the same way; 8 seeds sealed by the restatement (3 accepted;
  refused: another port, unknown key and a wrong sealed length byte as Forged, a short token as BadLength, a late one
  as Expired, each traced under gdb to that verdict); 23 seeds replay clean.
- Nine source mutations, each caught: the port, the version or the client's connection ID left out of the binding,
  a Retry token one second longer, the skew doubled, sealing under the previous key, NEW_TOKEN replay unchecked, every
  token in one filter block, the sealed length byte unchecked.
- `crd-check-diag-runbooks` fails in this partially built lane tree (the runbooks' tests of executables this batch did
  not build); nothing in this batch touches a runbook.

## Remaining on NET.2

ECH, then the clauses the row lists after it; a complete-tier hosted run that includes this batch.
