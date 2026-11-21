// SPDX-License-Identifier: AGPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/SLA/SupportPointGenerator.hpp"
#include <tbb/task_arena.h>

#include "libslic3r/SLA/SupportOverhangSafetyOffset.hpp"

using namespace Slic3r;
using namespace Slic3r::sla;

namespace {
ExPolygon rectangle(coord_t x, coord_t y, coord_t width, coord_t height)
{
    return ExPolygon(Polygon({Point(x,y),Point(x+width,y),Point(x+width,y+height),Point(x,y+height)}));
}

void same_derived_part(const LayerPart &actual, const LayerPart &expected)
{
    CHECK(*actual.shape == *expected.shape);
    CHECK(actual.samples == expected.samples);
    CHECK(actual.extend_shape == expected.extend_shape);
    REQUIRE(actual.prev_parts.size() == expected.prev_parts.size());
    for (size_t i=0;i<actual.prev_parts.size();++i)
        CHECK(*actual.prev_parts[i]->shape == *expected.prev_parts[i]->shape);
    REQUIRE(actual.peninsulas.size() == expected.peninsulas.size());
    for (size_t i=0;i<actual.peninsulas.size();++i) {
        CHECK(actual.peninsulas[i].unsuported_area == expected.peninsulas[i].unsuported_area);
        CHECK(actual.peninsulas[i].is_outline == expected.peninsulas[i].is_outline);
    }
}
}

TEST_CASE("Prepared safety paths preserve ordered clipping including holes and multiplicity", "[sla_overhang_safety]")
{
    ExPolygon ring=rectangle(-10000,-10000,20000,20000);
    auto hole=rectangle(-5000,-5000,10000,10000).contour;
    hole.reverse();ring.holes.push_back(hole);
    ExPolygon concave(Polygon({Point(-9000,-8000),Point(9000,-8000),Point(9000,8000),
        Point(3000,8000),Point(3000,-2000),Point(-3000,-2000),Point(-3000,8000),Point(-9000,8000)}));
    auto reversed=ring;reversed.contour.reverse();reversed.holes.front().reverse();
    auto duplicates=concave;duplicates.contour.points.insert(duplicates.contour.points.begin()+2,duplicates.contour.points[1]);
    const std::vector<ExPolygons> clips={ {}, {ring}, {concave}, {duplicates}, {reversed},
        {rectangle(0,0,1,1)}, {ring,concave}, {concave,ring}, {ring,ring}, {ring,reversed},
        {rectangle(-1000000,-1000000,100,100)}, {rectangle(1000000,1000000,100,100)} };
    const ExPolygons subjects={ring,concave,rectangle(-20000,-20000,40000,40000),rectangle(-5000,-5000,10000,10000),
        rectangle(9990,-100,100,200),rectangle(-1000000,-1000000,20,20)};
    for (size_t i=0;i<clips.size();++i) {
        CAPTURE(i);
        const auto original=clips[i];
        const auto prepared=prepare_support_safety_offset(clips[i]);
        const auto retained=prepared;
        for (const auto &subject:subjects) {
            const auto expected=diff_ex(subject,clips[i],ApplySafetyOffset::Yes);
            CHECK(diff_ex(subject,prepared,ApplySafetyOffset::No) == expected);
            CHECK(prepared == retained);
        }
        CHECK(clips[i] == original);
    }
}

TEST_CASE("Repeated overhang sources match independent preparation of each upper part", "[sla_overhang_reuse]")
{
    const coord_t mm=scaled<coord_t>(1.);
    PrepareSupportConfig config;config.minimal_bounding_sphere_radius=0;
    auto left=rectangle(0,0,12*mm,10*mm);
    auto right=rectangle(12*mm,0,12*mm,10*mm);
    auto hole=rectangle(4*mm,4*mm,2*mm,2*mm).contour;
    hole.reverse();left.holes.push_back(hole);
    const std::vector<ExPolygons> lower_variants={{left},{left,right},{right,left},{left,left}};
    const ExPolygons upper={rectangle(mm,8*mm,2*mm,4*mm),rectangle(7*mm,8*mm,2*mm,4*mm),
        rectangle(11*mm,mm,2*mm,2*mm),rectangle(11*mm,6*mm,2*mm,2*mm)};
    for (size_t i=0;i<lower_variants.size();++i) {
        CAPTURE(i);
        const std::vector<ExPolygons> slices={lower_variants[i],upper};
        const auto original=slices;
        const auto batch=prepare_generator_data(std::vector<ExPolygons>(slices),{.05f,.1f},config);
        REQUIRE(batch.layers.size()==2);
        REQUIRE(batch.layers[1].parts.size()==upper.size());
        for (size_t part=0;part<upper.size();++part) {
            const auto single=prepare_generator_data({lower_variants[i],{upper[part]}},{.05f,.1f},config);
            REQUIRE(single.layers.size()==2);
            REQUIRE(single.layers[1].parts.size()==1);
            same_derived_part(batch.layers[1].parts[part],single.layers[1].parts.front());
        }
        CHECK(slices==original);
    }
}

TEST_CASE("Cancelling repeated overhang preparation leaves the next run independent", "[sla_overhang_reuse]")
{
    const coord_t mm=scaled<coord_t>(1.);
    PrepareSupportConfig config;config.minimal_bounding_sphere_radius=0;
    const ExPolygons lower={rectangle(0,0,20*mm,10*mm)};
    const ExPolygons upper={rectangle(mm,8*mm,2*mm,4*mm),rectangle(6*mm,8*mm,2*mm,4*mm),rectangle(11*mm,8*mm,2*mm,4*mm)};
    const std::vector<ExPolygons> input={lower,upper};
    tbb::task_arena serial(1);
    serial.execute([&] {
        size_t checkpoints=0;
        const auto expected=prepare_generator_data(std::vector<ExPolygons>(input),{.05f,.1f},config,[&]{++checkpoints;});
        REQUIRE(checkpoints>0);
        struct Cancelled {};
        for (size_t stop=1;stop<=checkpoints;++stop) {
            CAPTURE(stop);
            size_t observed=0;
            CHECK_THROWS_AS(prepare_generator_data(std::vector<ExPolygons>(input),{.05f,.1f},config,
                [&]{if (++observed==stop) throw Cancelled{};}),Cancelled);
            CHECK(observed==stop);
        }
        const auto after=prepare_generator_data(std::vector<ExPolygons>(input),{.05f,.1f},config);
        REQUIRE(after.layers.size()==expected.layers.size());
        for (size_t i=0;i<after.layers.size();++i) {
            REQUIRE(after.layers[i].parts.size()==expected.layers[i].parts.size());
            for (size_t j=0;j<after.layers[i].parts.size();++j)
                same_derived_part(after.layers[i].parts[j],expected.layers[i].parts[j]);
        }
    });
}
