# The crypto fuzz targets (NET-STD, NET.2). Their committed corpora are replayed on every lane and linux-clang-fuzz
# builds the libFuzzer executables from the same sources. Contract: docs/design/test-instruments.md; harness:
# tests/support/fuzz.
#   crd-fuzz-crypto-capability decode_capability, decode_revocations and decode_schema against restated layouts, and
#                              generated lives (a hybrid-signed root, delegations narrower or widened, a revocation
#                              generation, an audience, a time and a presentation; a message schema, a time and an
#                              operation asked of a capability) against a restated containment rule, a model of each
#                              verification order and the restated operation verdict, with any flipped byte of a
#                              verified token or schema refused.
#   crd-fuzz-crypto-key-store  the key store's sealed file and changes: a body the harness seals opens exactly when a
#                              restatement of its rules accepts it, and lists the restated entries through a rewrite; a
#                              payload opens exactly when a restated header and opening accept it; generated changes
#                              against a model of rotation, revocation and destruction, across reopenings.
#   crd-fuzz-crypto-mls        MLS's vector headers and presence octets against a restatement of RFC 9420 section
#                              2.1, tree math against its own invariants, HPKE round trips with any bit changed,
#                              hostile openings and signatures through the labelled operations, and derivations
#                              (determinism, the length in the label, DeriveTreeSecret as ExpandWithLabel).
#   crd-fuzz-crypto-mls-commit MLS's proposals, commits and external joins: a member that joined an honest group
#                              processes any message, and the honest Update proposal, commit and external commit changed
#                              in any byte, in each suite as public and private messages; a refusal changes nothing and
#                              only the honest commits stage, and merge, their committers' epochs; a joiner refuses the
#                              published GroupInfo changed in any byte and joins only from the intact one.
#   crd-fuzz-crypto-mls-group  MLS's group structures and joins: whatever a decoder (Welcome, GroupInfo, GroupSecrets,
#                              GroupContext, required capabilities, external senders) accepts writes back exactly, an
#                              Extension list's repeats and lookups are a restatement's, and an honest Welcome changed
#                              in any byte is refused with the group left empty or joins only the honest epoch.
#   crd-fuzz-crypto-mls-message MLS's message structures, framing and protection: whatever a decoder accepts writes
#                              back to exactly the bytes it took; a private message opens to what was protected and,
#                              changed in any byte or hostile, is refused with nothing spent; a public message verifies
#                              exactly when unchanged.
#   crd-fuzz-crypto-mls-tree   MLS's ratchet tree and TreeKEM: an accepted tree writes back exactly, its resolutions
#                              and filtered direct paths are section 4.1's restated, tree changes follow a model and
#                              keep the held hashes equal to a fresh decode's, and a changed update path is refused
#                              unchanged or decrypts to the honest commit secret.
#   crd-fuzz-crypto-policy     decode_policy against a restated layout, and generated generation sequences signed by
#                              the test signer through PolicyStore::apply against the contract's adoption and window
#                              rules.
#   crd-fuzz-crypto-provider   the provider interface on hostile input: AEAD openings that must re-seal to their input,
#                              verdicts and refusals by length, key agreement with arbitrary peers, HKDF's prefix
#                              property, sign-then-verify from arbitrary seeds and the Secret Service transport
#                              (bounds of the service's value, openings that re-seal to their input).
#   crd-fuzz-crypto-quic       QUIC packet protection on hostile packets, Retries, packet numbers, address-validation
#                              tokens and packet number windows: header protection refuses exactly the unsampleable
#                              packets and round-trips, an opened packet re-seals to its input, a Retry verifies exactly
#                              when its tag is the computed one, packet numbers decode to the closest candidate, a
#                              token's verdict is the one its restated format, binding and lifetime give, and a
#                              window's verdict on every number is its restated contract's.
#   crd-fuzz-crypto-tls        TLS 1.3 sessions and QUIC handshakes fed hostile bytes (records, or CRYPTO data per
#                              level) as a server and as a client: never established, only the documented answers,
#                              failure is final, every buffer stays bounded and no client Application secret is
#                              reached; the same after a real resumption with early data (a client that offered it,
#                              a server that accepted it and reads no forged early byte); and
#                              compressed Certificates decoded only to reachable claims within the bound.
crd_fuzz_target(crd-fuzz-crypto-capability
    SOURCES fuzz_capability.cpp LIBRARIES crd-crypto CORPUS "${CMAKE_CURRENT_SOURCE_DIR}/corpus-capability")
crd_fuzz_target(crd-fuzz-crypto-key-store
    SOURCES fuzz_key_store.cpp LIBRARIES crd-crypto crd-platform CORPUS "${CMAKE_CURRENT_SOURCE_DIR}/corpus-key-store")
crd_fuzz_target(crd-fuzz-crypto-mls
    SOURCES fuzz_mls.cpp LIBRARIES crd-crypto CORPUS "${CMAKE_CURRENT_SOURCE_DIR}/corpus-mls")
crd_fuzz_target(crd-fuzz-crypto-mls-commit
    SOURCES fuzz_mls_commit.cpp LIBRARIES crd-crypto CORPUS "${CMAKE_CURRENT_SOURCE_DIR}/corpus-mls-commit")
crd_fuzz_target(crd-fuzz-crypto-mls-group
    SOURCES fuzz_mls_group.cpp LIBRARIES crd-crypto CORPUS "${CMAKE_CURRENT_SOURCE_DIR}/corpus-mls-group")
crd_fuzz_target(crd-fuzz-crypto-mls-message
    SOURCES fuzz_mls_message.cpp LIBRARIES crd-crypto CORPUS "${CMAKE_CURRENT_SOURCE_DIR}/corpus-mls-message")
crd_fuzz_target(crd-fuzz-crypto-mls-tree
    SOURCES fuzz_mls_tree.cpp LIBRARIES crd-crypto CORPUS "${CMAKE_CURRENT_SOURCE_DIR}/corpus-mls-tree")
crd_fuzz_target(crd-fuzz-crypto-policy
    SOURCES fuzz_policy.cpp LIBRARIES crd-crypto CORPUS "${CMAKE_CURRENT_SOURCE_DIR}/corpus")
crd_fuzz_target(crd-fuzz-crypto-provider
    SOURCES fuzz_provider.cpp LIBRARIES crd-crypto CORPUS "${CMAKE_CURRENT_SOURCE_DIR}/corpus-provider")
crd_fuzz_target(crd-fuzz-crypto-tls
    SOURCES fuzz_tls.cpp LIBRARIES crd-crypto crd-compression CORPUS "${CMAKE_CURRENT_SOURCE_DIR}/corpus-tls")
crd_fuzz_target(crd-fuzz-crypto-quic
    SOURCES fuzz_quic.cpp LIBRARIES crd-crypto CORPUS "${CMAKE_CURRENT_SOURCE_DIR}/corpus-quic")
