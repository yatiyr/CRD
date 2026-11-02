# NET.2: the key store and its DPAPI protector

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Date: 2026-11-02. Row: [NET.2](../ROADMAP.md#slice-net.2) (track C, batch 62; the
[parallel tracks](2026-10-31-parallel-tracks.md)). Contract: [the key store, decisions K1 to
K9](../design/network-foundation.md#key-store).

## The user's decisions this batch acts on

- 2026-11-01, evening: "I WANT NETWORK PARTS TO BE COMPLETELY DONE AND TESTED! I WANT FULL AND SECURE AND GOLD
  STANDARD NETWORK MODULE!": the principals' long-term keys get their store, with revocation that destroys.
- 2026-11-01, night: "DO EVERYTHING TO REDUCE THE COSTS ... SAVE MY MONEY!" Everything ran on this PC (the worktree's
  win-debug tree and the track's WSL lanes); no cloud machine ran (0 hours, 0 USD).

## CI first

No completed hosted run contains 8157a7f8 or later yet: push runs 41494210543 (8e7b784a) and 41495188182 (8a2436b0)
were cancelled, and 41496051688 (83f40382, another track's docs) is pending. Push run 41462446381 (d7371489) shows
three failed jobs. The third, `linux-gcc-shipping`, was read this batch: the same five LTO links (`crd-crypto-tests`
and the `mls`, `mls-commit`, `mls-group` and `mls-tree` fuzz targets) with the same `-Wnull-dereference` and
`-Warray-bounds` sites (`hpke.cpp:499` and `:682`, `mls.hpp:51`, `mls_group.hpp:764`, `mls_tree.hpp:132`/`133`) that
8157a7f8 fixed. Nothing new. Scheduled run 41429790527 (4f30bb98) passed all 31 jobs: the first hosted run of MLS
groups and commits on every lane.

## What landed

- **The store** ([key_store.hpp](../../engine/foundation/crypto/include/crd/crypto/key_store.hpp),
  [key_store.cpp](../../engine/foundation/crypto/src/key_store.cpp)): signature, agreement and KEM keys of every
  algorithm the provider implements, and secrets (AES-GCM and ChaCha20-Poly1305 keys, HKDF secrets, SipHash keys).
  `generate` and `import` add generation 1; `rotate` and `rotate_import` add the next as Current and retire the previous
  one; `revoke` erases a generation's private input and keeps its public key and identifier; `destroy` removes a name.
  Keys are used through `StoredSigner` (a `Signer`; `TlsCredential::init(StoredSigner)` binds it to TLS),
  `agreement_key` and `kem_key`; only secrets are read out. Private inputs never cross the interface (K1).
- **The file** (K3): one crd-platform durable save, no older generation kept, in an owner-only directory; the body is
  sealed with AES-256-GCM under HKDF-SHA256 of the master key and a fresh salt per save, the header (format, store
  identifier, sequence, protector, references, salt) authenticated. On open, every held private input must give the
  stored identifier (and so the stored public key): an authentic but inconsistent store is `Malformed`.
- **The master key** (K2, K5): a 32-byte key from the provider's randomness, guarded by a `KeyProtector`. Revocation
  and destruction replace it where the protector can forget the old one: save under the new key with the old
  reference pending, forget, clear. An earlier copy of the store then no longer opens; an interrupted replacement is
  finished by the next change before anything else.
- **DPAPI** ([platform_key_protector.cpp](../../engine/foundation/crypto/src/platform_key_protector.cpp)):
  `PlatformKeyProtector` on Windows, user scope, no prompt, its entropy the label and the store's identifier. It cannot
  forget, and says so (`forgets()` false). On Linux it answers `Unavailable` until the Secret Service protector lands.
- **Writers** (K6): every change takes the store's `FileLock` and applies itself to the newest save; an open store
  refuses an older save (`Replayed`) or another store in its place (`Forged`).
- **crd-platform**: `create_private_directory` (mode 0700; on Windows a protected DACL with one inheritable
  full-control entry for the token's user, linking advapi32) and `FileLock` (LockFileEx; flock). crd-crypto now
  depends on crd-platform (no cycle: crd-platform links no cryptography).
- **A defect found along the way**: `test_filesystem.cpp` and `test_file_watcher.cpp` made their scratch directory
  under the working directory, against `fs::temp_directory()`'s rule, and a run from the checkout left
  `.crd_platform_test_tmp` at its root (check-repository refused it). Both now use the system's scratch directory.
- **Errors** (K9): `NotFound`, `Exists`, `Revoked`, `Unavailable`, `Denied`, `Damaged` and `StorageFailure`.
- **Registers**: `cerid.key-store` (format; confidentiality and integrity; implemented); the crypto inventory
  regenerated; cerid-package's five findings restated to name the key store and reviewed (the counsel review stays
  the open register entry). The `crypto.providers` owner also holds the operating systems' key protection headers
  and calls (DPAPI, NCrypt, libsecret, the Keychain).
- **The fuzz target** `crd-fuzz-crypto-key-store`: a body the harness seals (decision K3 restated) opens exactly when
  a restatement of the body's rules accepts it, lists the restated entries and survives a rewrite; a raw payload
  opens exactly when a restated header and AEAD opening accept it; generated changes follow a model of K4 and K5
  across reopenings. Ten committed seeds.
- **Design**: [the key store](../design/network-foundation.md#key-store), decisions K1 to K9, after each platform's
  interface was read (DPAPI, the Secret Service, the Keychain and Secure Enclave, TPM 2.0, the Android Keystore, PIV).

## Evidence

Everything ran on this PC: the worktree's win-debug tree (MSVC) and the track's WSL lanes (linux-gcc-debug and
linux-gcc-asan).

- `[key-store]`: 12 cases, 2,352 assertions on win-debug (DPAPI protecting, recovering and refusing, a store under
  DPAPI created, reopened and used) and 2,330 on linux-gcc-debug and linux-gcc-asan (the platform protector
  `Unavailable`). They cover every kind of key generated, reopened and used (17 algorithms: signatures verified by the
  provider, agreements both ways, decapsulations, secrets and their identifiers), imports (RFC 8032's TEST 1 key),
  rotation and retired generations, every refusal of a change, revocation (material erased, an earlier copy of the
  store refused once its master key is forgotten, idempotent, the Current generation revoked and rotated again),
  destruction, an interrupted master key replacement finished by the next change, every byte of a payload changed and
  refused, truncations, a damaged envelope, an older and a substituted store refused, two stores and four threads
  writing through the lock with nothing lost, and a TLS credential from a stored signer.
- `[fs]` of crd-platform: 9 cases (77 assertions on Windows, 76 on Linux), the two new ones checking the owner-only
  directory against the operating system's own record (the DACL's protection, one inheritable full-control entry for
  the token's user; mode 0700 and the owner) and `FileLock` holders across objects and threads.
- The whole crd-platform suite after the scratch-directory fix, run from a scratch working directory: 66 cases
  (19,678 assertions) on win-debug and 67 (19,679) on linux-gcc-debug, nothing left in the working directory.
- `crd-fuzz-crypto-key-store`: 10 seeds written by `--seed` (identical on both platforms) and replayed, on ASan too;
  the corpus CTest passes.
- Provider error names: the case extended to the seven new values passes on both platforms.
- Source mutations, each built and run on win-debug, every one caught: no re-key on revocation (unit cases), no
  sequence check (unit cases), a private input not checked against its identifier (the fuzz corpus), the header
  neither sealed nor opened as associated data (unit cases and corpus), the protector's name not checked (unit cases),
  revocation keeping the private input (unit cases and corpus), changes outside the lock (unit cases).
- The CTest guards on linux-gcc-debug (`^crd-no-|^crd-check|^crd-diag-coverage$|^crd-diag-seeded-faults$`): the
  container guard found eight `std::string` and `std::wstring` uses in the new tests, now crd containers;
  `crd-check-peer-headers` crashed once (SEGFAULT) and passed when run again unchanged, this host's known
  misexecution (MEMORY, "This host misexecutes"); all 20 pass.
- `tidy-files.py` on the changed C++, on Linux (LLVM 20.1.0) and, for the Windows-only code (the DACL, LockFileEx,
  DPAPI and the DACL test), on win-debug's database (LLVM 20.1.8): clean after ten findings fixed. In the tests: an
  unused using, two local and two static constants' names, a nested conditional. In the Windows code: three
  `const_cast`s (DPAPI's blobs now point at the caller's own copies, the DACL call gets its own UTF-16 name, the test
  reads the ACE as mutable) and an integer-to-pointer cast (`FileLock` keeps a `HANDLE` and a descriptor apart). Both
  platforms rebuilt and every case, the whole platform suite and the corpus passed again.
- Validators: master-plan, repository, Allman braces, generated sources (the crypto inventory refreshed), registers
  (`cerid.key-store` implemented; cerid-package reviewed), capability owners.

## Next

The key store's next protector is the Secret Service on Linux: the freedesktop protocol over libdbus-1 loaded at
run time (its C ABI declared locally, as the DX12 and Vulkan loaders are), the `dh-ietf1024-sha256-aes128-cbc-pkcs7`
session so no master key crosses the bus in the clear, unlock prompts, `forget` deleting the item. It is proven
against a pinned gnome-keyring-daemon in a private `dbus-run-session` (its Ubuntu 24.04 packages, a testing
download). Then TPM 2.0 (simulator and this PC's TPM), then PIV tokens over PKCS#11 (SoftHSM2).
