// SPDX-License-Identifier: AGPL-3.0-or-later
// Owned fresh SL1 contract through public export APIs. The external UVtools
// gate separately checks raw bytes against the reference's normalized view.
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <boost/filesystem.hpp>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <nlohmann/json.hpp>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include "libslic3r/SLAPrint.hpp"
#include "libslic3r/SLA/LayerDocument.hpp"
#include "libslic3r/SLA/LayerConversionPlan.hpp"
#include "libslic3r/SLA/SL1FreshExportPlan.hpp"
#include "libslic3r/SLA/SL1FreshExportVerify.hpp"
#include "libslic3r/SLA/ProjectArchive.hpp"
#include "libslic3r/miniz_extension.hpp"
#include <map>
#include <sstream>
#include <type_traits>
#include <atomic>
#include <future>
#include <thread>
using namespace Slic3r;
namespace {
using F=sla::LayerParameter;
struct FreshSL1Directory {
    boost::filesystem::path path=boost::filesystem::unique_path("fresh-sl1-%%%%");
    bool keep=false;
    ~FreshSL1Directory(){if(!keep){boost::system::error_code error;boost::filesystem::remove_all(path,error);}}
};
DynamicPrintConfig fresh_sl1_profile(bool portrait) {
    DynamicPrintConfig config;config.apply(SLAFullPrintConfig{});
    config.option("printer_technology")->setInt(ptSLA);
    config.set("sla_archive_format","SL1");config.set("printer_model","SL1");
    config.set("display_pixels_x",8);config.set("display_pixels_y",4);
    config.set("display_width",16.);config.set("display_height",4.);
    config.option("display_orientation")->setInt(portrait?sladoPortrait:sladoLandscape);
    config.set("layer_height",.1);config.set("initial_layer_height",.1);
    config.set("exposure_time",2.);config.set("initial_exposure_time",20.);
    config.set("sla_bottom_layers",2);config.set("sla_transition_layers",0);
    config.set("sla_lift_height",7.);config.set("sla_bottom_lift_height",8.);
    config.set("sla_lift_speed",75.);config.set("sla_bottom_lift_speed",85.);
    return config;
}
sla::FreshLayerJob fresh_sl1_intent(bool portrait,sla::FreshLayerProducer producer) {
    sla::FreshLayerJob intent;intent.producer=producer;
    intent.geometry=portrait?sla::LayerRasterGeometry{4,8,4.,16.}:sla::LayerRasterGeometry{8,4,16.,4.};
    intent.globals.layer_height=.05;intent.globals.bottom_layers=1;intent.globals.transition_layers=0;
    intent.globals.normal={{F::Exposure,3.25},{F::LiftDistance,3.5},{F::LiftSpeed,60.},
        {F::LiftDistance2,1.25},{F::LiftSpeed2,180.},{F::RetractDistance2,.75},{F::RetractSpeed,150.},
        {F::RetractSpeed2,240.},{F::WaitBefore,.12},{F::WaitAfter,.34},{F::WaitAfterLift,.56},
        {F::LightOff,0.},{F::LightPWM,200.}};
    intent.globals.normal[F::LightOff]=.6;
    intent.globals.bottom={{F::Exposure,9.5},{F::LiftDistance,4.5},{F::LiftSpeed,90.},
        {F::LiftDistance2,2.25},{F::LiftSpeed2,120.},{F::RetractDistance2,1.25},{F::RetractSpeed,100.},
        {F::RetractSpeed2,80.},{F::WaitBefore,.22},{F::WaitAfter,.44},{F::WaitAfterLift,.66},
        {F::LightOff,0.},{F::LightPWM,129.}};
    intent.globals.bottom[F::LightOff]=.8;
    for(size_t i=0;i<3;++i) {
        auto row=i?intent.globals.normal:intent.globals.bottom;
        row[F::LightOff]=i?.6:.8;
        row[F::PositionZ]=std::vector<double>{.075,.125,.175}[i];intent.rows.push_back(row);
    }
    return intent;
}
std::string sl1_zip_text(const boost::filesystem::path &path,const char *entry) {
    mz_zip_archive zip{};
    if(!mz_zip_reader_init_file(&zip,path.string().c_str(),0))throw std::runtime_error("Cannot read SL1 ZIP");
    size_t size=0;void *data=mz_zip_reader_extract_file_to_heap(&zip,entry,&size,0);
    if(!data){mz_zip_reader_end(&zip);throw std::runtime_error("Missing SL1 metadata entry");}
    const std::string value(static_cast<const char *>(data),size);mz_free(data);mz_zip_reader_end(&zip);return value;
}
std::map<std::string,std::string> sl1_ini(const std::string &text) {
    std::map<std::string,std::string> values;std::istringstream input(text);std::string line;
    while(std::getline(input,line)){
        const auto equal=line.find(" = ");if(equal!=std::string::npos)values[line.substr(0,equal)]=line.substr(equal+3);
    }
    return values;
}
void sl1_check(const boost::filesystem::path &path,const sla::FreshLayerJob &intent,const std::vector<uint8_t> &pixels) {
    const auto config=sl1_ini(sl1_zip_text(path,"config.ini"));
    const auto slicer=sl1_ini(sl1_zip_text(path,"prusaslicer.ini"));
    CHECK(std::stod(config.at("layerHeight"))==.05);CHECK(std::stod(config.at("expTime"))==3.25);
    CHECK(std::stod(config.at("expTimeFirst"))==9.5);CHECK(std::stoi(config.at("numFade"))==1);
    CHECK(std::stoi(config.at("numSlow"))+std::stoi(config.at("numFast"))==3);
    CHECK(std::stod(slicer.at("initial_layer_height"))==.075);
    CHECK(std::stod(slicer.at("display_width"))==intent.geometry.width_mm);
    CHECK(std::stod(slicer.at("display_height"))==intent.geometry.height_mm);
    CHECK(std::stoul(slicer.at("display_pixels_x"))==intent.geometry.columns);
    CHECK(std::stoul(slicer.at("display_pixels_y"))==intent.geometry.rows);
    CHECK(slicer.at("printer_notes").find("BottomLightOffDelay_0.800000")!=std::string::npos);
    CHECK(slicer.at("printer_notes").find("LightOffDelay_0.600000")!=std::string::npos);
    const auto saved=sla::LayerDocument::load_archive(path.string());REQUIRE(saved.layer_count()==3);
    for(size_t i=0;i<3;++i){CHECK(saved.image(i).buf==pixels);CHECK(saved.image(i).cols==intent.geometry.columns);CHECK(saved.image(i).rows==intent.geometry.rows);}
}
}
TEST_CASE("Owned fresh SL1 globals schedule and masks survive both exports and restart", "[sla_sl1_fresh]") {
    const bool portrait=GENERATE(false,true);const bool direct=GENERATE(false,true);const bool restart=GENERATE(false,true);
    const auto producer=GENERATE(sla::FreshLayerProducer::Raster,sla::FreshLayerProducer::ModelSlice,sla::FreshLayerProducer::Calibration,sla::FreshLayerProducer::PCB);
    const std::string format=GENERATE("SL1","SL1S");CAPTURE(portrait,direct,restart,int(producer),format);
    auto config=fresh_sl1_profile(portrait);config.set("sla_archive_format",format);
    const auto intent=fresh_sl1_intent(portrait,producer);std::vector<uint8_t> pixels(32,0);pixels[1]=255;pixels[20]=127;
    const auto mask=sla::PNGRasterEncoder{}(pixels.data(),portrait?4:8,portrait?8:4,1);
    const auto original=sla::LayerDocument::from_fresh_rasters({mask,mask,mask},intent);
    FreshSL1Directory directory;
    if(const char *root=std::getenv("SLA_SL1_FRESH_FIXTURES")) {
        directory.keep=true;directory.path=boost::filesystem::path(root)/(std::to_string(int(producer))+"-"+(portrait?"portrait":"landscape")+"-"+(direct?"direct":"explicit")+"-"+(restart?"restart":"initial")+"-"+format);
    }
    boost::filesystem::create_directories(directory.path);const std::vector<double> positions{.075,.125,.175};
    sla::ProjectRasterState state{original,positions,"",{},sla::ProjectLayerOrigin::GeneratedRaster};
    if(producer==sla::FreshLayerProducer::ModelSlice){state.origin=sla::ProjectLayerOrigin::ModelSlice;state.source_fingerprint=std::string(40,'c');}
    const auto project=directory.path/"fresh.sla-project";REQUIRE_NOTHROW(sla::save_layer_project(project.string(),state));
    auto document=original;
    if(restart){const auto restored=sla::read_project_layers(project.string());REQUIRE(restored);document=restored->document;}
    const auto job=SLAPrint::create_raster_job(config,document,positions);
    const auto output=directory.path/(format=="SL1"?"output.sl1":"output.sl1s");
    if(direct)REQUIRE_NOTHROW(job->export_print(output.string()));
    else REQUIRE_NOTHROW(job->export_layer_document(document,format,output.string(),{},"fresh-sl1"));
    sl1_check(output,intent,pixels);
    REQUIRE(document.fresh_job());CHECK(document.fresh_job()->rows==intent.rows);
    CHECK(document.fresh_job()->globals.normal==intent.globals.normal);CHECK(document.fresh_job()->globals.bottom==intent.globals.bottom);
    CHECK(job->full_print_config().opt_float("exposure_time")==2.);CHECK(job->full_print_config().opt_float("sla_lift_height")==7.);
}
TEST_CASE("Fresh SL1 refuses arbitrary rows without replacing previous file", "[sla_sl1_fresh_negative]") {
    const bool direct=GENERATE(false,true);const int defect=GENERATE(0,1,2,3);CAPTURE(direct,defect);
    auto intent=fresh_sl1_intent(false,sla::FreshLayerProducer::Raster);
    const auto field=defect==0?F::Exposure:defect==1?F::WaitBefore:defect==2?F::LightPWM:F::PositionZ;
    const char *name=defect==0?"Exposure":defect==1?"WaitBefore":defect==2?"LightPWM":"PositionZ";
    intent.rows[defect==3?2:1][field]+=defect==2?1:.01;
    auto config=fresh_sl1_profile(false);std::vector<uint8_t> pixels(32,0);pixels[1]=255;pixels[20]=127;
    const auto mask=sla::PNGRasterEncoder{}(pixels.data(),8,4,1);
    const auto document=sla::LayerDocument::from_fresh_rasters({mask,mask,mask},intent);
    const auto job=SLAPrint::create_raster_job(config,document,{.075,.125,defect==3?.185:.175});
    FreshSL1Directory directory;boost::filesystem::create_directories(directory.path);const auto output=directory.path/"keep.sl1";
    const std::string sentinel="previous valid print";{std::ofstream file(output.string(),std::ios::binary);file<<sentinel;}
    if(direct)CHECK_THROWS_WITH(job->export_print(output.string()),Catch::Matchers::ContainsSubstring(name));
    else CHECK_THROWS_WITH(job->export_layer_document(document,"SL1",output.string()),Catch::Matchers::ContainsSubstring(name));
    std::ifstream input(output.string(),std::ios::binary);const std::string saved(std::istreambuf_iterator<char>(input),{});CHECK(saved==sentinel);
    CHECK(document.fresh_job()->rows==intent.rows);
}

TEST_CASE("Fresh SL1 keeps executable edge schedules and full grayscale", "[sla_sl1_fresh_edges]") {
    const bool direct=GENERATE(false,true);const int variant=GENERATE(0,1,2,3,4,5,6);
    const std::string format=GENERATE("SL1","SL1S");CAPTURE(direct,variant,format);
    auto config=fresh_sl1_profile(false);config.set("sla_archive_format",format);
    auto intent=fresh_sl1_intent(false,sla::FreshLayerProducer::Raster);
    if(variant==0){intent.globals.normal[F::Exposure]=3.135;intent.globals.bottom[F::Exposure]=9.125;}
    if(variant==1)intent.globals.transition_layers=1;
    if(variant==2)for(auto *bucket:{&intent.globals.normal,&intent.globals.bottom})
        for(auto field:{F::LiftDistance,F::LiftDistance2,F::LiftSpeed,F::LiftSpeed2,F::RetractDistance2,F::RetractSpeed,F::RetractSpeed2})(*bucket)[field]=0.;
    if(variant==3){intent.globals.normal[F::LightOff]=15.;intent.globals.bottom[F::LightOff]=20.;}
    if(variant==6){config.set("material_density",1.25);config.set("bottle_cost",60.);config.set("bottle_volume",750.);}
    if(variant==5){
        config.set("material_notes",std::string("resin user note\nWaitTimeBeforeCure_999\nBottomLightOffDelay_123\n"),true);
        config.set("printer_notes",std::string("printer user note\nLightOffDelay_777\nBottomLiftHeight_999\nTransitionLayerCount_99\n"),true);
    }
    for(size_t i=0;i<3;++i){const auto z=intent.rows[i].at(F::PositionZ);intent.rows[i]=i?intent.globals.normal:intent.globals.bottom;intent.rows[i][F::PositionZ]=z;}
    if(variant==1)intent.rows[1][F::Exposure]=6.37;
    std::vector<uint8_t> pixels(32,0);pixels[1]=variant==4?1:255;pixels[20]=variant==4?128:127;
    const auto mask=sla::PNGRasterEncoder{}(pixels.data(),8,4,1);
    const auto document=sla::LayerDocument::from_fresh_rasters({mask,mask,mask},intent);
    const auto job=SLAPrint::create_raster_job(config,document,{.075,.125,.175});
    FreshSL1Directory directory;
    if(const char *root=std::getenv("SLA_SL1_FRESH_EDGE_FIXTURES")){
        directory.keep=true;directory.path=boost::filesystem::path(root)/(std::to_string(variant)+"-"+(direct?"direct":"explicit")+"-"+format);
    }
    boost::filesystem::create_directories(directory.path);const auto output=directory.path/(format=="SL1"?"output.sl1":"output.sl1s");
    if(direct)REQUIRE_NOTHROW(job->export_print(output.string()));
    else REQUIRE_NOTHROW(job->export_layer_document(document,format,output.string(),{},"edge-sl1"));
    const auto config_ini=sl1_ini(sl1_zip_text(output,"config.ini"));
    CHECK(float(std::stod(config_ini.at("expTime")))==float(intent.globals.normal.at(F::Exposure)));
    CHECK(float(std::stod(config_ini.at("expTimeFirst")))==float(intent.globals.bottom.at(F::Exposure)));
    const auto loaded=sla::LayerDocument::load_archive(output.string());
    for(size_t i=0;i<3;++i)CHECK(loaded.image(i).buf==pixels);
    if(variant==5){
        const auto slicer=sl1_ini(sl1_zip_text(output,"prusaslicer.ini"));
        CHECK(slicer.at("material_notes").find("resin user note")!=std::string::npos);
        CHECK(slicer.at("printer_notes").find("printer user note")!=std::string::npos);
        CHECK(slicer.at("printer_notes").find("_999")==std::string::npos);
        CHECK(slicer.at("material_notes").find("_999")==std::string::npos);
    }
    if(variant==6){
        const auto slicer=sl1_ini(sl1_zip_text(output,"prusaslicer.ini"));
        CHECK(std::stod(slicer.at("material_density"))==1.25);
        CHECK(std::stod(slicer.at("bottle_cost"))==60.);
        CHECK(std::stod(slicer.at("bottle_volume"))==750.);
    }
    CHECK(document.fresh_job()->rows==intent.rows);CHECK(document.fresh_job()->globals.normal==intent.globals.normal);
}

TEST_CASE("Fresh SL1 rejects native precision and invalid material before publication", "[sla_sl1_fresh_hostile]") {
    const bool direct=GENERATE(false,true);const int defect=GENERATE(0,1,2,3,4,5,6,7,8,9);
    const std::string format=GENERATE("SL1","SL1S");CAPTURE(direct,defect,format);
    auto config=fresh_sl1_profile(false);config.set("sla_archive_format",format);
    auto intent=fresh_sl1_intent(false,sla::FreshLayerProducer::Raster);
    const char *name="";
    if(defect==0){intent.globals.bottom[F::WaitBefore]=.225;name="WaitBefore";}
    if(defect==1){intent.globals.normal[F::LiftSpeed]=.004;name="LiftSpeed";}
    if(defect==2){intent.globals.normal[F::LiftDistance]=.004;name="LiftDistance";}
    if(defect==3)name="PositionZ";
    if(defect==4){intent.globals.layer_height=.050005;name="layer_height";}
    if(defect==5){intent.globals.transition_layers=2;name="transition_layers";}
    if(defect==6){config.set("material_density",1e100);name="material_density";}
    if(defect==7){config.set("bottle_cost",1e100);name="bottle_cost";}
    if(defect==8){config.set("bottle_volume",0.);name="bottle_volume";}
    if(defect==9){intent.globals.normal[F::LightOff]=.125;name="LightOff";}
    const std::vector<double> positions=defect==3?std::vector<double>{.075005,.125005,.175005}:std::vector<double>{.075,.125,.175};
    for(size_t i=0;i<3;++i){intent.rows[i]=i?intent.globals.normal:intent.globals.bottom;intent.rows[i][F::PositionZ]=positions[i];}
    std::vector<uint8_t> pixels(32,0);pixels[1]=255;pixels[20]=127;
    const auto mask=sla::PNGRasterEncoder{}(pixels.data(),8,4,1);
    const auto document=sla::LayerDocument::from_fresh_rasters({mask,mask,mask},intent);
    const auto job=SLAPrint::create_raster_job(config,document,positions);
    FreshSL1Directory directory;boost::filesystem::create_directories(directory.path);
    const auto output=directory.path/(format=="SL1"?"keep.sl1":"keep.sl1s");
    const std::string sentinel="previous valid print";{std::ofstream file(output.string(),std::ios::binary);file<<sentinel;}
    if(direct)CHECK_THROWS_WITH(job->export_print(output.string()),Catch::Matchers::ContainsSubstring(name));
    else CHECK_THROWS_WITH(job->export_layer_document(document,format,output.string()),Catch::Matchers::ContainsSubstring(name));
    std::ifstream input(output.string(),std::ios::binary);const std::vector<char> bytes(std::istreambuf_iterator<char>(input),{});
    const bool unchanged=bytes.size()==sentinel.size() && std::equal(bytes.begin(),bytes.end(),sentinel.begin());CHECK(unchanged);
    CHECK(document.fresh_job()->rows==intent.rows);
}


// Planning scans the masks to derive physical material use. Cancellation must
// reach this work, including the final layer, before an export can be published.
TEST_CASE("Fresh SL1 planning can cancel during mask accounting without changing the source", "[sla_sl1_plan_cancel]") {
    const std::string format=GENERATE("SL1","SL1S");
    const size_t cancel_after=GENERATE(size_t(1),size_t(2),size_t(3));
    CAPTURE(format,cancel_after);
    const auto intent=fresh_sl1_intent(false,sla::FreshLayerProducer::Raster);
    std::vector<uint8_t> pixels(32,0);pixels[1]=1;pixels[20]=255;
    const auto mask=sla::PNGRasterEncoder{}(pixels.data(),8,4,1);
    const auto document=sla::LayerDocument::from_fresh_rasters({mask,mask,mask},intent);
    const auto revision=document.revision();
    bool reached=false;
    const sla::LayerProgress stop=[&](size_t done,size_t total) {
        CHECK(total==document.layer_count());
        CHECK(done<=total);
        if(done>=cancel_after) {reached=true;return false;}
        return true;
    };
    CHECK_THROWS_AS(sla::plan_layer_conversion(document,format,stop),sla::LayerOperationCancelled);
    CHECK(reached);
    REQUIRE(document.fresh_job());
    CHECK(document.revision()==revision);
    CHECK(document.fresh_job()->rows==intent.rows);
    for(size_t i=0;i<document.layer_count();++i)CHECK(document.image(i).buf==pixels);

    // A cancelled attempt must not poison the next immutable plan. Independent
    // dimensions and layer heights give 2 lit pixels * 2 mm2 * .175 mm / 1000.
    std::vector<size_t> observed;
    const auto plan=sla::plan_layer_conversion(document,format,[&](size_t done,size_t total) {
        CHECK(total==document.layer_count());observed.push_back(done);return true;
    });
    REQUIRE(plan.sl1_fresh);
    CHECK(plan.sl1_fresh->material_ml==Catch::Approx(.0007).margin(1e-9));
    REQUIRE_FALSE(observed.empty());
    CHECK(observed.back()==document.layer_count());
    CHECK(std::is_sorted(observed.begin(),observed.end()));
}

TEST_CASE("SL1 transition preview agrees with complete executable schedules", "[sla_sl1_transition_preview]") {
    static_assert(!std::is_convertible<sla::SL1ExposureTransitionPreview, sla::SL1FreshExportPlan>::value,
        "A metadata preview cannot stand in for a fully validated export plan");
    const std::string format = GENERATE("SL1", "SL1S");
    const bool portrait = GENERATE(false, true);
    const unsigned variant = GENERATE(0u, 1u, 2u, 3u, 4u, 5u);
    CAPTURE(format, portrait, variant);
    auto intent = fresh_sl1_intent(portrait, sla::FreshLayerProducer::ModelSlice);
    if (variant >= 1 && variant <= 3) intent.globals.transition_layers = variant == 3 ? 2 : 1;
    if (variant == 4) intent.globals.bottom_layers = 0;
    if (variant == 5) {
        intent.globals.transition_layers = 1;
        intent.globals.bottom[F::Exposure] = 3.25;
        intent.globals.normal[F::Exposure] = 9.5;
    }
    const size_t count = variant == 3 ? 4 : 3;
    intent.rows.clear();
    const std::vector<double> positions{.075, .125, .175, .225};
    for (size_t i = 0; i < count; ++i) {
        auto row = i < intent.globals.bottom_layers ? intent.globals.bottom : intent.globals.normal;
        row[F::PositionZ] = positions[i];
        intent.rows.push_back(std::move(row));
    }
    // These expected schedules are authored literals, not values obtained by
    // calling the new shared projection helper.
    size_t expected_count = 0; double expected_change = 0;
    if (variant == 1) intent.rows[1][F::Exposure] = 6.37; // Already executable.
    if (variant == 2) {
        intent.rows[1][F::Exposure] = 6.375; // Producer ideal, reference executes6.37.
        expected_count = 1;
        expected_change = std::abs(double(float(6.37)) - 6.375);
    }
    if (variant == 3) {
        intent.rows[1][F::Exposure] = 89. / 12.;
        intent.rows[2][F::Exposure] = 16. / 3.;
        expected_count = 2;
        expected_change = std::max(std::abs(double(float(7.42)) - 89. / 12.),
            std::abs(double(float(5.34)) - 16. / 3.));
    }
    std::vector<uint8_t> pixels(32, 0); pixels[1] = 255; pixels[20] = 1;
    const auto mask = sla::PNGRasterEncoder{}(pixels.data(), portrait ? 4 : 8, portrait ? 8 : 4, 1);
    const auto document = sla::LayerDocument::from_fresh_rasters(std::vector<sla::EncodedRaster>(count, mask), intent);
    const auto revision = document.revision();
    const auto before = sla::plan_layer_conversion(document, format);
    REQUIRE(before.sl1_fresh);
    std::vector<size_t> progress;
    const auto preview = sla::sl1_exposure_transition_preview(document, [&](size_t done, size_t total) {
        CHECK(total == count); CHECK(done <= total); progress.push_back(done); return true;
    });
    CHECK(preview.projected_transition_exposures == expected_count);
    CHECK(preview.maximum_exposure_change == expected_change);
    CHECK(preview.projected_transition_exposures == before.sl1_fresh->projected_transition_exposures);
    CHECK(preview.maximum_exposure_change == before.sl1_fresh->maximum_exposure_change);
    REQUIRE_FALSE(progress.empty()); CHECK(progress.front() == 0); CHECK(progress.back() == count);
    CHECK(std::is_sorted(progress.begin(), progress.end()));
    const auto after = sla::plan_layer_conversion(document, format);
    REQUIRE(after.sl1_fresh);
    CHECK(after.sl1_fresh->rows == before.sl1_fresh->rows);
    CHECK(after.sl1_fresh->globals.normal == before.sl1_fresh->globals.normal);
    CHECK(after.sl1_fresh->globals.bottom == before.sl1_fresh->globals.bottom);
    CHECK(after.sl1_fresh->expected_print_time == before.sl1_fresh->expected_print_time);
    CHECK(after.sl1_fresh->material_ml == before.sl1_fresh->material_ml);
    // Two lit pixels *2mm2 per pixel, through the last native Z.
    CHECK(after.sl1_fresh->material_ml == 4. * double(float(positions[count-1])) / 1000.);
    CHECK(document.revision() == revision);
    CHECK(document.fresh_job()->rows == intent.rows);
    for (size_t layer = 0; layer < count; ++layer) {
        const auto bytes = document.encoded_layer(layer);
        CHECK(bytes.sz == mask.size());
        CHECK(std::memcmp(bytes.buf, mask.data(), mask.size()) == 0);
    }
}

TEST_CASE("SL1 transition preview retains native metadata rejection before confirmation", "[sla_sl1_transition_preview]") {
    const std::string format = GENERATE("SL1", "SL1S");
    const unsigned defect = GENERATE(0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u);
    CAPTURE(format, defect);
    auto intent = fresh_sl1_intent(false, sla::FreshLayerProducer::Raster);
    const char *field = "";
    if (defect == 0) { intent.globals.bottom[F::WaitBefore] = .225; field = "WaitBefore"; }
    if (defect == 1) { intent.globals.normal[F::LiftSpeed] = .004; field = "LiftSpeed"; }
    if (defect == 2) { intent.globals.normal[F::LiftDistance] = .004; field = "LiftDistance"; }
    if (defect == 3) { intent.globals.layer_height = .050005; field = "layer_height"; }
    if (defect == 4) { intent.globals.transition_layers = 2; field = "transition_layers"; }
    if (defect == 5) field = "PositionZ";
    if (defect == 6) field = "Exposure";
    if (defect == 7) field = "LightPWM";
    if (defect == 8) { intent.globals.normal[F::LightOff] = .125; field = "LightOff"; }
    for (size_t i = 0; i < intent.rows.size(); ++i) {
        const auto z = intent.rows[i].at(F::PositionZ);
        intent.rows[i] = i ? intent.globals.normal : intent.globals.bottom;
        intent.rows[i][F::PositionZ] = z + (defect == 5 ? .000005 : 0.);
    }
    if (defect == 6) intent.rows[1][F::Exposure] += .01;
    if (defect == 7) intent.rows[1][F::LightPWM] += 1;
    const std::vector<uint8_t> pixels(32, 255);
    const auto mask = sla::PNGRasterEncoder{}(pixels.data(), 8, 4, 1);
    const auto document = sla::LayerDocument::from_fresh_rasters({mask, mask, mask}, intent);
    const auto failure = [](const auto &operation) {
        try { operation(); } catch (const RuntimeError &error) { return std::string(error.what()); }
        return std::string{};
    };
    const auto preview_error = failure([&] { (void)sla::sl1_exposure_transition_preview(document); });
    const auto complete_error = failure([&] { (void)sla::plan_layer_conversion(document, format); });
    REQUIRE_FALSE(preview_error.empty());
    CHECK(preview_error.find(field) != std::string::npos);
    CHECK(preview_error == complete_error);
    CHECK(document.fresh_job()->rows == intent.rows);
    CHECK(document.image(0).buf == pixels);
    const auto unowned = sla::LayerDocument::from_rasters({mask});
    CHECK_THROWS_WITH(sla::sl1_exposure_transition_preview(unowned),
        "SL1 fresh export requires an explicit owned descriptor");
}

TEST_CASE("SL1 transition preview cancels metadata work without changing its source", "[sla_sl1_transition_preview]") {
    const auto intent = fresh_sl1_intent(false, sla::FreshLayerProducer::Raster);
    const std::vector<uint8_t> pixels(32, 255);
    const auto mask = sla::PNGRasterEncoder{}(pixels.data(), 8, 4, 1);
    const auto document = sla::LayerDocument::from_fresh_rasters({mask, mask, mask}, intent);
    size_t callbacks = 0;
    const auto complete = sla::sl1_exposure_transition_preview(document,
        [&](size_t, size_t) { ++callbacks; return true; });
    REQUIRE(callbacks > 2);
    for (const size_t stop : {size_t(1), callbacks / 2, callbacks}) {
        CAPTURE(stop); size_t seen = 0;
        CHECK_THROWS_AS(sla::sl1_exposure_transition_preview(document,
            [&](size_t, size_t) { return ++seen < stop; }), sla::LayerOperationCancelled);
        CHECK(seen == stop);
        CHECK(document.revision() == 0);
        CHECK(document.fresh_job()->rows == intent.rows);
        CHECK(document.image(0).buf == pixels);
    }
    CHECK_THROWS_AS(sla::sl1_exposure_transition_preview(document,
        [](size_t, size_t) -> bool { throw std::logic_error("preview owner"); }), std::logic_error);
    const auto retry = sla::sl1_exposure_transition_preview(document);
    CHECK(retry.projected_transition_exposures == complete.projected_transition_exposures);
    CHECK(retry.maximum_exposure_change == complete.maximum_exposure_change);
}

TEST_CASE("SL1 transition preview never authorizes malformed raster publication", "[sla_sl1_transition_preview]") {
    const std::string format = GENERATE("SL1", "SL1S");
    auto intent = fresh_sl1_intent(false, sla::FreshLayerProducer::Raster);
    intent.globals.transition_layers = 1; intent.rows[1][F::Exposure] = 6.375;
    const std::vector<uint8_t> pixels(32, 255);
    const auto mask = sla::PNGRasterEncoder{}(pixels.data(), 8, 4, 1);
    const auto good = sla::LayerDocument::from_fresh_rasters({mask, mask, mask}, intent);
    const auto *bytes = static_cast<const uint8_t *>(mask.data());
    std::vector<uint8_t> corrupted(bytes, bytes + mask.size());
    auto word = [&](size_t at) { return (uint32_t(corrupted[at]) << 24) |
        (uint32_t(corrupted[at+1]) << 16) | (uint32_t(corrupted[at+2]) << 8) | corrupted[at+3]; };
    size_t idat = 8;
    while (idat + 12 <= corrupted.size() && std::memcmp(corrupted.data()+idat+4, "IDAT", 4) != 0)
        idat += size_t(word(idat)) + 12;
    REQUIRE(idat + 12 <= corrupted.size());
    const size_t length = word(idat);
    REQUIRE(idat + length + 12 <= corrupted.size());
    corrupted[idat + length + 8] ^= 1; // CRC only; intact geometry/metadata.
    const sla::EncodedRaster broken(std::move(corrupted), "png");
    // Gray8 construction checks its declared metadata/PNG header. This test
    // deliberately supplies an invalid body to distinguish preview from the
    // complete decode/material/publication gates.
    const auto document = sla::LayerDocument::from_fresh_rasters({broken, broken, broken}, intent);
    const auto preview = sla::sl1_exposure_transition_preview(document);
    CHECK(preview.projected_transition_exposures == 1);
    CHECK_THROWS_WITH(sla::sl1_fresh_export_plan(document), "Cannot decode grayscale layer PNG");
    auto config = fresh_sl1_profile(false); config.set("sla_archive_format", format);
    const auto job = SLAPrint::create_raster_job(config, good, {.075, .125, .175});
    FreshSL1Directory directory; boost::filesystem::create_directories(directory.path);
    const auto path = directory.path / (format == "SL1" ? "keep.sl1" : "keep.sl1s");
    const std::string sentinel = "previous complete output";
    { std::ofstream output(path.string(), std::ios::binary); output << sentinel; }
    CHECK_THROWS_WITH(job->export_layer_document(document, format, path.string()),
        "Cannot decode grayscale layer PNG");
    std::ifstream input(path.string(), std::ios::binary);
    const std::string retained(std::istreambuf_iterator<char>(input), {});
    CHECK(retained == sentinel);
    CHECK(document.fresh_job()->rows == intent.rows);
    CHECK(document.encoded_layer(0).sz == broken.size());
    CHECK(std::memcmp(document.encoded_layer(0).buf, broken.data(), broken.size()) == 0);
}

// Explicitly selected performance contract: no timing threshold in regression.
// Both public APIs see the same immutable document in one future binary.
TEST_CASE("Profile SL1 metadata preview and full plan on retained owned masks",
          "[.][sla_sl1_transition_preview_benchmark]") {
    const char *input = std::getenv("SLA_SL1_PREVIEW_BENCH_PROJECT");
    const char *output = std::getenv("SLA_SL1_PREVIEW_BENCH_REPORT");
    REQUIRE(input); REQUIRE(output);
    const auto load_started = std::chrono::steady_clock::now();
    const auto state = sla::read_project_layers(input);
    const double load_seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - load_started).count();
    REQUIRE(state); REQUIRE(state->document.fresh_job());
    const auto &document = state->document;
    const auto expected = *document.fresh_job();
    REQUIRE(document.layer_count() == 128);
    REQUIRE(expected.geometry.columns == 4096);
    REQUIRE(expected.geometry.rows == 4096);
    REQUIRE(expected.geometry.width_mm == 200.);
    REQUIRE(expected.geometry.height_mm == 200.);
    REQUIRE(expected.mask_precision == sla::LayerMaskPrecision::Gray8);
    REQUIRE(expected.globals.bottom_layers == 1);
    REQUIRE(expected.globals.transition_layers == 0);
    REQUIRE(expected.globals.normal.at(F::Exposure) == 3.25);
    REQUIRE(expected.globals.bottom.at(F::Exposure) == 9.5);
    REQUIRE(expected.rows.back().at(F::PositionZ) == 6.4);
    const auto revision = document.revision();
    const auto dirty = document.dirty();
    std::vector<std::string> encoded_before;
    size_t encoded_bytes = 0;
    const auto preparation_started = std::chrono::steady_clock::now();
    for (size_t layer = 0; layer < document.layer_count(); ++layer) {
        const auto bytes = document.encoded_layer(layer);
        encoded_before.emplace_back(static_cast<const char *>(bytes.buf), bytes.sz);
        encoded_bytes += bytes.sz;
    }
    const double preparation_seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - preparation_started).count();
    const auto require_source_unchanged = [&] {
        REQUIRE(document.fresh_job());
        const auto &actual = *document.fresh_job();
        CHECK(actual.producer == expected.producer);
        CHECK(actual.parameter_origin == expected.parameter_origin);
        CHECK(actual.mask_precision == expected.mask_precision);
        CHECK(actual.position_policy == expected.position_policy);
        CHECK(actual.geometry.columns == expected.geometry.columns);
        CHECK(actual.geometry.rows == expected.geometry.rows);
        CHECK(actual.geometry.width_mm == expected.geometry.width_mm);
        CHECK(actual.geometry.height_mm == expected.geometry.height_mm);
        CHECK(actual.globals.normal == expected.globals.normal);
        CHECK(actual.globals.bottom == expected.globals.bottom);
        CHECK(actual.globals.layer_height == expected.globals.layer_height);
        CHECK(actual.globals.bottom_layers == expected.globals.bottom_layers);
        CHECK(actual.globals.transition_layers == expected.globals.transition_layers);
        CHECK(actual.rows == expected.rows);
        CHECK(document.revision() == revision);
        CHECK(document.dirty() == dirty);
    };
    nlohmann::json runs = nlohmann::json::array();
    std::optional<sla::SL1FreshExportPlan> reference;
    std::optional<size_t> full_callbacks, preview_callbacks;
    // Three balanced blocks. No hidden warm-up or PNG predecode; the first full
    // pass is retained in raw results. File loading is reported independently.
    for (size_t block = 0; block < 3; ++block) {
        for (size_t step = 0; step < 4; ++step) {
            const bool full = step == 0 || step == 3;
            size_t callbacks = 0, last_done = 0;
            bool progress_valid = true;
            const sla::LayerProgress progress = [&](size_t done, size_t total) {
                ++callbacks;
                progress_valid = progress_valid && total == 128 && done >= last_done && done <= total;
                last_done = done;
                return true;
            };
            sla::SL1ExposureTransitionPreview preview;
            std::optional<sla::SL1FreshExportPlan> plan;
            const auto started = std::chrono::steady_clock::now();
            if (full) plan = sla::sl1_fresh_export_plan(document, progress);
            else preview = sla::sl1_exposure_transition_preview(document, progress);
            const double seconds = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - started).count();
            CHECK(progress_valid); CHECK(last_done == 128); CHECK(callbacks > 0);
            auto &expected_callbacks = full ? full_callbacks : preview_callbacks;
            if (expected_callbacks) CHECK(callbacks == *expected_callbacks);
            else expected_callbacks = callbacks;
            if (full) {
                REQUIRE(plan);
                preview = {plan->projected_transition_exposures, plan->maximum_exposure_change};
                // Literal ramp: 1/256 pixels are zero in every layer; all unique
                // final markers remain nonzero. Native final Z is float32(6.4).
                const double expected_material = (4096. * 4096. - 4096. * 16.) *
                    (200. / 4096.) * (200. / 4096.) * double(float(6.4)) / 1000.;
                CHECK(plan->material_ml == Catch::Approx(expected_material).epsilon(1e-12));
                if (reference) {
                    CHECK(plan->globals.normal == reference->globals.normal);
                    CHECK(plan->globals.bottom == reference->globals.bottom);
                    CHECK(plan->globals.layer_height == reference->globals.layer_height);
                    CHECK(plan->globals.bottom_layers == reference->globals.bottom_layers);
                    CHECK(plan->globals.transition_layers == reference->globals.transition_layers);
                    CHECK(plan->rows == reference->rows);
                    CHECK(plan->geometry.columns == reference->geometry.columns);
                    CHECK(plan->geometry.rows == reference->geometry.rows);
                    CHECK(plan->geometry.width_mm == reference->geometry.width_mm);
                    CHECK(plan->geometry.height_mm == reference->geometry.height_mm);
                    CHECK(plan->initial_layer_height == reference->initial_layer_height);
                    CHECK(plan->expected_print_time == reference->expected_print_time);
                    CHECK(plan->material_ml == reference->material_ml);
                } else reference = *plan;
            }
            REQUIRE(reference);
            // Exact, independently known for this unmodified retained fixture.
            CHECK(preview.projected_transition_exposures == 0);
            CHECK(preview.maximum_exposure_change == 0.);
            CHECK(preview.projected_transition_exposures == reference->projected_transition_exposures);
            CHECK(preview.maximum_exposure_change == reference->maximum_exposure_change);
            require_source_unchanged();
            runs.push_back({{"block", block}, {"step", step}, {"kind", full ? "full" : "preview"},
                {"seconds", seconds}, {"callbacks", callbacks},
                {"projected_transition_exposures", preview.projected_transition_exposures},
                {"maximum_exposure_change", preview.maximum_exposure_change}});
        }
    }
    for (size_t layer = 0; layer < document.layer_count(); ++layer) {
        const auto bytes = document.encoded_layer(layer);
        REQUIRE(bytes.sz == encoded_before[layer].size());
        CHECK(std::memcmp(bytes.buf, encoded_before[layer].data(), bytes.sz) == 0);
    }
    std::ofstream report(output); REQUIRE(report);
    report << nlohmann::json{{"load_seconds", load_seconds}, {"preparation_seconds", preparation_seconds},
        {"layers", document.layer_count()}, {"columns", expected.geometry.columns}, {"rows", expected.geometry.rows},
        {"encoded_bytes", encoded_bytes}, {"decoded_bytes_per_full_pass", uint64_t(128) * 4096 * 4096},
        {"png_byte_comparisons", document.layer_count()}, {"source_revision", revision},
        {"material_ml_full_plan", reference->material_ml}, {"runs", runs},
        {"scope", "same-binary public full plan versus metadata preview; load and source-byte capture separate; no export, supports, slicing or GUI timings"}}.dump(2) << '\n';
    report.close(); REQUIRE(report);
}

namespace {
struct PlanMaskEnvironment {
    std::optional<std::string> previous;
    explicit PlanMaskEnvironment(const char *value) {
        if(const char *old=std::getenv("SLA_EXPERIMENT_SL1_PLAN_MASKS"))previous=old;
        set(value);
    }
    static void set(const char *value) {
#ifdef _WIN32
        _putenv_s("SLA_EXPERIMENT_SL1_PLAN_MASKS",value?value:"");
#else
        if(value)setenv("SLA_EXPERIMENT_SL1_PLAN_MASKS",value,1);
        else unsetenv("SLA_EXPERIMENT_SL1_PLAN_MASKS");
#endif
    }
    ~PlanMaskEnvironment(){set(previous?previous->c_str():nullptr);}
};
struct PlanMaskFixture {
    static constexpr size_t width=1025,height=1024,count=5;
    sla::LayerDocument document;
    std::vector<size_t> lit;
    PlanMaskFixture() {
        auto intent=fresh_sl1_intent(false,sla::FreshLayerProducer::ModelSlice);
        intent.geometry={width,height,17.25,13.125};intent.rows.clear();
        const double z[]{.075,.125,.175,.225,.275};
        std::vector<sla::EncodedRaster> masks;
        for(size_t i=0;i<count;++i) {
            std::vector<uint8_t> pixels(width*height,255);
            std::fill_n(pixels.begin(),17*(i+1),uint8_t(0));
            const uint8_t tail[]{1,127,128,254,255};std::copy(std::begin(tail),std::end(tail),pixels.end()-5);
            masks.push_back(sla::PNGRasterEncoder{}(pixels.data(),width,height,1));lit.push_back(pixels.size()-17*(i+1));
            auto row=i?intent.globals.normal:intent.globals.bottom;row[F::PositionZ]=z[i];intent.rows.push_back(row);
        }
        document=sla::LayerDocument::from_fresh_rasters(std::move(masks),std::move(intent));
        if(!document.paint_stroke(0,0,{{0,0}},0,1))throw std::runtime_error("Missing fixture stroke");
        ++lit[0];
    }
    double expected_ml() const {
        const double z[]{double(float(.075)),double(float(.125)),double(float(.175)),double(float(.225)),double(float(.275))};
        double result=0.,previous=0.;const double area=17.25/width*13.125/height;
        for(size_t i=0;i<count;++i){result+=lit[i]*area*(z[i]-previous);previous=z[i];}
        return result/1000.;
    }
};
}

TEST_CASE("SL1 mask batches bound actual raster memory before decoding", "[sla_sl1_mask_batches]") {
    using sla::sl1_fresh_detail::mask_batch_width;
    // Machine-dependent (RAM/threads) result: assert the invariants, and test
    // the exact sizing rule separately with explicit budgets below.
    const size_t hardware=size_t(std::max(1,tbb::this_task_arena::max_concurrency()));
    CHECK(mask_batch_width(8,4,5)==1);
    for(const auto &[columns,rows]:{std::pair<size_t,size_t>{1025,1024},{5760,3600},{8192,8192},{32768,4096}}) {
        const auto width=mask_batch_width(columns,rows,213);
        CHECK(width>=1);CHECK(width<=hardware);
        CHECK(uint64_t(width)*columns*rows*4<=std::max<uint64_t>(sla::inflight_raster_budget(),uint64_t(columns)*rows*4));
    }
    CHECK(mask_batch_width(5760,3600,2)<=2);
    CHECK(mask_batch_width(32768,4096,5)>=1);
    CHECK(mask_batch_width(5760,3600,0)==0);
    CHECK_THROWS_AS(mask_batch_width(0,1,1),RuntimeError);
    CHECK_THROWS_AS(mask_batch_width(32768,32768,1),RuntimeError);
    CHECK_THROWS_AS(mask_batch_width(SIZE_MAX,SIZE_MAX,1),RuntimeError);
    const auto mask=sla::PNGRasterEncoder{}(std::vector<uint8_t>(32,1).data(),8,4,1);
    const png::ReadBuf valid{mask.data(),mask.size()};
    REQUIRE_NOTHROW(sla::sl1_fresh_detail::require_mask_dimensions(valid,8,4));
    CHECK_THROWS_AS(sla::sl1_fresh_detail::require_mask_dimensions(valid,4,8),RuntimeError);
    const auto *data=static_cast<const uint8_t *>(mask.data());std::vector<uint8_t> bytes(data,data+mask.size());
    for(size_t length:{size_t(0),size_t(8),size_t(23),size_t(32)})
        CHECK_THROWS_AS(sla::sl1_fresh_detail::require_mask_dimensions({bytes.data(),length},8,4),RuntimeError);
    bytes[25]=2; // RGB must not multiply a decoder budget intended for Gray8.
    CHECK_THROWS_AS(sla::sl1_fresh_detail::require_mask_dimensions({bytes.data(),bytes.size()},8,4),RuntimeError);
}

TEST_CASE("SL1 batched accounting preserves literal pixels schedule and coordinator progress", "[sla_sl1_mask_batches]") {
    const std::string format=GENERATE("SL1","SL1S");PlanMaskFixture fixture;
    PlanMaskEnvironment mode("0");const auto serial=sla::plan_layer_conversion(fixture.document,format);REQUIRE(serial.sl1_fresh);
    const auto revision=fixture.document.revision();PlanMaskEnvironment::set("1");
    const auto coordinator=std::this_thread::get_id();std::atomic<bool> wrong_thread{false};std::vector<size_t> done_values;
    const auto parallel=sla::plan_layer_conversion(fixture.document,format,[&](size_t done,size_t total) {
        if(std::this_thread::get_id()!=coordinator){wrong_thread=true;return false;}
        CHECK(total==5);CHECK(done<=total);done_values.push_back(done);return true;
    });
    REQUIRE(parallel.sl1_fresh);CHECK_FALSE(wrong_thread);
    REQUIRE_FALSE(done_values.empty());CHECK(done_values.back()==5);CHECK(std::is_sorted(done_values.begin(),done_values.end()));
    const auto &a=*serial.sl1_fresh;const auto &b=*parallel.sl1_fresh;
    CHECK(a.rows==b.rows);CHECK(a.globals.normal==b.globals.normal);CHECK(a.globals.bottom==b.globals.bottom);
    CHECK(a.globals.layer_height==b.globals.layer_height);CHECK(a.globals.bottom_layers==b.globals.bottom_layers);
    CHECK(a.globals.transition_layers==b.globals.transition_layers);CHECK(a.initial_layer_height==b.initial_layer_height);
    CHECK(a.expected_print_time==b.expected_print_time);CHECK(a.projected_transition_exposures==b.projected_transition_exposures);
    CHECK(a.maximum_exposure_change==b.maximum_exposure_change);CHECK(a.material_ml==b.material_ml);
    CHECK(b.material_ml==fixture.expected_ml());CHECK(fixture.document.revision()==revision);CHECK(fixture.document.can_undo());
    // First, middle and final layers differ, including nonbinary gray values
    // beyond the 1MiB boundary and a batch tail containing just one mask.
    for(size_t i=0;i<5;++i) {
        const auto pixels=fixture.document.image(i);CHECK(pixels.cols==1025);CHECK(pixels.rows==1024);
        CHECK(size_t(std::count_if(pixels.buf.begin(),pixels.buf.end(),[](uint8_t p){return p!=0;}))==fixture.lit[i]);
    }
}

TEST_CASE("SL1 batched accounting cancels every coordinator checkpoint and retries", "[sla_sl1_mask_batches]") {
    PlanMaskEnvironment mode("1");PlanMaskFixture fixture;size_t calls=0;
    const auto expected=sla::sl1_fresh_export_plan(fixture.document,[&](size_t,size_t){++calls;return true;});
    /* per-batch plus per-layer coordinator checkpoints */ REQUIRE(calls>=6);const auto revision=fixture.document.revision();const auto rows=fixture.document.fresh_job()->rows;
    const auto coordinator=std::this_thread::get_id();
    for(size_t stop=1;stop<=calls;++stop) {
        CAPTURE(stop,calls);size_t seen=0;std::atomic<bool> wrong_thread{false};
        CHECK_THROWS_AS(sla::sl1_fresh_export_plan(fixture.document,[&](size_t,size_t){
            if(std::this_thread::get_id()!=coordinator){wrong_thread=true;return false;}
            return ++seen<stop;
        }),sla::LayerOperationCancelled);
        CHECK(seen==stop);CHECK_FALSE(wrong_thread);CHECK(fixture.document.revision()==revision);
        CHECK(fixture.document.fresh_job()->rows==rows);CHECK(fixture.document.can_undo());CHECK_FALSE(fixture.document.can_redo());
    }
    CHECK(sla::sl1_fresh_export_plan(fixture.document).material_ml==expected.material_ml);
    CHECK_THROWS_AS(sla::sl1_fresh_export_plan(fixture.document,[](size_t done,size_t){
        if(done==1)throw std::logic_error("caller failure");return true;
    }),std::logic_error);
    CHECK(sla::sl1_fresh_export_plan(fixture.document).material_ml==fixture.expected_ml());
}

TEST_CASE("SL1 batched accounting rejects broken PNG bodies in every batch position", "[sla_sl1_mask_batches]") {
    const size_t broken_index=GENERATE(size_t(0),size_t(3),size_t(4));PlanMaskEnvironment mode("1");PlanMaskFixture fixture;
    std::vector<sla::EncodedRaster> masks;
    for(size_t i=0;i<5;++i) {
        const auto encoded=fixture.document.encoded_layer(i);const auto *data=static_cast<const uint8_t *>(encoded.buf);
        std::vector<uint8_t> bytes(data,data+encoded.sz);if(i==broken_index)bytes.back()^=1; // IEND CRC.
        masks.emplace_back(std::move(bytes),"png");
    }
    const auto broken=sla::LayerDocument::from_fresh_rasters(std::move(masks),*fixture.document.fresh_job());
    CHECK_THROWS_WITH(sla::sl1_fresh_export_plan(broken),"Cannot decode grayscale layer PNG");
    CHECK(broken.revision()==0);CHECK_FALSE(broken.can_undo());
    CHECK(sla::sl1_fresh_export_plan(fixture.document).material_ml==fixture.expected_ml());
}

TEST_CASE("Concurrent SL1 plans keep cancellation and callback ownership independent", "[sla_sl1_mask_batches]") {
    PlanMaskEnvironment mode("1");PlanMaskFixture fixture;
    std::promise<void> start,ready_a,ready_b;auto signal=start.get_future().share();
    auto run=[&](bool cancel,std::promise<void> &ready) {
        const auto coordinator=std::this_thread::get_id();std::atomic<bool> wrong_thread{false};
        ready.set_value();signal.wait();
        try {
            const auto plan=sla::sl1_fresh_export_plan(fixture.document,[&](size_t done,size_t){
                if(std::this_thread::get_id()!=coordinator){wrong_thread=true;return false;}
                return !cancel || done<2;
            });
            return std::make_pair(!cancel && !wrong_thread,plan.material_ml);
        } catch(const sla::LayerOperationCancelled &) {return std::make_pair(cancel && !wrong_thread,0.);}
    };
    auto a=std::async(std::launch::async,[&]{return run(true,ready_a);});
    auto b=std::async(std::launch::async,[&]{return run(false,ready_b);});
    ready_a.get_future().wait();ready_b.get_future().wait();start.set_value();
    const auto cancelled=a.get(),completed=b.get();CHECK(cancelled.first);CHECK(cancelled.second==0.);
    CHECK(completed.first);CHECK(completed.second==fixture.expected_ml());CHECK(fixture.document.can_undo());
}

TEST_CASE("Worker sizing is bounded by RAM budget, hardware threads and remaining work", "[sla_sl1_mask_batches]") {
    constexpr uint64_t GiB=uint64_t(1)<<30;
    // 8K Gray8 (~33 Mpx) at 4 bytes/pixel on the 6 GiB cap with 15 threads.
    CHECK(sla::memory_bounded_workers(6*GiB,uint64_t(7680)*4320*4,1000,15)==15);
    // 12K (~75 Mpx): 6 GiB / 299 MB = 21 -> capped by hardware.
    CHECK(sla::memory_bounded_workers(6*GiB,uint64_t(11520)*6480*4,1000,64)==21);
    // Small host: 2 GiB budget (8 GB RAM) at 12K -> 7 workers, never 0.
    CHECK(sla::memory_bounded_workers(2*GiB,uint64_t(11520)*6480*4,1000,64)==7);
    CHECK(sla::memory_bounded_workers(1,uint64_t(1)<<40,1000,64)==1);
    CHECK(sla::memory_bounded_workers(6*GiB,1,3,64)==3);
    CHECK(sla::memory_bounded_workers(6*GiB,1,0,64)==0);
    CHECK(sla::inflight_raster_budget(8*GiB)==2*GiB);
    CHECK(sla::inflight_raster_budget(16*GiB)==4*GiB);
    CHECK(sla::inflight_raster_budget(256*GiB)==6*GiB);
    CHECK(sla::inflight_raster_budget(0)==GiB);
}

TEST_CASE("Batched runner keeps callbacks on the coordinator, covers every item once and propagates errors", "[sla_sl1_mask_batches]") {
    const auto coordinator=std::this_thread::get_id();std::atomic<bool> wrong{false};
    std::vector<std::atomic<int>> hits(103);std::vector<size_t> firsts;
    sla::for_each_in_batches(hits.size(),5,[&](size_t i){++hits[i];},
        [&](size_t first){if(std::this_thread::get_id()!=coordinator)wrong=true;firsts.push_back(first);},
        [&](size_t,size_t){if(std::this_thread::get_id()!=coordinator)wrong=true;});
    CHECK_FALSE(wrong);for(const auto &h:hits)CHECK(h==1);
    CHECK(firsts.front()==0);CHECK(firsts.size()==6); // batches of 4*5=20
    // A cancelling "before" callback stops all later work.
    std::atomic<size_t> ran{0};
    CHECK_THROWS_AS(sla::for_each_in_batches(100,4,[&](size_t){++ran;},[&](size_t first){
        if(first>=32)throw sla::LayerOperationCancelled();},[](size_t,size_t){}),sla::LayerOperationCancelled);
    CHECK(ran==32);
    CHECK_THROWS_AS(sla::for_each_in_batches(50,4,[&](size_t i){if(i==37)throw std::runtime_error("x");},
        [](size_t){},[](size_t,size_t){}),std::runtime_error);
}

TEST_CASE("Parallel SL1 readback verification detects a tampered layer in any position", "[sla_sl1_mask_batches]") {
    PlanMaskFixture fixture;const auto job=*fixture.document.fresh_job();
    const auto copy_masks=[&](size_t tamper,int mode) {
        std::vector<sla::EncodedRaster> masks;
        for(size_t i=0;i<PlanMaskFixture::count;++i) {
            const auto encoded=fixture.document.encoded_layer(i);const auto *data=static_cast<const uint8_t *>(encoded.buf);
            std::vector<uint8_t> bytes(data,data+encoded.sz);
            if(i==tamper&&mode==1)bytes.back()^=1; // IEND CRC: undecodable
            if(i==tamper&&mode==2){ // valid PNG, one pixel differs
                auto pixels=fixture.document.image(i);pixels.buf[12345]^=0x40;
                const auto again=sla::PNGRasterEncoder{}(pixels.buf.data(),pixels.cols,pixels.rows,1);
                const auto *p=static_cast<const uint8_t *>(again.data());bytes.assign(p,p+again.size());
            }
            masks.emplace_back(std::move(bytes),"png");
        }
        return sla::LayerDocument::from_fresh_rasters(std::move(masks),job);
    };
    const auto geometry=job.geometry;
    // Identical documents (separately encoded bytes path and equal-bytes path) pass.
    const auto same=copy_masks(0,0);
    for(size_t workers:{size_t(1),size_t(3),size_t(0)})
        CHECK_NOTHROW(sla::sl1_fresh_verify_detail::verify_layer_pixels(same,fixture.document,geometry,PlanMaskFixture::count,{},workers));
    for(size_t index:{size_t(0),size_t(2),size_t(4)}) for(int mode:{1,2}) {
        CAPTURE(index,mode);
        // LayerDocument::from_fresh_rasters already rejects a bad IEND CRC only when decoded;
        // build lazily and expect either construction or verification to throw.
        bool threw=false;
        try {
            const auto bad=copy_masks(index,mode);
            for(size_t workers:{size_t(1),size_t(4)})
                CHECK_THROWS(sla::sl1_fresh_verify_detail::verify_layer_pixels(bad,fixture.document,geometry,PlanMaskFixture::count,{},workers));
            threw=true;
        } catch(const std::exception &) {threw=true;}
        CHECK(threw);
    }
    // Cancellation from the checkpoint propagates.
    CHECK_THROWS_AS(sla::sl1_fresh_verify_detail::verify_layer_pixels(same,fixture.document,geometry,PlanMaskFixture::count,
        []{throw sla::LayerOperationCancelled();}),sla::LayerOperationCancelled);
}

TEST_CASE("Streaming runner has deterministic coordinator progress, one run per item and clean shutdown", "[sla_sl1_mask_batches]") {
    const auto coordinator=std::this_thread::get_id();
    for(size_t workers:{size_t(1),size_t(5)}) {
        CAPTURE(workers);
        std::atomic<bool> wrong{false};std::vector<std::atomic<int>> hits(103);std::vector<size_t> seen;
        sla::for_each_streaming(hits.size(),workers,[&](size_t i){++hits[i];},
            [&](size_t done){if(std::this_thread::get_id()!=coordinator)wrong=true;seen.push_back(done);});
        CHECK_FALSE(wrong);for(const auto &h:hits)CHECK(h==1);
        // One checkpoint before any work and one per finished item, in order.
        REQUIRE(seen.size()==hits.size()+1);for(size_t i=0;i<seen.size();++i)CHECK(seen[i]==i);
        seen.clear();
        sla::for_each_streaming(0,workers,[](size_t){throw std::logic_error("no items");},[&](size_t done){seen.push_back(done);});
        CHECK(seen==std::vector<size_t>{0});
        // A cancelling checkpoint stops new items; in-flight ones finish before it returns.
        std::atomic<size_t> started{0},finished{0};
        CHECK_THROWS_AS(sla::for_each_streaming(1000,workers,[&](size_t){
            ++started;std::this_thread::sleep_for(std::chrono::milliseconds(1));++finished;
        },[&](size_t done){if(done>=10)throw sla::LayerOperationCancelled();}),sla::LayerOperationCancelled);
        CHECK(started==finished); // no worker still inside body()
        CHECK(started<=10+workers);
        const auto after=started.load();std::this_thread::sleep_for(std::chrono::milliseconds(20));
        CHECK(started==after);    // and none started later
        // A failing item surfaces with its own type and also stops the rest.
        CHECK_THROWS_AS(sla::for_each_streaming(50,workers,[&](size_t i){if(i==37)throw std::runtime_error("x");},
            [](size_t){}),std::runtime_error);
    }
}

TEST_CASE("Rasterizer lit-pixel ledger is trusted only for byte-identical masks", "[sla_sl1_mask_batches]") {
    constexpr size_t w=PlanMaskFixture::width,h=PlanMaskFixture::height;
    PlanMaskFixture fixture;
    const auto exact=sla::sl1_fresh_export_plan(fixture.document);
    REQUIRE(exact.material_ml==fixture.expected_ml());
    // Exact counts reproduce the decoded plan bit for bit.
    const sla::MaskLitLedger honest(fixture.document,w,h,fixture.lit);
    CHECK(sla::sl1_fresh_export_plan(fixture.document,{},&honest).material_ml==exact.material_ml);
    // A matching layer really is taken from the ledger: a skewed count shows up.
    auto skewed_counts=fixture.lit;skewed_counts[3]+=100;
    const sla::MaskLitLedger skewed(fixture.document,w,h,skewed_counts);
    CHECK(sla::sl1_fresh_export_plan(fixture.document,{},&skewed).material_ml>exact.material_ml);
    // Wrong geometry, or a layer the rasterizer did not count, is ignored.
    const sla::MaskLitLedger wrong_geometry(fixture.document,w+1,h,skewed_counts);
    CHECK(sla::sl1_fresh_export_plan(fixture.document,{},&wrong_geometry).material_ml==exact.material_ml);
    auto uncounted=skewed_counts;uncounted[3]=SIZE_MAX;
    const sla::MaskLitLedger partial(fixture.document,w,h,uncounted);
    CHECK(sla::sl1_fresh_export_plan(fixture.document,{},&partial).material_ml==exact.material_ml);
    CHECK_FALSE(partial.lit_pixels(3,fixture.document.encoded_layer(3)).has_value());
    CHECK(partial.lit_pixels(2,fixture.document.encoded_layer(2))==fixture.lit[2]);
    CHECK_FALSE(partial.lit_pixels(2,fixture.document.encoded_layer(1)).has_value()); // other layer's bytes
    CHECK_FALSE(partial.lit_pixels(99,fixture.document.encoded_layer(1)).has_value());
    // Editing the layer changes its PNG bytes, so even the skewed ledger can no
    // longer overrule the decoded pixels.
    REQUIRE(fixture.document.paint_stroke(3,3,{{0,0}},0,1));
    const auto edited=sla::sl1_fresh_export_plan(fixture.document);
    CHECK(edited.material_ml>exact.material_ml);
    CHECK(sla::sl1_fresh_export_plan(fixture.document,{},&skewed).material_ml==edited.material_ml);
    // Corrupt bytes: a ledger entry cannot vouch for them; decoding still rejects.
    std::vector<sla::EncodedRaster> masks;
    for(size_t i=0;i<PlanMaskFixture::count;++i) {
        const auto encoded=fixture.document.encoded_layer(i);const auto *data=static_cast<const uint8_t *>(encoded.buf);
        std::vector<uint8_t> bytes(data,data+encoded.sz);if(i==2)bytes.back()^=1;
        masks.emplace_back(std::move(bytes),"png");
    }
    const auto broken=sla::LayerDocument::from_fresh_rasters(std::move(masks),*fixture.document.fresh_job());
    CHECK_THROWS(sla::sl1_fresh_export_plan(broken,{},&skewed));
}
