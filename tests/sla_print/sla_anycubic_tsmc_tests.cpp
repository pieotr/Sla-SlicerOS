#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <boost/filesystem.hpp>
#include <fstream>
#include <cstring>
#include <cstdlib>
#include "libslic3r/SLAPrint.hpp"
#include "libslic3r/Format/AnycubicSLA.hpp"
#include "libslic3r/SLA/LayerDocument.hpp"

using namespace Slic3r;

TEST_CASE("Anycubic modern versions store distinct two-stage bottom and normal motion", "[sla_reference][sla_anycubic_tsmc]")
{
    const std::string format=GENERATE("pwc", "dl2p", "pm5", "pm3n", "px6s", "pm5s", "m5sp");
    const bool portrait=GENERATE(false,true);
    const bool two_stage=GENERATE(false,true);
    CAPTURE(format,portrait,two_stage);
    DynamicPrintConfig cfg;cfg.apply(SLAFullPrintConfig{});
    cfg.option("printer_technology")->setInt(ptSLA);cfg.set("sla_archive_format",format);
    cfg.set("display_width",16.);cfg.set("display_height",4.);
    cfg.set("display_pixels_x",8);cfg.set("display_pixels_y",4);
    cfg.option("display_orientation")->setInt(portrait?sladoPortrait:sladoLandscape);
    cfg.set("display_mirror_x",false);cfg.set("display_mirror_y",false);cfg.set("max_print_height",200.);
    cfg.set("layer_height",.05);cfg.set("initial_layer_height",.1);
    cfg.set("sla_bottom_layers",1);cfg.set("sla_transition_layers",1);
    cfg.set("exposure_time",3.);cfg.set("initial_exposure_time",25.);
    cfg.set("sla_lift_height",3.);cfg.set("sla_bottom_lift_height",5.);
    cfg.set("sla_lift_height2",two_stage?2.5:0.);cfg.set("sla_bottom_lift_height2",two_stage?3.:0.);
    cfg.set("sla_lift_speed",90.);cfg.set("sla_bottom_lift_speed",60.);
    cfg.set("sla_lift_speed2",180.);cfg.set("sla_bottom_lift_speed2",120.);
    cfg.set("sla_retract_speed",150.);cfg.set("sla_bottom_retract_speed",210.);
    cfg.set("sla_retract_speed2",240.);cfg.set("sla_bottom_retract_speed2",300.);
    cfg.set("sla_wait_before_cure",1.25);cfg.set("sla_bottom_wait_before_cure",1.25);
    cfg.set("material_density",1.1);cfg.set("bottle_volume",500.);cfg.set("bottle_cost",50.);
    std::vector<uint8_t> pixels(32,0);
    pixels[9]=32;pixels[10]=64;pixels[11]=128;pixels[17]=192;pixels[18]=255;pixels[19]=85;
    const auto png=sla::PNGRasterEncoder{}(pixels.data(),portrait?4:8,portrait?8:4,1);
    const auto document=sla::LayerDocument::from_rasters({png,png,png});
    auto job=SLAPrint::create_raster_job(cfg,document,{.1,.15,.2});
    struct File {
        boost::filesystem::path path;bool keep=false;
        ~File(){if(!keep){boost::system::error_code error;boost::filesystem::remove(path,error);}}
    } file{boost::filesystem::unique_path("anycubic-tsmc-%%%%."+format)};
    if(const char *directory=std::getenv("SLA_ANYCUBIC_TSMC_DIRECTORY")) {
        boost::filesystem::create_directories(directory);
        file.path=boost::filesystem::path(directory)/((portrait?"portrait-":"landscape-")+std::string(two_stage?"two-stage":"single-stage")+"."+format);file.keep=true;
    }
    job->export_print(file.path.string());
    auto read=[&](){std::ifstream in(file.path.string(),std::ios::binary);return std::vector<uint8_t>{std::istreambuf_iterator<char>(in),{}};};
    const auto bytes=read();
    auto u32=[&](size_t p){REQUIRE(p+4<=bytes.size());return uint32_t(bytes[p])|(uint32_t(bytes[p+1])<<8)|(uint32_t(bytes[p+2])<<16)|(uint32_t(bytes[p+3])<<24);};
    auto f32=[&](size_t p){const auto bits=u32(p);float v;std::memcpy(&v,&bits,4);return v;};
    const uint32_t version=format=="pwc"?516:(format=="pm5s"||format=="m5sp")?518:517;
    REQUIRE(u32(12)==version);
    REQUIRE(u32(20)==(version==516?52:version==517?56:64));
    REQUIRE(u32(u32(20)+12)==(version==516?84:version==517?92:96));
    REQUIRE(u32(u32(44)+12)==(version==518?224:156));
    REQUIRE(u32(u32(44)+132)==(version==516?1:version==517?7:15));
    REQUIRE(u32(u32(44)+148)==(format=="pwc"?518:version));
    if(version==516) REQUIRE(u32(24)==0);
    else {
        REQUIRE(u32(24)>0);REQUIRE(u32(52)>0);
        REQUIRE(u32(u32(24)+32)==164);REQUIRE(u32(u32(52)+12)==48);
    }
    const auto h=u32(20),e=u32(40),table=u32(36);
    REQUIRE(f32(h+16)==Catch::Approx(2000.));
    REQUIRE(f32(h+40)==Catch::Approx(two_stage?5.5:3.));
    REQUIRE(f32(e+20)==Catch::Approx(5.));REQUIRE(f32(e+32)==Catch::Approx(two_stage?3.:0.));
    REQUIRE(f32(e+24)==Catch::Approx(1.));REQUIRE(f32(e+36)==Catch::Approx(2.));
    REQUIRE(f32(e+28)==Catch::Approx(5.));REQUIRE(f32(e+40)==Catch::Approx(3.5));
    REQUIRE(f32(e+48)==Catch::Approx(3.));REQUIRE(f32(e+60)==Catch::Approx(two_stage?2.5:0.));
    REQUIRE(f32(e+52)==Catch::Approx(1.5));REQUIRE(f32(e+64)==Catch::Approx(3.));
    REQUIRE(f32(e+56)==Catch::Approx(4.));REQUIRE(f32(e+68)==Catch::Approx(2.5));
    REQUIRE(f32(h+28)==Catch::Approx(1.25));
    for(size_t i=0;i<3;++i) {
        const auto at=table+20+i*32;
        REQUIRE(f32(at+8)==Catch::Approx(i==0?(two_stage?8.:5.):(two_stage?5.5:3.)));
        REQUIRE(f32(at+12)==Catch::Approx(i==0?1.:1.5));
        REQUIRE(f32(at+16)==Catch::Approx(i==0?25.:i==1?14.:3.));
    }
    // Invalid motion must never replace an existing output, including a full
    // valid two-stage job. The writer validates before creating staging data.
    for(const std::string key: {"sla_retract_height2", "sla_wait_after_cure", "sla_bottom_wait_after_lift"}) {
        auto invalid=cfg;invalid.set(key,1.);
        auto bad=SLAPrint::create_raster_job(invalid,document,{.1,.15,.2});
        REQUIRE_THROWS(bad->export_print(file.path.string()));REQUIRE(read()==bytes);
    }
    auto invalid=cfg;invalid.set("sla_lift_speed",0.);
    REQUIRE_THROWS_WITH(([&] {
        auto bad=SLAPrint::create_raster_job(invalid,document,{.1,.15,.2});
        bad->export_print(file.path.string());
    })(),Catch::Matchers::ContainsSubstring("motion speeds"));
    REQUIRE(read()==bytes);
}

TEST_CASE("Anycubic explicit printer version reaches native export and survives reload", "[sla_anycubic_versions]")
{
    const std::string format = GENERATE("pwc", "pm4n", "pwmx", "pwmo", "pwms", "pmsq", "dlp", "pwma", "pm3", "pm3m", "pwmb", "dl2p", "pmx2", "pm3r", "pm3n", "pm5", "px6s", "pm5s", "m5sp");
    std::vector<std::string> versions;
    for (uint16_t version : anycubic_available_versions(format))
        if (version >= 516) versions.push_back(std::to_string(version));
    const std::string requested = GENERATE_COPY(Catch::Generators::from_range(versions));
    CAPTURE(format, requested);
    DynamicPrintConfig cfg; cfg.apply(SLAFullPrintConfig{});
    cfg.option("printer_technology")->setInt(ptSLA);
    cfg.set("sla_archive_format", format);
    cfg.set("sla_anycubic_version", requested);
    cfg.set("display_width", 16.); cfg.set("display_height", 4.);
    cfg.set("display_pixels_x", 8); cfg.set("display_pixels_y", 4);
    cfg.option("display_orientation")->setInt(sladoLandscape);
    cfg.set("sla_wait_before_cure", 1.); cfg.set("sla_bottom_wait_before_cure", 1.);
    cfg.set("max_print_height",200.);
    cfg.set("layer_height",.05); cfg.set("initial_layer_height",.05);
    cfg.set("sla_bottom_layers",1); cfg.set("sla_transition_layers",0);
    cfg.set("exposure_time",3.); cfg.set("initial_exposure_time",25.);
    cfg.set("sla_lift_height",3.); cfg.set("sla_bottom_lift_height",5.);
    cfg.set("sla_lift_height2",2.5); cfg.set("sla_bottom_lift_height2",3.);
    cfg.set("sla_lift_speed",90.); cfg.set("sla_bottom_lift_speed",60.);
    cfg.set("sla_lift_speed2",180.); cfg.set("sla_bottom_lift_speed2",120.);
    cfg.set("sla_retract_speed",150.); cfg.set("sla_bottom_retract_speed",210.);
    cfg.set("sla_retract_speed2",240.); cfg.set("sla_bottom_retract_speed2",300.);

    struct Files {
        boost::filesystem::path output=boost::filesystem::unique_path("anycubic-version-%%%%.pwc");
        boost::filesystem::path config=boost::filesystem::unique_path("anycubic-version-%%%%.ini");
        bool keep=false;
        ~Files() {boost::system::error_code error; if(!keep) boost::filesystem::remove(output,error); boost::filesystem::remove(config,error);}
    } files;
    files.output.replace_extension(format);
    if (const char *directory=std::getenv("SLA_ANYCUBIC_VERSIONS_DIRECTORY")) {
        boost::filesystem::create_directories(directory);
        files.output=boost::filesystem::path(directory)/(requested+"."+format); files.keep=true;
    }
    cfg.save(files.config.string());
    DynamicPrintConfig loaded; loaded.apply(SLAFullPrintConfig{}); loaded.load(files.config.string(),ForwardCompatibilitySubstitutionRule::Disable);
    REQUIRE(loaded.opt_string("sla_anycubic_version") == requested);
    std::vector<uint8_t> pixels(32,255);
    const auto png=sla::PNGRasterEncoder{}(pixels.data(),8,4,1);
    const auto document=sla::LayerDocument::from_rasters({png});
    auto job=SLAPrint::create_raster_job(loaded,document,{.05});
    job->export_print(files.output.string());
    std::ifstream in(files.output.string(),std::ios::binary);
    std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(in),{}};
    REQUIRE(bytes.size()>16);
    REQUIRE((uint32_t(bytes[12])|(uint32_t(bytes[13])<<8)|(uint32_t(bytes[14])<<16)|(uint32_t(bytes[15])<<24))==std::stoul(requested));
    // A bad imported selection can be applied and inspected without crashing;
    // the error is reported before slicing, and the existing file is untouched.
    loaded.set("sla_anycubic_version",std::string("invalid"));
    SLAPrint bad;
    REQUIRE_NOTHROW(bad.apply(Model{},loaded));
    REQUIRE(bad.validate().find("Anycubic file version")!=std::string::npos);
    REQUIRE_THROWS(SLAPrint::create_raster_job(loaded,document,{.05}));
    auto invalid_writer=SLAArchiveWriter::create(format,bad.printer_config());
    REQUIRE_THROWS(invalid_writer->export_print(files.output.string(),*job,{}));
    std::ifstream unchanged(files.output.string(),std::ios::binary);
    REQUIRE(std::vector<uint8_t>{std::istreambuf_iterator<char>(unchanged),{}}==bytes);
}
