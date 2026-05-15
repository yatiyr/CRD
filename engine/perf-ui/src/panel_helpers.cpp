// ---------------------------------------------------------------------------
// crd-perf-ui -- panel rendering helpers (D-003 v0g).
// ---------------------------------------------------------------------------

#include <crd/perf/ui/panel_helpers.hpp>

#include <cstdio>

namespace crd::perf::ui
{

namespace
{

// HSV -> RGB. h in [0,1), s in [0,1], v in [0,1]. Stable + branchless-ish.
[[nodiscard]] crd::u32 hsv_to_rgb32(double h, double s, double v) noexcept
{
    const double scaled = h * 6.0;
    const int    sector = static_cast<int>(scaled) % 6;
    const double f      = scaled - static_cast<int>(scaled);
    const double p      = v * (1.0 - s);
    const double q      = v * (1.0 - s * f);
    const double t      = v * (1.0 - s * (1.0 - f));
    double r = v, g = v, b = v;
    switch (sector)
    {
        case 0: r = v; g = t; b = p; break;
        case 1: r = q; g = v; b = p; break;
        case 2: r = p; g = v; b = t; break;
        case 3: r = p; g = q; b = v; break;
        case 4: r = t; g = p; b = v; break;
        case 5: r = v; g = p; b = q; break;
        default:                     break;
    }
    const auto cr = static_cast<crd::u8>(r * 255.0);
    const auto cg = static_cast<crd::u8>(g * 255.0);
    const auto cb = static_cast<crd::u8>(b * 255.0);
    // ImGui IM_COL32: 0xAABBGGRR (little-endian alpha-most-significant)
    return (0xFFU << 24) | (static_cast<crd::u32>(cb) << 16) | (static_cast<crd::u32>(cg) << 8)
           | static_cast<crd::u32>(cr);
}

// FNV-1a-ish 32-bit mix of a 32-bit input -> a well-distributed hash.
[[nodiscard]] crd::u32 mix32(crd::u32 x) noexcept
{
    x ^= x >> 16;
    x *= 0x7FEB352DU;
    x ^= x >> 15;
    x *= 0x846CA68BU;
    x ^= x >> 16;
    return x;
}

} // namespace

[[nodiscard]] Color32 color_for_name(NameId name) noexcept
{
    const crd::u32 h = mix32(name.is_valid() ? name.value + 1U : 0U);
    // Pick hue from the hash; pin saturation high + value mid-bright so
    // every name reads as a saturated palette color.
    const double hue = static_cast<double>(h) / 4294967296.0;
    return Color32{hsv_to_rgb32(hue, 0.62, 0.78)};
}

[[nodiscard]] Color32 color_for_category(Category cat) noexcept
{
    switch (cat)
    {
        case Category::User:   return Color32{0xFFB0BEC5U};  // grey-blue
        case Category::Job:    return Color32{0xFF7CB342U};  // green
        case Category::System: return Color32{0xFFFFB300U};  // amber
        case Category::Pass:   return Color32{0xFF42A5F5U};  // blue
        case Category::Render: return Color32{0xFF26A69AU};  // teal
        case Category::Gpu:    return Color32{0xFFAB47BCU};  // purple
        case Category::Memory: return Color32{0xFFEC407AU};  // pink
        case Category::Io:     return Color32{0xFF8D6E63U};  // brown
        case Category::Wait:   return Color32{0xFF78909CU};  // grey
    }
    return Color32{0xFF888888U};
}

crd::usize format_duration(crd::i64 ns, char* buf, crd::usize buf_size) noexcept
{
    if (buf == nullptr || buf_size == 0U)
    {
        return 0U;
    }
    const crd::i64 abs_ns = ns < 0 ? -ns : ns;
    int written = 0;
    if (abs_ns >= 1'000'000'000)
    {
        written = std::snprintf(buf, buf_size, "%.3f s", static_cast<double>(ns) * 1e-9);
    }
    else if (abs_ns >= 1'000'000)
    {
        written = std::snprintf(buf, buf_size, "%.3f ms", static_cast<double>(ns) * 1e-6);
    }
    else if (abs_ns >= 1'000)
    {
        written = std::snprintf(buf, buf_size, "%.3f us", static_cast<double>(ns) * 1e-3);
    }
    else
    {
        written = std::snprintf(buf, buf_size, "%lld ns", static_cast<long long>(ns));
    }
    return written < 0 ? 0U : static_cast<crd::usize>(written);
}

crd::usize format_bytes(crd::u64 bytes, char* buf, crd::usize buf_size) noexcept
{
    if (buf == nullptr || buf_size == 0U)
    {
        return 0U;
    }
    int written = 0;
    if (bytes >= (1ULL << 30))
    {
        written = std::snprintf(buf, buf_size, "%.2f GB", static_cast<double>(bytes) / (1ULL << 30));
    }
    else if (bytes >= (1ULL << 20))
    {
        written = std::snprintf(buf, buf_size, "%.2f MB", static_cast<double>(bytes) / (1ULL << 20));
    }
    else if (bytes >= (1ULL << 10))
    {
        written = std::snprintf(buf, buf_size, "%.2f KB", static_cast<double>(bytes) / (1ULL << 10));
    }
    else
    {
        written = std::snprintf(buf, buf_size, "%llu B", static_cast<unsigned long long>(bytes));
    }
    return written < 0 ? 0U : static_cast<crd::usize>(written);
}

crd::usize format_count(crd::u64 count, char* buf, crd::usize buf_size) noexcept
{
    if (buf == nullptr || buf_size == 0U)
    {
        return 0U;
    }
    int written = 0;
    if (count >= 1'000'000'000ULL)
    {
        written = std::snprintf(buf, buf_size, "%.2fB", static_cast<double>(count) * 1e-9);
    }
    else if (count >= 1'000'000ULL)
    {
        written = std::snprintf(buf, buf_size, "%.2fM", static_cast<double>(count) * 1e-6);
    }
    else if (count >= 1'000ULL)
    {
        written = std::snprintf(buf, buf_size, "%.2fk", static_cast<double>(count) * 1e-3);
    }
    else
    {
        written = std::snprintf(buf, buf_size, "%llu", static_cast<unsigned long long>(count));
    }
    return written < 0 ? 0U : static_cast<crd::usize>(written);
}

[[nodiscard]] crd::u64 total_thread_duration_ns(
    crd::containers::ConstSpan<Sample> samples) noexcept
{
    crd::u64 sum = 0U;
    for (const auto& s : samples)
    {
        if (s.depth == 0U && s.end_ns >= s.begin_ns)
        {
            sum += static_cast<crd::u64>(s.end_ns - s.begin_ns);
        }
    }
    return sum;
}

crd::u32 aggregate_top_level_by_name(crd::containers::ConstSpan<Sample> samples,
                                      NameTotal* out_totals,
                                      crd::u32 out_capacity) noexcept
{
    if (out_totals == nullptr || out_capacity == 0U)
    {
        return 0U;
    }
    crd::u32 count = 0U;
    for (const auto& s : samples)
    {
        if (s.depth != 0U)
        {
            continue;
        }
        if (s.end_ns < s.begin_ns)
        {
            continue;
        }
        const crd::u64 dur = static_cast<crd::u64>(s.end_ns - s.begin_ns);
        // Linear scan for an existing slot (small N).
        bool merged = false;
        for (crd::u32 i = 0U; i < count; ++i)
        {
            if (out_totals[i].name.value == s.name_id)
            {
                out_totals[i].total_ns += dur;
                out_totals[i].occurrences += 1U;
                merged = true;
                break;
            }
        }
        if (!merged && count < out_capacity)
        {
            out_totals[count].name        = NameId{s.name_id};
            out_totals[count].total_ns    = dur;
            out_totals[count].occurrences = 1U;
            ++count;
        }
    }
    return count;
}

} // namespace crd::perf::ui
