# 2026-11-03: NET.2, the key holders' stored keys and a policy store's start (Track C batch 76)

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET.2](../ROADMAP.md#slice-net.2). Contract: [S22](../design/network-foundation.md#policy-issue) and
[S23](../design/network-foundation.md#policy-start).

CI first: dispatch 41568608043 has five more failed jobs (win-shipping, win-public-checks, win-debug-scalar,
linux-gcc-asan 2/2, linux-clang-arm64-asan); each fails only crd-repository-tools, crd-build-profiles,
crd-ci-platform and crd-ci-orchestrator, the batch-72 defects 98b3a9b0 and 26c32908 fixed. No hosted run holds
04524cf5 yet (41579304146 queued, 41583188484 pending), so the win-tidy read of policy.cpp waits for one.

## What landed

- `ceridc policy key`: makes a key holder's signature key in crd-crypto's key store (`--protector platform|tpm`,
  `--tcti`, `--device tpm|pkcs11` with `--module`, `--label` and the PIN in `--pin-file`, never on the command line)
  or reports the one it holds; an existing name under another algorithm or placement, a non-signature algorithm, a
  bad name and another protector are refused. The report carries the `[[key]]` trust set entry.
- `ceridc policy sign`: checks the body first (new `check_policy_body`, which `write_policy` now uses), signs the
  rendered source or a body file through `StoredSigner`, checks the signature under the stored public key, decodes
  the one-signature generation, then writes it. ceridc links crd-crypto for the protector and device objects.
- `PolicyStore::start(persisted, bootstrap, now)` and `PolicyError::Absent` (S23): the persisted generation, then
  the build's bootstrap; an older bootstrap is a harmless rollback, a newer one supersedes, neither usable leaves the
  store empty (fail closed).

## Evidence (win-debug)

- crd-crypto-tests `[net-std]`: 22 cases, 1348 assertions (start: fresh install, floor above the bootstrap, same
  generation twice, newer bootstrap, damaged persisted file, neither usable, nothing held; check_policy_body).
- crd-ceridc-tests `[policy]`: 7 cases, 423 assertions, with `CRD_CERIDC_EXE`; stored keys under an in-memory
  protector signed generation 1 from the source and from the body, joined and verified against their trust entries;
  the real binary ran the whole ceremony under DPAPI (`--protector platform`) and verified.

## Next

- The bootstrap embedding: a generated header of the signed generation and trust set (empty until the ceremony
  commits them), and crd-net's installation state saving the accepted generation beside its floor.
- The ceremony is the key holders' (user action): `policy key` for ML-DSA-65 on an offline machine and ECDSA P-256
  in a TPM or token, `policy sign` each, `policy assemble`, commit the trust set and signed generation.
