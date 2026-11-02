#pragma once

// crd-crypto -- the Messaging Layer Security protocol, RFC 9420 (NET.2; contract:
// docs/design/network-foundation.md#mls): end-to-end encrypted groups whose relays and delivery servers carry
// ciphertext they cannot read (threat T14), the group keys of calls (SFrame, NET-LIVE) and of shared projects.
//
// Cerid's own implementation over the provider interface (decision M1): the cipher suites of the profile (M2), the
// presentation language's variable-length vectors and optional values (section 2.1), array-based tree math
// (Appendix C) and the labelled operations every later layer derives from (sections 5.1.2, 5.1.3, 5.2, 8 and 9). The
// MLS working group's test vectors, which the other implementations generate and check, hold each layer to the byte.
//
//   lengths     a hash, a derived secret and a reference are the suite's Nh; an expansion is 1 to 65535 bytes and at
//               most 255 * Nh. A wrong length is BadLength.
//   refusals    a key or a signer of another algorithm than the suite's is Unsupported; HPKE's refusals pass through
//               (hpke.hpp). Every refusal zeroes the outputs.
//   allocation  none for labels and contexts that fit 256 bytes together (every per-message derivation); a longer
//               context or a signed message is laid out once from the default allocator.

#include <crd/containers/span.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/containers/wire_bytes.hpp>
#include <crd/core/types.hpp>
#include <crd/crypto/hpke.hpp>
#include <crd/crypto/provider.hpp>
#include <crd/crypto/verifier.hpp>

#include <optional>

namespace crd::crypto::mls
{
// ---- Cipher suites (sections 5.1 and 17.1) ----

// One cipher suite of the profile: its HPKE algorithms, its hash (the KDF's, which is also the MAC's: HMAC with it)
// and its signature algorithm.
struct CipherSuite
{
    u16         id = 0U; // the MLS Cipher Suites codepoint
    AlgorithmId kem{};   // the DHKEM's group (hpke.hpp)
    AlgorithmId kdf{};
    AlgorithmId aead{};
    AlgorithmId signature{};

    [[nodiscard]] HpkeSuite hpke() const noexcept { return {kem, kdf, aead}; }
    // Nh: the length of a hash, of KDF.Extract's output and of a MAC.
    [[nodiscard]] usize hash_bytes() const noexcept { return hkdf_hash_bytes(kdf); }
};

// The suites of the profile, in codepoint order: 0x0001, 0x0002, 0x0003 and 0x0007 (decision M2).
[[nodiscard]] cont::ConstSpan<CipherSuite> cipher_suites() noexcept;
// The suite `id` names; null for a suite outside the profile.
[[nodiscard]] const CipherSuite* cipher_suite(u16 id) noexcept;

// ---- The presentation language (section 2.1) ----

// The largest length a variable-size vector header encodes (2^30 - 1).
inline constexpr usize kMaxVarint = (usize{1} << 30U) - 1U;

// The bytes of the minimal header of `value`: 1, 2 or 4; 0 above kMaxVarint.
[[nodiscard]] usize varint_bytes(usize value) noexcept;
// The bytes of `length` bytes behind their header; 0 above kMaxVarint.
[[nodiscard]] usize vector_bytes(usize length) noexcept;

// Writes the minimal header of `value`, which is at most kMaxVarint (a larger one stops the process: the layout that
// sized `out` was wrong).
void put_varint(cont::WireWriter& out, usize value) noexcept;
// A header and its bytes.
void put_vector(cont::WireWriter& out, cont::ConstSpan<u8> bytes) noexcept;

// Takes a header: false for a short input, the prefix 0b11 and an encoding longer than needed, which are malformed.
// After false the reader's position is unspecified and the decoder refuses the whole message.
[[nodiscard]] bool take_varint(cont::WireReader& in, usize& value) noexcept;
// A header and the bytes it counts, as a view of the input.
[[nodiscard]] bool take_vector(cont::WireReader& in, cont::ConstSpan<u8>& out) noexcept;
// The presence octet of an optional value: false for a short input and any octet but 0 and 1 (section 2.1.1).
[[nodiscard]] bool take_presence(cont::WireReader& in, bool& present) noexcept;

// ---- Array-based trees (Appendix C) ----

// A ratchet tree is a perfect binary tree of 2^d leaves (section 4.1). Nodes are numbered from the left, leaves at the
// even indices (leaf i is node 2i) and parents at the odd ones; a tree of n leaves has 2n - 1 nodes. Every function
// answers for a tree of 1 to 2^31 leaves, a power of two, and a node inside it; any other input has no answer
// (nullopt, 0 or an empty path), so a size or an index read off the wire never reaches past the tree.
namespace tree
{
inline constexpr u32 kMaxLeaves = u32{1} << 31U;
inline constexpr u32 kMaxDepth  = 31U; // the longest direct path

[[nodiscard]] bool valid_size(u32 leaves) noexcept;
// The node count of a tree of `leaves` leaves (0 for an invalid size).
[[nodiscard]] u32 node_width(u32 leaves) noexcept;
[[nodiscard]] std::optional<u32> root(u32 leaves) noexcept;
// A node's level: 0 for a leaf, the level of its children plus one for a parent.
[[nodiscard]] u32                level(u32 node) noexcept;
[[nodiscard]] std::optional<u32> left(u32 node, u32 leaves) noexcept;
[[nodiscard]] std::optional<u32> right(u32 node, u32 leaves) noexcept;
[[nodiscard]] std::optional<u32> parent(u32 node, u32 leaves) noexcept;
[[nodiscard]] std::optional<u32> sibling(u32 node, u32 leaves) noexcept;
// The direct path (a node's ancestors, from its parent to the root) and the copath (the sibling of the node and of
// each ancestor below the root), into `out` (kMaxDepth entries hold any); the count written.
[[nodiscard]] usize direct_path(u32 node, u32 leaves, cont::Span<u32> out) noexcept;
[[nodiscard]] usize copath(u32 node, u32 leaves, cont::Span<u32> out) noexcept;
// The lowest node whose subtree holds both `a` and `b` (either, when one is an ancestor of the other).
[[nodiscard]] std::optional<u32> common_ancestor(u32 a, u32 b, u32 leaves) noexcept;
} // namespace tree

// ---- Labelled operations ----

// "MLS 1.0 " before every label of a signature, an encryption and a key derivation (sections 5.1.2, 5.1.3, 8).
inline constexpr cont::StringView kLabelPrefix = "MLS 1.0 ";

// RefHash(label, value) = Hash(RefHashInput{label, value}) (section 5.2); `label` is complete ("MLS 1.0 KeyPackage
// Reference"), `out` Nh long.
[[nodiscard]] CryptoError ref_hash(const CipherSuite& suite, cont::StringView label, cont::ConstSpan<u8> value,
                                   cont::Span<u8> out) noexcept;

// ExpandWithLabel(secret, label, context, out.size()) = KDF.Expand(secret, KDFLabel{length, "MLS 1.0 " + label,
// context}, length) (section 8).
[[nodiscard]] CryptoError expand_with_label(const CipherSuite& suite, cont::ConstSpan<u8> secret,
                                            cont::StringView label, cont::ConstSpan<u8> context,
                                            cont::Span<u8> out) noexcept;

// DeriveSecret(secret, label) = ExpandWithLabel(secret, label, "", Nh).
[[nodiscard]] CryptoError derive_secret(const CipherSuite& suite, cont::ConstSpan<u8> secret, cont::StringView label,
                                        cont::Span<u8> out) noexcept;

// DeriveTreeSecret(secret, label, generation, out.size()) = ExpandWithLabel(secret, label, generation as a big-endian
// uint32, length) (section 9.1).
[[nodiscard]] CryptoError derive_tree_secret(const CipherSuite& suite, cont::ConstSpan<u8> secret,
                                             cont::StringView label, u32 generation, cont::Span<u8> out) noexcept;

// SignWithLabel(key, label, content) = Signature.Sign(key, SignContent{"MLS 1.0 " + label, content}) (section 5.1.2):
// `signer` holds the suite's signature algorithm, `out` and `written` as Signer::sign.
[[nodiscard]] CryptoError sign_with_label(const CipherSuite& suite, const Signer& signer, cont::StringView label,
                                          cont::ConstSpan<u8> content, cont::Span<u8> out, usize& written) noexcept;

// VerifyWithLabel(key, label, content, signature) under the suite's signature algorithm.
[[nodiscard]] Verification verify_with_label(const CipherSuite& suite, const SignatureVerifier& verifier,
                                             cont::ConstSpan<u8> public_key, cont::StringView label,
                                             cont::ConstSpan<u8> content, cont::ConstSpan<u8> signature) noexcept;

// EncryptWithLabel(public_key, label, context, plaintext) = SealBase(public_key, EncryptContext{"MLS 1.0 " + label,
// context}, "", plaintext) (section 5.1.3): `kem_output` and `ciphertext` as hpke_seal's `enc` and `ciphertext`.
[[nodiscard]] CryptoError encrypt_with_label(const CipherSuite& suite, cont::ConstSpan<u8> public_key,
                                             cont::StringView label, cont::ConstSpan<u8> context,
                                             cont::ConstSpan<u8> plaintext, cont::Span<u8> kem_output,
                                             cont::Span<u8> ciphertext) noexcept;

// DecryptWithLabel(private_key, label, context, kem_output, ciphertext) = OpenBase(kem_output, private_key,
// EncryptContext, "", ciphertext): `key` holds the suite's KEM, `plaintext` as HpkeKey::open's `out`.
[[nodiscard]] CryptoError decrypt_with_label(const CipherSuite& suite, const HpkeKey& key, cont::StringView label,
                                             cont::ConstSpan<u8> context, cont::ConstSpan<u8> kem_output,
                                             cont::ConstSpan<u8> ciphertext, cont::Span<u8> plaintext) noexcept;
} // namespace crd::crypto::mls
