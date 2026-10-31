#pragma once

// crd-platform (private) -- the engine's UTF-8 text handed to Win32 as UTF-16 and read back, for the platform's Windows
// sources. Both directions are crd-core's transcoding (crd/core/utf16.hpp): an unpaired surrogate or a byte that starts
// no UTF-8 sequence reads as U+FFFD, as on every platform.

#include <crd/core/platform.hpp>

#if CRD_OS_WINDOWS
#include <crd/containers/array.hpp>
#include <crd/containers/string.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/core/utf16.hpp>

#include <string_view>

namespace crd::platform::wide_text
{

// `text` as a NUL-terminated UTF-16 string.
[[nodiscard]] inline containers::Array<wchar_t> utf8_to_wide(containers::StringView text)
{
    const auto*                bytes = reinterpret_cast<const u8*>(text.data());
    const usize                units = utf8_to_utf16(bytes, text.size(), static_cast<wchar_t*>(nullptr), 0U);
    containers::Array<wchar_t> wide;
    wide.resize(units + 1U); // value-initialised: the last unit is the NUL
    (void)utf8_to_utf16(bytes, text.size(), wide.data(), units);
    return wide;
}

// `wide` as UTF-8.
[[nodiscard]] inline containers::String wide_to_utf8(std::wstring_view wide)
{
    const usize        size = utf16_to_utf8(wide.data(), wide.size(), static_cast<u8*>(nullptr), 0U);
    containers::String text;
    text.resize(size);
    (void)utf16_to_utf8(wide.data(), wide.size(), reinterpret_cast<u8*>(text.data()), size);
    return text;
}

} // namespace crd::platform::wide_text
#endif
