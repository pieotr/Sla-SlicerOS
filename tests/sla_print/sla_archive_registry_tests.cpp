#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <boost/filesystem.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/uuid/detail/sha1.hpp>
#include <fstream>
#include <cstring>
#include <chrono>
#include <iomanip>
#include <numeric>
#include <cstdlib>
#include <nlohmann/json.hpp>
#include <miniz.h>
#include "sla_archive_test_config.hpp"
#include "libslic3r/Preset.hpp"

#include "libslic3r/Format/SLAArchiveFormatRegistry.hpp"
#include "libslic3r/Format/SL1.hpp"
#include "libslic3r/Format/ZCodeRSA.hpp"
#include "libslic3r/Format/NanoDLPEncoder.hpp"
#include "libslic3r/Format/NanoDLPPacket.hpp"
#include "libslic3r/Format/NanoDLPContourStats.hpp"
#include "libslic3r/Format/NanoDLPSchema.hpp"
#include "libslic3r/Format/NanoDLPArchive.hpp"
#include "libslic3r/Format/ZCodeArchive.hpp"
#include <tbb/task_arena.h>
#include "libslic3r/Format/ChituboxArchive.hpp"
#include "libslic3r/Format/PhrozenArchive.hpp"
#include "libslic3r/Format/FDGArchive.hpp"
#include "libslic3r/Format/FlashforgeArchive.hpp"
#include "libslic3r/Format/SVGXContourPacket.hpp"
#include "libslic3r/Format/SVGXFile.hpp"
#include "libslic3r/Format/CTBEncryptedCrypto.hpp"
#include "libslic3r/Format/CTBEncryptedArchive.hpp"
#include "libslic3r/Format/ChituboxRasterEncoder.hpp"
#include "libslic3r/Format/LongerOrangeArchive.hpp"
#include "libslic3r/Format/MakerbaseArchive.hpp"
#include "libslic3r/Format/Emake3DArchive.hpp"
#include "libslic3r/Format/AnetArchive.hpp"
#include "libslic3r/Format/UVToolsCompatArchive.hpp"
#include "libslic3r/Format/OpenStandardsArchive.hpp"
#include "libslic3r/Format/AnycubicSLA.hpp"
#include "libslic3r/Format/CrealityCXDLPv4.hpp"
#include "libslic3r/Format/CrealityCXDLPv1Archive.hpp"
#include "libslic3r/Format/CrealityContourArea.hpp"
#include "libslic3r/Format/CrealityCXDLPv4Layout.hpp"
#include "libslic3r/Format/SL1_SVG.hpp"
#include "libslic3r/Format/ZipperArchiveImport.hpp"
#include "libslic3r/PNGReadWrite.hpp"
#include "libslic3r/Zipper.hpp"
#include "libslic3r/SLAPrint.hpp"
#include "libslic3r/SLA/LayerDocument.hpp"
#include "libslic3r/SLA/PrintParameters.hpp"

using namespace Slic3r;

TEST_CASE("Explicit layer export uses destination despite a legacy source format label", "[sla_reference][sla_export_destination]") {
    const std::string source_format = GENERATE("SL1 archive (.sl1)", "Creality CXDLPv4 (.cxdlpv4)", "cbddlp", "ctb_encrypted");
    const std::string destination = GENERATE("SL1", "cxdlpv4");
    CAPTURE(source_format, destination);
    SLAFullPrintConfig full;
    full.printer_technology.setInt(ptSLA);
    // CXDLP requires an explicit device identifier. This small raster tests
    // software routing only; it is not a certified physical CL-60 profile.
    full.set("printer_model",std::string("CL-60"));
    full.set("sla_archive_format", source_format);
    full.set("display_width", 4.); full.set("display_height", 3.);
    full.set("display_pixels_x", 8); full.set("display_pixels_y", 6);
    full.display_orientation.value = sladoLandscape;
    full.set("supports_enable", false); full.set("pad_enable", false);
    full.set("support_object_elevation", 0.); full.set("hollowing_enable", false);
    full.set("layer_height", .05); full.set("initial_layer_height", .05);
    DynamicPrintConfig config; config.apply(full);
    Model model;
    auto *object = model.add_object("export regression", "", make_cube(.2, .2, .2));
    object->add_instance()->set_offset(Vec3d(2., 1.5, 0.));
    SLAPrint print;
    print.set_status_callback([](const PrintBase::SlicingStatus &) {});
    print.apply(model, config); print.process();
    REQUIRE(print.finished()); REQUIRE(print.layer_document() != nullptr);
    auto edited = *print.layer_document(); edited.invert(0);
    struct File {
        boost::filesystem::path path;
        ~File() { boost::system::error_code error; boost::filesystem::remove(path, error); }
    } file{boost::filesystem::unique_path("export-destination-%%%%." + std::string(get_default_extension(destination.c_str())))};
    print.export_layer_document(edited, destination, file.path.string(), {}, "regression");
    const auto restored = sla::LayerDocument::load_archive(file.path.string());
    REQUIRE(restored.archive_format() == destination);
    REQUIRE(restored.layer_count() == edited.layer_count());
    auto expected = edited.image(0);
    if (destination == "cxdlpv4")
        for (auto &value : expected.buf)
            value = value < 2 ? 0 : uint8_t((value & 0xfe) | 1);
    const auto actual = restored.image(0);
    REQUIRE(actual.cols == expected.cols); REQUIRE(actual.rows == expected.rows);
    REQUIRE(actual.buf == expected.buf);
    REQUIRE(print.printer_config().sla_archive_format.value == source_format);
    REQUIRE_FALSE(print.layer_document()->dirty());
}

// Opt-in benchmark against a user-supplied SL1. Source bytes remain untouched.
TEST_CASE("Measure real SL1 export stages", "[.sla_export_benchmark]") {
    const char *source = std::getenv("SLA_EXPORT_BENCHMARK_INPUT");
    const char *folder = std::getenv("SLA_EXPORT_BENCHMARK_OUTPUT");
    REQUIRE(source != nullptr); REQUIRE(folder != nullptr);
    boost::filesystem::create_directories(folder);
    using Clock = std::chrono::steady_clock;
    const auto start = Clock::now();
    auto document = sla::LayerDocument::load_sl1(source);
    DynamicPrintConfig config; config.apply(SLAFullPrintConfig{});
    SL1Reader reader(source, SLAImportQuality::Balanced, {}); reader.read(config);
    config.set("sla_archive_format", std::string("SL1"));
    std::vector<double> levels;
    for (size_t i=0;i<document.layer_count();++i)
        levels.push_back(config.opt_float("initial_layer_height")+i*config.opt_float("layer_height"));
    const auto loaded = Clock::now();
    auto job = SLAPrint::create_raster_job(config, document, levels);
    const auto created = Clock::now();
    nlohmann::json report{{"layers",document.layer_count()},
        {"load_ms",std::chrono::duration<double,std::milli>(loaded-start).count()},
        {"statistics_ms",std::chrono::duration<double,std::milli>(created-loaded).count()}};
    for (const std::string format : {"SL1","cxdlpv4"}) {
        const auto begin = Clock::now();
        const auto path = boost::filesystem::path(folder)/("export."+std::string(get_default_extension(format.c_str())));
        job->export_layer_document(document, format, path.string(), {}, "benchmark");
        report[format] = {{"export_ms",std::chrono::duration<double,std::milli>(Clock::now()-begin).count()},
                          {"bytes",boost::filesystem::file_size(path)}};
    }
    std::ofstream output(boost::filesystem::path(folder).string()+"/timings.json");
    output << report.dump(2) << '\n'; REQUIRE(output.good());
}

// Opt-in geometry benchmark: it runs the actual mesh pipeline and fingerprints
// every resulting PNG. Compare the same machine/config before and after edits;
// this measures neither firmware compatibility nor an artificial mask-only job.
TEST_CASE("Measure native SLA mesh slicing", "[.sla_mesh_benchmark]") {
    nlohmann::json reports=nlohmann::json::array();
    for (const bool tapered : {false,true}) {
        Model model; auto mesh=make_cube(10.,10.,10.);
        if (tapered) for (auto &vertex : mesh.its.vertices)
            if (vertex.z()>5.) vertex.x() *= .5;
        auto *object=model.add_object("benchmark","",mesh);
        object->add_instance()->set_offset(Vec3d(30.,30.,0.));
        SLAFullPrintConfig full; full.printer_technology.setInt(ptSLA);
        full.set("sla_archive_format",std::string("uvj"));
        full.set("display_width",200.); full.set("display_height",120.);
        full.set("display_pixels_x",4000); full.set("display_pixels_y",2400);
        full.display_orientation.value=sladoLandscape;
        full.set("supports_enable",false); full.set("pad_enable",false);
        full.set("support_object_elevation",0.); full.set("hollowing_enable",false);
        full.set("layer_height",.05); full.set("initial_layer_height",.05);
        DynamicPrintConfig config; config.apply(full);
        SLAPrint print; print.set_status_callback([](const PrintBase::SlicingStatus &){});
        const auto start=std::chrono::steady_clock::now();
        print.apply(model,config); print.process();
        const auto stop=std::chrono::steady_clock::now();
        REQUIRE(print.finished()); REQUIRE(print.layer_document()!=nullptr);
        const auto first=print.layer_document()->image(0);
        REQUIRE(first.cols==4000); REQUIRE(first.rows==2400);
        nlohmann::json hashes=nlohmann::json::array(), heights=nlohmann::json::array(), legacy_hashes=nlohmann::json::array();
        size_t identical_adjacent=0;
        for(size_t i=0;i<print.print_layers().size();++i) {
            const auto png=print.layer_document()->encoded_layer(i);
            boost::uuids::detail::sha1 digest; digest.process_bytes(png.buf,png.sz);
            boost::uuids::detail::sha1::digest_type words; digest.get_digest(words);
            std::ostringstream hex; hex<<std::hex<<std::setfill('0');
            for (const auto word:words) hex<<std::setw(8)<<word;
            hashes.push_back(hex.str()); heights.push_back(double(print.print_layers()[i].level())*SCALING_FACTOR);
            if(std::getenv("SLA_MESH_BENCHMARK_LEGACY_PNG")) {
                const auto image=print.layer_document()->image(i);
                size_t size=0;
                void *legacy=tdefl_write_image_to_png_file_in_memory(image.buf.data(),int(image.cols),int(image.rows),1,&size);
                REQUIRE(legacy!=nullptr);
                boost::uuids::detail::sha1 old_digest;old_digest.process_bytes(legacy,size);mz_free(legacy);
                old_digest.get_digest(words);
                std::ostringstream old_hex;old_hex<<std::hex<<std::setfill('0');
                for(const auto word:words)old_hex<<std::setw(8)<<word;
                legacy_hashes.push_back(old_hex.str());
            }

            if(i && print.print_layers()[i].transformed_slices()==print.print_layers()[i-1].transformed_slices())
                ++identical_adjacent;
        }
        reports.push_back({{"shape",tapered?"taper":"prism"},{"resolution",{4000,2400}},
            {"total_ms",std::chrono::duration<double,std::milli>(stop-start).count()},
            {"identical_adjacent_geometry",identical_adjacent},{"positions_z",heights},{"png_sha1",hashes},{"legacy_png_sha1",legacy_hashes}});
    }
    const char *path=std::getenv("SLA_MESH_BENCHMARK_PATH");
    std::ofstream output(path?path:"sla-mesh-benchmark.json"); output<<reports.dump(2)<<'\n'; REQUIRE(output.good());
}

TEST_CASE("Raster reuse draws only representatives and validates copy dependencies", "[sla_reference]") {
    struct Archive : SL1Archive {
        using SL1Archive::SL1Archive;
        const auto &layers() const {return m_layers;}
    };
    SLAPrinterConfig config; config.display_width.value=1.; config.display_height.value=1.;
    config.display_pixels_x.value=2; config.display_pixels_y.value=2;
    config.display_orientation.value=sladoLandscape; config.gamma_correction.value=1.;
    Archive archive(config); std::vector<sla::EncodedRaster> masks;
    const std::vector<size_t> representatives{0,0,2,2};
    std::atomic<unsigned> draws{0};
    archive.draw_layers(4,[&](sla::RasterBase &raster,size_t i) {
        ++draws;
        if(i==0) raster.draw(ExPolygon(Polygon(Points{Point(0,0),Point(scaled(1.),0),
            Point(scaled(1.),scaled(1.)),Point(0,scaled(1.))})));
    },[]{return false;},ExecutionTBB{},&masks,&representatives);
    REQUIRE(draws==2); REQUIRE(archive.layers().size()==4);
    auto document=sla::LayerDocument::from_rasters(std::move(masks));
    REQUIRE(document.image(0).buf==document.image(1).buf);
    REQUIRE(document.image(2).buf==document.image(3).buf);
    REQUIRE(document.image(0).buf!=document.image(2).buf);
    for(size_t i=0;i<4;++i) {
        const auto png=document.encoded_layer(i);
        const auto &native=archive.layers()[i];
        REQUIRE(native.size()==png.sz); REQUIRE(std::memcmp(native.data(),png.buf,png.sz)==0);
    }
    for(const std::vector<size_t> invalid : {std::vector<size_t>{0}, {1,1,2,2}, {0,0,1,2}})
        REQUIRE_THROWS_AS(archive.draw_layers(4,[](sla::RasterBase &,size_t){},[]{return false;},
            ExecutionTBB{},nullptr,&invalid),std::invalid_argument);
}

TEST_CASE("Anycubic PWS and PW0 masks use their own RLE and versioned table addresses", "[sla_reference][sla_anycubic_binary]") {
    const std::string format = GENERATE("pws", "pw0", "pwmb", "dl2p", "pwc", "pm5", "pm3n", "px6s", "pm5s", "m5sp");
    const bool portrait = GENERATE(false, true);
    CAPTURE(format, portrait);
    DynamicPrintConfig config; config.apply(SLAFullPrintConfig{});
    config.option("printer_technology")->setInt(ptSLA); config.set("sla_archive_format", format);
    const bool machine_table = format == "pwc" || format == "dl2p" || format == "pm5" || format == "pm3n" || format == "px6s" || format == "pm5s" || format == "m5sp";
    const auto *legacy = machine_table ? nullptr : anycubic_legacy_geometry(format);
    const double width = legacy ? legacy->width : 9., height = legacy ? legacy->height : 2.;
    config.set("display_width", legacy && portrait ? height : width);
    config.set("display_height", legacy && portrait ? width : height);
    config.set("max_print_height", legacy ? legacy->machine_z : 200.);
    config.set("display_pixels_x", 6); config.set("display_pixels_y", 2);
    config.option("display_orientation")->setInt(portrait ? sladoPortrait : sladoLandscape);
    config.set("display_mirror_x", false); config.set("display_mirror_y", false);
    config.set("layer_height", .05); config.set("initial_layer_height", .1);
    config.set("sla_bottom_layers", 1); config.set("sla_transition_layers", 1);
    config.set("sla_lift_speed", 90.); config.set("sla_bottom_lift_speed", 60.);
    config.set("sla_retract_speed", 150.); config.set("sla_bottom_retract_speed", 150.);
    config.set("sla_retract_speed2", 210.); config.set("sla_bottom_retract_speed2", 270.);
    config.set("sla_wait_before_cure", 1.5); config.set("sla_bottom_wait_before_cure", 1.5);
    config.set("material_density", 1.2); config.set("bottle_volume", 1000.); config.set("bottle_cost", 60.);
    const std::vector<uint8_t> pixels{0,1,15,16,127,128,254,255,85,170,255,0};
    const auto mask = sla::PNGRasterEncoder{}(pixels.data(), portrait ? 2 : 6, portrait ? 6 : 2, 1);
    auto document = sla::LayerDocument::from_rasters({mask,mask,mask});
    auto job = SLAPrint::create_raster_job(config, document, {.1,.15,.2});
    struct File {
        boost::filesystem::path path; bool keep=false;
        ~File() { if (!keep) {boost::system::error_code error; boost::filesystem::remove(path,error);} }
    } file{boost::filesystem::unique_path("anycubic-reference-%%%%."+format)};
    if (const char *directory = std::getenv("SLA_ANYCUBIC_REFERENCE_DIRECTORY")) {
        boost::filesystem::create_directories(directory);
        file.path=boost::filesystem::path(directory)/((portrait?"portrait-":"landscape-")+format+"."+format); file.keep=true;
    }
    ThumbnailData thumbnail; thumbnail.set(4,2);
    std::fill(thumbnail.pixels.begin(),thumbnail.pixels.end(),0);
    for (size_t pixel=0;pixel<8;++pixel) {
        thumbnail.pixels[pixel*4+(pixel<4?2:0)]=255;
        thumbnail.pixels[pixel*4+3]=255;
    }
    thumbnail.pixels[4*4+3]=128;
    job->export_print(file.path.string(), {thumbnail});
    std::ifstream input(file.path.string(),std::ios::binary);
    const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(input),{}};
    auto le = [&](size_t p) { REQUIRE(p+4<=bytes.size()); return uint32_t(bytes[p])|(uint32_t(bytes[p+1])<<8)|(uint32_t(bytes[p+2])<<16)|(uint32_t(bytes[p+3])<<24); };
    auto f32 = [&](size_t p) { const auto bits=le(p);float value;std::memcpy(&value,&bits,4);return value; };
    const uint32_t version=(format=="pm5s" || format=="m5sp")?518:(format=="dl2p" || format=="pm5" || format=="pm3n" || format=="px6s")?517:format=="pwc"?516:format=="pwmb"?515:1;
    REQUIRE(le(12)==version); REQUIRE(le(20)==(version>=518?64:version>=517?56:version>=516?52:48));
    const auto header=le(20), table=le(36);
    const auto preview=le(28);
    REQUIRE(le(preview+16)==224); REQUIRE(le(preview+24)==168);
    REQUIRE(std::any_of(bytes.begin()+preview+28, bytes.begin()+preview+28+224*168*2, [](uint8_t value) {return value != 0;}));
    auto preview_pixel = [&](size_t x,size_t y) {
        const size_t offset=preview+28+2*(y*224+x);
        return uint16_t(bytes[offset])|(uint16_t(bytes[offset+1])<<8);
    };
    REQUIRE(preview_pixel(112,0)==0); REQUIRE(preview_pixel(112,27)==0);
    REQUIRE(preview_pixel(112,28)==0xf800); REQUIRE(preview_pixel(112,83)==0xf800);
    REQUIRE(preview_pixel(112,84)==0x001f); REQUIRE(preview_pixel(112,139)==0x001f);
    REQUIRE(preview_pixel(112,140)==0); REQUIRE(preview_pixel(112,167)==0);
    REQUIRE(preview_pixel(28,28)==0x8000);
    if (version >= 516) {
        const auto machine = le(44);
        REQUIRE(std::string(bytes.begin()+machine,bytes.begin()+machine+7) == "MACHINE");
        REQUIRE(le(machine+12)==(version>=518?224:156));
        REQUIRE(f32(machine+136)==Catch::Approx(portrait?height:width));
        REQUIRE(f32(machine+140)==Catch::Approx(portrait?width:height));
        REQUIRE(f32(machine+144)==Catch::Approx(200.));
    }
    REQUIRE(le(header+60)==(portrait?2:6)); REQUIRE(le(header+64)==(portrait?6:2));
    REQUIRE(le(table+16)==3);
    const bool pws=format=="pws";
    const std::vector<uint8_t> expected=pws?std::vector<uint8_t>{0,0,0,0,0,255,255,255,0,255,255,0}:
        std::vector<uint8_t>{0,0,0,17,119,136,255,255,85,170,255,0};
    const uint32_t nonzero=pws?5:8;
    const double volume_ml=nonzero*.2*width*height/12./1000.;
    REQUIRE(f32(header+52)==Catch::Approx(volume_ml));
    REQUIRE(f32(header+68)==Catch::Approx(volume_ml*1.2));
    for (size_t layer=0;layer<3;++layer) {
        const auto entry=table+20+32*layer;
        const auto offset=le(entry), length=le(entry+4);
        REQUIRE(uint64_t(offset)+length<=bytes.size());
        REQUIRE(le(entry+24)==nonzero);
        REQUIRE(f32(entry+20)==Catch::Approx(layer? .05:.1).margin(1e-7));
        std::vector<uint8_t> decoded;
        for (size_t p=offset;p<offset+length;++p) {
            const unsigned code=pws?bytes[p]>>7:bytes[p]>>4;
            unsigned run=bytes[p]&(pws?127:15);
            if (!pws&&(code==0||code==15)) { REQUIRE(p+1<offset+length);run=(run<<8)|bytes[++p]; }
            REQUIRE(run>0); REQUIRE(decoded.size()+run<=pixels.size());
            decoded.insert(decoded.end(),run,pws?(code?255:0):uint8_t(code*17));
        }
        REQUIRE(decoded==expected);
    }
    if (version == 518) {
        REQUIRE(le(16)==11);
        REQUIRE(le(header+12)==96); REQUIRE(le(header+96)==1);
        REQUIRE(le(header+108)==0); // Explicit exposure, no intelligent override.
        REQUIRE(le(header+88)==1); // One transition layer.
        const auto extra=le(40), machine=le(44), software=le(24), model=le(52), sub=le(56), second=le(60);
        REQUIRE(std::string(bytes.begin()+extra,bytes.begin()+extra+5)=="EXTRA");
        REQUIRE(le(extra+12)==24); REQUIRE(extra+72==machine);
        REQUIRE(f32(extra+20)==Catch::Approx(6.));
        REQUIRE(f32(extra+48)==Catch::Approx(6.));
        REQUIRE(f32(extra+24)==Catch::Approx(1.)); REQUIRE(f32(extra+28)==Catch::Approx(4.5));
        REQUIRE(f32(extra+40)==Catch::Approx(2.5));
        REQUIRE(f32(extra+52)==Catch::Approx(1.5)); REQUIRE(f32(extra+56)==Catch::Approx(3.5));
        REQUIRE(f32(extra+68)==Catch::Approx(2.5));
        REQUIRE(le(machine+132)==15); REQUIRE(machine+224==software);
        REQUIRE(le(software+32)==164); REQUIRE(software+164==model);
        REQUIRE(std::string(bytes.begin()+model,bytes.begin()+model+5)=="MODEL");
        REQUIRE(le(model+12)==48); REQUIRE(f32(model+36)==Catch::Approx(.2));
        REQUIRE(model+48==sub);
        REQUIRE(std::string(bytes.begin()+sub,bytes.begin()+sub+7)=="SUBIMGS");
        REQUIRE(le(sub+12)==24+3*44); REQUIRE(le(sub+16)==3); REQUIRE(le(sub+20)==1);
        for(size_t i=0;i<3;++i) {
            REQUIRE(le(sub+24+i*44)==le(table+20+i*32));
            REQUIRE(le(sub+28+i*44)==le(table+24+i*32));
            REQUIRE(le(sub+32+i*44)==8);
        }
        REQUIRE(sub+24+3*44==second);
        REQUIRE(std::string(bytes.begin()+second,bytes.begin()+second+8)=="PREVIEW2");
        REQUIRE(le(second+12)==28+330*190*2);
        REQUIRE(le(second+16)==330); REQUIRE(le(second+20)==4293); REQUIRE(le(second+24)==190);
        REQUIRE(second+28+330*190*2==le(48));
        auto second_pixel=[&](size_t x,size_t y) {
            const size_t p=second+28+2*(y*330+x);return uint16_t(bytes[p])|(uint16_t(bytes[p+1])<<8);
        };
        REQUIRE(second_pixel(165,0)==0); REQUIRE(second_pixel(165,12)==0xf800);
        REQUIRE(second_pixel(165,95)==0x001f); REQUIRE(second_pixel(165,176)==0x001f);
        REQUIRE(second_pixel(165,177)==0);
        config.set("sla_light_pwm",128);
        auto incompatible=SLAPrint::create_raster_job(config,document,{.1,.15,.2});
        REQUIRE_THROWS_WITH(incompatible->export_print(file.path.string()),Catch::Matchers::ContainsSubstring("custom UV PWM"));
        std::ifstream unchanged(file.path.string(),std::ios::binary);
        REQUIRE(std::vector<uint8_t>{std::istreambuf_iterator<char>(unchanged),{}}==bytes);
        config.set("sla_light_pwm",255);
    }
    if (legacy) {
        config.set("display_width", config.opt_float("display_width")+1.);
        auto incompatible = SLAPrint::create_raster_job(config, document, {.1,.15,.2});
        REQUIRE_THROWS_WITH(incompatible->export_print(file.path.string()),
            Catch::Matchers::ContainsSubstring("cannot store custom machine dimensions"));
        std::ifstream unchanged(file.path.string(),std::ios::binary);
        REQUIRE(std::vector<uint8_t>{std::istreambuf_iterator<char>(unchanged),{}} == bytes);
    }
}

TEST_CASE("PWS splits binary runs at 127 pixels", "[sla_reference][sla_anycubic_binary]") {
    DynamicPrintConfig config; config.apply(SLAFullPrintConfig{});
    config.option("printer_technology")->setInt(ptSLA); config.set("sla_archive_format",std::string("pws"));
    config.set("display_width",68.04); config.set("display_height",120.96);
    config.set("max_print_height",165.); config.set("display_pixels_x",512); config.set("display_pixels_y",1);
    config.option("display_orientation")->setInt(sladoLandscape);
    config.set("layer_height",.05); config.set("initial_layer_height",.05);
    std::vector<uint8_t> pixels(512,0); std::fill(pixels.begin()+300,pixels.begin()+500,255);
    auto document=sla::LayerDocument::from_rasters({sla::PNGRasterEncoder{}(pixels.data(),512,1,1)});
    auto job=SLAPrint::create_raster_job(config,document,{.05});
    struct File { boost::filesystem::path path=boost::filesystem::unique_path("pws-runs-%%%%.pws");
        ~File(){boost::system::error_code error;boost::filesystem::remove(path,error);} } file;
    job->export_print(file.path.string());
    std::ifstream input(file.path.string(),std::ios::binary);
    const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(input),{}};
    auto le=[&](size_t p){REQUIRE(p+4<=bytes.size());return uint32_t(bytes[p])|(uint32_t(bytes[p+1])<<8)|(uint32_t(bytes[p+2])<<16)|(uint32_t(bytes[p+3])<<24);};
    const auto table=le(36), offset=le(table+20), length=le(table+24);
    REQUIRE(length==6); REQUIRE(offset+length==bytes.size());
    const std::vector<uint8_t> encoded(bytes.begin()+offset,bytes.begin()+offset+length);
    REQUIRE(encoded==std::vector<uint8_t>{127,127,46,255,201,12});
}

TEST_CASE("Image-and-geometry ZIP families preserve their independent schemas", "[sla_reference][sla_genericzip][sla_vda]") {
    const std::string format = GENERATE("genericzip", "vda");
    const bool portrait = GENERATE(false, true);
    DynamicPrintConfig config; config.apply(SLAFullPrintConfig{});
    config.option("printer_technology")->setInt(ptSLA); config.set("sla_archive_format", format);
    config.set("display_width", 100.); config.set("display_height", 60.);
    config.set("display_pixels_x", 3); config.set("display_pixels_y", 2);
    config.option("display_orientation")->setInt(portrait ? sladoPortrait : sladoLandscape);
    config.set("display_mirror_x", false); config.set("display_mirror_y", false);
    config.set("layer_height", .05); config.set("initial_layer_height", .05);
    const std::vector<uint8_t> pixels{0,1,127,128,254,255};
    const auto raster = sla::PNGRasterEncoder{}(pixels.data(), portrait ? 2 : 3, portrait ? 3 : 2, 1);
    const std::vector<uint8_t> tenth{17,18,19,20,21,22};
    auto rasters = std::vector<sla::EncodedRaster>(12, raster);
    rasters[9] = sla::PNGRasterEncoder{}(tenth.data(), portrait ? 2 : 3, portrait ? 3 : 2, 1);
    auto document = sla::LayerDocument::from_rasters(std::move(rasters));
    std::vector<double> heights; for (size_t i=0;i<12;++i) heights.push_back((i+1)*.05);
    auto job = SLAPrint::create_raster_job(config, document, heights);
    struct File {
        boost::filesystem::path path = boost::filesystem::unique_path("generic-reference-%%%%.zip");
        bool keep = false;
        ~File() { if (!keep) { boost::system::error_code error; boost::filesystem::remove(path,error); } }
    } file;
    if (const char *requested = std::getenv(format == "vda" ? "SLA_VDA_REFERENCE_PATH" : "SLA_GENERICZIP_REFERENCE_PATH")) { file.path=requested; file.keep=true; }
    job->export_print(file.path.string());
    auto restored = sla::LayerDocument::load_archive(file.path.string());
    REQUIRE(restored.archive_format() == format); REQUIRE(restored.layer_count() == 12);
    for (size_t i=0;i<12;++i) REQUIRE(restored.image(i).buf == (i == 9 ? tenth : pixels));
    restored.invert(1); restored.save_archive(file.path.string());
    auto edited = sla::LayerDocument::load_archive(file.path.string());
    REQUIRE(edited.image(0).buf == pixels);
    REQUIRE(edited.image(1).buf == std::vector<uint8_t>{255,254,128,127,1,0});
    REQUIRE(edited.image(9).buf == tenth);
    REQUIRE_THROWS(edited.save_archive(file.path.string()+".sl1"));
    const auto zip = read_zipper_archive(file.path.string(), {""}, {});
    REQUIRE(std::none_of(zip.entries.begin(),zip.entries.end(),[](const auto &entry) { return entry.fname == "config.ini"; }));
    const auto manifest = std::find_if(zip.entries.begin(),zip.entries.end(),[&](const auto &entry) { return entry.fname == (format == "vda" ? "sla_job.xml" : "manifest.uvtools"); });
    REQUIRE(manifest != zip.entries.end());
    const std::string xml(manifest->buf.begin(),manifest->buf.end());
    REQUIRE(xml.find(format == "vda" ? (portrait ? "<Resolution>2*3P</Resolution>" : "<Resolution>3*2P</Resolution>") :
        (portrait ? "<ResolutionX>2</ResolutionX>" : "<ResolutionX>3</ResolutionX>")) != std::string::npos);
    REQUIRE(xml.find(format == "vda" ? (portrait ? "<XLength>60</XLength>" : "<XLength>100</XLength>") :
        (portrait ? "<DisplayWidth>60</DisplayWidth>" : "<DisplayWidth>100</DisplayWidth>")) != std::string::npos);
    heights[0] = .04;
    auto variable = SLAPrint::create_raster_job(config, document, heights);
    REQUIRE_THROWS(variable->export_print(file.path.string()));
    REQUIRE(sla::LayerDocument::load_archive(file.path.string()).image(1).buf == edited.image(1).buf);
}

TEST_CASE("Chitubox ZIP keeps numeric masks and distinct layer motion", "[sla_reference][sla_chitubox_zip]") {
    const bool portrait=GENERATE(false,true);
    DynamicPrintConfig config; config.apply(SLAFullPrintConfig{});
    config.option("printer_technology")->setInt(ptSLA); config.set("sla_archive_format",std::string("chitubox_zip"));
    config.set("display_width",100.); config.set("display_height",60.);
    config.set("display_pixels_x",3); config.set("display_pixels_y",2);
    config.option("display_orientation")->setInt(portrait?sladoPortrait:sladoLandscape);
    config.set("layer_height",.05); config.set("initial_layer_height",.1);
    config.set("sla_bottom_layers",1); config.set("sla_transition_layers",2);
    config.set("exposure_time",2.); config.set("initial_exposure_time",20.);
    config.set("sla_lift_height",5.); config.set("sla_bottom_lift_height",3.);
    config.set("sla_lift_height2",2.); config.set("sla_bottom_lift_height2",1.);
    config.set("sla_lift_speed",90.); config.set("sla_bottom_lift_speed",60.);
    config.set("sla_lift_speed2",120.); config.set("sla_bottom_lift_speed2",80.);
    config.set("sla_retract_speed",150.); config.set("sla_bottom_retract_speed",100.);
    config.set("sla_retract_height2",1.); config.set("sla_bottom_retract_height2",.5);
    config.set("sla_retract_speed2",90.); config.set("sla_bottom_retract_speed2",60.);
    config.set("sla_wait_before_cure",.7); config.set("sla_bottom_wait_before_cure",1.2);
    config.set("sla_wait_after_cure",.4); config.set("sla_bottom_wait_after_cure",.6);
    config.set("sla_wait_after_lift",.3); config.set("sla_bottom_wait_after_lift",.8);
    config.set("sla_light_pwm",170); config.set("sla_bottom_light_pwm",250);
    config.set("material_density",1.2);config.set("bottle_volume",1000.);config.set("bottle_cost",60.);
    const std::vector<uint8_t> pixels{0,1,127,128,254,255}, tenth{17,18,19,20,21,22};
    auto masks=std::vector<sla::EncodedRaster>(12,sla::PNGRasterEncoder{}(pixels.data(),portrait?2:3,portrait?3:2,1));
    masks[9]=sla::PNGRasterEncoder{}(tenth.data(),portrait?2:3,portrait?3:2,1);
    auto document=sla::LayerDocument::from_rasters(std::move(masks));
    std::vector<double> heights;for(size_t i=0;i<12;++i) heights.push_back(.1+i*.05);
    auto job=SLAPrint::create_raster_job(config,document,heights);
    struct File{boost::filesystem::path path=boost::filesystem::unique_path("chitubox-%%%%.zip");bool keep=false;
        ~File(){if(!keep){boost::system::error_code error;boost::filesystem::remove(path,error);}}}file;
    if(const char *directory=std::getenv("SLA_CHITUBOX_ZIP_REFERENCE_DIRECTORY")){
        boost::filesystem::create_directories(directory);file.path=boost::filesystem::path(directory)/(portrait?"portrait.zip":"landscape.zip");file.keep=true;
    }
    job->export_print(file.path.string());
    auto restored=sla::LayerDocument::load_archive(file.path.string());
    REQUIRE(restored.archive_format()=="chitubox_zip");REQUIRE(restored.layer_count()==12);
    for(size_t i=0;i<12;++i) REQUIRE(restored.image(i).buf==(i==9?tenth:pixels));
    const auto zip=read_zipper_archive(file.path.string(),{""},{});
    auto sequence=std::find_if(zip.entries.begin(),zip.entries.end(),[](const auto &entry){return entry.fname=="run.gcode";});
    REQUIRE(sequence!=zip.entries.end()); const std::string text(sequence->buf.begin(),sequence->buf.end());
    REQUIRE(text.find(";totalLayer:12\n")!=std::string::npos);
    REQUIRE(text.find(";mirror:0\n")!=std::string::npos);
    REQUIRE(text.find("G0 Z3.1 F60\nG0 Z4.1 F80\nG4 P00\nG4 P800\nG0 Z0.6 F100\nG0 Z0.1 F60\n")!=std::string::npos);
    REQUIRE(text.find("M106 S250\nG4 P20000\nM106 S0\nG4 P600\n")!=std::string::npos);
    REQUIRE(text.find("M106 S170\nG4 P2000\nM106 S0\nG4 P400\n")!=std::string::npos);
    restored.invert(1);restored.save_archive(file.path.string());
    auto edited=sla::LayerDocument::load_archive(file.path.string());
    REQUIRE(edited.image(1).buf==std::vector<uint8_t>{255,254,128,127,1,0});
    REQUIRE(edited.image(9).buf==tenth);REQUIRE_THROWS(edited.save_archive(file.path.string()+".sl1"));
    File volume_file;
    auto cleared=restored.image(0);std::fill(cleared.buf.begin(),cleared.buf.end(),0);
    restored.replace_image(0,cleared);restored.save_archive(volume_file.path.string());
    const auto volume_zip=read_zipper_archive(volume_file.path.string(),{""},{});
    const auto volume_entry=std::find_if(volume_zip.entries.begin(),volume_zip.entries.end(),[](const auto &entry){return entry.fname=="run.gcode";});
    REQUIRE(volume_entry!=volume_zip.entries.end());
    const std::string updated(volume_entry->buf.begin(),volume_entry->buf.end());
    REQUIRE(updated.find(";volume:2.8\n")!=std::string::npos);
    REQUIRE(updated.find(";weight:3.36\n")!=std::string::npos);
    REQUIRE(updated.find(";price:0.168\n")!=std::string::npos);
    REQUIRE(updated.substr(updated.find("G21\n"))==text.substr(text.find("G21\n")));
    File invalid;
    for(const int scenario:{0,1,2,3}) {
        CAPTURE(scenario);
        {
            Zipper output(invalid.path.string());
            for(const auto &entry:zip.entries) {
                if(scenario==1 && entry.fname=="10.png") continue;
                if(entry.fname=="run.gcode" && (scenario==0 || scenario==3)) {
                    auto bad=text;
                    if(scenario==0) bad+=";totalLayer:12\n";
                    else {
                        const auto start=bad.find(";resolutionX:")+12, end=bad.find('\n',start);
                        bad.replace(start,end-start,"1.5");
                    }
                    output.add_entry(entry.fname); output<<bad;
                } else output.add_entry(entry.fname,entry.buf.data(),entry.buf.size());
            }
            if(scenario==2) {const auto png=document.encoded_layer(0);output.add_entry("01.png",png.buf,png.sz);}
            output.finalize();
        }
        REQUIRE_THROWS_AS(sla::LayerDocument::load_archive(invalid.path.string()),RuntimeError);
    }
}

TEST_CASE("CXDLPv4 editing relocates sections and retains encrypted layer parameters", "[sla_reference][sla_cxdlpv4_editor]") {
    const bool portrait=GENERATE(false,true);
    const uint32_t seed=GENERATE(0u,0x12345678u,0xffffffffu);
    DynamicPrintConfig config;config.apply(SLAFullPrintConfig{});
    config.option("printer_technology")->setInt(ptSLA);config.set("sla_archive_format",std::string("cxdlpv4"));
    config.set("display_width",6.);config.set("display_height",4.);
    config.set("display_pixels_x",3);config.set("display_pixels_y",2);
    config.option("display_orientation")->setInt(portrait?sladoPortrait:sladoLandscape);
    config.set("layer_height",.05);config.set("initial_layer_height",.1);
    config.set("sla_bottom_layers",1);config.set("sla_transition_layers",1);
    config.set("sla_wait_before_cure",.25);config.set("sla_bottom_wait_before_cure",.75);
    const std::vector<uint8_t> pixels{0,1,127,128,254,255};
    const std::vector<uint8_t> expected{0,0,127,129,255,255};
    const auto png=sla::PNGRasterEncoder{}(pixels.data(),portrait?2:3,portrait?3:2,1);
    const auto masks=sla::LayerDocument::from_rasters({png,png,png});
    auto job=SLAPrint::create_raster_job(config,masks,{.1,.15,.2});
    struct File {boost::filesystem::path path=boost::filesystem::unique_path("v4-editor-%%%%.cxdlpv4");bool keep=false;
        ~File(){if(!keep){boost::system::error_code error;boost::filesystem::remove(path,error);}}}source,edited;
    if(const char *directory=std::getenv("SLA_CXDLPV4_EDITOR_REFERENCE_DIRECTORY")) {
        boost::filesystem::create_directories(directory);
        const auto stem=std::string(portrait?"portrait":"landscape")+"-"+std::to_string(seed);
        source.path=boost::filesystem::path(directory)/(stem+".cxdlpv4");source.keep=true;
        edited.path=boost::filesystem::path(directory)/(stem+"-edited.cxdlpv4");edited.keep=true;
    }
    job->export_print(source.path.string());
    auto read=[](const boost::filesystem::path &path){std::ifstream input(path.string(),std::ios::binary);return std::vector<uint8_t>{std::istreambuf_iterator<char>(input),{}};};
    auto bytes=read(source.path);
    auto write=[&](const boost::filesystem::path &path,const auto &data){std::ofstream output(path.string(),std::ios::binary|std::ios::trunc);output.write(reinterpret_cast<const char *>(data.data()),data.size());};
    auto checksum=[](auto &data){const auto sum=sla::creality_checksum(data.data(),data.size()-4,4);for(unsigned i=0;i<4;++i)data[data.size()-4+i]=uint8_t(sum>>(24-8*i));};
    const sla::CrealityCXDLPv4Layout original(bytes);
    if(seed) {
        for(unsigned i=0;i<4;++i)bytes[original.base+68+i]=uint8_t(seed>>(8*i));
        for(size_t i=0;i<original.layers.size();++i) {
            const auto &layer=original.layers[i];std::vector<uint8_t> payload(bytes.begin()+layer.data+44,bytes.begin()+layer.data+layer.length);
            CrealityCXDLPv4Archive::crypt_mask(payload,seed,uint32_t(i));
            std::copy(payload.begin(),payload.end(),bytes.begin()+layer.data+44);
        }
        checksum(bytes);write(source.path,bytes);
    }
    auto imported=sla::LayerDocument::load_archive(source.path.string());
    REQUIRE(imported.archive_format()=="cxdlpv4");REQUIRE(imported.layer_count()==3);
    REQUIRE(imported.image(0).buf==expected);REQUIRE(imported.image(2).buf==expected);
    imported.save_archive(edited.path.string());REQUIRE(read(edited.path)==bytes);
    imported.invert(0);REQUIRE(imported.image(0).buf==std::vector<uint8_t>{255,255,129,127,0,0});
    REQUIRE(imported.undo());REQUIRE(imported.image(0).buf==expected);REQUIRE(imported.redo());
    imported.save_archive(edited.path.string());REQUIRE_FALSE(imported.dirty());
    auto restored=sla::LayerDocument::load_archive(edited.path.string());
    REQUIRE(restored.image(0).buf==imported.image(0).buf);
    REQUIRE(restored.image(1).buf==expected);REQUIRE(restored.image(2).buf==expected);
    const auto saved=read(edited.path);const sla::CrealityCXDLPv4Layout layout(saved);
    REQUIRE(layout.seed==seed);REQUIRE(layout.layers.size()==3);
    for(size_t i=0;i<3;++i) {
        REQUIRE(std::equal(bytes.begin()+original.layers[i].data,bytes.begin()+original.layers[i].data+44,saved.begin()+layout.layers[i].data));
        REQUIRE(std::equal(bytes.begin()+original.table+40*i,bytes.begin()+original.table+40*i+12,saved.begin()+layout.table+40*i));
    }
    for(const size_t field:{original.base+28,original.base+40}) {
        const auto old=original.integer(field),current=layout.integer(field);
        const auto image=original.integer(old+8),length=original.integer(old+12),new_image=layout.integer(current+8);
        REQUIRE(std::equal(bytes.begin()+image,bytes.begin()+image+length,saved.begin()+new_image));
    }
    REQUIRE_THROWS(imported.save_archive(edited.path.string()+".cxdlp"));
    // Changing mask size moves the slicer section; old per-layer parameters survive.
    REQUIRE(restored.transform_layers(0,0,sla::LayerDocument::MaskOperation::Fill,255));
    restored.save_archive(edited.path.string());
    const auto filled=read(edited.path);const sla::CrealityCXDLPv4Layout filled_layout(filled);
    REQUIRE(filled_layout.layers[0].length<layout.layers[0].length);
    REQUIRE(filled_layout.integer(filled_layout.base+72)<layout.integer(layout.base+72));
    REQUIRE(sla::LayerDocument::load_archive(edited.path.string()).image(0).buf==std::vector<uint8_t>(6,255));
    const auto parameters=filled_layout.integer(filled_layout.base+52);
    REQUIRE(filled_layout.number(parameters+20)==Catch::Approx(.004));
    // Repeated saving must not append old payloads or change metadata again.
    restored.save_archive(edited.path.string());REQUIRE(read(edited.path)==filled);
    REQUIRE(restored.transform_layers(0,0,sla::LayerDocument::MaskOperation::Fill,127));
    REQUIRE(restored.dirty());
    REQUIRE_THROWS(restored.save_archive(edited.path.string()+".cxdlp"));
    REQUIRE(restored.dirty());REQUIRE(read(edited.path)==filled);
    File unavailable;
    boost::filesystem::create_directory(unavailable.path);
    REQUIRE_THROWS(restored.save_archive(unavailable.path.string()));
    REQUIRE(restored.dirty());REQUIRE(boost::filesystem::is_directory(unavailable.path));
    if(seed==0) {
        // Some files deduplicate identical payloads. Editing one reference must
        // split the shared data without changing the other layer's pixels.
        auto shared=bytes;
        for(unsigned i=0;i<4;++i) {
            shared[original.table+40+12+i]=uint8_t(original.layers[0].data>>(8*i));
            shared[original.table+40+16+i]=uint8_t(original.layers[0].length>>(8*i));
        }
        checksum(shared);File aliased,split;write(aliased.path,shared);
        auto document=sla::LayerDocument::load_archive(aliased.path.string());
        document.invert(0);document.save_archive(split.path.string());
        auto reloaded=sla::LayerDocument::load_archive(split.path.string());
        REQUIRE(reloaded.image(0).buf==document.image(0).buf);
        REQUIRE(reloaded.image(1).buf==expected);REQUIRE(reloaded.image(2).buf==expected);
        const auto split_bytes=read(split.path);const sla::CrealityCXDLPv4Layout split_layout(split_bytes);
        REQUIRE(split_layout.layers[0].data!=split_layout.layers[1].data);
        document.save_archive(split.path.string());REQUIRE(read(split.path)==split_bytes);
    }
    File broken;
    for(unsigned scenario=0;scenario<9;++scenario) {
        auto invalid=bytes;
        auto put=[&](size_t offset,uint32_t value){for(unsigned i=0;i<4;++i)invalid[offset+i]=uint8_t(value>>(8*i));};
        if(scenario==0)invalid.back()^=1;
        if(scenario==1) {invalid[original.table+16]=43;invalid[original.table+17]=invalid[original.table+18]=invalid[original.table+19]=0;checksum(invalid);}
        if(scenario==2) {invalid[original.table+20]=1;checksum(invalid);}
        if(scenario==3) {for(unsigned i=0;i<4;++i)invalid[original.table+12+i]=uint8_t(original.table>>(8*i));checksum(invalid);}
        if(scenario==4) {put(original.base+36,0);checksum(invalid);}
        if(scenario==5) {const auto preview=original.integer(original.base+28);put(preview+8,uint32_t(preview+16));checksum(invalid);}
        if(scenario==6) {put(original.table+4,0x7fc00000);checksum(invalid);}
        if(scenario==7) {put(original.table+16,45);checksum(invalid);}
        if(scenario==8) {put(original.table+40+12,original.layers[0].data+1);checksum(invalid);}
        write(broken.path,invalid);REQUIRE_THROWS_AS(sla::LayerDocument::load_archive(broken.path.string()),RuntimeError);
    }
}

TEST_CASE("CXDLPv4 RLE handles every run prefix and rejects incomplete images", "[sla_reference][sla_cxdlpv4_editor]") {
    const std::vector<std::pair<size_t,std::vector<uint8_t>>> samples{
        {1,{127}},{2,{255,2}},{127,{255,127}},{128,{255,128,128}},
        {16383,{255,191,255}},{16384,{255,192,64,0}},
        {2097151,{255,223,255,255}},{2097152,{255,224,32,0,0}}};
    for(const auto &[count,expected]:samples) {
        CAPTURE(count);
        size_t columns=std::min<size_t>(count,32768);while(count%columns)--columns;
        const size_t rows=count/columns;std::vector<uint8_t> pixels(count,255);
        const auto encoded=CrealityCXDLPv4Archive::encode_mask(pixels.data(),columns,rows,1);
        const auto *data=static_cast<const uint8_t *>(encoded.data());
        REQUIRE(std::vector<uint8_t>(data,data+encoded.size())==expected);
        REQUIRE(CrealityCXDLPv4Archive::decode_mask(data,encoded.size(),columns,rows)==pixels);
    }
    std::vector<uint8_t> levels(256);for(unsigned i=0;i<256;++i)levels[i]=uint8_t(i);
    const auto encoded=CrealityCXDLPv4Archive::encode_mask(levels.data(),256,1,1);
    const auto decoded=CrealityCXDLPv4Archive::decode_mask(static_cast<const uint8_t *>(encoded.data()),encoded.size(),256,1);
    for(unsigned i=0;i<256;++i)REQUIRE(decoded[i]==(i<2?0:uint8_t((i&254)|1)));
    for(const auto &invalid:std::vector<std::vector<uint8_t>>{{},{128},{128,0},{128,240},{128,128},{255,2}})
        REQUIRE_THROWS_AS(CrealityCXDLPv4Archive::decode_mask(invalid.data(),invalid.size(),1,1),RuntimeError);
    const uint8_t white=127;
    REQUIRE_THROWS(CrealityCXDLPv4Archive::decode_mask(&white,1,2,1));
    REQUIRE_THROWS(CrealityCXDLPv4Archive::decode_mask(nullptr,1,1,1));
    REQUIRE_THROWS(CrealityCXDLPv4Archive::encode_mask(&white,1,1,3));
}

TEST_CASE("CXDLPv4 previews use the complete header and a separate RGB15 repeat bit", "[sla_reference][sla_cxdlpv4_preview]") {
    DynamicPrintConfig config;config.apply(SLAFullPrintConfig{});
    config.option("printer_technology")->setInt(ptSLA);config.set("sla_archive_format",std::string("cxdlpv4"));
    config.set("display_width",6.);config.set("display_height",4.);
    config.set("display_pixels_x",3);config.set("display_pixels_y",2);
    config.option("display_orientation")->setInt(sladoLandscape);
    config.set("layer_height",.05);config.set("initial_layer_height",.05);
    const std::vector<uint8_t> pixels{0,1,127,128,254,255};
    const auto mask=sla::PNGRasterEncoder{}(pixels.data(),3,2,1);
    const auto document=sla::LayerDocument::from_rasters({mask});
    auto job=SLAPrint::create_raster_job(config,document,{.05});
    ThumbnailData image;image.set(2,3);
    image.pixels={0,0,255,128,0,0,255,128,0,255,0,255,0,255,0,255,255,0,0,255,255,0,0,255};
    struct File {boost::filesystem::path path=boost::filesystem::unique_path("preview-%%%%.cxdlpv4");bool keep=false;
        ~File(){if(!keep){boost::system::error_code error;boost::filesystem::remove(path,error);}}}file;
    if(const char *directory=std::getenv("SLA_CXDLPV4_REFERENCE_DIRECTORY")) {
        boost::filesystem::create_directories(directory);file.path=boost::filesystem::path(directory)/"rgb-preview.cxdlpv4";file.keep=true;
    }
    job->export_print(file.path.string(),{ThumbnailData{},image});
    std::ifstream input(file.path.string(),std::ios::binary);
    const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(input),{}};
    auto integer=[&](size_t offset){REQUIRE(offset+4<=bytes.size());return uint32_t(bytes[offset])|(uint32_t(bytes[offset+1])<<8)|(uint32_t(bytes[offset+2])<<16)|(uint32_t(bytes[offset+3])<<24);};
    const size_t model_size=(uint32_t(bytes[15])<<24)|(uint32_t(bytes[16])<<16)|(uint32_t(bytes[17])<<8)|bytes[18];
    const size_t base=19+model_size;
    const size_t params=integer(base+52);
    const auto volume_bits=integer(params+20);float volume;std::memcpy(&volume,&volume_bits,4);
    REQUIRE(volume==Catch::Approx(.0008)); // Four decoded foreground pixels, not five canonical ones.
    const size_t table=integer(base+32);
    REQUIRE(integer(table+28)==2000); // Largest contour after gray 1 becomes zero.
    for(const auto location:{base+28,base+40}) {
        const auto preview=integer(location),width=integer(preview),height=integer(preview+4),address=integer(preview+8),length=integer(preview+12);
        REQUIRE(width==height);REQUIRE(address==preview+32);REQUIRE(length<width*height*2);
        for(unsigned field=0;field<4;++field)REQUIRE(integer(preview+16+4*field)==0);
        std::vector<uint16_t> decoded;
        for(size_t cursor=address;cursor<address+length;) {
            REQUIRE(cursor+2<=address+length);
            const uint16_t pixel=uint16_t(bytes[cursor])|(uint16_t(bytes[cursor+1])<<8);cursor+=2;
            size_t run=1;
            if(pixel&0x20) {
                REQUIRE(cursor+2<=address+length);
                run+=uint16_t(bytes[cursor])|((uint16_t(bytes[cursor+1])&15)<<8);cursor+=2;
            }
            REQUIRE(decoded.size()+run<=width*height);
            decoded.insert(decoded.end(),run,pixel&~0x20);
        }
        REQUIRE(decoded.size()==width*height);
        auto rgb=[&](unsigned x,unsigned y){return decoded[size_t(y)*width+x];};
        const unsigned left=width/6;
        REQUIRE(rgb(0,0)==0);REQUIRE(rgb(left,0)==0xf800);
        REQUIRE(rgb(left,height/3)==0x07c0);REQUIRE(rgb(left,2*height/3)==0x0010);
    }
}

TEST_CASE("CXDLPv2 and v3 store physical contour area and checksums the complete file", "[sla_reference][sla_cxdlpv3]") {
    const bool portrait=GENERATE(false,true);
    const uint16_t version=GENERATE(2,3);
    const bool direct=GENERATE(false,true);
    CAPTURE(version,portrait,direct);
    DynamicPrintConfig config;config.apply(SLAFullPrintConfig{});
    config.option("printer_technology")->setInt(ptSLA);
    config.set("sla_archive_format",std::string(direct?(version==2?"cxdlpv2":"cxdlp"):"SL1"));config.set("printer_model",std::string("CL-60"));
    config.set("display_width",6.);config.set("display_height",4.);
    config.set("display_pixels_x",3);config.set("display_pixels_y",2);
    config.option("display_orientation")->setInt(portrait?sladoPortrait:sladoLandscape);
    config.set("layer_height",.05);config.set("initial_layer_height",.05);
    config.set("exposure_time",2.5);config.set("initial_exposure_time",20.);
    config.set("sla_bottom_layers",1);config.set("sla_transition_layers",0);
    config.set("sla_lift_height",6.);config.set("sla_bottom_lift_height",4.);
    config.set("sla_lift_speed",120.);config.set("sla_bottom_lift_speed",60.);
    config.set("sla_retract_speed",180.);config.set("sla_bottom_retract_speed",180.);
    config.set("sla_wait_before_cure",1.);config.set("sla_bottom_wait_before_cure",1.);
    config.set("sla_light_pwm",170);config.set("sla_bottom_light_pwm",250);
    const std::vector<uint8_t> pixels{0,1,127,128,254,255};
    const auto mask=sla::PNGRasterEncoder{}(pixels.data(),portrait?2:3,portrait?3:2,1);
    const auto document=sla::LayerDocument::from_rasters({mask,mask});
    auto job=SLAPrint::create_raster_job(config,document,{.05,.1});
    struct File {boost::filesystem::path path=boost::filesystem::unique_path("cxdlpv3-%%%%.cxdlp");bool keep=false;
        ~File(){if(!keep){boost::system::error_code error;boost::filesystem::remove(path,error);}}}file;
    if(version==2)file.path=boost::filesystem::unique_path("cxdlpv2-%%%%.v2.cxdlp");
    if(const char *directory=std::getenv("SLA_CXDLPV3_REFERENCE_DIRECTORY")) {
        boost::filesystem::create_directories(directory);
        file.path=boost::filesystem::path(directory)/(std::string(portrait?"portrait":"landscape")+(version==2?".v2.cxdlp":".cxdlp"));file.keep=true;
    }
    job->export_print(file.path.string());
    std::ifstream input(file.path.string(),std::ios::binary);
    const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(input),{}};
    size_t cursor=0;
    auto be32=[&](){REQUIRE(cursor+4<=bytes.size());auto v=(uint32_t(bytes[cursor])<<24)|(uint32_t(bytes[cursor+1])<<16)|(uint32_t(bytes[cursor+2])<<8)|bytes[cursor+3];cursor+=4;return v;};
    auto be16=[&](){REQUIRE(cursor+2<=bytes.size());auto v=(uint16_t(bytes[cursor])<<8)|bytes[cursor+1];cursor+=2;return v;};
    auto magic=[&](){REQUIRE(be32()==9);REQUIRE(std::string(bytes.begin()+cursor,bytes.begin()+cursor+9)==std::string("CXSW3DV2\0",9));cursor+=9;};
    auto page=[&](){REQUIRE(be16()==0x0d0a);};
    auto string=[&](){const auto size=be32();REQUIRE(size>0);REQUIRE(cursor+size<=bytes.size());REQUIRE(bytes[cursor+size-1]==0);std::string s(bytes.begin()+cursor,bytes.begin()+cursor+size-1);cursor+=size;return s;};
    magic();REQUIRE(be16()==version);REQUIRE(string()=="CL-60");REQUIRE(be16()==2);
    REQUIRE(be16()==(portrait?2:3));REQUIRE(be16()==(portrait?3:2));cursor+=64;
    for(const unsigned size:{116,290,290}) {
        const auto end=cursor+size*size*2;REQUIRE(end<bytes.size());
        REQUIRE(std::any_of(bytes.begin()+cursor,bytes.begin()+end,[](uint8_t v){return v!=0;}));cursor=end;page();
    }
    auto numeric=[&](){const auto size=be32();REQUIRE(size%2==0);std::string value;for(size_t i=0;i<size/2;++i)value.push_back(char(be16()));return value;};
    REQUIRE(numeric()==(portrait?"4":"6"));REQUIRE(numeric()==(portrait?"6":"4"));REQUIRE(numeric()=="0.05");
    for(const uint16_t value:{25,1,20,1,4,1,6,2,3,250,170}) REQUIRE(be16()==value);
    // The five foreground pixels form a 1.5-pixel-centre contour in both orientations.
    REQUIRE(sla::creality_largest_contour_area(pixels.data(),portrait?2:3,portrait?3:2)==1.5);
    REQUIRE(be32()==6000);REQUIRE(be32()==6000);page();
    const std::vector<uint8_t> v3{0,0x58,2,0,0,0xe0,0x93,4,0,0,0,0,0,0xe8,3,1,1,255,0,2,13,10};
    if(version>=3) {
    REQUIRE(string()=="SLA-Slicer");REQUIRE(string().empty());
    REQUIRE(std::equal(v3.begin(),v3.end(),bytes.begin()+cursor));cursor+=v3.size();
    }
    for(unsigned layer=0;layer<2;++layer) {
        REQUIRE(be32()==6000);const auto lines=be32();std::vector<uint8_t> decoded(6,0);
        const size_t columns=portrait?2:3,rows=portrait?3:2;
        for(unsigned line=0;line<lines;++line) {
            REQUIRE(cursor+6<=bytes.size());const auto *p=bytes.data()+cursor;
            const size_t start=(((uint32_t(p[0])<<8)|p[1])>>3)&0x1fff;
            const size_t end=(((uint32_t(p[1])<<16)|(uint32_t(p[2])<<8)|p[3])>>6)&0x1fff;
            const size_t x=((uint32_t(p[3])<<8)|p[4])&0x3fff;
            REQUIRE(start<=end);REQUIRE(end<rows);REQUIRE(x<columns);
            for(size_t y=start;y<=end;++y){REQUIRE(decoded[y*columns+x]==0);decoded[y*columns+x]=p[5];}
            cursor+=6;
        }
        REQUIRE(decoded==pixels);page();
    }
    magic();REQUIRE(cursor+4==bytes.size());
    uint32_t crc=0;
    for(size_t i=0;i<cursor;++i) {crc^=bytes[i];for(unsigned bit=0;version>=3 && bit<8;++bit)crc=(crc&1)?(crc>>1)^0xedb88320u:crc>>1;}
    REQUIRE(be32()==crc);REQUIRE(cursor==bytes.size());
    auto imported=sla::LayerDocument::load_archive(file.path.string());
    REQUIRE(imported.archive_format()==(version==2?"cxdlpv2":"cxdlp"));
    REQUIRE(imported.image(0).buf==pixels);REQUIRE(imported.image(1).buf==pixels);
    imported.invert(0);REQUIRE(imported.undo());REQUIRE(imported.redo());
    File edited;edited.path=file.path.string()+"-edited"+(version==2?".v2.cxdlp":".cxdlp");
    if(file.keep)edited.keep=true;
    imported.save_archive(edited.path.string());REQUIRE_FALSE(imported.dirty());
    const auto restored=sla::LayerDocument::load_archive(edited.path.string());
    REQUIRE(restored.image(0).buf==std::vector<uint8_t>{255,254,128,127,1,0});REQUIRE(restored.image(1).buf==pixels);
    // Prefix and all informational metadata survive binary editing verbatim.
    std::ifstream edited_input(edited.path.string(),std::ios::binary);
    const std::vector<uint8_t> edited_bytes{std::istreambuf_iterator<char>(edited_input),{}};
    const auto metadata_end=std::search(bytes.begin(),bytes.end(),v3.begin(),v3.end());
    const size_t prefix_end=version==3?size_t(metadata_end-bytes.begin())+v3.size():size_t(15+10+6+64+2*(116*116+290*290+290*290)+6);
    if(version==3)REQUIRE(std::equal(bytes.begin(),bytes.begin()+prefix_end,edited_bytes.begin()));
    REQUIRE(std::equal(bytes.end()-17,bytes.end()-4,edited_bytes.end()-17));
    File broken;
    for(const unsigned scenario:{0,1,2}) {
        auto corrupt=bytes;
        if(scenario==0)corrupt[100]^=1;
        if(scenario==1)corrupt.back()^=1;
        if(scenario==2)corrupt.pop_back();
        {std::ofstream stream(broken.path.string(),std::ios::binary|std::ios::trunc);stream.write(reinterpret_cast<const char *>(corrupt.data()),corrupt.size());}
        REQUIRE_THROWS_AS(sla::LayerDocument::load_archive(broken.path.string()),RuntimeError);
    }
    for(const std::string key:{"exposure_time","sla_lift_speed","sla_wait_before_cure","sla_lift_height2","initial_layer_height","printer_model"}) {
        auto bad=config;
        if(key=="printer_model")bad.set(key,std::string("SL1"));
        else bad.set(key,key=="exposure_time"?2.55:key=="sla_lift_speed"?90.:key=="sla_wait_before_cure"?0.:key=="initial_layer_height"?.1:1.);
        auto invalid=SLAPrint::create_raster_job(bad,document,key=="initial_layer_height"?std::vector<double>{.1,.15}:std::vector<double>{.05,.1});
        REQUIRE_THROWS(invalid->export_print(file.path.string()));
        std::ifstream preserved(file.path.string(),std::ios::binary);
        REQUIRE(std::vector<uint8_t>{std::istreambuf_iterator<char>(preserved),{}}==bytes);
    }
}

TEST_CASE("CXDLPv1 preserves inclusive grayscale runs and original big-endian tables", "[sla_reference][sla_cxdlpv1]") {
    const bool portrait=GENERATE(false,true), direct=GENERATE(false,true);
    CAPTURE(portrait,direct);
    DynamicPrintConfig config;config.apply(SLAFullPrintConfig{});
    config.option("printer_technology")->setInt(ptSLA);
    config.set("sla_archive_format",std::string(direct?"cxdlpv1":"SL1"));
    config.set("display_width",6.);config.set("display_height",4.);
    config.set("display_pixels_x",3);config.set("display_pixels_y",2);
    config.option("display_orientation")->setInt(portrait?sladoPortrait:sladoLandscape);
    config.set("layer_height",.05);config.set("initial_layer_height",.05);
    config.set("exposure_time",3.);config.set("initial_exposure_time",20.);
    config.set("sla_bottom_layers",1);config.set("sla_transition_layers",0);
    config.set("sla_lift_height",6.);config.set("sla_bottom_lift_height",4.);
    config.set("sla_lift_speed",90.);config.set("sla_bottom_lift_speed",60.);
    config.set("sla_retract_speed",150.);config.set("sla_bottom_retract_speed",150.);
    config.set("sla_wait_before_cure",1.);config.set("sla_bottom_wait_before_cure",1.);
    config.set("sla_light_pwm",170);config.set("sla_bottom_light_pwm",250);
    const std::vector<uint8_t> pixels{0,1,127,128,254,255};
    const auto mask=sla::PNGRasterEncoder{}(pixels.data(),portrait?2:3,portrait?3:2,1);
    const auto document=sla::LayerDocument::from_rasters({mask,mask});
    auto job=SLAPrint::create_raster_job(config,document,{.05,.1});
    struct File {boost::filesystem::path path=boost::filesystem::unique_path("cxdlpv1-%%%%.v1.cxdlp");bool keep=false;
        ~File(){if(!keep){boost::system::error_code error;boost::filesystem::remove(path,error);}}}file;
    if(const char *directory=std::getenv("SLA_CXDLPV1_REFERENCE_DIRECTORY")) {
        boost::filesystem::create_directories(directory);
        file.path=boost::filesystem::path(directory)/(portrait?"portrait.v1.cxdlp":"landscape.v1.cxdlp");file.keep=true;
    }
    job->export_print(file.path.string());
    std::ifstream input(file.path.string(),std::ios::binary);
    const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(input),{}};
    size_t cursor=0;
    auto be32=[&](){REQUIRE(cursor+4<=bytes.size());auto v=(uint32_t(bytes[cursor])<<24)|(uint32_t(bytes[cursor+1])<<16)|(uint32_t(bytes[cursor+2])<<8)|bytes[cursor+3];cursor+=4;return v;};
    auto be16=[&](){REQUIRE(cursor+2<=bytes.size());auto v=(uint16_t(bytes[cursor])<<8)|bytes[cursor+1];cursor+=2;return v;};
    auto magic=[&](){REQUIRE(be32()==9);REQUIRE(std::string(bytes.begin()+cursor,bytes.begin()+cursor+9)==std::string("CXSW3DV2\0",9));cursor+=9;};
    auto page=[&](){REQUIRE(be16()==0x0d0a);};
    magic();REQUIRE(be16()==2);REQUIRE(be16()==(portrait?2:3));REQUIRE(be16()==(portrait?3:2));
    for(const unsigned size:{116,290,290}){
        const auto end=cursor+size*size*2;REQUIRE(end<bytes.size());
        REQUIRE(std::any_of(bytes.begin()+cursor,bytes.begin()+end,[](uint8_t v){return v!=0;}));cursor=end;page();
    }
    auto text=[&](){const auto size=be32();REQUIRE(size%2==0);std::string value;for(size_t i=0;i<size/2;++i)value.push_back(char(be16()));return value;};
    REQUIRE(text()==(portrait?"4":"6"));REQUIRE(text()==(portrait?"6":"4"));REQUIRE(text()=="0.05");
    for(const uint16_t value:{3,1,20,1,4,60,6,90,150,250,170}) REQUIRE(be16()==value);
    const size_t prefix_end=cursor;
    REQUIRE(be32()==5);REQUIRE(be32()==5);page();
    for(size_t layer=0;layer<2;++layer){
        REQUIRE(be32()==5);const auto lines=be32();std::vector<uint8_t> decoded(6,0);
        const size_t cols=portrait?2:3,rows=portrait?3:2;
        for(size_t line=0;line<lines;++line){
            REQUIRE(cursor+6<=bytes.size());const auto *p=bytes.data()+cursor;
            const size_t start=(((uint32_t(p[0])<<8)|p[1])>>3)&0x1fff;
            const size_t end=(((uint32_t(p[1])<<16)|(uint32_t(p[2])<<8)|p[3])>>6)&0x1fff;
            const size_t x=((uint32_t(p[3])<<8)|p[4])&0x3fff;
            REQUIRE(start<=end);REQUIRE(end<rows);REQUIRE(x<cols);
            for(size_t y=start;y<=end;++y){REQUIRE(decoded[y*cols+x]==0);decoded[y*cols+x]=p[5];}
            cursor+=6;
        }
        REQUIRE(decoded==pixels);page();
    }
    magic();REQUIRE(be32()==7);REQUIRE(cursor==bytes.size());
    auto imported=sla::LayerDocument::load_archive(file.path.string());
    REQUIRE(imported.archive_format()=="cxdlpv1");REQUIRE(imported.layer_count()==2);
    REQUIRE(imported.image(0).buf==pixels);REQUIRE(imported.image(1).buf==pixels);
    imported.invert(0);REQUIRE(imported.undo());REQUIRE(imported.redo());
    File edited;
    if(const char *directory=std::getenv("SLA_CXDLPV1_EDITOR_REFERENCE_DIRECTORY")) {
        boost::filesystem::create_directories(directory);
        edited.path=boost::filesystem::path(directory)/(portrait?"portrait-edited.v1.cxdlp":"landscape-edited.v1.cxdlp");edited.keep=true;
    }
    imported.save_archive(edited.path.string());REQUIRE_FALSE(imported.dirty());
    REQUIRE_THROWS(imported.save_archive(edited.path.string()+".cxdlp"));
    auto restored=sla::LayerDocument::load_archive(edited.path.string());
    REQUIRE(restored.image(0).buf==std::vector<uint8_t>{255,254,128,127,1,0});REQUIRE(restored.image(1).buf==pixels);
    std::ifstream edited_input(edited.path.string(),std::ios::binary);
    const std::vector<uint8_t> edited_bytes{std::istreambuf_iterator<char>(edited_input),{}};
    REQUIRE(std::equal(bytes.begin(),bytes.begin()+prefix_end,edited_bytes.begin()));
    REQUIRE(std::equal(bytes.end()-17,bytes.end(),edited_bytes.end()-17));
    File broken;
    for(const unsigned scenario:{0,1,2,3}) {
        auto corrupt=bytes;
        if(scenario==0) {corrupt[15]=0;corrupt[16]=0;}
        if(scenario==1) corrupt[prefix_end+3]=6;
        if(scenario==2) corrupt[corrupt.size()-17+4]='Z';
        if(scenario==3) corrupt.pop_back();
        {std::ofstream output(broken.path.string(),std::ios::binary|std::ios::trunc);output.write(reinterpret_cast<const char *>(corrupt.data()),corrupt.size());}
        REQUIRE_THROWS_AS(sla::LayerDocument::load_archive(broken.path.string()),RuntimeError);
    }
    config.set("sla_archive_format",std::string("cxdlpv1"));
    for(const std::string key:{"exposure_time","sla_wait_before_cure","sla_lift_height2","initial_layer_height"}) {
        auto incompatible=config;incompatible.set(key,key=="sla_lift_height2"?1.:key=="sla_wait_before_cure"?0.:key=="initial_layer_height"?.1:2.5);
        auto invalid=SLAPrint::create_raster_job(incompatible,document,key=="initial_layer_height"?std::vector<double>{.1,.15}:std::vector<double>{.05,.1});
        REQUIRE_THROWS(invalid->export_print(file.path.string()));
        std::ifstream preserved(file.path.string(),std::ios::binary);
        REQUIRE(std::vector<uint8_t>{std::istreambuf_iterator<char>(preserved),{}}==bytes);
    }
}

TEST_CASE("CXDLPv1 retains maximal coordinates and long grayscale runs", "[sla_reference][sla_cxdlpv1]") {
    for(const bool vertical:{false,true}) {
        const size_t cols=vertical?2:16384,rows=vertical?8192:1;
        std::vector<uint8_t> pixels(cols*rows,0);
        if(vertical)for(size_t y=0;y<rows;++y)pixels[y*cols]=42;
        else pixels.back()=253;
        const auto encoded=CrealityCXDLPv1Archive::encode_mask(pixels.data(),cols,rows,1);
        REQUIRE(encoded.size()==14);const auto *p=static_cast<const uint8_t *>(encoded.data())+8;
        const size_t y0=(((uint32_t(p[0])<<8)|p[1])>>3)&0x1fff;
        const size_t y1=(((uint32_t(p[1])<<16)|(uint32_t(p[2])<<8)|p[3])>>6)&0x1fff;
        const size_t x=((uint32_t(p[3])<<8)|p[4])&0x3fff;
        REQUIRE(y0==0);REQUIRE(y1==(vertical?8191:0));REQUIRE(x==(vertical?0:16383));
        REQUIRE(p[5]==(vertical?42:253));
        std::vector<uint8_t> decoded(cols*rows,0);for(size_t y=y0;y<=y1;++y)decoded[y*cols+x]=p[5];
        REQUIRE(decoded==pixels);
    }
    uint8_t pixel=0;
    REQUIRE_THROWS(CrealityCXDLPv1Archive::encode_mask(&pixel,16385,1,1));
    REQUIRE_THROWS(CrealityCXDLPv1Archive::encode_mask(&pixel,1,8193,1));
    REQUIRE_THROWS(CrealityCXDLPv1Archive::encode_mask(&pixel,1,1,3));
}

TEST_CASE("Anycubic ZIP variants preserve masks geometry and per-layer parameters", "[sla_reference][sla_anycubic_zip]")
{
    const std::string format = GENERATE("pm4u", "pm7", "pm7m", "pwsz");
    const bool portrait = GENERATE(false, true);
    CAPTURE(format, portrait);
    REQUIRE(archive_export_unavailable_reason(format.c_str()).empty());
    Model model;
    auto *object = model.add_object("zip-reference", "", make_cube(4., 3., 0.3));
    object->add_instance()->set_offset(Vec3d(10., 10., 0.));
    SLAFullPrintConfig full;
    full.printer_technology.setInt(ptSLA);
    full.display_orientation.value = portrait ? sladoPortrait : sladoLandscape;
    full.set("sla_archive_format", format);
    full.set("display_width", 100.); full.set("display_height", 60.);
    full.set("display_pixels_x", 200); full.set("display_pixels_y", 100);
    full.set("supports_enable", false); full.set("pad_enable", false);
    full.set("support_object_elevation", 0.);
    full.set("layer_height", 0.05); full.set("initial_layer_height", 0.1);
    full.set("sla_bottom_layers", 1); full.set("sla_transition_layers", 2);
    full.set("sla_lift_height", 7.); full.set("sla_bottom_lift_height", 4.);
    full.set("sla_lift_height2", 2.); full.set("sla_bottom_lift_height2", 1.);
    full.set("sla_lift_speed", 90.); full.set("sla_bottom_lift_speed", 60.);
    full.set("sla_retract_speed", 150.); full.set("sla_bottom_retract_speed", 120.);
    full.set("sla_wait_before_cure", 0.7); full.set("sla_bottom_wait_before_cure", 0.7);
    DynamicPrintConfig config; config.apply(full);
    SLAPrint print; print.set_status_callback([](const PrintBase::SlicingStatus &) {});
    print.apply(model, config); print.process();
    auto edited = *print.layer_document();
    auto image = edited.image(0);
    // All 256 input gray levels, long black/white and intermediate-gray runs.
    for (size_t i = 0; i < 256; ++i) image.buf[i] = uint8_t(i);
    std::fill(image.buf.begin()+256, image.buf.begin()+5000, 255);
    std::fill(image.buf.begin()+5000, image.buf.begin()+5100, 127);
    edited.replace_image(0, image);
    auto blank = edited.image(edited.layer_count()-1);
    std::fill(blank.buf.begin(), blank.buf.end(), 0);
    edited.replace_image(edited.layer_count()-1, blank);
    print.replace_layer_document(edited);
    struct File {
        boost::filesystem::path path;
        ~File() { boost::system::error_code error; boost::filesystem::remove(path, error); }
    } file{boost::filesystem::unique_path("anycubic-zip-%%%%." + format)};
    print.export_print(file.path.string());
    const auto archive = read_zipper_archive(file.path.string(), {""}, {});
    auto entry = [&](const std::string &name) -> const std::vector<uint8_t> & {
        auto lower = name;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        const auto found = std::find_if(archive.entries.begin(), archive.entries.end(), [&](const auto &entry) { return entry.fname == lower; });
        REQUIRE(found != archive.entries.end()); return found->buf;
    };
    const auto settings = nlohmann::json::parse(entry("anycubic_photon_resins.pwsp"));
    const auto &machine = settings.at("machine_type");
    REQUIRE(machine.at("key_suffix") == format);
    REQUIRE(machine.at("key_image_format") == "pw0Img");
    REQUIRE(machine.at("res_x") == (portrait ? 100 : 200));
    REQUIRE(machine.at("res_y") == (portrait ? 200 : 100));
    REQUIRE(machine.at("print_xsize").get<double>() == Catch::Approx(portrait ? 60. : 100.));
    REQUIRE(machine.at("print_ysize").get<double>() == Catch::Approx(portrait ? 100. : 60.));
    REQUIRE(machine.at("xy_pixel").get<double>() == Catch::Approx(portrait ? 600. : 500.));
    REQUIRE(machine.at("xy_pixel_y").get<double>() == Catch::Approx(portrait ? 500. : 600.));
    const auto &resin = settings.at("machine_extern").at("user_resins").at(0);
    REQUIRE(resin.at("slicepara").at("use_indivi_layerpara") == 1);
    REQUIRE(resin.at("slicepara").at("bott_layers") == 1);
    REQUIRE(resin.at("slice_extpara").at("transition_layercount") == 2);
    REQUIRE(resin.at("slice_extpara").at("multi_state_used") == 1);
    REQUIRE(resin.at("slice_extpara").at("multi_state_paras").at("normal_0").at("up_speed").get<double>() == Catch::Approx(1.5));
    REQUIRE(resin.at("slice_extpara").at("multi_state_paras").at("bott_0").at("down_speed").get<double>() == Catch::Approx(2.));
    const auto layers = nlohmann::json::parse(entry("layers_controller.conf"));
    const auto count = print.print_layers().size();
    REQUIRE(layers.at("count") == count);
    const auto &scene = entry("scene.slice");
    auto integer = [&](size_t at) {
        REQUIRE(at+4 <= scene.size());
        uint32_t result = 0;
        for (size_t i = 0; i < 4; ++i) result |= uint32_t(scene[at+i]) << (8*i);
        return result;
    };
    auto floating = [&](size_t at) { const auto bits = integer(at); float result; std::memcpy(&result, &bits, 4); return result; };
    double previous_z = 0, volume = 0;
    for (size_t i = 0; i < count; ++i) {
        const auto z = double(print.print_layers()[i].level()) * SCALING_FACTOR;
        const auto &parameters = layers.at("paras").at(i);
        REQUIRE(parameters.at("layer_index") == i);
        REQUIRE(parameters.at("layer_minheight").get<double>() == Catch::Approx(previous_z));
        REQUIRE(parameters.at("layer_thickness").get<double>() == Catch::Approx(z-previous_z));
        REQUIRE(parameters.at("zup_height").get<double>() == Catch::Approx(i == 0 ? 4. : 7.));
        REQUIRE(parameters.at("zup_speed").get<double>() == Catch::Approx(i == 0 ? 1. : 1.5));
        const double exposure = i == 0 ? full.initial_exposure_time.value : i < 3 ?
            full.initial_exposure_time.value + (full.exposure_time.value-full.initial_exposure_time.value)*double(i)/3. : full.exposure_time.value;
        REQUIRE(parameters.at("exposure_time").get<double>() == Catch::Approx(exposure));
        // Independent decoder matching the documented PW0 byte layout.
        const auto &rle = entry("layer_images/layer_" + std::to_string(i) + ".pw0Img");
        std::vector<uint8_t> decoded;
        for (size_t p = 0; p < rle.size(); ++p) {
            const auto color = rle[p] >> 4;
            size_t run = rle[p] & 15;
            if (color == 0 || color == 15) { REQUIRE(p+1 < rle.size()); run = run*256 + rle[++p]; }
            REQUIRE(run > 0); REQUIRE(decoded.size()+run <= image.buf.size());
            decoded.insert(decoded.end(), run, uint8_t(color*17));
        }
        auto expected = edited.image(i).buf;
        for (auto &pixel : expected) pixel = uint8_t((pixel >> 4)*17);
        REQUIRE(decoded == expected);
        const auto pixels = std::count_if(decoded.begin(), decoded.end(), [](uint8_t pixel) { return pixel != 0; });
        volume += pixels * .3 * (z-previous_z);
        REQUIRE(floating(396+i*64) == Catch::Approx(z));
        REQUIRE(floating(396+i*64+4) == Catch::Approx(pixels*.3));
        // Independent flood fill checks the streaming connected-component
        // measurement, including diagonal neighbors and the empty final mask.
        auto remaining = decoded;
        uint32_t objects = 0; size_t largest = 0;
        const size_t cols = portrait ? 100 : 200, rows = portrait ? 200 : 100;
        for (size_t p = 0; p < remaining.size(); ++p) {
            if (!remaining[p]) continue;
            ++objects; remaining[p] = 0;
            std::vector<size_t> queue{p}; size_t area = 0;
            while (!queue.empty()) {
                const auto at = queue.back(); queue.pop_back(); ++area;
                const auto x = at % cols, y = at / cols;
                for (size_t ny = y ? y-1 : 0; ny <= std::min(y+1, rows-1); ++ny)
                    for (size_t nx = x ? x-1 : 0; nx <= std::min(x+1, cols-1); ++nx)
                        if (remaining[ny*cols+nx]) { remaining[ny*cols+nx] = 0; queue.push_back(ny*cols+nx); }
            }
            largest = std::max(largest, area);
        }
        REQUIRE(integer(396+i*64+24) == objects);
        REQUIRE(floating(396+i*64+28) == Catch::Approx(largest*.3));
        previous_z = z;
    }
    const auto info = nlohmann::json::parse(entry("print_info.json"));
    REQUIRE(info.at("volume").get<double>() == Catch::Approx(volume / 1000.));
    REQUIRE(scene.size() == 400 + 64*count);
    REQUIRE(std::string(scene.begin(), scene.begin()+13) == "ANYCUBIC-PWSZ");
    REQUIRE(std::string(scene.begin()+388, scene.begin()+392) == "<---");
    REQUIRE(std::string(scene.end()-4, scene.end()) == "--->");
    uint32_t layer_def_count = 0;
    for (size_t j = 0; j < 4; ++j) layer_def_count |= uint32_t(scene[392+j]) << (8*j);
    REQUIRE(layer_def_count == count);
    SECTION("Unsupported parameters preserve an existing destination") {
        config.set("sla_light_pwm", 200);
        print.apply(model, config); print.process();
        const auto before = boost::filesystem::file_size(file.path);
        REQUIRE_THROWS_WITH(print.export_print(file.path.string()), "Anycubic ZIP cannot store a custom UV PWM");
        REQUIRE(boost::filesystem::file_size(file.path) == before);
    }
}

TEST_CASE("Explicit SLA reader IDs override filenames", "[sla_registry]")
{
    auto reader = SLAArchiveReader::create("renamed.data", "sl1");
    REQUIRE(dynamic_cast<SL1Reader *>(reader.get()) != nullptr);
    reader = SLAArchiveReader::create("no_extension", "SL1SVG");
    REQUIRE(dynamic_cast<SL1_SVGReader *>(reader.get()) != nullptr);
    REQUIRE(SLAArchiveReader::create("valid.sl1", "unknown") == nullptr);
}

TEST_CASE("SLA readers preserve compound suffixes and case", "[sla_registry]")
{
    auto reader = SLAArchiveReader::create("example.SL1S", "");
    REQUIRE(dynamic_cast<SL1Reader *>(reader.get()) != nullptr);
    reader = SLAArchiveReader::create("example.RGB.ZIP", "");
    // A known compound suffix with no reader must not fall back to a shorter
    // ZIP alias and pretend to reconstruct an SL1/SVG archive.
    REQUIRE(reader == nullptr);
    reader = SLAArchiveReader::create("example.SL1_SVG", "");
    REQUIRE(dynamic_cast<SL1_SVGReader *>(reader.get()) != nullptr);
    REQUIRE(SLAArchiveReader::create("example.sl1.backup", "") == nullptr);
    REQUIRE(SLAArchiveReader::create("no_extension", "") == nullptr);
}

TEST_CASE("Unimplemented binary readers do not masquerade as SL1", "[sla_registry]")
{
    for (const char *id : {"ctb", "cxdlpv4", "goo", "pwmx"}) {
        INFO(id);
        REQUIRE(get_archive_entry(id) != nullptr);
        REQUIRE_FALSE(bool(get_reader_factory(id)));
        REQUIRE(SLAArchiveReader::create(std::string("sample.") + id, "") == nullptr);
        REQUIRE(SLAArchiveReader::create("sample.sl1", id) == nullptr);
    }
}

TEST_CASE("SLA format suffixes are validated without losing punctuation", "[sla_registry]")
{
    for (const char *ext : {".CTB", "ctb"}) {
        const auto *entry = get_archive_entry_by_extension(ext);
        REQUIRE(entry != nullptr);
        REQUIRE(std::string(entry->id) == "ctb");
    }
    for (const char *ext : {"encrypted.ctb", ".ENCRYPTED.CTB"}) {
        const auto *encrypted = get_archive_entry_by_extension(ext);
        REQUIRE(encrypted != nullptr);
        REQUIRE(std::string(encrypted->id) == "ctb_encrypted");
        REQUIRE(std::string(get_default_extension(encrypted->id)) == "encrypted.ctb");
        REQUIRE(archive_export_unavailable_reason(encrypted->id).empty());
    }
    REQUIRE(std::string(get_archive_entry_by_extension(".V4.ENCRYPTED.CTB")->id) == "ctb_encrypted_v4");
    const auto *entry = get_archive_entry_by_extension(".SL1_SVG");
    REQUIRE(entry != nullptr);
    REQUIRE(std::string(entry->id) == "SL1SVG");
    for (const char *ext : {"ctb)", "*.ctb", "encryptedctb", "sl1svg", "..ctb", "ctb.", "", "ctb/"}) {
        INFO(ext);
        REQUIRE(get_archive_entry_by_extension(ext) == nullptr);
    }
    REQUIRE(get_archive_entry_by_extension(nullptr) == nullptr);
}

TEST_CASE("SL1 slicing preserves geometry and exposure settings", "[sla_reference]")
{
    // A small synthetic model keeps the smoke test independent of external
    // fixtures and avoids generating hundreds of full-resolution layers.
    Model model;
    auto *object = model.add_object("reference", "", make_cube(4., 3., 0.3));
    object->add_instance()->set_offset(Vec3d(10., 10., 0.));

    SLAFullPrintConfig fullcfg;
    fullcfg.printer_technology.setInt(ptSLA);
    const bool portrait = GENERATE(false, true);
    fullcfg.display_orientation.value = portrait ? sladoPortrait : sladoLandscape;
    fullcfg.display_mirror_x.value = GENERATE(false, true);
    fullcfg.display_mirror_y.value = GENERATE(false, true);
    fullcfg.set("sla_archive_format", std::string("SL1"));
    fullcfg.set("supports_enable", false);
    fullcfg.set("pad_enable", false);
    fullcfg.set("support_object_elevation", 0.0);
    fullcfg.set("layer_height", 0.05);
    fullcfg.set("initial_layer_height", 0.05);
    fullcfg.set("display_width", 100.0);
    fullcfg.set("display_height", 60.0);
    fullcfg.set("display_pixels_x", 1000);
    fullcfg.set("display_pixels_y", 300);
    fullcfg.set("exposure_time", 2.5);
    fullcfg.set("initial_exposure_time", 20.0);

    DynamicPrintConfig cfg;
    cfg.apply(fullcfg);
    SLAPrint print;
    print.set_status_callback([](const PrintBase::SlicingStatus &) {});
    print.apply(model, cfg);
    print.process();

    cfg.set("gamma_correction", 0.5);
    print.apply(model, cfg);
    REQUIRE(print.is_step_done(slaposObjectSlice));
    const auto &state = static_cast<const PrintBaseWithState<SLAPrintStep, slapsCount>&>(print);
    REQUIRE(state.is_step_done(slapsMergeSlicesAndEval));
    REQUIRE_FALSE(state.is_step_done(slapsRasterize));
    print.process();
    REQUIRE(print.finished());

    struct TemporaryArchive {
        boost::filesystem::path path = boost::filesystem::unique_path("sla-reference-%%%%-%%%%.sl1");
        ~TemporaryArchive() {
            boost::system::error_code error;
            boost::filesystem::remove(path, error);
        }
    } archive;
    print.export_print(archive.path.string(), {}, "reference");
    REQUIRE(boost::filesystem::file_size(archive.path) > 0);

    const auto contents = read_zipper_archive(archive.path.string(), {"png", "ini"}, {"thumbnail"});
    REQUIRE(contents.entries.size() == 6);
    REQUIRE(contents.config.get<std::string>("slaSlicerProfileGeometry") == "raster-v1");
    REQUIRE(contents.profile.get<int>("display_pixels_x") == (portrait ? 300 : 1000));
    REQUIRE(contents.profile.get<double>("display_width") == Catch::Approx(portrait ? 60. : 100.));
    for (const auto &entry : contents.entries) {
        png::ImageGreyscale image;
        REQUIRE(png::decode_png(png::ReadBuf{entry.buf.data(), entry.buf.size()}, image));
        REQUIRE(image.cols == (portrait ? 300 : 1000));
        REQUIRE(image.rows == (portrait ? 1000 : 300));
        REQUIRE(std::any_of(image.buf.begin(), image.buf.end(), [](uint8_t p) { return p > 128; }));
        REQUIRE(std::any_of(image.buf.begin(), image.buf.end(), [](uint8_t p) { return p == 0; }));
    }

    // Exercise explicit format selection and the default progress callback:
    // both previously prevented this normal read path from completing.
    auto reader = SLAArchiveReader::create(archive.path.string(), "SL1");
    REQUIRE(reader != nullptr);
    DynamicPrintConfig restored;
    std::vector<ExPolygons> slices;
    reader->read(slices, restored);
    REQUIRE(slices.size() == 6);
    for (const auto &slice : slices)
        REQUIRE_FALSE(slice.empty());
    REQUIRE(restored.opt_float("exposure_time") == Catch::Approx(2.5));
    REQUIRE(restored.opt_float("initial_exposure_time") == Catch::Approx(20.0));
    REQUIRE(restored.opt_float("layer_height") == Catch::Approx(0.05));
    REQUIRE(restored.opt_int("display_pixels_x") == 1000);
    REQUIRE(restored.opt_int("display_pixels_y") == 300);
    REQUIRE(restored.opt_float("display_width") == Catch::Approx(100.));
    REQUIRE(restored.opt_float("display_height") == Catch::Approx(60.));
    DynamicPrintConfig profile_only;
    reader->read(profile_only);
    REQUIRE(profile_only == restored);
    auto document = sla::LayerDocument::load_sl1(archive.path.string());
    REQUIRE(document.layer_count() == slices.size());
    document.paint(0, 5, 5, 2, 255);
    document.save_sl1(archive.path.string());
    REQUIRE(sla::LayerDocument::load_sl1(archive.path.string()).image(0).get(5, 5) == 255);
    DynamicPrintConfig edited_profile;
    std::vector<ExPolygons> edited_slices;
    reader->read(edited_slices, edited_profile);
    REQUIRE(edited_profile == restored);
    REQUIRE(edited_slices.size() == slices.size());
    for (const auto &slice : slices) {
        REQUIRE(std::any_of(slice.begin(), slice.end(), [](const ExPolygon &poly) {
            return poly.contains(Point(scaled(12.), scaled(11.5)));
        }));
    }
}

template<class Base> struct ExposedRasterArchive : Base {
    using Base::Base;
    using Base::create_raster;
};

TEST_CASE("Archive rasters preserve physical geometry with rectangular pixels", "[sla_reference]")
{
    // Expose the production raster factory without involving a second renderer.
    struct RasterArchive : SL1Archive {
        using SL1Archive::SL1Archive;
        using SL1Archive::create_raster;
    };
    const int writer = GENERATE(0, 1, 2, 3, 4, 5, 6);
    CAPTURE(writer);
    for (bool portrait : {false, true})
        for (bool mirror_x : {false, true})
            for (bool mirror_y : {false, true}) {
                CAPTURE(portrait, mirror_x, mirror_y);
                SLAPrinterConfig cfg;
                cfg.display_width.value = 100.;
                cfg.display_height.value = 60.;
                cfg.display_pixels_x.value = 1000;
                cfg.display_pixels_y.value = 300;
                cfg.display_orientation.value = portrait ? sladoPortrait : sladoLandscape;
                cfg.display_mirror_x.value = mirror_x;
                cfg.display_mirror_y.value = mirror_y;
                std::unique_ptr<sla::RasterBase> raster;
                switch (writer) {
                case 0: raster = RasterArchive(cfg).create_raster(); break;
                case 1: raster = ExposedRasterArchive<AnycubicSLAArchive>(cfg).create_raster(); break;
                case 2: raster = ExposedRasterArchive<FDGArchive>(cfg).create_raster(); break;
                case 3: raster = ExposedRasterArchive<UVToolsCWSArchive>(cfg).create_raster(); break;
                case 4: raster = ExposedRasterArchive<CrealityCXDLPv4Archive>(cfg).create_raster(); break;
                case 5: raster = ExposedRasterArchive<NanoDLPArchive>(cfg).create_raster(); break;
                case 6: raster = ExposedRasterArchive<FlashforgeSVGXArchive>(cfg).create_raster(); break;
                }
                // An off-centre rectangle catches rotation, scaling and clipping.
                raster->draw(ExPolygon(Polygon(Points{
                    Point(scaled(72.), scaled(42.)), Point(scaled(80.), scaled(42.)),
                    Point(scaled(80.), scaled(48.)), Point(scaled(72.), scaled(48.))})));
                const auto encoded = raster->encode(sla::PNGRasterEncoder{});
                png::ImageGreyscale image;
                REQUIRE(png::decode_png(png::ReadBuf{encoded.data(), encoded.size()}, image));
                REQUIRE(image.cols == (portrait ? 300 : 1000));
                REQUIRE(image.rows == (portrait ? 1000 : 300));
                // Independent pixel-space oracle: rectangle [720,800) x [210,240).
                size_t lit = 0;
                for (size_t y = 0; y < image.rows; ++y)
                    for (size_t x = 0; x < image.cols; ++x) {
                        size_t u = (portrait ? !mirror_x : mirror_x) ? image.cols - 1 - x : x;
                        size_t v = !mirror_y ? image.rows - 1 - y : y;
                        if (portrait) std::swap(u, v);
                        const bool expected = u >= 720 && u < 800 && v >= 210 && v < 240;
                        if ((image.get(y, x) > 128) != expected)
                            FAIL("Unexpected exposure at pixel " << x << ", " << y);
                        lit += image.get(y, x) > 128;
                    }
                REQUIRE(lit == 2400);
            }
}

TEST_CASE("SL1 profile geometry conventions are explicit", "[sla_reference]")
{
    ZipperArchive archive;
    archive.profile.put("display_orientation", "portrait");
    archive.profile.put("display_width", "100");
    archive.profile.put("display_height", "60");
    archive.profile.put("display_pixels_x", "1000");
    archive.profile.put("display_pixels_y", "300");
    DynamicPrintConfig legacy;
    extract_profile(archive, legacy);
    REQUIRE(legacy.opt_float("display_width") == 100.);
    REQUIRE(legacy.opt_int("display_pixels_x") == 1000);
    archive.config.put("slaSlicerProfileGeometry", "raster-v1");
    DynamicPrintConfig tagged;
    extract_profile(archive, tagged);
    REQUIRE(tagged.opt_float("display_width") == 60.);
    REQUIRE(tagged.opt_int("display_pixels_x") == 300);
    archive.config.put("slaSlicerProfileGeometry", "raster-v99");
    REQUIRE_THROWS(extract_profile(archive, tagged));
}

TEST_CASE("SL1 rejects invalid display dimensions before reconstruction", "[sla_reference]")
{
    DynamicPrintConfig cfg;
    cfg.apply(SLAFullPrintConfig{});
    cfg.set("display_pixels_x", 0);
    REQUIRE_THROWS(get_raster_params(cfg));
    cfg.set("display_pixels_x", 1000);
    cfg.set("display_height", -1.);
    REQUIRE_THROWS(get_raster_params(cfg));
}

TEST_CASE("NanoDLP requires a printer controller profile before slicing or writing", "[sla_registry]")
{
    SLAPrinterConfig cfg;
    SLAPrint print;
    for (const char *id : {"nanodlp", "nanodlp_v1", "nanodlp_rgb", "nanodlp_rgb_v1"}) {
        CAPTURE(id);
        REQUIRE(archive_export_unavailable_reason(id).empty());
        auto writer = SLAArchiveWriter::create(id, cfg);
        REQUIRE(writer != nullptr);
        const auto path = boost::filesystem::unique_path("missing-profile-%%%%.nanodlp");
        REQUIRE_THROWS(writer->export_print(path.string(), print, {}, "test"));
        REQUIRE_FALSE(boost::filesystem::exists(path));
        SLAFullPrintConfig full; Test::configure_archive_fixture(full,id);
        full.nanodlp_device_profile.value.clear();
        DynamicPrintConfig settings;settings.apply(full);
        SLAPrint missing;missing.apply(Model{},settings);
        REQUIRE(missing.validate().find("controller profile") != std::string::npos);
    }
    REQUIRE_FALSE(archive_export_unavailable_reason("unknown-format").empty());
    REQUIRE(archive_export_unavailable_reason("ctb_encrypted").empty());
    REQUIRE(std::string(get_default_extension("nanodlp_rgb_v1")) == "v1.rgb.nanodlp");
}

TEST_CASE("Distinct encrypted and unknown containers cannot fall back to ordinary CTB", "[sla_registry]")
{
    const uint8_t pixel = 255;
    auto document = sla::LayerDocument::from_rasters({sla::PNGRasterEncoder{}(&pixel, 1, 1, 1)});
    SLAPrinterConfig cfg;
    CTBEncryptedArchive encrypted(cfg);
    UVToolsChituboxArchive phz(cfg,0x9FDA83AE);
    REQUIRE_NOTHROW(encrypted.encode_layers(document));
    REQUIRE_THROWS(phz.encode_layers(document));
    struct File {
        boost::filesystem::path path = boost::filesystem::unique_path("preserved-%%%%.ENCRYPTED.CTB");
        ~File() { boost::system::error_code error; boost::filesystem::remove(path, error); }
    } file;
    const std::string original = "existing printer file";
    { std::ofstream out(file.path.string(), std::ios::binary); out << original; }
    SLAPrint print;
    UVToolsChituboxArchive ordinary(cfg);
    REQUIRE_THROWS_WITH(ordinary.export_print(file.path.string(), print, {}, ""),
                       "Ordinary Chitubox v2 cannot be exported as encrypted CTB v5");
    REQUIRE_THROWS(phz.export_print(file.path.string(), print, {}, ""));
    std::ifstream preserved(file.path.string(), std::ios::binary);
    REQUIRE(std::string(std::istreambuf_iterator<char>(preserved), {}) == original);
    REQUIRE(sla_archive_filename("part.encrypted.ctb", "ctb") == "part.ctb");
    REQUIRE(sla_archive_filename("part.v4.encrypted.ctb", "encrypted.ctb") == "part.encrypted.ctb");
    REQUIRE(sla_archive_filename("part.encrypted.ctb", "v4.encrypted.ctb") == "part.v4.encrypted.ctb");
    REQUIRE(sla_archive_filename("part.ctb", "encrypted.ctb") == "part.encrypted.ctb");
}

TEST_CASE("SLA filenames follow the selected format", "[sla_registry]")
{
    REQUIRE(sla_archive_filename("part.sl1", "goo") == "part.goo");
    REQUIRE(sla_archive_filename("part.encrypted.ctb", ".cxdlpv4") == "part.cxdlpv4");
    REQUIRE(sla_archive_filename("part.CTB", ".SL1") == "part.sl1");
    REQUIRE(sla_archive_filename("part.custom", "pwmx") == "part.pwmx");
    REQUIRE(sla_archive_filename("part", "sl1_svg") == "part.sl1_svg");
    REQUIRE(sla_archive_filename("part.xml.cws", "rgb.cws") == "part.rgb.cws");
    REQUIRE(sla_archive_filename("part.RGB.CWS", "xml.cws") == "part.xml.cws");
    REQUIRE(sla_archive_filename("part.rgb.cws", "cws") == "part.cws");
    REQUIRE(std::string(get_archive_entry_by_extension(".RGB.CWS")->id)=="cwsrgb");
    REQUIRE(sla_archive_filename("", "sl1") == "print.sl1");
    REQUIRE_THROWS(sla_archive_filename("part.sl1", "bad-format"));
}

// Explicit diagnostic target: generates small files for an independent decoder.
// Passing this test means the report was produced, not that every format works.
TEST_CASE("Generate SLA exporter audit fixtures", "[.sla_export_audit]")
{
    const boost::filesystem::path dir("sla-format-audit");
    boost::filesystem::create_directories(dir);
    nlohmann::json snapshot=nlohmann::json::array();
    for(const auto &entry:registered_sla_archives())
        snapshot.push_back({{"id",entry.id},{"primary_extension",entry.ext},{"extensions",get_extensions(entry)},
            {"export_block",archive_export_unavailable_reason(entry.id)},{"reader_registered",bool(entry.rdfactoryfn)}});
    std::ofstream((dir/"registry.json").string()) << snapshot.dump(2) << '\n';
    std::ofstream report((dir / "exports.tsv").string());
    report << "format\tstatus\tslice_ms\texport_ms\tdetail\n";
    using Clock = std::chrono::steady_clock;
    for (const auto &entry : registered_sla_archives()) {
        const auto reason = archive_export_unavailable_reason(entry.id);
        if (!reason.empty()) { report << entry.id << "\tblocked\t0\t0\t" << reason << '\n'; continue; }
        try {
            Model model;
            auto *object = model.add_object("audit", "", make_cube(4., 3., 0.15));
            object->add_instance()->set_offset(Vec3d(10., 10., 0.));
            SLAFullPrintConfig full;
            Test::configure_archive_fixture(full, entry.id);
            full.set("supports_enable", false); full.set("pad_enable", false);
            full.set("support_object_elevation", 0.);
            DynamicPrintConfig cfg; cfg.apply(full);
            SLAPrint print; print.set_status_callback([](const PrintBase::SlicingStatus &) {});
            auto start = Clock::now(); print.apply(model, cfg); print.process(); auto sliced = Clock::now();
            const auto path = dir / (std::string("audit-") + entry.id + "." + entry.ext);
            print.export_print(path.string(), {}, "audit"); auto end = Clock::now();
            report << entry.id << "\twritten\t" << std::chrono::duration<double, std::milli>(sliced-start).count()
                   << '\t' << std::chrono::duration<double, std::milli>(end-sliced).count()
                   << '\t' << boost::filesystem::file_size(path) << " bytes\n";
        } catch (const std::exception &error) { report << entry.id << "\terror\t0\t0\t" << error.what() << '\n'; }
    }
    REQUIRE(report.good());
}

TEST_CASE("CTB RLE retains grayscale and all variable length count boundaries", "[sla_reference][sla_ctb]")
{
    SLAPrinterConfig config;
    UVToolsChituboxArchive archive(config);
    const auto encoder = archive.get_encoder();
    for (const size_t count : {size_t(1), size_t(2), size_t(127), size_t(128),
                              size_t(16383), size_t(16384), size_t(2097151), size_t(2097152)}) {
        CAPTURE(count);
        // Every run crosses a row boundary. Adjacent input gray levels sharing
        // a 7-bit value must merge; white must survive as an ordinary color.
        const size_t width = 256;
        const size_t height = (count + 5 + width - 1) / width;
        std::vector<uint8_t> pixels(width * height, 0);
        std::fill(pixels.begin() + 5, pixels.begin() + 5 + count, 255);
        pixels[0] = 0; pixels[1] = 1; pixels[2] = 2; pixels[3] = 3; pixels[4] = 128;
        const auto encoded = encoder(pixels.data(), width, height, 1);
        const auto *bytes = static_cast<const uint8_t *>(encoded.data());
        std::vector<uint8_t> decoded;
        for (size_t offset = 0; offset < encoded.size();) {
            const uint8_t token = bytes[offset++];
            uint32_t run = 1;
            if (token & 0x80) {
                REQUIRE(offset < encoded.size());
                const uint8_t first = bytes[offset++];
                unsigned extra = 0;
                if (!(first & 0x80)) run = first;
                else if ((first & 0xc0) == 0x80) { run = first & 0x3f; extra = 1; }
                else if ((first & 0xe0) == 0xc0) { run = first & 0x1f; extra = 2; }
                else { REQUIRE((first & 0xf0) == 0xe0); run = first & 0x0f; extra = 3; }
                REQUIRE(offset + extra <= encoded.size());
                while (extra--) run = (run << 8) | bytes[offset++];
                REQUIRE(run > 1);
            }
            const uint8_t gray = token & 0x7f;
            REQUIRE(decoded.size() + run <= pixels.size());
            decoded.insert(decoded.end(), run, gray ? uint8_t((gray << 1) | 1) : 0);
        }
        REQUIRE(decoded.size() == pixels.size());
        std::transform(pixels.begin(), pixels.end(), pixels.begin(), [](uint8_t gray) {
            return gray > 1 ? uint8_t(gray | 1) : uint8_t(0);
        });
        REQUIRE(decoded == pixels);
    }
}

TEST_CASE("CBDDLP eight bitplanes retain every representable gray and split 125-pixel runs", "[sla_reference][sla_cbddlp]")
{
    SLAPrinterConfig config;
    UVToolsChituboxArchive archive(config, 0x12fd0019);
    const auto encoder = archive.get_encoder();
    std::vector<uint8_t> pixels(256 * 128);
    for (size_t i = 0; i < pixels.size(); ++i) pixels[i] = uint8_t(i / 128);
    const auto encoded = encoder(pixels.data(), 256, 128, 1);
    const auto *bytes = static_cast<const uint8_t *>(encoded.data());
    auto u32 = [&](size_t offset) {
        REQUIRE(offset + 4 <= encoded.size());
        return uint32_t(bytes[offset]) | (uint32_t(bytes[offset+1]) << 8) |
               (uint32_t(bytes[offset+2]) << 16) | (uint32_t(bytes[offset+3]) << 24);
    };
    REQUIRE((u32(0) | (uint64_t(u32(4)) << 32)) == 225 * 128);
    std::vector<uint8_t> levels(pixels.size(), 0);
    size_t offset = 40;
    for (unsigned plane = 0; plane < 8; ++plane) {
        const uint32_t size = u32(8 + 4 * plane);
        REQUIRE(offset + size <= encoded.size());
        size_t pixel = 0;
        for (size_t index = 0; index < size; ++index) {
            const uint8_t token = bytes[offset + index];
            const unsigned count = token & 0x7f;
            REQUIRE(count >= 1); REQUIRE(count <= 125);
            REQUIRE(pixel + count <= pixels.size());
            if (token & 0x80)
                for (size_t n = 0; n < count; ++n) ++levels[pixel + n];
            pixel += count;
        }
        REQUIRE(pixel == pixels.size()); offset += size;
    }
    REQUIRE(offset == encoded.size());
    std::transform(levels.begin(), levels.end(), levels.begin(), [](uint8_t count) {
        return count ? uint8_t(count * 32 - 1) : uint8_t(0);
    });
    std::transform(pixels.begin(), pixels.end(), pixels.begin(), [](uint8_t gray) {
        return gray < 31 ? uint8_t(0) : uint8_t(((unsigned(gray) + 1) / 32) * 32 - 1);
    });
    REQUIRE(levels == pixels);
}

// Opt-in original-UVtools fixture: exercise every RLE count width and the
// canonical edited-mask export, including nonuniform Z and transition exposure.
TEST_CASE("Generate Chitubox full decoder grayscale fixtures", "[.sla_ctb_oracle][.sla_chitubox_oracle]")
{
    const char *folder = std::getenv("SLA_CTB_ORACLE_OUTPUT");
    REQUIRE(folder != nullptr);
    boost::filesystem::create_directories(folder);
    SLAFullPrintConfig full;
    full.printer_technology.setInt(ptSLA);
    full.display_orientation.value = sladoLandscape;
    full.set("sla_archive_format", std::string("SL1"));
    full.set("display_width", 100.); full.set("display_height", 60.);
    full.set("display_pixels_x", 256); full.set("display_pixels_y", 8193);
    full.set("layer_height", .05); full.set("initial_layer_height", .05);
    full.set("sla_bottom_layers", 1); full.set("sla_transition_layers", 1);
    full.set("exposure_time", 2.5); full.set("initial_exposure_time", 20.);
    full.set("sla_wait_before_cure", 1.25); full.set("sla_bottom_wait_before_cure", 2.5);
    DynamicPrintConfig config; config.apply(full);
    std::vector<sla::EncodedRaster> rasters;
    std::vector<uint8_t> pixels(256 * 8193, 255);
    rasters.push_back(sla::PNGRasterEncoder{}(pixels.data(), 256, 8193, 1));
    std::fill(pixels.begin(), pixels.end(), 0);
    rasters.push_back(sla::PNGRasterEncoder{}(pixels.data(), 256, 8193, 1));
    size_t offset = 0;
    const size_t lengths[]{1, 2, 127, 128, 16383, 16384};
    for (unsigned gray = 0; gray < 256; ++gray) {
        const size_t count = lengths[gray % 6];
        std::fill(pixels.begin() + offset, pixels.begin() + offset + count, uint8_t(gray));
        offset += count;
    }
    rasters.push_back(sla::PNGRasterEncoder{}(pixels.data(), 256, 8193, 1));
    const auto document = sla::LayerDocument::from_rasters(std::move(rasters));
    const auto job = SLAPrint::create_raster_job(config, document, {.05, .1, .15});
    for (const std::string format : {"SL1", "ctb", "cbddlp", "photon"}) {
        const auto path = boost::filesystem::path(folder) / ("grayscale." + std::string(get_default_extension(format.c_str())));
        job->export_layer_document(document, format, path.string(), {}, "grayscale");
        REQUIRE(boost::filesystem::file_size(path) > 0);
    }
    const auto variable = SLAPrint::create_raster_job(config, document, {.05, .15, .2});
    for (const std::string format : {"ctb", "cbddlp", "photon"})
        variable->export_layer_document(document, format,
            (boost::filesystem::path(folder) / ("variable-z." + format)).string(), {}, "grayscale");
}

TEST_CASE("CTB v2 metadata follows printer geometry and sliced layer heights", "[sla_reference]")
{
    Model model;
    auto *object = model.add_object("ctb-reference", "", make_cube(4., 3., 0.3));
    object->add_instance()->set_offset(Vec3d(10., 10., 0.));
    SLAFullPrintConfig full;
    full.printer_technology.setInt(ptSLA);
    full.display_orientation.value = sladoPortrait;
    full.set("sla_archive_format", std::string("ctb"));
    full.set("supports_enable", false); full.set("pad_enable", false);
    full.set("support_object_elevation", 0.);
    full.set("display_width", 92.); full.set("display_height", 51.);
    full.set("display_pixels_x", 184); full.set("display_pixels_y", 102);
    full.set("sla_wait_before_cure", 1.25); full.set("sla_bottom_wait_before_cure", 2.5);
    full.set("layer_height", 0.05); full.set("initial_layer_height", 0.1);
    DynamicPrintConfig cfg; cfg.apply(full);
    SLAPrint print; print.set_status_callback([](const PrintBase::SlicingStatus &) {});
    print.apply(model, cfg); print.process();
    struct Temporary {
        boost::filesystem::path path = boost::filesystem::unique_path("ctb-reference-%%%%-%%%%.ctb");
        ~Temporary() { boost::system::error_code ec; boost::filesystem::remove(path, ec); }
    } file;
    print.export_print(file.path.string(), {}, "ctb-reference");
    std::ifstream in(file.path.string(), std::ios::binary);
    std::vector<unsigned char> data((std::istreambuf_iterator<char>(in)), {});
    auto u32 = [&](size_t offset) {
        REQUIRE(offset + 4 <= data.size());
        return uint32_t(data[offset]) | (uint32_t(data[offset+1]) << 8) |
               (uint32_t(data[offset+2]) << 16) | (uint32_t(data[offset+3]) << 24);
    };
    auto f32 = [&](size_t offset) { uint32_t bits = u32(offset); float value; std::memcpy(&value, &bits, 4); return value; };
    REQUIRE(u32(4) == 2);
    REQUIRE(f32(8) == Catch::Approx(51.));
    REQUIRE(f32(12) == Catch::Approx(92.));
    REQUIRE(u32(52) == 102);
    REQUIRE(u32(56) == 184);
    REQUIRE(u32(68) == print.print_layers().size());
    REQUIRE(u32(48) <= u32(68));
    const auto parameters = u32(84);
    REQUIRE(parameters == 112);
    REQUIRE(u32(88) == 60);
    REQUIRE(u32(80) == 1); // The fixture assumes mirrored projection.
    REQUIRE(f32(parameters) == Catch::Approx(6.));
    REQUIRE(f32(parameters+4) == Catch::Approx(60.));
    REQUIRE(f32(parameters+8) == Catch::Approx(6.));
    REQUIRE(f32(parameters+12) == Catch::Approx(80.));
    REQUIRE(f32(parameters+16) == Catch::Approx(150.));
    REQUIRE(f32(44) == Catch::Approx(8.15));
    REQUIRE(f32(parameters+32) == Catch::Approx(10.9));
    REQUIRE(f32(parameters+36) == Catch::Approx(8.15));
    REQUIRE(f32(28) == Catch::Approx(double(print.print_layers().back().level()) * SCALING_FACTOR));
    const auto table = u32(64);
    for (size_t i = 0; i < print.print_layers().size(); ++i) {
        REQUIRE(f32(table + i*36) == Catch::Approx(double(print.print_layers()[i].level()) * SCALING_FACTOR));
        REQUIRE(f32(table+i*36+8) == Catch::Approx(i < u32(48) ? 10.9 : 8.15));
        REQUIRE(uint64_t(u32(table+i*36+12)) + u32(table+i*36+16) <= data.size());
    }
}

TEST_CASE("LCD exports preserve Z exposure and distinct bottom and normal motion", "[sla_reference][sla_native]")
{
    for (const std::string format : {"cxdlpv4", "goo", "prz"}) {
        CAPTURE(format);
        Model model;
        auto *object = model.add_object("lcd-reference", "", make_cube(4., 3., 0.4));
        object->add_instance()->set_offset(Vec3d(10., 10., 0.));
        SLAFullPrintConfig full;
        full.printer_technology.setInt(ptSLA);
        full.display_orientation.value = sladoPortrait;
        full.set("sla_archive_format", format);
        // Owned exports require an explicit printer identity, independently
        // of the mask, geometry and motion contracts exercised here.
        full.set("printer_model", "GENERIC");
        full.set("supports_enable", false); full.set("pad_enable", false);
        full.set("support_object_elevation", 0.);
        full.set("display_width", 100.); full.set("display_height", 60.);
        full.set("display_pixels_x", 200); full.set("display_pixels_y", 120);
        full.set("max_print_height", 220.);
        full.set("layer_height", 0.05); full.set("initial_layer_height", 0.1);
        full.set("exposure_time", 2.5); full.set("initial_exposure_time", 20.);
        full.set("sla_bottom_layers", 1); full.set("sla_transition_layers", 2);
        full.set("sla_lift_height", 7.5); full.set("sla_bottom_lift_height", 4.5);
        full.set("sla_lift_speed", 90.); full.set("sla_bottom_lift_speed", 55.);
        full.set("sla_lift_height2", 1.5); full.set("sla_bottom_lift_height2", 2.5);
        full.set("sla_lift_speed2", 120.); full.set("sla_bottom_lift_speed2", 65.);
        full.set("sla_retract_height2", 1.); full.set("sla_bottom_retract_height2", 2.);
        full.set("sla_wait_before_cure", 1.2); full.set("sla_bottom_wait_before_cure", 0.3);
        full.set("sla_light_pwm", 180); full.set("sla_bottom_light_pwm", 250);
        DynamicPrintConfig cfg; cfg.apply(full);
        SLAPrint print; print.set_status_callback([](const PrintBase::SlicingStatus &) {});
        print.apply(model, cfg); print.process();
        auto edited = *print.layer_document();
        auto mask = edited.image(0);
        mask.buf[0] = 127; mask.buf[1] = 132; mask.buf[2] = 130;
        edited.replace_image(0, mask);
        print.replace_layer_document(edited);
        struct File {
            boost::filesystem::path path;
            ~File() { boost::system::error_code ec; boost::filesystem::remove(path, ec); }
        } file{boost::filesystem::unique_path("lcd-metadata-%%%%." + format)};
        print.export_print(file.path.string());
        std::ifstream in(file.path.string(), std::ios::binary);
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), {});
        auto integer = [&](size_t at, bool big) {
            REQUIRE(at + 4 <= bytes.size());
            uint32_t value = 0;
            for (size_t i = 0; i < 4; ++i) value |= uint32_t(bytes[at+i]) << (8 * (big ? 3-i : i));
            return value;
        };
        auto floating = [&](size_t at, bool big) {
            const auto value = integer(at, big); float result; std::memcpy(&result, &value, 4); return result;
        };
        const auto count = print.print_layers().size();
        REQUIRE(count > 3);
        auto exposure = [](size_t i) { return i == 0 ? 20. : i < 3 ? 20. + (2.5 - 20.) * double(i) / 3. : 2.5; };
        if (format == "cxdlpv4") {
            // Header offsets come from UVtools CrealityCXDLPv4File.FieldOrder.
            const auto base = size_t(19) + integer(15, true);
            REQUIRE(floating(base+4, false) == Catch::Approx(60.));
            REQUIRE(floating(base+8, false) == Catch::Approx(100.));
            REQUIRE(floating(base+12, false) == Catch::Approx(220.));
            REQUIRE(integer(base+24, false) == 1);
            const auto slicer = integer(base+72, false);
            REQUIRE(integer(base+76, false) == 76);
            REQUIRE(slicer + 76 + 4 == bytes.size());
            REQUIRE(floating(slicer, false) == Catch::Approx(2.5));
            REQUIRE(floating(slicer+8, false) == Catch::Approx(1.5));
            REQUIRE(integer(slicer+28, false) == 1);
            REQUIRE(floating(slicer+52, false) == Catch::Approx(2.5));
            REQUIRE(floating(slicer+56, false) == Catch::Approx(20.));
            REQUIRE(integer(slicer+64, false) == 2);
            uint32_t checksum = 0;
            for (size_t pos = 0; pos + 4 < bytes.size(); ++pos) {
                checksum ^= bytes[pos];
                for (unsigned bit = 0; bit < 8; ++bit)
                    checksum = (checksum >> 1) ^ ((checksum & 1) ? 0xEDB88320u : 0u);
            }
            REQUIRE(integer(bytes.size()-4, true) == checksum);
            const auto table = integer(base+32, false);
            REQUIRE(integer(base+36, false) == count);
            for (size_t i = 0; i < count; ++i) {
                const auto row = table + i * 40;
                REQUIRE(floating(row, false) == Catch::Approx(double(print.print_layers()[i].level()) * SCALING_FACTOR));
                REQUIRE(floating(row+4, false) == Catch::Approx(exposure(i)));
                const auto extended = integer(row+12, false);
                // The wire stores TOTAL; UVtools subtracts SECOND to read FIRST.
                REQUIRE(floating(extended, false) == Catch::Approx(i == 0 ? 7. : 9.));
                REQUIRE(floating(extended, false) - floating(extended+8, false) ==
                    Catch::Approx(i == 0 ? 4.5 : 7.5));
                REQUIRE(floating(extended+4, false) == Catch::Approx(i == 0 ? 55. : 90.));
                REQUIRE(floating(extended+8, false) == Catch::Approx(i == 0 ? 2.5 : 1.5));
                REQUIRE(floating(extended+12, false) == Catch::Approx(i == 0 ? 65. : 120.));
                REQUIRE(floating(extended+36, false) == Catch::Approx(i == 0 ? 0.3 : 1.2));
                REQUIRE(floating(extended+40, false) == Catch::Approx(i == 0 ? 250. : 180.));
            }
        } else {
            // GooFile.Header has two fixed previews and a 195477-byte header.
            REQUIRE(integer(195310, true) == count);
            REQUIRE(floating(195320, true) == Catch::Approx(60.));
            REQUIRE(floating(195324, true) == Catch::Approx(100.));
            REQUIRE(floating(195328, true) == Catch::Approx(220.));
            REQUIRE(integer(195373, true) == 1);
            REQUIRE(floating(195409, true) == Catch::Approx(2.5));
            REQUIRE(floating(195413, true) == Catch::Approx(65.));
            REQUIRE(floating(195417, true) == Catch::Approx(1.5));
            REQUIRE(floating(195421, true) == Catch::Approx(120.));
            REQUIRE(bytes.at(195445) == 1);
            REQUIRE(integer(195446, true) > 0);
            size_t row = integer(195470, true);
            for (size_t i = 0; i < count; ++i) {
                REQUIRE(floating(row+6, true) == Catch::Approx(double(print.print_layers()[i].level()) * SCALING_FACTOR));
                REQUIRE(floating(row+10, true) == Catch::Approx(exposure(i)));
                REQUIRE(floating(row+26, true) == Catch::Approx(i == 0 ? 0.3 : 1.2));
                REQUIRE(floating(row+30, true) == Catch::Approx(i == 0 ? 4.5 : 7.5));
                REQUIRE(floating(row+34, true) == Catch::Approx(i == 0 ? 55. : 90.));
                const auto length = integer(row+66, true);
                const auto end = row + 70 + length;
                REQUIRE(end + 2 <= bytes.size());
                REQUIRE(bytes.at(row+70) == 0x55);
                uint8_t checksum = 0;
                for (size_t j = row+71; j+1 < end; ++j) checksum += bytes[j];
                REQUIRE(bytes.at(end-1) == uint8_t(~checksum));
                std::vector<uint8_t> decoded;
                uint8_t color = 0;
                for (size_t j = row+71; j+1 < end;) {
                    const auto tag = bytes.at(j++);
                    const unsigned kind = tag >> 6;
                    unsigned run;
                    if (kind == 2) {
                        color = uint8_t((tag & 0x20) ? color - (tag & 15) : color + (tag & 15));
                        run = tag & 0x10 ? bytes.at(j++) : 1;
                    } else {
                        color = kind == 0 ? 0 : kind == 3 ? 255 : bytes.at(j++);
                        run = tag & 15;
                        for (unsigned extra = (tag >> 4) & 3; extra > 0; --extra)
                            run += unsigned(bytes.at(j++)) << (4 + 8 * (extra-1));
                    }
                    REQUIRE(run > 0);
                    REQUIRE(decoded.size() + run <= edited.image(i).buf.size());
                    decoded.insert(decoded.end(), run, color);
                }
                REQUIRE(decoded == edited.image(i).buf);
                row = end + 2;
            }
            REQUIRE(bytes.size() < 200000); // Tiny masks must remain compressed.
        }
    }
}

TEST_CASE("CWS stores real exposure and two-stage motion commands", "[sla_reference][sla_cws]")
{
    const std::string format=GENERATE(std::string("cws"),std::string("cwsxml"),std::string("cwsnovamaker"),std::string("cwsrgb"));
    CAPTURE(format);
    const bool packed=format=="cwsrgb";
    const int display_x=packed?201:200,display_y=packed?102:100;
    Model model;
    auto *object = model.add_object("cws", "", make_cube(4., 3., 0.3));
    object->add_instance()->set_offset(Vec3d(10., 10., 0.));
    SLAFullPrintConfig full;
    full.printer_technology.setInt(ptSLA);
    full.set("sla_archive_format", format);
    full.set("display_width", 100.); full.set("display_height", 60.);
    full.set("display_pixels_x", display_x); full.set("display_pixels_y", display_y);
    full.display_orientation.value = GENERATE(sladoLandscape, sladoPortrait);
    full.set("supports_enable", false); full.set("pad_enable", false); full.set("support_object_elevation", 0.);
    full.set("layer_height", 0.05); full.set("initial_layer_height", 0.1);
    full.set("sla_bottom_layers", 1); full.set("sla_transition_layers", 2);
    full.set("exposure_time", 2.5); full.set("initial_exposure_time", 20.);
    for (const bool bottom : {false, true}) {
        const std::string prefix = bottom ? "sla_bottom_" : "sla_";
        full.set(prefix+"lift_height", bottom ? 4. : 7.);
        full.set(prefix+"lift_height2", bottom ? 1. : 2.);
        full.set(prefix+"lift_speed", bottom ? 60. : 90.);
        full.set(prefix+"lift_speed2", bottom ? 100. : 150.);
        full.set(prefix+"retract_height2", bottom ? 0.5 : 1.);
        full.set(prefix+"retract_speed", bottom ? 120. : 180.);
        full.set(prefix+"retract_speed2", bottom ? 90. : 150.);
        full.set(prefix+"wait_before_cure", bottom ? 0.7 : 0.4);
        full.set(prefix+"wait_after_cure", bottom ? 0.2 : 0.3);
        full.set(prefix+"wait_after_lift", bottom ? 0.8 : 0.6);
        full.set(prefix+"light_pwm", bottom ? 220 : 200);
    }
    DynamicPrintConfig config; config.apply(full);
    SLAPrint print; print.set_status_callback([](const PrintBase::SlicingStatus &) {});
    print.apply(model, config); print.process();
    struct File {
        boost::filesystem::path path = boost::filesystem::unique_path("cws-reference-%%%%.cws");
        bool keep = false;
        ~File() { if (!keep) { boost::system::error_code error; boost::filesystem::remove(path, error); } }
    } file;
    if (const char *output = std::getenv("SLA_CWS_REFERENCE_PATH")) { file.path = output; file.keep = true; }
    if(format=="cwsxml") file.path.replace_extension(".xml.cws");
    if(packed)file.path.replace_extension(".rgb.cws");
    if(format=="cwsnovamaker" && file.keep)file.path=file.path.parent_path()/"novamaker-reference.cws";
    if(packed && file.keep)file.path=file.path.parent_path()/"rgb-cws-reference.rgb.cws";
    print.export_print(file.path.string(), {}, "../../unsafe&project42");
    const auto zip = read_zipper_archive(file.path.string(), {""}, {});
    auto contents = [&](const std::string &name) {
        const auto entry = std::find_if(zip.entries.begin(), zip.entries.end(), [&](const auto &e) { return e.fname == name; });
        REQUIRE(entry != zip.entries.end());
        return std::string(entry->buf.begin(), entry->buf.end());
    };
    for(size_t i=0;i<print.print_layers().size();++i) {
        std::ostringstream name;name<<"sla_job"<<std::setw(5)<<std::setfill('0')<<i<<".png";
        const auto bytes=contents(name.str());
        REQUIRE(bytes.size()>25);REQUIRE(uint8_t(bytes[25])==uint8_t(packed?2:format=="cwsxml"?6:0));
        std::vector<unsigned char> rgba;unsigned width=0,height=0;
        REQUIRE(png::decode_png(bytes,rgba,width,height));
        const auto mask=print.layer_document()->image(i);
        REQUIRE(width==mask.cols/(packed?3:1));REQUIRE(height==mask.rows);REQUIRE(rgba.size()==mask.buf.size()*4/(packed?3:1));
        bool pixels_match=true;
        for(size_t j=0;j<mask.buf.size();++j)
            pixels_match &= packed ? rgba[4*(j/3)+2-j%3]==mask.buf[j] && rgba[4*(j/3)+3]==255 :
                rgba[4*j]==mask.buf[j] && rgba[4*j+1]==mask.buf[j] && rgba[4*j+2]==mask.buf[j] && rgba[4*j+3]==255;
        REQUIRE(pixels_match);
    }
    if(format=="cwsxml" || format=="cwsnovamaker" || packed) {
        auto document=sla::LayerDocument::load_archive(file.path.string());
        REQUIRE(document.archive_format()==format);
        document.invert(0);File edited;
        if(format=="cwsxml")edited.path.replace_extension(".xml.cws");
        if(packed)edited.path.replace_extension(".rgb.cws");
        document.save_archive(edited.path.string());
        REQUIRE(sla::LayerDocument::load_archive(edited.path.string()).image(0).buf==document.image(0).buf);
        const auto saved=read_zipper_archive(edited.path.string(),{""},{});
        for(const auto &entry:saved.entries) {
            if(entry.fname=="manifest.xml" || entry.fname=="sla_job.slicing" || entry.fname=="sla_job.gcode" || entry.fname=="slice.conf")
                REQUIRE(std::string(entry.buf.begin(),entry.buf.end())==contents(entry.fname));
            if(entry.fname.size()>4 && entry.fname.substr(entry.fname.size()-4)==".png") {
                REQUIRE(entry.buf.size()>25);REQUIRE(entry.buf[25]==(packed?2:format=="cwsxml"?6:0));
            }
        }
        REQUIRE_THROWS(document.save_archive(edited.path.string()+".zip"));
    }
    REQUIRE((contents("sla_job.gcode").find(";<Takes> ") != std::string::npos) == (format == "cwsxml"));
    const bool portrait = full.display_orientation.value == sladoPortrait;
    if(format=="cwsnovamaker" || packed) {
        std::map<std::string,double> fields;
        std::istringstream settings(contents("slice.conf"));
        for(std::string line;std::getline(settings,line);) {
            const auto split=line.find('=');if(split==std::string::npos)continue;
            fields.emplace(boost::algorithm::trim_copy(line.substr(0,split)),std::stod(line.substr(split+1)));
        }
        REQUIRE(fields.size()==14);
        REQUIRE(fields.at("xres")== (portrait?display_y:display_x)); REQUIRE(fields.at("yres")== (portrait?display_x:display_y));
        REQUIRE(fields.at("xppm")==Catch::Approx(portrait?display_y/60.:display_x/100.));
        REQUIRE(fields.at("yppm")==Catch::Approx(portrait?display_x/100.:display_y/60.));
        REQUIRE(fields.at("thickness")==Catch::Approx(.05));REQUIRE(fields.at("layers_num")==print.print_layers().size());
        REQUIRE(fields.at("head_layers_num")==1); REQUIRE(fields.at("layers_expo_ms")==2500);
        REQUIRE(fields.at("head_layers_expo_ms")==20000);REQUIRE(fields.at("wait_before_expo_ms")==400);
        REQUIRE(fields.at("lift_distance")==7);REQUIRE(fields.at("lift_up_speed")==90);
        REQUIRE(fields.at("lift_down_speed")==180);REQUIRE(fields.at("lift_when_finished")==0);
        REQUIRE(std::none_of(zip.entries.begin(),zip.entries.end(),[](const auto &e){return e.fname=="manifest.xml" || e.fname=="sla_job.slicing";}));
    } else {
        const auto profile = contents("sla_job.slicing");
        const std::string start="<DotsPermmX>",end="</DotsPermmX>";
        const auto at=profile.find(start);REQUIRE(at!=std::string::npos);
        const auto finish=profile.find(end,at+start.size());REQUIRE(finish!=std::string::npos);
        const double dots=std::stod(profile.substr(at+start.size(),finish-at-start.size()));
        REQUIRE(dots==Catch::Approx(portrait?display_y/60.:display_x/100.).margin(1e-8));
        REQUIRE(contents("manifest.xml").find("unsafe") == std::string::npos);
    }
    std::istringstream stream(contents("sla_job.gcode"));
    std::string line;
    size_t selected = size_t(-1), exposed = 0, movements = 0;
    double z = 0, exposure = 0; int pwm = 0;
    while (std::getline(stream, line)) {
        if (line.rfind(";LAYER_START:", 0) == 0) selected = std::stoul(line.substr(13));
        else if (line.rfind("G1 Z", 0) == 0) {
            REQUIRE(selected < print.print_layers().size());
            const auto separator = line.find(" F"); REQUIRE(separator != std::string::npos);
            const double distance = std::stod(line.substr(4, separator-4));
            const double speed = std::stod(line.substr(separator+2));
            REQUIRE(speed > 0); z += distance; ++movements;
        } else if (line.rfind("M106 S", 0) == 0) pwm = std::stoi(line.substr(6));
        else if (line.rfind(";<Delay> ", 0) == 0 && pwm > 0) {
            exposure = std::stod(line.substr(9)) / 1000.;
            REQUIRE(z == Catch::Approx(double(print.print_layers()[selected].level()) * SCALING_FACTOR).margin(0.0001));
            const double expected = selected == 0 ? 20. : selected == 1 ? 20.-17.5/3 : selected == 2 ? 20.-35./3 : 2.5;
            REQUIRE(exposure == Catch::Approx(expected).margin(0.001));
            REQUIRE(pwm == (selected == 0 ? 220 : 200)); ++exposed;
        }
    }
    REQUIRE(exposed == print.print_layers().size()); REQUIRE(movements == 4 * exposed);
    SECTION("SL1 stores canonical LCD settings through UVtools note keywords") {
        struct SL1File {
            boost::filesystem::path path = boost::filesystem::unique_path("sl1-lcd-%%%%.sl1");
            bool keep = false;
            ~SL1File() { if (!keep) { boost::system::error_code error; boost::filesystem::remove(path, error); } }
        } sl1;
        if (const char *output = std::getenv("SLA_SL1_REFERENCE_PATH")) { sl1.path = output; sl1.keep = true; }
        config.set("material_notes", std::string("Operator note\nLiftHeight_999\nBottomLightPWM_111"), true);
        config.set("printer_notes", std::string("Hardware note\nTransitionLayerCount_99\nBottomLiftHeight_999"), true);
        print.apply(model, config); print.process();
        print.export_print(sl1.path.string(), {}, "reference");
        const auto archive = read_zipper_archive(sl1.path.string(), {"ini"}, {});
        REQUIRE(archive.config.get<int>("numFade") == 1);
        const auto notes = archive.profile.get<std::string>("printer_notes");
        REQUIRE(notes.find("Hardware note") != std::string::npos);
        REQUIRE(archive.profile.get<std::string>("material_notes") == "Operator note");
        REQUIRE(notes.find("BottomLiftHeight_999") == std::string::npos);
        REQUIRE(notes.find("TransitionLayerCount_99") == std::string::npos);
        REQUIRE(notes.find("TransitionLayerCount_2") != std::string::npos);
        REQUIRE(notes.find("BottomLiftHeight_4.000000000") != std::string::npos);
        REQUIRE(notes.find("LiftHeight2_2.000000000") != std::string::npos);
        REQUIRE(notes.find("BottomLightPWM_220") != std::string::npos);
        REQUIRE(notes.find("LightPWM_200") != std::string::npos);
        REQUIRE(sla::LayerDocument::load_sl1(sl1.path.string()).layer_count() == exposed);
    }
    SECTION("UVJ preserves all layer parameters and exact grayscale masks") {
        struct UVJFile {
            boost::filesystem::path path = boost::filesystem::unique_path("uvj-reference-%%%%.uvj");
            bool keep = false;
            ~UVJFile() { if (!keep) { boost::system::error_code error; boost::filesystem::remove(path, error); } }
        } uvj;
        if (const char *output = std::getenv("SLA_UVJ_REFERENCE_PATH")) { uvj.path = output; uvj.keep = true; }
        const auto old_mask = print.layer_document()->image(0).buf;
        config.set("sla_archive_format", std::string("uvj")); print.apply(model, config); print.process();
        REQUIRE(print.layer_document()->image(0).buf == old_mask);
        print.export_print(uvj.path.string());
        const auto archive = read_zipper_archive(uvj.path.string(), {""}, {});
        auto entry = [&](const std::string &name) -> const std::vector<uint8_t> & {
            auto found = std::find_if(archive.entries.begin(), archive.entries.end(), [&](const auto &e) { return e.fname == name; });
            REQUIRE(found != archive.entries.end()); return found->buf;
        };
        const auto json = nlohmann::json::parse(entry("config.json"));
        REQUIRE(json["Properties"]["Size"]["X"] == (portrait ? display_y : display_x));
        REQUIRE(json["Properties"]["Size"]["Y"] == (portrait ? display_x : display_y));
        REQUIRE(json["Properties"]["Size"]["Millimeter"]["X"] == (portrait ? 60 : 100));
        REQUIRE(json["Properties"]["Size"]["Millimeter"]["Y"] == (portrait ? 100 : 60));
        REQUIRE(json["Properties"]["Bottom"]["Count"] == 1);
        REQUIRE(json["Layers"].size() == exposed);
        for (size_t i = 0; i < exposed; ++i) {
            const auto &layer = json["Layers"][i];
            const std::string prefix = i == 0 ? "sla_bottom_" : "sla_";
            REQUIRE(layer["Z"].get<double>() == Catch::Approx(double(print.print_layers()[i].level()) * SCALING_FACTOR));
            for (const auto &key : std::vector<std::pair<const char *, const char *>>{
                {"WaitTimeBeforeCure", "wait_before_cure"}, {"WaitTimeAfterCure", "wait_after_cure"},
                {"LiftHeight", "lift_height"}, {"LiftSpeed", "lift_speed"}, {"LiftHeight2", "lift_height2"},
                {"LiftSpeed2", "lift_speed2"}, {"WaitTimeAfterLift", "wait_after_lift"}, {"RetractSpeed", "retract_speed"},
                {"RetractHeight2", "retract_height2"}, {"RetractSpeed2", "retract_speed2"}})
                REQUIRE(layer["Exposure"][key.first].get<double>() == Catch::Approx(config.opt_float(prefix+key.second)));
            REQUIRE(layer["Exposure"]["LightPWM"] == (i == 0 ? 220 : 200));
            const double expected = i == 0 ? 20. : i == 1 ? 20.-17.5/3 : i == 2 ? 20.-35./3 : 2.5;
            REQUIRE(layer["Exposure"]["LightOnTime"].get<double>() == Catch::Approx(expected));
            std::ostringstream name; name << "slice/" << std::setw(8) << std::setfill('0') << i << ".png";
            const auto &bytes = entry(name.str()); png::ImageGreyscale mask;
            REQUIRE(png::decode_png(png::ReadBuf{bytes.data(), bytes.size()}, mask));
            REQUIRE(mask.buf == print.layer_document()->image(i).buf);
        }
        REQUIRE(png::is_png(png::ReadBuf{entry("preview/tiny.png").data(), entry("preview/tiny.png").size()}));
        REQUIRE(png::is_png(png::ReadBuf{entry("preview/huge.png").data(), entry("preview/huge.png").size()}));
    }
    for (size_t i = 0; i < exposed; ++i) {
        std::ostringstream name; name << "sla_job" << std::setw(5) << std::setfill('0') << i << ".png";
        const auto bytes = contents(name.str());
        png::ImageGreyscale decoded;
        if(packed) {
            std::vector<unsigned char> pixels;unsigned width=0,height=0;
            REQUIRE(png::decode_png(bytes,pixels,width,height));
            decoded.cols=3*width;decoded.rows=height;decoded.buf.resize(width*height*3);
            for(size_t j=0;j<decoded.buf.size();++j)decoded.buf[j]=pixels[4*(j/3)+2-j%3];
        } else REQUIRE(png::decode_color_png(png::ReadBuf{bytes.data(), bytes.size()}, decoded));
        REQUIRE(decoded.buf == print.layer_document()->image(i).buf);
    }
}

TEST_CASE("CWS color conversion reuse preserves changed and repeated masks", "[sla_reference][sla_cws_cache]") {
    const std::string format=GENERATE(std::string("cwsrgb"),std::string("cwsxml"));
    SLAFullPrintConfig full;full.printer_technology.setInt(ptSLA);
    full.set("sla_archive_format",format);full.display_orientation.value=sladoLandscape;
    full.set("display_width",3.);full.set("display_height",1.);
    full.set("display_pixels_x",6);full.set("display_pixels_y",2);
    full.set("layer_height",.05);full.set("initial_layer_height",.05);
    DynamicPrintConfig config;config.apply(full);
    const std::vector<uint8_t> a{0,17,85,128,254,255,20,40,60,80,100,120};
    auto b=a;std::reverse(b.begin(),b.end());
    const auto first=sla::PNGRasterEncoder{}(a.data(),6,2,1),second=sla::PNGRasterEncoder{}(b.data(),6,2,1);
    const auto document=sla::LayerDocument::from_rasters({first,first,second,first,second,second});
    auto print=SLAPrint::create_raster_job(config,document,{.05,.1,.15,.2,.25,.3});
    struct File {
        boost::filesystem::path path;
        ~File(){boost::system::error_code error;boost::filesystem::remove(path,error);}
    } file{boost::filesystem::unique_path("cws-conversion-%%%%."+std::string(format=="cwsrgb"?"rgb.cws":"xml.cws"))};
    print->export_print(file.path.string());
    const auto restored=sla::LayerDocument::load_archive(file.path.string());
    REQUIRE(restored.layer_count()==6);
    for(size_t i=0;i<6;++i)REQUIRE(restored.image(i).buf==(i==2 || i>=4?b:a));
}

namespace {
struct AnetReferenceReader {
    const std::vector<uint8_t> &bytes;
    size_t position=0;
    uint32_t u32() {
        if(position+4>bytes.size()) throw std::runtime_error("truncated Anet reference header");
        uint32_t value=0;for(unsigned i=0;i<4;++i) value=(value<<8)|bytes[position++];return value;
    }
    double number() {
        if(position+8>bytes.size()) throw std::runtime_error("truncated Anet binary64");
        uint64_t bits=0;for(unsigned i=0;i<8;++i) bits=(bits<<8)|bytes[position++];
        double value;std::memcpy(&value,&bits,8);return value;
    }
    std::u16string text() {
        const uint32_t size=u32();if(size%2 || size>65536 || position+size>bytes.size()) throw std::runtime_error("invalid Anet UTF-16BE");
        std::u16string result;for(size_t end=position+size;position<end;position+=2) result.push_back(char16_t((uint16_t(bytes[position])<<8)|bytes[position+1]));return result;
    }
    std::vector<uint8_t> take(size_t size) {
        if(position+size>bytes.size()) throw std::runtime_error("truncated Anet block");
        std::vector<uint8_t> result(bytes.begin()+position,bytes.begin()+position+size);position+=size;return result;
    }
};
std::vector<uint8_t> decode_anet_bits(const std::vector<uint8_t> &bytes,uint32_t count,size_t expected_columns,size_t expected_rows) {
    uint32_t position=0;
    auto bits=[&](unsigned size) {
        if(size>32 || uint64_t(position)+size>count || (uint64_t(count)+7)/8!=bytes.size()) throw std::runtime_error("invalid Anet bit bounds");
        uint32_t result=0;for(unsigned i=0;i<size;++i,++position) result=(result<<1)|((bytes[position/8]>>(position%8))&1);return result;
    };
    const auto columns=bits(16),rows=bits(16);REQUIRE(columns==expected_columns);REQUIRE(rows==expected_rows);
    bool white=bits(1)!=0;std::vector<uint8_t> decoded;decoded.reserve(size_t(columns)*rows);
    while(decoded.size()<size_t(columns)*rows) {
        const auto key=bits(5);const auto run=bits(key+1);
        if(!run || run>size_t(columns)*rows-decoded.size()) throw std::runtime_error("invalid Anet pixel run");
        decoded.insert(decoded.end(),run,white?255:0);white=!white;
    }
    REQUIRE(position==count);return decoded;
}
}
TEST_CASE(
    "Native Anet headers and bit streams preserve representable parameters",
    "[sla_reference][sla_anet]") {
  const bool n7 = GENERATE(false, true), portrait = GENERATE(false, true);
  const size_t columns = n7 ? 2560 : 1440, rows = n7 ? 1600 : 2560;
  const double width = n7 ? 192. : 68.04, height = n7 ? 120. : 120.96,
               max_z = n7 ? 300. : 135.;
  CAPTURE(n7, portrait);
  DynamicPrintConfig config;
  config.apply(SLAFullPrintConfig{});
  config.set("sla_archive_format", std::string(n7 ? "n7" : "n4"));
  config.set("display_width", portrait ? height : width);
  config.set("display_height", portrait ? width : height);
  config.set("display_pixels_x", int(portrait ? rows : columns));
  config.set("display_pixels_y", int(portrait ? columns : rows));
  config.option("display_orientation")
      ->setInt(portrait ? sladoPortrait : sladoLandscape);
  config.set("max_print_height", max_z);
  config.set("layer_height", .05);
  config.set("initial_layer_height", .05);
  config.set("exposure_time", 3.);
  config.set("initial_exposure_time", 20.);
  config.set("sla_bottom_layers", 1);
  config.set("sla_transition_layers", 0);
  for (const char *key : {"sla_lift_speed", "sla_bottom_lift_speed",
                          "sla_retract_speed", "sla_bottom_retract_speed"})
    config.set(key, 120.);
  png::ImageGreyscale a;
  a.cols = columns;
  a.rows = rows;
  a.buf.resize(columns * rows, 0);
  a.buf[0] = 255;
  a.buf[1] = 1;
  a.buf[2] = 127;
  a.buf[3] = 128;
  a.buf[4] = 255;
  a.buf[6] = 170;
  a.buf.back() = 255;
  auto b = a;
  b.buf.assign(a.buf.size(), 0);
  auto c = b;
  c.buf.back() = 255;
  auto encode = [](const auto &image) {
    return sla::PNGRasterEncoder{}(image.buf.data(), image.cols, image.rows, 1);
  };
  auto doc =
      sla::LayerDocument::from_rasters({encode(a), encode(b), encode(c)});
  auto print = SLAPrint::create_raster_job(config, doc, {.05, .1, .15});
  SLAPrinterConfig printer;
  printer.apply(config, true);
  std::unique_ptr<SLAArchiveWriter> writer =
      n7 ? std::unique_ptr<SLAArchiveWriter>(new AnetN7Archive(printer))
         : std::unique_ptr<SLAArchiveWriter>(new AnetN4Archive(printer));
  writer->encode_layers(doc);
  ThumbnailData thumbnail;
  thumbnail.set(2, 2);
  thumbnail.pixels = {255, 0, 0,   255, 0,   255, 0,   255,
                      0,   0, 255, 255, 255, 255, 255, 255};
  struct File {
    boost::filesystem::path path;
    ~File() {
      boost::system::error_code e;
      boost::filesystem::remove(path, e);
    }
  } file{boost::filesystem::absolute(boost::filesystem::unique_path(
      n7 ? "anet-ref-%%%%.n7" : "anet-ref-%%%%.n4"))};
  writer->export_print(file.path.string(), *print, {thumbnail}, "PCB Żółć Ω");
  // Exercise the same factory/document export path used by the GUI.
  print->export_layer_document(doc, n7 ? "n7" : "n4", file.path.string(),
                               {thumbnail}, "PCB Żółć Ω");
  if (const char *directory = std::getenv("SLA_ANET_REFERENCE_DIR")) {
    boost::filesystem::create_directories(directory);
    boost::filesystem::copy_file(
        file.path,
        boost::filesystem::path(directory) /
            (n7 ? (portrait ? "portrait.n7" : "landscape.n7")
                : (portrait ? "portrait.n4" : "landscape.n4")),
        boost::filesystem::copy_option::overwrite_if_exists);
  }
  std::ifstream input(file.path.string(), std::ios::binary);
  const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(input),
                                   std::istreambuf_iterator<char>()};
  AnetReferenceReader read{bytes};
  REQUIRE(read.text() == u"3");
  REQUIRE(read.text() == u"PCB Żółć Ω");
  REQUIRE(read.text().empty());
  REQUIRE(read.number() == Catch::Approx(width / columns).margin(1e-9));
  REQUIRE(read.number() == Catch::Approx(.05).margin(1e-9));
  REQUIRE(read.u32() == 0);
  REQUIRE(read.u32() == 0);
  REQUIRE(read.u32() == 3);
  REQUIRE(read.u32() == 20);
  REQUIRE(read.u32() == 1);
  REQUIRE(read.u32() == 2);
  REQUIRE(read.u32() == 6);
  REQUIRE(read.u32() == 260);
  REQUIRE(read.u32() == 140);
  REQUIRE(read.u32() == 72866);
  const auto preview = read.take(72866);
  REQUIRE(preview[0] == 'B');
  REQUIRE(preview[1] == 'M');
  REQUIRE(preview[66 + 60 * 2] == 31);
  REQUIRE(preview[66 + 60 * 2 + 1] == 0); // top-left blue
  REQUIRE(preview[66 + (139 * 260 + 60) * 2] == 0);
  REQUIRE(preview[66 + (139 * 260 + 60) * 2 + 1] == 248); // bottom-left red
  REQUIRE(read.number() ==
          Catch::Approx(6. * (width / columns) * (height / rows) * .05)
              .margin(1e-9));
  REQUIRE(read.u32() == 44);
  REQUIRE(read.u32() == 3);
  size_t layer = 0;
  for (const auto *mask : {&a, &b, &c}) {
    const uint32_t white = read.u32(), xmin = read.u32(), ymin = read.u32(),
                   xmax = read.u32(), ymax = read.u32(), count = read.u32();
    auto raw = read.take((uint64_t(count) + 7) / 8);
    const auto decoded = decode_anet_bits(raw, count, columns, rows);
    std::vector<uint8_t> expected = mask->buf;
    for (auto &pixel : expected)
      pixel = pixel > 127 ? 255 : 0;
    REQUIRE(decoded == expected);
    REQUIRE(white ==
            std::count(expected.begin(), expected.end(), uint8_t(255)));
    if (layer == 1) {
      REQUIRE(xmin == 0);
      REQUIRE(ymin == 0);
      REQUIRE(xmax == 0);
      REQUIRE(ymax == 0);
    } else {
      REQUIRE(xmax == columns - 1);
      REQUIRE(ymax == rows - 1);
      REQUIRE(xmin == (layer == 2 ? columns - 1 : 0));
      REQUIRE(ymin == (layer == 2 ? rows - 1 : 0));
    }
    ++layer;
  }
  REQUIRE(read.u32() == 0);
  REQUIRE(read.position == bytes.size());
  SECTION("unsupported exposure does not overwrite the existing file") {
    auto incompatible = config;
    incompatible.set("exposure_time", 2.5);
    auto other = SLAPrint::create_raster_job(incompatible, doc, {.05, .1, .15});
    REQUIRE_THROWS(writer->export_print(file.path.string(), *other, {thumbnail},
                                        "invalid"));
    std::ifstream previous(file.path.string(), std::ios::binary);
    const std::vector<uint8_t> preserved{
        std::istreambuf_iterator<char>(previous),
        std::istreambuf_iterator<char>()};
    REQUIRE(preserved == bytes);
  }
  SECTION("unsupported geometry and first Z are rejected") {
    auto incompatible = config;
    incompatible.set("display_width", 100.);
    SLAPrinterConfig changed;
    changed.apply(incompatible, true);
    std::unique_ptr<SLAArchiveWriter> invalid =
        n7 ? std::unique_ptr<SLAArchiveWriter>(new AnetN7Archive(changed))
           : std::unique_ptr<SLAArchiveWriter>(new AnetN4Archive(changed));
    invalid->encode_layers(doc);
    REQUIRE_THROWS(invalid->export_print(file.path.string(), *print,
                                         {thumbnail}, "invalid"));
    auto other = SLAPrint::create_raster_job(config, doc, {.1, .15, .2});
    REQUIRE_THROWS(writer->export_print(file.path.string(), *other, {thumbnail},
                                        "invalid"));
  }
}

TEST_CASE("Native OSLA indexed PNG layers preserve masks and full layer motion",
          "[sla_reference][sla_osla]") {
  const bool portrait = GENERATE(false, true);
  DynamicPrintConfig config;
  config.apply(SLAFullPrintConfig{});
  config.set("sla_archive_format", std::string("osla"));
  config.set("display_width", 8.);
  config.set("display_height", 5.);
  config.set("display_pixels_x", 16);
  config.set("display_pixels_y", 10);
  config.option("display_orientation")
      ->setInt(portrait ? sladoPortrait : sladoLandscape);
  config.set("layer_height", .05);
  config.set("initial_layer_height", .1);
  config.set("sla_bottom_layers", 1);
  config.set("sla_transition_layers", 2);
  config.set("exposure_time", 2.5);
  config.set("initial_exposure_time", 20.);
  config.set("printer_model", std::string("OSLA Żółć Ω"));
  for (const bool bottom : {false, true}) {
    const std::string prefix = bottom ? "sla_bottom_" : "sla_";
    config.set(prefix + "lift_height", bottom ? 7. : 5.);
    config.set(prefix + "lift_height2", 2.);
    config.set(prefix + "retract_height2", 1.);
    config.set(prefix + "lift_speed2", 180.);
    config.set(prefix + "retract_speed2", 90.);
    config.set(prefix + "wait_before_cure", .7);
    config.set(prefix + "wait_after_cure", .2);
    config.set(prefix + "wait_after_lift", .4);
    config.set(prefix + "light_pwm", bottom ? 220 : 200);
  }
  const size_t columns = portrait ? 10 : 16, rows = portrait ? 16 : 10;
  png::ImageGreyscale a;
  a.cols = columns;
  a.rows = rows;
  a.buf.resize(columns * rows, 0);
  a.buf[0] = 1;
  a.buf[3] = 128;
  a.buf.back() = 255;
  auto b = a;
  b.buf.assign(columns * rows, 0);
  b.buf[columns + 2] = 127;
  auto encode = [](const auto &image) {
    return sla::PNGRasterEncoder{}(image.buf.data(), image.cols, image.rows, 1);
  };
  const auto doc =
      sla::LayerDocument::from_rasters({encode(a), encode(b), encode(a)});
  const auto print = SLAPrint::create_raster_job(config, doc, {.1, .15, .22});
  SLAPrinterConfig printer;
  printer.apply(config, true);
  OpenSLAArchive writer(printer);
  writer.encode_layers(doc);
  struct File {
    boost::filesystem::path path =
        boost::filesystem::unique_path("osla-reference-%%%%.osla");
    bool keep = false;
    ~File() {
      if (!keep) {
        boost::system::error_code error;
        boost::filesystem::remove(path, error);
      }
    }
  } file;
  if (const char *directory = std::getenv("SLA_OSLA_REFERENCE_DIR")) {
    boost::filesystem::create_directories(directory);
    file.path = boost::filesystem::path(directory) /
                (portrait ? "portrait.osla" : "landscape.osla");
    file.keep = true;
  }
  writer.export_print(file.path.string(), *print, {}, "reference");
  print->export_layer_document(doc, "osla", file.path.string(), {},
                               "reference");
  std::ifstream input(file.path.string(), std::ios::binary);
  std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(input),
                             std::istreambuf_iterator<char>()};
  size_t position = 0;
  auto u8 = [&]() {
    REQUIRE(position < bytes.size());
    return bytes[position++];
  };
  auto u16 = [&]() {
    const auto lo = u8();
    return uint16_t(lo | (uint16_t(u8()) << 8));
  };
  auto u32 = [&]() {
    uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i)
      value |= uint32_t(u8()) << (8 * i);
    return value;
  };
  auto number = [&]() {
    const auto bits = u32();
    float value;
    std::memcpy(&value, &bits, 4);
    return value;
  };
  auto text = [&](size_t count) {
    REQUIRE(position + count <= bytes.size());
    std::string value(reinterpret_cast<const char *>(bytes.data() + position),
                      count);
    position += count;
    return value.substr(0, value.find('\0'));
  };
  REQUIRE(text(8) == "OSLATiCo");
  REQUIRE(u16() == 0);
  REQUIRE(text(20).size() == 20);
  REQUIRE(text(50) == "SLA-Slicer");
  REQUIRE(text(20).size() == 20);
  REQUIRE(text(50) == "SLA-Slicer");
  REQUIRE(position == 150);
  REQUIRE(u32() == 199);
  REQUIRE(u32() == columns);
  REQUIRE(u32() == rows);
  REQUIRE(number() == Catch::Approx(config.opt_float("max_print_height")));
  REQUIRE(number() == Catch::Approx(portrait ? 5. : 8.));
  REQUIRE(number() == Catch::Approx(portrait ? 8. : 5.));
  REQUIRE(u8() == 0);
  REQUIRE(text(16) == "PNG");
  REQUIRE(text(16) == "PNG");
  REQUIRE(u32() == 8);
  REQUIRE(u32() == 2);
  REQUIRE(number() == Catch::Approx(.05));
  REQUIRE(u16() == 1);
  REQUIRE(u32() == 3);
  REQUIRE(u32() == 73);
  const auto table = u32(), program = u32();
  REQUIRE(u32() > 0);
  REQUIRE(number() == Catch::Approx(.00014).margin(1e-8));
  REQUIRE(number() >= 0);
  REQUIRE(text(50).empty());
  REQUIRE(text(50) == "OSLA Żółć Ω");
  REQUIRE(position == 349);
  REQUIRE(u32() == 0);
  for (size_t side : {size_t(400), size_t(200)}) {
    REQUIRE(u16() == side);
    REQUIRE(u16() == side);
    const auto count = u32();
    REQUIRE(position + count <= bytes.size());
    REQUIRE(png::is_png(png::ReadBuf{bytes.data() + position, count}));
    position += count;
  }
  REQUIRE(position == table);
  std::vector<uint32_t> addresses;
  for (size_t i = 0; i < 3; ++i) {
    position = table + i * 73;
    addresses.push_back(u32());
    REQUIRE(number() == Catch::Approx(i == 0 ? .1 : i == 1 ? .15 : .22));
    const std::string prefix = i == 0 ? "sla_bottom_" : "sla_";
    for (const char *key :
         {"lift_height", "lift_speed", "lift_height2", "lift_speed2",
          "wait_after_lift", "retract_speed", "retract_height2",
          "retract_speed2", "wait_before_cure"})
      REQUIRE(number() == Catch::Approx(config.opt_float(prefix + key)));
    REQUIRE(number() == Catch::Approx(i == 0   ? 20.
                                      : i == 1 ? 20. - 17.5 / 3
                                               : 20. - 35. / 3));
    REQUIRE(number() == Catch::Approx(.2));
    REQUIRE(u8() == (i == 0 ? 220 : 200));
    REQUIRE(u32() == (i == 1 ? 1 : 3));
    REQUIRE(u32() == (i == 1 ? 2 : 0));
    REQUIRE(u32() == (i == 1 ? 1 : 0));
    REQUIRE(u32() == (i == 1 ? 1 : columns));
    REQUIRE(u32() == (i == 1 ? 1 : rows));
    REQUIRE(position == table + (i + 1) * 73);
  }
  REQUIRE(addresses[0] == addresses[2]);
  REQUIRE(addresses[0] != addresses[1]);
  for (size_t i = 0; i < 3; ++i) {
    position = addresses[i];
    const auto size = u32();
    REQUIRE(position + size <= bytes.size());
    png::ImageGreyscale decoded;
    REQUIRE(
        png::decode_png(png::ReadBuf{bytes.data() + position, size}, decoded));
    REQUIRE(decoded.cols == columns);
    REQUIRE(decoded.rows == rows);
    REQUIRE(decoded.buf == (i == 1 ? b.buf : a.buf));
  }
  position = program;
  const auto size = u32();
  const auto motion = text(size);
  REQUIRE(motion.find("M6054 2;") != std::string::npos);
  REQUIRE(motion.find("M106 S220.000000;") != std::string::npos);
  REQUIRE(motion.find("G4 P700.000000;") != std::string::npos);
  REQUIRE(text(9) == ";OSLATiCo");
  REQUIRE(position == bytes.size());
}

TEST_CASE("Native OSF v4 preserves its tables and seven-bit masks", "[sla_reference][sla_osf]") {
    const bool portrait=GENERATE(false,true);
    DynamicPrintConfig config;config.apply(SLAFullPrintConfig{});
    config.set("display_width",60.);config.set("display_height",40.);
    config.set("display_pixels_x",300);config.set("display_pixels_y",200);
    config.option("display_orientation")->setInt(portrait ? sladoPortrait : sladoLandscape);
    config.set("layer_height",.05);config.set("initial_layer_height",.05);
    config.set("exposure_time",2.);config.set("initial_exposure_time",20.);
    config.set("sla_bottom_layers",1);config.set("sla_transition_layers",2);
    config.set("sla_lift_height",3.);config.set("sla_lift_height2",2.);
    config.set("sla_retract_height2",1.);config.set("sla_lift_speed",60.);
    config.set("sla_lift_speed2",120.);config.set("sla_retract_speed",150.);
    config.set("sla_retract_speed2",90.);config.set("sla_wait_before_cure",.25);
    config.set("sla_wait_after_cure",.5);config.set("sla_wait_after_lift",.75);
    config.set("sla_bottom_wait_before_cure",1.25);config.set("sla_bottom_wait_after_cure",1.5);
    config.set("sla_bottom_wait_after_lift",1.75);config.set("sla_light_pwm",127);
    png::ImageGreyscale mask;mask.cols=portrait?200:300;mask.rows=portrait?300:200;
    mask.buf.resize(mask.cols*mask.rows);
    std::fill_n(mask.buf.begin()+2,10,12);mask.buf[12]=255;
    std::fill_n(mask.buf.begin()+13,11,13); // Both marker-collision run lengths.
    std::fill_n(mask.buf.begin()+100,17000,127);
    mask.buf.back()=254;mask.buf[90]=1;mask.buf[91]=2;mask.buf[92]=128;
    auto encoded=sla::PNGRasterEncoder{}(mask.buf.data(),mask.cols,mask.rows,1);
    auto document=sla::LayerDocument::from_rasters({encoded,encoded});
    auto job=SLAPrint::create_raster_job(config,document,{.05,.1});
    SLAPrinterConfig printer;printer.apply(config,true);
    UVToolsOSFArchive writer(printer);writer.encode_layers(document);
    struct File {boost::filesystem::path path=boost::filesystem::unique_path("osf-native-%%%%.osf");
        ~File(){boost::system::error_code error;boost::filesystem::remove(path,error);}} file;
    writer.export_print(file.path.string(),*job,{},"reference");
    std::ifstream input(file.path.string(),std::ios::binary);
    const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>()};
    size_t position=0;
    auto read=[&](unsigned width){uint32_t value=0;for(unsigned i=0;i<width;++i){if(position>=bytes.size())throw std::runtime_error("truncated OSF");value=(value<<8)|bytes[position++];}return value;};
    const auto header_length=read(4);REQUIRE(header_length==350001);REQUIRE(read(2)==4);REQUIRE(read(1)==2);
    for(const uint32_t size:{148u*80*2,300u*140*2,208u*116*2,404u*240*2}){REQUIRE(read(3)==size);position+=size;}
    const auto settings=position;
    auto field=[&](unsigned offset,unsigned width){position=settings+offset;return read(width);};
    REQUIRE(field(0,2)==mask.cols);REQUIRE(field(2,2)==mask.rows);REQUIRE(field(4,2)==20000);
    REQUIRE(field(6,1)==0);REQUIRE(field(8,1)==127);REQUIRE(field(12,4)==2);REQUIRE(field(18,4)==1);
    REQUIRE(field(22,3)==5000);REQUIRE(field(25,1)==1);REQUIRE(field(26,3)==200);REQUIRE(field(29,3)==2000);
    REQUIRE(field(38,1)==2);REQUIRE(field(40,3)==600);
    REQUIRE(field(43,3)==50);REQUIRE(field(46,3)==75);REQUIRE(field(49,3)==25);
    REQUIRE(field(58,3)==3000);REQUIRE(field(61,3)==5000);
    REQUIRE(field(70,3)==1000);REQUIRE(field(73,3)==5000);
    REQUIRE(field(86,2)==60);REQUIRE(field(88,2)==120);REQUIRE(field(100,2)==90);REQUIRE(field(102,2)==150);
    REQUIRE(field(117,2)==150);REQUIRE(field(119,2)==175);REQUIRE(field(121,2)==125);
    REQUIRE(settings+126==header_length);
    position=header_length;
    for(unsigned layer=0;layer<2;++layer) {
        REQUIRE(read(2)==0x0d0a);const auto runs=read(4);REQUIRE(read(2)==0);
        std::vector<uint8_t> decoded;
        for(uint32_t run=0;run<runs;++run){const auto color=read(1);uint32_t length=1;
            if(color&1){const auto first=read(1);if(!(first&0x80))length=first;
                else if((first&0xc0)==0x80)length=((first&0x3f)<<8)|read(1);
                else if((first&0xe0)==0xc0)length=((first&0x1f)<<16)|read(2);
                else if((first&0xf0)==0xe0)length=((first&0xf)<<24)|read(3);
                else throw std::runtime_error("invalid OSF run");}
            REQUIRE(length>0);REQUIRE(length<=mask.buf.size()-decoded.size());
            const auto gray=uint8_t((color&0xfe)==0?0:(color&0xfe)|1);
            decoded.insert(decoded.end(),length,gray);
        }
        auto expected=mask.buf;for(auto &gray:expected)gray=(gray&0xfe)==0?0:uint8_t((gray&0xfe)|1);
        REQUIRE(decoded==expected);
    }
    REQUIRE(position==bytes.size());
    REQUIRE(archive_export_unavailable_reason("osf").empty());
    REQUIRE_NOTHROW(job->export_print(file.path.string()));
    boost::filesystem::create_directories("osf-reference");
    boost::filesystem::copy_file(file.path,portrait?"osf-reference/portrait.osf":"osf-reference/landscape.osf",boost::filesystem::copy_option::overwrite_if_exists);
    auto incompatible=config;incompatible.set("display_height",41.);printer.apply(incompatible,true);
    UVToolsOSFArchive nonsquare(printer);nonsquare.encode_layers(document);
    REQUIRE_THROWS(nonsquare.export_print(file.path.string(),*job,{},"bad"));
    auto variable=SLAPrint::create_raster_job(config,document,{.1,.15});
    REQUIRE_THROWS(writer.export_print(file.path.string(),*variable,{},"bad"));
}

TEST_CASE("Shared ZIP aliases preserve the selected SLA container", "[sla_reference][sla_alias]") {
    DynamicPrintConfig config;config.apply(SLAFullPrintConfig{});
    config.set("sla_archive_format",std::string("SL1SVG"));
    config.set("display_width",100.);config.set("display_height",60.);
    config.set("display_pixels_x",200);config.set("display_pixels_y",120);
    config.set("layer_height",.05);config.set("initial_layer_height",.05);
    config.set("supports_enable",false);config.set("pad_enable",false);config.set("support_object_elevation",0.);
    Model model;auto *object=model.add_object("alias","",make_cube(4.,3.,.15));
    object->add_instance()->set_offset(Vec3d(10.,10.,0.));
    SLAPrint print;print.set_status_callback([](const PrintBase::SlicingStatus &){});
    print.apply(model,config);print.process();REQUIRE(print.finished());
    struct File {boost::filesystem::path path=boost::filesystem::unique_path("svg-alias-%%%%.ZIP");
        ~File(){boost::system::error_code error;boost::filesystem::remove(path,error);}} file;
    REQUIRE_NOTHROW(print.export_print(file.path.string()));
    const auto archive=read_zipper_archive(file.path.string(),{"svg"},{});
    REQUIRE(archive.entries.size()==print.print_layers().size());
    REQUIRE_FALSE(archive.entries.empty());
    for(const auto &entry:archive.entries)
        REQUIRE(std::string(entry.buf.begin(),entry.buf.end()).find("<svg")!=std::string::npos);
}

TEST_CASE("Photon S bit-reversed runs retain binary threshold and all counts", "[sla_reference][sla_photons]") {
    std::vector<uint8_t> pixels;
    for (unsigned count = 1; count <= 128; ++count) {
        pixels.insert(pixels.end(), count, 127);
        pixels.insert(pixels.end(), count, 128);
    }
    REQUIRE(pixels.size() == 128*129);
    const auto mask = ChituboxPhotonSArchive::encode_mask(pixels.data(),128,129,1);
    const auto *bytes = static_cast<const uint8_t *>(mask.data());
    std::vector<uint8_t> decoded;
    REQUIRE(mask.size() == 256);
    for (size_t i = 0; i < mask.size(); ++i) {
        unsigned length = 1;
        for (unsigned bit = 0; bit < 7; ++bit) length += ((bytes[i]>>(7-bit))&1u)<<bit;
        REQUIRE(length == i/2+1);
        REQUIRE((bytes[i]&1u) == i%2);
        decoded.insert(decoded.end(),length,(bytes[i]&1u)?255:0);
    }
    for (auto &pixel : pixels) pixel = pixel > 127 ? 255 : 0;
    REQUIRE(decoded == pixels);
    REQUIRE_THROWS(ChituboxPhotonSArchive::encode_mask(nullptr,128,129,1));
    REQUIRE_THROWS(ChituboxPhotonSArchive::encode_mask(pixels.data(),0,129,1));
    REQUIRE_THROWS(ChituboxPhotonSArchive::encode_mask(pixels.data(),128,129,3));
}

TEST_CASE("Photon S exports masks and rejects parameters its schema cannot store", "[sla_reference][sla_photons]") {
    SLAFullPrintConfig full; Test::configure_archive_fixture(full,"photons");
    full.set("sla_archive_format",std::string("SL1"));
    full.set("sla_bottom_layers",1);
    full.set("sla_wait_before_cure",1.25); full.set("sla_bottom_wait_before_cure",1.25);
    DynamicPrintConfig config; config.apply(full);
    std::vector<uint8_t> pixels(1440*2560,255);
    std::vector<sla::EncodedRaster> masks;
    masks.push_back(sla::PNGRasterEncoder{}(pixels.data(),1440,2560,1));
    std::fill(pixels.begin(),pixels.end(),0);
    masks.push_back(sla::PNGRasterEncoder{}(pixels.data(),1440,2560,1));
    for (size_t i=0;i<pixels.size();++i) pixels[i]=uint8_t(i%256);
    masks.push_back(sla::PNGRasterEncoder{}(pixels.data(),1440,2560,1));
    auto document=sla::LayerDocument::from_rasters(std::move(masks));
    auto job=SLAPrint::create_raster_job(config,document,{.05,.1,.15});
    auto valid_config=config;valid_config.set("sla_archive_format",std::string("photons"));
    auto valid=SLAPrint::create_raster_job(valid_config,document,{.05,.1,.15});
    REQUIRE(valid->validate().empty());

    const char *folder=std::getenv("SLA_PHOTONS_ORACLE_OUTPUT");
    struct Directory {
        boost::filesystem::path path;
        bool keep;
        ~Directory() { if(!keep) {boost::system::error_code error;boost::filesystem::remove_all(path,error);} }
    } dir{folder?boost::filesystem::path(folder):boost::filesystem::unique_path("photons-%%%%"),bool(folder)};
    boost::filesystem::create_directories(dir.path);
    const auto path=dir.path/"grayscale.photons";
    ThumbnailData red;red.set(1,1);red.pixels={255,0,0,255};
    job->export_layer_document(document,"photons",path.string(),{red},"Photon S");
    REQUIRE(boost::filesystem::file_size(path)>98+224*168*2+4);
    std::ifstream stream(path.string(),std::ios::binary);
    const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(stream),{}};
    auto u32=[&](size_t at){REQUIRE(at+4<=bytes.size());return uint32_t(bytes[at])<<24|uint32_t(bytes[at+1])<<16|uint32_t(bytes[at+2])<<8|bytes[at+3];};
    REQUIRE(u32(0)==2);REQUIRE(bytes[4]==0);REQUIRE(bytes[5]==49);
    const uint64_t pitch_bits=(uint64_t(u32(6))<<32)|u32(10);
    double pitch=0.;std::memcpy(&pitch,&pitch_bits,sizeof(pitch));
    REQUIRE(pitch==Catch::Approx(68.04/1440).epsilon(1e-12));

    REQUIRE(u32(82)==224);REQUIRE(u32(90)==168);
    REQUIRE(u32(98+224*168*2)==3);
    // Actual caller-supplied preview: red is BGR565 little endian, letterboxed.
    const size_t center=98+2*(84*224+112);
    REQUIRE(bytes[center]==31);REQUIRE(bytes[center+1]==0);
    REQUIRE(bytes[98]==0);REQUIRE(bytes[99]==0);
    size_t offset=98+224*168*2+4;
    for(size_t layer=0;layer<3;++layer) {
        REQUIRE(u32(offset)==44944);REQUIRE(u32(offset+12)==1440);REQUIRE(u32(offset+16)==2560);
        const size_t count=(u32(offset+20)>>3)-4;
        REQUIRE(u32(offset+24)==2684702720u);offset+=28;
        REQUIRE(offset+count<=bytes.size());
        auto expected=document.image(layer);
        size_t pixel=0;
        for(size_t index=0;index<count;++index) {
            const auto token=bytes[offset+index];unsigned run=1;
            for(unsigned bit=0;bit<7;++bit)run+=((token>>(7-bit))&1u)<<bit;
            REQUIRE(pixel+run<=expected.buf.size());
            for(size_t n=0;n<run;++n) {
                const uint8_t value=expected.buf[pixel+n]>127?255:0;
                if(value!=((token&1)?255:0)) FAIL("Photon S mask differs from source threshold");
            }
            pixel+=run;
        }
        REQUIRE(pixel==expected.buf.size());offset+=count;
    }
    REQUIRE(offset==bytes.size());
    if(folder) job->export_layer_document(document,"SL1",(dir.path/"grayscale.sl1").string(),{},"Photon S source");
    for(const char *key:{"sla_transition_layers","sla_bottom_lift_speed","sla_bottom_wait_before_cure","sla_light_pwm","sla_lift_height2","display_width","initial_layer_height","sla_retract_speed"}) {
        CAPTURE(key);auto invalid_config=config;
        if(std::string(key)=="sla_transition_layers"||std::string(key)=="sla_light_pwm")invalid_config.set(key,1);
        else invalid_config.set(key,std::string(key)=="sla_retract_speed"?80.:1.);
        auto invalid=SLAPrint::create_raster_job(invalid_config,document,std::string(key)=="initial_layer_height"?std::vector<double>{1.,1.05,1.1}:std::vector<double>{.05,.1,.15});
        auto selected_config=invalid_config;selected_config.set("sla_archive_format",std::string("photons"));
        REQUIRE_THROWS(SLAPrint::create_raster_job(selected_config,document,std::string(key)=="initial_layer_height"?std::vector<double>{1.,1.05,1.1}:std::vector<double>{.05,.1,.15}));
        REQUIRE_THROWS(invalid->export_layer_document(document,"photons",path.string(),{},"invalid"));
        std::ifstream unchanged(path.string(),std::ios::binary);
        REQUIRE(std::vector<uint8_t>{std::istreambuf_iterator<char>(unchanged),{}}==bytes);
    }
    auto variable=SLAPrint::create_raster_job(config,document,{.05,.15,.2});
    REQUIRE_THROWS(variable->export_layer_document(document,"photons",path.string(),{},"variable Z"));
}

TEST_CASE("PHZ seed parsing and layer cipher match original UVtools", "[sla_reference][sla_phz]") {
    REQUIRE(PhrozenPHZArchive::parse_encryption_key("0")==0);
    REQUIRE(PhrozenPHZArchive::parse_encryption_key(" 0xFFFFFFFF ")==UINT32_MAX);
    REQUIRE(PhrozenPHZArchive::parse_encryption_key("4294967295")==UINT32_MAX);
    REQUIRE(PhrozenPHZArchive::parse_encryption_key("0X4324")==0x4324);
    for(const char *invalid:{"","-1","+1","4294967296","0x100000000","0x","12x","1 2"})
        REQUIRE_THROWS(PhrozenPHZArchive::parse_encryption_key(invalid));
    std::vector<uint8_t> bytes(32);std::iota(bytes.begin(),bytes.end(),0);
    const auto original=bytes;
    PhrozenPHZArchive::crypt_mask(bytes,UINT32_MAX,65535);
    const std::vector<uint8_t> expected={0x23,0xcc,0xac,0x1b,0xaa,0x9e,0x9d,0x9b,0x31,0x63,0x82,0x2b,0xc8,0x35,0x7b,0xab,0x5f,0x16,0x70,0x3b,0xce,0xc0,0x58,0xbb,0x7d,0xbd,0x21,0x2b,0xec,0x6f,0x36,0xab};
    REQUIRE(bytes==expected); // Produced by unmodified PHZFile.LayerRleCryptBuffer.
    PhrozenPHZArchive::crypt_mask(bytes,UINT32_MAX,65535);REQUIRE(bytes==original);
    PhrozenPHZArchive::crypt_mask(bytes,0,123);REQUIRE(bytes==original);
    PhrozenPHZArchive::crypt_mask(bytes,0x4324,123);REQUIRE(bytes==original);
    SLAFullPrintConfig full;full.set("sla_phz_encryption_key",std::string("0xFFFFFFFF"));
    SLAPrinterConfig printer;printer.apply(full,true);
    REQUIRE(printer.sla_phz_encryption_key.value=="0xFFFFFFFF");
}

TEST_CASE("PHZ masks preserve row and half-row runs with grayscale quantization", "[sla_reference][sla_phz]") {
    for(const size_t w:{size_t(1),size_t(251),size_t(256),size_t(1024)}) {
        const size_t h=256;std::vector<uint8_t> pixels(w*h);
        for(size_t y=0;y<h;++y)std::fill(pixels.begin()+y*w,pixels.begin()+(y+1)*w,uint8_t(y));
        auto encoded=PhrozenPHZArchive::encode_mask(pixels.data(),w,h,1);
        const auto *data=static_cast<const uint8_t *>(encoded.data());
        std::vector<uint8_t> decoded;uint8_t color=0;
        size_t literals=0;
        for(size_t i=0;i<encoded.size();++i) {
            const auto code=data[i];
            if(code&128){++literals;const auto grey=code&127;REQUIRE(grey<=124);color=uint8_t((grey<<1)|(grey&1));decoded.push_back(color);}
            else {REQUIRE(code>=1);REQUIRE(code<=125);decoded.insert(decoded.end(),code,color);}
        }
        REQUIRE(literals==h*(w==1?1:2));
        for(auto &pixel:pixels){const auto grey=std::min<unsigned>(pixel>>1,124);pixel=uint8_t((grey<<1)|(grey&1));}
        REQUIRE(decoded==pixels);
    }
    uint8_t p=255;
    REQUIRE_THROWS(PhrozenPHZArchive::encode_mask(nullptr,1,1,1));
    REQUIRE_THROWS(PhrozenPHZArchive::encode_mask(&p,1,1,3));
}

TEST_CASE("PHZ exports grayscale Z exposure motion previews and optional encryption", "[sla_reference][sla_phz]") {
    const bool encrypted=GENERATE(false,true);
    SLAFullPrintConfig full;Test::configure_archive_fixture(full,"phz");
    full.set("sla_archive_format",std::string("SL1"));
    full.set("display_pixels_x",1024);full.set("display_pixels_y",256);
    full.set("sla_bottom_layers",1);full.set("sla_transition_layers",1);
    full.set("sla_wait_before_cure",1.25);full.set("sla_bottom_wait_before_cure",2.5);
    full.set("sla_light_pwm",200);full.set("sla_bottom_light_pwm",160);
    full.set("sla_phz_encryption_key",std::string(encrypted?"0xFFFFFFFF":"0"));
    DynamicPrintConfig config;config.apply(full);
    std::vector<uint8_t> pixels(1024*256,255);std::vector<sla::EncodedRaster> masks;
    masks.push_back(sla::PNGRasterEncoder{}(pixels.data(),1024,256,1));
    std::fill(pixels.begin(),pixels.end(),0);masks.push_back(sla::PNGRasterEncoder{}(pixels.data(),1024,256,1));
    for(size_t y=0;y<256;++y)std::fill(pixels.begin()+y*1024,pixels.begin()+(y+1)*1024,uint8_t(y));
    const auto grey=sla::PNGRasterEncoder{}(pixels.data(),1024,256,1);masks.push_back(grey);masks.push_back(grey);
    auto document=sla::LayerDocument::from_rasters(std::move(masks));
    auto job=SLAPrint::create_raster_job(config,document,{.05,.1,.15,.2});
    const char *folder=std::getenv("SLA_PHZ_ORACLE_OUTPUT");
    struct Directory {boost::filesystem::path path;bool keep;~Directory(){if(!keep){boost::system::error_code error;boost::filesystem::remove_all(path,error);}}}
        dir{folder?boost::filesystem::path(folder):boost::filesystem::unique_path("phz-%%%%"),bool(folder)};
    boost::filesystem::create_directories(dir.path);
    const auto path=dir.path/(encrypted?"encrypted.phz":"grayscale.phz");
    ThumbnailData red;red.set(1,1);red.pixels={255,0,0,255};
    job->export_layer_document(document,"phz",path.string(),{red},"PHZ");
    std::ifstream in(path.string(),std::ios::binary);const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(in),{}};
    auto u32=[&](size_t at){REQUIRE(at+4<=bytes.size());return uint32_t(bytes[at])|uint32_t(bytes[at+1])<<8|uint32_t(bytes[at+2])<<16|uint32_t(bytes[at+3])<<24;};
    REQUIRE(u32(0)==0x9FDA83AE);REQUIRE(u32(4)==2);REQUIRE(u32(40)==4);
    REQUIRE(u32(88)==(encrypted?UINT32_MAX:0));REQUIRE(u32(176)==28);
    REQUIRE(bytes[60]==200);REQUIRE(bytes[62]==160);
    for(const auto at:{u32(32),u32(44)}) {
        REQUIRE((u32(at)==400||u32(at)==200));REQUIRE(u32(at+8)==at+32);REQUIRE(u32(at+12)>0);
        REQUIRE(u32(at+8)+u32(at+12)<=bytes.size());
    }
    const auto table=u32(36);
    REQUIRE((u32(table+2*36+12)==u32(table+3*36+12))==!encrypted);
    for(size_t layer=0;layer<4;++layer) {
        const auto at=table+layer*36;const uint64_t address=(uint64_t(u32(at+20))<<32)|u32(at+12);
        const auto size=u32(at+16);REQUIRE(address+size<=bytes.size());
        std::vector<uint8_t> decoded(bytes.begin()+address,bytes.begin()+address+size);
        PhrozenPHZArchive::crypt_mask(decoded,u32(88),uint32_t(layer));
        const auto image=document.image(layer);
        const auto expected=PhrozenPHZArchive::encode_mask(image.buf.data(),image.cols,image.rows,1);
        REQUIRE(decoded.size()==expected.size());REQUIRE(!std::memcmp(decoded.data(),expected.data(),decoded.size()));
    }
    if(folder) {
        job->export_layer_document(document,"SL1",(dir.path/"grayscale.sl1").string(),{},"PHZ source");
        auto variable=SLAPrint::create_raster_job(config,document,{.05,.15,.2,.3});
        variable->export_layer_document(document,"phz",(dir.path/(encrypted?"variable-encrypted.phz":"variable.phz")).string(),{},"variable PHZ");
    }
    auto invalid=config;invalid.set("sla_phz_encryption_key",std::string("4294967296"));
    auto invalid_source=SLAPrint::create_raster_job(invalid,document,{.05,.1,.15,.2});
    REQUIRE_THROWS(invalid_source->export_layer_document(document,"phz",path.string(),{},"invalid"));
    invalid.set("sla_archive_format",std::string("phz"));
    REQUIRE_THROWS(SLAPrint::create_raster_job(invalid,document,{.05,.1,.15,.2}));
    std::ifstream unchanged(path.string(),std::ios::binary);REQUIRE(std::vector<uint8_t>{std::istreambuf_iterator<char>(unchanged),{}}==bytes);
}

TEST_CASE("LGS nibble RLE preserves all gray levels long runs and 4K rotation", "[sla_reference][sla_lgs]") {
    for(const bool rotate:{false,true}) {
        for(const auto dimensions:std::vector<std::pair<size_t,size_t>>{{1,1},{1,16},{1,256},{256,256},{128,256}}) {
            const auto [width,height]=dimensions;
            for(const bool constant:{false,true}) {
                std::vector<uint8_t> source(width*height,255);
                if(!constant) for(size_t i=0;i<source.size();++i) source[i]=uint8_t(i);
                const auto encoded=LongerOrangeArchive::encode_mask(source.data(),width,height,1,rotate);
                const auto *data=static_cast<const uint8_t *>(encoded.data());
                std::vector<uint8_t> decoded;
                for(size_t i=0;i<encoded.size();) {
                    const uint8_t color=data[i]>>4;size_t run=0;
                    REQUIRE((data[i]&15)!=0); // Most significant run digit.
                    do {run=(run<<4)|(data[i++]&15);} while(i<encoded.size() && data[i]>>4==color);
                    REQUIRE(run>0);REQUIRE(decoded.size()+run<=source.size());
                    decoded.insert(decoded.end(),run,uint8_t(color*17));
                }
                REQUIRE(decoded.size()==source.size());
                if(rotate) {
                    auto wire=decoded;
                    for(size_t y=0;y<height;++y)for(size_t x=0;x<width;++x)
                        decoded[y*width+x]=wire[x*height+(height-1-y)];
                }
                for(auto &pixel:source) pixel=uint8_t((pixel>>4)*17);
                REQUIRE(decoded==source);
            }
        }
    }
    REQUIRE_THROWS(LongerOrangeArchive::encode_mask(nullptr,1,1,1,false));
    uint8_t pixel=255;REQUIRE_THROWS(LongerOrangeArchive::encode_mask(&pixel,1,1,3,false));
}

TEST_CASE("LGS model exports preserve previews masks exposure and supported motion", "[sla_reference][sla_lgs]") {
    const std::string format=GENERATE(std::string("lgs"),std::string("lgs30"),std::string("lgs120"),std::string("lgs4k"),std::string("lgs4k-color"));
    const std::string id=format=="lgs4k-color"?"lgs4k":format;
    SLAFullPrintConfig full;Test::configure_archive_fixture(full,id);
    full.set("sla_archive_format",std::string("SL1"));
    full.set("display_pixels_x",128);full.set("display_pixels_y",256);
    full.set("display_width",64.);full.set("display_height",128.);
    full.set("sla_bottom_layers",1);full.set("sla_wait_before_cure",1.25);full.set("sla_bottom_wait_before_cure",2.5);
    full.set("sla_bottom_lift_height",4.);full.set("sla_bottom_lift_speed",60.);
    if(format=="lgs4k-color") full.set("printer_model",std::string("4000"));
    DynamicPrintConfig config;config.apply(full);
    std::vector<uint8_t> pixels(128*256,255);std::vector<sla::EncodedRaster> masks;
    masks.push_back(sla::PNGRasterEncoder{}(pixels.data(),128,256,1));
    std::fill(pixels.begin(),pixels.end(),0);masks.push_back(sla::PNGRasterEncoder{}(pixels.data(),128,256,1));
    for(size_t i=0;i<pixels.size();++i)pixels[i]=uint8_t(i);
    masks.push_back(sla::PNGRasterEncoder{}(pixels.data(),128,256,1));
    auto document=sla::LayerDocument::from_rasters(std::move(masks));
    auto job=SLAPrint::create_raster_job(config,document,{.05,.1,.15});
    const char *folder=std::getenv("SLA_LGS_ORACLE_OUTPUT");
    struct Directory {boost::filesystem::path path;bool keep;~Directory(){if(!keep){boost::system::error_code error;boost::filesystem::remove_all(path,error);}}}
        dir{folder?boost::filesystem::path(folder):boost::filesystem::unique_path("lgs-%%%%"),bool(folder)};
    boost::filesystem::create_directories(dir.path);
    const auto path=dir.path/((format=="lgs4k-color"?"color":"grayscale")+std::string(".")+id);
    ThumbnailData red;red.set(120,150);
    for(size_t i=0;i<red.pixels.size();i+=4){red.pixels[i]=255;red.pixels[i+1]=0;red.pixels[i+2]=0;red.pixels[i+3]=255;}
    job->export_layer_document(document,id,path.string(),{red},"LGS");
    std::ifstream in(path.string(),std::ios::binary);const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(in),{}};
    auto u32=[&](size_t at){REQUIRE(at+4<=bytes.size());return uint32_t(bytes[at])|uint32_t(bytes[at+1])<<8|uint32_t(bytes[at+2])<<16|uint32_t(bytes[at+3])<<24;};
    REQUIRE(std::string(bytes.begin(),bytes.begin()+8)==std::string("Longer3D\0",8));
    const uint32_t model=LongerOrangeArchive::model_for_format(id,full.printer_model.value);
    REQUIRE(u32(16)==model);REQUIRE(u32(164)==3);REQUIRE(u32(172)==120);REQUIRE(u32(176)==150);
    REQUIRE(bytes[180]==0xf8);REQUIRE(bytes[181]==0); // Big-endian RGB565 red, not BGR565.
    size_t offset=180+120*150*2;
    if(id=="lgs120") {
        const auto size=u32(offset);REQUIRE(size>8);
        REQUIRE(bytes[offset+4]==137);REQUIRE(bytes[offset+5]=='P');
        REQUIRE(bytes[offset+4+size]==0);REQUIRE(bytes[offset+5+size]==0);offset+=size+6;
    }
    for(size_t layer=0;layer<3;++layer) {
        const size_t size=u32(offset);offset+=4;REQUIRE(offset+size<=bytes.size());
        const auto image=document.image(layer);
        const auto expected=LongerOrangeArchive::encode_mask(image.buf.data(),128,256,1,id=="lgs4k");
        REQUIRE(expected.size()==size);REQUIRE(!std::memcmp(expected.data(),bytes.data()+offset,size));offset+=size;
    }
    REQUIRE(offset==bytes.size());
    if(folder) job->export_layer_document(document,"SL1",(dir.path/"grayscale.sl1").string(),{},"LGS source");
    for(const char *key:{"sla_transition_layers","sla_light_pwm","sla_retract_speed","display_pixels_x","display_mirror_x","max_print_height","initial_layer_height"}) {
        CAPTURE(format,key);auto invalid_config=config;
        const std::string option=key;
        if(option=="sla_transition_layers" || option=="sla_light_pwm") invalid_config.set(key,1);
        else if(option=="display_pixels_x") invalid_config.set(key,512);
        else if(option=="display_mirror_x") invalid_config.set(key,false);
        else invalid_config.set(key,option=="sla_retract_speed"?150.:1.);
        auto selected=invalid_config;selected.set("sla_archive_format",id);
        REQUIRE_THROWS(SLAPrint::create_raster_job(selected,document,{.05,.1,.15}));
        if(option=="display_pixels_x") {
            // Dimension mismatch is rejected before a raster job exists.
            REQUIRE_THROWS(SLAPrint::create_raster_job(invalid_config,document,{.05,.1,.15}));
        } else {
            auto invalid=SLAPrint::create_raster_job(invalid_config,document,option=="initial_layer_height"?std::vector<double>{1.,1.05,1.1}:std::vector<double>{.05,.1,.15});
            REQUIRE_THROWS(invalid->export_layer_document(document,id,path.string(),{},"invalid"));
        }
        std::ifstream unchanged(path.string(),std::ios::binary);
        REQUIRE(std::vector<uint8_t>{std::istreambuf_iterator<char>(unchanged),{}}==bytes);
    }
    auto variable=SLAPrint::create_raster_job(config,document,{.05,.15,.2});
    REQUIRE_THROWS(variable->export_layer_document(document,id,path.string(),{},"variable"));
}

TEST_CASE("Makerbase vertical spans reproduce thresholded masks and column order", "[sla_reference][sla_makerbase]") {
    for(const auto dims:std::vector<std::pair<size_t,size_t>>{{1,1},{65,256},{128,256},{256,256}}) {
        const auto [width,height]=dims;
        for(const int pattern:{0,1,2}) {
            std::vector<uint8_t> pixels(width*height,uint8_t(pattern==1?255:0));
            if(pattern==2)for(size_t i=0;i<pixels.size();++i)pixels[i]=uint8_t(i);
            const auto encoded=MakerbaseArchive::encode_mask(pixels.data(),width,height,1);
            const auto *data=static_cast<const uint8_t *>(encoded.data());
            const uint32_t count=uint32_t(data[0])<<24|uint32_t(data[1])<<16|uint32_t(data[2])<<8|data[3];
            REQUIRE(encoded.size()==uint64_t(count)*6+6);REQUIRE(data[encoded.size()-2]==13);REQUIRE(data[encoded.size()-1]==10);
            auto u16=[&](size_t at){return uint16_t(uint16_t(data[at])<<8|data[at+1]);};
            std::vector<uint8_t> decoded(pixels.size(),0);
            size_t previous_x=0;int previous_end=-1;
            for(size_t at=4;at<encoded.size()-2;at+=6) {
                const size_t start=u16(at),end=u16(at+2),x=u16(at+4);
                REQUIRE(start<=end);REQUIRE(end<height);REQUIRE(x<width);REQUIRE(x>=previous_x);
                if(x==previous_x)REQUIRE(int(start)>previous_end);
                previous_x=x;previous_end=int(end);
                for(size_t y=start;y<=end;++y)decoded[y*width+x]=255;
            }
            for(auto &pixel:pixels)pixel=pixel>=128?255:0;
            REQUIRE(decoded==pixels);
        }
    }
    REQUIRE_THROWS(MakerbaseArchive::encode_mask(nullptr,1,1,1));
    uint8_t p=255;REQUIRE_THROWS(MakerbaseArchive::encode_mask(&p,1,1,3));
}

TEST_CASE("Makerbase GR1 and MDLP export their own settings and exact binary spans", "[sla_reference][sla_makerbase]") {
    const bool mdlp=GENERATE(false,true);
    const bool portrait=GENERATE(false,true);
    const std::string id=mdlp?"mdlp":"gr1";
    SLAFullPrintConfig full;Test::configure_archive_fixture(full,id);
    full.set("sla_archive_format",std::string("SL1"));full.set("sla_bottom_layers",1);
    full.display_orientation.value=portrait?sladoPortrait:sladoLandscape;
    full.set("display_pixels_x",portrait?256:128);full.set("display_pixels_y",portrait?128:256);
    full.set("display_width",portrait?128.:64.);full.set("display_height",portrait?64.:128.);
    if(!mdlp){full.set("sla_light_pwm",200);full.set("sla_bottom_light_pwm",160);}
    DynamicPrintConfig config;config.apply(full);
    std::vector<uint8_t> pixels(128*256,255);std::vector<sla::EncodedRaster> masks;
    masks.push_back(sla::PNGRasterEncoder{}(pixels.data(),128,256,1));
    std::fill(pixels.begin(),pixels.end(),0);masks.push_back(sla::PNGRasterEncoder{}(pixels.data(),128,256,1));
    for(size_t i=0;i<pixels.size();++i)pixels[i]=uint8_t(i);
    masks.push_back(sla::PNGRasterEncoder{}(pixels.data(),128,256,1));
    auto document=sla::LayerDocument::from_rasters(std::move(masks));
    auto job=SLAPrint::create_raster_job(config,document,{.05,.1,.15});
    const char *folder=std::getenv("SLA_MAKERBASE_ORACLE_OUTPUT");
    struct Directory {boost::filesystem::path path;bool keep;~Directory(){if(!keep){boost::system::error_code error;boost::filesystem::remove_all(path,error);}}}
        dir{folder?boost::filesystem::path(folder):boost::filesystem::unique_path("makerbase-%%%%"),bool(folder)};
    boost::filesystem::create_directories(dir.path);
    const auto path=dir.path/(std::string(portrait?"portrait":"landscape")+"."+id);
    ThumbnailData red;red.set(1,1);red.pixels={255,0,0,255};
    job->export_layer_document(document,id,path.string(),{red},"Makerbase");
    std::ifstream input(path.string(),std::ios::binary);const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(input),{}};
    const std::vector<uint8_t> magic{0,0,0,7,'M','K','S','D','L','P',0};
    REQUIRE(std::equal(magic.begin(),magic.end(),bytes.begin()));REQUIRE(std::equal(magic.begin(),magic.end(),bytes.end()-11));
    REQUIRE(bytes[11]==0xf8);REQUIRE(bytes[12]==0);
    auto u16=[&](size_t at){REQUIRE(at+2<=bytes.size());return uint16_t(uint16_t(bytes[at])<<8|bytes[at+1]);};
    auto u32=[&](size_t at){REQUIRE(at+4<=bytes.size());return uint32_t(bytes[at])<<24|uint32_t(bytes[at+1])<<16|uint32_t(bytes[at+2])<<8|bytes[at+3];};
    size_t at=11+116*116*2;REQUIRE(bytes[at]==13);REQUIRE(bytes[at+1]==10);
    at+=2+290*290*2;REQUIRE(bytes[at]==13);REQUIRE(bytes[at+1]==10);at+=2;
    REQUIRE(u16(at)==3);REQUIRE(u16(at+2)==128);REQUIRE(u16(at+4)==256);at+=6;
    for(const std::string wanted:{"64","128","0.05"}) {
        const auto size=u32(at);at+=4;REQUIRE(size==wanted.size()*2);
        for(const char c:wanted){REQUIRE(bytes[at++]==0);REQUIRE(bytes[at++]==uint8_t(c));}
    }
    const std::vector<uint16_t> common{3,8,20,1};
    for(const auto value:common){REQUIRE(u16(at)==value);at+=2;}
    if(!mdlp)for(const auto value:std::vector<uint16_t>{4,60,6,120,120,160,200}){REQUIRE(u16(at)==value);at+=2;}
    for(size_t layer=0;layer<3;++layer) {
        const auto image=document.image(layer);
        const auto expected=MakerbaseArchive::encode_mask(image.buf.data(),128,256,1);
        REQUIRE(at+expected.size()<=bytes.size());REQUIRE(!std::memcmp(bytes.data()+at,expected.data(),expected.size()));at+=expected.size();
    }
    REQUIRE(at+11==bytes.size());
    if(folder)job->export_layer_document(document,"SL1",(dir.path/(id+"-source.sl1")).string(),{},"Makerbase source");
    for(const char *key:{"exposure_time","sla_transition_layers","sla_lift_height","sla_wait_before_cure","display_mirror_x","initial_layer_height"}) {
        CAPTURE(id,portrait,key);auto invalid_config=config;const std::string option=key;
        if(option=="sla_transition_layers")invalid_config.set(key,1);
        else if(option=="display_mirror_x")invalid_config.set(key,true);
        else invalid_config.set(key,option=="exposure_time"?2.5:option=="sla_lift_height"?7.:1.);
        auto selected=invalid_config;selected.set("sla_archive_format",id);
        REQUIRE_THROWS(SLAPrint::create_raster_job(selected,document,{.05,.1,.15}));
        auto invalid=SLAPrint::create_raster_job(invalid_config,document,option=="initial_layer_height"?std::vector<double>{1.,1.05,1.1}:std::vector<double>{.05,.1,.15});
        REQUIRE_THROWS(invalid->export_layer_document(document,id,path.string(),{},"invalid"));
        std::ifstream unchanged(path.string(),std::ios::binary);
        REQUIRE(std::vector<uint8_t>{std::istreambuf_iterator<char>(unchanged),{}}==bytes);
    }
    auto variable=SLAPrint::create_raster_job(config,document,{.05,.15,.2});
    REQUIRE_THROWS(variable->export_layer_document(document,id,path.string(),{},"variable"));
}

TEST_CASE("QDT exports relative laser paths both parities previews and fixed metadata", "[sla_reference][sla_qdt]") {
    const bool portrait = GENERATE(false, true);
    SLAFullPrintConfig full;
    Test::configure_archive_fixture(full, "qdt");
    full.set("sla_archive_format", std::string("SL1"));
    full.display_orientation.value = portrait ? sladoPortrait : sladoLandscape;
    full.set("display_pixels_x", portrait ? 400 : 800);
    full.set("display_pixels_y", portrait ? 800 : 400);
    full.set("display_width", portrait ? 200. : 400.);
    full.set("display_height", portrait ? 400. : 200.);
    DynamicPrintConfig config;
    config.apply(full);
    std::vector<uint8_t> pixels(800 * 400, 255);
    std::vector<sla::EncodedRaster> masks;
    masks.push_back(sla::PNGRasterEncoder{}(pixels.data(), 800, 400, 1));
    std::fill(pixels.begin(), pixels.end(), 0);
    masks.push_back(sla::PNGRasterEncoder{}(pixels.data(), 800, 400, 1));
    for (size_t i = 0; i < pixels.size(); ++i)
        pixels[i] = uint8_t(i);
    const auto gradient = sla::PNGRasterEncoder{}(pixels.data(), 800, 400, 1);
    masks.push_back(gradient);
    std::fill(pixels.begin(), pixels.end(), 0);
    pixels[0] = 255;
    pixels.back() = 255;
    for (size_t y = 7; y <= 200; ++y)
        pixels[y * 800 + 3] = 255;
    pixels[75 * 800 + 3] = 0;
    pixels[799] = 255;
    masks.push_back(sla::PNGRasterEncoder{}(pixels.data(), 800, 400, 1));
    masks.push_back(gradient);
    masks.push_back(gradient);
    auto document = sla::LayerDocument::from_rasters(std::move(masks));
    const std::vector<double> positions{.05, .1, .15, .2, .25, .3};
    auto job = SLAPrint::create_raster_job(config, document, positions);
    const char *folder = std::getenv("SLA_QDT_ORACLE_OUTPUT");
    struct Directory {
        boost::filesystem::path path;
        bool keep;
        ~Directory() {
            if (!keep) {
                boost::system::error_code error;
                boost::filesystem::remove_all(path, error);
            }
        }
    } dir{folder ? boost::filesystem::path(folder) : boost::filesystem::unique_path("qdt-%%%%"), bool(folder)};
    boost::filesystem::create_directories(dir.path);
    const auto path = dir.path / (portrait ? "portrait.qdt" : "landscape.qdt");
    job->export_layer_document(document, "qdt", path.string(), {}, "QDT");
    std::ifstream input(path.string());
    const std::string text{std::istreambuf_iterator<char>(input), {}};
    std::istringstream stream(text);
    std::string line;
    REQUIRE(bool(std::getline(stream, line)));
    REQUIRE(line == "JieHe,50,400,800,2,019,0,FA");
    using Coordinates = std::array<unsigned, 3>;
    auto coordinate = [&](const std::string &text) {
        std::istringstream row(text);
        Coordinates c{};
        char comma = 0;
        REQUIRE(bool(row >> c[0] >> comma));
        REQUIRE(comma == ',');
        REQUIRE(bool(row >> c[1] >> comma));
        REQUIRE(comma == ',');
        REQUIRE(bool(row >> c[2]));
        REQUIRE(c[2] <= 1);
        return c;
    };
    auto decode = [&](const std::vector<Coordinates> &coords, size_t width, size_t height, bool mirror) {
        std::vector<uint8_t> image(width * height, 0);
        if (coords.empty())
            return image;
        REQUIRE(coords[0][2] == 0);
        unsigned zeros = 1, start_y = coords[0][1], end_y = start_y;
        for (size_t i = 1; i < coords.size(); ++i) {
            if (coords[i][2] == 0) {
                ++zeros;
                continue;
            }
            REQUIRE(zeros >= 1);
            REQUIRE(zeros <= 2);
            if (zeros == 1 && i > 1)
                start_y = end_y + coords[i - 1][1];
            else if (zeros == 2)
                start_y = coords[i - 1][1];
            end_y = start_y + coords[i][1];
            REQUIRE(end_y < height);
            const unsigned x = mirror ? unsigned(width + 1) - coords[i][0] : coords[i][0];
            REQUIRE(x < width);
            for (size_t y = start_y; y <= end_y; ++y)
                image[y * width + x] = 255;
            zeros = 0;
        }
        return image;
    };
    for (size_t layer = 0; layer < 6; ++layer) {
        REQUIRE(bool(std::getline(stream, line)));
        REQUIRE(line == std::to_string(layer + 1));
        std::vector<Coordinates> preview, main;
        while (std::getline(stream, line) && line != "FB")
            preview.push_back(coordinate(line));
        REQUIRE(line == "FB");
        while (std::getline(stream, line) && line != "FC")
            main.push_back(coordinate(line));
        REQUIRE(line == "FC");
        const auto actual = decode(main, 800, 400, layer % 2 != 0);
        auto image = document.image(layer);
        std::vector<uint8_t> expected = image.buf;
        for (auto &p : expected)
            p = p >= 128 ? 255 : 0;
        REQUIRE(actual == expected);
        std::vector<uint8_t> small(80 * 40);
        for (size_t y = 0; y < 40; ++y)
            for (size_t x = 0; x < 80; ++x) {
                const auto &p = image.buf;
                const size_t at = (y * 10 + 4) * 800 + x * 10 + 4;
                const unsigned sum = p[at] + p[at + 1] + p[at + 800] + p[at + 801];
                small[y * 80 + x] = (sum + 2) / 4 >= 128 ? 255 : 0;
            }
        REQUIRE(decode(preview, 80, 40, false) == small);
    }
    REQUIRE(bool(std::getline(stream, line)));
    REQUIRE(line == "FD");
    REQUIRE(bool(std::getline(stream, line)));
    REQUIRE(line == "6");
    REQUIRE_FALSE(bool(std::getline(stream, line)));
    if (folder)
        job->export_layer_document(document, "SL1", (dir.path / "source.sl1").string(), {}, "QDT source");
    for (const char *key : {"exposure_time", "sla_bottom_layers", "sla_wait_before_cure", "sla_lift_speed",
                            "sla_light_pwm", "display_width", "max_print_height", "initial_layer_height"}) {
        CAPTURE(key);
        auto invalid_config = config;
        const std::string option = key;
        if (option == "sla_bottom_layers" || option == "sla_light_pwm")
            invalid_config.set(key, 1);
        else
            invalid_config.set(key, option == "exposure_time" ? 2.5 : option == "sla_lift_speed" ? 120. : 1.);
        auto selected = invalid_config;
        selected.set("sla_archive_format", std::string("qdt"));
        REQUIRE_THROWS(SLAPrint::create_raster_job(selected, document, positions));
        auto invalid = SLAPrint::create_raster_job(
            invalid_config, document,
            option == "initial_layer_height" ? std::vector<double>{1., 1.05, 1.1, 1.15, 1.2, 1.25} : positions);
        REQUIRE_THROWS(invalid->export_layer_document(document, "qdt", path.string(), {}, "invalid"));
        std::ifstream unchanged(path.string());
        REQUIRE(std::string{std::istreambuf_iterator<char>(unchanged), {}} == text);
    }
    auto variable = SLAPrint::create_raster_job(config, document, {.05, .15, .2, .25, .3, .35});
    REQUIRE_THROWS(variable->export_layer_document(document, "qdt", path.string(), {}, "variable"));
}

TEST_CASE("VDT preserves grayscale variable Z and full two-stage layer settings", "[sla_reference][sla_vdt]") {
    const bool portrait = GENERATE(false, true);
    SLAFullPrintConfig full;
    Test::configure_archive_fixture(full, "vdt");
    full.set("sla_archive_format", std::string("SL1"));
    full.set("display_width", portrait ? 128. : 64.);
    full.set("display_height", portrait ? 64. : 128.);
    full.set("display_pixels_x", portrait ? 256 : 128);
    full.set("display_pixels_y", portrait ? 128 : 256);
    full.display_orientation.value = portrait ? sladoPortrait : sladoLandscape;
    full.set("display_mirror_x", true);
    full.set("display_mirror_y", true);
    full.set("sla_bottom_layers", 1);
    full.set("sla_transition_layers", 1);
    full.set("exposure_time", 3.);
    full.set("initial_exposure_time", 20.);
    for (const std::string prefix : {"sla_", "sla_bottom_"}) {
        const bool bottom = prefix == "sla_bottom_";
        full.set(prefix + "lift_height", bottom ? 4. : 6.);
        full.set(prefix + "lift_height2", 2.);
        full.set(prefix + "lift_speed", bottom ? 60. : 120.);
        full.set(prefix + "lift_speed2", 180.);
        full.set(prefix + "retract_height2", 1.);
        full.set(prefix + "retract_speed", bottom ? 80. : 100.);
        full.set(prefix + "retract_speed2", 140.);
        full.set(prefix + "wait_before_cure", bottom ? 2.5 : 1.25);
        full.set(prefix + "wait_after_cure", .5);
        full.set(prefix + "wait_after_lift", .75);
        full.set(prefix + "light_pwm", bottom ? 160 : 200);
    }
    DynamicPrintConfig config;
    config.apply(full);
    std::vector<uint8_t> pixels(128 * 256, 255);
    std::vector<sla::EncodedRaster> masks;
    masks.push_back(sla::PNGRasterEncoder{}(pixels.data(), 128, 256, 1));
    std::fill(pixels.begin(), pixels.end(), 0);
    masks.push_back(sla::PNGRasterEncoder{}(pixels.data(), 128, 256, 1));
    for (size_t i = 0; i < pixels.size(); ++i)
        pixels[i] = uint8_t(i);
    masks.push_back(sla::PNGRasterEncoder{}(pixels.data(), 128, 256, 1));
    auto document = sla::LayerDocument::from_rasters(std::move(masks));
    auto job = SLAPrint::create_raster_job(config, document, {.05, .13, .2});
    const char *folder = std::getenv("SLA_VDT_ORACLE_OUTPUT");
    struct Directory {
        boost::filesystem::path path;
        bool keep;
        ~Directory() {
            if (!keep) {
                boost::system::error_code error;
                boost::filesystem::remove_all(path, error);
            }
        }
    } dir{folder ? boost::filesystem::path(folder) : boost::filesystem::unique_path("vdt-%%%%"), bool(folder)};
    boost::filesystem::create_directories(dir.path);
    const auto path = dir.path / (portrait ? "portrait.vdt" : "landscape.vdt");
    ThumbnailData preview;
    preview.set(2, 1);
    preview.pixels = {255, 0, 0, 255, 0, 0, 255, 255};
    job->export_layer_document(document, "vdt", path.string(), {preview}, "VDT test \"żywica\"");
    auto uniform = SLAPrint::create_raster_job(config, document, {.05, .1, .15});
    uniform->export_layer_document(document, "vdt",
                                   (dir.path / (portrait ? "uniform-portrait.vdt" : "uniform-landscape.vdt")).string(),
                                   {}, "VDT uniform");
    uniform->export_layer_document(document, "SL1", (dir.path / "source.sl1").string(), {}, "VDT source");
    const auto zip = read_zipper_archive(path.string(), {""}, {});
    auto entry = [&](const std::string &name) -> const std::vector<uint8_t> & {
        auto found =
            std::find_if(zip.entries.begin(), zip.entries.end(), [&](const auto &e) { return e.fname == name; });
        REQUIRE(found != zip.entries.end());
        return found->buf;
    };
    const auto manifest = nlohmann::json::parse(entry("manifest.json"));
    REQUIRE(manifest["project_name"] == "VDT test \"żywica\"");
    REQUIRE(manifest["machine"]["lcd_width"] == 64);
    REQUIRE(manifest["machine"]["lcd_height"] == 128);
    REQUIRE(manifest["machine"]["resolution_x"] == 128);
    REQUIRE(manifest["machine"]["resolution_y"] == 256);
    REQUIRE(manifest["machine"]["x_mirror"] == true);
    REQUIRE(manifest["machine"]["y_mirror"] == true);
    REQUIRE(manifest["print"]["bottom_layers"] == 1);
    REQUIRE(manifest["print"]["transition_layer"] == 1);
    REQUIRE(manifest["layers"].size() == 3);
    for (size_t i = 0; i < 3; ++i) {
        const auto &layer = manifest["layers"][i];
        REQUIRE(layer["height"].get<float>() == sla::layer_z(*job, i));
        REQUIRE(layer["exposure_time"].get<float>() == sla::layer_exposure(*job, i));
        REQUIRE(layer["lift_distance"] == (i == 0 ? 4 : 6));
        REQUIRE(layer["lift_distance2"] == 2);
        REQUIRE(layer["retract_distance2"] == 1);
        REQUIRE(layer["retract_speed"] == (i == 0 ? 80 : 100));
        REQUIRE(layer["light_pwm"] == (i == 0 ? 160 : 200));
        REQUIRE(layer["wait_time_before_cure"] == (i == 0 ? 2.5 : 1.25));
        REQUIRE(layer["wait_time_after_cure"] == .5);
        REQUIRE(layer["wait_time_after_lift"] == .75);
        png::ImageGreyscale image;
        const auto &bytes = entry(std::to_string(i) + ".png");
        REQUIRE(png::decode_png(png::ReadBuf{bytes.data(), bytes.size()}, image));
        REQUIRE(image.buf == document.image(i).buf);
    }
    auto statistics_config = config;
    statistics_config.set("bottle_cost", 25.);
    statistics_config.set("bottle_volume", 1000.);
    statistics_config.set("material_density", 1.24);
    statistics_config.set("sla_transition_layers", 2);
    std::vector<uint8_t> white(128*256, 255);
    const auto white_png = sla::PNGRasterEncoder{}(white.data(),128,256,1);
    auto repeated = sla::LayerDocument::from_rasters({white_png,white_png,white_png});
    auto statistics_job = SLAPrint::create_raster_job(statistics_config,repeated,{.05,.13,.2});
    const auto statistics_path = dir.path/(portrait?"statistics-portrait.vdt":"statistics-landscape.vdt");
    statistics_job->export_layer_document(repeated,"vdt",statistics_path.string(),{},"statistics");
    const auto statistics_zip = read_zipper_archive(statistics_path.string(),{""},{});
    const auto statistics_entry = std::find_if(statistics_zip.entries.begin(),statistics_zip.entries.end(),
        [](const auto &e){return e.fname=="manifest.json";});
    REQUIRE(statistics_entry!=statistics_zip.entries.end());
    const auto statistics = nlohmann::json::parse(statistics_entry->buf);
    REQUIRE(statistics["print_statistics"]["volume"].get<double>() == Catch::Approx(1.6384).epsilon(1e-6));
    REQUIRE(statistics["print_statistics"]["weight"].get<double>() == Catch::Approx(2.031616).epsilon(1e-6));
    REQUIRE(statistics["print_statistics"]["price"].get<double>() == Catch::Approx(.04096).epsilon(1e-6));
    REQUIRE(statistics["resin"]["cost"] == statistics["print_statistics"]["price"]);
    REQUIRE(statistics["resin"]["density"].get<double>() == Catch::Approx(1.24).epsilon(1e-6));
    REQUIRE(statistics["layers"][1]["exposure_time"].get<float>() == 14.33f);
    REQUIRE(statistics["layers"][2]["exposure_time"].get<float>() == 8.67f);
    const auto original = entry("manifest.json");
    auto invalid = config;
    invalid.set("sla_bottom_retract_height2", 100.);
    REQUIRE_THROWS(([&] {
        auto bad = SLAPrint::create_raster_job(invalid, document, {.05, .13, .2});
        bad->export_layer_document(document, "vdt", path.string(), {}, "invalid");
    }()));
    const auto after = read_zipper_archive(path.string(), {""}, {});
    const auto kept = std::find_if(after.entries.begin(), after.entries.end(),
                                   [](const auto &e) { return e.fname == "manifest.json"; });
    REQUIRE(kept != after.entries.end());
    REQUIRE(kept->buf == original);
    auto too_precise = SLAPrint::create_raster_job(config, document, {.05, .1305, .2});
    REQUIRE_THROWS(too_precise->export_layer_document(document, "vdt", path.string(), {}, "Z precision"));
    auto fine_wait = config;
    fine_wait.set("sla_wait_before_cure", 1.251);
    auto fine = SLAPrint::create_raster_job(fine_wait, document, {.05, .13, .2});
    REQUIRE_THROWS(fine->export_layer_document(document, "vdt", path.string(), {}, "wait precision"));
}

TEST_CASE("JXS preserves packed RGB masks and actual SLA action sequence", "[sla_reference][sla_jxs]") {
    const bool portrait = GENERATE(false, true);
    SLAFullPrintConfig full;
    Test::configure_archive_fixture(full, "jxs");
    full.set("sla_archive_format", std::string("SL1"));
    full.display_orientation.value = portrait ? sladoPortrait : sladoLandscape;
    full.set("display_pixels_x", portrait ? 2880 : 4920);
    full.set("display_pixels_y", portrait ? 4920 : 2880);
    full.set("display_width", portrait ? 129.6 : 221.4);
    full.set("display_height", portrait ? 221.4 : 129.6);
    full.set("sla_bottom_layers", 1);
    full.set("sla_transition_layers", 1);
    full.set("exposure_time", 3.);
    full.set("initial_exposure_time", 20.);
    for (const std::string prefix : {"sla_", "sla_bottom_"}) {
        const bool bottom = prefix == "sla_bottom_";
        full.set(prefix + "lift_height", bottom ? 4. : 6.);
        full.set(prefix + "lift_height2", 2.);
        full.set(prefix + "lift_speed", bottom ? 60. : 120.);
        full.set(prefix + "lift_speed2", 180.);
        full.set(prefix + "retract_height2", 1.);
        full.set(prefix + "retract_speed", bottom ? 80. : 100.);
        full.set(prefix + "retract_speed2", 140.);
        full.set(prefix + "wait_before_cure", bottom ? 2.5 : 1.25);
        full.set(prefix + "wait_after_cure", .5);
        full.set(prefix + "wait_after_lift", .75);
        full.set(prefix + "light_pwm", bottom ? 160 : 200);
    }
    DynamicPrintConfig config;
    config.apply(full);
    std::vector<uint8_t> pixels(4920 * 2880, 255);
    std::vector<sla::EncodedRaster> masks;
    masks.push_back(sla::PNGRasterEncoder{}(pixels.data(), 4920, 2880, 1));
    std::fill(pixels.begin(), pixels.end(), 0);
    masks.push_back(sla::PNGRasterEncoder{}(pixels.data(), 4920, 2880, 1));
    for (size_t i = 0; i < pixels.size(); ++i)
        pixels[i] = uint8_t(i);
    masks.push_back(sla::PNGRasterEncoder{}(pixels.data(), 4920, 2880, 1));
    auto document = sla::LayerDocument::from_rasters(std::move(masks));
    auto job = SLAPrint::create_raster_job(config, document, {.05, .13, .2});
    const char *folder = std::getenv("SLA_JXS_ORACLE_OUTPUT");
    struct Directory {
        boost::filesystem::path path;
        bool keep;
        ~Directory() {
            if (!keep) {
                boost::system::error_code error;
                boost::filesystem::remove_all(path, error);
            }
        }
    } dir{folder ? boost::filesystem::path(folder) : boost::filesystem::unique_path("jxs-%%%%"), bool(folder)};
    boost::filesystem::create_directories(dir.path);
    const auto path = dir.path / (portrait ? "portrait.jxs" : "landscape.jxs");
    job->export_layer_document(document, "jxs", path.string(), {}, "JXS test");
    auto uniform = SLAPrint::create_raster_job(config, document, {.05, .1, .15});
    uniform->export_layer_document(document, "jxs",
                                   (dir.path / (portrait ? "uniform-portrait.jxs" : "uniform-landscape.jxs")).string(),
                                   {}, "JXS uniform");
    uniform->export_layer_document(document, "SL1", (dir.path / "source.sl1").string(), {}, "JXS source");
    auto zero_wait_config = config;
    zero_wait_config.set("sla_wait_before_cure", 0.);
    zero_wait_config.set("sla_bottom_wait_before_cure", 0.);
    auto zero_wait = SLAPrint::create_raster_job(zero_wait_config, document, {.05, .13, .2});
    zero_wait->export_layer_document(document, "jxs", (dir.path / "zero-wait.jxs").string(), {}, "zero wait");
    const auto zip = read_zipper_archive(path.string(), {""}, {});
    auto entry = [&](const std::string &name) -> const std::vector<uint8_t> & {
        const auto found =
            std::find_if(zip.entries.begin(), zip.entries.end(), [&](const auto &e) { return e.fname == name; });
        REQUIRE(found != zip.entries.end());
        return found->buf;
    };
    const auto control = nlohmann::json::parse(entry("control.json"));
    REQUIRE(control["total_slices"] == 3);
    REQUIRE(control["action_list"].size() > 30);
    REQUIRE(zip.config.get<uint32_t>("FirstLayerTime") == 20000);
    REQUIRE(zip.config.get<uint32_t>("NumLayers") == 3);
    REQUIRE(zip.config.get<uint32_t>("ZMoveTimeCompensation") == 1250);
    for (size_t i = 0; i < 3; ++i) {
        std::ostringstream name;
        name << std::setw(5) << std::setfill('0') << i << ".png";
        const auto &png = entry(name.str());
        REQUIRE(png.size() > 33);
        REQUIRE(std::memcmp(png.data(), "\x89PNG\r\n\x1a\n", 8) == 0);
        REQUIRE(png[24] == 8);
        REQUIRE(png[25] == 2); // packed RGB, not grayscale with a misleading extension.
        auto be = [&](size_t at) {
            return uint32_t(png[at]) << 24 | uint32_t(png[at + 1]) << 16 | uint32_t(png[at + 2]) << 8 | png[at + 3];
        };
        REQUIRE(be(16) == 1640);
        REQUIRE(be(20) == 2880);
    }
    const auto original = entry("control.json");
    for (const std::string key : {"display_width", "display_pixels_x", "max_print_height", "display_mirror_y"}) {
        auto invalid = config;
        if (key == "display_mirror_y")
            invalid.set(key, false);
        else if (key == "display_pixels_x")
            invalid.set(key, 1);
        else
            invalid.set(key, 1.);
        REQUIRE_THROWS(([&] {
            auto bad = SLAPrint::create_raster_job(invalid, document, {.05, .13, .2});
            bad->export_layer_document(document, "jxs", path.string(), {}, "bad");
        }()));
    }
    const auto after = read_zipper_archive(path.string(), {""}, {});
    const auto kept = std::find_if(after.entries.begin(), after.entries.end(),
                                   [](const auto &e) { return e.fname == "control.json"; });
    REQUIRE(kept != after.entries.end());
    REQUIRE(kept->buf == original);
}

TEST_CASE("ZCodex preserves grayscale and relative SLA motion without losing settings", "[sla_reference][sla_zcodex]") {
    const bool portrait = GENERATE(false, true);
    SLAFullPrintConfig full;
    Test::configure_archive_fixture(full, "zcodex");
    full.set("sla_archive_format", std::string("SL1"));
    if (portrait) {
        full.set("display_width",132.88); full.set("display_height",74.67);
        full.set("display_pixels_x",2560); full.set("display_pixels_y",1440);
        full.display_orientation.value = sladoPortrait;
    }
    full.set("sla_bottom_layers",1); full.set("exposure_time",3.); full.set("initial_exposure_time",20.);
    full.set("sla_wait_before_cure",1.25); full.set("sla_bottom_wait_before_cure",1.25);
    full.set("material_density",1.24);
    DynamicPrintConfig cfg; cfg.apply(full);
    std::vector<uint8_t> pixels(1440*2560,255);
    std::vector<sla::EncodedRaster> masks;
    masks.push_back(sla::PNGRasterEncoder{}(pixels.data(),1440,2560,1));
    std::fill(pixels.begin(),pixels.end(),0);
    masks.push_back(sla::PNGRasterEncoder{}(pixels.data(),1440,2560,1));
    for(size_t i=0;i<pixels.size();++i) pixels[i]=uint8_t(i);
    masks.push_back(sla::PNGRasterEncoder{}(pixels.data(),1440,2560,1));
    auto doc=sla::LayerDocument::from_rasters(std::move(masks));
    const char *folder=std::getenv("SLA_ZCODEX_ORACLE_OUTPUT");
    struct Directory {
        boost::filesystem::path path; bool keep;
        ~Directory(){if(!keep){boost::system::error_code e;boost::filesystem::remove_all(path,e);}}
    } dir{folder?boost::filesystem::path(folder):boost::filesystem::unique_path("zcodex-%%%%"),bool(folder)};
    boost::filesystem::create_directories(dir.path);
    ThumbnailData preview; preview.set(2,1); preview.pixels={255,0,0,255,0,0,255,255};
    auto job=SLAPrint::create_raster_job(cfg,doc,{.05,.13,.2});
    const auto path=dir.path/(portrait?"portrait.zcodex":"landscape.zcodex");
    job->export_layer_document(doc,"zcodex",path.string(),{preview},"ZCodex test");
    auto uniform=SLAPrint::create_raster_job(cfg,doc,{.05,.1,.15});
    uniform->export_layer_document(doc,"zcodex",(dir.path/(portrait?"uniform-portrait.zcodex":"uniform-landscape.zcodex")).string(),{},"uniform");
    uniform->export_layer_document(doc,"SL1",(dir.path/"source.sl1").string(),{},"source");
    const auto zip=read_zipper_archive(path.string(),{""},{});
    auto entry=[&](const std::string &name)->const std::vector<uint8_t>& {
        const auto e=std::find_if(zip.entries.begin(),zip.entries.end(),[&](const auto &e){return e.fname==boost::algorithm::to_lower_copy(name);});
        REQUIRE(e!=zip.entries.end()); return e->buf;
    };
    const auto resin=nlohmann::json::parse(entry("ResinMetadata"));
    REQUIRE(resin["TotalLayersCount"]==3); REQUIRE(resin["BottomLayersNumber"]==1);
    REQUIRE(resin["BlankingLayerTime"]==1250); REQUIRE(resin["LayerTime"]==3000);
    const auto &wire=entry("ResinGCodeData");
    const std::string protocol(wire.begin(),wire.end());
    REQUIRE(protocol.find("G1 Z-5.92 F100")!=std::string::npos);
    REQUIRE(protocol.find("G1 Z-5.93 F100")!=std::string::npos);
    REQUIRE(protocol.find("G1 Z40.0 F120\nM18\n")!=std::string::npos);
    for(size_t i=0;i<3;++i) {
        std::ostringstream name; name<<"ResinSlicesData/Slice"<<std::setw(5)<<std::setfill('0')<<i<<".png";
        const auto &bytes=entry(name.str());png::ImageGreyscale image;
        REQUIRE(png::decode_png(png::ReadBuf{bytes.data(),bytes.size()},image));
        REQUIRE(image.cols==1440);REQUIRE(image.rows==2560);REQUIRE(image.buf==doc.image(i).buf);
    }
    auto pwm_cfg=cfg;pwm_cfg.set("sla_bottom_light_pwm",160);pwm_cfg.set("sla_light_pwm",200);
    auto pwm=SLAPrint::create_raster_job(pwm_cfg,doc,{.05,.13,.2});
    pwm->export_layer_document(doc,"zcodex",(dir.path/(portrait?"pwm-portrait.zcodex":"pwm-landscape.zcodex")).string(),{},"PWM");
    for(const auto &bad: {std::pair{"display_width",75.}, {"sla_transition_layers",1.},
        {"sla_bottom_wait_before_cure",2.}, {"sla_lift_speed",120.5}, {"sla_lift_height2",2.},
        {"sla_wait_after_cure",.5}, {"sla_wait_before_cure",1.251}}) {
        auto invalid=cfg;
        if(std::string(bad.first)=="sla_transition_layers")invalid.set(bad.first,int(bad.second));
        else invalid.set(bad.first,bad.second);
        REQUIRE_THROWS(([&]{auto j=SLAPrint::create_raster_job(invalid,doc,{.05,.13,.2});
            j->export_layer_document(doc,"zcodex",path.string(),{},"invalid");}()));
    }
    auto fine=SLAPrint::create_raster_job(cfg,doc,{.05,.1305,.2});
    REQUIRE_THROWS(fine->export_layer_document(doc,"zcodex",path.string(),{},"fine Z"));
    auto high=SLAPrint::create_raster_job(cfg,doc,{130.,133.,136.});
    REQUIRE_THROWS(high->export_layer_document(doc,"zcodex",path.string(),{},"unsafe park"));
    const auto after=read_zipper_archive(path.string(),{""},{});
    const auto kept=std::find_if(after.entries.begin(),after.entries.end(),[](const auto &e){return e.fname=="resingcodedata";});
    REQUIRE(kept!=after.entries.end());REQUIRE(kept->buf==wire);
}

TEST_CASE("ZCode RSA framing matches independent modular exponentiation", "[sla_reference][sla_zcode_rsa]") {
    using namespace sla::zcode;
    const std::vector<std::pair<std::string,std::string>> vectors = {
        {"G21;","oJC+T84AdLSP+3Jaca6Zhh3wT2SRrPyfDTzqyx0GnxlRQHNc2bE58NJW7asZHVKzkUnv3xxo4NmSEizN8b0n+Q=="},
        {"M6054 \"1.png\";","M2FrZm0r0XeIgx+uRpG7jfCp3C42+8M9QEUm01//Vz64ytVDVLylrd5quslm/M+IRPLtqWEBFeyZNF8AMJtbuQ=="},
        {"G0 Z6.05 F12;","5bBY6adqZdp1pZkodrBB5RcMSs8gdrQ0FDmY8ruIwpw2Y4UB5Q9I6951MQdW6KqWPYFb/rc1JRsdtV/YjKpL7A=="},
        {"G4 P02500;","RnONMo+83mSox7xdwLbW0QbhuODT/PxZvEcQmeMsRumqykPN1O6gHMa2PQkDMaRm0R9qzdjGyEdN0+2zwFhmqA=="},
        {"M106 S300;","LI1qu/TEgZln537v8lbmNKmBK7uTdCwQo3ThwqFw+wDlmXcFDsW5FVMaZd9KTmpaDW2LDrYds18JlhRZXKYf4Q=="},
        {"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx","zHaUe0SGTVSMumDWv6xhT6TwsP2OYdpz2ShaJn3SVcUCee+EKoU9oKLcCGEn9zMdrb2sGaUuVQ9k4VKauOhWHA=="},
        {"ąąąąąąąąąąąąąąąąąąąąąąąąąąąąąą","xaLkQ9anRQDGsmrtjF9RcTIILJ2rqfyLfutc3F6m5GI2fLENGMe8V3cHPsC/oaKOHngabRqhsb/4vWqjC+Fnuw=="},
    };
    std::vector<std::string> lines;
    std::string expected;
    for (const auto &v:vectors) {
        REQUIRE(base64(encrypt_line(v.first))==v.second);
        lines.push_back(v.first);expected+=v.second+"\n";
    }
    REQUIRE(encrypt_lines(lines)==expected);
    REQUIRE_THROWS(encrypt_line(""));
    REQUIRE_THROWS(encrypt_line(std::string(62,'x')));
    REQUIRE_THROWS(encrypt_line("G21\nG90"));
    REQUIRE_THROWS(encrypt_line("G21\r"));
    std::vector<std::string> batch(20000);
    for(size_t i=0;i<batch.size();++i)batch[i]="G0 Z"+std::to_string(i)+" F12;";
    tbb::task_arena single(1),parallel(4);
    const auto start=std::chrono::steady_clock::now();
    const auto serial=single.execute([&]{return encrypt_lines(batch);});
    const auto middle=std::chrono::steady_clock::now();
    const auto concurrent=parallel.execute([&]{return encrypt_lines(batch);});
    const auto finish=std::chrono::steady_clock::now();
    REQUIRE(serial==concurrent);
    REQUIRE(serial.size()==batch.size()*89);
    if(const char *folder=std::getenv("SLA_ZCODE_RSA_OUTPUT")) {
        boost::filesystem::create_directories(folder);
        std::ofstream(std::string(folder)+"/rsa-lines.txt",std::ios::binary)<<serial;
        std::ofstream(std::string(folder)+"/benchmark.json")<<nlohmann::json{
            {"lines",batch.size()},
            {"serial_seconds",std::chrono::duration<double>(middle-start).count()},
            {"parallel_seconds",std::chrono::duration<double>(finish-middle).count()},
            {"workers",4},{"identical",serial==concurrent}}.dump(2);
    }
}

TEST_CASE("ZCode writes full grayscale XML and encrypted two-stage SLA protocol", "[sla_reference][sla_zcode]") {
    const bool portrait=GENERATE(false,true);
    SLAFullPrintConfig full;Test::configure_archive_fixture(full,"zcode");
    full.set("sla_archive_format",std::string("SL1"));
    full.display_orientation.value=portrait?sladoPortrait:sladoLandscape;
    full.set("display_width",portrait?128.:64.);full.set("display_height",portrait?64.:128.);
    full.set("display_pixels_x",portrait?256:128);full.set("display_pixels_y",portrait?128:256);
    full.set("max_print_height",220.);full.set("printer_model",std::string("IBEE"));
    full.set("sla_bottom_layers",1);full.set("exposure_time",3.);full.set("initial_exposure_time",20.);
    full.set("material_density",1.24);full.set("bottle_cost",25.);full.set("bottle_volume",1000.);
    for(const std::string prefix:{"sla_","sla_bottom_"}) {
        const bool bottom=prefix=="sla_bottom_";
        full.set(prefix+"lift_height",bottom?4.:6.);full.set(prefix+"lift_height2",2.);
        full.set(prefix+"lift_speed",bottom?60.:120.);full.set(prefix+"lift_speed2",180.);
        full.set(prefix+"retract_height2",1.);full.set(prefix+"retract_speed",bottom?80.:100.);
        full.set(prefix+"retract_speed2",140.);full.set(prefix+"wait_before_cure",bottom?2.5:1.25);
        full.set(prefix+"wait_after_cure",.5);full.set(prefix+"wait_after_lift",.75);
    }
    DynamicPrintConfig cfg;cfg.apply(full);
    auto rounded=cfg;
    rounded.set("sla_archive_format",std::string("zcode"));
    rounded.set("display_width",portrait?123.84:198.14);rounded.set("display_height",portrait?198.14:123.84);
    rounded.set("display_pixels_x",portrait?3600:5760);rounded.set("display_pixels_y",portrait?5760:3600);
    SLAPrint probe;Model empty;probe.apply(empty,rounded);
    REQUIRE_NOTHROW(ZCodeArchive::validate_configuration(probe));
    std::vector<uint8_t> pixels(128*256,255);std::vector<sla::EncodedRaster> masks;
    masks.push_back(sla::PNGRasterEncoder{}(pixels.data(),128,256,1));
    std::fill(pixels.begin(),pixels.end(),0);masks.push_back(sla::PNGRasterEncoder{}(pixels.data(),128,256,1));
    for(size_t i=0;i<pixels.size();++i)pixels[i]=uint8_t(i);
    masks.push_back(sla::PNGRasterEncoder{}(pixels.data(),128,256,1));
    auto doc=sla::LayerDocument::from_rasters(std::move(masks));
    const char *folder=std::getenv("SLA_ZCODE_ORACLE_OUTPUT");
    struct Directory{boost::filesystem::path path;bool keep;
        ~Directory(){if(!keep){boost::system::error_code e;boost::filesystem::remove_all(path,e);}}}
        dir{folder?boost::filesystem::path(folder):boost::filesystem::unique_path("zcode-%%%%"),bool(folder)};
    boost::filesystem::create_directories(dir.path);
    const std::string label="ZCode test <żywica> & \"test\"";
    ThumbnailData preview;preview.set(2,1);preview.pixels={255,0,0,255,0,0,255,255};
    auto job=SLAPrint::create_raster_job(cfg,doc,{.05,.13,.2});
    const auto path=dir.path/(portrait?"portrait.zcode":"landscape.zcode");
    job->export_layer_document(doc,"zcode",path.string(),{preview},label);
    auto uniform=SLAPrint::create_raster_job(cfg,doc,{.05,.1,.15});
    uniform->export_layer_document(doc,"zcode",(dir.path/(portrait?"uniform-portrait.zcode":"uniform-landscape.zcode")).string(),{},label);
    uniform->export_layer_document(doc,"SL1",(dir.path/"source.sl1").string(),{},"source");
    for(const std::string variant:{"pwm","transition","zero-before"}) {
        auto modified=cfg;
        if(variant=="pwm"){modified.set("sla_bottom_light_pwm",160);modified.set("sla_light_pwm",200);}
        else if(variant=="zero-before"){modified.set("sla_bottom_wait_before_cure",0.);modified.set("sla_wait_before_cure",0.);}
        else if(variant=="zero-pwm"){modified.set("sla_bottom_light_pwm",0);modified.set("sla_light_pwm",0);}
        else modified.set("sla_transition_layers",1);
        auto varied=SLAPrint::create_raster_job(modified,doc,{.05,.13,.2});
        varied->export_layer_document(doc,"zcode",(dir.path/(variant+(portrait?"-portrait.zcode":"-landscape.zcode"))).string(),{},label);
    }
    auto disabled=cfg;disabled.set("sla_light_pwm",0);
    REQUIRE_THROWS(([&]{auto j=SLAPrint::create_raster_job(disabled,doc,{.05,.13,.2});
        j->export_layer_document(doc,"zcode",path.string(),{},"zero PWM");}()));
    const auto zip=read_zipper_archive(path.string(),{""},{});
    auto entry=[&](const std::string &name)->const std::vector<uint8_t>& {
        const auto e=std::find_if(zip.entries.begin(),zip.entries.end(),[&](const auto &e){return e.fname==name;});
        REQUIRE(e!=zip.entries.end());return e->buf;
    };
    const auto &xml=entry("task.xml");const std::string manifest(xml.begin(),xml.end());
    REQUIRE(manifest.find("&lt;żywica&gt; &amp; &quot;test&quot;")!=std::string::npos);
    REQUIRE(manifest.find("pixel_size=\"500\"")!=std::string::npos);
    REQUIRE(manifest.find("<layers>3</layers>")!=std::string::npos);
    const auto &cipher=entry("lcd.gcode");std::istringstream stream(std::string(cipher.begin(),cipher.end()));std::string line;
    size_t count=0;while(std::getline(stream,line)){REQUIRE(line.size()==88);REQUIRE(line.substr(86)=="==");++count;}
    REQUIRE(count>30);
    for(size_t i=0;i<3;++i) {
        const auto &bytes=entry(std::to_string(i+1)+".png");png::ImageGreyscale image;
        REQUIRE(png::decode_png(png::ReadBuf{bytes.data(),bytes.size()},image));REQUIRE(image.buf==doc.image(i).buf);
    }
    for(const auto &bad:{std::pair{"display_width",65.},{"max_print_height",220.0005},{"sla_lift_speed",120.01},
        {"sla_wait_before_cure",1.251},{"sla_retract_height2",100.}}) {
        auto invalid=cfg;invalid.set(bad.first,bad.second);
        REQUIRE_THROWS(([&]{auto j=SLAPrint::create_raster_job(invalid,doc,{.05,.13,.2});
            j->export_layer_document(doc,"zcode",path.string(),{},"invalid");}()));
    }
    auto fine=SLAPrint::create_raster_job(cfg,doc,{.05,.1305,.2});
    REQUIRE_THROWS(fine->export_layer_document(doc,"zcode",path.string(),{},"fine"));
    const auto after=read_zipper_archive(path.string(),{""},{});
    const auto kept=std::find_if(after.entries.begin(),after.entries.end(),[](const auto &e){return e.fname=="lcd.gcode";});
    REQUIRE(kept!=after.entries.end());REQUIRE(kept->buf==cipher);
}


TEST_CASE("NanoDLP preserves every grayscale byte in packed, plain and RGB PNGs", "[sla_reference][nanodlp_encoder]") {
    const size_t width = GENERATE(size_t(200), size_t(201));
    constexpr size_t height = 7;
    std::vector<uint8_t> source(width * height);
    for (size_t i = 0; i < source.size(); ++i) source[i] = uint8_t((i * 73 + i / width * 19) % 256);
    const auto mono = nanodlp::encode_layer(source.data(), width, height, 1);
    const auto rgb = nanodlp::encode_layer(source.data(), width, height, 1, nanodlp::ImageMode::RGB);
    auto be32 = [](const uint8_t *p) { return uint32_t(p[0])<<24 | uint32_t(p[1])<<16 | uint32_t(p[2])<<8 | p[3]; };
    for (const auto *image : {&mono, &rgb}) {
        REQUIRE(image->size() > 33);
        const auto *bytes = static_cast<const uint8_t *>(image->data());
        REQUIRE(std::memcmp(bytes, "\x89PNG\r\n\x1a\n", 8) == 0);
        REQUIRE(be32(bytes + 20) == height);
        REQUIRE(bytes[24] == 8);
    }
    const auto *m = static_cast<const uint8_t *>(mono.data());
    REQUIRE(be32(m + 16) == (width % 3 == 0 ? width / 3 : width));
    REQUIRE(m[25] == (width % 3 == 0 ? 2 : 0));
    const auto *r = static_cast<const uint8_t *>(rgb.data());
    REQUIRE(be32(r + 16) == width); REQUIRE(r[25] == 2);
    if (width % 3 != 0) {
        png::ImageGreyscale decoded;
        REQUIRE(png::decode_png(png::ReadBuf{mono.data(), mono.size()}, decoded));
        REQUIRE(decoded.buf == source);
    }
    REQUIRE_THROWS(nanodlp::encode_layer(nullptr, width, height, 1));
    REQUIRE_THROWS(nanodlp::encode_layer(source.data(), width, height, 3));
    REQUIRE_THROWS(nanodlp::encode_layer(source.data(), 0, height, 1));
    REQUIRE_THROWS(nanodlp::encode_layer(source.data(), 32769, height, 1));
    REQUIRE_THROWS(nanodlp::encode_layer(source.data(), 32768, 32768, 1));
    auto schema = nanodlp::schema_defaults();
    REQUIRE(schema["meta"]["FormatVersion"] == 2);
    REQUIRE(schema["slicer"]["DisplayController"] == 1);
    schema["profile"]["Title"] = "changed";
    REQUIRE(nanodlp::schema_defaults()["profile"]["Title"] == "");
    if (const char *output = std::getenv("SLA_NANODLP_FIXTURE_DIR")) {
        const boost::filesystem::path dir(output); boost::filesystem::create_directories(dir);
        auto write = [&](const std::string &name, const void *data, size_t size) {
            std::ofstream out((dir / name).string(), std::ios::binary);
            REQUIRE(bool(out)); out.write(static_cast<const char *>(data), size); REQUIRE(bool(out));
        };
        const auto stem = std::to_string(width);
        write(stem + ".gray", source.data(), source.size());
        write(stem + "-mono.png", mono.data(), mono.size());
        write(stem + "-rgb.png", rgb.data(), rgb.size());
        const auto defaults = nanodlp::schema_defaults().dump(2);
        write("schema-native.json", defaults.data(), defaults.size());
    }
    // A direct call cannot silently succeed, even while registry access is blocked.
    NanoDLPArchive archive{SLAPrinterConfig{}}; SLAPrint print;
    REQUIRE_THROWS(archive.export_print("unused.nanodlp", print, {}, "test"));
}


TEST_CASE("NanoDLP contour statistics include holes and nested islands", "[sla_reference][nanodlp_contours][pixel_contours]") {
    nlohmann::json cases=nlohmann::json::array();
    const char *path_output=std::getenv("SLA_PIXEL_CONTOUR_FIXTURES");
    const bool packet_roundtrip=std::getenv("SLA_SVGX_PACKET_ROUNDTRIP")!=nullptr;
    auto check=[&](const std::string &name, size_t width, size_t height, const std::vector<uint8_t> &pixels, float pixel_area=.0036f) {
        std::vector<sla::PixelContour> contours;
        const auto stats=path_output?sla::trace_pixel_contours(pixels.data(),width,height,&contours):nanodlp::contour_statistics(pixels.data(),width,height);
        if(packet_roundtrip) {
            auto cached=svgx::decode_contours(svgx::encode_contours(pixels.data(),width,height,1));
            REQUIRE(cached.occupied_pixels==stats.occupied_pixels);
            contours=std::move(cached.contours);
        }
        REQUIRE(stats.occupied_pixels==size_t(std::count_if(pixels.begin(),pixels.end(),[](uint8_t p){return p!=0;})));
        REQUIRE(stats.external_count<=stats.occupied_pixels);
        REQUIRE(stats.largest_area>=stats.smallest_area);
        if(stats.occupied_pixels) {
            REQUIRE(stats.min_x<stats.max_x);REQUIRE(stats.min_y<stats.max_y);
            REQUIRE(stats.max_x<=width);REQUIRE(stats.max_y<=height);
        } else {
            REQUIRE(stats.total_area==0);REQUIRE(stats.external_count==0);
            REQUIRE(stats.min_x==0);REQUIRE(stats.min_y==0);
        }
        std::string hex;hex.reserve(pixels.size()*2);const char *digits="0123456789abcdef";
        for(const auto p:pixels){hex+=digits[p>>4];hex+=digits[p&15];}
        cases.push_back({{"name",name},{"width",width},{"height",height},{"pixels",hex},
            {"pixel_area",pixel_area},{"info",nanodlp::layer_info(stats,pixel_area)},
            {"total_area",stats.total_area},{"smallest_area",stats.smallest_area},
            {"largest_area",stats.largest_area},{"external_count",stats.external_count},
            {"occupied_pixels",stats.occupied_pixels},{"bbox",{stats.min_x,stats.min_y,stats.max_x,stats.max_y}}});
        if(path_output) {
            nlohmann::json paths=nlohmann::json::array();
            for(const auto &contour:contours) {
                bool valid=!contour.points.empty() && contour.root>=0 && size_t(contour.root)<contours.size() &&
                    contour.parent>=-1 && (contour.parent<0 || size_t(contour.parent)<contours.size());
                for(const auto &[x,y]:contour.points)
                    valid=valid && x>=0 && y>=0 && size_t(x)<width && size_t(y)<height && pixels[size_t(y)*width+x]!=0;
                REQUIRE(valid);
                paths.push_back({{"points",contour.points},{"parent",contour.parent},{"root",contour.root},
                    {"hole",contour.hole},{"area",contour.area},{"perimeter",contour.perimeter}});
            }
            cases.back()["contours"]=std::move(paths);
        }
        return stats;
    };
    // Exhaustive 3x3 patterns include single pixels, diagonal branches, closed
    // holes and boundary contacts. Vary intensity to enforce nonzero semantics.
    for(unsigned bits=0;bits<512;++bits) {
        std::vector<uint8_t> pixels(9);
        for(unsigned i=0;i<9;++i)pixels[i]=(bits&(1u<<i))?uint8_t(i*31+1):0;
        check("exhaustive-"+std::to_string(bits),3,3,pixels);
    }
    // All 4x4 masks exercise multiple disjoint objects/holes and diagonal
    // hierarchy contacts. Original EmguContours verifies every case separately.
    for(unsigned bits=0;bits<65536;++bits) {
        std::vector<uint8_t> pixels(16);
        for(unsigned i=0;i<16;++i)pixels[i]=(bits&(1u<<i))?uint8_t((i*17)%255+1):0;
        check("exhaustive4-"+std::to_string(bits),4,4,pixels);
    }
    uint32_t state=0x8264129;
    for(unsigned i=0;i<2000;++i) {
        const size_t width=4+i%13,height=4+(i/13)%13;std::vector<uint8_t> pixels(width*height);
        for(auto &pixel:pixels){state=state*1664525u+1013904223u;pixel=(state>>24)>i%256?uint8_t((state>>16)|1):0;}
        check("random-"+std::to_string(i),width,height,pixels);
    }
    std::vector<uint8_t> nested(21*21);
    auto rectangle=[&](int a,int b,uint8_t value){for(int y=a;y<b;++y)for(int x=a;x<b;++x)nested[y*21+x]=value;};
    rectangle(1,20,255);rectangle(3,18,0);rectangle(5,16,1);rectangle(7,14,0);rectangle(9,12,128);
    const auto stats=check("nested-five-borders",21,21,nested);
    REQUIRE(stats.external_count==1);
    // Pixel-centre outer squares, minus rounded-corner hole borders:
    // 18^2 - (16^2-2) + 10^2 - (8^2-2) + 2^2 = 112.
    REQUIRE(stats.total_area==112);
    const float areas[]{.00000125f,.00005f,.0001f,.0016f,.04725f*.04725f,1.5f,999.999f};
    for(size_t i=0;i<sizeof(areas)/sizeof(areas[0]);++i)
        check("rounding-"+std::to_string(i),21,21,nested,areas[i]);
    REQUIRE_THROWS(nanodlp::layer_info(stats,0));
    REQUIRE_THROWS(nanodlp::layer_info(stats,-1));
    REQUIRE_THROWS(nanodlp::contour_statistics(nullptr,3,3));
    REQUIRE_THROWS(nanodlp::contour_statistics(nested.data(),0,21));
    if(const char *output=std::getenv("SLA_NANODLP_CONTOUR_FIXTURES")) {
        std::ofstream stream(output);REQUIRE(bool(stream));stream<<cases.dump(2);REQUIRE(bool(stream));
    }
    if(path_output) {
        std::ofstream stream(path_output);REQUIRE(bool(stream));stream<<cases.dump();REQUIRE(bool(stream));
    }
}


TEST_CASE("Pixel contour paths retain a hole and nested isolated island", "[sla_reference][pixel_contours]") {
    std::vector<uint8_t> pixels(49,255);
    for(int y=2;y<5;++y)for(int x=2;x<5;++x)pixels[y*7+x]=0;
    pixels[3*7+3]=1;
    std::vector<sla::PixelContour> contours;
    const auto stats=sla::trace_pixel_contours(pixels.data(),7,7,&contours);
    REQUIRE(stats.occupied_pixels==41);REQUIRE(stats.total_area==22);REQUIRE(contours.size()==3);
    const auto root=std::find_if(contours.begin(),contours.end(),[](const auto &c){return c.parent==-1;});
    const auto hole=std::find_if(contours.begin(),contours.end(),[](const auto &c){return c.hole;});
    const auto island=std::find_if(contours.begin(),contours.end(),[](const auto &c){return c.points.size()==1;});
    REQUIRE(root!=contours.end());REQUIRE(hole!=contours.end());REQUIRE(island!=contours.end());
    REQUIRE(root->area==36);REQUIRE(root->perimeter==24);REQUIRE(root->points.size()==4);
    REQUIRE(hole->area==14);REQUIRE(hole->points.size()==8);
    REQUIRE(island->area==0);REQUIRE(island->perimeter==0);
    REQUIRE(island->points.front()==std::make_pair(3,3));
    REQUIRE(hole->parent==std::distance(contours.begin(),root));
    REQUIRE(island->parent==std::distance(contours.begin(),hole));
    REQUIRE(island->root==std::distance(contours.begin(),root));
    std::fill(pixels.begin(),pixels.end(),0);
    REQUIRE(sla::trace_pixel_contours(pixels.data(),7,7,&contours).occupied_pixels==0);
    REQUIRE(contours.empty());
    FlashforgeSVGXArchive archive{SLAPrinterConfig{}};SLAPrint print;
    const auto path=boost::filesystem::unique_path("incomplete-svgx-%%%%.svgx");
    REQUIRE_THROWS(archive.export_print(path.string(),print,{},"unfinished"));
    REQUIRE_FALSE(boost::filesystem::exists(path));
}

TEST_CASE("SVGX private packets preserve paths and reject corrupted trees", "[sla_reference][svgx_packet]") {
    std::vector<uint8_t> pixels(49,255);
    for(int y=2;y<5;++y)for(int x=2;x<5;++x)pixels[y*7+x]=0;
    pixels[24]=1;
    const auto encoded=svgx::encode_contours(pixels.data(),7,7,1);
    const auto packet=svgx::decode_contours(encoded);
    REQUIRE(packet.width==7);REQUIRE(packet.height==7);REQUIRE(packet.occupied_pixels==41);
    REQUIRE(packet.contours.size()==3);
    REQUIRE(packet.contours[0].area==36);REQUIRE(packet.contours[0].perimeter==24);
    REQUIRE(packet.contours[1].points==std::vector<std::pair<int,int>>{{3,3}});
    REQUIRE(packet.contours[1].parent==2);REQUIRE(packet.contours[2].parent==0);
    const auto *bytes=static_cast<const uint8_t *>(encoded.data());
    const std::vector<uint8_t> valid(bytes,bytes+encoded.size());
    auto rejects=[&](std::vector<uint8_t> data){REQUIRE_THROWS(svgx::decode_contours(sla::EncodedRaster(std::move(data),"test")));};
    auto bad=valid;bad[0]='P';rejects(bad);
    bad=valid;bad.pop_back();rejects(bad);
    bad=valid;bad.push_back(0);rejects(bad);
    bad=valid;bad[24]=0;bad[25]=bad[26]=bad[27]=0;rejects(bad); // Root points to itself.
    bad=valid;bad[32]=1;rejects(bad); // A root cannot be a hole.
    bad=valid;bad[56]=7;rejects(bad); // Point outside the raster.
    REQUIRE_THROWS(svgx::encode_contours(pixels.data(),7,7,3));
    std::fill(pixels.begin(),pixels.end(),0);
    const auto black=svgx::decode_contours(svgx::encode_contours(pixels.data(),7,7,1));
    REQUIRE(black.contours.empty());REQUIRE(black.occupied_pixels==0);
}

TEST_CASE("SVGX coordinates survive float projection and binary SVG files retain masks", "[sla_reference][svgx_file]") {
    for(uint32_t resolution:{1u,37u,65u,640u,1440u,5760u,32768u})
        for(float display:{.001f,7.789f,11.123f,68.04f,120.f,218.88f,1000.f}) {
            const auto coordinates=svgx::pixel_coordinates(resolution,display);
            for(uint32_t x=0;x<resolution;++x) {
                const float projected=std::clamp(display/2+coordinates[x],0.f,display)*(float(resolution)/display);
                REQUIRE(int(projected)==int(x));
            }
        }
    const bool portrait=GENERATE(false,true),wide=GENERATE(false,true);
    svgx::FileParameters p;
    p.width=wide?5760:65;p.height=37;p.display_width=wide?218.88f:11.123f;p.display_height=7.789f;
    if(portrait){std::swap(p.width,p.height);std::swap(p.display_width,p.display_height);}
    p.machine_z=210;p.layer_height=.05f;p.bottom_layers=1;p.bottom_exposure=20;p.exposure=3;
    p.machine="SVGX test <&\" żywica";p.material="Resin & test";
    std::vector<sla::EncodedRaster> layers,pngs;nlohmann::json masks=nlohmann::json::array();
    for(unsigned layer=0;layer<6;++layer) {
        std::vector<uint8_t> pixels(size_t(p.width)*p.height,0);
        if(layer==1)std::fill(pixels.begin(),pixels.end(),255);
        if(layer==2) {
            for(uint32_t y=2;y<p.height-2;++y)for(uint32_t x=2;x<p.width-2;++x)pixels[size_t(y)*p.width+x]=1;
            for(uint32_t y=5;y<p.height-5;++y)for(uint32_t x=5;x<p.width-5;++x)pixels[size_t(y)*p.width+x]=0;
            pixels[size_t(p.height/2)*p.width+p.width/2]=127;
        }
        if(layer>=3) {
            for(uint32_t x=0;x<p.width;++x)pixels[size_t((x*7+layer)%p.height)*p.width+x]=uint8_t(1+x%255);
            pixels.front()=pixels.back()=1;
        }
        layers.push_back(svgx::encode_contours(pixels.data(),p.width,p.height,1));
        pngs.push_back(sla::PNGRasterEncoder{}(pixels.data(),p.width,p.height,1));
        std::string hex;const char *digits="0123456789abcdef";
        for(auto value:pixels){hex+=digits[value>>4];hex+=digits[value&15];}
        masks.push_back(hex);
    }
    ThumbnailData thumbnail;thumbnail.set(2,2);
    thumbnail.pixels={255,0,0,255,0,255,0,255,0,0,255,255,255,255,255,255};
    ThumbnailsList thumbnails{thumbnail};
    const char *output=std::getenv("SLA_SVGX_FILE_FIXTURES");
    const auto directory=output?boost::filesystem::path(output):boost::filesystem::temp_directory_path();
    boost::filesystem::create_directories(directory);
    const auto path=directory/(output?std::string(wide?"wide-":"small-")+(portrait?"portrait":"landscape")+".svgx":boost::filesystem::unique_path("svgx-file-%%%%.svgx").string());
    svgx::write_file(path.string(),p,layers,thumbnails);
    REQUIRE(boost::filesystem::file_size(path)>28+2*54+128*128*3+200*240*3);
    std::ifstream first_file(path.string(),std::ios::binary);
    const std::vector<uint8_t> expected{std::istreambuf_iterator<char>(first_file),{}};first_file.close();
    SLAFullPrintConfig full;Test::configure_archive_fixture(full,"uvj");
    full.display_orientation.value=portrait?sladoPortrait:sladoLandscape;
    full.set("display_pixels_x",int(portrait?p.height:p.width));full.set("display_pixels_y",int(portrait?p.width:p.height));
    full.set("display_width",double(portrait?p.display_height:p.display_width));full.set("display_height",double(portrait?p.display_width:p.display_height));
    full.set("display_mirror_x",false);full.set("display_mirror_y",true);
    full.set("sla_transition_layers",0);full.set("sla_bottom_layers",1);
    full.set("max_print_height",210.);full.set("printer_model",p.machine);
    full.set("exposure_time",3.);
    for(bool bottom:{false,true}) {
        const std::string prefix=bottom?"sla_bottom_":"sla_";
        full.set(prefix+"lift_height",6.);full.set(prefix+"lift_speed",100.);full.set(prefix+"retract_speed",100.);
        full.set(prefix+"wait_before_cure",0.);full.set(prefix+"wait_after_cure",0.);full.set(prefix+"wait_after_lift",0.);
        full.set(prefix+"lift_height2",0.);full.set(prefix+"retract_height2",0.);full.set(prefix+"light_pwm",255);
    }
    DynamicPrintConfig cfg;cfg.apply(full);cfg.set_key_value("material_type",new ConfigOptionString(p.material));
    const auto document=sla::LayerDocument::from_rasters(std::move(pngs));
    const auto print=SLAPrint::create_raster_job(cfg,document,{.05,.1,.15,.2,.25,.3});
    FlashforgeSVGXArchive archive(print->printer_config());archive.encode_layers(document);
    archive.export_print(path.string(),*print,thumbnails,"SVGX native API");
    auto bytes=[&](){std::ifstream in(path.string(),std::ios::binary);return std::vector<uint8_t>{std::istreambuf_iterator<char>(in),{}};};
    REQUIRE(bytes()==expected);
    for(const auto &[key,value]:std::vector<std::pair<std::string,double>>{
        {"sla_lift_height",7},{"sla_bottom_lift_speed",120},{"sla_retract_speed",120},
        {"sla_wait_before_cure",1},{"sla_bottom_wait_after_cure",1},{"sla_lift_height2",1}}) {
        CAPTURE(key);
        auto bad=cfg;bad.set(key,value);
        const auto invalid=SLAPrint::create_raster_job(bad,document,{.05,.1,.15,.2,.25,.3});
        REQUIRE_THROWS(archive.export_print(path.string(),*invalid,{},"unsupported"));
        REQUIRE(bytes()==expected);
    }
    auto height_cfg=cfg;height_cfg.set("initial_layer_height",.1);
    SLAPrint height_print;height_print.apply(Model{},height_cfg);
    REQUIRE_THROWS(FlashforgeSVGXArchive::validate_configuration(height_print));
    auto named_cfg=cfg;named_cfg.set_key_value("sla_material_settings_id",new ConfigOptionString("Named SLA preset"));
    const auto named_print=SLAPrint::create_raster_job(named_cfg,document,{.05,.1,.15,.2,.25,.3});
    const auto named_path=path.string()+".named.svgx";
    archive.export_print(named_path,*named_print,thumbnails,"Named material");
    std::ifstream named_file(named_path,std::ios::binary);const std::string named_text{std::istreambuf_iterator<char>(named_file),{}};
    REQUIRE(named_text.find("materialname=\"Named SLA preset\"")!=std::string::npos);named_file.close();
    boost::filesystem::remove(named_path);
    height_cfg.set("initial_layer_height",.025);height_cfg.set("layer_height",.025);
    height_print.apply(Model{},height_cfg);
    REQUIRE_NOTHROW(FlashforgeSVGXArchive::validate_configuration(height_print));
    const auto half_height=SLAPrint::create_raster_job(height_cfg,document,{.025,.05,.075,.1,.125,.15});
    const auto height_path=path.string()+".025.svgx";
    archive.export_print(height_path,*half_height,thumbnails,"0.025 mm");
    REQUIRE(boost::filesystem::file_size(height_path)>194000);
    if(!output)boost::filesystem::remove(height_path);
    height_cfg.set("initial_layer_height",.0255);height_cfg.set("layer_height",.0255);
    height_print.apply(Model{},height_cfg);
    REQUIRE_THROWS(FlashforgeSVGXArchive::validate_configuration(height_print));
    auto bad=cfg;bad.set("sla_light_pwm",200);
    auto invalid=SLAPrint::create_raster_job(bad,document,{.05,.1,.15,.2,.25,.3});
    REQUIRE_THROWS(FlashforgeSVGXArchive::validate_configuration(*invalid));
    bad=cfg;bad.set("sla_transition_layers",1);
    invalid=SLAPrint::create_raster_job(bad,document,{.05,.1,.15,.2,.25,.3});
    REQUIRE_THROWS(FlashforgeSVGXArchive::validate_configuration(*invalid));
    bad=cfg;bad.set("display_mirror_y",false);
    invalid=SLAPrint::create_raster_job(bad,document,{.05,.1,.15,.2,.25,.3});
    REQUIRE_THROWS(FlashforgeSVGXArchive::validate_configuration(*invalid));
    invalid=SLAPrint::create_raster_job(cfg,document,{.05,.1,.15,.2,.25,.31});
    REQUIRE_THROWS(archive.export_print(path.string(),*invalid,{},"variable Z"));
    REQUIRE(bytes()==expected);
    if(output) {
        nlohmann::json fixture={{"filename",path.string()},{"width",p.width},{"height",p.height},
            {"display_width",p.display_width},{"display_height",p.display_height},{"masks",masks}};
        std::ofstream manifest(path.string()+".json");manifest<<fixture.dump();REQUIRE(bool(manifest));
    } else boost::filesystem::remove(path);
    REQUIRE_THROWS(svgx::pixel_coordinates(0,120));REQUIRE_THROWS(svgx::pixel_coordinates(65,0));
}

TEST_CASE("NanoDLP staged metadata preserves geometry masks and cached statistics", "[sla_reference][nanodlp_metadata]") {
    const bool portrait=GENERATE(false,true), rgb=GENERATE(false,true);
    const int scene_width=GENERATE(200,201);
    SLAFullPrintConfig full;
    full.printer_technology.setInt(ptSLA);full.set("sla_archive_format","sl1");
    full.set("display_pixels_x",scene_width);full.set("display_pixels_y",12);
    full.set("display_width",scene_width*.1);full.set("display_height",1.2);
    full.set("display_mirror_x",false);full.set("display_mirror_y",false);
    full.display_orientation.value=portrait?sladoPortrait:sladoLandscape;
    full.set("max_print_height",210.);full.set("printer_model","NanoDLP test żywica");
    full.set("layer_height",.05);full.set("initial_layer_height",.05);
    full.set("sla_bottom_layers",1);full.set("sla_transition_layers",0);
    full.set("initial_exposure_time",20.);full.set("exposure_time",3.);
    for(bool bottom:{false,true}) {
        const std::string prefix=bottom?"sla_bottom_":"sla_";
        full.set(prefix+"light_pwm",180);full.set(prefix+"wait_before_cure",bottom?2.5:1.25);
        full.set(prefix+"wait_after_cure",.5);full.set(prefix+"wait_after_lift",.75);
        full.set(prefix+"lift_height",bottom?2.:3.);
    }
    DynamicPrintConfig config;config.apply(full);
    const size_t width=portrait?12:scene_width,height=portrait?scene_width:12;
    std::vector<uint8_t> pixels(width*height,0),black(width*height,0);
    for(size_t y=2;y<6;++y)for(size_t x=2;x<6;++x)pixels[y*width+x]=uint8_t(1+(x+y)*25);
    const auto encoded=sla::PNGRasterEncoder{}(pixels.data(),width,height,1);
    const auto blank=sla::PNGRasterEncoder{}(black.data(),width,height,1);
    const auto document=sla::LayerDocument::from_rasters({encoded,encoded,blank});
    auto print=SLAPrint::create_raster_job(config,document,{.05,.1,.15});
    struct Archive : NanoDLPArchive {
        using NanoDLPArchive::NanoDLPArchive;
        using NanoDLPArchive::m_layers;
    };
    Archive archive(print->printer_config(),rgb);
    archive.encode_layers(document);
    const auto metadata=archive.prepare_metadata(*print,"test <projekt> & żywica");
    REQUIRE(metadata["meta"]["Program"]=="Sla-SlicerOS");
    REQUIRE(metadata["profile"]["ShieldBeforeLayer"]=="");
    REQUIRE(metadata["profile"]["ShieldAfterLayer"]=="");
    REQUIRE(metadata["slicer"]["PWidth"]==width);REQUIRE(metadata["slicer"]["PHeight"]==height);
    REQUIRE(metadata["slicer"]["XPixelSize"].get<double>()==Catch::Approx(.1));
    REQUIRE(metadata["slicer"]["YPixelSize"].get<double>()==Catch::Approx(.1));
    REQUIRE(metadata["plate"]["LayersCount"]==3);
    REQUIRE(metadata["profile"]["SupportCureTime"]==20);
    REQUIRE(metadata["profile"]["CureTime"]==3);
    REQUIRE(metadata["profile"]["SupportWaitBeforePrint"]==2.5);
    REQUIRE(metadata["profile"]["WaitBeforePrint"]==1.25);
    REQUIRE(metadata["profile"]["SupportWaitHeight"]==2);
    REQUIRE(metadata["profile"]["WaitHeight"]==3);
    REQUIRE(metadata["info"][0]==metadata["info"][1]);
    REQUIRE(metadata["info"][0]["TotalSolidArea"]==.09);
    REQUIRE(metadata["info"][0]["AreaCount"]==1);
    REQUIRE(metadata["info"][0]["MinX"]==2);REQUIRE(metadata["info"][0]["MaxX"]==6);
    REQUIRE(metadata["info"][2]["TotalSolidArea"]==0);
    REQUIRE(metadata["info"][2]["AreaCount"]==0);
    REQUIRE(metadata["plate"]["TotalSolidArea"].get<double>()==Catch::Approx(.18));
    REQUIRE(metadata["native_statistics"]["material_milliliters"].get<double>()==Catch::Approx(.000016));
    REQUIRE(archive.m_layers[0].size()==archive.m_layers[1].size());
    REQUIRE(std::memcmp(archive.m_layers[0].data(),archive.m_layers[1].data(),archive.m_layers[0].size())==0);
    REQUIRE(metadata["plate"]["ZMax"].get<double>()==Catch::Approx(.15));
    REQUIRE(metadata["plate"]["Updated"].get<uint64_t>()>0);
    for(const auto &packet:archive.m_layers) {
        const auto view=nanodlp::inspect_packet(packet);
        REQUIRE(std::memcmp(view.png,"\x89PNG\r\n\x1a\n",8)==0);
        REQUIRE(view.metadata["rgb"]==rgb);
    }
    auto damaged=std::vector<uint8_t>(8,0);
    REQUIRE_THROWS(nanodlp::inspect_packet(sla::EncodedRaster(std::move(damaged),"nanodlp-internal")));
    REQUIRE_THROWS(archive.export_print("unused.nanodlp",*print,{},"test"));
    auto bad_screen=print->printer_config();bad_screen.display_width.value=0;
    Archive invalid_screen(bad_screen,rgb);invalid_screen.encode_layers(document);
    REQUIRE_THROWS(invalid_screen.prepare_metadata(*print,"invalid screen"));
    auto mismatch=print->printer_config();mismatch.display_pixels_x.value=199;
    Archive invalid_resolution(mismatch,rgb);invalid_resolution.encode_layers(document);
    REQUIRE_THROWS(invalid_resolution.prepare_metadata(*print,"mismatched resolution"));
    REQUIRE_THROWS(archive.prepare_metadata(*print,std::string(1,char(0xff))));

    if(const char *output=std::getenv("SLA_NANODLP_METADATA_FIXTURES")) {
        const boost::filesystem::path dir(output);boost::filesystem::create_directories(dir);
        const auto stem=std::to_string(scene_width)+(portrait?"-portrait":"-landscape")+(rgb?"-rgb":"-mono");
        std::ofstream json((dir/(stem+".json")).string());REQUIRE(bool(json));json<<metadata.dump(2);
        std::ofstream source((dir/(stem+".gray")).string(),std::ios::binary);source.write(reinterpret_cast<const char*>(pixels.data()),pixels.size());
        // This harness writes a decoder-only archive. Application export_print
        // remains blocked: empty motion templates cannot drive a printer.
        Zipper zip((dir/(stem+(rgb?".rgb.nanodlp":".nanodlp"))).string());
        for(const auto &name:{"meta","slicer","plate","profile","info"}) {
            zip.add_entry(std::string(name)+".json");zip<<metadata[name].dump(2);
        }
        for(size_t i=0;i<archive.m_layers.size();++i) {
            const auto view=nanodlp::inspect_packet(archive.m_layers[i]);
            zip.add_entry(std::to_string(i+1)+".png",view.png,view.png_size);
        }
        zip.finalize();
    }
}


TEST_CASE("Native FDG writes full headers previews motion and encrypted grayscale", "[sla_reference][sla_fdg]") {
    const bool encrypted=GENERATE(false,true),portrait=GENERATE(false,true);
    SLAFullPrintConfig full;Test::configure_archive_fixture(full,"phz");
    full.set("sla_archive_format","sl1");full.set("display_pixels_x",1024);full.set("display_pixels_y",256);
    full.display_orientation.value=portrait?sladoPortrait:sladoLandscape;
    full.set("printer_model","Voxelab Proxima");full.set("max_print_height",220.);
    full.set("sla_bottom_layers",1);full.set("sla_transition_layers",1);
    full.set("sla_wait_before_cure",1.25);full.set("sla_bottom_wait_before_cure",2.5);
    full.set("sla_light_pwm",255);full.set("sla_bottom_light_pwm",255);
    full.set("sla_lift_height",6.);full.set("sla_bottom_lift_height",4.);
    full.set("sla_lift_speed",120.);full.set("sla_bottom_lift_speed",60.);
    full.set("sla_retract_speed",100.);full.set("sla_bottom_retract_speed",100.);
    full.set("material_density",1.24);full.set("bottle_cost",25.);full.set("bottle_volume",1000.);
    DynamicPrintConfig cfg;cfg.apply(full);
    const size_t width=portrait?256:1024,height=portrait?1024:256;
    std::vector<uint8_t> pixels(width*height,255);std::vector<sla::EncodedRaster> rasters;
    rasters.push_back(sla::PNGRasterEncoder{}(pixels.data(),width,height,1));
    std::fill(pixels.begin(),pixels.end(),0);rasters.push_back(sla::PNGRasterEncoder{}(pixels.data(),width,height,1));
    for(size_t y=0;y<height;++y)std::fill(pixels.begin()+y*width,pixels.begin()+(y+1)*width,uint8_t(y));
    const auto gray=sla::PNGRasterEncoder{}(pixels.data(),width,height,1);rasters.push_back(gray);rasters.push_back(gray);
    const auto document=sla::LayerDocument::from_rasters(std::move(rasters));
    auto print=SLAPrint::create_raster_job(cfg,document,{.05,.13,.2,.3});
    const char *output=std::getenv("SLA_FDG_FIXTURES");
    struct Directory {boost::filesystem::path path;bool keep;~Directory(){if(!keep){boost::system::error_code e;boost::filesystem::remove_all(path,e);}}}
        dir{output?boost::filesystem::path(output):boost::filesystem::unique_path("fdg-%%%%"),bool(output)};
    boost::filesystem::create_directories(dir.path);
    const auto stem=std::string(portrait?"portrait":"landscape")+(encrypted?"-encrypted":"-plain");
    const auto path=dir.path/(stem+".fdg");
    ThumbnailData red;red.set(1,1);red.pixels={255,0,0,255};
    FDGArchive archive(print->printer_config(),encrypted?UINT32_MAX:0);
    archive.encode_layers(document);archive.export_print(path.string(),*print,{red},"FDG");
    std::ifstream input(path.string(),std::ios::binary);const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(input),{}};
    auto u32=[&](size_t at){REQUIRE(at+4<=bytes.size());return uint32_t(bytes[at])|uint32_t(bytes[at+1])<<8|uint32_t(bytes[at+2])<<16|uint32_t(bytes[at+3])<<24;};
    auto scalar=[&](size_t at){const uint32_t bits=u32(at);float value;std::memcpy(&value,&bits,4);return value;};
    REQUIRE(u32(0)==0xBD3C7AC8);REQUIRE(u32(4)==2);REQUIRE(u32(8)==4);
    REQUIRE(u32(12)==1);REQUIRE(u32(20)==1);REQUIRE(u32(24)==width);REQUIRE(u32(28)==height);
    REQUIRE(u32(92)==(encrypted?UINT32_MAX:0));REQUIRE(u32(96)==16);REQUIRE(u32(100)==0x4c);
    REQUIRE(bytes[64]==255);REQUIRE(bytes[66]==255);
    REQUIRE(scalar(80)==(portrait?60:100));REQUIRE(scalar(84)==(portrait?100:60));REQUIRE(scalar(88)==220);
    REQUIRE(scalar(136)==4);REQUIRE(scalar(140)==60);REQUIRE(scalar(144)==6);REQUIRE(scalar(148)==120);REQUIRE(scalar(152)==100);
    REQUIRE(scalar(124)==8.9f);REQUIRE(scalar(128)==7.85f);
    for(const auto offset:{u32(44),u32(48)}) {
        REQUIRE(offset>=216);REQUIRE(u32(offset+8)==offset+32);
        REQUIRE(uint64_t(u32(offset+8))+u32(offset+12)<=bytes.size());
    }
    const auto table=u32(52);REQUIRE(table>=216);
    REQUIRE((u32(table+2*36+12)==u32(table+3*36+12))==!encrypted);
    for(size_t i=0;i<4;++i) {
        const auto entry=table+i*36;const uint64_t address=uint64_t(u32(entry+20))*0x100000000ULL+u32(entry+12);
        REQUIRE(u32(entry+24)==84);REQUIRE(address+u32(entry+16)<=bytes.size());
        std::vector<uint8_t> decoded(bytes.begin()+address,bytes.begin()+address+u32(entry+16));
        FDGArchive::crypt_mask(decoded,u32(92),uint32_t(i));
        const auto image=document.image(i);const auto expected=PhrozenPHZArchive::encode_mask(image.buf.data(),image.cols,image.rows,1);
        REQUIRE(decoded.size()==expected.size());REQUIRE(std::memcmp(decoded.data(),expected.data(),decoded.size())==0);
    }
    // UVJ retains variable Z; SL1 cannot represent this fixture's layer heights.
    if(output)print->export_layer_document(document,"uvj",(dir.path/(stem+".uvj")).string(),{},"FDG source");
    auto bad=cfg;bad.set("sla_lift_height2",1.);
    auto invalid=SLAPrint::create_raster_job(bad,document,{.05,.13,.2,.3});
    REQUIRE_THROWS(archive.export_print(path.string(),*invalid,{},"invalid"));
    std::ifstream unchanged(path.string(),std::ios::binary);REQUIRE(std::vector<uint8_t>{std::istreambuf_iterator<char>(unchanged),{}}==bytes);
    bad=cfg;bad.set("display_mirror_y",true);
    invalid=SLAPrint::create_raster_job(bad,document,{.05,.13,.2,.3});
    REQUIRE_THROWS(FDGArchive::validate_configuration(*invalid));
    bad=cfg;bad.set("material_density",0.);
    invalid=SLAPrint::create_raster_job(bad,document,{.05,.13,.2,.3});
    REQUIRE_NOTHROW(FDGArchive::validate_configuration(*invalid));
}

TEST_CASE("FDG cipher matches a fixed original UVtools overflow vector", "[sla_reference][sla_fdg]") {
    std::vector<uint8_t> bytes(19);std::iota(bytes.begin(),bytes.end(),uint8_t(0));
    const std::vector<uint8_t> original={0xc5,0xc7,0x3c,0x11,0xd6,0x76,0x8f,0xde,0xd7,0x29,0xde,0xab,0xe0,0xc0,0x10,0x67,0xe9,0x6b,0x7b};
    FDGArchive::crypt_mask(bytes,UINT32_MAX,0);REQUIRE(bytes==original);
}

TEST_CASE("Legacy CBT presets resolve to explicit CTB v1 with grayscale layers", "[sla_reference][sla_ctb_v1]") {
    const bool portrait=GENERATE(false,true);
    const auto *entry=get_archive_entry("cbt");REQUIRE(entry!=nullptr);REQUIRE(std::string(entry->id)=="ctb_v1");
    REQUIRE(std::string(get_default_extension("cbt"))=="v1.ctb");
    REQUIRE(get_archive_entry_by_extension("cbt")==nullptr);
    DynamicPrintConfig legacy;legacy.set_deserialize_strict("sla_archive_format","CBT");
    REQUIRE(legacy.opt_string("sla_archive_format")=="ctb_v1");
    SLAFullPrintConfig full;Test::configure_archive_fixture(full,"ctb_v1");
    full.set("display_pixels_x",512);full.set("display_pixels_y",256);
    full.display_orientation.value=portrait?sladoPortrait:sladoLandscape;
    full.set("sla_bottom_layers",1);full.set("sla_transition_layers",1);
    full.set("sla_wait_before_cure",1.25);full.set("sla_bottom_wait_before_cure",2.5);
    full.set("sla_light_pwm",200);full.set("sla_bottom_light_pwm",160);
    DynamicPrintConfig cfg;cfg.apply(full);
    const size_t width=portrait?256:512,height=portrait?512:256;
    std::vector<uint8_t> pixels(width*height,255);std::vector<sla::EncodedRaster> rasters;
    rasters.push_back(sla::PNGRasterEncoder{}(pixels.data(),width,height,1));
    std::fill(pixels.begin(),pixels.end(),0);rasters.push_back(sla::PNGRasterEncoder{}(pixels.data(),width,height,1));
    for(size_t y=0;y<height;++y)std::fill(pixels.begin()+y*width,pixels.begin()+(y+1)*width,uint8_t(y));
    const auto gray=sla::PNGRasterEncoder{}(pixels.data(),width,height,1);rasters.push_back(gray);rasters.push_back(gray);
    const auto document=sla::LayerDocument::from_rasters(std::move(rasters));
    const auto print=SLAPrint::create_raster_job(cfg,document,{.05,.13,.2,.3});
    const char *output=std::getenv("SLA_CTB_V1_FIXTURES");
    struct Directory {boost::filesystem::path path;bool keep;~Directory(){if(!keep){boost::system::error_code e;boost::filesystem::remove_all(path,e);}}}
        dir{output?boost::filesystem::path(output):boost::filesystem::unique_path("ctb-v1-%%%%"),bool(output)};
    boost::filesystem::create_directories(dir.path);
    const auto stem=std::string(portrait?"portrait":"landscape");const auto path=dir.path/(stem+".v1.ctb");
    print->export_layer_document(document,"ctb_v1",path.string(),{},"CTB v1");
    std::ifstream input(path.string(),std::ios::binary);const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(input),{}};
    auto u32=[&](size_t at){REQUIRE(at+4<=bytes.size());return uint32_t(bytes[at])|uint32_t(bytes[at+1])<<8|uint32_t(bytes[at+2])<<16|uint32_t(bytes[at+3])<<24;};
    REQUIRE(u32(0)==0x12fd0086);REQUIRE(u32(4)==1);REQUIRE(u32(84)==0);REQUIRE(u32(88)==60);
    REQUIRE(u32(60)==112);REQUIRE(u32(64)==u32(72)+32+u32(u32(72)+12));
    REQUIRE(u32(68)==4);REQUIRE(u32(92)==1);
    REQUIRE(u32(52)==width);REQUIRE(u32(56)==height);
    REQUIRE(bytes[96]==200);REQUIRE(bytes[98]==160);
    for(size_t i=0;i<4;++i) {
        const size_t at=u32(64)+36*i;const uint64_t address=uint64_t(u32(at+20))*0x100000000ULL+u32(at+12);
        const auto image=document.image(i);const auto expected=UVToolsChituboxArchive(print->printer_config()).get_encoder()(image.buf.data(),image.cols,image.rows,1);
        REQUIRE(u32(at+16)==expected.size());REQUIRE(address+expected.size()<=bytes.size());
        REQUIRE(std::memcmp(bytes.data()+address,expected.data(),expected.size())==0);
    }
    if(output)print->export_layer_document(document,"uvj",(dir.path/(stem+".uvj")).string(),{},"CTB v1 source");
    auto bad=cfg;bad.set("sla_lift_height",6.);
    REQUIRE_THROWS(SLAPrint::create_raster_job(bad,document,{.05,.13,.2,.3}));
    bad.set("sla_archive_format","uvj");
    const auto source=SLAPrint::create_raster_job(bad,document,{.05,.13,.2,.3});
    REQUIRE_THROWS(source->export_layer_document(document,"ctb_v1",path.string(),{},"unsupported motion"));
    std::ifstream unchanged(path.string(),std::ios::binary);REQUIRE(std::vector<uint8_t>{std::istreambuf_iterator<char>(unchanged),{}}==bytes);
}

TEST_CASE("Encrypted CTB protocol crypto preserves blocks and checksum framing", "[sla_reference][ctb_encrypted_crypto]") {
    nlohmann::json vectors=nlohmann::json::array();
    auto hex=[](const auto &bytes){std::string text;const char *digits="0123456789abcdef";for(uint8_t byte:bytes){text+=digits[byte>>4];text+=digits[byte&15];}return text;};
    uint64_t random=0x123456789abcdef0ULL;
    for(unsigned n=0;n<256;++n) {
        random^=random<<13;random^=random>>7;random^=random<<17;
        const uint64_t checksum=n==0?0:n==1?1:n==2?0xCAFEBABEULL:n==3?UINT64_MAX:random;
        const size_t length=n<7?std::array<size_t,7>{0,16,32,48,288,512,1024}[n]:16*(1+n%64);
        std::vector<uint8_t> plain(length);
        for(size_t i=0;i<length;++i){random^=random<<13;random^=random>>7;random^=random<<17;plain[i]=uint8_t(random);}
        if(n==8)std::fill(plain.begin(),plain.end(),0);
        if(n==9)std::fill(plain.begin(),plain.end(),255);
        const auto encrypted=ctb_encrypted::aes_cbc(plain,true);
        const auto decoded=ctb_encrypted::aes_cbc(encrypted,false);
        REQUIRE(decoded==plain);REQUIRE(encrypted.size()==plain.size());
        const auto digest=ctb_encrypted::checksum_digest(checksum);
        const auto signature=ctb_encrypted::signature(checksum);
        REQUIRE(ctb_encrypted::aes_cbc(signature,false)==std::vector<uint8_t>{digest.begin(),digest.end()});
        const uint32_t seed=n<4?std::array<uint32_t,4>{0,1,0xEFBEADDE,UINT32_MAX}[n]:uint32_t(random);
        const uint32_t index=n<4?std::array<uint32_t,4>{0,1,12345,UINT32_MAX}[n]:uint32_t(random>>32);
        auto xored=plain;ctb_encrypted::xor_layer(xored,index,seed);
        auto restored=xored;ctb_encrypted::xor_layer(restored,index,seed);REQUIRE(restored==plain);
        vectors.push_back({{"name",n},{"checksum",checksum},{"seed",seed},{"index",index},{"plaintext",hex(plain)},
            {"encrypted",hex(encrypted)},{"decrypted",hex(decoded)},{"digest",hex(digest)},{"signature",hex(signature)},{"xor",hex(xored)}});
    }
    REQUIRE_THROWS(ctb_encrypted::aes_cbc(std::vector<uint8_t>(15),true));
    REQUIRE_THROWS(ctb_encrypted::aes_cbc(std::vector<uint8_t>(17),false));
    if(const char *output=std::getenv("SLA_CTB_ENCRYPTED_CRYPTO_FIXTURES")) {
        std::ofstream out(output);out<<vectors.dump();REQUIRE(bool(out));
    }
}

TEST_CASE("Native encrypted CTB v4 and v5 retain masks and independent two-stage settings", "[sla_reference][ctb_encrypted_file]") {
    const bool portrait=GENERATE(false,true),unicode=GENERATE(false,true);
    const uint32_t version=GENERATE(4u,5u);
    SLAFullPrintConfig full;Test::configure_archive_fixture(full,"uvj");
    full.set("display_pixels_x",512);full.set("display_pixels_y",256);
    full.display_orientation.value=portrait?sladoPortrait:sladoLandscape;
    full.set("display_mirror_x",true);full.set("display_mirror_y",false);
    full.set("max_print_height",220.);full.set("printer_model",unicode?"Żywica test model":"Voxelab Proxima");
    full.set("material_density",1.25);full.set("bottle_cost",24.5);full.set("bottle_volume",500.);
    full.set("sla_bottom_layers",1);full.set("sla_transition_layers",1);
    full.set("sla_lift_height",3.);full.set("sla_lift_height2",2.);
    full.set("sla_bottom_lift_height",2.);full.set("sla_bottom_lift_height2",1.);
    full.set("sla_lift_speed",120.);full.set("sla_lift_speed2",80.);
    full.set("sla_bottom_lift_speed",60.);full.set("sla_bottom_lift_speed2",90.);
    full.set("sla_retract_height2",1.);full.set("sla_bottom_retract_height2",.5);
    full.set("sla_retract_speed",180.);full.set("sla_retract_speed2",65.);
    full.set("sla_bottom_retract_speed",160.);full.set("sla_bottom_retract_speed2",75.);
    full.set("sla_wait_before_cure",1.25);full.set("sla_bottom_wait_before_cure",2.5);
    full.set("sla_wait_after_cure",.5);full.set("sla_bottom_wait_after_cure",1.5);
    full.set("sla_wait_after_lift",.75);full.set("sla_bottom_wait_after_lift",1.75);
    full.set("sla_light_pwm",200);full.set("sla_bottom_light_pwm",160);
    DynamicPrintConfig cfg;cfg.apply(full);
    cfg.set_key_value("material_type",new ConfigOptionString(unicode?"Żywica & tough":"Tough"));
    cfg.set_key_value("sla_material_settings_id",new ConfigOptionString(unicode?"Żywica material":"Test material"));
    const size_t width=portrait?256:512,height=portrait?512:256;
    std::vector<uint8_t> pixels(width*height,255);std::vector<sla::EncodedRaster> rasters;
    rasters.push_back(sla::PNGRasterEncoder{}(pixels.data(),width,height,1));
    std::fill(pixels.begin(),pixels.end(),0);rasters.push_back(sla::PNGRasterEncoder{}(pixels.data(),width,height,1));
    for(size_t i=0;i<pixels.size();++i)pixels[i]=uint8_t(i%256);
    const auto gray=sla::PNGRasterEncoder{}(pixels.data(),width,height,1);rasters.push_back(gray);rasters.push_back(gray);
    const auto document=sla::LayerDocument::from_rasters(std::move(rasters));
    const auto print=SLAPrint::create_raster_job(cfg,document,{.05,.13,.2,.3});
    CTBEncryptedArchive archive(print->printer_config(),version);archive.encode_layers(document);
    const char *output=std::getenv("SLA_CTB_ENCRYPTED_FILE_FIXTURES");
    struct Directory {boost::filesystem::path path;bool keep;~Directory(){if(!keep){boost::system::error_code e;boost::filesystem::remove_all(path,e);}}}
        dir{output?boost::filesystem::path(output):boost::filesystem::unique_path("ctb-encrypted-%%%%"),bool(output)};
    boost::filesystem::create_directories(dir.path);
    const auto stem=std::string(portrait?"portrait":"landscape")+"-v"+std::to_string(version)+(unicode?"-utf8":"-ascii");
    const auto path=dir.path/(stem+".encrypted.ctb");archive.export_print(path.string(),*print,{},"native");
    std::ifstream in(path.string(),std::ios::binary);const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(in),{}};in.close();
    auto u32=[&](size_t at){REQUIRE(at+4<=bytes.size());return uint32_t(bytes[at])|uint32_t(bytes[at+1])<<8|uint32_t(bytes[at+2])<<16|uint32_t(bytes[at+3])<<24;};
    REQUIRE(u32(0)==0x12FD0107);REQUIRE(u32(4)==288);REQUIRE(u32(8)==48);REQUIRE(u32(16)==version);
    const auto settings=ctb_encrypted::aes_cbc(std::vector<uint8_t>(bytes.begin()+48,bytes.begin()+336),false);
    auto field=[&](size_t at){return uint32_t(settings[at])|uint32_t(settings[at+1])<<8|uint32_t(settings[at+2])<<16|uint32_t(settings[at+3])<<24;};
    REQUIRE(field(0)==0xCAFEBABE);REQUIRE(field(4)==0);REQUIRE(field(56)==width);REQUIRE(field(60)==height);REQUIRE(field(64)==4);
    REQUIRE(settings[171]==0x40);REQUIRE(field(128)==0xEFBEADDE);REQUIRE((field(276)>0)==(version==5));
    REQUIRE(u32(20)==32);const auto signature=ctb_encrypted::signature(0xCAFEBABE);
    REQUIRE(u32(24)+32+4==bytes.size());REQUIRE(std::equal(signature.begin(),signature.end(),bytes.begin()+u32(24)));
    for(size_t i=0;i<4;++i) {
        const size_t pointer=field(8)+16*i;const uint64_t address=uint64_t(u32(pointer))+uint64_t(u32(pointer+4))*0x100000000ULL;
        REQUIRE(u32(pointer+8)==88);REQUIRE(u32(address)==88);REQUIRE(u32(address+16)==address+88);
        REQUIRE(u32(address+32)==0);REQUIRE(u32(address+36)==0);
        auto encrypted=std::vector<uint8_t>(bytes.begin()+address+88,bytes.begin()+address+88+u32(address+24));
        ctb_encrypted::xor_layer(encrypted,uint32_t(i));
        const auto image=document.image(i);const auto expected=chitubox::Gray7RasterEncoder{}(image.buf.data(),width,height,1);
        REQUIRE(encrypted.size()==expected.size());REQUIRE(std::memcmp(encrypted.data(),expected.data(),expected.size())==0);
    }
    if(output)print->export_layer_document(document,"uvj",(dir.path/(stem+".uvj")).string(),{},"source");
    auto bad=cfg;bad.set("display_mirror_y",true);
    auto invalid=SLAPrint::create_raster_job(bad,document,{.05,.13,.2,.3});
    REQUIRE_THROWS(archive.export_print(path.string(),*invalid,{},"invalid"));
    bad=cfg;bad.set("sla_retract_height2",6.);
    REQUIRE_THROWS(SLAPrint::create_raster_job(bad,document,{.05,.13,.2,.3}));
    SLAPrint invalid_motion;invalid_motion.apply(Model{},bad);
    REQUIRE_THROWS(CTBEncryptedArchive::validate_configuration(invalid_motion));
    std::ifstream preserved(path.string(),std::ios::binary);REQUIRE(std::vector<uint8_t>{std::istreambuf_iterator<char>(preserved),{}}==bytes);
}

TEST_CASE("NanoDLP explicit device profile writer preserves all manifest and PNG entries", "[sla_reference][nanodlp_profile_file]")
{
    const bool portrait = GENERATE(false, true), rgb = GENERATE(false, true);
    const unsigned version = GENERATE(1u, 2u);
    const int columns = GENERATE(156, 157);
    SLAFullPrintConfig full;
    Test::configure_archive_fixture(full, "uvj");
    full.set("display_pixels_x", columns); full.set("display_pixels_y", 24);
    full.set("display_width", columns * .1); full.set("display_height", 2.4);
    full.display_orientation.value = portrait ? sladoPortrait : sladoLandscape;
    full.set("display_mirror_x", true); full.set("display_mirror_y", false);
    full.set("printer_model", "NanoDLP urządzenie");
    full.set("sla_bottom_layers", 1); full.set("sla_transition_layers", 0);
    full.set("exposure_time", 3.);
    for (const bool bottom : {false, true}) {
        const std::string prefix = bottom ? "sla_bottom_" : "sla_";
        full.set(prefix + "light_pwm", 180);
        full.set(prefix + "wait_before_cure", bottom ? 2.5 : 1.25);
        full.set(prefix + "wait_after_cure", .5); full.set(prefix + "wait_after_lift", .75);
        full.set(prefix + "lift_height", bottom ? 2. : 3.);
    }
    DynamicPrintConfig config; config.apply(full);
    const size_t width = portrait ? 24 : columns, height = portrait ? columns : 24;
    std::vector<uint8_t> pixels(width * height);
    for (size_t index = 0; index < pixels.size(); ++index)
        pixels[index] = uint8_t(index % 256);
    const auto gray = sla::PNGRasterEncoder{}(pixels.data(), width, height, 1);
    std::fill(pixels.begin(), pixels.end(), 0);
    const auto black = sla::PNGRasterEncoder{}(pixels.data(), width, height, 1);
    const auto document = sla::LayerDocument::from_rasters({gray, gray, black});
    const auto print = SLAPrint::create_raster_job(config, document, {.05, .1, .15});
    NanoDLPArchive writer(print->printer_config(), rgb); writer.encode_layers(document);
    auto device = nanodlp::schema_defaults()["profile"];
    device["CustomValues"]["NativeTestValue"] = "Żywica & <test>";
    // Deliberately stale global values must yield to the current job; controller
    // templates and custom values must remain byte-for-byte unchanged.
    device["CureTime"] = 99; device["WaitHeight"] = 99; device["Title"] = "stale";
    ThumbnailData thumbnail; thumbnail.width = 2; thumbnail.height = 2;
    thumbnail.pixels = {0,0,255,255, 255,255,255,128, 255,0,0,255, 0,255,0,255};
    const char *output = std::getenv("SLA_NANODLP_PROFILE_FIXTURES");
    struct Directory {
        boost::filesystem::path path; bool keep;
        ~Directory() { if (!keep) { boost::system::error_code error; boost::filesystem::remove_all(path,error); } }
    } dir{output ? boost::filesystem::path(output) : boost::filesystem::unique_path("nano-profile-%%%%"), bool(output)};
    boost::filesystem::create_directories(dir.path);
    const auto stem = std::to_string(columns) + (portrait ? "-portrait" : "-landscape") +
        (rgb ? "-rgb" : "-mono") + "-v" + std::to_string(version);
    const auto path = dir.path / (stem + (rgb ? ".rgb.nanodlp" : ".nanodlp"));
    writer.export_with_profile(path.string(), *print, {thumbnail}, device, version, "Żywica <projekt>");
    REQUIRE(boost::filesystem::is_regular_file(path));
    const auto read_entries = [](const std::string &filename) {
        std::map<std::string, std::vector<uint8_t>> result;
        for (const auto &entry : read_zipper_archive(filename, {""}, {}).entries)
            result.emplace(entry.fname, entry.buf);
        return result;
    };
    const auto zip = read_entries(path.string());
    REQUIRE(zip.size() == 9);
    for (const char *entry : {"meta.json","slicer.json","plate.json","profile.json","info.json","3d.png","1.png","2.png","3.png"})
        REQUIRE(zip.count(entry) == 1);
    const auto parse = [&](const char *key) { return nlohmann::json::parse(zip.at(key)); };
    REQUIRE(parse("meta.json")["FormatVersion"] == version);
    REQUIRE(parse("meta.json")["Program"] == "Sla-SlicerOS");
    REQUIRE(parse("profile.json")["ShieldBeforeLayer"] == device["ShieldBeforeLayer"]);
    REQUIRE(parse("profile.json")["ShieldAfterLayer"] == device["ShieldAfterLayer"]);
    REQUIRE(parse("profile.json")["CustomValues"]["NativeTestValue"] == "Żywica & <test>");
    REQUIRE(parse("profile.json")["CureTime"] == 3);
    REQUIRE(parse("profile.json")["WaitHeight"] == 3);
    REQUIRE(parse("profile.json")["Title"] == "NanoDLP urządzenie");
    REQUIRE(parse("slicer.json")["PreviewWidth"] == 607);
    REQUIRE(parse("slicer.json")["PreviewHeight"] == 487);
    REQUIRE(zip.at("1.png") == zip.at("2.png"));
    auto existing = zip;
    for (const auto &invalid : {nlohmann::json{}, nlohmann::json::object(),
                               nlohmann::json{{"ShieldBeforeLayer", " "}, {"ShieldAfterLayer", "UVLED_OFF"}}})
        REQUIRE_THROWS(writer.export_with_profile(path.string(), *print, {}, invalid));
    auto invalid = device; invalid["NotAProfileField"] = "unknown";
    REQUIRE_THROWS(writer.export_with_profile(path.string(), *print, {}, invalid));
    invalid = device; invalid["CustomValues"]["NativeTestValue"] = 5;
    REQUIRE_THROWS(writer.export_with_profile(path.string(), *print, {}, invalid));
    invalid = device; invalid["ShieldBeforeLayer"] = std::string(1, char(0xff));
    REQUIRE_THROWS(writer.export_with_profile(path.string(), *print, {}, invalid));
    for (const auto &bad : {nlohmann::json(-1), nlohmann::json(1.5), nlohmann::json(1.0), nlohmann::json(uint64_t(UINT32_MAX)+1)}) {
        invalid = device; invalid["ZStepWait"] = bad;
        REQUIRE_THROWS(writer.export_with_profile(path.string(), *print, {}, invalid));
    }
    invalid = device; invalid["Updated"] = uint64_t(INT64_MAX)+1;
    REQUIRE_THROWS(writer.export_with_profile(path.string(), *print, {}, invalid));
    REQUIRE_THROWS(writer.export_with_profile(path.string(), *print, {}, device, 3));
    REQUIRE(read_entries(path.string()) == existing);
    auto variable = SLAPrint::create_raster_job(config, document, {.05, .13, .2});
    REQUIRE_THROWS(writer.export_with_profile(path.string(), *variable, {}, device));
    for (const auto &bad : {std::pair{"sla_bottom_light_pwm",160.}, {"sla_wait_before_cure",1.251},
                           {"initial_layer_height",.04}}) {
        auto changed = config;
        if (std::string(bad.first) == "sla_bottom_light_pwm") changed.set(bad.first,int(bad.second));
        else changed.set(bad.first,bad.second);
        INFO(bad.first);
        const std::vector<double> levels = std::string(bad.first) == "initial_layer_height" ?
            std::vector<double>{.04,.09,.14} : std::vector<double>{.05,.1,.15};
        auto job = SLAPrint::create_raster_job(changed,document,levels);
        REQUIRE_THROWS(writer.export_with_profile(path.string(), *job, {}, device));
    }
    REQUIRE(read_entries(path.string()) == existing);
    REQUIRE(archive_export_unavailable_reason("nanodlp").empty());
    if (output) {
        const auto source = dir.path / (stem + ".uvj");
        print->export_layer_document(document, "uvj", source.string(), {}, "source");
        std::ofstream profile((dir.path / (stem + "-device.json")).string());
        profile << device.dump(2);
    }
}

TEST_CASE("NanoDLP printer profile is portable and drives all registered variants", "[sla_reference][nanodlp_printer_profile]")
{
    const std::string format = GENERATE("nanodlp", "nanodlp_v1", "nanodlp_rgb", "nanodlp_rgb_v1");
    SLAFullPrintConfig full; Test::configure_archive_fixture(full,format);
    full.set("display_pixels_x",128);full.set("display_pixels_y",24);
    full.set("display_width",12.8);full.set("display_height",2.4);
    full.set("sla_bottom_layers",1);
    auto device = NanoDLPArchive::parse_device_profile(full.nanodlp_device_profile.value);
    device["CustomValues"]["PresetProof"] = "Żywica \"<&>\"";
    full.nanodlp_device_profile.value = device.dump(2);
    DynamicPrintConfig cfg;cfg.apply(full);
    struct Directory {
        boost::filesystem::path path;
        ~Directory(){boost::system::error_code e;boost::filesystem::remove_all(path,e);}
    } dir{boost::filesystem::unique_path("nano-preset-%%%%")};
    boost::filesystem::create_directories(dir.path);
    const auto saved = dir.path / "printer.ini";cfg.save(saved.string());
    DynamicPrintConfig restored;restored.load(saved.string(),ForwardCompatibilitySubstitutionRule::Disable);
    REQUIRE(restored.opt_string("nanodlp_device_profile") == cfg.opt_string("nanodlp_device_profile"));
    REQUIRE(restored.opt_string("sla_archive_format") == format);
    REQUIRE(std::find(Preset::sla_printer_options().begin(),Preset::sla_printer_options().end(),"nanodlp_device_profile") != Preset::sla_printer_options().end());
    std::vector<uint8_t> pixels(128*24,0);pixels[123]=1;pixels[444]=128;pixels[789]=255;
    auto raster = sla::PNGRasterEncoder{}(pixels.data(),128,24,1);
    auto document = sla::LayerDocument::from_rasters({raster,raster,raster});
    const auto job = SLAPrint::create_raster_job(restored,document,{.05,.1,.15});
    REQUIRE(job->validate().empty());
    const auto filename = dir.path / ("result." + std::string(get_default_extension(format.c_str())));
    REQUIRE_NOTHROW(job->export_layer_document(document,format,filename.string(),{},"preset"));
    const auto zip = read_zipper_archive(filename.string(),{""},{});
    auto read = [&](const std::string &name) {
        const auto entry = std::find_if(zip.entries.begin(),zip.entries.end(),[&](const auto &e){return e.fname==name;});
        REQUIRE(entry!=zip.entries.end());return nlohmann::json::parse(entry->buf);
    };
    REQUIRE(read("meta.json")["FormatVersion"] == (format.find("_v1") != std::string::npos ? 1 : 2));
    REQUIRE(read("slicer.json")["DisplayController"] == (format.find("_rgb") != std::string::npos ? 0 : 1));
    const auto profile = read("profile.json");
    REQUIRE(profile["CustomValues"]["PresetProof"] == device["CustomValues"]["PresetProof"]);
    REQUIRE(profile["ShieldBeforeLayer"] == device["ShieldBeforeLayer"]);
    REQUIRE(profile["ShieldAfterLayer"] == device["ShieldAfterLayer"]);
    const auto compound = dir.path / "explicit.v1.rgb.nanodlp";
    REQUIRE_NOTHROW(job->export_print(compound.string(),{},"compound"));
    const auto shared = dir.path / "selected.nanodlp";
    REQUIRE_NOTHROW(job->export_print(shared.string(),{},"shared"));
    auto manifest = [](const boost::filesystem::path &path,const std::string &name) {
        auto archive=read_zipper_archive(path.string(),{""},{});
        auto entry=std::find_if(archive.entries.begin(),archive.entries.end(),[&](const auto &e){return e.fname==name;});
        REQUIRE(entry!=archive.entries.end());return nlohmann::json::parse(entry->buf);
    };
    REQUIRE(manifest(compound,"meta.json")["FormatVersion"] == 1);
    REQUIRE(manifest(compound,"slicer.json")["DisplayController"] == 0);
    REQUIRE(manifest(shared,"meta.json")["FormatVersion"] == (format.find("_v1") != std::string::npos ? 1 : 2));
    REQUIRE(manifest(shared,"slicer.json")["DisplayController"] == (format.find("_rgb") != std::string::npos ? 0 : 1));
    REQUIRE_THROWS(NanoDLPArchive::parse_device_profile("{bad json"));
    REQUIRE_THROWS(NanoDLPArchive::parse_device_profile("{}"));
    auto second = cfg;auto other = device;other["CustomValues"]["PresetProof"]="second printer";
    second.set("nanodlp_device_profile",other.dump());
    const auto second_path=dir.path/"second-printer.ini";second.save(second_path.string());
    DynamicPrintConfig reloaded;reloaded.load(saved.string(),ForwardCompatibilitySubstitutionRule::Disable);
    REQUIRE(reloaded.opt_string("nanodlp_device_profile") == cfg.opt_string("nanodlp_device_profile"));
    for (const std::string value : {"", "{}", "{bad json"}) {
        auto invalid=cfg;invalid.set("nanodlp_device_profile",value);
        SLAPrint print;print.apply(Model{},invalid);
        REQUIRE_FALSE(print.validate().empty());
        REQUIRE_THROWS(SLAPrint::create_raster_job(invalid,document,{.05,.1,.15}));
    }
}

TEST_CASE("Ordinary CTB v3-v5 and GKtwo preserve their own tables and raster payloads", "[sla_reference][ctb_modern_file]") {
    const bool portrait=GENERATE(false,true),unicode=GENERATE(false,true);
    const std::string format=GENERATE("ctb_v3","ctb_v4","ctb_v5","ctb_gktwo");
    const unsigned version=format=="ctb_v3"?3:format=="ctb_v5"?5:4;
    SLAFullPrintConfig full;Test::configure_archive_fixture(full,"uvj");
    full.set("display_pixels_x",512);full.set("display_pixels_y",256);
    full.display_orientation.value=portrait?sladoPortrait:sladoLandscape;
    full.set("display_mirror_x",true);full.set("display_mirror_y",false);
    full.set("max_print_height",220.);full.set("printer_model",unicode?"Żywica test model":"Voxelab Proxima");
    full.set("material_density",1.25);full.set("bottle_cost",24.5);full.set("bottle_volume",500.);
    full.set("sla_bottom_layers",1);full.set("sla_transition_layers",1);
    full.set("sla_lift_height",3.);full.set("sla_lift_height2",2.);
    full.set("sla_bottom_lift_height",2.);full.set("sla_bottom_lift_height2",1.);
    full.set("sla_lift_speed",120.);full.set("sla_lift_speed2",80.);
    full.set("sla_bottom_lift_speed",60.);full.set("sla_bottom_lift_speed2",90.);
    full.set("sla_retract_height2",1.);full.set("sla_bottom_retract_height2",.5);
    full.set("sla_retract_speed",180.);full.set("sla_retract_speed2",65.);
    full.set("sla_bottom_retract_speed",160.);full.set("sla_bottom_retract_speed2",75.);
    full.set("sla_wait_before_cure",1.25);full.set("sla_bottom_wait_before_cure",2.5);
    full.set("sla_wait_after_cure",.5);full.set("sla_bottom_wait_after_cure",1.5);
    full.set("sla_wait_after_lift",.75);full.set("sla_bottom_wait_after_lift",1.75);
    full.set("sla_light_pwm",200);full.set("sla_bottom_light_pwm",160);
    full.set("sla_archive_format",format);
    if(version==3) {
        for(const char *key:{"sla_lift_height2","sla_bottom_lift_height2","sla_retract_height2","sla_bottom_retract_height2",
            "sla_wait_after_cure","sla_bottom_wait_after_cure","sla_wait_after_lift","sla_bottom_wait_after_lift"})full.set(key,0.);
    }
    DynamicPrintConfig cfg;cfg.apply(full);
    cfg.set_key_value("material_type",new ConfigOptionString(unicode?"Żywica & tough":"Tough"));
    cfg.set_key_value("sla_material_settings_id",new ConfigOptionString(unicode?"Żywica material":"Test material"));
    const size_t width=portrait?256:512,height=portrait?512:256;
    std::vector<uint8_t> pixels(width*height,255);std::vector<sla::EncodedRaster> rasters;
    rasters.push_back(sla::PNGRasterEncoder{}(pixels.data(),width,height,1));
    std::fill(pixels.begin(),pixels.end(),0);rasters.push_back(sla::PNGRasterEncoder{}(pixels.data(),width,height,1));
    for(size_t i=0;i<pixels.size();++i)pixels[i]=uint8_t(i%256);
    const auto gray=sla::PNGRasterEncoder{}(pixels.data(),width,height,1);rasters.push_back(gray);rasters.push_back(gray);
    const auto document=sla::LayerDocument::from_rasters(std::move(rasters));
    const auto print=SLAPrint::create_raster_job(cfg,document,{.05,.13,.2,.3});
    ChituboxCTBModernArchive archive(print->printer_config(),version,format=="ctb_gktwo");
    archive.encode_layers(document);
    const char *output=std::getenv("SLA_CTB_MODERN_FIXTURES");
    struct Directory {boost::filesystem::path path;bool keep;~Directory(){if(!keep){boost::system::error_code e;boost::filesystem::remove_all(path,e);}}}
        dir{output?boost::filesystem::path(output):boost::filesystem::unique_path("ctb-modern-%%%%"),bool(output)};
    boost::filesystem::create_directories(dir.path);
    const auto stem=std::string(portrait?"portrait":"landscape")+"-"+format+(unicode?"-utf8":"-ascii");
    const auto *entry=get_archive_entry(format.c_str());REQUIRE(entry!=nullptr);
    const auto path=dir.path/(stem+"."+entry->ext);
    archive.export_print(path.string(),*print,{},"native");
    std::ifstream in(path.string(),std::ios::binary);
    const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(in),{}};in.close();
    auto u32=[&](size_t at){REQUIRE(at+4<=bytes.size());return uint32_t(bytes[at])|uint32_t(bytes[at+1])<<8|uint32_t(bytes[at+2])<<16|uint32_t(bytes[at+3])<<24;};
    REQUIRE(u32(0)==(format=="ctb_gktwo"?0xFF220810u:version==3?0x12FD0086u:0x12FD0106u));
    REQUIRE(u32(4)==version);REQUIRE(u32(52)==width);REQUIRE(u32(56)==height);REQUIRE(u32(68)==4);
    REQUIRE(u32(88)==60);REQUIRE(u32(92)==1);REQUIRE(u32(100)==0xEFBEADDE);REQUIRE(u32(108)==76);
    const auto slicer=u32(104);REQUIRE(bytes[slicer+39]==version*0x10);
    const auto params4=u32(slicer+64);REQUIRE((params4>0)==(version>=4));
    if(version>=4) {
        REQUIRE(u32(params4+48)==5);REQUIRE(u32(params4+52)==3);
        REQUIRE(u32(params4+76)==(format=="ctb_gktwo"?323:320));
        REQUIRE((u32(params4+80)>0)==(version==5));
    }
    for(size_t i=0;i<4;++i) {
        const size_t table=u32(64)+36*i;
        const auto data=u32(table+12),size=u32(table+16);
        REQUIRE(u32(table+24)==84);REQUIRE(data>=84);REQUIRE(data+size<=bytes.size());
        const auto extended=data-84;
        REQUIRE(u32(extended+12)==data);REQUIRE(u32(extended+16)==size);
        REQUIRE(u32(extended+36)==84+size);REQUIRE(u32(extended+24)==84);
        auto encrypted=std::vector<uint8_t>(bytes.begin()+data,bytes.begin()+data+size);
        ctb_encrypted::xor_layer(encrypted,uint32_t(i));
        const auto image=document.image(i);const auto expected=chitubox::Gray7RasterEncoder{}(image.buf.data(),width,height,1);
        REQUIRE(encrypted.size()==expected.size());REQUIRE(std::memcmp(encrypted.data(),expected.data(),expected.size())==0);
    }
    if(output)print->export_layer_document(document,"uvj",(dir.path/(stem+".uvj")).string(),{},"source");
    auto bad=cfg;bad.set("display_mirror_y",true);SLAPrint invalid;invalid.apply(Model{},bad);
    REQUIRE_THROWS(ChituboxCTBModernArchive::validate_configuration(invalid,version));
    REQUIRE_THROWS(archive.export_print(path.string(),invalid,{},"invalid"));
    std::ifstream preserved(path.string(),std::ios::binary);
    REQUIRE(std::vector<uint8_t>{std::istreambuf_iterator<char>(preserved),{}}==bytes);
}

TEST_CASE("Canonical raster capture has an independent encoder and preserves native PNGs", "[sla_canonical_encoder]") {
    struct Archive : SL1Archive {
        using SL1Archive::SL1Archive;
        const auto &layers() const {return m_layers;}
    };
    SLAPrinterConfig config;config.display_width.value=1.;config.display_height.value=1.;
    config.display_pixels_x.value=2;config.display_pixels_y.value=2;
    config.display_orientation.value=sladoLandscape;config.gamma_correction.value=1.;
    Archive archive(config);std::vector<sla::EncodedRaster> masks;
    const std::vector<size_t> representatives{0,0,2,2};std::atomic<unsigned> captures{0};
    const sla::RasterEncoder capture=[&](const void *,size_t w,size_t h,size_t n) {
        ++captures;if(n!=1)throw std::runtime_error("Expected grayscale");const std::vector<uint8_t> zero(w*h,0);
        return sla::PNGRasterEncoder{}(zero.data(),w,h,1);
    };
    archive.draw_layers(4,[](sla::RasterBase &raster,size_t i) {
        if(i==0)raster.draw(ExPolygon(Polygon(Points{Point(0,0),Point(scaled(1.),0),
            Point(scaled(1.),scaled(1.)),Point(0,scaled(1.))})));
    },[]{return false;},ExecutionTBB{},&masks,&representatives,capture);
    CHECK(captures==2);
    const auto document=sla::LayerDocument::from_rasters(masks);
    for(size_t i=0;i<4;++i)CHECK(document.image(i).buf==std::vector<uint8_t>{0,0,0,0});
    const auto native=sla::LayerDocument::from_rasters(archive.layers());
    CHECK(native.image(0).buf==std::vector<uint8_t>{255,255,255,255});
    CHECK(native.image(1).buf==native.image(0).buf);
    CHECK(native.image(2).buf==std::vector<uint8_t>{0,0,0,0});
}

TEST_CASE("Cancelled canonical capture retains the last complete encoding pair", "[sla_canonical_encoder]") {
    struct Archive : SL1Archive {
        using SL1Archive::SL1Archive;
        const auto &layers() const {return m_layers;}
    };
    SLAPrinterConfig config;config.display_width.value=1.;config.display_height.value=1.;
    config.display_pixels_x.value=2;config.display_pixels_y.value=2;
    config.display_orientation.value=sladoLandscape;
    Archive archive(config);std::vector<sla::EncodedRaster> masks;
    archive.draw_layers(4,[](sla::RasterBase &,size_t){},[]{return false;},ExecutionTBB{},&masks);
    const auto saved_native=archive.layers(),saved_masks=masks;
    const auto equal=[](const auto &a,const auto &b) {
        if(a.size()!=b.size())return false;
        for(size_t i=0;i<a.size();++i)
            if(a[i].size()!=b[i].size() || std::memcmp(a[i].data(),b[i].data(),a[i].size())!=0)return false;
        return true;
    };
    const auto action=GENERATE("exception","cancel");CAPTURE(action);
    if(std::string(action)=="exception") {
        const sla::RasterEncoder capture=[](const void *,size_t,size_t,size_t)->sla::EncodedRaster {
            throw sla::LayerOperationCancelled{};
        };
        CHECK_THROWS_AS(archive.draw_layers(2,[](sla::RasterBase &,size_t){},[]{return false;},
            ExecutionTBB{},&masks,nullptr,capture),sla::LayerOperationCancelled);
    } else {
        std::atomic<bool> cancel{false};
        archive.draw_layers(2,[&](sla::RasterBase &,size_t){cancel.store(true);},
            [&]{return cancel.load();},ExecutionTBB{},&masks);
    }
    CHECK(equal(archive.layers(),saved_native));CHECK(equal(masks,saved_masks));
}
