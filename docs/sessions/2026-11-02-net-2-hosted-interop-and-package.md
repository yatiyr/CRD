# NET.2: the interop test on a hosted lane, crd-crypto in the package

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Date: 2026-11-02. Row: [NET.2](../ROADMAP.md#slice-net.2) (track C, batch 45; the
[parallel tracks](2026-10-31-parallel-tracks.md)). Contracts: [the package](../design/public-consumption.md) and
[TLS sessions](../design/network-foundation.md#tls-sessions).

## The user's decisions this batch acts on

- 2026-11-01, evening: "I WANT NETWORK PARTS TO BE COMPLETELY DONE AND TESTED! I WANT FULL AND SECURE AND GOLD
  STANDARD NETWORK MODULE!" This batch takes NET.2's first Remaining clause: the interoperability CTest in a hosted
  Linux lane and `crd-crypto` in the package consumer test.
- 2026-11-01, night: "DO EVERYTHING TO REDUCE THE COSTS ... SAVE MY MONEY!" Everything ran on this PC: the win-debug
  profile in the worktree and the track's WSL lane (`~/cerid-src-track-c`, `~/cerid-track-c-build`). No cloud machine
  ran (0 hours, 0 USD). No complete-tier run was dispatched: run 41342960677 is still running and the nightly
  complete run (03:00 UTC) on main's head is the first hosted run of both CTests.

## CI first

Complete-tier run 41342960677 (59a3347e) had 12 jobs passing, 16 running and two failed. One failure is this track's,
in two parts, both fixed here; the other is not:

- **win-clang-cl-shipping, `crd-check-isa-isolation`.** The first link-time-IL Windows images that link AWS-LC
  (`crd-crypto-tests.exe`) failed on AWS-LC's MULX/ADX Montgomery multiplication (`x86_64-mont5.asm`), which runs only
  behind its own CPU check. The guard left a provider's functions out of ELF images but judged a map image's provider
  code as any other library's against the reviewed exceptions. `Owners` now takes the manifest's provider archives
  and classifies their object columns as `provider`, which `judge_map_image` leaves out (two new unit cases: the
  classification, and a MULX body that fails in a general object and passes in the provider's archive; ADR-0134 §6
  amended).
- **win-clang-cl-shipping, `crd-check-compile-commands`.** jitterentropy's sources carry `/Od` (the library's timing
  measurements need it) after the configuration's `/O2`: two levels on one command. AWS-LC's directories now inherit
  the MSVC-style configuration flags without their optimization level, and every AWS-LC target whose sources name no
  level of their own gets it back as a compile option (MSVC-style compilers only: GCC's last `-O` wins without a
  warning and AWS-LC's GCC miscompilation probe reads `CMAKE_C_FLAGS_RELEASE`).
- **win-tidy** fails on `readability-use-anyofallof` in `tests/numerics/hesap-fft/test_fft_kernels.cpp:803`, the
  FFT track's test, not this track's; main has no fix yet.

Push run 41358455047 (33ac38d4) is pending; every push run between it and 59a3347e was cancelled before it started.

## What landed

- **The interop test on a hosted lane.** `linux-gcc-public-checks` (complete tier, the `linux-gcc` job) turns on
  `CRD_BUILD_TLS_VS_OPENSSL`, so the lane builds the pinned OpenSSL 4.0.3 peer and runs
  `crd-crypto-tls-interop-openssl` beside the package consumer test. The job installs `zlib1g-dev` for the peer's
  zlib certificate compression (the workflow, `ci/pipeline.py`'s x64 list, which the parity test holds to it, the
  `unpinned` apt record of `cmake/pins.json` and the regenerated license manifest).
- **`crd-crypto` in the package.** `test-package-consumer.py` packages `CRD_MODULES=perf,math,crypto` and checks
  `crd-crypto` and AWS-LC's two archives (`Cerid::aws-lc-ssl`, `Cerid::aws-lc-crypto`) like every member. The
  diagnostic application gains a `crypto` route (`example_crypto.cpp`): one TLS 1.3 connection between two sessions
  in the application, both ends authenticated by pinned ML-DSA-65 keys over X25519MLKEM768, the server's certificate
  compressed with zstd, a message each way and close_notify both ways, judged field by field (`crypto_problems`).
- **Two package defects the run found, fixed.** libcrypto holds the objects of AWS-LC's object libraries
  (crypto_objects, fipsmodule, jitterentropy), which wrote their debug types to `vc140.pdb` files of their own, so
  under the `pdb` form the install found no `crypto.pdb` and failed. They now write to the archive's compile PDB, as
  the engine's object libraries do (`cmake/CrdSimd.cmake`). And AWS-LC's exported link interface names
  `Threads::Threads` on Windows too, which `CeridConfig.cmake` found only outside Windows: a Windows consumer failed
  to generate. It now finds Threads whenever the package lists `aws-lc-crypto`.
- **The debug information of every archive is proven.** Beside the engine's probe, the application's PDB or DWARF
  must name a member of a type defined only in libssl (`cert_compression_negotiated`) and one only in libcrypto
  (`lag_prediction_success_run`). Candidates that also reach a program another way were measured and dropped:
  `ex_pathlen` (x509_st) is in libssl's PDB too, and `reseed_calls_since_initialization` reached the program with
  libcrypto's PDB removed.

## Evidence (this PC)

- win-debug (MSVC 19.51, Ninja, the `pdb` form, no compiler launcher, as the CTest runs it):
  `test-package-consumer.py --preset win-debug` passes: the layout with `crypto.lib`, `ssl.lib` and their PDBs, the
  three probes, every example, the codecs route and `CONSUMER_CRYPTO group=0x0101 aead=0x0501 signature=0x0302
  certificate_compression=3 handshake=1 mutual=1 round_trip=1 closed=1`. The installed `crypto.pdb` names types of all
  three object libraries (`ctr_drbg_state_st`, `x509_st`, `rand_data`'s members). One run hit the host's
  misexecution (C1001 in `<xlocnum>` and an access violation in Ninja) and passed when rerun.
- Teeth: deleting `crypto.pdb` from the moved prefix fails the run naming libcrypto's probe; deleting `ssl.pdb`
  fails it naming libssl's.
- linux-gcc-public-checks (GCC 13.3, the `dwarf` form) through `wsl-lane.sh`: the preset configures with both
  options, builds the two peers and the pinned OpenSSL, `crd-crypto-tls-interop-openssl` passes 69 of 69 cases in
  2.5 s and `crd-package-consumer` passes in 54 s.
- One level per command: fresh MSVC 19.51 Release and clang-cl shipping (Release, thin LTO, `CRD_SHIPPING`)
  configures of the crypto module pass `check-compile-commands.py` on 518 commands each (jitterentropy `/Od` alone,
  bcm.c, x509.c, ssl_lib.cc and tls.cpp `/O2`); the same check failed the hosted lane before.
- win-debug after the CMake change: `crd-crypto-tests "[crypto]"` 120 cases, 582,052 assertions; the WSL lane's run
  above repeated on the final tree (configure, both peers, interop 69 of 69, the consumer test).
- `test-repository-tools.py` (316 cases) and `test-ci-platform.py` (99) pass; the consumer cases cover the crypto
  judge, the new dependencies and every probe, the ISA cases the provider's map code.

## Remaining on NET.2

The clauses the row lists after this one; the hosted evidence is the first complete-tier run that includes this batch.
