// crd-draw -- DrawTheme global storage (Phase 3.1 v1a-draw d2-theme).
//
// Set-once-at-startup, read-many globally. No mutex: writes are serialized
// to startup (or between frames) by the caller; per-frame reads only see
// committed values.

#include <crd/draw/theme.hpp>

namespace crd::draw
{
namespace
{
DrawTheme& mutable_theme() noexcept
{
    static DrawTheme s;
    return s;
}
} // namespace

const DrawTheme& current_theme() noexcept
{
    return mutable_theme();
}

void set_theme(const DrawTheme& theme) noexcept
{
    mutable_theme() = theme;
}

} // namespace crd::draw
