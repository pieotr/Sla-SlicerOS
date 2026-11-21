// SPDX-License-Identifier: AGPL-3.0-or-later
#include "libslic3r/SLAPrint.hpp"
#include "libslic3r/SLA/PCBExposure.hpp"
#include "libslic3r/SLA/GeneratedLayerOwnership.hpp"
#include "libslic3r/SLA/ExportRequest.hpp"
#include "libslic3r/SLA/ProjectArchive.hpp"
#include "libslic3r/Format/ZipperArchiveImport.hpp"
#include <nlohmann/json.hpp>
#include <boost/filesystem.hpp>
#include <algorithm>
#include <array>
#include <cmath>
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
DynamicPrintConfig pcb_precision_config(const std::string &format,unsigned version,bool portrait)
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
std::vector<std::vector<uint8_t>> pcb_native_masks(const boost::filesystem::path &path,unsigned version,
    size_t columns,size_t rows,size_t count)
{
    std::ifstream input(path.string(),std::ios::binary);const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(input),{}};
    auto u32=[&](size_t at){REQUIRE(at<=bytes.size());REQUIRE(bytes.size()-at>=4);
        return uint32_t(bytes[at])|(uint32_t(bytes[at+1])<<8)|(uint32_t(bytes[at+2])<<16)|(uint32_t(bytes[at+3])<<24);};
    auto real=[&](size_t at){auto word=u32(at);float value;std::memcpy(&value,&word,4);return value;};
    REQUIRE(bytes.size()>=48);CHECK(bool(std::string(bytes.begin(),bytes.begin()+8)=="ANYCUBIC"));CHECK(u32(12)==version);
    const size_t header=u32(20),table=u32(36);CHECK(u32(header+60)==columns);CHECK(u32(header+64)==rows);
    CHECK(real(header+20)==float(.05));CHECK(real(header+24)==float(30.12));CHECK(real(header+32)==float(30.12));
    CHECK(real(header+36)==0.f);CHECK(u32(header+88)==0);CHECK(real(header+28)==0.f);
    CHECK(real(header+40)==0.f);CHECK(real(header+44)==0.f);CHECK(real(header+48)==0.f);
    CHECK(u32(header+84)==uint32_t(count==1?330:360));REQUIRE(u32(table+16)==count);
    std::vector<std::vector<uint8_t>> masks;
    for(size_t layer=0;layer<count;++layer) {
        const size_t row=table+20+32*layer,begin=u32(row),length=u32(row+4);
        REQUIRE(begin<=bytes.size());REQUIRE(length<=bytes.size()-begin);
        CHECK(real(row+8)==0.f);CHECK(real(row+12)==0.f);CHECK(real(row+16)==float(30.12));CHECK(real(row+20)==float(.05));
        std::vector<uint8_t> mask;mask.reserve(columns*rows);
        for(size_t at=begin;at<begin+length;) {
            const unsigned token=bytes[at++],bucket=token>>4;size_t run=token&15;
            if(bucket==0 || bucket==15){REQUIRE(at<begin+length);run=run*256+bytes[at++];}
            REQUIRE(run>0);REQUIRE(run<=columns*rows-mask.size());mask.insert(mask.end(),run,uint8_t(bucket*17));
        }
        REQUIRE(mask.size()==columns*rows);masks.push_back(std::move(mask));
    }
    return masks;
}
}
TEST_CASE("Anycubic515 PCB first PNG precision preserves stationary source and clean polarity baseline",
          "[sla_anycubic_pcb_precision]")
{
    const std::string format=GENERATE("pmx2","pwmb");
    const unsigned version=GENERATE(515u,516u,517u);
    const bool portrait=GENERATE(false,true);
    const bool merge=GENERATE(false,true);
    const bool invert=GENERATE(false,true);
    CAPTURE(format,version,portrait,merge,invert);const auto config=pcb_precision_config(format,version,portrait);const auto accepted=config;
    const std::string gerber="%FSLAX24Y24*%\n%MOMM*%\n";
    const auto first=sla::parse_gerber(gerber+"%ADD10C,3.137*%D10*X100000Y100000D03*M02*");
    const auto second=sla::parse_gerber(gerber+"%ADD10C,2.313*%D10*X115000Y100000D03*M02*");
    sla::PCBPlacement placement;placement.center=false;placement.margin=0.;
    SLAPrinterConfig printer;printer.apply(config,true);
    auto reference_config=config;reference_config.set("sla_archive_format",std::string("uvj"));
    SLAPrinterConfig reference_printer;reference_printer.apply(reference_config,true);
    const auto reference=sla::rasterize_pcb_layers({first,second},reference_printer,merge,placement);
    const size_t count=merge?1:2,columns=portrait?250:400,rows=portrait?400:250;REQUIRE(reference.layer_count()==count);
    const auto pure=sla::rasterize_pcb_layers({first,second},printer,merge,placement);
    REQUIRE(pure.layer_count()==count);CHECK_FALSE(pure.fresh_job());CHECK(pure.parameter_capabilities().empty());
    if(version==515)CHECK(pure.mask_contract_precision()==sla::LayerMaskPrecision::NativeGray4);
    bool actual_aa=false;
    for(size_t layer=0;layer<count;++layer) {
        const auto raw=reference.image(layer);auto expected=raw.buf;
        for(auto &value:expected){actual_aa|=value!=0 && value!=255;value=uint8_t((value>>4)*17);}
        if(version==515)CHECK(bool(pure.image(layer).buf==expected));
    }
    CHECK(actual_aa);
    auto job=sla::create_pcb_exposure_job(config,{first,second},merge,placement,30.12,invert);
    REQUIRE(job);REQUIRE(job->layer_document());REQUIRE(job->layer_document()->layer_count()==count);
    const bool owned=version>=516;const auto source=*job->layer_document();
    CHECK(bool(source.fresh_job())==owned);CHECK(source.parameter_capabilities().empty()==!owned);
    CHECK(source.mask_contract_precision()==(owned?std::optional<sla::LayerMaskPrecision>{}:
        std::optional<sla::LayerMaskPrecision>{sla::LayerMaskPrecision::NativeGray4}));
    CHECK(source.revision()==0);CHECK_FALSE(source.dirty());CHECK_FALSE(source.can_undo());CHECK_FALSE(source.can_redo());
    if(owned) {REQUIRE(source.fresh_job());CHECK(source.fresh_job()->producer==sla::FreshLayerProducer::PCB);
        CHECK(source.fresh_job()->parameter_origin==sla::FreshLayerParameterOrigin::Explicit);
        CHECK(source.fresh_job()->mask_precision==sla::LayerMaskPrecision::NativeGray4);}
    for(size_t layer=0;layer<count;++layer) {
        auto expected=reference.image(layer).buf;
        for(auto &value:expected){value=uint8_t((value>>4)*17);if(invert)value=uint8_t(255-value);}
        const auto image=source.image(layer);CHECK(image.cols==columns);CHECK(image.rows==rows);CHECK(bool(image.buf==expected));
        // Literal physical interior/overlap/background points, independent of
        // the same renderer used above for the AA reference.
        struct Sample {double x,y;bool first,second;};
        for(const Sample &s:std::array<Sample,5>{{{9.1,10.,true,false},{12.1,10.,false,true},
                {10.9,10.,true,true},{7.,10.,false,false},{10.9,12.5,false,false}}}) {
            const size_t px=size_t(std::floor(s.x/config.opt_float("display_width")*400.));
            const size_t py=size_t(std::floor(s.y/config.opt_float("display_height")*250.));
            const size_t col=portrait?249-py:px,row=portrait?399-px:249-py;
            REQUIRE(col<columns);REQUIRE(row<rows);const bool copper=merge?(s.first||s.second):(layer?s.second:s.first);
            CAPTURE(layer,s.x,s.y,col,row);CHECK(image.buf[row*columns+col]==uint8_t(copper!=invert?255:0));
        }
    }
    const auto &material=job->material_config();CHECK(material.sla_bottom_layers.value==0);CHECK(material.sla_transition_layers.value==0);
    for(const char *prefix:{"sla_","sla_bottom_"})for(const char *field:{"lift_height","lift_height2","lift_speed","lift_speed2",
            "retract_speed","retract_height2","retract_speed2","wait_before_cure","wait_after_cure","wait_after_lift"})
        CHECK(material.option(std::string(prefix)+field)->getFloat()==0.);
    const char *fixtures=std::getenv("SLA_ANYCUBIC_PCB_PRECISION_FIXTURES");
    const auto name=format+"-"+std::to_string(version)+(portrait?"-portrait":"-landscape")+(merge?"-merged":"-separate")+(invert?"-inverted":"-normal");
    const auto folder=fixtures?boost::filesystem::path(fixtures)/name:boost::filesystem::temp_directory_path()/boost::filesystem::unique_path("any-pcb-precision-%%%%");
    boost::filesystem::create_directories(folder);
    struct Cleanup{boost::filesystem::path path;bool keep;~Cleanup(){if(!keep){boost::system::error_code error;boost::filesystem::remove_all(path,error);}}} cleanup{folder,fixtures!=nullptr};
    config.save((folder/"source.ini").string());job->full_print_config().save((folder/"effective.ini").string());
    for(size_t layer=0;layer<count;++layer) {
        const auto png=reference.encoded_layer(layer);std::ofstream out((folder/("gray8-reference-"+std::to_string(layer)+".png")).string(),std::ios::binary);
        out.write(static_cast<const char *>(png.buf),png.sz);REQUIRE(out.good());
    }
    auto readback=[&](const boost::filesystem::path &path,const sla::LayerDocument &document) {
        const auto decoded=pcb_native_masks(path,version,columns,rows,count);
        for(size_t layer=0;layer<count;++layer)CHECK(bool(decoded[layer]==document.image(layer).buf));
    };
    const auto direct=folder/("direct."+format);job->export_print(direct.string());readback(direct,source);
    const auto snapshot=std::make_shared<const sla::ExportSnapshot>(job->capture_export_snapshot());REQUIRE(snapshot->document());
    const auto frozen=folder/("frozen."+format);sla::ExportRequest(snapshot,format,frozen.string()).write();readback(frozen,source);
    auto manifest=[&](const boost::filesystem::path &path) {
        const auto archive=read_zipper_archive(path.string(),{"metadata/sla/layers.json"},{});REQUIRE(archive.entries.size()==1);
        const auto data=nlohmann::json::parse(archive.entries.front().buf);CHECK(data.at("version")== (owned?5:8));
        CHECK(data.at("source_fingerprint")=="");CHECK(data.at("document").contains("fresh_job")==owned);
        CHECK(data.at("document").contains("mask_contract")==!owned);
        if(!owned)CHECK(data.at("document").value("mask_contract",nlohmann::json())==nlohmann::json({{"version",1},{"precision",18}}));
    };
    const auto source_project=folder/"source.sla-project";sla::save_layer_project(source_project.string(),sla::capture_generated_project(*job,source));manifest(source_project);
    auto edited=source;auto image=edited.image(0);REQUIRE(image.buf.size()>=256);
    for(unsigned v=0;v<256;++v)image.buf[v]=uint8_t(v);edited.replace_image(0,image);
    for(unsigned v=0;v<256;++v)CHECK(edited.image(0).buf[v]==uint8_t((v>>4)*17));
    edited.paint(0,2,2,0,128);CHECK(edited.image(0).get(2,2)==136);
    edited.paint(0,3,2,0,127);CHECK(edited.image(0).get(2,3)==119);
    const auto edited_pixels=edited.image(0).buf;REQUIRE(edited.undo());REQUIRE(edited.redo());CHECK(bool(edited.image(0).buf==edited_pixels));
    for(size_t layer=1;layer<count;++layer)CHECK(bool(edited.image(layer).buf==source.image(layer).buf));
    job->replace_layer_document(edited);const auto project=folder/"edited.sla-project";
    sla::save_layer_project(project.string(),sla::capture_generated_project(*job,*job->layer_document()));manifest(project);
    const auto state=sla::read_project_layers(project.string());REQUIRE(state);REQUIRE(state->origin==sla::ProjectLayerOrigin::GeneratedRaster);
    auto restored=sla::restore_generated_project(*state);REQUIRE(restored);REQUIRE(restored->layer_document());
    CHECK(bool(restored->layer_document()->fresh_job())==owned);CHECK(restored->layer_document()->mask_contract_precision()==source.mask_contract_precision());
    for(size_t layer=0;layer<count;++layer)CHECK(bool(restored->layer_document()->image(layer).buf==edited.image(layer).buf));
    const auto restart=folder/("restart."+format);restored->export_print(restart.string());readback(restart,*restored->layer_document());
    CHECK(config.diff(accepted).empty());
}

// Read actual pre-fix files only. This is generated-raster preservation, not
// geometric-model fingerprint validation and not target-based migration.
TEST_CASE("Actual old Anycubic515 PCB projects retain unknown Gray8 across generated restore",
          "[.sla_anycubic_515_pcb_legacy_reopen]")
{
    const char *inputs=std::getenv("SLA_ANYCUBIC_PCB_LEGACY_RED_FIXTURES");
    const char *outputs=std::getenv("SLA_ANYCUBIC_PCB_LEGACY_REOPEN_OUTPUT");
    REQUIRE(inputs!=nullptr);REQUIRE(outputs!=nullptr);
    const std::string format=GENERATE("pmx2","pwmb");
    const bool portrait=GENERATE(false,true);
    const bool merge=GENERATE(false,true);
    const bool invert=GENERATE(false,true);
    const std::string kind=GENERATE("source","edited");
    CAPTURE(format,portrait,merge,invert,kind);
    const auto name=format+"-515"+(portrait?"-portrait":"-landscape")+
        (merge?"-merged":"-separate")+(invert?"-inverted":"-normal");
    const auto folder=boost::filesystem::path(inputs)/name;
    const auto path=folder/(kind+".sla-project");
    auto bytes=[](const boost::filesystem::path &file) {
        std::ifstream input(file.string(),std::ios::binary);REQUIRE(input.good());
        return std::string{std::istreambuf_iterator<char>(input),{}};
    };
    const auto original=bytes(path),source_ini=bytes(folder/"source.ini"),effective_ini=bytes(folder/"effective.ini");
    const auto archive=read_zipper_archive(path.string(),{"metadata/sla/layers.json"},{});
    REQUIRE(archive.entries.size()==1);const auto metadata=nlohmann::json::parse(archive.entries.front().buf);
    REQUIRE(metadata.at("version")==2);REQUIRE(metadata.at("source_kind")=="generated_raster");
    REQUIRE(metadata.at("source_fingerprint")=="");REQUIRE_FALSE(metadata.contains("calibration_recipe"));
    REQUIRE_FALSE(metadata.at("document").contains("fresh_job"));REQUIRE_FALSE(metadata.at("document").contains("mask_contract"));
    const auto state=sla::read_project_layers(path.string());REQUIRE(state);
    REQUIRE(state->origin==sla::ProjectLayerOrigin::GeneratedRaster);REQUIRE(state->source_fingerprint.empty());
    REQUIRE(state->settings);REQUIRE(state->document.layer_count()==(merge?1:2));
    REQUIRE_FALSE(state->document.fresh_job());REQUIRE_FALSE(state->document.mask_contract_precision());
    REQUIRE(state->document.parameter_capabilities().empty());
    std::vector<std::vector<uint8_t>> expected;bool noncanonical=false;
    for(size_t i=0;i<state->document.layer_count();++i) {
        expected.push_back(state->document.image(i).buf);
        for(const auto value:expected.back())noncanonical|=value%17!=0;
    }
    REQUIRE(noncanonical);
    if(kind=="edited") {
        REQUIRE(expected[0].size()>=256);
        for(unsigned value=0;value<256;++value)REQUIRE(expected[0][value]==uint8_t(value));
        CHECK(state->document.image(0).get(2,2)==128);CHECK(state->document.image(0).get(2,3)==127);
    }
    auto restored=sla::restore_generated_project(*state);REQUIRE(restored);REQUIRE(restored->layer_document());
    const auto &document=*restored->layer_document();
    CHECK_FALSE(document.fresh_job());CHECK_FALSE(document.mask_contract_precision());
    CHECK(document.parameter_capabilities().empty());CHECK_FALSE(restored->calibration_recipe());
    CHECK_FALSE(document.can_undo());CHECK_FALSE(document.can_redo());CHECK_FALSE(document.dirty());
    for(size_t i=0;i<expected.size();++i)CHECK(bool(document.image(i).buf==expected[i]));
    auto editable=document;editable.paint(0,2,2,0,128);CHECK(editable.image(0).get(2,2)==128);
    const auto recaptured=sla::capture_generated_project(*restored,document);
    CHECK(recaptured.source_fingerprint.empty());CHECK(recaptured.origin==sla::ProjectLayerOrigin::GeneratedRaster);
    CHECK(bool(recaptured.positions_z==state->positions_z));
    const auto output=boost::filesystem::path(outputs)/(name+"-"+kind+".sla-project");
    boost::filesystem::create_directories(output.parent_path());REQUIRE_FALSE(boost::filesystem::exists(output));
    sla::save_layer_project(output.string(),recaptured);
    const auto reopened=sla::read_project_layers(output.string());REQUIRE(reopened);
    CHECK(reopened->source_fingerprint.empty());CHECK(reopened->origin==sla::ProjectLayerOrigin::GeneratedRaster);
    CHECK_FALSE(reopened->document.fresh_job());CHECK_FALSE(reopened->document.mask_contract_precision());
    CHECK(reopened->document.parameter_capabilities().empty());CHECK(bool(reopened->positions_z==state->positions_z));
    for(size_t i=0;i<expected.size();++i)CHECK(bool(reopened->document.image(i).buf==expected[i]));
    const auto saved=read_zipper_archive(output.string(),{"metadata/sla/layers.json"},{});
    REQUIRE(saved.entries.size()==1);const auto savedmeta=nlohmann::json::parse(saved.entries.front().buf);
    CHECK(bool(savedmeta==metadata)); // Includes exact settings, Z, origin and all document descriptors.
    CHECK(bool(bytes(path)==original));CHECK(bool(bytes(folder/"source.ini")==source_ini));
    CHECK(bool(bytes(folder/"effective.ini")==effective_ini));
}
