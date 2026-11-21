// SPDX-License-Identifier: AGPL-3.0-or-later
#include "libslic3r/SLA/ExposureFinderDraft.hpp"
#include "libslic3r/PrintConfig.hpp"
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <algorithm>
using namespace Slic3r;
using namespace Slic3r::sla;
TEST_CASE("Finder draft integer lists distinguish canonical geometry from stored brightness", "[sla_exposure_finder_draft]") {
    CHECK(parse_exposure_finder_features("9,1;5 1\t9")==std::vector<unsigned>{1,5,9});
    CHECK(parse_exposure_finder_features("").empty());
    CHECK(parse_exposure_finder_rings("60:4;20:2 20:2")==std::vector<std::array<unsigned,2>>{{20,2},{60,4}});
    CHECK(parse_exposure_finder_brightness("127,255;127 0")==std::vector<uint8_t>{127,255,127,0});
    CHECK(parse_exposure_finder_brightness("0 0")==std::vector<uint8_t>{0,0});
    CHECK(parse_exposure_finder_brightness(" ").empty());
    for(const std::string text:{"-1","+1","1.5","0","501","99999999999999999999999999999999999999999999","1,,2","1,","1 ą","1:2"}) {
        INFO(text);CHECK_THROWS(parse_exposure_finder_features(text));
    }
    for(const std::string text:{"256","-1","true","127.0","1; ;2"}) {INFO(text);CHECK_THROWS(parse_exposure_finder_brightness(text));}
    for(const std::string text:{"20","20:0","0:2","20:2:3","20:2 20:3","501:2","20:-1"}) {INFO(text);CHECK_THROWS(parse_exposure_finder_rings(text));}
    CHECK_THROWS(parse_exposure_finder_features(std::string("1\0 2",4)));
    CHECK_THROWS(parse_exposure_finder_features(std::string(65537,'1')));
}
TEST_CASE("Finder manual CSV retains invalid rows duplicate order and disabled brightness", "[sla_exposure_finder_draft]") {
    const auto rows=parse_exposure_finder_manual_rows(" .100,20,3,127\r\n\n.0504,15.225,2.675,255\n.05,15.225,2.675,255\n-.001,0,-1,0\n");
    REQUIRE(rows.size()==4);CHECK(rows[0]==ExposureFinderTableRow{.1,20,3,127});
    CHECK(rows[1]==ExposureFinderTableRow{.0504,15.225,2.675,255});
    CHECK(rows[2]==ExposureFinderTableRow{.05,15.225,2.675,255});
    CHECK(rows[3]==ExposureFinderTableRow{-.001,0,-1,0});CHECK_FALSE(rows[3].valid());
    CHECK(parse_exposure_finder_manual_rows("\n \r\n").empty());
    for(const std::string text:{".05,15,2",".05,15,2,255,0","height,bottom,normal,brightness",".05,nan,2,255",".05,inf,2,255",".05,1e300,2,255",".05,15,2,256",".05,15,2,-1",".05,15,2,127.5",".05,15,,255",".05,15,2,255ą"}) {
        INFO(text);CHECK_THROWS(parse_exposure_finder_manual_rows(text));
    }
}
namespace {
DynamicPrintConfig finder_draft_config() {
    DynamicPrintConfig config;config.apply(SLAFullPrintConfig{});
    config.option("printer_technology")->setInt(ptSLA);config.option("display_orientation")->setInt(sladoLandscape);
    config.set("sla_archive_format","uvj");config.set("display_pixels_x",2048);config.set("display_pixels_y",1536);
    config.set("display_width",100.);config.set("display_height",75.);config.set("max_print_height",10.);return config;
}
ExposureFinderGenerationParameters finder_draft_request() {
    ExposureFinderGenerationParameters request;request.recipe.table_parameters.tests=1;
    request.recipe.rows=generate_exposure_finder_table(request.recipe.table_parameters);
    ExposureFinderScheduleParameters schedule;schedule.base_height=.05;schedule.features_height=.1;schedule.bottom_layers=0;
    schedule.multiple_brightness=true;schedule.brightness={127,255,127,0};schedule.base_mode=ExposureFinderBaseMode::Custom;
    schedule.custom_base_exposure=3.25;request.recipe.schedule=schedule;
    request.pattern.holes={1,5,9};request.pattern.bars={2,4,12};request.pattern.bullseyes={{20,2}};
    request.pattern.text="Łódź / Привет";request.pattern.font_face=3;request.pattern.feature_margin_mm=.245;request.pattern.text_scale=.805;
    return request;
}
}
TEST_CASE("Finder generation preflight reports actual fits and source schedule without full masks", "[sla_exposure_finder_draft]") {
    auto config=finder_draft_config();auto request=finder_draft_request();
    const auto plan=plan_exposure_finder_generation(config,request);
    CHECK(plan.geometry.columns==2048);CHECK(plan.geometry.rows==1536);
    CHECK(plan.spatial_parts==4);CHECK(plan.exposure_records==5);CHECK(plan.physical_layers==3);CHECK(plan.max_z==.15);
    CHECK(plan.requested_pattern.text=="Łódź / Привет");CHECK(plan.requested_pattern.feature_margin_mm==.245);
    CHECK(plan.effective_pattern.feature_margin_mm==.24);CHECK(plan.effective_pattern.text_scale==.8);
    CHECK(plan.cell_columns>0);CHECK(plan.cell_rows>0);CHECK(plan.grid_columns*plan.grid_rows>=4);
    CHECK(plan.cell_width_mm==Catch::Approx(plan.cell_columns*100./2048));
    CHECK(plan.cell_height_mm==Catch::Approx(plan.cell_rows*75./1536));
    CHECK(plan.raster_pixels==uint64_t(2048)*1536);CHECK(plan.logical_mask_bytes==uint64_t(2048)*1536*5);
    CHECK(plan.encoded_mask_limit_bytes==(uint64_t(1)<<30));
    CHECK_FALSE(describe_exposure_finder_generation_plan(plan).empty());
    size_t previous=0;bool complete=false,mutated=false;
    const auto frozen=plan_exposure_finder_generation(config,request,[&](size_t done,size_t total){
        CHECK(done>=previous);CHECK(total==100);previous=done;complete=done==100;
        if(!mutated){mutated=true;request.pattern.text.clear();request.recipe.rows.clear();config.set("display_pixels_x",1);}return true;
    });
    CHECK(complete);CHECK(frozen.cell_columns==plan.cell_columns);CHECK(frozen.exposure_records==5);
    CHECK(frozen.requested_pattern.text=="Łódź / Привет");
    CHECK_THROWS_AS(plan_exposure_finder_generation(finder_draft_config(),finder_draft_request(),[](size_t,size_t){return false;}),LayerOperationCancelled);
}
TEST_CASE("Finder preflight rejects invalid printer motion format bounds and footprint", "[sla_exposure_finder_draft]") {
    const auto good=finder_draft_config();const auto request=finder_draft_request();
    for(int defect=0;defect<8;++defect) {
        INFO(defect);auto config=good;auto bad=request;
        if(defect==0)config.set("sla_archive_format","SL1SVG");
        if(defect==1)config.set("sla_archive_format","unknown");
        if(defect==2)config.set("max_print_height",.1);
        if(defect==3)config.set("display_pixels_x",0);
        if(defect==4){config.set("sla_lift_height",5.);config.set("sla_lift_speed",0.);}
        if(defect==5)bad.layout.cell_columns=1;
        if(defect==6)bad.layout.margin_x_mm=100.;
        if(defect==7)bad.recipe.schedule.reset();
        CHECK_THROWS(plan_exposure_finder_generation(config,bad));
    }
    auto portrait=good;portrait.option("display_orientation")->setInt(sladoPortrait);
    const auto rotated=plan_exposure_finder_generation(portrait,request);
    CHECK(rotated.geometry.columns==1536);CHECK(rotated.geometry.rows==2048);
    CHECK(rotated.geometry.width_mm==75.);CHECK(rotated.geometry.height_mm==100.);
}

TEST_CASE("Finder preflight resolves repeated motion before cells and freezes callback ownership", "[sla_exposure_finder_draft]") {
    auto config=finder_draft_config();auto request=finder_draft_request();
    for(const char *prefix:{"sla_","sla_bottom_"})
        for(const char *name:{"lift_height","lift_height2","retract_height2","lift_speed","lift_speed2","retract_speed","retract_speed2"})
            config.set(std::string(prefix)+name,0.);
    request.recipe.schedule->repeated_settings=true;request.recipe.schedule->repeated_lift_enabled=true;
    request.recipe.schedule->repeated_lift_height=.5;
    size_t furthest=0;
    CHECK_THROWS(plan_exposure_finder_generation(config,request,[&](size_t done,size_t){furthest=std::max(furthest,done);return true;}));
    CHECK(furthest<35); // Cell-plane rendering starts at phase35.
    request.recipe.schedule->repeated_lift_height=0.;
    const auto stationary=plan_exposure_finder_generation(config,request);
    CHECK(stationary.exposure_records==5);
    LayerProgress callback;size_t calls=0,replacement_calls=0;
    callback=[&](size_t,size_t){++calls;callback=[&](size_t,size_t){++replacement_calls;return false;};return true;};
    CHECK_NOTHROW(plan_exposure_finder_generation(config,request,callback));
    CHECK(calls>1);CHECK(replacement_calls==0);
}
TEST_CASE("Finder manual normalization is explicit and leaves raw request untouched", "[sla_exposure_finder_draft]") {
    auto request=finder_draft_request();request.recipe.table_origin=ExposureFinderTableOrigin::Manual;
    request.recipe.rows=parse_exposure_finder_manual_rows(".0504,15.225,2.675,255\n.0504,15.225,2.675,255\n-.001,0,-1,0\n");
    const auto original=request.recipe.rows;REQUIRE(original.size()==3);
    CHECK(original[0]==original[1]);CHECK(original[0].layer_height==.0504);CHECK_FALSE(original[2].valid());
    CHECK_THROWS(plan_exposure_finder_generation(finder_draft_config(),request));
    CHECK(request.recipe.rows==original);
    auto normalized=request;normalized.recipe.rows=normalize_exposure_finder_table(request.recipe.rows);
    CHECK(normalized.recipe.rows[0].layer_height==.05);CHECK(normalized.recipe.rows[0].normal_exposure==2.68);
    CHECK(normalized.recipe.rows[0]==normalized.recipe.rows[1]);CHECK_FALSE(normalized.recipe.rows[2].valid());
    CHECK_NOTHROW(plan_exposure_finder_generation(finder_draft_config(),normalized));
    CHECK(request.recipe.rows==original);
}
