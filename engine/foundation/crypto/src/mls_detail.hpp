#pragma once

// crd-crypto -- what MLS's units share (mls.cpp, mls_message.cpp, mls_tree.cpp, mls_group.cpp and mls_commit.cpp):
// the head of a labelled structure, so a signed structure is laid out once behind its label instead of copied there,
// a leaf node's signed fields, credentials, the sizing and list helpers of the structures' codecs, and the checks and
// byte helpers the group units share. Private to engine/foundation/crypto/src.

#include <crd/containers/array.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/containers/wire_bytes.hpp>
#include <crd/core/types.hpp>
#include <crd/crypto/mls_message.hpp>

namespace crd::crypto::mls
{
namespace cont = crd::containers;

// The bytes before `content_bytes` bytes of content in the struct {opaque label<V>; opaque content<V>;} that
// SignContent and EncryptContext share: the vector "MLS 1.0 " + label and the content's header; 0 when either cannot
// be encoded.
[[nodiscard]] usize labelled_head_bytes(cont::StringView label, usize content_bytes) noexcept;
void                put_labelled_head(cont::WireWriter& out, cont::StringView label, usize content_bytes) noexcept;

// The longest HPKE public key of the profile: an uncompressed P-384 point.
inline constexpr usize kMaxNodeKeyBytes = 97U;

// The total of a structure's fields; a field that cannot be encoded (0 bytes) makes it 0.
class Size
{
public:
    Size& fixed(usize bytes) noexcept
    {
        m_bytes += bytes;
        return *this;
    }
    // A field sized by its own *_bytes or vector_bytes.
    Size& field(usize bytes) noexcept
    {
        m_ok = m_ok && bytes != 0U;
        m_bytes += bytes;
        return *this;
    }
    Size& vector(usize length) noexcept { return field(vector_bytes(length)); }

    [[nodiscard]] usize bytes() const noexcept { return m_ok ? m_bytes : 0U; }

private:
    usize m_bytes = 0U;
    bool  m_ok    = true;
};

// The bytes a decoder took since `start` (the reader's rest() before it began), as a view of the input.
[[nodiscard]] inline cont::ConstSpan<u8> taken_since(cont::ConstSpan<u8> start, const cont::WireReader& in) noexcept
{
    return start.first(start.size() - in.rest().size());
}

// A list whose body is a sequence of `take` structures.
template <typename T, typename Take>
[[nodiscard]] bool take_list(cont::WireReader& in, cont::ConstSpan<u8>& body, Take take) noexcept
{
    if (!take_vector(in, body))
    {
        return false;
    }
    cont::WireReader items(body);
    while (!items.done())
    {
        T item{};
        if (!take(items, item))
        {
            return false;
        }
    }
    return true;
}

// A list of 16-bit codepoints.
[[nodiscard]] inline bool take_u16_list(cont::WireReader& in, cont::ConstSpan<u8>& body) noexcept
{
    return take_vector(in, body) && body.size() % 2U == 0U;
}

// A Credential (section 5.3; mls_message.cpp): its type, never 0, and its vector (a list of certificates for X.509).
[[nodiscard]] bool  take_credential(cont::WireReader& in, Credential& out) noexcept;
[[nodiscard]] usize credential_bytes(const Credential& credential) noexcept;
void                put_credential(cont::WireWriter& out, const Credential& credential) noexcept;

// Copies `from` to `to`, which holds at least from.size() bytes.
inline void copy_bytes(cont::ConstSpan<u8> from, u8* to) noexcept
{
    for (usize i = 0U; i < from.size(); ++i)
    {
        to[i] = from[i];
    }
}

// Appendix C's children of a parent already known to lie inside a tree (level 1 to 31, so the shift stays inside the
// word).
[[nodiscard]] inline u32 left_of(u32 parent) noexcept
{
    return parent ^ (1U << (tree::level(parent) - 1U));
}

[[nodiscard]] inline u32 right_of(u32 parent) noexcept
{
    return parent ^ (3U << (tree::level(parent) - 1U));
}

// A leaf node's fields before its signature (mls_message.cpp), which LeafNodeTBS begins with (mls_tree.cpp); 0 when a
// field cannot be encoded.
[[nodiscard]] usize leaf_fields_bytes(const LeafNode& node) noexcept;
void                put_leaf_fields(cont::WireWriter& out, const LeafNode& node) noexcept;

// The bytes of a PrivateMessageContent before its padding (mls_message.cpp): the body and the authentication; 0 when
// either cannot be encoded.
[[nodiscard]] usize private_content_bytes(const FramedContent& content, const ContentAuth& auth) noexcept;

// Whether capabilities support a GroupContext's extensions: every extension type of the list's body and the
// required_capabilities extension's types (sections 7.3 and 11.1; mls_group.cpp). Unsupported otherwise, Malformed
// for an undecodable required_capabilities.
[[nodiscard]] CryptoError supports_context(cont::ConstSpan<u8> extensions, const Capabilities& capabilities) noexcept;

// Section 12.1.4's checks of a pre-shared key's identity outside reinitialization and branching (mls_group.cpp):
// Malformed for a nonce not Nh long, Unsupported for a resumption key of usage reinit or branch.
[[nodiscard]] CryptoError check_psk_id(const CipherSuite& suite, const PreSharedKeyId& id) noexcept;

// A byte array as a view, a copy of bytes into one, and a buffer that may hold secrets resized (zeroed first).
[[nodiscard]] inline cont::ConstSpan<u8> view(const cont::Array<u8>& bytes) noexcept
{
    return cont::ConstSpan<u8>(bytes.data(), bytes.size());
}

[[nodiscard]] inline CryptoError copy_into(cont::ConstSpan<u8> bytes, cont::Array<u8>& out) noexcept
{
    out.clear();
    if (!out.try_reserve(bytes.size()))
    {
        return CryptoError::ProviderFailure;
    }
    for (const u8 byte : bytes)
    {
        out.push_back(byte);
    }
    return CryptoError::None;
}

inline bool fresh_buffer(cont::Array<u8>& out, usize bytes) noexcept
{
    secure_zero(cont::Span<u8>(out.data(), out.size()));
    out.clear();
    if (!out.try_reserve(bytes))
    {
        return false;
    }
    out.resize(bytes);
    return true;
}

// A secret of up to kMaxHashBytes on the stack, zeroed when it goes out of scope.
class Secret
{
public:
    Secret() noexcept = default;
    ~Secret() noexcept { secure_zero(m_bytes); }
    Secret(const Secret&)            = delete;
    Secret& operator=(const Secret&) = delete;
    Secret(Secret&&)                 = delete;
    Secret& operator=(Secret&&)      = delete;

    [[nodiscard]] cont::Span<u8> span(usize bytes) noexcept { return cont::Span<u8>(m_bytes, bytes); }

private:
    u8 m_bytes[kMaxHashBytes] = {};
};
} // namespace crd::crypto::mls
