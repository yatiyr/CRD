#include <crd/cooker/mesh_cook_options.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace crd::cooker
{

namespace
{

bool sv_starts_with(crd::containers::StringView s, crd::containers::StringView prefix) noexcept
{
    return s.starts_with(prefix);
}

crd::containers::StringView trim_left(crd::containers::StringView s) noexcept
{
    crd::usize i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) { ++i; }
    return s.substr(i);
}

// Parses a non-negative finite float in the form 1, 1.0, 0.01, 1e-3, etc.
// Returns the parsed value on success, or a negative sentinel on failure so
// the caller can reject invalid authoring without throwing.
crd::f32 parse_float_literal(crd::containers::StringView s) noexcept
{
    // strtof on a NUL-terminated buffer; copy through a fixed local stack
    // buffer to avoid pulling crd-memory just for parsing.
    char buf[64] = {};
    const crd::usize n = (s.size() < sizeof(buf) - 1) ? s.size() : (sizeof(buf) - 1);
    for (crd::usize i = 0; i < n; ++i) { buf[i] = s[i]; }
    char* end = nullptr;
    const float v = std::strtof(buf, &end); // NOLINT(cert-err34-c)
    if (end == buf) { return -1.0F; }
    return v;
}

} // namespace

MeshCookOptions parse_mesh_cook_options(crd::containers::StringView meta_text) noexcept
{
    MeshCookOptions out;

    // Tiny line-oriented scanner. The .meta grammar is "section heading
    // in [brackets], one key=value per line". No nesting, no escapes.
    bool in_cook_section = false;
    crd::usize i = 0;
    while (i < meta_text.size())
    {
        // Find end of this line.
        crd::usize end = i;
        while (end < meta_text.size() && meta_text[end] != '\n' && meta_text[end] != '\r') { ++end; }
        crd::containers::StringView line = trim_left(meta_text.substr(i, end - i));

        // Advance past the line terminator (handles \n, \r\n, \r).
        i = end;
        if (i < meta_text.size() && meta_text[i] == '\r') { ++i; }
        if (i < meta_text.size() && meta_text[i] == '\n') { ++i; }

        // Strip inline '#' comments.
        crd::usize hash = 0;
        while (hash < line.size() && line[hash] != '#') { ++hash; }
        line = line.substr(0, hash);

        // Trim trailing whitespace.
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t'))
        {
            line = line.substr(0, line.size() - 1);
        }
        if (line.empty()) { continue; }

        if (line.front() == '[' && line.back() == ']')
        {
            const crd::containers::StringView name = line.substr(1, line.size() - 2);
            in_cook_section = (name == crd::containers::StringView("cook"));
            continue;
        }

        if (!in_cook_section) { continue; }

        // key = value
        if (sv_starts_with(line, crd::containers::StringView("position_scale")))
        {
            crd::containers::StringView rhs = line.substr(crd::containers::StringView("position_scale").size());
            rhs = trim_left(rhs);
            if (rhs.empty() || rhs.front() != '=') { continue; }
            rhs = trim_left(rhs.substr(1));
            const crd::f32 v = parse_float_literal(rhs);
            if (v > 0.0F && std::isfinite(v))
            {
                out.position_scale = v;
            }
            else
            {
                std::fprintf(stderr,
                             "mesh_cook: ignoring non-positive/non-finite position_scale; "
                             "defaulting to 1.0\n");
            }
        }
        else if (sv_starts_with(line, crd::containers::StringView("smooth_angle_deg"))) // GEO-2: normal-generation crease
        {
            crd::containers::StringView rhs = line.substr(crd::containers::StringView("smooth_angle_deg").size());
            rhs = trim_left(rhs);
            if (rhs.empty() || rhs.front() != '=') { continue; }
            rhs = trim_left(rhs.substr(1));
            const crd::f32 v = parse_float_literal(rhs);
            if (v >= 0.0F && v <= 180.0F && std::isfinite(v))
            {
                out.smooth_angle_deg = v;
            }
            else
            {
                std::fprintf(stderr, "mesh_cook: ignoring out-of-range smooth_angle_deg (want 0..180); keeping %g\n",
                             static_cast<double>(out.smooth_angle_deg));
            }
        }
    }

    return out;
}

} // namespace crd::cooker
