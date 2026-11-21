// SPDX-License-Identifier: AGPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <limits>
#include <algorithm>
#include <boost/filesystem.hpp>
#include <fstream>
#include <cstdlib>
#include <nlohmann/json.hpp>
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/FileReader.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Format/3mf.hpp"
#include "libslic3r/SLA/ProjectArchive.hpp"
#include <cmath>
#include "libslic3r/SLA/StatisticsPresentation.hpp"
#include "libslic3r/miniz_extension.hpp"
using namespace Slic3r;
namespace {
SLAPrintStatistics presentation_statistics() {
    SLAPrintStatistics s;
    s.time_basis=SLAPrintStatistics::TimeBasis::OwnedNeutralSchedule;
    s.raster_area_mm2=64.;s.objects_used_material=.7;s.support_used_material=0.;
    s.total_cost=.00014;s.total_weight=.00105;s.estimated_print_time=25.;
    s.layers_areas={4.,6.,2.};s.layers_times_running_total={8.5,16.25,25.};
    return s;
}
SLAMaterialConfig presentation_material() {
    SLAMaterialConfig m;m.bottle_cost.value=100.;m.bottle_volume.value=500.;m.material_density.value=1.5;
    return m;
}
}
TEST_CASE("Statistics time presentation preserves double precision and bounds", "[sla_statistics_presentation]") {
    CHECK(sla::statistics_time(0.)=="0s");
    CHECK(sla::statistics_time(60.)=="1m 0s");
    CHECK(sla::statistics_time(3600.)=="1h 0m 0s");
    CHECK(sla::statistics_time(86400.)=="1d 0h 0m 0s");
    CHECK(sla::statistics_time(16777217.)=="194d 4h 20m 17s");
    CHECK(sla::statistics_time(59.)=="59s");
    CHECK(sla::statistics_time(61.,true)=="1m");
    CHECK(sla::statistics_time_is_dhms(0.));
    CHECK(sla::statistics_time_is_dhms(1e12));
    CHECK_FALSE(sla::statistics_time_is_dhms(1e12+1.));
    const auto huge=sla::statistics_time(1e300);
    CHECK(huge!="N/A");CHECK(huge.find('e')!=std::string::npos);
    CHECK(huge.find('s')!=std::string::npos);
    CHECK(std::stod(huge)==Catch::Approx(1e300).epsilon(1e-14));
    auto statistics=presentation_statistics();
    statistics.estimated_print_time=1e300;
    CHECK(statistics.config().opt_string("print_time")==sla::statistics_time(1e300,true));
    statistics.estimated_print_time=std::numeric_limits<double>::infinity();
    CHECK(statistics.config().opt_string("print_time")=="N/A");
}
TEST_CASE("Invalid time and quantity presentation is safe", "[sla_statistics_presentation]") {
    const double value=GENERATE(-1.,std::numeric_limits<double>::quiet_NaN(),
        std::numeric_limits<double>::infinity(),-std::numeric_limits<double>::infinity());
    CHECK_FALSE(sla::statistics_time_is_dhms(value));
    CHECK(sla::statistics_time(value)=="N/A");
    CHECK(sla::statistics_time(value,true)=="N/A");
    CHECK(sla::statistics_number(value,2)=="N/A");
}
TEST_CASE("Tiny positive statistics retain four significant scientific digits", "[sla_statistics_presentation]") {
    CHECK(sla::statistics_number(0.,2)=="0.00");
    CHECK(sla::statistics_number(1.25,2)=="1.25");
    CHECK(sla::statistics_number(.00014,3)=="1.400e-04");
    CHECK(sla::statistics_number(1e-300,2)=="1.000e-300");
    CHECK(sla::statistics_number(1e300,2)!="N/A");
}
TEST_CASE("Statistics cost uses owned result and preserves legacy context calculation", "[sla_statistics_presentation]") {
    auto s=presentation_statistics();auto material=presentation_material();
    material.bottle_cost.value=900.;material.bottle_volume.value=100.;
    const auto owned=sla::statistics_cost(s,material);REQUIRE(owned);
    CHECK(*owned==s.total_cost);
    s.time_basis=SLAPrintStatistics::TimeBasis::LegacyProcess;
    s.support_used_material=.3;
    const auto legacy=sla::statistics_cost(s,material);REQUIRE(legacy);
    CHECK(*legacy==Catch::Approx(.009).epsilon(0).margin(1e-14));
    material.bottle_volume.value=0.;CHECK_FALSE(sla::statistics_cost(s,material));
    material=presentation_material();material.bottle_cost.value=-1.;CHECK_FALSE(sla::statistics_cost(s,material));
    material=presentation_material();material.bottle_volume.value=std::numeric_limits<double>::infinity();
    CHECK_FALSE(sla::statistics_cost(s,material));
    s=presentation_statistics();material=presentation_material();s.total_cost=std::numeric_limits<double>::infinity();
    CHECK_FALSE(sla::statistics_cost(s,material));
    s.total_cost=-1.;CHECK_FALSE(sla::statistics_cost(s,material));
}
TEST_CASE("Layer fill uses owned screen area and refuses invalid fresh geometry", "[sla_statistics_presentation]") {
    auto s=presentation_statistics();
    const auto owned=sla::statistics_layer_fill(s,32.,1);REQUIRE(owned);CHECK(*owned==9.375);
    s.time_basis=SLAPrintStatistics::TimeBasis::LegacyProcess;
    const auto legacy=sla::statistics_layer_fill(s,32.,1);REQUIRE(legacy);CHECK(*legacy==18.75);
    s.time_basis=SLAPrintStatistics::TimeBasis::OwnedNeutralSchedule;
    s.raster_area_mm2=0.;CHECK_FALSE(sla::statistics_layer_fill(s,32.,1));
    s.raster_area_mm2=std::numeric_limits<double>::infinity();CHECK_FALSE(sla::statistics_layer_fill(s,32.,1));
    s=presentation_statistics();s.layers_areas[1]=-1.;CHECK_FALSE(sla::statistics_layer_fill(s,32.,1));
    s=presentation_statistics();s.layers_areas[1]=std::numeric_limits<double>::quiet_NaN();
    CHECK_FALSE(sla::statistics_layer_fill(s,32.,1));
    s=presentation_statistics();CHECK_FALSE(sla::statistics_layer_fill(s,32.,3));
}
TEST_CASE("Layer times validate graph cardinality and selected monotonic interval", "[sla_statistics_presentation]") {
    auto s=presentation_statistics();
    const auto first=sla::statistics_layer_time(s,0),middle=sla::statistics_layer_time(s,1),last=sla::statistics_layer_time(s,2);
    REQUIRE(first);REQUIRE(middle);REQUIRE(last);
    CHECK(*first==8.5);CHECK(*middle==7.75);CHECK(*last==8.75);
    CHECK_FALSE(sla::statistics_layer_time(s,3));
    s.layers_times_running_total.pop_back();CHECK_FALSE(sla::statistics_layer_time(s,0));
    s=presentation_statistics();s.layers_times_running_total[2]=15.;CHECK_FALSE(sla::statistics_layer_time(s,2));
    s=presentation_statistics();s.layers_times_running_total[1]=-1.;CHECK_FALSE(sla::statistics_layer_time(s,1));
    s=presentation_statistics();s.layers_times_running_total[2]=std::numeric_limits<double>::infinity();
    CHECK_FALSE(sla::statistics_layer_time(s,2));
    s=presentation_statistics();s.layers_times_running_total[1]=std::numeric_limits<double>::quiet_NaN();
    CHECK_FALSE(sla::statistics_layer_time(s,1));
}

TEST_CASE("Owned model statistics fixtures survive queued 3mf restoration", "[sla_statistics_model_fixture]") {
    struct Directory {
        boost::filesystem::path path=boost::filesystem::temp_directory_path()/boost::filesystem::unique_path("sla-statistics-model-%%%%");
        bool keep=false;
        ~Directory(){if(!keep){boost::system::error_code error;boost::filesystem::remove_all(path,error);}}
    } files;
    if(const char *root=std::getenv("SLA_STATISTICS_UI_FIXTURES")) {files.path=root;files.keep=true;}
    boost::filesystem::create_directories(files.path);
    Model model;auto *object=model.add_object("Owned statistics cube","",make_cube(4.,3.,.15));
    object->add_instance()->set_offset(Vec3d(10.,10.,0.));
    SLAFullPrintConfig full;full.printer_technology.setInt(ptSLA);
    full.set("sla_archive_format",std::string("SL1"));full.set("printer_model",std::string("SL1"));
    full.set("display_width",100.);full.set("display_height",60.);
    full.set("display_pixels_x",200);full.set("display_pixels_y",120);
    full.display_orientation.setInt(sladoLandscape);
    full.set("supports_enable",false);full.set("pad_enable",false);full.set("support_object_elevation",0.);
    full.set("layer_height",.05);full.set("initial_layer_height",.05);
    full.set("exposure_time",10.);full.set("initial_exposure_time",20.);
    full.set("sla_bottom_layers",2);full.set("sla_transition_layers",0);
    full.set("material_density",1.5);full.set("bottle_cost",100.);full.set("bottle_volume",500.);
    DynamicPrintConfig config;config.apply(full);
    SLAPrint print;print.set_status_callback([](const PrintBase::SlicingStatus &){});
    print.apply(model,config);print.process();REQUIRE(print.finished());REQUIRE(print.layer_document());
    REQUIRE(print.layer_document()->layer_count()>=2);
    const auto legacy_statistics=print.print_statistics();
    const auto legacy_path=(files.path/"legacy-model.3mf").string();
    REQUIRE(store_3mf(legacy_path.c_str(),&model,&config,false));
    config.save((files.path/"settings.ini").string());
    auto descriptor=print.make_fresh_layer_job(sla::FreshLayerProducer::ModelSlice);
    using F=sla::LayerParameter;
    descriptor.globals.bottom_layers=1;descriptor.globals.transition_layers=0;
    descriptor.globals.normal={{F::Exposure,3.},{F::LiftDistance,1.},{F::LiftSpeed,60.},
        {F::LiftDistance2,2.},{F::LiftSpeed2,120.},{F::RetractDistance2,.5},
        {F::RetractSpeed2,120.},{F::RetractSpeed,150.},{F::WaitBefore,.25},
        {F::WaitAfter,.5},{F::WaitAfterLift,.75},{F::LightOff,0.},{F::LightPWM,255.}};
    descriptor.globals.bottom=descriptor.globals.normal;
    descriptor.globals.bottom[F::Exposure]=2.;descriptor.globals.bottom[F::LightOff]=5.;
    std::vector<sla::EncodedRaster> rasters;
    std::vector<double> actual_z,areas,cumulative;
    std::vector<size_t> lit_pixels;
    double volume=0.,previous_z=0.,total_time=0.;
    for(size_t i=0;i<print.layer_document()->layer_count();++i) {
        const auto encoded=print.layer_document()->encoded_layer(i);
        const auto *bytes=static_cast<const uint8_t *>(encoded.buf);
        rasters.emplace_back(std::vector<uint8_t>(bytes,bytes+encoded.sz),"png");
        const auto pixels=print.layer_document()->image(i);
        const size_t lit=size_t(std::count_if(pixels.buf.begin(),pixels.buf.end(),[](uint8_t value){return value!=0;}));
        const double z=descriptor.rows[i].at(F::PositionZ)+4e-7;
        auto row=i?descriptor.globals.normal:descriptor.globals.bottom;
        row[F::Exposure]=double(i+2);row[F::LightOff]=i?0.:5.;row[F::PositionZ]=z;
        descriptor.rows[i]=row;
        const double area=double(lit)*.25; // Literal100x60mm /200x120pixels.
        volume+=area*(z-previous_z);previous_z=z;
        total_time+=i==0?8.5:double(i)+6.75; // Independent explicit row formula, no presentation helper.
        actual_z.push_back(z);areas.push_back(area);cumulative.push_back(total_time);lit_pixels.push_back(lit);
    }
    const auto document=sla::LayerDocument::from_fresh_rasters(std::move(rasters),descriptor);
    SECTION("Generated project capture retains exact owned Z too") {
        auto generated_descriptor=descriptor;
        generated_descriptor.producer=sla::FreshLayerProducer::Raster;
        std::vector<sla::EncodedRaster> generated_masks;
        for(size_t i=0;i<document.layer_count();++i) {
            const auto encoded=document.encoded_layer(i);
            const auto *bytes=static_cast<const uint8_t *>(encoded.buf);
            generated_masks.emplace_back(std::vector<uint8_t>(bytes,bytes+encoded.sz),"png");
        }
        const auto generated_document=sla::LayerDocument::from_fresh_rasters(std::move(generated_masks),generated_descriptor);
        const auto generated=SLAPrint::create_raster_job(config,generated_document,actual_z);
        const auto captured=sla::capture_generated_project(*generated,generated_document);
        CHECK(captured.positions_z==actual_z);
    }
    print.replace_layer_document(document);
    CHECK(print.print_statistics().estimated_print_time==total_time);
    CHECK(print.print_statistics().objects_used_material==Catch::Approx(volume).epsilon(0).margin(1e-12));
    CHECK(print.print_statistics().time_basis==SLAPrintStatistics::TimeBasis::OwnedNeutralSchedule);
    CHECK(print.print_statistics().estimated_print_time!=legacy_statistics.estimated_print_time);
    // Public capture must retain owned actualZ rather than the engine grid.
    const auto state=sla::capture_project_layers(model,config,print);
    CHECK(state.positions_z==actual_z);
    const auto owned_path=(files.path/"owned-model.3mf").string();
    REQUIRE(store_3mf(owned_path.c_str(),&model,&config,false,nullptr,true,&state));
    const auto saved=sla::read_project_layers(owned_path);REQUIRE(saved);
    CHECK(saved->document.fresh_job()->rows==descriptor.rows);
    CHECK(saved->positions_z==actual_z);
    Model loaded;DynamicPrintConfig loaded_config;
    ConfigSubstitutionContext substitutions{ForwardCompatibilitySubstitutionRule::Enable};
    boost::optional<Semver> version;
    REQUIRE(load_3mf(owned_path.c_str(),loaded_config,substitutions,&loaded,false,version));
    SLAPrint restored;restored.set_status_callback([](const PrintBase::SlicingStatus &){});
    restored.apply(loaded,loaded_config);
    restored.queue_project_layers(std::make_shared<const sla::ProjectRasterState>(*saved));
    restored.process();
    REQUIRE(restored.layer_document()->fresh_job());
    CHECK(restored.layer_document()->fresh_job()->rows==descriptor.rows);
    CHECK(restored.print_statistics().estimated_print_time==total_time);
    CHECK(restored.print_statistics().objects_used_material==Catch::Approx(volume).epsilon(0).margin(1e-12));
    CHECK(restored.print_statistics().time_basis==SLAPrintStatistics::TimeBasis::OwnedNeutralSchedule);
    // A selected codec can refuse this executable schedule, but the source
    // document remains valid and must be safe to inspect in the real GUI.
    auto huge_descriptor=descriptor;
    std::vector<sla::EncodedRaster> huge_masks;
    for(size_t i=0;i<document.layer_count();++i) {
        huge_descriptor.rows[i][F::Exposure]=1e300;
        const auto encoded=document.encoded_layer(i);
        const auto *bytes=static_cast<const uint8_t *>(encoded.buf);
        huge_masks.emplace_back(std::vector<uint8_t>(bytes,bytes+encoded.sz),"png");
    }
    const auto huge_document=sla::LayerDocument::from_fresh_rasters(std::move(huge_masks),huge_descriptor);
    print.replace_layer_document(huge_document);
    const auto huge_state=sla::capture_project_layers(model,config,print);
    const auto huge_path=(files.path/"huge-model.3mf").string();
    REQUIRE(store_3mf(huge_path.c_str(),&model,&config,false,nullptr,true,&huge_state));
    const auto huge_saved=sla::read_project_layers(huge_path);REQUIRE(huge_saved);
    CHECK(huge_saved->document.fresh_job()->rows==huge_descriptor.rows);
    CHECK(std::isfinite(print.print_statistics().estimated_print_time));
    const double ml=volume/1000.,cost=ml*100./500.,weight=ml*1.5;
    nlohmann::json expected{{"owned_project",owned_path},{"legacy_project",legacy_path},
        {"settings",(files.path/"settings.ini").string()},{"layers",actual_z.size()},
        {"raster_area_mm2",6000.},{"pixel_area_mm2",.25},{"lit_pixels",lit_pixels},
        {"positions_z",actual_z},{"layers_areas",areas},{"layers_times_running_total",cumulative},
        {"estimated_print_time",total_time},{"objects_used_material",volume},{"material_ml",ml},
        {"total_cost",cost},{"total_weight",weight},{"bottom_layers",1},
        {"normal_layers",actual_z.size()-1},{"legacy_time_diagnostic",legacy_statistics.estimated_print_time},
        {"source_precision_offset",4e-7},{"time_basis","OwnedNeutralSchedule"},
        {"legacy_material_ml",(legacy_statistics.objects_used_material+legacy_statistics.support_used_material)/1000.},
        {"legacy_cost",(legacy_statistics.objects_used_material+legacy_statistics.support_used_material)/1000.*100./500.}};
    expected["legacy_volume_mm3"]=legacy_statistics.objects_used_material+legacy_statistics.support_used_material;
    expected["huge_project"]=huge_path;
    expected["huge_estimated_print_time"]=double(actual_z.size())*1e300;
    std::vector<double> huge_times;
    for(size_t i=0;i<actual_z.size();++i)huge_times.push_back(double(i+1)*1e300);
    expected["huge_layers_times_running_total"]=huge_times;
    std::ofstream((files.path/"expected.json").string())<<expected.dump(2)<<"\n";
}

namespace {
// The PrinterTechnology enum has no FFF value any more: a legacy FFF fixture is authored by rewriting the raw
// "printer_technology = SLA" line (config text of the archive / ini) to "FFF", as an old PrusaSlicer wrote it.
void rewrite_technology_to_fff_text(std::string &text)
{
  const std::string from="printer_technology = SLA";
  const auto pos=text.find(from);
  REQUIRE(pos!=std::string::npos);
  text.replace(pos,from.size(),"printer_technology = FFF");
}

// The FFF option definitions no longer exist, so the FFF settings of the legacy fixture are authored as raw text,
// exactly as an old PrusaSlicer wrote them ("; key = value" lines inside a 3MF config, "key = value" in an ini).
void append_legacy_fff_keys(std::string &text,const char *prefix)
{
  if(!text.empty() && text.back()!='\n')text+='\n';
  for(const char *line:{"nozzle_diameter = 0.6","filament_diameter = 1.75","temperature = 217","bed_temperature = 63",
      "print_settings_id = QA legacy FFF process 0.2","filament_settings_id = QA legacy FFF material 1.75"})
    text+=std::string(prefix)+line+"\n";
}

void rewrite_3mf_technology_to_fff(const std::string &path)
{
  std::vector<std::pair<std::string,std::string>> entries;
  mz_zip_archive zip;mz_zip_zero_struct(&zip);
  REQUIRE(open_zip_reader(&zip,path));
  for(mz_uint i=0;i<mz_zip_reader_get_num_files(&zip);++i) {
    mz_zip_archive_file_stat stat;REQUIRE(mz_zip_reader_file_stat(&zip,i,&stat));
    if(mz_zip_reader_is_file_a_directory(&zip,i))continue;
    std::string data(size_t(stat.m_uncomp_size),'\0');
    REQUIRE(mz_zip_reader_extract_to_mem(&zip,i,data.data(),data.size(),0));
    if(std::string(stat.m_filename)=="Metadata/Slic3r_PE.config"){rewrite_technology_to_fff_text(data);append_legacy_fff_keys(data,"; ");}
    entries.emplace_back(stat.m_filename,std::move(data));
  }
  close_zip_reader(&zip);
  mz_zip_archive out;mz_zip_zero_struct(&out);
  REQUIRE(open_zip_writer(&out,path));
  for(const auto &[name,data]:entries)
    REQUIRE(mz_zip_writer_add_mem(&out,name.c_str(),data.data(),data.size(),MZ_DEFAULT_COMPRESSION));
  REQUIRE(mz_zip_writer_finalize_archive(&out));
  close_zip_writer(&out);
}
}

TEST_CASE("App owned legacy FFF 3mf fixture contains real geometry and distinct FFF settings",
          "[sla_legacy_fff_3mf_fixture]") {
  const char *directory=std::getenv("SLA_LEGACY_FFF_3MF_FIXTURES");
  if(!directory){SUCCEED("Set SLA_LEGACY_FFF_3MF_FIXTURES to emit the app-owned fixture");return;}
  const boost::filesystem::path root(directory);boost::filesystem::create_directories(root);
  Model model;auto *object=model.add_object("QA legacy FFF asymmetric block","",make_cube(3.,5.,7.));
  object->add_instance()->set_offset(Vec3d(12.,14.,0.));
  DynamicPrintConfig config;config.apply(SLAFullPrintConfig{});
  ConfigSubstitutionContext authored_substitutions{ForwardCompatibilitySubstitutionRule::Disable};
  config.set_key_value("printer_technology",new ConfigOptionEnum<PrinterTechnology>(ptSLA));
  config.set_deserialize("layer_height","0.2",authored_substitutions);
  config.set_deserialize("initial_layer_height","0.3",authored_substitutions);
  config.set_deserialize("bed_shape","0x0,300x0,300x250,0x250",authored_substitutions);
  config.set_deserialize("printer_settings_id","QA legacy FFF printer 300x250 nozzle0.6",authored_substitutions);
  const auto path=(root/"legacy-fff-authored.3mf").string();
  REQUIRE(store_3mf(path.c_str(),&model,&config,false));
  rewrite_3mf_technology_to_fff(path);
  Model loaded;DynamicPrintConfig imported;
  ConfigSubstitutionContext substitutions{ForwardCompatibilitySubstitutionRule::Disable};
  boost::optional<Semver> version;
  REQUIRE(load_3mf(path.c_str(),imported,substitutions,&loaded,false,version));
  // "printer_technology = FFF" is recognised on the raw string only (no enum value, config key stays absent).
  CHECK(imported.option("printer_technology")==nullptr);
  CHECK(substitutions.legacy_fff_technology);
  CHECK(imported.opt_float("layer_height")==.2);
  REQUIRE(loaded.objects.size()==1);REQUIRE(loaded.objects.front()->instances.size()==1);
  // add_volume centers local mesh coordinates; the loader may transfer that
  // volume offset into its single instance (3mf.cpp:1006). Authoring build
  // offset12/14/0 is proved from archive XML, not required of this normalized
  // in-memory decomposition. The complete world geometry must remain exact.
  const auto mesh=loaded.mesh();CHECK(mesh.its.vertices.size()==8);CHECK(mesh.its.indices.size()==12);
  const auto bbox=mesh.bounding_box();CHECK(bbox.min.isApprox(Vec3d(12.,14.,0.)));
  CHECK(bbox.max.isApprox(Vec3d(15.,19.,7.)));
  CHECK(bbox.size().isApprox(Vec3d(3.,5.,7.)));
  CHECK(its_volume(mesh.its)==105.f);
  for(const auto &vertex:mesh.its.vertices) {
    CHECK((vertex.x()==12.f || vertex.x()==15.f));
    CHECK((vertex.y()==14.f || vertex.y()==19.f));
    CHECK((vertex.z()==0.f || vertex.z()==7.f));
  }
  {
    const auto ini=(root/"legacy-fff-authored.ini").string();
    config.save(ini);
    std::string text;
    {std::ifstream in(ini);text.assign(std::istreambuf_iterator<char>(in),std::istreambuf_iterator<char>());}
    rewrite_technology_to_fff_text(text);
    append_legacy_fff_keys(text,"");
    std::ofstream(ini)<<text;
  }
  const nlohmann::json expected={{"fixture","legacy-fff-authored.3mf"},
      {"object_name","QA legacy FFF asymmetric block"},{"objects",1},{"instances",1},
      {"vertices",8},{"triangles",12},{"mesh_mm",{3.,5.,7.}},
      {"instance_offset_mm",{12.,14.,0.}},{"world_min_mm",{12.,14.,0.}},
      {"world_max_mm",{15.,19.,7.}},{"physical_volume_mm3",105.},
      {"source_printer_technology","FFF"},{"source_layer_height",.2},
      {"source_nozzle_diameter",.6},{"source_bed_mm",{300.,250.}},
      {"ownership","geometry plus legacy config; no SLA raster descriptor"}};
  std::ofstream out((root/"expected.json").string());out<<expected.dump(2)<<'\n';
}

TEST_CASE("Shared reader retains untagged legacy project configuration and physical geometry",
          "[sla_untagged_fff_3mf_fixture]") {
    const char *directory = std::getenv("SLA_UNTAGGED_FFF_3MF_FIXTURES");
    if (directory == nullptr) {
        SUCCEED("Set SLA_UNTAGGED_FFF_3MF_FIXTURES to the authored untagged fixture directory");
        return;
    }
    const auto path = (boost::filesystem::path(directory) / "legacy-fff-untagged.3mf").string();
    DynamicPrintConfig loaded;
    ConfigSubstitutionContext substitutions{ForwardCompatibilitySubstitutionRule::Disable};
    boost::optional<Semver> version;
    FileReader::LoadStats statistics;
    Model model = FileReader::load_model_with_config(path, &loaded, &substitutions, version,
        FileReader::LoadAttribute::CheckVersion, &statistics);
    REQUIRE_FALSE(loaded.empty());
    CHECK(loaded.option("printer_technology") == nullptr);
    CHECK(loaded.opt_float("layer_height") == .2);
    // The FFF schema is gone: the legacy FFF key in the fixture is dropped silently, with no substitution.
    CHECK(loaded.option("nozzle_diameter") == nullptr);
    CHECK(substitutions.empty());
    REQUIRE(model.objects.size() == 1);
    REQUIRE(model.objects.front()->instances.size() == 1);
    const auto mesh = model.mesh();
    CHECK(mesh.its.vertices.size() == 8);
    CHECK(mesh.its.indices.size() == 12);
    const auto box = mesh.bounding_box();
    CHECK(box.min.isApprox(Vec3d(12.,14.,0.)));
    CHECK(box.max.isApprox(Vec3d(15.,19.,7.)));
    CHECK(box.size().isApprox(Vec3d(3.,5.,7.)));
    CHECK(its_volume(mesh.its) == 105.f);
    for (const auto &vertex : mesh.its.vertices) {
        CHECK((vertex.x() == 12.f || vertex.x() == 15.f));
        CHECK((vertex.y() == 14.f || vertex.y() == 19.f));
        CHECK((vertex.z() == 0.f || vertex.z() == 7.f));
    }
}
