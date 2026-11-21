#include <chrono>
#include <random>
#include <nlohmann/json.hpp>
// SPDX-License-Identifier: AGPL-3.0-or-later
// Owned fresh CXDLPv4 contract through public export APIs. The external UVtools
// gate separately checks raw bytes against the reference's normalized view.
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <boost/filesystem.hpp>
#include <array>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include "libslic3r/SLAPrint.hpp"
#include "libslic3r/Format/CrealityCXDLPv1Archive.hpp"
#include "libslic3r/Format/CrealityContourArea.hpp"
#include "libslic3r/SLA/LayerDocument.hpp"
#include "libslic3r/SLA/ProjectArchive.hpp"
#include "libslic3r/SLA/CrealityCXDLPv4LayerParameters.hpp"
#include "libslic3r/SLA/CrealityCXDLPv4JobReader.hpp"
#include "libslic3r/SLA/CrealityCXDLPv4MaskReader.hpp"
#include "libslic3r/SLA/ExportSnapshot.hpp"
#include "libslic3r/SLA/ExportRequest.hpp"
#include "libslic3r/SLA/LayerConversionPlan.hpp"
#include "libslic3r/SLA/CrealityCXDLPv4FreshExportPlan.hpp"
#include <thread>
using namespace Slic3r;
namespace {
using F=sla::LayerParameter;
struct FreshCXDirectory {
    boost::filesystem::path path=boost::filesystem::unique_path("fresh-cx-%%%%");
    bool keep=false;
    ~FreshCXDirectory(){if(!keep){boost::system::error_code error;boost::filesystem::remove_all(path,error);}}
};
DynamicPrintConfig fresh_cx_profile(bool portrait) {
    DynamicPrintConfig config;config.apply(SLAFullPrintConfig{});
    config.option("printer_technology")->setInt(ptSLA);
    config.set("sla_archive_format","cxdlpv4");config.set("printer_model","CL-60");
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


sla::FreshLayerJob fresh_cx_intent(bool portrait,sla::FreshLayerProducer producer) {
    sla::FreshLayerJob intent;intent.producer=producer;
    intent.geometry=portrait?sla::LayerRasterGeometry{4,8,4.,16.}:sla::LayerRasterGeometry{8,4,16.,4.};
    intent.globals.layer_height=.05;intent.globals.bottom_layers=1;intent.globals.transition_layers=0;
    intent.globals.normal={{F::Exposure,3.25},{F::LiftDistance,3.5},{F::LiftSpeed,60.},
        {F::LiftDistance2,1.25},{F::LiftSpeed2,180.},{F::RetractDistance2,.75},{F::RetractSpeed,150.},
        {F::RetractSpeed2,240.},{F::WaitBefore,.12},{F::WaitAfter,.34},{F::WaitAfterLift,.56},
        {F::LightOff,0.},{F::LightPWM,200.}};
    intent.globals.bottom={{F::Exposure,9.5},{F::LiftDistance,4.5},{F::LiftSpeed,90.},
        {F::LiftDistance2,2.25},{F::LiftSpeed2,120.},{F::RetractDistance2,1.25},{F::RetractSpeed,100.},
        {F::RetractSpeed2,80.},{F::WaitBefore,.22},{F::WaitAfter,.44},{F::WaitAfterLift,.66},
        {F::LightOff,0.},{F::LightPWM,129.}};
    for(size_t i=0;i<3;++i) {
        auto row=i?intent.globals.normal:intent.globals.bottom;
        if(i==1){row[F::LiftDistance]=2.5;row[F::LiftSpeed]=120.;row[F::WaitBefore]=.78;row[F::LightPWM]=180.;}
        row[F::PositionZ]=std::vector<double>{.05,.1,.15}[i];intent.rows.push_back(row);
    }
    return intent;
}
sla::LayerParameters cx_wire_values(const sla::LayerParameters &input) {
    auto result=input;for(auto &[field,value]:result)value=double(float(value));
    result[F::LiftDistance]=double(float(input.at(F::LiftDistance)+input.at(F::LiftDistance2)))-result.at(F::LiftDistance2);
    return result;
}
std::vector<uint8_t> fresh_cx_read(const boost::filesystem::path &path) {
    std::ifstream input(path.string(),std::ios::binary);return std::vector<uint8_t>(std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>());
}
void cx_check_print(const boost::filesystem::path &path,const sla::FreshLayerJob &intent,const std::vector<uint8_t> &pixels) {
    const auto bytes=fresh_cx_read(path);const sla::CrealityCXDLPv4Layout layout(bytes);
    const auto native=sla::cxdlpv4_native_globals(layout);
    CHECK(native.normal==cx_wire_values(intent.globals.normal));
    auto expected_bottom=cx_wire_values(intent.globals.bottom);
    for(F field:{F::WaitBefore,F::WaitAfter,F::WaitAfterLift,F::RetractSpeed,F::RetractDistance2,F::RetractSpeed2})expected_bottom.erase(field);
    CHECK(native.bottom==expected_bottom);CHECK(native.bottom.size()==7);
    CHECK(native.layer_height==double(float(.05)));CHECK(native.bottom_layers==1);
    CHECK(native.parameter_bottom_layers==1);CHECK(native.transition_layers==0);
    CHECK(layout.number(layout.base+4)==float(intent.geometry.width_mm));
    CHECK(layout.number(layout.base+8)==float(intent.geometry.height_mm));
    CHECK(layout.number(layout.base+16)==float(.15));
    // Independent reference literal: 300 preparation + three public row
    // intervals yields float32 337.13s; the native integer truncates to337.
    CHECK(layout.integer(layout.base+44)==337);
    const auto saved=sla::LayerDocument::load_archive(path.string());
    const auto view=saved.cxdlpv4_reference_bottom();REQUIRE(view);
    CHECK(view->meaningful_bottom_layer==std::optional<size_t>(0));CHECK(view->virtual_bottom.size()==6);
    for(F field:{F::WaitBefore,F::WaitAfter,F::WaitAfterLift,F::RetractSpeed,F::RetractDistance2,F::RetractSpeed2})
        CHECK(view->virtual_bottom.at(field)==double(float(intent.globals.bottom.at(field))));
    CHECK(saved.parameter_capabilities().empty());CHECK_FALSE(saved.job_parameters());
    for(size_t i=0;i<3;++i) {
        CHECK(sla::cxdlpv4_layer_parameters(layout,i)==cx_wire_values(intent.rows[i]));
        const auto image=saved.image(i);CHECK(image.buf==pixels);
        CHECK(image.cols==intent.geometry.columns);CHECK(image.rows==intent.geometry.rows);
    }
    // Actual geometry and final Z, not nominal-height preset, determine volume.
    CHECK(layout.number(layout.integer(layout.base+52)+20)==Catch::Approx(.0006f).margin(1e-9f));
}
}
TEST_CASE("Owned fresh CXDLPv4 intents survive both export routes and restart", "[sla_cxdlpv4_fresh]") {
    const bool portrait=GENERATE(false,true);const bool direct=GENERATE(false,true);const bool restart=GENERATE(false,true);
    const auto producer=GENERATE(sla::FreshLayerProducer::Raster,sla::FreshLayerProducer::ModelSlice,sla::FreshLayerProducer::Calibration,sla::FreshLayerProducer::PCB);
    CAPTURE(portrait,direct,restart,int(producer));
    const auto config=fresh_cx_profile(portrait);const auto intent=fresh_cx_intent(portrait,producer);
    std::vector<uint8_t> pixels(32,0);pixels[1]=255;pixels[20]=127;
    const auto mask=sla::PNGRasterEncoder{}(pixels.data(),portrait?4:8,portrait?8:4,1);
    const auto original=sla::LayerDocument::from_fresh_rasters({mask,mask,mask},intent);
    FreshCXDirectory directory;
    if(const char *root=std::getenv("SLA_CXDLPV4_FRESH_FIXTURES")) {
        directory.keep=true;directory.path=boost::filesystem::path(root)/(std::to_string(int(producer))+"-"+(portrait?"portrait":"landscape")+"-"+(direct?"direct":"explicit")+"-"+(restart?"restart":"initial"));
    }
    boost::filesystem::create_directories(directory.path);
    const std::vector<double> positions{.05,.1,.15};
    sla::ProjectRasterState state{original,positions,"",{},sla::ProjectLayerOrigin::GeneratedRaster};
    if(producer==sla::FreshLayerProducer::ModelSlice){state.origin=sla::ProjectLayerOrigin::ModelSlice;state.source_fingerprint=std::string(40,'b');}
    const auto project=directory.path/"fresh.sla-project";
    REQUIRE_NOTHROW(sla::save_layer_project(project.string(),state));
    auto document=original;
    if(restart){const auto restored=sla::read_project_layers(project.string());REQUIRE(restored);document=restored->document;}
    const auto job=SLAPrint::create_raster_job(config,document,positions);
    const auto output=directory.path/"output.cxdlpv4";
    if(direct)REQUIRE_NOTHROW(job->export_print(output.string()));
    else REQUIRE_NOTHROW(job->export_layer_document(document,"cxdlpv4",output.string(),{},"fresh-cx"));
    cx_check_print(output,intent,pixels);
    REQUIRE(document.fresh_job());CHECK(document.fresh_job()->rows==intent.rows);
    CHECK(document.fresh_job()->globals.normal==intent.globals.normal);CHECK(document.fresh_job()->globals.bottom==intent.globals.bottom);
    CHECK(job->layer_document()->fresh_job()->rows==intent.rows);
    CHECK(job->full_print_config().opt_float("exposure_time")==2.);
    CHECK(job->full_print_config().opt_float("sla_lift_height")==7.);
}
TEST_CASE("Fresh CXDLPv4 refuses lossy fields before replacing output", "[sla_cxdlpv4_fresh_negative]") {
    const bool direct=GENERATE(false,true);const int defect=GENERATE(0,1,2,3,4);CAPTURE(direct,defect);
    auto intent=fresh_cx_intent(false,sla::FreshLayerProducer::Raster);auto config=fresh_cx_profile(false);
    std::vector<uint8_t> pixels(32,0);pixels[1]=255;pixels[20]=127;
    const char *field="";
    if(defect==0){intent.globals.bottom[F::WaitBefore]=.23;field="bottom.WaitBefore";}
    if(defect==1){intent.globals.bottom[F::RetractSpeed]=.004;intent.rows[0][F::RetractSpeed]=.004;field="bottom.RetractSpeed";}
    if(defect==2){pixels[1]=1;field="pixels";}
    if(defect==3){intent.globals.transition_layers=2;field="transition_layers";}
    if(defect==4){config.set("printer_model",std::string{});field="printer model identifier";}
    const auto mask=sla::PNGRasterEncoder{}(pixels.data(),8,4,1);
    const auto document=sla::LayerDocument::from_fresh_rasters({mask,mask,mask},intent);
    const auto job=SLAPrint::create_raster_job(config,document,{.05,.1,.15});
    FreshCXDirectory directory;boost::filesystem::create_directories(directory.path);const auto output=directory.path/"keep.cxdlpv4";
    const std::string sentinel="previous valid print";{std::ofstream file(output.string(),std::ios::binary);file<<sentinel;}
    if(direct)CHECK_THROWS_WITH(job->export_print(output.string()),Catch::Matchers::ContainsSubstring(field));
    else CHECK_THROWS_WITH(job->export_layer_document(document,"cxdlpv4",output.string()),Catch::Matchers::ContainsSubstring(field));
    const auto bytes=fresh_cx_read(output);
    const bool unchanged=bytes.size()==sentinel.size() && std::equal(bytes.begin(),bytes.end(),sentinel.begin());
    CHECK(unchanged);
    CHECK(document.fresh_job()->rows==intent.rows);CHECK(document.fresh_job()->globals.bottom==intent.globals.bottom);
}

TEST_CASE("Fresh CXDLPv4 preserves TOTAL when FIRST residual is not float-representable", "[sla_cxdlpv4_fresh_precision]") {
    const bool direct=GENERATE(false,true);
    auto intent=fresh_cx_intent(false,sla::FreshLayerProducer::Raster);
    // Synthetic precision boundary, not a printable motion recommendation.
    // These globals remain independent of the moderate per-layer overrides.
    intent.globals.normal[F::LiftDistance]=99999.99609375;
    intent.globals.normal[F::LiftDistance2]=.01171875;
    const auto config=fresh_cx_profile(false);
    std::vector<uint8_t> pixels(32,0);pixels[1]=255;pixels[20]=127;
    const auto mask=sla::PNGRasterEncoder{}(pixels.data(),8,4,1);
    const auto document=sla::LayerDocument::from_fresh_rasters({mask,mask,mask},intent);
    const auto job=SLAPrint::create_raster_job(config,document,{.05,.1,.15});
    FreshCXDirectory directory;boost::filesystem::create_directories(directory.path);
    if(const char *root=std::getenv("SLA_CXDLPV4_FRESH_PRECISION_FIXTURES")) {
        directory.keep=true;directory.path=boost::filesystem::path(root)/(direct?"direct":"explicit");
        boost::filesystem::create_directories(directory.path);
    }
    const auto output=directory.path/"output.cxdlpv4";
    if(direct)REQUIRE_NOTHROW(job->export_print(output.string()));
    else REQUIRE_NOTHROW(job->export_layer_document(document,"cxdlpv4",output.string()));
    cx_check_print(output,intent,pixels);
}

TEST_CASE("Fresh CXDLPv4 refuses nonfinite material metadata before publication", "[sla_cxdlpv4_fresh_material]") {
    const bool direct=GENERATE(false,true);
    const bool weight=GENERATE(false,true);
    auto config=fresh_cx_profile(false);
    // Finite profile inputs whose product cannot fit the native float field.
    config.set(weight ? "material_density" : "bottle_cost",1e100);
    const auto intent=fresh_cx_intent(false,sla::FreshLayerProducer::Raster);
    std::vector<uint8_t> pixels(32,0);pixels[1]=255;pixels[20]=127;
    const auto mask=sla::PNGRasterEncoder{}(pixels.data(),8,4,1);
    const auto document=sla::LayerDocument::from_fresh_rasters({mask,mask,mask},intent);
    const auto job=SLAPrint::create_raster_job(config,document,{.05,.1,.15});
    FreshCXDirectory directory;boost::filesystem::create_directories(directory.path);
    const auto output=directory.path/"keep.cxdlpv4";
    const std::string sentinel="previous valid print";
    {std::ofstream file(output.string(),std::ios::binary);file<<sentinel;}
    const char *field=weight ? "material_weight" : "material_cost";
    if(direct)CHECK_THROWS_WITH(job->export_print(output.string()),Catch::Matchers::ContainsSubstring(field));
    else CHECK_THROWS_WITH(job->export_layer_document(document,"cxdlpv4",output.string()),Catch::Matchers::ContainsSubstring(field));
    const auto bytes=fresh_cx_read(output);
    const bool unchanged=bytes.size()==sentinel.size() && std::equal(bytes.begin(),bytes.end(),sentinel.begin());
    CHECK(unchanged);
}

TEST_CASE("Fresh CXDLPv4 preserves real resin mass and bottle cost", "[sla_cxdlpv4_fresh_material_valid]") {
    const bool direct=GENERATE(false,true);
    auto config=fresh_cx_profile(false);
    config.set("material_density",1.25);config.set("bottle_cost",60.);config.set("bottle_volume",750.);
    const auto intent=fresh_cx_intent(false,sla::FreshLayerProducer::Raster);
    std::vector<uint8_t> pixels(32,0);pixels[1]=255;pixels[20]=127;
    const auto mask=sla::PNGRasterEncoder{}(pixels.data(),8,4,1);
    const auto document=sla::LayerDocument::from_fresh_rasters({mask,mask,mask},intent);
    const auto job=SLAPrint::create_raster_job(config,document,{.05,.1,.15});
    FreshCXDirectory directory;boost::filesystem::create_directories(directory.path);
    if(const char *root=std::getenv("SLA_CXDLPV4_FRESH_MATERIAL_FIXTURES")) {
        directory.keep=true;directory.path=boost::filesystem::path(root)/(direct?"direct":"explicit");
        boost::filesystem::create_directories(directory.path);
    }
    const auto output=directory.path/"output.cxdlpv4";
    if(direct)REQUIRE_NOTHROW(job->export_print(output.string()));
    else REQUIRE_NOTHROW(job->export_layer_document(document,"cxdlpv4",output.string()));
    cx_check_print(output,intent,pixels);
    const auto bytes=fresh_cx_read(output);const sla::CrealityCXDLPv4Layout layout(bytes);
    const auto parameters=layout.integer(layout.base+52);
    // Independently calculated native literals: 0.0006mL *1.25g/mL;
    // material cost=volume *60currency /750mL bottle, rounded once on wire.
    CHECK(layout.number(parameters+24)==float(.000750000006519258));
    CHECK(layout.number(parameters+28)==float(.000048000001697801054));
}

TEST_CASE("CXDLPv4 native readback preserves masks without an editor document", "[sla_cxdlpv4_readback]") {
    const bool encrypted=GENERATE(false,true);
    auto config=fresh_cx_profile(false);
    auto intent=fresh_cx_intent(false,sla::FreshLayerProducer::ModelSlice);
    std::vector<sla::EncodedRaster> masks;
    std::vector<std::vector<uint8_t>> expected;
    for(size_t layer=0;layer<3;++layer) {
        auto &pixels=expected.emplace_back(32,0);
        for(size_t i=0;i<pixels.size();++i) {
            const auto value=uint8_t((i+layer)%128);
            pixels[i]=value?uint8_t(value*2+1):uint8_t(0);
        }
        masks.push_back(sla::PNGRasterEncoder{}(pixels.data(),8,4,1));
    }
    const auto document=sla::LayerDocument::from_fresh_rasters(std::move(masks),intent);
    auto job=SLAPrint::create_raster_job(config,document,{.05,.1,.15});
    FreshCXDirectory directory;boost::filesystem::create_directories(directory.path);
    const auto file=directory.path/"readback.cxdlpv4";job->export_print(file.string());
    auto bytes=fresh_cx_read(file);
    const auto checksum=[&] {
        const uint32_t crc=sla::creality_checksum(bytes.data(),bytes.size()-4,4);
        for(unsigned i=0;i<4;++i)bytes[bytes.size()-4+i]=uint8_t(crc>>(24-8*i));
    };
    if(encrypted) {
        const sla::CrealityCXDLPv4Layout original(bytes);constexpr uint32_t seed=0x12345678;
        for(size_t i=0;i<original.layers.size();++i) {
            const auto &row=original.layers[i];
            std::vector<uint8_t> payload(bytes.begin()+row.data+44,bytes.begin()+row.data+row.length);
            CrealityCXDLPv4Archive::crypt_mask(payload,seed,uint32_t(i));
            std::copy(payload.begin(),payload.end(),bytes.begin()+row.data+44);
        }
        for(unsigned i=0;i<4;++i)bytes[original.base+68+i]=uint8_t(seed>>(8*i));
        checksum();
    }
    const sla::CrealityCXDLPv4Layout layout(bytes);
    for(size_t i=0;i<3;++i) {
        const auto image=sla::cxdlpv4_layer_image(layout,i);
        CHECK(image.cols==8);CHECK(image.rows==4);CHECK(image.buf==expected[i]);
    }
    CHECK_THROWS_AS(sla::cxdlpv4_layer_image(layout,3),std::out_of_range);
    CHECK_THROWS_AS(sla::cxdlpv4_layer_image(layout,0,[]{throw CanceledException{};}),CanceledException);
    unsigned checkpoints=0;
    CHECK_THROWS_AS(sla::cxdlpv4_layer_image(layout,0,[&] {
        if(++checkpoints==2)throw CanceledException{};
    }),CanceledException);
    CHECK(checkpoints==2);
    auto broken=bytes;broken.back()^=1;
    CHECK_THROWS_AS(sla::CrealityCXDLPv4Layout(broken),RuntimeError);
    if(!encrypted) {
        bytes[layout.layers[0].data+44]=128;bytes[layout.layers[0].data+45]=0;checksum();
        const sla::CrealityCXDLPv4Layout malformed(bytes);
        CHECK_THROWS_AS(sla::cxdlpv4_layer_image(malformed,0),RuntimeError);
    }
}

TEST_CASE("CXDLPv4 readback rejects damaged staging before publication", "[sla_cxdlpv4_readback]") {
    const std::string damage=GENERATE("crc","pixels","truncate");
    auto config=fresh_cx_profile(false);
    const auto intent=fresh_cx_intent(false,sla::FreshLayerProducer::ModelSlice);
    std::vector<uint8_t> pixels(32,255);pixels[0]=0;
    const auto mask=sla::PNGRasterEncoder{}(pixels.data(),8,4,1);
    const auto document=sla::LayerDocument::from_fresh_rasters({mask,mask,mask},intent);
    auto source=SLAPrint::create_raster_job(config,document,{.05,.1,.15});
    auto snapshot=std::make_shared<const sla::ExportSnapshot>(source->capture_export_snapshot());
    FreshCXDirectory directory;boost::filesystem::create_directories(directory.path);
    const auto target=directory.path/"old.cxdlpv4";{std::ofstream file(target.string());file<<"old";}
    const sla::ExportRequest request(snapshot,"cxdlpv4",target.string());bool changed=false;
    const auto corrupt=[&] {
        if(changed)return;
        for(const auto &entry:boost::filesystem::directory_iterator(directory.path)) {
            if(entry.path().filename().string().find(".sla-export-")!=0)continue;
            auto bytes=fresh_cx_read(entry.path());
            // The writer may have created an incomplete stream at an earlier
            // checkpoint. Wait for a complete native file before corrupting it.
            std::optional<size_t> payload;
            try {const sla::CrealityCXDLPv4Layout layout(bytes);payload=layout.layers[0].data+44;}
            catch(const RuntimeError &) {continue;}
            changed=true;
            if(damage=="truncate")bytes.resize(8);
            else if(damage=="crc")bytes.back()^=1;
            else {
                bytes[*payload]^=1; // Valid RLE, altered exposure pixel.
                const auto crc=sla::creality_checksum(bytes.data(),bytes.size()-4,4);
                for(unsigned i=0;i<4;++i)bytes[bytes.size()-4+i]=uint8_t(crc>>(24-8*i));
            }
            std::ofstream output(entry.path().string(),std::ios::binary|std::ios::trunc);
            output.write(reinterpret_cast<const char *>(bytes.data()),std::streamsize(bytes.size()));
            break;
        }
    };
    if(damage=="pixels")CHECK_THROWS_WITH(request.write(corrupt),Catch::Matchers::ContainsSubstring("grayscale pixels"));
    else CHECK_THROWS_AS(request.write(corrupt),RuntimeError);
    CHECK(changed);CHECK(fresh_cx_read(target)==std::vector<uint8_t>{'o','l','d'});
    CHECK(std::distance(boost::filesystem::directory_iterator(directory.path),boost::filesystem::directory_iterator{})==1);
    CHECK(snapshot->generation().current());CHECK(source->layer_document()->image(0).buf==pixels);
}

TEST_CASE("CXDLPv4 planning is cancellable without changing source masks", "[sla_cxdlpv4_plan_progress]") {
    auto intent = fresh_cx_intent(false, sla::FreshLayerProducer::ModelSlice);
    const std::vector<uint8_t> pixels(32, 255);
    const auto mask = sla::PNGRasterEncoder{}(pixels.data(), 8, 4, 1);
    const auto document = sla::LayerDocument::from_fresh_rasters({mask, mask, mask}, intent);
    const auto revision = document.revision();
    const auto owner = std::this_thread::get_id();
    for (const size_t stop : {size_t(0), size_t(1), size_t(3)}) {
        CAPTURE(stop);
        bool reached = false;
        CHECK_THROWS_AS(sla::cxdlpv4_fresh_export_plan(document,
            [&](size_t done, size_t total) {
                CHECK(std::this_thread::get_id() == owner);
                if (total == document.layer_count() && done == stop) {
                    reached = true;
                    return false;
                }
                return true;
            }), sla::LayerOperationCancelled);
        CHECK(reached);
        CHECK(document.revision() == revision);
        CHECK(document.image(0).buf == pixels);
    }
    bool finished = false;
    CHECK_NOTHROW(sla::cxdlpv4_fresh_export_plan(document,
        [&](size_t done, size_t total) {
            CHECK(std::this_thread::get_id() == owner);
            finished |= done == document.layer_count() && total == document.layer_count();
            return true;
        }));
    CHECK(finished);
}

TEST_CASE("CXDLPv4 planning preserves ordered accounting across mask batches", "[sla_cxdlpv4_plan_progress]") {
    auto intent = fresh_cx_intent(false, sla::FreshLayerProducer::ModelSlice);
    intent.rows.clear();
    intent.globals.bottom_layers = 10;
    std::vector<sla::EncodedRaster> masks;
    std::vector<size_t> counts;
    double expected_mm3 = 0., previous_z = 0.;
    for (size_t i = 0; i < 17; ++i) {
        auto row = i < 10 ? intent.globals.bottom : intent.globals.normal;
        row[F::PositionZ] = .025 * double((i + 1) * (i + 2) / 2);
        intent.rows.push_back(row);
        const size_t lit = i < 8 ? i % 2 : i - 6;
        counts.push_back(lit);
        std::vector<uint8_t> pixels(32, 0);
        for (size_t p = 0; p < lit; ++p) pixels[p] = uint8_t(3 + p * 2);
        masks.push_back(sla::PNGRasterEncoder{}(pixels.data(), 8, 4, 1));
        const double native_z = double(float(row.at(F::PositionZ)));
        expected_mm3 += double(lit) * 2. * (native_z - previous_z);
        previous_z = native_z;
    }
    const auto document = sla::LayerDocument::from_fresh_rasters(std::move(masks), intent);
    const auto owner = std::this_thread::get_id();
    size_t previous_done = 0;
    auto plan = sla::plan_layer_conversion(document, "cxdlpv4", [&](size_t done, size_t total) {
        CHECK(std::this_thread::get_id() == owner);
        CHECK(total == 17);
        CHECK(done >= previous_done);
        previous_done = done;
        return true;
    });
    REQUIRE(plan.cxdlpv4_fresh);
    CHECK(previous_done == 17);
    CHECK(plan.cxdlpv4_fresh->meaningful_bottom_layer == std::optional<size_t>(8));
    CHECK(plan.cxdlpv4_fresh->material_ml == float(expected_mm3 / 1000.));
    CHECK(plan.cxdlpv4_fresh->material_ml == .0391f);
    CHECK(plan.cxdlpv4_fresh->rows.size() == 17);
    CHECK_THROWS_WITH(sla::plan_layer_conversion(document, "cxdlpv4", [&](size_t done, size_t) {
        if (done == 8) throw std::logic_error("planning checkpoint");
        return true;
    }), "planning checkpoint");
    CHECK(document.fresh_job()->rows == intent.rows);
    for (size_t i = 0; i < 17; ++i) {
        const auto image = document.image(i);
        CHECK(size_t(std::count_if(image.buf.begin(), image.buf.end(), [](uint8_t p) { return p != 0; })) == counts[i]);
    }
    // A non-native value after the first two batches must still be rejected.
    auto invalid = document.snapshot();
    auto image = invalid.image(16);image.buf[0] = 2;
    invalid.replace_image(16, image);
    CHECK_THROWS_WITH(sla::cxdlpv4_fresh_export_plan(invalid),
        Catch::Matchers::ContainsSubstring("grayscale pixels"));
}


TEST_CASE("CXDLPv4 material and contours count only exposed row spans", "[sla_cxdlpv4_geometry]") {
    auto config=fresh_cx_profile(false);
    config.set("display_width",8.);config.set("display_height",4.);
    config.set("layer_height",.05);config.set("initial_layer_height",.05);
    // Literal shapes exercise empty rows, the last screen pixel, interior
    // zero/gray-1 holes, both span endpoints, and duplicate-mask reuse.
    std::vector<std::vector<uint8_t>> pixels(6,std::vector<uint8_t>(32,0));
    std::fill(pixels[0].begin(),pixels[0].end(),1); // Quantizes to a blank mask.
    pixels[1][31]=2;
    pixels[2][9]=2;pixels[2][10]=255;
    for(size_t i:{size_t(10),size_t(11),size_t(18),size_t(19)})pixels[3][i]=255;
    for(size_t i:{size_t(2),size_t(3),size_t(4),size_t(10),size_t(12),size_t(18),size_t(19),size_t(20)})pixels[4][i]=127;
    pixels[4][11]=1; // A hole after native seven-bit projection.
    pixels[5]=pixels[3];
    std::vector<sla::EncodedRaster> masks;
    for(const auto &image:pixels)masks.push_back(sla::PNGRasterEncoder{}(image.data(),8,4,1));
    const auto document=sla::LayerDocument::from_rasters(std::move(masks));
    auto job=SLAPrint::create_raster_job(config,document,{.05,.1,.15,.2,.25,.3});
    FreshCXDirectory directory;
    if(const char *output=std::getenv("SLA_CXDLPV4_GEOMETRY_FIXTURES")) {
        directory.path=output;directory.keep=true;
    }
    boost::filesystem::create_directories(directory.path);
    const auto file=directory.path/"row-spans.cxdlpv4";
    REQUIRE_NOTHROW(job->export_print(file.string()));
    const auto bytes=fresh_cx_read(file);const sla::CrealityCXDLPv4Layout layout(bytes);
    REQUIRE(layout.columns==8);REQUIRE(layout.rows==4);REQUIRE(layout.layers.size()==6);
    // 0+1+2+4+8+4 exposed pixels, 1 mm² per pixel, 0.05 mm per layer.
    const size_t parameters=layout.integer(layout.base+52);
    CHECK(layout.number(parameters+20)==Catch::Approx(.00095).margin(1.e-10));
    const std::array<uint32_t,6> areas{0,0,0,1000,4000,1000};
    for(size_t i=0;i<pixels.size();++i) {
        CAPTURE(i);
        CHECK(layout.integer(layout.table+40*i+28)==areas[i]);
        auto expected=pixels[i];
        for(auto &value:expected)value=value<2?0:uint8_t((value>>1)*2+1);
        CHECK(sla::cxdlpv4_layer_image(layout,i).buf==expected);
        CHECK(document.image(i).buf==pixels[i]);
    }
}

// Slender owned masks exercise each axis limit without allocating a full LCD.
// They verify the declared software raster domain, not a physical printer.
TEST_CASE("CXDLPv4 contour accounting covers the declared raster axes", "[sla_cxdlpv4_contour_bounds]") {
    const auto dimensions = GENERATE(std::make_pair(1u,8193u), std::make_pair(16385u,1u),
        std::make_pair(2u,32768u), std::make_pair(32768u,2u),
        std::make_pair(2u,8192u), std::make_pair(16384u,2u));
    const bool direct = GENERATE(false,true);
    const size_t columns = dimensions.first, rows = dimensions.second;
    CAPTURE(columns,rows,direct);
    constexpr double pitch = .0625; // Exact binary and four-decimal native pitch.
    auto config = fresh_cx_profile(false);
    config.set("display_pixels_x",int(columns)); config.set("display_pixels_y",int(rows));
    config.set("display_width",columns*pitch); config.set("display_height",rows*pitch);
    config.set("layer_height",.05); config.set("initial_layer_height",.05);
    config.set("sla_bottom_layers",1);
    const std::vector<uint8_t> pixels(columns*rows,255);
    if (columns>16384 || rows>8192)
        CHECK_THROWS(CrealityCXDLPv1Archive::encode_mask(pixels.data(),columns,rows,1));
    const auto mask = sla::PNGRasterEncoder{}(pixels.data(),columns,rows,1);
    const auto document = sla::LayerDocument::from_rasters({mask});
    const auto job = SLAPrint::create_raster_job(config,document,{.05});
    FreshCXDirectory directory;
    if (const char *output = std::getenv("SLA_CXDLPV4_CONTOUR_BOUND_FIXTURES")) {
        directory.path = boost::filesystem::path(output)/(std::to_string(columns)+"x"+
            std::to_string(rows)+(direct?"-direct":"-document"));
        directory.keep = true;
    }
    boost::filesystem::create_directories(directory.path);
    const auto path = directory.path/"original.cxdlpv4";
    if (direct) REQUIRE_NOTHROW(job->export_print(path.string()));
    else REQUIRE_NOTHROW(job->export_layer_document(document,"cxdlpv4",path.string(),{},"bounds"));
    const auto original_bytes = fresh_cx_read(path);
    const sla::CrealityCXDLPv4Layout original(original_bytes);
    REQUIRE(original.layers.size()==1);
    CHECK(original.columns==columns); CHECK(original.rows==rows);
    CHECK(original.width==columns*pitch); CHECK(original.height==rows*pitch);
    CHECK(sla::cxdlpv4_layer_image(original,0).buf==pixels);
    // A solid rectangle's contour is measured between pixel centres.
    const double rectangle_area = double(columns-1)*double(rows-1);
    CHECK(original.integer(original.table+28)==uint32_t(rectangle_area*pitch*pitch*1000.));
    CHECK(original.number(original.integer(original.base+52)+20)==
        Catch::Approx(double(pixels.size())*pitch*pitch*double(float(.05))/1000.).margin(1.e-9));

    auto edited = sla::LayerDocument::load_archive(path.string());
    auto image = edited.image(0); image.buf.front()=0;
    REQUIRE_NOTHROW(edited.replace_image(0,image));
    const auto edited_path = directory.path/"edited.cxdlpv4";
    REQUIRE_NOTHROW(edited.save_archive(edited_path.string()));
    const auto edited_bytes = fresh_cx_read(edited_path);
    const sla::CrealityCXDLPv4Layout saved(edited_bytes);
    CHECK(saved.columns==columns); CHECK(saved.rows==rows);
    CHECK(sla::cxdlpv4_layer_image(saved,0).buf==image.buf);
    // Removing one corner of a rectangle at least two pixels wide/high cuts
    // half a pixel-centre square. One-dimensional contours still have area 0.
    const double edited_area = columns>1 && rows>1 ? rectangle_area-.5 : 0.;
    CHECK(saved.integer(saved.table+28)==uint32_t(edited_area*pitch*pitch*1000.));
    CHECK(saved.number(saved.integer(saved.base+52)+20)==
        Catch::Approx(double(pixels.size()-1)*pitch*pitch*double(float(.05))/1000.).margin(1.e-9));
    CHECK(document.image(0).buf==pixels);
    CHECK(fresh_cx_read(path)==original_bytes);
}

TEST_CASE("Creality contour geometry rejects axes and raster budget overflow", "[sla_cxdlpv4_contour_bounds]") {
    // Each invalid geometry must fail before dereferencing the one-byte input.
    // The public native writer's valid upper axes are exercised above.
    const std::vector<uint8_t> pixel(1,255);
    for (const auto dimensions : {std::make_pair(size_t(0),size_t(1)),
         std::make_pair(size_t(1),size_t(0)), std::make_pair(size_t(32769),size_t(1)),
         std::make_pair(size_t(1),size_t(32769)), std::make_pair(size_t(32768),size_t(4097)),
         std::make_pair(size_t(4097),size_t(32768))}) {
        CAPTURE(dimensions.first,dimensions.second);
        CHECK_THROWS_AS(sla::creality_largest_contour_area(pixel.data(),dimensions.first,dimensions.second),std::invalid_argument);
    }
}


TEST_CASE("CXDLPv4 projection errors identify the setting and both values",
          "[sla_cxdlpv4_fresh_negative][sla_export_parameter_diagnostic]") {
    const bool direct = GENERATE(false, true);
    const bool meaningful_bottom = GENERATE(false, true);
    CAPTURE(direct, meaningful_bottom);
    auto intent = fresh_cx_intent(false, sla::FreshLayerProducer::ModelSlice);
    std::vector<uint8_t> pixels(32, 0);
    if (meaningful_bottom) {
        pixels[1] = 255;
        pixels[20] = 127;
        // The inactive second phase legitimately requests zero speed. The
        // file's public bottom projection still substitutes 80 mm/min.
        intent.globals.bottom[F::RetractDistance2] = 0.;
        intent.globals.bottom[F::RetractSpeed2] = 0.;
        intent.rows[0][F::RetractDistance2] = 0.;
        intent.rows[0][F::RetractSpeed2] = 0.;
    }
    const auto mask = sla::PNGRasterEncoder{}(pixels.data(), 8, 4, 1);
    const auto document = sla::LayerDocument::from_fresh_rasters({mask, mask, mask}, intent);
    const auto job = SLAPrint::create_raster_job(fresh_cx_profile(false), document, {.05, .1, .15});
    const auto configuration = job->full_print_config();
    const auto revision = document.revision();
    FreshCXDirectory directory;
    boost::filesystem::create_directories(directory.path);
    const auto output = directory.path / "preserved.cxdlpv4";
    { std::ofstream file(output.string()); file << "old"; }
    std::string message;
    const auto save = [&] {
        try {
            if (direct) job->export_print(output.string());
            else job->export_layer_document(document, "cxdlpv4", output.string());
        } catch (const RuntimeError &error) {
            message = error.what();
            throw;
        }
    };
    CHECK_THROWS_AS(save(), RuntimeError);
    CHECK_THAT(message, Catch::Matchers::ContainsSubstring(
        meaningful_bottom ? "bottom.RetractSpeed2" : "bottom.WaitBefore"));
    CHECK_THAT(message, Catch::Matchers::ContainsSubstring(meaningful_bottom ?
        "Requested: 0 mm/min. Read back from CXDLPv4: 80 mm/min." :
        "Requested: 0.22 s. Read back from CXDLPv4: 0 s."));
    CHECK_THAT(message, Catch::Matchers::ContainsSubstring(meaningful_bottom ?
        "This value is derived from bottom layer 1." :
        "No bottom layer has more than one lit pixel and exposure above 0.01 s after format rounding"));
    CHECK_THAT(message, Catch::Matchers::ContainsSubstring(meaningful_bottom ?
        "Materials > LCD exposure and motion > Bottom layers > Second retract speed" :
        "Materials > LCD exposure and motion > Bottom layers > Wait before exposure"));
    if (meaningful_bottom)
        CHECK_THAT(message, Catch::Matchers::ContainsSubstring("even when that phase has zero travel"));
    CHECK(fresh_cx_read(output) == std::vector<uint8_t>{'o', 'l', 'd'});
    CHECK(std::distance(boost::filesystem::directory_iterator(directory.path),
                        boost::filesystem::directory_iterator{}) == 1);
    CHECK(document.revision() == revision);
    CHECK(document.fresh_job()->rows == intent.rows);
    CHECK(document.fresh_job()->globals.bottom == intent.globals.bottom);
    CHECK(job->full_print_config() == configuration);
}

// End-to-end planner contract, including byte positions at SIMD boundaries.
// Expected membership is literal; do not use the optimized validator as oracle.
TEST_CASE("CXDLPv4 planner checks every native grayscale value and tail", "[sla_cxdlpv4_plan_scan]") {
    auto intent = fresh_cx_intent(false, sla::FreshLayerProducer::Raster);
    intent.geometry.columns = 65; intent.geometry.rows = 1;
    for (const size_t position : {size_t(0), size_t(15), size_t(16), size_t(31), size_t(32), size_t(64)}) {
        for (unsigned value = 0; value != 256; ++value) {
            CAPTURE(position, value);
            std::vector<uint8_t> pixels(65, 255); pixels[position] = uint8_t(value);
            const auto mask = sla::PNGRasterEncoder{}(pixels.data(), 65, 1, 1);
            // Gray8 input is legal; the destination must reject its lossy colors.
            const auto document = sla::LayerDocument::from_fresh_rasters({mask, mask, mask}, intent);
            const bool supported = value == 0 || (value >= 3 && value % 2 == 1);
            if (!supported) {
                CHECK_THROWS_WITH(sla::cxdlpv4_fresh_export_plan(document),
                    "CXDLPv4 cannot preserve grayscale pixels outside 0/odd 3..255");
            } else {
                const auto plan = sla::cxdlpv4_fresh_export_plan(document);
                CHECK(plan.meaningful_bottom_layer == std::optional<size_t>(0));
                CHECK(plan.rows.size() == 3);
                const size_t lit = value == 0 ? 64 : 65;
                const double volume = double(lit) * (16. / 65.) * 4. * double(float(.15)) / 1000.;
                CHECK(plan.material_ml == Catch::Approx(volume).margin(1e-9));
            }
            CHECK(document.image(0).buf == pixels);
            CHECK(document.revision() == 0);
            CHECK(document.fresh_job()->rows == intent.rows);
        }
    }
}

TEST_CASE("CXDLPv4 planning still fully decodes PNG before replacing output", "[sla_cxdlpv4_plan_scan]") {
    const auto intent = fresh_cx_intent(false, sla::FreshLayerProducer::Raster);
    const std::vector<uint8_t> pixels(32, 255);
    const auto mask = sla::PNGRasterEncoder{}(pixels.data(), 8, 4, 1);
    const auto good = sla::LayerDocument::from_fresh_rasters({mask, mask, mask}, intent);
    const auto *begin = static_cast<const uint8_t *>(mask.data());
    std::vector<uint8_t> bytes(begin, begin+mask.size());
    const auto word = [&](size_t at) { return (uint32_t(bytes[at])<<24) | (uint32_t(bytes[at+1])<<16) |
        (uint32_t(bytes[at+2])<<8) | bytes[at+3]; };
    size_t idat = 8;
    while (idat+12 <= bytes.size() && std::memcmp(bytes.data()+idat+4, "IDAT", 4) != 0)
        idat += size_t(word(idat))+12;
    REQUIRE(idat+12 <= bytes.size());
    const size_t length = word(idat); REQUIRE(idat+length+12 <= bytes.size());
    bytes[idat+length+8] ^= 1; // Bad CRC; valid dimensions and compressed pixels.
    const sla::EncodedRaster damaged(std::move(bytes), "png");
    const auto document = sla::LayerDocument::from_fresh_rasters({mask, mask, damaged}, intent);
    CHECK_THROWS_WITH(sla::cxdlpv4_fresh_export_plan(document), "Cannot decode grayscale layer PNG");
    auto job = SLAPrint::create_raster_job(fresh_cx_profile(false), good, {.05,.1,.15});
    FreshCXDirectory directory; boost::filesystem::create_directories(directory.path);
    const auto target = directory.path/"keep.cxdlpv4";
    { std::ofstream file(target.string()); file << "previous complete output"; }
    CHECK_THROWS_WITH(job->export_layer_document(document, "cxdlpv4", target.string()),
        "Cannot decode grayscale layer PNG");
    const auto retained = fresh_cx_read(target);
    CHECK(std::string(retained.begin(),retained.end()) == "previous complete output");
    CHECK(document.fresh_job()->rows == intent.rows);
}

// Opt-in whole public editor export. Source construction and independent
// reopening are outside the timer. No hardware or general speed threshold here.
TEST_CASE("Measure fresh CXDLPv4 editor planning and full export", "[.][sla_cxdlpv4_plan_benchmark]") {
    const char *destination = std::getenv("SLA_PLAN_BENCH_OUTPUT");
    const char *scenario = std::getenv("SLA_PLAN_BENCH_CASE"); REQUIRE(destination); REQUIRE(scenario);
    const std::string mode(scenario); REQUIRE((mode=="large" || mode=="small" || mode=="noisy"));
    const size_t width = mode=="large" ? 4096 : 200, height = mode=="large" ? 2560 : 120;
    const size_t count = mode=="large" ? 24 : 3;
    const boost::filesystem::path folder(destination); boost::filesystem::create_directories(folder);
    REQUIRE_FALSE(boost::filesystem::exists(folder/"report.json"));
    auto intent = fresh_cx_intent(false, sla::FreshLayerProducer::Raster);
    intent.geometry = {width,height,153.408,87.04};intent.rows.clear();
    std::vector<sla::EncodedRaster> masks; std::vector<double> positions;
    std::vector<uint8_t> pixels(width*height); std::mt19937 random(1729);
    for (size_t layer=0; layer<count; ++layer) {
        std::fill(pixels.begin(),pixels.end(),0);
        for(size_t y=height/5; y<height*4/5; ++y)
            for(size_t x=width/4+layer; x<width*3/4; ++x) pixels[y*width+x]=(y%17==0)?127:255;
        if(mode=="noisy") for(auto &pixel:pixels) {const auto v=uint8_t(random()|1);pixel=v==1?0:v;}
        masks.push_back(sla::PNGRasterEncoder{}(pixels.data(),width,height,1));
        positions.push_back((layer+1)*.05);
        auto row=layer?intent.globals.normal:intent.globals.bottom;row[F::PositionZ]=positions.back();intent.rows.push_back(row);
    }
    auto document=sla::LayerDocument::from_fresh_rasters(std::move(masks),intent);
    auto config=fresh_cx_profile(false);
    config.set("display_pixels_x",int(width));config.set("display_pixels_y",int(height));
    config.set("display_width",intent.geometry.width_mm);config.set("display_height",intent.geometry.height_mm);
    auto job=SLAPrint::create_raster_job(config,document,positions);
    const auto plan_start=std::chrono::steady_clock::now();const auto plan=sla::cxdlpv4_fresh_export_plan(document);
    const double planning=std::chrono::duration<double>(std::chrono::steady_clock::now()-plan_start).count();
    const auto begin=std::chrono::steady_clock::now();
    job->export_layer_document(document,"cxdlpv4",(folder/"output.cxdlpv4").string());
    const double total=std::chrono::duration<double>(std::chrono::steady_clock::now()-begin).count();
    const auto saved=sla::LayerDocument::load_archive((folder/"output.cxdlpv4").string());
    REQUIRE(saved.layer_count()==count);
    for(size_t layer=0;layer<count;++layer) CHECK(saved.image(layer).buf==document.image(layer).buf);
    std::ofstream(folder/"report.json") << nlohmann::json{{"case",mode},{"width",width},{"height",height},
        {"layers",count},{"plan_seconds",planning},{"export_seconds",total},{"material_ml",plan.material_ml}}.dump(2) << '\n';
}

namespace {
void fresh_cx_check_cached(const sla::LayerDocument &document, size_t layer, size_t lit)
{
    const char *flag = std::getenv("SLA_EXPERIMENT_VALIDATED_MASK_STATS");
#if defined(__SSE2__)
    const bool expected = flag ? std::string(flag) == "1" : true;
#else
    const bool expected = flag && std::string(flag) == "1";
#endif
    const auto count = document.validated_mask_statistics(layer);
    REQUIRE(bool(count) == expected);
    if (count) CHECK(count->nonzero_pixels == lit);
}
}

TEST_CASE("CXDLPv4 counts survive snapshots metadata edits history and restart", "[sla_cxdlpv4_plan_cache]") {
    auto intent = fresh_cx_intent(false, sla::FreshLayerProducer::Raster);
    intent.mask_precision = sla::LayerMaskPrecision::NativeGray7;
    std::vector<uint8_t> pixels(32, 0); pixels[1] = 255; pixels[20] = 127;
    const auto png = sla::PNGRasterEncoder{}(pixels.data(), 8, 4, 1);
    auto document = sla::LayerDocument::from_fresh_rasters({png,png,png}, intent);
    fresh_cx_check_cached(document, 0, 2);
    auto snapshot = document.snapshot();
    const auto check_volume = [](const sla::LayerDocument &value, double ml) {
        CHECK(sla::cxdlpv4_fresh_export_plan(value).material_ml == Catch::Approx(ml).margin(1e-9));
    };
    check_volume(document, .0006);
    document.paint(1, 0, 0, 0, 255);
    CHECK_FALSE(document.validated_mask_statistics(1));
    fresh_cx_check_cached(document, 0, 2);
    check_volume(document, .0007); check_volume(snapshot, .0006);
    REQUIRE(document.undo()); check_volume(document, .0006);
    CHECK_FALSE(document.validated_mask_statistics(1));
    REQUIRE(document.redo()); check_volume(document, .0007);
    CHECK_FALSE(document.validated_mask_statistics(1));
    snapshot.set_layer_parameters(1, {{F::Exposure, 2.}});
    fresh_cx_check_cached(snapshot, 1, 2); check_volume(snapshot, .0006);
    CHECK(sla::cxdlpv4_fresh_export_plan(snapshot).rows[1].at(F::Exposure) == 2.);
    FreshCXDirectory directory; boost::filesystem::create_directories(directory.path);
    const auto file = directory.path/"cache.sla-project";
    sla::save_layer_project(file.string(), {document,{.05,.1,.15},"",{},sla::ProjectLayerOrigin::GeneratedRaster});
    const auto restored = sla::read_project_layers(file.string()); REQUIRE(restored);
    check_volume(restored->document, .0007);
    for (size_t i=0;i<3;++i) {
        CHECK_FALSE(restored->document.validated_mask_statistics(i));
        CHECK(restored->document.image(i).buf == document.image(i).buf);
    }
}

TEST_CASE("CXDLPv4 cannot borrow palette validation from cached Gray4 masks", "[sla_cxdlpv4_plan_cache]") {
    auto intent = fresh_cx_intent(false, sla::FreshLayerProducer::Raster);
    intent.mask_precision = sla::LayerMaskPrecision::NativeGray4;
    std::vector<uint8_t> pixels(32, 255); pixels[17] = 34;
    const auto png = sla::PNGRasterEncoder{}(pixels.data(), 8, 4, 1);
    const auto document = sla::LayerDocument::from_fresh_rasters({png,png,png}, intent);
    fresh_cx_check_cached(document, 0, 32);
    CHECK_THROWS_WITH(sla::cxdlpv4_fresh_export_plan(document),
        "CXDLPv4 cannot preserve grayscale pixels outside 0/odd 3..255");
    CHECK(document.image(1).buf == pixels);
}

TEST_CASE("CXDLPv4 cached counts preserve physical unions at repeated Z", "[sla_cxdlpv4_plan_cache]") {
    const bool overlap = GENERATE(false,true);
    auto intent = fresh_cx_intent(false, sla::FreshLayerProducer::Raster);
    intent.mask_precision = sla::LayerMaskPrecision::NativeGray7;
    intent.position_policy = sla::ExposurePositionPolicy::RepeatedExposureHeights;
    intent.rows[0][F::PositionZ]=.1; intent.rows[1][F::PositionZ]=.1; intent.rows[2][F::PositionZ]=.2;
    std::vector<sla::EncodedRaster> masks;
    for (size_t i=0;i<3;++i) {
        std::vector<uint8_t> pixels(32,0);
        const size_t start = i==1 && !overlap ? 4 : 0;
        pixels[start]=255; pixels[start+1]=127;
        masks.push_back(sla::PNGRasterEncoder{}(pixels.data(),8,4,1));
    }
    const auto document=sla::LayerDocument::from_fresh_rasters(std::move(masks),intent);
    for (size_t i=0;i<3;++i) fresh_cx_check_cached(document,i,2);
    CHECK(sla::cxdlpv4_fresh_export_plan(document).material_ml ==
        Catch::Approx(overlap ? .0008 : .0012).margin(1e-9));
}

TEST_CASE("CXDLPv4 cached plan remains cancellable on its calling thread", "[sla_cxdlpv4_plan_cache]") {
    auto intent=fresh_cx_intent(false,sla::FreshLayerProducer::Raster);
    intent.mask_precision=sla::LayerMaskPrecision::NativeGray7;
    std::vector<uint8_t> pixels(32,255);
    const auto png=sla::PNGRasterEncoder{}(pixels.data(),8,4,1);
    const auto document=sla::LayerDocument::from_fresh_rasters({png,png,png},intent);
    fresh_cx_check_cached(document,0,32);
    const auto owner=std::this_thread::get_id(); size_t calls=0;
    sla::cxdlpv4_fresh_export_plan(document,[&](size_t,size_t){
        CHECK(std::this_thread::get_id()==owner); ++calls; return true;
    });
    REQUIRE(calls>3);
    for (size_t stop : {size_t(1),calls/2,calls}) {
        size_t seen=0;
        CHECK_THROWS_AS(sla::cxdlpv4_fresh_export_plan(document,[&](size_t,size_t){
            CHECK(std::this_thread::get_id()==owner); return ++seen<stop;
        }),sla::LayerOperationCancelled);
    }
    CHECK(document.revision()==0); CHECK(document.image(0).buf==pixels);
}
