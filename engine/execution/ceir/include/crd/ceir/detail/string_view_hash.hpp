#pragma once

// crd-ceir detail — a FNV-1a-over-bytes hasher for `crd::containers::StringView` keys. crd::containers ships a
// DefaultHash for std::string_view and its own String, but NOT for StringView, so the CEIR maps that key on an
// arena-stable StringView (symbol table, dialect registry) carry this one hasher rather than duplicating it.

#include <crd/containers/hash.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>

namespace crd::ceir::detail
{
struct StringViewHash
{
    [[nodiscard]] u64 operator()(containers::StringView sv) const noexcept
    {
        return containers::hash_string(sv.data(), sv.size());
    }
};
} // namespace crd::ceir::detail
