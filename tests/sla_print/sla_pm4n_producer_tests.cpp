// SPDX-License-Identifier: AGPL-3.0-or-later
// App-authored calibration, model and Gerber fixtures; no user model.
#include "libslic3r/SLAPrint.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/SLA/PCBExposure.hpp"
#include <array>
#include <cmath>
#include "libslic3r/SLA/CalibrationRaster.hpp"
#include "libslic3r/SLA/GeneratedLayerOwnership.hpp"
#include "libslic3r/SLA/ExportRequest.hpp"
#include "libslic3r/SLA/ProjectArchive.hpp"
#include "libslic3r/SLA/RasterBase.hpp"
#include "libslic3r/Format/ZipperArchiveImport.hpp"
#include <nlohmann/json.hpp>
#include <boost/filesystem.hpp>
#include <algorithm>
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
DynamicPrintConfig pm4n_config(const std::string &requested,bool portrait)
{
    DynamicPrintConfig config;config.apply(SLAFullPrintConfig{});
    config.option("printer_technology")->setInt(ptSLA);
    config.set("sla_archive_format",std::string("pm4n"));config.set("sla_anycubic_version",requested);
    const double width=153.408,height=87.040;
    config.set("display_width",portrait?height:width);config.set("display_height",portrait?width:height);
    const std::string w=std::to_string(portrait?height:width),h=std::to_string(portrait?width:height);
    config.set_deserialize_strict("bed_shape","0x0,"+w+"x0,"+w+"x"+h+",0x"+h);
    config.set("max_print_height",165.);
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
std::vector<std::vector<uint8_t>> pm4n_native(const boost::filesystem::path &path,
                                            unsigned version,size_t columns,size_t rows,size_t count=3,bool pcb=false)
{
    std::ifstream input(path.string(),std::ios::binary);REQUIRE(input.good());
    const std::vector<uint8_t> data{std::istreambuf_iterator<char>(input),{}};
    auto u32=[&](size_t at){REQUIRE(at<=data.size());REQUIRE(data.size()-at>=4);
        return uint32_t(data[at])|(uint32_t(data[at+1])<<8)|(uint32_t(data[at+2])<<16)|(uint32_t(data[at+3])<<24);};
    auto real=[&](size_t at){auto word=u32(at);float f;std::memcpy(&f,&word,4);return f;};
    REQUIRE(data.size()>=48);CHECK(bool(std::string(data.begin(),data.begin()+8)=="ANYCUBIC"));CHECK(u32(12)==version);
    const size_t h=u32(20),t=u32(36);CHECK(u32(h+60)==columns);CHECK(u32(h+64)==rows);REQUIRE(u32(t+16)==count);
    CHECK(real(h+20)==float(.05));CHECK(real(h+24)==(pcb?float(30.12):2.f));CHECK(real(h+32)==(pcb?float(30.12):10.f));CHECK(real(h+36)==(pcb?0.f:1.f));
    CHECK(real(h+28)==0.f);CHECK(real(h+40)==(pcb?0.f:3.f));CHECK(real(h+44)==(pcb?0.f:1.f));CHECK(real(h+48)==(pcb?0.f:2.f));
    CHECK(u32(h+84)==(pcb?(count==1?330u:360u):327u));CHECK(u32(h+88)==0); // Reference literal:300+10+2+2+3*(3+1.5)=327.5, uint truncation.
    std::vector<std::vector<uint8_t>> masks;
    for(size_t i=0;i<count;++i) {
        const size_t p=t+20+32*i,begin=u32(p),length=u32(p+4);REQUIRE(begin<=data.size());REQUIRE(length<=data.size()-begin);
        CHECK(real(p+8)==(pcb?0.f:3.f));CHECK(real(p+12)==(pcb?0.f:1.f));CHECK(real(p+16)==(pcb?float(30.12):float(i?2.:10.)));CHECK(real(p+20)==float(.05));
        std::vector<uint8_t> mask;mask.reserve(columns*rows);
        for(size_t at=begin;at<begin+length;) {
            const unsigned token=data[at++],level=token>>4;size_t count=token&15;
            if(level==0 || level==15){REQUIRE(at<begin+length);count=count*256+data[at++];}
            REQUIRE(count>0);REQUIRE(count<=columns*rows-mask.size());mask.insert(mask.end(),count,uint8_t(level*17));
        }
        REQUIRE(mask.size()==columns*rows);masks.push_back(std::move(mask));
    }
    return masks;
}
}
TEST_CASE("PM4n actual auto515 and explicit515 calibration preserve source palette and native clock",
          "[sla_pm4n_calibration_precision]")
{
    const std::string requested=GENERATE("auto","515","516");
    const bool portrait=GENERATE(false,true);CAPTURE(requested,portrait);
    const unsigned version=requested=="516"?516:515;const bool owned=version==516;
    const auto config=pm4n_config(requested,portrait);const auto original=config;
    CHECK(sla::generated_anycubic_version("pm4n",requested)==version);
    CHECK(sla::generated_anycubic_version("pm4n","auto")==515);
    sla::CalibrationParameters parameters;parameters.pattern=sla::CalibrationPattern::GrayscalePatches;
    parameters.size_x=parameters.size_y=8.;parameters.margin=2.;parameters.height=.15;
    parameters.grayscale_steps=2;parameters.grayscale_first=1;parameters.grayscale_last=15;
    const auto collapsed=sla::calibration_grayscale_plan(config,parameters);
    CHECK(collapsed.precision==sla::LayerMaskPrecision::NativeGray4);
    CHECK(bool(collapsed.effective==std::vector<uint8_t>{0,0}));CHECK(collapsed.has_duplicates());
    CHECK_THROWS(sla::create_calibration(config,parameters));
    parameters.grayscale_first=16;parameters.grayscale_last=47;
    const auto plan=sla::calibration_grayscale_plan(config,parameters);
    CHECK(plan.precision==sla::LayerMaskPrecision::NativeGray4);CHECK_FALSE(plan.has_duplicates());
    CHECK(bool(plan.requested==std::vector<uint8_t>{16,47}));CHECK(bool(plan.effective==std::vector<uint8_t>{17,34}));
    auto job=sla::create_calibration(config,parameters);REQUIRE(job);REQUIRE(job->layer_document());
    const auto source=*job->layer_document();REQUIRE(source.layer_count()==3);REQUIRE(job->calibration_recipe());
    CHECK(job->calibration_recipe()->precision==sla::LayerMaskPrecision::NativeGray4);
    CHECK(bool(job->calibration_recipe()->effective==std::vector<uint8_t>{17,34}));
    CHECK(bool(source.fresh_job())==owned);CHECK(source.parameter_capabilities().empty()==!owned);
    CHECK(source.mask_contract_precision()==(owned?std::optional<sla::LayerMaskPrecision>{}:
        std::optional<sla::LayerMaskPrecision>{sla::LayerMaskPrecision::NativeGray4}));
    CHECK_FALSE(source.can_undo());CHECK_FALSE(source.can_redo());CHECK_FALSE(source.dirty());
    auto refconfig=config;refconfig.set("sla_archive_format",std::string("uvj"));
    // Calibration draws the effective patch levels before antialiasing.
    // Use literal representable levels in the independent Gray8 reference;
    // Q(render(requested)) would incorrectly differ at the patch edges.
    auto reference_parameters=parameters;
    reference_parameters.grayscale_first=17;reference_parameters.grayscale_last=34;
    auto reference=sla::create_calibration(refconfig,reference_parameters);REQUIRE(reference);REQUIRE(reference->layer_document());
    const size_t columns=portrait?250:400,rows=portrait?400:250;
    for(size_t i=0;i<3;++i) {
        auto truth=reference->layer_document()->image(i).buf;for(auto &v:truth)v=uint8_t((v>>4)*17);
        CHECK(bool(source.image(i).buf==truth));CHECK(source.image(i).cols==columns);CHECK(source.image(i).rows==rows);
    }
    const char *root=std::getenv("SLA_PM4N_CALIBRATION_FIXTURES");
    const auto folder=root?boost::filesystem::path(root)/(requested+(portrait?"-portrait":"-landscape")):
        boost::filesystem::temp_directory_path()/boost::filesystem::unique_path("pm4n-calibration-%%%%");
    boost::filesystem::create_directories(folder);
    struct Cleanup {boost::filesystem::path p;bool keep;~Cleanup(){if(!keep){boost::system::error_code e;boost::filesystem::remove_all(p,e);}}} cleanup{folder,root!=nullptr};
    config.save((folder/"source.ini").string());
    for(size_t i=0;i<3;++i) {
        const auto png=reference->layer_document()->encoded_layer(i);
        std::ofstream output((folder/("gray8-reference-"+std::to_string(i)+".png")).string(),std::ios::binary);
        output.write(static_cast<const char *>(png.buf),png.sz);REQUIRE(output.good());
    }
    auto compare=[&](const boost::filesystem::path &path,const sla::LayerDocument &document) {
        const auto native=pm4n_native(path,version,columns,rows);
        for(size_t i=0;i<3;++i)CHECK(bool(native[i]==document.image(i).buf));
    };
    const auto direct=folder/"direct.pm4n";job->export_print(direct.string());compare(direct,source);
    const auto snapshot=std::make_shared<const sla::ExportSnapshot>(job->capture_export_snapshot());
    const auto frozen=folder/"frozen.pm4n";sla::ExportRequest(snapshot,"pm4n",frozen.string()).write();compare(frozen,source);
    auto save=[&](const std::string &name,const sla::LayerDocument &document) {
        const auto file=folder/(name+".sla-project");sla::save_layer_project(file.string(),sla::capture_generated_project(*job,document));
        const auto zip=read_zipper_archive(file.string(),{"metadata/sla/layers.json"},{});REQUIRE(zip.entries.size()==1);
        const auto data=nlohmann::json::parse(zip.entries.front().buf);CHECK(data.at("version")== (owned?6:8));
        CHECK(data.at("source_kind")=="generated_raster");CHECK(data.at("source_fingerprint")=="");
        CHECK(data.at("document").contains("fresh_job")==owned);CHECK(data.at("document").contains("mask_contract")==!owned);
        CHECK(data.at("calibration_recipe").at("precision")==18);CHECK(data.at("calibration_recipe").at("effective")==nlohmann::json::array({17,34}));
    };
    save("source",source);
    auto edited=source;auto image=edited.image(0);REQUIRE(image.buf.size()>=256);
    for(unsigned v=0;v<256;++v)image.buf[v]=uint8_t(v);
    edited.replace_image(0,image);
    for(unsigned v=0;v<256;++v)CHECK(edited.image(0).buf[v]==uint8_t((v>>4)*17));
    edited.paint(0,2,2,0,128);edited.paint(0,3,2,0,127);
    CHECK(edited.image(0).get(2,2)==136);CHECK(edited.image(0).get(2,3)==119);
    const auto changed=edited.image(0).buf;REQUIRE(edited.undo());REQUIRE(edited.redo());CHECK(bool(edited.image(0).buf==changed));
    job->replace_layer_document(edited);const auto edited_file=folder/"edited.pm4n";job->export_print(edited_file.string());compare(edited_file,edited);save("edited",edited);
    const auto state=sla::read_project_layers((folder/"edited.sla-project").string());REQUIRE(state);
    auto restored=sla::restore_generated_project(*state);REQUIRE(restored);REQUIRE(restored->layer_document());
    for(size_t i=0;i<3;++i)CHECK(bool(restored->layer_document()->image(i).buf==edited.image(i).buf));
    CHECK(bool(restored->layer_document()->fresh_job())==owned);CHECK(restored->layer_document()->mask_contract_precision()==source.mask_contract_precision());
    const auto restart=folder/"restart.pm4n";restored->export_print(restart.string());compare(restart,*restored->layer_document());
    if(!owned) {
        std::vector<uint8_t> spectrum(columns*rows,0);for(unsigned v=0;v<256;++v)spectrum[v]=uint8_t(v);
        const auto png=sla::PNGRasterEncoder{}(spectrum.data(),columns,rows,1);const auto raw=sla::LayerDocument::from_rasters({png,png,png});
        auto rawjob=SLAPrint::create_raster_job(config,raw,{.05,.1,.15});REQUIRE(rawjob);CHECK_FALSE(rawjob->layer_document()->fresh_job());CHECK_FALSE(rawjob->layer_document()->mask_contract_precision());
        const auto file=folder/"raw256.pm4n";rawjob->export_print(file.string());const auto decoded=pm4n_native(file,515,columns,rows);
        for(auto &v:spectrum)v=uint8_t((v>>4)*17);
        for(const auto &mask:decoded)CHECK(bool(mask==spectrum));
    }
    CHECK(config.diff(original).empty());
}

TEST_CASE("PM4n actual auto515 and explicit515 model preserve source precision and encoded time",
          "[sla_pm4n_model_precision]")
{
    const std::string format="pm4n";
    const std::string requested=GENERATE("auto","515","516");
    const unsigned version=requested=="516"?516:515;
    const bool portrait=GENERATE(false,true);CAPTURE(format,requested,version,portrait);
    const auto config=pm4n_config(requested,portrait);const auto initial=config;
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
    const char *fixtures=std::getenv("SLA_PM4N_MODEL_FIXTURES");
    const auto folder=fixtures?boost::filesystem::path(fixtures)/(requested+(portrait?"-portrait":"-landscape")):
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
        const auto masks=pm4n_native(path,version,columns,rows);
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


TEST_CASE("PM4n actual auto515 and explicit515 PCB preserve palette polarity and stationary time",
          "[sla_pm4n_pcb_precision]")
{
    const std::string format="pm4n";
    const std::string requested=GENERATE("auto","515","516");
    const unsigned version=requested=="516"?516:515;
    const bool portrait=GENERATE(false,true);
    const bool merge=GENERATE(false,true);
    const bool invert=GENERATE(false,true);
    CAPTURE(format,requested,version,portrait,merge,invert);const auto config=pm4n_config(requested,portrait);const auto accepted=config;
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
    const char *fixtures=std::getenv("SLA_PM4N_PCB_FIXTURES");
    const auto name=requested+(portrait?"-portrait":"-landscape")+(merge?"-merged":"-separate")+(invert?"-inverted":"-normal");
    const auto folder=fixtures?boost::filesystem::path(fixtures)/name:boost::filesystem::temp_directory_path()/boost::filesystem::unique_path("any-pcb-precision-%%%%");
    boost::filesystem::create_directories(folder);
    struct Cleanup{boost::filesystem::path path;bool keep;~Cleanup(){if(!keep){boost::system::error_code error;boost::filesystem::remove_all(path,error);}}} cleanup{folder,fixtures!=nullptr};
    config.save((folder/"source.ini").string());job->full_print_config().save((folder/"effective.ini").string());
    for(size_t layer=0;layer<count;++layer) {
        const auto png=reference.encoded_layer(layer);std::ofstream out((folder/("gray8-reference-"+std::to_string(layer)+".png")).string(),std::ios::binary);
        out.write(static_cast<const char *>(png.buf),png.sz);REQUIRE(out.good());
    }
    auto readback=[&](const boost::filesystem::path &path,const sla::LayerDocument &document) {
        const auto decoded=pm4n_native(path,version,columns,rows,count,true);
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

