# 2026-11-03: NET.2, prefixed provider symbols (Track C batch 79)

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET.2](../ROADMAP.md#slice-net.2) (Partial). Contract: [P5](../design/network-foundation.md#provider).

CI first: nothing to fix. No completed run holds 255decba or later; dispatches 41589181174, 41579304146 and
41574979037 are still queued or running, with no failure past the android boot fault e608e642 fixed. The Android
Keystore stays behind its lane: WSL here has `/dev/kvm`, but this user is not in its `kvm` group.

## What landed

- `scripts/provider-prefix.py`: reads ar archives (GNU, BSD, Microsoft) of ELF64, COFF (bigobj too) and Mach-O 64
  objects in Python, writes AWS-LC's three renaming headers in `make_prefix_headers.go`'s format, `collect`s a lane's
  unprefixed names and `check`s built libraries. Linker-merged definitions (COFF COMDAT select-any, weak C++) are left
  alone; a C++ definition outside `bssl::crd_awslc` is renamed through its outermost name (`ssl_st`).
- `cmake/aws-lc-symbols.txt`: 4703 names, the union of win-debug (MSVC), linux-gcc debug/release/shipping/asan,
  linux-clang-tsan, android-x86_64 and an NDK arm64-v8a scratch build of the pinned AWS-LC.
- The configure writes the headers (`BORINGSSL_PREFIX_HEADERS`, no Go) and names `BORINGSSL_PREFIX` on every AWS-LC
  command and on crypto's build interface. The first build showed why: an existing tree kept objects that had found
  AWS-LC's empty stand-in headers, and only jitterentropy (forced include) was renamed, so bcm.c failed to link.
- `X25519` is renamed by a function-like macro: `AlgorithmId::X25519` shares the name, and object-like renaming broke
  provider.cpp and tls.cpp. A use without a call would fail the link, never bind silently.
- `crd-check-provider-symbols` (every lane) and `crd-crypto-coexist-tests` (`CRD_BUILD_TLS_VS_OPENSSL`): crd-crypto,
  its TLS/QUIC handshake tests and OpenSSL 4.0's libcrypto and libssl in one program.

## Evidence

- win-debug: `check` passes on crypto.lib and ssl.lib (NASM objects included); all 303 crypto CTests pass (1 PKCS #11
  device case skips as before).
- linux-gcc-public-checks: `crd-check-provider-symbols`, `crd-crypto-tls-interop-openssl` and
  `crd-crypto-openssl-coexist` pass; the program defines 3305 `crd_awslc_` functions beside OpenSSL's unprefixed
  `SHA256`, `EVP_Digest` and `SSL_new`, and runs 86 cases, 15895 assertions.
- linux-gcc-debug: 29 lane CTests (`crd-crypto-*`, key store with swtpm, SoftHSM2 and gnome-keyring) pass.
- `test-repository-tools.py ProviderPrefix`: 9 tests. LLVM 20 tidy: test_openssl_coexist.cpp clean.

## Next

- Read the first hosted run holding this commit: `crd-check-provider-symbols` on the ARM64, clang-cl, shipping and
  android lanes (any name a lane adds goes into the list with `collect`).
- Then P3 (Windows ARM64 assembly through clang), the Keychain lane, the boards against OpenSSL and BoringSSL.
