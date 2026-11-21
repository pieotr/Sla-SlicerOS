// SPDX-License-Identifier: AGPL-3.0-or-later
#include "libslic3r/SLAPrint.hpp"
#include "libslic3r/SLA/ProjectArchive.hpp"
#include "libslic3r/SLA/RasterBase.hpp"
#include "libslic3r/SLA/ExposureMaskUnion.hpp"
#include "libslic3r/SLA/CrealityCXDLPv4FreshExportPlan.hpp"
#include "libslic3r/Format/CrealityCXDLPv4Layout.hpp"
#include "libslic3r/SLA/CrealityCXDLPv4LayerParameters.hpp"
#include "libslic3r/SLA/ExportRequest.hpp"
#include "libslic3r/Zipper.hpp"
#include "libslic3r/Format/ZipperArchiveImport.hpp"
#include <nlohmann/json.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <boost/filesystem.hpp>
#include <cstdlib>
#include <fstream>
using namespace Slic3r;
namespace {
DynamicPrintConfig sequence_config() {
    DynamicPrintConfig c;c.apply(SLAFullPrintConfig{});
    c.option("printer_technology")->setInt(ptSLA);
    c.set("sla_archive_format","uvj");c.set("display_pixels_x",8);c.set("display_pixels_y",4);
    c.set("display_width",8.);c.set("display_height",4.);
    c.option("display_orientation")->setInt(sladoLandscape);
    c.set("layer_height",.05);c.set("initial_layer_height",.05);c.set("max_print_height",10.);
    c.set("sla_bottom_layers",0);c.set("sla_transition_layers",0);c.set("exposure_time",10.);
    for(const char *prefix:{"sla_","sla_bottom_"})
        for(const char *key:{"lift_height","lift_height2","retract_height2","wait_before_cure","wait_after_cure","wait_after_lift"})
            c.set(std::string(prefix)+key,0.);
    return c;
}
sla::LayerDocument sequence_masks(size_t columns=8,size_t rows=4) {
    std::vector<sla::EncodedRaster> masks;
    for(const auto &lit:std::vector<std::vector<size_t>>{{0},{1},{0,1},{1,2}}) {
        std::vector<uint8_t> pixels(columns*rows,0);for(auto i:lit)pixels[i]=255;
        masks.push_back(sla::PNGRasterEncoder{}(pixels.data(),columns,rows,1));
    }
    return sla::LayerDocument::from_rasters(std::move(masks));
}
}
TEST_CASE("Repeated exposure production counts union volume and retains all records", "[sla_exposure_sequence]") {
    auto config=sequence_config();config.set("printer_model","GENERIC");auto masks=sequence_masks();
    // Isolate destination schedule support from invalid printer metadata.
    const std::vector<double> z{.05,.05,.1,.1};
    CHECK_THROWS_AS(SLAPrint::create_raster_job(config,masks,z,sla::FreshLayerProducer::Raster),RuntimeError);
    auto job=SLAPrint::create_raster_job(config,masks,z,sla::FreshLayerProducer::Raster,sla::ExposurePositionPolicy::RepeatedExposureHeights);
    REQUIRE(job->layer_document()->fresh_job());
    CHECK(job->layer_document()->fresh_job()->position_policy==sla::ExposurePositionPolicy::RepeatedExposureHeights);
    CHECK(job->print_statistics().objects_used_material==Catch::Approx(.25));
    CHECK(job->print_statistics().estimated_print_time==Catch::Approx(40.));
    auto edited=*job->layer_document();edited.paint(1,3,0,0,255);job->replace_layer_document(edited);
    CHECK(job->print_statistics().objects_used_material==Catch::Approx(.3));
    REQUIRE(edited.undo());job->replace_layer_document(edited);
    CHECK(job->print_statistics().objects_used_material==Catch::Approx(.25));
    const char *fixtures=std::getenv("SLA_EXPOSURE_SEQUENCE_FIXTURES");
    const auto folder=fixtures?boost::filesystem::path(fixtures):boost::filesystem::temp_directory_path()/boost::filesystem::unique_path("sequence-%%%%");
    boost::filesystem::create_directories(folder);
    const auto project=folder/"sequence.sla-project";
    sla::save_layer_project(project.string(),sla::capture_generated_project(*job,*job->layer_document()));
    const auto saved=sla::read_project_layers(project.string());REQUIRE(saved);
    CHECK(saved->document.fresh_job()->position_policy==sla::ExposurePositionPolicy::RepeatedExposureHeights);
    auto restored=sla::restore_generated_project(*saved);
    CHECK(restored->print_statistics().objects_used_material==Catch::Approx(.25));
    for(size_t i=0;i<4;++i)CHECK(restored->layer_document()->image(i).buf==masks.image(i).buf);
    REQUIRE_NOTHROW(restored->export_print((folder/"sequence.uvj").string(),"sequence"));
    for(const char *format:{"SL1","ctb_v4","pwc"}) {
        const auto path=folder/(std::string("refused.")+format);
        {std::ofstream out(path.string());out<<"keep";}
        CHECK_THROWS_AS(restored->export_layer_document(*restored->layer_document(),format,path.string(),{},"sequence"),RuntimeError);
        CHECK_THROWS_WITH(restored->export_layer_document(*restored->layer_document(),format,path.string(),{},"sequence"),
            Catch::Matchers::StartsWith(std::string("Destination ")+format+" cannot store repeated-height exposure sequences") &&
            Catch::Matchers::ContainsSubstring("uvj or cxdlpv4"));
        std::ifstream in(path.string());std::string contents;in>>contents;CHECK(contents=="keep");
    }
    if(!fixtures)boost::filesystem::remove_all(folder);
}

TEST_CASE("Exposure union is idempotent and bounded", "[sla_exposure_sequence]") {
    sla::ExposureMaskUnion occupied(4);
    occupied.append({1,0,127,0});CHECK(occupied.nonzero_pixels()==2);
    occupied.append({255,0,255,0});CHECK(occupied.nonzero_pixels()==2);
    occupied.append({0,255,0,0});CHECK(occupied.nonzero_pixels()==3);
    CHECK_THROWS_AS(occupied.append({255}),RuntimeError);CHECK(occupied.nonzero_pixels()==3);
    occupied.reset();CHECK(occupied.nonzero_pixels()==0);
    occupied.append({0,0,0,0});CHECK(occupied.nonzero_pixels()==0);
    CHECK_THROWS_AS(sla::ExposureMaskUnion(0),RuntimeError);
    CHECK_THROWS_AS(sla::ExposureMaskUnion((size_t(128)<<20)+1),RuntimeError);
}

TEST_CASE("Owned parameter tables apply atomically as one history edit", "[sla_owned_parameter_table]") {
    auto job=SLAPrint::create_raster_job(sequence_config(),sequence_masks(),{.05,.05,.1,.1},
        sla::FreshLayerProducer::Raster,sla::ExposurePositionPolicy::RepeatedExposureHeights);
    auto doc=*job->layer_document();using F=sla::LayerParameter;
    const auto original=doc.fresh_job()->rows;
    auto table=original;table[1][F::Exposure]=3.25;table[2][F::WaitBefore]=.5;
    const auto revision=doc.revision();
    REQUIRE_NOTHROW(doc.replace_layer_parameter_table(table));
    CHECK(doc.fresh_job()->rows==table);CHECK(doc.revision()!=revision);
    auto invalid=table;invalid[0][F::Exposure]=99.;invalid[3][F::LiftDistance]=1.;invalid[3][F::LiftSpeed]=0.;
    const auto edited=doc.revision();
    CHECK_THROWS_AS(doc.replace_layer_parameter_table(invalid),RuntimeError);
    CHECK(doc.revision()==edited);CHECK(doc.fresh_job()->rows==table);
    REQUIRE(doc.undo());CHECK(doc.fresh_job()->rows==original);CHECK(doc.revision()==revision);
    REQUIRE(doc.redo());CHECK(doc.fresh_job()->rows==table);
    doc.replace_layer_parameter_table(table);CHECK(doc.revision()==edited);
    invalid=table;invalid[3].erase(F::WaitBefore);CHECK_THROWS_AS(doc.replace_layer_parameter_table(invalid),RuntimeError);
    invalid=table;invalid[3][F::PositionZ]=.15;CHECK_THROWS_AS(doc.replace_layer_parameter_table(invalid),RuntimeError);
    CHECK(doc.fresh_job()->rows==table);
}

TEST_CASE("Owned exposure parameters are accessible and edit with transactional history", "[sla_owned_parameters]") {
    auto job=SLAPrint::create_raster_job(sequence_config(),sequence_masks(),{.05,.05,.1,.1},
        sla::FreshLayerProducer::Raster,sla::ExposurePositionPolicy::RepeatedExposureHeights);
    auto doc=*job->layer_document();using F=sla::LayerParameter;
    REQUIRE(doc.parameter_capabilities().size()==14);
    CHECK(doc.layer_parameters(1)==doc.fresh_job()->rows[1]);
    const auto original=doc.image(1).buf;const auto before=doc.revision();
    doc.set_layer_parameters(1,{{F::Exposure,3.25},{F::WaitBefore,.5}});
    CHECK(doc.layer_parameters(1).at(F::Exposure)==3.25);
    CHECK(doc.fresh_job()->rows[1].at(F::WaitBefore)==.5);
    CHECK(doc.revision()!=before);CHECK(doc.image(1).buf==original);
    job->replace_layer_document(doc);CHECK(job->print_statistics().estimated_print_time==Catch::Approx(33.75));
    const auto changed=doc.revision();
    CHECK_THROWS_AS(doc.set_layer_parameters(1,{{F::LiftDistance,2.},{F::LiftSpeed,0.}}),RuntimeError);
    CHECK(doc.revision()==changed);CHECK(doc.layer_parameters(1).at(F::Exposure)==3.25);
    REQUIRE(doc.undo());CHECK(doc.revision()==before);CHECK(doc.layer_parameters(1).at(F::Exposure)==10.);
    REQUIRE(doc.redo());CHECK(doc.revision()==changed);CHECK(doc.layer_parameters(1).at(F::Exposure)==3.25);
    doc.set_layer_parameters(1,{{F::Exposure,3.25}});CHECK(doc.revision()==changed);
    CHECK_THROWS_AS(doc.set_layer_parameters(1,{{F::PositionZ,.075}}),RuntimeError);
    CHECK(doc.revision()==changed);
}

TEST_CASE("Repeated exposure ownership and actual physical thickness are explicit", "[sla_exposure_sequence]") {
    auto c=sequence_config();const auto masks=sequence_masks();
    const auto repeat=sla::ExposurePositionPolicy::RepeatedExposureHeights;
    const std::vector<double> z{.075,.075,.175,.175};
    CHECK_THROWS_AS(SLAPrint::create_raster_job(c,masks,z,std::nullopt,repeat),RuntimeError);
    CHECK_THROWS_AS(SLAPrint::create_raster_job(c,masks,z,sla::FreshLayerProducer::ModelSlice,repeat),RuntimeError);
    CHECK_THROWS_AS(SLAPrint::create_raster_job(c,masks,z,sla::FreshLayerProducer::Raster,sla::ExposurePositionPolicy(2)),RuntimeError);
    CHECK_THROWS_AS(SLAPrint::create_raster_job(c,masks,{.05,.050000001,.1,.1},sla::FreshLayerProducer::Raster,repeat),RuntimeError);
    auto job=SLAPrint::create_raster_job(c,masks,z,sla::FreshLayerProducer::Calibration,repeat);
    CHECK(job->print_statistics().objects_used_material==Catch::Approx(.45));
    CHECK(job->print_statistics().estimated_print_time==Catch::Approx(40.));
}

TEST_CASE("Manual model parameters survive redo and rebase onto refreshed automatic defaults", "[sla_owned_parameters]") {
    auto job=SLAPrint::create_raster_job(sequence_config(),sequence_masks(),{.05,.1,.15,.2},sla::FreshLayerProducer::Raster);
    auto intent=*job->layer_document()->fresh_job();using F=sla::LayerParameter;
    intent.producer=sla::FreshLayerProducer::ModelSlice;
    intent.parameter_origin=sla::FreshLayerParameterOrigin::AutomaticModel;
    std::vector<sla::EncodedRaster> masks;
    for(size_t i=0;i<4;++i) {
        const auto bytes=job->layer_document()->encoded_layer(i);const auto *begin=static_cast<const uint8_t *>(bytes.buf);
        masks.emplace_back(std::vector<uint8_t>(begin,begin+bytes.sz),"png");
    }
    auto doc=sla::LayerDocument::from_fresh_rasters(std::move(masks),intent);
    doc.set_layer_parameters(1,{{F::Exposure,20.}});
    CHECK(doc.fresh_job()->parameter_origin==sla::FreshLayerParameterOrigin::Explicit);
    CHECK_THROWS_AS(doc.refresh_automatic_model_parameters(intent),RuntimeError);
    doc.set_layer_parameters(1,{{F::WaitBefore,.5}});
    REQUIRE(doc.undo());REQUIRE(doc.undo());
    CHECK(doc.fresh_job()->parameter_origin==sla::FreshLayerParameterOrigin::AutomaticModel);
    intent.globals.normal[F::Exposure]=17.;intent.globals.normal[F::WaitBefore]=.25;
    for(auto &row:intent.rows){row[F::Exposure]=17.;row[F::WaitBefore]=.25;}
    doc.refresh_automatic_model_parameters(intent);
    REQUIRE(doc.redo());CHECK(doc.layer_parameters(1).at(F::Exposure)==20.);
    CHECK(doc.layer_parameters(1).at(F::WaitBefore)==.25);
    REQUIRE(doc.redo());CHECK(doc.layer_parameters(1).at(F::WaitBefore)==.5);
    REQUIRE(doc.undo());CHECK(doc.layer_parameters(1).at(F::WaitBefore)==.25);
    REQUIRE(doc.undo());CHECK(doc.layer_parameters(1).at(F::Exposure)==17.);
    CHECK(doc.fresh_job()->parameter_origin==sla::FreshLayerParameterOrigin::AutomaticModel);
}

TEST_CASE("Exposure sequence projects reject downgraded and malformed ownership", "[sla_exposure_sequence]") {
    auto job=SLAPrint::create_raster_job(sequence_config(),sequence_masks(),{.05,.05,.1,.1},
        sla::FreshLayerProducer::Raster,sla::ExposurePositionPolicy::RepeatedExposureHeights);
    const auto folder=boost::filesystem::temp_directory_path()/boost::filesystem::unique_path("sequence-schema-%%%%");
    boost::filesystem::create_directories(folder);const auto good=folder/"good.sla-project";
    sla::save_layer_project(good.string(),sla::capture_generated_project(*job,*job->layer_document()));
    const auto original=read_zipper_archive(good.string(),{"metadata/sla/"},{});
    using Json=nlohmann::json;
    const std::vector<std::function<void(Json &)>> mutations{
        [](auto &j){j["version"]=5;}, [](auto &j){j["version"]=8;},
        [](auto &j){j["document"]["fresh_job"].erase("position_policy");},
        [](auto &j){j["document"]["fresh_job"]["position_policy"]=0;},
        [](auto &j){j["document"]["fresh_job"]["position_policy"]=2;},
        [](auto &j){j["document"]["fresh_job"]["position_policy"]=-1;},
        [](auto &j){j["document"]["fresh_job"]["position_policy"]=1.0;},
        [](auto &j){j["document"]["fresh_job"]["producer"]=1;},
        [](auto &j){j["document"]["fresh_job"]["producer"]=3;},
        [](auto &j){j["document"]["fresh_job"]["parameter_origin"]=1;},
        [](auto &j){j["document"]["fresh_job"]["version"]=3;},
        [](auto &j){j["positions_z"][1]=.051;}
    };
    for(const auto &mutate:mutations) {
        auto entries=original.entries;
        auto found=std::find_if(entries.begin(),entries.end(),[](const auto &e){return e.fname=="metadata/sla/layers.json";});
        REQUIRE(found!=entries.end());auto meta=Json::parse(found->buf);mutate(meta);
        const auto text=meta.dump();found->buf.assign(text.begin(),text.end());
        const auto bad=folder/"bad.sla-project";
        {Zipper zip(bad.string());for(const auto &entry:entries)zip.add_entry(entry.fname,entry.buf.data(),entry.buf.size());zip.finalize();}
        CHECK_THROWS(sla::read_project_layers(bad.string()));
        REQUIRE(sla::read_project_layers(good.string()));
    }
    boost::filesystem::remove_all(folder);
}

TEST_CASE("CXDLPv4 preserves repeated exposure heights and physical union volume", "[sla_cxdlpv4_sequence]") {
    const bool portrait=GENERATE(false,true), restart=GENERATE(false,true), bottom=GENERATE(false,true);
    const auto producer=GENERATE(sla::FreshLayerProducer::Raster,sla::FreshLayerProducer::Calibration);
    auto config=sequence_config();config.set("printer_model","CL-60");config.set("sla_archive_format","cxdlpv4");
    config.set("sla_bottom_retract_speed",0.);config.set("sla_bottom_retract_speed2",0.);
    if(portrait) config.option("display_orientation")->setInt(sladoPortrait);
    auto masks=sequence_masks(portrait?4:8,portrait?8:4);
    if(bottom) {
        config.set("sla_bottom_layers",2);
        config.set("sla_bottom_retract_speed",100.);config.set("sla_bottom_retract_speed2",80.);
        masks.paint(0,3,0,0,255); // Meaningful bottom row, not reference dummy mask.
    }
    auto job=SLAPrint::create_raster_job(config,masks,{.05,.05,.1,.1},
        producer,sla::ExposurePositionPolicy::RepeatedExposureHeights);
    using F=sla::LayerParameter;
    auto edited=*job->layer_document();
    edited.set_layer_parameters(3,{{F::Exposure,12.25},{F::WaitBefore,.25}});
    job->replace_layer_document(edited);
    const auto original_rows=job->layer_document()->fresh_job()->rows;
    const char *fixtures=std::getenv("SLA_CXDLPV4_SEQUENCE_FIXTURES");
    auto folder=fixtures?boost::filesystem::path(fixtures):boost::filesystem::temp_directory_path()/boost::filesystem::unique_path("cx-sequence-%%%%");
    if(fixtures)folder/=std::string(portrait?"portrait":"landscape")+(restart?"-restart":"-direct")+(bottom?"-bottom":"-normal")+std::to_string(int(producer));
    boost::filesystem::create_directories(folder);
    const auto project=folder/"sequence.sla-project",output=folder/"sequence.cxdlpv4";
    sla::save_layer_project(project.string(),sla::capture_generated_project(*job,*job->layer_document()));
    if(restart) {
        const auto restored=sla::read_project_layers(project.string());REQUIRE(restored);
        job=sla::restore_generated_project(*restored);
        REQUIRE_NOTHROW(job->export_print(output.string(),"sequence"));
    } else REQUIRE_NOTHROW(job->export_layer_document(*job->layer_document(),"cxdlpv4",output.string(),{},"sequence"));
    const auto plan=sla::cxdlpv4_fresh_export_plan(*job->layer_document());
    CHECK(plan.material_ml==Catch::Approx(bottom?.0003:.00025));
    std::ifstream input(output.string(),std::ios::binary);
    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)),{});
    const sla::CrealityCXDLPv4Layout layout(bytes);
    CHECK(layout.number(layout.integer(layout.base+52)+20)==Catch::Approx(bottom?.0003:.00025));
    auto reopened=sla::LayerDocument::load_archive(output.string());
    REQUIRE(reopened.layer_count()==4);
    CHECK(job->layer_document()->fresh_job()->rows==original_rows);
    for(size_t i=0;i<4;++i) {
        CHECK(reopened.image(i).buf==job->layer_document()->image(i).buf);
        CHECK(sla::cxdlpv4_layer_parameters(layout,i).at(F::PositionZ)==Catch::Approx(i<2?.05:.1));
        CHECK(sla::cxdlpv4_layer_parameters(layout,i).at(F::Exposure)==Catch::Approx(original_rows[i].at(F::Exposure)));
    }
    if(!fixtures)boost::filesystem::remove_all(folder);
}

TEST_CASE("CXDLPv4 repeated positions preserve precision groups and cancellation", "[sla_cxdlpv4_sequence]") {
    auto config=sequence_config();config.set("printer_model","CL-60");
    config.set("sla_bottom_retract_speed",0.);config.set("sla_bottom_retract_speed2",0.);
    auto job=SLAPrint::create_raster_job(config,sequence_masks(),{.05,.05,.1,.1},
        sla::FreshLayerProducer::Raster,sla::ExposurePositionPolicy::RepeatedExposureHeights);
    const auto original=*job->layer_document();
    auto intent=*original.fresh_job();using F=sla::LayerParameter;
    intent.rows[2][F::PositionZ]=.0501;intent.rows[3][F::PositionZ]=.0501;
    const auto collapsed=sla::LayerDocument::from_fresh_rasters(
        [&] { std::vector<sla::EncodedRaster> masks;
            for(size_t i=0;i<4;++i){const auto mask=original.image(i);masks.push_back(sla::PNGRasterEncoder{}(mask.buf.data(),mask.cols,mask.rows,1));}
            return masks; }(),intent);
    CHECK_THROWS_AS(sla::cxdlpv4_fresh_export_plan(collapsed),RuntimeError);
    bool completed=false;
    CHECK_THROWS_AS(sla::cxdlpv4_fresh_export_plan(original,[&](size_t done,size_t total){
        if(done==total)completed=true;
        return !completed;
    }),sla::LayerOperationCancelled);
    CHECK(completed);
    CHECK(original.fresh_job()->rows==job->layer_document()->fresh_job()->rows);
}

TEST_CASE("CXDLPv4 exposure union crosses batches and cancelled export preserves target", "[sla_cxdlpv4_sequence]") {
    auto config=sequence_config();config.set("printer_model","CL-60");
    config.set("sla_bottom_retract_speed",0.);config.set("sla_bottom_retract_speed2",0.);
    std::vector<sla::EncodedRaster> masks;std::vector<double> positions;
    for(size_t i=0;i<17;++i) {
        std::vector<uint8_t> pixels(32,0);pixels[i%5]=127;
        masks.push_back(sla::PNGRasterEncoder{}(pixels.data(),8,4,1));
        positions.push_back(i<10?.05:(i<16?.1:.15));
    }
    auto job=SLAPrint::create_raster_job(config,sla::LayerDocument::from_rasters(std::move(masks)),positions,
        sla::FreshLayerProducer::Raster,sla::ExposurePositionPolicy::RepeatedExposureHeights);
    const auto before=*job->layer_document();
    CHECK(sla::cxdlpv4_fresh_export_plan(before).material_ml==Catch::Approx(.00055));
    const auto folder=boost::filesystem::temp_directory_path()/boost::filesystem::unique_path("cx-sequence-cancel-%%%%");
    boost::filesystem::create_directories(folder);const auto output=folder/"keep.cxdlpv4";
    {std::ofstream out(output.string());out<<"keep";}
    auto snapshot=std::make_shared<const sla::ExportSnapshot>(job->capture_export_snapshot());
    const sla::ExportRequest request(snapshot,"cxdlpv4",output.string());
    bool cancelled=false;
    CHECK_THROWS_AS(request.write([&] {
        for(const auto &entry:boost::filesystem::directory_iterator(folder))
            if(entry.path().filename().string().find(".sla-export-")==0 && boost::filesystem::file_size(entry.path())>0) {
                request.cancel();cancelled=true;break;
            }
    }),CanceledException);
    CHECK(cancelled);
    {std::ifstream in(output.string());std::string value;in>>value;CHECK(value=="keep");}
    CHECK(std::distance(boost::filesystem::directory_iterator(folder),boost::filesystem::directory_iterator{})==1);
    CHECK(job->layer_document()->fresh_job()->rows==before.fresh_job()->rows);
    REQUIRE_NOTHROW(job->export_layer_document(before,"cxdlpv4",output.string()));
    CHECK(sla::LayerDocument::load_archive(output.string()).layer_count()==17);
    boost::filesystem::remove_all(folder);
}
