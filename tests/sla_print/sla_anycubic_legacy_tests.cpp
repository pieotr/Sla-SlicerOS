#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>
#include <boost/filesystem.hpp>
#include <fstream>
#include <cstring>
#include <cstdlib>
#include "libslic3r/Format/AnycubicSLA.hpp"
#include "libslic3r/SLAPrint.hpp"
#include "libslic3r/SLA/LayerDocument.hpp"
using namespace Slic3r;

#ifndef _WIN32
// POSIX permits a 255-byte component. Appending a staging suffix to a valid
// filename used to make modern exports fail before writing any bytes.
TEST_CASE("Anycubic atomic export accepts long filenames", "[sla_anycubic_filename]")
{
    const int version = GENERATE(1, 515, 516, 517, 518);
    CAPTURE(version);
    struct Directory {
        boost::filesystem::path path = boost::filesystem::unique_path("anycubic-name-%%%%");
        ~Directory() { boost::system::error_code error; boost::filesystem::remove_all(path, error); }
    } directory;
    boost::filesystem::create_directory(directory.path);
    DynamicPrintConfig cfg;
    cfg.apply(SLAFullPrintConfig{});
    cfg.option("printer_technology")->setInt(ptSLA);
    cfg.set("sla_archive_format", "pwc");
    cfg.set("sla_anycubic_version", std::to_string(version));
    cfg.set("display_width", 16.);
    cfg.set("display_height", 4.);
    cfg.set("display_pixels_x", 8);
    cfg.set("display_pixels_y", 4);
    cfg.option("display_orientation")->setInt(sladoLandscape);
    cfg.set("display_mirror_x", false);
    cfg.set("display_mirror_y", false);
    const std::vector<uint8_t> pixels(32, 255);
    const auto png = sla::PNGRasterEncoder{}(pixels.data(), 8, 4, 1);
    const auto document = sla::LayerDocument::from_rasters({png});
    auto job = SLAPrint::create_raster_job(cfg, document, {.05});
    const auto short_path = directory.path / "short.pwc";
    const auto long_path = directory.path / (std::string(240, 'x') + ".pwc");
    auto read = [](const boost::filesystem::path &path) {
        std::ifstream input(path.string(), std::ios::binary);
        return std::vector<uint8_t>{std::istreambuf_iterator<char>(input), {}};
    };
    REQUIRE_NOTHROW(job->export_print(short_path.string()));
    REQUIRE_NOTHROW(job->export_print(long_path.string()));
    const auto expected = read(short_path);
    REQUIRE_FALSE(expected.empty());
    REQUIRE(read(long_path) == expected);
    REQUIRE_NOTHROW(job->export_print(long_path.string()));
    REQUIRE(read(long_path) == expected);
    // A rejected replacement must preserve the previous file and leave no
    // staging file behind in the destination directory.
    cfg.set("sla_wait_after_cure", 1.);
    auto invalid = SLAPrint::create_raster_job(cfg, document, {.05});
    REQUIRE_THROWS(invalid->export_print(long_path.string()));
    REQUIRE(read(long_path) == expected);
    REQUIRE(std::distance(boost::filesystem::directory_iterator(directory.path),
                          boost::filesystem::directory_iterator{}) == 2);
}
#endif

TEST_CASE("Anycubic legacy extension versions stream complete validated layouts", "[sla_reference][sla_anycubic_legacy]")
{
    const std::string format=GENERATE("pws","pw0","pwx","dlp","dl2p","pwmx","pmx2","pwmb","pwmo","pm4n","pwms","pwma","pmsq","pm3","pm3m","pm3r","pwc");
    std::vector<uint16_t> versions;
    for (auto version:anycubic_available_versions(format)) if(version<=515) versions.push_back(version);
    const auto version=GENERATE_COPY(Catch::Generators::from_range(versions));
    const bool portrait=GENERATE(false,true);
    CAPTURE(format,version,portrait);
    const auto *geometry=anycubic_legacy_geometry(format);
    const double width=geometry?geometry->width:16.,height=geometry?geometry->height:4.,machine_z=geometry?geometry->machine_z:200.;
    DynamicPrintConfig cfg;cfg.apply(SLAFullPrintConfig{});
    cfg.option("printer_technology")->setInt(ptSLA);cfg.set("sla_archive_format",format);
    cfg.set("sla_anycubic_version",std::to_string(version));
    cfg.set("display_width",portrait?height:width);cfg.set("display_height",portrait?width:height);
    cfg.set("max_print_height",machine_z);
    cfg.set("display_pixels_x",8);cfg.set("display_pixels_y",4);
    cfg.option("display_orientation")->setInt(portrait?sladoPortrait:sladoLandscape);
    cfg.set("display_mirror_x",false);cfg.set("display_mirror_y",false);
    cfg.set("layer_height",.05);cfg.set("initial_layer_height",.1);
    cfg.set("sla_bottom_layers",1);cfg.set("sla_transition_layers",1);
    cfg.set("exposure_time",3.);cfg.set("initial_exposure_time",25.);
    cfg.set("sla_lift_height",3.);cfg.set("sla_bottom_lift_height",5.);
    cfg.set("sla_lift_speed",90.);cfg.set("sla_bottom_lift_speed",60.);
    cfg.set("sla_retract_speed",150.);cfg.set("sla_bottom_retract_speed",150.);
    cfg.set("sla_wait_before_cure",1.25);cfg.set("sla_bottom_wait_before_cure",1.25);
    cfg.set("material_density",1.1);cfg.set("bottle_volume",500.);cfg.set("bottle_cost",50.);
    std::vector<uint8_t> pixels(32,0);
    for(auto [at,value]:{std::pair<size_t,uint8_t>{9,32},{10,64},{11,128},{17,192},{18,255},{19,85}})pixels[at]=value;
    const auto png=sla::PNGRasterEncoder{}(pixels.data(),portrait?4:8,portrait?8:4,1);
    const auto document=sla::LayerDocument::from_rasters({png,png,png});
    auto job=SLAPrint::create_raster_job(cfg,document,{.1,.15,.2});
    struct File {boost::filesystem::path path;bool keep=false;~File(){if(!keep){boost::system::error_code e;boost::filesystem::remove(path,e);}}}
        file{boost::filesystem::unique_path("anycubic-legacy-%%%%."+format)};
    if(const char *directory=std::getenv("SLA_ANYCUBIC_LEGACY_DIRECTORY")){
        boost::filesystem::create_directories(directory);
        file.path=boost::filesystem::path(directory)/((portrait?"portrait-":"landscape-")+std::to_string(version)+"."+format);file.keep=true;
    }
    job->export_print(file.path.string());
    auto read=[&](){std::ifstream in(file.path.string(),std::ios::binary);return std::vector<uint8_t>{std::istreambuf_iterator<char>(in),{}};};
    const auto bytes=read();
    auto u32=[&](size_t p){REQUIRE(p+4<=bytes.size());return uint32_t(bytes[p])|(uint32_t(bytes[p+1])<<8)|(uint32_t(bytes[p+2])<<16)|(uint32_t(bytes[p+3])<<24);};
    auto f32=[&](size_t p){const auto bits=u32(p);float value;std::memcpy(&value,&bits,4);return value;};
    REQUIRE(u32(12)==version);REQUIRE(u32(16)==(version==515?5:4));REQUIRE(u32(20)==48);
    const auto h=u32(20),preview=u32(28),table=u32(36),image=u32(44);
    REQUIRE(u32(h+12)==80);REQUIRE(preview==144);REQUIRE(u32(h+88)==1);
    REQUIRE(u32(40)==(version==515?image:0));
    if(version==515){const auto color=u32(32);REQUIRE(color==preview+28+224*168*2);REQUIRE(table==color+28);REQUIRE(u32(color)==0);REQUIRE(u32(color+4)==16);REQUIRE(u32(color+24)==0);
        for(size_t i=0;i<16;++i)REQUIRE(bytes[color+8+i]==255);
    }else{REQUIRE(u32(32)==0);REQUIRE(table==preview+28+224*168*2);}
    REQUIRE(u32(table+12)==100);REQUIRE(u32(table+16)==3);REQUIRE(image==table+116);
    const uint32_t nonzero=format=="pws"?3:6;
    REQUIRE(f32(h+52)==Catch::Approx(nonzero*width*height*.2/32./1000.));
    uint32_t next=image;
    for(size_t i=0;i<3;++i){const auto at=table+20+i*32;REQUIRE(u32(at)==next);next+=u32(at+4);
        REQUIRE(f32(at+8)==Catch::Approx(i==0?5.:3.));REQUIRE(f32(at+12)==Catch::Approx(i==0?1.:1.5));
        REQUIRE(f32(at+16)==Catch::Approx(i==0?25.:i==1?14.:3.));REQUIRE(f32(at+20)==Catch::Approx(i==0?.1:.05));REQUIRE(u32(at+24)==nonzero);
    }
    REQUIRE(next==bytes.size());
    // Validation must precede replacement of a valid file.
    auto invalid=cfg;invalid.set("sla_lift_height2",1.);
    auto bad=SLAPrint::create_raster_job(invalid,document,{.1,.15,.2});
    REQUIRE_THROWS(bad->export_print(file.path.string()));REQUIRE(read()==bytes);
    if(geometry){invalid=cfg;invalid.set("display_width",cfg.opt_float("display_width")+1.);
        bad=SLAPrint::create_raster_job(invalid,document,{.1,.15,.2});
        REQUIRE_THROWS(bad->export_print(file.path.string()));REQUIRE(read()==bytes);}
}
