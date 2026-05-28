#include <catch2/catch_test_macros.hpp>

#include <crd/containers/string_view.hpp>
#include <crd/cooker/mesh_cook_options.hpp>

#include <cmath>

using crd::containers::StringView;
using crd::cooker::parse_mesh_cook_options;

namespace
{
constexpr crd::f32 kAbsTol = 1.0e-6F;
}

TEST_CASE("mesh_cook_options: defaults to 1.0 when meta is empty", "[cooker][units]")
{
    const auto opts = parse_mesh_cook_options(StringView(""));
    REQUIRE(opts.position_scale == 1.0F);
}

TEST_CASE("mesh_cook_options: defaults to 1.0 when [cook] section is absent", "[cooker][units]")
{
    const StringView text =
        "[id]\n"
        "uuid = \"00000000-0000-0000-0000-000000000000\"\n";
    const auto opts = parse_mesh_cook_options(text);
    REQUIRE(opts.position_scale == 1.0F);
}

TEST_CASE("mesh_cook_options: reads [cook] position_scale = 0.01 (cm -> m)", "[cooker][units]")
{
    const StringView text =
        "[id]\n"
        "uuid = \"00000000-0000-0000-0000-000000000000\"\n"
        "[cook]\n"
        "position_scale = 0.01\n";
    const auto opts = parse_mesh_cook_options(text);
    REQUIRE(std::fabs(opts.position_scale - 0.01F) < kAbsTol);
}

TEST_CASE("mesh_cook_options: reads scientific notation 1e-3 (mm -> m)", "[cooker][units]")
{
    const StringView text =
        "[cook]\n"
        "position_scale = 1e-3\n";
    const auto opts = parse_mesh_cook_options(text);
    REQUIRE(std::fabs(opts.position_scale - 0.001F) < kAbsTol);
}

TEST_CASE("mesh_cook_options: tolerates Windows CRLF line endings", "[cooker][units]")
{
    const StringView text = "[cook]\r\nposition_scale = 0.0254\r\n"; // in -> m
    const auto opts = parse_mesh_cook_options(text);
    REQUIRE(std::fabs(opts.position_scale - 0.0254F) < kAbsTol);
}

TEST_CASE("mesh_cook_options: rejects non-positive scale", "[cooker][units]")
{
    const StringView text =
        "[cook]\n"
        "position_scale = -1.0\n";
    const auto opts = parse_mesh_cook_options(text);
    REQUIRE(opts.position_scale == 1.0F); // fallback
}

TEST_CASE("mesh_cook_options: rejects zero scale", "[cooker][units]")
{
    const StringView text =
        "[cook]\n"
        "position_scale = 0\n";
    const auto opts = parse_mesh_cook_options(text);
    REQUIRE(opts.position_scale == 1.0F);
}

TEST_CASE("mesh_cook_options: ignores key outside [cook] section", "[cooker][units]")
{
    const StringView text =
        "[id]\n"
        "position_scale = 99.0\n"; // wrong section
    const auto opts = parse_mesh_cook_options(text);
    REQUIRE(opts.position_scale == 1.0F);
}

TEST_CASE("mesh_cook_options: strips inline comments", "[cooker][units]")
{
    const StringView text =
        "[cook]\n"
        "position_scale = 0.5  # half scale, blender cm export\n";
    const auto opts = parse_mesh_cook_options(text);
    REQUIRE(std::fabs(opts.position_scale - 0.5F) < kAbsTol);
}

TEST_CASE("mesh_cook_options: last [cook] key wins on duplicate", "[cooker][units]")
{
    const StringView text =
        "[cook]\n"
        "position_scale = 0.01\n"
        "position_scale = 0.001\n";
    const auto opts = parse_mesh_cook_options(text);
    REQUIRE(std::fabs(opts.position_scale - 0.001F) < kAbsTol);
}

TEST_CASE("mesh_cook_options: section toggling -- [other] disables [cook] tail", "[cooker][units]")
{
    const StringView text =
        "[cook]\n"
        "position_scale = 0.01\n"
        "[other]\n"
        "position_scale = 99.0\n";
    const auto opts = parse_mesh_cook_options(text);
    REQUIRE(std::fabs(opts.position_scale - 0.01F) < kAbsTol);
}

TEST_CASE("mesh_cook_options: SI invariant -- default scale yields 1.0 (no normalization)",
          "[cooker][units][si]")
{
    // ADR-0078: glTF positions are SI meters by spec. The default cook
    // path must therefore be a pure pass-through (scale == 1.0F exactly,
    // no FP drift introduced by the cooker on conformant assets).
    const auto opts = parse_mesh_cook_options(StringView{});
    REQUIRE(opts.position_scale == 1.0F);
    constexpr float sample_positions[3] = {1.25F, -3.5F, 0.125F};
    for (float v : sample_positions)
    {
        REQUIRE(v * opts.position_scale == v); // bit-exact pass-through
    }
}
