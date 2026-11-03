# 2026-11-03: NET.2, the address validation model (Track C batch 87)

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET.2](../ROADMAP.md#slice-net.2). Design: [protocol model](../design/network-foundation.md#protocol-model),
[S13](../design/network-foundation.md#address-validation).

## CI first

No completed hosted run contains 4bf36dc1 yet (push run 41688808713 is pending). There is nothing new to fix:

- 41646840079 linux-gcc-relwithdebinfo: the LTO `-Wnull-dereference` in `save.constprop`. Its commit 8d7b6a2b comes
  before 1d038940, which fixed it.
- 41646840079 linux-gcc-asan 2/2: the Vulkan validation archive failed its SHA-256 check in one download. Shard 1/2 of
  the same run verified the same pin an hour later, so this was the server's response that time, not the pin.

## The model

`tests/foundation/crypto/model/quic_address_validation.spthy` covers:
- **Addresses:** the adversary can send from any claimed source, but reads only addresses whose path it owns.
- **Token keys:** a fleet's keys rotate (the newest seals, the one before it only opens) and any key can be revealed.
- **Tokens:** Retry and NEW_TOKEN tokens, with their associated data as `token_associated` builds it.
- **Validation:** by a token or by the handshake. NEW_TOKEN tokens are published, and a single-use filter holds them.

Eleven lemmas verify in 17 s:
- An address is validated only after a datagram the server sent there reached the client there, unless the adversary
  owns that path or a token key leaked.
- Each kind is accepted only with the binding it was minted with.
- A NEW_TOKEN token is accepted once.
- A token from before the last rotation still opens. One from before the last two rotations never does.
- The bound: a replayed Retry token is accepted again within its binding.

Time and unlinkability stay with test_quic_token.cpp.

Termination took three modelling choices, each recorded in the model:
- The keys are persistent facts held by order restrictions, not a linear key state.
- A sources lemma also covers what an adversary-owned address reads and the token a client resends.
- A client that sends a NEW_TOKEN token ends there, because the client continuing looped the search.

**Evidence:** `check-protocol-model.py` passes all four models in WSL (17 s, 77 s, 63 s and 41 s). The first full run
aborted in tls_resumption.spthy with a GHC `scavenge: strange closure type` internal error, and the rerun passed.
The same error also stopped one single-lemma run of `key_secrecy`. This is the host fault MEMORY records. The
model's first `previous_key_opens` (any token, unguided) aborted twice and segfaulted with one thread. The Retry-guided
form proves in 13 steps.

**Teeth** (each mutation falsifies its lemma):
- Without the filter, `new_token_once` fails (16 steps).
- Without the address in NEW_TOKEN's associated data, `new_token_authentic` fails (7 steps) and so does
  `address_validated` (14 steps).
- Without the port in Retry's associated data, `retry_token_authentic` fails (7 steps).

## Next

Audit the first hosted run that contains 4bf36dc1 and this batch. Then the unbounded epoch chain, state learning,
PIV, the Android Keystore and the Keychain.
