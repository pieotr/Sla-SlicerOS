#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <boost/filesystem.hpp>
#include "libslic3r/SLA/LayerDocument.hpp"
#include "libslic3r/SLA/ProjectedPNGMaskEncoder.hpp"
#include "libslic3r/SLA/SupportPointGenerator.hpp"
#include "libslic3r/SLA/RasterBase.hpp"
#include "libslic3r/Zipper.hpp"
#include "libslic3r/Format/ZipperArchiveImport.hpp"
#include "libslic3r/SLAPrint.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Format/SLAArchiveWriter.hpp"
#include "libslic3r/Format/3mf.hpp"
#include "libslic3r/SLA/ProjectArchive.hpp"
#include "libslic3r/SLA/ColorMask.hpp"
#include <atomic>
#include <chrono>
#include <numeric>
#include "libslic3r/TriangleMesh.hpp"
#include <fstream>
#include <nlohmann/json.hpp>
using namespace Slic3r;

TEST_CASE("Pending model saves refresh automatic schedules without changing masks or explicit edits",
          "[sla_layers][sla_project][sla_pending_project_metadata]")
{
    using F = sla::LayerParameter;
    const bool explicit_parameters = GENERATE(false, true);
    CAPTURE(explicit_parameters);
    Model model;
    const double height = SLAPrintObjectConfig{}.layer_height.value;
    auto *object = model.add_object("pending metadata", "", make_cube(4., 3., 3 * height));
    object->add_instance()->set_offset(Vec3d(10., 10., 0.));
    DynamicPrintConfig config;
    config.apply(SLAFullPrintConfig{});
    config.set("sla_archive_format", std::string("cxdlpv4"));
    config.set("display_width", 100.); config.set("display_height", 60.);
    config.set("display_pixels_x", 200); config.set("display_pixels_y", 120);
    config.set("supports_enable", false); config.set("pad_enable", false);
    config.set("support_object_elevation", 0.);
    config.set("layer_height", height); config.set("initial_layer_height", height);
    config.set("sla_bottom_layers", 1); config.set("sla_transition_layers", 0);
    config.set("exposure_time", 2.); config.set("initial_exposure_time", 10.);
    SLAPrint print;
    print.set_status_callback([](const PrintBase::SlicingStatus &) {});
    print.apply(model, config); print.process();
    auto state = sla::capture_project_layers(model, config, print);
    REQUIRE(state.document.layer_count() == 3);
    state.document.paint(0, 0, 0, 1, 127);
    if (explicit_parameters) state.document.set_layer_parameters(1, {{F::Exposure, 4.}});
    const auto before = state;
    const auto old_rows = before.document.fresh_job()->rows;
    const auto old_globals = before.document.fresh_job()->globals;
    auto current = config;
    current.set("exposure_time", 3.); current.set("initial_exposure_time", 12.);
    current.set("sla_bottom_layers", 2); current.set("sla_transition_layers", 9);
    current.set("sla_lift_speed", 75.); current.set("sla_bottom_lift_speed", 55.);
    REQUIRE(sla::refresh_model_project_settings(state, model, current));
    REQUIRE(state.settings->material.values.at("exposure_time") == "3");
    REQUIRE(state.source_fingerprint == before.source_fingerprint);
    REQUIRE(state.positions_z == before.positions_z);
    REQUIRE(state.document.revision() == before.document.revision());
    REQUIRE(state.document.can_undo() == before.document.can_undo());
    REQUIRE(state.document.can_redo() == before.document.can_redo());
    REQUIRE(state.document.fresh_job()->mask_precision == before.document.fresh_job()->mask_precision);
    for (size_t i = 0; i < state.document.layer_count(); ++i) {
        REQUIRE(state.document.image(i).buf == before.document.image(i).buf);
        REQUIRE(state.document.fresh_job()->rows[i].at(F::PositionZ) == state.positions_z[i]);
    }
    const auto &job = *state.document.fresh_job();
    if (explicit_parameters) {
        REQUIRE(job.parameter_origin == sla::FreshLayerParameterOrigin::Explicit);
        REQUIRE(job.rows == old_rows);
        REQUIRE(job.globals.normal == old_globals.normal);
        REQUIRE(job.globals.bottom == old_globals.bottom);
    } else {
        REQUIRE(job.parameter_origin == sla::FreshLayerParameterOrigin::AutomaticModel);
        REQUIRE(job.globals.normal.at(F::Exposure) == 3.);
        REQUIRE(job.globals.bottom.at(F::Exposure) == 12.);
        REQUIRE(job.globals.bottom_layers == 2);
        REQUIRE(job.globals.transition_layers == 1);
        REQUIRE(job.rows[0].at(F::Exposure) == 12.);
        REQUIRE(job.rows[1].at(F::Exposure) == 12.);
        // A clipped transition count must retain the configured denominator.
        REQUIRE(job.rows[2].at(F::Exposure) == Catch::Approx(11.1));
        REQUIRE(job.rows[0].at(F::LiftSpeed) == 55.);
        REQUIRE(job.rows[2].at(F::LiftSpeed) == 75.);
    }
    // ConfigBase permits omitted defaults. Normalize the same process defaults
    // as the fingerprint, without dereferencing an absent dynamic option.
    current.erase("layer_height");
    REQUIRE(sla::refresh_model_project_settings(state, model, current));
    REQUIRE(state.document.fresh_job()->globals.layer_height == height);
    const auto accepted_settings = state.settings;
    const auto accepted_rows = job.rows;
    Model moved = model;
    moved.objects[0]->instances[0]->set_offset(Vec3d(11., 10., 0.));
    REQUIRE_FALSE(sla::refresh_model_project_settings(state, moved, current));
    REQUIRE(state.settings == accepted_settings);
    REQUIRE(state.document.fresh_job()->rows == accepted_rows);
    auto invalid = current;
    invalid.set("max_print_height", .01);
    REQUIRE_THROWS(sla::refresh_model_project_settings(state, model, invalid));
    REQUIRE(state.settings == accepted_settings);
    REQUIRE(state.document.fresh_job()->rows == accepted_rows);
    if (!explicit_parameters) {
        invalid = current; invalid.set("exposure_time", -1.);
        REQUIRE_THROWS(sla::refresh_model_project_settings(state, model, invalid));
        REQUIRE(state.settings == accepted_settings);
        REQUIRE(state.document.fresh_job()->rows == accepted_rows);
    }
    REQUIRE(state.document.image(0).buf == before.document.image(0).buf);
    // Refreshing a copy cannot alter the earlier saved result.
    REQUIRE(before.document.fresh_job()->rows == old_rows);
    REQUIRE(before.settings->material.values.at("exposure_time") == "2");
}

TEST_CASE("Legacy SL1 duplicate previews are accepted only when byte identical", "[sla_layers][sla_sl1_duplicates]") {
    const unsigned scenario=GENERATE(0u,1u,2u);
    struct File {
        boost::filesystem::path path=boost::filesystem::unique_path("duplicate-preview-%%%%.sl1");
        ~File(){boost::system::error_code error;boost::filesystem::remove(path,error);}
    } file;
    const std::vector<uint8_t> pixels{0,1,127,255};
    const auto mask=sla::PNGRasterEncoder{}(pixels.data(),2,2,1);
    const auto different=sla::PNGRasterEncoder{}(pixels.data(),4,1,1);
    {
        Zipper zip(file.path.string());
        zip.add_entry("config.ini");zip<<"layerHeight = 0.05\n";
        zip.add_entry("prusaslicer.ini");zip<<"display_width = 2\n";
        zip.add_entry("job00000.png",mask.data(),mask.size());
        zip.add_entry("thumbnail/thumbnail2x2.png",mask.data(),mask.size());
        if(scenario==0)zip.add_entry("thumbnail/thumbnail2x2.png",mask.data(),mask.size());
        if(scenario==1)zip.add_entry("thumbnail/thumbnail2x2.png",different.data(),different.size());
        if(scenario==2)zip.add_entry("job00000.png",mask.data(),mask.size());
        zip.finalize();
    }
    if(scenario){REQUIRE_THROWS(sla::LayerDocument::load_sl1(file.path.string()));return;}
    const auto restored=sla::LayerDocument::load_sl1(file.path.string());
    REQUIRE(restored.layer_count()==1);REQUIRE(restored.image(0).buf==pixels);
}

TEST_CASE("SL1 export writes each thumbnail size once", "[sla_layers][sla_sl1_duplicates]") {
    struct File {
        boost::filesystem::path path=boost::filesystem::unique_path("unique-preview-%%%%.sl1");
        ~File(){boost::system::error_code error;boost::filesystem::remove(path,error);}
    } file;
    DynamicPrintConfig config;config.apply(SLAFullPrintConfig{});
    config.option("printer_technology")->setInt(ptSLA);
    config.set("sla_archive_format",std::string("SL1"));
    config.set("display_pixels_x",2);config.set("display_pixels_y",2);
    config.set("display_width",2.);config.set("display_height",2.);
    config.option("display_orientation")->setInt(sladoLandscape);
    const std::vector<uint8_t> pixels{0,1,127,255};
    std::vector<sla::EncodedRaster> masks;
    masks.push_back(sla::PNGRasterEncoder{}(pixels.data(),2,2,1));
    auto document=sla::LayerDocument::from_rasters(std::move(masks));
    auto job=SLAPrint::create_raster_job(config,document,{.05});
    ThumbnailData preview;preview.set(2,2);std::fill(preview.pixels.begin(),preview.pixels.end(),255);
    job->export_layer_document(document,"SL1",file.path.string(),{preview,preview},"unique");
    const auto previews=read_zipper_archive(file.path.string(),{"thumbnail"},{});
    REQUIRE(previews.entries.size()==1);
    REQUIRE(sla::LayerDocument::load_sl1(file.path.string()).image(0).buf==pixels);
}

TEST_CASE("CWS editing follows its manifest order and preserves SLA commands", "[sla_layers][sla_cws_editor]")
{
    struct File {
        boost::filesystem::path path = boost::filesystem::unique_path("cws-editor-%%%%.cws");
        ~File() { boost::system::error_code error; boost::filesystem::remove(path, error); }
    } file;
    const std::vector<uint8_t> first{0,17,85,128,254,255}, second{255,1,2,3,4,5};
    auto a = sla::PNGRasterEncoder{}(first.data(), 3, 2, 1), b = sla::PNGRasterEncoder{}(second.data(), 3, 2, 1);
    std::string slices = "<Slice><name>z.png</name></Slice><Slice><name>a.png</name></Slice>";
    std::string profile = "<SliceBuildConfig><XResolution>3</XResolution><YResolution>2</YResolution></SliceBuildConfig>";
    bool invalid = false;
    SECTION("manifest order survives editing") {}
    SECTION("duplicate mask reference is rejected") { slices += "<Slice><name>z.png</name></Slice>"; invalid = true; }
    SECTION("missing mask is rejected") { slices = "<Slice><name>missing.png</name></Slice>"; invalid = true; }
    SECTION("profile dimension mismatch is rejected") { a = sla::PNGRasterEncoder{}(first.data(), 2, 3, 1); invalid = true; }
    SECTION("external XML declarations are rejected") { profile = "<!DOCTYPE SliceBuildConfig SYSTEM 'file:///etc/passwd'>" + profile; invalid = true; }
    const std::string commands = ";<Slice> 0\nM106 S255\n;<Delay> 2500\nM106 S0\n";
    { Zipper zip(file.path.string());
      zip.add_entry("manifest.xml"); zip << "<manifest><Slices>" + slices + "</Slices><SliceProfile><name>job.slicing</name></SliceProfile><GCode><name>job.gcode</name></GCode></manifest>";
      zip.add_entry("job.slicing"); zip << profile;
      zip.add_entry("job.gcode"); zip << commands;
      zip.add_entry("a.png", b.data(), b.size()); zip.add_entry("z.png", a.data(), a.size());
      zip.add_entry("unknown.txt"); zip << "preserved"; zip.finalize(); }
    if (invalid) { REQUIRE_THROWS(sla::LayerDocument::load_archive(file.path.string())); return; }
    auto document = sla::LayerDocument::load_archive(file.path.string());
    REQUIRE(document.archive_format() == "cws"); REQUIRE(document.layer_count() == 2);
    REQUIRE(document.image(0).buf == first); REQUIRE(document.image(1).buf == second);
    document.invert(0); document.save_archive(file.path.string());
    auto restored = sla::LayerDocument::load_archive(file.path.string());
    REQUIRE(restored.image(0).buf == std::vector<uint8_t>{255,238,170,127,1,0});
    REQUIRE(restored.image(1).buf == second);
    const auto archive = read_zipper_archive(file.path.string(), {""}, {});
    for (const auto &entry : archive.entries) {
        if (entry.fname == "job.gcode") REQUIRE(std::string(entry.buf.begin(), entry.buf.end()) == commands);
        if (entry.fname == "job.slicing") REQUIRE(std::string(entry.buf.begin(), entry.buf.end()) == profile);
        if (entry.fname == "unknown.txt") REQUIRE(std::string(entry.buf.begin(), entry.buf.end()) == "preserved");
    }
}

TEST_CASE("NovaMaker CWS masks follow numeric indices and preserve settings", "[sla_layers][sla_novamaker]") {
    struct File {
        boost::filesystem::path path=boost::filesystem::unique_path("novamaker-editor-%%%%.cws");
        ~File(){boost::system::error_code error;boost::filesystem::remove(path,error);}
    } file,saved;
    const std::vector<uint8_t> first{0,17,85,128,254,255},second{255,1,2,3,4,5};
    auto a=sla::PNGRasterEncoder{}(first.data(),3,2,1),b=sla::PNGRasterEncoder{}(second.data(),3,2,1);
    std::string settings="# conf version 1.0\nxres = 3\nyres = 2\nxppm = 2\nyppm = 1\nthickness = 0.05\nlayers_num = 2\n";
    std::string second_name="job00001.png";bool invalid=false,missing_command=false,extra_command=false,missing_layer=false,corrupt=false;
    SECTION("reverse ZIP order and unknown entries survive editing"){}
    SECTION("duplicate settings are rejected"){settings+="xres = 3\n";invalid=true;}
    SECTION("missing settings are rejected"){settings="yres = 2\nlayers_num = 2\n";invalid=true;}
    SECTION("invalid physical dimensions are rejected"){settings+="yppm2 = 1\n";settings.replace(settings.find("xppm = 2"),8,"xppm = nan");invalid=true;}
    SECTION("fractional resolution is rejected"){settings.replace(settings.find("xres = 3"),8,"xres = 3.5");invalid=true;}
    SECTION("duplicate numeric index is rejected"){second_name="different00000.png";invalid=true;}
    SECTION("noncontiguous index is rejected"){second_name="job00002.png";invalid=true;}
    SECTION("missing mask is rejected"){missing_layer=true;invalid=true;}
    SECTION("missing instructions are rejected"){missing_command=true;invalid=true;}
    SECTION("ambiguous instructions are rejected"){extra_command=true;invalid=true;}
    SECTION("wrong mask dimensions are rejected"){b=sla::PNGRasterEncoder{}(second.data(),2,3,1);invalid=true;}
    SECTION("damaged PNG is rejected"){corrupt=true;invalid=true;}
    {
        Zipper zip(file.path.string());zip.add_entry("slice.conf");zip<<settings;
        if(!missing_layer)zip.add_entry(second_name,b.data(),b.size());
        if(corrupt) {
            const auto *p=static_cast<const uint8_t *>(a.data());std::vector<uint8_t> bytes(p,p+a.size());bytes.at(bytes.size()-1)^=1;
            zip.add_entry("job00000.png",bytes.data(),bytes.size());
        } else zip.add_entry("job00000.png",a.data(),a.size());
        if(!missing_command){zip.add_entry("job.gcode");zip<<"; preserved exposure/motion commands\n";}
        if(extra_command){zip.add_entry("ambiguous.gcode");zip<<"; unknown instructions\n";}
        zip.add_entry("notes/operator.txt");zip<<"unknown payload";zip.finalize();
    }
    if(invalid){REQUIRE_THROWS(sla::LayerDocument::load_archive(file.path.string()));return;}
    auto document=sla::LayerDocument::load_archive(file.path.string());
    REQUIRE(document.archive_format()=="cwsnovamaker");REQUIRE(document.layer_count()==2);
    REQUIRE(document.image(0).buf==first);REQUIRE(document.image(1).buf==second);
    document.invert(0);REQUIRE(document.undo());REQUIRE(document.redo());
    document.save_archive(saved.path.string());
    auto restored=sla::LayerDocument::load_archive(saved.path.string());
    REQUIRE(restored.image(0).buf==document.image(0).buf);REQUIRE(restored.image(1).buf==second);
    const auto archive=read_zipper_archive(saved.path.string(),{""},{});
    for(const auto &entry:archive.entries) {
        if(entry.fname=="slice.conf")REQUIRE(std::string(entry.buf.begin(),entry.buf.end())==settings);
        if(entry.fname=="job.gcode")REQUIRE(std::string(entry.buf.begin(),entry.buf.end())=="; preserved exposure/motion commands\n");
        if(entry.fname=="notes/operator.txt")REQUIRE(std::string(entry.buf.begin(),entry.buf.end())=="unknown payload");
    }
    REQUIRE_FALSE(document.dirty());REQUIRE_THROWS(document.save_archive(saved.path.string()+".zip"));
}

TEST_CASE("RGB.CWS preserves every grayscale value and BGR packing", "[sla_layers][sla_cwsrgb]") {
    png::ImageGreyscale original;original.cols=768;original.rows=2;original.buf.resize(1536);
    for(size_t i=0;i<original.buf.size();++i)original.buf[i]=uint8_t(i);
    const auto encoded=sla::encode_bgr_packed_mask(original);
    const auto *data=static_cast<const uint8_t *>(encoded.data());
    REQUIRE(encoded.size()>25);REQUIRE(data[24]==8);REQUIRE(data[25]==2);
    std::vector<unsigned char> rgba;unsigned width=0,height=0;
    REQUIRE(png::decode_png(std::string(reinterpret_cast<const char *>(data),encoded.size()),rgba,width,height));
    REQUIRE(width==256);REQUIRE(height==2);
    for(size_t i=0;i<original.buf.size();++i)REQUIRE(rgba[4*(i/3)+2-i%3]==original.buf[i]);
    png::ImageGreyscale restored;
    REQUIRE(sla::decode_bgr_packed_mask({encoded.data(),encoded.size()},768,2,restored));
    REQUIRE(restored.cols==768);REQUIRE(restored.rows==2);REQUIRE(restored.buf==original.buf);
    std::vector<uint8_t> broken(data,data+encoded.size());broken.at(broken.size()-1)^=1;
    REQUIRE_FALSE(sla::decode_bgr_packed_mask({broken.data(),broken.size()},768,2,restored));
    REQUIRE_FALSE(sla::decode_bgr_packed_mask({encoded.data(),encoded.size()/2},768,2,restored));
    REQUIRE_FALSE(sla::decode_bgr_packed_mask({encoded.data(),encoded.size()},767,2,restored));
    REQUIRE_FALSE(sla::decode_bgr_packed_mask({encoded.data(),encoded.size()},768,3,restored));
    REQUIRE_FALSE(sla::decode_bgr_packed_mask({nullptr,100},768,2,restored));
    original.cols=767;REQUIRE_THROWS(sla::encode_bgr_packed_mask(original));
}

TEST_CASE("RGB.CWS editor expands and repacks masks without losing metadata", "[sla_layers][sla_cwsrgb]") {
    struct File {
        boost::filesystem::path path=boost::filesystem::unique_path("rgb-cws-editor-%%%%.rgb.cws");
        ~File(){boost::system::error_code error;boost::filesystem::remove(path,error);}
    } file,saved;
    png::ImageGreyscale mask;mask.cols=9;mask.rows=2;mask.buf={0,17,85,128,254,255,255,1,2,3,4,5,16,32,64,128,192,250};
    auto a=sla::encode_bgr_packed_mask(mask),b=a;
    std::string settings="xres = 9\nyres = 2\nxppm = 2\nyppm = 1\nthickness = 0.05\nlayers_num = 2\n";
    bool invalid=false,corrupt=false;
    SECTION("two layers and metadata survive Undo Redo and Save"){}
    SECTION("mixed image formats are rejected"){b=sla::PNGRasterEncoder{}(mask.buf.data(),9,2,1);invalid=true;}
    SECTION("nondivisible LCD width is rejected"){settings.replace(settings.find("xres = 9"),8,"xres = 8");invalid=true;}
    SECTION("wrong packed PNG width is rejected"){mask.cols=6;mask.buf.resize(12);b=sla::encode_bgr_packed_mask(mask);invalid=true;}
    SECTION("wrong PNG color type is rejected"){b=sla::encode_rgba_mask(mask);invalid=true;}
    SECTION("damaged packed PNG is rejected"){corrupt=true;invalid=true;}
    {
        Zipper zip(file.path.string());zip.add_entry("slice.conf");zip<<settings;
        zip.add_entry("job00001.png",b.data(),b.size());
        if(corrupt) {
            const auto *p=static_cast<const uint8_t *>(a.data());std::vector<uint8_t> bytes(p,p+a.size());bytes.at(bytes.size()-1)^=1;
            zip.add_entry("job00000.png",bytes.data(),bytes.size());
        } else zip.add_entry("job00000.png",a.data(),a.size());
        zip.add_entry("job.gcode");zip<<"; preserved exposure/motion\n";
        zip.add_entry("operator.txt");zip<<"unknown payload";zip.finalize();
    }
    if(invalid){REQUIRE_THROWS(sla::LayerDocument::load_archive(file.path.string()));return;}
    auto document=sla::LayerDocument::load_archive(file.path.string());
    REQUIRE(document.archive_format()=="cwsrgb");REQUIRE(document.layer_count()==2);
    REQUIRE(document.image(0).cols==9);REQUIRE(document.image(0).rows==2);
    REQUIRE(document.image(0).buf==mask.buf);REQUIRE(document.image(1).buf==mask.buf);
    document.invert(0);REQUIRE(document.undo());REQUIRE(document.image(0).buf==mask.buf);REQUIRE(document.redo());
    document.save_archive(saved.path.string());
    const auto reloaded=sla::LayerDocument::load_archive(saved.path.string());
    REQUIRE(reloaded.image(0).buf==document.image(0).buf);REQUIRE(reloaded.image(1).buf==mask.buf);
    const auto archive=read_zipper_archive(saved.path.string(),{""},{});
    for(const auto &entry:archive.entries) {
        if(entry.fname=="slice.conf")REQUIRE(std::string(entry.buf.begin(),entry.buf.end())==settings);
        if(entry.fname=="job.gcode")REQUIRE(std::string(entry.buf.begin(),entry.buf.end())=="; preserved exposure/motion\n");
        if(entry.fname=="operator.txt")REQUIRE(std::string(entry.buf.begin(),entry.buf.end())=="unknown payload");
        if(entry.fname=="job00000.png") {
            std::vector<unsigned char> pixels;unsigned width=0,height=0;
            REQUIRE(png::decode_png(std::string(entry.buf.begin(),entry.buf.end()),pixels,width,height));
            REQUIRE(width==3);REQUIRE(height==2);
            for(size_t i=0;i<mask.buf.size();++i)REQUIRE(pixels[4*(i/3)+2-i%3]==uint8_t(255-mask.buf[i]));
        }
    }
    REQUIRE_THROWS(document.save_archive(saved.path.string()+".cws"));
    REQUIRE_FALSE(document.dirty());
}

TEST_CASE("Standalone PNG masks preserve grayscale and save atomically without a ZIP wrapper", "[sla_layers][sla_png]")
{
    struct Files {
        boost::filesystem::path dir = boost::filesystem::unique_path("png-editor-%%%%");
        Files() { boost::filesystem::create_directory(dir); }
        ~Files() { boost::system::error_code error; boost::filesystem::remove_all(dir, error); }
    } files;
    const auto input = (files.dir / "input.png").string(), output = (files.dir / "edited.png").string();
    auto write = [&](const auto &bytes) {
        std::ofstream stream(input, std::ios::binary | std::ios::trunc);
        stream.write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
    };
    const std::vector<uint8_t> pixels{0,1,127,128,254,255};
    const auto encoded = sla::PNGRasterEncoder{}(pixels.data(), 3, 2, 1);
    const auto *data = static_cast<const uint8_t *>(encoded.data());
    std::vector<uint8_t> original(data, data + encoded.size());
    write(original);
    auto document = sla::LayerDocument::load_archive(input);
    REQUIRE(document.archive_format() == "png"); REQUIRE(document.layer_count() == 1);
    REQUIRE(document.image(0).buf == pixels);
    REQUIRE_THROWS(sla::LayerDocument::load_sl1(input));
    document.save_archive(output);
    REQUIRE(document.encoded_layer(0).sz == original.size());
    document.invert(0); REQUIRE(document.dirty());
    REQUIRE_THROWS(document.save_archive((files.dir / "edited.uvj").string()));
    REQUIRE_THROWS(document.save_archive((files.dir / "missing" / "edited.png").string()));
    REQUIRE(document.dirty());
    REQUIRE(sla::LayerDocument::load_archive(output).image(0).buf == pixels);
    document.save_archive(output); REQUIRE_FALSE(document.dirty());
    REQUIRE(sla::LayerDocument::load_archive(output).image(0).buf == std::vector<uint8_t>{255,254,128,127,1,0});
    REQUIRE(sla::LayerDocument::load_archive(input).image(0).buf == pixels);
    REQUIRE(document.undo()); REQUIRE(document.image(0).buf == pixels);
    original[original.size()-1] ^= 1; write(original);
    REQUIRE_THROWS(sla::LayerDocument::load_archive(input));
    original.assign(data, data + encoded.size());
    for (size_t offset = 8; offset + 12 <= original.size();) {
        auto be32 = [&](size_t p) { return (uint32_t(original[p]) << 24) | (uint32_t(original[p+1]) << 16) |
            (uint32_t(original[p+2]) << 8) | original[p+3]; };
        const size_t length = be32(offset);
        if (std::string(original.begin()+offset+4, original.begin()+offset+8) == "IDAT") {
            original[offset+8] = 0; // Invalid deflate header, but a valid PNG chunk checksum.
            const uint32_t crc = mz_crc32(0, original.data()+offset+4, length+4);
            for (size_t byte=0; byte<4; ++byte) original[offset+8+length+byte] = uint8_t(crc >> (24-8*byte));
            break;
        }
        offset += length+12;
    }
    write(original);
    REQUIRE_THROWS(sla::LayerDocument::load_archive(input));
    original.resize(33); original[16] = 0x7f; write(original);
    REQUIRE_THROWS(sla::LayerDocument::load_archive(input));
}

TEST_CASE("Color PNG masks use luminance and composite transparency over black", "[sla_layers][sla_png]")
{
    const std::vector<uint8_t> rgba{255,0,0,255, 0,255,0,255, 0,0,255,255,
                                  255,255,255,255, 255,255,255,0, 127,127,127,128};
    const auto encoded = sla::PNGRasterEncoder{}(rgba.data(), 3, 2, 4);
    struct File {
        boost::filesystem::path path = boost::filesystem::unique_path("rgba-mask-%%%%.png");
        ~File() { boost::system::error_code error; boost::filesystem::remove(path, error); }
    } file;
    { std::ofstream stream(file.path.string(), std::ios::binary); stream.write(static_cast<const char *>(encoded.data()), encoded.size()); }
    auto document = sla::LayerDocument::load_archive(file.path.string());
    REQUIRE(document.image(0).buf == std::vector<uint8_t>{76,150,29,255,0,64});
    document.save_archive(file.path.string());
    const auto bytes = document.encoded_layer(0);
    REQUIRE(static_cast<const uint8_t *>(bytes.buf)[25] == 0);
    REQUIRE(sla::LayerDocument::load_archive(file.path.string()).image(0).buf == document.image(0).buf);
}

TEST_CASE("Saved project layer metadata rejects malformed masks tables and duplicate entries", "[sla_layers][sla_project]") {
    struct File {boost::filesystem::path path=boost::filesystem::unique_path("project-invalid-%%%%.3mf");
        ~File(){boost::system::error_code error;boost::filesystem::remove(path,error);}} file;
    nlohmann::json metadata{{"version",1},{"source_fingerprint",std::string(40,'a')},{"columns",3},{"rows",2},{"layers",1},{"positions_z",{.05}}};
    std::vector<uint8_t> pixels{0,1,127,128,254,255};
    auto mask=sla::PNGRasterEncoder{}(pixels.data(),3,2,1);
    bool extra=false,duplicate=false,missing=false;
    SECTION("fractional version") {metadata["version"]=1.5;}
    SECTION("fractional geometry") {metadata["columns"]=3.1;}
    SECTION("invalid fingerprint") {metadata["source_fingerprint"]=std::string(40,'z');}
    SECTION("negative Z") {metadata["positions_z"]={-.05};}
    SECTION("height count mismatch") {metadata["positions_z"]={.05,.1};}
    SECTION("extra mask") {extra=true;}
    SECTION("missing mask") {missing=true;}
    SECTION("duplicate manifest with different casing") {duplicate=true;}
    SECTION("wrong PNG dimensions") {mask=sla::PNGRasterEncoder{}(pixels.data(),2,3,1);}
    SECTION("truncated PNG") {mask=sla::EncodedRaster(std::vector<uint8_t>(35,0),"png");}
    {
        Zipper zip(file.path.string());zip.add_entry("Metadata/SLA/layers.json");zip<<metadata.dump();
        if(duplicate){zip.add_entry("metadata/sla/LAYERS.json");zip<<metadata.dump();}
        if(!missing)zip.add_entry("Metadata/SLA/layers/00000000.png",mask.data(),mask.size());
        if(extra)zip.add_entry("Metadata/SLA/layers/00000001.png",mask.data(),mask.size());
        zip.finalize();
    }
    REQUIRE_THROWS(sla::read_project_layers(file.path.string()));
}

TEST_CASE("3MF retains edited SLA masks and restores only matching geometry and Z", "[sla_layers][sla_project]")
{
  struct File {
    boost::filesystem::path path =
        boost::filesystem::unique_path("sla-project-%%%%.3mf");
    ~File() {
      boost::system::error_code error;
      boost::filesystem::remove(path, error);
    }
  } file;
  Model model;
  auto *object =
      model.add_object("persisted edits", "", make_cube(4., 3., .15));
  object->add_instance()->set_offset(Vec3d(10., 10., 0.));
  const bool transformed = GENERATE(false, true);
  CAPTURE(transformed);
  if (transformed) {
    object->instances.front()->set_rotation(Vec3d(0., 0., .371));
    object->instances.front()->set_scaling_factor(Vec3d(1.125, .95, 1.));
    object->volumes.front()->set_rotation(Vec3d(0., 0., .173));
    object->sla_support_points.push_back({Vec3f(1., 1., .1), .2f, sla::SupportPointType::manual_add});
    object->sla_points_status = sla::PointsStatus::UserModified;
    object->sla_drain_holes.emplace_back(Vec3f(2., 1., .1), Vec3f::UnitZ(), .1f, .05f);
  }
  SLAFullPrintConfig full;
  full.set("sla_archive_format", std::string("SL1"));
  full.set("display_width", 100.);
  full.set("display_height", 60.);
  full.set("display_pixels_x", 200);
  full.set("display_pixels_y", 120);
  full.set("supports_enable", false);
  full.set("pad_enable", false);
  full.set("support_object_elevation", 0.);
  full.set("layer_height", .05);
  full.set("initial_layer_height", .05);
  DynamicPrintConfig config;
  config.apply(full);
  SLAPrint print;
  print.set_status_callback([](const PrintBase::SlicingStatus &) {});
  print.apply(model, config);
  print.process();
  auto edited = *print.layer_document();
  edited.paint(0, 0, 0, 1, 127);
  print.replace_layer_document(edited);
  const auto captured = sla::capture_project_layers(model, config, print);
  REQUIRE(print.layer_document()->can_undo());
  REQUIRE_FALSE(captured.document.can_undo());
  REQUIRE(captured.document.revision() == print.layer_document()->revision());
  REQUIRE(store_3mf(file.path.string().c_str(), &model, &config, false, nullptr,
                    true, &captured));
  auto saved = sla::read_project_layers(file.path.string());
  REQUIRE(saved.has_value());
  REQUIRE(saved->document.layer_count() == edited.layer_count());
  REQUIRE_FALSE(saved->document.dirty());
  REQUIRE_FALSE(saved->document.can_undo());
  REQUIRE(saved->document.revision() != 0);
  REQUIRE(saved->source_fingerprint == captured.source_fingerprint);
  for (size_t i = 0; i < edited.layer_count(); ++i)
    REQUIRE(saved->document.image(i).buf == edited.image(i).buf);
  Model reopened;
  DynamicPrintConfig loaded_config;
  ConfigSubstitutionContext substitutions{
      ForwardCompatibilitySubstitutionRule::Enable};
  boost::optional<Semver> version;
  REQUIRE(load_3mf(file.path.string().c_str(), loaded_config, substitutions,
                   &reopened, false, version));
  REQUIRE(sla::raster_source_fingerprint(reopened, loaded_config) ==
          captured.source_fingerprint);
  SLAPrint restored;
  restored.set_status_callback([](const PrintBase::SlicingStatus &) {});
  restored.apply(reopened, loaded_config);
  restored.process();
  REQUIRE(restored.layer_document()->image(0).get(0, 0) == 0);
  REQUIRE(
      sla::restore_project_layers(restored, reopened, loaded_config, *saved));
  REQUIRE(restored.layer_document()->image(0).get(0, 0) == 127);
  REQUIRE(restored.print_statistics().objects_used_material ==
          Catch::Approx(print.print_statistics().objects_used_material));
  SECTION(
      "exposure metadata and export format do not invalidate cached pixels") {
    auto metadata = loaded_config;
    metadata.set("exposure_time", 4.);
    metadata.set("sla_archive_format", std::string("uvj"));
    metadata.set("sla_cbddlp_aa",4);
    metadata.set("sla_anycubic_version",std::string("518"));
    REQUIRE(sla::raster_source_fingerprint(reopened, metadata) ==
            saved->source_fingerprint);
    restored.apply(reopened, metadata);
    restored.process();
    REQUIRE(sla::restore_project_layers(restored, reopened, metadata, *saved));
    REQUIRE(restored.layer_document()->image(0).get(0, 0) == 127);
    auto different=loaded_config;
    different.set("material_ow_support_head_front_diameter", .6);
    REQUIRE_FALSE(sla::restore_project_layers(restored,reopened,different,*saved));
    REQUIRE(restored.layer_document()->image(0).get(0,0)==127);
  }
  SECTION("capture cannot label stale print masks with a new scene") {
    Model moved=reopened;moved.objects[0]->instances[0]->set_offset(Vec3d(11.,10.,0.));
    REQUIRE_THROWS(sla::capture_project_layers(moved,loaded_config,restored));
    auto different=loaded_config;different.set("display_pixels_x",201);
    REQUIRE_THROWS(sla::capture_project_layers(reopened,different,restored));
  }
  SECTION("restore checks the actual print source not just the claimed scene") {
    Model moved=reopened;moved.objects[0]->instances[0]->set_offset(Vec3d(11.,10.,0.));
    restored.apply(moved,loaded_config);restored.process();
    const auto before=restored.layer_document()->image(0).buf;
    REQUIRE_FALSE(sla::restore_project_layers(restored,reopened,loaded_config,*saved));
    REQUIRE(restored.layer_document()->image(0).buf==before);
  }
  SECTION("pending model project refresh keeps pixels and current metadata atomically") {
    const auto original_settings=saved->settings;REQUIRE(original_settings.has_value());
    auto current=loaded_config;current.set("exposure_time",4.);current.set("sla_archive_format","uvj");
    REQUIRE(sla::refresh_model_project_settings(*saved,reopened,current));
    REQUIRE(saved->settings->material.values.at("exposure_time")=="4");
    REQUIRE(saved->settings->printer.values.at("sla_archive_format")=="uvj");
    REQUIRE(saved->source_fingerprint==captured.source_fingerprint);
    REQUIRE(saved->document.image(0).get(0,0)==127);
    const auto updated_settings=saved->settings;
    Model moved=reopened;moved.objects[0]->instances[0]->set_offset(Vec3d(11.,10.,0.));
    REQUIRE_FALSE(sla::refresh_model_project_settings(*saved,moved,current));
    REQUIRE(saved->settings==updated_settings);
    current.set("max_print_height",.01);
    REQUIRE_THROWS(sla::refresh_model_project_settings(*saved,reopened,current));
    REQUIRE(saved->settings==updated_settings);
    REQUIRE(saved->document.image(0).get(0,0)==127);
  }
  SECTION("moving geometry or changing the screen rejects the cache without "
          "mutation") {
    reopened.objects[0]->instances[0]->set_offset(Vec3d(11., 10., 0.));
    REQUIRE_FALSE(
        sla::restore_project_layers(restored, reopened, loaded_config, *saved));
    REQUIRE(restored.layer_document()->image(0).get(0, 0) == 127);
    reopened.objects[0]->instances[0]->set_offset(Vec3d(10., 10., 0.));
    auto different = loaded_config;
    different.set("display_pixels_x", 201);
    REQUIRE_FALSE(
        sla::restore_project_layers(restored, reopened, different, *saved));
    REQUIRE(restored.layer_document()->image(0).get(0, 0) == 127);
    auto heights = *saved;
    heights.positions_z[0] += .001;
    if (const auto &owned=heights.document.fresh_job()) {
      // Keep the typed snapshot internally consistent, so this still checks
      // engine-Z mismatch rather than corrupt descriptor/positions metadata.
      auto job=*owned;
      job.parameter_origin=sla::FreshLayerParameterOrigin::Explicit;
      job.rows[0][sla::LayerParameter::PositionZ]=heights.positions_z[0];
      std::vector<sla::EncodedRaster> masks;
      for(size_t i=0;i<heights.document.layer_count();++i) {
        const auto bytes=heights.document.encoded_layer(i);
        const auto *begin=static_cast<const uint8_t *>(bytes.buf);
        masks.emplace_back(std::vector<uint8_t>(begin,begin+bytes.sz),"png");
      }
      heights.document=sla::LayerDocument::from_fresh_rasters(std::move(masks),std::move(job));
    }
    REQUIRE_FALSE(sla::restore_project_layers(restored, reopened, loaded_config,
                                              heights));
  }
  SECTION("a failed project payload write preserves the previous entire file") {
    auto contents = [&]() {
      std::ifstream input(file.path.string(), std::ios::binary);
      return std::vector<char>{std::istreambuf_iterator<char>(input),
                               std::istreambuf_iterator<char>()};
    };
    const auto original = contents();
    auto invalid = captured;
    invalid.positions_z.pop_back();
    REQUIRE_FALSE(store_3mf(file.path.string().c_str(), &model, &config, false,
                            nullptr, true, &invalid));
    REQUIRE(contents() == original);
    REQUIRE(sla::read_project_layers(file.path.string()).has_value());
  }
  SECTION("ordinary 3MF projects remain compatible without raster payload") {
    REQUIRE(store_3mf(file.path.string().c_str(), &model, &config, false));
    REQUIRE_FALSE(sla::read_project_layers(file.path.string()).has_value());
  }
  SECTION("triangle order and object names do not change the source fingerprint") {
    Model equivalent = reopened;
    equivalent.objects[0]->name = "renamed";
    auto mesh = equivalent.objects[0]->volumes[0]->mesh().its;
    std::reverse(mesh.indices.begin(), mesh.indices.end());
    equivalent.objects[0]->volumes[0]->set_mesh(std::move(mesh));
    REQUIRE(sla::raster_source_fingerprint(equivalent, loaded_config) == saved->source_fingerprint);
  }
  SECTION(
      "queued restoration occurs before a direct export following slicing") {
    SLAPrint queued;
    queued.set_status_callback([](const PrintBase::SlicingStatus &) {});
    queued.queue_project_layers(
        std::make_shared<const sla::ProjectRasterState>(*saved));
    queued.apply(reopened, loaded_config);
    queued.process();
    REQUIRE_FALSE(queued.project_layer_restore_failed());
    REQUIRE(queued.layer_document()->image(0).get(0, 0) == 127);
    const auto output = boost::filesystem::unique_path("queued-layer-%%%%.sl1");
    struct Cleanup {
      boost::filesystem::path path;
      ~Cleanup() {
        boost::system::error_code error;
        boost::filesystem::remove(path, error);
      }
    } cleanup{output};
    queued.export_print(output.string());
    REQUIRE(sla::LayerDocument::load_sl1(output.string()).image(0).get(0, 0) ==
            127);
    reopened.objects[0]->instances[0]->set_offset(Vec3d(11., 10., 0.));
    queued.queue_project_layers(
        std::make_shared<const sla::ProjectRasterState>(*saved));
    queued.apply(reopened, loaded_config);
    queued.process();
    REQUIRE(queued.project_layer_restore_failed());
    REQUIRE(queued.layer_document()->image(0).get(0, 0) == 0);
  }
}

TEST_CASE("Range edits are atomic cancellable and one undo step",
          "[sla_layers]") {
  png::ImageGreyscale image;
  image.cols = 4;
  image.rows = 3;
  image.buf = {0, 17, 127, 255, 1, 2, 3, 4, 5, 6, 7, 8};
  std::vector<sla::EncodedRaster> masks;
  for (unsigned i = 0; i < 4; ++i)
    masks.push_back(
        sla::PNGRasterEncoder{}(image.buf.data(), image.cols, image.rows, 1));
  auto doc = sla::LayerDocument::from_rasters(std::move(masks));
  using Op = sla::LayerDocument::MaskOperation;
  REQUIRE_FALSE(doc.transform_layers(
      0, 3, Op::Invert, 0, [](size_t done, size_t) { return done < 2; }));
  REQUIRE_FALSE(doc.can_undo());
  REQUIRE_FALSE(doc.dirty());
  for (size_t i = 0; i < 4; ++i)
    REQUIRE(doc.image(i).buf == image.buf);
  REQUIRE(doc.transform_layers(1, 3, Op::MirrorHorizontal));
  const auto mirrored = doc.image(1).buf;
  REQUIRE(mirrored[0] == 255);
  REQUIRE(mirrored[3] == 0);
  REQUIRE(doc.image(0).buf == image.buf);
  REQUIRE(doc.undo());
  REQUIRE_FALSE(doc.can_undo());
  for (size_t i = 0; i < 4; ++i)
    REQUIRE(doc.image(i).buf == image.buf);
  REQUIRE(doc.redo());
  doc.mark_saved();
  REQUIRE(doc.transform_layers(1, 3, Op::MirrorHorizontal));
  REQUIRE(doc.image(1).buf == image.buf);
  REQUIRE(doc.dirty());
  REQUIRE(doc.undo());
  REQUIRE_FALSE(doc.dirty());
  REQUIRE_THROWS(doc.transform_layers(3, 2, Op::Invert));
  REQUIRE_THROWS(doc.transform_layers(0, 4, Op::Invert));
  REQUIRE_FALSE(doc.dirty());
  REQUIRE(doc.transform_layers(0, 0, Op::MirrorVertical));
  REQUIRE(doc.image(0).get(0, 0) == 5);
  REQUIRE(doc.undo());
  REQUIRE(doc.transform_layers(0, 0, Op::Rotate180));
  REQUIRE(doc.image(0).get(0, 0) == 8);
  REQUIRE(doc.undo());
  REQUIRE(doc.transform_layers(0, 0, Op::Threshold, 127));
  REQUIRE(doc.image(0).get(0, 1) == 0);
  REQUIRE(doc.image(0).get(0, 2) == 255);
  REQUIRE(doc.undo());
  REQUIRE(doc.transform_layers(0, 3, Op::Fill, 127));
  REQUIRE(doc.image(0).get(2, 3) == 127);
  REQUIRE(doc.image(3).get(0, 0) == 127);
  REQUIRE(doc.undo());
  REQUIRE(doc.image(0).buf == image.buf);
}

TEST_CASE("A continuous brush stroke remains a single reversible edit", "[sla_layers]")
{
    png::ImageGreyscale image;
    image.cols = 16; image.rows = 12; image.buf.resize(16 * 12);
    std::vector<sla::EncodedRaster> masks;
    masks.push_back(sla::PNGRasterEncoder{}(image.buf.data(), image.cols, image.rows, 1));
    auto doc = sla::LayerDocument::from_rasters(std::move(masks));
    sla::LayerDocument::draw_line(image, 1, 2, 13, 2, 0, 127);
    sla::LayerDocument::draw_line(image, 13, 2, 13, 9, 0, 255);
    doc.replace_image(0, image);
    for (size_t x = 1; x < 13; ++x) REQUIRE(doc.image(0).get(2, x) == 127);
    for (size_t y = 2; y <= 9; ++y) REQUIRE(doc.image(0).get(y, 13) == 255);
    REQUIRE(doc.undo());
    REQUIRE_FALSE(doc.can_undo());
    REQUIRE_FALSE(doc.dirty());
    REQUIRE(doc.image(0).get(2, 5) == 0);
    REQUIRE(doc.redo());
    REQUIRE(doc.image(0).buf == image.buf);
    REQUIRE_THROWS(sla::LayerDocument::draw_line(image, -1, 0, 1, 0, 0, 255));
    REQUIRE_THROWS(doc.save_sl1("invalid-sliced-document.sl1"));
}

TEST_CASE("Applied layer edits reach SL1 and another native container without reslicing", "[sla_layers]")
{
    struct Files {
        boost::filesystem::path sl1 = boost::filesystem::unique_path("native-edit-%%%%.sl1");
        boost::filesystem::path cws = boost::filesystem::unique_path("native-edit-%%%%.cws");
        ~Files() { boost::system::error_code ec; boost::filesystem::remove(sl1, ec); boost::filesystem::remove(cws, ec); }
    } files;
    Model model;
    auto *object = model.add_object("edit", "", make_cube(4., 3., 0.15));
    object->add_instance()->set_offset(Vec3d(10., 10., 0.));
    SLAFullPrintConfig full;
    full.printer_technology.setInt(ptSLA);
    full.set("sla_archive_format", std::string("SL1"));
    full.set("display_width", 100.); full.set("display_height", 60.);
    full.set("display_pixels_x", 200); full.set("display_pixels_y", 120);
    full.set("supports_enable", false); full.set("pad_enable", false);
    full.set("support_object_elevation", 0.);
    full.set("layer_height", 0.05); full.set("initial_layer_height", 0.05);
    DynamicPrintConfig config; config.apply(full);
    SLAPrint print; print.set_status_callback([](const PrintBase::SlicingStatus &) {});
    print.apply(model, config); print.process();
    REQUIRE(print.finished());
    REQUIRE(print.layer_document() != nullptr);
    auto edited = *print.layer_document();
    const auto before = edited.image(0);
    REQUIRE(before.get(0, 0) == 0);
    edited.paint(0, 0, 0, 0, 255);
    auto mask_volume = [&](const sla::LayerDocument &document) {
        double volume=0., previous=0.;
        for(size_t i=0;i<document.layer_count();++i) {
            const auto mask=document.image(i);
            const double z=double(print.print_layers()[i].level())*SCALING_FACTOR;
            volume+=std::count_if(mask.buf.begin(),mask.buf.end(),[](uint8_t value){return value!=0;})*
                100.*60./(200.*120.)*(z-previous);
            previous=z;
        }
        return volume;
    };
    // Exporting an unapplied document must update archive metadata while
    // leaving both the active masks and active statistics unchanged.
    const auto original_statistics=print.print_statistics().objects_used_material;
    ThumbnailData stale_preview;
    stale_preview.set(unsigned(before.cols),unsigned(before.rows)); // White 3D thumbnail must be replaced after editing.
    auto check_preview = [&]() {
        const auto images = read_zipper_archive(files.sl1.string(), {"thumbnail"}, {});
        REQUIRE(images.entries.size() == 1);
        std::vector<unsigned char> rgba;
        unsigned width=0,height=0;
        const auto &bytes=images.entries.front().buf;
        REQUIRE(png::decode_png(std::string(bytes.begin(),bytes.end()),rgba,width,height));
        REQUIRE(width==before.cols); REQUIRE(height==before.rows);
        // PNG coordinates match edited masks despite bottom-up thumbnail storage.
        REQUIRE(rgba[0]==255); REQUIRE(rgba[1]==255); REQUIRE(rgba[2]==255);
        REQUIRE(rgba[4]==0); REQUIRE(rgba[(size_t(height)-1)*width*4]==0);
    };
    print.export_layer_document(edited,"SL1",files.sl1.string(),{stale_preview});
    check_preview();
    REQUIRE(read_zipper_archive(files.sl1.string(),{"ini"},{}).config.get<double>("usedMaterial") ==
        Catch::Approx(mask_volume(edited)/1000.).margin(1e-6));
    REQUIRE(print.layer_document()->image(0).get(0,0)==0);
    REQUIRE(print.print_statistics().objects_used_material==original_statistics);
    print.replace_layer_document(edited);
    REQUIRE(print.print_statistics().objects_used_material==Catch::Approx(mask_volume(edited)));
    print.export_print(files.sl1.string(),{stale_preview});
    check_preview();
    REQUIRE(sla::LayerDocument::load_sl1(files.sl1.string()).image(0).get(0, 0) == 255);
    print.export_print(files.cws.string());
    const auto archive = read_zipper_archive(files.cws.string(), {"png"}, {});
    REQUIRE(archive.entries.size() == edited.layer_count());
    for (size_t i = 0; i < archive.entries.size(); ++i) {
        png::ImageGreyscale decoded;
        const auto &bytes = archive.entries[i].buf;
        REQUIRE(png::decode_png(png::ReadBuf{bytes.data(), bytes.size()}, decoded));
        REQUIRE(decoded.buf == edited.image(i).buf);
    }
    REQUIRE(print.finished());
    REQUIRE(print.printer_config().sla_archive_format.value == "SL1");
    SECTION("Exposure and motion changes preserve manual edits") {
        config.set("exposure_time", 4.);
        config.set("sla_lift_height", 8.);
        print.apply(model, config);
        REQUIRE(print.layer_document() != nullptr);
        print.process();
        REQUIRE(print.finished());
        REQUIRE(print.layer_document()->image(0).get(0, 0) == 255);
        REQUIRE(print.print_statistics().objects_used_material==Catch::Approx(mask_volume(edited)));
        print.export_print(files.sl1.string());
        REQUIRE(sla::LayerDocument::load_sl1(files.sl1.string()).image(0).get(0, 0) == 255);
    }
    SECTION("Changing CBDDLP AA re-encodes the file while preserving edited pixels and undo") {
        config.set("sla_archive_format",std::string("cbddlp"));
        config.set("sla_cbddlp_aa",4);
        print.apply(model,config);
        REQUIRE(print.layer_document()!=nullptr);
        REQUIRE(print.layer_document()->image(0).buf==edited.image(0).buf);
        REQUIRE(print.layer_document()->can_undo());
        print.process();REQUIRE(print.finished());
        REQUIRE(print.layer_document()->image(0).buf==edited.image(0).buf);
        const auto output=boost::filesystem::path(files.sl1.string()).replace_extension(".cbddlp");
        struct Cleanup {boost::filesystem::path path;~Cleanup(){boost::system::error_code ignored;boost::filesystem::remove(path,ignored);}} cleanup{output};
        print.export_print(output.string());
        std::ifstream stream(output.string(),std::ios::binary);stream.seekg(92);
        unsigned char planes[4]{};stream.read(reinterpret_cast<char *>(planes),4);
        REQUIRE(planes[0]==4);REQUIRE(planes[1]==0);REQUIRE(planes[2]==0);REQUIRE(planes[3]==0);
        config.set("sla_cbddlp_aa",16);print.apply(model,config);print.process();
        REQUIRE(print.layer_document()->image(0).buf==edited.image(0).buf);
        print.export_print(output.string());
        std::ifstream updated(output.string(),std::ios::binary);updated.seekg(92);updated.read(reinterpret_cast<char *>(planes),4);
        REQUIRE(planes[0]==16);
    }
    SECTION("Display changes discard masks that no longer match the printer") {
        config.set("display_pixels_x", 300);
        print.apply(model, config);
        REQUIRE(print.layer_document() == nullptr);
        print.process();
        REQUIRE(print.layer_document()->image(0).get(0, 0) == 0);
    }
    SECTION("Anycubic version changes preserve edited masks and undo before export") {
        config.set("sla_archive_format",std::string("pwc"));
        const auto output=boost::filesystem::path(files.sl1.string()).replace_extension(".pwc");
        struct Cleanup {boost::filesystem::path path;~Cleanup(){boost::system::error_code ignored;boost::filesystem::remove(path,ignored);}} cleanup{output};
        for (const std::string version : {"516", "517", "518"}) {
            CAPTURE(version);
            config.set("sla_anycubic_version",version);
            print.apply(model,config);
            REQUIRE(print.layer_document()!=nullptr);
            REQUIRE(print.layer_document()->image(0).buf==edited.image(0).buf);
            REQUIRE(print.layer_document()->can_undo());
            print.process(); REQUIRE(print.finished());
            REQUIRE(print.layer_document()->image(0).buf==edited.image(0).buf);
            REQUIRE(print.layer_document()->can_undo());
            print.export_print(output.string());
            std::ifstream stream(output.string(),std::ios::binary); stream.seekg(12);
            unsigned char bytes[4]{}; stream.read(reinterpret_cast<char *>(bytes),4);
            REQUIRE((uint32_t(bytes[0])|(uint32_t(bytes[1])<<8)|(uint32_t(bytes[2])<<16)|(uint32_t(bytes[3])<<24))==std::stoul(version));
        }
    }
    SECTION("Moving the model invalidates its previous edited masks") {
        object->instances.front()->set_offset(Vec3d(15., 10., 0.));
        print.apply(model, config);
        REQUIRE(print.layer_document() == nullptr);
        print.process();
        REQUIRE(print.layer_document()->image(0).get(0, 0) == 0);
    }
    SECTION("An unfinished printer container still allows slicing and editing") {
        config.set("sla_archive_format", std::string("n7"));
        print.apply(model, config);
        print.process();
        REQUIRE(print.finished());
        REQUIRE(print.layer_document()->image(0).get(0, 0) == 255);
        print.export_print(files.sl1.string());
        REQUIRE(sla::LayerDocument::load_sl1(files.sl1.string()).image(0).get(0, 0) == 255);
    }
    SECTION("Changing to vector layers does not reuse pixel masks or mislabel raster exports") {
        config.set("sla_archive_format", std::string("SL1SVG"));
        print.apply(model, config);
        REQUIRE(print.layer_document() == nullptr);
        print.process();
        REQUIRE(print.finished());
        REQUIRE(print.layer_document() == nullptr);
        REQUIRE_THROWS(print.export_print(files.sl1.string()));
    }
    SECTION("Blocked export preserves an existing destination") {
        const auto path = files.cws.parent_path() / "blocked-output.n7";
        std::ofstream(path.string(), std::ios::binary) << "original file";
        REQUIRE_THROWS(print.export_print(path.string()));
        std::ifstream file(path.string(), std::ios::binary);
        const std::string contents{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
        REQUIRE(contents == "original file");
        file.close(); boost::filesystem::remove(path);
    }
}

TEST_CASE("Layer edits survive archive save with metadata and unknown entries intact", "[sla_layers]")
{
    struct Files {
        boost::filesystem::path input = boost::filesystem::unique_path("layers-%%%%-%%%%.sl1");
        boost::filesystem::path output = boost::filesystem::unique_path("edited-%%%%-%%%%.sl1");
        ~Files() { boost::system::error_code ec; boost::filesystem::remove(input, ec); boost::filesystem::remove(output, ec); }
    } files;
    std::vector<uint8_t> pixels(32, 0);
    pixels[7] = 127;
    auto png = sla::PNGRasterEncoder{}(pixels.data(), 8, 4, 1);
    {
        Zipper zip(files.input.string());
        zip.add_entry("config.ini"); zip << "layerHeight = 0.05\n";
        zip.add_entry("prusaslicer.ini"); zip << "display_width = 8\n";
        zip.add_entry("vendor.dat"); zip << "opaque vendor payload";
        zip.add_entry("logo.png", png.data(), png.size());
        zip.add_entry("thumbnail/thumbnail.png", png.data(), png.size());
        zip.add_entry("job00001.png", png.data(), png.size());
        zip.add_entry("job00000.png", png.data(), png.size());
        zip.finalize();
    }
    auto doc = sla::LayerDocument::load_sl1(files.input.string());
    REQUIRE(doc.layer_count() == 2);
    REQUIRE_FALSE(doc.dirty());
    REQUIRE(doc.image(0).buf == pixels);
    SECTION("Invert preserves grayscale and affects only the selected layer") {
        doc.invert(0);
        const auto inverted = doc.image(0).buf;
        for (size_t i = 0; i < pixels.size(); ++i)
            REQUIRE(inverted[i] == 255 - pixels[i]);
        REQUIRE(doc.image(1).buf == pixels);
        REQUIRE(doc.dirty());
        doc.save_sl1(files.output.string());
        REQUIRE(sla::LayerDocument::load_sl1(files.output.string()).image(0).buf == inverted);
        REQUIRE(doc.undo());
        REQUIRE(doc.image(0).buf == pixels);
        REQUIRE(doc.redo());
        REQUIRE(doc.image(0).buf == inverted);
        doc.invert(0);
        REQUIRE(doc.image(0).buf == pixels);
        REQUIRE_THROWS(doc.invert(doc.layer_count()));
    }
    SECTION("Paint, threshold and save retain metadata and history") {
        doc.paint(0, 0, 0, 1, 255);
        auto painted = doc.image(0).buf;
        REQUIRE(painted[0] == 255);
        REQUIRE(painted[1] == 255);
        REQUIRE(painted[8] == 255);
        REQUIRE(painted[9] == 0);
        REQUIRE(doc.image(1).buf == pixels);
        doc.paint(0, 0, 0, 1, 255); // No-op must not add an undo step.
        REQUIRE(doc.undo());
        REQUIRE_FALSE(doc.dirty());
        REQUIRE(doc.image(0).buf == pixels);
        REQUIRE(doc.redo());
        REQUIRE(doc.image(0).buf == painted);
        REQUIRE_THROWS(doc.paint(0, -1, 0, 1, 255));
        REQUIRE_THROWS(doc.paint(0, 0, 0, 4097, 255));
        REQUIRE_THROWS(doc.image(2));
        REQUIRE_THROWS(doc.save_sl1((files.input / "missing" / "output.sl1").string()));
        REQUIRE(doc.dirty());
        REQUIRE(sla::LayerDocument::load_sl1(files.input.string()).image(0).buf == pixels);
        doc.save_sl1(files.output.string());
        REQUIRE_FALSE(doc.dirty());
        auto reopened = sla::LayerDocument::load_sl1(files.output.string());
        REQUIRE(reopened.image(0).buf == painted);
        REQUIRE(reopened.image(1).buf == pixels);
        const auto before = read_zipper_archive(files.input.string(), {"ini", "dat", "thumbnail", "logo"}, {});
        const auto after = read_zipper_archive(files.output.string(), {"ini", "dat", "thumbnail", "logo"}, {});
        REQUIRE(before.config == after.config);
        REQUIRE(before.profile == after.profile);
        REQUIRE(before.entries.size() == after.entries.size());
        for (size_t i = 0; i < before.entries.size(); ++i) {
            REQUIRE(before.entries[i].fname == after.entries[i].fname);
            REQUIRE(before.entries[i].buf == after.entries[i].buf);
        }
        doc.undo();
        doc.threshold(0, 128);
        REQUIRE_FALSE(doc.can_redo());
        REQUIRE(doc.image(0).buf[7] == 0);
        // Replacing an existing output also uses the temporary-file save path.
        doc.save_sl1(files.output.string());
        REQUIRE(sla::LayerDocument::load_sl1(files.output.string()).image(0).buf[7] == 0);
    }
}

TEST_CASE("Grayscale morphology agrees with an independent square kernel", "[sla_layers]")
{
    using Op = sla::LayerDocument::MaskOperation;
    for (size_t width : {size_t(1), size_t(2), size_t(7)})
    for (size_t height : {size_t(1), size_t(3), size_t(6)})
    for (unsigned radius : {0u, 1u, 4u}) {
        CAPTURE(width, height, radius);
        std::vector<uint8_t> original(width * height);
        for (size_t i = 0; i < original.size(); ++i) original[i] = uint8_t((i * 73 + 19) % 256);
        auto reference = [&](const auto &input, bool dilate) {
            auto reflect = [](int index, int length) {
                if (length == 1) return 0;
                while (index < 0 || index >= length)
                    index = index < 0 ? -index : 2 * length - 2 - index;
                return index;
            };
            std::vector<uint8_t> output(input.size());
            for (int y = 0; y < int(height); ++y)
            for (int x = 0; x < int(width); ++x) {
                uint8_t value = dilate ? 0 : 255;
                for (int dy = -int(radius); dy <= int(radius); ++dy)
                for (int dx = -int(radius); dx <= int(radius); ++dx) {
                    const auto pixel = input[size_t(reflect(y+dy, int(height))) * width + size_t(reflect(x+dx, int(width)))];
                    value = dilate ? std::max(value, pixel) : std::min(value, pixel);
                }
                output[size_t(y) * width + size_t(x)] = value;
            }
            return output;
        };
        const auto eroded = reference(original, false), dilated = reference(original, true);
        const auto opened = reference(eroded, true), closed = reference(dilated, false);
        for (Op op : {Op::Erode, Op::Dilate, Op::Open, Op::Close, Op::Gradient, Op::WhiteTopHat, Op::BlackTopHat}) {
            CAPTURE(int(op));
            std::vector<sla::EncodedRaster> layers;
            for (unsigned i = 0; i < 2; ++i)
                layers.push_back(sla::PNGRasterEncoder{}(original.data(), width, height, 1));
            auto doc = sla::LayerDocument::from_rasters(std::move(layers));
            auto expected = original;
            for (size_t i = 0; i < expected.size(); ++i) {
                switch (op) {
                case Op::Erode: expected[i] = eroded[i]; break;
                case Op::Dilate: expected[i] = dilated[i]; break;
                case Op::Open: expected[i] = opened[i]; break;
                case Op::Close: expected[i] = closed[i]; break;
                case Op::Gradient: expected[i] = dilated[i]-eroded[i]; break;
                case Op::WhiteTopHat: expected[i] = original[i]-opened[i]; break;
                case Op::BlackTopHat: expected[i] = closed[i]-original[i]; break;
                default: FAIL("Unexpected operation");
                }
            }
            REQUIRE(doc.transform_layers(0, 1, op, 255, {}, radius));
            REQUIRE(doc.image(0).buf == expected);
            REQUIRE(doc.image(1).buf == expected);
            if (expected != original) {
                REQUIRE(doc.undo()); REQUIRE_FALSE(doc.can_undo());
                REQUIRE(doc.image(0).buf == original); REQUIRE(doc.image(1).buf == original);
                REQUIRE(doc.redo()); REQUIRE(doc.image(1).buf == expected);
            }
            REQUIRE_THROWS(doc.transform_layers(0, 1, op, 255, {}, 4097));
            REQUIRE(doc.image(1).buf == expected);
        }
    }
}

TEST_CASE("UVJ archives reopen edit and preserve their original metadata", "[sla_layers]")
{
    struct Files {
        boost::filesystem::path input = boost::filesystem::unique_path("uvj-input-%%%%.uvj");
        boost::filesystem::path output = boost::filesystem::unique_path("uvj-output-%%%%.uvj");
        ~Files() { boost::system::error_code error; boost::filesystem::remove(input, error); boost::filesystem::remove(output, error); }
    } files;
    std::vector<uint8_t> pixels{0, 17, 127, 255, 1, 2, 3, 4, 5, 6, 7, 8};
    const auto encoded = sla::PNGRasterEncoder{}(pixels.data(), 4, 3, 1);
    const std::string metadata = R"({"Properties":{"Size":{"X":4,"Y":3,"Millimeter":{"X":8,"Y":6},"Layers":2,"LayerHeight":0.05},"Vendor":{"unknown":[1,2,3]}},"Layers":[{"Z":0.1},{"Z":0.15}]})";
    auto write = [&](const std::string &json, const std::string &first = "slice/00000000.png",
                     const std::string &second = "slice/00000001.png") {
        Zipper zip(files.input.string());
        zip.add_entry("config.json"); zip << json;
        zip.add_entry(first, encoded.data(), encoded.size()); zip.add_entry(second, encoded.data(), encoded.size());
        zip.add_entry("vendor.dat"); zip << "opaque metadata";
        zip.finalize();
    };
    write(metadata);
    auto document = sla::LayerDocument::load_archive(files.input.string());
    REQUIRE(document.archive_format() == "uvj"); REQUIRE(document.layer_count() == 2);
    REQUIRE_THROWS(sla::LayerDocument::load_sl1(files.input.string()));
    SECTION("Edited PNGs round trip without changing unknown metadata") {
        REQUIRE(document.image(0).buf == pixels); REQUIRE_FALSE(document.dirty());
        document.invert(0); const auto edited = document.image(0).buf;
        REQUIRE_THROWS(document.save_sl1(files.output.string()));
        REQUIRE_THROWS(document.save_archive(files.output.string()+".sl1"));
        REQUIRE(document.dirty()); REQUIRE_FALSE(boost::filesystem::exists(files.output));
        document.save_archive(files.output.string()); REQUIRE_FALSE(document.dirty());
        const auto reopened = sla::LayerDocument::load_archive(files.output.string());
        REQUIRE(reopened.image(0).buf == edited); REQUIRE(reopened.image(1).buf == pixels);
        const auto before = read_zipper_archive(files.input.string(), {"json", "dat"}, {});
        const auto after = read_zipper_archive(files.output.string(), {"json", "dat"}, {});
        REQUIRE(before.entries.size() == after.entries.size());
        for (size_t i = 0; i < before.entries.size(); ++i) REQUIRE(before.entries[i].buf == after.entries[i].buf);
        REQUIRE(document.undo()); REQUIRE(document.image(0).buf == pixels);
        REQUIRE(document.redo()); REQUIRE(document.image(0).buf == edited);
    }
    SECTION("Duplicate names are rejected case insensitively") {
        write(metadata, "slice/00000000.png", "SLICE/00000000.PNG");
        REQUIRE_THROWS(sla::LayerDocument::load_archive(files.input.string()));
    }
    SECTION("Missing indices are rejected") {
        write(metadata, "slice/00000001.png", "slice/00000002.png");
        REQUIRE_THROWS(sla::LayerDocument::load_archive(files.input.string()));
    }
    SECTION("Mismatched raster dimensions are rejected before preview allocation") {
        auto json = metadata; json.replace(json.find("\"X\":4"), 5, "\"X\":5"); write(json);
        REQUIRE_THROWS(sla::LayerDocument::load_archive(files.input.string()));
    }
    SECTION("Nonincreasing Z is rejected") {
        auto json = metadata; json.replace(json.find("0.15"), 4, "0.05"); write(json);
        REQUIRE_THROWS(sla::LayerDocument::load_archive(files.input.string()));
    }
    SECTION("Count cannot exceed the actual entries") {
        auto json = metadata; json.replace(json.find("\"Layers\":2"), 10, "\"Layers\":3"); write(json);
        REQUIRE_THROWS(sla::LayerDocument::load_archive(files.input.string()));
    }
}

TEST_CASE("Morphology cancellation can stop within a single mask", "[sla_layers]")
{
    std::vector<uint8_t> pixels(64 * 64, 0); pixels[32 * 64 + 32] = 127;
    std::vector<sla::EncodedRaster> layers;
    layers.push_back(sla::PNGRasterEncoder{}(pixels.data(), 64, 64, 1));
    auto doc = sla::LayerDocument::from_rasters(std::move(layers));
    unsigned calls = 0;
    REQUIRE_FALSE(doc.transform_layers(0, 0, sla::LayerDocument::MaskOperation::Dilate, 255,
        [&](size_t done, size_t count) { REQUIRE(done == 0); REQUIRE(count == 1); return ++calls < 8; }, 4));
    REQUIRE(calls == 8); REQUIRE_FALSE(doc.dirty()); REQUIRE_FALSE(doc.can_undo());
    REQUIRE(doc.image(0).buf == pixels);
}

TEST_CASE("Generated SLA raster jobs export without a dummy model or slicing", "[sla_layers]")
{
    const bool portrait = GENERATE(false, true);
    std::vector<uint8_t> pixels{0, 17, 127, 255, 1, 2, 3, 4, 5, 6, 7, 8};
    std::vector<sla::EncodedRaster> masks;
    for (unsigned i = 0; i < 3; ++i)
        masks.push_back(sla::PNGRasterEncoder{}(pixels.data(), portrait ? 3 : 4, portrait ? 4 : 3, 1));
    const auto document = sla::LayerDocument::from_rasters(std::move(masks));
    DynamicPrintConfig config; config.apply(SLAFullPrintConfig{});
    config.set("display_width", 8.); config.set("display_height", 6.);
    config.set("display_pixels_x", 4); config.set("display_pixels_y", 3);
    config.set("sla_archive_format", std::string("SL1"));
    config.option("display_orientation")->setInt(int(portrait ? sladoPortrait : sladoLandscape));
    config.set("layer_height", 0.05); config.set("initial_layer_height", 0.05);
    const std::vector<double> positions{0.1, 0.15, 0.2};
    auto job = SLAPrint::create_raster_job(config, document, positions);
    REQUIRE(job->is_raster_job()); REQUIRE(job->objects().empty()); REQUIRE(job->finished());
    REQUIRE(job->material_config().initial_layer_height.value == 0.1);
    REQUIRE(job->full_print_config().opt_float("initial_layer_height") == 0.1);
    REQUIRE(job->print_statistics().objects_used_material == Catch::Approx(11 * 4 * 0.2));
    REQUIRE(job->print_statistics().estimated_print_time > 0);
    job->process(); REQUIRE(job->layer_document()->image(0).buf == pixels);
    struct Files {
        boost::filesystem::path sl1 = boost::filesystem::unique_path("generated-%%%%.sl1");
        boost::filesystem::path uvj = boost::filesystem::unique_path("generated-%%%%.uvj");
        ~Files() { boost::system::error_code error; boost::filesystem::remove(sl1, error); boost::filesystem::remove(uvj, error); }
    } files;
    job->export_print(files.sl1.string());
    const auto reopened = sla::LayerDocument::load_sl1(files.sl1.string());
    REQUIRE(reopened.image(0).buf == pixels); REQUIRE(reopened.layer_count() == 3);
    // Editing a separately opened file uses its embedded physical geometry,
    // including a different first-layer height, rather than an active preset.
    auto archive_edit = reopened;
    archive_edit.threshold(0,128);
    archive_edit.save_sl1(files.sl1.string());
    REQUIRE(read_zipper_archive(files.sl1.string(),{"ini"},{}).config.get<double>("usedMaterial") == Catch::Approx(4.8/1000.));
    const auto metadata_entries=read_zipper_archive(files.sl1.string(),{"json"},{});
    const auto &metadata_bytes=metadata_entries.entries.front().buf;
    const auto metadata=nlohmann::json::parse(metadata_bytes);
    const double material=metadata["usedMaterial"].is_string() ? std::stod(metadata["usedMaterial"].get<std::string>()) : metadata["usedMaterial"].get<double>();
    REQUIRE(material==Catch::Approx(4.8/1000.));
    job->export_print(files.uvj.string());
    REQUIRE(sla::LayerDocument::load_archive(files.uvj.string()).image(2).buf == pixels);
    auto edited = document; edited.threshold(0, 128);
    job->export_layer_document(edited, "SL1", files.sl1.string(), {}, "generated");
    const auto standalone = read_zipper_archive(files.sl1.string(), {"ini"}, {});
    REQUIRE(standalone.config.get<double>("usedMaterial") == Catch::Approx(4.8 / 1000));
    REQUIRE(job->layer_document()->image(0).buf == pixels); // Export has no apply side effect.
    job->replace_layer_document(edited);
    REQUIRE(job->print_statistics().objects_used_material == Catch::Approx(4.8));
    auto variable = SLAPrint::create_raster_job(config, document, {0.1, 0.15, 0.35});
    REQUIRE_THROWS(variable->export_print(files.sl1.string()));
    REQUIRE(sla::LayerDocument::load_sl1(files.sl1.string()).image(0).buf == edited.image(0).buf);
    variable->export_print(files.uvj.string());
    REQUIRE(sla::LayerDocument::load_archive(files.uvj.string()).image(2).buf == pixels);
    REQUIRE_THROWS(SLAPrint::create_raster_job(config, document, {}));
    REQUIRE_THROWS(SLAPrint::create_raster_job(config, document, {0.1, 0.1, 0.2}));
    REQUIRE_THROWS(SLAPrint::create_raster_job(config, document, {0.1, 0.15, 300.}));
    config.set("display_pixels_x", 5);
    REQUIRE_THROWS(SLAPrint::create_raster_job(config, document, positions));
    config.set("display_pixels_x", 4); config.set("sla_lift_speed", 0.);
    REQUIRE_THROWS(SLAPrint::create_raster_job(config, document, positions));
    job->clear(); REQUIRE_FALSE(job->finished()); REQUIRE_FALSE(job->is_raster_job());
    REQUIRE(job->print_layers().empty()); REQUIRE(job->layer_document() == nullptr);
}

TEST_CASE("Identical raster masks encode once and failed encodes preserve the previous result", "[sla_layers]")
{
    class CountingWriter final : public SLAArchiveWriter {
    public:
        mutable std::atomic<unsigned> calls{0};
        bool fail = false;
        size_t oversized = 0;
        const auto &layers() const { return m_layers; }
        std::unique_ptr<sla::RasterBase> create_raster() const override { return {}; }
        sla::RasterEncoder get_encoder() const override {
            return [this](const void *data, size_t width, size_t height, size_t) {
                ++calls;
                if (fail) return sla::EncodedRaster{};
                if (oversized) return sla::EncodedRaster(std::vector<uint8_t>(oversized,0), "test");
                const auto *bytes = static_cast<const uint8_t *>(data);
                return sla::EncodedRaster(std::vector<uint8_t>(bytes,bytes+width*height), "test");
            };
        }
        void export_print(const std::string, const SLAPrint &, const ThumbnailsList &, const std::string &) override {}
    } writer;
    std::vector<uint8_t> first{0,17,85,170,255,1}, second{0,17,85,170,255,2};
    const auto a = sla::PNGRasterEncoder{}(first.data(),3,2,1), b = sla::PNGRasterEncoder{}(second.data(),3,2,1);
    auto doc = sla::LayerDocument::from_rasters({a,a,b,a,b});
    writer.encode_layers(doc);
    REQUIRE(writer.calls.load() == 2); REQUIRE(writer.layers().size() == 5);
    for (size_t i=0;i<5;++i) {
        const auto &encoded = writer.layers()[i];
        const auto *bytes = static_cast<const uint8_t *>(encoded.data());
        REQUIRE(std::vector<uint8_t>(bytes,bytes+encoded.size()) == (i==2 || i==4 ? second : first));
    }
    writer.fail = true;
    REQUIRE_THROWS(writer.encode_layers(doc));
    REQUIRE(writer.layers().size() == 5);
    REQUIRE(static_cast<const uint8_t *>(writer.layers()[0].data())[1] == 17);
    writer.fail = false; writer.oversized = 2 * 1024 * 1024;
    auto repeated = sla::LayerDocument::from_rasters(std::vector<sla::EncodedRaster>(1000,a));
    REQUIRE_THROWS(writer.encode_layers(repeated));
    REQUIRE(writer.layers().size() == 5);
    REQUIRE(static_cast<const uint8_t *>(writer.layers()[0].data())[1] == 17);
}

TEST_CASE("Box blur keeps grayscale rounding reflected borders and range history", "[sla_layers][sla_box_blur]") {
    using Op=sla::LayerDocument::MaskOperation;
    const std::vector<uint8_t> impulse{0,0,0,0,255,0,0,0,0};
    const std::vector<uint8_t> expected{113,57,113,57,28,57,113,57,113};
    const auto encoded=sla::PNGRasterEncoder{}(impulse.data(),3,3,1);
    auto document=sla::LayerDocument::from_rasters({encoded,encoded});
    REQUIRE(document.transform_layers(0,1,Op::BoxBlur,255,{},1));
    REQUIRE(document.image(0).buf==expected);REQUIRE(document.image(1).buf==expected);
    REQUIRE(document.undo());REQUIRE_FALSE(document.dirty());
    REQUIRE(document.image(0).buf==impulse);REQUIRE(document.image(1).buf==impulse);
    REQUIRE_FALSE(document.undo());REQUIRE(document.redo());
    REQUIRE(document.image(0).buf==expected);
    REQUIRE(document.undo());
    const auto revision=document.revision();
    unsigned callbacks=0;
    REQUIRE_FALSE(document.transform_layers(0,1,Op::BoxBlur,255,[&](size_t,size_t){return ++callbacks<5;},1));
    REQUIRE(document.revision()==revision);REQUIRE(document.image(0).buf==impulse);
    REQUIRE(document.image(1).buf==impulse);REQUIRE_FALSE(document.can_undo());
    REQUIRE_THROWS(document.transform_layers(0,1,Op::BoxBlur,255,{},4097));
    REQUIRE(document.revision()==revision);
    REQUIRE(document.transform_layers(0,1,Op::BoxBlur,255,{},0));
    REQUIRE(document.revision()==revision);
    for(bool column:{false,true}) {
        const std::vector<uint8_t> line{0,255};
        const auto pixels=sla::PNGRasterEncoder{}(line.data(),column?1:2,column?2:1,1);
        auto tiny=sla::LayerDocument::from_rasters({pixels});
        REQUIRE(tiny.transform_layers(0,0,Op::BoxBlur,255,{},4096));
        REQUIRE(tiny.image(0).buf==std::vector<uint8_t>{127,128});
    }
    const uint8_t white=255;
    auto single=sla::LayerDocument::from_rasters({sla::PNGRasterEncoder{}(&white,1,1,1)});
    REQUIRE(single.transform_layers(0,0,Op::BoxBlur,255,{},4096));
    REQUIRE(single.image(0).buf==std::vector<uint8_t>{255});
    REQUIRE_FALSE(single.dirty());
}

TEST_CASE("Median blur removes grayscale noise with replicated borders and range history", "[sla_layers][sla_median_blur]") {
    using Op=sla::LayerDocument::MaskOperation;
    const std::vector<uint8_t> impulse{17,17,17,17,255,17,17,17,17}, expected(9,17);
    const auto encoded=sla::PNGRasterEncoder{}(impulse.data(),3,3,1);
    auto document=sla::LayerDocument::from_rasters({encoded,encoded});
    REQUIRE(document.transform_layers(0,1,Op::MedianBlur,255,{},1));
    REQUIRE(document.image(0).buf==expected);REQUIRE(document.image(1).buf==expected);
    REQUIRE(document.undo());REQUIRE_FALSE(document.dirty());
    REQUIRE(document.image(0).buf==impulse);REQUIRE(document.image(1).buf==impulse);
    REQUIRE_FALSE(document.undo());REQUIRE(document.redo());
    REQUIRE(document.image(0).buf==expected);REQUIRE(document.undo());
    const auto revision=document.revision();
    unsigned callbacks=0;
    REQUIRE_FALSE(document.transform_layers(0,1,Op::MedianBlur,255,[&](size_t,size_t){return ++callbacks<5;},1));
    REQUIRE(document.revision()==revision);REQUIRE(document.image(0).buf==impulse);
    REQUIRE(document.image(1).buf==impulse);REQUIRE_FALSE(document.can_undo());
    REQUIRE(document.can_redo());
    REQUIRE_THROWS(document.transform_layers(0,1,Op::MedianBlur,255,{},4097));
    REQUIRE(document.revision()==revision);
    REQUIRE(document.transform_layers(0,1,Op::MedianBlur,255,{},0));
    REQUIRE(document.revision()==revision);
    for(bool column:{false,true}) {
        const std::vector<uint8_t> line{10,230};
        const auto pixels=sla::PNGRasterEncoder{}(line.data(),column?1:2,column?2:1,1);
        auto tiny=sla::LayerDocument::from_rasters({pixels});
        REQUIRE(tiny.transform_layers(0,0,Op::MedianBlur,255,{},4096));
        REQUIRE(tiny.image(0).buf==line);REQUIRE_FALSE(tiny.dirty());
    }
    const uint8_t white=255;
    auto single=sla::LayerDocument::from_rasters({sla::PNGRasterEncoder{}(&white,1,1,1)});
    REQUIRE(single.transform_layers(0,0,Op::MedianBlur,255,{},4096));
    REQUIRE(single.image(0).buf==std::vector<uint8_t>{255});REQUIRE_FALSE(single.dirty());
}

TEST_CASE("XML.CWS color masks decode without an RGBA working image", "[sla_layers][sla_cwsxml]") {
    const std::vector<uint8_t> rgba{17,17,17,255,255,255,255,128,255,0,0,255,0,255,0,255,0,0,255,255,255,255,255,0};
    const auto encoded=sla::PNGRasterEncoder{}(rgba.data(),3,2,4);
    png::ImageGreyscale gray;
    REQUIRE(png::decode_color_png({encoded.data(),encoded.size()},gray));
    REQUIRE(gray.cols==3);REQUIRE(gray.rows==2);
    // libpng's fixed-point RGB conversion truncates green's 149.685 to 149.
    // Canonical XML.CWS masks have equal RGB channels, which remain exact.
    REQUIRE(gray.buf==std::vector<uint8_t>{17,128,76,149,29,0});
    png::ImageGreyscale strict;
    REQUIRE_FALSE(png::decode_png(png::ReadBuf{encoded.data(),encoded.size()},strict));
    const auto *data=static_cast<const uint8_t *>(encoded.data());
    std::vector<uint8_t> corrupt(data,data+encoded.size());corrupt.back()^=1;
    REQUIRE_FALSE(png::decode_color_png({corrupt.data(),corrupt.size()},gray));
    REQUIRE_FALSE(png::decode_color_png({encoded.data(),encoded.size()/2},gray));
    std::vector<uint8_t> ramp(256*4);
    for(size_t i=0;i<256;++i) {
        ramp[4*i]=ramp[4*i+1]=ramp[4*i+2]=uint8_t(i);ramp[4*i+3]=255;
    }
    const auto gradient=sla::PNGRasterEncoder{}(ramp.data(),256,1,4);
    REQUIRE(png::decode_color_png({gradient.data(),gradient.size()},gray));
    for(size_t i=0;i<256;++i) REQUIRE(unsigned(gray.buf[i])==i);
}

TEST_CASE("Versioned project entry graphs reject malformed snapshots", "[sla_layers][sla_project_document]") {
    struct File {boost::filesystem::path path=boost::filesystem::unique_path("project-snapshot-%%%%.3mf");
        ~File(){boost::system::error_code e;boost::filesystem::remove(path,e);}} file;
    const std::vector<uint8_t> pixels{0,127,255,0,1,254};
    const auto mask=sla::PNGRasterEncoder{}(pixels.data(),3,2,1);
    nlohmann::json descriptor{{"archive_format",""},{"entries",{"layer0"}},{"main",{0}},
                              {"secondary",nlohmann::json::array()},{"parameters",nlohmann::json::array()}};
    nlohmann::json metadata{{"version",2},{"columns",3},{"rows",2},{"layers",1},
                            {"source_fingerprint",std::string(40,'a')},{"positions_z",{.05}}};
    bool missing=false,extra=false,duplicate=false,valid=false,legacy=false;
    SECTION("valid snapshot"){valid=true;}
    SECTION("legacy v1"){valid=true;legacy=true;metadata["version"]=1;}
    SECTION("missing payload"){missing=true;}
    SECTION("extra payload"){extra=true;}
    SECTION("duplicate payload casing"){duplicate=true;}
    SECTION("negative index"){descriptor["main"]={-1};}
    SECTION("fractional index"){descriptor["main"]={0.5};}
    SECTION("out of range index"){descriptor["main"]={1};}
    SECTION("duplicate main"){descriptor["main"]={0,0};metadata["layers"]=2;metadata["positions_z"]={.05,.1};}
    SECTION("invalid secondary count"){descriptor["secondary"]={0,0};}
    SECTION("parameter overlaps mask"){descriptor["parameters"]={0};}
    SECTION("unknown format"){descriptor["archive_format"]="invented";}
    SECTION("native without native source"){descriptor["archive_format"]="pwc";}
    SECTION("entry name contains NUL"){descriptor["entries"]={std::string("a\0b",3)};}
    SECTION("unsupported version"){metadata["version"]=3;}
    SECTION("layer count mismatch"){metadata["layers"]=2;metadata["positions_z"]={.05,.1};}
    SECTION("wrong dimensions"){metadata["columns"]=2;}
    metadata["document"]=descriptor;
    {
        Zipper zip(file.path.string());zip.add_entry("Metadata/SLA/layers.json");zip<<metadata.dump();
        if(!missing)zip.add_entry(legacy?"Metadata/SLA/layers/00000000.png":"Metadata/SLA/document/00000000.bin",mask.data(),mask.size());
        if(extra)zip.add_entry("Metadata/SLA/document/00000001.bin",mask.data(),mask.size());
        if(duplicate)zip.add_entry("metadata/sla/DOCUMENT/00000000.bin",mask.data(),mask.size());
        zip.finalize();
    }
    if(!valid){REQUIRE_THROWS(sla::read_project_layers(file.path.string()));return;}
    const auto restored=sla::read_project_layers(file.path.string());REQUIRE(restored.has_value());
    REQUIRE(restored->document.image(0).buf==pixels);REQUIRE_FALSE(restored->document.can_undo());
}

TEST_CASE("Layer project origins do not invent a model fingerprint or layer heights", "[sla_layers][sla_project_origin]") {
    struct File {
        boost::filesystem::path path=boost::filesystem::unique_path("project-origin-%%%%.sla-project");
        ~File(){boost::system::error_code e;boost::filesystem::remove(path,e);}
    } file;
    const std::vector<uint8_t> pixels{0,127,255,0,1,254};
    const auto mask=sla::PNGRasterEncoder{}(pixels.data(),3,2,1);
    nlohmann::json metadata{{"version",2},{"columns",3},{"rows",2},{"layers",1},
        {"source_fingerprint",""},{"positions_z",nlohmann::json::array()},
        {"source_kind","imported_archive"},
        {"document",{{"archive_format","png"},{"entries",{"mask.png"}},{"main",{0}},
            {"secondary",nlohmann::json::array()},{"parameters",nlohmann::json::array()}}}};
    bool valid=false;
    SECTION("imported image has unknown heights and no model") {valid=true;}
    SECTION("generated raster has known heights but no model") {
        valid=true;metadata["source_kind"]="generated_raster";metadata["positions_z"]={.05};
        metadata["document"]["archive_format"]="";
    }
    SECTION("unknown origin must not silently become model cache") {
        metadata["source_kind"]="invented";metadata["source_fingerprint"]=std::string(40,'a');metadata["positions_z"]={.05};
    }
    SECTION("null origin must not silently become model cache") {
        metadata["source_kind"]=nullptr;metadata["source_fingerprint"]=std::string(40,'a');metadata["positions_z"]={.05};
    }
    SECTION("imported layers cannot impersonate model cache") {metadata["source_fingerprint"]=std::string(40,'a');metadata["positions_z"]={.05};}
    SECTION("imported unknown Z cannot receive invented baseline heights") {metadata["positions_z"]={.05};}
    SECTION("generated raster must supply its heights") {metadata["source_kind"]="generated_raster";}
    SECTION("generated raster cannot carry a model fingerprint") {
        metadata["source_kind"]="generated_raster";metadata["positions_z"]={.05};metadata["source_fingerprint"]=std::string(40,'a');
    }
    SECTION("legacy v1 cannot opt out of its model contract") {metadata["version"]=1;}
    {
        Zipper zip(file.path.string());zip.add_entry("Metadata/SLA/layers.json");zip<<metadata.dump();
        zip.add_entry("Metadata/SLA/document/00000000.bin",mask.data(),mask.size());zip.finalize();
    }
    if(!valid){CHECK_THROWS(sla::read_project_layers(file.path.string()));return;}
    std::optional<sla::ProjectRasterState> restored;
    REQUIRE_NOTHROW(restored=sla::read_project_layers(file.path.string()));
    REQUIRE(restored.has_value());CHECK(restored->source_fingerprint.empty());
    CHECK(restored->document.image(0).buf==pixels);
    mz_zip_archive heap{};REQUIRE(mz_zip_writer_init_heap(&heap,0,0));
    CHECK_NOTHROW(sla::write_project_layers(heap,*restored));REQUIRE(mz_zip_writer_end(&heap));
    SLAPrint print;Model model;DynamicPrintConfig cfg;cfg.apply(SLAFullPrintConfig{});
    CHECK_FALSE(sla::restore_project_layers(print,model,cfg,*restored));
}

TEST_CASE("Standalone layer projects publish atomically and cancel without replacing user data", "[sla_layers][sla_project_origin][sla_project_publication]") {
    struct Directory {
        boost::filesystem::path path=boost::filesystem::unique_path("project-publication-%%%%");
        ~Directory(){boost::system::error_code e;boost::filesystem::remove_all(path,e);}
    } dir;
    boost::filesystem::create_directory(dir.path);
    const auto output=dir.path/"layers.sla-project";
    const std::vector<uint8_t> pixels{0,127,255,0,1,254};
    const auto mask=sla::PNGRasterEncoder{}(pixels.data(),3,2,1);
    sla::ProjectRasterState state{sla::LayerDocument::from_rasters({mask,mask}),{.05,.1},"",{},sla::ProjectLayerOrigin::GeneratedRaster};
    state.document.paint(0,0,0,0,255);
    {std::ofstream file(output.string());file<<"previous user file";}
    auto contents=[&] {std::ifstream file(output.string(),std::ios::binary);return std::string(std::istreambuf_iterator<char>(file),{});};
    bool success=false;
    SECTION("successful publication") {success=true;}
    SECTION("invalid metadata") {state.positions_z={.1,.05};}
    SECTION("invalid enum") {state.origin=static_cast<sla::ProjectLayerOrigin>(999);}
    SECTION("cancel before staging") {
        REQUIRE_THROWS_AS(sla::save_layer_project(output.string(),state,[](size_t,size_t){return false;}),sla::LayerOperationCancelled);
    }
    SECTION("cancel while writing") {
        REQUIRE_THROWS_AS(sla::save_layer_project(output.string(),state,[](size_t done,size_t total){return total==0 || done<2;}),sla::LayerOperationCancelled);
    }
    SECTION("cancel before publication") {
        REQUIRE_THROWS_AS(sla::save_layer_project(output.string(),state,[](size_t done,size_t total){return !(done==1 && total==1);}),sla::LayerOperationCancelled);
    }
    SECTION("publication failure preserves directory") {
        boost::filesystem::remove(output);boost::filesystem::create_directory(output);
        REQUIRE_THROWS(sla::save_layer_project(output.string(),state));
        REQUIRE(boost::filesystem::is_directory(output));
        REQUIRE(std::distance(boost::filesystem::directory_iterator(dir.path),boost::filesystem::directory_iterator{})==1);
        return;
    }
    if(success) {
        REQUIRE_NOTHROW(sla::save_layer_project(output.string(),state));
        const auto restored=sla::read_project_layers(output.string());REQUIRE(restored.has_value());
        CHECK(restored->origin==sla::ProjectLayerOrigin::GeneratedRaster);CHECK(restored->positions_z==state.positions_z);
        CHECK(restored->document.image(0).buf==state.document.image(0).buf);
        REQUIRE_THROWS_AS(sla::read_project_layers(output.string(),[](size_t,size_t){return false;}),sla::LayerOperationCancelled);
        size_t checks=0;
        REQUIRE_THROWS_AS(sla::read_project_layers(output.string(),[&](size_t,size_t){return ++checks<5;}),sla::LayerOperationCancelled);
    } else {
        if(state.positions_z.front()>.05 || state.origin!=sla::ProjectLayerOrigin::GeneratedRaster)
            REQUIRE_THROWS(sla::save_layer_project(output.string(),state));
        CHECK(contents()=="previous user file");
    }
    CHECK(state.document.dirty()); // Publication does not implicitly apply/clear session edits.
    CHECK(std::distance(boost::filesystem::directory_iterator(dir.path),boost::filesystem::directory_iterator{})==1);
}

TEST_CASE("Layer projects retain complete resolved SLA settings without ambient presets", "[sla_layers][sla_project_settings]") {
    struct Directory {boost::filesystem::path path=boost::filesystem::unique_path("project-settings-%%%%");
        ~Directory(){boost::system::error_code e;boost::filesystem::remove_all(path,e);}} dir;
    boost::filesystem::create_directory(dir.path);
    DynamicPrintConfig printer;printer.apply(SLAPrinterConfig{});printer.option("printer_technology")->setInt(ptSLA);
    printer.set("display_width",16.);printer.set("display_height",4.);printer.set("display_pixels_x",8);printer.set("display_pixels_y",4);
    printer.option("display_orientation")->setInt(sladoLandscape);
    printer.set_key_value("bed_shape",new ConfigOptionPoints({{0,0},{16,0},{16,4},{0,4}}));
    printer.set("sla_archive_format","pwc");printer.set("sla_anycubic_version","516");
    DynamicPrintConfig material;material.apply(SLAMaterialConfig{});material.set("exposure_time",7.25);material.set("material_density",1.3);
    DynamicPrintConfig process;process.apply(SLAPrintConfig{});process.apply(SLAPrintObjectConfig{});process.set("hollowing_min_thickness",2.7);
    auto role=[](const ConfigBase &cfg,const std::string &name) {
        nlohmann::json values=nlohmann::json::object();for(const auto &key:cfg.keys())values[key]=cfg.opt_serialize(key);
        return nlohmann::json{{"name",name},{"values",values}};
    };
    nlohmann::json settings{{"version",1},{"printer",role(printer,"Drukarka testowa")},
        {"material",role(material,"Żywica testowa")},{"process",role(process,"Proces hollow")}};
    const std::vector<uint8_t> pixels(32,127);const auto mask=sla::PNGRasterEncoder{}(pixels.data(),8,4,1);
    nlohmann::json manifest{{"version",2},{"source_kind","generated_raster"},{"source_fingerprint",""},
        {"positions_z",{.05}},{"columns",8},{"rows",4},{"layers",1},{"settings",settings},
        {"document",{{"archive_format",""},{"entries",{"layer0"}},{"main",{0}},
            {"secondary",nlohmann::json::array()},{"parameters",nlohmann::json::array()}}}};
    bool valid=false,absent=false;
    SECTION("all roles survive read and save") {valid=true;}
    SECTION("no settings remains opt out") {valid=true;absent=true;manifest.erase("settings");}
    SECTION("null settings remains opt out") {valid=true;absent=true;manifest["settings"]=nullptr;}
    SECTION("unknown settings version") {manifest["settings"]["version"]=3;}
    SECTION("v2 requires explicit metadata members") {manifest["settings"]["version"]=2;}
    SECTION("missing process") {manifest["settings"].erase("process");}
    SECTION("unknown settings member") {manifest["settings"]["future"]="must not disappear";}
    SECTION("unknown role attribute") {manifest["settings"]["material"]["inherits"]="must not disappear";}
    SECTION("missing effective printer option") {manifest["settings"]["printer"]["values"].erase("display_width");}
    SECTION("FFF option in printer") {manifest["settings"]["printer"]["values"]["nozzle_diameter"]="0.4";}
    SECTION("network secret cannot be a printer setting") {manifest["settings"]["printer"]["values"]["printhost_apikey"]="test-placeholder";}
    SECTION("wrong role cannot shadow printer geometry") {manifest["settings"]["material"]["values"]["display_width"]="16";}
    SECTION("wrong technology") {manifest["settings"]["printer"]["values"]["printer_technology"]="FFF";}
    SECTION("invalid finite screen") {manifest["settings"]["printer"]["values"]["display_width"]="nan";}
    SECTION("bed must match LCD") {manifest["settings"]["printer"]["values"]["bed_shape"]="0x0,10x0,10x4,0x4";}
    SECTION("invalid format") {manifest["settings"]["printer"]["values"]["sla_archive_format"]="invented";}
    SECTION("wrong typed value") {manifest["settings"]["material"]["values"]["material_density"]=1.3;}
    SECTION("unsupported version for extension") {manifest["settings"]["printer"]["values"]["sla_anycubic_version"]="518.5";}
    const auto input=dir.path/"input.sla-project",output=dir.path/"output.sla-project";
    {Zipper zip(input.string());zip.add_entry("Metadata/SLA/layers.json");zip<<manifest.dump();
     zip.add_entry("Metadata/SLA/document/00000000.bin",mask.data(),mask.size());zip.finalize();}
    if(!valid){CHECK_THROWS(sla::read_project_layers(input.string()));return;}
    const auto restored=sla::read_project_layers(input.string());REQUIRE(restored.has_value());
    REQUIRE_NOTHROW(sla::save_layer_project(output.string(),*restored));
    const auto entries=read_zipper_archive(output.string(),{"metadata/sla/layers.json"},{}).entries;REQUIRE(entries.size()==1);
    const auto saved=nlohmann::json::parse(entries.front().buf);
    if(absent)CHECK_FALSE(saved.contains("settings"));else CHECK(saved.at("settings")==settings);
}

TEST_CASE("SLA project settings capture effective values and identities without FFF or host state", "[sla_layers][sla_project_settings]") {
    DynamicPrintConfig source;source.apply(SLAFullPrintConfig{});source.option("printer_technology")->setInt(ptSLA);
    source.set_key_value("printer_settings_id",new ConfigOptionString("Moja drukarka"));source.set_key_value("sla_material_settings_id",new ConfigOptionString("Moja żywica"));source.set_key_value("sla_print_settings_id",new ConfigOptionString("Mój proces"));
    source.set("display_width",16.);source.set("display_height",4.);source.set("display_pixels_x",8);source.set("display_pixels_y",4);
    source.set("exposure_time",7.25);source.set("hollowing_min_thickness",2.7);
    source.set("sla_archive_format","pwc");source.set("sla_anycubic_version","516");
    source.set_key_value("printhost_apikey",new ConfigOptionString("test-placeholder"));source.set_key_value("nozzle_diameter",new ConfigOptionFloats({.4}));
    const auto source_bed=source.opt_serialize("bed_shape");
    const auto settings=sla::capture_project_settings(source);
    REQUIRE(settings.printer.name=="Moja drukarka");REQUIRE(settings.material.name=="Moja żywica");REQUIRE(settings.process.name=="Mój proces");
    const auto restored=sla::restore_project_settings(settings);
    REQUIRE(restored.opt_string("printer_settings_id")==settings.printer.name);
    REQUIRE(restored.opt_string("sla_material_settings_id")==settings.material.name);
    REQUIRE(restored.opt_string("sla_print_settings_id")==settings.process.name);
    REQUIRE(restored.opt_string("sla_anycubic_version")=="516");
    REQUIRE(restored.opt_float("exposure_time")==7.25);REQUIRE(restored.opt_float("hollowing_min_thickness")==2.7);
    REQUIRE_FALSE(restored.has("printhost_apikey"));REQUIRE_FALSE(restored.has("nozzle_diameter"));
    REQUIRE(source.opt_serialize("bed_shape")==source_bed); // Capture is local, never mutates the active printer.
    REQUIRE(restored.option<ConfigOptionPoints>("bed_shape")->values==std::vector<Vec2d>{{0,0},{16,0},{16,4},{0,4}});
    REQUIRE(sla::capture_project_settings(restored)==settings);
    for(const auto &key:SLAFullPrintConfig{}.keys()) {
        if(key=="bed_shape")continue;
        CHECK(restored.opt_serialize(key)==source.opt_serialize(key));
    }
    auto invalid=settings;invalid.printer.values["nanodlp_device_profile"]=std::string(1024*1024+1,'x');
    CHECK_THROWS(sla::restore_project_settings(invalid));
}

TEST_CASE("Generated projects restart with owned settings and edited masks", "[sla_layers][sla_generated_project]") {
    struct Directory {boost::filesystem::path path=boost::filesystem::unique_path("generated-project-%%%%");
        ~Directory(){boost::system::error_code e;boost::filesystem::remove_all(path,e);}} dir;
    boost::filesystem::create_directory(dir.path);
    DynamicPrintConfig cfg;cfg.apply(SLAFullPrintConfig{});
    cfg.option("printer_technology")->setInt(ptSLA);cfg.option("display_orientation")->setInt(sladoLandscape);
    cfg.set("display_width",16.);cfg.set("display_height",4.);cfg.set("display_pixels_x",8);cfg.set("display_pixels_y",4);
    cfg.set("sla_archive_format","uvj");cfg.set("exposure_time",7.25);cfg.set("material_density",1.3);
    cfg.set_key_value("printer_settings_id",new ConfigOptionString("Generator drukarka"));
    const std::vector<uint8_t> pixels(32,127);const auto mask=sla::PNGRasterEncoder{}(pixels.data(),8,4,1);
    auto original=sla::LayerDocument::from_rasters({mask,mask,mask});
    const std::vector<double> heights{.1,.15,.25};
    const auto job=SLAPrint::create_raster_job(cfg,original,heights);
    auto edited=original;edited.paint(1,0,0,0,255);
    std::optional<sla::ProjectRasterState> state;
    REQUIRE_NOTHROW(state=sla::capture_generated_project(*job,edited));
    REQUIRE(state->origin==sla::ProjectLayerOrigin::GeneratedRaster);
    REQUIRE(state->positions_z.size()==heights.size());
    for(size_t i=0;i<heights.size();++i)CHECK(state->positions_z[i]==Catch::Approx(heights[i]));
    CHECK(state->source_fingerprint.empty());REQUIRE(state->settings.has_value());
    CHECK(state->settings->printer.name=="Generator drukarka");CHECK(state->document.image(1).buf==edited.image(1).buf);
    CHECK(job->layer_document()->image(1).buf==original.image(1).buf);CHECK(edited.dirty());
    const auto path=(dir.path/"generated.sla-project").string();
    REQUIRE_NOTHROW(sla::save_layer_project(path,*state));
    const auto reopened=sla::read_project_layers(path);REQUIRE(reopened.has_value());
    std::unique_ptr<SLAPrint> restored;REQUIRE_NOTHROW(restored=sla::restore_generated_project(*reopened));
    REQUIRE(restored->is_raster_job());CHECK(restored->finished());
    CHECK(restored->full_print_config().opt_float("exposure_time")==7.25);
    CHECK(restored->full_print_config().opt_float("material_density")==1.3);
    CHECK(restored->full_print_config().opt_float("initial_layer_height")==Catch::Approx(.1));
    CHECK(restored->layer_document()->image(1).buf==edited.image(1).buf);
    REQUIRE(restored->print_layers().size()==heights.size());
    for(size_t i=0;i<heights.size();++i)CHECK(double(restored->print_layers()[i].level())*SCALING_FACTOR==Catch::Approx(heights[i]));
    const auto exported=(dir.path/"generated.uvj").string();
    REQUIRE_NOTHROW(restored->export_layer_document(*restored->layer_document(),"uvj",exported));
    const auto decoded=sla::LayerDocument::load_archive(exported);REQUIRE(decoded.layer_count()==3);
    CHECK(decoded.image(1).buf==edited.image(1).buf);
    CHECK_THROWS(restored->export_layer_document(*restored->layer_document(),"SL1",(dir.path/"unsupported.sl1").string()));
    CHECK_FALSE(boost::filesystem::exists(dir.path/"unsupported.sl1"));
    auto invalid=*reopened;invalid.settings.reset();CHECK_THROWS(sla::restore_generated_project(invalid));
    invalid=*reopened;invalid.origin=sla::ProjectLayerOrigin::ModelSlice;CHECK_THROWS(sla::restore_generated_project(invalid));
    invalid=*reopened;invalid.positions_z={.1,.05,.25};CHECK_THROWS(sla::restore_generated_project(invalid));
    SLAPrint unfinished;CHECK_THROWS(sla::capture_generated_project(unfinished,edited));
}

TEST_CASE("Layer project metadata v2 survives publication and rejects damaged role members", "[sla_layers][sla_project_settings][sla_project_metadata_archive]") {
    struct Directory {boost::filesystem::path path=boost::filesystem::unique_path("project-metadata-%%%%");
        ~Directory(){boost::system::error_code e;boost::filesystem::remove_all(path,e);}} dir;
    boost::filesystem::create_directory(dir.path);
    PresetBundle bundle;
    auto &printer=bundle.printers.get_edited_preset().config;
    printer.set("display_width",16.);printer.set("display_height",4.);
    printer.set("display_pixels_x",8);printer.set("display_pixels_y",4);
    printer.option("display_orientation")->setInt(sladoLandscape);
    printer.set("printer_model",std::string("CL-60"));
    printer.set("sla_archive_format",std::string("uvj"));
    Preset::normalize(printer);
    bundle.sla_materials.get_edited_preset().config.set_key_value("compatible_printers",new ConfigOptionStrings({"Drukarka Łódź","Printer; B"}));
    const auto settings=sla::capture_project_settings(bundle.full_config());
    const std::vector<uint8_t> pixels(32,127);const auto mask=sla::PNGRasterEncoder{}(pixels.data(),8,4,1);
    sla::ProjectRasterState state{sla::LayerDocument::from_rasters({mask}),{.05},"",{},sla::ProjectLayerOrigin::GeneratedRaster,settings};
    const auto input=dir.path/"input.sla-project", output=dir.path/"output.sla-project";
    sla::save_layer_project(input.string(),state);
    const auto entries=read_zipper_archive(input.string(),{"metadata/sla/layers.json"},{}).entries;
    REQUIRE(entries.size()==1);
    auto manifest=nlohmann::json::parse(entries.front().buf);
    REQUIRE(manifest["settings"]["version"]==2);
    bool valid=false,legacy=false;
    SECTION("full metadata roundtrip") {valid=true;}
    SECTION("legacy settings remain metadata opt out") {
        valid=true;legacy=true;manifest["settings"]["version"]=1;
        for(const auto *role:{"printer","material","process"})manifest["settings"][role].erase("metadata");
    }
    SECTION("future settings version") {manifest["settings"]["version"]=3;}
    SECTION("missing role metadata member") {manifest["settings"]["material"].erase("metadata");}
    SECTION("unknown metadata role attribute") {manifest["settings"]["process"]["metadata"]["future_setting"]="must not disappear";}
    SECTION("wrong metadata type") {manifest["settings"]["printer"]["metadata"]["printer_variant"]=1;}
    SECTION("metadata cannot shadow the firmware model") {manifest["settings"]["printer"]["metadata"]["printer_model"]="CL-92";}
    SECTION("host state in metadata") {manifest["settings"]["printer"]["metadata"]["print_host"]="test-placeholder";}
    SECTION("metadata cannot replace effective geometry") {manifest["settings"]["printer"]["metadata"]["display_width"]="99";}
    {Zipper zip(output.string());zip.add_entry("Metadata/SLA/layers.json");zip<<manifest.dump();
     zip.add_entry("Metadata/SLA/document/00000000.bin",mask.data(),mask.size());zip.finalize();}
    if(!valid){REQUIRE_THROWS(sla::read_project_layers(output.string()));return;}
    const auto reopened=sla::read_project_layers(output.string());REQUIRE(reopened.has_value());REQUIRE(reopened->settings.has_value());
    if(!legacy)CHECK(*reopened->settings==settings);
    else for(const auto *role:{&reopened->settings->printer,&reopened->settings->material,&reopened->settings->process})CHECK_FALSE(role->metadata.has_value());
    CHECK(reopened->document.image(0).buf==pixels);
    sla::save_layer_project(input.string(),*reopened);
    const auto again=sla::read_project_layers(input.string());REQUIRE(again.has_value());CHECK(again->settings==reopened->settings);
}

TEST_CASE("Explicit fresh layer intentions survive copy, pixel history and project restart", "[sla_fresh_parameters][sla_project]") {
    using F = sla::LayerParameter;
    const auto producer = GENERATE(sla::FreshLayerProducer::Raster, sla::FreshLayerProducer::ModelSlice,
                                  sla::FreshLayerProducer::Calibration, sla::FreshLayerProducer::PCB);
    CAPTURE(int(producer));
    const std::vector<uint8_t> pixels{0,1,127,255,20,240};
    const auto mask = sla::PNGRasterEncoder{}(pixels.data(),3,2,1);
    sla::FreshLayerJob intended;
    intended.producer = producer;
    intended.geometry = {3,2,1.23456789,2.34567891};
    intended.globals.layer_height = .05;
    intended.globals.bottom_layers = 1; intended.globals.transition_layers = 1;
    intended.globals.normal = {{F::Exposure,3.123456789},{F::LiftDistance,3.125},{F::LiftSpeed,89.123456789},
        {F::LiftDistance2,2.25},{F::LiftSpeed2,180.},{F::RetractDistance2,1.125},{F::RetractSpeed,150.},
        {F::RetractSpeed2,240.},{F::WaitBefore,.125},{F::WaitAfter,.25},{F::WaitAfterLift,.375},
        {F::LightOff,0.},{F::LightPWM,180.}};
    intended.globals.bottom = intended.globals.normal;
    intended.globals.bottom[F::Exposure] = 11.987654321;
    intended.globals.bottom[F::LiftDistance] = 5.25;
    intended.globals.bottom[F::WaitBefore] = .875; // neutral model permits distinct bottom waits
    const std::vector<double> positions{.073456789,.123456789,.207654321};
    for (size_t i=0;i<3;++i) {
        auto row = i==0 ? intended.globals.bottom : intended.globals.normal;
        if(i==1) row[F::Exposure]=(intended.globals.bottom.at(F::Exposure)+intended.globals.normal.at(F::Exposure))*.5;
        row[F::PositionZ]=positions[i]; intended.rows.push_back(row);
    }
    auto plain = sla::LayerDocument::from_rasters({mask,mask,mask});
    REQUIRE_FALSE(plain.fresh_job());
    auto document = sla::LayerDocument::from_fresh_rasters({mask,mask,mask}, intended);
    REQUIRE(document.fresh_job());
    auto verify = [&](const sla::LayerDocument &doc) {
        REQUIRE(doc.fresh_job()); const auto &actual=*doc.fresh_job();
        CHECK(actual.producer==intended.producer);
        CHECK(actual.globals.layer_height==intended.globals.layer_height);
        CHECK(actual.globals.bottom_layers==1); CHECK(actual.globals.transition_layers==1);
        CHECK(actual.globals.normal==intended.globals.normal); CHECK(actual.globals.bottom==intended.globals.bottom);
        CHECK(actual.rows==intended.rows);
        CHECK(actual.geometry.columns==3); CHECK(actual.geometry.rows==2);
        CHECK(actual.geometry.width_mm==intended.geometry.width_mm); CHECK(actual.geometry.height_mm==intended.geometry.height_mm);
        CHECK(doc.parameter_capabilities().size()==14); CHECK(doc.archive_format().empty());
    };
    verify(document);
    document.paint(1,0,0,0,255); verify(document);
    REQUIRE(document.undo()); verify(document); REQUIRE(document.redo()); verify(document);
    auto copied = document; verify(copied);
    struct Directory {
        boost::filesystem::path path=boost::filesystem::unique_path("fresh-factory-%%%%"); bool keep=false;
        ~Directory(){if(!keep){boost::system::error_code error;boost::filesystem::remove_all(path,error);}}
    } directory;
    if(const char *root=std::getenv("SLA_FRESH_FACTORY_FIXTURES")) {
        directory.path=boost::filesystem::path(root)/std::to_string(int(producer));directory.keep=true;
    }
    boost::filesystem::create_directories(directory.path);
    sla::ProjectRasterState state{document,positions,"",{},sla::ProjectLayerOrigin::GeneratedRaster};
    if(producer==sla::FreshLayerProducer::ModelSlice) {
        state.origin=sla::ProjectLayerOrigin::ModelSlice;state.source_fingerprint=std::string(40,'a');
    }
    const auto project=directory.path/"fresh.sla-project";
    REQUIRE_NOTHROW(sla::save_layer_project(project.string(),state));
    const auto restored=sla::read_project_layers(project.string());REQUIRE(restored);verify(restored->document);
    CHECK(restored->positions_z==positions);CHECK(restored->origin==state.origin);
    for(size_t i=0;i<3;++i) CHECK(restored->document.image(i).buf==document.image(i).buf);
    const auto roundtrip=directory.path/"roundtrip.sla-project";
    REQUIRE_NOTHROW(sla::save_layer_project(roundtrip.string(),*restored));
    const auto restarted=sla::read_project_layers(roundtrip.string());
    REQUIRE(restarted); verify(restarted->document);
    CHECK(restarted->positions_z==positions);
    CHECK(restarted->origin==state.origin);
    CHECK(restarted->source_fingerprint==state.source_fingerprint);
    for(size_t i=0;i<3;++i) {
        const auto original=document.encoded_layer(i), actual=restarted->document.encoded_layer(i);
        const bool identical=original.sz==actual.sz && std::equal(static_cast<const uint8_t *>(original.buf),static_cast<const uint8_t *>(original.buf)+original.sz,static_cast<const uint8_t *>(actual.buf));
        CHECK(identical);
    }
    const auto entries=read_zipper_archive(project.string(),{"metadata/sla/layers.json"},{}).entries;
    REQUIRE(entries.size()==1);
    const auto manifest=nlohmann::json::parse(entries.front().buf);
    REQUIRE(manifest.at("version")==3);
    auto write_manifest=[&](const boost::filesystem::path &path, const std::string &json, bool legacy=false) {
        Zipper zip(path.string());zip.add_entry("Metadata/SLA/layers.json");zip<<json;
        for(size_t i=0;i<3;++i) {
            const auto bytes=document.encoded_layer(i);
            const auto name=std::string(legacy?"Metadata/SLA/layers/0000000":"Metadata/SLA/document/0000000")+
                std::to_string(i)+(legacy?".png":".bin");
            zip.add_entry(name,bytes.buf,bytes.sz);
        }
        zip.finalize();
    };
    const auto malformed=directory.path/"malformed.sla-project";
    for(int defect=0;defect<17;++defect) {
        CAPTURE(defect);
        auto changed=manifest;
        auto &fresh=changed["document"]["fresh_job"];
        if(defect==0) changed["version"]=2;
        if(defect==1) changed["document"].erase("fresh_job");
        if(defect==2) changed.erase("source_kind");
        if(defect==3) fresh["producer"]=99;
        if(defect==4) fresh["rows"][1]["0"]=fresh["rows"][0]["0"];
        if(defect==5) fresh["rows"][0].erase("13");
        if(defect==6) fresh["rows"][0]["77"]=1.;
        if(defect==7) fresh["globals"]["normal"].erase("3");
        if(defect==8) fresh["version"]=2;
        if(defect==9) fresh["globals"]["bottom_layers"]=4;
        if(defect==10) fresh["geometry"]["columns"]=4;
        if(defect==11) changed["positions_z"][1]=positions[1]+1e-8;
        if(defect==12) fresh["rows"][0]["13"]=1.5;
        if(defect==13) fresh["rows"][0]["3"]=0.;
        if(defect==14) fresh["rows"][0]["1"]=true;
        if(defect==15) fresh["extra"]=1;
        if(defect==16) {
            changed["source_kind"]=state.origin==sla::ProjectLayerOrigin::ModelSlice?"generated_raster":"model_slice";
            changed["source_fingerprint"]=state.origin==sla::ProjectLayerOrigin::ModelSlice?"":std::string(40,'a');
        }
        write_manifest(malformed,changed.dump());
        CHECK_THROWS(sla::read_project_layers(malformed.string()));
    }
    // A valid legacy PNG layout plus a fresh descriptor must not be flattened
    // while the descriptor is silently ignored by the v1 reader branch.
    auto downgraded=manifest;downgraded["version"]=1;
    downgraded.erase("source_kind");downgraded["source_fingerprint"]=std::string(40,'a');
    write_manifest(directory.path/"downgraded.sla-project",downgraded.dump(),true);
    CHECK_THROWS(sla::read_project_layers((directory.path/"downgraded.sla-project").string()));
    auto duplicate=manifest.dump();
    const std::string producer_key="\"producer\":"+std::to_string(int(producer));
    const auto at=duplicate.find(producer_key);REQUIRE(at!=std::string::npos);
    duplicate.replace(at,producer_key.size(),producer_key+","+producer_key);
    write_manifest(directory.path/"duplicate.sla-project",duplicate);
    CHECK_THROWS(sla::read_project_layers((directory.path/"duplicate.sla-project").string()));
    sla::LayerExportProfile physical;
    physical.columns=3;physical.rows=2;physical.screen_width_mm=intended.geometry.width_mm+.1;
    physical.screen_height_mm=intended.geometry.height_mm;physical.density_g_ml=1.;
    physical.cost_per_ml=0.;physical.max_print_height_mm=10.;
    auto bad_profile=state;bad_profile.export_profile=physical;
    const auto keep=directory.path/"keep.sla-project";
    {std::ofstream stream(keep.string(),std::ios::binary);stream<<"previous valid project";}
    CHECK_THROWS(sla::save_layer_project(keep.string(),bad_profile));
    {std::ifstream stream(keep.string(),std::ios::binary);
     const bool previous_project_preserved=std::string(std::istreambuf_iterator<char>(stream),{})=="previous valid project";
     CHECK(previous_project_preserved);}
    // Exercise the public factory itself, including values which JSON cannot
    // represent, without using persistence as a proxy for input validation.
    for(int defect=0;defect<22;++defect) {
        CAPTURE(defect);
        auto invalid=intended;
        if(defect==0) invalid.producer=static_cast<sla::FreshLayerProducer>(99);
        if(defect==1) invalid.rows.pop_back();
        if(defect==2) invalid.rows[1][F::PositionZ]=invalid.rows[0][F::PositionZ];
        if(defect==3) invalid.rows[0][F::PositionZ]=0.;
        if(defect==4) invalid.rows[0].erase(F::LightPWM);
        if(defect==5) invalid.rows[0][F::Exposure]=std::numeric_limits<double>::quiet_NaN();
        if(defect==6) invalid.rows[0][F::WaitAfterLift]=std::numeric_limits<double>::infinity();
        if(defect==7) invalid.rows[0][F::Exposure]=-1.;
        if(defect==8) {
            invalid.rows[0][F::LiftDistance]=std::numeric_limits<double>::max();
            invalid.rows[0][F::LiftDistance2]=std::numeric_limits<double>::max();
        }
        if(defect==9) invalid.rows[0][F::RetractDistance2]=100.;
        if(defect==10) invalid.rows[0][F::LightPWM]=1.5;
        if(defect==11) invalid.rows[0][F::LightPWM]=256.;
        if(defect==12) invalid.rows[0][F::LiftSpeed]=0.;
        if(defect==13) invalid.rows[0][F::LiftSpeed2]=0.;
        if(defect==14) invalid.rows[0][F::RetractSpeed]=0.;
        if(defect==15) invalid.rows[0][F::RetractSpeed2]=0.;
        if(defect==16) invalid.globals.bottom_layers=4;
        if(defect==17) invalid.globals.normal[F::Exposure]=std::numeric_limits<double>::infinity();
        if(defect==18) invalid.geometry.columns=0;
        if(defect==19) invalid.geometry.width_mm=std::numeric_limits<double>::quiet_NaN();
        if(defect==20) invalid.geometry.columns=4;
        if(defect==21) {invalid.geometry.columns=32768;invalid.geometry.rows=32768;}
        CHECK_THROWS(sla::LayerDocument::from_fresh_rasters({mask,mask,mask},invalid));
    }
    verify(document);

}

TEST_CASE("Fresh jobs export owned defaults and rows through both print routes", "[sla_fresh_export][sla_layer_conversion]") {
    using F=sla::LayerParameter;
    const std::string format=GENERATE("ctb_v4","ctb_v5","ctb_encrypted_v4","ctb_encrypted");
    const bool direct=GENERATE(false,true);
    const auto producer=GENERATE(sla::FreshLayerProducer::Raster,sla::FreshLayerProducer::ModelSlice,
                                sla::FreshLayerProducer::Calibration,sla::FreshLayerProducer::PCB);
    CAPTURE(format,direct,int(producer));
    DynamicPrintConfig config;config.apply(SLAFullPrintConfig{});
    config.option("printer_technology")->setInt(ptSLA);
    config.set("display_pixels_x",3);config.set("display_pixels_y",2);
    config.set("display_width",8.);config.set("display_height",6.);
    config.option("display_orientation")->setInt(sladoLandscape);
    config.set("sla_archive_format",format);config.set("printer_model",std::string("fresh-fixture"));
    config.set("layer_height",.05);config.set("initial_layer_height",.05);
    config.set("exposure_time",2.);config.set("initial_exposure_time",20.);
    config.set("sla_bottom_layers",2);config.set("sla_transition_layers",0);
    const std::vector<uint8_t> pixels{0,3,127,255,21,241};
    const auto mask=sla::PNGRasterEncoder{}(pixels.data(),3,2,1);
    sla::FreshLayerJob intent;intent.producer=producer;intent.geometry={3,2,8.,6.};
    intent.globals.layer_height=.05;intent.globals.bottom_layers=1;intent.globals.transition_layers=1;
    intent.globals.normal={{F::Exposure,3.123456789},{F::LiftDistance,3.125},{F::LiftSpeed,89.123456789},
        {F::LiftDistance2,2.25},{F::LiftSpeed2,180.},{F::RetractDistance2,1.125},{F::RetractSpeed,150.},
        {F::RetractSpeed2,240.},{F::WaitBefore,.125},{F::WaitAfter,.25},{F::WaitAfterLift,.375},
        {F::LightOff,0.},{F::LightPWM,180.}};
    intent.globals.bottom=intent.globals.normal;intent.globals.bottom[F::Exposure]=11.987654321;
    const std::vector<double> positions{.1,.15,.2};
    for(size_t i=0;i<3;++i) {
        auto row=i==0?intent.globals.bottom:intent.globals.normal;
        if(i==1)row[F::Exposure]=7.25;
        row[F::PositionZ]=positions[i];intent.rows.push_back(row);
    }
    const auto document=sla::LayerDocument::from_fresh_rasters({mask,mask,mask},intent);
    const auto job=SLAPrint::create_raster_job(config,document,positions);
    struct Directory {boost::filesystem::path path=boost::filesystem::unique_path("fresh-export-%%%%");bool keep=false;
        ~Directory(){if(!keep){boost::system::error_code e;boost::filesystem::remove_all(path,e);}}} directory;
    if(const char *root=std::getenv("SLA_FRESH_EXPORT_FIXTURES")) {
        directory.path=boost::filesystem::path(root)/(format+"-"+std::to_string(direct)+"-"+std::to_string(int(producer)));directory.keep=true;
    }
    boost::filesystem::create_directories(directory.path);
    const auto output=directory.path/"output.ctb";
    if(direct) REQUIRE_NOTHROW(job->export_print(output.string()));
    else REQUIRE_NOTHROW(job->export_layer_document(document,format,output.string(),{},"fresh"));
    const auto saved=sla::LayerDocument::load_archive(output.string());
    REQUIRE(saved.job_parameters());
    CHECK_NOTHROW(saved.require_job_parameters(intent.globals));
    for(size_t i=0;i<3;++i) {
        auto expected=intent.rows[i];
        for(auto &[field,value]:expected)value=double(float(value));
        expected[F::LiftDistance]=double(float(intent.rows[i].at(F::LiftDistance)+intent.rows[i].at(F::LiftDistance2)))-expected.at(F::LiftDistance2);
        const bool rows_match=saved.layer_parameters(i)==expected;
        CHECK(rows_match);CHECK(saved.image(i).buf==pixels);
    }
    sla::ProjectRasterState state{document,positions,"",{},sla::ProjectLayerOrigin::GeneratedRaster};
    if(producer==sla::FreshLayerProducer::ModelSlice) {
        state.origin=sla::ProjectLayerOrigin::ModelSlice;state.source_fingerprint=std::string(40,'b');
    }
    const auto project=directory.path/"fresh.sla-project";
    REQUIRE_NOTHROW(sla::save_layer_project(project.string(),state));
    const auto restored=sla::read_project_layers(project.string());REQUIRE(restored);
    REQUIRE(restored->document.fresh_job());
    CHECK(restored->document.fresh_job()->rows==intent.rows);
    const auto reopened=SLAPrint::create_raster_job(config,restored->document,positions);
    const auto restart_output=directory.path/"restart.ctb";
    REQUIRE_NOTHROW(reopened->export_print(restart_output.string()));
    const auto restarted=sla::LayerDocument::load_archive(restart_output.string());
    REQUIRE_NOTHROW(restarted.require_job_parameters(intent.globals));
    for(size_t i=0;i<3;++i) {
        CHECK(restarted.layer_parameters(i)==saved.layer_parameters(i));
        CHECK(restarted.image(i).buf==pixels);
    }
    // Unsupported codecs and lossy native representation must preserve the
    // previous output through either public route.
    const auto keep=directory.path/"keep.ctb";
    const auto bad_mask=sla::PNGRasterEncoder{}(std::vector<uint8_t>{0,1,127,255,20,240}.data(),3,2,1);
    for(int defect=0;defect<13;++defect) {
        CAPTURE(defect);
        auto invalid=intent;
        std::string target=format;
        if(defect==0)target="SL1";
        if(defect==1)target="ctb_v3";
        if(defect==2)target="ctb_gktwo";
        if(defect==3)target="unknown-format";
        if(defect==4)invalid.globals.bottom[F::WaitBefore]=.875;
        if(defect==5)invalid.globals.layer_height=.0500001;
        if(defect==6)invalid.rows[1][F::PositionZ]=.1000000001;
        if(defect==7)invalid.rows[0][F::LiftSpeed]=1e-50;
        if(defect==8)invalid.rows[0][F::Exposure]=1e40;
        if(defect==9)invalid.rows[0][F::LiftDistance]=1e-20;
        if(defect==10)invalid.rows[0][F::LiftDistance2]=1e40;
        if(defect==11)invalid.geometry.width_mm=8.1;
        const auto &input=defect==12?bad_mask:mask;
        const auto invalid_document=sla::LayerDocument::from_fresh_rasters({input,input,input},invalid);
        {std::ofstream file(keep.string(),std::ios::binary);file<<"previous valid print";}
        if(direct && defect>=4) {
            const auto invalid_job=SLAPrint::create_raster_job(config,invalid_document,positions);
            CHECK_THROWS(invalid_job->export_print(keep.string()));
        } else CHECK_THROWS(job->export_layer_document(invalid_document,target,keep.string()));
        {std::ifstream file(keep.string(),std::ios::binary);
         const bool preserved=std::string(std::istreambuf_iterator<char>(file),{})=="previous valid print";
         CHECK(preserved);}
    }
    REQUIRE(job->layer_document()->fresh_job());
    CHECK(job->layer_document()->fresh_job()->rows==intent.rows);
    CHECK(job->layer_document()->fresh_job()->globals.normal==intent.globals.normal);
    CHECK(job->full_print_config().opt_float("exposure_time")==2.);
}

TEST_CASE("Fresh named tilting jobs accept native representation without changing intentions", "[sla_fresh_export][sla_tilt_target]") {
    using F=sla::LayerParameter;
    const std::string target=GENERATE("ctb_encrypted_v4","ctb_encrypted");
    const std::string machine=GENERATE("Saturn 4 Ultra","Mars 5 Ultra");
    const bool direct=GENERATE(false,true);
    CAPTURE(target,machine,direct);
    DynamicPrintConfig config;config.apply(SLAFullPrintConfig{});
    config.option("printer_technology")->setInt(ptSLA);
    config.set("display_pixels_x",3);config.set("display_pixels_y",2);
    config.set("display_width",8.);config.set("display_height",6.);
    config.option("display_orientation")->setInt(sladoLandscape);
    config.set("sla_archive_format",target);config.set("printer_model",machine);
    sla::FreshLayerJob intent;intent.geometry={3,2,8.,6.};
    intent.globals.layer_height=.05;intent.globals.bottom_layers=1;
    intent.globals.normal={{F::Exposure,1.},{F::LiftDistance,.05},{F::LiftSpeed,.05},
        {F::LiftDistance2,0.},{F::LiftSpeed2,0.},{F::RetractDistance2,0.},{F::RetractSpeed,.05},
        {F::RetractSpeed2,0.},{F::WaitBefore,.125},{F::WaitAfter,.25},{F::WaitAfterLift,.375},
        {F::LightOff,0.},{F::LightPWM,180.}};
    intent.globals.bottom=intent.globals.normal;intent.globals.bottom[F::Exposure]=8.;
    const std::vector<double> positions{.05,.1};
    for(size_t i=0;i<2;++i) {auto row=i==0?intent.globals.bottom:intent.globals.normal;row[F::PositionZ]=positions[i];intent.rows.push_back(row);}
    const std::vector<uint8_t> pixels{0,3,127,255,21,241};
    const auto mask=sla::PNGRasterEncoder{}(pixels.data(),3,2,1);
    const auto document=sla::LayerDocument::from_fresh_rasters({mask,mask},intent);
    const auto job=SLAPrint::create_raster_job(config,document,positions);
    struct Directory {boost::filesystem::path path=boost::filesystem::unique_path("fresh-named-%%%%");bool keep=false;
        ~Directory(){if(!keep){boost::system::error_code e;boost::filesystem::remove_all(path,e);}}} directory;
    if(const char *root=std::getenv("SLA_FRESH_EXPORT_FIXTURES")) {
        directory.path=boost::filesystem::path(root)/("named-"+target+"-"+machine+"-"+std::to_string(direct));directory.keep=true;
    }
    boost::filesystem::create_directories(directory.path);const auto output=directory.path/"output.ctb";
    if(direct)REQUIRE_NOTHROW(job->export_print(output.string()));
    else REQUIRE_NOTHROW(job->export_layer_document(document,target,output.string()));
    const auto saved=sla::LayerDocument::load_archive(output.string());
    CHECK_NOTHROW(saved.require_job_parameters(intent.globals));
    for(size_t i=0;i<2;++i) {
        auto expected=intent.rows[i];for(auto &[field,value]:expected)value=double(float(value));
        CHECK(saved.layer_parameters(i)==expected);CHECK(saved.image(i).buf==pixels);
    }
    CHECK(job->layer_document()->fresh_job()->rows==intent.rows);
    auto invalid=intent;invalid.globals.normal[F::LiftDistance]=.1;
    const auto incompatible=sla::LayerDocument::from_fresh_rasters({mask,mask},invalid);
    const auto keep=directory.path/"keep.ctb";
    {std::ofstream file(keep.string(),std::ios::binary);file<<"previous valid print";}
    CHECK_THROWS(job->export_layer_document(incompatible,target,keep.string()));
    {std::ifstream file(keep.string(),std::ios::binary);const bool preserved=std::string(std::istreambuf_iterator<char>(file),{})=="previous valid print";CHECK(preserved);}
}


TEST_CASE("Fresh producer builder uses exact settings and finalized engine levels", "[sla_fresh_builder]") {
    using F=sla::LayerParameter;
    const auto producer=GENERATE(sla::FreshLayerProducer::Raster,sla::FreshLayerProducer::ModelSlice,
        sla::FreshLayerProducer::Calibration,sla::FreshLayerProducer::PCB);
    const bool portrait=GENERATE(false,true);
    const size_t count=GENERATE(size_t(1),size_t(3),size_t(8));
    const size_t bottom_count=GENERATE(size_t(0),size_t(2),size_t(20));
    const size_t transition_count=GENERATE(size_t(0),size_t(5));
    CAPTURE(int(producer),portrait,count,bottom_count,transition_count);
    DynamicPrintConfig config;config.apply(SLAFullPrintConfig{});
    config.option("printer_technology")->setInt(ptSLA);
    config.set("sla_archive_format",std::string("SL1"));
    config.set("display_pixels_x",3);config.set("display_pixels_y",2);
    config.set("display_width",8.123456789);config.set("display_height",6.987654321);
    config.option("display_orientation")->setInt(portrait?sladoPortrait:sladoLandscape);
    config.set("layer_height",.050000123456);
    config.set("exposure_time",3.123456789);config.set("initial_exposure_time",11.987654321);
    config.set("sla_bottom_layers",int(bottom_count));config.set("sla_transition_layers",int(transition_count));
    const std::map<F,std::string> keys{{F::LiftDistance,"lift_height"},{F::LiftDistance2,"lift_height2"},
        {F::LiftSpeed,"lift_speed"},{F::LiftSpeed2,"lift_speed2"},{F::RetractSpeed,"retract_speed"},
        {F::RetractSpeed2,"retract_speed2"},{F::RetractDistance2,"retract_height2"},
        {F::WaitBefore,"wait_before_cure"},{F::WaitAfter,"wait_after_cure"},{F::WaitAfterLift,"wait_after_lift"}};
    const std::map<F,double> values{{F::LiftDistance,6.123456789},{F::LiftDistance2,2.123456789},
        {F::LiftSpeed,89.123456789},{F::LiftSpeed2,180.123456789},{F::RetractSpeed,150.123456789},
        {F::RetractSpeed2,240.123456789},{F::RetractDistance2,1.123456789},
        {F::WaitBefore,.123456789},{F::WaitAfter,.234567891},{F::WaitAfterLift,.345678912}};
    for(bool bottom:{false,true}) {
        const std::string prefix=bottom?"sla_bottom_":"sla_";
        for(const auto &[field,key]:keys)config.set(prefix+key,values.at(field)+(bottom?.5:0.));
        config.set(prefix+"light_pwm",bottom?181:179);
    }
    const std::vector<uint8_t> pixels{0,3,127,255,21,241};
    const auto mask=sla::PNGRasterEncoder{}(pixels.data(),portrait?2:3,portrait?3:2,1);
    std::vector<sla::EncodedRaster> masks(count,mask);
    std::vector<double> requested;
    for(size_t i=0;i<count;++i)requested.push_back(.100000123456+double(i)*.070000234567);
    const auto plain=sla::LayerDocument::from_rasters(masks);
    const auto job=SLAPrint::create_raster_job(config,plain,requested);
    REQUIRE_FALSE(job->layer_document()->fresh_job());
    const auto intent=job->make_fresh_layer_job(producer);
    CHECK(intent.producer==producer);
    CHECK(intent.geometry.columns==size_t(portrait?2:3));CHECK(intent.geometry.rows==size_t(portrait?3:2));
    CHECK(intent.geometry.width_mm==(portrait?6.987654321:8.123456789));
    CHECK(intent.geometry.height_mm==(portrait?8.123456789:6.987654321));
    CHECK(intent.globals.layer_height==.050000123456);
    CHECK(intent.globals.bottom_layers==std::min(bottom_count,count));
    CHECK(intent.globals.transition_layers==std::min(transition_count,count-std::min(bottom_count,count)));
    for(bool bottom:{false,true}) {
        const auto &defaults=bottom?intent.globals.bottom:intent.globals.normal;
        REQUIRE(defaults.size()==13);
        CHECK(defaults.at(F::Exposure)==(bottom?11.987654321:3.123456789));
        for(const auto &[field,value]:values)CHECK(defaults.at(field)==value+(bottom?.5:0.));
        CHECK(defaults.at(F::LightOff)==0.);CHECK(defaults.at(F::LightPWM)==double(bottom?181:179));
    }
    REQUIRE(intent.rows.size()==count);
    for(size_t i=0;i<count;++i) {
        auto expected=i<bottom_count?intent.globals.bottom:intent.globals.normal;
        expected[F::PositionZ]=double(job->print_layers()[i].level())*SCALING_FACTOR;
        if(i>=bottom_count && i-bottom_count<transition_count)expected[F::Exposure]=11.987654321+(3.123456789-11.987654321)*double(i-bottom_count+1)/double(transition_count+1);
        CHECK(intent.rows[i]==expected);
    }
    CHECK(intent.rows.front().at(F::PositionZ)!=requested.front());
    CHECK_NOTHROW(intent.validate(count));
    const auto owned=sla::LayerDocument::from_fresh_rasters(masks,intent);
    CHECK(owned.fresh_job()->rows==intent.rows);
    // The builder is pure: imports and temporary conversion contexts stay image-only.
    CHECK_FALSE(job->layer_document()->fresh_job());
    CHECK(job->full_print_config().opt_float("exposure_time")==3.123456789);
    CHECK_THROWS(job->make_fresh_layer_job(static_cast<sla::FreshLayerProducer>(99)));
}

TEST_CASE("Fresh producer builder rejects missing finalized engine layers", "[sla_fresh_builder]") {
    const SLAPrint empty;
    CHECK_THROWS(empty.make_fresh_layer_job(sla::FreshLayerProducer::ModelSlice));
}

TEST_CASE("Export document snapshots preserve current pixels without session history", "[sla_export_snapshot]") {
    const std::vector<uint8_t> pixels{0,1,32,64,127,128,254,255};
    const auto png=sla::PNGRasterEncoder{}(pixels.data(),4,2,1);
    auto source=sla::LayerDocument::from_rasters({png,png,png});
    source.invert(0);source.invert(1);REQUIRE(source.undo());
    REQUIRE(source.can_undo());REQUIRE(source.can_redo());
    const auto revision=source.revision();const auto first=source.image(0).buf,second=source.image(1).buf;
    auto snapshot=source.snapshot();
    CHECK_FALSE(snapshot.can_undo());CHECK_FALSE(snapshot.can_redo());
    CHECK(snapshot.revision()==revision);CHECK(snapshot.dirty()==source.dirty());
    CHECK(snapshot.layer_count()==source.layer_count());CHECK(snapshot.mask_plane_count()==source.mask_plane_count());
    CHECK(snapshot.image(0).buf==first);CHECK(snapshot.image(1).buf==second);
    REQUIRE(source.redo());CHECK(snapshot.image(1).buf==second);
    snapshot.invert(2);CHECK(source.image(2).buf==pixels);
    REQUIRE(snapshot.undo());CHECK(snapshot.image(2).buf==pixels);
}
TEST_CASE("Cancelled export snapshot leaves source pixels history and revision intact", "[sla_export_snapshot]") {
    const std::vector<uint8_t> pixels{0,255,32,128};
    const auto png=sla::PNGRasterEncoder{}(pixels.data(),2,2,1);
    auto source=sla::LayerDocument::from_rasters({png,png});source.invert(0);
    const auto revision=source.revision();const auto edited=source.image(0).buf;
    CHECK_THROWS_AS(source.snapshot([](size_t,size_t){return false;}),sla::LayerOperationCancelled);
    CHECK(source.image(0).buf==edited);CHECK(source.revision()==revision);CHECK(source.can_undo());
    REQUIRE(source.undo());CHECK(source.image(0).buf==pixels);
}

TEST_CASE("Range brush preserves each mask and uses one undo step", "[sla_range_brush]") {
    const std::vector<uint8_t> a{10,0,0,0,0,0}, b{0,0,0,0,0,80};
    auto doc=sla::LayerDocument::from_rasters({sla::PNGRasterEncoder{}(a.data(),3,2,1),
        sla::PNGRasterEncoder{}(b.data(),3,2,1),sla::PNGRasterEncoder{}(a.data(),3,2,1)});
    const auto revision=doc.revision();
    CHECK(doc.overlay(0,1).buf==std::vector<uint8_t>{10,0,0,0,0,80});
    CHECK(doc.revision()==revision);
    REQUIRE(doc.paint_stroke(0,1,{{1,0},{1,1}},0,127));
    CHECK(doc.image(0).buf==std::vector<uint8_t>{10,127,0,0,127,0});
    CHECK(doc.image(1).buf==std::vector<uint8_t>{0,127,0,0,127,80});
    CHECK(doc.image(2).buf==a);
    REQUIRE(doc.undo()); CHECK(doc.image(0).buf==a); CHECK(doc.image(1).buf==b);
    CHECK_FALSE(doc.can_undo()); REQUIRE(doc.redo());
    REQUIRE(doc.paint_stroke(0,1,{{1,0},{1,1}},0,0));
    CHECK(doc.image(0).buf==a); CHECK(doc.image(1).buf==b);
}
TEST_CASE("Range brush rejects invalid late points and cancellation atomically", "[sla_range_brush]") {
    const std::vector<uint8_t> pixels{0,0,0,255};
    const auto png=sla::PNGRasterEncoder{}(pixels.data(),2,2,1);
    auto doc=sla::LayerDocument::from_rasters({png,png,png});
    CHECK_FALSE(doc.paint_stroke(0,2,{{0,0}},0,255,sla::LayerMaskPlane::Main,
        [](size_t done,size_t){return done<2;}));
    CHECK_THROWS(doc.paint_stroke(0,2,{{0,0},{2,0}},0,255));
    CHECK_THROWS(doc.overlay(2,1));
    for(size_t i=0;i<3;++i) CHECK(doc.image(i).buf==pixels);
    CHECK_FALSE(doc.can_undo()); CHECK(doc.revision()==0);
}

TEST_CASE("Support peninsula detection respects complete coverage and holes", "[sla_peninsula_coverage]") {
    auto rectangle=[](double x0,double y0,double x1,double y1) {
        return ExPolygon(Polygon({Point(scale_(x0),scale_(y0)),Point(scale_(x1),scale_(y0)),
            Point(scale_(x1),scale_(y1)),Point(scale_(x0),scale_(y1))}));
    };
    const auto mode=GENERATE("same","subset","union","overhang","hole");
    CAPTURE(mode);
    ExPolygons below{rectangle(0,0,20,20)};
    ExPolygon above=rectangle(0,0,20,20);
    if(std::string(mode)=="subset")above=rectangle(2,2,18,18);
    if(std::string(mode)=="union")below={rectangle(0,0,10,20),rectangle(10,0,20,20)};
    if(std::string(mode)=="overhang")above=rectangle(0,0,25,20);
    if(std::string(mode)=="hole") {
        auto hole=rectangle(7,7,13,13).contour;hole.make_clockwise();
        below[0].holes.push_back(hole);
    }
    sla::PrepareSupportConfig config;config.minimal_bounding_sphere_radius=0;
    auto data=sla::prepare_generator_data({below,{above}},{.05f,.1f},config);
    REQUIRE(data.layers.size()==2);REQUIRE(data.layers[1].parts.size()==1);
    const auto &part=data.layers[1].parts.front();
    if(std::string(mode)=="overhang" || std::string(mode)=="hole")CHECK_FALSE(part.peninsulas.empty());
    else CHECK(part.peninsulas.empty());
    CHECK(*part.shape==above);
}

TEST_CASE("Initial projected PNG encoder preserves native palettes and raw source", "[sla_projected_png]") {
    std::vector<uint8_t> pixels(256);std::iota(pixels.begin(),pixels.end(),0);
    const auto original=pixels;
    std::vector<sla::LayerMaskPrecision> precisions{sla::LayerMaskPrecision::Gray8,
        sla::LayerMaskPrecision::NativeGray7,sla::LayerMaskPrecision::NativeGray4};
    for(unsigned planes=1;planes<=16;++planes)precisions.push_back(sla::bitplane_mask_precision(planes));
    for(const auto precision:precisions) {
        CAPTURE(int(precision));
        const auto png=sla::ProjectedPNGMaskEncoder{precision,{}}(pixels.data(),256,1,1);
        const auto decoded=sla::LayerDocument::from_rasters({png}).image(0);
        auto expected=pixels;for(auto &value:expected)value=sla::project_mask_value(value,precision);
        const bool matches=decoded.buf==expected;
        CHECK(matches);CHECK(pixels==original);
    }
    CHECK_THROWS_AS((sla::ProjectedPNGMaskEncoder{sla::LayerMaskPrecision::NativeGray7,
        []{throw sla::LayerOperationCancelled{};}}(pixels.data(),256,1,1)),sla::LayerOperationCancelled);
    CHECK(pixels==original);
    CHECK_THROWS(sla::ProjectedPNGMaskEncoder{static_cast<sla::LayerMaskPrecision>(99),{}}(pixels.data(),256,1,1));
    const std::vector<uint8_t> rgb(256*3,127);
    CHECK_THROWS(sla::ProjectedPNGMaskEncoder{sla::LayerMaskPrecision::NativeGray7,{}}(rgb.data(),256,1,3));
}

TEST_CASE("Projected PNG cancellation after a chunk retains source pixels", "[sla_projected_png]") {
    const std::vector<uint8_t> pixels(2048*1024,128);unsigned checks=0;
    const sla::ProjectedPNGMaskEncoder encoder{sla::LayerMaskPrecision::NativeGray7,[&] {
        if(++checks>=3)throw sla::LayerOperationCancelled{};
    }};
    CHECK_THROWS_AS(encoder(pixels.data(),2048,1024,1),sla::LayerOperationCancelled);
    CHECK(checks==3);
    CHECK(std::all_of(pixels.begin(),pixels.end(),[](uint8_t value){return value==128;}));
}


TEST_CASE("Owning raster factories can cancel without publishing a partial document", "[sla_owning_cancel]") {
    using F = sla::LayerParameter;
    const bool generated = GENERATE(false, true);
    const auto precision = GENERATE(sla::LayerMaskPrecision::Gray8, sla::LayerMaskPrecision::NativeGray7);
    CAPTURE(generated, int(precision));
    sla::FreshLayerJob job;
    job.producer = sla::FreshLayerProducer::ModelSlice;
    job.mask_precision = precision;
    job.geometry = {1024, 1024, 100., 100.};
    job.globals.layer_height = .05;
    job.globals.normal = {{F::Exposure, 2.}, {F::LiftDistance, 5.}, {F::LiftSpeed, 60.},
        {F::LiftDistance2, 0.}, {F::LiftSpeed2, 0.}, {F::RetractDistance2, 0.},
        {F::RetractSpeed, 120.}, {F::RetractSpeed2, 0.}, {F::WaitBefore, 0.},
        {F::WaitAfter, 0.}, {F::WaitAfterLift, 0.}, {F::LightOff, 0.}, {F::LightPWM, 255.}};
    job.globals.bottom = job.globals.normal;
    std::vector<sla::EncodedRaster> masks;
    for (size_t i = 0; i < 3; ++i) {
        auto row = job.globals.normal; row[F::PositionZ] = .05 * (i + 1); job.rows.push_back(row);
        // Distinct masks force validation beyond the repeated-PNG cache.
        std::vector<uint8_t> pixels(1024 * 1024, uint8_t(3 + i * 2));
        masks.push_back(sla::PNGRasterEncoder{}(pixels.data(), 1024, 1024, 1));
    }
    const auto originals = masks;
    const auto rows = job.rows;
    auto make = [&](const sla::LayerProgress &progress) {
        return generated ? sla::LayerDocument::from_generated_rasters(masks, job, progress) :
            sla::LayerDocument::from_fresh_rasters(masks, job, progress);
    };
    CHECK_THROWS_AS(make([](size_t, size_t) { return false; }), sla::LayerOperationCancelled);
    size_t calls = 0;
    const auto complete = make([&](size_t, size_t) { ++calls; return true; });
    CHECK(calls > masks.size());
    // Cancel at early, intermediate and final observed checkpoints; assignment
    // must retain the previous complete document, history and revision.
    auto published = complete; published.invert(0);
    const auto revision = published.revision(); const auto edited = published.image(0).buf;
    for (const auto stop : {size_t(1), std::max(size_t(1), calls / 2), std::max(size_t(1), calls)}) {
        size_t observed = 0;
        CHECK_THROWS_AS(published = make([&](size_t, size_t) { return ++observed < stop; }), sla::LayerOperationCancelled);
        CHECK(published.revision() == revision); CHECK(published.can_undo());
        CHECK(published.image(0).buf == edited);
    }
    CHECK_THROWS_AS(job.validate(masks.size(), [](size_t, size_t) { return false; }), sla::LayerOperationCancelled);
    CHECK_THROWS_AS(make([](size_t, size_t) -> bool { throw std::logic_error("cancel owner"); }), std::logic_error);
    if (precision != sla::LayerMaskPrecision::Gray8) {
        // A high-resolution palette scan must itself service cancellation.
        CHECK_THROWS_AS(make([](size_t done, size_t total) {
            return total != 1024 * 1024 || done == 0;
        }), sla::LayerOperationCancelled);
    }
    CHECK(job.rows == rows);
    for (size_t i = 0; i < masks.size(); ++i) {
        CHECK(masks[i].size() == originals[i].size());
        CHECK(std::memcmp(masks[i].data(), originals[i].data(), masks[i].size()) == 0);
        CHECK(complete.image(i).buf == std::vector<uint8_t>(1024 * 1024, uint8_t(3 + i * 2)));
    }
    CHECK_THROWS_AS(sla::LayerDocument::from_rasters(masks, [](size_t, size_t) { return false; }), sla::LayerOperationCancelled);
}

TEST_CASE("Owned palette validation retains chunk cancellation and rejects malformed source PNGs", "[sla_palette_validation]") {
    using F = sla::LayerParameter;
    constexpr size_t boundary = size_t(1) << 20;
    constexpr size_t width = 1024, height = 1025, pixels_count = width * height;
    const auto precision = GENERATE(sla::LayerMaskPrecision::NativeGray7,
        sla::LayerMaskPrecision::NativeGray4, sla::bitplane_mask_precision(3),
        sla::bitplane_mask_precision(16));
    CAPTURE(int(precision));
    // Literal values are independent of project_mask_value, including the
    // non-power-of-two AA palette whose largest value is254 rather than255.
    const uint8_t canonical = precision == sla::LayerMaskPrecision::NativeGray7 ? 3 :
        precision == sla::LayerMaskPrecision::NativeGray4 ? 17 :
        precision == sla::bitplane_mask_precision(3) ? 84 : 15;
    std::vector<uint8_t> pixels(pixels_count, 0);
    for (size_t at = 0; at < pixels.size(); at += 257) pixels[at] = canonical;
    pixels.back() = canonical;
    const auto mask = sla::PNGRasterEncoder{}(pixels.data(), width, height, 1);
    const auto *raw = static_cast<const uint8_t *>(mask.data());
    const std::vector<uint8_t> original(raw, raw + mask.size());
    sla::FreshLayerJob job;
    job.producer = sla::FreshLayerProducer::ModelSlice;
    job.mask_precision = precision;
    job.geometry = {width, height, 100., 100.};
    job.globals.layer_height = .05;
    job.globals.normal = {{F::Exposure, 2.}, {F::LiftDistance, 5.}, {F::LiftSpeed, 60.},
        {F::LiftDistance2, 0.}, {F::LiftSpeed2, 0.}, {F::RetractDistance2, 0.},
        {F::RetractSpeed, 120.}, {F::RetractSpeed2, 0.}, {F::WaitBefore, 0.},
        {F::WaitAfter, 0.}, {F::WaitAfterLift, 0.}, {F::LightOff, 0.}, {F::LightPWM, 255.}};
    job.globals.bottom = job.globals.normal;
    auto row = job.globals.normal; row[F::PositionZ] = .05; job.rows.push_back(row);
    const auto original_rows = job.rows;
    std::vector<size_t> checkpoints;
    const auto complete = sla::LayerDocument::from_fresh_rasters({mask}, job,
        [&](size_t done, size_t total) {
            if (total == pixels_count) checkpoints.push_back(done);
            return true;
        });
    CHECK(checkpoints == std::vector<size_t>{0, boundary, pixels_count});
    REQUIRE(complete.image(0).buf == pixels);
    // A repeated compressed PNG is fully checked once, then reuses only its
    // successful palette result; the uncompressed image is not retained.
    auto repeated_job = job;
    row[F::PositionZ] = .1; repeated_job.rows.push_back(row);
    checkpoints.clear();
    const auto repeated = sla::LayerDocument::from_fresh_rasters({mask, mask}, repeated_job,
        [&](size_t done, size_t total) {
            if (total == pixels_count) checkpoints.push_back(done);
            return true;
        });
    CHECK(checkpoints == std::vector<size_t>{0, boundary, pixels_count});
    CHECK(repeated.image(1).buf == pixels);
    auto published = complete;
    published.invert(0);
    const auto revision = published.revision();
    const auto edited_pixels = published.image(0).buf;
    auto check_retained = [&] {
        CHECK(published.revision() == revision);
        CHECK(published.can_undo());
        CHECK(published.image(0).buf == edited_pixels);
        CHECK(job.rows == original_rows);
        CHECK(mask.size() == original.size());
        CHECK(std::memcmp(mask.data(), original.data(), original.size()) == 0);
    };
    checkpoints.clear();
    CHECK_THROWS_AS(published = sla::LayerDocument::from_fresh_rasters({mask}, job,
        [&](size_t done, size_t total) {
            if (total != pixels_count) return true;
            checkpoints.push_back(done);
            return done != boundary;
        }), sla::LayerOperationCancelled);
    CHECK(checkpoints == std::vector<size_t>{0, boundary});
    check_retained();
    CHECK_THROWS_AS(published = sla::LayerDocument::from_fresh_rasters({mask}, job,
        [&](size_t done, size_t total) {
            if (total == pixels_count && done == boundary) throw std::logic_error("palette owner");
            return true;
        }), std::logic_error);
    check_retained();
    for (const size_t at : {size_t(0), boundary - 1, boundary, boundary + 1, pixels_count - 1}) {
        CAPTURE(at);
        auto invalid = pixels; invalid[at] = 1; // Invalid in all four declared palettes.
        const auto bad_mask = sla::PNGRasterEncoder{}(invalid.data(), width, height, 1);
        const auto *bytes = static_cast<const uint8_t *>(bad_mask.data());
        const std::vector<uint8_t> before(bytes, bytes + bad_mask.size());
        CHECK_THROWS_WITH(published = sla::LayerDocument::from_fresh_rasters({bad_mask}, job),
            "Source mask pixels violate declared precision");
        check_retained();
        CHECK(bad_mask.size() == before.size());
        CHECK(std::memcmp(bad_mask.data(), before.data(), before.size()) == 0);
    }
    auto mismatched = job; mismatched.geometry.rows += 1;
    CHECK_THROWS_WITH(published = sla::LayerDocument::from_fresh_rasters({mask}, mismatched),
        "Source mask dimensions differ from the declared raster geometry");
    check_retained();
    // Corrupt a critical PNG chunk, then separately use invalid deflate bytes
    // with a correct PNG CRC. Both must fail decoding before palette scanning.
    size_t idat = 8;
    auto be32 = [&](size_t at) { return (uint32_t(original[at]) << 24) |
        (uint32_t(original[at+1]) << 16) | (uint32_t(original[at+2]) << 8) | original[at+3]; };
    while (idat + 12 <= original.size() && std::memcmp(original.data()+idat+4, "IDAT", 4) != 0)
        idat += size_t(be32(idat)) + 12;
    REQUIRE(idat + 12 <= original.size());
    const size_t length = be32(idat);
    REQUIRE(length > 0);
    REQUIRE(idat + length + 12 <= original.size());
    for (const bool valid_crc : {false, true}) {
        CAPTURE(valid_crc);
        auto damaged = original;
        if (!valid_crc) damaged[idat + length + 8] ^= 1;
        else {
            damaged[idat + 8] = 0; // Invalid zlib compression header.
            const uint32_t crc = mz_crc32(0, damaged.data()+idat+4, length+4);
            for (size_t byte = 0; byte != 4; ++byte)
                damaged[idat + length + 8 + byte] = uint8_t(crc >> (24 - 8 * byte));
        }
        const auto damaged_copy = damaged;
        const sla::EncodedRaster corrupt(std::move(damaged), "png");
        CHECK_THROWS_WITH(published = sla::LayerDocument::from_fresh_rasters({corrupt}, job),
            "Cannot decode grayscale layer PNG");
        check_retained();
        CHECK(std::memcmp(corrupt.data(), damaged_copy.data(), damaged_copy.size()) == 0);
    }
    REQUIRE(published.undo());
    CHECK(published.image(0).buf == pixels);
}

// Optional retained-fixture benchmark, excluded from ordinary regression.
// It times public validation independently of supports, export and test checks.
TEST_CASE("Profile public palette validation of retained owned layers", "[.][sla_palette_validation_benchmark]") {
    const char *input = std::getenv("SLA_PALETTE_BENCH_PROJECT");
    const char *output = std::getenv("SLA_PALETTE_BENCH_REPORT");
    REQUIRE(input); REQUIRE(output);
    const auto load_started = std::chrono::steady_clock::now();
    const auto state = sla::read_project_layers(input);
    const double load_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now()-load_started).count();
    REQUIRE(state);
    REQUIRE(state->document.fresh_job());
    REQUIRE(state->document.layer_count() == 213);
    const auto &expected = *state->document.fresh_job();
    REQUIRE(expected.mask_precision == sla::LayerMaskPrecision::NativeGray7);
    REQUIRE(expected.geometry.columns == 5760);
    REQUIRE(expected.geometry.rows == 3600);
    std::vector<sla::EncodedRaster> masks;
    masks.reserve(state->document.layer_count());
    size_t encoded_bytes = 0;
    for (size_t layer = 0; layer < state->document.layer_count(); ++layer) {
        const auto source = state->document.encoded_layer(layer);
        const auto *begin = static_cast<const uint8_t *>(source.buf);
        masks.emplace_back(std::vector<uint8_t>(begin, begin+source.sz), "png");
        encoded_bytes += source.sz;
    }
    // Preparation/copies of benchmark input stay outside the factory timer.
    // The factory still performs all ordinary byte copying, PNG decode/CRC,
    // geometry, field/palette validation and cancellation checkpoints.
    auto intentions = expected;
    size_t callbacks = 0, palette_callbacks = 0;
    const sla::LayerProgress progress = [&](size_t, size_t total) {
        ++callbacks;
        if (total == expected.geometry.columns*expected.geometry.rows) ++palette_callbacks;
        return true;
    };
    const auto validation_started = std::chrono::steady_clock::now();
    const auto reconstructed = sla::LayerDocument::from_fresh_rasters(
        std::move(masks), std::move(intentions), progress);
    const double validation_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now()-validation_started).count();
    REQUIRE(reconstructed.fresh_job());
    REQUIRE(reconstructed.layer_count() == state->document.layer_count());
    const auto &actual = *reconstructed.fresh_job();
    CHECK(actual.producer == expected.producer);
    CHECK(actual.parameter_origin == expected.parameter_origin);
    CHECK(actual.mask_precision == expected.mask_precision);
    CHECK(actual.position_policy == expected.position_policy);
    CHECK(actual.geometry.columns == expected.geometry.columns);
    CHECK(actual.geometry.rows == expected.geometry.rows);
    CHECK(actual.geometry.width_mm == expected.geometry.width_mm);
    CHECK(actual.geometry.height_mm == expected.geometry.height_mm);
    CHECK(actual.rows == expected.rows);
    CHECK(actual.globals.normal == expected.globals.normal);
    CHECK(actual.globals.bottom == expected.globals.bottom);
    CHECK(actual.globals.layer_height == expected.globals.layer_height);
    CHECK(actual.globals.bottom_layers == expected.globals.bottom_layers);
    CHECK(actual.globals.transition_layers == expected.globals.transition_layers);
    CHECK(palette_callbacks > 0);
    for (size_t layer = 0; layer < reconstructed.layer_count(); ++layer) {
        const auto before = state->document.encoded_layer(layer);
        const auto after = reconstructed.encoded_layer(layer);
        REQUIRE(before.sz == after.sz);
        CHECK(std::memcmp(before.buf, after.buf, before.sz) == 0);
    }
    std::ofstream report(output);
    REQUIRE(report);
    report << nlohmann::json{{"load_seconds", load_seconds}, {"validation_seconds", validation_seconds},
        {"layers", reconstructed.layer_count()}, {"columns", expected.geometry.columns},
        {"rows", expected.geometry.rows}, {"precision", int(expected.mask_precision)},
        {"encoded_bytes", encoded_bytes}, {"callbacks", callbacks}, {"palette_callbacks", palette_callbacks},
        {"png_byte_comparisons", reconstructed.layer_count()},
        {"scope", "public from_fresh_rasters including copy, PNG decode/CRC, geometry, fields and palette; excludes supports/export"}}.dump(2) << '\n';
    report.close(); REQUIRE(report);
}
