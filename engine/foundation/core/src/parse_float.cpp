#include <crd/core/parse_float.hpp>

#include <crd/core/platform.hpp>

#include <charconv>
#include <cmath>
#include <cstring>
#include <system_error>

#if !CRD_OS_WINDOWS
#include <cerrno>
#include <clocale>
#include <cstdlib>
#if CRD_OS_MAC
#include <xlocale.h>
#endif
#endif

namespace crd
{

#if !CRD_OS_WINDOWS
namespace detail
{
bool parse_f64_c_locale(const char* first, const char* last, f64& out) noexcept
{
    if (first == nullptr || last <= first || static_cast<usize>(last - first) > kParseFloatMaxBytes)
    {
        return false;
    }
    const usize bytes = static_cast<usize>(last - first);
    // What strtod reads beyond std::chars_format::general: leading space, a '+' and the hexadecimal form.
    const char* digits = first[0] == '-' ? first + 1 : first;
    if (digits == last || *digits == '+' || *digits == ' ' || (*digits >= '\t' && *digits <= '\r') ||
        (last - digits >= 2 && digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X')))
    {
        return false;
    }
    char text[kParseFloatMaxBytes + 1U];
    std::memcpy(text, first, bytes);
    text[bytes] = '\0';
    static const locale_t kC = ::newlocale(LC_ALL_MASK, "C", static_cast<locale_t>(nullptr));
    if (kC == static_cast<locale_t>(nullptr))
    {
        return false;
    }
    char*     end   = nullptr;
    const int saved = errno;
    errno           = 0;
    const f64 value = ::strtod_l(text, &end, kC);
    const int error = errno;
    errno           = saved;
    // from_chars refuses what overflows a double and what underflows to zero; a subnormal result it keeps.
    if (end != text + bytes || (error == ERANGE && (std::isinf(value) || value == 0.0)))
    {
        return false;
    }
    out = value;
    return true;
}
} // namespace detail
#endif

bool parse_f64(const char* first, const char* last, f64& out) noexcept
{
#if defined(__cpp_lib_to_chars)
    f64        value  = 0.0;
    const auto result = std::from_chars(first, last, value, std::chars_format::general);
    if (first == nullptr || first == last || result.ec != std::errc{} || result.ptr != last)
    {
        return false;
    }
    out = value;
    return true;
#else
    return detail::parse_f64_c_locale(first, last, out);
#endif
}

} // namespace crd
