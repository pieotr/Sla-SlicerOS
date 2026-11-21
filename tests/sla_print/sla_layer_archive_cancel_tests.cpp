#include "libslic3r/SLA/ExportGeneration.hpp"
#include "libslic3r/SLA/ExportSnapshot.hpp"
#include "libslic3r/SLA/ExportRequest.hpp"
#include "libslic3r/SLA/SlicedResultSession.hpp"
#include "libslic3r/SLA/ProjectArchive.hpp"
#include "libslic3r/SLA/LayerPolygonComposition.hpp"
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/core.hpp>
#include <boost/log/expressions.hpp>
#include <boost/log/sinks/sync_frontend.hpp>
#include <boost/log/sinks/text_ostream_backend.hpp>
#include <boost/make_shared.hpp>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <map>
#include <algorithm>
#include <boost/algorithm/string/trim.hpp>
#include <future>
#include <thread>
#include <mutex>
#include <chrono>
#include <cfenv>
#include <cstring>
#include <cmath>
#include <nlohmann/json.hpp>
#include "libslic3r/FileReader.hpp"
#include "libslic3r/BuildVolume.hpp"
#include "libslic3r/MultipleBeds.hpp"
#include "sla_archive_test_config.hpp"
#include "libslic3r/SLA/LayerDocument.hpp"
#include "libslic3r/SLA/ExportPublication.hpp"
#include "libslic3r/SLAPrint.hpp"
#include "libslic3r/Format/SLAArchiveFormatRegistry.hpp"
#include "libslic3r/Format/SLAArchiveWriter.hpp"
#include "libslic3r/Format/ZipperArchiveImport.hpp"
#include "libslic3r/Format/ChituboxLayerLayout.hpp"
#include "libslic3r/SLA/CrealityCXDLPv4LayerParameters.hpp"
#include "libslic3r/SLA/CrealityCXDLPv4JobReader.hpp"
#include "libslic3r/Zipper.hpp"
#include "libslic3r/miniz_extension.hpp"
using namespace Slic3r;
namespace {
std::vector<uint8_t> read_file(const boost::filesystem::path &path) {
    std::ifstream input(path.string(),std::ios::binary);return {std::istreambuf_iterator<char>(input),{}};
}

// Explicit opt-in benchmark: reads the supplied model, never persists its mesh.
// It exercises the same snapshot/request implementation used by the GUI queue.
TEST_CASE("Measure a real model through live and frozen export", "[.][sla_frozen_model_benchmark]") {
    const char *input=std::getenv("SLA_EXPORT_BENCH_MODEL");
    const char *settings=std::getenv("SLA_EXPORT_BENCH_CONFIG");
    const char *destination=std::getenv("SLA_EXPORT_BENCH_OUTPUT");
    REQUIRE(input);REQUIRE(settings);REQUIRE(destination);
    const boost::filesystem::path folder(destination);boost::filesystem::create_directories(folder);
    REQUIRE_FALSE(boost::filesystem::exists(folder/"report.json"));
    auto bytes_before=read_file(input);
    const auto began=std::chrono::steady_clock::now();
    set_logging_level(4);
    const auto seconds=[&] {return std::chrono::duration<double>(std::chrono::steady_clock::now()-began).count();};
    DynamicPrintConfig embedded;ConfigSubstitutionContext substitutions(ForwardCompatibilitySubstitutionRule::Enable);
    boost::optional<Semver> version;
    // Authored OBJ fixtures have geometry only. Keep the existing 3MF path,
    // including its embedded configuration, unchanged for model benchmarks.
    auto model=boost::filesystem::path(input).extension()==".obj" ?
        FileReader::load_model(input) :
        FileReader::load_model_with_config(input,&embedded,&substitutions,version,FileReader::LoadAttribute::AddDefaultInstances);
    if (std::getenv("SLA_EXPORT_BENCH_MESH_ASSERTIONS")) {
        REQUIRE(model.objects.size()==1);
        const Vec3d input_size=model.bounding_box_exact().size();
        CHECK(input_size.x()==3.);CHECK(input_size.y()==5.);CHECK(input_size.z()==7.);
    }
    DynamicPrintConfig config;config.apply(SLAFullPrintConfig{});config.apply(embedded);
    config.load(settings,ForwardCompatibilitySubstitutionRule::Disable);
    config.option("printer_technology")->setInt(ptSLA);
    model.add_default_instances();model.center_instances_around_point(Vec2d(99.07,61.92));
    BoundingBoxf3 bounds;
    for(const auto *object:model.objects)bounds.merge(object->instance_bounding_box(0,false));
    for(auto *object:model.objects) {
        for(auto *instance:object->instances)instance->set_offset(Z,instance->get_offset(Z)-bounds.min.z());
        // The original performance fixture is explicitly scaled to the bed.
        // Tiny authored regression fixtures instead retain their declared mm.
        const char *keep_scale=std::getenv("SLA_EXPORT_BENCH_TEST_KEEP_SCALE");
        if (keep_scale==nullptr || std::strcmp(keep_scale,"1")!=0)
            object->scale_to_fit(Vec3d(194.,120.,200.));
        // Match the CLI's final transform pass after scaling as well as its
        // initial centering; otherwise support/pad geometry is a different job.
        object->ensure_on_bed();
    }
    if (std::getenv("SLA_EXPORT_BENCH_MESH_ASSERTIONS")) {
        const Vec3d fixture_size=model.bounding_box_exact().size();
        CHECK(fixture_size.x()==3.);CHECK(fixture_size.y()==5.);CHECK(fixture_size.z()==7.);
    }
    // Opt-in test transform applied AFTER benchmark bed normalization.
    // Own fixture only: a lifted horizontal bottom genuinely needs supports.
    double input_lift_mm=0.;
    if(const char *requested=std::getenv("SLA_EXPORT_BENCH_TEST_LIFT_MM")) {
        size_t consumed=0;input_lift_mm=std::stod(requested,&consumed);
        REQUIRE(consumed==std::strlen(requested));
        REQUIRE(std::isfinite(input_lift_mm));REQUIRE(input_lift_mm>=0.);
        for(auto *object:model.objects)
            for(auto *instance:object->instances)
                instance->set_offset(Z,instance->get_offset(Z)+input_lift_mm);
    }
    const auto bed=config.option<ConfigOptionPoints>("bed_shape")->values;
    BoundingBoxf bed_bounds;for(const auto &point:bed)bed_bounds.merge(point);
    s_multiple_beds.update_build_volume(bed_bounds);
    model.update_print_volume_state(BuildVolume(bed,config.opt_float("max_print_height")));
    SLAPrint source;MultipleBedsUtils::with_single_bed_model_sla(model,0,[&] {source.apply(model,config);});
    REQUIRE(source.validate().empty());REQUIRE_FALSE(source.empty());
    source.full_print_config().save((folder/"effective.ini").string());
    nlohmann::json report{{"source",input},{"settings",settings},{"fit_mm",{194.,120.,200.}},
        {"center_mm",{99.07,61.92}},{"setup_seconds",seconds()}};
    const double slice_begin=seconds();source.process();report["slicing_seconds"]=seconds()-slice_begin;
    REQUIRE(source.finished());REQUIRE(source.layer_document());
    if(std::getenv("SLA_EXPORT_BENCH_MESH_ASSERTIONS")) {
        REQUIRE_FALSE(source.objects().empty());
        auto expected_flag=[](const char *name) {
            const char *value=std::getenv(name);REQUIRE(value);
            const std::string literal(value);REQUIRE((literal=="0" || literal=="1"));
            return literal=="1";
        };
        // Independent literal request: never infer the expected behavior from
        // the same object configuration whose import/application is tested.
        const bool expected_supports=expected_flag("SLA_EXPORT_BENCH_EXPECT_SUPPORTS");
        const bool expected_pad=expected_flag("SLA_EXPORT_BENCH_EXPECT_PAD");
        const char *height=std::getenv("SLA_EXPORT_BENCH_EXPECT_SUPPORT_ELEVATION_MM");REQUIRE(height);
        size_t height_consumed=0;const double expected_elevation=std::stod(height,&height_consumed);
        REQUIRE(height_consumed==std::strlen(height));
        REQUIRE(std::isfinite(expected_elevation));REQUIRE(expected_elevation>=0.);
        report["workflow_request"]={{"supports",expected_supports},{"pad",expected_pad},
            {"support_object_elevation_mm",expected_elevation},{"pad_around_object",false}};
        nlohmann::json mesh_evidence=nlohmann::json::array();
        auto mesh_facts=[&](const TriangleMesh &mesh,bool expected,const char *kind) {
            CAPTURE(kind,expected);
            if(!expected) {
                CHECK(mesh.its.vertices.empty());CHECK(mesh.its.indices.empty());
                return nlohmann::json{{"requested",false},{"vertices",mesh.its.vertices.size()},
                    {"triangles",mesh.its.indices.size()}};
            }
            REQUIRE_FALSE(mesh.its.vertices.empty());REQUIRE_FALSE(mesh.its.indices.empty());
            for(const auto &vertex:mesh.its.vertices)REQUIRE(vertex.allFinite());
            for(const auto &triangle:mesh.its.indices) {
                for(int corner=0;corner<3;++corner) {
                    REQUIRE(triangle[corner]>=0);
                    REQUIRE(size_t(triangle[corner])<mesh.its.vertices.size());
                }
                CHECK(triangle[0]!=triangle[1]);CHECK(triangle[1]!=triangle[2]);CHECK(triangle[0]!=triangle[2]);
            }
            const auto bounds=mesh.bounding_box();const auto extent=bounds.size();
            CHECK(extent.x()>0.);CHECK(extent.y()>0.);CHECK(extent.z()>0.);
            const double volume=std::abs(double(its_volume(mesh.its)));
            CHECK(std::isfinite(volume));CHECK(volume>0.);
            return nlohmann::json{{"requested",true},{"vertices",mesh.its.vertices.size()},
                {"triangles",mesh.its.indices.size()},{"absolute_signed_volume_mm3",volume},
                {"extent_mm",{extent.x(),extent.y(),extent.z()}}};
        };
        for(const auto *object:source.objects()) {
            const bool supports=object->config().supports_enable.getBool();
            const bool pad=object->config().pad_enable.getBool();
            CAPTURE(supports,pad,expected_supports,expected_pad);
            REQUIRE(supports==expected_supports);REQUIRE(pad==expected_pad);
            REQUIRE(object->config().support_object_elevation.getFloat()==expected_elevation);
            // Embedding pads intentionally suppress object elevation; this
            // fixture explicitly requests an ordinary elevated support pad.
            REQUIRE_FALSE(object->config().pad_around_object.getBool());
            if(expected_supports) {
                REQUIRE(input_lift_mm>0.); // This fixture must be explicitly above bed.
                REQUIRE(object->is_step_done(slaposSupportTree));
                CHECK_FALSE(object->get_support_points().empty());
            } else CHECK(object->get_support_points().empty());
            if(expected_pad)REQUIRE(object->is_step_done(slaposPad));
            mesh_evidence.push_back({{"supports",mesh_facts(object->support_mesh(),expected_supports,"supports")},
                                     {"pad",mesh_facts(object->pad_mesh(),expected_pad,"pad")}});
        }
        report["input_test_lift_mm"]=input_lift_mm;
        report["generated_meshes"]=std::move(mesh_evidence);
    }
    if (std::getenv("SLA_EXPORT_BENCH_SUPPORTS")) {
        // Test-only exact snapshot of generated supports, never model geometry.
        // Preserve sequence and float32 bits, including radius (whose normal
        // SupportPoint equality intentionally has a tolerance).
        const auto bits=[](float value) {
            uint32_t result;
            static_assert(sizeof(result)==sizeof(value));
            std::memcpy(&result,&value,sizeof(result));
            return result;
        };
        nlohmann::json objects=nlohmann::json::array();
        for (const auto *object:source.objects()) {
            nlohmann::json points=nlohmann::json::array();
            for (const auto &point:object->get_support_points())
                points.push_back({{"position_mm",{point.pos.x(),point.pos.y(),point.pos.z()}},
                    {"position_f32_bits",{bits(point.pos.x()),bits(point.pos.y()),bits(point.pos.z())}},
                    {"radius_mm",point.head_front_radius},{"radius_f32_bits",bits(point.head_front_radius)},
                    {"type",static_cast<int>(point.type)}});
            objects.push_back(std::move(points));
        }
        report["support_points"]=std::move(objects);
    }
    if (std::getenv("SLA_EXPORT_BENCH_EDITOR_SESSION")) {
        const double capture_begin=seconds();
        auto session=sla::SlicedResultSession::capture(source);
        report["editor_capture_seconds"]=seconds()-capture_begin;
        const auto output=(folder/("session."+std::string(get_default_extension(
            source.printer_config().sla_archive_format.value.c_str())))).string();
        const double export_begin=seconds();
        session->context().export_layer_document(session->document(),
            source.printer_config().sla_archive_format.value,output);
        report["editor_export_seconds"]=seconds()-export_begin;
        report["editor_file"]=output;
        CHECK(session->source_current());
        CHECK(session->configuration()==source.full_print_config());
        CHECK(session->context().is_raster_job()==source.is_raster_job());
    }
    if(const char *layers=std::getenv("SLA_EXPORT_BENCH_SAVE_LAYERS"))
        sla::save_layer_project(layers,sla::capture_project_layers(model,config,source));
    const auto format=source.printer_config().sla_archive_format.value;
    const std::string suffix=get_default_extension(format.c_str());
    const auto live=(folder/("live."+suffix)).string();
    const double live_begin=seconds();source.export_print(live);report["live_export_seconds"]=seconds()-live_begin;
    const double capture_begin=seconds();
    auto snapshot=std::make_shared<const sla::ExportSnapshot>(source.capture_export_snapshot());
    report["capture_seconds"]=seconds()-capture_begin;
    report["reusable_cxdlpv4_layers"]=snapshot->reusable_cxdlpv4_layer_count();
    report["reusable_cxdlpv4_payload_bytes"]=snapshot->reusable_cxdlpv4_payload_bytes();
    if (const char *expected=std::getenv("SLA_EXPORT_BENCH_EXPECT_ENCODED_REUSE")) {
        const std::string literal(expected);REQUIRE((literal=="0" || literal=="1"));
        CHECK(snapshot->reusable_cxdlpv4_layer_count()==
            (literal=="1" ? source.layer_document()->layer_count() : size_t(0)));
    }
    const sla::ExportRequest request(snapshot,format,(folder/("frozen."+suffix)).string());
    const double write_begin=seconds();request.write();report["frozen_export_seconds"]=seconds()-write_begin;
    report["total_seconds"]=seconds();report["layers"]=source.layer_document()->layer_count();
    report["format"]=format;report["live_file"]=live;report["frozen_file"]=request.path();
    CHECK(bytes_before==read_file(input));CHECK(snapshot->generation().current());
    std::ofstream(folder/"report.json")<<report.dump(2)<<'\n';
}
TEST_CASE("Profile an owned layer-only export fixture", "[.][sla_raster_export_benchmark]") {
    const char *input=std::getenv("SLA_EXPORT_BENCH_LAYERS");
    const char *output=std::getenv("SLA_EXPORT_BENCH_OUTPUT");REQUIRE(input);REQUIRE(output);
    set_logging_level(4);
    const auto state=sla::read_project_layers(input);REQUIRE(state);REQUIRE(state->settings);
    const auto configuration=sla::restore_project_settings(*state->settings);
    auto source=SLAPrint::create_raster_job(configuration,state->document,state->positions_z);
    auto snapshot=std::make_shared<const sla::ExportSnapshot>(source->capture_export_snapshot());
    const auto format=configuration.opt_string("sla_archive_format");
    const sla::ExportRequest request(snapshot,format,output);
    const auto began=std::chrono::steady_clock::now();request.write();
    std::ofstream(std::string(output)+".timing.json")<<nlohmann::json{
        {"seconds",std::chrono::duration<double>(std::chrono::steady_clock::now()-began).count()},
        {"layers",state->document.layer_count()}}.dump(2)<<'\n';
}
struct Directory {
    boost::filesystem::path path=boost::filesystem::unique_path("layer-cancel-%%%%");
    Directory(){boost::filesystem::create_directories(path);}
    ~Directory(){boost::system::error_code e;boost::filesystem::remove_all(path,e);}
};
}

// Optional diagnostic fixtures are derived 2D layers in build/, not a copy
// of the user's 3MF or a prerequisite for ordinary regression tests.
TEST_CASE("Equivalent real SLA contour orders compose identically", "[.][sla_layer_order_fixture]") {
    const char *root=std::getenv("SLA_LAYER_ORDER_FIXTURES");REQUIRE(root);
    const auto load=[](const boost::filesystem::path &path) {
        std::ifstream input(path.string());nlohmann::json data;input>>data;
        const auto polygons=[](const nlohmann::json &source) {
            ExPolygons result;
            const auto ring=[](const nlohmann::json &points) {
                Points result;for(const auto &point:points)result.emplace_back(point[0].get<coord_t>(),point[1].get<coord_t>());
                return Polygon(std::move(result));
            };
            for(const auto &polygon:source) {
                result.emplace_back(ring(polygon["contour"]));
                for(const auto &hole:polygon["holes"])result.back().holes.push_back(ring(hole));
            }
            return result;
        };
        return std::make_pair(polygons(data["models"]),polygons(data["supports"]));
    };
    const auto a=load(boost::filesystem::path(root)/"dump-c/layer142-input.json");
    const auto b=load(boost::filesystem::path(root)/"dump-d/layer142-input.json");
    REQUIRE(std::fesetround(FE_TONEAREST)==0);
    const auto first=sla::compose_layer_polygons(a.first,a.second);
    const auto second=sla::compose_layer_polygons(b.first,b.second);
    const auto points=[](const ExPolygons &polygons) {
        size_t result=0;for(const auto &p:polygons) {result+=p.contour.size();for(const auto &h:p.holes)result+=h.size();}
        return result;
    };
    CHECK(points(first.polygons)==points(second.polygons));
    CHECK(first.polygons==second.polygons);
    CHECK(first.model_area==second.model_area);CHECK(first.support_area==second.support_area);
}
TEST_CASE("SLA publication preserves the target when writing validation or cancellation fails",
          "[sla_export_publication][sla_export_cancel]") {
    const std::string failure=GENERATE("cancel_after_write","write_error","validation_error","empty","missing");
    const bool existing=GENERATE(false,true);
    CAPTURE(failure,existing);Directory dir;
    const auto output=dir.path/"result.alias";
    const std::vector<uint8_t> original={'o','l','d'},complete={'n','e','w'};
    if(existing) {std::ofstream file(output.string(),std::ios::binary);file.write("old",3);}
    bool cancelled=false,writer_called=false;boost::filesystem::path staged;
    auto write=[&](const std::string &temporary) {
        writer_called=true;staged=temporary;
        REQUIRE(staged.parent_path()==output.parent_path());REQUIRE(staged.extension()==".ctb");
        if(failure=="missing")return;
        {std::ofstream file(temporary,std::ios::binary);
            if(failure!="empty")file.write("new",3);}
        if(failure=="write_error")throw FileIOError("injected write failure");
        if(failure=="validation_error")throw RuntimeError("injected readback failure");
        if(failure=="cancel_after_write")cancelled=true;
    };
    auto check=[&] {if(cancelled)throw CanceledException{};};
    if(failure=="cancel_after_write")
        REQUIRE_THROWS_AS(sla::publish_export(output.string(),write,check,"ctb"),CanceledException);
    else if(failure=="validation_error")
        REQUIRE_THROWS_AS(sla::publish_export(output.string(),write,check,"ctb"),RuntimeError);
    else REQUIRE_THROWS_AS(sla::publish_export(output.string(),write,check,"ctb"),FileIOError);
    REQUIRE(writer_called);REQUIRE(!boost::filesystem::exists(staged));
    REQUIRE(boost::filesystem::exists(output)==existing);
    if(existing)REQUIRE(read_file(output)==original);
    REQUIRE(std::distance(boost::filesystem::directory_iterator(dir.path),boost::filesystem::directory_iterator{})==int(existing));
    // Failure leaves no residue preventing a later successful publication.
    sla::publish_export(output.string(),[&](const std::string &temporary) {
        REQUIRE(boost::filesystem::path(temporary).extension()==".alias");
        std::ofstream file(temporary,std::ios::binary);file.write("new",3);
    },[]{});
    REQUIRE(read_file(output)==complete);
    REQUIRE(std::distance(boost::filesystem::directory_iterator(dir.path),boost::filesystem::directory_iterator{})==1);
}

TEST_CASE("SLA publication cleans staging when native rename fails", "[sla_export_publication][sla_export_cancel]") {
    Directory dir;const auto output=dir.path/"result.ctb";
    boost::filesystem::create_directory(output);
    {std::ofstream file((output/"preserved.txt").string());file<<"old";}
    boost::filesystem::path staged;
    REQUIRE_THROWS_AS(sla::publish_export(output.string(),[&](const std::string &temporary) {
        staged=temporary;std::ofstream file(temporary);file<<"new";
    },[]{}),FileIOError);
    REQUIRE(!boost::filesystem::exists(staged));REQUIRE(boost::filesystem::is_directory(output));
    REQUIRE(read_file(output/"preserved.txt")==std::vector<uint8_t>{'o','l','d'});
    REQUIRE(std::distance(boost::filesystem::directory_iterator(dir.path),boost::filesystem::directory_iterator{})==1);
}
TEST_CASE("Cancelled SLA export never replaces an existing target", "[sla_export_cancel][sla_layers]") {
    const std::string format=GENERATE("SL1","ctb","cxdlpv4");
    const bool explicit_document=GENERATE(false,true);
    CAPTURE(format,explicit_document);Directory dir;
    SLAFullPrintConfig full;Test::configure_archive_fixture(full,format);
    DynamicPrintConfig cfg;cfg.apply(full);
    std::vector<uint8_t> pixels(full.display_pixels_x.value*full.display_pixels_y.value,255);
    auto document=sla::LayerDocument::from_rasters({sla::PNGRasterEncoder{}(
        pixels.data(),full.display_pixels_x.value,full.display_pixels_y.value,1)});
    document.invert(0);
    auto job=SLAPrint::create_raster_job(cfg,document,{.05});
    const auto output=dir.path/("existing."+std::string(get_archive_entry(format.c_str())->ext));
    const std::vector<uint8_t> sentinel={'o','l','d'};
    {std::ofstream file(output.string(),std::ios::binary);file.write(reinterpret_cast<const char *>(sentinel.data()),sentinel.size());}
    const auto revision=document.revision();const auto mask=document.image(0).buf;
    job->cancel();
    auto export_job=[&] {
        if(explicit_document)job->export_layer_document(document,format,output.string(),{},"cancel");
        else job->export_print(output.string(),{},"cancel");
    };
    CHECK_THROWS_AS(export_job(),CanceledException);
    CHECK(read_file(output)==sentinel);
    CHECK(document.revision()==revision);CHECK(document.image(0).buf==mask);
    CHECK(document.dirty());CHECK(document.can_undo());
    CHECK(std::distance(boost::filesystem::directory_iterator(dir.path),boost::filesystem::directory_iterator{})==1);
    job->restart();export_job();
    CHECK(read_file(output)!=sentinel);
    CHECK(sla::LayerDocument::load_archive(output.string()).image(0).buf==mask);
}
TEST_CASE("SLA configuration changes stop the background owner before replacing export data",
          "[sla_export_mutation][sla_layers]") {
    const std::string key=GENERATE("printer_model","material_density","bottle_cost","output_filename_format");
    CAPTURE(key);SLAFullPrintConfig full;Test::configure_archive_fixture(full,"SL1");
    DynamicPrintConfig original;original.apply(full);Model model;SLAPrint job;
    job.apply(model,original);
    auto edited=original;
    if(key=="printer_model")edited.set(key,std::string("CL60"));
    else if(key=="output_filename_format")edited.set(key,std::string("new-{print_time}"));
    else edited.set(key,original.opt_float(key)+1.);
    bool stopped=false,saw_original=false;
    job.set_cancel_callback([&] {
        stopped=true;
        saw_original=*job.full_print_config().option(key)==*original.option(key);
    });
    job.apply(model,edited);
    CHECK(stopped);CHECK(saw_original);
    CHECK(*job.full_print_config().option(key)==*edited.option(key));
    // A no-op apply must not free option storage borrowed by an exporter.
    const auto *borrowed=job.full_print_config().option(key);
    stopped=false;job.apply(model,edited);CHECK_FALSE(stopped);
    CHECK(job.full_print_config().option(key)==borrowed);
    job.set_cancel_callback([]{});
}

TEST_CASE("Replacing SLA masks stops the background owner before releasing its document",
          "[sla_export_mutation][sla_layers]") {
    SLAFullPrintConfig full;Test::configure_archive_fixture(full,"SL1");
    DynamicPrintConfig cfg;cfg.apply(full);
    std::vector<uint8_t> pixels(full.display_pixels_x.value*full.display_pixels_y.value,255);
    auto document=sla::LayerDocument::from_rasters({sla::PNGRasterEncoder{}(
        pixels.data(),full.display_pixels_x.value,full.display_pixels_y.value,1)});
    auto job=SLAPrint::create_raster_job(cfg,document,{.05});
    auto edited=document;edited.invert(0);
    bool stopped=false,saw_original=false;
    job->set_cancel_callback([&] {
        stopped=true;saw_original=job->layer_document()->image(0).buf==pixels;
    });
    job->replace_layer_document(edited);
    CHECK(stopped);CHECK(saw_original);
    CHECK(bool(job->layer_document()->image(0).buf==edited.image(0).buf));
    job->set_cancel_callback([] {throw std::logic_error("stop failure");});
    CHECK_THROWS_AS(job->replace_layer_document(document),std::logic_error);
    CHECK(bool(job->layer_document()->image(0).buf==edited.image(0).buf));
    job->set_cancel_callback([]{});
}
TEST_CASE("Applying layer edits requires the captured result identity", "[sla_result_session_apply]") {
    SLAFullPrintConfig full;Test::configure_archive_fixture(full,"SL1");
    DynamicPrintConfig cfg;cfg.apply(full);
    std::vector<uint8_t> pixels(full.display_pixels_x.value*full.display_pixels_y.value,255);
    auto document=sla::LayerDocument::from_rasters({sla::PNGRasterEncoder{}(
        pixels.data(),full.display_pixels_x.value,full.display_pixels_y.value,1)});
    auto job=SLAPrint::create_raster_job(cfg,document,{.05});
    auto other=SLAPrint::create_raster_job(cfg,document,{.05});
    const auto expected=job->export_generation_guard();
    auto edited=document;edited.invert(0);
    const auto action=GENERATE("other source","replaced result","cancelled request","current");
    CAPTURE(action);
    auto guard=expected.with_cancellation();
    if(std::string(action)=="other source") {
        CHECK_THROWS(other->replace_layer_document(edited,guard));
        CHECK(bool(other->layer_document()->image(0).buf==pixels));
    } else if(std::string(action)=="replaced result") {
        // Equal revision, dimensions and pixels do not identify the same result.
        job->replace_layer_document(document);
        CHECK_THROWS(job->replace_layer_document(edited,guard));
        CHECK(bool(job->layer_document()->image(0).buf==pixels));
    } else if(std::string(action)=="cancelled request") {
        guard.cancel();
        CHECK_THROWS(job->replace_layer_document(edited,guard));
        CHECK(bool(job->layer_document()->image(0).buf==pixels));
    } else {
        CHECK_NOTHROW(job->replace_layer_document(edited,guard));
        CHECK(bool(job->layer_document()->image(0).buf==edited.image(0).buf));
        CHECK_FALSE(expected.current());
    }
    CHECK(edited.dirty());CHECK(edited.can_undo());
}

TEST_CASE("Sliced result sessions own configuration and survive their source", "[sla_result_session]") {
    Directory dir;
    SLAFullPrintConfig full;Test::configure_archive_fixture(full,"SL1");
    DynamicPrintConfig cfg;cfg.apply(full);
    cfg.set("exposure_time",2.73);cfg.set("material_density",1.17);
    std::vector<uint8_t> pixels(full.display_pixels_x.value*full.display_pixels_y.value,255);
    auto document=sla::LayerDocument::from_rasters({sla::PNGRasterEncoder{}(
        pixels.data(),full.display_pixels_x.value,full.display_pixels_y.value,1)});
    auto source=SLAPrint::create_raster_job(cfg,document,{.05});
    auto session=sla::SlicedResultSession::capture(*source);
    CHECK(session->source_current());CHECK_FALSE(session->document().dirty());
    const auto accepted=source->full_print_config();
    CHECK(session->configuration()==accepted);
    cfg.set("exposure_time",999.);cfg.set("sla_archive_format",std::string("ctb"));
    source->apply(Model{},cfg);
    CHECK_FALSE(session->source_current());CHECK(session->configuration()==accepted);
    source.reset();
    CHECK_FALSE(session->source_current());CHECK(session->configuration()==accepted);
    session->document().invert(0);
    CHECK(session->document().dirty());CHECK(session->document().can_undo());
    const auto output=dir.path/"session.sl1";
    REQUIRE_NOTHROW(session->context().export_layer_document(session->document(),"SL1",output.string()));
    const auto reopened=sla::LayerDocument::load_archive(output.string());
    CHECK(bool(reopened.image(0).buf==session->document().image(0).buf));
    CHECK(session->document().dirty());CHECK(session->document().can_undo());
    const auto captured=session->context().capture_export_snapshot();
    CHECK(captured.configuration()==accepted);
    CHECK(captured.levels().size()==1);CHECK(captured.levels()[0]==scaled<coord_t>(.05));
}

TEST_CASE("Session Apply advances its source baseline only after success", "[sla_result_session]") {
    SLAFullPrintConfig full;Test::configure_archive_fixture(full,"SL1");
    DynamicPrintConfig cfg;cfg.apply(full);
    std::vector<uint8_t> pixels(full.display_pixels_x.value*full.display_pixels_y.value,255);
    auto document=sla::LayerDocument::from_rasters({sla::PNGRasterEncoder{}(
        pixels.data(),full.display_pixels_x.value,full.display_pixels_y.value,1)});
    auto source=SLAPrint::create_raster_job(cfg,document,{.05});
    auto session=sla::SlicedResultSession::capture(*source);
    session->document().invert(0);
    const auto action=GENERATE("current","stale","other","stop failure");
    CAPTURE(action);
    if(std::string(action)=="current") {
        session->apply(*source);
        CHECK_FALSE(session->document().dirty());CHECK(session->document().can_undo());
        CHECK(session->source_current());
        session->document().undo();
        CHECK(session->document().dirty());
        session->apply(*source);
        CHECK_FALSE(session->document().dirty());CHECK(session->document().can_redo());
        CHECK(session->source_current());
        CHECK(bool(source->layer_document()->image(0).buf==pixels));
    } else {
        std::unique_ptr<SLAPrint> other;
        if(std::string(action)=="stale")source->replace_layer_document(document);
        if(std::string(action)=="stop failure")source->set_cancel_callback([] {throw std::runtime_error("stop failed");});
        if(std::string(action)=="other")other=SLAPrint::create_raster_job(cfg,document,{.05});
        CHECK_THROWS(session->apply(other?*other:*source));
        CHECK(session->document().dirty());CHECK(session->document().can_undo());
        CHECK(bool(source->layer_document()->image(0).buf==pixels));
        source->set_cancel_callback([]{});
    }
}

TEST_CASE("Reopening a result session retains committed Undo and Redo", "[sla_result_session_history]") {
    SLAFullPrintConfig full;Test::configure_archive_fixture(full,"SL1");
    DynamicPrintConfig cfg;cfg.apply(full);
    std::vector<uint8_t> pixels(full.display_pixels_x.value*full.display_pixels_y.value,255);
    auto document=sla::LayerDocument::from_rasters({sla::PNGRasterEncoder{}(
        pixels.data(),full.display_pixels_x.value,full.display_pixels_y.value,1)});
    auto source=SLAPrint::create_raster_job(cfg,document,{.05});
    auto session=sla::SlicedResultSession::capture(*source);
    session->document().invert(0);session->apply(*source);
    auto reopened=sla::SlicedResultSession::capture(*source);
    REQUIRE(reopened->document().can_undo());
    reopened->document().undo();
    CHECK(bool(reopened->document().image(0).buf==pixels));
    CHECK(reopened->document().dirty());CHECK(reopened->document().can_redo());
    reopened->apply(*source);
    auto again=sla::SlicedResultSession::capture(*source);
    REQUIRE(again->document().can_redo());
    again->document().redo();
    CHECK(bool(again->document().image(0).buf==session->document().image(0).buf));
    CHECK(again->document().dirty());
}

TEST_CASE("Session capture cancellation retains the ready source", "[sla_result_session]") {
    SLAFullPrintConfig full;Test::configure_archive_fixture(full,"SL1");
    DynamicPrintConfig cfg;cfg.apply(full);
    std::vector<uint8_t> pixels(full.display_pixels_x.value*full.display_pixels_y.value,255);
    auto document=sla::LayerDocument::from_rasters({sla::PNGRasterEncoder{}(
        pixels.data(),full.display_pixels_x.value,full.display_pixels_y.value,1)});
    auto source=SLAPrint::create_raster_job(cfg,document,{.05});
    const auto generation=source->export_generation();
    const unsigned cancel_at=GENERATE(0u,5u,10u);
    unsigned calls=0;
    CHECK_THROWS_AS(sla::SlicedResultSession::capture(*source,[&](size_t,size_t){return calls++<cancel_at;}),sla::LayerOperationCancelled);
    CHECK(calls>cancel_at);
    CHECK(source->finished());CHECK(source->export_generation()==generation);
    CHECK(bool(source->layer_document()->image(0).buf==pixels));
}

TEST_CASE("SLA mask encoding propagates cancellation from worker checks", "[sla_export_cancel][sla_layers]") {
    SLAFullPrintConfig full;Test::configure_archive_fixture(full,"ctb");
    std::vector<uint8_t> pixels(full.display_pixels_x.value*full.display_pixels_y.value,255);
    auto document=sla::LayerDocument::from_rasters({sla::PNGRasterEncoder{}(
        pixels.data(),full.display_pixels_x.value,full.display_pixels_y.value,1)});
    auto writer=SLAArchiveWriter::create("ctb",full);
    std::atomic<bool> reached_encoding{false};
    // A single unique mask still runs through the TBB arena. The entry and
    // deduplication checks happen before it; cancel on the encoding check.
    std::atomic<size_t> polls{0};
    REQUIRE_THROWS_AS(writer->encode_layers(document,[&] {
        if(++polls==3) {reached_encoding=true;throw CanceledException{};}
    }),CanceledException);
    REQUIRE(reached_encoding);REQUIRE(polls==3);
    REQUIRE_NOTHROW(writer->encode_layers(document));
}
TEST_CASE("Raster job mutation joins its export reader before releasing masks",
          "[sla_export_threaded_mutation][sla_export_mutation]") {
    const std::string action=GENERATE("clear","replace","apply_scene");
    CAPTURE(action);Directory dir;
    SLAFullPrintConfig full;Test::configure_archive_fixture(full,"SL1");
    DynamicPrintConfig cfg;cfg.apply(full);
    std::vector<uint8_t> pixels(full.display_pixels_x.value*full.display_pixels_y.value,255);
    auto document=sla::LayerDocument::from_rasters({sla::PNGRasterEncoder{}(
        pixels.data(),full.display_pixels_x.value,full.display_pixels_y.value,1)});
    auto job=SLAPrint::create_raster_job(cfg,document,{.05});
    const auto output=dir.path/"existing.sl1";
    {std::ofstream file(output.string());file<<"old";}
    std::promise<void> resume;auto gate=resume.get_future();bool released=false;
    auto release=[&] {if(!released){released=true;resume.set_value();}};
    bool saw_original=false,cancelled_export=false,stopped=false;
    std::exception_ptr worker_error;
    std::thread reader([&] {
        gate.wait();
        try {
            const auto *borrowed=job->layer_document();
            saw_original=borrowed && borrowed->image(0).buf==pixels;
            try {job->export_print(output.string());}
            catch(const CanceledException &) {cancelled_export=true;}
        } catch(...) {worker_error=std::current_exception();}
    });
    // Same ownership protocol as BackgroundSlicingProcess: cancel the worker
    // and join it before the mutator is allowed to free borrowed storage.
    job->set_cancel_callback([&] {
        stopped=true;job->cancel();release();if(reader.joinable())reader.join();
    });
    try {
        if(action=="clear")job->clear();
        else if(action=="replace") {auto edited=document;edited.invert(0);job->replace_layer_document(edited);}
        else job->apply(Model{},cfg);
    } catch(...) {
        release();if(reader.joinable())reader.join();job->set_cancel_callback([]{});throw;
    }
    // If no stop was requested, read only after the mutation has returned.
    // This reproduces the missing ordering without dereferencing freed memory.
    release();if(reader.joinable())reader.join();job->set_cancel_callback([]{});
    CHECK(stopped);CHECK(saw_original);CHECK(cancelled_export);CHECK(!worker_error);
    CHECK(bool(read_file(output)==std::vector<uint8_t>{'o','l','d'}));
    CHECK(std::distance(boost::filesystem::directory_iterator(dir.path),boost::filesystem::directory_iterator{})==1);
    if(action=="replace") {
        CHECK(job->layer_document()!=nullptr);
        CHECK(bool(job->layer_document()->image(0).buf!=pixels));
    } else CHECK(job->layer_document()==nullptr);
}
TEST_CASE("Cancelled archive import and save preserve source output dirty state and history", "[sla_archive_cancel][sla_layers]") {
    const std::string format=GENERATE("SL1","uvj","genericzip","vda","cws","cwsxml","cwsrgb","chitubox_zip",
        "ctb","ctb_v1","cbddlp","cbddlp_v1","photon","photon_v1","cxdlpv1","cxdlpv2","cxdlp","cxdlpv4");
    CAPTURE(format);Directory dir;
    SLAFullPrintConfig full;Test::configure_archive_fixture(full,format);
    DynamicPrintConfig cfg;cfg.apply(full);
    const size_t width=full.display_pixels_x.value,height=full.display_pixels_y.value;
    std::vector<uint8_t> pixels(width*height,255);
    auto png=sla::PNGRasterEncoder{}(pixels.data(),width,height,1);
    auto source=sla::LayerDocument::from_rasters({png,png,png});
    auto job=SLAPrint::create_raster_job(cfg,source,{.05,.1,.15});
    const auto path=dir.path/("source."+std::string(get_archive_entry(format.c_str())->ext));
    job->export_layer_document(source,format,path.string(),{},"cancel test");
    const auto original=read_file(path);
    REQUIRE_THROWS_AS(sla::LayerDocument::load_archive(path.string(),[](size_t,size_t){return false;}),sla::LayerOperationCancelled);
    bool during_layers=false;
    REQUIRE_THROWS_AS(sla::LayerDocument::load_archive(path.string(),[&](size_t done,size_t total){
        if(total && done>=1){during_layers=true;return false;}return true;
    }),sla::LayerOperationCancelled);
    REQUIRE(during_layers);REQUIRE(read_file(path)==original);
    auto document=sla::LayerDocument::load_archive(path.string());document.invert(0);
    const auto revision=document.revision(),count=document.layer_count();
    REQUIRE(document.dirty());REQUIRE(document.can_undo());
    const auto mask=document.image(0).buf;
    const auto output=dir.path/("existing."+std::string(get_archive_entry(document.archive_format().c_str())->ext));
    const std::vector<uint8_t> sentinel={'p','r','e','v','i','o','u','s'};
    {std::ofstream file(output.string(),std::ios::binary);file.write(reinterpret_cast<const char *>(sentinel.data()),sentinel.size());}
    REQUIRE_THROWS_AS(document.save_archive(output.string(),[](size_t,size_t){return false;}),sla::LayerOperationCancelled);
    bool stopped=false;
    REQUIRE_THROWS_AS(document.save_archive(output.string(),[&](size_t done,size_t total){
        if(total && done>=1){stopped=true;return false;}return true;
    }),sla::LayerOperationCancelled);
    REQUIRE(stopped);REQUIRE(read_file(output)==sentinel);
    REQUIRE(document.revision()==revision);REQUIRE(document.layer_count()==count);
    REQUIRE(document.dirty());REQUIRE(document.image(0).buf==mask);REQUIRE(document.can_undo());
    for(const auto &entry:boost::filesystem::directory_iterator(dir.path))REQUIRE(entry.path().extension()!=".tmp");
    // The last callback is before atomic publication, including the unchanged
    // binary copy path. A refusal must still preserve the old target.
    REQUIRE_THROWS_AS(document.save_archive(output.string(),[](size_t done,size_t total){return !total || done<total;}),sla::LayerOperationCancelled);
    REQUIRE(read_file(output)==sentinel);REQUIRE(document.dirty());
    document.save_archive(output.string());REQUIRE(!document.dirty());
    REQUIRE(sla::LayerDocument::load_archive(output.string()).image(0).buf==mask);
    REQUIRE(document.undo());REQUIRE(document.dirty());
}
TEST_CASE("ZIP extraction cancellation and callback errors unwind outside miniz", "[sla_archive_cancel][sla_layers]") {
    Directory dir;const auto path=dir.path/"large.sl1";
    std::vector<uint8_t> bytes(8*1024*1024,7);
    {Zipper zip(path.string());zip.add_entry("large-unknown-data.bin",bytes.data(),bytes.size());zip.finalize();}
    size_t polls=0;
    REQUIRE_THROWS_AS(sla::LayerDocument::load_archive(path.string(),[&](size_t,size_t total){return !total || ++polls<2;}),sla::LayerOperationCancelled);
    REQUIRE(polls==2);
    polls=0;
    REQUIRE_THROWS_AS(sla::LayerDocument::load_archive(path.string(),[&](size_t,size_t total){
        if(total && ++polls==2)throw std::logic_error("callback failure");return true;
    }),std::logic_error);
    REQUIRE(polls==2);REQUIRE(boost::filesystem::exists(path));
}
TEST_CASE("Chitubox decoder observes cancellation inside a long raster run", "[sla_archive_cancel][sla_reference]") {
    Directory dir;SLAFullPrintConfig full;Test::configure_archive_fixture(full,"ctb");
    full.set("display_pixels_x",4096);full.set("display_pixels_y",4096);DynamicPrintConfig cfg;cfg.apply(full);
    std::vector<uint8_t> white(4096*4096,255);
    auto document=sla::LayerDocument::from_rasters({sla::PNGRasterEncoder{}(white.data(),4096,4096,1)});
    auto job=SLAPrint::create_raster_job(cfg,document,{.05});const auto path=dir.path/"run.ctb";
    job->export_layer_document(document,"ctb",path.string(),{},"run");
    const auto bytes=read_file(path);const sla::ChituboxLayerLayout layout(bytes);size_t polls=0;
    REQUIRE_THROWS_AS(layout.decode(0,[&]{if(++polls==4)throw sla::LayerOperationCancelled{};}),sla::LayerOperationCancelled);
    REQUIRE(polls==4);REQUIRE(read_file(path)==bytes);
}

TEST_CASE("Generate large addressed archive for real GUI cancellation", "[.sla_archive_cancel_gui]") {
    const auto *folder=std::getenv("SLA_ARCHIVE_CANCEL_GUI_OUTPUT");REQUIRE(folder!=nullptr);
    boost::filesystem::create_directories(folder);
    SLAFullPrintConfig full;Test::configure_archive_fixture(full,"ctb");
    full.set("display_pixels_x",4096);full.set("display_pixels_y",4096);DynamicPrintConfig cfg;cfg.apply(full);
    std::vector<uint8_t> white(4096*4096,255);const auto png=sla::PNGRasterEncoder{}(white.data(),4096,4096,1);
    for(size_t count:{300u,1200u}) {
        std::vector<sla::EncodedRaster> layers(count,png);auto document=sla::LayerDocument::from_rasters(std::move(layers));
        std::vector<double> positions(count);for(size_t i=0;i<count;++i)positions[i]=.05*(i+1);
        auto job=SLAPrint::create_raster_job(cfg,document,positions);
        job->export_layer_document(document,"ctb",(boost::filesystem::path(folder)/(std::to_string(count)+".ctb")).string(),{},"cancel fixture");
    }
}

TEST_CASE("Export generations distinguish source replacements and metadata-only mutations", "[sla_export_generation]") {
    const std::string action=GENERATE("replace_equal_revision","price","printer_identity","clear","model_transform");
    CAPTURE(action);
    SLAFullPrintConfig full;Test::configure_archive_fixture(full,"SL1");
    DynamicPrintConfig config;config.apply(full);
    config.set("supports_enable",false);config.set("pad_enable",false);config.set("support_object_elevation",0.);
    config.set("layer_height",.05);config.set("initial_layer_height",.05);
    Model model;auto *object=model.add_object("generation cube","",make_cube(1.,1.,.1));
    object->add_instance()->set_offset(Vec3d(1.,1.,0.));
    auto job=std::make_unique<SLAPrint>();job->apply(model,config);job->process();
    REQUIRE(job->layer_document());config=job->full_print_config();
    const auto before=job->export_generation();
    const auto guard=job->export_generation_guard();
    job->apply(model,config);
    CHECK(job->export_generation()==before);
    REQUIRE(job->layer_document());
    const auto revision=job->layer_document()->revision();
    if(action=="replace_equal_revision") {
        const auto &source=*job->layer_document();const auto image=source.image(0);
        std::vector<uint8_t> pixels(image.cols*image.rows,0);
        const auto black=sla::PNGRasterEncoder{}(pixels.data(),image.cols,image.rows,1);
        const auto other=sla::LayerDocument::from_fresh_rasters(
            std::vector<sla::EncodedRaster>(source.layer_count(),black),*source.fresh_job());
        REQUIRE(other.revision()==revision);
        job->replace_layer_document(other);
        CHECK(job->layer_document()->revision()==revision);
    } else if(action=="clear") job->clear();
    else if(action=="model_transform") {
        object->instances.front()->set_offset(Vec3d(2.,1.,0.));
        job->apply(model,config);
        CHECK_FALSE(job->layer_document());
    } else {
        if(action=="price")config.set("bottle_cost",config.opt_float("bottle_cost")+1.);
        else config.set("printer_model","Different printer");
        job->apply(model,config);
        REQUIRE(job->layer_document());CHECK(job->layer_document()->revision()==revision);
    }
    CHECK(job->export_generation()>before);
    CHECK_FALSE(guard.current());CHECK_THROWS_AS(guard.check(),CanceledException);
}

TEST_CASE("Generation guards survive source destruction and distinguish independent jobs", "[sla_export_generation]") {
    SLAFullPrintConfig full;Test::configure_archive_fixture(full,"SL1");DynamicPrintConfig config;config.apply(full);
    std::vector<uint8_t> pixels(full.display_pixels_x.value*full.display_pixels_y.value,255);
    const auto document=sla::LayerDocument::from_rasters({sla::PNGRasterEncoder{}(
        pixels.data(),full.display_pixels_x.value,full.display_pixels_y.value,1)});
    auto first=SLAPrint::create_raster_job(config,document,{.05});
    auto second=SLAPrint::create_raster_job(config,document,{.05});
    CHECK(first->layer_document()->revision()==second->layer_document()->revision());
    CHECK(first->export_generation()!=second->export_generation());
    CHECK_THROWS_AS(sla::ExportGenerationGuard(nullptr),RuntimeError);
    const auto guard=first->export_generation_guard(), other=second->export_generation_guard();
    auto transferred_from=first->export_generation_guard();auto transferred=std::move(transferred_from);
    CHECK(transferred.current());CHECK_FALSE(transferred_from.current());
    CHECK_THROWS_AS(transferred_from.check(),CanceledException);
    REQUIRE_NOTHROW(guard.check());first.reset();
    CHECK_FALSE(guard.current());CHECK_THROWS_AS(guard.check(),CanceledException);
    CHECK(other.current());REQUIRE_NOTHROW(other.check());
}
TEST_CASE("Stale generation before publication preserves the previous target", "[sla_export_generation][sla_export_publication]") {
    const bool destroy=GENERATE(false,true);Directory directory;
    SLAFullPrintConfig full;Test::configure_archive_fixture(full,"SL1");DynamicPrintConfig config;config.apply(full);
    std::vector<uint8_t> pixels(full.display_pixels_x.value*full.display_pixels_y.value,255);
    const auto document=sla::LayerDocument::from_rasters({sla::PNGRasterEncoder{}(
        pixels.data(),full.display_pixels_x.value,full.display_pixels_y.value,1)});
    auto source=SLAPrint::create_raster_job(config,document,{.05});const auto guard=source->export_generation_guard();
    const auto target=directory.path/"result.sl1";{std::ofstream file(target.string());file<<"old";}
    boost::filesystem::path staged;
    REQUIRE_THROWS_AS(sla::publish_export(target.string(),[&](const std::string &temporary) {
        staged=temporary;{std::ofstream file(temporary);file<<"new";}
        if(destroy)source.reset();else source->clear();
    },[&]{guard.check();}),CanceledException);
    CHECK(read_file(target)==std::vector<uint8_t>{'o','l','d'});CHECK_FALSE(boost::filesystem::exists(staged));
    CHECK(std::distance(boost::filesystem::directory_iterator(directory.path),boost::filesystem::directory_iterator{})==1);
}

TEST_CASE("Source change after final export check cannot replace the previous target", "[sla_export_commit]") {
    const bool destroy = GENERATE(false, true);
    Directory directory;
    SLAFullPrintConfig full; Test::configure_archive_fixture(full, "SL1");
    DynamicPrintConfig config; config.apply(full);
    std::vector<uint8_t> pixels(full.display_pixels_x.value * full.display_pixels_y.value, 255);
    const auto document = sla::LayerDocument::from_rasters({sla::PNGRasterEncoder{}(
        pixels.data(), full.display_pixels_x.value, full.display_pixels_y.value, 1)});
    auto source = SLAPrint::create_raster_job(config, document, {.05});
    const auto guard = source->export_generation_guard();
    const auto target = directory.path / "result.sl1";
    { std::ofstream file(target.string()); file << "old"; }
    size_t checks = 0;
    boost::filesystem::path staged;
    // Deterministically inject the same interleaving as a source mutation
    // arriving after the final current() observation and before rename.
    const auto check = [&] {
        guard.check();
        if (++checks == 2) {
            if (destroy) source.reset(); else source->clear();
        }
    };
    CHECK_THROWS_AS(sla::publish_export(target.string(), [&](const std::string &temporary) {
        staged = temporary;
        std::ofstream file(temporary); file << "new";
    }, check, "", &guard), CanceledException);
    CHECK_FALSE(guard.current());
    CHECK(read_file(target) == std::vector<uint8_t>{'o', 'l', 'd'});
    CHECK_FALSE(boost::filesystem::exists(staged));
    CHECK(std::distance(boost::filesystem::directory_iterator(directory.path),
                        boost::filesystem::directory_iterator{}) == 1);
}

TEST_CASE("Publication waits for invalidation and rejects a changed source", "[sla_export_commit]") {
    const bool invalidate = GENERATE(false, true);
    Directory directory;
    auto state = std::make_shared<sla::ExportGenerationState>();
    const sla::ExportGenerationGuard guard(state);
    const auto target = directory.path / "result.sl1";
    { std::ofstream file(target.string()); file << "old"; }
    std::promise<void> staged_ready;
    auto ready = staged_ready.get_future();
    std::future<void> worker;
    boost::filesystem::path staged;
    {
        // Hold an invalidation transaction open while the real publisher
        // finishes staging. Publication cannot complete inside this interval.
        std::unique_lock<std::mutex> invalidation(state->commit_mutex);
        worker = std::async(std::launch::async, [&] {
            sla::publish_export(target.string(), [&](const std::string &temporary) {
                staged = temporary;
                { std::ofstream file(temporary); file << "new"; }
                staged_ready.set_value();
            }, [] {}, "", &guard);
        });
        CHECK(ready.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
        CHECK(worker.wait_for(std::chrono::milliseconds(50)) == std::future_status::timeout);
        if (invalidate) state->generation.fetch_add(1, std::memory_order_release);
    }
    if (invalidate) {
        CHECK_THROWS_AS(worker.get(), CanceledException);
        CHECK(read_file(target) == std::vector<uint8_t>{'o', 'l', 'd'});
    } else {
        CHECK_NOTHROW(worker.get());
        CHECK(read_file(target) == std::vector<uint8_t>{'n', 'e', 'w'});
    }
    CHECK_FALSE(boost::filesystem::exists(staged));
    CHECK(std::distance(boost::filesystem::directory_iterator(directory.path),
                        boost::filesystem::directory_iterator{}) == 1);
}

TEST_CASE("Calibration provenance changes invalidate export requests", "[sla_export_recipe_generation]") {
    Directory directory;
    SLAFullPrintConfig full; Test::configure_archive_fixture(full, "SL1");
    DynamicPrintConfig config; config.apply(full);
    std::vector<uint8_t> pixels(full.display_pixels_x.value * full.display_pixels_y.value, 255);
    const auto document = sla::LayerDocument::from_rasters({sla::PNGRasterEncoder{}(
        pixels.data(), full.display_pixels_x.value, full.display_pixels_y.value, 1)});
    auto source = SLAPrint::create_raster_job(config, document, {.05});
    const auto before = source->export_generation_guard();
    source->set_calibration_recipe(nullptr);
    CHECK(before.current());
    auto recipe = std::make_shared<sla::CalibrationRecipe>();
    source->set_calibration_recipe(recipe);
    CHECK_FALSE(before.current());
    REQUIRE(source->calibration_recipe());
    // Failed input must preserve both current provenance and valid requests.
    const auto current = source->export_generation_guard();
    const auto original = source->calibration_recipe();
    auto bad = std::make_shared<sla::CalibrationRecipe>(*recipe);
    bad->parameters.pattern = static_cast<sla::CalibrationPattern>(99);
    CHECK_THROWS(source->set_calibration_recipe(bad));
    CHECK(current.current());
    CHECK(source->calibration_recipe() == original);
    source->set_calibration_recipe(nullptr);
    CHECK_FALSE(current.current());
    CHECK_FALSE(source->calibration_recipe());
}

TEST_CASE("Guarded publication cleans staging when final rename fails", "[sla_export_commit]") {
    Directory directory;
    auto state = std::make_shared<sla::ExportGenerationState>();
    const sla::ExportGenerationGuard guard(state);
    const auto target = directory.path / "blocked.sl1";
    boost::filesystem::create_directory(target);
    const auto child = target / "previous";
    { std::ofstream file(child.string()); file << "old"; }
    boost::filesystem::path staged;
    CHECK_THROWS_AS(sla::publish_export(target.string(), [&](const std::string &temporary) {
        staged = temporary;
        std::ofstream file(temporary); file << "new";
    }, [] {}, "", &guard), FileIOError);
    CHECK(read_file(child) == std::vector<uint8_t>{'o', 'l', 'd'});
    CHECK_FALSE(boost::filesystem::exists(staged));
    CHECK(guard.current());
}

TEST_CASE("Ready model export snapshot owns masks settings metadata and previews", "[sla_engine_snapshot]") {
    SLAFullPrintConfig full; Test::configure_archive_fixture(full, "SL1");
    DynamicPrintConfig config; config.apply(full);
    config.set("supports_enable", false); config.set("pad_enable", false);
    config.set("support_object_elevation", 0.); config.set("bottle_cost", 37.);
    config.set("printer_model", "Snapshot printer");
    Model model; auto *object = model.add_object("snapshot cube", "", make_cube(1.,1.,.1));
    object->add_instance()->set_offset(Vec3d(1.,1.,0.));
    auto source = std::make_unique<SLAPrint>(); source->apply(model, config); source->process();
    REQUIRE(source->layer_document()); REQUIRE(source->layer_document()->layer_count() >= 2);
    auto edited = *source->layer_document(); edited.invert(0); edited.invert(1); REQUIRE(edited.undo());
    source->replace_layer_document(edited);
    REQUIRE(source->layer_document()->can_undo()); REQUIRE(source->layer_document()->can_redo());
    ThumbnailsList thumbnails(1); thumbnails[0].set(2,2); thumbnails[0].pixels[0] = 11;
    const auto snapshot = source->capture_export_snapshot(thumbnails);
    const bool same_config = snapshot.configuration() == source->full_print_config(); CHECK(same_config);
    CHECK(snapshot.objects().size() == source->objects().size());
    CHECK(snapshot.thumbnails().size() == 1);
    CHECK(snapshot.levels().size() == source->print_layers().size());
    REQUIRE(snapshot.document());
    CHECK(snapshot.generation().current()); CHECK_FALSE(snapshot.is_raster_job());
    CHECK(snapshot.vector_layers().empty());
    CHECK_FALSE(snapshot.document()->can_undo()); CHECK_FALSE(snapshot.document()->can_redo());
    CHECK(snapshot.document()->revision() == edited.revision());
    CHECK(snapshot.document()->fresh_job()->rows == edited.fresh_job()->rows);
    for (size_t i = 0; i < edited.layer_count(); ++i) {
        CHECK(snapshot.document()->image(i).buf == edited.image(i).buf);
        CHECK(snapshot.levels()[i] == source->print_layers()[i].level());
    }
    REQUIRE(snapshot.objects().size() == 1);
    CHECK(snapshot.objects()[0].name == "snapshot cube");
    CHECK(snapshot.objects()[0].configuration.hollowing_enable.value == source->objects()[0]->config().hollowing_enable.value);
    CHECK(snapshot.statistics().estimated_print_time == source->print_statistics().estimated_print_time);
    CHECK(snapshot.statistics().layers_areas == source->print_statistics().layers_areas);
    thumbnails[0].pixels[0] = 99;
    CHECK(snapshot.thumbnails()[0].pixels[0] == 11);
    const auto pixels = snapshot.document()->image(0).buf;
    auto changed = source->full_print_config(); changed.set("bottle_cost", 82.); source->apply(model,changed);
    CHECK_FALSE(snapshot.generation().current());
    CHECK(snapshot.configuration().opt_float("bottle_cost") == 37.);
    source.reset();
    CHECK(snapshot.document()->image(0).buf == pixels);
    CHECK(snapshot.objects()[0].name == "snapshot cube");
    CHECK_THROWS_AS(snapshot.generation().check(), CanceledException);
}

TEST_CASE("Cancelled engine capture preserves source masks history and generation", "[sla_engine_snapshot]") {
    SLAFullPrintConfig full; Test::configure_archive_fixture(full, "SL1");
    DynamicPrintConfig config; config.apply(full);
    std::vector<uint8_t> pixels(full.display_pixels_x.value * full.display_pixels_y.value, 255);
    auto document = sla::LayerDocument::from_rasters({sla::PNGRasterEncoder{}(
        pixels.data(),full.display_pixels_x.value,full.display_pixels_y.value,1)});
    document.invert(0);
    auto source = SLAPrint::create_raster_job(config,document,{.05});
    const auto generation = source->export_generation();
    CHECK_THROWS_AS(source->capture_export_snapshot({}, [](size_t,size_t) { return false; }), sla::LayerOperationCancelled);
    CHECK(source->export_generation() == generation);
    CHECK(source->layer_document()->can_undo());
    CHECK(source->layer_document()->image(0).buf == document.image(0).buf);
}

TEST_CASE("Vector export snapshot retains contours after source destruction", "[sla_engine_snapshot]") {
    SLAFullPrintConfig full; Test::configure_archive_fixture(full,"SL1SVG");
    DynamicPrintConfig config; config.apply(full);
    config.set("supports_enable",false); config.set("pad_enable",false); config.set("support_object_elevation",0.);
    Model model; auto *object = model.add_object("vector cube","",make_cube(1.,1.,.1));
    object->add_instance()->set_offset(Vec3d(1.,1.,0.));
    auto source = std::make_unique<SLAPrint>(); source->apply(model,config); source->process();
    REQUIRE(source->finished()); REQUIRE_FALSE(source->layer_document());
    const auto snapshot = source->capture_export_snapshot();
    CHECK_FALSE(snapshot.document()); CHECK_FALSE(snapshot.is_raster_job());
    REQUIRE(snapshot.vector_layers().size() == source->print_layers().size());
    REQUIRE_FALSE(snapshot.vector_layers().empty());
    for(size_t i=0;i<snapshot.vector_layers().size();++i) {
        CHECK(snapshot.vector_layers()[i].level == source->print_layers()[i].level());
        const bool same = snapshot.vector_layers()[i].contours == source->print_layers()[i].transformed_slices();
        CHECK(same);
    }
    const auto contours = snapshot.vector_layers()[0].contours;
    source.reset();
    const bool same = snapshot.vector_layers()[0].contours == contours;
    CHECK(same); CHECK_FALSE(snapshot.generation().current());
}

TEST_CASE("Engine snapshot keeps immutable calibration provenance", "[sla_engine_snapshot]") {
    SLAFullPrintConfig full; Test::configure_archive_fixture(full,"SL1");
    DynamicPrintConfig config; config.apply(full);
    std::vector<uint8_t> pixels(full.display_pixels_x.value*full.display_pixels_y.value,255);
    const auto document = sla::LayerDocument::from_rasters({sla::PNGRasterEncoder{}(
        pixels.data(),full.display_pixels_x.value,full.display_pixels_y.value,1)});
    auto source = SLAPrint::create_raster_job(config,document,{.05});
    auto recipe = std::make_shared<sla::CalibrationRecipe>();
    source->set_calibration_recipe(recipe);
    const auto snapshot = source->capture_export_snapshot();
    CHECK(snapshot.is_raster_job()); REQUIRE(snapshot.calibration_recipe());
    const auto old_width = snapshot.calibration_recipe()->parameters.size_x;
    recipe->parameters.size_x = 42.; source->set_calibration_recipe(recipe);
    CHECK(snapshot.calibration_recipe()->parameters.size_x == old_width);
    CHECK_FALSE(snapshot.generation().current());
    source.reset(); CHECK(snapshot.calibration_recipe()->parameters.size_x == old_width);
}

TEST_CASE("Unfinished SLA results cannot be captured", "[sla_engine_snapshot]") {
    SLAPrint source;
    CHECK_THROWS_AS(source.capture_export_snapshot(),ExportError);
}

TEST_CASE("Engine capture serializes concurrent source mutation", "[sla_engine_snapshot]") {
    SLAFullPrintConfig full; Test::configure_archive_fixture(full,"SL1");
    DynamicPrintConfig config; config.apply(full);
    std::vector<uint8_t> pixels(full.display_pixels_x.value*full.display_pixels_y.value,255);
    const auto document=sla::LayerDocument::from_rasters({sla::PNGRasterEncoder{}(
        pixels.data(),full.display_pixels_x.value,full.display_pixels_y.value,1)});
    auto source=SLAPrint::create_raster_job(config,document,{.05});
    auto before=std::make_shared<sla::CalibrationRecipe>();source->set_calibration_recipe(before);
    auto after=std::make_shared<sla::CalibrationRecipe>(*before);after->parameters.size_x=42.;
    std::promise<void> entered, release;
    auto entered_future=entered.get_future();auto release_future=release.get_future().share();
    std::atomic<bool> paused{false};
    auto capture=std::async(std::launch::async,[&] {
        return source->capture_export_snapshot({},[&](size_t,size_t) {
            if (!paused.exchange(true)) { entered.set_value(); release_future.wait(); }
            return true;
        });
    });
    CHECK(entered_future.wait_for(std::chrono::seconds(5))==std::future_status::ready);
    auto mutation=std::async(std::launch::async,[&] {source->set_calibration_recipe(after);});
    CHECK(mutation.wait_for(std::chrono::milliseconds(50))==std::future_status::timeout);
    release.set_value();
    const auto snapshot=capture.get();mutation.get();
    REQUIRE(snapshot.calibration_recipe());
    CHECK(snapshot.calibration_recipe()->parameters.size_x==before->parameters.size_x);
    CHECK(source->calibration_recipe()->parameters.size_x==42.);
    CHECK_FALSE(snapshot.generation().current());
}

TEST_CASE("Frozen request exports captured masks and keeps source untouched", "[sla_frozen_request]") {
    Directory directory;
    SLAFullPrintConfig full; Test::configure_archive_fixture(full,"SL1");
    DynamicPrintConfig config;config.apply(full);config.set("printer_model","Frozen printer");
    std::vector<uint8_t> pixels(full.display_pixels_x.value*full.display_pixels_y.value,0);
    pixels[3]=127;pixels[9]=255;
    auto document=sla::LayerDocument::from_rasters({sla::PNGRasterEncoder{}(
        pixels.data(),full.display_pixels_x.value,full.display_pixels_y.value,1)});
    document.invert(0);
    auto source=SLAPrint::create_raster_job(config,document,{.05});
    auto snapshot=std::make_shared<const sla::ExportSnapshot>(source->capture_export_snapshot());
    std::string format="SL1",path=(directory.path/"frozen.sl1").string(),name="Captured project";
    const sla::ExportRequest request(snapshot,format,path,name);
    format="cxdlpv4";name="Changed project";
    CHECK(request.format()=="SL1");CHECK(request.projectname()=="Captured project");
    const auto generation=source->export_generation(),revision=source->layer_document()->revision();
    request.write();
    const auto loaded=sla::LayerDocument::load_sl1(path);
    CHECK(loaded.image(0).buf==document.image(0).buf);
    CHECK(source->export_generation()==generation);CHECK(source->layer_document()->revision()==revision);
    CHECK(source->layer_document()->can_undo());CHECK(snapshot->generation().current());
}

TEST_CASE("Cancelled stale and destroyed requests preserve the destination", "[sla_frozen_request]") {
    const std::string action=GENERATE("cancel","clear","destroy");
    Directory directory;
    SLAFullPrintConfig full;Test::configure_archive_fixture(full,"SL1");
    DynamicPrintConfig config;config.apply(full);
    std::vector<uint8_t> pixels(full.display_pixels_x.value*full.display_pixels_y.value,255);
    const auto document=sla::LayerDocument::from_rasters({sla::PNGRasterEncoder{}(
        pixels.data(),full.display_pixels_x.value,full.display_pixels_y.value,1)});
    auto source=SLAPrint::create_raster_job(config,document,{.05});
    auto snapshot=std::make_shared<const sla::ExportSnapshot>(source->capture_export_snapshot());
    const auto target=directory.path/"old.sl1";{std::ofstream file(target.string());file<<"old";}
    const sla::ExportRequest request(snapshot,"SL1",target.string());
    if(action=="cancel") {
        const auto copy=request;copy.cancel();CHECK(request.cancelled());CHECK(snapshot->generation().current());
    } else if(action=="clear")source->clear();else source.reset();
    CHECK_THROWS_AS(request.write(),CanceledException);
    CHECK(read_file(target)==std::vector<uint8_t>{'o','l','d'});
    CHECK(std::distance(boost::filesystem::directory_iterator(directory.path),boost::filesystem::directory_iterator{})==1);
}

TEST_CASE("Frozen requests keep independent cancellation on shared captured data", "[sla_frozen_request]") {
    Directory directory;
    SLAFullPrintConfig full;Test::configure_archive_fixture(full,"SL1");DynamicPrintConfig config;config.apply(full);
    std::vector<uint8_t> pixels(full.display_pixels_x.value*full.display_pixels_y.value,255);
    const auto document=sla::LayerDocument::from_rasters({sla::PNGRasterEncoder{}(
        pixels.data(),full.display_pixels_x.value,full.display_pixels_y.value,1)});
    auto source=SLAPrint::create_raster_job(config,document,{.05});
    auto snapshot=std::make_shared<const sla::ExportSnapshot>(source->capture_export_snapshot());
    const sla::ExportRequest first(snapshot,"SL1",(directory.path/"first.sl1").string());
    const sla::ExportRequest second(snapshot,"SL1",(directory.path/"second.sl1").string());
    first.cancel();CHECK(first.cancelled());CHECK_FALSE(second.cancelled());
    CHECK_THROWS_AS(first.write(),CanceledException);second.write();
    CHECK_FALSE(boost::filesystem::exists(first.path()));CHECK(boost::filesystem::exists(second.path()));
    CHECK(snapshot->generation().current());
}

TEST_CASE("Frozen request palette copy requires approval and preserves original masks", "[sla_frozen_request]") {
    Directory directory;
    SLAFullPrintConfig full;Test::configure_archive_fixture(full,"SL1");DynamicPrintConfig config;config.apply(full);
    config.set("printer_model","GENERIC");
    std::vector<uint8_t> pixels(full.display_pixels_x.value*full.display_pixels_y.value,0);
    pixels[1]=1;pixels[3]=127;pixels[4]=128;pixels[5]=254;pixels[6]=255;
    const auto document=sla::LayerDocument::from_rasters({sla::PNGRasterEncoder{}(
        pixels.data(),full.display_pixels_x.value,full.display_pixels_y.value,1)});
    auto source=SLAPrint::create_raster_job(config,document,{.05},sla::FreshLayerProducer::Raster);
    auto snapshot=std::make_shared<const sla::ExportSnapshot>(source->capture_export_snapshot());
    const auto target=directory.path/"copy.cxdlpv4";{std::ofstream file(target.string());file<<"old";}
    const sla::ExportRequest declined(snapshot,"cxdlpv4",target.string());
    CHECK_THROWS_AS(declined.write(),ExportError);
    CHECK(read_file(target)==std::vector<uint8_t>{'o','l','d'});
    const sla::ExportRequest accepted(snapshot,"cxdlpv4",target.string(),"",true);accepted.write();
    const auto output=sla::LayerDocument::load_archive(target.string());
    auto expected=pixels;for(auto &value:expected)value=sla::project_mask_value(value,sla::LayerMaskPrecision::NativeGray7);
    CHECK(output.image(0).buf==expected);
    CHECK(source->layer_document()->image(0).buf==pixels);
    CHECK(snapshot->document()->image(0).buf==pixels);
    CHECK(snapshot->document()->fresh_job()->mask_precision==sla::LayerMaskPrecision::Gray8);
    CHECK(snapshot->generation().current());
    if(const char *root=std::getenv("SLA_FROZEN_REQUEST_FIXTURES")) {
        const auto fixture=boost::filesystem::path(root)/"palette";
        boost::filesystem::create_directories(fixture);
        boost::filesystem::copy_file(target,fixture/"frozen.cxdlpv4",boost::filesystem::copy_options::overwrite_existing);
        source->export_print((fixture/"source.sl1").string());
        source->full_print_config().save((fixture/"source.ini").string());
    }
}

TEST_CASE("Frozen vector requests preserve SVG and can rasterize without the source meshes", "[sla_frozen_request]") {
    const std::string target_format=GENERATE("SL1SVG","SL1","cxdlpv4");
    Directory directory;
    SLAFullPrintConfig full;Test::configure_archive_fixture(full,"SL1SVG");DynamicPrintConfig config;config.apply(full);
    config.set("supports_enable",false);config.set("pad_enable",false);config.set("support_object_elevation",0.);
    config.set("printer_model","GENERIC");
    Model model;auto *object=model.add_object("vector request","",make_cube(1.,1.,.1));
    object->add_instance()->set_offset(Vec3d(1.,1.,0.));
    SLAPrint source;source.apply(model,config);source.process();REQUIRE_FALSE(source.layer_document());
    auto snapshot=std::make_shared<const sla::ExportSnapshot>(source.capture_export_snapshot());
    const auto target=directory.path/("vector."+std::string(get_default_extension(target_format.c_str())));
    const sla::ExportRequest request(snapshot,target_format,target.string());request.write();
    CHECK(boost::filesystem::file_size(target)>0);
    if(target_format!="SL1SVG") {
        const auto output=sla::LayerDocument::load_archive(target.string());
        CHECK(output.layer_count()==snapshot->levels().size());
        CHECK(output.image(0).cols==size_t(full.display_pixels_x.value));
        CHECK(output.image(0).rows==size_t(full.display_pixels_y.value));
        const auto image=output.image(0);
        CHECK(std::any_of(image.buf.begin(),image.buf.end(),[](uint8_t value){return value!=0;}));
    }
    CHECK(snapshot->generation().current());CHECK_FALSE(source.layer_document());
}

TEST_CASE("Request cancel or source invalidation after staging preserves old output", "[sla_frozen_request]") {
    const bool cancel=GENERATE(false,true);Directory directory;
    SLAFullPrintConfig full;Test::configure_archive_fixture(full,"SL1");DynamicPrintConfig config;config.apply(full);
    std::vector<uint8_t> pixels(full.display_pixels_x.value*full.display_pixels_y.value,255);
    const auto document=sla::LayerDocument::from_rasters({sla::PNGRasterEncoder{}(
        pixels.data(),full.display_pixels_x.value,full.display_pixels_y.value,1)});
    auto source=SLAPrint::create_raster_job(config,document,{.05});
    auto snapshot=std::make_shared<const sla::ExportSnapshot>(source->capture_export_snapshot());
    const auto target=directory.path/"old.sl1";{std::ofstream file(target.string());file<<"old";}
    const sla::ExportRequest request(snapshot,"SL1",target.string());bool triggered=false;
    CHECK_THROWS_AS(request.write([&] {
        if(triggered)return;
        for(const auto &entry:boost::filesystem::directory_iterator(directory.path))
            if(entry.path().filename().string().find(".sla-export-")==0 && boost::filesystem::file_size(entry.path())>0) {
                triggered=true;if(cancel)request.cancel();else source->clear();break;
            }
    }),CanceledException);
    CHECK(triggered);CHECK(read_file(target)==std::vector<uint8_t>{'o','l','d'});
    CHECK(std::distance(boost::filesystem::directory_iterator(directory.path),boost::filesystem::directory_iterator{})==1);
}

TEST_CASE("Frozen model metadata remains owned and aliases do not change selected writer", "[sla_frozen_request]") {
    Directory directory;
    SLAFullPrintConfig full;Test::configure_archive_fixture(full,"SL1");DynamicPrintConfig config;config.apply(full);
    config.set("supports_enable",false);config.set("pad_enable",false);config.set("support_object_elevation",0.);
    config.set("hollowing_enable",true);config.set("hollowing_min_thickness",.3);
    config.set_key_value("sla_material_settings_id",new ConfigOptionString("Frozen resin"));
    config.set_key_value("sla_print_settings_id",new ConfigOptionString("Frozen process"));
    Model model;auto *object=model.add_object("hollow metadata","",make_cube(6.,6.,1.));
    object->add_instance()->set_offset(Vec3d(1.,1.,0.));
    SLAPrint source;source.apply(model,config);source.process();
    REQUIRE(source.objects().size()==1);REQUIRE(source.objects()[0]->config().hollowing_enable.value);
    auto snapshot=std::make_shared<const sla::ExportSnapshot>(source.capture_export_snapshot());
    const bool placeholders_same=snapshot->placeholders()==source.placeholder_parser().config();CHECK(placeholders_same);
    const auto target=directory.path/"explicit-choice.zip";
    const sla::ExportRequest request(snapshot,"SL1",target.string(),"Owned name");request.write();
    const auto loaded=sla::LayerDocument::load_sl1(target.string());
    CHECK(loaded.layer_count()==snapshot->document()->layer_count());
    mz_zip_archive archive{};REQUIRE(mz_zip_reader_init_file(&archive,target.string().c_str(),0));
    size_t size=0;void *payload=mz_zip_reader_extract_file_to_heap(&archive,"config.ini",&size,0);
    REQUIRE(payload);const std::string metadata(static_cast<const char *>(payload),size);
    mz_free(payload);mz_zip_reader_end(&archive);
    CHECK(metadata.find("hollow = 1")!=std::string::npos);
    CHECK(metadata.find("materialName = Frozen resin")!=std::string::npos);
    CHECK(metadata.find("printProfile = Frozen process")!=std::string::npos);
    CHECK(snapshot->objects()[0].name=="hollow metadata");CHECK(snapshot->generation().current());
    if(const char *root=std::getenv("SLA_FROZEN_REQUEST_FIXTURES")) {
        const auto fixture=boost::filesystem::path(root)/"hollow";
        boost::filesystem::create_directories(fixture);
        boost::filesystem::copy_file(target,fixture/"frozen.sl1",boost::filesystem::copy_options::overwrite_existing);
        source.export_print((fixture/"source.sl1").string());
    }
}

TEST_CASE("Deferred export input survives producing masks for that same model", "[sla_export_input]") {
    SLAFullPrintConfig full;Test::configure_archive_fixture(full,"SL1");DynamicPrintConfig config;config.apply(full);
    config.set("supports_enable",false);config.set("pad_enable",false);config.set("support_object_elevation",0.);
    Model model;auto *object=model.add_object("deferred input","",make_cube(1.,1.,.1));
    object->add_instance()->set_offset(Vec3d(1.,1.,0.));
    SLAPrint source;source.apply(model,config);
    const auto input=source.export_input_guard();const auto output=source.export_generation_guard();
    source.process();REQUIRE(source.finished());REQUIRE(source.layer_document());
    CHECK(input.current());CHECK_NOTHROW(input.check());CHECK_FALSE(output.current());
    const auto snapshot=source.capture_export_snapshot();
    CHECK(input.current());CHECK(snapshot.generation().current());
    source.apply(model,source.full_print_config());CHECK(input.current());
    auto original_guard=source.export_input_guard();
    const auto moved=std::move(original_guard);CHECK(moved.current());CHECK_FALSE(original_guard.current());
}

TEST_CASE("Deferred export input expires on operator changes or source destruction", "[sla_export_input]") {
    const std::string action=GENERATE("price","identity","move","replace","clear","destroy");
    SLAFullPrintConfig full;Test::configure_archive_fixture(full,"SL1");DynamicPrintConfig config;config.apply(full);
    config.set("supports_enable",false);config.set("pad_enable",false);config.set("support_object_elevation",0.);
    Model model;auto *object=model.add_object("input changes","",make_cube(1.,1.,.1));
    object->add_instance()->set_offset(Vec3d(1.,1.,0.));
    auto source=std::make_unique<SLAPrint>();source->apply(model,config);source->process();
    const auto input=source->export_input_guard();
    if(action=="destroy")source.reset();
    else if(action=="clear")source->clear();
    else if(action=="replace")source->replace_layer_document(*source->layer_document());
    else {
        auto changed=source->full_print_config();
        if(action=="price")changed.set("bottle_cost",changed.opt_float("bottle_cost")+1.);
        if(action=="identity")changed.set("printer_model","Changed input printer");
        if(action=="move")object->instances.front()->set_offset(Vec3d(2.,1.,0.));
        source->apply(model,changed);
    }
    CHECK_FALSE(input.current());CHECK_THROWS_AS(input.check(),CanceledException);
}

TEST_CASE("Queuing a saved layer result invalidates prior export input", "[sla_export_input]") {
    SLAFullPrintConfig full;Test::configure_archive_fixture(full,"SL1");DynamicPrintConfig config;config.apply(full);
    config.set("supports_enable",false);config.set("pad_enable",false);config.set("support_object_elevation",0.);
    Model model;auto *object=model.add_object("cache input","",make_cube(1.,1.,.1));
    object->add_instance()->set_offset(Vec3d(1.,1.,0.));
    SLAPrint source;source.apply(model,config);source.process();REQUIRE(source.layer_document());
    auto saved=std::make_shared<sla::ProjectRasterState>(sla::capture_project_layers(model,config,source));
    saved->document.paint(0,0,0,1,127);
    const auto prior_input=source.export_input_guard();const auto prior_output=source.export_generation_guard();
    source.queue_project_layers(saved);
    CHECK_FALSE(prior_input.current());CHECK_FALSE(prior_output.current());
    const auto accepted=source.export_input_guard();
    source.process();CHECK_FALSE(source.project_layer_restore_failed());
    CHECK(accepted.current());REQUIRE(source.layer_document());
    CHECK(source.layer_document()->image(0).buf==saved->document.image(0).buf);
}

TEST_CASE("Deferred input survives first slicing plus saved-mask restoration", "[sla_export_input]") {
    SLAFullPrintConfig full;Test::configure_archive_fixture(full,"SL1");DynamicPrintConfig config;config.apply(full);
    config.set("supports_enable",false);config.set("pad_enable",false);config.set("support_object_elevation",0.);
    Model model;auto *object=model.add_object("restored deferred","",make_cube(1.,1.,.1));
    object->add_instance()->set_offset(Vec3d(1.,1.,0.));
    SLAPrint initial;initial.apply(model,config);initial.process();REQUIRE(initial.layer_document());
    auto saved=std::make_shared<sla::ProjectRasterState>(sla::capture_project_layers(model,config,initial));
    saved->document.paint(0,0,0,1,127);
    SLAPrint restored;restored.apply(model,config);restored.queue_project_layers(saved);
    const auto input=restored.export_input_guard();const auto output=restored.export_generation_guard();
    restored.process();REQUIRE_FALSE(restored.project_layer_restore_failed());
    CHECK(input.current());CHECK_FALSE(output.current());
    REQUIRE(restored.layer_document());CHECK(restored.layer_document()->image(0).buf==saved->document.image(0).buf);
}

TEST_CASE("Deferred input survives metadata refresh without losing painted masks", "[sla_export_input]") {
    SLAFullPrintConfig full;Test::configure_archive_fixture(full,"SL1");DynamicPrintConfig config;config.apply(full);
    config.set("supports_enable",false);config.set("pad_enable",false);config.set("support_object_elevation",0.);
    Model model;auto *object=model.add_object("metadata refresh","",make_cube(1.,1.,.1));
    object->add_instance()->set_offset(Vec3d(1.,1.,0.));
    SLAPrint source;source.apply(model,config);source.process();REQUIRE(source.layer_document());
    auto edited=*source.layer_document();edited.paint(0,0,0,1,127);
    source.replace_layer_document(edited);
    const auto original=edited.image(0).buf;
    const auto old_input=source.export_input_guard();
    config=source.full_print_config();config.set("exposure_time",config.opt_float("exposure_time")+1.);
    source.apply(model,config);CHECK_FALSE(old_input.current());
    const auto accepted=source.export_input_guard();const auto output=source.export_generation_guard();
    source.process();REQUIRE(source.finished());REQUIRE(source.layer_document());
    CHECK(accepted.current());CHECK_FALSE(output.current());
    CHECK(source.layer_document()->image(0).buf==original);
    CHECK(source.full_print_config().opt_float("exposure_time")==config.opt_float("exposure_time"));
}

TEST_CASE("Changing queued masks stops the worker after invalidating its input", "[sla_export_input]") {
    SLAPrint source;
    auto saved=std::make_shared<sla::ProjectRasterState>();
    const auto prior=source.export_input_guard();unsigned stops=0;
    source.set_cancel_callback([&] {++stops;CHECK_FALSE(prior.current());});
    source.queue_project_layers(saved);CHECK(stops==1);
    const auto selected=source.export_input_guard();
    source.queue_project_layers(saved);CHECK(stops==1);CHECK(selected.current());
    source.queue_project_layers(nullptr);CHECK(stops==2);CHECK_FALSE(selected.current());
    const auto cleared=source.export_input_guard();
    source.queue_project_layers(nullptr);CHECK(stops==2);CHECK(cleared.current());
    source.set_cancel_callback([] {});
}

TEST_CASE("Deferred request binds its own sliced result and accepted output choices", "[sla_deferred_request]") {
    Directory directory;
    SLAFullPrintConfig full;Test::configure_archive_fixture(full,"SL1");DynamicPrintConfig config;config.apply(full);
    config.set("supports_enable",false);config.set("pad_enable",false);config.set("support_object_elevation",0.);
    Model model;auto *object=model.add_object("deferred request","",make_cube(1.,1.,.1));
    object->add_instance()->set_offset(Vec3d(1.,1.,0.));
    SLAPrint source;source.apply(model,config);
    const auto path=(directory.path/"accepted.zip").string();
    const sla::DeferredExportRequest intent(source.export_input_guard(),"SL1",path,"Accepted name");
    source.process();REQUIRE(source.finished());
    auto snapshot=std::make_shared<const sla::ExportSnapshot>(source.capture_export_snapshot());
    const auto ready=intent.bind(snapshot);
    CHECK(ready.format()=="SL1");CHECK(ready.path()==path);CHECK(ready.projectname()=="Accepted name");
    CHECK_FALSE(ready.palette_approved());
    ready.write();
    CHECK(sla::LayerDocument::load_sl1(path).image(0).buf==source.layer_document()->image(0).buf);
    CHECK_THROWS_AS(intent.bind(snapshot),ExportError);
}

TEST_CASE("Deferred request rejects stale foreign and missing results", "[sla_deferred_request]") {
    const auto action=GENERATE("other_source","operator_change","destroy","missing");
    SLAFullPrintConfig full;Test::configure_archive_fixture(full,"SL1");DynamicPrintConfig config;config.apply(full);
    std::vector<uint8_t> pixels(full.display_pixels_x.value*full.display_pixels_y.value,255);
    const auto document=sla::LayerDocument::from_rasters({sla::PNGRasterEncoder{}(
        pixels.data(),full.display_pixels_x.value,full.display_pixels_y.value,1)});
    auto source=SLAPrint::create_raster_job(config,document,{.05});
    const sla::DeferredExportRequest intent(source->export_input_guard(),"SL1","unused.sl1");
    auto snapshot=std::make_shared<const sla::ExportSnapshot>(source->capture_export_snapshot());
    if(std::string(action)=="other_source") {
        auto other=SLAPrint::create_raster_job(config,document,{.05});
        auto foreign=std::make_shared<const sla::ExportSnapshot>(other->capture_export_snapshot());
        CHECK_THROWS_AS(intent.bind(foreign),ExportError);
        CHECK_NOTHROW(intent.check());
    } else if(std::string(action)=="missing")CHECK_THROWS_AS(intent.bind(nullptr),ExportError);
    else {
        if(std::string(action)=="destroy")source.reset();else source->replace_layer_document(document);
        CHECK_THROWS_AS(intent.bind(snapshot),CanceledException);
    }
}

TEST_CASE("Deferred cancellation follows binding and copies without affecting another intent", "[sla_deferred_request]") {
    const bool before_binding=GENERATE(false,true);
    Directory directory;
    SLAFullPrintConfig full;Test::configure_archive_fixture(full,"SL1");DynamicPrintConfig config;config.apply(full);
    std::vector<uint8_t> pixels(full.display_pixels_x.value*full.display_pixels_y.value,255);
    const auto document=sla::LayerDocument::from_rasters({sla::PNGRasterEncoder{}(
        pixels.data(),full.display_pixels_x.value,full.display_pixels_y.value,1)});
    auto source=SLAPrint::create_raster_job(config,document,{.05});
    auto snapshot=std::make_shared<const sla::ExportSnapshot>(source->capture_export_snapshot());
    const auto target=directory.path/"cancelled.sl1";{std::ofstream file(target.string());file<<"old";}
    const sla::DeferredExportRequest first(source->export_input_guard(),"SL1",target.string());
    const auto copy=first;
    const sla::DeferredExportRequest independent(source->export_input_guard(),"SL1",(directory.path/"independent.sl1").string());
    if(before_binding) {
        copy.cancel();CHECK_THROWS_AS(first.bind(snapshot),CanceledException);
    } else {
        const auto ready=first.bind(snapshot);copy.cancel();
        CHECK(ready.cancelled());CHECK_THROWS_AS(ready.write(),CanceledException);
    }
    CHECK(first.cancelled());CHECK(copy.cancelled());CHECK_FALSE(independent.cancelled());
    CHECK(read_file(target)==std::vector<uint8_t>{'o','l','d'});
    CHECK(snapshot->generation().current());independent.bind(snapshot).write();
    CHECK(boost::filesystem::exists(directory.path/"independent.sl1"));
}

TEST_CASE("Deferred binding serializes competing workers and cancellation", "[sla_deferred_request]") {
    const bool cancel_race=GENERATE(false,true);
    Directory directory;
    SLAFullPrintConfig full;Test::configure_archive_fixture(full,"SL1");DynamicPrintConfig config;config.apply(full);
    std::vector<uint8_t> pixels(full.display_pixels_x.value*full.display_pixels_y.value,255);
    const auto document=sla::LayerDocument::from_rasters({sla::PNGRasterEncoder{}(
        pixels.data(),full.display_pixels_x.value,full.display_pixels_y.value,1)});
    auto source=SLAPrint::create_raster_job(config,document,{.05});
    auto snapshot=std::make_shared<const sla::ExportSnapshot>(source->capture_export_snapshot());
    for(unsigned iteration=0;iteration<16;++iteration) {
        const auto path=directory.path/(std::to_string(iteration)+".sl1");
        const sla::DeferredExportRequest intent(source->export_input_guard(),"SL1",path.string());
        std::promise<void> go;const auto start=go.get_future().share();
        auto bind=[&]() -> std::optional<sla::ExportRequest> {
            start.wait();
            try {return intent.bind(snapshot);}
            catch(const CanceledException &) {if(cancel_race)return std::nullopt;throw;}
            catch(const ExportError &) {if(!cancel_race)return std::nullopt;throw;}
        };
        auto first=std::async(std::launch::async,bind);
        if(cancel_race) {
            auto cancel=std::async(std::launch::async,[&] {start.wait();intent.cancel();});
            go.set_value();auto ready=first.get();cancel.get();
            CHECK(intent.cancelled());
            if(ready)CHECK_THROWS_AS(ready->write(),CanceledException);
            CHECK_FALSE(boost::filesystem::exists(path));
        } else {
            auto second=std::async(std::launch::async,bind);
            go.set_value();auto a=first.get();auto b=second.get();
            REQUIRE(bool(a)!=bool(b));
            (a?*a:*b).write();CHECK(boost::filesystem::exists(path));
        }
        CHECK(snapshot->generation().current());
    }
}

TEST_CASE("Deferred owner cancellation after staging keeps the old file", "[sla_deferred_request]") {
    Directory directory;
    SLAFullPrintConfig full;Test::configure_archive_fixture(full,"SL1");DynamicPrintConfig config;config.apply(full);
    std::vector<uint8_t> pixels(full.display_pixels_x.value*full.display_pixels_y.value,255);
    const auto document=sla::LayerDocument::from_rasters({sla::PNGRasterEncoder{}(
        pixels.data(),full.display_pixels_x.value,full.display_pixels_y.value,1)});
    auto source=SLAPrint::create_raster_job(config,document,{.05});
    auto snapshot=std::make_shared<const sla::ExportSnapshot>(source->capture_export_snapshot());
    const auto path=directory.path/"old.sl1";{std::ofstream file(path.string());file<<"old";}
    const sla::DeferredExportRequest intent(source->export_input_guard(),"SL1",path.string());
    const auto request=intent.bind(snapshot);const auto owner=intent;bool triggered=false;
    CHECK_THROWS_AS(request.write([&] {
        if(triggered)return;
        for(const auto &entry:boost::filesystem::directory_iterator(directory.path))
            if(entry.path().filename().string().find(".sla-export-")==0 && boost::filesystem::file_size(entry.path())>0) {
                triggered=true;owner.cancel();break;
            }
    }),CanceledException);
    CHECK(triggered);CHECK(request.cancelled());CHECK(read_file(path)==std::vector<uint8_t>{'o','l','d'});
    CHECK(std::distance(boost::filesystem::directory_iterator(directory.path),boost::filesystem::directory_iterator{})==1);
    CHECK(snapshot->generation().current());
}

namespace {
sla::LayerDocument native_request_fixture(const boost::filesystem::path &path) {
    const std::vector<uint8_t> pixels{0, 3, 127, 255};
    const auto png = sla::PNGRasterEncoder{}(pixels.data(), 2, 2, 1);
    Zipper zip(path.string());
    zip.add_entry("config.ini"); zip << "layerHeight = 0.05\n";
    zip.add_entry("prusaslicer.ini"); zip << "display_width = 2\n";
    zip.add_entry("job00000.png", png.data(), png.size());
    zip.add_entry("vendor-data.bin"); zip << "opaque native metadata";
    zip.finalize();
    return sla::LayerDocument::load_archive(path.string());
}
}
TEST_CASE("Native document generations follow edits history copies and lifetime", "[sla_native_request]") {
    Directory dir;
    auto document = native_request_fixture(dir.path/"input.sl1");
    auto before = document.export_generation_guard();
    document.paint(0, 0, 0, 0, 0); // Already black: no content change.
    CHECK(before.current());
    document.invert(0); CHECK_FALSE(before.current());
    before = document.export_generation_guard();
    REQUIRE(document.undo()); CHECK_FALSE(before.current());
    before = document.export_generation_guard();
    REQUIRE(document.redo()); CHECK_FALSE(before.current());
    auto copy = document;
    CHECK_FALSE(copy.export_input_guard().same_source(document.export_generation_guard()));
    before = document.export_generation_guard();
    document = copy; CHECK_FALSE(before.current());
    before = document.export_generation_guard();
    auto moved = std::move(document); CHECK(before.current());
    CHECK(moved.export_input_guard().same_source(before));
    auto doomed = std::make_unique<sla::LayerDocument>(moved);
    before = doomed->export_generation_guard();doomed.reset();CHECK_FALSE(before.current());
}
TEST_CASE("Native requests reject changed sources without replacing output", "[sla_native_request]") {
    Directory dir;
    auto source = native_request_fixture(dir.path/"input.sl1");
    source.invert(0);
    const auto target = dir.path/"keep.sl1";
    {std::ofstream file(target.string());file << "old";}
    const sla::NativeArchiveExportRequest request(source, target.string());
    const auto captured = request.document().image(0).buf;
    source.undo();
    CHECK(request.document().image(0).buf == captured);
    CHECK_THROWS_AS(request.write(), CanceledException);
    CHECK((read_file(target) == std::vector<uint8_t>{'o','l','d'}));
}
TEST_CASE("Native request cancellation during writing keeps target and source history", "[sla_native_request]") {
    Directory dir;
    auto source = native_request_fixture(dir.path/"input.sl1");source.invert(0);
    const auto target = dir.path/"keep.sl1";
    {std::ofstream file(target.string());file << "old";}
    const sla::NativeArchiveExportRequest request(source, target.string());
    const auto pixels = source.image(0).buf;bool triggered = false;
    CHECK_THROWS_AS(request.write([&](size_t, size_t) {
        for (const auto &entry : boost::filesystem::directory_iterator(dir.path)) {
            if (entry.path().filename().string().find(".sla-edit-") == 0) {
                triggered = true;request.cancel();break;
            }
        }
        return true;
    }), CanceledException);
    CHECK(triggered);CHECK(source.dirty());CHECK(source.can_undo());
    CHECK(source.image(0).buf == pixels);
    CHECK((read_file(target) == std::vector<uint8_t>{'o','l','d'}));
    CHECK(std::distance(boost::filesystem::directory_iterator(dir.path), boost::filesystem::directory_iterator{}) == 2);
}

TEST_CASE("Native request publication and source save baseline are separate", "[sla_native_request]") {
    Directory dir;auto source = native_request_fixture(dir.path/"input.sl1");source.invert(0);
    const auto pixels = source.image(0).buf;
    const sla::NativeArchiveExportRequest request(source, (dir.path/"saved.sl1").string());
    CHECK_FALSE(request.document().can_undo());
    CHECK_FALSE(request.acknowledge_saved(source));CHECK(source.dirty());
    auto foreign = source;
    request.write();
    CHECK(source.dirty());CHECK(source.can_undo());CHECK(source.image(0).buf == pixels);
    CHECK(sla::LayerDocument::load_archive(request.path()).image(0).buf == pixels);
    const auto archive = read_zipper_archive(request.path(), {"vendor-data.bin"}, {});
    REQUIRE(archive.entries.size() == 1);
    CHECK(std::string(archive.entries[0].buf.begin(), archive.entries[0].buf.end()) == "opaque native metadata");
    CHECK_FALSE(request.acknowledge_saved(foreign));CHECK(foreign.dirty());
    CHECK(request.acknowledge_saved(source));CHECK_FALSE(source.dirty());CHECK(source.can_undo());
    source.undo();CHECK(source.dirty());CHECK_FALSE(request.acknowledge_saved(source));
}
TEST_CASE("Native requests share cancellation only with their own copies", "[sla_native_request]") {
    Directory dir;auto source = native_request_fixture(dir.path/"input.sl1");source.invert(0);
    const sla::NativeArchiveExportRequest first(source, (dir.path/"first.sl1").string());
    const auto copy = first;
    const sla::NativeArchiveExportRequest second(source, (dir.path/"second.sl1").string());
    copy.cancel();CHECK(first.cancelled());CHECK_FALSE(second.cancelled());
    CHECK_THROWS_AS(first.write(), CanceledException);CHECK_FALSE(first.acknowledge_saved(source));
    CHECK_NOTHROW(second.write());CHECK(source.dirty());
    CHECK(second.acknowledge_saved(source));CHECK_FALSE(source.dirty());
}
TEST_CASE("Native request outlives its document without borrowing it", "[sla_native_request]") {
    Directory dir;auto source = std::make_unique<sla::LayerDocument>(native_request_fixture(dir.path/"input.sl1"));
    const sla::NativeArchiveExportRequest request(*source, (dir.path/"output.sl1").string());
    const auto pixels = request.document().image(0).buf;
    source.reset();CHECK(request.document().image(0).buf == pixels);
    CHECK_THROWS_AS(request.write(), CanceledException);CHECK_FALSE(boost::filesystem::exists(request.path()));
}
TEST_CASE("Native snapshot capture stops when a callback changes the source", "[sla_native_request]") {
    Directory dir;auto source = native_request_fixture(dir.path/"input.sl1");bool changed = false;
    CHECK_THROWS_AS(sla::NativeArchiveExportRequest(source, (dir.path/"output.sl1").string(), {},
        [&](size_t done, size_t) {
            if (done && !changed) { changed = true;source.invert(0); }
            return true;
        }), CanceledException);
    CHECK(changed);CHECK(source.can_undo());CHECK_FALSE(boost::filesystem::exists(dir.path/"output.sl1"));
}
TEST_CASE("Native legacy save rejects source mutation from a progress callback", "[sla_native_request]") {
    Directory dir;auto source = native_request_fixture(dir.path/"input.sl1");
    const auto target = dir.path/"keep.sl1";{std::ofstream file(target.string());file<<"old";}
    bool changed = false;
    CHECK_THROWS_AS(source.save_archive(target.string(), [&](size_t, size_t) {
        for (const auto &entry : boost::filesystem::directory_iterator(dir.path))
            if (!changed && entry.path().filename().string().find(".sla-edit-") == 0) {
                changed = true;source.invert(0);break;
            }
        return true;
    }), CanceledException);
    CHECK(changed);CHECK(source.dirty());CHECK(source.can_undo());
    CHECK((read_file(target) == std::vector<uint8_t>{'o','l','d'}));
    CHECK(std::distance(boost::filesystem::directory_iterator(dir.path), boost::filesystem::directory_iterator{}) == 2);
}

TEST_CASE("Native request preserves masks and parameters across codec families", "[sla_native_request_matrix]") {
    const std::string format = GENERATE("SL1", "cxdlpv4", "ctb_v4", "pm5s");
    CAPTURE(format);
    Directory dir;
    SLAFullPrintConfig full;Test::configure_archive_fixture(full, format);
    DynamicPrintConfig configuration;configuration.apply(full);
    const auto columns = size_t(full.display_pixels_x.value), rows = size_t(full.display_pixels_y.value);
    std::vector<uint8_t> pixels(columns * rows, 255);pixels[0] = 0;
    const auto png = sla::PNGRasterEncoder{}(pixels.data(), columns, rows, 1);
    const auto masks = sla::LayerDocument::from_rasters({png, png, png});
    auto job = SLAPrint::create_raster_job(configuration, masks, {.05, .1, .15});
    const auto extension = std::string(get_default_extension(format.c_str()));
    const auto input = dir.path/("input." + extension), output = dir.path/("output." + extension);
    job->export_print(input.string());
    auto source = sla::LayerDocument::load_archive(input.string());source.invert(0);
    std::vector<sla::LayerParameters> parameters;
    if (!source.parameter_capabilities().empty())
        for (size_t i = 0; i < source.layer_count(); ++i) parameters.push_back(source.layer_parameters(i));
    std::optional<sla::LayerExportProfile> profile;
    if (format == "pm5s") profile = sla::LayerExportProfile{columns, rows,
        full.display_width.value, full.display_height.value, 1.2, .25, 200.};
    const sla::NativeArchiveExportRequest request(source, output.string(), profile);
    if (profile) profile->columns = 0; // Accepted context is owned, not borrowed.
    REQUIRE_NOTHROW(request.write());
    const auto readback = sla::LayerDocument::load_archive(output.string());
    REQUIRE(readback.layer_count() == source.layer_count());
    for (size_t i = 0; i < source.layer_count(); ++i) {
        CHECK(readback.image(i).buf == source.image(i).buf);
        if (!parameters.empty()) CHECK(readback.layer_parameters(i) == parameters[i]);
    }
    CHECK(source.dirty());CHECK(source.can_undo());
    CHECK(request.acknowledge_saved(source));CHECK_FALSE(source.dirty());
    if (const char *root = std::getenv("SLA_NATIVE_REQUEST_FIXTURES")) {
        const boost::filesystem::path fixtures(root);boost::filesystem::create_directories(fixtures);
        boost::filesystem::copy_file(input, fixtures/(format + "-input." + extension));
        boost::filesystem::copy_file(output, fixtures/(format + "-output." + extension));
    }
}

namespace {
sla::LayerDocument conversion_request_fixture(const boost::filesystem::path &path, DynamicPrintConfig &configuration,
    size_t layer_count=3, size_t columns=200, size_t rows=120) {
    SLAFullPrintConfig full;Test::configure_archive_fixture(full,"ctb_v4");
    full.set("display_pixels_x",int(columns));full.set("display_pixels_y",int(rows));configuration.apply(full);
    std::vector<sla::EncodedRaster> rasters;std::vector<double> positions;
    for(size_t i=0;i<layer_count;++i) {
        std::vector<uint8_t> pixels(columns*rows,255);pixels[i%pixels.size()]=0;
        rasters.push_back(sla::PNGRasterEncoder{}(pixels.data(),columns,rows,1));positions.push_back(double(i+1)*.05);
    }
    const auto masks=sla::LayerDocument::from_rasters(std::move(rasters));
    auto job=SLAPrint::create_raster_job(configuration,masks,positions);job->export_print(path.string());
    return sla::LayerDocument::load_archive(path.string());
}
}
TEST_CASE("Conversion request cancellation reaches native staging", "[sla_native_conversion_request]") {
    Directory dir;DynamicPrintConfig config;auto source=conversion_request_fixture(dir.path/"input.v4.ctb",config);
    source.invert(0);const auto pixels=source.image(0).buf;
    const auto target=dir.path/"keep.v5.ctb";{std::ofstream file(target.string());file<<"old";}
    const sla::NativeArchiveConversionRequest request(source,config,"ctb_v5",target.string());bool triggered=false;
    CHECK_THROWS_AS(request.write([&](size_t,size_t) {
        for(const auto &entry:boost::filesystem::directory_iterator(dir.path))
            if(entry.path().filename().string().find(".sla-export-")==0) {triggered=true;request.cancel();break;}
        return true;
    }),CanceledException);
    CHECK(triggered);CHECK((read_file(target)==std::vector<uint8_t>{'o','l','d'}));
    CHECK(source.dirty());CHECK(source.can_undo());CHECK(source.image(0).buf==pixels);
    CHECK(std::distance(boost::filesystem::directory_iterator(dir.path),boost::filesystem::directory_iterator{})==2);
}
TEST_CASE("Conversion requests retain printer choices and preserve source history", "[sla_native_conversion_request]") {
    Directory dir;DynamicPrintConfig config;auto source=conversion_request_fixture(dir.path/"input.v4.ctb",config);
    source.invert(0);const auto pixels=source.image(0).buf;
    const sla::NativeArchiveConversionRequest request(source,config,"ctb_v5",(dir.path/"output.v5.ctb").string());
    const auto accepted_config=config;
    config.set("display_width",999.);config.set("sla_archive_format","SL1");
    CHECK(request.configuration().opt_float("display_width")==100.);CHECK(request.format()=="ctb_v5");
    REQUIRE_NOTHROW(request.write());
    const auto saved=sla::LayerDocument::load_archive(request.path());
    CHECK(saved.archive_format()=="ctb_v5");CHECK(saved.image(0).buf==pixels);
    CHECK(source.dirty());CHECK(source.can_undo());CHECK_FALSE(request.document().can_undo());
    if(const char *root=std::getenv("SLA_NATIVE_CONVERSION_FIXTURE_ROOT")) {
        const boost::filesystem::path fixtures(root);boost::filesystem::create_directories(fixtures);
        accepted_config.save((fixtures/"conversion.ini").string());
        boost::filesystem::copy_file(dir.path/"input.v4.ctb",fixtures/"input.v4.ctb");
        boost::filesystem::copy_file(request.path(),fixtures/"output.v5.ctb");
    }
}
TEST_CASE("Conversion progress cancellation is an owned outcome", "[sla_native_conversion_request]") {
    Directory dir;DynamicPrintConfig config;auto source=conversion_request_fixture(dir.path/"input.v4.ctb",config);
    const sla::NativeArchiveConversionRequest request(source,config,"ctb_v5",(dir.path/"output.v5.ctb").string());
    CHECK_THROWS_AS(request.write([](size_t,size_t){return false;}),sla::LayerOperationCancelled);
    CHECK(request.cancelled());CHECK_FALSE(boost::filesystem::exists(request.path()));
}

TEST_CASE("Conversion callbacks stay on the calling thread", "[sla_conversion_callback_thread]") {
    Directory dir;DynamicPrintConfig config;auto source=conversion_request_fixture(dir.path/"input.v4.ctb",config,32,800,480);
    const sla::NativeArchiveConversionRequest request(source,config,"ctb_v5",(dir.path/"output.v5.ctb").string());
    const auto owner=std::this_thread::get_id();std::atomic<size_t> wrong_thread{0},calls{0};
    REQUIRE_NOTHROW(request.write([&](size_t,size_t) {
        ++calls;if(std::this_thread::get_id()!=owner)++wrong_thread;return true;
    }));
    CHECK(calls.load()>0);CHECK(wrong_thread.load()==0);
}
TEST_CASE("Source change or destruction during conversion preserves output", "[sla_native_conversion_request]") {
    const bool destroy=GENERATE(false,true);Directory dir;DynamicPrintConfig config;
    auto source=std::make_unique<sla::LayerDocument>(conversion_request_fixture(dir.path/"input.v4.ctb",config));
    const auto target=dir.path/"keep.v5.ctb";{std::ofstream file(target.string());file<<"old";}
    const sla::NativeArchiveConversionRequest request(*source,config,"ctb_v5",target.string());
    const auto pixels=request.document().image(0).buf;bool triggered=false;
    CHECK_THROWS_AS(request.write([&](size_t,size_t) {
        if(!triggered)for(const auto &entry:boost::filesystem::directory_iterator(dir.path))
            if(entry.path().filename().string().find(".sla-export-")==0) {
                triggered=true;if(destroy)source.reset();else source->invert(0);break;
            }
        return true;
    }),CanceledException);
    CHECK(triggered);CHECK((read_file(target)==std::vector<uint8_t>{'o','l','d'}));
    CHECK(request.document().image(0).buf==pixels);
    CHECK(std::distance(boost::filesystem::directory_iterator(dir.path),boost::filesystem::directory_iterator{})==2);
}
TEST_CASE("Generate large native conversion GUI fixture", "[.][sla_native_conversion_large_fixture]") {
    const char *root=std::getenv("SLA_NATIVE_CONVERSION_LARGE_ROOT");REQUIRE(root);
    const boost::filesystem::path folder(root);boost::filesystem::create_directories(folder);
    REQUIRE_FALSE(boost::filesystem::exists(folder/"input.v4.ctb"));
    DynamicPrintConfig config;const auto source=conversion_request_fixture(folder/"input.v4.ctb",config,128,8192,4096);
    config.save((folder/"conversion.ini").string());CHECK(source.layer_count()==128);
}

TEST_CASE("Late cancellation does not undo a published native save", "[sla_native_late_cancel]") {
    Directory dir;auto source=native_request_fixture(dir.path/"input.sl1");source.invert(0);
    const sla::NativeArchiveExportRequest request(source,(dir.path/"saved.sl1").string());
    request.write();request.cancel();
    CHECK(request.cancelled());CHECK(boost::filesystem::exists(request.path()));
    CHECK(request.acknowledge_saved(source));CHECK_FALSE(source.dirty());CHECK(source.can_undo());
}

namespace {
void configure_slicing_progress_cube(Model &model,DynamicPrintConfig &config) {
    SLAFullPrintConfig full;Test::configure_archive_fixture(full,"SL1");
    config.apply(full);
    config.set("supports_enable",false);config.set("pad_enable",false);config.set("support_object_elevation",0.);
    config.set("layer_height",.05);config.set("initial_layer_height",.05);
    auto *object=model.add_object("progress cube","",make_cube(1.,1.,.15));
    object->add_instance()->set_offset(Vec3d(1.,1.,0.));
}
}

TEST_CASE("Slicing reaches one hundred percent only after final layer work", "[sla_slicing_progress]") {
    Model model;DynamicPrintConfig config;configure_slicing_progress_cube(model,config);
    SLAPrint print;
    std::mutex mutex;
    std::vector<PrintBase::SlicingStatus> events;
    print.set_status_callback([&](const PrintBase::SlicingStatus &status) {
        std::lock_guard<std::mutex> lock(mutex);
        if(status.percent>=0)events.push_back(status);
    });
    print.apply(model,config);print.process();
    REQUIRE(print.finished());REQUIRE(print.layer_document());REQUIRE_FALSE(events.empty());
    CHECK(events.back().percent==100);CHECK(events.back().text=="Slicing done");
    CHECK(std::count_if(events.begin(),events.end(),[](const auto &event){return event.percent>=100;})==1);
    for(size_t i=1;i<events.size();++i)CHECK(events[i].percent>=events[i-1].percent);
    for(const auto &event:events) {
        CAPTURE(event.percent,event.text);
        if(event.percent>=100)CHECK(event.text=="Slicing done");
    }
}

TEST_CASE("Final slicing phases honor cancellation before completion and can retry", "[sla_slicing_progress]") {
    const char *stop_phase=GENERATE("Finalizing layers","Generating statistics");
    CAPTURE(stop_phase);
    Model model;DynamicPrintConfig config;configure_slicing_progress_cube(model,config);
    SLAPrint print;std::mutex mutex;
    std::vector<PrintBase::SlicingStatus> events;
    bool triggered=false,should_cancel=true;
    print.set_status_callback([&](const PrintBase::SlicingStatus &status) {
        std::lock_guard<std::mutex> lock(mutex);
        if(status.percent>=0)events.push_back(status);
        if(should_cancel && status.text==stop_phase) {triggered=true;print.cancel();}
    });
    print.apply(model,config);
    CHECK_THROWS_AS(print.process(),CanceledException);
    REQUIRE(triggered);CHECK(print.canceled());
    CHECK(std::none_of(events.begin(),events.end(),[](const auto &event){return event.percent>=100 || event.text=="Slicing done";}));
    // Existing step flags may already be done when statistics is cancelled;
    // the observable contract is cancellation without a completion event.
    should_cancel=false;events.clear();print.restart();
    REQUIRE_NOTHROW(print.process());
    REQUIRE(print.finished());REQUIRE(print.layer_document());REQUIRE_FALSE(events.empty());
    CHECK(events.back().percent==100);CHECK(events.back().text=="Slicing done");
    CHECK(std::count_if(events.begin(),events.end(),[](const auto &event){return event.percent>=100;})==1);
    for(size_t i=1;i<events.size();++i)CHECK(events[i].percent>=events[i-1].percent);
}

TEST_CASE("Metadata rerun retains painted layers and completes after refreshed statistics", "[sla_slicing_progress]") {
    Model model;DynamicPrintConfig config;configure_slicing_progress_cube(model,config);
    SLAPrint print;print.apply(model,config);print.process();
    REQUIRE(print.layer_document());REQUIRE(print.layer_document()->fresh_job());
    auto edited=*print.layer_document();edited.paint(0,0,0,0,127);
    print.replace_layer_document(edited);
    std::vector<std::vector<uint8_t>> before_pixels;
    for(size_t i=0;i<edited.layer_count();++i)before_pixels.push_back(edited.image(i).buf);
    const auto before_rows=print.layer_document()->fresh_job()->rows;
    const auto before_time=print.print_statistics().estimated_print_time;
    config=print.full_print_config();
    config.set("exposure_time",config.opt_float("exposure_time")+1.);
    config.set("initial_exposure_time",config.opt_float("initial_exposure_time")+1.);
    std::mutex mutex;std::vector<PrintBase::SlicingStatus> events;
    print.set_status_callback([&](const PrintBase::SlicingStatus &status) {
        std::lock_guard<std::mutex> lock(mutex);
        if(status.percent>=0)events.push_back(status);
    });
    print.apply(model,config);
    REQUIRE(print.layer_document());
    REQUIRE_NOTHROW(print.process());
    REQUIRE(print.finished());REQUIRE(print.layer_document());REQUIRE(print.layer_document()->fresh_job());
    REQUIRE(print.layer_document()->layer_count()==before_pixels.size());
    for(size_t i=0;i<before_pixels.size();++i) {
        CHECK(print.layer_document()->image(i).buf==before_pixels[i]);
        CHECK(print.layer_document()->fresh_job()->rows[i].at(sla::LayerParameter::Exposure)==
              before_rows[i].at(sla::LayerParameter::Exposure)+1.);
    }
    CHECK(std::abs(print.print_statistics().estimated_print_time-(before_time+double(before_pixels.size())))<1e-9);
    REQUIRE_FALSE(events.empty());
    CHECK(events.back().percent==100);CHECK(events.back().text=="Slicing done");
    CHECK(std::count_if(events.begin(),events.end(),[](const auto &event){return event.percent>=100;})==1);
    CHECK(std::any_of(events.begin(),events.end(),[](const auto &event){return event.text=="Generating statistics" && event.percent==99;}));
    for(size_t i=1;i<events.size();++i)CHECK(events[i].percent>=events[i-1].percent);
}

namespace {
bool same_export_globals(const sla::LayerJobParameters &a,const sla::LayerJobParameters &b) {
    return a.layer_height==b.layer_height && a.bottom_layers==b.bottom_layers &&
        a.transition_layers==b.transition_layers && a.normal==b.normal && a.bottom==b.bottom;
}
struct EditedExportFixture {
    std::unique_ptr<SLAPrint> source;
    sla::LayerDocument edited;
    std::vector<std::vector<uint8_t>> expected;
    EditedExportFixture() {
        SLAFullPrintConfig full;Test::configure_archive_fixture(full,"SL1");
        full.set("display_pixels_x",8);full.set("display_pixels_y",6);
        full.set("display_width",8.);full.set("display_height",6.);
        full.set("sla_bottom_layers",1);full.set("sla_transition_layers",0);
        DynamicPrintConfig config;config.apply(full);
        std::vector<sla::EncodedRaster> rasters;
        for(size_t i=0;i<3;++i) {
            std::vector<uint8_t> pixels(48,0);pixels[i]=255;
            rasters.push_back(sla::PNGRasterEncoder{}(pixels.data(),8,6,1));
            expected.push_back(std::move(pixels));
        }
        const auto masks=sla::LayerDocument::from_rasters(std::move(rasters));
        source=SLAPrint::create_raster_job(config,masks,{.05,.1,.15},sla::FreshLayerProducer::Raster);
        auto fresh=*source->layer_document()->fresh_job();
        using F=sla::LayerParameter;
        // Deliberately differ from the immutable print's masks AND metadata.
        // This is Export without Apply: capture_export_snapshot(source) would
        // silently restore the wrong input if used as an implementation shortcut.
        fresh.globals.bottom[F::Exposure]=9.5;
        fresh.globals.normal[F::Exposure]=3.25;
        for(size_t i=0;i<3;++i) {
            fresh.rows[i][F::PositionZ]=.075+.05*double(i);
            fresh.rows[i][F::Exposure]=i==0?9.5:3.25;
        }
        std::vector<sla::EncodedRaster> owned;
        for(const auto &pixels:expected)owned.push_back(sla::PNGRasterEncoder{}(pixels.data(),8,6,1));
        edited=sla::LayerDocument::from_fresh_rasters(std::move(owned),std::move(fresh));
        edited.invert(0);
        for(auto &value:expected[0])value=uint8_t(255-value);
    }
};
std::map<std::string,double> exported_sl1_numbers(const boost::filesystem::path &path) {
    // Read the literal ZIP entries: read_zipper_archive deliberately moves
    // these two names into parsed profile/config trees, not its entries list.
    struct Archive {
        mz_zip_archive zip{};
        ~Archive() { mz_zip_reader_end(&zip); }
    } archive;
    REQUIRE(mz_zip_reader_init_file(&archive.zip,path.string().c_str(),0));
    std::map<std::string,double> values;
    for(const std::string entry_name:{"config.ini","prusaslicer.ini"}) {
        const int index=mz_zip_reader_locate_file(&archive.zip,entry_name.c_str(),nullptr,0);
        REQUIRE(index>=0);
        mz_zip_archive_file_stat entry{};
        REQUIRE(mz_zip_reader_file_stat(&archive.zip,mz_uint(index),&entry));
        std::string bytes(size_t(entry.m_uncomp_size),'\0');
        REQUIRE(mz_zip_reader_extract_to_mem(&archive.zip,mz_uint(index),bytes.data(),bytes.size(),0));
        std::istringstream text(bytes);
        for(std::string line;std::getline(text,line);) {
            const auto equal=line.find('=');if(equal==std::string::npos)continue;
            std::string key=line.substr(0,equal);boost::trim(key);
            if(entry_name=="prusaslicer.ini")key="slicer."+key;
            std::istringstream number(line.substr(equal+1));double value;
            if(number>>value)values.emplace(std::move(key),value);
        }
    }
    return values;
}
}

TEST_CASE("Edited layer export cancels every reachable checkpoint without cancelling its source",
          "[sla_editor_export_cancel][sla_export_cancel]") {
    const std::string format=GENERATE("SL1","SL1S");
    CAPTURE(format);Directory directory;EditedExportFixture fixture;
    const auto output=directory.path/(format=="SL1"?"owned.sl1":"owned.sl1s");
    const auto source_revision=fixture.source->layer_document()->revision();
    const auto source_generation=fixture.source->export_generation();
    const auto source_config=fixture.source->full_print_config();
    const auto source_stats=fixture.source->print_statistics();
    const auto source_fresh=*fixture.source->layer_document()->fresh_job();
    const auto edited_revision=fixture.edited.revision();
    const auto edited_rows=fixture.edited.fresh_job()->rows;
    const auto edited_globals=fixture.edited.fresh_job()->globals;
    const std::vector<uint8_t> sentinel={'o','l','d'};
    size_t complete_checks=0,checks_with_staging=0;
    const auto coordinator=std::this_thread::get_id();
    const auto stage_present=[&] {
        return std::any_of(boost::filesystem::directory_iterator(directory.path),boost::filesystem::directory_iterator{},
            [](const auto &entry) {return entry.path().filename().string().find(".sla-export-")==0;});
    };
    // Discover the finite callback sequence from a successful tiny export.
    // Do not encode magic callback counts or add a production test hook.
    auto successful=fixture.source->export_generation_guard().with_cancellation();
    fixture.source->export_layer_document(fixture.edited,format,output.string(),{},"owned",[&] {
        REQUIRE(std::this_thread::get_id()==coordinator);
        ++complete_checks;if(stage_present())++checks_with_staging;
    },&successful);
    REQUIRE(complete_checks>checks_with_staging);REQUIRE(checks_with_staging>0);
    REQUIRE_FALSE(stage_present());
    const auto verify_payload=[&] {
        const auto decoded=sla::LayerDocument::load_archive(output.string());
        REQUIRE(decoded.layer_count()==3);
        for(size_t i=0;i<3;++i)CHECK(decoded.image(i).buf==fixture.expected[i]);
        const auto numbers=exported_sl1_numbers(output);
        CHECK(numbers.at("expTime")==3.25);CHECK(numbers.at("expTimeFirst")==9.5);
        CHECK(float(numbers.at("slicer.initial_layer_height"))==float(.075));
        CHECK(float(numbers.at("slicer.layer_height"))==float(.05));
        CHECK(float(numbers.at("layerHeight"))==float(.05));
        // 1 mm2 pixels, first mask 47 lit, others one lit; SL1 native Z float32.
        const double expected_ml=(47.*double(float(.075))+
            double(float(.125))-double(float(.075))+
            double(float(.175))-double(float(.125)))/1000.;
        CHECK(std::abs(numbers.at("usedMaterial")-expected_ml)<1e-12);
        CHECK(numbers.at("numSlow")==1);CHECK(numbers.at("numFast")==2);
    };
    verify_payload();
    for(size_t cancel_at=1;cancel_at<=complete_checks;++cancel_at) {
        CAPTURE(cancel_at,complete_checks);size_t visited=0;
        {std::ofstream file(output.string(),std::ios::binary|std::ios::trunc);file<<"old";}
        auto attempt=fixture.source->export_generation_guard().with_cancellation();
        CHECK_THROWS_AS(fixture.source->export_layer_document(fixture.edited,format,output.string(),{},"owned",[&] {
            REQUIRE(std::this_thread::get_id()==coordinator);
            if(++visited==cancel_at)attempt.cancel();
        },&attempt),CanceledException);
        REQUIRE(visited==cancel_at);CHECK(attempt.cancelled());
        CHECK(read_file(output)==sentinel);CHECK_FALSE(stage_present());
        CHECK(std::distance(boost::filesystem::directory_iterator(directory.path),boost::filesystem::directory_iterator{})==1);
        CHECK_FALSE(fixture.source->canceled());
        CHECK(fixture.source->export_generation()==source_generation);
        CHECK(fixture.source->layer_document()->revision()==source_revision);
        CHECK(fixture.source->full_print_config()==source_config);
        CHECK(fixture.source->print_statistics().objects_used_material==source_stats.objects_used_material);
        CHECK(fixture.source->print_statistics().estimated_print_time==source_stats.estimated_print_time);
        for(size_t i=0;i<3;++i) {
            std::vector<uint8_t> source_pixels(48,0);source_pixels[i]=255;
            CHECK(fixture.source->layer_document()->image(i).buf==source_pixels);
        }
        CHECK(fixture.source->layer_document()->fresh_job()->rows==source_fresh.rows);
        CHECK(same_export_globals(fixture.source->layer_document()->fresh_job()->globals,source_fresh.globals));
        CHECK(fixture.edited.revision()==edited_revision);CHECK(fixture.edited.dirty());
        CHECK(fixture.edited.can_undo());CHECK_FALSE(fixture.edited.can_redo());
        CHECK(fixture.edited.fresh_job()->rows==edited_rows);
        CHECK(same_export_globals(fixture.edited.fresh_job()->globals,edited_globals));
        for(size_t i=0;i<3;++i)CHECK(fixture.edited.image(i).buf==fixture.expected[i]);
    }
    auto retry=fixture.source->export_generation_guard().with_cancellation();
    CHECK_FALSE(retry.cancelled());
    fixture.source->export_layer_document(fixture.edited,format,output.string(),{},"owned",{},&retry);
    verify_payload();
    // Publication has won. Cancellation after return cannot revoke its result.
    const auto published=read_file(output);retry.cancel();CHECK(read_file(output)==published);
    CHECK_FALSE(fixture.source->canceled());CHECK(fixture.edited.dirty());
    // Old callers need neither a callback nor a request guard.
    fixture.source->export_layer_document(fixture.edited,format,output.string(),{},"owned");verify_payload();
}

TEST_CASE("Edited layer export rejects foreign guards and preserves non-cancellation failures",
          "[sla_editor_export_cancel][sla_export_cancel]") {
    const bool fail_after_staging=GENERATE(false,true);
    CAPTURE(fail_after_staging);Directory directory;EditedExportFixture fixture,foreign;
    const auto target=directory.path/"preserved.sl1";{std::ofstream file(target.string());file<<"old";}
    const std::vector<uint8_t> sentinel={'o','l','d'};
    auto wrong=foreign.source->export_generation_guard().with_cancellation();
    bool entered=false;
    CHECK_THROWS_AS(fixture.source->export_layer_document(fixture.edited,"SL1",target.string(),{},"owned",
        [&]{entered=true;},&wrong),RuntimeError);
    CHECK_FALSE(entered);CHECK(read_file(target)==sentinel);
    auto current=fixture.source->export_generation_guard().with_cancellation();
    CHECK_THROWS_AS(fixture.source->export_layer_document(fixture.edited,"SL1",target.string(),{},"owned",
        [&] {
            const bool staged=std::any_of(boost::filesystem::directory_iterator(directory.path),boost::filesystem::directory_iterator{},
                [](const auto &entry) {return entry.path().filename().string().find(".sla-export-")==0;});
            if(!fail_after_staging || staged)throw std::logic_error("caller checkpoint failed");
        },&current),std::logic_error);
    CHECK(read_file(target)==sentinel);CHECK_FALSE(current.cancelled());CHECK_FALSE(fixture.source->canceled());
    CHECK(std::distance(boost::filesystem::directory_iterator(directory.path),boost::filesystem::directory_iterator{})==1);
    // Caller may express progress cancellation as LayerOperationCancelled too.
    CHECK_THROWS_AS(fixture.source->export_layer_document(fixture.edited,"SL1",target.string(),{},"owned",
        []{sla::check_layer_progress([](size_t,size_t){return false;});},&current),sla::LayerOperationCancelled);
    CHECK(read_file(target)==sentinel);CHECK(fixture.edited.dirty());CHECK(fixture.edited.can_undo());
}

TEST_CASE("Request cancellation after final callback still wins the publication mutex",
          "[sla_editor_export_cancel][sla_export_commit]") {
    Directory directory;EditedExportFixture fixture;
    const auto target=directory.path/"preserved.sl1";{std::ofstream file(target.string());file<<"old";}
    const auto generation=fixture.source->export_generation_guard().with_cancellation();
    size_t calls=0;
    CHECK_THROWS_AS(sla::publish_export(target.string(),[](const std::string &temporary) {
        std::ofstream file(temporary);file<<"completed bytes";
    },[&] {
        generation.check();
        // Cancel arrives after the callback's own last check, before rename.
        if(++calls==2)generation.cancel();
    },"sl1",&generation),CanceledException);
    CHECK(calls==2);CHECK(read_file(target)==std::vector<uint8_t>{'o','l','d'});
    CHECK(std::distance(boost::filesystem::directory_iterator(directory.path),boost::filesystem::directory_iterator{})==1);
    CHECK_FALSE(fixture.source->canceled());
}

namespace {
void require_session_export_payload(const boost::filesystem::path &path,
                                    const EditedExportFixture &fixture) {
    const auto decoded=sla::LayerDocument::load_archive(path.string());
    REQUIRE(decoded.layer_count()==fixture.expected.size());
    for(size_t i=0;i<fixture.expected.size();++i)
        CHECK(decoded.image(i).buf==fixture.expected[i]);
    const auto numbers=exported_sl1_numbers(path);
    CHECK(numbers.at("expTime")==3.25);CHECK(numbers.at("expTimeFirst")==9.5);
    CHECK(float(numbers.at("slicer.initial_layer_height"))==float(.075));
    CHECK(float(numbers.at("slicer.layer_height"))==float(.05));
    const double expected_ml=(47.*double(float(.075))+
        double(float(.125))-double(float(.075))+
        double(float(.175))-double(float(.125)))/1000.;
    CHECK(std::abs(numbers.at("usedMaterial")-expected_ml)<1e-12);
}
bool session_export_staging_exists(const boost::filesystem::path &directory) {
    return std::any_of(boost::filesystem::directory_iterator(directory),boost::filesystem::directory_iterator{},
        [](const auto &entry) {return entry.path().filename().string().find(".sla-export-")==0;});
}
}

TEST_CASE("Editor request guard belongs to the surviving captured session context",
          "[sla_editor_export_cancel][sla_result_session]") {
    const std::string format=GENERATE("SL1","SL1S");
    const bool destroy_original=GENERATE(false,true);
    CAPTURE(format,destroy_original);Directory directory;EditedExportFixture fixture;
    auto session=sla::SlicedResultSession::capture(*fixture.source);
    REQUIRE(session->source_current());
    // Exercise the actual public editor boundary, including edits that have
    // never been applied to either the source job or the captured context.
    session->document()=fixture.edited;
    const SLAPrint &context=session->context();
    const auto accepted_configuration=session->configuration();
    const auto accepted_statistics=context.print_statistics();
    const auto accepted_generation=context.export_generation();
    const auto document_revision=session->document().revision();
    const auto document_rows=session->document().fresh_job()->rows;
    const auto original_guard=fixture.source->export_generation_guard().with_cancellation();
    auto request=context.export_generation_guard().with_cancellation();
    CHECK(context.export_input_guard().same_source(request));
    CHECK_FALSE(context.export_input_guard().same_source(original_guard));
    if(destroy_original)fixture.source.reset();
    else fixture.source->clear();
    REQUIRE_FALSE(session->source_current());
    CHECK_FALSE(original_guard.current());CHECK(request.current());
    CHECK_FALSE(context.canceled());CHECK(context.finished());
    const auto target=directory.path/(format=="SL1"?"session.sl1":"session.sl1s");
    size_t callbacks=0;const auto coordinator=std::this_thread::get_id();
    REQUIRE_NOTHROW(context.export_layer_document(session->document(),format,target.string(),{},"session",[&] {
        CHECK(std::this_thread::get_id()==coordinator);++callbacks;
    },&request));
    CHECK(callbacks>0);require_session_export_payload(target,fixture);
    CHECK_FALSE(session_export_staging_exists(directory.path));
    CHECK(session->configuration()==accepted_configuration);
    CHECK(context.export_generation()==accepted_generation);
    CHECK(context.print_statistics().objects_used_material==accepted_statistics.objects_used_material);
    CHECK(context.print_statistics().estimated_print_time==accepted_statistics.estimated_print_time);
    CHECK(session->document().revision()==document_revision);
    CHECK(session->document().fresh_job()->rows==document_rows);
    CHECK(session->document().dirty());CHECK(session->document().can_undo());
    CHECK_FALSE(session->document().can_redo());
    // The session's accepted context still holds its original masks; export
    // must not silently Apply the editor document while changing output.
    for(size_t i=0;i<3;++i) {
        std::vector<uint8_t> original(48,0);original[i]=255;
        CHECK(context.layer_document()->image(i).buf==original);
    }
    REQUIRE(session->document().undo());
    CHECK(session->document().can_redo());
    REQUIRE(session->document().redo());
    for(size_t i=0;i<3;++i)CHECK(session->document().image(i).buf==fixture.expected[i]);
}

TEST_CASE("Captured session requests cancel independently and retry with their own context guard",
          "[sla_editor_export_cancel][sla_result_session]") {
    Directory directory;EditedExportFixture fixture;
    auto session=sla::SlicedResultSession::capture(*fixture.source);
    session->document()=fixture.edited;
    const SLAPrint &context=session->context();
    const auto source_guard=fixture.source->export_generation_guard().with_cancellation();
    const auto target=directory.path/"session.sl1";
    const std::vector<uint8_t> sentinel={'o','l','d'};
    {std::ofstream file(target.string());file<<"old";}
    size_t foreign_callbacks=0;
    // Equal accepted configuration/masks do not make the original source's
    // guard a valid binding for the independent session context.
    CHECK_THROWS_AS(context.export_layer_document(session->document(),"SL1",target.string(),{},"session",
        [&]{++foreign_callbacks;},&source_guard),RuntimeError);
    CHECK(foreign_callbacks==0);CHECK(read_file(target)==sentinel);
    CHECK_FALSE(session_export_staging_exists(directory.path));
    auto first=context.export_generation_guard().with_cancellation();
    auto independent=context.export_generation_guard().with_cancellation();
    CHECK(first.current());CHECK(independent.current());
    const auto context_generation=context.export_generation();
    const auto source_generation=fixture.source->export_generation();
    const auto document_revision=session->document().revision();
    bool cancelled_staged_write=false;
    CHECK_THROWS_AS(context.export_layer_document(session->document(),"SL1",target.string(),{},"session",[&] {
        if(session_export_staging_exists(directory.path)) {
            cancelled_staged_write=true;first.cancel();
        }
    },&first),CanceledException);
    REQUIRE(cancelled_staged_write);CHECK(first.cancelled());CHECK_FALSE(independent.cancelled());
    CHECK(read_file(target)==sentinel);CHECK_FALSE(session_export_staging_exists(directory.path));
    CHECK(context.export_generation()==context_generation);CHECK_FALSE(context.canceled());
    CHECK(fixture.source->export_generation()==source_generation);CHECK_FALSE(fixture.source->canceled());
    CHECK(session->source_current());CHECK(session->document().revision()==document_revision);
    CHECK(session->document().dirty());CHECK(session->document().can_undo());CHECK_FALSE(session->document().can_redo());
    // A sibling guard created BEFORE Cancel remains usable, as does a NEW
    // retry created afterwards. Neither shares first's cancellation flag.
    REQUIRE_NOTHROW(context.export_layer_document(session->document(),"SL1",target.string(),{},"session",{},&independent));
    require_session_export_payload(target,fixture);
    auto retry=context.export_generation_guard().with_cancellation();
    CHECK_FALSE(retry.cancelled());
    REQUIRE_NOTHROW(context.export_layer_document(session->document(),"SL1",target.string(),{},"session",{},&retry));
    require_session_export_payload(target,fixture);
    const auto published=read_file(target);size_t cancelled_callbacks=0;
    CHECK_THROWS_AS(context.export_layer_document(session->document(),"SL1",target.string(),{},"session",
        [&]{++cancelled_callbacks;},&first),CanceledException);
    CHECK(cancelled_callbacks==0);CHECK(read_file(target)==published);
    CHECK(context.export_generation()==context_generation);CHECK_FALSE(context.canceled());
    CHECK(session->document().revision()==document_revision);CHECK(session->document().dirty());
    CHECK(session->document().can_undo());CHECK_FALSE(session->document().can_redo());
    for(size_t i=0;i<3;++i)CHECK(session->document().image(i).buf==fixture.expected[i]);
    CHECK_FALSE(session_export_staging_exists(directory.path));
}

TEST_CASE("Same-context stale editor export guards fail before callbacks and preserve the destination",
          "[sla_editor_export_cancel][sla_export_generation]") {
    Directory directory;EditedExportFixture fixture;
    auto stale=fixture.source->export_generation_guard().with_cancellation();
    const auto generation_before=fixture.source->export_generation();
    // This is the public mutable SLAPrint API, before export begins. A session
    // intentionally exposes only const context(), so do not const_cast it to
    // invent a GUI mutation path that its ownership contract forbids.
    auto replacement=*fixture.source->layer_document();replacement.invert(1);
    REQUIRE_NOTHROW(fixture.source->replace_layer_document(replacement));
    REQUIRE(fixture.source->export_generation()!=generation_before);
    REQUIRE(fixture.source->export_input_guard().same_source(stale));
    REQUIRE_FALSE(stale.current());CHECK_FALSE(stale.cancelled());
    CHECK_FALSE(fixture.source->canceled());
    const auto target=directory.path/"stale.sl1";{std::ofstream file(target.string());file<<"old";}
    const auto current_revision=fixture.source->layer_document()->revision();
    const auto document_revision=fixture.edited.revision();size_t callbacks=0;
    CHECK_THROWS_AS(fixture.source->export_layer_document(fixture.edited,"SL1",target.string(),{},"stale",
        [&]{++callbacks;},&stale),CanceledException);
    CHECK(callbacks==0);CHECK(read_file(target)==std::vector<uint8_t>{'o','l','d'});
    CHECK_FALSE(session_export_staging_exists(directory.path));
    CHECK(fixture.source->layer_document()->revision()==current_revision);
    CHECK(fixture.edited.revision()==document_revision);CHECK(fixture.edited.dirty());CHECK(fixture.edited.can_undo());
    auto current=fixture.source->export_generation_guard().with_cancellation();
    REQUIRE_NOTHROW(fixture.source->export_layer_document(fixture.edited,"SL1",target.string(),{},"stale",{},&current));
    require_session_export_payload(target,fixture);
    CHECK_FALSE(fixture.source->canceled());CHECK_FALSE(current.cancelled());
    CHECK(fixture.source->layer_document()->revision()==current_revision);
    CHECK(fixture.source->layer_document()->image(1).buf==replacement.image(1).buf);
    CHECK(fixture.edited.revision()==document_revision);CHECK(fixture.edited.dirty());CHECK(fixture.edited.can_undo());
}

// Public backend prerequisite for the Plater GUI contract. This does not
// pretend to unit-test the private on_process_completed handler.
TEST_CASE("Cancelled deferred export keeps its ready owned SLA result and can retry",
          "[sla_background_export_ready][sla_deferred_request]") {
    const std::string format=GENERATE("SL1","cxdlpv4");
    CAPTURE(format);Directory directory;
    SLAFullPrintConfig full;Test::configure_archive_fixture(full,format);
    full.set("sla_transition_layers",0);
    DynamicPrintConfig config;config.apply(full);
    const size_t columns=full.display_pixels_x.value,rows=full.display_pixels_y.value;
    std::vector<uint8_t> pixels(columns*rows,0);
    std::fill(pixels.begin(),pixels.begin()+pixels.size()/2,255);
    const auto masks=sla::LayerDocument::from_rasters({sla::PNGRasterEncoder{}(pixels.data(),columns,rows,1)});
    auto source=SLAPrint::create_raster_job(config,masks,{.05},sla::FreshLayerProducer::Raster);
    auto edited=*source->layer_document();edited.invert(0);source->replace_layer_document(edited);
    const auto expected=edited.image(0).buf;
    const auto generation=source->export_generation();const auto revision=source->layer_document()->revision();
    const auto rows_before=source->layer_document()->fresh_job()->rows;
    const auto target=directory.path/("result."+std::string(get_archive_entry(format.c_str())->ext));
    {std::ofstream file(target.string());file<<"old";}
    auto snapshot=std::make_shared<const sla::ExportSnapshot>(source->capture_export_snapshot());
    const sla::DeferredExportRequest intent(source->export_input_guard(),format,target.string());
    const auto request=intent.bind(snapshot);bool cancelled_after_staging=false;
    CHECK_THROWS_AS(request.write([&] {
        for(const auto &entry:boost::filesystem::directory_iterator(directory.path))
            if(entry.path().filename().string().find(".sla-export-")==0 && boost::filesystem::file_size(entry.path())>0) {
                cancelled_after_staging=true;intent.cancel();return;
            }
    }),CanceledException);
    REQUIRE(cancelled_after_staging);CHECK(intent.cancelled());CHECK(request.cancelled());
    CHECK(read_file(target)==std::vector<uint8_t>{'o','l','d'});
    CHECK(std::distance(boost::filesystem::directory_iterator(directory.path),boost::filesystem::directory_iterator{})==1);
    CHECK(source->finished());CHECK_FALSE(source->canceled());CHECK(source->export_generation()==generation);
    CHECK(source->layer_document()->revision()==revision);CHECK(source->layer_document()->image(0).buf==expected);
    CHECK(source->layer_document()->fresh_job()->rows==rows_before);
    CHECK(source->layer_document()->dirty());CHECK(source->layer_document()->can_undo());
    // The main worker separately calls cancel/restart for its state machine.
    // SLA cleanup is a no-op: the cached accepted result survives that cycle.
    source->cancel();source->cleanup();source->restart();REQUIRE(source->finished());
    const sla::DeferredExportRequest retry(source->export_input_guard(),format,target.string());
    REQUIRE_NOTHROW(retry.bind(snapshot).write());
    CHECK(sla::LayerDocument::load_archive(target.string()).image(0).buf==expected);
    CHECK(source->finished());CHECK_FALSE(source->canceled());CHECK(source->export_generation()==generation);
    CHECK(source->layer_document()->revision()==revision);CHECK(source->layer_document()->can_undo());
}


TEST_CASE("Public SL1 export cannot borrow a callback-mutable document",
          "[sla_sl1_png_ownership][sla_editor_export_cancel]") {
    const std::string format = GENERATE("SL1", "SL1S");
    const bool ready_editor_context = GENERATE(false, true);
    CAPTURE(format, ready_editor_context);
    Directory directory;
    EditedExportFixture fixture;
    auto session = sla::SlicedResultSession::capture(*fixture.source);
    const SLAPrint &context = ready_editor_context ? session->context() : *fixture.source;
    REQUIRE(context.finished());
    const auto target = directory.path / (format == "SL1" ? "preserved.sl1" : "preserved.sl1s");
    { std::ofstream file(target.string()); file << "old"; }
    const auto context_generation = context.export_generation();
    const auto source_revision = fixture.source->layer_document()->revision();
    bool changed = false;
    auto request = context.export_generation_guard().with_cancellation();
    // Intentional adversarial reentrancy outside the public immutability
    // precondition: keep the owned-buffer fallback's existing rejection.
    // Staging exists only after encode_layers has completed. Moving one lit
    // pixel preserves material/Z metadata; exact mask readback must catch it.
    CHECK_THROWS_AS(context.export_layer_document(fixture.edited, format, target.string(), {}, "owned", [&] {
        if (!changed && session_export_staging_exists(directory.path)) {
            auto image = fixture.edited.image(1);
            REQUIRE(image.buf[1] == 255);
            REQUIRE(image.buf[7] == 0);
            std::swap(image.buf[1], image.buf[7]);
            changed = true;
            fixture.edited.replace_image(1, image);
        }
    }, &request), RuntimeError);
    REQUIRE(changed);
    CHECK(read_file(target) == std::vector<uint8_t>{'o', 'l', 'd'});
    CHECK_FALSE(session_export_staging_exists(directory.path));
    CHECK(request.current());
    CHECK_FALSE(request.cancelled());
    CHECK(context.export_generation() == context_generation);
    CHECK(fixture.source->layer_document()->revision() == source_revision);
    // A new ordinary public call may export the now-stable edited document.
    REQUIRE_NOTHROW(context.export_layer_document(fixture.edited, format, target.string(), {}, "retry"));
    const auto decoded = sla::LayerDocument::load_archive(target.string());
    REQUIRE(decoded.layer_count() == fixture.edited.layer_count());
    for (size_t i = 0; i < decoded.layer_count(); ++i)
        CHECK(decoded.image(i).buf == fixture.edited.image(i).buf);
}


namespace {
struct Sl1PngEnvironment {
    std::string name;
    std::optional<std::string> previous;
    Sl1PngEnvironment(std::string variable, const char *value) : name(std::move(variable)) {
        if (const char *old = std::getenv(name.c_str())) previous = old;
        set(value);
    }
    void set(const char *value) const {
#ifdef _WIN32
        _putenv_s(name.c_str(), value ? value : "");
#else
        if (value) setenv(name.c_str(), value, 1);
        else unsetenv(name.c_str());
#endif
    }
    ~Sl1PngEnvironment() { set(previous ? previous->c_str() : nullptr); }
};
// Observe the actual selected path and completed ZIP consumption. Equal files
// alone cannot distinguish an optimized writer from its original fallback.
struct Sl1PngTrace {
    using Sink = boost::log::sinks::synchronous_sink<boost::log::sinks::text_ostream_backend>;
    Sl1PngEnvironment enabled{"SLA_TRACE_EXPORT_STAGES", "1"};
    unsigned previous_level = get_logging_level();
    boost::shared_ptr<std::ostringstream> text = boost::make_shared<std::ostringstream>();
    boost::shared_ptr<Sink> sink = boost::make_shared<Sink>();
    Sl1PngTrace() {
        sink->locked_backend()->add_stream(text);
        sink->set_formatter(boost::log::expressions::stream << boost::log::expressions::smessage);
        boost::log::core::get()->add_sink(sink);
        set_logging_level(3);
    }
    ~Sl1PngTrace() {
        boost::log::core::get()->remove_sink(sink);
        set_logging_level(previous_level);
    }
    size_t count(const char *prefix) {
        sink->flush();
        std::istringstream lines(text->str());
        size_t result = 0;
        for (std::string line; std::getline(lines, line);)
            result += line.rfind(prefix, 0) == 0;
        return result;
    }
    void expect(size_t selected, size_t consumed, size_t encoded) {
        INFO(text->str());
        CHECK(count("Layer export PNG encoding bypass selected:") == selected);
        CHECK(count("Layer export PNG document consumed:") == consumed);
        CHECK(count("Layer export masks encoded:") == encoded);
        text->str(std::string{});
        text->clear();
    }
};
struct Sl1PngFixture {
    EditedExportFixture data;
    Sl1PngFixture() {
        auto image = data.edited.image(0);
        std::fill(image.buf.begin(), image.buf.end(), uint8_t(0));
        const std::vector<uint8_t> gray{0, 1, 127, 128, 254, 255};
        std::copy(gray.begin(), gray.end(), image.buf.begin());
        data.edited.replace_image(0, image);
        data.expected[0] = image.buf;
        const auto configuration = data.source->full_print_config();
        data.source = SLAPrint::create_raster_job(configuration, data.edited, {.075, .125, .175});
    }
    std::shared_ptr<const sla::ExportSnapshot> snapshot() const {
        return std::make_shared<const sla::ExportSnapshot>(data.source->capture_export_snapshot());
    }
};
std::map<std::string, double> require_sl1_png_payload(const boost::filesystem::path &path,
                                                     const sla::LayerDocument &expected) {
    struct Zip {
        mz_zip_archive archive{};
        ~Zip() { mz_zip_reader_end(&archive); }
    } zip;
    REQUIRE(mz_zip_reader_init_file(&zip.archive, path.string().c_str(), 0));
    REQUIRE(expected.layer_count() == 3);
    for (size_t i = 0; i < expected.layer_count(); ++i) {
        const auto name = "owned0000" + std::to_string(i) + ".png";
        const int index = mz_zip_reader_locate_file(&zip.archive, name.c_str(), nullptr, 0);
        REQUIRE(index >= 0);
        mz_zip_archive_file_stat entry{};
        REQUIRE(mz_zip_reader_file_stat(&zip.archive, mz_uint(index), &entry));
        const auto view = expected.encoded_layer(i);
        REQUIRE(entry.m_uncomp_size == view.sz);
        std::vector<uint8_t> actual(view.sz);
        REQUIRE(mz_zip_reader_extract_to_mem(&zip.archive, mz_uint(index), actual.data(), actual.size(), 0));
        const auto *begin = static_cast<const uint8_t *>(view.buf);
        CHECK(actual == std::vector<uint8_t>(begin, begin + view.sz));
    }
    const auto decoded = sla::LayerDocument::load_archive(path.string());
    REQUIRE(decoded.layer_count() == expected.layer_count());
    for (size_t i = 0; i < expected.layer_count(); ++i) {
        const auto image = decoded.image(i);
        CHECK(image.cols == 8);
        CHECK(image.rows == 6);
        CHECK(image.buf == expected.image(i).buf);
    }
    auto numbers = exported_sl1_numbers(path);
    CHECK(numbers.at("expTime") == 3.25);
    CHECK(numbers.at("expTimeFirst") == 9.5);
    CHECK(float(numbers.at("slicer.initial_layer_height")) == float(.075));
    CHECK(float(numbers.at("layerHeight")) == float(.05));
    CHECK(numbers.at("numSlow") == 1);
    CHECK(numbers.at("numFast") == 2);
    const double material_ml = (5. * double(float(.075)) +
        double(float(.125)) - double(float(.075)) +
        double(float(.175)) - double(float(.125))) / 1000.;
    CHECK(std::abs(numbers.at("usedMaterial") - material_ml) < 1e-12);
    return numbers;
}
}

TEST_CASE("Private frozen SL1 borrowing preserves literal PNGs and public fallback",
          "[sla_sl1_png_frozen][sla_sl1_png_ownership]") {
    const std::string format = GENERATE("SL1", "SL1S");
    CAPTURE(format);
    Directory directory;
    Sl1PngFixture fixture;
    const auto snapshot = fixture.snapshot();
    REQUIRE(snapshot->document());
    const auto &document = *snapshot->document();
    const auto revision = fixture.data.source->layer_document()->revision();
    const auto generation = fixture.data.source->export_generation();
    const auto rows = document.fresh_job()->rows;
    const auto globals = document.fresh_job()->globals;
    std::optional<std::map<std::string, double>> baseline_numbers;
    Sl1PngTrace trace;
    for (const char *mode : {"0", "1"}) {
        CAPTURE(mode);
        Sl1PngEnvironment policy("SLA_EXPERIMENT_SL1_BORROW_PNG", mode);
        const bool borrowed = std::strcmp(mode, "1") == 0;
        const auto output = directory.path / (std::string("frozen-") + mode + "." + format);
        const sla::ExportRequest request(snapshot, format, output.string(), "owned");
        REQUIRE_NOTHROW(request.write());
        trace.expect(borrowed ? 1 : 0, borrowed ? 1 : 0, borrowed ? 0 : 1);
        const auto numbers = require_sl1_png_payload(output, document);
        if (baseline_numbers) CHECK(numbers == *baseline_numbers);
        else baseline_numbers = numbers;
        // A normal public argument cannot grant the private borrowing capability.
        const auto public_output = directory.path / (std::string("public-") + mode + "." + format);
        fixture.data.source->export_layer_document(fixture.data.edited, format, public_output.string(), {}, "owned");
        trace.expect(0, 0, 1);
        CHECK(require_sl1_png_payload(public_output, document) == *baseline_numbers);
        // finished=true on the actual captured editor context is insufficient too.
        auto session = sla::SlicedResultSession::capture(*fixture.data.source);
        REQUIRE(session->context().finished());
        const auto editor_output = directory.path / (std::string("editor-") + mode + "." + format);
        session->context().export_layer_document(session->document(), format, editor_output.string(), {}, "owned");
        trace.expect(0, 0, 1);
        CHECK(require_sl1_png_payload(editor_output, document) == *baseline_numbers);
        CHECK(document.fresh_job()->rows == rows);
        CHECK(same_export_globals(document.fresh_job()->globals, globals));
        CHECK(fixture.data.source->layer_document()->revision() == revision);
        CHECK(fixture.data.source->export_generation() == generation);
        CHECK(snapshot->generation().current());
        CHECK_FALSE(session_export_staging_exists(directory.path));
    }
}

TEST_CASE("Private frozen SL1 borrowing survives original-source retirement at a checkpoint",
          "[sla_sl1_png_frozen][sla_frozen_request]") {
    const std::string format = GENERATE("SL1", "SL1S");
    const std::string action = GENERATE("cancel", "clear", "destroy");
    CAPTURE(format, action);
    Directory directory;
    Sl1PngFixture fixture;
    const auto snapshot = fixture.snapshot();
    const auto target = directory.path / ("old." + format);
    { std::ofstream file(target.string()); file << "old"; }
    Sl1PngEnvironment policy("SLA_EXPERIMENT_SL1_BORROW_PNG", "1");
    Sl1PngTrace trace;
    const sla::ExportRequest request(snapshot, format, target.string(), "owned");
    bool reached = false;
    CHECK_THROWS_AS(request.write([&] {
        if (!reached && session_export_staging_exists(directory.path)) {
            reached = true;
            if (action == "cancel") request.cancel();
            else if (action == "clear") fixture.data.source->clear();
            else fixture.data.source.reset();
        }
    }), CanceledException);
    REQUIRE(reached);
    trace.expect(1, 0, 0);
    CHECK(read_file(target) == std::vector<uint8_t>{'o', 'l', 'd'});
    CHECK_FALSE(session_export_staging_exists(directory.path));
    CHECK(snapshot->generation().current() == (action == "cancel"));
    // Retired generations cannot be retried. Build a fresh ordinary source
    // from the retained immutable masks, then use a new request and guard.
    auto retry_source = SLAPrint::create_raster_job(snapshot->configuration(), *snapshot->document(), {.075, .125, .175});
    const auto retry_snapshot = std::make_shared<const sla::ExportSnapshot>(retry_source->capture_export_snapshot());
    const sla::ExportRequest retry(retry_snapshot, format, target.string(), "owned");
    REQUIRE_NOTHROW(retry.write());
    trace.expect(1, 1, 0);
    require_sl1_png_payload(target, *snapshot->document());
    CHECK(retry_snapshot->generation().current());
}

TEST_CASE("Private frozen SL1 borrowing still validates the completed archive before publication",
          "[sla_sl1_png_frozen][sla_export_publication]") {
    const std::string format = GENERATE("SL1", "SL1S");
    CAPTURE(format);
    Directory directory;
    Sl1PngFixture fixture;
    const auto snapshot = fixture.snapshot();
    const auto target = directory.path / ("old." + format);
    { std::ofstream file(target.string()); file << "old"; }
    Sl1PngEnvironment policy("SLA_EXPERIMENT_SL1_BORROW_PNG", "1");
    Sl1PngTrace trace;
    const sla::ExportRequest request(snapshot, format, target.string(), "owned");
    bool damaged = false;
    CHECK_THROWS_AS(request.write([&] {
        // The real completion trace occurs after Zipper::finalize and before
        // sl1_verify_fresh_archive's first public checkpoint. No timing hook.
        if (!damaged && trace.count("Layer export PNG document consumed:") == 1) {
            for (const auto &entry : boost::filesystem::directory_iterator(directory.path)) {
                if (entry.path().filename().string().find(".sla-export-") != 0) continue;
                std::ofstream file(entry.path().string(), std::ios::binary | std::ios::trunc);
                REQUIRE(file.good());
                file << "broken ZIP";
                file.close();
                REQUIRE(file.good());
                damaged = true;
            }
        }
    }), RuntimeError);
    REQUIRE(damaged);
    trace.expect(1, 1, 0);
    CHECK(read_file(target) == std::vector<uint8_t>{'o', 'l', 'd'});
    CHECK_FALSE(session_export_staging_exists(directory.path));
    CHECK(snapshot->generation().current());
    CHECK_FALSE(request.cancelled());
    REQUIRE_NOTHROW(request.write());
    trace.expect(1, 1, 0);
    require_sl1_png_payload(target, *snapshot->document());
}

TEST_CASE("SL1 encoded readback is selected only for a validated private borrowed document",
          "[sla_sl1_encoded_readback]") {
    const std::string format = GENERATE("SL1", "SL1S");
    CAPTURE(format);
    Directory directory;
    Sl1PngFixture fixture;
    const auto snapshot = fixture.snapshot();
    const auto &document = *snapshot->document();
    const auto generation = fixture.data.source->export_generation();
    const auto revision = fixture.data.source->layer_document()->revision();
    const auto rows = document.fresh_job()->rows;
    const auto globals = document.fresh_job()->globals;
    for (size_t i = 0; i < document.layer_count(); ++i)
        REQUIRE(document.encoded_layer(i).sz < (size_t(1) << 20));
    Sl1PngTrace trace;
    std::optional<std::map<std::string, double>> baseline;
    for (const char *borrow_mode : {"0", "1"}) {
        CAPTURE(borrow_mode);
        Sl1PngEnvironment borrow("SLA_EXPERIMENT_SL1_BORROW_PNG", borrow_mode);
        for (const char *compare_mode : std::array<const char *, 2>{"0", "1"}) {
            INFO("compare mode: " << (compare_mode ? compare_mode : "unset"));
            Sl1PngEnvironment compare("SLA_EXPERIMENT_SL1_COMPARE_ENCODED", compare_mode);
            const bool borrowed = std::strcmp(borrow_mode, "1") == 0;
            const bool compared = borrowed && std::strcmp(compare_mode, "1") == 0;
            const auto target = directory.path / (std::string("owned-") + borrow_mode +
                "-" + (compare_mode ? compare_mode : "unset") + "." + format);
            const sla::ExportRequest request(snapshot, format, target.string(), "owned");
            REQUIRE_NOTHROW(request.write());
            const size_t expected_chunks = compared ? 3 : 0;
            CHECK(trace.count("SL1 encoded readback chunk:") == expected_chunks);
            trace.expect(borrowed ? 1 : 0, borrowed ? 1 : 0, borrowed ? 0 : 1);
            const auto numbers = require_sl1_png_payload(target, document);
            if (baseline) CHECK(numbers == *baseline); else baseline = numbers;
            if (compared) {
                // The opt-in environment and a ready context do not grant the
                // private proof to a public document, including the real editor.
                const auto public_path = directory.path / ("public." + format);
                REQUIRE_NOTHROW(fixture.data.source->export_layer_document(fixture.data.edited,
                    format, public_path.string(), {}, "owned"));
                CHECK(trace.count("SL1 encoded readback chunk:") == 0);
                trace.expect(0, 0, 1);
                CHECK(require_sl1_png_payload(public_path, document) == *baseline);
                auto session = sla::SlicedResultSession::capture(*fixture.data.source);
                const auto editor_path = directory.path / ("editor." + format);
                REQUIRE_NOTHROW(session->context().export_layer_document(session->document(),
                    format, editor_path.string(), {}, "owned"));
                CHECK(trace.count("SL1 encoded readback chunk:") == 0);
                trace.expect(0, 0, 1);
                CHECK(require_sl1_png_payload(editor_path, document) == *baseline);
            }
            CHECK(fixture.data.source->export_generation() == generation);
            CHECK(fixture.data.source->layer_document()->revision() == revision);
            CHECK(document.fresh_job()->rows == rows);
            CHECK(same_export_globals(document.fresh_job()->globals, globals));
            CHECK(snapshot->generation().current());
            CHECK_FALSE(session_export_staging_exists(directory.path));
        }
    }
}

namespace {
// Mutate a completed PNG while retaining valid PNG and ZIP CRCs. Cover
// same-size byte differences and moved exposure pixels with unchanged volume.
enum class Sl1PngMutation { Annotation, DeflateHeader, MovePixel };
void mutate_sl1_png(const boost::filesystem::path &path, Sl1PngMutation mutation) {
    struct Archive {
        mz_zip_archive zip{};
        ~Archive() { mz_zip_reader_end(&zip); }
    };
    std::vector<std::pair<std::string, std::vector<uint8_t>>> entries;
    {
        Archive input;
        REQUIRE(mz_zip_reader_init_file(&input.zip, path.string().c_str(), 0));
        for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&input.zip); ++i) {
            mz_zip_archive_file_stat stat{};
            REQUIRE(mz_zip_reader_file_stat(&input.zip, i, &stat));
            entries.emplace_back(stat.m_filename, std::vector<uint8_t>(size_t(stat.m_uncomp_size)));
            auto &bytes = entries.back().second;
            REQUIRE(mz_zip_reader_extract_to_mem(&input.zip, i, bytes.data(), bytes.size(), 0));
        }
    }
    size_t changed = 0;
    for (auto &[name, bytes] : entries) if (name == "owned00001.png") {
        REQUIRE(bytes.size() >= 12);
        REQUIRE(std::memcmp(bytes.data() + bytes.size() - 8, "IEND", 4) == 0);
        png::ImageGreyscale original, modified;
        REQUIRE(png::decode_png({bytes.data(), bytes.size()}, original));
        if (mutation == Sl1PngMutation::MovePixel) {
            REQUIRE(original.buf[1] == 255); REQUIRE(original.buf[7] == 0);
            auto changed_image = original;
            std::swap(changed_image.buf[1], changed_image.buf[7]);
            const auto replacement = sla::PNGRasterEncoder{}(changed_image.buf.data(), original.cols, original.rows, 1);
            const auto *begin = static_cast<const uint8_t *>(replacement.data());
            bytes.assign(begin, begin + replacement.size());
        } else if (mutation == Sl1PngMutation::DeflateHeader) {
            // A valid alternative zlib compression-level hint changes bytes
            // without changing payload, PNG size, pixels or exposure metadata.
            const auto read_word = [&](size_t pos) {
                return (uint32_t(bytes[pos]) << 24) | (uint32_t(bytes[pos+1]) << 16) |
                    (uint32_t(bytes[pos+2]) << 8) | uint32_t(bytes[pos+3]);
            };
            bool found = false;
            for (size_t pos = 8; pos + 12 <= bytes.size();) {
                const size_t length = read_word(pos);
                REQUIRE(length <= bytes.size() - pos - 12);
                if (std::memcmp(bytes.data()+pos+4, "IDAT", 4) == 0) {
                    REQUIRE(length >= 2);
                    const uint8_t old = bytes[pos+9];
                    REQUIRE((old & 32) == 0);
                    const unsigned level = ((old >> 6) + 1) % 4;
                    unsigned flag = level << 6;
                    flag += (31 - ((unsigned(bytes[pos+8]) * 256 + flag) % 31)) % 31;
                    REQUIRE(flag != old); bytes[pos+9] = uint8_t(flag);
                    uint32_t crc = uint32_t(mz_crc32(MZ_CRC32_INIT, bytes.data()+pos+4, length+4));
                    for (int i = 0; i < 4; ++i) bytes[pos+8+length+i] = uint8_t(crc >> (24-8*i));
                    found = true; break;
                }
                pos += length + 12;
            }
            REQUIRE(found);
        } else {
        const std::vector<uint8_t> text{'Q','A',0,'b','y','t','e','s'};
        std::vector<uint8_t> chunk;
        const auto word = [&chunk](uint32_t value) {
            for (int shift : {24,16,8,0}) chunk.push_back(uint8_t(value >> shift));
        };
        word(uint32_t(text.size()));
        chunk.insert(chunk.end(), {'t','E','X','t'});
        chunk.insert(chunk.end(), text.begin(), text.end());
        const auto crc = mz_crc32(MZ_CRC32_INIT, chunk.data() + 4, chunk.size() - 4);
        word(uint32_t(crc));
        bytes.insert(bytes.end() - 12, chunk.begin(), chunk.end());
        }
        REQUIRE(png::decode_png({bytes.data(), bytes.size()}, modified));
        REQUIRE(original.cols == modified.cols); REQUIRE(original.rows == modified.rows);
        if (mutation == Sl1PngMutation::MovePixel) {
            REQUIRE(original.buf != modified.buf);
            REQUIRE(std::count(original.buf.begin(), original.buf.end(), uint8_t(0)) ==
                    std::count(modified.buf.begin(), modified.buf.end(), uint8_t(0)));
        } else REQUIRE(original.buf == modified.buf);
        ++changed;
    }
    REQUIRE(changed == 1);
    Zipper output(path.string(), Zipper::FAST_COMPRESSION);
    for (const auto &[name, bytes] : entries) output.add_entry(name.c_str(), bytes.data(), bytes.size());
    output.finalize();
}
}

TEST_CASE("SL1 readback rejects valid PNG mutations before publication",
          "[sla_sl1_encoded_readback][sla_export_publication]") {
    const std::string format = GENERATE("SL1", "SL1S");
    const bool compared = GENERATE(false, true);
    const auto mutation = GENERATE(Sl1PngMutation::Annotation, Sl1PngMutation::DeflateHeader, Sl1PngMutation::MovePixel);
    CAPTURE(format, compared, int(mutation));
    Directory directory;
    Sl1PngFixture fixture;
    const auto snapshot = fixture.snapshot();
    const auto target = directory.path / ("preserved." + format);
    { std::ofstream file(target.string()); file << "old"; }
    Sl1PngEnvironment borrow("SLA_EXPERIMENT_SL1_BORROW_PNG", "1");
    Sl1PngEnvironment compare("SLA_EXPERIMENT_SL1_COMPARE_ENCODED", compared ? "1" : "0");
    Sl1PngTrace trace;
    const sla::ExportRequest request(snapshot, format, target.string(), "owned");
    bool changed = false;
    const auto amend_completed_zip = [&] {
        if (changed || trace.count("Layer export PNG document consumed:") != 1) return;
        for (const auto &entry : boost::filesystem::directory_iterator(directory.path)) {
            if (entry.path().filename().string().find(".sla-export-") != 0) continue;
            mutate_sl1_png(entry.path(), mutation); changed = true;
        }
    };
    const bool reject = compared || mutation == Sl1PngMutation::MovePixel;
    if (reject) CHECK_THROWS_AS(request.write(amend_completed_zip), RuntimeError);
    else REQUIRE_NOTHROW(request.write(amend_completed_zip));
    REQUIRE(changed);
    CHECK(trace.count("SL1 encoded readback chunk:") == (compared ? 1 : 0));
    trace.expect(1, 1, 0);
    if (reject) CHECK(read_file(target) == std::vector<uint8_t>{'o','l','d'});
    else {
        const auto decoded = sla::LayerDocument::load_archive(target.string());
        REQUIRE(decoded.layer_count() == 3);
        for (size_t i = 0; i < 3; ++i)
            CHECK(decoded.image(i).buf == snapshot->document()->image(i).buf);
    }
    CHECK_FALSE(session_export_staging_exists(directory.path));
    CHECK(snapshot->generation().current()); CHECK_FALSE(request.cancelled());
    REQUIRE_NOTHROW(request.write());
    CHECK(trace.count("SL1 encoded readback chunk:") == (compared ? 3 : 0));
    trace.expect(1, 1, 0);
    require_sl1_png_payload(target, *snapshot->document());
}

TEST_CASE("Private SL1 encoded readback cancels after a completed PNG chunk and retries",
          "[sla_sl1_encoded_readback][sla_export_cancel][sla_frozen_request]") {
    const std::string format = GENERATE("SL1", "SL1S");
    CAPTURE(format);
    constexpr size_t width = 1280, height = 1024, chunk_size = 1024 * 1024;
    Directory directory;
    EditedExportFixture fixture;
    auto configuration = fixture.source->full_print_config();
    configuration.set_key_value("display_pixels_x", new ConfigOptionInt(int(width)));
    configuration.set_key_value("display_pixels_y", new ConfigOptionInt(int(height)));
    configuration.set_key_value("display_width", new ConfigOptionFloat(128.));
    configuration.set_key_value("display_height", new ConfigOptionFloat(102.4));
    auto fresh = *fixture.edited.fresh_job();
    fresh.geometry = {width, height, 128., 102.4};
    std::vector<std::vector<uint8_t>> expected_pixels, expected_png;
    std::vector<sla::EncodedRaster> rasters;
    // Deterministic high-entropy Gray8 bytes make the actual compressed PNG
    // exceed one comparison chunk. No oversized model or test-only writer.
    for (size_t layer = 0; layer < 3; ++layer) {
        uint32_t state = uint32_t(0x9e3779b9u + layer);
        std::vector<uint8_t> pixels(width * height);
        for (auto &pixel : pixels) {
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            pixel = uint8_t(state >> 24);
        }
        rasters.push_back(sla::PNGRasterEncoder{}(pixels.data(), width, height, 1));
        expected_pixels.push_back(std::move(pixels));
    }
    fixture.edited = sla::LayerDocument::from_fresh_rasters(std::move(rasters), std::move(fresh));
    fixture.source = SLAPrint::create_raster_job(configuration, fixture.edited, {.075, .125, .175});
    const auto snapshot = std::make_shared<const sla::ExportSnapshot>(fixture.source->capture_export_snapshot());
    REQUIRE(snapshot->document());
    const auto &document = *snapshot->document();
    REQUIRE(document.layer_count() == 3);
    REQUIRE(document.fresh_job());
    REQUIRE(document.fresh_job()->mask_precision == sla::LayerMaskPrecision::Gray8);
    size_t expected_chunks = 0;
    for (size_t layer = 0; layer < document.layer_count(); ++layer) {
        const auto view = document.encoded_layer(layer);
        REQUIRE(view.sz > chunk_size);
        const auto *bytes = static_cast<const uint8_t *>(view.buf);
        expected_png.emplace_back(bytes, bytes + view.sz);
        expected_chunks += (view.sz + chunk_size - 1) / chunk_size;
    }
    const auto generation = fixture.source->export_generation();
    const auto revision = fixture.source->layer_document()->revision();
    const auto rows = document.fresh_job()->rows;
    const auto globals = document.fresh_job()->globals;
    const auto unchanged = [&] {
        CHECK(fixture.source->export_generation() == generation);
        CHECK(fixture.source->layer_document()->revision() == revision);
        CHECK_FALSE(fixture.source->canceled());
        CHECK(snapshot->generation().current());
        CHECK(document.fresh_job()->rows == rows);
        CHECK(same_export_globals(document.fresh_job()->globals, globals));
        CHECK(fixture.source->layer_document()->fresh_job()->rows == rows);
        for (size_t layer = 0; layer < 3; ++layer) {
            const auto view = document.encoded_layer(layer);
            const auto *bytes = static_cast<const uint8_t *>(view.buf);
            CHECK(std::vector<uint8_t>(bytes, bytes + view.sz) == expected_png[layer]);
            CHECK(document.image(layer).buf == expected_pixels[layer]);
            const auto original = fixture.source->layer_document()->encoded_layer(layer);
            const auto *source_bytes = static_cast<const uint8_t *>(original.buf);
            CHECK(std::vector<uint8_t>(source_bytes, source_bytes + original.sz) == expected_png[layer]);
        }
    };
    const auto target = directory.path / ("old." + format);
    const std::vector<uint8_t> sentinel{'o', 'l', 'd'};
    { std::ofstream file(target.string(), std::ios::binary); file << "old"; }
    Sl1PngEnvironment borrow("SLA_EXPERIMENT_SL1_BORROW_PNG", "1");
    Sl1PngEnvironment compare("SLA_EXPERIMENT_SL1_COMPARE_ENCODED", "1");
    Sl1PngTrace trace;
    const sla::ExportRequest request(snapshot, format, target.string(), "owned");
    bool reached = false;
    size_t observed_total = 0;
    const std::string prefix = "SL1 encoded readback chunk: layer=0; bytes=1048576/";
    CHECK_THROWS_AS(request.write([&] {
        trace.sink->flush();
        std::istringstream lines(trace.text->str());
        for (std::string line; !reached && std::getline(lines, line);) {
            if (line.rfind(prefix, 0) != 0) continue;
            std::istringstream total(line.substr(prefix.size()));
            REQUIRE(bool(total >> observed_total));
            total >> std::ws;
            REQUIRE(total.eof());
            REQUIRE(observed_total > chunk_size);
            REQUIRE(observed_total == expected_png[0].size());
            REQUIRE(session_export_staging_exists(directory.path));
            CHECK(read_file(target) == sentinel);
            reached = true;
            request.cancel();
        }
    }), CanceledException);
    REQUIRE(reached);
    CHECK(request.cancelled());
    CHECK(trace.count("SL1 encoded readback chunk:") == 1);
    trace.expect(1, 1, 0);
    CHECK(read_file(target) == sentinel);
    CHECK_FALSE(session_export_staging_exists(directory.path));
    unchanged();

    // Cancellation belongs to the first request; retry the same immutable
    // source through a NEW real request, keeping the successful fast path.
    const sla::ExportRequest retry(snapshot, format, target.string(), "owned");
    CHECK_FALSE(retry.cancelled());
    REQUIRE_NOTHROW(retry.write());
    CHECK(trace.count("SL1 encoded readback chunk:") == expected_chunks);
    for (size_t layer = 0; layer < 3; ++layer) {
        const auto size = std::to_string(expected_png[layer].size());
        const std::string complete = "SL1 encoded readback chunk: layer=" + std::to_string(layer) +
            "; bytes=" + size + "/" + size;
        CHECK(trace.count(complete.c_str()) == 1);
    }
    trace.expect(1, 1, 0);
    CHECK_FALSE(session_export_staging_exists(directory.path));
    const auto decoded = sla::LayerDocument::load_archive(target.string());
    REQUIRE(decoded.layer_count() == 3);
    const auto numbers = exported_sl1_numbers(target);
    for (size_t layer = 0; layer < 3; ++layer) {
        const auto image = decoded.image(layer);
        CHECK(image.cols == width);
        CHECK(image.rows == height);
        CHECK(image.buf == expected_pixels[layer]);
        // Native SL1 stores a global schedule, not explicit per-layer rows.
        // Reconstruct the executed schedule from the actual exported fields.
        CHECK(decoded.layer_parameters(layer).empty());
        CHECK(float(numbers.at("slicer.initial_layer_height") + layer * numbers.at("layerHeight")) ==
              float(rows[layer].at(sla::LayerParameter::PositionZ)));
        CHECK(numbers.at(layer < size_t(numbers.at("numSlow")) ? "expTimeFirst" : "expTime") ==
              rows[layer].at(sla::LayerParameter::Exposure));
    }
    struct Zip {
        mz_zip_archive archive{};
        ~Zip() { mz_zip_reader_end(&archive); }
    } zip;
    REQUIRE(mz_zip_reader_init_file(&zip.archive, target.string().c_str(), 0));
    for (size_t layer = 0; layer < 3; ++layer) {
        const std::string name = "owned0000" + std::to_string(layer) + ".png";
        const int index = mz_zip_reader_locate_file(&zip.archive, name.c_str(), nullptr, 0);
        REQUIRE(index >= 0);
        mz_zip_archive_file_stat entry{};
        REQUIRE(mz_zip_reader_file_stat(&zip.archive, mz_uint(index), &entry));
        REQUIRE(entry.m_uncomp_size == expected_png[layer].size());
        std::vector<uint8_t> bytes(expected_png[layer].size());
        REQUIRE(mz_zip_reader_extract_to_mem(&zip.archive, mz_uint(index), bytes.data(), bytes.size(), 0));
        CHECK(bytes == expected_png[layer]);
    }
    CHECK(numbers.at("expTime") == 3.25);
    CHECK(numbers.at("expTimeFirst") == 9.5);
    CHECK(numbers.at("numSlow") == 1);
    CHECK(numbers.at("numFast") == 2);
    CHECK(float(numbers.at("layerHeight")) == float(.05));
    CHECK(float(numbers.at("slicer.initial_layer_height")) == float(.075));
    CHECK(numbers.at("slicer.display_pixels_x") == double(width));
    CHECK(numbers.at("slicer.display_pixels_y") == double(height));
    unchanged();
}

TEST_CASE("Private SL1 export uses borrowed PNGs and encoded readback by default",
          "[sla_sl1_default_policy]") {
    const std::string format = GENERATE("SL1", "SL1S");
    CAPTURE(format);
    Directory directory;
    Sl1PngFixture fixture;
    const auto snapshot = fixture.snapshot();
    Sl1PngEnvironment borrow("SLA_EXPERIMENT_SL1_BORROW_PNG", nullptr);
    Sl1PngEnvironment compare("SLA_EXPERIMENT_SL1_COMPARE_ENCODED", nullptr);
    Sl1PngTrace trace;
    const auto target = directory.path / ("default." + format);
    const sla::ExportRequest request(snapshot, format, target.string(), "owned");
    REQUIRE_NOTHROW(request.write());
    CHECK(trace.count("SL1 encoded readback chunk:") == 3);
    trace.expect(1, 1, 0);
    const auto numbers = require_sl1_png_payload(target, *snapshot->document());
    const auto public_target = directory.path / ("public." + format);
    REQUIRE_NOTHROW(fixture.data.source->export_layer_document(fixture.data.edited,
        format, public_target.string(), {}, "owned"));
    CHECK(trace.count("SL1 encoded readback chunk:") == 0);
    trace.expect(0, 0, 1);
    CHECK(require_sl1_png_payload(public_target, *snapshot->document()) == numbers);
    CHECK(snapshot->generation().current());
    CHECK_FALSE(session_export_staging_exists(directory.path));
}

TEST_CASE("Editor snapshot exports edited masks and intentions independently of its context",
          "[sla_editor_snapshot_export]") {
    const std::string format = GENERATE("SL1", "SL1S");
    const bool captured_context = GENERATE(false, true);
    CAPTURE(format, captured_context);
    Directory directory;
    EditedExportFixture baseline;
    Sl1PngFixture edited;
    auto session = sla::SlicedResultSession::capture(*baseline.source);
    const auto &context = captured_context ? session->context() : *baseline.source;
    const auto generation = context.export_generation();
    const auto revision = edited.data.edited.revision();
    const auto configuration = context.full_print_config();
    REQUIRE(context.layer_document()->image(0).buf != edited.data.edited.image(0).buf);
    REQUIRE(context.layer_document()->fresh_job()->rows != edited.data.edited.fresh_job()->rows);
    Sl1PngEnvironment compare("SLA_EXPERIMENT_SL1_COMPARE_ENCODED", "1");
    Sl1PngTrace trace;
    std::optional<std::map<std::string,double>> expected_numbers;
    for (const char *mode : {"0", "1"}) {
        Sl1PngEnvironment borrow("SLA_EXPERIMENT_SL1_BORROW_PNG", mode);
        const bool optimized = std::strcmp(mode,"1") == 0;
        const auto target = directory.path / (std::string("snapshot-") + mode + "." + format);
        REQUIRE_NOTHROW(context.export_layer_document_snapshot(edited.data.edited,format,target.string(),{},"owned"));
        CHECK(trace.count("SL1 encoded readback chunk:") == (optimized ? 3 : 0));
        trace.expect(optimized ? 1 : 0, optimized ? 1 : 0, optimized ? 0 : 1);
        const auto numbers = require_sl1_png_payload(target,edited.data.edited);
        if (expected_numbers) CHECK(numbers == *expected_numbers);
        else expected_numbers = numbers;
        CHECK(context.export_generation() == generation);
        CHECK(context.full_print_config() == configuration);
        CHECK(edited.data.edited.revision() == revision);
        CHECK(edited.data.edited.can_undo());
        CHECK_FALSE(session_export_staging_exists(directory.path));
    }
}

TEST_CASE("Editor snapshot isolates captured layers and output arguments after capture",
          "[sla_editor_snapshot_export]") {
    const std::string selected = GENERATE("SL1", "SL1S");
    Directory directory;
    EditedExportFixture baseline;
    Sl1PngFixture edited;
    const auto before = edited.data.edited.snapshot();
    const auto target = directory.path / ("snapshot." + selected);
    std::string format=selected,path=target.string(),name="owned";
    Sl1PngEnvironment borrow("SLA_EXPERIMENT_SL1_BORROW_PNG", "1");
    Sl1PngEnvironment compare("SLA_EXPERIMENT_SL1_COMPARE_ENCODED", "1");
    Sl1PngTrace trace;
    bool changed=false;
    REQUIRE_NOTHROW(baseline.source->export_layer_document_snapshot(edited.data.edited,format,path,{},name,[&] {
        // Only after planning/encoding selection; capture itself still requires
        // stable input, just like the ordinary ready-result snapshot contract.
        if (!changed && trace.count("Layer export PNG encoding bypass selected:") == 1) {
            edited.data.edited = sla::LayerDocument{};
            format="unsupported";path=(directory.path/"wrong.sl1").string();name="wrong";
            changed=true;
        }
    }));
    REQUIRE(changed);
    CHECK(edited.data.edited.layer_count()==0);
    CHECK_FALSE(boost::filesystem::exists(path));
    CHECK(trace.count("SL1 encoded readback chunk:")==3);
    trace.expect(1,1,0);
    require_sl1_png_payload(target,before);
    CHECK_FALSE(session_export_staging_exists(directory.path));
}

TEST_CASE("Editor snapshot cancellation preserves destination source history and retry",
          "[sla_editor_snapshot_export][sla_export_cancel]") {
    const std::string format = GENERATE("SL1", "SL1S");
    Directory directory;Sl1PngFixture fixture;
    const auto target=directory.path/("cancel."+format);
    Sl1PngEnvironment borrow("SLA_EXPERIMENT_SL1_BORROW_PNG","1");
    Sl1PngEnvironment compare("SLA_EXPERIMENT_SL1_COMPARE_ENCODED","1");
    const auto generation=fixture.data.source->export_generation();
    const auto revision=fixture.data.edited.revision();
    size_t checks=0;
    fixture.data.source->export_layer_document_snapshot(fixture.data.edited,format,target.string(),{},"owned",[&]{++checks;});
    REQUIRE(checks>10);
    const std::set<size_t> stops{1,2,3,checks/2,checks-1,checks};
    for (const auto stop:stops) {
        CAPTURE(format,stop,checks);
        {std::ofstream file(target.string());file<<"old";}
        const auto request=fixture.data.source->export_generation_guard().with_cancellation();
        size_t observed=0;
        CHECK_THROWS_AS(fixture.data.source->export_layer_document_snapshot(fixture.data.edited,format,target.string(),{},"owned",[&] {
            if (++observed==stop) request.cancel();
        },&request),CanceledException);
        CHECK(observed==stop);
        CHECK(read_file(target)==std::vector<uint8_t>{'o','l','d'});
        CHECK_FALSE(session_export_staging_exists(directory.path));
        CHECK(fixture.data.source->export_generation()==generation);
        CHECK_FALSE(fixture.data.source->canceled());
        CHECK(fixture.data.edited.revision()==revision);
        CHECK(fixture.data.edited.can_undo());
        const auto retry=fixture.data.source->export_generation_guard().with_cancellation();
        REQUIRE_NOTHROW(fixture.data.source->export_layer_document_snapshot(fixture.data.edited,format,target.string(),{},"owned",{},&retry));
        require_sl1_png_payload(target,fixture.data.edited);
    }
}

TEST_CASE("Editor snapshot rejects changed output and foreign request ownership",
          "[sla_editor_snapshot_export][sla_export_publication]") {
    const std::string format=GENERATE("SL1","SL1S");
    Directory directory;Sl1PngFixture fixture;EditedExportFixture other;
    const auto target=directory.path/("protected."+format);
    {std::ofstream file(target.string());file<<"old";}
    const auto foreign=other.source->export_generation_guard().with_cancellation();
    size_t callbacks=0;
    CHECK_THROWS_AS(fixture.data.source->export_layer_document_snapshot(fixture.data.edited,format,target.string(),{},"owned",[&]{++callbacks;},&foreign),RuntimeError);
    CHECK(callbacks==0);
    CHECK(read_file(target)==std::vector<uint8_t>{'o','l','d'});
    Sl1PngEnvironment borrow("SLA_EXPERIMENT_SL1_BORROW_PNG","1");
    Sl1PngEnvironment compare("SLA_EXPERIMENT_SL1_COMPARE_ENCODED","1");
    Sl1PngTrace trace;bool changed=false;
    CHECK_THROWS_AS(fixture.data.source->export_layer_document_snapshot(fixture.data.edited,format,target.string(),{},"owned",[&] {
        if (changed || trace.count("Layer export PNG document consumed:")!=1) return;
        for (const auto &entry:boost::filesystem::directory_iterator(directory.path)) {
            if (entry.path().filename().string().find(".sla-export-")!=0) continue;
            mutate_sl1_png(entry.path(),Sl1PngMutation::MovePixel);changed=true;
        }
    }),RuntimeError);
    REQUIRE(changed);
    CHECK(read_file(target)==std::vector<uint8_t>{'o','l','d'});
    CHECK_FALSE(session_export_staging_exists(directory.path));
    trace.expect(1,1,0);
    REQUIRE_NOTHROW(fixture.data.source->export_layer_document_snapshot(fixture.data.edited,format,target.string(),{},"owned"));
    require_sl1_png_payload(target,fixture.data.edited);
}

TEST_CASE("Editor snapshot preserves other raster destinations without granting SL1 borrowing",
          "[sla_editor_snapshot_export]") {
    const std::string format=GENERATE("uvj","cxdlpv4");
    Directory directory;EditedExportFixture fixture;
    auto config=fixture.source->full_print_config();config.set("printer_model","CL-60");
    const auto context=SLAPrint::create_raster_job(config,*fixture.source->layer_document(),{.05,.1,.15});
    const auto target=directory.path/("snapshot."+format);
    Sl1PngTrace trace;
    REQUIRE_NOTHROW(context->export_layer_document_snapshot(fixture.edited,format,target.string(),{},"owned"));
    trace.expect(0,0,1);
    const auto reopened=sla::LayerDocument::load_archive(target.string());
    REQUIRE(reopened.layer_count()==3);
    for(size_t i=0;i<3;++i)
        CHECK(reopened.image(i).buf==fixture.expected[i]);
    // Reopened UVJ/CXDLPv4 documents do not expose an editable parameter
    // table. Inspect every wire field instead of treating that empty API as
    // evidence that the writer dropped the edited document's intentions.
    using F=sla::LayerParameter;
    const auto &intent=*fixture.edited.fresh_job();
    if(format=="uvj") {
        const auto archive=read_zipper_archive(target.string(),{"json"},{});
        REQUIRE(archive.entries.size()==1);
        const auto metadata=nlohmann::json::parse(archive.entries[0].buf);
        const std::map<F,const char *> fields{{F::Exposure,"LightOnTime"},{F::LightOff,"LightOffTime"},
            {F::WaitBefore,"WaitTimeBeforeCure"},{F::WaitAfter,"WaitTimeAfterCure"},{F::WaitAfterLift,"WaitTimeAfterLift"},
            {F::LiftDistance,"LiftHeight"},{F::LiftDistance2,"LiftHeight2"},{F::LiftSpeed,"LiftSpeed"},
            {F::LiftSpeed2,"LiftSpeed2"},{F::RetractDistance2,"RetractHeight2"},
            {F::RetractSpeed,"RetractSpeed"},{F::RetractSpeed2,"RetractSpeed2"},{F::LightPWM,"LightPWM"}};
        auto check_exposure=[&](const nlohmann::json &json,const sla::LayerParameters &row) {
            for(const auto &[field,name]:fields) {
                CAPTURE(name);
                CHECK(json.at(name).get<double>()==double(float(row.at(field))));
            }
            CHECK(json.at("RetractHeight").get<double>()==double(float(
                row.at(F::LiftDistance)+row.at(F::LiftDistance2)-row.at(F::RetractDistance2))));
        };
        const auto &properties=metadata.at("Properties");
        CHECK(properties.at("Size").at("X")==8);CHECK(properties.at("Size").at("Y")==6);
        CHECK(properties.at("Size").at("Layers")==3);
        CHECK(properties.at("Size").at("Millimeter").at("X")==8.);
        CHECK(properties.at("Size").at("Millimeter").at("Y")==6.);
        CHECK(properties.at("Size").at("LayerHeight").get<double>()==double(float(.05)));
        CHECK(properties.at("Bottom").at("Count")==1);
        check_exposure(properties.at("Exposure"),intent.globals.normal);
        check_exposure(properties.at("Bottom"),intent.globals.bottom);
        REQUIRE(metadata.at("Layers").size()==3);
        for(size_t i=0;i<3;++i) {
            CAPTURE(i);
            CHECK(metadata.at("Layers").at(i).at("Z").get<double>()==double(float(intent.rows[i].at(F::PositionZ))));
            check_exposure(metadata.at("Layers").at(i).at("Exposure"),intent.rows[i]);
        }
    } else {
        const auto bytes=read_file(target);const sla::CrealityCXDLPv4Layout layout(bytes);
        auto wire=[](sla::LayerParameters row) {
            const double total=row.at(F::LiftDistance)+row.at(F::LiftDistance2);
            for(auto &[field,value]:row)value=double(float(value));
            row[F::LiftDistance]=double(float(total))-row.at(F::LiftDistance2);
            return row;
        };
        CHECK(layout.columns==8);CHECK(layout.rows==6);CHECK(layout.width==8.);CHECK(layout.height==6.);
        const auto globals=sla::cxdlpv4_native_globals(layout);
        CHECK(globals.layer_height==double(float(.05)));CHECK(globals.bottom_layers==1);
        CHECK(globals.parameter_bottom_layers==1);CHECK(globals.transition_layers==0);
        CHECK(globals.normal==wire(intent.globals.normal));
        auto bottom=wire(intent.globals.bottom);
        const auto reference=reopened.cxdlpv4_reference_bottom();REQUIRE(reference);
        REQUIRE(reference->meaningful_bottom_layer==std::optional<size_t>(0));
        for(const auto field:{F::WaitBefore,F::WaitAfter,F::WaitAfterLift,F::RetractSpeed,F::RetractDistance2,F::RetractSpeed2}) {
            CHECK(reference->virtual_bottom.at(field)==bottom.at(field));bottom.erase(field);
        }
        CHECK(globals.bottom==bottom);
        for(size_t i=0;i<3;++i) {
            CAPTURE(i);
            CHECK(sla::cxdlpv4_layer_parameters(layout,i)==wire(intent.rows[i]));
        }
    }
    CHECK(fixture.edited.can_undo());
}
