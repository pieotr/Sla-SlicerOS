#include <catch2/catch_test_macros.hpp>
#include <fstream>
#include <cstdlib>
#include <nlohmann/json.hpp>
#include "libslic3r/Format/AnycubicSLA.hpp"

// Opt-in local-source oracle contract: no UVtools runtime dependency and no
// hard-coded mirror of the native matrix. Capture --registry from the original
// development oracle and supply that evidence explicitly.
TEST_CASE("Anycubic version contract matches original extension API", "[.sla_anycubic_versions]")
{
    const char *path=std::getenv("SLA_ANYCUBIC_REFERENCE_REGISTRY");
    REQUIRE(path!=nullptr);
    std::ifstream in(path);REQUIRE(in.good());
    const auto registry=nlohmann::json::parse(in);
    size_t count=0;
    for(const auto &family:registry) {
        if(family.at("family")!="AnycubicFile") continue;
        for(const auto &entry:family.at("extensionVersions")) {
            const std::string extension=entry.at("extension");
            CAPTURE(extension);
            REQUIRE(Slic3r::anycubic_available_versions(extension)==entry.at("versions").get<std::vector<uint16_t>>());
            ++count;
        }
    }
    REQUIRE(count==22);
}

TEST_CASE("Anycubic preset selection rejects malformed and incompatible values", "[sla_anycubic_versions]")
{
    using namespace Slic3r;
    REQUIRE(anycubic_resolve_version("pm4n", "auto", 515) == 515);
    REQUIRE(anycubic_resolve_version(".PWC", "518", 516) == 518);
    for (const std::string invalid : {"", "automatic", "516junk", "+516", "0516", "517"}) {
        CAPTURE(invalid);
        REQUIRE_THROWS_AS(anycubic_resolve_version("pwmo", invalid, 1), std::invalid_argument);
    }
    REQUIRE_THROWS_AS(anycubic_resolve_version("unknown", "auto", 1), std::invalid_argument);
}
