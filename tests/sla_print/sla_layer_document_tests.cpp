#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <boost/filesystem.hpp>
#include "libslic3r/SLA/LayerDocument.hpp"
#include "libslic3r/SLA/RasterBase.hpp"
#include "libslic3r/Zipper.hpp"
#include "libslic3r/Format/ZipperArchiveImport.hpp"
#include "libslic3r/SLAPrint.hpp"
#include "libslic3r/Format/SLAArchiveWriter.hpp"
#include "libslic3r/Format/3mf.hpp"
#include "libslic3r/SLA/ProjectArchive.hpp"
#include "libslic3r/SLA/ColorMask.hpp"
#include <atomic>
#include "libslic3r/TriangleMesh.hpp"
#include <fstream>
#include <nlohmann/json.hpp>
using namespace Slic3r;

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
    SECTION("Display changes discard masks that no longer match the printer") {
        config.set("display_pixels_x", 300);
        print.apply(model, config);
        REQUIRE(print.layer_document() == nullptr);
        print.process();
        REQUIRE(print.layer_document()->image(0).get(0, 0) == 0);
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
