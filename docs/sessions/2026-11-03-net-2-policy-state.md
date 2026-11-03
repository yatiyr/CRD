# 2026-11-03: NET.2, the generated bootstrap and an installation's saved policy state (Track C batch 78)

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET.2](../ROADMAP.md#slice-net.2) (Partial). Contract: [S24](../design/network-foundation.md#policy-state).

CI first: nothing to fix. Every run holding e608e642 or later is still queued (dispatch 41589181174 has not left
Preflight), so the android lane's audit waits; 41574979037's android failure is the boot fault e608e642 fixed.

## What landed

- `scripts/gen-policy-bootstrap.py`: embeds a directory's `trust-set.toml` and newest `generation-<N>.policy` as
  constexpr data (key identifiers computed, bytes copied, every trust-set key and value checked). Outputs: the build's
  `engine/foundation/crypto/src/policy_bootstrap.hpp` (empty until the ceremony) and the test fixture header.
  Manifest entries, the `crd-crypto-policy-bootstrap-drift` CTest, `*.policy` marked binary.
- The fixture: a test ML-DSA-65 and ECDSA P-256 key made with `ceridc policy key` under DPAPI in a scratch store
  (deleted after), generation 1 signed from `generation-1.policy.toml` and assembled with `ceridc`, verified with
  `ceridc policy verify --source`.
- crd-crypto `policy_state.hpp`: `policy_bootstrap()`, `PolicyInstallation` (open: lock, read, raise the floor, S23
  start, save what changed; apply: lock, catch up with another process's save, adopt, save) and
  `PolicyStore::raise_floor`. One durable file (`policy-state`, one older save kept).
- `TempDirectory` moved from the key store tests to `tests/foundation/crypto/temp_directory.hpp`, shared by both.

## Evidence

- win-debug: `crd-crypto-tests "[net-std]"` 29 cases, 1499 assertions; `[key-store]` 22 passed, 1 skipped (9964
  assertions); `crd-crypto-policy-bootstrap-drift` and both register-ids drift CTests pass.
- linux-gcc-debug (WSL, track-c trees): 52 CTests (`Policy`, `Key store`, the drift test) pass, 3 device cases skip
  as before; the unsaved and unavailable paths behave as on Windows.
- win-tidy (LLVM 20.1.8, fresh configure): policy_state.cpp, policy.cpp, test_policy.cpp and test_key_store.cpp
  clean.
- `test-repository-tools.py PolicyBootstrapHeaders`: 6 tests.

## Next

- Audit dispatch 41589181174's android job when it runs; then the Android Keystore host (JNI library under
  `app_process`), then the Keychain lane.
