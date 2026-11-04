#pragma once

// crd-core -- the engine's one locale-independent parse of a decimal floating-point number (capability
// text.parse-float): the readers of JSON, CEIR text and diagnostic commands share it.
//
// std::from_chars (std::chars_format::general) where the standard library has it for floating point; Apple's libc++
// (LLVM 19 in Xcode 16) has it for integers only, so there the number is copied into a bounded buffer and read with
// strtod_l under the "C" locale, which agrees on every number the general format accepts. No allocation.

#include <crd/core/types.hpp>

namespace crd
{

// The longest number parse_f64 reads where it falls back to strtod_l (the fallback's buffer): longer text is refused.
inline constexpr usize kParseFloatMaxBytes = 1023U;

// Whether [first, last) is exactly one number of std::chars_format::general (an optional '-', digits with an optional
// fraction, an optional exponent; or "inf", "infinity", "nan") whose value a double holds, then written to `out`.
// False, with `out` unchanged, for an empty or partial match and for a value out of a double's range.
[[nodiscard]] bool parse_f64(const char* first, const char* last, f64& out) noexcept;

namespace detail
{
// The strtod_l reading parse_f64 uses where the standard library lacks a floating from_chars; on every POSIX host, so
// its tests compare the two wherever both exist.
[[nodiscard]] bool parse_f64_c_locale(const char* first, const char* last, f64& out) noexcept;
} // namespace detail

} // namespace crd
