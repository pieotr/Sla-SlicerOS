// SPDX-License-Identifier: AGPL-3.0-or-later
#include "libslic3r/SLA/ExposurePositionSchedule.hpp"
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <limits>
using namespace Slic3r;
using namespace Slic3r::sla;

TEST_CASE("Exposure records explicitly group at one physical height", "[sla_exposure_positions]") {
    const std::vector<double> positions{.05, .05, .05, .1, .1, .15};
    CHECK_THROWS_AS(group_exposure_positions(positions), RuntimeError);
    const auto groups = group_exposure_positions(positions, ExposurePositionPolicy::RepeatedExposureHeights);
    REQUIRE(groups.size() == 3);
    CHECK(groups[0].first_record == 0); CHECK(groups[0].past_last_record == 3);
    CHECK(groups[1].first_record == 3); CHECK(groups[1].past_last_record == 5);
    CHECK(groups[2].first_record == 5); CHECK(groups[2].past_last_record == 6);
    for (const auto &group : groups) CHECK(group.thickness == Catch::Approx(.05));
    CHECK(groups[1].position_z == .1);
    const auto single = group_exposure_positions({.05,.1,.2});
    CHECK(single.size() == 3); CHECK(single.back().thickness == Catch::Approx(.1));
}

TEST_CASE("Exposure position validation rejects invalid or descending schedules", "[sla_exposure_positions]") {
    for (auto policy : {ExposurePositionPolicy::IncreasingLayers, ExposurePositionPolicy::RepeatedExposureHeights}) {
        CHECK_THROWS_AS(group_exposure_positions({},policy),RuntimeError);
        for (const auto &positions : std::vector<std::vector<double>>{
            {0}, {-.05}, {.05,.04}, {.05,0},
            {std::numeric_limits<double>::infinity()}, {std::numeric_limits<double>::quiet_NaN()}})
            CHECK_THROWS_AS(group_exposure_positions(positions,policy),RuntimeError);
    }
    CHECK_THROWS_AS(group_exposure_positions({.05},ExposurePositionPolicy(2)),RuntimeError);
    ExposurePositionValidator validator(ExposurePositionPolicy::RepeatedExposureHeights);
    REQUIRE(validator.append(.05));
    for(size_t i=1;i<100000;++i) REQUIRE_FALSE(validator.append(.05));
    CHECK_THROWS_AS(validator.append(.05),RuntimeError);
    ExposurePositionValidator recover;
    REQUIRE(recover.append(.05));
    CHECK_THROWS_AS(recover.append(.04),RuntimeError);
    REQUIRE(recover.append(.1));
}

TEST_CASE("Native position projection preserves exposure group boundaries", "[sla_exposure_positions]") {
    const auto repeated = ExposurePositionPolicy::RepeatedExposureHeights;
    const std::vector<double> source{.05,.05,.1,.1};
    const std::vector<double> native{double(float(.05)),double(float(.05)),double(float(.1)),double(float(.1))};
    CHECK_NOTHROW(require_exposure_position_projection(source,native,repeated,.001));
    CHECK_THROWS_AS(require_exposure_position_projection(source,{.05,.05001,.1,.1},repeated,.001),RuntimeError);
    CHECK_THROWS_AS(require_exposure_position_projection({.05,.050000001},{.05,.05},repeated,1e-10),RuntimeError);
    CHECK_THROWS_AS(require_exposure_position_projection(source,{.05,.05,.05,.05},repeated,.001),RuntimeError);
    CHECK_THROWS_AS(require_exposure_position_projection(source,{.05,.05,.101,.101},repeated,.001),RuntimeError);
    CHECK_THROWS_AS(require_exposure_position_projection(source,{.05},repeated,.001),RuntimeError);
    CHECK_THROWS_AS(require_exposure_position_projection(source,native,repeated,0),RuntimeError);
    CHECK_THROWS_AS(require_exposure_position_projection(source,native,repeated,std::numeric_limits<double>::infinity()),RuntimeError);
    CHECK_THROWS_AS(require_exposure_position_projection({.0001},{.0001},repeated,.001),RuntimeError);
    CHECK_THROWS_AS(require_exposure_position_projection({.05,.0501},{.05,.0501},repeated,.001),RuntimeError);
    CHECK_NOTHROW(require_exposure_position_projection({.05,.1},{double(float(.05)),double(float(.1))},ExposurePositionPolicy::IncreasingLayers,.001));
    ExposurePositionProjectionValidator recover(repeated,.001);
    REQUIRE(recover.append(.05,double(float(.05))));
    CHECK_THROWS_AS(recover.append(.05,.051),RuntimeError);
    REQUIRE_FALSE(recover.append(.05,double(float(.05))));
    REQUIRE(recover.append(.1,double(float(.1))));
}
