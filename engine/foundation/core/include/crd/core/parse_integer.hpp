#pragma once

// crd-core -- the engine's one parse of a decimal integer (capability text.parse-integer): the diagnostic commands,
// the fault plans, the Matrix Market reader and every tool's numeric flags share it. Digits only, no whitespace, no
// '+', no allocation; a value past its bound is refused, never wrapped or clamped.

#include <crd/core/types.hpp>

namespace crd
{

// Whether [first, last) is 1 to 20 decimal digits whose value is at most `max`, then written to `out`. False, with
// `out` unchanged, otherwise.
[[nodiscard]] bool parse_u64(const char* first, const char* last, u64 max, u64& out) noexcept;

// parse_u64 up to u64's maximum.
[[nodiscard]] inline bool parse_u64(const char* first, const char* last, u64& out) noexcept
{
    return parse_u64(first, last, ~u64{0}, out);
}

// Whether [first, last) is an optional '-' and then parse_u64's digits whose value an i64 holds, then written to
// `out`. False, with `out` unchanged, otherwise.
[[nodiscard]] bool parse_i64(const char* first, const char* last, i64& out) noexcept;

} // namespace crd
