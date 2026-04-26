#include <crd/containers/string.hpp>
#include <crd/containers/string_view.hpp>

namespace crd::containers
{
// Definition for the forward declaration in string_view.hpp. Lives here
// so callers can use to_view(s) without dragging the full <string.hpp>
// into headers that only need StringView.
StringView to_view(const String& s) noexcept
{
    return StringView{s.data(), s.size()};
}

// Force-link helper. Same pattern as log_channel.cpp's anchor: ensures
// string.cpp gets pulled into the test executable even though String
// is otherwise a header-only template-free class. The anchor variable
// lives in containers.hpp.
int force_link_string() noexcept
{
    return 0;
}
} // namespace crd::containers
