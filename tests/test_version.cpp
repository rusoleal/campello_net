#include <campello_net/version.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace systems::leal::campello_net;

TEST_CASE("Version string is correct", "[version]") {
    REQUIRE(version_string() == "0.1.0");
}

TEST_CASE("Version components are correct", "[version]") {
    REQUIRE(version_major() == 0);
    REQUIRE(version_minor() == 1);
    REQUIRE(version_patch() == 0);
}
