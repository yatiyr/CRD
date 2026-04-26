#pragma once

#include <crd/core/types.hpp>

#include <string_view>

namespace crd::containers
{
// StringView is std::string_view. We don't gain anything by writing our
// own — std::string_view is already a non-owning (data, size) pair with
// a thoroughly tested API. The alias keeps the engine-side namespace
// consistent.
using StringView = std::string_view;

// Forward decl so to_view can talk about String without pulling in
// string.hpp (which itself uses std::string_view through this header
// chain). Real definition comes from string.hpp.
class String;

// Helper: explicit conversion from String to StringView. The implicit
// conversion operator on String also exists — this is for the call sites
// where you want it to be obvious that no copy is happening.
StringView to_view(const String& s) noexcept;
} // namespace crd::containers
