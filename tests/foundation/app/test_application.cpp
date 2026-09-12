#include <crd/app/app.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cstdlib>

namespace
{
[[nodiscard]] bool headless_requested() noexcept
{
    const char* v = std::getenv("CRD_PLATFORM_HEADLESS");
    return v != nullptr && v[0] == '1';
}

class ClosingLayer final : public crd::app::Layer
{
public:
    explicit ClosingLayer(crd::app::Application& app) : Layer("ClosingLayer"), m_app(app) {}

    void on_attach() override { ++attach_count; }
    void on_detach() override { ++detach_count; }
    void on_update(crd::f64 /*dt*/) override
    {
        ++update_count;
        m_app.close();
    }

    int attach_count = 0;
    int detach_count = 0;
    int update_count = 0;

private:
    crd::app::Application& m_app;
};
} // namespace

TEST_CASE("Application creates invisible window and ticks once", "[app][application]")
{
    if (headless_requested())
    {
        SUCCEED("CRD_PLATFORM_HEADLESS=1, skipping app/window test");
        return;
    }

    crd::app::ApplicationDesc desc;
    desc.window.visible = false;
    desc.window.title = crd::containers::String("crd-app-tests");

    crd::app::Application app(desc);
    REQUIRE(app.is_valid());

    auto layer = std::make_unique<ClosingLayer>(app);
    auto* raw = layer.get();
    app.push_layer(std::move(layer));
    REQUIRE(raw->attach_count == 1);

    REQUIRE_FALSE(app.tick());
    REQUIRE(raw->update_count == 1);
}
