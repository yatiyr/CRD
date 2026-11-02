#pragma once

// crd-crypto -- what MLS's units share (mls.cpp, mls_message.cpp and mls_tree.cpp): the head of a labelled structure,
// so a signed structure is laid out once behind its label instead of copied there, and a leaf node's signed fields.
// Private to engine/foundation/crypto/src.

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
} // namespace crd::crypto::mls
