#pragma once

// The redaction class every diagnostic record carries (docs/design/runtime-diagnostics.md, the event, capture and
// failure schema; ADR-0133 ID-8). A Restricted record keeps its class, its code and its scalars, never its content:
// the flight recorder keeps no text of a restricted event or trigger, a restricted diagnostic event is made without its
// message, and a restricted inspected CEIR value is reported without its bits.

#include <crd/core/types.hpp>

namespace crd
{

enum class RedactionClass : crd::u8
{
    Public = 0U,
    Restricted, // the content never leaves its owner
};

// "public" or "restricted".
[[nodiscard]] constexpr const char* redaction_class_name(RedactionClass c) noexcept
{
    return c == RedactionClass::Restricted ? "restricted" : "public";
}

} // namespace crd
