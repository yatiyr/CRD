# 2026-11-03: NET.2, the resumption model and two key store CI fixes (Track C batch 85)

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET.2](../ROADMAP.md#slice-net.2). Design: [protocol model](../design/network-foundation.md#protocol-model).

## CI first

Scheduled run 41619527221 (8d2485b5) had six failed jobs. Four were already fixed on main: the key-log
`poisoning existing macro` on win-clang-cl-shipping and linux-gcc-shipping, `std::system` on linux-gcc-release, and
the android ISA floor; relwithdebinfo's profile.cpp link error was fixed too. Two failures were new, both in this
track's key store, both fixed in 1d038940:

- **linux-clang-tsan, "writers in several threads and stores are ordered by the lock"**: one writer thread's
  `open()` failed, losing its six names. Diagnosis: `open()` and `refresh()` read the store without the lock, and
  `load_generation` reads a save in steps (the header, then the file it names), so a read racing a writer's rename
  sees a mismatch. Both now read under the lock every change takes (`read_locked`). In an existing directory they
  create the lock file; a directory that cannot take the lock and holds no store is still `NotFound`. The test holds
  the lock and checks that an open waits for it; teeth: without the lock the open finished at once (TSan lane, WSL).
- **linux-gcc-relwithdebinfo, LTO `-Wnull-dereference` in `save.constprop`** (reproduced in WSL): GCC could not rule
  out a wrapped, empty payload in the create clone. `save()` now refuses the sizes `read_header()` refuses.

Evidence: crd-crypto-tests `[key-store]` passes on linux-clang-tsan (the writers test 40 times in a row) and
linux-gcc-relwithdebinfo, whose LTO link now succeeds; linux-gcc-release links.

## The resumption model

`tests/foundation/crypto/model/tls_resumption.spthy` (S9, S16, S17): tickets sealed under the ticket key with a
fresh salt, a single-use client store, psk_dhe_ke resumption with binder and early data, the server's single-use
filter, and reveals of the ticket key, either end's session and either key share. Twelve lemmas verify: PSK secrecy
(timed), early data accepted at most once unless the ticket key leaked, early data authentic and secret, injective
agreement both ways, forward-secret resumed traffic secrets, and the bound (no forward secrecy for early data).
The server-side agreement lemma has a broken-key-exchange disjunct because Tamarin did not terminate without it
(an attack also needs the PSK). One chain link is modelled, since the unrolled chain did not terminate.

- `check-protocol-model.py` passes all three models in 3 min 26 s in WSL (resumption: 46 s). It now passes
  `--derivcheck-timeout=120`, because Tamarin's default budget timed out the derivation checks on a busy host.
- Teeth: dropping the filter falsifies `early_data_once` (27 steps); a ticket sealed under a public key falsifies
  `psk_secrecy` (8 steps).

## Next

Audit the first hosted run with 1d038940 (linux-clang-tsan, linux-gcc-relwithdebinfo). Then the address validation
token model (S13), the unbounded epoch chain, state learning, PIV, Android Keystore and Keychain.
