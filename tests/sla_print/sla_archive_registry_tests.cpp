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
#include <cstdlib>
#include <nlohmann/json.hpp>
#include <miniz.h>

#include "libslic3r/Format/SLAArchiveFormatRegistry.hpp"
#include "libslic3r/Format/SL1.hpp"
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

using namespace Slic3r;

TEST_CASE("Explicit layer export uses destination despite a legacy source format label", "[sla_reference][sla_export_destination]") {
    const std::string source_format = GENERATE("SL1 archive (.sl1)", "Creality CXDLPv4 (.cxdlpv4)", "cbddlp");
    const std::string destination = GENERATE("SL1", "cxdlpv4");
    CAPTURE(source_format, destination);
    SLAFullPrintConfig full;
    full.printer_technology.setInt(ptSLA);
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
    const std::string format = GENERATE("pws", "pw0", "pwmb", "dl2p", "pwc");
    const bool portrait = GENERATE(false, true);
    CAPTURE(format, portrait);
    DynamicPrintConfig config; config.apply(SLAFullPrintConfig{});
    config.option("printer_technology")->setInt(ptSLA); config.set("sla_archive_format", format);
    const auto *legacy = anycubic_legacy_geometry(format);
    const double width = legacy ? legacy->width : 9., height = legacy ? legacy->height : 2.;
    config.set("display_width", legacy && portrait ? height : width);
    config.set("display_height", legacy && portrait ? width : height);
    config.set("max_print_height", legacy ? legacy->machine_z : 200.);
    config.set("display_pixels_x", 6); config.set("display_pixels_y", 2);
    config.option("display_orientation")->setInt(portrait ? sladoPortrait : sladoLandscape);
    config.set("display_mirror_x", false); config.set("display_mirror_y", false);
    config.set("layer_height", .05); config.set("initial_layer_height", .1);
    config.set("sla_bottom_layers", 1); config.set("sla_transition_layers", 1);
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
    const uint32_t version=format=="dl2p"?517:format=="pwc"?516:format=="pwmb"?515:1;
    REQUIRE(le(12)==version); REQUIRE(le(20)==(version>=517?56:version>=516?52:48));
    const auto header=le(20), table=le(36);
    const auto preview=le(28);
    REQUIRE(le(preview+16)==224); REQUIRE(le(preview+24)==168);
    REQUIRE(std::any_of(bytes.begin()+preview+28, bytes.begin()+preview+28+224*168*2, [](uint8_t value) {return value != 0;}));
    auto preview_pixel = [&](size_t x,size_t y) {
        const size_t offset=preview+28+2*(y*224+x);
        return uint16_t(bytes[offset])|(uint16_t(bytes[offset+1])<<8);
    };
    REQUIRE(preview_pixel(112,0)==0); REQUIRE(preview_pixel(112,27)==0);
    REQUIRE(preview_pixel(112,28)==0x001f); REQUIRE(preview_pixel(112,83)==0x001f);
    REQUIRE(preview_pixel(112,84)==0xf800); REQUIRE(preview_pixel(112,139)==0xf800);
    REQUIRE(preview_pixel(112,140)==0); REQUIRE(preview_pixel(112,167)==0);
    REQUIRE(preview_pixel(28,28)==0x0010);
    if (version >= 516) {
        const auto machine = le(44);
        REQUIRE(std::string(bytes.begin()+machine,bytes.begin()+machine+7) == "MACHINE");
        REQUIRE(le(machine+12)==224);
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
    REQUIRE(dynamic_cast<SL1Reader *>(reader.get()) != nullptr);
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
    for (const char *ext : {".CTB", "ctb", "encrypted.ctb"}) {
        const auto *entry = get_archive_entry_by_extension(ext);
        REQUIRE(entry != nullptr);
        REQUIRE(std::string(entry->id) == "ctb");
    }
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
    const int writer = GENERATE(0, 1, 2, 3, 4);
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
                case 2: raster = ExposedRasterArchive<UVToolsFDGArchive>(cfg).create_raster(); break;
                case 3: raster = ExposedRasterArchive<UVToolsCWSArchive>(cfg).create_raster(); break;
                case 4: raster = ExposedRasterArchive<CrealityCXDLPv4Archive>(cfg).create_raster(); break;
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

TEST_CASE("Unavailable exports fail before drawing or writing a file", "[sla_registry]")
{
    SLAPrinterConfig cfg;
    SLAPrint print;
    for (const char *id : {"cbddlp", "cbt", "photon", "photons",
                          "qdt", "svgx", "lgs", "lgs30", "lgs120", "lgs4k", "gr1",
                          "mdlp", "nanodlp", "phz",
                          "jxs", "vdt", "zcode", "zcodex", "fdg"}) {
        CAPTURE(id);
        auto writer = SLAArchiveWriter::create(id, cfg);
        REQUIRE(writer != nullptr);
        bool drawn = false;
        REQUIRE_THROWS_WITH(writer->draw_layers(1, [&](sla::RasterBase &, size_t) { drawn = true; }, [] { return false; }),
                            archive_export_unavailable_reason(id));
        REQUIRE_FALSE(drawn);
        const auto path = boost::filesystem::unique_path("unsupported-%%%%-%%%%.tmp");
        REQUIRE_THROWS_WITH(writer->export_print(path.string(), print, {}, "test"),
                            archive_export_unavailable_reason(id));
        REQUIRE_FALSE(boost::filesystem::exists(path));
    }
    REQUIRE(archive_export_unavailable_reason("SL1").empty());
    REQUIRE(archive_export_unavailable_reason("goo").empty());
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
            full.printer_technology.setInt(ptSLA);
            full.display_orientation.value = sladoLandscape;
            full.set("sla_archive_format", std::string(entry.id));
            full.set("supports_enable", false); full.set("pad_enable", false);
            full.set("support_object_elevation", 0.);
            full.set("display_width", 100.); full.set("display_height", 60.);
            full.set("display_pixels_x", 200); full.set("display_pixels_y", 120);
            if(std::string(entry.id)=="cwsrgb")full.set("display_pixels_x",201);
            if (const auto *machine = anycubic_legacy_geometry(entry.id)) {
                full.set("display_width", machine->width); full.set("display_height", machine->height);
                full.set("max_print_height", machine->machine_z);
            }
            full.set("layer_height", 0.05); full.set("initial_layer_height", 0.05);
            full.set("exposure_time", 2.5); full.set("initial_exposure_time", 20.);
            if(std::string(entry.id)=="cxdlpv1" || std::string(entry.id)=="cxdlp" || std::string(entry.id)=="cxdlpv2") {
                full.set("exposure_time",3.);
                if(std::string(entry.id)=="cxdlp" || std::string(entry.id)=="cxdlpv2") {
                    full.set("printer_model",std::string("CL-60"));
                    full.set("sla_lift_speed",120.);full.set("sla_bottom_lift_speed",60.);
                    full.set("sla_retract_speed",180.);full.set("sla_bottom_retract_speed",180.);
                }
                full.set("sla_wait_before_cure",1.);full.set("sla_bottom_wait_before_cure",1.);
            }
            if (std::string(entry.id) == "n4" || std::string(entry.id) == "n7") {
                const bool n7 = std::string(entry.id) == "n7";
                full.set("display_width", n7 ? 192. : 68.04);
                full.set("display_height", n7 ? 120. : 120.96);
                full.set("display_pixels_x", n7 ? 2560 : 1440);
                full.set("display_pixels_y", n7 ? 1600 : 2560);
                full.set("max_print_height", n7 ? 300. : 135.);
                full.set("exposure_time", 3.);
                full.set("sla_transition_layers", 0);
                for (const char *key : {"sla_lift_speed", "sla_bottom_lift_speed",
                                        "sla_retract_speed", "sla_bottom_retract_speed"})
                    full.set(key, 120.);
            }
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
    REQUIRE(f32(parameters) == Catch::Approx(6.));
    REQUIRE(f32(parameters+4) == Catch::Approx(60.));
    REQUIRE(f32(parameters+8) == Catch::Approx(6.));
    REQUIRE(f32(parameters+12) == Catch::Approx(80.));
    REQUIRE(f32(parameters+16) == Catch::Approx(150.));
    REQUIRE(f32(28) == Catch::Approx(double(print.print_layers().back().level()) * SCALING_FACTOR));
    const auto table = u32(64);
    for (size_t i = 0; i < print.print_layers().size(); ++i) {
        REQUIRE(f32(table + i*36) == Catch::Approx(double(print.print_layers()[i].level()) * SCALING_FACTOR));
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
                REQUIRE(floating(extended, false) == Catch::Approx(i == 0 ? 4.5 : 7.5));
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
        REQUIRE(profile.find(portrait ? "<DotsPermmX>1.66666667</DotsPermmX>" : "<DotsPermmX>2</DotsPermmX>") != std::string::npos);
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
