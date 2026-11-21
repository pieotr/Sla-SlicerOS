// SPDX-License-Identifier: AGPL-3.0-or-later
// App-authored geometry only. No downloaded or user-owned model fixtures.
#include "libslic3r/SLAPrint.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/SLA/GeneratedLayerOwnership.hpp"
#include "libslic3r/SLA/ExportRequest.hpp"
#include "libslic3r/SLA/ProjectArchive.hpp"
#include "libslic3r/Format/ZipperArchiveImport.hpp"
#include <nlohmann/json.hpp>
#include <boost/filesystem.hpp>
#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

using namespace Slic3r;
namespace {
DynamicPrintConfig model_precision_config(const std::string &format,unsigned version,bool portrait)
{
    DynamicPrintConfig config;config.apply(SLAFullPrintConfig{});
    config.option("printer_technology")->setInt(ptSLA);
    config.set("sla_archive_format",format);config.set("sla_anycubic_version",std::to_string(version));
    const double width=format=="pmx2"?196.61:198.15,height=format=="pmx2"?122.88:123.84;
    config.set("display_width",portrait?height:width);config.set("display_height",portrait?width:height);
    const std::string w=std::to_string(portrait?height:width),h=std::to_string(portrait?width:height);
    config.set_deserialize_strict("bed_shape","0x0,"+w+"x0,"+w+"x"+h+",0x"+h);
    config.set("max_print_height",format=="pmx2"?200.:245.);
    config.set("display_pixels_x",400);config.set("display_pixels_y",250);
    config.option("display_orientation")->setInt(portrait?sladoPortrait:sladoLandscape);
    config.set("display_mirror_x",false);config.set("display_mirror_y",false);config.set("gamma_correction",1.);
    config.set("supports_enable",false);config.set("pad_enable",false);config.set("support_object_elevation",0.);
    config.set("layer_height",.05);config.set("initial_layer_height",.05);
    config.set("exposure_time",2.);config.set("initial_exposure_time",10.);
    config.set("sla_bottom_layers",1);config.set("sla_transition_layers",0);
    config.set("material_notes",std::string(""),true);config.set("printer_notes",std::string(""),true);
    for(const char *prefix:{"sla_","sla_bottom_"}) {
        const std::string key(prefix);
        config.set(key+"lift_height",3.);config.set(key+"lift_speed",60.);config.set(key+"retract_speed",120.);
        for(const char *suffix:{"lift_height2","lift_speed2","retract_height2","retract_speed2",
                                "wait_before_cure","wait_after_cure","wait_after_lift"})config.set(key+suffix,0.);
        config.set(key+"light_pwm",255);
    }
    return config;
}
std::vector<std::vector<uint8_t>> independent_native_masks(const boost::filesystem::path &path,
    unsigned version,size_t columns,size_t rows)
{
    std::ifstream input(path.string(),std::ios::binary);
    const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(input),{}};
    auto u32=[&](size_t at){REQUIRE(at<=bytes.size());REQUIRE(bytes.size()-at>=4);
        return uint32_t(bytes[at])|(uint32_t(bytes[at+1])<<8)|(uint32_t(bytes[at+2])<<16)|(uint32_t(bytes[at+3])<<24);};
    auto real=[&](size_t at){const auto word=u32(at);float value;std::memcpy(&value,&word,4);return value;};
    REQUIRE(bytes.size()>=48);CHECK(bool(std::string(bytes.begin(),bytes.begin()+8)=="ANYCUBIC"));CHECK(u32(12)==version);
    const size_t header=u32(20),table=u32(36);CHECK(u32(header+60)==columns);CHECK(u32(header+64)==rows);
    CHECK(real(header+20)==float(.05));CHECK(real(header+24)==2.f);CHECK(real(header+32)==10.f);
    CHECK(real(header+36)==1.f);CHECK(u32(header+84)==327);REQUIRE(u32(table+16)==3);
    std::vector<std::vector<uint8_t>> masks;
    for(size_t layer=0;layer<3;++layer) {
        const size_t row=table+20+32*layer,begin=u32(row),length=u32(row+4);
        REQUIRE(begin<=bytes.size());REQUIRE(length<=bytes.size()-begin);
        CHECK(real(row+8)==3.f);CHECK(real(row+12)==1.f);CHECK(real(row+16)==(layer?2.f:10.f));
        std::vector<uint8_t> mask;mask.reserve(columns*rows);
        for(size_t at=begin;at<begin+length;) {
            const unsigned token=bytes[at++],bucket=token>>4;size_t count=token&15;
            if(bucket==0 || bucket==15){REQUIRE(at<begin+length);count=count*256+bytes[at++];}
            REQUIRE(count>0);REQUIRE(count<=columns*rows-mask.size());
            mask.insert(mask.end(),count,uint8_t(bucket*17));
        }
        REQUIRE(mask.size()==columns*rows);masks.push_back(std::move(mask));
    }
    return masks;
}
}

TEST_CASE("Anycubic515 model source precision preserves native pixels without acquiring parameter ownership",
          "[sla_anycubic_model_precision]")
{
    const std::string format=GENERATE("pmx2","pwmb");
    const unsigned version=GENERATE(515u,516u,517u);
    const bool portrait=GENERATE(false,true);CAPTURE(format,version,portrait);
    const auto config=model_precision_config(format,version,portrait);const auto initial=config;
    Model model;auto *object=model.add_object("Own rotated precision fixture","",make_cube(4.137,2.313,.15));
    auto *instance=object->add_instance();instance->set_rotation(Vec3d(0.,0.,.371));instance->set_offset(Vec3d(10.,10.,0.));
    SLAPrint print;print.apply(model,config);REQUIRE(print.validate().empty());print.process();
    REQUIRE(print.layer_document());REQUIRE(print.layer_document()->layer_count()==3);
    const bool owned=version>=516;const auto source=*print.layer_document();
    CHECK(bool(source.fresh_job())==owned);CHECK(source.parameter_capabilities().empty()==!owned);
    CHECK_FALSE(print.calibration_recipe());
    CHECK_FALSE(source.dirty());CHECK_FALSE(source.can_undo());CHECK_FALSE(source.can_redo());
    CHECK(source.mask_contract_precision()==(owned?std::optional<sla::LayerMaskPrecision>{}:
        std::optional<sla::LayerMaskPrecision>{sla::LayerMaskPrecision::NativeGray4}));
    if(owned) {
        REQUIRE(source.fresh_job());const auto &fresh=*source.fresh_job();
        CHECK(fresh.mask_precision==sla::LayerMaskPrecision::NativeGray4);
        CHECK(fresh.producer==sla::FreshLayerProducer::ModelSlice);
        CHECK(fresh.parameter_origin==sla::FreshLayerParameterOrigin::AutomaticModel);
        CHECK(fresh.globals.layer_height==.05);CHECK(fresh.globals.bottom_layers==1);
        CHECK(fresh.globals.transition_layers==0);REQUIRE(fresh.rows.size()==3);
        for(size_t layer=0;layer<3;++layer)CHECK(fresh.rows[layer].at(sla::LayerParameter::Exposure)==(layer?2.:10.));
    }
    CHECK(sla::generated_anycubic_version(format,"auto")==515);
    auto reference_config=config;reference_config.set("sla_archive_format",std::string("uvj"));
    SLAPrint reference;reference.apply(model,reference_config);REQUIRE(reference.validate().empty());reference.process();
    REQUIRE(reference.layer_document());REQUIRE(reference.layer_document()->layer_count()==3);
    bool intermediate_aa=false;
    for(size_t layer=0;layer<3;++layer) {
        const auto raw=reference.layer_document()->image(layer);const auto actual=source.image(layer);auto expected=raw.buf;
        for(auto &value:expected){intermediate_aa|=value!=0 && value!=255;value=uint8_t((value>>4)*17);}
        CHECK(actual.cols==raw.cols);CHECK(actual.rows==raw.rows);CHECK(bool(actual.buf==expected));
        CHECK(std::all_of(actual.buf.begin(),actual.buf.end(),[](uint8_t value){return value%17==0;}));
    }
    CHECK(intermediate_aa); // A binary-only shape must not mask the original defect.
    const char *fixtures=std::getenv("SLA_ANYCUBIC_MODEL_PRECISION_FIXTURES");
    const auto folder=fixtures?boost::filesystem::path(fixtures)/(format+"-"+std::to_string(version)+(portrait?"-portrait":"-landscape")):
        boost::filesystem::temp_directory_path()/boost::filesystem::unique_path("any-model-precision-%%%%");
    boost::filesystem::create_directories(folder);
    struct Cleanup {boost::filesystem::path path;bool keep;~Cleanup(){if(!keep){boost::system::error_code error;boost::filesystem::remove_all(path,error);}}} cleanup{folder,fixtures!=nullptr};
    config.save((folder/"source.ini").string());
    for(size_t layer=0;layer<3;++layer) {
        const auto png=reference.layer_document()->encoded_layer(layer);
        std::ofstream output((folder/("gray8-reference-"+std::to_string(layer)+".png")).string(),std::ios::binary);
        output.write(static_cast<const char *>(png.buf),png.sz);REQUIRE(output.good());
    }
    const size_t columns=portrait?250:400,rows=portrait?400:250;
    auto check_native=[&](const boost::filesystem::path &path,const sla::LayerDocument &document) {
        const auto masks=independent_native_masks(path,version,columns,rows);
        for(size_t layer=0;layer<3;++layer)CHECK(bool(masks[layer]==document.image(layer).buf));
    };
    const auto direct=folder/("direct."+format);print.export_print(direct.string());check_native(direct,source);
    const auto snapshot=std::make_shared<const sla::ExportSnapshot>(print.capture_export_snapshot());
    REQUIRE(snapshot->document());CHECK(bool(snapshot->document()->fresh_job())==owned);
    CHECK(snapshot->document()->mask_contract_precision()==source.mask_contract_precision());
    const auto frozen=folder/("frozen."+format);sla::ExportRequest(snapshot,format,frozen.string()).write();check_native(frozen,source);
    const auto explicit_path=folder/("explicit."+format);print.export_layer_document(source,format,explicit_path.string());check_native(explicit_path,source);
    auto check_manifest=[&](const boost::filesystem::path &path) {
        const auto archive=read_zipper_archive(path.string(),{"metadata/sla/layers.json"},{});
        REQUIRE(archive.entries.size()==1);const auto json=nlohmann::json::parse(archive.entries.front().buf);
        CHECK(json.at("version")== (owned?5:8));CHECK_FALSE(json.contains("calibration_recipe"));
        CHECK_FALSE(json.at("source_fingerprint").get<std::string>().empty());
        const auto &document=json.at("document");CHECK(document.contains("fresh_job")==owned);
        CHECK(document.contains("mask_contract")==!owned);
        if(!owned && document.contains("mask_contract"))CHECK(document.at("mask_contract")==nlohmann::json({{"version",1},{"precision",18}}));
        return owned?document.at("fresh_job"):document.value("mask_contract",nlohmann::json());
    };
    const auto source_project=folder/"source.sla-project";
    sla::save_layer_project(source_project.string(),sla::capture_project_layers(model,config,print));
    const auto original_descriptor=check_manifest(source_project);
    auto edited=source;auto spectrum=edited.image(0);REQUIRE(spectrum.buf.size()>=256);
    for(unsigned value=0;value<256;++value)spectrum.buf[value]=uint8_t(value);
    edited.replace_image(0,spectrum);const auto projected=edited.image(0);
    for(unsigned value=0;value<256;++value)CHECK(projected.buf[value]==uint8_t((value>>4)*17));
    // Distinct pixels outside the spectrum avoid portrait coordinate overlap.
    edited.paint(0,2,2,0,128);CHECK(edited.image(0).get(2,2)==136);
    edited.paint(0,3,2,0,127);CHECK(edited.image(0).get(2,3)==119);
    const auto edited_pixels=edited.image(0).buf;
    REQUIRE(edited.undo());REQUIRE(edited.redo());CHECK(bool(edited.image(0).buf==edited_pixels));
    for(size_t layer=1;layer<3;++layer)CHECK(bool(edited.image(layer).buf==source.image(layer).buf));
    print.replace_layer_document(edited);
    const auto edited_path=folder/("edited."+format);print.export_print(edited_path.string());check_native(edited_path,edited);
    const auto project=folder/"edited.sla-project";
    sla::save_layer_project(project.string(),sla::capture_project_layers(model,config,print));
    CHECK(check_manifest(project)==original_descriptor);
    const auto loaded=sla::read_project_layers(project.string());REQUIRE(loaded);
    const auto original=sla::read_project_layers(source_project.string());REQUIRE(original);
    CHECK(loaded->source_fingerprint==original->source_fingerprint);
    SLAPrint restored;restored.apply(model,config);restored.queue_project_layers(std::make_shared<const sla::ProjectRasterState>(*loaded));restored.process();
    REQUIRE(restored.layer_document());CHECK_FALSE(restored.project_layer_restore_failed());
    CHECK(bool(restored.layer_document()->fresh_job())==owned);
    CHECK(restored.layer_document()->mask_contract_precision()==source.mask_contract_precision());
    for(size_t layer=0;layer<3;++layer)CHECK(bool(restored.layer_document()->image(layer).buf==edited.image(layer).buf));
    const auto restart=folder/("restart."+format);restored.export_print(restart.string());check_native(restart,*restored.layer_document());
    CHECK(config.diff(initial).empty());
}

// Explicit opt-in only: these inputs must be the actual immutable pre-fix RED
// artifacts. Do not manufacture a schema downgrade or overwrite a fingerprint.
TEST_CASE("Real pre-fix Anycubic515 model projects retain unknown Gray8 source after new producer hookup",
          "[.sla_anycubic_515_legacy_reopen]")
{
    const char *red=std::getenv("SLA_ANYCUBIC_MODEL_LEGACY_RED_FIXTURES");
    const char *output=std::getenv("SLA_ANYCUBIC_MODEL_LEGACY_REOPEN_OUTPUT");
    REQUIRE(red!=nullptr);REQUIRE(output!=nullptr);
    const std::string format=GENERATE("pmx2","pwmb");
    const bool portrait=GENERATE(false,true);
    const std::string kind=GENERATE("source","edited");CAPTURE(format,portrait,kind);
    const auto folder=boost::filesystem::path(red)/(format+"-515"+(portrait?"-portrait":"-landscape"));
    const auto input_path=folder/(kind+".sla-project"),ini_path=folder/"source.ini";
    auto file_bytes=[](const boost::filesystem::path &path) {
        std::ifstream input(path.string(),std::ios::binary);REQUIRE(input.good());
        return std::string{std::istreambuf_iterator<char>(input),{}};
    };
    const auto original_bytes=file_bytes(input_path),original_ini=file_bytes(ini_path);
    const auto archive=read_zipper_archive(input_path.string(),{"metadata/sla/layers.json"},{});
    REQUIRE(archive.entries.size()==1);const auto metadata=nlohmann::json::parse(archive.entries.front().buf);
    REQUIRE(metadata.at("version")==2);REQUIRE_FALSE(metadata.at("document").contains("mask_contract"));
    REQUIRE_FALSE(metadata.at("document").contains("fresh_job"));REQUIRE_FALSE(metadata.contains("calibration_recipe"));
    const auto state=sla::read_project_layers(input_path.string());REQUIRE(state);
    REQUIRE(state->origin==sla::ProjectLayerOrigin::ModelSlice);REQUIRE(state->document.layer_count()==3);
    REQUIRE_FALSE(state->document.fresh_job());REQUIRE_FALSE(state->document.mask_contract_precision());
    DynamicPrintConfig config;config.load_from_ini(ini_path.string(),ForwardCompatibilitySubstitutionRule::Disable);
    REQUIRE(config.opt_string("sla_archive_format")==format);REQUIRE(config.opt_string("sla_anycubic_version")=="515");
    const auto original_config=config;
    Model model;auto *object=model.add_object("Own rotated precision fixture","",make_cube(4.137,2.313,.15));
    auto *instance=object->add_instance();instance->set_rotation(Vec3d(0.,0.,.371));instance->set_offset(Vec3d(10.,10.,0.));
    REQUIRE_FALSE(state->source_fingerprint.empty());
    REQUIRE(sla::raster_source_fingerprint(model,config)==state->source_fingerprint);
    std::vector<std::vector<uint8_t>> expected;
    bool noncanonical=false;
    for(size_t layer=0;layer<3;++layer) {
        expected.push_back(state->document.image(layer).buf);
        for(const auto value:expected.back())noncanonical|=value%17!=0;
    }
    REQUIRE(noncanonical); // Actual legacy Gray8 source, not a synthetic canonical file.
    if(kind=="edited") {
        REQUIRE(expected[0].size()>=256);
        for(unsigned value=0;value<256;++value)REQUIRE(expected[0][value]==uint8_t(value));
        CHECK(state->document.image(0).get(2,2)==128);CHECK(state->document.image(0).get(2,3)==127);
    }
    SLAPrint restored;restored.apply(model,config);
    restored.queue_project_layers(std::make_shared<const sla::ProjectRasterState>(*state));restored.process();
    REQUIRE(restored.layer_document());REQUIRE_FALSE(restored.project_layer_restore_failed());
    const auto &document=*restored.layer_document();
    CHECK_FALSE(document.fresh_job());CHECK_FALSE(document.mask_contract_precision());
    CHECK(document.parameter_capabilities().empty());CHECK_FALSE(restored.calibration_recipe());
    for(size_t layer=0;layer<3;++layer)CHECK(bool(document.image(layer).buf==expected[layer]));
    auto edit=document;edit.paint(0,2,2,0,128);CHECK(edit.image(0).get(2,2)==128);
    const auto destination=boost::filesystem::path(output)/(format+(portrait?"-portrait-":"-landscape-")+kind+".sla-project");
    boost::filesystem::create_directories(destination.parent_path());REQUIRE_FALSE(boost::filesystem::exists(destination));
    const auto recaptured=sla::capture_project_layers(model,config,restored);
    CHECK(recaptured.source_fingerprint==state->source_fingerprint);
    sla::save_layer_project(destination.string(),recaptured);
    const auto reread=sla::read_project_layers(destination.string());REQUIRE(reread);
    CHECK_FALSE(reread->document.fresh_job());CHECK_FALSE(reread->document.mask_contract_precision());
    for(size_t layer=0;layer<3;++layer)CHECK(bool(reread->document.image(layer).buf==expected[layer]));
    const auto after=read_zipper_archive(destination.string(),{"metadata/sla/layers.json"},{});
    REQUIRE(after.entries.size()==1);const auto remeta=nlohmann::json::parse(after.entries.front().buf);
    CHECK(remeta.at("version")==2);CHECK_FALSE(remeta.at("document").contains("mask_contract"));
    CHECK_FALSE(remeta.at("document").contains("fresh_job"));
    CHECK(bool(file_bytes(input_path)==original_bytes));CHECK(bool(file_bytes(ini_path)==original_ini));
    CHECK(config.diff(original_config).empty());
}
