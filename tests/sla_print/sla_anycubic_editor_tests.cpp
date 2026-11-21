#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>
#include <boost/filesystem.hpp>
#include <fstream>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <catch2/catch_approx.hpp>
#include "libslic3r/Format/AnycubicSLA.hpp"
#include "libslic3r/SLAPrint.hpp"
#include "libslic3r/SLA/LayerDocument.hpp"
#include "libslic3r/SLA/LayerExportProfile.hpp"
#include "libslic3r/SLA/ProjectArchive.hpp"
#include "libslic3r/miniz_extension.hpp"
#include "libslic3r/Zipper.hpp"
#include "libslic3r/Format/ZipperArchiveImport.hpp"
#include <nlohmann/json.hpp>
using namespace Slic3r;

TEST_CASE("Anycubic native editor preserves the original container and mask edits", "[sla_reference][sla_anycubic_editor]")
{
    const std::string format = GENERATE("pws","pw0","pwx","dlp","dl2p","pwmx","pmx2","pwmb","pwmo","pm4n","pwms","pwma","pmsq","pm3","pm3m","pm3r","pm3n","pm5","px6s","pm5s","m5sp","pwc");
    const auto version = GENERATE_COPY(Catch::Generators::from_range(anycubic_available_versions(format)));
    const bool portrait = GENERATE(false, true);
    CAPTURE(format, version, portrait);
    DynamicPrintConfig cfg; cfg.apply(SLAFullPrintConfig{});
    cfg.option("printer_technology")->setInt(ptSLA);
    cfg.set("sla_archive_format", format); cfg.set("sla_anycubic_version", std::to_string(version));
    cfg.set("display_pixels_x", 8); cfg.set("display_pixels_y", 4);
    cfg.option("display_orientation")->setInt(portrait ? sladoPortrait : sladoLandscape);
    cfg.set("display_width",16.); cfg.set("display_height",4.); cfg.set("max_print_height",200.);
    if (const auto *geometry = anycubic_legacy_geometry(format); version <= 515 && geometry) {
        cfg.set("display_width", portrait ? geometry->height : geometry->width); cfg.set("display_height", portrait ? geometry->width : geometry->height);
        cfg.set("max_print_height", geometry->machine_z);
    }
    std::vector<uint8_t> pixels(32);
    for (auto [at, value] : {std::pair<size_t,uint8_t>{9,32},{10,64},{11,128},{17,192},{18,255},{19,85}}) pixels[at] = value;
    const auto png = sla::PNGRasterEncoder{}(pixels.data(), portrait ? 4 : 8, portrait ? 8 : 4, 1);
    const auto doc = sla::LayerDocument::from_rasters({png, png, png});
    auto job = SLAPrint::create_raster_job(cfg, doc, {.1, .15, .2});
    struct Directory {
        boost::filesystem::path path = boost::filesystem::unique_path("anycubic-editor-%%%%");
        bool keep = false;
        ~Directory() { if (!keep) { boost::system::error_code e; boost::filesystem::remove_all(path, e); } }
    } dir;
    if (const char *folder = std::getenv("SLA_ANYCUBIC_EDITOR_FIXTURES")) {
        dir.path = boost::filesystem::path(folder)/(format+"-"+std::to_string(version)+(portrait?"-portrait":"-landscape")); dir.keep = true;
    }
    boost::filesystem::create_directories(dir.path);
    cfg.save((dir.path/"source.ini").string());
    const auto source = dir.path / ("source." + format), saved = dir.path / ("saved." + format);
    job->export_print(source.string());
    auto read = [](const boost::filesystem::path &path) {
        std::ifstream stream(path.string(), std::ios::binary);
        return std::vector<uint8_t>{std::istreambuf_iterator<char>(stream), {}};
    };
    // Unknown trailing manufacturer metadata must survive both paths.
    { std::ofstream stream(source.string(), std::ios::binary|std::ios::app); stream << "vendor metadata retained"; }
    const auto original = read(source);
    auto loaded = sla::LayerDocument::load_archive(source.string());
    REQUIRE(loaded.archive_format() == format); REQUIRE(loaded.layer_count() == 3);
    for (auto &value : pixels) value = format == "pws" ? (value >= 128 ? 255 : 0) : uint8_t((value >> 4) * 17);
    for (size_t i = 0; i < 3; ++i) REQUIRE(loaded.image(i).buf == pixels);
    loaded.save_archive(saved.string()); REQUIRE(read(saved) == original);
    loaded.invert(1);
    auto expected = pixels;
    for (auto &value : expected) value = uint8_t(255 - value);
    REQUIRE(loaded.image(1).buf == expected);
    loaded.paint(1, 0, 0, 0, 0); expected[0] = 0;
    REQUIRE(loaded.image(1).buf == expected);
    loaded.save_archive(saved.string());
    auto edited = sla::LayerDocument::load_archive(saved.string());
    REQUIRE(edited.image(0).buf == pixels); REQUIRE(edited.image(1).buf == expected); REQUIRE(edited.image(2).buf == pixels);
    const auto written = read(saved);
    auto u32 = [](const std::vector<uint8_t> &data, size_t at) { return uint32_t(data.at(at))|(uint32_t(data.at(at+1))<<8)|(uint32_t(data.at(at+2))<<16)|(uint32_t(data.at(at+3))<<24); };
    auto f32 = [&](const std::vector<uint8_t> &data, size_t at) { const auto bits = u32(data, at); float value; std::memcpy(&value, &bits, 4); return value; };
    const auto header = u32(original,20), table = u32(original,36);
    const auto old_count = std::count_if(pixels.begin(),pixels.end(),[](uint8_t value){return value!=0;});
    const auto new_count = std::count_if(expected.begin(),expected.end(),[](uint8_t value){return value!=0;});
    const double ratio = (old_count*.15+new_count*.05)/(old_count*.2);
    REQUIRE(f32(written,header+52)==Catch::Approx(f32(original,header+52)*ratio));
    REQUIRE(u32(written,table+20+32+24)==new_count);
    if (version >= 517) {
        const auto model = u32(original,52), machine = u32(original,44);
        REQUIRE(f32(written,model+16)==Catch::Approx(-f32(original,machine+136)/2));
        REQUIRE(f32(written,model+20)==Catch::Approx(-f32(original,machine+140)/2));
    }
    REQUIRE(loaded.undo()); REQUIRE(loaded.undo());
    const auto restored = dir.path/("restored."+format);
    loaded.save_archive(restored.string()); REQUIRE(read(restored) == original);
}

TEST_CASE("Anycubic malformed inputs and cancellation never publish partial data", "[sla_reference][sla_anycubic_editor]")
{
    DynamicPrintConfig cfg; cfg.apply(SLAFullPrintConfig{});
    cfg.option("printer_technology")->setInt(ptSLA);
    cfg.set("sla_archive_format",std::string("pwc")); cfg.set("sla_anycubic_version",std::string("518"));
    cfg.set("display_pixels_x",8); cfg.set("display_pixels_y",4);
    cfg.option("display_orientation")->setInt(sladoLandscape);
    const auto pixels=std::vector<uint8_t>(32,255);
    const auto png=sla::PNGRasterEncoder{}(pixels.data(),8,4,1);
    const auto masks=sla::LayerDocument::from_rasters({png,png,png});
    auto job=SLAPrint::create_raster_job(cfg,masks,{.1,.15,.2});
    struct Directory {
        boost::filesystem::path path=boost::filesystem::unique_path("anycubic-invalid-%%%%");
        ~Directory(){boost::system::error_code e;boost::filesystem::remove_all(path,e);}
    } dir;
    boost::filesystem::create_directory(dir.path);
    const auto source=dir.path/"source.pwc",invalid=dir.path/"invalid.pwc";
    job->export_print(source.string());
    auto read=[&](const boost::filesystem::path &path){std::ifstream in(path.string(),std::ios::binary);return std::vector<uint8_t>{std::istreambuf_iterator<char>(in),{}};};
    const auto bytes=read(source);
    auto u32=[&](size_t at){return uint32_t(bytes[at])|(uint32_t(bytes[at+1])<<8)|(uint32_t(bytes[at+2])<<16)|(uint32_t(bytes[at+3])<<24);};
    const auto header=u32(20),table=u32(36);
    for(auto [at,value]:{std::pair<size_t,uint32_t>{36,UINT32_MAX},{header+60,0},{header+56,0},{table+16,UINT32_MAX},
                         {table+20,UINT32_MAX},{table+24,UINT32_MAX},{table+40,0},{56,UINT32_MAX}}) {
        auto damaged=bytes;for(unsigned n=0;n<4;++n)damaged[at+n]=uint8_t(value>>(8*n));
        {std::ofstream out(invalid.string(),std::ios::binary);out.write(reinterpret_cast<const char *>(damaged.data()),damaged.size());}
        CAPTURE(at,value);REQUIRE_THROWS(sla::LayerDocument::load_archive(invalid.string()));
    }
    {
        // A syntactically valid RLE stream hidden in metadata must not become
        // a layer merely because a corrupted pointer targets it.
        auto damaged = bytes;
        const auto inside_table = table+20+32+28;
        auto put = [&](size_t at,uint32_t value){for(unsigned n=0;n<4;++n)damaged[at+n]=uint8_t(value>>(8*n));};
        put(inside_table, 32u<<8); put(table+20, inside_table); put(table+24, 2);
        {std::ofstream out(invalid.string(),std::ios::binary);out.write(reinterpret_cast<const char *>(damaged.data()),damaged.size());}
        REQUIRE_THROWS(sla::LayerDocument::load_archive(invalid.string()));
    }
    REQUIRE_THROWS_AS(sla::LayerDocument::load_archive(source.string(),[](size_t done,size_t){return done<1;}),sla::LayerOperationCancelled);
    auto loaded=sla::LayerDocument::load_archive(source.string());loaded.invert(1);
    REQUIRE_THROWS_AS(loaded.save_archive(source.string(),[](size_t done,size_t){return done<1;}),sla::LayerOperationCancelled);
    REQUIRE(read(source)==bytes);REQUIRE(loaded.dirty());REQUIRE(loaded.undo());
    for(const auto &entry:boost::filesystem::directory_iterator(dir.path))
        REQUIRE(entry.path().filename().string().find(".sla-edit-")==std::string::npos);
}

TEST_CASE("Anycubic empty model edits compute volume from physical geometry", "[sla_reference][sla_anycubic_editor]")
{
    const std::string format = GENERATE("pws", "pm5s", "pwc");
    const unsigned version = format == "pm5s" ? 518 : 1;
    DynamicPrintConfig cfg; cfg.apply(SLAFullPrintConfig{});
    cfg.option("printer_technology")->setInt(ptSLA);
    cfg.set("sla_archive_format", format); cfg.set("sla_anycubic_version", std::to_string(version));
    cfg.set("display_pixels_x", 8); cfg.set("display_pixels_y", 4);
    cfg.option("display_orientation")->setInt(sladoLandscape);
    cfg.set("display_width", 16.); cfg.set("display_height", 4.);
    if (const auto *geometry = anycubic_legacy_geometry(format)) {
        cfg.set("display_width", geometry->width); cfg.set("display_height", geometry->height);
        cfg.set("max_print_height", geometry->machine_z);
    }
    const std::vector<uint8_t> pixels(32, 0);
    const auto png = sla::PNGRasterEncoder{}(pixels.data(), 8, 4, 1);
    auto job = SLAPrint::create_raster_job(cfg, sla::LayerDocument::from_rasters({png, png}), {.1, .15});
    struct Directory {
        boost::filesystem::path path = boost::filesystem::unique_path("anycubic-empty-%%%%");
        ~Directory() { boost::system::error_code e; boost::filesystem::remove_all(path, e); }
    } dir;
    boost::filesystem::create_directory(dir.path);
    const auto source = dir.path/("source."+format), saved = dir.path/("saved."+format);
    job->export_print(source.string());
    auto doc = sla::LayerDocument::load_archive(source.string());
    doc.paint(1, 2, 1, 0, 255);
    if (format == "pwc") {
        // No MACHINE and no model-specific dimensions: scalar pixel pitch
        // cannot recover the physical area of anisotropic pixels.
        std::ofstream(saved.string()) << "existing destination";
        REQUIRE_THROWS(doc.save_archive(saved.string()));
        std::ifstream in(saved.string()); std::string preserved; std::getline(in, preserved);
        REQUIRE(preserved == "existing destination");
        REQUIRE(doc.dirty()); REQUIRE(doc.undo());
    } else {
        doc.save_archive(saved.string());
        std::ifstream in(saved.string(), std::ios::binary);
        const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(in), {}};
        auto u32 = [&](size_t at) { return uint32_t(bytes.at(at)) | (uint32_t(bytes.at(at+1))<<8) | (uint32_t(bytes.at(at+2))<<16) | (uint32_t(bytes.at(at+3))<<24); };
        auto f32 = [&](size_t at) { const auto bits=u32(at); float value; std::memcpy(&value,&bits,4); return value; };
        // One illuminated pixel at relative height .05 mm, in ml.
        const double volume = cfg.option("display_width")->getFloat()*cfg.option("display_height")->getFloat()/32*.05/1000;
        REQUIRE(f32(u32(20)+52) == Catch::Approx(volume));
        REQUIRE(sla::LayerDocument::load_archive(saved.string()).image(1).buf.at(10) == 255);
        REQUIRE(doc.undo()); doc.save_archive(saved.string());
        std::ifstream restored(saved.string(),std::ios::binary);
        const std::vector<uint8_t> restored_bytes{std::istreambuf_iterator<char>(restored),{}};
        REQUIRE(restored_bytes != bytes);
    }
}

TEST_CASE("PWS import and edit preserve all decoded AA levels", "[sla_reference][sla_anycubic_editor]")
{
    const unsigned planes = GENERATE(Catch::Generators::range(1u, 17u));
    CAPTURE(planes);
    DynamicPrintConfig cfg; cfg.apply(SLAFullPrintConfig{});
    cfg.option("printer_technology")->setInt(ptSLA);
    cfg.set("sla_archive_format", std::string("pws"));
    cfg.set("display_pixels_x", 8); cfg.set("display_pixels_y", 4);
    cfg.option("display_orientation")->setInt(sladoLandscape);
    const auto *geometry = anycubic_legacy_geometry("pws");
    cfg.set("display_width", geometry->width); cfg.set("display_height", geometry->height);
    cfg.set("max_print_height", geometry->machine_z);
    const std::vector<uint8_t> blank(32, 0);
    const auto png = sla::PNGRasterEncoder{}(blank.data(), 8, 4, 1);
    auto job = SLAPrint::create_raster_job(cfg, sla::LayerDocument::from_rasters({png}), {.1});
    struct Directory {
        boost::filesystem::path path = boost::filesystem::unique_path("pws-aa-%%%%");
        bool keep = false;
        ~Directory() { if (!keep) { boost::system::error_code e; boost::filesystem::remove_all(path, e); } }
    } dir;
    if (const char *folder=std::getenv("SLA_PWS_AA_EDITOR_FIXTURES")) {
        dir.path=boost::filesystem::path(folder)/("aa-"+std::to_string(planes)); dir.keep=true;
    }
    boost::filesystem::create_directories(dir.path);
    const auto source = dir.path/"source.pws", saved = dir.path/"saved.pws";
    job->export_print(source.string());
    std::ifstream input(source.string(),std::ios::binary);
    std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(input),{}};
    auto u32 = [&](size_t at) { return uint32_t(bytes.at(at)) | (uint32_t(bytes.at(at+1))<<8) | (uint32_t(bytes.at(at+2))<<16) | (uint32_t(bytes.at(at+3))<<24); };
    auto put32 = [&](size_t at,unsigned value) { for (unsigned n=0;n<4;++n) bytes.at(at+n)=uint8_t(value>>(8*n)); };
    const auto header=u32(20), record=u32(36)+20;
    put32(header+56,planes); put32(record,unsigned(bytes.size())); put32(record+4,32*planes);
    // Independent hand-built stream: one byte per pixel per bitplane, no
    // native encoder used to create the expected shades or payload.
    std::vector<uint8_t> expected(32);
    for (unsigned x=0;x<32;++x) {
        const unsigned level=x%(planes+1);
        expected[x]=level ? uint8_t(level*(256/planes)-1) : 0;
    }
    const unsigned nonzero=unsigned(std::count_if(expected.begin(),expected.end(),[](uint8_t value){return value!=0;}));
    put32(record+24,nonzero);
    const float source_volume=float(nonzero*geometry->width*geometry->height/32*.1/1000);
    uint32_t volume_bits; std::memcpy(&volume_bits,&source_volume,4); put32(header+52,volume_bits);
    for (unsigned plane=0;plane<planes;++plane)
        for (unsigned x=0;x<32;++x) bytes.push_back(uint8_t(1 | (x%(planes+1)>plane ? 128 : 0)));
    {std::ofstream output(source.string(),std::ios::binary);output.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());}
    auto doc=sla::LayerDocument::load_archive(source.string());
    REQUIRE(doc.image(0).buf==expected);
    for(unsigned x=0;x<32;++x) doc.paint(0,x%8,x/8,0,expected[x]);
    REQUIRE(doc.image(0).buf==expected); // quantization is idempotent
    doc.save_archive(saved.string());
    std::ifstream unchanged(saved.string(),std::ios::binary);
    const std::vector<uint8_t> same{std::istreambuf_iterator<char>(unchanged),{}};
    REQUIRE(same==bytes);
    doc.paint(0,0,0,0,255); expected[0]=uint8_t(planes*(256/planes)-1);
    REQUIRE(doc.image(0).buf==expected);
    doc.save_archive(saved.string());
    REQUIRE(sla::LayerDocument::load_archive(saved.string()).image(0).buf==expected);
}

TEST_CASE("Explicit printer and material context repairs missing Anycubic resin estimates", "[sla_reference][sla_anycubic_editor][sla_layer_profile]")
{
    const std::string format=GENERATE("pwc", "pm5s");
    const unsigned version=format=="pwc"?1:518;
    DynamicPrintConfig cfg;cfg.apply(SLAFullPrintConfig{});
    cfg.option("printer_technology")->setInt(ptSLA);
    cfg.set("sla_archive_format",format);cfg.set("sla_anycubic_version",std::to_string(version));
    cfg.set("display_pixels_x",8);cfg.set("display_pixels_y",4);
    cfg.option("display_orientation")->setInt(sladoLandscape);
    cfg.set("display_width",16.);cfg.set("display_height",4.);
    std::vector<uint8_t> pixels(32);pixels[9]=255;
    const auto png=sla::PNGRasterEncoder{}(pixels.data(),8,4,1);
    auto job=SLAPrint::create_raster_job(cfg,sla::LayerDocument::from_rasters({png}),{.1});
    struct Directory {
        boost::filesystem::path path=boost::filesystem::unique_path("layer-profile-%%%%");
        bool keep=false;
        ~Directory(){if(!keep){boost::system::error_code e;boost::filesystem::remove_all(path,e);}}
    } dir;
    if(const char *folder=std::getenv("SLA_LAYER_PROFILE_FIXTURES")){
        dir.path=boost::filesystem::path(folder)/(format+"-"+std::to_string(version));dir.keep=true;
    }
    boost::filesystem::create_directories(dir.path);
    const auto source=dir.path/("source."+format),saved=dir.path/("saved."+format);
    job->export_print(source.string());
    std::ifstream in(source.string(),std::ios::binary);
    std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(in),{}};
    auto u32=[](const std::vector<uint8_t> &data,size_t at){return uint32_t(data.at(at))|(uint32_t(data.at(at+1))<<8)|(uint32_t(data.at(at+2))<<16)|(uint32_t(data.at(at+3))<<24);};
    const auto header=u32(bytes,20);
    for(size_t offset:{52u,68u,72u})std::fill(bytes.begin()+header+offset,bytes.begin()+header+offset+4,0);
    {std::ofstream out(source.string(),std::ios::binary);out.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());}
    auto doc=sla::LayerDocument::load_archive(source.string());
    sla::LayerExportProfile profile{8,4,16,4,1.2,.2};
    const auto stats_only=dir.path/("statistics_only."+format);
    doc.save_archive(stats_only.string(),{},&profile);
    REQUIRE_FALSE(doc.dirty());
    REQUIRE(sla::LayerDocument::load_archive(stats_only.string()).image(0).buf==pixels);
    std::ifstream stats_input(stats_only.string(),std::ios::binary);
    const std::vector<uint8_t> stats_bytes{std::istreambuf_iterator<char>(stats_input),{}};
    const auto volume_bits=u32(stats_bytes,header+52);float stats_volume;std::memcpy(&stats_volume,&volume_bits,4);
    REQUIRE(stats_volume==Catch::Approx(.0002));
    REQUIRE(stats_bytes.size()==bytes.size());
    doc.paint(0,2,1,0,255);
    doc.save_archive(saved.string(),{},&profile);
    std::ifstream written(saved.string(),std::ios::binary);
    const std::vector<uint8_t> output{std::istreambuf_iterator<char>(written),{}};
    auto f32=[&](size_t at){const auto bits=u32(output,at);float value;std::memcpy(&value,&bits,4);return value;};
    const double volume=2*16.*4/32*.1/1000;
    REQUIRE(f32(header+52)==Catch::Approx(volume));
    REQUIRE(f32(header+68)==Catch::Approx(volume*1.2));
    REQUIRE(f32(header+72)==Catch::Approx(volume*.2));
    REQUIRE(sla::LayerDocument::load_archive(saved.string()).image(0).buf.at(10)==255);
    doc.paint(0,3,1,0,255);
    for(unsigned error=0;error<4;++error) {
        auto invalid=profile;
        if(error==0)invalid.columns=4;
        if(error==1)invalid.density_g_ml=0;
        if(error==2)invalid.cost_per_ml=-1;
        if(error==3)invalid.screen_width_mm=0;
        REQUIRE_THROWS(doc.save_archive(saved.string(),{},&invalid));
        std::ifstream preserved(saved.string(),std::ios::binary);
        const std::vector<uint8_t> preserved_bytes{std::istreambuf_iterator<char>(preserved),{}};
        REQUIRE(preserved_bytes==output);REQUIRE(doc.dirty());
    }
    if(version>=516){auto invalid=profile;invalid.screen_width_mm=17;REQUIRE_THROWS(doc.save_archive(saved.string(),{},&invalid));}
    REQUIRE_THROWS_AS(doc.save_archive(saved.string(),[](size_t done,size_t){return done<1;},&profile),sla::LayerOperationCancelled);
    REQUIRE(doc.dirty());REQUIRE(doc.undo());REQUIRE_FALSE(doc.dirty());
}

TEST_CASE("Layer export profiles validate physical quantities before writing", "[sla_reference][sla_layer_profile]")
{
    sla::LayerExportProfile profile{8,4,16,4,1.2,.2};
    REQUIRE_NOTHROW(profile.validate(8,4));
    REQUIRE(profile.pixel_area()==Catch::Approx(2));
    REQUIRE_THROWS(profile.validate(4,8));
    for (double value : {0., -1., std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
        auto invalid=profile;invalid.density_g_ml=value;REQUIRE_THROWS(invalid.validate(8,4));
        invalid=profile;invalid.screen_width_mm=value;REQUIRE_THROWS(invalid.validate(8,4));
    }
    auto invalid=profile;invalid.cost_per_ml=-1;REQUIRE_THROWS(invalid.validate(8,4));
    invalid=profile;invalid.cost_per_ml=std::numeric_limits<double>::quiet_NaN();REQUIRE_THROWS(invalid.validate(8,4));
    invalid=profile;invalid.screen_width_mm=1e308;invalid.screen_height_mm=1e308;REQUIRE_THROWS(invalid.validate(8,4));
}

TEST_CASE("Project preserves opt-in physical and material context for imported layers", "[sla_layers][sla_project_profile]") {
    const std::string variant=GENERATE("pwc-1","pwc-515","pwc-516","pm5s-518");
    const bool portrait=GENERATE(false,true);CAPTURE(variant,portrait);
    const auto dash=variant.find('-');const auto format=variant.substr(0,dash),version=variant.substr(dash+1);
    struct Directory {boost::filesystem::path path=boost::filesystem::unique_path("project-profile-%%%%");bool keep=false;
        ~Directory(){if(!keep){boost::system::error_code e;boost::filesystem::remove_all(path,e);}}} dir;
    if(const char *folder=std::getenv("SLA_PROJECT_PROFILE_FIXTURES")) {
        dir.path=boost::filesystem::path(folder)/(variant+(portrait?"-portrait":"-landscape"));dir.keep=true;
    }
    boost::filesystem::create_directories(dir.path);
    DynamicPrintConfig cfg;cfg.apply(SLAFullPrintConfig{});cfg.option("printer_technology")->setInt(ptSLA);
    cfg.set("sla_archive_format",format);cfg.set("sla_anycubic_version",version);
    cfg.set("display_pixels_x",8);cfg.set("display_pixels_y",4);cfg.set("display_width",16.);cfg.set("display_height",4.);
    cfg.option("display_orientation")->setInt(portrait?sladoPortrait:sladoLandscape);
    std::vector<uint8_t> pixels(32,0);pixels[1]=255;
    const auto png=sla::PNGRasterEncoder{}(pixels.data(),portrait?4:8,portrait?8:4,1);
    auto job=SLAPrint::create_raster_job(cfg,sla::LayerDocument::from_rasters({png,png,png}),{.1,.15,.2});
    const auto source=dir.path/("source."+format),project=dir.path/"profile.3mf";
    job->export_print(source.string());auto document=sla::LayerDocument::load_archive(source.string());
    document.paint(1,0,0,0,255);
    sla::LayerExportProfile profile{portrait?4u:8u,portrait?8u:4u,portrait?4.:16.,portrait?16.:4.,1.3,.37,200.};
    sla::ProjectRasterState state{document,{},"",profile,sla::ProjectLayerOrigin::ImportedArchive};
    REQUIRE_NOTHROW(sla::save_layer_project(project.string(),state));
    // Reload while the original native file is unavailable: the project owns it.
    const auto hidden_source=dir.path/("source-unavailable."+format);
    boost::filesystem::rename(source,hidden_source);
    auto restored=sla::read_project_layers(project.string());REQUIRE(restored.has_value());
    REQUIRE(restored->export_profile.has_value());
    REQUIRE(restored->origin==sla::ProjectLayerOrigin::ImportedArchive);
    REQUIRE(restored->source_fingerprint.empty());REQUIRE(restored->positions_z.empty());
    boost::filesystem::rename(hidden_source,source);
    const auto actual=*restored->export_profile;
    REQUIRE(actual.columns==profile.columns);REQUIRE(actual.rows==profile.rows);
    REQUIRE(actual.screen_width_mm==profile.screen_width_mm);REQUIRE(actual.screen_height_mm==profile.screen_height_mm);
    REQUIRE(actual.density_g_ml==profile.density_g_ml);REQUIRE(actual.cost_per_ml==profile.cost_per_ml);
    REQUIRE(actual.max_print_height_mm==profile.max_print_height_mm);
    const auto before=dir.path/("expected."+format),after=dir.path/("restored."+format);
    document.save_archive(before.string(),{},&profile);
    restored->document.save_archive(after.string(),{},&actual);
    auto bytes=[](const boost::filesystem::path &path){std::ifstream in(path.string(),std::ios::binary);return std::vector<uint8_t>{std::istreambuf_iterator<char>(in),{}};};
    REQUIRE(bytes(before)==bytes(after));
    const auto entries=read_zipper_archive(project.string(),{"metadata/sla/"},{}).entries;
    const auto manifest=std::find_if(entries.begin(),entries.end(),[](const auto &e){return e.fname=="metadata/sla/layers.json";});
    REQUIRE(manifest!=entries.end());const auto metadata=nlohmann::json::parse(manifest->buf);
    for(unsigned error=0;error<9;++error) {
        auto invalid=metadata;auto &context=invalid["export_profile"];
        if(error==0)context["columns"]=-1;
        if(error==1)context["rows"]=2.5;
        if(error==2)context["density_g_ml"]=0;
        if(error==3)context["max_print_height_mm"]=-1;
        if(error==4)context["max_print_height_mm"]=.1;
        if(error==5)context["cost_per_ml"]="expensive";
        if(error==6)context["screen_width_mm"]=nullptr;
        if(error==7)context.erase("density_g_ml");
        if(error==8)context["screen_height_mm"]=0;
        const auto damaged=dir.path/("invalid-profile-"+std::to_string(error)+".3mf");
        {Zipper zip(damaged.string());for(const auto &entry:entries) {
            if(entry.fname==manifest->fname){zip.add_entry(entry.fname);zip<<invalid.dump();}
            else zip.add_entry(entry.fname,entry.buf.data(),entry.buf.size());
        }zip.finalize();}
        REQUIRE_THROWS(sla::read_project_layers(damaged.string()));
    }
    for(bool explicit_null:{false,true}) {
        auto absent=metadata;if(explicit_null)absent["export_profile"]=nullptr;else absent.erase("export_profile");
        const auto unbound=dir.path/(explicit_null?"null-profile.3mf":"no-profile.3mf");
        {Zipper zip(unbound.string());for(const auto &entry:entries) {
            if(entry.fname==manifest->fname){zip.add_entry(entry.fname);zip<<absent.dump();}
            else zip.add_entry(entry.fname,entry.buf.data(),entry.buf.size());
        }zip.finalize();}
        const auto loaded=sla::read_project_layers(unbound.string());REQUIRE(loaded.has_value());REQUIRE_FALSE(loaded->export_profile.has_value());
    }

    for(unsigned error=0;error<5;++error) {
        auto invalid=state;
        if(error==0)invalid.export_profile->columns++;
        if(error==1)invalid.export_profile->density_g_ml=0;
        if(error==2)invalid.export_profile->max_print_height_mm=.1;
        if(error==3)invalid.export_profile->max_print_height_mm=-1;
        if(error==4)invalid.export_profile->cost_per_ml=std::numeric_limits<double>::quiet_NaN();
        mz_zip_archive heap{};REQUIRE(mz_zip_writer_init_heap(&heap,0,0));
        CHECK_THROWS(sla::write_project_layers(heap,invalid));CHECK(heap.m_total_files==0);REQUIRE(mz_zip_writer_end(&heap));
    }
}

TEST_CASE("Native float accumulation at machine Z preserves legitimate last layer edits", "[sla_layers][sla_project_profile][sla_native_height_precision]") {
    struct Directory {boost::filesystem::path path=boost::filesystem::unique_path("native-height-%%%%");bool keep=false;
        ~Directory(){if(!keep){boost::system::error_code e;boost::filesystem::remove_all(path,e);}}} dir;
    if(const char *folder=std::getenv("SLA_NATIVE_HEIGHT_FIXTURES")){dir.path=folder;dir.keep=true;}
    boost::filesystem::create_directories(dir.path);
    DynamicPrintConfig cfg;cfg.apply(SLAFullPrintConfig{});cfg.option("printer_technology")->setInt(ptSLA);
    cfg.set("sla_archive_format",std::string("pwc"));cfg.set("sla_anycubic_version",std::string("516"));
    cfg.set("display_pixels_x",8);cfg.set("display_pixels_y",4);cfg.set("display_width",16.);cfg.set("display_height",4.);
    cfg.set("max_print_height",200.);cfg.option("display_orientation")->setInt(sladoLandscape);
    std::vector<uint8_t> pixels(32,0);pixels[1]=255;
    const auto png=sla::PNGRasterEncoder{}(pixels.data(),8,4,1);
    std::vector<double> positions;for(size_t i=0;i<4000;++i)positions.push_back((i+1)*.05);
    auto job=SLAPrint::create_raster_job(cfg,sla::LayerDocument::from_rasters(std::vector<sla::EncodedRaster>(4000,png)),positions);
    const auto source=dir.path/"source.pwc",saved=dir.path/"saved.pwc";job->export_print(source.string());
    auto document=sla::LayerDocument::load_archive(source.string());using F=sla::LayerParameter;
    REQUIRE(document.layer_parameters(3999).at(F::PositionZ)>200.); // source float deltas accumulate above exact double max
    sla::LayerExportProfile profile{8,4,16,4,1.3,.37,200.};
    SECTION("metadata edit") {REQUIRE_NOTHROW(document.set_layer_parameters(3999,{{F::Exposure,7.25}}));}
    SECTION("profiled native save") {REQUIRE_NOTHROW(document.save_archive(saved.string(),{},&profile));}
    SECTION("profiled project save") {
        const sla::ProjectRasterState state{document,positions,std::string(40,'a'),profile};
        mz_zip_archive heap{};REQUIRE(mz_zip_writer_init_heap(&heap,0,0));
        CHECK_NOTHROW(sla::write_project_layers(heap,state));REQUIRE(mz_zip_writer_end(&heap));
    }
    SECTION("meaningful height excess") {
        REQUIRE_THROWS(document.set_layer_parameters(3999,{{F::PositionZ,200.01}}));
        auto smaller=profile;smaller.max_print_height_mm=199.99;
        REQUIRE_THROWS(document.save_archive(saved.string(),{},&smaller));
    }
}
