// SPDX-License-Identifier: AGPL-3.0-or-later
#include "libslic3r/SLA/ExposureFinderRaster.hpp"
#include "libslic3r/SLA/CancellablePNGEncoder.hpp"
#include "libslic3r/SLA/ExposureFinderCell.hpp"
#include "libslic3r/SLA/ExposureFinderPattern.hpp"
#include "libslic3r/SLAPrint.hpp"
#include "libslic3r/SLA/ProjectArchive.hpp"
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <boost/filesystem.hpp>
#include <nlohmann/json.hpp>
#include <fstream>
#include <cstdlib>
#include <set>
#include <cfenv>
using namespace Slic3r;
using namespace Slic3r::sla;
TEST_CASE("ExposureFinder layout fits all cells in physical LCD margins", "[sla_exposure_raster]") {
    ExposureFinderLayoutParameters p;p.cell_columns=3;p.cell_rows=2;
    p.margin_x_mm=1.;p.margin_y_mm=.5;p.gap_x_mm=.5;p.gap_y_mm=.25;
    const auto layout=plan_exposure_finder_layout({20,12,10.,3.},8,p);
    REQUIRE(layout.origins.size()==8);
    CHECK(layout.origins[0]==ExposureFinderCellOrigin{2,3});
    CHECK(layout.origins[7]==ExposureFinderCellOrigin{14,6});
}
TEST_CASE("PNG cancellation never publishes an incomplete encoded raster", "[sla_exposure_raster]") {
    std::vector<uint8_t> pixels(80*40,127);bool called=false;
    CHECK_THROWS_AS(encode_png_raster(pixels.data(),80,40,1,[&]{called=true;throw LayerOperationCancelled{};}),LayerOperationCancelled);
    CHECK(called);
}

TEST_CASE("Freeze original PNG encoder byte output for raster regression", "[sla_png_baseline_capture]") {
    const char *path=std::getenv("SLA_PNG_BASELINE_OUTPUT");
    if(!path)return;
    const boost::filesystem::path folder(path);boost::filesystem::create_directories(folder);
    for(size_t components=1;components<=4;++components) {
        std::vector<uint8_t> pixels(31*17*components);
        for(size_t y=0;y<17;++y)for(size_t x=0;x<31;++x)for(size_t c=0;c<components;++c)
            pixels[(y*31+x)*components+c]=uint8_t((x*13+y*7+c*19)%256);
        const auto png=Slic3r::sla::PNGRasterEncoder{}(pixels.data(),31,17,components);REQUIRE(png.size()>0);
        const auto stem="components-"+std::to_string(components);
        {std::ofstream output((folder/(stem+".png")).string(),std::ios::binary);output.write(static_cast<const char *>(png.data()),std::streamsize(png.size()));}
        {std::ofstream output((folder/(stem+".raw")).string(),std::ios::binary);output.write(reinterpret_cast<const char *>(pixels.data()),std::streamsize(pixels.size()));}
    }
}

namespace {
ExposureFinderSchedule raster_schedule() {
    ExposureFinderRecipe recipe;
    recipe.table_parameters.tests=3;
    recipe.rows=generate_exposure_finder_table(recipe.table_parameters);
    ExposureFinderScheduleParameters parameters;
    parameters.base_height=.05;parameters.features_height=.05;parameters.bottom_layers=0;
    parameters.multiple_brightness=true;parameters.brightness={255,127};
    parameters.brightness_exclusion=ExposureFinderBrightnessExclusion::None;
    return plan_exposure_finder_schedule(recipe,parameters);
}
ExposureFinderCellRenderer analytic_cell() {
    return [](const ExposureFinderPart &,const ExposureFinderExposure &record,const std::function<void()> &check) {
        check();
        return png::ImageGreyscale{record.base?std::vector<uint8_t>(6,255):std::vector<uint8_t>{0,64,255,128,200,255},2,3};
    };
}
}
TEST_CASE("ExposureFinder composition preserves oriented pixels and AA doses", "[sla_exposure_raster]") {
    const bool portrait=GENERATE(false,true),center=GENERATE(false,true);
    const bool mirror_x=GENERATE(false,true),mirror_y=GENERATE(false,true);
    CAPTURE(portrait,center,mirror_x,mirror_y);
    const auto schedule=raster_schedule();
    REQUIRE(schedule.parts.size()==8);REQUIRE(schedule.exposures.size()==8);
    const LayerRasterGeometry geometry=portrait?LayerRasterGeometry{12,20,3.,10.}:LayerRasterGeometry{20,12,10.,3.};
    ExposureFinderLayoutParameters parameters;
    parameters.cell_columns=3;parameters.cell_rows=2;parameters.center=center;
    parameters.mirror_x=mirror_x;parameters.mirror_y=mirror_y;
    parameters.margin_x_mm=portrait?.25:1.;parameters.margin_y_mm=.5;
    parameters.gap_x_mm=portrait?.25:.5;parameters.gap_y_mm=portrait?.5:.25;
    const auto layout=plan_exposure_finder_layout(geometry,8,parameters);
    const auto document=compose_exposure_finder_layers(schedule,layout,analytic_cell());
    REQUIRE(document.layer_count()==8);
    nlohmann::json manifest=nlohmann::json::array();
    for(size_t record=0;record<8;++record) {
        const auto image=document.image(record);
        REQUIRE(image.cols==geometry.columns);REQUIRE(image.rows==geometry.rows);
        std::vector<uint8_t> expected(image.buf.size(),0);
        const auto &exposure=schedule.exposures[record];
        for(size_t i=0;i<exposure.parts.size();++i) {
            const size_t part=exposure.parts[i],grid_columns=portrait?2:4;
            const size_t x0=(center?2:(portrait?1:2))+(part%grid_columns)*4;
            const size_t y0=(center?(portrait?4:3):(portrait?1:2))+(part/grid_columns)*3;
            const uint8_t features[]{0,64,255,128,200,255};
            for(size_t y=0;y<2;++y)for(size_t x=0;x<3;++x) {
                size_t px=x0+x,py=y0+y;
                if(mirror_x)px=geometry.columns-1-px;
                if(mirror_y)py=geometry.rows-1-py;
                const int raw=exposure.base?255:features[y*3+x];
                expected[py*geometry.columns+px]=uint8_t(std::max(0,raw-(255-exposure.mask_brightness[i])));
            }
        }
        CHECK(image.buf==expected);
        if(const char *root=std::getenv("SLA_EXPOSURE_RASTER_FIXTURES")) {
            const auto folder=boost::filesystem::path(root)/(std::to_string(portrait)+std::to_string(center)+std::to_string(mirror_x)+std::to_string(mirror_y));
            boost::filesystem::create_directories(folder);
            const auto encoded=encode_png_raster(image.buf.data(),image.cols,image.rows,1);
            std::ofstream output((folder/(std::to_string(record)+".png")).string(),std::ios::binary);
            output.write(static_cast<const char *>(encoded.data()),std::streamsize(encoded.size()));
            manifest.push_back({{"base",exposure.base},{"parts",exposure.parts},{"brightness",exposure.mask_brightness}});
            std::ofstream meta((folder/"records.json").string());meta<<manifest.dump(2);
        }
    }
}
TEST_CASE("ExposureFinder refuses clipping and malformed schedules before rendering", "[sla_exposure_raster]") {
    auto schedule=raster_schedule();
    ExposureFinderLayoutParameters parameters;parameters.cell_columns=3;parameters.cell_rows=2;
    const LayerRasterGeometry geometry{20,12,10.,3.};
    auto layout=plan_exposure_finder_layout(geometry,8,parameters);
    SECTION("capacity") {CHECK_THROWS(plan_exposure_finder_layout(geometry,100,parameters));}
    SECTION("negative spacing") {parameters.gap_x_mm=-.1;CHECK_THROWS(plan_exposure_finder_layout(geometry,8,parameters));}
    SECTION("forged origins") {layout.origins[0].column=20;CHECK_THROWS(compose_exposure_finder_layers(schedule,layout,analytic_cell()));}
    SECTION("no renderer") {CHECK_THROWS(compose_exposure_finder_layers(schedule,layout,{}));}
    SECTION("duplicate part") {schedule.exposures[0].parts[1]=schedule.exposures[0].parts[0];CHECK_THROWS(compose_exposure_finder_layers(schedule,layout,analytic_cell()));}
    SECTION("wrong physical count") {++schedule.physical_layers;CHECK_THROWS(compose_exposure_finder_layers(schedule,layout,analytic_cell()));}
    SECTION("wrong cell") {
        CHECK_THROWS(compose_exposure_finder_layers(schedule,layout,[](const auto &,const auto &,const auto &){return png::ImageGreyscale{{255},1,1};}));
    }
    SECTION("atomic final cancellation and retry") {
        auto previous=compose_exposure_finder_layers(schedule,layout,analytic_cell());
        const auto image=previous.image(0).buf;
        CHECK_THROWS_AS(previous=compose_exposure_finder_layers(schedule,layout,analytic_cell(),[](size_t done,size_t total){return done!=total;}),LayerOperationCancelled);
        CHECK(previous.image(0).buf==image);
        CHECK(compose_exposure_finder_layers(schedule,layout,analytic_cell()).layer_count()==8);
    }
    SECTION("callbacks cannot mutate captured inputs") {
        auto renderer=analytic_cell();bool mutated=false;
        auto document=compose_exposure_finder_layers(schedule,layout,renderer,[&](size_t,size_t){
            if(!mutated){mutated=true;schedule.parts.clear();layout.origins.clear();renderer={};}return true;
        });
        CHECK(document.layer_count()==8);
    }
}
TEST_CASE("PNG preserves all component bytes across row cancellation", "[sla_exposure_raster]") {
    const size_t components=GENERATE(1,2,3,4);
    std::vector<uint8_t> pixels(31*17*components);
    for(size_t y=0;y<17;++y)for(size_t x=0;x<31;++x)for(size_t c=0;c<components;++c)
        pixels[(y*31+x)*components+c]=uint8_t((x*13+y*7+c*19)%256);
    const auto before=pixels;
    auto output=encode_png_raster(pixels.data(),31,17,components);
    const auto bytes=std::vector<uint8_t>(static_cast<const uint8_t *>(output.data()),static_cast<const uint8_t *>(output.data())+output.size());
    for(size_t threshold:{size_t(1),size_t(2),size_t(10),size_t(19)}) {
        size_t calls=0;
        CHECK_THROWS_AS(output=encode_png_raster(pixels.data(),31,17,components,[&]{if(++calls==threshold)throw LayerOperationCancelled{};}),LayerOperationCancelled);
        CHECK(calls==threshold);CHECK(pixels==before);
        CHECK(std::vector<uint8_t>(static_cast<const uint8_t *>(output.data()),static_cast<const uint8_t *>(output.data())+output.size())==bytes);
    }
    size_t calls=0;
    const auto retry=encode_png_raster(pixels.data(),31,17,components,[&]{++calls;});CHECK(calls==19);CHECK(retry.size()==bytes.size());
    if(const char *root=std::getenv("SLA_PNG_REFERENCE_FOLDER")) {
        const auto name=boost::filesystem::path(root)/("components-"+std::to_string(components)+".png");
        std::ifstream reference(name.string(),std::ios::binary);REQUIRE(reference.good());
        const std::vector<uint8_t> expected((std::istreambuf_iterator<char>(reference)),std::istreambuf_iterator<char>());
        CHECK(bytes==expected);
    }
    if(const char *root=std::getenv("SLA_NEW_PNG_FIXTURES")) {
        boost::filesystem::create_directories(root);
        std::ofstream target((boost::filesystem::path(root)/("components-"+std::to_string(components)+".png")).string(),std::ios::binary);
        target.write(static_cast<const char *>(retry.data()),std::streamsize(retry.size()));
    }
}
TEST_CASE("PNG row callback exceptions release their active exception owner", "[sla_exposure_raster]") {
    struct Probe {std::shared_ptr<int> owner;};
    std::weak_ptr<int> weak;
    std::vector<uint8_t> pixels(32*32,100);
    for(size_t attempt=0;attempt<8;++attempt) {
        size_t calls=0;
        CHECK_THROWS_AS(encode_png_raster(pixels.data(),32,32,1,[&]{
            if(++calls==10){auto owner=std::make_shared<int>(42);weak=owner;throw Probe{owner};}
        }),Probe);
        CHECK(weak.expired());
    }
    CHECK(encode_png_raster(pixels.data(),32,32,1).size()>0);
}

TEST_CASE("ExposureFinder staircase shrinks by tested height and vanishes on final layer", "[sla_exposure_cell]") {
    png::ImageGreyscale base{std::vector<uint8_t>(12*7,255),7,12};
    png::ImageGreyscale features{std::vector<uint8_t>(12*7,0),7,12};
    ExposureFinderCellParameters p;p.base_height=.1;p.features_height=.2;
    p.staircase_width=10;p.staircase_thickness=2;
    auto renderer=make_exposure_finder_cell_renderer(base,features,p);
    ExposureFinderPart part{std::nullopt,.05,15,2,255,15,2};
    for(size_t i=1;i<=6;++i) {
        ExposureFinderExposure record{double(i)*.05,.05,2,false,i<=2,i-1,{0},{255},{},{}};
        // Use canonical decimal Z, as produced by the schedule.
        record.position_z=std::vector<double>{.05,.1,.15,.2,.25,.3}[i-1];
        auto actual=renderer(part,record,{});
        std::vector<uint8_t> expected(12*7,i<=2?255:0);
        if(i>=3 && i<6) {
            const size_t first=(i-3)*4;
            for(size_t y=0;y<2;++y)for(size_t x=first;x<10;++x)expected[y*12+x]=255;
        }
        CHECK(actual.buf==expected);
    }
    SECTION("single feature layer has no division by zero") {
        p.features_height=.05;
        renderer=make_exposure_finder_cell_renderer(base,features,p);
        CHECK(renderer(part,{.15,.05,2,false,false,2,{0},{255},{},{}},{}).buf==features.buf);
    }
    SECTION("captured planes remain independent") {
        base.buf.clear();features.buf.clear();p.staircase_width=1;
        CHECK(renderer(part,{.05,.05,2,false,true,0,{0},{255},{},{}},{}).buf==std::vector<uint8_t>(84,255));
    }
}
TEST_CASE("ExposureFinder erosion matches repeated Reflect101 kernels", "[sla_exposure_cell]") {
    const size_t width=GENERATE(1,2,7,19),height=GENERATE(1,3,11);
    const unsigned erosion=GENERATE(0,1,4,20),chamfer=GENERATE(0,1,6);
    png::ImageGreyscale plane{std::vector<uint8_t>(width*height),height,width};
    for(size_t i=0;i<plane.buf.size();++i)plane.buf[i]=uint8_t((i*71+i/width*43+113)%256);
    ExposureFinderCellParameters p;p.bottom_erosion=erosion;p.chamfer_layers=chamfer;
    const auto renderer=make_exposure_finder_cell_renderer(plane,plane,p);
    auto expected=plane;
    const auto reflect=[](int position,int length) {
        if(length==1)return 0;
        while(position<0 || position>=length)position=position<0?-position:2*length-position-2;
        return position;
    };
    const unsigned iterations=erosion+(chamfer>1?chamfer-1:0);
    for(unsigned pass=0;pass<iterations;++pass) {
        auto before=expected.buf;
        for(size_t y=0;y<height;++y)for(size_t x=0;x<width;++x) {
            uint8_t value=255;
            for(int dy=-1;dy<=1;++dy)for(int dx=-1;dx<=1;++dx)
                value=std::min(value,before[size_t(reflect(int(y)+dy,int(height)))*width+size_t(reflect(int(x)+dx,int(width)))]);
            expected.buf[y*width+x]=value;
        }
    }
    ExposureFinderPart part{std::nullopt,.05,15,2,255,15,2};
    const auto actual=renderer(part,{.05,.05,15,true,true,0,{0},{255},{},{}},{});
    CHECK(actual.buf==expected.buf);
    if(const char *root=std::getenv("SLA_EXPOSURE_CELL_FIXTURES")) {
        const auto folder=boost::filesystem::path(root);boost::filesystem::create_directories(folder);
        const auto stem=std::to_string(width)+"-"+std::to_string(height)+"-"+std::to_string(erosion)+"-"+std::to_string(chamfer);
        nlohmann::json value{{"columns",width},{"rows",height},{"input",plane.buf},{"bottom_erosion",erosion},{"chamfer_layers",chamfer},{"expected",actual.buf}};
        std::ofstream(folder/(stem+".json"))<<value.dump();
    }
}
TEST_CASE("ExposureFinder cell cancellation is atomic through the compositor", "[sla_exposure_cell]") {
    const auto schedule=raster_schedule();
    ExposureFinderLayoutParameters layout_parameters;layout_parameters.cell_columns=3;layout_parameters.cell_rows=2;
    const auto layout=plan_exposure_finder_layout({20,12,10.,3.},8,layout_parameters);
    png::ImageGreyscale plane{{10,20,30,40,50,60},2,3};
    ExposureFinderCellParameters p;p.base_height=.05;p.features_height=.05;p.bottom_erosion=2;p.chamfer_layers=5;
    const auto renderer=make_exposure_finder_cell_renderer(plane,plane,p);
    auto document=compose_exposure_finder_layers(schedule,layout,renderer);
    const auto previous=document.image(0).buf;
    for(size_t threshold:{size_t(1),size_t(20),size_t(60),size_t(100)}) {
        size_t checks=0;
        CHECK_THROWS_AS(document=compose_exposure_finder_layers(schedule,layout,renderer,[&](size_t,size_t){return ++checks!=threshold;}),LayerOperationCancelled);
        CHECK(document.image(0).buf==previous);
    }
    CHECK(compose_exposure_finder_layers(schedule,layout,renderer).layer_count()==8);
    p.staircase_width=100;
    CHECK_THROWS(make_exposure_finder_cell_renderer(plane,plane,p));
    p={};p.features_height=-1;CHECK_THROWS(make_exposure_finder_cell_renderer(plane,plane,p));
}

TEST_CASE("ExposureFinder cell rejects inconsistent pattern and exposure geometry", "[sla_exposure_cell]") {
    png::ImageGreyscale plane{std::vector<uint8_t>(24,255),4,6};
    ExposureFinderCellParameters p;
    SECTION("height rounds to no printable layers") {
        p.base_height=.001;p.features_height=.001;
        CHECK_THROWS(make_exposure_finder_cell_renderer(plane,plane,p));
    }
    SECTION("plane buffer mismatch") {
        plane.buf.pop_back();CHECK_THROWS(make_exposure_finder_cell_renderer(plane,plane,p));
    }
    SECTION("staircase width without thickness") {
        p.staircase_width=3;CHECK_THROWS(make_exposure_finder_cell_renderer(plane,plane,p));
    }
    SECTION("erosion outside supported range") {
        p.bottom_erosion=256;CHECK_THROWS(make_exposure_finder_cell_renderer(plane,plane,p));
    }
    SECTION("inconsistent base flag") {
        const auto renderer=make_exposure_finder_cell_renderer(plane,plane,p);
        ExposureFinderPart part{std::nullopt,.05,15,2,255,15,2};
        CHECK_THROWS(renderer(part,{.05,.05,2,false,false,0,{0},{255},{},{}},{}));
    }
    SECTION("exposure above pattern height") {
        const auto renderer=make_exposure_finder_cell_renderer(plane,plane,p);
        ExposureFinderPart part{std::nullopt,.05,15,2,255,15,2};
        CHECK_THROWS(renderer(part,{.25,.05,2,false,false,4,{0},{255},{},{}},{}));
    }
    SECTION("exposure off tested height grid") {
        const auto renderer=make_exposure_finder_cell_renderer(plane,plane,p);
        ExposureFinderPart part{std::nullopt,.05,15,2,255,15,2};
        CHECK_THROWS(renderer(part,{.07,.05,2,false,true,1,{0},{255},{},{}},{}));
    }
}
TEST_CASE("ExposureFinder cell decimal staircase is independent of caller rounding", "[sla_exposure_cell]") {
    const int mode=GENERATE(FE_TONEAREST,FE_UPWARD,FE_DOWNWARD,FE_TOWARDZERO);
    const int original=std::fegetround();
    struct Restore {int mode;~Restore(){std::fesetround(mode);}} restore{original};
    REQUIRE(std::fesetround(mode)==0);
    png::ImageGreyscale plane{std::vector<uint8_t>(24,0),4,6};
    ExposureFinderCellParameters p;p.base_height=.06;p.features_height=.12;
    p.staircase_width=6;p.staircase_thickness=1;
    const auto renderer=make_exposure_finder_cell_renderer(plane,plane,p);
    ExposureFinderPart part{std::nullopt,.03,15,2,255,15,2};
    const auto image=renderer(part,{.12,.03,2,false,false,3,{0},{255},{},{}},{});
    CHECK(image.buf==std::vector<uint8_t>{0,0,255,255,255,255,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0});
    CHECK(std::fegetround()==mode);
}

TEST_CASE("Native ExposureFinder pattern planes match original UVtools", "[sla_exposure_pattern]") {
    const char *root=std::getenv("SLA_EXPOSURE_PATTERN_REFERENCE");
    if(!root){WARN("Set SLA_EXPOSURE_PATTERN_REFERENCE for the independent UVtools gate");return;}
    const boost::filesystem::path folder(root);
    std::ifstream input((folder/"cases.json").string());REQUIRE(input.good());
    const auto cases=nlohmann::json::parse(input);REQUIRE(cases.size()==60);
    for(const auto &fixture:cases) {
        const unsigned index=fixture.at("index");CAPTURE(index);
        ExposureFinderPatternParameters p;p.antialias=fixture.at("aa");p.font_face=fixture.at("font");
        if(fixture.at("holes").get<bool>())p.holes={1,2,5,6,9,20};
        p.square_holes=fixture.at("square");
        if(!fixture.at("bars").get<bool>())p.bars.clear();
        if(!fixture.at("bullseyes").get<bool>())p.bullseyes.clear();
        p.triangle_fence=index>=38 && index<40;p.invert_bullseye_quadrants=index!=37;
        p.text=fixture.at("text");
        p.feature_margin_mm=fixture.at("margin");p.bar_length_mm=fixture.at("bar_length");p.bar_spacing_mm=fixture.at("bar_spacing");
        p.text_scale=fixture.at("text_scale");p.text_thickness=fixture.at("text_thickness");
        const auto pattern=make_exposure_finder_pattern({2048,1536,100.,fixture.at("anisotropic").get<bool>()?60.:75.},p);
        CHECK(pattern.parameters.feature_margin_mm==fixture.at("normalized_margin").get<double>());
        CHECK(pattern.parameters.text_scale==fixture.at("normalized_scale").get<double>());
        CHECK(pattern.base.cols==fixture.at("columns").get<size_t>());
        CHECK(pattern.base.rows==fixture.at("rows").get<size_t>());
        CHECK(pattern.positive_marking==fixture.at("positive").get<std::array<int,2>>());
        CHECK(pattern.negative_marking==fixture.at("negative").get<std::array<int,2>>());
        for(unsigned plane=0;plane<2;++plane) {
            std::ifstream source((folder/(std::to_string(index)+"-"+std::to_string(plane)+".raw")).string(),std::ios::binary);
            REQUIRE(source.good());const std::vector<uint8_t> expected((std::istreambuf_iterator<char>(source)),{});
            const auto &actual=plane?pattern.features.buf:pattern.base.buf;
            REQUIRE(actual.size()==expected.size());
            size_t differences=0;for(size_t i=0;i<actual.size();++i)if(actual[i]!=expected[i])++differences;
            CHECK(differences==0);
        }
    }
}
TEST_CASE("Native ExposureFinder pattern produces finished LCD exposure masks", "[sla_exposure_pattern]") {
    ExposureFinderPatternParameters p;p.antialias=GENERATE(false,true);
    p.holes={1,2,5,6,9,20};p.square_holes=GENERATE(false,true);
    const LayerRasterGeometry geometry{2048,1536,100.,75.};
    auto pattern=make_exposure_finder_pattern(geometry,p);
    ExposureFinderRecipe recipe;recipe.table_parameters.tests=1;
    recipe.rows=generate_exposure_finder_table(recipe.table_parameters);
    ExposureFinderScheduleParameters settings;settings.base_height=.1;settings.features_height=.2;settings.bottom_layers=1;
    const auto schedule=plan_exposure_finder_schedule(recipe,settings);
    ExposureFinderLayoutParameters lp;lp.cell_columns=pattern.base.cols;lp.cell_rows=pattern.base.rows;
    const auto layout=plan_exposure_finder_layout(geometry,schedule.parts.size(),lp);
    ExposureFinderCellParameters cp;cp.base_height=.1;cp.features_height=.2;cp.bottom_erosion=1;cp.chamfer_layers=2;
    const auto renderer=make_exposure_finder_pattern_renderer(pattern,cp,false);
    const auto document=compose_exposure_finder_layers(schedule,layout,renderer);
    REQUIRE(document.layer_count()==schedule.exposures.size());
    const auto first=document.image(0),last=document.image(document.layer_count()-1);
    CHECK(first.cols==2048);CHECK(first.rows==1536);CHECK(first.buf!=last.buf);
    CHECK(std::count_if(last.buf.begin(),last.buf.end(),[](uint8_t value){return value!=0;})>0);
    if(const char *root=std::getenv("SLA_EXPOSURE_PATTERN_EXPORTS")) {
        for(const char *format:{"uvj","cxdlpv4"}) {
            const auto folder=boost::filesystem::path(root)/(std::to_string(p.antialias)+std::to_string(p.square_holes)+"-"+format);
            boost::filesystem::create_directories(folder);
            DynamicPrintConfig config;config.apply(SLAFullPrintConfig{});
            config.option("printer_technology")->setInt(ptSLA);config.option("display_orientation")->setInt(sladoLandscape);
            config.set("sla_archive_format",format);config.set("printer_model","CL-60");
            config.set("display_pixels_x",2048);config.set("display_pixels_y",1536);
            config.set("display_width",100.);config.set("display_height",75.);
            config.set("layer_height",.05);config.set("initial_layer_height",.05);config.set("max_print_height",10.);
            config.set("sla_bottom_layers",int(schedule.bottom_records));config.set("sla_transition_layers",0);
            config.set("gamma_correction",1.);config.set("exposure_time",2.);config.set("initial_exposure_time",15.);
            ExposureFinderGenerationParameters generation;generation.recipe=recipe;generation.recipe.schedule=settings;
            generation.pattern=p;generation.bottom_erosion=1;generation.chamfer_layers=2;
            auto job=create_exposure_finder_calibration(config,generation);
            save_layer_project((folder/"pattern.sla-project").string(),capture_generated_project(*job,*job->layer_document()));
            const auto restored=read_project_layers((folder/"pattern.sla-project").string());REQUIRE(restored);
            job=restore_generated_project(*restored);
            REQUIRE_NOTHROW(job->export_print((folder/(std::string("pattern.")+format)).string()));
        }
    }
    const auto before=last.buf;
    pattern.base.buf.clear();pattern.features.buf.clear();
    CHECK(compose_exposure_finder_layers(schedule,layout,renderer).image(document.layer_count()-1).buf==before);
}

TEST_CASE("Native ExposureFinder pattern snapshots and cancellation preserve ownership", "[sla_exposure_pattern]") {
    ExposureFinderPattern empty;
    CHECK_THROWS_AS(make_exposure_finder_pattern_renderer(empty,{},false),RuntimeError);
    png::ImageGreyscale empty_image;
    CHECK_THROWS_AS(make_exposure_finder_cell_renderer(empty_image,empty_image,{}),RuntimeError);
    ExposureFinderPatternParameters p;
    const LayerRasterGeometry geometry{2048,1536,100.,75.};
    const auto baseline=make_exposure_finder_pattern(geometry,p);
    bool mutated=false;
    const auto snapshot=make_exposure_finder_pattern(geometry,p,[&]{if(!mutated){mutated=true;p.bars.clear();p.bullseyes.clear();p.text.clear();}});
    CHECK(snapshot.base.buf==baseline.base.buf);CHECK(snapshot.features.buf==baseline.features.buf);
    CHECK_THROWS_AS(make_exposure_finder_pattern(geometry,baseline.parameters,[]{throw LayerOperationCancelled{};}),LayerOperationCancelled);
    p=baseline.parameters;p.holes={20,2};CHECK_THROWS(make_exposure_finder_pattern(geometry,p));
    p=baseline.parameters;p.font_face=32;CHECK_THROWS(make_exposure_finder_pattern(geometry,p));
    p=baseline.parameters;p.text=std::string(4097,'a');CHECK_THROWS(make_exposure_finder_pattern(geometry,p));
    p=baseline.parameters;p.feature_margin_mm=std::numeric_limits<double>::quiet_NaN();CHECK_THROWS(make_exposure_finder_pattern(geometry,p));
    p=baseline.parameters;p.bars.clear();p.bullseyes.clear();p.text.clear();CHECK_THROWS(make_exposure_finder_pattern(geometry,p));
}

TEST_CASE("Native ExposureFinder generator owns explicit per-record motion and cancellation", "[sla_exposure_pattern]") {
    const bool portrait=GENERATE(false,true),mixed_heights=GENERATE(false,true);
    const std::string format=GENERATE(std::string("uvj"),std::string("cxdlpv4"));
    DynamicPrintConfig config;config.apply(SLAFullPrintConfig{});
    config.option("printer_technology")->setInt(ptSLA);config.option("display_orientation")->setInt(portrait?sladoPortrait:sladoLandscape);
    config.set("sla_archive_format",format);config.set("printer_model","CL-60");config.set("display_pixels_x",2048);config.set("display_pixels_y",1536);
    config.set("display_width",100.);config.set("display_height",75.);config.set("max_print_height",10.);
    config.set("sla_lift_height",5.);config.set("sla_lift_height2",1.);config.set("sla_retract_height2",2.);
    config.set("sla_bottom_lift_height",7.);config.set("sla_bottom_lift_height2",2.);config.set("sla_bottom_retract_height2",3.);
    for(const char *prefix:{"sla_","sla_bottom_"})for(const char *key:{"lift_speed","lift_speed2","retract_speed","retract_speed2"})config.set(std::string(prefix)+key,100.);
    ExposureFinderGenerationParameters parameters;
    parameters.recipe.table_parameters.tests=1;
    parameters.recipe.table_parameters.multiple_layer_heights=mixed_heights;
    parameters.recipe.table_parameters.maximum_layer_height=.1;parameters.recipe.table_parameters.layer_height_step=.05;
    parameters.recipe.rows=generate_exposure_finder_table(parameters.recipe.table_parameters);
    ExposureFinderScheduleParameters settings;settings.base_height=.1;settings.features_height=.2;settings.bottom_layers=1;
    settings.repeated_settings=true;settings.repeated_lift_height=.5;settings.repeated_wait_before=.25;
    parameters.recipe.schedule=settings;
    const auto schedule=plan_exposure_finder_schedule(parameters.recipe,settings);
    auto job=create_exposure_finder_calibration(config,parameters);
    REQUIRE(job->is_raster_job());REQUIRE(job->layer_document()->fresh_job());
    const auto &fresh=*job->layer_document()->fresh_job();
    CHECK(fresh.producer==FreshLayerProducer::Calibration);
    CHECK(fresh.geometry.columns==(portrait?1536:2048));CHECK(fresh.geometry.rows==(portrait?2048:1536));
    REQUIRE(fresh.rows.size()==schedule.exposures.size());
    for(size_t i=0;i<schedule.exposures.size();++i) {
        const auto &record=schedule.exposures[i];const auto &row=fresh.rows[i];
        CHECK(row.at(LayerParameter::PositionZ)==record.position_z);CHECK(row.at(LayerParameter::Exposure)==record.exposure);
        CHECK(row.at(LayerParameter::LiftDistance)==(record.lift_height_override?*record.lift_height_override:(record.bottom?7.:5.)));
        CHECK(row.at(LayerParameter::LiftDistance2)==(record.lift_height_override?0.:(record.bottom?2.:1.)));
        CHECK(row.at(LayerParameter::RetractDistance2)==(record.lift_height_override?.5:(record.bottom?3.:2.)));
        if(record.wait_before_override)CHECK(row.at(LayerParameter::WaitBefore)==.25);
    }
    const auto before=job->layer_document()->image(0).buf;const auto guard=job->export_generation_guard();
    CHECK_THROWS_AS(job=create_exposure_finder_calibration(config,parameters,[](size_t done,size_t total){return done!=total;}),LayerOperationCancelled);
    CHECK(job->layer_document()->image(0).buf==before);CHECK(guard.current());
    auto draft=parameters;auto cfg=config;bool mutated=false;
    auto frozen=create_exposure_finder_calibration(cfg,draft,[&](size_t,size_t){if(!mutated){mutated=true;draft.recipe.rows.clear();cfg.set("display_pixels_x",1);}return true;});
    CHECK(frozen->layer_document()->image(0).buf==before);
    auto oversized=parameters;oversized.pattern.holes.resize(501);bool called=false;
    CHECK_THROWS(create_exposure_finder_calibration(config,oversized,[&](size_t,size_t){called=true;return true;}));CHECK_FALSE(called);
    size_t previous=0;bool completed=false,monotonic=true,totals=true;
    auto progress_job=create_exposure_finder_calibration(config,parameters,[&](size_t done,size_t total){totals=totals && total==100;monotonic=monotonic && done>=previous;previous=done;completed=done==total;return true;});
    CHECK(monotonic);CHECK(totals);CHECK(completed);CHECK(progress_job->layer_document()->layer_count()==fresh.rows.size());
    if(const char *root=std::getenv("SLA_EXPOSURE_PATTERN_FACTORY_EXPORTS")) {
        const auto folder=boost::filesystem::path(root)/(std::to_string(portrait)+std::to_string(mixed_heights)+"-"+format);
        boost::filesystem::create_directories(folder);
        save_layer_project((folder/"pattern.sla-project").string(),capture_generated_project(*progress_job,*progress_job->layer_document()));
        const auto restored=read_project_layers((folder/"pattern.sla-project").string());REQUIRE(restored);
        progress_job=restore_generated_project(*restored);
        REQUIRE_NOTHROW(progress_job->export_print((folder/("pattern."+format)).string()));
    }
    parameters.recipe.schedule.reset();CHECK_THROWS(create_exposure_finder_calibration(config,parameters));
}

TEST_CASE("Actual ExposureFinder factory persists requested generation and restores exact masks", "[sla_exposure_generation_factory]") {
    DynamicPrintConfig config;config.apply(SLAFullPrintConfig{});
    config.option("printer_technology")->setInt(ptSLA);config.option("display_orientation")->setInt(sladoLandscape);
    config.set("sla_archive_format","uvj");config.set("display_pixels_x",2048);config.set("display_pixels_y",1536);
    config.set("display_width",100.);config.set("display_height",75.);config.set("max_print_height",10.);
    ExposureFinderGenerationParameters request;
    request.recipe.rows=generate_exposure_finder_table(request.recipe.table_parameters);
    ExposureFinderScheduleParameters schedule;schedule.base_height=.05;schedule.features_height=.1;schedule.bottom_layers=0;
    schedule.base_mode=ExposureFinderBaseMode::Custom;schedule.custom_base_exposure=3.25;
    schedule.multiple_brightness=true;schedule.brightness={127,255,127,0};schedule.repeated_settings=true;
    schedule.repeated_lift_height=.5;schedule.repeated_wait_before=.25;request.recipe.schedule=schedule;
    auto &p=request.pattern;p.holes={1,5,9};p.square_holes=false;p.bars={2,4,12};p.bullseyes={{20,2},{60,4}};
    p.feature_margin_mm=.245;p.bar_length_mm=.75;p.bar_spacing_mm=.25;p.bar_splitter=-2;p.bar_fence=3;
    p.bar_fence_offset=-1;p.bullseye_fence=0;p.bullseye_fence_offset=-2;p.triangle_tip_offset=-3;
    p.staircase_thickness=3;p.antialias=true;p.text="Łódź / Привет";p.font_face=3;p.text_scale=.805;p.text_thickness=1;
    request.layout.margin_x_mm=.25;request.layout.margin_y_mm=.5;request.layout.gap_x_mm=.75;request.layout.gap_y_mm=.25;
    request.layout.center=false;request.layout.mirror_x=true;request.layout.mirror_y=true;
    request.bottom_erosion=1;request.chamfer_layers=2;
    auto job=create_exposure_finder_calibration(config,request);
    REQUIRE(job->calibration_recipe()->exposure_finder->generation);
    const auto &saved=*job->calibration_recipe()->exposure_finder->generation;
    CHECK(saved.pattern.feature_margin_mm==.245);CHECK(saved.pattern.text_scale==.805);
    CHECK(saved.pattern.text=="Łódź / Привет");CHECK(saved.layout.cell_columns==0);CHECK(saved.layout.cell_rows==0);
    CHECK(saved.layout.mirror_x);CHECK(saved.layout.mirror_y);CHECK(saved.bottom_erosion==1);CHECK(saved.chamfer_layers==2);
    const auto *root=std::getenv("SLA_EXPOSURE_GENERATION_FIXTURES");
    const auto directory=root?boost::filesystem::path(root):boost::filesystem::temp_directory_path()/boost::filesystem::unique_path("finder-generation-%%%%");
    struct Cleanup {boost::filesystem::path path;bool keep;~Cleanup(){if(!keep){boost::system::error_code ec;boost::filesystem::remove_all(path,ec);}}} cleanup{directory,root!=nullptr};
    boost::filesystem::create_directories(directory);const auto path=directory/"exposure-generation.sla-project";
    save_layer_project(path.string(),capture_generated_project(*job,*job->layer_document()));
    const auto state=read_project_layers(path.string());REQUIRE(state);auto restored=restore_generated_project(*state);
    REQUIRE(restored->layer_document()->layer_count()==job->layer_document()->layer_count());
    for(size_t i=0;i<job->layer_document()->layer_count();++i)
        CHECK(restored->layer_document()->image(i).buf==job->layer_document()->image(i).buf);
    CHECK(restored->layer_document()->fresh_job()->rows==job->layer_document()->fresh_job()->rows);
    REQUIRE(restored->calibration_recipe()->exposure_finder->generation);
    CHECK(restored->calibration_recipe()->exposure_finder->generation->pattern.text_scale==.805);
    request.pattern.text="caller changed";request.layout.margin_x_mm=99.;
    CHECK(restored->calibration_recipe()->exposure_finder->generation->pattern.text=="Łódź / Привет");
    CHECK(job->calibration_recipe()->exposure_finder->generation->layout.margin_x_mm==.25);
}
