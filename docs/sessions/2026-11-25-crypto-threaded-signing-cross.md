# 2026-11-25: NET.2, the threaded ML-DSA signing refusal crossed between crd-crypto and raw calls

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Rows: [NET-PATH](../ROADMAP.md#slice-net-path) batch; the entry is [NET.2](../ROADMAP.md#slice-net.2)'s provider.
Follows [the split](2026-11-25-crypto-threaded-signing-control.md).

**CI first:** CI 45529733321 (fdd5c20c) failed `crd-ci-platform` on every lane: the parity table, already regenerated
by 76ed9f1d. Its linux-gcc-asan red beside it is TP-5 (MikkTSpace), registered. Nothing else to fix.

## Why a cross, not the three suspects alone

All 828 faults were a thread's first signing. Before that signing's `EVP_DigestSign` returns, crd-crypto has run only
`ready()`, `find_entry` and `signature_digest`. The seed's `random_bytes` and `secure_zero` run on the main thread,
and `ErrorScope`'s peek and the verification run after the signing. So they can act only through other threads. The
other difference is where the calls live: in crd-crypto's `provider.cpp`, not in the program. The win-asan compile
lines of `provider.cpp` and the two programs match (only `CRD_TLS_KEY_LOG` differs). Leg 5 of 45547501396 also shows
fast exits with no output (0xC0000409, 9 of 106 crd-crypto events), which no raw arm had.

## What landed

- `raw_signing.hpp`: `copy` makes `SigningKey::sign`'s and `ProviderVerifier::verify`'s calls exactly, in the
  program: stack contexts, a heap signature, and `ErrorScope`'s peek and clear after each success. A refusal now names
  the provider error's source file and line.
- `crd-mldsa-threads-control copy`; `crd-mldsa-threads-cerid` adds `seed-key`, `raw-verify`, `no-verify`, and
  `raw copy` and `raw crd-verify`. It refuses crossings that choose nothing.
- `provider-host-check.py --arms` runs only the named arms (unit-tested). It adds six arms.
- `host-check.yml`: `provider_arms`; `provider_cpu=affected` (the default) runs only on Xeons from Sapphire Rapids on.
  There are ten legs.
- Local win-asan (i9-14900K): every new mode is clean and every refused crossing prints its SETUP line.

## Evidence

Pending: the dispatch on this commit.

## Next

Read the dispatch, then fix the cause in crd-crypto, with teeth on the failing Xeons.
