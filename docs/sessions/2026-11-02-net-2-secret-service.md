# NET.2: the key store's Secret Service protector

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Date: 2026-11-02. Row: [NET.2](../ROADMAP.md#slice-net.2) (track C, batch 63). Contract: [the key store, decision
K10](../design/network-foundation.md#key-store). Everything ran on this PC (the track's WSL lanes); no cloud machine
ran (0 hours, 0 USD).

## CI first

Push run 41462446381 (d7371489): `win-public-checks` failed one case, "HPKE: exports agree on both sides of every
suite", because a one-byte export is zero once in 256 runs per suite. 13c815d3 fixes the test (unlike fillings prove
both sides wrote; only exports of at least Nh bytes are checked nonzero). Complete-tier dispatch 41508851661 runs at
13c815d3. Two defects of batch 62 found here and fixed: the key store tests read `store.find(name)->generation`
unchecked (GCC 13 LTO `-Wnull-dereference` on linux-gcc-release), and `crd-fuzz-crypto-key-store` had no
`scripts/fuzz.py` entry (`crd-repository-tools`).

## What landed

- **`SecretTransportKey`** (provider.hpp): `dh-ietf1024-sha256-aes128-cbc-pkcs7`, RFC 2409's 1024-bit MODP group
  (prime checked against its formula and as a safe prime), HKDF-SHA256 of the left-padded shared value, AES-128-CBC
  with PKCS #7; the peer value must lie in [2, p - 2]. No algorithm identifier, so no policy can name it.
- **The protector** ([secret_service.cpp](../../engine/foundation/crypto/src/secret_service.cpp)): libdbus-1 loaded at
  run time and never unloaded, a private connection per call, every reply's signature checked, items found by schema,
  store and a random handle, deleted to forget (`forgets()` true), prompts only when `PlatformKeyProtector(ms)` allows.
- **The pinned gnome-keyring** (tools/gnome-keyring-linux-x64, `scripts/install-gnome-keyring.py`): three Ubuntu
  packages from Launchpad, SHA-256 checked, unpacked without root; `secret_service_session.py` runs the hidden cases in
  a private bus and scratch home, unlocked and locked (`crd-crypto-secret-service-*`, on linux-gcc-public-checks).
- **Registers**: `freedesktop.secret-service` (spec type `freedesktop`, new in check-registers and register-review),
  the inventory regenerated and cerid-package's five findings restated and reviewed (counsel review stays open).

## Evidence

- linux-gcc-debug and linux-gcc-asan: `[key-store]`, `Provider*` and `[hpke]`, 49 cases, 114,705 assertions; the two
  Secret Service CTests (1,338 assertions unlocked: 256 sessions, four threads, a store whose revoked master key's
  earlier copy no longer opens; locked: Denied without a prompt and with a dismissed one); fuzz corpora replayed.
- linux-gcc-release (LTO): `[key-store]` and `Provider*`, 41 cases.
- `wsl-lane.sh dev linux-gcc-debug` on the changed files: 308 selected CTests (the guards included) and LLVM-20 tidy,
  clean after three fixes (a widening cast, two names). One run failed `crd-check-allocation-results`, which passed
  unchanged on rerun and from the mirror: this host's known misexecution.
- Repository tool tests: 320, 3 skipped (the installer, the `freedesktop` spec type and the fuzz entries).
- Teeth, each caught: the peer bound widened (unit test), the HKDF info changed (only gnome-keyring catches it: the real
  peer is the derivation's known answer), forget not deleting, the store attribute dropped.
- ASan first found libdbus's process-wide allocations reported as leaks: the library was unloaded at exit; it now stays
  loaded, as every libdbus user keeps it.

## Next

NET.2's remaining clauses in its row: TPM 2.0, PIV over PKCS#11, the first policy generation, the Tamarin or ProVerif
model, RFC 9850 key logging's guard, ARM64 assembly and prefixed symbols, the boards against OpenSSL and BoringSSL.
