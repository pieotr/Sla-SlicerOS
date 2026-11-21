// SPDX-License-Identifier: AGPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <boost/filesystem.hpp>
#include <cstdlib>
#include <limits>
#include <set>
#include <fstream>
#include <cstring>
#include "libslic3r/Format/AnycubicLayerLayout.hpp"
#include "libslic3r/Format/CrealityCXDLPv4Counts.hpp"
#include "libslic3r/Format/ChituboxLayerLayout.hpp"
#include "libslic3r/Format/CTBEncryptedLayerLayout.hpp"
#include "libslic3r/SLAPrint.hpp"
#include "libslic3r/Format/SLAArchiveFormatRegistry.hpp"
#include "libslic3r/SLA/LayerDocument.hpp"
#include "libslic3r/SLA/LayerParameterConversion.hpp"
#include "libslic3r/SLA/LayerConversionPlan.hpp"
#include "libslic3r/SLA/CTBEncryptedLayerParameters.hpp"
#include "libslic3r/SLA/CrealityCXDLPv4LayerParameters.hpp"
#include "libslic3r/SLA/CrealityCXDLPv4JobReader.hpp"
#include "libslic3r/SLA/CrealityCXDLPv4ReferenceBottom.hpp"
#include "libslic3r/SLA/ChituboxJobOverlay.hpp"
#include "libslic3r/SLA/ProjectArchive.hpp"
#include "libslic3r/miniz_extension.hpp"
#include "libslic3r/Zipper.hpp"
#include "libslic3r/Format/ZipperArchiveImport.hpp"
#include <nlohmann/json.hpp>
using namespace Slic3r;
TEST_CASE("Raster conversion preserves native CTB fields instead of target preset defaults", "[sla_reference][sla_layer_conversion]")
{
    const std::string source_format=GENERATE("ctb_v4","ctb_v5","ctb_encrypted_v4","ctb_encrypted");
    const std::string target=GENERATE("ctb_v4","ctb_v5","ctb_encrypted_v4","ctb_encrypted");
    const bool edited=GENERATE(false,true);
    const bool compact_z=GENERATE(false,true);
    CAPTURE(source_format,target,edited,compact_z);
    struct Directory {
        boost::filesystem::path path=boost::filesystem::unique_path("layer-conversion-%%%%");
        bool keep=false;
        ~Directory(){if(!keep){boost::system::error_code error;boost::filesystem::remove_all(path,error);}}
    } directory;
    if(const char *root=std::getenv("SLA_LAYER_CONVERSION_FIXTURES")) {
        directory.path=boost::filesystem::path(root)/(source_format+"-to-"+target+(edited?"-edited":"-pristine")+
            (compact_z?"-compact":"-grid"));directory.keep=true;
    }
    boost::filesystem::create_directories(directory.path);
    DynamicPrintConfig config;config.apply(SLAFullPrintConfig{});
    config.option("printer_technology")->setInt(ptSLA);config.set("sla_archive_format",source_format);
    config.set("display_pixels_x",8);config.set("display_pixels_y",4);
    config.set("display_width",16.);config.set("display_height",4.);
    config.option("display_orientation")->setInt(sladoLandscape);
    std::vector<uint8_t> pixels(32,0);pixels[1]=255;pixels[20]=255;
    const auto png=sla::PNGRasterEncoder{}(pixels.data(),8,4,1);
    auto job=SLAPrint::create_raster_job(config,sla::LayerDocument::from_rasters({png,png,png}),{.1,.15,.2});
    const auto source=directory.path/(std::string("source.")+get_default_extension(source_format.c_str()));
    job->export_print(source.string());
    auto document=sla::LayerDocument::load_archive(source.string());using F=sla::LayerParameter;
    // Persist a genuinely variable source, then test both its pristine import
    // and additional unsaved changes. A pristine import is not a uniform job.
    document.set_layer_parameters(1,{{F::PositionZ,.16},{F::Exposure,7.25},{F::LightOff,1.25},
        {F::LiftDistance,3},{F::LiftSpeed,75},{F::LiftDistance2,2},{F::LiftSpeed2,180},
        {F::RetractSpeed,150},{F::RetractDistance2,1.5},{F::RetractSpeed2,90},
        {F::WaitBefore,.75},{F::WaitAfter,.5},{F::WaitAfterLift,.25},{F::LightPWM,128}});
    if(compact_z) {
        document.set_layer_parameters(1,{{F::PositionZ,.1002}});
        document.set_layer_parameters(2,{{F::PositionZ,.1003}});
        document.set_layer_parameters(0,{{F::PositionZ,.1001}});
    }
    document.save_archive(source.string());document=sla::LayerDocument::load_archive(source.string());
    REQUIRE_FALSE(document.has_parameter_edits());
    REQUIRE_THROWS(SLAPrint::create_raster_job(config,document,{.1,.16,.2}));
    if(edited)document.set_layer_parameters(1,{{F::Exposure,8.5},{F::LightPWM,96}});
    if(directory.keep) {
        auto expected=document;expected.save_archive((directory.path/(std::string("expected.")+
            get_default_extension(source_format.c_str()))).string());
        config.save((directory.path/"source.ini").string());
    }
    const auto revision=document.revision();
    const auto plan = sla::plan_layer_conversion(document, target);
    REQUIRE(plan.destination == target);
    REQUIRE(plan.rows.size() == document.layer_count());
    REQUIRE(plan.globals.has_value());
    for (size_t i = 0; i < document.layer_count(); ++i)
        REQUIRE(plan.rows[i] == document.layer_parameters(i));
    REQUIRE_THROWS_WITH(sla::plan_layer_conversion(document, "ctb_v3"),
        "Destination ctb_v3 cannot preserve source field: Lift distance 2 (mm)");
    REQUIRE_THROWS_WITH(sla::plan_layer_conversion(document, "sl1"),
        "This destination has no implemented parameter-preserving conversion yet");
    REQUIRE(document.revision() == revision);
    auto context=SLAPrint::create_raster_job(config,sla::LayerDocument::from_rasters({png,png,png}),{.1,.16,.2});
    const auto output=directory.path/(std::string("converted.")+get_default_extension(target.c_str()));
    if(edited)REQUIRE_NOTHROW(SLAPrint::convert_layer_archive(config,document,target,output.string()));
    else REQUIRE_NOTHROW(context->export_layer_document(document,target,output.string(),{},"conversion"));
    const auto reopened=sla::LayerDocument::load_archive(output.string());
    REQUIRE(reopened.archive_format()==target);
    for(size_t i=0;i<3;++i){
        REQUIRE(reopened.layer_parameters(i)==document.layer_parameters(i));
        REQUIRE(reopened.image(i).buf==document.image(i).buf);
    }
    auto batch=reopened;
    std::vector<sla::LayerParameters> table;
    for(size_t i=0;i<3;++i) {
        table.push_back(batch.layer_parameters(i));
        table.back()[F::PositionZ]=double(float(.21+.01*i));
    }
    // Moving all rows beyond the old neighbours is legal in the final table.
    batch.replace_layer_parameter_table(table);
    for(size_t i=0;i<3;++i)REQUIRE(batch.layer_parameters(i)==table[i]);
    REQUIRE(batch.undo());REQUIRE_FALSE(batch.can_undo());REQUIRE(batch.can_redo());
    const auto batch_revision=batch.revision();
    for(size_t i=0;i<3;++i)REQUIRE(batch.layer_parameters(i)==reopened.layer_parameters(i));
    for(int malformed=0;malformed<4;++malformed) {
        auto invalid=table;
        if(malformed==0)invalid[1][F::PositionZ]=invalid[0][F::PositionZ];
        if(malformed==1)invalid[1].erase(F::Exposure);
        if(malformed==2)invalid[1][F::Exposure]=.1; // Not an exact native float.
        if(malformed==3)invalid.pop_back();
        REQUIRE_THROWS(batch.replace_layer_parameter_table(invalid));
        REQUIRE(batch.revision()==batch_revision);REQUIRE(batch.can_redo());
        for(size_t i=0;i<3;++i)REQUIRE(batch.layer_parameters(i)==reopened.layer_parameters(i));
    }
    REQUIRE(batch.redo());
    for(size_t i=0;i<3;++i)REQUIRE(batch.layer_parameters(i)==table[i]);
    REQUIRE(document.revision()==revision);
    const auto incompatible=directory.path/"keep.v3.ctb";
    {std::ofstream existing(incompatible.string(),std::ios::binary);existing<<"previous valid output";}
    REQUIRE_THROWS(context->export_layer_document(document,"ctb_v3",incompatible.string(),{},"conversion"));
    std::ifstream existing(incompatible.string(),std::ios::binary);
    REQUIRE(std::string(std::istreambuf_iterator<char>(existing),{})=="previous valid output");
}
TEST_CASE("Fractional CTB lift phases survive editing and conversion", "[sla_reference][sla_layer_conversion][sla_fractional_lift]")
{
    const std::string source_format=GENERATE("ctb_v4","ctb_v5","ctb_encrypted_v4","ctb_encrypted");
    const std::string target=GENERATE("ctb_v4","ctb_v5","ctb_encrypted_v4","ctb_encrypted");
    CAPTURE(source_format,target);
    struct Directory {boost::filesystem::path path=boost::filesystem::unique_path("fractional-lift-%%%%");
        bool keep=false;
        ~Directory(){if(!keep){boost::system::error_code error;boost::filesystem::remove_all(path,error);}}} directory;
    if(const char *root=std::getenv("SLA_FRACTIONAL_LIFT_FIXTURES")) {
        directory.path=boost::filesystem::path(root)/(source_format+"-to-"+target);directory.keep=true;
    }
    boost::filesystem::create_directories(directory.path);
    DynamicPrintConfig config;config.apply(SLAFullPrintConfig{});config.option("printer_technology")->setInt(ptSLA);
    config.set("sla_archive_format",source_format);
    config.set("display_pixels_x",8);config.set("display_pixels_y",4);config.set("display_width",16.);config.set("display_height",4.);
    config.option("display_orientation")->setInt(sladoLandscape);
    std::vector<uint8_t> pixels(32,0);pixels[1]=255;
    const auto png=sla::PNGRasterEncoder{}(pixels.data(),8,4,1);
    auto job=SLAPrint::create_raster_job(config,sla::LayerDocument::from_rasters({png}),{.1});
    const auto source=directory.path/(std::string("source.")+get_default_extension(source_format.c_str()));
    job->export_print(source.string());auto document=sla::LayerDocument::load_archive(source.string());
    using F=sla::LayerParameter;
    document.set_layer_parameters(0,{{F::LiftDistance,5.1},{F::LiftDistance2,.01},{F::LiftSpeed2,120}});
    const auto edited=document.layer_parameters(0);
    // The wire format stores total and second as floats, not first and second.
    // The first phase must therefore already match their difference in the UI.
    CHECK(edited.at(F::LiftDistance)==double(float(5.1+double(float(.01))))-double(float(.01)));
    REQUIRE_NOTHROW(document.save_archive(source.string()));
    auto reopened=sla::LayerDocument::load_archive(source.string());
    CHECK(reopened.layer_parameters(0)==edited);
    const auto output=directory.path/(std::string("converted.")+get_default_extension(target.c_str()));
    REQUIRE_NOTHROW(SLAPrint::convert_layer_archive(config,reopened,target,output.string()));
    auto converted=sla::LayerDocument::load_archive(output.string());
    REQUIRE(converted.layer_parameters(0)==reopened.layer_parameters(0));
    REQUIRE(converted.image(0).buf==reopened.image(0).buf);
    // Editing only the second phase still preserves the requested first phase
    // through the total field's representation, and remains one Undo action.
    const auto before=converted.layer_parameters(0);
    converted.set_layer_parameters(0,{{F::LiftDistance2,.03}});
    const auto after=converted.layer_parameters(0);
    converted.save_archive(output.string());
    REQUIRE(sla::LayerDocument::load_archive(output.string()).layer_parameters(0)==after);
    REQUIRE(converted.undo());REQUIRE(converted.layer_parameters(0)==before);
}
TEST_CASE("Anycubic conversion preserves motion meaning in CTB", "[sla_reference][sla_layer_conversion][sla_anycubic_conversion]")
{
    const std::string version=GENERATE("516","517");
    const std::string target=GENERATE("ctb_v4","ctb_v5","ctb_encrypted_v4","ctb_encrypted");
    const bool portrait=GENERATE(false,true),second_phase=GENERATE(false,true);
    CAPTURE(version,target,portrait,second_phase);
    struct Directory {boost::filesystem::path path=boost::filesystem::unique_path("any-conversion-%%%%");
        bool keep=false;
        ~Directory(){if(!keep){boost::system::error_code error;boost::filesystem::remove_all(path,error);}}} directory;
    if(const char *root=std::getenv("SLA_ANY_CONVERSION_FIXTURES")) {
        directory.path=boost::filesystem::path(root)/(version+"-to-"+target+(portrait?"-portrait":"-landscape")+(second_phase?"-two":"-one"));directory.keep=true;
    }
    boost::filesystem::create_directories(directory.path);
    DynamicPrintConfig config;config.apply(SLAFullPrintConfig{});config.option("printer_technology")->setInt(ptSLA);
    config.set("sla_archive_format",std::string("pwc"));config.set("sla_anycubic_version",version);
    config.set("display_pixels_x",8);config.set("display_pixels_y",4);config.set("display_width",16.);config.set("display_height",4.);
    config.option("display_orientation")->setInt(portrait?sladoPortrait:sladoLandscape);
    for(const char *prefix:{"sla_","sla_bottom_"}) {
        config.set(std::string(prefix)+"lift_height",3.);
        config.set(std::string(prefix)+"lift_height2",second_phase?2.:0.);
        config.set(std::string(prefix)+"lift_speed",200.);
        config.set(std::string(prefix)+"lift_speed2",120.);
        config.set(std::string(prefix)+"retract_speed",200.);
        config.set(std::string(prefix)+"retract_speed2",120.);
        config.set(std::string(prefix)+"wait_before_cure",.75);
    }
    std::vector<uint8_t> pixels(32,0);pixels[1]=255;pixels[20]=255;
    const auto png=sla::PNGRasterEncoder{}(pixels.data(),portrait?4:8,portrait?8:4,1);
    auto job=SLAPrint::create_raster_job(config,sla::LayerDocument::from_rasters({png,png,png}),{.1,.151,.203});
    const auto source=directory.path/"source.pwc";job->export_print(source.string());
    auto document=sla::LayerDocument::load_archive(source.string());using F=sla::LayerParameter;
    // Target defaults must not create waits, PWM or retract phases that the
    // source format cannot encode. The mapper has to describe those absences.
    for(const char *prefix:{"sla_","sla_bottom_"}) {
        config.set(std::string(prefix)+"wait_after_cure",.5);
        config.set(std::string(prefix)+"wait_after_lift",.25);
        config.set(std::string(prefix)+"retract_height2",1.);
        config.set(std::string(prefix)+"light_pwm",128);
    }
    const auto output=directory.path/(std::string("converted.")+get_default_extension(target.c_str()));
    REQUIRE_NOTHROW(SLAPrint::convert_layer_archive(config,document,target,output.string()));
    const auto converted=sla::LayerDocument::load_archive(output.string());
    for(size_t i=0;i<3;++i) {
        const auto before=document.layer_parameters(i),after=converted.layer_parameters(i);
        REQUIRE(after.at(F::PositionZ)==double(float(before.at(F::PositionZ))));
        REQUIRE(after.at(F::Exposure)==before.at(F::Exposure));
        REQUIRE(after.at(F::LiftDistance)==3.);
        REQUIRE(after.at(F::LiftDistance2)==(second_phase?2.:0.));
        REQUIRE(after.at(F::LiftSpeed)==double(float(before.at(F::LiftSpeed))));
        REQUIRE(after.at(F::RetractSpeed)==double(float(before.at(F::RetractSpeed))));
        REQUIRE(after.at(F::WaitBefore)==.75);
        REQUIRE(after.at(F::WaitAfter)==0.);REQUIRE(after.at(F::WaitAfterLift)==0.);
        REQUIRE(after.at(F::LightOff)==0.);REQUIRE(after.at(F::LightPWM)==255.);
        REQUIRE(after.at(F::RetractDistance2)==0.);
        REQUIRE(converted.image(i).buf==document.image(i).buf);
    }
    REQUIRE_FALSE(document.dirty());REQUIRE_FALSE(document.can_undo());
}
TEST_CASE("Anycubic conversion preserves fractional lift and unsaved exposure edits", "[sla_reference][sla_layer_conversion][sla_anycubic_conversion_edges]")
{
    const std::string target=GENERATE("ctb_v4","ctb_v5","ctb_encrypted_v4","ctb_encrypted");
    const bool edited=GENERATE(false,true);
    CAPTURE(target,edited);
    struct Directory {
        boost::filesystem::path path=boost::filesystem::unique_path("any-fractional-%%%%");
        ~Directory(){boost::system::error_code error;boost::filesystem::remove_all(path,error);}
    } directory;
    boost::filesystem::create_directories(directory.path);
    DynamicPrintConfig config;config.apply(SLAFullPrintConfig{});
    config.option("printer_technology")->setInt(ptSLA);config.set("sla_archive_format",std::string("pwc"));
    config.set("sla_anycubic_version",std::string("516"));
    config.set("display_pixels_x",8);config.set("display_pixels_y",4);
    config.set("display_width",16.);config.set("display_height",4.);
    config.option("display_orientation")->setInt(sladoLandscape);
    for(const char *prefix:{"sla_","sla_bottom_"}) {
        config.set(std::string(prefix)+"lift_height",5.1);
        config.set(std::string(prefix)+"lift_height2",.01);
        config.set(std::string(prefix)+"lift_speed",200.);
        config.set(std::string(prefix)+"lift_speed2",120.);
        config.set(std::string(prefix)+"retract_speed",200.);
        config.set(std::string(prefix)+"retract_speed2",120.);
    }
    std::vector<uint8_t> pixels(32,0);pixels[3]=255;
    const auto png=sla::PNGRasterEncoder{}(pixels.data(),8,4,1);
    auto job=SLAPrint::create_raster_job(config,sla::LayerDocument::from_rasters({png,png}),{.1,.15});
    const auto source=directory.path/"source.pwc";
    job->export_print(source.string());auto document=sla::LayerDocument::load_archive(source.string());
    using F=sla::LayerParameter;
    if(edited)document.set_layer_parameters(1,{{F::Exposure,8.5}});
    const auto revision=document.revision();
    const auto output=directory.path/(std::string("converted.")+get_default_extension(target.c_str()));
    REQUIRE_NOTHROW(SLAPrint::convert_layer_archive(config,document,target,output.string()));
    const auto converted=sla::LayerDocument::load_archive(output.string());
    for(size_t i=0;i<2;++i) {
        const auto before=document.layer_parameters(i),after=converted.layer_parameters(i);
        // Source exposes TOTAL lift; the destination exposes FIRST. Fractional
        // fields must remain their native total-minus-second representation.
        REQUIRE(after.at(F::LiftDistance)==before.at(F::LiftDistance)-before.at(F::LiftDistance2));
        REQUIRE(after.at(F::LiftDistance)+after.at(F::LiftDistance2)==before.at(F::LiftDistance));
        REQUIRE(after.at(F::Exposure)==before.at(F::Exposure));
        REQUIRE(converted.image(i).buf==pixels);
    }
    REQUIRE(document.revision()==revision);
    REQUIRE(document.dirty()==edited);REQUIRE(document.can_undo()==edited);
    if(edited) {REQUIRE(document.undo());REQUIRE_FALSE(document.dirty());}
}

TEST_CASE("Anycubic conversion rejects absolute Z collapse without publishing", "[sla_reference][sla_layer_conversion][sla_anycubic_conversion_edges]")
{
    const std::string target=GENERATE("ctb_v4","ctb_v5","ctb_encrypted_v4","ctb_encrypted");
    CAPTURE(target);
    struct Directory {
        boost::filesystem::path path=boost::filesystem::unique_path("any-z-collapse-%%%%");
        ~Directory(){boost::system::error_code error;boost::filesystem::remove_all(path,error);}
    } directory;
    boost::filesystem::create_directories(directory.path);
    DynamicPrintConfig config;config.apply(SLAFullPrintConfig{});
    config.option("printer_technology")->setInt(ptSLA);config.set("sla_archive_format",std::string("pwc"));
    config.set("sla_anycubic_version",std::string("516"));
    config.set("display_pixels_x",8);config.set("display_pixels_y",4);
    config.set("display_width",16.);config.set("display_height",4.);
    config.option("display_orientation")->setInt(sladoLandscape);
    std::vector<uint8_t> pixels(32,0);pixels[3]=255;
    const auto png=sla::PNGRasterEncoder{}(pixels.data(),8,4,1);
    auto job=SLAPrint::create_raster_job(config,sla::LayerDocument::from_rasters({png,png}),{.1,.15});
    const auto source=directory.path/"source.pwc";
    job->export_print(source.string());
    auto bytes=[&](const boost::filesystem::path &path) {
        std::ifstream stream(path.string(),std::ios::binary);
        return std::vector<uint8_t>{std::istreambuf_iterator<char>(stream),{}};
    };
    auto native=bytes(source);const AnycubicLayerLayout layout(native,"pwc");
    // A positive relative float height can advance the source double sum while
    // remaining smaller than one absolute CTB float ULP at this Z.
    const float tiny_height=1e-10f;uint32_t bits;std::memcpy(&bits,&tiny_height,4);
    const auto address=layout.record(1)+20;
    for(size_t n=0;n<4;++n)native[address+n]=uint8_t(bits>>(8*n));
    {std::ofstream file(source.string(),std::ios::binary|std::ios::trunc);file.write(reinterpret_cast<const char *>(native.data()),native.size());}
    auto document=sla::LayerDocument::load_archive(source.string());using F=sla::LayerParameter;
    const double first=document.layer_parameters(0).at(F::PositionZ),second=document.layer_parameters(1).at(F::PositionZ);
    REQUIRE(second>first);REQUIRE(float(first)==float(second));
    const auto revision=document.revision();
    const auto output=directory.path/(std::string("keep.")+get_default_extension(target.c_str()));
    {std::ofstream file(output.string(),std::ios::binary);file<<"previous valid output";}
    const auto previous=bytes(output);
    REQUIRE_THROWS_WITH(sla::plan_layer_conversion(document, target),
        "Destination float precision cannot preserve strictly increasing layer Z");
    REQUIRE_THROWS_WITH(SLAPrint::convert_layer_archive(config,document,target,output.string()),
        "Destination float precision cannot preserve strictly increasing layer Z");
    REQUIRE(bytes(output)==previous);REQUIRE(bytes(source)==native);
    REQUIRE(document.revision()==revision);REQUIRE_FALSE(document.dirty());REQUIRE_FALSE(document.can_undo());
    REQUIRE_THROWS_WITH(job->export_layer_document(document,"ctb_v3",output.string(),{},"conversion"),
        "Destination ctb_v3 cannot preserve source field: Global wait before exposure (s)");
    REQUIRE(bytes(output)==previous);
}

TEST_CASE("Anycubic parameter mapping rejects invalid native ranges and phases", "[sla_reference][sla_layer_conversion][sla_anycubic_conversion_edges]")
{
    using F=sla::LayerParameter;
    const sla::LayerParameters source{{F::PositionZ,.1},{F::Exposure,8.5},{F::LiftDistance,5.},
        {F::LiftDistance2,2.},{F::LiftSpeed,200.},{F::LiftSpeed2,120.},
        {F::RetractSpeed,200.},{F::RetractSpeed2,120.},{F::WaitBefore,.75}};
    const auto schema=sla::parameter_conversion_schema("ctb_v4");
    for(double invalid:{-1.,std::numeric_limits<double>::infinity(),
                        std::numeric_limits<double>::quiet_NaN(),double(std::numeric_limits<float>::max())*2.}) {
        auto parameters=source;parameters[F::LiftSpeed]=invalid;
        REQUIRE_THROWS(sla::anycubic_parameters_for_ctb(parameters,schema));
    }
    auto invalid=source;invalid[F::LiftDistance]=1.;
    REQUIRE_THROWS_WITH(sla::anycubic_parameters_for_ctb(invalid,schema),
        "Source total lift is smaller than its second phase");
    invalid=source;invalid[F::LiftSpeed]=0.;
    REQUIRE_THROWS_WITH(sla::anycubic_parameters_for_ctb(invalid,schema),
        "A moving phase requires a positive speed");
    invalid=source;invalid.erase(F::Exposure);
    REQUIRE_THROWS_WITH(sla::anycubic_parameters_for_ctb(invalid,schema),
        "Source layer is missing a required motion or exposure parameter");
}

TEST_CASE("Native conversion rejects a destination that changes exposure pixels", "[sla_reference][sla_layer_conversion]")
{
    struct Directory {boost::filesystem::path path=boost::filesystem::unique_path("conversion-gray-%%%%");
        ~Directory(){boost::system::error_code error;boost::filesystem::remove_all(path,error);}} directory;
    boost::filesystem::create_directories(directory.path);
    DynamicPrintConfig config;config.apply(SLAFullPrintConfig{});config.option("printer_technology")->setInt(ptSLA);
    config.set("sla_archive_format",std::string("pwc"));config.set("sla_anycubic_version",std::string("516"));
    config.set("display_pixels_x",8);config.set("display_pixels_y",4);config.set("display_width",16.);config.set("display_height",4.);
    config.option("display_orientation")->setInt(sladoLandscape);
    std::vector<uint8_t> pixels(32,0);pixels[1]=34;
    for(const char *prefix:{"sla_","sla_bottom_"})
        for(const char *suffix:{"lift_speed","lift_speed2","retract_speed","retract_speed2"})
            config.set(std::string(prefix)+suffix,60.);
    const auto png=sla::PNGRasterEncoder{}(pixels.data(),8,4,1);
    auto job=SLAPrint::create_raster_job(config,sla::LayerDocument::from_rasters({png}),{.1});
    const auto source=directory.path/"source.pwc",output=directory.path/"keep.v4.ctb";
    job->export_print(source.string());auto document=sla::LayerDocument::load_archive(source.string());
    REQUIRE(document.image(0).buf[1]==34);
    {std::ofstream existing(output.string(),std::ios::binary);existing<<"previous output";}
    bool rejected=false;
    try {SLAPrint::convert_layer_archive(config,document,"ctb_v4",output.string());}
    catch(const std::exception &error) {
        rejected=true;CHECK(std::string(error.what())=="Destination grayscale precision cannot preserve the source exposure pixels");
    }
    REQUIRE(rejected);
    std::ifstream existing(output.string(),std::ios::binary);
    REQUIRE(std::string(std::istreambuf_iterator<char>(existing),{})=="previous output");
    REQUIRE_FALSE(document.dirty());REQUIRE_FALSE(document.can_undo());
}
namespace {
void verify_project_parameters(const sla::LayerDocument &document,const boost::filesystem::path &directory) {
    const auto project=directory/"parameters-project.3mf";
    struct Writer {mz_zip_archive archive{};~Writer(){if(archive.m_pState)close_zip_writer(&archive);}} writer;
    REQUIRE(open_zip_writer(&writer.archive,project.string()));
    const sla::ProjectRasterState state{document,{.1,.15,.2},std::string(40,'a')};
    REQUIRE_NOTHROW(sla::write_project_layers(writer.archive,state));
    REQUIRE(mz_zip_writer_finalize_archive(&writer.archive));REQUIRE(close_zip_writer(&writer.archive));
    auto restored=sla::read_project_layers(project.string());REQUIRE(restored.has_value());
    REQUIRE(restored->document.archive_format()==document.archive_format());
    for(size_t i=0;i<document.layer_count();++i) {
        REQUIRE(restored->document.layer_parameters(i)==document.layer_parameters(i));
        REQUIRE(restored->document.image(i).buf==document.image(i).buf);
    }
    auto expected=document;
    const std::string extension=get_default_extension(document.archive_format().c_str());
    const auto before=directory/("project-expected."+extension),after=directory/("project-restored."+extension);
    expected.save_archive(before.string());restored->document.save_archive(after.string());
    auto bytes=[](const boost::filesystem::path &path){std::ifstream in(path.string(),std::ios::binary);return std::vector<uint8_t>{std::istreambuf_iterator<char>(in),{}};};
    REQUIRE(bytes(before)==bytes(after));
    const auto original=read_zipper_archive(project.string(),{"metadata/sla/"},{});
    auto probe=[&](const std::string &name,const auto &mutate,bool reject=true) {
        auto entries=original.entries;
        auto manifest=std::find_if(entries.begin(),entries.end(),[](const auto &e){return e.fname=="metadata/sla/layers.json";});
        REQUIRE(manifest!=entries.end());auto json=nlohmann::json::parse(manifest->buf);
        auto payload=[&](size_t index)->std::vector<uint8_t>& {
            auto number=std::to_string(index);number=std::string(8-number.size(),'0')+number;
            const auto found=std::find_if(entries.begin(),entries.end(),[&](const auto &e){return e.fname=="metadata/sla/document/"+number+".bin";});
            if(found==entries.end())throw std::runtime_error("Missing test payload");
            return found->buf;
        };
        mutate(json,payload);const auto text=json.dump();manifest->buf.assign(text.begin(),text.end());
        const auto damaged=directory/("invalid-project-"+name+".3mf");
        {Zipper zip(damaged.string());for(const auto &entry:entries)zip.add_entry(entry.fname,entry.buf.data(),entry.buf.size());zip.finalize();}
        CAPTURE(name,document.archive_format());
        if(reject)CHECK_THROWS(sla::read_project_layers(damaged.string()));
        else REQUIRE_NOTHROW(sla::read_project_layers(damaged.string()));
    };
    probe("native-geometry",[&](auto &json,auto &payload){
        const auto columns=json["columns"].template get<size_t>(),rows=json["rows"].template get<size_t>();
        json["columns"]=rows;json["rows"]=columns;std::set<size_t> masks;
        for(const auto *key:{"main","secondary"})for(const auto &index:json["document"][key])masks.insert(index.template get<size_t>());
        for(const auto index:masks) {
            auto &data=payload(index);png::ImageGreyscale image;REQUIRE(png::decode_png({data.data(),data.size()},image));
            const auto encoded=sla::PNGRasterEncoder{}(image.buf.data(),rows,columns,1);
            const auto *start=static_cast<const uint8_t *>(encoded.data());data.assign(start,start+encoded.size());
        }
    });
    bool known_height=true;
    if(document.archive_format()=="pwc") {
        const auto source=std::find_if(original.entries.begin(),original.entries.end(),[](const auto &entry){return entry.fname=="metadata/sla/document/00000000.bin";});
        REQUIRE(source!=original.entries.end());
        known_height=AnycubicLayerLayout(source->buf,"pwc").version>=516;
    }
    probe("height-limit",[](auto &json,auto &payload){
        auto &data=payload(json["document"]["parameters"].back().template get<size_t>());
        auto values=nlohmann::json::parse(data);values["0"]=10000.;const auto text=values.dump();data.assign(text.begin(),text.end());
    },known_height); // PWC legacy has no encoded machine Z; do not invent its limit.
    probe("noncanonical-float",[](auto &json,auto &payload){
        auto &data=payload(json["document"]["parameters"][1].template get<size_t>());
        auto values=nlohmann::json::parse(data);values["1"]=.1;
        const auto text=values.dump();data.assign(text.begin(),text.end());
    });
    probe("parameter-key",[](auto &json,auto &payload){
        auto &data=payload(json["document"]["parameters"][0].template get<size_t>());
        auto values=nlohmann::json::parse(data);values["0suffix"]=values["0"];values.erase("0");
        const auto text=values.dump();data.assign(text.begin(),text.end());
    });
    for(const auto &cap:document.parameter_capabilities())if(!cap.editable) {
        probe("readonly-"+std::to_string(int(cap.field)),[&](auto &json,auto &payload){
            auto &data=payload(json["document"]["parameters"][0].template get<size_t>());
            auto values=nlohmann::json::parse(data);const auto key=std::to_string(int(cap.field));
            values[key]=values[key].template get<double>()+1;const auto text=values.dump();data.assign(text.begin(),text.end());
        });
    }
    const auto format=document.archive_format();
    if(format=="ctb_v4" || format=="ctb_v5" || format=="ctb_encrypted_v4" || format=="ctb_encrypted")
        probe("native-version",[&](auto &json,auto &){json["document"]["archive_format"]=format=="ctb_v4"?"ctb_v5":format=="ctb_v5"?"ctb_v4":format=="ctb_encrypted_v4"?"ctb_encrypted":"ctb_encrypted_v4";});
    if(document.mask_plane_count()>1)
        probe("missing-secondary",[](auto &json,auto &){json["document"]["secondary"]=nlohmann::json::array();});

}
}
TEST_CASE("Anycubic exposes typed layer parameters without invented defaults", "[sla_reference][sla_layer_parameters]")
{
    const std::string variant=GENERATE("pwc-1","pwc-515","pwc-516","pwc-517","pwc-518","pws-1","pm5s-518");
    const bool portrait=GENERATE(false,true);
    const auto dash=variant.find('-');const auto format=variant.substr(0,dash),version=variant.substr(dash+1);
    CAPTURE(variant,portrait);
    struct Directory {
        boost::filesystem::path path=boost::filesystem::unique_path("layer-parameters-%%%%");bool keep=false;
        ~Directory(){if(!keep){boost::system::error_code e;boost::filesystem::remove_all(path,e);}}
    } dir;
    if(const char *root=std::getenv("SLA_LAYER_PARAMETERS_FIXTURES")) {
        dir.path=boost::filesystem::path(root)/(variant+(portrait?"-portrait":"-landscape"));dir.keep=true;
    }
    boost::filesystem::create_directories(dir.path);
    DynamicPrintConfig cfg;cfg.apply(SLAFullPrintConfig{});cfg.option("printer_technology")->setInt(ptSLA);
    cfg.set("sla_archive_format",format);cfg.set("sla_anycubic_version",version);
    cfg.set("display_pixels_x",8);cfg.set("display_pixels_y",4);cfg.set("display_width",16.);cfg.set("display_height",4.);
    cfg.option("display_orientation")->setInt(portrait?sladoPortrait:sladoLandscape);
    if (const auto *geometry=anycubic_legacy_geometry(format);std::stoul(version)<=515 && geometry) {
        cfg.set("display_width",portrait?geometry->height:geometry->width);
        cfg.set("display_height",portrait?geometry->width:geometry->height);
        cfg.set("max_print_height",geometry->machine_z);
    }
    std::vector<uint8_t> pixels(32,0);pixels[1]=255;pixels[20]=255;
    const auto png=sla::PNGRasterEncoder{}(pixels.data(),portrait?4:8,portrait?8:4,1);
    auto job=SLAPrint::create_raster_job(cfg,sla::LayerDocument::from_rasters({png,png,png}),{.1,.15,.2});
    const auto source=dir.path/("source."+format);job->export_print(source.string());cfg.save((dir.path/"source.ini").string());
    auto document=sla::LayerDocument::load_archive(source.string());
    const auto capabilities=document.parameter_capabilities();REQUIRE(capabilities.size()==(std::stoul(version)>=516?9:6));
    using Field=sla::LayerParameter;
    const auto parameters=document.layer_parameters(1);REQUIRE(parameters.size()==(std::stoul(version)>=516?9:6));
    REQUIRE(parameters.at(Field::PositionZ)==Catch::Approx(.15));
    REQUIRE(parameters.at(Field::Exposure)==Catch::Approx(15));
    REQUIRE(parameters.at(Field::LiftSpeed)==Catch::Approx(60));
    REQUIRE(parameters.count(Field::LightPWM)==0);
    REQUIRE_FALSE(document.dirty());REQUIRE_FALSE(document.has_parameter_edits());
    document.set_layer_parameters(1,{{Field::Exposure,7.25},{Field::PositionZ,.16}});
    REQUIRE(document.dirty());REQUIRE(document.has_parameter_edits());
    const auto saved=dir.path/("parameters."+format);document.save_archive(saved.string());
    auto reopened=sla::LayerDocument::load_archive(saved.string());
    verify_project_parameters(document,dir.path);
    REQUIRE(reopened.layer_parameters(1).at(Field::Exposure)==Catch::Approx(7.25));
    REQUIRE(reopened.layer_parameters(1).at(Field::PositionZ)==Catch::Approx(.16));
    REQUIRE(reopened.layer_parameters(2).at(Field::PositionZ)==Catch::Approx(.2));
    for(size_t i=0;i<3;++i) REQUIRE(reopened.image(i).buf==document.image(i).buf);
    REQUIRE_THROWS(SLAPrint::create_raster_job(cfg,document,{.1,.16,.2}));
    REQUIRE(document.undo());REQUIRE_FALSE(document.has_parameter_edits());
    REQUIRE(document.layer_parameters(1)==parameters);REQUIRE(document.dirty()); // Undo differs from the most recently saved edit.
    const auto restored=dir.path/("restored."+format);document.save_archive(restored.string());
    auto read=[](const boost::filesystem::path &path){std::ifstream in(path.string(),std::ios::binary);return std::vector<uint8_t>{std::istreambuf_iterator<char>(in),{}};};
    REQUIRE(read(restored)==read(source));REQUIRE(document.redo());REQUIRE(document.has_parameter_edits());
    const auto revision=document.revision();const auto snapshot=document.layer_parameters(1);
    for(const auto &patch:std::vector<sla::LayerParameters>{{{Field::PositionZ,.1}},{{Field::PositionZ,.2}},
        {{Field::Exposure,-1}},{{Field::Exposure,std::numeric_limits<double>::infinity()}},
        {{Field::Exposure,std::numeric_limits<double>::quiet_NaN()}},{{Field::LightPWM,128}},{{Field::LiftSpeed,0}}}) {
        REQUIRE_THROWS(document.set_layer_parameters(1,patch));
        REQUIRE(document.revision()==revision);REQUIRE(document.layer_parameters(1)==snapshot);
    }
    document.set_layer_parameters(1,{});REQUIRE(document.revision()==revision);
    REQUIRE(document.undo());document.set_layer_parameters(2,{{Field::PositionZ,.23}});
    const auto taller=dir.path/("taller."+format);document.save_archive(taller.string());
    auto tall=sla::LayerDocument::load_archive(taller.string());REQUIRE(tall.layer_parameters(2).at(Field::PositionZ)==Catch::Approx(.23));
    REQUIRE(tall.image(2).buf==pixels);
    REQUIRE(document.undo());document.save_archive(restored.string());REQUIRE(read(restored)==read(source));
    if(format=="pwc" && std::stoul(version)<516) {
        // Missing geometry and zero resin estimates remain explicitly unknown
        // during a metadata-only edit; no fabricated material profile is needed.
        auto bytes=read(source);const AnycubicLayerLayout layout(bytes,format);
        for(size_t off:{52u,68u,72u})for(size_t n=0;n<4;++n)bytes[layout.header+off+n]=0;
        const auto unknown_path=dir.path/"unknown.pwc",unknown_saved=dir.path/"unknown-edited.pwc";
        {std::ofstream out(unknown_path.string(),std::ios::binary);out.write(reinterpret_cast<const char *>(bytes.data()),bytes.size());}
        auto unknown=sla::LayerDocument::load_archive(unknown_path.string());
        unknown.set_layer_parameters(2,{{Field::PositionZ,.23}});unknown.save_archive(unknown_saved.string());
        const auto raw=read(unknown_saved);const AnycubicLayerLayout edited(raw,format);
        REQUIRE(edited.number(edited.header+52)==0);REQUIRE(unknown.image(2).buf==pixels);
        REQUIRE(sla::LayerDocument::load_archive(unknown_saved.string()).layer_parameters(2).at(Field::PositionZ)==Catch::Approx(.23));
    }
    const auto single=sla::LayerDocument::from_rasters({png});REQUIRE(single.parameter_capabilities().empty());
    REQUIRE(single.layer_parameters(0).empty());
}

TEST_CASE("Modern CTB parameters retain phase semantics through native save", "[sla_reference][sla_ctb_parameters]")
{
    const std::string format=GENERATE("ctb_v3","ctb_v4","ctb_v5","ctb_gktwo");
    const bool portrait=GENERATE(false,true),seeded=GENERATE(false,true);
    CAPTURE(format,portrait,seeded);
    struct Directory {
        boost::filesystem::path path=boost::filesystem::unique_path("ctb-parameters-%%%%");bool keep=false;
        ~Directory(){if(!keep){boost::system::error_code e;boost::filesystem::remove_all(path,e);}}
    } dir;
    if(const char *root=std::getenv("SLA_CTB_PARAMETERS_FIXTURES")) {dir.path=boost::filesystem::path(root)/(format+(portrait?"-portrait":"-landscape")+(seeded?"-seeded":"-plain"));dir.keep=true;}
    boost::filesystem::create_directories(dir.path);
    DynamicPrintConfig cfg;cfg.apply(SLAFullPrintConfig{});cfg.option("printer_technology")->setInt(ptSLA);
    cfg.set("sla_archive_format",format);cfg.set("display_pixels_x",8);cfg.set("display_pixels_y",4);
    cfg.set("display_width",16.);cfg.set("display_height",8.);
    cfg.option("display_orientation")->setInt(portrait?sladoPortrait:sladoLandscape);
    std::vector<uint8_t> pixels(32,0);pixels[1]=255;pixels[20]=255;
    const auto png=sla::PNGRasterEncoder{}(pixels.data(),portrait?4:8,portrait?8:4,1);
    auto job=SLAPrint::create_raster_job(cfg,sla::LayerDocument::from_rasters({png,png,png}),{.1,.15,.2});
    const std::string suffix=get_archive_entry(format.c_str())->ext;
    const auto source=dir.path/("source."+suffix),saved=dir.path/("parameters."+suffix),restored=dir.path/("restored."+suffix);
    job->export_print(source.string());cfg.save((dir.path/"source.ini").string());
    if(!seeded) {
        std::ifstream in(source.string(),std::ios::binary);
        std::vector<uint8_t> raw{std::istreambuf_iterator<char>(in),{}};in.close();
        const sla::ChituboxLayerLayout layout(raw);
        for(size_t i=0;i<3;++i) {
            const auto at=layout.record(i);const auto data=layout.integer(at+12),length=layout.integer(at+16);
            std::vector<uint8_t> payload(raw.begin()+data,raw.begin()+data+length);
            ctb_encrypted::xor_layer(payload,uint32_t(i),layout.seed);
            std::copy(payload.begin(),payload.end(),raw.begin()+data);
        }
        std::fill(raw.begin()+100,raw.begin()+104,0);
        std::ofstream out(source.string(),std::ios::binary|std::ios::trunc);out.write(reinterpret_cast<const char *>(raw.data()),raw.size());
    }
    auto document=sla::LayerDocument::load_archive(source.string());using F=sla::LayerParameter;
    const bool modern=format!="ctb_v3";
    REQUIRE(document.parameter_capabilities().size()==(modern?14:7));
    const auto before=document.layer_parameters(1);REQUIRE(before.at(F::PositionZ)==Catch::Approx(.15));
    sla::LayerParameters patch{{F::PositionZ,.16},{F::Exposure,7.25},{F::LightOff,1.25},
        {F::LiftDistance,3},{F::LiftSpeed,75},{F::RetractSpeed,150},{F::LightPWM,128}};
    if(modern) for(auto [field,value]:sla::LayerParameters{{F::LiftDistance2,2},{F::LiftSpeed2,180},
        {F::RetractDistance2,1.5},{F::RetractSpeed2,90},{F::WaitBefore,.75},{F::WaitAfter,.5},{F::WaitAfterLift,.25}}) patch[field]=value;
    document.set_layer_parameters(1,patch);REQUIRE(document.has_parameter_edits());
    document.save_archive(saved.string());auto reopened=sla::LayerDocument::load_archive(saved.string());
    verify_project_parameters(document,dir.path);
    for(auto [field,value]:patch) REQUIRE(reopened.layer_parameters(1).at(field)==Catch::Approx(value));
    for(size_t i=0;i<3;++i) REQUIRE(reopened.image(i).buf==document.image(i).buf);
    REQUIRE(reopened.layer_parameters(0)==document.layer_parameters(0));
    REQUIRE(reopened.layer_parameters(2)==document.layer_parameters(2));
    const auto revision=document.revision();const auto snapshot=document.layer_parameters(1);
    for(const auto &invalid:std::vector<sla::LayerParameters>{{{F::LightPWM,256}},{{F::LightPWM,127.5}},
        {{F::LiftSpeed,0}},{{F::RetractSpeed,0}},{{F::PositionZ,.2}},{{F::LiftDistance,-1}}}) {
        REQUIRE_THROWS(document.set_layer_parameters(1,invalid));REQUIRE(document.revision()==revision);
        REQUIRE(document.layer_parameters(1)==snapshot);
    }
    if(modern) REQUIRE_THROWS(document.set_layer_parameters(1,{{F::RetractDistance2,6}}));
    else REQUIRE_THROWS(document.set_layer_parameters(1,{{F::WaitBefore,1}}));
    REQUIRE(document.undo());REQUIRE_FALSE(document.has_parameter_edits());document.save_archive(restored.string());
    auto read=[](const boost::filesystem::path &p){std::ifstream f(p.string(),std::ios::binary);return std::vector<uint8_t>{std::istreambuf_iterator<char>(f),{}};};
    REQUIRE(read(source)==read(restored));REQUIRE(document.redo());
    document.invert(1);document.save_archive((dir.path/("mask-and-parameters."+suffix)).string());
    auto combined=sla::LayerDocument::load_archive((dir.path/("mask-and-parameters."+suffix)).string());
    for(auto [field,value]:patch) REQUIRE(combined.layer_parameters(1).at(field)==Catch::Approx(value));
    REQUIRE(combined.image(1).buf==document.image(1).buf);
}

TEST_CASE("Encrypted CTB parameters retain phase semantics and signed settings", "[sla_reference][sla_ctb_encrypted_parameters]")
{
    const std::string format=GENERATE("ctb_encrypted_v4","ctb_encrypted");
    const bool portrait=GENERATE(false,true),seeded=GENERATE(false,true);
    const bool aes=GENERATE(false,true);
    CAPTURE(format,portrait,seeded,aes);
    struct Directory {
        boost::filesystem::path path=boost::filesystem::unique_path("ctb-parameters-%%%%");bool keep=false;
        ~Directory(){if(!keep){boost::system::error_code e;boost::filesystem::remove_all(path,e);}}
    } dir;
    if(const char *root=std::getenv("SLA_CTB_ENCRYPTED_PARAMETERS_FIXTURES")) {dir.path=boost::filesystem::path(root)/(format+(portrait?"-portrait":"-landscape")+(seeded?"-seeded":"-plain")+(aes?"-aes":"-xor"));dir.keep=true;}
    boost::filesystem::create_directories(dir.path);
    DynamicPrintConfig cfg;cfg.apply(SLAFullPrintConfig{});cfg.option("printer_technology")->setInt(ptSLA);
    cfg.set("sla_archive_format",format);cfg.set("display_pixels_x",32);cfg.set("display_pixels_y",16);
    cfg.set("display_width",16.);cfg.set("display_height",8.);
    cfg.option("display_orientation")->setInt(portrait?sladoPortrait:sladoLandscape);
    std::vector<uint8_t> pixels(512);for(size_t i=0;i<pixels.size();++i)pixels[i]=uint8_t(i*3);
    const auto png=sla::PNGRasterEncoder{}(pixels.data(),portrait?16:32,portrait?32:16,1);
    auto job=SLAPrint::create_raster_job(cfg,sla::LayerDocument::from_rasters({png,png,png}),{.1,.15,.2});
    const std::string suffix=get_archive_entry(format.c_str())->ext;
    const auto source=dir.path/("source."+suffix),saved=dir.path/("parameters."+suffix),restored=dir.path/("restored."+suffix);
    job->export_print(source.string());cfg.save((dir.path/"source.ini").string());
    {
        std::ifstream in(source.string(),std::ios::binary);std::vector<uint8_t> raw{std::istreambuf_iterator<char>(in),{}};in.close();
        const sla::CTBEncryptedLayerLayout layout(raw);auto settings=layout.settings;
        auto put=[](std::vector<uint8_t> &data,size_t at,uint32_t value){for(unsigned n=0;n<4;++n)data.at(at+n)=uint8_t(value>>(8*n));};
        for(size_t i=0;i<3;++i) {
            const auto at=layout.record(i);const auto data=layout.integer(at+16),length=layout.integer(at+24);
            REQUIRE(length>=48);
            if(!seeded) {
                std::vector<uint8_t> payload(raw.begin()+data,raw.begin()+data+length);
                ctb_encrypted::xor_layer(payload,uint32_t(i),layout.seed);std::copy(payload.begin(),payload.end(),raw.begin()+data);
            }
            if(aes) {
                const auto encrypted=ctb_encrypted::aes_cbc({raw.begin()+data+16,raw.begin()+data+48},true);
                std::copy(encrypted.begin(),encrypted.end(),raw.begin()+data+16);put(raw,at+32,16);put(raw,at+36,32);
            }
        }
        if(!seeded)put(settings,128,0);
        const auto encrypted=ctb_encrypted::aes_cbc(settings,true);std::copy(encrypted.begin(),encrypted.end(),raw.begin()+layout.settings_offset);
        std::ofstream out(source.string(),std::ios::binary|std::ios::trunc);out.write(reinterpret_cast<const char *>(raw.data()),raw.size());
    }
    auto document=sla::LayerDocument::load_archive(source.string());using F=sla::LayerParameter;
    const bool modern=true;
    REQUIRE(document.parameter_capabilities().size()==(modern?14:7));
    const auto before=document.layer_parameters(1);REQUIRE(before.at(F::PositionZ)==Catch::Approx(.15));
    sla::LayerParameters patch{{F::PositionZ,.16},{F::Exposure,7.25},{F::LightOff,1.25},
        {F::LiftDistance,3},{F::LiftSpeed,75},{F::RetractSpeed,150},{F::LightPWM,128}};
    if(modern) for(auto [field,value]:sla::LayerParameters{{F::LiftDistance2,2},{F::LiftSpeed2,180},
        {F::RetractDistance2,1.5},{F::RetractSpeed2,90},{F::WaitBefore,.75},{F::WaitAfter,.5},{F::WaitAfterLift,.25}}) patch[field]=value;
    document.set_layer_parameters(1,patch);REQUIRE(document.has_parameter_edits());
    document.save_archive(saved.string());auto reopened=sla::LayerDocument::load_archive(saved.string());
    verify_project_parameters(document,dir.path);
    for(auto [field,value]:patch) REQUIRE(reopened.layer_parameters(1).at(field)==Catch::Approx(value));
    for(size_t i=0;i<3;++i) REQUIRE(reopened.image(i).buf==document.image(i).buf);
    REQUIRE(reopened.layer_parameters(0)==document.layer_parameters(0));
    REQUIRE(reopened.layer_parameters(2)==document.layer_parameters(2));
    const auto revision=document.revision();const auto snapshot=document.layer_parameters(1);
    for(const auto &invalid:std::vector<sla::LayerParameters>{{{F::LightPWM,256}},{{F::LightPWM,127.5}},
        {{F::LiftSpeed,0}},{{F::RetractSpeed,0}},{{F::PositionZ,.2}},{{F::LiftDistance,-1}}}) {
        REQUIRE_THROWS(document.set_layer_parameters(1,invalid));REQUIRE(document.revision()==revision);
        REQUIRE(document.layer_parameters(1)==snapshot);
    }
    if(modern) REQUIRE_THROWS(document.set_layer_parameters(1,{{F::RetractDistance2,6}}));
    else REQUIRE_THROWS(document.set_layer_parameters(1,{{F::WaitBefore,1}}));
    REQUIRE(document.undo());REQUIRE_FALSE(document.has_parameter_edits());document.save_archive(restored.string());
    auto read=[](const boost::filesystem::path &p){std::ifstream f(p.string(),std::ios::binary);return std::vector<uint8_t>{std::istreambuf_iterator<char>(f),{}};};
    REQUIRE(read(source)==read(restored));REQUIRE(document.redo());
    document.invert(1);document.save_archive((dir.path/("mask-and-parameters."+suffix)).string());
    auto combined=sla::LayerDocument::load_archive((dir.path/("mask-and-parameters."+suffix)).string());
    for(auto [field,value]:patch) REQUIRE(combined.layer_parameters(1).at(field)==Catch::Approx(value));
    REQUIRE(combined.image(1).buf==document.image(1).buf);
}

TEST_CASE("Encrypted CTB tilting printers protect motion and retain unknown time", "[sla_reference][sla_ctb_encrypted_parameters]")
{
    const std::string machine=GENERATE("Saturn 4 Ultra","Mars 5 Ultra","numeric");
    const bool unknown_time=GENERATE(false,true);CAPTURE(machine,unknown_time);
    struct Directory {
        boost::filesystem::path path=boost::filesystem::unique_path("ctb-tilting-%%%%");
        ~Directory(){boost::system::error_code e;boost::filesystem::remove_all(path,e);}
    } dir;
    boost::filesystem::create_directories(dir.path);
    DynamicPrintConfig cfg;cfg.apply(SLAFullPrintConfig{});cfg.option("printer_technology")->setInt(ptSLA);
    cfg.set("sla_archive_format",std::string("ctb_encrypted"));cfg.set("printer_model",machine);
    cfg.set("display_pixels_x",8);cfg.set("display_pixels_y",4);cfg.option("display_orientation")->setInt(sladoLandscape);
    const std::vector<uint8_t> pixels(32,255);const auto png=sla::PNGRasterEncoder{}(pixels.data(),8,4,1);
    auto job=SLAPrint::create_raster_job(cfg,sla::LayerDocument::from_rasters({png,png,png}),{.1,.15,.2});
    const auto source=dir.path/"source.encrypted.ctb",saved=dir.path/"saved.encrypted.ctb";job->export_print(source.string());
    auto read=[](const boost::filesystem::path &p){std::ifstream f(p.string(),std::ios::binary);return std::vector<uint8_t>{std::istreambuf_iterator<char>(f),{}};};
    auto bytes=read(source);const sla::CTBEncryptedLayerLayout layout(bytes);auto settings=layout.settings;
    auto put=[](std::vector<uint8_t> &data,size_t at,uint32_t value){for(unsigned n=0;n<4;++n)data.at(at+n)=uint8_t(value>>(8*n));};
    if(machine=="numeric") {
        put(settings,92,layout.setting(36)); put(settings,140,0);
        const float speed=.05f;uint32_t bits;std::memcpy(&bits,&speed,4);put(settings,96,bits);
    }
    if(unknown_time)put(settings,76,0);
    const auto encrypted=ctb_encrypted::aes_cbc(settings,true);std::copy(encrypted.begin(),encrypted.end(),bytes.begin()+layout.settings_offset);
    {std::ofstream out(source.string(),std::ios::binary|std::ios::trunc);out.write(reinterpret_cast<const char *>(bytes.data()),bytes.size());}
    auto document=sla::LayerDocument::load_archive(source.string());const auto capabilities=document.parameter_capabilities();
    REQUIRE(std::count_if(capabilities.begin(),capabilities.end(),[](auto c){return c.editable;})==7);
    using F=sla::LayerParameter;const auto before=document.layer_parameters(1);const auto revision=document.revision();
    REQUIRE_THROWS(document.set_layer_parameters(1,{{F::LiftDistance,3}}));REQUIRE(document.revision()==revision);
    document.set_layer_parameters(1,{{F::Exposure,7.25},{F::WaitBefore,.75},{F::LightOff,10}});document.save_archive(saved.string());
    const auto written=read(saved);const sla::CTBEncryptedLayerLayout updated(written);
    if(unknown_time)REQUIRE(updated.setting(76)==0);
    else REQUIRE(updated.setting(76)==uint32_t(std::ceil(layout.setting(76)+7.25-before.at(F::Exposure)+.75-before.at(F::WaitBefore)+10-std::max(4.,before.at(F::LightOff)))));
    REQUIRE(written.size()==bytes.size());REQUIRE(updated.number(updated.record(1)+40)==layout.number(layout.record(1)+40));
    REQUIRE(document.undo());document.save_archive(saved.string());REQUIRE(read(saved)==bytes);
}

TEST_CASE("Anycubic layer motion edits preserve global phase settings", "[sla_reference][sla_anycubic_motion_parameters]")
{
    const std::string variant=GENERATE("pwc-1","pwc-515","pwc-516","pwc-517","pwc-518","pws-1","pm5s-518");
    const bool portrait=GENERATE(false,true);const bool two_stage=GENERATE(false,true);
    const auto dash=variant.find('-');const auto format=variant.substr(0,dash),version=variant.substr(dash+1);
    const bool modern=std::stoul(version)>=516;
    if(two_stage && !modern)return;
    CAPTURE(variant,portrait,two_stage);
    struct Directory {
        boost::filesystem::path path=boost::filesystem::unique_path("anycubic-motion-%%%%");bool keep=false;
        ~Directory(){if(!keep){boost::system::error_code e;boost::filesystem::remove_all(path,e);}}
    } dir;
    if(const char *root=std::getenv("SLA_ANYCUBIC_MOTION_FIXTURES")) {
        dir.path=boost::filesystem::path(root)/(variant+(portrait?"-portrait":"-landscape")+(two_stage?"-two":"-single"));dir.keep=true;
    }
    boost::filesystem::create_directories(dir.path);
    DynamicPrintConfig cfg;cfg.apply(SLAFullPrintConfig{});cfg.option("printer_technology")->setInt(ptSLA);
    cfg.set("sla_archive_format",format);cfg.set("sla_anycubic_version",version);
    cfg.set("display_pixels_x",8);cfg.set("display_pixels_y",4);cfg.set("display_width",16.);cfg.set("display_height",4.);
    cfg.option("display_orientation")->setInt(portrait?sladoPortrait:sladoLandscape);
    if(const auto *geometry=anycubic_legacy_geometry(format);!modern && geometry) {
        cfg.set("display_width",portrait?geometry->height:geometry->width);cfg.set("display_height",portrait?geometry->width:geometry->height);cfg.set("max_print_height",geometry->machine_z);
    }
    cfg.set("sla_bottom_layers",1);cfg.set("sla_transition_layers",0);
    for(const bool bottom:{false,true}) {
        const std::string prefix=bottom?"sla_bottom_":"sla_";
        cfg.set(prefix+"lift_height",bottom?5.:3.);cfg.set(prefix+"lift_speed",bottom?60.:90.);
        cfg.set(prefix+"lift_height2",two_stage?2.:0.);cfg.set(prefix+"lift_speed2",180.);
        cfg.set(prefix+"retract_speed",modern?(bottom?210.:150.):150.);cfg.set(prefix+"retract_speed2",240.);
        cfg.set(prefix+"wait_before_cure",1.25);
    }
    std::vector<uint8_t> pixels(32,0);pixels[1]=255;pixels[20]=255;
    const auto png=sla::PNGRasterEncoder{}(pixels.data(),portrait?4:8,portrait?8:4,1);
    auto job=SLAPrint::create_raster_job(cfg,sla::LayerDocument::from_rasters({png,png,png}),{.1,.15,.2});
    const auto source=dir.path/("source."+format),saved=dir.path/("motion."+format),restored=dir.path/("restored."+format);
    job->export_print(source.string());cfg.save((dir.path/"source.ini").string());
    auto document=sla::LayerDocument::load_archive(source.string());using F=sla::LayerParameter;
    const auto capabilities=document.parameter_capabilities();REQUIRE(std::count_if(capabilities.begin(),capabilities.end(),[](auto c){return c.editable;})==4);
    const auto before=document.layer_parameters(1);
    REQUIRE(before.at(F::LiftDistance)==Catch::Approx(two_stage?5.:3.));
    REQUIRE(before.at(F::LiftSpeed)==Catch::Approx(90));
    document.set_layer_parameters(1,{{F::LiftDistance,6.},{F::LiftSpeed,80.3}});
    REQUIRE(document.has_parameter_edits());document.save_archive(saved.string());
    auto reopened=sla::LayerDocument::load_archive(saved.string());
    verify_project_parameters(document,dir.path);
    REQUIRE(reopened.layer_parameters(1).at(F::LiftDistance)==Catch::Approx(6));
    REQUIRE(reopened.layer_parameters(1).at(F::LiftSpeed)==Catch::Approx(80.3));
    REQUIRE(reopened.layer_parameters(0)==document.layer_parameters(0));
    REQUIRE(reopened.layer_parameters(2)==document.layer_parameters(2));
    for(size_t i=0;i<3;++i)REQUIRE(reopened.image(i).buf==document.image(i).buf);
    const auto revision=document.revision();const auto snapshot=document.layer_parameters(1);
    for(const auto &invalid:std::vector<sla::LayerParameters>{{{F::LiftSpeed,0}},{{F::LiftSpeed,-1}},{{F::LiftDistance,-1}},{{F::RetractSpeed,180}}}) {
        REQUIRE_THROWS(document.set_layer_parameters(1,invalid));REQUIRE(document.revision()==revision);REQUIRE(document.layer_parameters(1)==snapshot);
    }
    if(two_stage)REQUIRE_THROWS(document.set_layer_parameters(1,{{F::LiftDistance,1}}));
    REQUIRE(document.undo());REQUIRE_FALSE(document.has_parameter_edits());document.save_archive(restored.string());
    auto read=[](const boost::filesystem::path &p){std::ifstream f(p.string(),std::ios::binary);return std::vector<uint8_t>{std::istreambuf_iterator<char>(f),{}};};
    REQUIRE(read(source)==read(restored));REQUIRE(document.redo());
    document.invert(1);document.save_archive((dir.path/("mask-and-motion."+format)).string());
    REQUIRE(sla::LayerDocument::load_archive((dir.path/("mask-and-motion."+format)).string()).layer_parameters(1)==snapshot);
    if(two_stage && variant=="pwc-516" && !portrait) {
        auto bytes=read(source);const AnycubicLayerLayout raw(bytes,format);
        for(size_t n=0;n<4;++n)bytes[raw.header+96+n]=0; // Basic ignores the stored EXTRA phases.
        const auto basic_path=dir.path/"basic.pwc";
        {std::ofstream out(basic_path.string(),std::ios::binary);out.write(reinterpret_cast<const char *>(bytes.data()),bytes.size());}
        auto basic=sla::LayerDocument::load_archive(basic_path.string());
        REQUIRE(basic.layer_parameters(1).at(F::LiftDistance2)==0);
        REQUIRE(basic.layer_parameters(0).at(F::RetractSpeed)==Catch::Approx(raw.number(raw.header+48)*60.));
        basic.set_layer_parameters(1,{{F::LiftDistance,1.},{F::LiftSpeed,120.}});
        basic.save_archive((dir.path/"basic-edited.pwc").string());
        // A finite mm/s field can overflow the reference's canonical mm/min
        // float. Reject that edit before mutating the document/history.
        const auto basic_revision=basic.revision();
        REQUIRE_THROWS(basic.set_layer_parameters(1,{{F::LiftSpeed,double(std::numeric_limits<float>::max())*30.}}));
        REQUIRE(basic.revision()==basic_revision);
    }
}

TEST_CASE("Anycubic conversion preserves global normal and bottom settings independently of target preset", "[sla_reference][sla_layer_conversion][sla_global_conversion]")
{
    const std::string version=GENERATE("516","517");
    const std::string target=GENERATE("ctb_v4","ctb_v5","ctb_encrypted_v4","ctb_encrypted");
    CAPTURE(version,target);
    struct Directory {boost::filesystem::path path=boost::filesystem::unique_path("global-conversion-%%%%");
        bool keep=false;
        ~Directory(){if(!keep){boost::system::error_code error;boost::filesystem::remove_all(path,error);}}} directory;
    if(const char *root=std::getenv("SLA_GLOBAL_CONVERSION_FIXTURES")) {
        directory.path=boost::filesystem::path(root)/(version+"-to-"+target);directory.keep=true;
    }
    boost::filesystem::create_directories(directory.path);
    DynamicPrintConfig source;source.apply(SLAFullPrintConfig{});source.option("printer_technology")->setInt(ptSLA);
    source.set("sla_archive_format",std::string("pwc"));source.set("sla_anycubic_version",version);
    source.set("display_pixels_x",8);source.set("display_pixels_y",4);source.set("display_width",16.);source.set("display_height",4.);
    source.option("display_orientation")->setInt(sladoLandscape);
    source.set("layer_height",.05);source.set("initial_layer_height",.1);
    source.set("exposure_time",7.);source.set("initial_exposure_time",23.);
    source.set("sla_bottom_layers",1);source.set("sla_transition_layers",1);
    for(bool bottom:{false,true}) {
        const std::string prefix=bottom?"sla_bottom_":"sla_";
        source.set(prefix+"lift_height",bottom?4.:3.);
        source.set(prefix+"lift_height2",bottom?1.:2.);
        source.set(prefix+"lift_speed",bottom?180.:200.);
        source.set(prefix+"lift_speed2",bottom?90.:120.);
        source.set(prefix+"retract_speed",bottom?240.:300.);
        source.set(prefix+"retract_speed2",bottom?60.:150.);
        source.set(prefix+"wait_before_cure",.75);
    }
    const std::vector<uint8_t> pixels(32,255);
    const auto mask=sla::PNGRasterEncoder{}(pixels.data(),8,4,1);
    auto job=SLAPrint::create_raster_job(source,sla::LayerDocument::from_rasters({mask,mask,mask}),{.1,.15,.2});
    const auto path=directory.path/"source.pwc";job->export_print(path.string());
    const auto document=sla::LayerDocument::load_archive(path.string());
    auto destination=source;
    destination.set("exposure_time",2.7);destination.set("initial_exposure_time",30.);
    destination.set("layer_height",.025);destination.set("sla_bottom_layers",2);destination.set("sla_transition_layers",0);
    for(const char *prefix:{"sla_","sla_bottom_"}) {
        destination.set(std::string(prefix)+"lift_height",6.);destination.set(std::string(prefix)+"lift_height2",0.);
        destination.set(std::string(prefix)+"wait_before_cure",0.);
    }
    const auto output=directory.path/(std::string("converted.")+get_default_extension(target.c_str()));
    SLAPrint::convert_layer_archive(destination,document,target,output.string());
    std::ifstream stream(output.string(),std::ios::binary);
    const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(stream),std::istreambuf_iterator<char>()};
    if(target.find("encrypted")!=std::string::npos) {
        const sla::CTBEncryptedLayerLayout layout(bytes);
        CHECK(layout.parameter(36)==float(.05));CHECK(layout.parameter(40)==7.);CHECK(layout.parameter(44)==23.);
        CHECK(layout.setting(52)==1);CHECK(layout.setting(188)==1);
        CHECK(layout.parameter(84)-layout.parameter(132)==4.);CHECK(layout.parameter(132)==1.);
        CHECK(layout.parameter(92)-layout.parameter(140)==3.);CHECK(layout.parameter(140)==2.);
        CHECK(layout.parameter(88)==180.);CHECK(layout.parameter(96)==200.);
        CHECK(layout.parameter(136)==90.);CHECK(layout.parameter(144)==120.);
        CHECK(layout.parameter(100)==300.);CHECK(layout.parameter(152)==150.);
        CHECK(layout.parameter(192)==240.);CHECK(layout.parameter(196)==60.);
        CHECK(layout.parameter(216)==.75);
    } else {
        const sla::ChituboxLayerLayout layout(bytes);
        const auto slicer=layout.integer(104),v4=layout.integer(slicer+64),params=layout.parameters;
        CHECK(layout.number(32)==float(.05));CHECK(layout.number(36)==7.);CHECK(layout.number(40)==23.);
        CHECK(layout.integer(48)==1);CHECK(layout.integer(slicer+60)==1);
        CHECK(layout.number(params)-layout.number(slicer)==4.);CHECK(layout.number(slicer)==1.);
        CHECK(layout.number(params+8)-layout.number(slicer+8)==3.);CHECK(layout.number(slicer+8)==2.);
        CHECK(layout.number(params+4)==180.);CHECK(layout.number(params+12)==200.);
        CHECK(layout.number(slicer+4)==90.);CHECK(layout.number(slicer+12)==120.);
        CHECK(layout.number(params+16)==300.);CHECK(layout.number(slicer+20)==150.);
        CHECK(layout.number(v4)==240.);CHECK(layout.number(v4+4)==60.);
        CHECK(layout.number(v4+24)==.75);
    }
    // Invalid counts and nominal heights rejected by UVtools must fail
    // atomically; valid native exposure precision must be preserved below.
    std::ifstream original_stream(path.string(),std::ios::binary);
    const std::vector<uint8_t> original_bytes{std::istreambuf_iterator<char>(original_stream),std::istreambuf_iterator<char>()};
    const AnycubicLayerLayout source_layout(original_bytes,"pwc");
    for(const auto &[offset,value] : std::map<size_t,float>{{36,1.5f},{20,.050005f}}) {
        auto invalid=original_bytes;uint32_t bits;std::memcpy(&bits,&value,4);
        for(size_t n=0;n<4;++n)invalid[source_layout.header+offset+n]=uint8_t(bits>>(8*n));
        const auto invalid_path=directory.path/"invalid.pwc";
        {std::ofstream file(invalid_path.string(),std::ios::binary);file.write(reinterpret_cast<const char *>(invalid.data()),invalid.size());}
        const auto invalid_document=sla::LayerDocument::load_archive(invalid_path.string());
        REQUIRE_THROWS(SLAPrint::convert_layer_archive(destination,invalid_document,target,output.string()));
        std::ifstream preserved(output.string(),std::ios::binary);
        const std::vector<uint8_t> preserved_bytes{std::istreambuf_iterator<char>(preserved),std::istreambuf_iterator<char>()};
        REQUIRE(preserved_bytes==bytes);REQUIRE_FALSE(invalid_document.dirty());REQUIRE_FALSE(invalid_document.can_undo());
        boost::filesystem::remove(invalid_path);
    }
    auto precise_bytes=original_bytes;const float precise_exposure=1.234f;uint32_t precise_bits;
    std::memcpy(&precise_bits,&precise_exposure,4);
    for(size_t n=0;n<4;++n)precise_bytes[source_layout.header+24+n]=uint8_t(precise_bits>>(8*n));
    const auto precise_path=directory.path/"precise.pwc";
    {std::ofstream file(precise_path.string(),std::ios::binary);file.write(reinterpret_cast<const char *>(precise_bytes.data()),precise_bytes.size());}
    const auto precise_document=sla::LayerDocument::load_archive(precise_path.string());
    const auto precise_output=directory.path/(std::string("precise.")+get_default_extension(target.c_str()));
    REQUIRE_NOTHROW(SLAPrint::convert_layer_archive(destination,precise_document,target,precise_output.string()));
    const auto precise_converted=sla::LayerDocument::load_archive(precise_output.string());
    REQUIRE_NOTHROW(precise_converted.require_job_parameters(*precise_document.job_parameters()));
    auto basic_bytes=original_bytes;
    for(size_t n=0;n<4;++n)basic_bytes[source_layout.header+96+n]=0;
    const auto basic_path=directory.path/"basic.pwc";
    {std::ofstream file(basic_path.string(),std::ios::binary);file.write(reinterpret_cast<const char *>(basic_bytes.data()),basic_bytes.size());}
    const auto basic_document=sla::LayerDocument::load_archive(basic_path.string());
    const auto basic_globals=basic_document.job_parameters();REQUIRE(basic_globals.has_value());
    for(const auto *row:{&basic_globals->normal,&basic_globals->bottom}) {
        REQUIRE(row->at(sla::LayerParameter::LiftDistance)==5.);
        REQUIRE(row->at(sla::LayerParameter::LiftDistance2)==0.);
        REQUIRE(float(row->at(sla::LayerParameter::LiftSpeed))==200.);
        REQUIRE(float(row->at(sla::LayerParameter::RetractSpeed))==300.);
    }
    const auto basic_output=directory.path/(std::string("basic.")+get_default_extension(target.c_str()));
    REQUIRE_NOTHROW(SLAPrint::convert_layer_archive(destination,basic_document,target,basic_output.string()));
    const auto basic_converted=sla::LayerDocument::load_archive(basic_output.string());
    REQUIRE_NOTHROW(basic_converted.require_job_parameters(*basic_globals));
    for(size_t i=0;i<basic_document.layer_count();++i)REQUIRE(basic_converted.image(i).buf==basic_document.image(i).buf);
    REQUIRE_FALSE(document.dirty());REQUIRE_FALSE(document.can_undo());
}

TEST_CASE("CTB conversion preserves global native floats independently of the destination preset", "[sla_reference][sla_layer_conversion][sla_ctb_global_conversion]")
{
    const std::string source_format=GENERATE("ctb_v4","ctb_v5","ctb_encrypted_v4","ctb_encrypted");
    const std::string target=GENERATE("ctb_v4","ctb_v5","ctb_encrypted_v4","ctb_encrypted");
    const bool native_fractional=GENERATE(false,true);
    CAPTURE(source_format,target,native_fractional);
    struct Directory {boost::filesystem::path path=boost::filesystem::unique_path("ctb-global-%%%%");bool keep=false;
        ~Directory(){if(!keep){boost::system::error_code error;boost::filesystem::remove_all(path,error);}}} directory;
    if(const char *root=std::getenv("SLA_CTB_GLOBAL_FIXTURES")) {
        directory.path=boost::filesystem::path(root)/(source_format+"-to-"+target+(native_fractional?"-native":"-standard"));directory.keep=true;
    }
    boost::filesystem::create_directories(directory.path);
    DynamicPrintConfig config;config.apply(SLAFullPrintConfig{});config.option("printer_technology")->setInt(ptSLA);
    config.set("sla_archive_format",source_format);config.set("display_pixels_x",8);config.set("display_pixels_y",4);
    config.set("display_width",16.);config.set("display_height",4.);config.option("display_orientation")->setInt(sladoLandscape);
    config.set("exposure_time",7.);config.set("initial_exposure_time",23.);config.set("layer_height",.05);
    config.set("sla_bottom_layers",1);config.set("sla_transition_layers",1);
    for(const char *prefix:{"sla_","sla_bottom_"}) {
        config.set(std::string(prefix)+"lift_height",3.);config.set(std::string(prefix)+"lift_height2",2.);
        config.set(std::string(prefix)+"lift_speed",200.);config.set(std::string(prefix)+"lift_speed2",120.);
        config.set(std::string(prefix)+"wait_before_cure",.75);config.set(std::string(prefix)+"wait_after_cure",.5);
    }
    std::vector<uint8_t> pixels(32,0);pixels[1]=255;pixels[20]=255;
    const auto png=sla::PNGRasterEncoder{}(pixels.data(),8,4,1);
    auto job=SLAPrint::create_raster_job(config,sla::LayerDocument::from_rasters({png,png,png}),{.1,.15,.2});
    const auto source=directory.path/(std::string("source.")+get_default_extension(source_format.c_str()));job->export_print(source.string());
    auto read=[](const boost::filesystem::path &path) {std::ifstream stream(path.string(),std::ios::binary);
        return std::vector<uint8_t>{std::istreambuf_iterator<char>(stream),std::istreambuf_iterator<char>()};};
    if(native_fractional) {
        auto bytes=read(source);
        auto number=[](std::vector<uint8_t> &data,size_t at,float value) {uint32_t bits;std::memcpy(&bits,&value,4);
            for(size_t n=0;n<4;++n)data.at(at+n)=uint8_t(bits>>(8*n));};
        if(source_format.find("encrypted")!=std::string::npos) {
            const sla::CTBEncryptedLayerLayout layout(bytes);auto settings=layout.settings;
            number(settings,40,1.234f);number(settings,92,5.123f);number(settings,48,1.25f);
            const auto encrypted=ctb_encrypted::aes_cbc(settings,true);
            std::copy(encrypted.begin(),encrypted.end(),bytes.begin()+layout.settings_offset);
        } else {
            const sla::ChituboxLayerLayout layout(bytes);
            number(bytes,36,1.234f);number(bytes,layout.parameters+8,5.123f);
            number(bytes,44,1.25f);number(bytes,layout.parameters+36,1.25f);
        }
        std::ofstream file(source.string(),std::ios::binary|std::ios::trunc);file.write(reinterpret_cast<const char *>(bytes.data()),bytes.size());
    }
    auto document=sla::LayerDocument::load_archive(source.string());const auto source_bytes=read(source);
    document.set_layer_parameters(1,{{sla::LayerParameter::Exposure,8.5}});
    const auto revision=document.revision();
    if(directory.keep) {auto expected=document;expected.save_archive((directory.path/(std::string("expected.")+get_default_extension(source_format.c_str()))).string());}
    config.set("exposure_time",2.7);config.set("initial_exposure_time",30.);config.set("layer_height",.025);
    config.set("sla_bottom_layers",2);config.set("sla_transition_layers",0);
    for(const char *prefix:{"sla_","sla_bottom_"}) {
        config.set(std::string(prefix)+"lift_height",6.);config.set(std::string(prefix)+"lift_height2",0.);
        config.set(std::string(prefix)+"wait_before_cure",0.);config.set(std::string(prefix)+"wait_after_cure",0.);
    }
    const auto output=directory.path/(std::string("converted.")+get_default_extension(target.c_str()));
    REQUIRE_NOTHROW(SLAPrint::convert_layer_archive(config,document,target,output.string()));
    const auto bytes=read(output);
    auto check=[&](auto number,auto integer,size_t height,size_t exposure,size_t bottom_exposure,size_t count,
                   size_t transition,size_t total,size_t second,size_t wait,size_t after,size_t off) {
        CHECK(number(height)==float(.05));CHECK(number(exposure)==float(native_fractional?1.234:7.));
        CHECK(number(bottom_exposure)==23.);CHECK(integer(count)==1);CHECK(integer(transition)==1);
        CHECK(number(total)==float(native_fractional?5.123:5.));CHECK(number(second)==2.);
        CHECK(number(wait)==.75);CHECK(number(after)==.5);CHECK(number(off)==(native_fractional?1.25:0.));
    };
    if(target.find("encrypted")!=std::string::npos) {
        const sla::CTBEncryptedLayerLayout layout(bytes);
        check([&](size_t at){return layout.parameter(at);},[&](size_t at){return layout.setting(at);},36,40,44,52,188,92,140,216,224,48);
    } else {
        const sla::ChituboxLayerLayout layout(bytes);const auto slicer=layout.integer(104),v4=layout.integer(slicer+64);
        check([&](size_t at){return layout.number(at);},[&](size_t at){return layout.integer(at);},32,36,40,48,slicer+60,layout.parameters+8,slicer+8,v4+24,v4+32,44);
    }
    const auto converted=sla::LayerDocument::load_archive(output.string());
    for(size_t i=0;i<3;++i) {
        REQUIRE(converted.layer_parameters(i)==document.layer_parameters(i));REQUIRE(converted.image(i).buf==document.image(i).buf);
    }
    REQUIRE(document.revision()==revision);REQUIRE(document.dirty());REQUIRE(document.can_undo());REQUIRE(read(source)==source_bytes);
    // Exercise the globals-only save path: AES must reencrypt settings even
    // when masks and every native layer row remain unchanged.
    auto pristine=sla::LayerDocument::load_archive(source.string());auto globals=*pristine.job_parameters();
    globals.normal[sla::LayerParameter::Exposure]=double(float(1.111));
    const auto overlay_path=directory.path/(std::string("globals-only.")+get_default_extension(source_format.c_str()));
    REQUIRE_NOTHROW(pristine.save_archive(overlay_path.string(),{},nullptr,&globals));
    const auto overlaid=sla::LayerDocument::load_archive(overlay_path.string());
    REQUIRE_NOTHROW(overlaid.require_job_parameters(globals));
    for(size_t i=0;i<3;++i) {
        REQUIRE(overlaid.layer_parameters(i)==pristine.layer_parameters(i));REQUIRE(overlaid.image(i).buf==pristine.image(i).buf);
    }
    REQUIRE_FALSE(pristine.dirty());REQUIRE_FALSE(pristine.can_undo());REQUIRE(read(source)==source_bytes);
    const auto overlay_bytes=read(overlay_path);
    globals.bottom[sla::LayerParameter::WaitBefore]=9.;
    REQUIRE_THROWS(pristine.save_archive(overlay_path.string(),{},nullptr,&globals));
    REQUIRE(read(overlay_path)==overlay_bytes);REQUIRE_FALSE(pristine.dirty());
    // A decoder accepting raw records is insufficient evidence that firmware
    // uses those records. Reject ambiguous/unknown serialized flag states and
    // nominal heights rejected by the independent UVtools reader.
    for(const std::string defect:{"unknown_flag","disabled_conflicting_rows","invalid_height","conflicting_waits"}) {
        CAPTURE(defect);auto invalid=source_bytes;
        auto integer=[](std::vector<uint8_t> &data,size_t at,uint32_t value) {
            for(size_t n=0;n<4;++n)data.at(at+n)=uint8_t(value>>(8*n));};
        auto scalar=[&](std::vector<uint8_t> &data,size_t at,float value) {
            uint32_t bits;std::memcpy(&bits,&value,4);integer(data,at,bits);};
        if(source_format.find("encrypted")!=std::string::npos) {
            const sla::CTBEncryptedLayerLayout layout(invalid);auto settings=layout.settings;
            if(defect=="invalid_height")scalar(settings,36,.050005f);
            else if(defect=="conflicting_waits")scalar(settings,180,9.5f);
            else {settings.at(171)=defect=="unknown_flag"?0x71:0;scalar(invalid,layout.record(1)+8,9.5f);}
            const auto encrypted=ctb_encrypted::aes_cbc(settings,true);
            std::copy(encrypted.begin(),encrypted.end(),invalid.begin()+layout.settings_offset);
        } else {
            const sla::ChituboxLayerLayout layout(invalid);
            if(defect=="invalid_height")scalar(invalid,32,.050005f);
            else if(defect=="conflicting_waits")scalar(invalid,layout.integer(104)+52,9.5f);
            else {invalid.at(layout.integer(104)+39)=defect=="unknown_flag"?0x71:0;
                scalar(invalid,layout.record(1)+4,9.5f);scalar(invalid,layout.extended_record(1)+4,9.5f);}
        }
        const auto invalid_path=directory.path/(defect+"."+get_default_extension(source_format.c_str()));
        {std::ofstream file(invalid_path.string(),std::ios::binary);file.write(reinterpret_cast<const char *>(invalid.data()),invalid.size());}
        const auto invalid_document=sla::LayerDocument::load_archive(invalid_path.string());
        REQUIRE_THROWS(SLAPrint::convert_layer_archive(config,invalid_document,target,output.string()));
        REQUIRE(read(output)==bytes);REQUIRE(read(invalid_path)==invalid);
        REQUIRE_FALSE(invalid_document.dirty());REQUIRE_FALSE(invalid_document.can_undo());
    }
}

TEST_CASE("Nominal SLA defaults retain independent waits and intended precision", "[sla_layer_parameters][sla_neutral_job]")
{
    using F=sla::LayerParameter;
    sla::LayerJobParameters job;
    job.layer_height=.05;job.bottom_layers=1;job.transition_layers=1;
    job.normal=job.bottom={{F::Exposure,7},{F::LiftDistance,3},{F::LiftDistance2,2},
        {F::LiftSpeed,200},{F::LiftSpeed2,120},{F::RetractSpeed,300},{F::RetractSpeed2,180},
        {F::RetractDistance2,1},{F::WaitBefore,.75},{F::WaitAfter,.5},{F::WaitAfterLift,.25},
        {F::LightOff,0},{F::LightPWM,255}};
    SECTION("bottom waits are independent printable intentions") {
        job.bottom[F::WaitBefore]=2;job.bottom[F::WaitAfter]=1;job.bottom[F::WaitAfterLift]=.5;
    }
    SECTION("nominal height precision is a destination decision") {job.layer_height=.050005;}
    SECTION("layer counts are not limited by one native header") {job.bottom_layers=70000;}
    SECTION("finite double motion is not limited by native float storage") {
        job.normal[F::LiftSpeed]=double(std::numeric_limits<float>::max())*2;
    }
    const auto normal=job.normal,bottom=job.bottom;const auto height=job.layer_height;
    REQUIRE_NOTHROW(job.validate(70002));
    REQUIRE(job.normal==normal);REQUIRE(job.bottom==bottom);REQUIRE(job.layer_height==height);
    REQUIRE_THROWS(sla::validate_chitubox_job(job,70002));
    DynamicPrintConfig destination;destination.apply(SLAFullPrintConfig{});const auto before=destination;
    REQUIRE_THROWS(sla::apply_chitubox_container_baseline(job,70002,destination));
    REQUIRE(destination.diff(before).empty());
}

TEST_CASE("Neutral SLA defaults reject invalid intentions without changing source values", "[sla_layer_parameters][sla_neutral_job]")
{
    using F=sla::LayerParameter;
    sla::LayerJobParameters job;
    job.layer_height=.05;job.bottom_layers=1;job.transition_layers=1;
    job.normal=job.bottom={{F::Exposure,7},{F::LiftDistance,3},{F::LiftDistance2,2},
        {F::LiftSpeed,200},{F::LiftSpeed2,120},{F::RetractSpeed,300},{F::RetractSpeed2,180},
        {F::RetractDistance2,1},{F::WaitBefore,.75},{F::WaitAfter,.5},{F::WaitAfterLift,.25},
        {F::LightOff,0},{F::LightPWM,255}};
    REQUIRE_NOTHROW(job.validate(3));REQUIRE_NOTHROW(sla::validate_chitubox_job(job,3));
    for(const auto &[field,value]:job.normal) {
        CAPTURE(field);auto invalid=job;
        invalid.bottom[field]=std::numeric_limits<double>::quiet_NaN();REQUIRE_THROWS(invalid.validate(3));
        invalid.bottom[field]=-1;REQUIRE_THROWS(invalid.validate(3));
    }
    for(const std::string defect:{"missing","extra_z","zero_speed","retract","pwm_fraction","pwm_range","height","bottom_count","transition_count","total_overflow"}) {
        CAPTURE(defect);auto invalid=job;
        if(defect=="missing")invalid.normal.erase(F::LightPWM);
        if(defect=="extra_z")invalid.normal[F::PositionZ]=.1;
        if(defect=="zero_speed")invalid.normal[F::RetractSpeed]=0;
        if(defect=="retract")invalid.normal[F::RetractDistance2]=6;
        if(defect=="pwm_fraction")invalid.normal[F::LightPWM]=.5;
        if(defect=="pwm_range")invalid.normal[F::LightPWM]=256;
        if(defect=="height")invalid.layer_height=0;
        if(defect=="bottom_count")invalid.bottom_layers=4;
        if(defect=="transition_count")invalid.transition_layers=3;
        if(defect=="total_overflow")invalid.normal[F::LiftDistance]=invalid.normal[F::LiftDistance2]=std::numeric_limits<double>::max();
        const auto normal=invalid.normal,bottom=invalid.bottom;
        REQUIRE_THROWS(invalid.validate(3));REQUIRE(invalid.normal==normal);REQUIRE(invalid.bottom==bottom);
        if(defect!="bottom_count" && defect!="transition_count") {
            DynamicPrintConfig destination;destination.apply(SLAFullPrintConfig{});const auto before=destination;
            REQUIRE_THROWS(invalid.apply_to(destination));REQUIRE(destination.diff(before).empty());
        }
    }
    REQUIRE_THROWS(job.validate(0));
    {auto invalid=job;invalid.normal[F::LightPWM]=std::numeric_limits<double>::quiet_NaN();
        DynamicPrintConfig destination;destination.apply(SLAFullPrintConfig{});const auto before=destination;
        REQUIRE_THROWS(invalid.apply_to(destination));REQUIRE(destination.diff(before).empty());}
    // Stationary PCB recipes legitimately have zero distances and speeds.
    for(auto *row:{&job.normal,&job.bottom})for(const auto field:{F::LiftDistance,F::LiftDistance2,F::RetractDistance2,
        F::LiftSpeed,F::LiftSpeed2,F::RetractSpeed,F::RetractSpeed2}) (*row)[field]=0;
    REQUIRE_NOTHROW(job.validate(3));
}

TEST_CASE("Image-only documents do not acquire invented conversion parameters", "[sla_conversion_preflight]")
{
    const uint8_t pixel = 255;
    const auto mask = sla::PNGRasterEncoder{}(&pixel, 1, 1, 1);
    const auto document = sla::LayerDocument::from_rasters({mask});
    REQUIRE_THROWS_WITH(sla::plan_layer_conversion(document, "ctb_v4"),
        "This imported format does not expose a complete parameter table for conversion yet");
    REQUIRE(document.parameter_capabilities().empty());
    REQUIRE_FALSE(document.job_parameters().has_value());
    REQUIRE_FALSE(document.raster_geometry().has_value());
    REQUIRE_FALSE(document.dirty());
    REQUIRE_FALSE(document.can_undo());
}

TEST_CASE("Imported conversion preserves physical scale and actual printer Z limit", "[sla_layer_conversion][sla_conversion_geometry]")
{
    const std::string format = GENERATE("ctb_v4", "ctb_v5", "ctb_encrypted_v4", "ctb_encrypted", "pwc");
    const bool portrait = GENERATE(false, true);
    const std::string defect = GENERATE("width", "height", "resolution", "height_limit");
    CAPTURE(format, portrait, defect);
    struct Directory {
        boost::filesystem::path path = boost::filesystem::unique_path("conversion-geometry-%%%%");
        bool keep = false;
        ~Directory() { if (!keep) { boost::system::error_code error; boost::filesystem::remove_all(path, error); } }
    } directory;
    if (const char *root = std::getenv("SLA_GEOMETRY_FIXTURES")) {
        directory.path = boost::filesystem::path(root) /
            (format + (portrait ? "-portrait-" : "-landscape-") + defect);
        directory.keep = true;
    }
    boost::filesystem::create_directories(directory.path);
    DynamicPrintConfig config; config.apply(SLAFullPrintConfig{});
    config.option("printer_technology")->setInt(ptSLA);
    config.set("sla_archive_format", format); config.set("sla_anycubic_version", std::string("516"));
    config.set("display_pixels_x", 8); config.set("display_pixels_y", 4);
    config.set("display_width", 16.1234567); config.set("display_height", 4.7654321);
    config.option("display_orientation")->setInt(portrait ? sladoPortrait : sladoLandscape);
    const size_t columns = portrait ? 4 : 8, rows = portrait ? 8 : 4;
    std::vector<uint8_t> pixels(32, 255);
    const auto mask = sla::PNGRasterEncoder{}(pixels.data(), columns, rows, 1);
    const auto images = sla::LayerDocument::from_rasters({mask, mask, mask});
    auto source_job = SLAPrint::create_raster_job(config, images, {.1, .15, .2});
    const auto source = directory.path / (std::string("source.") + get_default_extension(format.c_str()));
    source_job->export_print(source.string());
    auto document = sla::LayerDocument::load_archive(source.string());
    document.set_layer_parameters(1, {{sla::LayerParameter::Exposure, 8.5}});
    const auto revision = document.revision();
    auto read = [](const boost::filesystem::path &path) {
        std::ifstream stream(path.string(), std::ios::binary);
        return std::vector<uint8_t>{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
    };
    const auto original = read(source);
    const auto geometry = document.raster_geometry();
    REQUIRE(geometry.has_value());
    REQUIRE(geometry->columns == columns); REQUIRE(geometry->rows == rows);
    REQUIRE(float(geometry->width_mm) == float(config.opt_float(portrait ? "display_height" : "display_width")));
    REQUIRE(float(geometry->height_mm) == float(config.opt_float(portrait ? "display_width" : "display_height")));
    if (directory.keep) {
        auto expected = document;
        expected.save_archive((directory.path / (std::string("expected.") + get_default_extension(format.c_str()))).string());
        config.save((directory.path / "source.ini").string());
    }
    const auto correct_output = directory.path / "valid.v4.ctb";
    REQUIRE_NOTHROW(SLAPrint::convert_layer_archive(config, document, "ctb_v4", correct_output.string()));
    const auto converted = sla::LayerDocument::load_archive(correct_output.string());
    REQUIRE(converted.raster_geometry().has_value());
    REQUIRE_NOTHROW(geometry->require_same_scale(*converted.raster_geometry()));
    auto destination = config; destination.set("sla_archive_format", std::string("ctb_v4"));
    std::string message;
    if (defect == "width" || defect == "height") {
        const bool oriented_width = defect == "width";
        const std::string key = oriented_width != portrait ? "display_width" : "display_height";
        destination.set(key, destination.opt_float(key) * 2);
        message = "Selected printer LCD " + defect + " differs from the source physical scale; rescale explicitly before conversion";
    } else if (defect == "resolution") {
        destination.set(portrait ? "display_pixels_y" : "display_pixels_x", 16);
        message = "Selected printer resolution differs from the source layer raster";
    } else {
        destination.set("max_print_height", .18);
        message = "Source layer Z exceeds the selected printer height";
    }
    const auto output = directory.path / "keep.v4.ctb";
    const std::vector<uint8_t> sentinel{'k','e','e','p'};
    auto reset = [&] { std::ofstream stream(output.string(), std::ios::binary | std::ios::trunc);
        stream.write(reinterpret_cast<const char *>(sentinel.data()), sentinel.size()); };
    reset();
    CHECK_THROWS_WITH(SLAPrint::convert_layer_archive(destination, document, "ctb_v4", output.string()), message);
    CHECK(read(output) == sentinel);
    reset();
    // Tiny temporary levels fit; the final source table must still be checked.
    std::vector<uint8_t> destination_pixels(size_t(destination.opt_int("display_pixels_x")) *
                                            size_t(destination.opt_int("display_pixels_y")), 255);
    const auto destination_mask = sla::PNGRasterEncoder{}(destination_pixels.data(),
        portrait ? destination.opt_int("display_pixels_y") : destination.opt_int("display_pixels_x"),
        portrait ? destination.opt_int("display_pixels_x") : destination.opt_int("display_pixels_y"), 1);
    auto destination_job = SLAPrint::create_raster_job(destination,
        sla::LayerDocument::from_rasters({destination_mask, destination_mask, destination_mask}), {.01, .02, .03});
    CHECK_THROWS_WITH(destination_job->export_layer_document(document, "ctb_v4", output.string()), message);
    CHECK(read(output) == sentinel);
    CHECK(read(source) == original);
    CHECK(document.revision() == revision);
    CHECK(document.dirty()); CHECK(document.can_undo());
}

TEST_CASE("Conversion geometry uses native precision and actual Z rather than baseline levels", "[sla_conversion_geometry]")
{
    using F = sla::LayerParameter;
    DynamicPrintConfig config; config.apply(SLAFullPrintConfig{});
    config.option("printer_technology")->setInt(ptSLA);
    config.set("display_pixels_x", 8); config.set("display_pixels_y", 4);
    config.set("display_width", 16.1234567); config.set("display_height", 4.7654321);
    config.option("display_orientation")->setInt(sladoLandscape);
    config.set("max_print_height", .15);
    sla::LayerConversionPlan plan;
    plan.source_geometry = sla::LayerRasterGeometry{8, 4, double(float(16.1234567)), double(float(4.7654321))};
    plan.rows = {{{F::PositionZ, double(float(.15))}}};
    REQUIRE_NOTHROW(sla::require_layer_conversion_geometry(plan, config));
    // A distinct native dimension must fail, even if rounded area stays near.
    auto changed = config;
    changed.set("display_width", double(std::nextafter(float(16.1234567), std::numeric_limits<float>::infinity())));
    REQUIRE_THROWS_WITH(sla::require_layer_conversion_geometry(plan, changed),
        "Selected printer LCD width differs from the source physical scale; rescale explicitly before conversion");
    changed = config; changed.set("display_width", config.opt_float("display_height"));
    changed.set("display_height", config.opt_float("display_width"));
    REQUIRE_THROWS(sla::require_layer_conversion_geometry(plan, changed));
    changed.option("display_orientation")->setInt(sladoPortrait);
    changed.set("display_pixels_x", 4); changed.set("display_pixels_y", 8);
    REQUIRE_NOTHROW(sla::require_layer_conversion_geometry(plan, changed));
    for (int defect = 0; defect < 7; ++defect) {
        changed = config;
        if (defect == 0) changed.set("display_width", 0.);
        if (defect == 1) changed.set("display_height", std::numeric_limits<double>::quiet_NaN());
        if (defect == 2) changed.set("max_print_height", 0.);
        if (defect == 3) changed.set("max_print_height", std::numeric_limits<double>::infinity());
        if (defect == 4) changed.set("display_pixels_x", 0);
        if (defect == 5) changed.set("display_pixels_y", 32769);
        if (defect == 6) changed.option("display_orientation")->setInt(99);
        REQUIRE_THROWS(sla::require_layer_conversion_geometry(plan, changed));
    }
    plan.rows[0][F::PositionZ] = .2;
    REQUIRE_THROWS_WITH(sla::require_layer_conversion_geometry(plan, config),
        "Source layer Z exceeds the selected printer height");
    plan.rows[0][F::PositionZ] = 0;
    REQUIRE_THROWS(sla::require_layer_conversion_geometry(plan, config));
    plan.rows[0][F::PositionZ] = std::numeric_limits<double>::quiet_NaN();
    REQUIRE_THROWS(sla::require_layer_conversion_geometry(plan, config));
    // Unknown source geometry remains unknown; target is not copied into it.
    plan.source_geometry.reset(); plan.rows[0][F::PositionZ] = .1;
    REQUIRE_NOTHROW(sla::require_layer_conversion_geometry(plan, config));
    REQUIRE_FALSE(plan.source_geometry.has_value());
}

TEST_CASE("Encrypted CTB numeric tilt detection uses normal lift distance and speed", "[sla_ctb_encrypted_parameters][sla_printer_motion]")
{
    const std::string format = GENERATE("ctb_encrypted_v4", "ctb_encrypted");
    const int pattern = GENERATE(0, 1, 2, 3, 4, 5, 6);
    CAPTURE(format, pattern);
    struct Directory {
        boost::filesystem::path path = boost::filesystem::unique_path("ctb-motion-%%%%");
        bool keep = false;
        ~Directory() { if (!keep) { boost::system::error_code error; boost::filesystem::remove_all(path, error); } }
    } directory;
    if (const char *root = std::getenv("SLA_PRINTER_MOTION_FIXTURES")) {
        directory.path = boost::filesystem::path(root) / (format + "-pattern" + std::to_string(pattern));
        directory.keep = true;
    }
    boost::filesystem::create_directories(directory.path);
    DynamicPrintConfig config; config.apply(SLAFullPrintConfig{});
    config.option("printer_technology")->setInt(ptSLA); config.set("sla_archive_format", format);
    config.set("printer_model", std::string("Numeric motion test"));
    config.set("display_pixels_x", 8); config.set("display_pixels_y", 4);
    config.option("display_orientation")->setInt(sladoLandscape);
    std::vector<uint8_t> pixels(32, 255);
    const auto mask = sla::PNGRasterEncoder{}(pixels.data(), 8, 4, 1);
    auto job = SLAPrint::create_raster_job(config, sla::LayerDocument::from_rasters({mask,mask,mask}), {.1,.15,.2});
    const auto source = directory.path / (std::string("source.") + get_default_extension(format.c_str()));
    job->export_print(source.string());
    std::ifstream input(source.string(), std::ios::binary);
    auto bytes = std::vector<uint8_t>{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    const sla::CTBEncryptedLayerLayout original(bytes); auto settings = original.settings;
    auto number = [&](size_t at, float value) {
        uint32_t bits; std::memcpy(&bits, &value, 4);
        for (size_t n = 0; n < 4; ++n) settings.at(at+n) = uint8_t(bits >> (8*n));
    };
    number(36, pattern == 3 ? .125f : pattern == 6 ? .5f : .05f);
    number(88, pattern == 0 || pattern == 3 ? 60.f : .05f); // Bottom speed is irrelevant.
    number(92, pattern == 1 ? .25f : pattern == 3 ? .375f : pattern == 6 ? .5f : .05f);
    number(140, pattern == 3 ? .25f : pattern == 4 ? .05f : 0.f);
    number(96, pattern == 0 || pattern == 3 || pattern == 4 || pattern == 6 ? .1f : pattern == 5 ? .5f : 60.f);
    const auto encoded = ctb_encrypted::aes_cbc(settings, true);
    std::copy(encoded.begin(), encoded.end(), bytes.begin() + original.settings_offset);
    { std::ofstream output(source.string(), std::ios::binary | std::ios::trunc);
      output.write(reinterpret_cast<const char *>(bytes.data()), bytes.size()); }
    const sla::CTBEncryptedLayerLayout layout(bytes);
    const bool tilted = pattern == 0 || pattern == 3;
    CHECK(sla::ctb_encrypted_tilting_vat(layout) == tilted);
    auto document = sla::LayerDocument::load_archive(source.string());
    const auto capabilities = document.parameter_capabilities();
    const auto lift = std::find_if(capabilities.begin(), capabilities.end(), [](const auto &field) {
        return field.field == sla::LayerParameter::LiftSpeed;
    });
    REQUIRE(lift != capabilities.end());
    CHECK(lift->editable == !tilted);
    const auto revision = document.revision();
    if (tilted) {
        CHECK_THROWS(document.set_layer_parameters(1, {{sla::LayerParameter::LiftSpeed, 90}}));
        CHECK(document.revision() == revision); CHECK_FALSE(document.can_undo());
    } else {
        CHECK_NOTHROW(document.set_layer_parameters(1, {{sla::LayerParameter::LiftSpeed, 90}}));
        CHECK(document.layer_parameters(1).at(sla::LayerParameter::LiftSpeed) == 90);
        CHECK(document.can_undo()); CHECK(document.undo()); CHECK(document.revision() == revision);
    }
    CHECK_NOTHROW(document.set_layer_parameters(1, {{sla::LayerParameter::Exposure, 8.5}}));
    CHECK(document.undo()); CHECK(document.revision() == revision);
}


TEST_CASE("Named tilting targets use fixed native motion and reference timing", "[sla_tilt_target]")
{
    const std::string format = GENERATE("ctb_encrypted_v4", "ctb_encrypted");
    const std::string machine = GENERATE("Saturn 4 Ultra", "Mars 5 Ultra");
    CAPTURE(format, machine);
    struct Directory {
        boost::filesystem::path path=boost::filesystem::unique_path("tilt-target-%%%%");
        bool keep=false;
        ~Directory(){if(!keep){boost::system::error_code e;boost::filesystem::remove_all(path,e);}}
    } directory;
    if(const char *root=std::getenv("SLA_TILT_TARGET_FIXTURES")) {
        directory.path=boost::filesystem::path(root)/(format+"-"+machine);directory.keep=true;
    }
    boost::filesystem::create_directories(directory.path);
    DynamicPrintConfig config; config.apply(SLAFullPrintConfig{});
    config.option("printer_technology")->setInt(ptSLA);
    config.set("sla_archive_format",format); config.set("printer_model",machine);
    config.set("display_pixels_x",8);config.set("display_pixels_y",4);
    config.option("display_orientation")->setInt(sladoLandscape);
    config.set("layer_height",.1); // Named tilt does not depend on nominal height=.05.
    std::vector<uint8_t> pixels(32,255);
    const auto png=sla::PNGRasterEncoder{}(pixels.data(),8,4,1);
    const auto masks=sla::LayerDocument::from_rasters({png,png,png});
    auto job=SLAPrint::create_raster_job(config,masks,{.1,.2,.3});
    const auto path=directory.path/(std::string("fresh.")+get_default_extension(format.c_str()));
    REQUIRE_NOTHROW(job->export_print(path.string()));
    auto read=[](const boost::filesystem::path &p){std::ifstream f(p.string(),std::ios::binary);return std::vector<uint8_t>{std::istreambuf_iterator<char>(f),{}};};
    const auto bytes=read(path);
    const sla::CTBEncryptedLayerLayout layout(bytes);
    // Protocol constants from CTBEncryptedFile.OnBeforeEncode, not values
    // obtained from our classifier or writer implementation.
    for(size_t offset:{84,88,92,96,100,192}) CHECK(layout.parameter(offset)==.05f);
    for(size_t offset:{132,136,140,144,148,152,196,228}) CHECK(layout.parameter(offset)==0.f);
    using F=sla::LayerParameter;
    float estimate=300;
    for(size_t i=0;i<3;++i) {
        const auto row=sla::ctb_encrypted_layer_parameters(layout,i);
        CHECK(row.at(F::LiftDistance)==double(.05f)); CHECK(row.at(F::LiftSpeed)==double(.05f));
        CHECK(row.at(F::RetractSpeed)==double(.05f));
        for(auto field:{F::LiftDistance2,F::LiftSpeed2,F::RetractDistance2,F::RetractSpeed2}) CHECK(row.at(field)==0);
        estimate+=float(row.at(F::Exposure)+row.at(F::WaitBefore)+row.at(F::WaitAfter)+row.at(F::WaitAfterLift)+4);
    }
    CHECK(layout.setting(76)==uint32_t(estimate));
    CHECK(layout.parameter(36)==.1f);
    auto document=sla::LayerDocument::load_archive(path.string());
    for(size_t i=0;i<3;++i) CHECK(document.image(i).buf==pixels);
    document.set_layer_parameters(1,{{F::Exposure,8.5}});
    const auto compatible_plan=sla::plan_layer_conversion(document,format);
    for(const auto &[field,value]:sla::ctb_encrypted_fixed_motion()) {
        for(int scope=0;scope<3;++scope) {
            auto invalid=compatible_plan;
            if(scope==0) invalid.globals->normal[field]=value+1;
            else if(scope==1) invalid.globals->bottom[field]=value+1;
            else invalid.rows[1][field]=value+1;
            CHECK_THROWS(sla::require_layer_conversion_motion(invalid,config));
        }
    }
    const auto target=directory.path/(std::string("converted.")+get_default_extension(format.c_str()));
    REQUIRE_NOTHROW(SLAPrint::convert_layer_archive(config,document,format,target.string()));
    const auto reopened=sla::LayerDocument::load_archive(target.string());
    for(size_t i=0;i<3;++i) CHECK(reopened.layer_parameters(i)==document.layer_parameters(i));

    // Ordinary source cannot be silently mapped onto fixed printer motion.
    config.set("printer_model",std::string("Ordinary source"));
    job=SLAPrint::create_raster_job(config,masks,{.1,.2,.3});
    const auto ordinary=directory.path/(std::string("ordinary.")+get_default_extension(format.c_str()));
    job->export_print(ordinary.string()); document=sla::LayerDocument::load_archive(ordinary.string());
    const auto revision=document.revision();const auto old=read(target);
    config.set("printer_model",machine);
    const auto plan=sla::plan_layer_conversion(document,format);
    CHECK_THROWS_WITH(sla::require_layer_conversion_motion(plan,config),
        Catch::Matchers::ContainsSubstring("Lift distance (mm)"));
    CHECK_THROWS(SLAPrint::convert_layer_archive(config,document,format,target.string()));
    auto target_job=SLAPrint::create_raster_job(config,masks,{.1,.2,.3});
    CHECK_THROWS(target_job->export_layer_document(document,format,target.string(),{},"negative"));
    const bool unchanged=read(target)==old;
    CHECK(unchanged);CHECK(document.revision()==revision);CHECK_FALSE(document.can_undo());
}


TEST_CASE("Anonymous numeric tilt keeps native motion and uses the stored classifier for time", "[sla_numeric_tilt]")
{
    const std::string format=GENERATE("ctb_encrypted_v4","ctb_encrypted");
    const double height=GENERATE(.05,.1);
    CAPTURE(format,height);
    struct Directory {
        boost::filesystem::path path=boost::filesystem::unique_path("fresh-numeric-%%%%");bool keep=false;
        ~Directory(){if(!keep){boost::system::error_code e;boost::filesystem::remove_all(path,e);}}
    } dir;
    if(const char *root=std::getenv("SLA_NUMERIC_TILT_FIXTURES")) {
        dir.path=boost::filesystem::path(root)/("fresh-"+format+"-"+std::to_string(height));dir.keep=true;
    }
    boost::filesystem::create_directories(dir.path);
    DynamicPrintConfig config;config.apply(SLAFullPrintConfig{});config.option("printer_technology")->setInt(ptSLA);
    config.set("sla_archive_format",format);config.set("printer_model",std::string("Generic numeric"));
    config.set("display_pixels_x",8);config.set("display_pixels_y",4);config.option("display_orientation")->setInt(sladoLandscape);
    config.set("layer_height",height);
    for(const auto &prefix:{std::string("sla_"),std::string("sla_bottom_")}) {
        config.set(prefix+"lift_height",height);config.set(prefix+"lift_speed",.1);
        config.set(prefix+"lift_height2",0.);config.set(prefix+"lift_speed2",0.);
        config.set(prefix+"retract_speed",.1);config.set(prefix+"retract_height2",0.);config.set(prefix+"retract_speed2",0.);
    }
    const std::vector<uint8_t> pixels(32,255);const auto png=sla::PNGRasterEncoder{}(pixels.data(),8,4,1);
    auto job=SLAPrint::create_raster_job(config,sla::LayerDocument::from_rasters({png,png,png}),{.1,.2,.3});
    const auto path=dir.path/(std::string("fresh.")+get_default_extension(format.c_str()));job->export_print(path.string());
    std::ifstream input(path.string(),std::ios::binary);const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(input),{}};
    const sla::CTBEncryptedLayerLayout layout(bytes);
    CHECK(sla::ctb_encrypted_tilting_vat(layout));
    CHECK(layout.parameter(92)==float(height));CHECK(layout.parameter(36)==float(height));
    float estimate=300;
    for(size_t i=0;i<3;++i) {
        const auto row=sla::ctb_encrypted_layer_parameters(layout,i);
        estimate+=float(row.at(sla::LayerParameter::Exposure)+row.at(sla::LayerParameter::WaitBefore)+
            row.at(sla::LayerParameter::WaitAfter)+row.at(sla::LayerParameter::WaitAfterLift)+4);
    }
    CHECK(layout.setting(76)==uint32_t(estimate));
}

TEST_CASE("AES conversion owns final numeric motion table and timing independently of bootstrap", "[sla_numeric_tilt][sla_layer_conversion]")
{
    const std::string source_format=GENERATE("ctb_v4","ctb_v5","ctb_encrypted_v4","ctb_encrypted");
    const std::string target=GENERATE("ctb_encrypted_v4","ctb_encrypted");
    const int pattern=GENERATE(0,1,2,3,4,5);
    const double off=GENERATE(0.,2.,4.,6.);
    CAPTURE(source_format,target,pattern,off);
    struct Directory {
        boost::filesystem::path path=boost::filesystem::unique_path("numeric-convert-%%%%");bool keep=false;
        ~Directory(){if(!keep){boost::system::error_code e;boost::filesystem::remove_all(path,e);}}
    } dir;
    if(const char *root=std::getenv("SLA_NUMERIC_TILT_FIXTURES")) {
        dir.path=boost::filesystem::path(root)/(source_format+"-to-"+target+"-pattern"+std::to_string(pattern)+"-off"+std::to_string(int(off)));dir.keep=true;
    }
    boost::filesystem::create_directories(dir.path);
    DynamicPrintConfig config;config.apply(SLAFullPrintConfig{});config.option("printer_technology")->setInt(ptSLA);
    config.set("sla_archive_format",source_format);config.set("printer_model",std::string("Generic numeric"));
    config.set("display_pixels_x",8);config.set("display_pixels_y",4);config.option("display_orientation")->setInt(sladoLandscape);
    const std::vector<uint8_t> pixels(32,255);const auto png=sla::PNGRasterEncoder{}(pixels.data(),8,4,1);
    const auto masks=sla::LayerDocument::from_rasters({png,png,png});
    auto job=SLAPrint::create_raster_job(config,masks,{.1,.2,.3});
    const auto source=dir.path/(std::string("source.")+get_default_extension(source_format.c_str()));job->export_print(source.string());
    auto read=[](const boost::filesystem::path &p){std::ifstream f(p.string(),std::ios::binary);return std::vector<uint8_t>{std::istreambuf_iterator<char>(f),{}};};
    auto globals=*sla::LayerDocument::load_archive(source.string()).job_parameters();using F=sla::LayerParameter;
    const float nominal=pattern==1?.1f:(pattern==2 || pattern==5)?.125f:.05f;
    const float first=pattern==1?.1f:(pattern==2 || pattern==5)?.125f:pattern==3?.051f:.05f;
    const float second=(pattern==2 || pattern==5)?.25f:0.f;
    const float speed=pattern==3?.06f:pattern==4?.05f:.1f;
    globals.layer_height=nominal;
    for(auto *row:{&globals.normal,&globals.bottom}) {
        (*row)[F::LiftDistance]=double(float(first+second))-double(second);(*row)[F::LiftDistance2]=second;
        (*row)[F::LiftSpeed]=speed;(*row)[F::LiftSpeed2]=second?.2f:0.f;
        (*row)[F::RetractSpeed]=speed;(*row)[F::RetractDistance2]=0;(*row)[F::RetractSpeed2]=0;
        (*row)[F::WaitBefore]=.75;(*row)[F::WaitAfter]=.5;(*row)[F::WaitAfterLift]=.25;(*row)[F::LightOff]=off;
    }
    auto bytes=read(source);
    auto scalar=[&](size_t at,float value){uint32_t bits;std::memcpy(&bits,&value,4);for(size_t n=0;n<4;++n)bytes.at(at+n)=uint8_t(bits>>(8*n));};
    const bool aes=source_format.find("encrypted")!=std::string::npos;
    std::vector<size_t> records;
    if(aes) {
        const sla::CTBEncryptedLayerLayout layout(bytes);auto settings=layout.settings;
        sla::overlay_encrypted_job(settings,globals);sla::job_u32(settings,76,0); // Deliberately unknown source estimate.
        const auto encrypted=ctb_encrypted::aes_cbc(settings,true);
        std::copy(encrypted.begin(),encrypted.end(),bytes.begin()+layout.settings_offset);
        for(size_t i=0;i<3;++i)records.push_back(layout.record(i));
    } else {
        const sla::ChituboxLayerLayout layout(bytes);sla::overlay_chitubox_job(bytes,layout,globals);sla::job_u32(bytes,76,0);
        for(size_t i=0;i<3;++i) {
            records.push_back(layout.extended_record(i));scalar(layout.record(i)+4, i==1?8.5f:10.f);scalar(layout.record(i)+8,float(off+i));
        }
    }
    for(size_t i=0;i<3;++i) {
        const size_t at=records[i];const float row_first=pattern==3?.05f:pattern==4?.125f:first;
        const float row_speed=pattern==5?1e-8f:pattern==3?.05f:pattern==4?.1f:speed;
        scalar(at+(aes?8:4),i==1?8.5f:10.f);scalar(at+(aes?12:8),float(off+i));
        scalar(at+40,row_first+second);scalar(at+44,row_speed);scalar(at+48,second);scalar(at+52,second?.2f:0.f);
        scalar(at+56,row_speed);scalar(at+60,0);scalar(at+64,0);scalar(at+68,.5f);scalar(at+72,.25f);scalar(at+76,.75f);
    }
    {std::ofstream output(source.string(),std::ios::binary|std::ios::trunc);output.write(reinterpret_cast<const char *>(bytes.data()),bytes.size());}
    auto document=sla::LayerDocument::load_archive(source.string());
    const auto revision=document.revision();
    const auto target_path=dir.path/(std::string("converted.")+get_default_extension(target.c_str()));
    const bool converted=([&] {
        try {
            if(pattern%2) job->export_layer_document(document,target,target_path.string(),{},"numeric");
            else SLAPrint::convert_layer_archive(config,document,target,target_path.string());
            return true;
        } catch(const std::exception &error) {WARN(error.what());return false;}
    })();
    REQUIRE(converted);
    const auto output=read(target_path);const sla::CTBEncryptedLayerLayout layout(output);
    CHECK(sla::ctb_encrypted_tilting_vat(layout)==(pattern!=3));
    auto reopened=sla::LayerDocument::load_archive(target_path.string());reopened.require_job_parameters(*document.job_parameters());
    double estimate=300;
    for(size_t i=0;i<3;++i) {
        const auto row=document.layer_parameters(i);
        CHECK(reopened.layer_parameters(i)==row);CHECK(reopened.image(i).buf==pixels);
        double cycle=4;
        if(pattern==3) cycle=std::nearbyint((60*row.at(F::LiftDistance)/row.at(F::LiftSpeed)+
            60*(row.at(F::LiftDistance)+row.at(F::LiftDistance2))/row.at(F::RetractSpeed))*100)/100;
        estimate+=row.at(F::Exposure)+row.at(F::WaitBefore)+row.at(F::WaitAfter)+row.at(F::WaitAfterLift)+std::max(cycle,row.at(F::LightOff));
    }
    CHECK(std::abs(double(layout.setting(76))-estimate)<=1.);
    const bool source_unchanged=read(source)==bytes;CHECK(source_unchanged);CHECK(document.revision()==revision);CHECK_FALSE(document.can_undo());
    if(pattern!=3) CHECK_THROWS(reopened.set_layer_parameters(1,{{F::LiftSpeed,90}}));
}

TEST_CASE("CTB v3 conversion keeps native defaults and neutral absent phases", "[sla_layer_conversion][sla_ctb_v3_globals]")
{
    using F = sla::LayerParameter;
    const std::string target = GENERATE("ctb_v3", "ctb_v4", "ctb_v5", "ctb_encrypted_v4", "ctb_encrypted");
    const bool edited = GENERATE(false, true);
    CAPTURE(target, edited);
    struct Directory {
        boost::filesystem::path path = boost::filesystem::unique_path("v3-globals-%%%%");
        bool keep = false;
        ~Directory() { if (!keep) { boost::system::error_code error; boost::filesystem::remove_all(path, error); } }
    } directory;
    if (const char *root = std::getenv("SLA_CTB_V3_FIXTURES")) {
        directory.path = boost::filesystem::path(root) / (target + (edited ? "-edited" : "-pristine"));
        directory.keep = true;
    }
    boost::filesystem::create_directories(directory.path);
    DynamicPrintConfig config; config.apply(SLAFullPrintConfig{});
    config.option("printer_technology")->setInt(ptSLA);
    config.set("sla_archive_format", std::string("ctb_v3"));
    config.set("display_pixels_x", 8); config.set("display_pixels_y", 4);
    config.set("display_width", 16.); config.set("display_height", 4.);
    config.option("display_orientation")->setInt(sladoLandscape);
    config.set("sla_bottom_layers", 1); config.set("sla_transition_layers", 1);
    config.set("exposure_time", 3.25); config.set("initial_exposure_time", 11.5);
    for (bool bottom : {false, true}) {
        const std::string prefix = bottom ? "sla_bottom_" : "sla_";
        config.set(prefix + "lift_height", bottom ? 5. : 3.);
        config.set(prefix + "lift_height2", 0.); config.set(prefix + "retract_height2", 0.);
        config.set(prefix + "lift_speed", bottom ? 60. : 90.);
        config.set(prefix + "retract_speed", 150.);
        config.set(prefix + "wait_before_cure", 1.25);
        config.set(prefix + "wait_after_cure", 0.); config.set(prefix + "wait_after_lift", 0.);
        config.set(prefix + "light_pwm", bottom ? 200 : 180);
    }
    std::vector<uint8_t> pixels(32, 0); pixels[1] = 255; pixels[20] = 255;
    const auto png = sla::PNGRasterEncoder{}(pixels.data(), 8, 4, 1);
    auto job = SLAPrint::create_raster_job(config, sla::LayerDocument::from_rasters({png, png, png}), {.1, .15, .2});
    const auto source = directory.path / "source.v3.ctb";
    job->export_print(source.string());
    auto read = [](const boost::filesystem::path &path) {
        std::ifstream stream(path.string(), std::ios::binary);
        return std::vector<uint8_t>{std::istreambuf_iterator<char>(stream), {}};
    };
    const auto original = read(source);
    auto document = sla::LayerDocument::load_archive(source.string());
    if (edited) document.set_layer_parameters(1, {{F::Exposure, 8.75}, {F::LiftDistance, 4.5}});
    const auto revision = document.revision();
    const auto globals = document.job_parameters();
    CHECK(globals.has_value());
    if (globals) {
        CHECK(globals->bottom_layers == 1); CHECK(globals->transition_layers == 1);
        CHECK(globals->normal.at(F::Exposure) == 3.25);
        CHECK(globals->bottom.at(F::Exposure) == 11.5);
        CHECK(globals->normal.at(F::LightOff) == double(4.45f));
        CHECK(globals->bottom.at(F::LightOff) == double(8.25f));
    }
    auto destination = config;
    destination.set("sla_archive_format", target);
    destination.set("exposure_time", 2.); destination.set("initial_exposure_time", 20.);
    destination.set("sla_bottom_layers", 2); destination.set("sla_transition_layers", 0);
    for (bool bottom : {false, true}) {
        const std::string prefix = bottom ? "sla_bottom_" : "sla_";
        destination.set(prefix + "lift_height2", 2.); destination.set(prefix + "retract_height2", 1.);
        destination.set(prefix + "lift_speed2", 180.); destination.set(prefix + "retract_speed2", 240.);
        destination.set(prefix + "wait_before_cure", .75); destination.set(prefix + "wait_after_cure", .5);
        destination.set(prefix + "wait_after_lift", .25);
    }
    destination.save((directory.path / "target.ini").string());
    const auto output = directory.path / ("converted." + std::string(get_default_extension(target.c_str())));
    REQUIRE_NOTHROW(SLAPrint::convert_layer_archive(destination, document, target, output.string()));
    const auto converted = sla::LayerDocument::load_archive(output.string());
    if (globals) REQUIRE_NOTHROW(converted.require_job_parameters(*globals));
    for (size_t i = 0; i < 3; ++i) {
        auto expected = document.layer_parameters(i);
        if (target != "ctb_v3") for (auto field : {F::LiftDistance2, F::LiftSpeed2, F::RetractDistance2, F::RetractSpeed2,
                           F::WaitBefore, F::WaitAfter, F::WaitAfterLift}) expected[field] = 0.;
        CHECK(converted.layer_parameters(i) == expected);
        CHECK(converted.image(i).buf == document.image(i).buf);
    }
    REQUIRE(globals.has_value());
    auto put_word = [](std::vector<uint8_t> &bytes, size_t at, uint32_t word) {
        for (unsigned n = 0; n < 4; ++n) bytes.at(at + n) = uint8_t(word >> (8 * n));
    };
    auto write_bytes = [](const boost::filesystem::path &path, const std::vector<uint8_t> &bytes) {
        std::ofstream stream(path.string(), std::ios::binary | std::ios::trunc);
        stream.write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
    };
    const sla::ChituboxLayerLayout layout(original);
    const auto params = layout.parameters, slicer = layout.integer(104);
    const auto malformed_path = directory.path / "malformed.v3.ctb";
    const auto protected_path = directory.path / "protected-overlay.v3.ctb";
    { std::ofstream stream(protected_path.string(), std::ios::binary); stream << "previous valid output"; }
    const auto protected_bytes = read(protected_path);
    for (int defect = 0; defect < 6; ++defect) {
        auto malformed = original;
        if (defect == 0) put_word(malformed, params + 40, 2); // contradictory count duplicate
        if (defect == 1) put_word(malformed, 44, 0); // contradictory LightOff duplicate
        if (defect == 2) malformed.at(slicer + 39) = 0x40; // wrong version's table mode
        if (defect == 3) put_word(malformed, slicer + 60, 3); // too many transitions
        if (defect == 4) put_word(malformed, 84, 0); // missing print parameters
        if (defect == 5) put_word(malformed, 108, 63); // short slicer section
        write_bytes(malformed_path, malformed);
        REQUIRE_THROWS(sla::LayerDocument::load_archive(malformed_path.string()).job_parameters());
        // Pixel/table import can succeed while nominal sections are malformed.
        // Optional typed overlays must not repair or overwrite such an input.
        write_bytes(protected_path, protected_bytes);
        std::optional<sla::LayerDocument> imported;
        try { imported = sla::LayerDocument::load_archive(malformed_path.string()); }
        catch (const std::exception &) { /* Earlier import rejection is also safe. */ }
        if (imported) {
            CHECK_THROWS(imported->save_archive(protected_path.string(), {}, nullptr, &*globals));
            CHECK(imported->revision() == 0);
        }
        const bool output_preserved = read(protected_path) == protected_bytes;
        CHECK(output_preserved);
        CHECK(document.revision() == revision);
        CHECK(read(source) == original);
    }
    // Opaque v3 slots must not acquire the semantics they have in v4. They
    // remain unchanged even when the known nominal defaults are overlaid.
    auto opaque = original;
    float marker = 123.25f; uint32_t bits; std::memcpy(&bits, &marker, 4);
    for (size_t offset : {0u, 4u, 8u, 12u, 16u, 20u, 24u, 52u, 56u}) put_word(opaque, slicer + offset, bits);
    for (size_t i = 0; i < 3; ++i)
        for (size_t offset : {48u, 52u, 60u, 64u, 68u, 72u, 76u}) put_word(opaque, layout.extended_record(i) + offset, bits);
    const auto opaque_path = directory.path / "opaque.v3.ctb";
    write_bytes(opaque_path, opaque);
    auto opaque_document = sla::LayerDocument::load_archive(opaque_path.string());
    REQUIRE_NOTHROW(opaque_document.require_job_parameters(*globals));
    for (size_t i = 0; i < 3; ++i)
        CHECK(opaque_document.layer_parameters(i) == sla::chitubox_layer_parameters(layout, i));
    const auto opaque_saved = directory.path / "opaque-saved.v3.ctb";
    REQUIRE_NOTHROW(opaque_document.save_archive(opaque_saved.string(), {}, nullptr, &*globals));
    CHECK(read(opaque_saved) == opaque);
    const auto sentinel = directory.path / "keep.v3.ctb";
    { std::ofstream stream(sentinel.string(), std::ios::binary); stream << "previous valid output"; }
    const auto sentinel_bytes = read(sentinel);
    for (int defect = 0; defect < 2; ++defect) {
        auto invalid = *globals;
        if (defect == 0) invalid.normal[F::LiftDistance2] = 1.;
        else invalid.bottom[F::RetractSpeed] += 10.;
        REQUIRE_THROWS(document.save_archive(sentinel.string(), {}, nullptr, &invalid));
        CHECK(read(sentinel) == sentinel_bytes);
        CHECK(document.revision() == revision);
    }
    CHECK(document.revision() == revision); CHECK(read(source) == original);
}

// Staged only: root may append to sla_layer_parameters_tests.cpp after its current freeze.
// No production parser is used for the semantic assertions below.
TEST_CASE("CXDLPv4 extended layers store total lift, not first phase", "[sla_reference][sla_cxdlpv4_two_stage][sla_cxdlpv4_time]")
{
    const bool portrait=GENERATE(false,true);
    const bool second_phase=GENERATE(false,true);
    const bool distinct_bottom=GENERATE(false,true);
    const bool fractional=GENERATE(false,true);
    CAPTURE(portrait,second_phase,distinct_bottom,fractional);
    struct Directory {
        boost::filesystem::path path=boost::filesystem::unique_path("cxdlpv4-two-stage-%%%%");
        bool keep=false;
        ~Directory(){if(!keep){boost::system::error_code error;boost::filesystem::remove_all(path,error);}}
    } directory;
    if(const char* root=std::getenv("SLA_CXDLPV4_TWO_STAGE_FIXTURES")) {
        directory.path=boost::filesystem::path(root)/(std::string(portrait?"portrait":"landscape")+"-"+(second_phase?"two":"one")+"-"+(distinct_bottom?"distinct":"shared")+"-"+(fractional?"fractional":"integer"));directory.keep=true;
    }
    boost::filesystem::create_directories(directory.path);
    DynamicPrintConfig config;config.apply(SLAFullPrintConfig{});
    config.option("printer_technology")->setInt(ptSLA);
    config.set("sla_archive_format","cxdlpv4");config.set("printer_model","CL-60");
    config.set("display_pixels_x",8);config.set("display_pixels_y",4);
    config.set("display_width",16.);config.set("display_height",4.);
    config.option("display_orientation")->setInt(portrait?sladoPortrait:sladoLandscape);
    config.set("sla_bottom_layers",1);config.set("sla_transition_layers",0);
    for(bool bottom:{false,true}) {
        const std::string prefix=bottom?"sla_bottom_":"sla_";
        config.set(prefix+"lift_height",bottom && distinct_bottom?4.:3.);
        config.set(prefix+"lift_height2",second_phase?(bottom && distinct_bottom?1.:2.):0.);
        config.set(prefix+"lift_speed",60.);config.set(prefix+"lift_speed2",180.);
        config.set(prefix+"retract_speed",150.);config.set(prefix+"retract_speed2",240.);
    }
    if(fractional) {
        config.set("exposure_time",3.135);config.set("initial_exposure_time",9.125);
        for(bool bottom:{false,true}) {
            const std::string prefix=bottom?"sla_bottom_":"sla_";
            config.set(prefix+"wait_before_cure",.125);config.set(prefix+"wait_after_cure",.235);
            config.set(prefix+"wait_after_lift",.345);config.set(prefix+"retract_height2",bottom?2.125:1.125);
        }
    }
    std::vector<uint8_t> pixels(32,0);pixels[1]=255;pixels[20]=127;
    const auto png=sla::PNGRasterEncoder{}(pixels.data(),portrait?4:8,portrait?8:4,1);
    auto job=SLAPrint::create_raster_job(config,sla::LayerDocument::from_rasters({png,png,png}),{.1,.15,.2});
    const auto output=directory.path/"two-stage.cxdlpv4";
    job->export_print(output.string());config.save((directory.path/"source.ini").string());
    std::ifstream stream(output.string(),std::ios::binary);
    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(stream)),{});
    auto u32=[&](size_t at){REQUIRE(at+4<=bytes.size());return uint32_t(bytes[at])|(uint32_t(bytes[at+1])<<8)|(uint32_t(bytes[at+2])<<16)|(uint32_t(bytes[at+3])<<24);};
    auto f32=[&](size_t at){const auto word=u32(at);float value;std::memcpy(&value,&word,4);return value;};
    REQUIRE(bytes.size()>19);
    const uint32_t model_size=(uint32_t(bytes[15])<<24)|(uint32_t(bytes[16])<<16)|(uint32_t(bytes[17])<<8)|uint32_t(bytes[18]);
    const size_t header=19+model_size;
    const size_t parameters=u32(header+52),table=u32(header+32);
    REQUIRE(u32(header+36)==3);
    const float normal_first=3.f,normal_second=second_phase?2.f:0.f;
    const float bottom_first=distinct_bottom?4.f:3.f,bottom_second=second_phase?(distinct_bottom?1.f:2.f):0.f;
    REQUIRE(f32(parameters)==bottom_first+bottom_second);
    REQUIRE(f32(parameters+8)==normal_first+normal_second);
    for(size_t i=0;i<3;++i) {
        CAPTURE(i);
        const size_t extended=u32(table+40*i+12);
        const float first=i==0?bottom_first:normal_first,second=i==0?bottom_second:normal_second;
        REQUIRE(f32(extended)==first+second);
        REQUIRE(f32(extended+8)==second);
        REQUIRE(f32(extended)-f32(extended+8)==first);
    }
    // Independent wire reader: UVtools begins with 300 seconds preparation,
    // rounds each motor estimate to hundredths and stores positive uint time.
    float complete_time=300.f;
    auto round2=[](float value){return std::nearbyint(value*100.f)/100.f;};
    for(size_t i=0;i<3;++i) {
        const size_t row=table+40*i,extended=u32(row+12);
        const float first=round2(round2(f32(extended))-f32(extended+8));
        const float second=round2(f32(extended+8));
        const float retract2=round2(f32(extended+20));
        float motor=0.f;
        auto phase=[&](float distance,float speed){if(distance>0 && speed>0)motor+=distance/(speed/60.f);};
        phase(first,round2(f32(extended+4)));phase(second,round2(f32(extended+12)));
        phase(retract2,round2(f32(extended+24)));phase(first+second-retract2,round2(f32(extended+16)));
        const float rests=round2(f32(extended+36))+round2(f32(row+4))+
            round2(f32(extended+28))+round2(f32(extended+32));
        complete_time+=rests+std::max(round2(motor),round2(f32(row+8)));
    }
    const auto expected_time=uint32_t(round2(complete_time));
    CHECK(u32(header+44)==expected_time);
    const auto reopened=sla::LayerDocument::load_archive(output.string());
    REQUIRE(reopened.layer_count()==3);
    for(size_t i=0;i<3;++i)CHECK(reopened.image(i).buf==pixels);
}


TEST_CASE("CXDLPv4 refuses ambiguous or overflowing time before replacing output", "[sla_cxdlpv4_time]") {
    const bool underflow_speed=GENERATE(false,true);
    DynamicPrintConfig config;config.apply(SLAFullPrintConfig{});
    config.option("printer_technology")->setInt(ptSLA);
    config.set("sla_archive_format","cxdlpv4");config.set("printer_model","CL-60");
    config.set("display_pixels_x",8);config.set("display_pixels_y",4);
    config.set("display_width",16.);config.set("display_height",4.);
    config.option("display_orientation")->setInt(sladoLandscape);
    for(bool bottom:{false,true}) {
        const std::string prefix=bottom?"sla_bottom_":"sla_";
        config.set(prefix+(underflow_speed?"lift_speed":"wait_before_cure"),underflow_speed?1e-8:1e30);
    }
    std::vector<uint8_t> pixels(32,0);pixels[1]=255;
    const auto png=sla::PNGRasterEncoder{}(pixels.data(),8,4,1);
    const auto job=SLAPrint::create_raster_job(config,sla::LayerDocument::from_rasters({png,png}),{.05,.1});
    struct File {boost::filesystem::path path=boost::filesystem::unique_path("time-protected-%%%%.cxdlpv4");
        ~File(){boost::system::error_code e;boost::filesystem::remove(path,e);}} file;
    {std::ofstream stream(file.path.string(),std::ios::binary);stream<<"previous valid print";}
    CHECK_THROWS(job->export_print(file.path.string()));
    {std::ifstream stream(file.path.string(),std::ios::binary);
     const bool preserved=std::string(std::istreambuf_iterator<char>(stream),{})=="previous valid print";
     CHECK(preserved);}
}


TEST_CASE("CXDLPv4 time uses reference speed fallback without changing native values", "[sla_cxdlpv4_fallback]") {
    const size_t field=GENERATE(size_t(0),size_t(1),size_t(2),size_t(3));
    const bool tiny_bottom=GENERATE(false,true);
    const int bottom_count=GENERATE(0,1,2);
    const bool direct=GENERATE(false,true);
    CAPTURE(field,tiny_bottom,bottom_count,direct);
    const std::array<std::string,4> keys{"lift_speed","lift_speed2","retract_speed","retract_speed2"};
    DynamicPrintConfig config;config.apply(SLAFullPrintConfig{});
    config.option("printer_technology")->setInt(ptSLA);
    config.set("sla_archive_format","cxdlpv4");config.set("printer_model","CL-60");
    config.set("display_pixels_x",8);config.set("display_pixels_y",4);
    config.set("display_width",16.);config.set("display_height",4.);
    config.option("display_orientation")->setInt(sladoLandscape);
    config.set("sla_bottom_layers",bottom_count);config.set("sla_transition_layers",0);
    config.set("initial_exposure_time",9.);config.set("exposure_time",3.);
    const std::array<float,4> regular{60.f,180.f,150.f,240.f};
    for(bool bottom:{false,true}) {
        const std::string prefix=bottom?"sla_bottom_":"sla_";
        config.set(prefix+"lift_height",3.);config.set(prefix+"lift_height2",2.);
        config.set(prefix+"retract_height2",1.);
        for(size_t k=0;k<4;++k)config.set(prefix+keys[k],double(regular[k]));
        config.set(prefix+"wait_before_cure",.125);config.set(prefix+"wait_after_cure",.25);
        config.set(prefix+"wait_after_lift",.375);
    }
    const std::string tiny_key=(tiny_bottom?"sla_bottom_":"sla_")+keys[field];
    config.set(tiny_key,.004);
    std::vector<uint8_t> pixels(32,0);pixels[1]=255;pixels[20]=127;
    const auto png=sla::PNGRasterEncoder{}(pixels.data(),8,4,1);
    const auto document=sla::LayerDocument::from_rasters({png,png,png});
    const auto job=SLAPrint::create_raster_job(config,document,{.05,.1,.15});
    struct Directory {boost::filesystem::path path=boost::filesystem::unique_path("speed-fallback-%%%%");bool keep=false;
        ~Directory(){if(!keep){boost::system::error_code e;boost::filesystem::remove_all(path,e);}}} directory;
    if(const char* root=std::getenv("SLA_CXDLPV4_FALLBACK_FIXTURES")) {
        directory.path=boost::filesystem::path(root)/(keys[field]+"-"+(tiny_bottom?"bottom":"normal")+
            "-"+std::to_string(bottom_count)+"-"+(direct?"direct":"explicit"));directory.keep=true;
    }
    boost::filesystem::create_directories(directory.path);
    config.save((directory.path/"source.ini").string());
    const auto output=directory.path/"fallback.cxdlpv4";
    if(direct)REQUIRE_NOTHROW(job->export_print(output.string()));
    else REQUIRE_NOTHROW(job->export_layer_document(document,"cxdlpv4",output.string()));
    std::ifstream input(output.string(),std::ios::binary);
    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)),{});
    auto u32=[&](size_t at){REQUIRE(at+4<=bytes.size());return uint32_t(bytes[at])|(uint32_t(bytes[at+1])<<8)|(uint32_t(bytes[at+2])<<16)|(uint32_t(bytes[at+3])<<24);};
    auto f32=[&](size_t at){auto bits=u32(at);float value;std::memcpy(&value,&bits,4);return value;};
    REQUIRE(bytes.size()>19);
    const size_t base=19+(size_t(bytes[15])<<24)+(size_t(bytes[16])<<16)+(size_t(bytes[17])<<8)+bytes[18];
    const auto table=u32(base+32);
    const std::array<size_t,4> offsets{4,12,16,24};
    float complete=300.f;
    auto round2=[](float value){return std::nearbyint(value*100.f)/100.f;};
    for(size_t i=0;i<3;++i) {
        const bool bottom=i<size_t(bottom_count);
        const auto extended=u32(table+40*i+12);
        auto speed=regular;
        if(bottom==tiny_bottom) {
            CHECK(f32(extended+offsets[field])==float(.004));
            // During decode the bottom retract globals still have the base
            // defaults. Lift/normal-retract globals expose native raw speeds.
            speed[field]=bottom && field>=2?(field==2?100.f:80.f):float(.004);
        }
        float motor=3.f/(speed[0]/60.f)+2.f/(speed[1]/60.f);
        motor+=1.f/(speed[3]/60.f);motor+=4.f/(speed[2]/60.f);
        complete+=(bottom?9.f:3.f)+.75f+round2(motor);
    }
    CHECK(u32(base+44)==uint32_t(round2(complete)));
    const auto restored=sla::LayerDocument::load_archive(output.string());
    for(size_t i=0;i<3;++i)CHECK(restored.image(i).buf==pixels);
    CHECK(job->full_print_config().opt_float(tiny_key)==.004);
    CHECK_FALSE(job->layer_document()->fresh_job());
}


TEST_CASE("CXDLPv4 typed rows retain raw native parameters without public fallback", "[sla_cxdlpv4_parameters]") {
    const bool portrait=GENERATE(false,true);
    const bool second=GENERATE(false,true);
    const bool tiny=GENERATE(false,true);
    const int bottom_count=GENERATE(0,2);
    CAPTURE(portrait,second,tiny,bottom_count);
    using F=sla::LayerParameter;
    DynamicPrintConfig config;config.apply(SLAFullPrintConfig{});
    config.option("printer_technology")->setInt(ptSLA);
    config.set("sla_archive_format","cxdlpv4");config.set("printer_model","CL-60");
    config.set("display_pixels_x",8);config.set("display_pixels_y",4);
    config.set("display_width",16.);config.set("display_height",4.);
    config.option("display_orientation")->setInt(portrait?sladoPortrait:sladoLandscape);
    config.set("sla_bottom_layers",bottom_count);config.set("sla_transition_layers",0);
    config.set("initial_exposure_time",9.125);config.set("exposure_time",3.135);
    for(bool bottom:{false,true}) {
        const std::string prefix=bottom?"sla_bottom_":"sla_";
        config.set(prefix+"lift_height",bottom?4.125:3.125);
        config.set(prefix+"lift_height2",second?2.125:0.);
        config.set(prefix+"lift_speed",tiny?.004:60.125);
        config.set(prefix+"lift_speed2",180.125);
        config.set(prefix+"retract_speed",tiny?.004:150.125);
        config.set(prefix+"retract_height2",second?1.125:0.);
        config.set(prefix+"retract_speed2",240.125);
        config.set(prefix+"wait_before_cure",bottom?.125:.235);
        config.set(prefix+"wait_after_cure",.345);
        config.set(prefix+"wait_after_lift",bottom?.455:.565);
        config.set(prefix+"light_pwm",bottom?129:200);
    }
    const uint32_t columns=portrait?4:8,rows=portrait?8:4;
    std::vector<uint8_t> pixels(32,0);pixels[1]=255;pixels[20]=127;
    const auto png=sla::PNGRasterEncoder{}(pixels.data(),columns,rows,1);
    const auto job=SLAPrint::create_raster_job(config,sla::LayerDocument::from_rasters({png,png,png}),{.05,.1,.15});
    struct File {boost::filesystem::path path=boost::filesystem::unique_path("cx-raw-%%%%.cxdlpv4");
        bool keep=false;
        ~File(){if(!keep){boost::system::error_code e;boost::filesystem::remove(path,e);}}} file;
    if(const char *root=std::getenv("SLA_CXDLPV4_PARAMETERS_FIXTURES")) {
        boost::filesystem::create_directories(root);
        file.path=boost::filesystem::path(root)/(std::string(portrait?"portrait":"landscape")+
            (second?"-second":"-single")+(tiny?"-tiny":"-regular")+"-bottom"+std::to_string(bottom_count)+".cxdlpv4");
        file.keep=true;
    }
    job->export_print(file.path.string());
    std::ifstream stream(file.path.string(),std::ios::binary);
    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(stream)),{});
    const sla::CrealityCXDLPv4Layout layout(bytes);
    for(size_t i=0;i<3;++i) {
        const bool bottom=i<size_t(bottom_count);
        const sla::LayerParameters expected{
            {F::PositionZ,double(float(.05*double(i+1)))},
            {F::Exposure,double(float(bottom?9.125:3.135))},{F::LightOff,0},
            {F::LiftDistance,bottom?4.125:3.125},{F::LiftDistance2,second?2.125:0},
            {F::LiftSpeed,double(float(tiny?.004:60.125))},{F::LiftSpeed2,180.125},
            {F::RetractSpeed,double(float(tiny?.004:150.125))},
            {F::RetractDistance2,second?1.125:0},{F::RetractSpeed2,240.125},
            {F::WaitBefore,double(float(bottom?.125:.235))},{F::WaitAfter,double(float(.345))},
            {F::WaitAfterLift,double(float(bottom?.455:.565))},{F::LightPWM,bottom?129.:200.}};
        const auto actual=sla::cxdlpv4_layer_parameters(layout,i);
        CHECK(actual==expected);
    }
    CHECK_THROWS(sla::cxdlpv4_layer_parameters(layout,layout.layers.size()));
    // Until global/write/history contracts are complete the imported editor
    // must retain its opaque mask-only route, not advertise editable fields.
    const auto imported=sla::LayerDocument::load_archive(file.path.string());
    CHECK(imported.parameter_capabilities().empty());
    CHECK_FALSE(imported.job_parameters());
    // Additional hostile/native coverage, added after the original RED.
    // Checksum-valid hostile phase values must not become typed parameters.
    auto patched=[&](size_t offset,float value) {
        auto copy=bytes;uint32_t bits;std::memcpy(&bits,&value,4);
        for(size_t k=0;k<4;++k)copy[offset+k]=uint8_t(bits>>(8*k));
        const uint32_t checksum=sla::creality_checksum(copy.data(),copy.size()-4,4);
        for(size_t k=0;k<4;++k)copy[copy.size()-4+k]=uint8_t(checksum>>(24-8*k));
        return copy;
    };
    for(const auto &[offset,value]:std::vector<std::pair<size_t,float>>{
        {layout.layers[0].data+8,16.f}, // SECOND greater than TOTAL
        {layout.layers[0].data+20,16.f}, // secondary retract exceeds TOTAL
        {layout.layers[0].data+40,127.5f}}) { // fractional PWM loses raw information
        const auto hostile=patched(offset,value);
        const sla::CrealityCXDLPv4Layout hostile_layout(hostile);
        CHECK_THROWS(sla::cxdlpv4_layer_parameters(hostile_layout,0));
    }
    // Zero native speed is still readable. Only a contextual planner can
    // decide whether the reference fallback or requested export is valid.
    const auto zero_speed=patched(layout.layers[0].data+4,0.f);
    const sla::CrealityCXDLPv4Layout zero_layout(zero_speed);
    CHECK(sla::cxdlpv4_layer_parameters(zero_layout,0).at(F::LiftSpeed)==0.);
}


TEST_CASE("CXDLPv4 native globals retain missing fields and independent metadata", "[sla_cxdlpv4_globals]") {
    const bool portrait=GENERATE(false,true),second=GENERATE(false,true),tiny=GENERATE(false,true);
    const int bottom_count=GENERATE(0,2);
    CAPTURE(portrait,second,tiny,bottom_count);
    using F=sla::LayerParameter;
    DynamicPrintConfig config;config.apply(SLAFullPrintConfig{});
    config.option("printer_technology")->setInt(ptSLA);
    config.set("sla_archive_format","cxdlpv4");config.set("printer_model","CL-60");
    config.set("display_pixels_x",8);config.set("display_pixels_y",4);
    config.set("display_width",16.);config.set("display_height",4.);
    config.option("display_orientation")->setInt(portrait?sladoPortrait:sladoLandscape);
    config.set("sla_bottom_layers",bottom_count);config.set("sla_transition_layers",5);
    config.set("initial_exposure_time",9.125);config.set("exposure_time",3.135);
    for(bool bottom:{false,true}) {
        const std::string prefix=bottom?"sla_bottom_":"sla_";
        config.set(prefix+"lift_height",bottom?4.125:3.125);
        config.set(prefix+"lift_height2",second?2.125:0.);
        config.set(prefix+"lift_speed",tiny?.004:60.125);config.set(prefix+"lift_speed2",180.125);
        config.set(prefix+"retract_speed",tiny?.004:150.125);
        config.set(prefix+"retract_height2",second?1.125:0.);config.set(prefix+"retract_speed2",240.125);
        config.set(prefix+"wait_before_cure",bottom?.125:.235);
        config.set(prefix+"wait_after_cure",.345);config.set(prefix+"wait_after_lift",bottom?.455:.565);
        config.set(prefix+"light_pwm",bottom?129:200);
    }
    std::vector<uint8_t> pixels(32,0);pixels[1]=255;pixels[20]=127;
    const auto png=sla::PNGRasterEncoder{}(pixels.data(),portrait?4:8,portrait?8:4,1);
    const auto job=SLAPrint::create_raster_job(config,sla::LayerDocument::from_rasters({png,png,png}),{.05,.1,.15});
    struct File {boost::filesystem::path path=boost::filesystem::unique_path("cx-globals-%%%%.cxdlpv4");bool keep=false;
        ~File(){if(!keep){boost::system::error_code e;boost::filesystem::remove(path,e);}}} file;
    if(const char *root=std::getenv("SLA_CXDLPV4_GLOBALS_FIXTURES")) {
        boost::filesystem::create_directories(root);
        file.path=boost::filesystem::path(root)/(std::string(portrait?"portrait":"landscape")+
            (second?"-second":"-single")+(tiny?"-tiny":"-regular")+"-bottom"+std::to_string(bottom_count)+".cxdlpv4");file.keep=true;
    }
    job->export_print(file.path.string());
    std::ifstream stream(file.path.string(),std::ios::binary);
    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(stream)),{});
    const sla::CrealityCXDLPv4Layout layout(bytes);
    const auto snapshot=sla::cxdlpv4_native_globals(layout);
    const sla::LayerParameters normal{
        {F::Exposure,double(float(3.135))},{F::LightOff,0},{F::LiftDistance,3.125},
        {F::LiftDistance2,second?2.125:0},{F::LiftSpeed,double(float(tiny?.004:60.125))},
        {F::LiftSpeed2,180.125},{F::RetractSpeed,double(float(tiny?.004:150.125))},
        {F::RetractDistance2,second?1.125:0},{F::RetractSpeed2,240.125},
        {F::WaitBefore,double(float(.235))},{F::WaitAfter,double(float(.345))},
        {F::WaitAfterLift,double(float(.565))},{F::LightPWM,200}};
    const sla::LayerParameters bottom{
        {F::Exposure,9.125},{F::LightOff,0},{F::LiftDistance,4.125},
        {F::LiftDistance2,second?2.125:0},{F::LiftSpeed,double(float(tiny?.004:60.125))},
        {F::LiftSpeed2,180.125},{F::LightPWM,129}};
    CHECK(snapshot.normal==normal);CHECK(snapshot.bottom==bottom);
    CHECK(snapshot.layer_height==double(float(config.opt_float("layer_height"))));
    CHECK(snapshot.bottom_layers==uint32_t(bottom_count));CHECK(snapshot.parameter_bottom_layers==uint32_t(bottom_count));
    CHECK(snapshot.transition_layers==0);CHECK(snapshot.raw_per_layer_mode==1);
    CHECK(snapshot.mirrors==std::array<float,3>{float(3.135),float(9.125),float(.455)});
    for(F field:{F::RetractSpeed,F::RetractDistance2,F::RetractSpeed2,F::WaitBefore,F::WaitAfter,F::WaitAfterLift})
        CHECK(snapshot.bottom.count(field)==0);
    const auto imported=sla::LayerDocument::load_archive(file.path.string());
    const auto read=imported.cxdlpv4_native_job();
    CHECK(bool(read));
    if(read) {CHECK(read->normal==normal);CHECK(read->bottom==bottom);CHECK(read->transition_layers==0);}
    CHECK(imported.parameter_capabilities().empty());CHECK_FALSE(imported.job_parameters());
    CHECK_FALSE(sla::LayerDocument::from_rasters({png}).cxdlpv4_native_job());
    const size_t parameters=layout.integer(layout.base+52),slicer=layout.integer(layout.base+72);
    auto patched=[&](size_t offset,uint32_t value,size_t length=4) {
        auto copy=bytes;
        for(size_t k=0;k<length;++k)copy[offset+k]=uint8_t(value>>(8*k));
        const uint32_t checksum=sla::creality_checksum(copy.data(),copy.size()-4,4);
        for(size_t k=0;k<4;++k)copy[copy.size()-4+k]=uint8_t(checksum>>(24-8*k));
        return copy;
    };
    auto bits=[](float value) {uint32_t result;std::memcpy(&result,&value,4);return result;};
    auto read_native=[](const std::vector<uint8_t> &data) {
        const sla::CrealityCXDLPv4Layout checked(data);
        return sla::cxdlpv4_native_globals(checked);
    };
    // Checksum-valid malformed metadata: additional coverage after actual RED.
    std::vector<std::pair<size_t,uint32_t>> hostile{
        {layout.base+52,0},{layout.base+72,0},
        {layout.base+56,67},{layout.base+76,75},
        {layout.base+52,uint32_t(bytes.size()+10)},{layout.base+72,uint32_t(bytes.size()+10)},
        {slicer+16,bits(100.f)}};
    for(float value:{0.f,-.05f,std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN()})
        hostile.emplace_back(layout.base+20,bits(value));
    for(size_t field:{parameters+44,parameters+48})
        for(float value:{-1.f,std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN()})
            hostile.emplace_back(field,bits(value));
    if(second)for(size_t field:{parameters,parameters+8})hostile.emplace_back(field,bits(1.f));
    for(const auto &[offset,value]:hostile)CHECK_THROWS(read_native(patched(offset,value)));
    for(size_t field:{layout.base+64,layout.base+66})CHECK_THROWS(read_native(patched(field,256,2)));
    // Read-only diagnostics retain native counts and flags independently.
    // This is not authorization to execute or re-export their settings.
    for(uint32_t count:{4u,70000u}) {
        CHECK(read_native(patched(layout.base+24,count)).bottom_layers==count);
        CHECK(read_native(patched(parameters+40,count)).parameter_bottom_layers==count);
        CHECK(read_native(patched(slicer+64,count)).transition_layers==count);
    }
    for(uint32_t flag:{0u,1u,2u,0xffffffffu})CHECK(read_native(patched(slicer+28,flag)).raw_per_layer_mode==flag);
    for(size_t i=0;i<3;++i) {
        const auto shadow=read_native(patched(slicer+52+4*i,bits(71.75f+float(i))));
        CHECK(shadow.normal==normal);CHECK(shadow.bottom==bottom);
        CHECK(shadow.mirrors[i]==71.75f+float(i));
    }
    auto zero=bytes;
    for(size_t field:{parameters+4,parameters+12,parameters+16,slicer+4,slicer+12,slicer+20}) {
        for(size_t k=0;k<4;++k)zero[field+k]=0;
    }
    const uint32_t checksum=sla::creality_checksum(zero.data(),zero.size()-4,4);
    for(size_t k=0;k<4;++k)zero[zero.size()-4+k]=uint8_t(checksum>>(24-8*k));
    const auto zero_native=read_native(zero);
    CHECK(zero_native.normal.at(F::LiftSpeed)==0.);CHECK(zero_native.bottom.at(F::LiftSpeed)==0.);
    CHECK(zero_native.normal.at(F::RetractSpeed)==0.);
    // Reading native globals cannot mutate mask edits, dirty state or history.
    auto edited=imported;edited.paint(0,2,2,0,255);
    const auto revision=edited.revision();const bool dirty=edited.dirty(),undo=edited.can_undo(),redo=edited.can_redo();
    const auto edited_snapshot=edited.cxdlpv4_native_job();
    REQUIRE(edited_snapshot);CHECK(edited_snapshot->normal==normal);CHECK(edited_snapshot->bottom==bottom);
    CHECK(edited.revision()==revision);CHECK(edited.dirty()==dirty);CHECK(edited.can_undo()==undo);CHECK(edited.can_redo()==redo);
    struct Project {boost::filesystem::path path=boost::filesystem::unique_path("cx-globals-project-%%%%.sla-project");
        ~Project(){boost::system::error_code e;boost::filesystem::remove(path,e);}} project;
    sla::ProjectRasterState state;state.document=edited;state.origin=sla::ProjectLayerOrigin::ImportedArchive;
    REQUIRE_NOTHROW(sla::save_layer_project(project.path.string(),state));
    const auto restored=sla::read_project_layers(project.path.string());REQUIRE(restored);
    const auto restart=restored->document.cxdlpv4_native_job();REQUIRE(restart);
    CHECK(restart->normal==normal);CHECK(restart->bottom==bottom);CHECK(restart->transition_layers==0);
    CHECK(restored->document.image(0).buf==edited.image(0).buf);
    CHECK(restored->document.parameter_capabilities().empty());CHECK_FALSE(restored->document.job_parameters());
}


TEST_CASE("CXDLPv4 emitted transition counts match decoded rows without changing exposure intent", "[sla_cxdlpv4_counts]") {
    struct Scenario {const char *name;size_t layers;int bottom,transition;double initial,normal;uint32_t expected;};
    const std::vector<Scenario> cases{
        {"no-bottom-fade",3,0,5,9.125,3.135,0},
        {"short-one-bottom",3,1,5,9.125,3.135,1},
        {"short-two-bottom",3,2,5,9.125,3.135,0},
        {"short-all-bottom",3,20,5,9.125,3.135,0},
        {"no-fade",8,2,0,9.125,3.135,0},
        {"one-fade",8,2,1,9.125,3.135,1},
        {"exact-maximum",8,1,6,9.125,3.135,6},
        {"over-maximum",8,1,20,9.125,3.135,6},
        {"short-over-maximum",3,1,20,9.125,3.135,1},
        {"normal-only",8,0,0,9.125,3.135,0},
        {"all-bottom",8,8,6,9.125,3.135,0},
        {"equal-exposure",8,1,6,5.,5.,6},
        {"increasing-exposure",8,1,6,3.135,9.125,6},
        {"rounded-later-plateau",8,1,6,3.17,3.145,1},
        {"rounded-first-plateau",8,1,20,3.17,3.145,6},
        {"fractional-one-fade",8,1,1,3.17,3.145,1}};
    const size_t index=GENERATE(size_t(0),size_t(1),size_t(2),size_t(3),size_t(4),size_t(5),size_t(6),size_t(7),
        size_t(8),size_t(9),size_t(10),size_t(11),size_t(12),size_t(13),size_t(14),size_t(15));
    const bool portrait=GENERATE(false,true);
    const bool direct=GENERATE(false,true);
    const auto &c=cases[index];CAPTURE(c.name,portrait,direct);
    DynamicPrintConfig config;config.apply(SLAFullPrintConfig{});
    config.option("printer_technology")->setInt(ptSLA);
    config.set("sla_archive_format","cxdlpv4");config.set("printer_model","CL-60");
    config.set("display_pixels_x",8);config.set("display_pixels_y",4);
    config.set("display_width",16.);config.set("display_height",4.);
    config.option("display_orientation")->setInt(portrait?sladoPortrait:sladoLandscape);
    config.set("sla_bottom_layers",c.bottom);config.set("sla_transition_layers",c.transition);
    config.set("initial_exposure_time",c.initial);config.set("exposure_time",c.normal);
    for(bool bottom:{false,true}) {
        const std::string prefix=bottom?"sla_bottom_":"sla_";
        config.set(prefix+"lift_height",3.);config.set(prefix+"lift_height2",2.);
        config.set(prefix+"lift_speed",60.);config.set(prefix+"lift_speed2",180.);
        config.set(prefix+"retract_speed",150.);config.set(prefix+"retract_height2",1.);config.set(prefix+"retract_speed2",240.);
        config.set(prefix+"wait_before_cure",.125);config.set(prefix+"wait_after_cure",.25);config.set(prefix+"wait_after_lift",.375);
    }
    std::vector<uint8_t> pixels(32,0);pixels[1]=255;pixels[20]=127;
    const auto png=sla::PNGRasterEncoder{}(pixels.data(),portrait?4:8,portrait?8:4,1);
    std::vector<sla::EncodedRaster> rasters(c.layers,png);
    std::vector<double> levels;for(size_t i=0;i<c.layers;++i)levels.push_back(.05*double(i+1));
    const auto document=sla::LayerDocument::from_rasters(std::move(rasters));
    const auto job=SLAPrint::create_raster_job(config,document,levels);
    struct Directory {boost::filesystem::path path=boost::filesystem::unique_path("cx-counts-%%%%");bool keep=false;
        ~Directory(){if(!keep){boost::system::error_code e;boost::filesystem::remove_all(path,e);}}} directory;
    if(const char *root=std::getenv("SLA_CXDLPV4_COUNTS_FIXTURES")) {
        directory.path=boost::filesystem::path(root)/(std::string(c.name)+(portrait?"-portrait":"-landscape")+(direct?"-direct":"-explicit"));directory.keep=true;
    }
    boost::filesystem::create_directories(directory.path);config.save((directory.path/"source.ini").string());
    const auto output=directory.path/"counts.cxdlpv4";
    if(direct)job->export_print(output.string());else job->export_layer_document(document,"cxdlpv4",output.string());
    std::ifstream stream(output.string(),std::ios::binary);const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(stream)),{});
    const sla::CrealityCXDLPv4Layout layout(bytes);const auto snapshot=sla::cxdlpv4_native_globals(layout);
    CHECK(snapshot.bottom_layers==std::min(uint32_t(c.bottom),uint32_t(c.layers)));
    CHECK(snapshot.parameter_bottom_layers==snapshot.bottom_layers);
    CHECK(snapshot.transition_layers==c.expected);
    for(size_t i=0;i<c.layers;++i) {
        double exposure=c.normal;
        if(i<size_t(c.bottom))exposure=c.initial;
        else if(i-size_t(c.bottom)<size_t(c.transition))
            exposure=c.initial+(c.normal-c.initial)*double(i-size_t(c.bottom)+1)/double(c.transition+1);
        CHECK(sla::cxdlpv4_layer_parameters(layout,i).at(sla::LayerParameter::Exposure)==double(float(exposure)));
        CHECK(sla::cxdlpv4_layer_parameters(layout,i).at(sla::LayerParameter::PositionZ)==double(float(levels[i])));
    }
    const auto reopened=sla::LayerDocument::load_archive(output.string());
    for(size_t i=0;i<c.layers;++i)CHECK(reopened.image(i).buf==pixels);
    CHECK(job->full_print_config().opt_int("sla_bottom_layers")==c.bottom);
    CHECK(job->full_print_config().opt_int("sla_transition_layers")==c.transition);
}

TEST_CASE("CXDLPv4 generated counts reject reference integer overflow", "[sla_cxdlpv4_counts]") {
    auto equal=[](size_t){return 5.f;};
    auto falling=[](size_t i){return float(100000-i);};
    CHECK_THROWS_AS(sla::cxdlpv4_generated_counts(0,0,0,equal),ExportError);
    CHECK_THROWS_AS(sla::cxdlpv4_generated_counts(100001,0,0,equal),ExportError);
    CHECK_THROWS_AS(sla::cxdlpv4_generated_counts(3,-1,0,equal),ExportError);
    CHECK_THROWS_AS(sla::cxdlpv4_generated_counts(3,1,-1,equal),ExportError);
    CHECK_THROWS_AS(sla::cxdlpv4_generated_counts(70001,70000,0,equal),ExportError);
    CHECK_THROWS_AS(sla::cxdlpv4_generated_counts(65538,1,65536,falling),ExportError);
    CHECK_THROWS_AS(sla::cxdlpv4_generated_counts(70000,1,70000,equal),ExportError);
    CHECK_THROWS_AS(sla::cxdlpv4_generated_counts(3,1,1,[](size_t){return std::numeric_limits<float>::infinity();}),ExportError);
    const auto largest=sla::cxdlpv4_generated_counts(65537,1,65535,falling);
    CHECK(largest.bottom==1);CHECK(largest.transition==65535);
    const auto all_bottom=sla::cxdlpv4_generated_counts(65535,65535,70000,equal);
    CHECK(all_bottom.bottom==65535);CHECK(all_bottom.transition==0);
    const auto clipped_bottom=sla::cxdlpv4_generated_counts(3,70000,70000,equal);
    CHECK(clipped_bottom.bottom==3);CHECK(clipped_bottom.transition==0);
    CHECK(sla::cxdlpv4_generated_counts(3,1,70000,equal).transition==1);
    CHECK(sla::cxdlpv4_generated_counts(70000,0,70000,equal).transition==0);
}

TEST_CASE("CXDLPv4 invalid generated counts preserve existing output", "[sla_cxdlpv4_counts]") {
    const bool direct=GENERATE(false,true),invalid_bottom=GENERATE(false,true);
    DynamicPrintConfig config;config.apply(SLAFullPrintConfig{});
    config.option("printer_technology")->setInt(ptSLA);
    config.set("sla_archive_format","cxdlpv4");config.set("printer_model","CL-60");
    config.set("display_pixels_x",8);config.set("display_pixels_y",4);
    config.set("display_width",16.);config.set("display_height",4.);
    config.set("sla_bottom_layers",invalid_bottom?-1:1);
    config.set("sla_transition_layers",invalid_bottom?1:-1);
    std::vector<uint8_t> pixels(32,0);pixels[1]=255;pixels[20]=127;
    const auto png=sla::PNGRasterEncoder{}(pixels.data(),8,4,1);
    const auto document=sla::LayerDocument::from_rasters({png,png,png});
    struct File {boost::filesystem::path path=boost::filesystem::unique_path("cx-count-refusal-%%%%.cxdlpv4");
        ~File(){boost::system::error_code e;boost::filesystem::remove(path,e);}} file;
    {std::ofstream out(file.path.string(),std::ios::binary);out<<"protected previous output";}
    // Invalid presets can be rejected by raster-job construction already;
    // either public workflow must leave an existing destination untouched.
    CHECK_THROWS([&] {
        const auto job=SLAPrint::create_raster_job(config,document,{.05,.1,.15});
        if(direct)job->export_print(file.path.string());
        else job->export_layer_document(document,"cxdlpv4",file.path.string());
    }());
    std::ifstream input(file.path.string(),std::ios::binary);
    CHECK(std::string((std::istreambuf_iterator<char>(input)),{})=="protected previous output");
}

TEST_CASE("CXDLPv4 reference bottom settings follow meaningful current masks", "[sla_cxdlpv4_effective]") {
    using F=sla::LayerParameter;
    const bool portrait=GENERATE(false,true);
    const int scenario=GENERATE(0,1,2,3);
    const uint32_t flag=GENERATE(0u,1u,2u,0xffffffffu);
    CAPTURE(portrait,scenario,flag);
    DynamicPrintConfig config;config.apply(SLAFullPrintConfig{});
    config.option("printer_technology")->setInt(ptSLA);
    config.set("sla_archive_format","cxdlpv4");config.set("printer_model","CL-60");
    config.set("display_pixels_x",8);config.set("display_pixels_y",4);
    config.set("display_width",16.);config.set("display_height",4.);
    config.option("display_orientation")->setInt(portrait?sladoPortrait:sladoLandscape);
    config.set("sla_bottom_layers",scenario==3?0:2);config.set("sla_transition_layers",0);
    config.set("initial_exposure_time",9.125);config.set("exposure_time",3.135);
    config.set("sla_bottom_lift_height",3.125);config.set("sla_bottom_lift_height2",2.125);
    std::vector<uint8_t> pixels(32,0);pixels[1]=255;pixels[20]=127;
    const auto png=sla::PNGRasterEncoder{}(pixels.data(),portrait?4:8,portrait?8:4,1);
    const auto job=SLAPrint::create_raster_job(config,sla::LayerDocument::from_rasters({png,png,png}),{.05,.1,.15});
    struct File {boost::filesystem::path path=boost::filesystem::unique_path("cx-effective-%%%%.cxdlpv4");bool keep=false;
        ~File(){if(!keep){boost::system::error_code e;boost::filesystem::remove(path,e);}}} file;
    if(const char *root=std::getenv("SLA_CXDLPV4_EFFECTIVE_FIXTURES")) {
        boost::filesystem::create_directories(root);file.path=boost::filesystem::path(root)/
            (std::string(portrait?"portrait":"landscape")+"-scenario"+std::to_string(scenario)+"-flag"+std::to_string(flag)+".cxdlpv4");file.keep=true;
    }
    job->export_print(file.path.string());
    std::ifstream stream(file.path.string(),std::ios::binary);std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(stream)),{});
    const sla::CrealityCXDLPv4Layout layout(bytes);
    auto put=[&](size_t offset,uint32_t value){for(size_t k=0;k<4;++k)bytes[offset+k]=uint8_t(value>>(8*k));};
    auto number=[&](size_t offset,float value){uint32_t bits;std::memcpy(&bits,&value,4);put(offset,bits);};
    put(layout.integer(layout.base+72)+28,flag);
    // Distinct authored native rows are diagnostic fixtures until row editing
    // is enabled. The source writer and its original output remain separate.
    for(size_t i=0;i<2;++i) {
        const size_t ex=layout.layers[i].data;
        if(i==1)number(ex,10.f); // row lift can differ from native global lift
        number(ex+16,i?123.456f:.004f);number(ex+20,i?9.f:1.125f);number(ex+24,i?234.567f:.004f);
        number(ex+36,i?1.235f:.125f);number(ex+28,i?2.345f:.345f);number(ex+32,i?3.455f:.455f);
        if(scenario==2 || (scenario==1&&i==0))number(layout.table+40*i+4,.014f);
    }
    const uint32_t checksum=sla::creality_checksum(bytes.data(),bytes.size()-4,4);
    for(size_t k=0;k<4;++k)bytes[bytes.size()-4+k]=uint8_t(checksum>>(24-8*k));
    {std::ofstream out(file.path.string(),std::ios::binary|std::ios::trunc);out.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());}
    auto document=sla::LayerDocument::load_archive(file.path.string());
    const auto native=*document.cxdlpv4_native_job();
    const auto before_revision=document.revision();
    const auto view=document.cxdlpv4_reference_bottom();REQUIRE(view);
    auto save_view=[&](const boost::filesystem::path &path,const sla::LayerDocument &doc) {
        const auto value=doc.cxdlpv4_reference_bottom();REQUIRE(value);
        nlohmann::json json;
        json["meaningful_bottom_layer"]=value->meaningful_bottom_layer ? nlohmann::json(*value->meaningful_bottom_layer):nlohmann::json(nullptr);
        json["decoded_bottom_layers"]=value->decoded_bottom_layers;
        json["native_bottom_layers"]=value->native.bottom_layers;json["native_flags"]=value->native.raw_per_layer_mode;
        for(const auto &[name,field]:std::map<std::string,F>{{"BottomRetractSpeed",F::RetractSpeed},{"BottomRetractHeight2",F::RetractDistance2},
            {"BottomRetractSpeed2",F::RetractSpeed2},{"BottomWaitTimeBeforeCure",F::WaitBefore},{"BottomWaitTimeAfterCure",F::WaitAfter},
            {"BottomWaitTimeAfterLift",F::WaitAfterLift}})json["virtual_bottom"][name]=value->virtual_bottom.at(field);
        for(const auto &[name,field]:std::map<std::string,F>{{"Exposure",F::Exposure},{"LightOff",F::LightOff},
            {"LiftDistance",F::LiftDistance},{"LiftDistance2",F::LiftDistance2},{"LiftSpeed",F::LiftSpeed},
            {"LiftSpeed2",F::LiftSpeed2},{"LightPWM",F::LightPWM}})json["native_bottom7"][name]=value->native.bottom.at(field);
        std::ofstream out(path.string()+".view.json");out<<json.dump(2)<<"\n";
    };
    if(file.keep)save_view(file.path,document);
    CHECK(view->native.bottom==native.bottom);CHECK(view->native.normal==native.normal);
    CHECK(view->native.raw_per_layer_mode==flag);CHECK(view->decoded_bottom_layers==(scenario==3?0:2));
    CHECK(view->native.bottom.size()==7);CHECK(view->virtual_bottom.size()==6);
    const std::optional<size_t> expected=scenario==0?std::optional<size_t>(0):scenario==1?std::optional<size_t>(1):std::nullopt;
    CHECK(view->meaningful_bottom_layer==expected);
    const sla::LayerParameters row0{{F::RetractSpeed,100.},{F::RetractDistance2,double(float(1.12f))},{F::RetractSpeed2,80.},
        {F::WaitBefore,double(float(.12f))},{F::WaitAfter,double(float(.34f))},{F::WaitAfterLift,double(float(.46f))}};
    const sla::LayerParameters row1{{F::RetractSpeed,double(float(123.46f))},{F::RetractDistance2,double(float(5.24f))},{F::RetractSpeed2,double(float(234.57f))},
        {F::WaitBefore,double(float(1.24f))},{F::WaitAfter,double(float(2.34f))},{F::WaitAfterLift,double(float(3.46f))}};
    if(expected)CHECK(view->virtual_bottom==(*expected==0?row0:row1));
    else for(const auto &[field,value]:view->virtual_bottom)CHECK(value==0.);
    CHECK(document.revision()==before_revision);CHECK_FALSE(document.dirty());CHECK_FALSE(document.can_undo());
    CHECK_THROWS_AS(document.cxdlpv4_reference_bottom([](size_t,size_t){return false;}),sla::LayerOperationCancelled);
    CHECK_FALSE(sla::LayerDocument::from_rasters({png}).cxdlpv4_reference_bottom());
    if(scenario==1) {
        CHECK_THROWS_AS(document.cxdlpv4_reference_bottom([](size_t done,size_t){return done<1;}),sla::LayerOperationCancelled);
        CHECK(document.revision()==before_revision);CHECK_FALSE(document.dirty());
        CHECK(document.cxdlpv4_reference_bottom()->meaningful_bottom_layer==expected);
    }
    if(scenario==0) {
        document.paint(0,1,0,0,0); // exactly one nonzero pixel: dummy
        CHECK(document.cxdlpv4_reference_bottom()->meaningful_bottom_layer==std::optional<size_t>(1));
        CHECK(document.cxdlpv4_reference_bottom()->virtual_bottom==row1);
        document.paint(1,1,0,0,0);
        CHECK_FALSE(document.cxdlpv4_reference_bottom()->meaningful_bottom_layer);
        REQUIRE(document.undo());CHECK(document.cxdlpv4_reference_bottom()->meaningful_bottom_layer==std::optional<size_t>(1));
        REQUIRE(document.undo());CHECK(document.cxdlpv4_reference_bottom()->meaningful_bottom_layer==std::optional<size_t>(0));
        REQUIRE(document.redo());CHECK(document.cxdlpv4_reference_bottom()->meaningful_bottom_layer==std::optional<size_t>(1));
        CHECK(document.cxdlpv4_native_job()->bottom==native.bottom);
        struct Project {boost::filesystem::path path=boost::filesystem::unique_path("cx-effective-project-%%%%.sla-project");
            ~Project(){boost::system::error_code e;boost::filesystem::remove(path,e);}} project;
        sla::ProjectRasterState state;state.document=document;state.origin=sla::ProjectLayerOrigin::ImportedArchive;
        sla::save_layer_project(project.path.string(),state);
        const auto restored=sla::read_project_layers(project.path.string());REQUIRE(restored);
        CHECK(restored->document.cxdlpv4_reference_bottom()->meaningful_bottom_layer==std::optional<size_t>(1));
        CHECK(restored->document.cxdlpv4_reference_bottom()->virtual_bottom==row1);
        CHECK(restored->document.cxdlpv4_native_job()->bottom==native.bottom);
        CHECK(restored->document.parameter_capabilities().empty());CHECK_FALSE(restored->document.job_parameters());
        if(file.keep) {
            const auto edited=file.path.parent_path()/(file.path.stem().string()+".edited.cxdlpv4");
            document.save_archive(edited.string());save_view(edited,document);
            const auto readback=sla::LayerDocument::load_archive(edited.string());
            CHECK(readback.cxdlpv4_reference_bottom()->virtual_bottom==row1);
            CHECK(readback.cxdlpv4_reference_bottom()->meaningful_bottom_layer==std::optional<size_t>(1));
        }
        auto grey=document;auto mask=grey.image(0);std::fill(mask.buf.begin(),mask.buf.end(),0);
        mask.buf[0]=1;mask.buf[1]=1;grey.replace_image(0,mask);
        CHECK(grey.cxdlpv4_reference_bottom()->meaningful_bottom_layer==std::optional<size_t>(1));
        // CX's native seven-bit alphabet quantizes 1 to zero. The weakest
        // representable grayscale 3 still counts as a lit pixel.
        mask.buf[0]=3;mask.buf[1]=3;grey.replace_image(0,mask);
        CHECK(grey.cxdlpv4_reference_bottom()->meaningful_bottom_layer==std::optional<size_t>(0));
        CHECK(document.cxdlpv4_reference_bottom()->meaningful_bottom_layer==std::optional<size_t>(1));
        File diagnostic;
        put(layout.base+24,70000u);number(layout.integer(layout.base+72)+60,700.25f);
        const uint32_t diagnostic_crc=sla::creality_checksum(bytes.data(),bytes.size()-4,4);
        for(size_t k=0;k<4;++k)bytes[bytes.size()-4+k]=uint8_t(diagnostic_crc>>(24-8*k));
        {std::ofstream out(diagnostic.path.string(),std::ios::binary);out.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());}
        const auto high=sla::LayerDocument::load_archive(diagnostic.path.string()).cxdlpv4_reference_bottom();REQUIRE(high);
        CHECK(high->native.bottom_layers==70000u);CHECK(high->decoded_bottom_layers==4464u);
        CHECK(high->native.parameter_bottom_layers==2);CHECK(high->native.mirrors[2]==700.25f);
        CHECK(high->meaningful_bottom_layer==std::optional<size_t>(0));CHECK(high->virtual_bottom==row0);
    }
}

TEST_CASE("CXDLPv4 public projection keeps large finite floats and round ties", "[sla_cxdlpv4_effective]") {
    CHECK(sla::cxdlpv4_public_round2(.125f)==.12f);
    CHECK(sla::cxdlpv4_public_round2(.135f)==.14f);
    CHECK(sla::cxdlpv4_public_round2(-.125f)==-.12f);
    CHECK(sla::cxdlpv4_public_round2(1e8f)==1e8f);
    CHECK(sla::cxdlpv4_public_round2(std::numeric_limits<float>::max())==std::numeric_limits<float>::max());
    CHECK_THROWS_AS(sla::cxdlpv4_public_round2(std::numeric_limits<float>::infinity()),RuntimeError);
    CHECK_THROWS_AS(sla::cxdlpv4_public_round2(std::numeric_limits<float>::quiet_NaN()),RuntimeError);
}
