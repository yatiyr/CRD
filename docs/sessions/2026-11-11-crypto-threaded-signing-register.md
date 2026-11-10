# 2026-11-11 — NET.2: the win-asan threaded ML-DSA signing refusal, registered

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Rows: [NET.2](../ROADMAP.md#slice-net.2) (its provider), read first in the [NET-PATH](../ROADMAP.md#slice-net-path)
batch after the [GCC slice](2026-11-10-net-path-gcc.md).

## What was seen

`Provider: one verifier and one key serve several threads` (crd-crypto-tests) failed on hosted win-asan twice:
push run 41797302943 (2026-11-04, shard 1, one failed round, the call not yet named) and push run 42856002748 at
`7d76aa2c` (2026-11-10, shard 2): one thread's first `SigningKey::sign` returned `ProviderFailure`; every other
signing and verification of the four threads passed. The failing case took 19.0 s and 17.0 s; it passes in 0.99 s on
win-asan (42360577693) and in 0.38 s on linux-clang-tsan, which ran it clean in both sighting runs.

## Why it is registered, not fixed

- The engine shares nothing writable across the threads: `SigningKey::sign` is const over a read-only key handle,
  and each call owns its signing context and error scope.
- The provider's ML-DSA code cannot return a signing error in this configuration (stack allocation, a randomness
  failure aborts, the attempt count is bounded), so the refusal is below Cerid on hosted Windows with ASan.
- By the third-party rule nothing in the provider is traced further; a run on this PC would not separate Cerid from
  this host either.

## What landed

- `tests/support/diag/nonreproduced-failures.json`: `win-asan-ml-dsa-threaded-signing-refused`, status
  `nonreproduced`, with both runs and the discriminators above.
- The case now also reports how long a refused signing took and what a second signing at once answers, so the next
  sighting tells a stall from a persistent refusal. win-debug: the case passes (10 assertions).

## Next

NET-PATH: QUIC receive timestamps feeding GCC's `received_ns`.
