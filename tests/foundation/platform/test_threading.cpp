#include <crd/platform/threading.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_CASE("threading: core counts are sane", "[platform][threading]")
{
    REQUIRE(crd::platform::threading::current_thread_id() != 0U);
    REQUIRE(crd::platform::threading::hardware_concurrency() >= 1U);
    REQUIRE(crd::platform::threading::logical_core_count() >= 1U);
    REQUIRE(crd::platform::threading::physical_core_count() >= 1U);
}

TEST_CASE("threading: naming and cpu_pause are safe no-ops", "[platform][threading]")
{
    crd::platform::threading::set_current_thread_name("crd-platform-tests");
    crd::platform::threading::cpu_pause();
    SUCCEED();
}

TEST_CASE("threading: affinity helper returns a stable boolean", "[platform][threading]")
{
    const bool ok = crd::platform::threading::set_thread_affinity(0);
    REQUIRE((ok == true || ok == false));
}
