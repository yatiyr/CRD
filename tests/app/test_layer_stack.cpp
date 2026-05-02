#include <crd/app/layer_stack.hpp>

#include <catch2/catch_test_macros.hpp>

namespace
{
class TestLayer final : public crd::app::Layer
{
public:
    explicit TestLayer(crd::containers::StringView name) : Layer(name) {}
};
} // namespace

TEST_CASE("LayerStack preserves layer/overlay ordering", "[app][layer]")
{
    TestLayer layer_a("LayerA");
    TestLayer layer_b("LayerB");
    TestLayer overlay_a("OverlayA");

    crd::app::LayerStack stack;
    stack.push_layer(&layer_a);
    stack.push_layer(&layer_b);
    stack.push_overlay(&overlay_a);

    REQUIRE(stack.size() == 3U);
    auto it = stack.begin();
    REQUIRE((*it++)->name() == "LayerA");
    REQUIRE((*it++)->name() == "LayerB");
    REQUIRE((*it++)->name() == "OverlayA");

    auto rit = stack.rbegin();
    REQUIRE((*rit++)->name() == "OverlayA");
    REQUIRE((*rit++)->name() == "LayerB");
    REQUIRE((*rit++)->name() == "LayerA");
}

TEST_CASE("LayerStack pop respects layer and overlay partitions", "[app][layer]")
{
    TestLayer layer("Layer");
    TestLayer overlay("Overlay");

    crd::app::LayerStack stack;
    stack.push_layer(&layer);
    stack.push_overlay(&overlay);
    stack.pop_layer(&overlay);
    REQUIRE(stack.size() == 2U);
    stack.pop_overlay(&overlay);
    REQUIRE(stack.size() == 1U);
    stack.pop_layer(&layer);
    REQUIRE(stack.size() == 0U);
}
