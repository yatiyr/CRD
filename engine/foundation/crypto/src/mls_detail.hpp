#pragma once

// crd-crypto -- what MLS's units share (mls.cpp and mls_message.cpp): the head of a labelled structure, so a signed
// structure is laid out once behind its label instead of copied there. Private to engine/foundation/crypto/src.

#include <crd/containers/string_view.hpp>
#include <crd/containers/wire_bytes.hpp>
#include <crd/core/types.hpp>

namespace crd::crypto::mls
{
namespace cont = crd::containers;

// The bytes before `content_bytes` bytes of content in the struct {opaque label<V>; opaque content<V>;} that
// SignContent and EncryptContext share: the vector "MLS 1.0 " + label and the content's header; 0 when either cannot
// be encoded.
[[nodiscard]] usize labelled_head_bytes(cont::StringView label, usize content_bytes) noexcept;
void                put_labelled_head(cont::WireWriter& out, cont::StringView label, usize content_bytes) noexcept;
} // namespace crd::crypto::mls
