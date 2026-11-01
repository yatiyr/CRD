# NET.2: QUIC packet protection, and the track's builds move to this PC

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Date: 2026-11-01. Row: [NET.2](../ROADMAP.md#slice-net.2) (track C, batch 41; the
[parallel tracks](2026-10-31-parallel-tracks.md)). Contract:
[QUIC packet protection](../design/network-foundation.md#quic-protection).

## The user's decisions this batch acts on

- 2026-11-01, evening: "I WANT NETWORK PARTS TO BE COMPLETELY DONE AND TESTED! I WANT FULL AND SECURE AND GOLD
  STANDARD NETWORK MODULE!" This batch takes the next part of NET.2's Remaining: QUIC's packet protection, the
  cryptography the QUIC handshake integration and the address-validation tokens build on.

<a id="network-testing-cost"></a>
- 2026-11-01, night, on network testing and its cost: "I approve your proposal. But we should not pass 50 dollars a
  month and we should actually spend a lot less so I thik we should focus on simulating network conditions with our
  tools and I remember that we were also building them so utilize everything that minimizes our costs but we need to
  do properly". Recorded as the decision on [NET-SIM](../ROADMAP.md#slice-net-sim) and
  [NET.5](../ROADMAP.md#slice-net.5): NET-SIM's deterministic simulator and NET-DEBUG's link conditioning carry the
  proof of loss, delay, jitter, reordering, duplication, partitions, clock skew, MTU, NAT behaviour and attack traffic,
  and every network row's tests run through them; real kernel stacks on one machine (network namespaces joined by
  veth pairs, tc netem, NAT of every class through nftables) come next; a real multi-machine run only for what
  neither proves (WAN paths between regions, the Windows stack against Linux, a cloud NAT), on the smallest spot
  machine, created for that run and deleted with its disk when it ends, its cost recorded, everything on cerid-ci
  together well under 50 USD a month.
- 2026-11-01, later that night: "DO EVERYTHING TO REDUCE THE COSTS DO NOT RUN MACHINES IF WE ARE NOT DOING ANYTHING
  WITHOUT DOING ANYTHING WE SHOULD NOT SPEND ANY MONEY! ... FIX ALL OF THEM! SAVE MY MONEY!" This track now builds and
  tests on this PC; no cloud machine ran in this batch (0 hours, 0 USD).

## CI first

No hosted run had completed with a failure from this track's newer commits. Complete-tier run 41323535550 (54f062f2)
and push run 41333397427 (59f64219) had finished some jobs; every failure in them was already fixed on main:

- win-public-checks, win-asan and win-debug-scalar: MSVC C4389 in `test_tls.cpp`, fixed by 9ef40e7e;
- win-vs, win-arm64-debug and both repository-check jobs: `crd-module-selection`'s package set, fixed by 47474b0b;
- win-arm64-debug's `crd-ci-platform` table: the CI platform's, fixed on main by df0d359f.

Push run 41338869330 (300c9c0f, the first to carry the certificate compression batch) was still pending.

## The source mirror on a worktree

`scripts/source-mirror.py` copied the checkout's `.git` as a directory and then its `index`. A linked worktree's
`.git` is a file naming its own git directory (`gitdir: D:/Dev/cerid/.git/worktrees/cerid-track-c`), so the copy
walked nothing and the index copy failed with NotADirectoryError. The mirror's repository is now resolved through
`git rev-parse --path-format=absolute --git-dir --git-common-dir` (the Windows git through WSL interop, its paths
turned back with `wslpath -u`): the common directory without its `worktrees` records, and the worktree's own HEAD,
index, logs and refs over it, its `commondir`, `gitdir` and `locked` pointers left out (with them the mirror's git
would read `.git` as a worktree of a Windows path it cannot open). Empty repository directories are kept (git
recognises a repository by `refs` and `objects`). A new `SourceMirror` case builds a worktree with its own branch
and a main checkout whose index differs, and proves the mirror is a standalone repository on the worktree's branch,
clean against the worktree's index, incrementally synced, and switched back by a sync from the main checkout; it fails
on the old script. This track's lanes now run with `CRD_SOURCE_MIRROR=~/cerid-src-track-c` and
`CRD_LANE_ROOT=~/cerid-track-c-build`: the first sync copied 6,777 files, linux-gcc-debug configured in 136 s and
`crd-crypto-tests` built in 26 s through the shared compiler cache.

## QUIC packet protection

`quic.hpp` (contract linked above): Initial secrets of versions 1 and 2, `QuicPacketKey` (AEAD key and IV per key
phase, in-place seal and open, `update()` with "quic ku", the RFC 9001 section 6.6 confidentiality limit enforced as
`Exhausted` with a lower limit a transport may set and `update_due()` at three quarters), `QuicHeaderKey` (AES-ECB or
ChaCha20 mask, protection and removal, kept across key updates), the packet number encoder and decoder of RFC 9000
appendices A.2 and A.3, and the Retry integrity tag of both versions. `CryptoError::Exhausted` is new. Nothing
allocates.

Read 2026-11-01 from the RFC Editor: RFC 9001 (`rfc9001.txt`, 126,175 bytes, SHA-256
3bbaecdf5afd278052a2c48348ce118c4ff8d0cf6b9915549858171b3f98a591) and RFC 9369 (`rfc9369.txt`, 26,887 bytes, SHA-256
3d5db815157c4a83778775ff3d123dc94565429c5858bb28a7275372c531b239); the sample packets of their appendices A are copied
into `test_quic.cpp` by script from those files, not retyped.

Evidence (this PC, linux-gcc-debug from the track's mirror):

- `crd-crypto-tests "[quic]"`: 10 cases, 15,055 assertions: both versions' Initial secrets; the client's 1,200-byte and
  the server's Initial packets sealed and protected to RFC's bytes and read back; both Retry tags, every bit of a
  Retry and of the original connection ID authenticated; the ChaCha20-Poly1305 short header packet and its key
  update against RFC's "ku" secret; RFC 9000's packet number examples, every window edge and the half-window ties;
  refusals and lengths; the usage limit; a changed byte anywhere in a protected packet; 200 random packets per suite
  and version; four threads on one key.
- `crd-fuzz-crypto-quic`: header protection refused exactly when the packet cannot be sampled and restored by a round
  trip, an opened packet re-sealing to its input, Retry verdicts against the computed tag, packet numbers against a
  restatement of appendix A.3; 15 seeds (an honest and a changed packet per version and suite, both RFC Retries, the
  RFC packet numbers) written through the harness's verdict sink, which refuses a seed whose verdict changed.
- Ten source mutations of `quic.cpp`, each caught by the `[quic]` cases: the long header's mask bits, version 2's
  header protection label, the decoder's lower tie and its upper window guard, the seal limit, the nonce's byte
  order, the Retry tag comparison, the encoder's range, the key update label and the sample offset.
- The whole `crd-crypto-tests` (82 cases, 113,315 assertions) and the four crypto corpora pass.

## A flaky expectation found and fixed

One of the first full runs failed in batch 40's certificate compression case: an ECDSA server sent its Certificate
with zstd where the case expected zlib. An ECDSA certificate's signature is random, and its two compressed forms lie
within a few bytes of each other, so either can be the smaller from run to run; the server's choice was right and the
expectation was not. The ECDSA rows now accept either algorithm, with both ends agreeing (the choice of the smaller
form stays exact in the deterministic Ed25519 row and in the case that crosses the client's and server's lists). Two
of the 20 runs before the fix failed this way; the ten runs after it all pass.

## Next

NET.2's QUIC handshake integration over the same sessions (TLS 1.3 through the provider's QUIC interface: each
encryption level's secrets into these keys, transport parameters, CRYPTO data per level), then the address-validation
tokens with Retry measured in round trips within the amplification limit.
