# 2026-11-03: NET.2, a product's policy layer over Cerid's (Track C batch 86)

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET.2](../ROADMAP.md#slice-net.2). Design: [S25](../design/network-foundation.md#policy-layers). User decision
(2026-11-03, option C): "We must grant flexibility without making our system more complicated".

## CI first

No completed hosted run contains 1d038940 or later (push run 41682201803 is pending). The failed jobs of the active
runs are all fixed on main already: `std::system` on linux-gcc-release (fd75495e), the Catch2 redirect fault in the
key store tests on linux-gcc-public-checks and linux-clang-tsan (0e206133), and the earlier shipping and android
faults. Two failures are outside this track: CEIR-19c DX12 on win-release (41619527221), and the Vulkan SDK download
on linux-gcc-asan 2/2 (41646840079): its checksum did not match the pin, while the sibling jobs of that run passed.

## What landed

- **Composition (crd-crypto policy.hpp/.cpp).** `PolicyConfig::base` makes a store a product layer over Cerid's. Its
  `snapshot()` holds both layers. An algorithm is refused when the base refuses it and otherwise has the product's
  status, so the earlier refusal date wins and the product's preference decides among what both accept. Freshness is
  the worse of the two layers, and None when either layer has no generation. A version must pass both minimums.
  `loosenings()` lists every product rule whose window goes outside the base rule, or that has no base rule. A base
  that has a base of its own is a bad config, and a cycle of stores is never followed. TLS, QUIC, ECH, MLS and
  capabilities read the composed snapshot without any change. A TLS session also records `base_policy_generation()`.
- **ceridc:** `policy verify --base-generation <f> --base-trust <trust.toml>` checks the base layer first, then
  reports the composed status of each rule, a `base` object and a `loosenings` list.
- **Tests:** `test_policy_layers.cpp` has 6 cases: tightening, every attempted loosening and its report, scopes and
  minimums, a missing or expired layer, separate trust sets, floors and rollbacks, a remote disable through the base,
  three layers refused, and a product installation's own saved state. A TLS case runs a session under a product
  layer. A ceridc case covers the layered verify and its refusals. The policy fuzz target has a new mode 2: sequences
  for both layers, checked against a model of the composition, plus one committed seed.

## Evidence (win-debug, this PC)

- crd-crypto-tests `[net-std],[tls],[mls],[capability],[policy-layers]`: 194 cases, 658292 assertions, all pass.
- crd-ceridc-tests `[policy]`: 8 cases pass. The fuzz corpus replays clean (21 inputs), and so do 3000 random
  mode-2 inputs.
- Teeth: with the base refusal disabled, the committed layered seed fails the oracle at fuzz_policy.cpp:696.
- The LLVM 20.1.8 tidy gate passes on all 10 changed sources after one fix (all_of in the layer test). The ctest
  guards pass. crd-check-capability-owners failed once with CPython's "internal error in regular expression engine"
  (this host misexecutes now and then) and passed when rerun.

## Next

The NET.2 items still open: Android Keystore, Keychain and Secure Enclave, PIV over ykcs11, the rest of the Tamarin
model, and the hosted evidence.
