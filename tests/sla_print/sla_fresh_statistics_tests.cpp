#include "libslic3r/SLA/ExportPrinterValidation.hpp"
// SPDX-License-Identifier: AGPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <limits>
#include <boost/filesystem.hpp>
#include "libslic3r/SLA/ProjectArchive.hpp"
#include "libslic3r/SLAPrint.hpp"
#include "libslic3r/SLA/LayerDocument.hpp"
#include "libslic3r/SLA/LayerMaskValidation.hpp"
#include "libslic3r/SLA/GeneratedLayerOwnership.hpp"
#include "libslic3r/SLA/CWSCommandCodec.hpp"
#include "libslic3r/SLA/CWSFreshExportPlan.hpp"
#include "libslic3r/SLA/CBDDLPFreshExportPlan.hpp"
#include "libslic3r/Format/ChituboxLayerLayout.hpp"
#include "libslic3r/Format/AnycubicLayerLayout.hpp"
#include "libslic3r/SLA/AnycubicFreshExportPlan.hpp"
#include "libslic3r/SLA/LayerPaletteProjection.hpp"
#include "libslic3r/SLA/SL1FreshExportPlan.hpp"
#include "libslic3r/Format/ZipperArchiveImport.hpp"
#include <fstream>
using namespace Slic3r;
namespace {
using F=sla::LayerParameter;
DynamicPrintConfig statistics_profile(bool portrait=false) {
    DynamicPrintConfig config;config.apply(SLAFullPrintConfig{});
    config.option("printer_technology")->setInt(ptSLA);
    config.set("sla_archive_format","uvj");
    config.set("display_pixels_x",8);config.set("display_pixels_y",4);
    config.set("display_width",8.);config.set("display_height",4.);
    config.option("display_orientation")->setInt(portrait?sladoPortrait:sladoLandscape);
    config.set("max_print_height",10.);
    config.set("layer_height",.1);config.set("initial_layer_height",.1);
    config.set("exposure_time",10.);config.set("initial_exposure_time",20.);
    config.set("sla_bottom_layers",2);config.set("sla_transition_layers",0);
    for(const char *prefix:{"sla_","sla_bottom_"})
        for(const char *key:{"lift_height","lift_height2","retract_height2","wait_before_cure","wait_after_cure","wait_after_lift"})
            config.set(std::string(prefix)+key,0.);
    config.set("material_density",1.5);config.set("bottle_cost",100.);config.set("bottle_volume",500.);
    return config;
}
sla::FreshLayerJob statistics_intent() {
    sla::FreshLayerJob job;job.producer=sla::FreshLayerProducer::Raster;
    job.geometry={8,4,16.,4.};
    job.globals.layer_height=.05;job.globals.bottom_layers=1;
    job.globals.normal={{F::Exposure,3.},{F::LiftDistance,1.},{F::LiftSpeed,60.},
        {F::LiftDistance2,2.},{F::LiftSpeed2,120.},{F::RetractDistance2,.5},
        {F::RetractSpeed2,120.},{F::RetractSpeed,150.},{F::WaitBefore,.25},
        {F::WaitAfter,.5},{F::WaitAfterLift,.75},{F::LightOff,0.},{F::LightPWM,255.}};
    job.globals.bottom=job.globals.normal;job.globals.bottom[F::Exposure]=2.;job.globals.bottom[F::LightOff]=5.;
    for(size_t i=0;i<3;++i) {
        auto row=i?job.globals.normal:job.globals.bottom;
        row[F::Exposure]=double(i+2);row[F::PositionZ]=std::vector<double>{.075,.125,.175}[i];
        job.rows.push_back(row);
    }
    return job;
}
std::vector<sla::EncodedRaster> statistics_masks(bool portrait=false) {
    std::vector<sla::EncodedRaster> layers;
    for(size_t lit:{2,3,1}) {
        std::vector<uint8_t> pixels(32,0);
        for(size_t i=0;i<lit;++i)pixels[1+3*i]=i==0?1:127;
        layers.push_back(sla::PNGRasterEncoder{}(pixels.data(),portrait?4:8,portrait?8:4,1));
    }
    return layers;
}
void check_owned_statistics(const SLAPrintStatistics &s,double volume=.7) {
    CHECK(s.time_basis==SLAPrintStatistics::TimeBasis::OwnedNeutralSchedule);
    CHECK(s.raster_area_mm2==64.);
    CHECK(s.objects_used_material==Catch::Approx(volume).epsilon(0).margin(1e-12));
    CHECK(s.support_used_material==0.);
    CHECK(s.estimated_print_time==Catch::Approx(25.).epsilon(0).margin(1e-12));
    CHECK(s.slow_layers_count==1);CHECK(s.fast_layers_count==2);
    REQUIRE(s.layers_areas.size()==3);REQUIRE(s.layers_times_running_total.size()==3);
    CHECK(s.layers_areas[0]==4.);CHECK(s.layers_areas[1]==6.);CHECK(s.layers_areas[2]==2.);
    CHECK(s.layers_times_running_total[0]==8.5);
    CHECK(s.layers_times_running_total[1]==16.25);
    CHECK(s.layers_times_running_total[2]==25.);
    CHECK(s.total_weight==Catch::Approx(volume/1000.*1.5).epsilon(0).margin(1e-14));
    CHECK(s.total_cost==Catch::Approx(volume/1000.*100./500.).epsilon(0).margin(1e-14));
}
}
TEST_CASE("Fresh raster statistics use owned geometry rows and exact actual Z", "[sla_fresh_statistics]") {
    auto intent=statistics_intent();const auto config=statistics_profile();
    const auto document=sla::LayerDocument::from_fresh_rasters(statistics_masks(),intent);
    const auto job=SLAPrint::create_raster_job(config,document,{.075,.125,.175});
    check_owned_statistics(job->print_statistics());
    CHECK(document.revision()==0);
    CHECK(job->full_print_config().opt_float("exposure_time")==10.);
    CHECK(job->full_print_config().opt_int("sla_bottom_layers")==2);
    CHECK(job->full_print_config().opt_float("display_width")==8.);
    REQUIRE(job->layer_document()->fresh_job());
    CHECK(job->layer_document()->fresh_job()->rows==intent.rows);
    CHECK(document.fresh_job()->globals.normal==intent.globals.normal);
    SECTION("Sub-grid source Z survives neutral material accounting") {
        for(auto &row:intent.rows)row[F::PositionZ]+=.0000004;
        const auto precise=sla::LayerDocument::from_fresh_rasters(statistics_masks(),intent);
        const auto exact=SLAPrint::create_raster_job(config,precise,{.0750004,.1250004,.1750004});
        check_owned_statistics(exact->print_statistics(),.7000016);
    }
    SECTION("Current pixels replace stats and undo restores source") {
        auto edited=document;edited.paint(1,0,0,0,255);
        job->replace_layer_document(edited);
        CHECK(job->print_statistics().objects_used_material==Catch::Approx(.8).epsilon(0).margin(1e-12));
        CHECK(job->print_statistics().layers_areas[1]==8.);
        CHECK(job->print_statistics().estimated_print_time==25.);
        CHECK(job->layer_document()->fresh_job()->rows==intent.rows);
        edited.undo();job->replace_layer_document(edited);check_owned_statistics(job->print_statistics());
        edited.redo();job->replace_layer_document(edited);
        CHECK(job->print_statistics().objects_used_material==Catch::Approx(.8).epsilon(0).margin(1e-12));
        CHECK(document.image(1).get(0,0)==0);
    }
}
TEST_CASE("Fresh statistics reject invalid material rather than publishing invalid metrics", "[sla_fresh_statistics]") {
    const int defect=GENERATE(0,1,2,3,4,5);auto config=statistics_profile();
    if(defect==0)config.set("material_density",-1.);
    if(defect==1)config.set("bottle_cost",-1.);
    if(defect==2)config.set("bottle_volume",-1.);
    if(defect==3)config.set("material_density",std::numeric_limits<double>::infinity());
    if(defect==4)config.set("bottle_cost",std::numeric_limits<double>::quiet_NaN());
    if(defect==5)config.set("bottle_volume",std::numeric_limits<double>::infinity());
    const auto intent=statistics_intent();
    const auto document=sla::LayerDocument::from_fresh_rasters(statistics_masks(),intent);
    CHECK_THROWS(SLAPrint::create_raster_job(config,document,{.075,.125,.175}));
    CHECK(document.fresh_job()->rows==intent.rows);CHECK(document.revision()==0);
}
TEST_CASE("Plain raster statistics retain legacy process authority", "[sla_fresh_statistics]") {
    const auto document=sla::LayerDocument::from_rasters(statistics_masks());
    const auto job=SLAPrint::create_raster_job(statistics_profile(),document,{.075,.125,.175});
    const auto &s=job->print_statistics();
    CHECK(s.time_basis==SLAPrintStatistics::TimeBasis::LegacyProcess);
    CHECK(s.objects_used_material==Catch::Approx(.35).epsilon(0).margin(1e-12));
    CHECK(s.estimated_print_time==50.);CHECK(s.slow_layers_count==2);CHECK(s.fast_layers_count==1);
    CHECK_FALSE(job->layer_document()->fresh_job());
}

TEST_CASE("Owned statistics survive all producer identities portrait and project restart", "[sla_fresh_statistics]") {
    const bool portrait=GENERATE(false,true);
    const auto producer=GENERATE(sla::FreshLayerProducer::Raster,sla::FreshLayerProducer::ModelSlice,
        sla::FreshLayerProducer::Calibration,sla::FreshLayerProducer::PCB);
    CAPTURE(portrait,int(producer));
    auto intent=statistics_intent();intent.producer=producer;
    if(portrait)intent.geometry={4,8,4.,16.};
    const auto document=sla::LayerDocument::from_fresh_rasters(statistics_masks(portrait),intent);
    const auto config=statistics_profile(portrait);
    const auto before=SLAPrint::create_raster_job(config,document,{.075,.125,.175});
    check_owned_statistics(before->print_statistics());
    struct Files {
        boost::filesystem::path path=boost::filesystem::temp_directory_path()/boost::filesystem::unique_path("owned-statistics-%%%%");
        ~Files(){boost::system::error_code error;boost::filesystem::remove_all(path,error);}
    } files;
    boost::filesystem::create_directories(files.path);
    sla::ProjectRasterState state{document,{.075,.125,.175},"",{},sla::ProjectLayerOrigin::GeneratedRaster};
    if(producer==sla::FreshLayerProducer::ModelSlice) {
        state.origin=sla::ProjectLayerOrigin::ModelSlice;state.source_fingerprint=std::string(40,'e');
    }
    const auto path=(files.path/"statistics.sla-project").string();
    REQUIRE_NOTHROW(sla::save_layer_project(path,state));
    const auto restored=sla::read_project_layers(path);REQUIRE(restored);
    // Project restore deliberately marks the document restored/edited.
    CHECK(restored->document.revision()==1);
    REQUIRE(restored->document.fresh_job());
    CHECK(restored->document.fresh_job()->producer==producer);
    CHECK(restored->document.fresh_job()->rows==intent.rows);
    const auto reopened=SLAPrint::create_raster_job(config,restored->document,restored->positions_z);
    check_owned_statistics(reopened->print_statistics());
    CHECK(reopened->full_print_config().opt_float("exposure_time")==10.);
    CHECK(reopened->full_print_config().opt_int("sla_bottom_layers")==2);
}
TEST_CASE("Overflowing neutral replacement preserves active masks and statistics", "[sla_fresh_statistics]") {
    const auto intent=statistics_intent();
    const auto document=sla::LayerDocument::from_fresh_rasters(statistics_masks(),intent);
    const auto job=SLAPrint::create_raster_job(statistics_profile(),document,{.075,.125,.175});
    const auto before=job->print_statistics();
    auto impossible=intent;
    impossible.rows[0][F::Exposure]=std::numeric_limits<double>::max();
    impossible.rows[1][F::Exposure]=std::numeric_limits<double>::max();
    // Every owned field is finite/valid; the cumulative estimate overflows.
    const auto invalid=sla::LayerDocument::from_fresh_rasters(statistics_masks(),impossible);
    CHECK_THROWS(job->replace_layer_document(invalid));
    CHECK(job->layer_document()->fresh_job()->rows==intent.rows);
    CHECK(job->layer_document()->revision()==document.revision());
    for(size_t i=0;i<3;++i)CHECK(job->layer_document()->image(i).buf==document.image(i).buf);
    CHECK(job->print_statistics().objects_used_material==before.objects_used_material);
    CHECK(job->print_statistics().estimated_print_time==before.estimated_print_time);
    CHECK(job->print_statistics().layers_times_running_total==before.layers_times_running_total);
    check_owned_statistics(job->print_statistics());
}
TEST_CASE("Finite neutral cost operands must not publish overflowing material metrics", "[sla_fresh_statistics]") {
    auto config=statistics_profile();
    config.set("bottle_cost",std::numeric_limits<double>::max());
    config.set("bottle_volume",std::numeric_limits<double>::min());
    const auto intent=statistics_intent();
    const auto document=sla::LayerDocument::from_fresh_rasters(statistics_masks(),intent);
    CHECK_THROWS(SLAPrint::create_raster_job(config,document,{.075,.125,.175}));
    CHECK(document.fresh_job()->rows==intent.rows);CHECK(document.revision()==0);
}
TEST_CASE("Zero volume price and inactive zero speed remain inspectable neutral intents", "[sla_fresh_statistics]") {
    auto config=statistics_profile();config.set("bottle_volume",0.);
    auto intent=statistics_intent();
    for(auto &row:intent.rows)
        for(const auto field:{F::LiftDistance,F::LiftDistance2,F::RetractDistance2,F::LiftSpeed,F::LiftSpeed2,F::RetractSpeed,F::RetractSpeed2})
            row[field]=0.;
    const auto document=sla::LayerDocument::from_fresh_rasters(statistics_masks(),intent);
    const auto job=SLAPrint::create_raster_job(config,document,{.075,.125,.175});
    const auto &s=job->print_statistics();
    CHECK(s.time_basis==SLAPrintStatistics::TimeBasis::OwnedNeutralSchedule);
    CHECK(s.estimated_print_time==18.5);CHECK(s.total_cost==0.);
    REQUIRE(s.layers_times_running_total.size()==3);
    CHECK(s.layers_times_running_total[0]==8.5);
    CHECK(s.layers_times_running_total[1]==13.);
    CHECK(s.layers_times_running_total[2]==18.5);
    CHECK(s.objects_used_material==Catch::Approx(.7).epsilon(0).margin(1e-12));
}
TEST_CASE("Neutral source precision is inspectable independently of destination codec support", "[sla_fresh_statistics]") {
    auto config=statistics_profile();config.set("sla_archive_format","cxdlpv4");config.set("printer_model","CL-60");
    auto intent=statistics_intent();const double exposure=1.234567890123;
    intent.rows[1][F::Exposure]=exposure;
    const auto document=sla::LayerDocument::from_fresh_rasters(statistics_masks(),intent);
    // Grayscale1 and independent row override are not preserving CX inputs,
    // but selecting that destination cannot prevent inspection of source.
    const auto job=SLAPrint::create_raster_job(config,document,{.075,.125,.175});
    CHECK(job->print_statistics().time_basis==SLAPrintStatistics::TimeBasis::OwnedNeutralSchedule);
    CHECK(job->print_statistics().estimated_print_time==Catch::Approx(25.+exposure-3.).epsilon(0).margin(1e-12));
    CHECK(job->layer_document()->fresh_job()->rows==intent.rows);
    CHECK(job->full_print_config().opt_float("exposure_time")==10.);
}

TEST_CASE("Owned source precision governs production editing and project restart", "[sla_source_precision]")
{
    using P=sla::LayerMaskPrecision;
    auto intent=statistics_intent();intent.geometry={256,1,25.6,.1};
    std::vector<uint8_t> input(256), expected(256);
    for(size_t i=0;i<256;++i) {
        input[i]=uint8_t(i);
        expected[i]=i<2?0:uint8_t((i&254)|1);
        CHECK(sla::project_mask_value(input[i],P::Gray8)==input[i]);
        CHECK(sla::project_mask_value(input[i],P::NativeGray7)==expected[i]);
        CHECK(sla::project_mask_value(expected[i],P::NativeGray7)==expected[i]);
    }
    const auto mask=sla::PNGRasterEncoder{}(input.data(),256,1,1);
    intent.mask_precision=P::NativeGray7;
    // Attaching precision to arbitrary stored pixels must not rewrite them.
    CHECK_THROWS(sla::LayerDocument::from_fresh_rasters({mask,mask,mask},intent));
    auto document=sla::LayerDocument::from_generated_rasters({mask,mask,mask},intent);
    CHECK(document.image(0).buf==expected);CHECK_FALSE(document.undo());
    auto edit=document.image(0);edit.buf[0]=132;edit.buf[1]=130;
    document.replace_image(0,edit);
    CHECK(document.image(0).buf[0]==133);CHECK(document.image(0).buf[1]==131);
    REQUIRE(document.undo());CHECK(document.image(0).buf==expected);
    REQUIRE(document.redo());CHECK(document.image(0).buf[0]==133);
    for(const std::string format:{"cxdlpv4","ctb_v4","ctb_v5","ctb_encrypted_v4","ctb_encrypted"})
        CHECK(sla::generated_mask_precision(format)==P::NativeGray7);
    CHECK(sla::generated_mask_precision("SL1")==P::Gray8);
    CHECK(sla::generated_mask_precision("cws")==P::Gray8);
    struct File {
        boost::filesystem::path path=boost::filesystem::unique_path("source-precision-%%%%.sla-project");
        ~File(){boost::system::error_code error;boost::filesystem::remove(path,error);}
    } file;
    sla::ProjectRasterState state{document,{.075,.125,.175},"",{},sla::ProjectLayerOrigin::GeneratedRaster};
    sla::save_layer_project(file.path.string(),state);
    const auto restored=sla::read_project_layers(file.path.string());REQUIRE(restored);
    REQUIRE(restored->document.fresh_job());
    CHECK(restored->document.fresh_job()->mask_precision==P::NativeGray7);
    CHECK(restored->document.image(0).buf==document.image(0).buf);
    auto resumed=restored->document;auto resumed_edit=resumed.image(0);resumed_edit.buf[2]=132;
    resumed.replace_image(0,resumed_edit);CHECK(resumed.image(0).buf[2]==133);
    auto invalid=intent;invalid.mask_precision=P(99);CHECK_THROWS(invalid.validate(3));
    auto neutral_intent=intent;neutral_intent.mask_precision=P::Gray8;
    const auto neutral=sla::LayerDocument::from_fresh_rasters({mask,mask,mask},neutral_intent);
    CHECK(neutral.image(0).buf==input);
}

TEST_CASE("CWS validates long relative schedules without accumulating spurious Z errors", "[sla_cws_position_accumulation]")
{
    const bool scaled_engine=GENERATE(false,true);
    auto source=statistics_intent();source.rows.clear();
    for(auto *group:{&source.globals.normal,&source.globals.bottom}) {
        (*group)[F::LightOff]=0;(*group)[F::LiftDistance]=6.;
        (*group)[F::LiftDistance2]=0;(*group)[F::RetractDistance2]=0;
    }
    for(size_t i=0;i<1000;++i) {
        auto row=source.globals.normal;row[F::PositionZ]=scaled_engine?double((i+1)*50000)*SCALING_FACTOR:double(i+1)*.05;
        source.rows.push_back(row);
    }
    source.validate(source.rows.size());
    const auto commands=sla::cws_encode_command_rows(source);
    const auto decoded=sla::cws_decode_command_rows(commands);
    REQUIRE(decoded.size()==source.rows.size());
    CHECK_NOTHROW(sla::cws_require_command_intent(source,decoded));
    auto bad_position=decoded;bad_position[20].position_z+=.001;
    CHECK_THROWS_AS(sla::cws_require_command_intent(source,bad_position),RuntimeError);
    auto tiny_position=decoded;
    tiny_position[20].position_z=std::nextafter(tiny_position[20].position_z,INFINITY);
    CHECK_THROWS_AS(sla::cws_require_command_intent(source,tiny_position),RuntimeError);
    auto bad_move=decoded;
    bad_move[20].lift[0].distance=std::nextafter(bad_move[20].lift[0].distance,INFINITY);
    CHECK_THROWS_AS(sla::cws_require_command_intent(source,bad_move),RuntimeError);
    if(const char *root=std::getenv("SLA_CWS_Z_FIXTURES")) {
        const auto directory=boost::filesystem::path(root)/(scaled_engine?"scaled":"decimal");
        boost::filesystem::create_directories(directory);
        std::vector<uint8_t> pixels(32,255);
        const auto mask=sla::PNGRasterEncoder{}(pixels.data(),8,4,1);
        const auto document=sla::LayerDocument::from_generated_rasters(std::vector<sla::EncodedRaster>(1000,mask),source);
        std::vector<double> z;for(const auto &row:source.rows)z.push_back(row.at(F::PositionZ));
        auto config=statistics_profile();config.set("display_width",16.);config.set("max_print_height",100.);
        config.set("sla_archive_format","cws");
        const auto print=SLAPrint::create_raster_job(config,document,z);
        print->export_layer_document(document,"cws",(directory/"unapplied.cws").string());
        print->export_print((directory/"applied.cws").string());
        sla::ProjectRasterState state{document,z,"",{},sla::ProjectLayerOrigin::GeneratedRaster};
        sla::save_layer_project((directory/"fresh.sla-project").string(),state);
        const auto restored=sla::read_project_layers((directory/"fresh.sla-project").string());REQUIRE(restored);
        CHECK(restored->document.fresh_job()->rows==source.rows);
        const auto resumed=SLAPrint::create_raster_job(config,restored->document,z);
        resumed->export_print((directory/"restarted.cws").string());
    }
}

TEST_CASE("CWS export projects transition times to milliseconds while preserving original intentions", "[sla_cws_time_projection]")
{
    const bool portrait=GENERATE(false,true);
    auto source=statistics_intent();
    if(portrait)source.geometry={4,8,4.,16.};
    for(auto *group:{&source.globals.normal,&source.globals.bottom}) (*group)[F::LightOff]=0;
    for(auto &row:source.rows)row[F::LightOff]=0;
    source.rows[1][F::Exposure]=20.-17.5/3.;
    source.rows[1][F::WaitBefore]=1./3.;
    source.globals.normal[F::WaitBefore]=.2504;
    const auto document=sla::LayerDocument::from_fresh_rasters(statistics_masks(portrait),source);
    const auto plan=sla::cws_fresh_export_plan(document);
    const auto actual=sla::cws_decode_command_rows(plan.commands);
    REQUIRE(actual.size()==3);
    CHECK(actual[1].exposure_ms==14167);CHECK(actual[1].waits_ms[1]==333);
    CHECK(plan.projected_time_fields==3);
    CHECK(plan.maximum_time_change==Catch::Approx(.0004).margin(1e-15));
    CHECK(plan.represented.globals.normal.at(F::WaitBefore)==.25);
    CHECK(plan.source.rows==source.rows);CHECK(document.fresh_job()->rows==source.rows);
    CHECK(plan.source.globals.normal==source.globals.normal);
    auto tiny=source;tiny.rows[1][F::Exposure]=.0001;
    const auto invalid=sla::LayerDocument::from_fresh_rasters(statistics_masks(portrait),tiny);
    CHECK_THROWS_AS(sla::cws_fresh_export_plan(invalid),RuntimeError);
    auto rejected_config=statistics_profile(portrait);rejected_config.set("display_width",16.);
    rejected_config.set("sla_archive_format","cws");
    const auto rejected=SLAPrint::create_raster_job(rejected_config,invalid,{.075,.125,.175});
    struct ProtectedFile {
        boost::filesystem::path path=boost::filesystem::unique_path("cws-time-range-%%%%.cws");
        ~ProtectedFile(){boost::system::error_code error;boost::filesystem::remove(path,error);}
    } protected_file;
    {std::ofstream(protected_file.path.string(),std::ios::binary)<<"previous print";}
    CHECK_THROWS_AS(rejected->export_print(protected_file.path.string()),RuntimeError);
    CHECK_THROWS_AS(rejected->export_layer_document(invalid,"cws",protected_file.path.string()),RuntimeError);
    std::ifstream kept(protected_file.path.string(),std::ios::binary);
    const std::string contents{std::istreambuf_iterator<char>(kept),std::istreambuf_iterator<char>()};
    CHECK(contents=="previous print");CHECK(rejected->layer_document()->fresh_job()->rows==tiny.rows);
    if(const char *root=std::getenv("SLA_CWS_TIME_FIXTURES")) {
        const auto directory=boost::filesystem::path(root)/(portrait?"portrait":"landscape");
        boost::filesystem::create_directories(directory);
        auto config=statistics_profile(portrait);config.set("display_width",16.);config.set("sla_archive_format","cws");
        const std::vector<double> z{.075,.125,.175};
        const auto print=SLAPrint::create_raster_job(config,document,z);
        print->export_layer_document(document,"cws",(directory/"unapplied.cws").string());
        print->export_print((directory/"applied.cws").string());
        CHECK(print->layer_document()->fresh_job()->rows==source.rows);
        sla::ProjectRasterState state{document,z,"",{},sla::ProjectLayerOrigin::GeneratedRaster};
        sla::save_layer_project((directory/"fresh.sla-project").string(),state);
        const auto restored=sla::read_project_layers((directory/"fresh.sla-project").string());REQUIRE(restored);
        CHECK(restored->document.fresh_job()->rows==source.rows);
        CHECK(restored->document.fresh_job()->globals.normal==source.globals.normal);
        const auto resumed=SLAPrint::create_raster_job(config,restored->document,z);
        resumed->export_print((directory/"restarted.cws").string());
    }
}

TEST_CASE("CWS owned commands preserve actual row motion waits exposure and PWM", "[sla_cws_owned_codec]")
{
    auto intent=statistics_intent();
    for(auto &row:intent.rows) row[F::LightOff]=0.;
    intent.rows[1][F::Exposure]=7.12;
    intent.rows[1][F::LiftDistance]=3.25;
    intent.rows[1][F::LightPWM]=180;
    const auto commands=sla::cws_encode_command_rows(intent);
    const auto actual=sla::cws_decode_command_rows(commands);
    REQUIRE(actual.size()==3);
    double previous=0;
    for(size_t i=0;i<3;++i) {
        CAPTURE(i);const auto &source=intent.rows[i];const auto &row=actual[i];
        CHECK(row.position_z==Catch::Approx(source.at(F::PositionZ)).epsilon(0).margin(1e-12));
        REQUIRE(row.lift.size()==2);REQUIRE(row.retract.size()==2);
        CHECK(row.lift[0].distance==source.at(F::PositionZ)-previous+source.at(F::LiftDistance));
        CHECK(row.lift[0].speed==source.at(F::LiftSpeed));
        CHECK(row.lift[1].distance==source.at(F::LiftDistance2));
        CHECK(row.lift[1].speed==source.at(F::LiftSpeed2));
        CHECK(row.retract[0].distance==-(source.at(F::LiftDistance)+source.at(F::LiftDistance2)-source.at(F::RetractDistance2)));
        CHECK(row.retract[0].speed==source.at(F::RetractSpeed));
        CHECK(row.retract[1].distance==-source.at(F::RetractDistance2));
        CHECK(row.retract[1].speed==source.at(F::RetractSpeed2));
        CHECK(row.waits_ms==std::array<uint32_t,3>{750,250,500});
        CHECK(row.exposure_ms==uint32_t(source.at(F::Exposure)*1000));
        CHECK(row.pwm==unsigned(source.at(F::LightPWM)));
        previous=source.at(F::PositionZ);
    }
    CHECK(intent.rows[1].at(F::Exposure)==7.12);
    SECTION("Equal exposure all-bottom is executable regardless of inferred native count") {
        auto equal=intent;equal.globals.bottom_layers=3;
        for(auto &row:equal.rows)row[F::Exposure]=5.12;
        const auto decoded=sla::cws_decode_command_rows(sla::cws_encode_command_rows(equal));
        REQUIRE(decoded.size()==3);for(auto &row:decoded)CHECK(row.exposure_ms==5120);
    }
    SECTION("A dark row still preserves actual delays") {
        auto dark=intent;dark.rows[1][F::LightPWM]=0.;
        const auto decoded=sla::cws_decode_command_rows(sla::cws_encode_command_rows(dark));
        // Zero power disables light, not the requested timer interval.
        CHECK(decoded[1].pwm==0);CHECK(decoded[1].exposure_ms==7120);
        CHECK(decoded[1].waits_ms==std::array<uint32_t,3>{750,250,500});
    }
    SECTION("Unsupported light-off and non-millisecond times are named refusals") {
        auto invalid=intent;invalid.rows[0][F::LightOff]=1.;
        CHECK_THROWS_WITH(sla::cws_encode_command_rows(invalid),"CWS cannot preserve layer 1.LightOff: independent light-off delay has no command field");
        invalid=intent;invalid.rows[0][F::Exposure]=2.1234;
        CHECK_THROWS_WITH(sla::cws_encode_command_rows(invalid),"CWS cannot preserve layer 1.Exposure: outside exact uint32 millisecond range");
    }
    SECTION("Reader rejects altered mode, ordering, motion and missing layer end") {
        auto corrupt=commands;corrupt.replace(corrupt.find("G91"),3,"G90");CHECK_THROWS(sla::cws_decode_command_rows(corrupt));
        corrupt=commands;corrupt.replace(corrupt.find("LAYER_START:1"),13,"LAYER_START:7");CHECK_THROWS(sla::cws_decode_command_rows(corrupt));
        corrupt=commands;corrupt.replace(corrupt.find(" F60"),4," F0");CHECK_THROWS(sla::cws_decode_command_rows(corrupt));
        corrupt=commands;corrupt.replace(corrupt.find(" F60"),4," F1e300");CHECK_THROWS(sla::cws_decode_command_rows(corrupt));
        corrupt=commands;const auto sync=corrupt.find("M400",corrupt.find(";LAYER_START:0"));
        corrupt.erase(sync,6);CHECK_THROWS(sla::cws_decode_command_rows(corrupt));
        corrupt=commands;corrupt.erase(corrupt.find(";LAYER_END:2"));CHECK_THROWS(sla::cws_decode_command_rows(corrupt));
    }
}

TEST_CASE("Owned CWS exports preserve overrides masks and source intent after restart", "[sla_cws_owned_export]")
{
    const bool portrait=GENERATE(false,true);
    auto intent=statistics_intent();
    if(portrait)intent.geometry={4,8,4.,16.};
    for(auto &row:intent.rows)row[F::LightOff]=0.;
    intent.rows[1][F::Exposure]=7.12;intent.rows[1][F::LightPWM]=180;
    std::vector<sla::EncodedRaster> masks;
    for(size_t i=0;i<3;++i) {
        std::vector<uint8_t> pixels(32,0);
        for(size_t j=0;j<6;++j)pixels[j]=std::vector<uint8_t>{1,127,128,130,132,255}[j];
        pixels[10+i]=255;
        masks.push_back(sla::PNGRasterEncoder{}(pixels.data(),portrait?4:8,portrait?8:4,1));
    }
    const auto document=sla::LayerDocument::from_fresh_rasters(std::move(masks),intent);
    auto config=statistics_profile(portrait);config.set("display_width",16.);
    config.set("sla_archive_format","cws");config.set("printer_model","GENERIC");
    // Conflicting, but valid, settings must not replace owned row intent.
    config.set("exposure_time",9.);config.set("initial_exposure_time",19.);
    config.set("sla_wait_before_cure",9.);config.set("sla_lift_height",10.);
    const auto print=SLAPrint::create_raster_job(config,document,{.075,.125,.175});
    auto edited=document;edited.paint(0,portrait?3:7,0,0,132);
    struct Directory {
        boost::filesystem::path path=boost::filesystem::unique_path("owned-cws-%%%%");bool keep=false;
        ~Directory(){if(!keep){boost::system::error_code error;boost::filesystem::remove_all(path,error);}}
    } directory;
    if(const char *root=std::getenv("SLA_CWS_OWNED_FIXTURES")) {
        directory.path=boost::filesystem::path(root)/(portrait?"portrait":"landscape");directory.keep=true;
    }
    boost::filesystem::create_directories(directory.path);
    auto verify=[&](const boost::filesystem::path &file) {
        const auto decoded=sla::LayerDocument::load_archive(file.string());
        REQUIRE(decoded.archive_format()=="cws");REQUIRE(decoded.layer_count()==3);
        for(size_t i=0;i<3;++i)CHECK(decoded.image(i).buf==edited.image(i).buf);
        const auto archive=read_zipper_archive(file.string(),{".gcode",".slicing"},{});
        REQUIRE(archive.entries.size()==2);
        for(const auto &entry:archive.entries) {
            const std::string text(entry.buf.begin(),entry.buf.end());
            if(entry.fname=="sla_job.gcode") {
                const auto rows=sla::cws_decode_command_rows(text);REQUIRE(rows.size()==3);
                CHECK(rows[0].exposure_ms==2000);CHECK(rows[1].exposure_ms==7120);CHECK(rows[2].exposure_ms==4000);
                CHECK(rows[1].pwm==180);CHECK(rows[0].waits_ms[1]==250);
                CHECK(rows[0].position_z==Catch::Approx(.075).epsilon(0).margin(1e-12));
                CHECK(rows[2].position_z==Catch::Approx(.175).epsilon(0).margin(1e-12));
            } else {
                CHECK(text.find("<LayerTime>3000</LayerTime>")!=std::string::npos);
                CHECK(text.find("<FirstLayerTime>2000</FirstLayerTime>")!=std::string::npos);
                CHECK(text.find("<NumberofBottomLayers>1</NumberofBottomLayers>")!=std::string::npos);
                CHECK(text.find("<SliceHeight>0.050000000000000003</SliceHeight>")!=std::string::npos);
            }
        }
    };
    auto file=directory.path/"unapplied.cws";
    print->export_layer_document(edited,"cws",file.string());verify(file);
    CHECK(print->layer_document()->image(0).buf==document.image(0).buf);
    CHECK(print->layer_document()->fresh_job()->rows==intent.rows);
    print->replace_layer_document(edited);file=directory.path/"applied.cws";
    print->export_print(file.string());verify(file);
    sla::ProjectRasterState state{edited,{.075,.125,.175},"",{},sla::ProjectLayerOrigin::GeneratedRaster};
    const auto project=directory.path/"fresh.sla-project";sla::save_layer_project(project.string(),state);
    const auto restored=sla::read_project_layers(project.string());REQUIRE(restored);
    const auto resumed=SLAPrint::create_raster_job(config,restored->document,{.075,.125,.175});
    file=directory.path/"restarted.cws";resumed->export_print(file.string());verify(file);
    CHECK(resumed->layer_document()->fresh_job()->rows==intent.rows);
    CHECK(resumed->layer_document()->fresh_job()->globals.bottom==intent.globals.bottom);
    auto invalid=intent;invalid.rows[0][F::LightOff]=1.;
    std::vector<sla::EncodedRaster> bad_masks;
    for(size_t i=0;i<3;++i) {
        const auto bytes=edited.encoded_layer(i);const auto *begin=static_cast<const uint8_t *>(bytes.buf);
        bad_masks.emplace_back(std::vector<uint8_t>(begin,begin+bytes.sz),"png");
    }
    const auto unsupported=sla::LayerDocument::from_fresh_rasters(std::move(bad_masks),invalid);
    file=directory.path/"sentinel.cws";{std::ofstream out(file.string(),std::ios::binary);out<<"sentinel";}
    CHECK_THROWS(print->export_layer_document(unsupported,"cws",file.string()));
    std::ifstream input(file.string(),std::ios::binary);const std::string retained((std::istreambuf_iterator<char>(input)),{});
    CHECK(retained=="sentinel");CHECK(print->layer_document()->fresh_job()->rows==intent.rows);
}

TEST_CASE("CWS executable boundary schedules survive both export routes", "[sla_cws_owned_boundaries]")
{
    const unsigned scenario=GENERATE(0u,1u,2u,3u,4u,5u);
    const bool portrait=GENERATE(false,true);
    const std::array<const char *,6> names{"dark","zero","equal","no_lift","second_lift","dummy"};
    CAPTURE(names[scenario],portrait);
    auto intent=statistics_intent();if(portrait)intent.geometry={4,8,4.,16.};
    for(auto &row:intent.rows)row[F::LightOff]=0.;
    if(scenario==0)intent.rows[1][F::LightPWM]=0;
    if(scenario==1)intent.rows[1][F::Exposure]=0;
    if(scenario==2) {
        intent.globals.bottom_layers=3;
        intent.globals.normal[F::Exposure]=intent.globals.bottom[F::Exposure]=5.12;
        for(auto &row:intent.rows)row[F::Exposure]=5.12;
    }
    if(scenario==3 || scenario==4) {
        for(auto &row:intent.rows) {
            row[F::LiftDistance]=0;
            row[F::LiftDistance2]=scenario==3?0:2;
            row[F::RetractDistance2]=scenario==3?0:.5;
        }
    }
    std::vector<sla::EncodedRaster> masks;
    for(size_t i=0;i<3;++i) {
        std::vector<uint8_t> pixels(32,0);
        if(scenario!=5 || i!=1)for(size_t j=0;j<7;++j)pixels[j]=255;
        else pixels[0]=255;
        masks.push_back(sla::PNGRasterEncoder{}(pixels.data(),portrait?4:8,portrait?8:4,1));
    }
    const auto document=sla::LayerDocument::from_fresh_rasters(std::move(masks),intent);
    auto config=statistics_profile(portrait);config.set("display_width",16.);config.set("sla_archive_format","cws");
    const auto print=SLAPrint::create_raster_job(config,document,{.075,.125,.175});
    struct Directory {
        boost::filesystem::path path=boost::filesystem::unique_path("cws-boundary-%%%%");bool keep=false;
        ~Directory(){if(!keep){boost::system::error_code error;boost::filesystem::remove_all(path,error);}}
    } directory;
    if(const char *root=std::getenv("SLA_CWS_BOUNDARY_FIXTURES")) {
        directory.path=boost::filesystem::path(root)/(std::string(names[scenario])+(portrait?"-portrait":"-landscape"));directory.keep=true;
    }
    boost::filesystem::create_directories(directory.path);
    for(const std::string route:{"print","document"}) {
        const auto path=(directory.path/(route+".cws")).string();
        if(route=="print")print->export_print(path);else print->export_layer_document(document,"cws",path);
        const auto native=sla::LayerDocument::load_archive(path);REQUIRE(native.layer_count()==3);
        for(size_t i=0;i<3;++i)CHECK(native.image(i).buf==document.image(i).buf);
        const auto archive=read_zipper_archive(path,{".gcode"},{});REQUIRE(archive.entries.size()==1);
        const auto &bytes=archive.entries[0].buf;
        const auto rows=sla::cws_decode_command_rows(std::string(bytes.begin(),bytes.end()));REQUIRE(rows.size()==3);
        for(size_t i=0;i<3;++i) {
            CHECK(rows[i].position_z==Catch::Approx(intent.rows[i].at(F::PositionZ)).epsilon(0).margin(1e-12));
            CHECK(rows[i].exposure_ms==uint32_t(std::round(intent.rows[i].at(F::Exposure)*1000)));
            CHECK(rows[i].pwm==(rows[i].exposure_ms?unsigned(intent.rows[i].at(F::LightPWM)):0));
            CHECK(rows[i].waits_ms==std::array<uint32_t,3>{750,250,500});
            CHECK(rows[i].z_advance_commit==(scenario==3));
        }
    }
    CHECK(print->layer_document()->fresh_job()->rows==intent.rows);
}

TEST_CASE("Legacy CWS dialects retain no-peel cure commands and dark timers", "[sla_cws_legacy_zero_lift]")
{
    const auto format=GENERATE("cws","cwsxml","cwsnovamaker","cwsrgb");
    const bool portrait=GENERATE(false,true);CAPTURE(format,portrait);
    auto config=statistics_profile(portrait);config.set("sla_archive_format",format);
    config.set("display_pixels_x",6);config.set("display_pixels_y",3);
    config.set("display_width",16.);config.set("display_height",4.);
    config.set("layer_height",.05);config.set("sla_bottom_layers",1);
    config.set("exposure_time",2.12);config.set("initial_exposure_time",5.12);
    for(const auto prefix:{"sla_","sla_bottom_"}) {
        config.set(std::string(prefix)+"lift_speed",60.);
        config.set(std::string(prefix)+"wait_before_cure",.25);
        config.set(std::string(prefix)+"wait_after_cure",.5);
        config.set(std::string(prefix)+"wait_after_lift",.75);
    }
    // No producer/context inference: plain masks retain the old archive route.
    std::vector<sla::EncodedRaster> masks;
    for(size_t i=0;i<3;++i) {
        std::vector<uint8_t> pixels(18,0);for(size_t j=0;j<7;++j)pixels[j]=uint8_t(j*30+1);
        masks.push_back(sla::PNGRasterEncoder{}(pixels.data(),portrait?3:6,portrait?6:3,1));
    }
    const auto document=sla::LayerDocument::from_rasters(std::move(masks));
    const auto print=SLAPrint::create_raster_job(config,document,{.05,.10,.15});
    CHECK_FALSE(print->layer_document()->fresh_job());
    struct Directory {
        boost::filesystem::path path=boost::filesystem::unique_path("cws-legacy-%%%%");bool keep=false;
        ~Directory(){if(!keep){boost::system::error_code error;boost::filesystem::remove_all(path,error);}}
    } directory;
    if(const char *root=std::getenv("SLA_CWS_LEGACY_FIXTURES")) {
        directory.path=boost::filesystem::path(root)/(std::string(format)+(portrait?"-portrait":"-landscape"));directory.keep=true;
    }
    boost::filesystem::create_directories(directory.path);
    const std::string suffix=std::string(format)=="cwsxml"?".xml.cws":std::string(format)=="cwsrgb"?".rgb.cws":".cws";
    for(bool dark:{false,true}) {
        auto exported_config=config;if(dark)exported_config.set("sla_light_pwm",0);
        const auto job=SLAPrint::create_raster_job(exported_config,document,{.05,.10,.15});
        const auto path=(directory.path/((dark?std::string("dark"):std::string("active"))+suffix)).string();
        job->export_print(path);
        const auto native=sla::LayerDocument::load_archive(path);REQUIRE(native.layer_count()==3);
        for(size_t i=0;i<3;++i)CHECK(native.image(i).buf==document.image(i).buf);
        const auto archive=read_zipper_archive(path,{".gcode"},{});REQUIRE(archive.entries.size()==1);
        const std::string commands(archive.entries[0].buf.begin(),archive.entries[0].buf.end());
        if(std::string(format)=="cwsxml")
            CHECK(commands.find(";<Delay> 00\nM400")!=std::string::npos);
        for(size_t i=0;i<3;++i) {
            CHECK(commands.find(";CWS_Z_ADVANCE_COMMIT:"+std::to_string(i))!=std::string::npos);
            CHECK(commands.find(";<Delay> "+std::to_string(i?2120:5120))!=std::string::npos);
        }
        CHECK_FALSE(job->layer_document()->fresh_job());
    }
}

TEST_CASE("CBDDLP owned plan projects single-stage motion and preserves arbitrary cure rows", "[sla_cbddlp_fresh_plan]")
{
    auto source=statistics_intent();
    for(auto *group:{&source.globals.normal,&source.globals.bottom}) {
        (*group)[F::LiftDistance2]=0;(*group)[F::RetractDistance2]=0;
        (*group)[F::WaitAfter]=0;(*group)[F::WaitAfterLift]=0;
        (*group)[F::LightOff]=0;(*group)[F::RetractSpeed]=120;
    }
    source.globals.bottom[F::LiftDistance]=2;
    for(size_t i=0;i<3;++i) {
        source.rows[i]=i?source.globals.normal:source.globals.bottom;
        source.rows[i][F::PositionZ]=std::vector<double>{.075,.125,.175}[i];
    }
    source.rows[1][F::Exposure]=7.12;
    source.rows[2][F::LightOff]=5.;
    auto pixels=std::vector<uint8_t>(32,255);
    const auto mask=sla::PNGRasterEncoder{}(pixels.data(),8,4,1);
    const auto document=sla::LayerDocument::from_fresh_rasters({mask,mask,mask},source);
    const auto plan=sla::cbddlp_fresh_export_plan(document);
    REQUIRE(plan.rows.size()==3);
    CHECK(plan.source.rows==source.rows);
    CHECK(plan.nominal_height==float(.05));CHECK(plan.bottom_layers==1);
    CHECK(plan.lift_height==std::array<float,2>{1,2});
    CHECK(plan.lift_speed==std::array<float,2>{60,60});CHECK(plan.retract_speed==120);
    // Motor time is 1.5s/3s; light-off is a floor, then before-cure is added.
    CHECK(plan.default_light_off==std::array<float,2>{1.75,3.25});
    CHECK(plan.rows[0].light_off==float(3.25));CHECK(plan.rows[1].light_off==float(1.75));
    CHECK(plan.rows[2].light_off==float(5.25));CHECK(plan.rows[1].exposure==float(7.12));
    for(size_t i=0;i<3;++i)CHECK(plan.rows[i].z==float(source.rows[i].at(F::PositionZ)));
    auto check_bad=[&](sla::FreshLayerJob invalid) {
        const auto bad=sla::LayerDocument::from_fresh_rasters({mask,mask,mask},std::move(invalid));
        CHECK_THROWS(sla::cbddlp_fresh_export_plan(bad));
    };
    SECTION("No per-row motion table may be invented") {
        auto invalid=source;invalid.rows[1][F::LiftDistance]=1.25;check_bad(invalid);
        invalid=source;invalid.rows[1][F::LightPWM]=180;check_bad(invalid);
    }
    SECTION("Two-stage and phase rests need a format that can represent them") {
        auto invalid=source;invalid.rows[1][F::WaitAfter]=.25;check_bad(invalid);
        invalid=source;invalid.rows[1][F::LiftDistance2]=1;check_bad(invalid);
    }
    SECTION("Native float conversion must not collapse layer Z") {
        auto invalid=source;invalid.rows[1][F::PositionZ]=.07500000001;check_bad(invalid);
    }
}

TEST_CASE("Owned CWS conversion checks source Z before staging a destination", "[sla_cws_height_limit]")
{
    auto intent=statistics_intent();for(auto &row:intent.rows)row[F::LightOff]=0;
    auto config=statistics_profile();config.set("display_width",16.);config.set("max_print_height",.1);
    const auto document=sla::LayerDocument::from_fresh_rasters(statistics_masks(),intent);
    struct File {
        boost::filesystem::path path=boost::filesystem::unique_path("cws-height-%%%%.cws");
        ~File(){boost::system::error_code error;boost::filesystem::remove(path,error);}
    } file;
    {std::ofstream output(file.path.string(),std::ios::binary);output<<"sentinel";}
    CHECK_THROWS_WITH(SLAPrint::convert_layer_archive(config,document,"cws",file.path.string()),
        "Source layer Z exceeds the selected printer height");
    std::ifstream input(file.path.string(),std::ios::binary);const std::string retained((std::istreambuf_iterator<char>(input)),{});
    CHECK(retained=="sentinel");CHECK(document.fresh_job()->rows==intent.rows);
}

TEST_CASE("CBDDLP source palettes preserve every native level through encoding and restart", "[sla_cbddlp_source_palette]")
{
    const unsigned planes=GENERATE(1u,2u,3u,4u,5u,6u,7u,8u,9u,10u,11u,12u,13u,14u,15u,16u);
    CAPTURE(planes);
    const auto precision=sla::LayerMaskPrecision(int(planes)+1);
    // Literal native palette is the accumulated plane-count decoder scale.
    // Non-power counts do not have 255 as their maximum decoded value.
    std::vector<uint8_t> palette{0};
    for(unsigned count=1;count<=planes;++count)palette.push_back(uint8_t(count*(256/planes)-1));
    std::vector<uint8_t> input(256),expected(256);
    for(size_t i=0;i<256;++i) {
        input[i]=uint8_t(i);
        expected[i]=*std::prev(std::upper_bound(palette.begin(),palette.end(),uint8_t(i)));
        REQUIRE(sla::project_mask_value(input[i],precision)==expected[i]);
        REQUIRE(sla::project_mask_value(expected[i],precision)==expected[i]);
    }
    auto intent=statistics_intent();intent.geometry={256,1,25.6,.1};intent.mask_precision=precision;
    const auto mask=sla::PNGRasterEncoder{}(input.data(),256,1,1);
    auto document=sla::LayerDocument::from_generated_rasters({mask,mask,mask},intent);
    REQUIRE(document.image(0).buf==expected);
    document.paint(0,0,0,0,255);CHECK(document.image(0).buf[0]==palette.back());
    REQUIRE(document.undo());CHECK(document.image(0).buf==expected);REQUIRE(document.redo());
    expected[0]=palette.back();CHECK(document.image(0).buf==expected);
    auto config=statistics_profile();config.set("display_pixels_x",256);config.set("display_pixels_y",1);
    config.set("display_width",25.6);config.set("display_height",.1);config.set("sla_archive_format","cbddlp");
    config.set("sla_cbddlp_aa",int(planes));
    const auto plain=sla::LayerDocument::from_rasters({mask,mask,mask});
    const auto context=SLAPrint::create_raster_job(config,plain,{.075,.125,.175});
    struct Directory {
        boost::filesystem::path path=boost::filesystem::unique_path("cbd-palette-%%%%");bool keep=false;
        ~Directory(){if(!keep){boost::system::error_code error;boost::filesystem::remove_all(path,error);}}
    } directory;
    if(const char *root=std::getenv("SLA_CBD_PALETTE_FIXTURES")) {
        directory.path=boost::filesystem::path(root)/std::to_string(planes);directory.keep=true;
    }
    boost::filesystem::create_directories(directory.path);
    sla::ProjectRasterState state{document,{.075,.125,.175},"",{},sla::ProjectLayerOrigin::GeneratedRaster};
    const auto project=directory.path/"palette.sla-project";sla::save_layer_project(project.string(),state);
    const auto restored=sla::read_project_layers(project.string());REQUIRE(restored);
    CHECK(restored->document.fresh_job()->mask_precision==precision);
    CHECK(restored->document.image(0).buf==expected);
    // Exercise real native encoder independently of the pending fresh writer.
    // Metadata here is intentionally the plain legacy context, not acceptance
    // of neutral motion/exposure/volume publication yet.
    auto writer=SLAArchiveWriter::create("cbddlp",context->printer_config());REQUIRE(writer);
    writer->encode_layers(restored->document);
    const auto file=directory.path/"palette.cbddlp";writer->export_print(file.string(),*context,{});
    const auto native=sla::LayerDocument::load_archive(file.string());
    CHECK(native.image(0).buf==expected);
    for(size_t i=1;i<3;++i) {
        auto untouched=expected;untouched[0]=0;CHECK(native.image(i).buf==untouched);
    }
}

TEST_CASE("Modern PWC preserves source speed precision across export and restart", "[sla_pwc_fresh_export]")
{
    const auto version=GENERATE("516","517","518");
    auto source=statistics_intent();
    for(auto *group:{&source.globals.normal,&source.globals.bottom}) {
        for(auto field:{F::RetractDistance2,F::WaitAfter,F::WaitAfterLift,F::LightOff}) (*group)[field]=0;
        (*group)[F::LiftSpeed2]=.019; // Float(mm/min) roundtrip loses a native mm/s bit.
    }
    for(size_t i=0;i<3;++i) {
        source.rows[i]=i?source.globals.normal:source.globals.bottom;
        source.rows[i][F::PositionZ]=std::vector<double>{.075,.125,.175}[i];
    }
    source.rows[1][F::Exposure]=7.12;
    std::vector<uint8_t> pixels(32);for(size_t i=0;i<32;++i)pixels[i]=uint8_t((i%16)*17);
    const auto mask=sla::PNGRasterEncoder{}(pixels.data(),8,4,1);
    auto document=sla::LayerDocument::from_generated_rasters({mask,mask,mask},source);
    auto config=statistics_profile();config.set("display_width",16.);
    config.set("sla_archive_format","pwc");config.set("sla_anycubic_version",version);
    auto print=SLAPrint::create_raster_job(config,document,{.075,.125,.175});
    document.paint(0,0,0,0,255);
    struct Directory {
        boost::filesystem::path path=boost::filesystem::unique_path("pwc-fresh-%%%%");bool keep=false;
        ~Directory(){if(!keep){boost::system::error_code error;boost::filesystem::remove_all(path,error);}}
    } directory;
    if(const char *root=std::getenv("SLA_PWC_FRESH_FIXTURES")) {
        directory.path=boost::filesystem::path(root)/version;directory.keep=true;
    }
    boost::filesystem::create_directories(directory.path);
    auto verify=[&](const boost::filesystem::path &path) {
        std::ifstream input(path.string(),std::ios::binary);
        const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)),{});
        const AnycubicLayerLayout layout(bytes,"pwc");
        CHECK(layout.version==std::stoul(version));
        const auto extra=layout.integer(40);
        CHECK(layout.number(extra+36)==float(.019/60.));
        CHECK(layout.number(extra+64)==float(.019/60.));
        for(size_t i=0;i<3;++i) {
            const auto at=layout.record(i);
            CHECK(layout.number(at+8)==1.f); // FIRST phase, not the combined lift.
            CHECK(layout.number(at+16)==float(source.rows[i].at(F::Exposure)));
            CHECK(layout.decode(i)==document.image(i).buf);
            if(layout.subtable)CHECK(layout.decode_subimage(i)==document.image(i).buf);
        }
    };
    auto output=directory.path/"unapplied.pwc";
    print->export_layer_document(document,"pwc",output.string());verify(output);
    CHECK(print->layer_document()->image(0).get(0,0)==0);
    print->replace_layer_document(document);output=directory.path/"applied.pwc";
    print->export_print(output.string());verify(output);
    sla::ProjectRasterState state{document,{.075,.125,.175},"",{},sla::ProjectLayerOrigin::GeneratedRaster};
    sla::save_layer_project((directory.path/"fresh.sla-project").string(),state);
    const auto restored=sla::read_project_layers((directory.path/"fresh.sla-project").string());REQUIRE(restored);
    const auto resumed=SLAPrint::create_raster_job(config,restored->document,{.075,.125,.175});
    output=directory.path/"restarted.pwc";resumed->export_print(output.string());verify(output);
    CHECK(resumed->layer_document()->fresh_job()->rows==source.rows);
}

TEST_CASE("Modern Anycubic extensions preserve owned intent through export and restart", "[sla_anycubic_modern_fresh]")
{
    const auto format=GENERATE("pwmx","pwmo","pwms","pmsq","dlp","pwma","pm3","pm3m",
        "pwmb","dl2p","pmx2","pm3r","pm3n","pm5","px6s","pm5s","m5sp","pm4n","pwc");
    for (const auto native_version : anycubic_available_versions(format)) {
    if (native_version < 516) continue;
    const auto version=std::to_string(native_version);
    CAPTURE(format, version);
    auto source=statistics_intent();
    for(auto *group:{&source.globals.normal,&source.globals.bottom}) {
        for(auto field:{F::RetractDistance2,F::WaitAfter,F::WaitAfterLift,F::LightOff}) (*group)[field]=0;
        (*group)[F::LiftSpeed2]=.019; // Float(mm/min) roundtrip loses a native mm/s bit.
    }
    for(size_t i=0;i<3;++i) {
        source.rows[i]=i?source.globals.normal:source.globals.bottom;
        source.rows[i][F::PositionZ]=std::vector<double>{.075,.125,.175}[i];
    }
    source.rows[1][F::Exposure]=7.12;
    std::vector<uint8_t> pixels(32);for(size_t i=0;i<32;++i)pixels[i]=uint8_t((i%16)*17);
    const auto mask=sla::PNGRasterEncoder{}(pixels.data(),8,4,1);
    auto document=sla::LayerDocument::from_generated_rasters({mask,mask,mask},source);
    auto config=statistics_profile();config.set("display_width",16.);
    config.set("sla_archive_format",format);config.set("sla_anycubic_version",version);
    auto print=SLAPrint::create_raster_job(config,document,{.075,.125,.175});
    document.paint(0,0,0,0,255);
    struct Directory {
        boost::filesystem::path path=boost::filesystem::unique_path("pwc-fresh-%%%%");bool keep=false;
        ~Directory(){if(!keep){boost::system::error_code error;boost::filesystem::remove_all(path,error);}}
    } directory;
    if(const char *root=std::getenv("SLA_ANYCUBIC_MODERN_FIXTURES")) {
        directory.path=boost::filesystem::path(root)/(std::string(format)+"-"+version);directory.keep=true;
    }
    boost::filesystem::create_directories(directory.path);
    auto verify=[&](const boost::filesystem::path &path) {
        std::ifstream input(path.string(),std::ios::binary);
        const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)),{});
        const AnycubicLayerLayout layout(bytes,format);
        CHECK(layout.version==std::stoul(version));
        const auto extra=layout.integer(40);
        CHECK(layout.number(extra+36)==float(.019/60.));
        CHECK(layout.number(extra+64)==float(.019/60.));
        for(size_t i=0;i<3;++i) {
            const auto at=layout.record(i);
            CHECK(layout.number(at+8)==1.f); // FIRST phase, not the combined lift.
            CHECK(layout.number(at+16)==float(source.rows[i].at(F::Exposure)));
            CHECK(layout.decode(i)==document.image(i).buf);
            if(layout.subtable)CHECK(layout.decode_subimage(i)==document.image(i).buf);
        }
    };
    auto output=directory.path/(std::string("unapplied.")+format);
    print->export_layer_document(document,format,output.string());verify(output);
    CHECK(print->layer_document()->image(0).get(0,0)==0);
    print->replace_layer_document(document);output=directory.path/(std::string("applied.")+format);
    print->export_print(output.string());verify(output);
    sla::ProjectRasterState state{document,{.075,.125,.175},"",{},sla::ProjectLayerOrigin::GeneratedRaster};
    sla::save_layer_project((directory.path/"fresh.sla-project").string(),state);
    const auto restored=sla::read_project_layers((directory.path/"fresh.sla-project").string());REQUIRE(restored);
    const auto resumed=SLAPrint::create_raster_job(config,restored->document,{.075,.125,.175});
    output=directory.path/(std::string("restarted.")+format);resumed->export_print(output.string());verify(output);
    CHECK(resumed->layer_document()->fresh_job()->rows==source.rows);
    }
}

TEST_CASE("Modern PWC rejects collapsed layer positions without replacing output", "[sla_pwc_fresh_boundaries]")
{
    const auto version=GENERATE("516","517","518");
    auto source=statistics_intent();
    for(auto *group:{&source.globals.normal,&source.globals.bottom})
        for(auto field:{F::RetractDistance2,F::WaitAfter,F::WaitAfterLift,F::LightOff}) (*group)[field]=0;
    const std::vector<double> z{.0001,.0002,.0003};
    for(size_t i=0;i<3;++i) {
        source.rows[i]=i?source.globals.normal:source.globals.bottom;
        source.rows[i][F::PositionZ]=z[i];
    }
    std::vector<uint8_t> pixels(32,255);
    const auto mask=sla::PNGRasterEncoder{}(pixels.data(),8,4,1);
    const auto document=sla::LayerDocument::from_generated_rasters({mask,mask,mask},source);
    CHECK_THROWS_AS(sla::anycubic_fresh_export_plan(document),RuntimeError);
    auto config=statistics_profile();config.set("display_width",16.);
    config.set("sla_archive_format","pwc");config.set("sla_anycubic_version",version);
    const auto print=SLAPrint::create_raster_job(config,document,z);
    struct File {
        boost::filesystem::path path=boost::filesystem::unique_path("pwc-height-%%%%.pwc");
        ~File(){boost::system::error_code error;boost::filesystem::remove(path,error);}
    } file;
    auto sentinel=[&](){std::ofstream(file.path.string(),std::ios::binary)<<"preserve original";};
    auto preserved=[&](){
        std::ifstream input(file.path.string(),std::ios::binary);
        const std::string contents{std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>()};
        const bool original_preserved=contents=="preserve original";
        CHECK(original_preserved);
        CHECK(print->layer_document()->fresh_job()->rows==source.rows);
        CHECK(print->layer_document()->image(0).buf==document.image(0).buf);
    };
    sentinel();CHECK_THROWS_AS(print->export_print(file.path.string()),RuntimeError);preserved();
    sentinel();CHECK_THROWS_AS(print->export_layer_document(document,"pwc",file.path.string()),RuntimeError);preserved();
}

TEST_CASE("Owned four-bit masks preserve their palette through editing and project restart", "[sla_pwc_source_palette]")
{
    const auto precision=sla::LayerMaskPrecision(18);
    for(unsigned value=0;value<256;++value) {
        const auto expected=uint8_t((value>>4)*17);
        CHECK(sla::project_mask_value(uint8_t(value),precision)==expected);
        CHECK(sla::project_mask_value(expected,precision)==expected);
    }
    auto source=statistics_intent();source.mask_precision=precision;
    std::vector<uint8_t> pixels(32);for(size_t i=0;i<32;++i)pixels[i]=uint8_t(i*8);
    const auto mask=sla::PNGRasterEncoder{}(pixels.data(),8,4,1);
    auto document=sla::LayerDocument::from_generated_rasters({mask,mask,mask},source);
    for(size_t i=0;i<32;++i)CHECK(document.image(0).buf[i]==uint8_t((pixels[i]>>4)*17));
    const auto before=document.image(0).buf;
    document.paint(0,0,0,0,33);CHECK(document.image(0).get(0,0)==34);
    document.undo();CHECK(document.image(0).buf==before);
    document.redo();CHECK(document.image(0).get(0,0)==34);
    struct File {
        boost::filesystem::path path=boost::filesystem::unique_path("pwc-palette-%%%%.sla-project");
        ~File(){boost::system::error_code error;boost::filesystem::remove(path,error);}
    } file;
    sla::ProjectRasterState state{document,{.075,.125,.175},"",{},sla::ProjectLayerOrigin::GeneratedRaster};
    sla::save_layer_project(file.path.string(),state);
    const auto restored=sla::read_project_layers(file.path.string());REQUIRE(restored);
    CHECK(restored->document.fresh_job()->mask_precision==precision);
    CHECK(restored->document.image(0).buf==document.image(0).buf);
    CHECK(restored->document.fresh_job()->rows==source.rows);
}

TEST_CASE("Explicit palette export copies preserve the source and survive restart", "[sla_pwc_palette_copy]")
{
    const auto version=GENERATE("516","517","518");const bool portrait=GENERATE(false,true);
    auto source=statistics_intent();if(portrait)source.geometry={4,8,4.,16.};
    for(auto *group:{&source.globals.normal,&source.globals.bottom}) {
        for(auto field:{F::RetractDistance2,F::WaitAfter,F::WaitAfterLift,F::LightOff}) (*group)[field]=0;
        (*group)[F::LiftSpeed2]=.019;
    }
    for(size_t i=0;i<3;++i) {
        source.rows[i]=i?source.globals.normal:source.globals.bottom;
        source.rows[i][F::PositionZ]=std::vector<double>{.075,.125,.175}[i];
    }
    source.rows[1][F::Exposure]=7.12;
    std::vector<uint8_t> pixels(32);for(size_t i=0;i<32;++i)pixels[i]=uint8_t(i*8);
    const auto mask=sla::PNGRasterEncoder{}(pixels.data(),portrait?4:8,portrait?8:4,1);
    auto document=sla::LayerDocument::from_generated_rasters({mask,mask,mask},source);
    document.paint(0,0,0,0,33);const auto original=document.image(0).buf;
    const auto revision=document.revision();const auto precision=sla::LayerMaskPrecision::NativeGray4;
    const auto loss=sla::layer_palette_loss(document,precision);
    CHECK(loss.changed_pixels==91);CHECK(loss.maximum_change==15);
    CHECK_THROWS_AS(sla::layer_palette_loss(document,sla::LayerMaskPrecision(99)),RuntimeError);
    const sla::LayerProgress cancel=[](size_t done,size_t){return done==0;};
    CHECK_THROWS_AS(sla::layer_palette_loss(document,precision,cancel),sla::LayerOperationCancelled);
    CHECK_THROWS_AS(sla::project_layer_palette_copy(document,precision,cancel),sla::LayerOperationCancelled);
    const auto copy=sla::project_layer_palette_copy(document,precision);
    CHECK(copy.fresh_job()->rows==source.rows);CHECK(copy.fresh_job()->mask_precision==precision);
    CHECK(sla::layer_palette_loss(copy,precision).changed_pixels==0);
    CHECK(document.image(0).buf==original);CHECK(document.revision()==revision);
    CHECK(document.can_undo());CHECK(document.dirty());
    document.undo();CHECK(document.image(0).get(0,0)==0);document.redo();CHECK(document.image(0).buf==original);
    auto config=statistics_profile(portrait);config.set("display_width",16.);
    config.set("sla_archive_format","pwc");config.set("sla_anycubic_version",version);
    const auto print=SLAPrint::create_raster_job(config,document,{.075,.125,.175});
    struct Directory {
        boost::filesystem::path path=boost::filesystem::unique_path("pwc-copy-%%%%");bool keep=false;
        ~Directory(){if(!keep){boost::system::error_code error;boost::filesystem::remove_all(path,error);}}
    } directory;
    if(const char *root=std::getenv("SLA_PWC_PALETTE_FIXTURES")) {
        directory.path=boost::filesystem::path(root)/(std::string(version)+(portrait?"-portrait":"-landscape"));directory.keep=true;
    }
    boost::filesystem::create_directories(directory.path);
    auto verify=[&](const boost::filesystem::path &path) {
        std::ifstream input(path.string(),std::ios::binary);const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)),{});
        const AnycubicLayerLayout layout(bytes,"pwc");
        for(size_t i=0;i<3;++i)CHECK(layout.decode(i)==copy.image(i).buf);
    };
    auto output=directory.path/"export-copy.pwc";
    print->export_layer_document(copy,"pwc",output.string());verify(output);
    CHECK(print->layer_document()->image(0).buf==original);
    const auto copied=SLAPrint::create_raster_job(config,copy,{.075,.125,.175});
    output=directory.path/"copied-job.pwc";copied->export_print(output.string());verify(output);
    sla::ProjectRasterState original_state{document,{.075,.125,.175},"",{},sla::ProjectLayerOrigin::GeneratedRaster};
    sla::save_layer_project((directory.path/"source.sla-project").string(),original_state);
    sla::ProjectRasterState state{copy,{.075,.125,.175},"",{},sla::ProjectLayerOrigin::GeneratedRaster};
    sla::save_layer_project((directory.path/"copy.sla-project").string(),state);
    const auto restored=sla::read_project_layers((directory.path/"copy.sla-project").string());REQUIRE(restored);
    CHECK(restored->document.fresh_job()->mask_precision==precision);
    CHECK(restored->document.fresh_job()->rows==source.rows);
    const auto resumed=SLAPrint::create_raster_job(config,restored->document,{.075,.125,.175});
    output=directory.path/"restarted.pwc";resumed->export_print(output.string());verify(output);
}

TEST_CASE("SL1 projects a canonical unrounded transition without accepting arbitrary row overrides", "[sla_sl1_transition_projection]")
{
    auto source=statistics_intent();source.globals.bottom[F::Exposure]=20.;source.globals.normal[F::Exposure]=2.5;
    source.globals.transition_layers=2;source.rows.clear();
    for(size_t i=0;i<6;++i) {
        auto row=i?source.globals.normal:source.globals.bottom;row[F::PositionZ]=double(i+1)*.05;
        if(i>=1 && i<=2)row[F::Exposure]=20.+(2.5-20.)*double(i)/3.;
        source.rows.push_back(row);
    }
    std::vector<uint8_t> pixels(32,255);const auto mask=sla::PNGRasterEncoder{}(pixels.data(),8,4,1);
    const auto document=sla::LayerDocument::from_fresh_rasters(std::vector<sla::EncodedRaster>(6,mask),source);
    const auto plan=sla::sl1_fresh_export_plan(document);
    CHECK(plan.rows[1].at(F::Exposure)==double(float(14.17)));
    CHECK(plan.rows[2].at(F::Exposure)==double(float(8.34)));
    CHECK(document.fresh_job()->rows==source.rows);
    source.rows[2][F::Exposure]+=.0001;
    const auto invalid=sla::LayerDocument::from_fresh_rasters(std::vector<sla::EncodedRaster>(6,mask),source);
    CHECK_THROWS_AS(sla::sl1_fresh_export_plan(invalid),RuntimeError);
}

TEST_CASE("UVJ native range failures preserve existing files and source intentions", "[sla_uvj_fresh_negative]")
{
    const int defect=GENERATE(0,1,2,3,4);auto source=statistics_intent();
    if(defect==0)source.rows[1][F::Exposure]=1e50;
    if(defect==1)source.rows[1][F::WaitBefore]=1e-50;
    if(defect==2)for(size_t i=0;i<3;++i)source.rows[i][F::PositionZ]=double(i+1)*.0001;
    if(defect==3)source.rows[1][F::RetractDistance2]=std::nextafter(3.,0.);
    if(defect==4){source.rows[1][F::LiftDistance]=2e38;source.rows[1][F::LiftDistance2]=2e38;}
    const auto document=sla::LayerDocument::from_fresh_rasters(statistics_masks(),source);
    auto config=statistics_profile();config.set("display_width",16.);config.set("sla_archive_format","uvj");
    std::vector<double> z;for(const auto &row:source.rows)z.push_back(row.at(F::PositionZ));
    const auto print=SLAPrint::create_raster_job(config,document,z);
    struct File {
        boost::filesystem::path path=boost::filesystem::unique_path("uvj-range-%%%%.uvj");
        ~File(){boost::system::error_code error;boost::filesystem::remove(path,error);}
    } file;
    {std::ofstream(file.path.string(),std::ios::binary)<<"previous print";}
    CHECK_THROWS_AS(print->export_print(file.path.string()),RuntimeError);
    CHECK_THROWS_AS(print->export_layer_document(document,"uvj",file.path.string()),RuntimeError);
    std::ifstream input(file.path.string(),std::ios::binary);
    const std::string contents{std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>()};
    CHECK(contents=="previous print");CHECK(print->layer_document()->fresh_job()->rows==source.rows);
}

TEST_CASE("Owned UVJ exports preserve all row fields and exact masks across restart", "[sla_uvj_fresh]")
{
    const bool portrait=GENERATE(false,true);
    auto source=statistics_intent();if(portrait)source.geometry={4,8,4.,16.};
    source.rows[1][F::Exposure]=7.123456789;source.rows[1][F::LiftDistance]=3.25;
    source.rows[1][F::LightPWM]=180;source.rows[2][F::PositionZ]=.2;
    const auto document=sla::LayerDocument::from_fresh_rasters(statistics_masks(portrait),source);
    auto config=statistics_profile(portrait);config.set("display_width",16.);config.set("sla_archive_format","uvj");
    const std::vector<double> z{.075,.125,.2};
    const auto print=SLAPrint::create_raster_job(config,document,z);
    struct Directory {
        boost::filesystem::path path=boost::filesystem::unique_path("uvj-owned-%%%%");bool keep=false;
        ~Directory(){if(!keep){boost::system::error_code error;boost::filesystem::remove_all(path,error);}}
    } directory;
    if(const char *root=std::getenv("SLA_UVJ_FRESH_FIXTURES")) {
        directory.path=boost::filesystem::path(root)/(portrait?"portrait":"landscape");directory.keep=true;
    }
    boost::filesystem::create_directories(directory.path);
    auto verify=[&](const boost::filesystem::path &path) {
        const auto read=sla::LayerDocument::load_archive(path.string());REQUIRE(read.layer_count()==3);
        for(size_t i=0;i<3;++i)CHECK(read.image(i).buf==document.image(i).buf);
    };
    auto output=directory.path/"unapplied.uvj";
    print->export_layer_document(document,"uvj",output.string());verify(output);
    output=directory.path/"applied.uvj";print->export_print(output.string());verify(output);
    CHECK(print->layer_document()->fresh_job()->rows==source.rows);
    sla::ProjectRasterState state{document,z,"",{},sla::ProjectLayerOrigin::GeneratedRaster};
    sla::save_layer_project((directory.path/"fresh.sla-project").string(),state);
    const auto restored=sla::read_project_layers((directory.path/"fresh.sla-project").string());REQUIRE(restored);
    CHECK(restored->document.fresh_job()->rows==source.rows);
    const auto resumed=SLAPrint::create_raster_job(config,restored->document,z);
    output=directory.path/"restarted.uvj";resumed->export_print(output.string());verify(output);
}

TEST_CASE("CBDDLP owned export preserves rows masks and material after project restart", "[sla_cbddlp_fresh_export]")
{
    const unsigned aa=GENERATE(1u,3u,4u,5u,10u,16u);
    const auto format=GENERATE("cbddlp","photon");const bool portrait=GENERATE(false,true);
    CAPTURE(aa,format,portrait);
    auto source=statistics_intent();if(portrait)source.geometry={4,8,4.,16.};
    source.mask_precision=sla::bitplane_mask_precision(aa);
    for(auto *g:{&source.globals.normal,&source.globals.bottom}) {
        (*g)[F::LiftDistance2]=0;(*g)[F::RetractDistance2]=0;
        (*g)[F::WaitAfter]=0;(*g)[F::WaitAfterLift]=0;(*g)[F::LightOff]=0;(*g)[F::RetractSpeed]=120;
    }
    source.globals.bottom[F::LiftDistance]=2;
    for(size_t i=0;i<3;++i) {
        source.rows[i]=i?source.globals.normal:source.globals.bottom;
        source.rows[i][F::PositionZ]=std::vector<double>{.075,.125,.175}[i];
    }
    source.rows[1][F::Exposure]=7.12;source.rows[2][F::LightOff]=5.;
    std::vector<uint8_t> pixels(32);for(size_t i=0;i<32;++i)pixels[i]=uint8_t(i*8);
    const auto mask=sla::PNGRasterEncoder{}(pixels.data(),portrait?4:8,portrait?8:4,1);
    auto document=sla::LayerDocument::from_generated_rasters({mask,mask,mask},source);
    auto config=statistics_profile(portrait);config.set("display_width",16.);config.set("sla_archive_format",format);
    config.set("sla_cbddlp_aa",int(aa));config.set("exposure_time",9.);config.set("initial_exposure_time",19.);
    const auto print=SLAPrint::create_raster_job(config,document,{.075,.125,.175});
    auto edited=document;edited.paint(0,0,0,0,255);
    struct Directory {
        boost::filesystem::path path=boost::filesystem::unique_path("cbd-fresh-%%%%");bool keep=false;
        ~Directory(){if(!keep){boost::system::error_code error;boost::filesystem::remove_all(path,error);}}
    } directory;
    if(const char *root=std::getenv("SLA_CBD_FRESH_FIXTURES")) {
        directory.path=boost::filesystem::path(root)/(std::string(format)+"-"+std::to_string(aa)+(portrait?"-portrait":"-landscape"));directory.keep=true;
    }
    boost::filesystem::create_directories(directory.path);
    auto verify=[&](const boost::filesystem::path &path) {
        const auto native=sla::LayerDocument::load_archive(path.string());REQUIRE(native.layer_count()==3);
        for(size_t i=0;i<3;++i)CHECK(native.image(i).buf==edited.image(i).buf);
        std::ifstream input(path.string(),std::ios::binary);const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)),{});
        const sla::ChituboxLayerLayout layout(bytes);CHECK(layout.planes==aa);CHECK(layout.number(32)==float(.05));
        CHECK(layout.number(36)==3);CHECK(layout.number(40)==2);CHECK(layout.integer(48)==1);
        for(unsigned plane=0;plane<aa;++plane)for(size_t i=0;i<3;++i) {
            const auto at=layout.record(i,plane);
            CHECK(layout.number(at)==float(source.rows[i].at(F::PositionZ)));
            CHECK(layout.number(at+4)==float(i==1?7.12:i==0?2.:3.));
            CHECK(layout.number(at+8)==float(i==0?3.25:i==1?1.75:5.25));
        }
    };
    auto path=directory.path/(std::string("unapplied.")+format);
    print->export_layer_document(edited,format,path.string());verify(path);
    CHECK(print->layer_document()->image(0).buf==document.image(0).buf);
    print->replace_layer_document(edited);path=directory.path/(std::string("applied.")+format);print->export_print(path.string());verify(path);
    sla::ProjectRasterState state{edited,{.075,.125,.175},"",{},sla::ProjectLayerOrigin::GeneratedRaster};
    const auto project=directory.path/"fresh.sla-project";sla::save_layer_project(project.string(),state);
    const auto restored=sla::read_project_layers(project.string());REQUIRE(restored);
    const auto resumed=SLAPrint::create_raster_job(config,restored->document,{.075,.125,.175});
    path=directory.path/(std::string("restarted.")+format);resumed->export_print(path.string());verify(path);
    CHECK(resumed->layer_document()->fresh_job()->rows==source.rows);
}

TEST_CASE("CXDLPv4 printer identity is rejected before palette analysis", "[sla_export_printer_preflight]") {
    const auto model=GENERATE(std::string{},std::string(256,'A'),std::string("bad\0model",9));
    const int export_route=GENERATE(0,1,2);
    CAPTURE(model.size(),export_route);
    auto config=statistics_profile();config.set("sla_archive_format","cxdlpv4");config.set("printer_model",model);
    const auto document=sla::LayerDocument::from_fresh_rasters(statistics_masks(),statistics_intent());
    const auto print=SLAPrint::create_raster_job(config,document,{.075,.125,.175});
    const auto output=boost::filesystem::temp_directory_path()/boost::filesystem::unique_path("printer-preflight-%%%%.cxdlpv4");
    {std::ofstream sentinel(output.string());sentinel<<"previous valid output";}
    std::string error;
    try {
        if(export_route==0)print->export_print(output.string());
        else if(export_route==1)print->export_layer_document(document,"cxdlpv4",output.string());
        else SLAPrint::convert_layer_archive(config,document,"cxdlpv4",output.string());
    } catch(const std::exception &failure) {error=failure.what();}
    CHECK(error.find("printer model identifier")!=std::string::npos);
    std::ifstream input(output.string());const std::string retained((std::istreambuf_iterator<char>(input)),{});
    CHECK(retained=="previous valid output");
    CHECK(print->layer_document()->fresh_job()->rows==document.fresh_job()->rows);
    CHECK(print->layer_document()->revision()==document.revision());
    boost::filesystem::remove(output);
}

TEST_CASE("CXDLPv4 identifier bounds count protocol bytes and retain legacy allowance", "[sla_export_printer_preflight]") {
    CHECK(sla::cxdlpv4_printer_identifier_error("",false).empty());
    CHECK_FALSE(sla::cxdlpv4_printer_identifier_error("",true).empty());
    CHECK(sla::cxdlpv4_printer_identifier_error("GENERIC",true).empty());
    CHECK(sla::cxdlpv4_printer_identifier_error(std::string(255,'A'),true).empty());
    CHECK_FALSE(sla::cxdlpv4_printer_identifier_error(std::string(256,'A'),true).empty());
    CHECK_FALSE(sla::cxdlpv4_printer_identifier_error(std::string("A\0B",3),false).empty());
    std::string utf8;
    for(size_t i=0;i<127;++i)utf8+="Ł";
    CHECK(sla::cxdlpv4_printer_identifier_error(utf8,true).empty());
    utf8+="Ł";
    CHECK_FALSE(sla::cxdlpv4_printer_identifier_error(utf8,true).empty());
}

namespace {
bool validated_statistics_requested()
{
    const char *flag = std::getenv("SLA_EXPERIMENT_VALIDATED_MASK_STATS");
    if (flag) return std::string(flag) == "1";
#if defined(__SSE2__)
    return true;
#else
    return false;
#endif
}
void check_validated_count(const sla::LayerDocument &document, size_t layer,
                           size_t columns, size_t rows, size_t count)
{
    const auto statistics = document.validated_mask_statistics(layer);
    REQUIRE(bool(statistics) == validated_statistics_requested());
    if (statistics) {
        CHECK(statistics->columns == columns);
        CHECK(statistics->rows == rows);
        CHECK(statistics->nonzero_pixels == count);
    }
}
}

TEST_CASE("Validated mask statistics count canonical pixels after projection", "[sla_validated_mask_statistics]")
{
    using P = sla::LayerMaskPrecision;
    const unsigned palette = GENERATE(0u,1u,2u,3u,4u,5u,6u,7u,8u,9u,10u,11u,12u,13u,14u,15u,16u,17u);
    const auto precision = palette == 0 ? P::NativeGray7 : palette == 1 ? P::NativeGray4 : sla::bitplane_mask_precision(palette-1);
    const size_t literal_counts[] = {254,240,1,129,172,193,206,215,221,225,229,232,234,236,238,239,240,241};
    const size_t expected = literal_counts[palette];
    auto intent = statistics_intent(); intent.mask_precision = precision; intent.geometry = {256,1,25.6,.1};
    std::vector<uint8_t> pixels(256);
    for (size_t i=0;i<pixels.size();++i) pixels[i]=uint8_t(i);
    const auto png = sla::PNGRasterEncoder{}(pixels.data(),256,1,1);
    const auto document = sla::LayerDocument::from_generated_rasters({png,png,png},intent);
    for (size_t layer=0;layer<3;++layer) check_validated_count(document,layer,256,1,expected);
    CHECK_FALSE(document.dirty()); CHECK_FALSE(document.can_undo());
    CHECK_THROWS(document.validated_mask_statistics(3));
    const auto neutral = sla::LayerDocument::from_rasters({png,png,png});
    CHECK_FALSE(neutral.validated_mask_statistics(0));
    intent.mask_precision = P::Gray8;
    const auto gray8 = sla::LayerDocument::from_fresh_rasters({png,png,png},intent);
    CHECK_FALSE(gray8.validated_mask_statistics(0));
    const auto canonical = document.encoded_layer(0);
    auto copy_png = sla::EncodedRaster(std::vector<uint8_t>(static_cast<const uint8_t*>(canonical.buf),
        static_cast<const uint8_t*>(canonical.buf)+canonical.sz),"png");
    intent.mask_precision = precision;
    const auto fresh = sla::LayerDocument::from_fresh_rasters({copy_png,copy_png,copy_png},intent);
    check_validated_count(fresh,1,256,1,expected);
    if (precision == P::NativeGray4 || precision == P::NativeBitplanes1) {
        const auto independent = sla::LayerDocument::from_precision_rasters({copy_png},precision);
        check_validated_count(independent,0,256,1,expected);
    }
}

TEST_CASE("Mask edits invalidate derived counts while snapshots and metadata remain independent", "[sla_validated_mask_statistics]")
{
    auto intent = statistics_intent(); intent.mask_precision = sla::LayerMaskPrecision::NativeGray7;
    auto document = sla::LayerDocument::from_generated_rasters(statistics_masks(),intent);
    check_validated_count(document,0,8,4,1); check_validated_count(document,1,8,4,2); check_validated_count(document,2,8,4,0);
    auto snapshot = document.snapshot();
    document.paint(1,0,0,0,255);
    CHECK_FALSE(document.validated_mask_statistics(1));
    check_validated_count(document,0,8,4,1); check_validated_count(snapshot,1,8,4,2);
    const auto config = statistics_profile();
    auto job = SLAPrint::create_raster_job(config,document,{.075,.125,.175});
    CHECK(job->print_statistics().layers_areas == std::vector<double>{2.,6.,0.});
    CHECK(job->print_statistics().objects_used_material == Catch::Approx(.45));
    REQUIRE(document.undo()); CHECK_FALSE(document.validated_mask_statistics(1));
    job->replace_layer_document(document);
    CHECK(job->print_statistics().layers_areas == std::vector<double>{2.,4.,0.});
    CHECK(job->print_statistics().objects_used_material == Catch::Approx(.35));
    REQUIRE(document.redo()); CHECK_FALSE(document.validated_mask_statistics(1));
    job->replace_layer_document(document);
    CHECK(job->print_statistics().objects_used_material == Catch::Approx(.45));
    snapshot.set_layer_parameters(1,{{F::Exposure,7.}});
    check_validated_count(snapshot,1,8,4,2);
    const auto changed = SLAPrint::create_raster_job(config,snapshot,{.075,.125,.175});
    CHECK(changed->print_statistics().objects_used_material == Catch::Approx(.35));
    CHECK(changed->print_statistics().estimated_print_time == 29.);
    snapshot.paint(0,0,0,0,255);
    CHECK_FALSE(snapshot.validated_mask_statistics(0));
    check_validated_count(document,0,8,4,1);
}

TEST_CASE("Validated counts are recomputed physically and are not authoritative project data", "[sla_validated_mask_statistics]")
{
    auto intent = statistics_intent(); intent.mask_precision = sla::LayerMaskPrecision::NativeGray7;
    const auto document = sla::LayerDocument::from_generated_rasters(statistics_masks(),intent);
    auto config = statistics_profile(); config.set("material_density",2.); config.set("bottle_cost",200.);
    const auto job = SLAPrint::create_raster_job(config,document,{.075,.125,.175});
    CHECK(job->print_statistics().objects_used_material == Catch::Approx(.35));
    CHECK(job->print_statistics().total_weight == Catch::Approx(.0007));
    CHECK(job->print_statistics().total_cost == Catch::Approx(.00014));
    struct Files {
        boost::filesystem::path path=boost::filesystem::temp_directory_path()/boost::filesystem::unique_path("validated-stats-%%%%.sla-project");
        ~Files(){boost::system::error_code error;boost::filesystem::remove(path,error);}
    } files;
    sla::save_layer_project(files.path.string(),{document,{.075,.125,.175},"",{},sla::ProjectLayerOrigin::GeneratedRaster});
    const auto restored = sla::read_project_layers(files.path.string()); REQUIRE(restored);
    for (size_t layer=0;layer<3;++layer) {
        CHECK_FALSE(restored->document.validated_mask_statistics(layer));
        CHECK(restored->document.image(layer).buf == document.image(layer).buf);
    }
    const auto reopened=SLAPrint::create_raster_job(config,restored->document,restored->positions_z);
    CHECK(reopened->print_statistics().layers_areas == job->print_statistics().layers_areas);
    CHECK(reopened->print_statistics().objects_used_material == job->print_statistics().objects_used_material);
    CHECK(reopened->print_statistics().estimated_print_time == job->print_statistics().estimated_print_time);
    CHECK(reopened->print_statistics().total_weight == job->print_statistics().total_weight);
    CHECK(reopened->print_statistics().total_cost == job->print_statistics().total_cost);
    intent.geometry.width_mm *= 2;
    const auto wider = sla::LayerDocument::from_generated_rasters(statistics_masks(),intent);
    check_validated_count(wider,1,8,4,2);
    const auto physical = SLAPrint::create_raster_job(config,wider,{.075,.125,.175});
    CHECK(physical->print_statistics().objects_used_material == Catch::Approx(.7));
}

TEST_CASE("Validated pixel counts cannot replace repeated exposure mask unions", "[sla_validated_mask_statistics]")
{
    const bool overlap = GENERATE(false,true);
    auto intent = statistics_intent(); intent.mask_precision = sla::LayerMaskPrecision::NativeGray7;
    intent.position_policy = sla::ExposurePositionPolicy::RepeatedExposureHeights;
    intent.rows[0][F::PositionZ]=.1; intent.rows[1][F::PositionZ]=.1; intent.rows[2][F::PositionZ]=.2;
    std::vector<sla::EncodedRaster> masks;
    for (size_t record=0;record<3;++record) {
        std::vector<uint8_t> pixels(32,0);
        pixels[record==1 && !overlap ? 1 : 0]=255;
        masks.push_back(sla::PNGRasterEncoder{}(pixels.data(),8,4,1));
    }
    const auto document=sla::LayerDocument::from_fresh_rasters(std::move(masks),intent);
    for (size_t record=0;record<3;++record) check_validated_count(document,record,8,4,1);
    const auto job=SLAPrint::create_raster_job(statistics_profile(),document,{.1,.1,.2},
        std::nullopt,sla::ExposurePositionPolicy::RepeatedExposureHeights);
    CHECK(job->print_statistics().layers_areas == std::vector<double>{2.,2.,2.});
    CHECK(job->print_statistics().objects_used_material == Catch::Approx(overlap ? .4 : .6));
    CHECK(job->print_statistics().estimated_print_time == 25.);
}

TEST_CASE("Derived mask statistics preserve CRC palette dimension and cancellation checks", "[sla_validated_mask_statistics]")
{
    auto intent=statistics_intent(); intent.mask_precision=sla::LayerMaskPrecision::NativeGray7;
    std::vector<uint8_t> pixels(32,255);
    const auto good=sla::PNGRasterEncoder{}(pixels.data(),8,4,1);
    CHECK_THROWS(sla::LayerDocument::from_fresh_rasters(statistics_masks(),intent)); // Noncanonical value1.
    const auto wrong_size=sla::PNGRasterEncoder{}(pixels.data(),4,8,1);
    CHECK_THROWS(sla::LayerDocument::from_fresh_rasters({wrong_size,wrong_size,wrong_size},intent));
    const auto *bytes=static_cast<const uint8_t*>(good.data());
    std::vector<uint8_t> damaged(bytes,bytes+good.size()); damaged.back()^=1; // IEND CRC, after all pixels.
    const auto bad=sla::EncodedRaster(std::move(damaged),"png");
    CHECK_THROWS(sla::LayerDocument::from_fresh_rasters({good,good,bad},intent));
    size_t callbacks=0;
    const auto count=[&](size_t,size_t){++callbacks;return true;};
    const auto clean=sla::LayerDocument::from_fresh_rasters({good,good,good},intent,count);
    REQUIRE(callbacks>5);
    for (size_t stop : {size_t(1), callbacks/2, callbacks}) {
        size_t seen=0;
        CHECK_THROWS_AS(sla::LayerDocument::from_fresh_rasters({good,good,good},intent,
            [&](size_t,size_t){return ++seen<stop;}),sla::LayerOperationCancelled);
    }
    check_validated_count(clean,2,8,4,32);
    CHECK_FALSE(clean.dirty()); CHECK_FALSE(clean.can_undo());
}

TEST_CASE("Fused mask scan preserves palettes counts tails and unaligned chunks", "[sla_validated_mask_statistics]")
{
    for(int id=-1;id<=19;++id) {
        CAPTURE(id);
        const auto precision=sla::LayerMaskPrecision(id);
        bool allowed[256]{};
        if(id>=2 && id<=17) {
            allowed[0]=true;const unsigned planes=unsigned(id-1);
            for(unsigned plane=1;plane<=planes;++plane) allowed[plane*(256/planes)-1]=true;
        } else for(unsigned value=0;value<256;++value)
            allowed[value]=id==1 ? value==0 || (value>=3 && value%2==1) : id==18 ? value%17==0 : true;
        const auto empty=sla::validate_and_count_mask_chunk(nullptr,0,precision);
        CHECK(empty.valid); CHECK(empty.nonzero_pixels==0);
        std::vector<uint8_t> palette;
        for(unsigned value=0;value<256;++value) {
            const uint8_t byte=uint8_t(value);
            const auto result=sla::validate_and_count_mask_chunk(&byte,1,precision);
            CHECK(result.valid==allowed[value]);
            if(allowed[value]) {CHECK(result.nonzero_pixels==size_t(value!=0));palette.push_back(byte);}
        }
        for(size_t alignment : {size_t(0),size_t(1),size_t(7),size_t(15)})
            for(size_t size : {size_t(1),size_t(15),size_t(16),size_t(17),size_t(31),size_t(32),size_t(33),size_t(255),(size_t(1)<<20)+17}) {
                CAPTURE(alignment,size);
                std::vector<uint8_t> bytes(alignment+size);
                for(size_t i=0;i<size;++i) bytes[alignment+i]=palette[i%palette.size()];
                const size_t expected=size-size/palette.size()-(size%palette.size()!=0);
                const auto result=sla::validate_and_count_mask_chunk(bytes.data()+alignment,size,precision);
                CHECK(result.valid);CHECK(result.nonzero_pixels==expected);
                for(unsigned value=0;value<256;++value) if(!allowed[value]) {
                    bytes.back()=uint8_t(value);
                    CHECK_FALSE(sla::validate_and_count_mask_chunk(bytes.data()+alignment,size,precision).valid);
                    break;
                }
            }
    }
}
