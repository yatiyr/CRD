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
- `touch` (8f1e45aa, both programs; arm `cerid-touch`): before its first signing, each thread touches 512 KiB of its
  stack, one page at a time.
- Local win-asan (i9-14900K): every new mode is clean and every refused crossing prints its SETUP line. The
  register entry now records the evidence below.

## Evidence (failures out of 40 runs)

- 45553773567 (dc6705c1, Xeon Platinum 8573C):
  - Failed: case 29; `cerid` 27; `seed-key` 26; `raw-verify` 26; `no-verify` 27.
  - Clean (0): `control`, `control-copy`, `raw`, `raw-copy`, `raw-crd-verify`.
  - So it is `SigningKey::sign` as crd-crypto runs it. The same calls written in the program pass. Neither the key's
    seed nor the verification matters.
- 45557190754 and 45557203999 (8f1e45aa, two Xeon 6973P-C; 45554828003 drew no affected CPU):
  - `cerid` failed 27 and 25. `cerid-touch` failed 0 and 0, and `cerid-raw` 0 and 0.
  - 10 of the 52 events were fast exits.
- Each refusal is mldsa-native's rejection loop run to its bound: 65535/L attempts, about 13,100 for ML-DSA-65. So
  every attempt of that one signing was rejected.
- **Trigger:** the thread's first touch of new stack pages during the signing, at crd-crypto's call depth. It may be
  those pages or their ASan shadow; the run does not separate them. The case has failed only under win-asan, on
  Emerald and Granite Rapids Xeons.

## Next

1. A Cerid-free control that places the first signing at swept stack depths. A failing depth is the reproduction a TP
   entry needs.
2. Separate stack pages from shadow pages: touch only the stack (a `__chkstk`-probed frame).
3. Then the handling. Whether a build without ASan can hit it is item 2's question.
