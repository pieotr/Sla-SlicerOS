#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include "libslic3r/SLA/LayerImageFile.hpp"
#include "libslic3r/SLA/LayerDocument.hpp"
#include "libslic3r/SLA/LayerPreviewCache.hpp"
#include "libslic3r/SLA/RasterBase.hpp"
#include <boost/filesystem.hpp>
#include <fstream>
#include <cmath>
#include <future>
#include <thread>
using namespace Slic3r;

namespace {
sla::LayerDocument preview_document() {
    const std::vector<std::vector<uint8_t>> pixels{
        {0,1,127,0,254,0}, {10,0,0,128,0,255}, {0,99,2,0,0,17}};
    std::vector<sla::EncodedRaster> masks;
    for (const auto &mask:pixels)
        masks.push_back(sla::PNGRasterEncoder{}(mask.data(),3,2,1));
    return sla::LayerDocument::from_rasters(std::move(masks));
}
using PreviewCache=sla::LayerPreviewCache;
constexpr auto MainPlane=sla::LayerMaskPlane::Main;
}

TEST_CASE("Layer preview reuse retains literal grayscale without changing history", "[sla_layer_preview]") {
    auto document=preview_document(); PreviewCache cache;
    const auto revision=document.revision();
    CHECK(cache.find(document,0,2,MainPlane)==nullptr);
    auto prepared=PreviewCache::prepare(document,0,2,MainPlane);
    cache.publish(std::move(prepared),document,0,2,MainPlane);
    const auto *preview=cache.find(document,0,2,MainPlane); REQUIRE(preview);
    CHECK(preview->cols==3); CHECK(preview->rows==2);
    CHECK(preview->buf==std::vector<uint8_t>{10,99,127,128,254,255});
    for(unsigned redraw=0;redraw<10;++redraw)
        CHECK(cache.find(document,0,2,MainPlane)==preview);
    // The live stroke is a copy: drawing must not poison the reusable preview.
    auto stroke=*preview; sla::LayerDocument::draw_line(stroke,1,0,1,1,0,0);
    CHECK(stroke.buf==std::vector<uint8_t>{10,0,127,128,0,255});
    CHECK(preview->buf==std::vector<uint8_t>{10,99,127,128,254,255});
    CHECK(cache.find(document,1,2,MainPlane)==nullptr);
    CHECK(cache.find(document,0,1,MainPlane)==nullptr);
    CHECK(cache.find(document,0,2,sla::LayerMaskPlane::Secondary)==nullptr);
    CHECK(document.revision()==revision); CHECK_FALSE(document.dirty()); CHECK_FALSE(document.can_undo());
    document.mark_saved(); CHECK(cache.find(document,0,2,MainPlane)==preview);
    auto single=PreviewCache::prepare(document,1,1,MainPlane);
    cache.publish(std::move(single),document,1,1,MainPlane);
    REQUIRE(cache.find(document,1,1,MainPlane));
    CHECK(cache.find(document,1,1,MainPlane)->buf==std::vector<uint8_t>{10,0,0,128,0,255});
    CHECK(cache.find(document,0,2,MainPlane)==nullptr);
    cache.clear(); CHECK(cache.find(document,1,1,MainPlane)==nullptr);
}

TEST_CASE("Layer preview keys distinguish document ownership from equal revisions", "[sla_layer_preview]") {
    auto original=preview_document(); PreviewCache cache;
    cache.publish(PreviewCache::prepare(original,0,2,MainPlane),original,0,2,MainPlane);
    auto copy=original;
    REQUIRE(copy.revision()==original.revision());
    CHECK(cache.find(copy,0,2,MainPlane)==nullptr);
    auto moved=std::move(original); CHECK(cache.find(moved,0,2,MainPlane)!=nullptr);
    const auto *previous=cache.find(moved,0,2,MainPlane);
    auto other=PreviewCache::prepare(copy,0,2,MainPlane);
    CHECK_THROWS(cache.publish(std::move(other),moved,0,2,MainPlane));
    CHECK(cache.find(moved,0,2,MainPlane)==previous);
    // Assignment retires ownership even when address, revision and bytes match.
    moved=copy; CHECK(cache.find(moved,0,2,MainPlane)==nullptr);
    auto stale=PreviewCache::prepare(moved,0,2,MainPlane);
    moved=preview_document();
    CHECK_THROWS(cache.publish(std::move(stale),moved,0,2,MainPlane));
    CHECK(cache.find(moved,0,2,MainPlane)==nullptr);
}

TEST_CASE("Layer preview invalidation follows paint undo redo and late completion", "[sla_layer_preview]") {
    auto document=preview_document(); PreviewCache cache;
    cache.publish(PreviewCache::prepare(document,0,2,MainPlane),document,0,2,MainPlane);
    const auto old_revision=document.revision();
    document.paint(0,0,0,0,255);
    CHECK(cache.find(document,0,2,MainPlane)==nullptr);
    auto painted=PreviewCache::prepare(document,0,2,MainPlane);
    REQUIRE(document.undo()); REQUIRE(document.revision()==old_revision);
    CHECK(cache.find(document,0,2,MainPlane)==nullptr);
    CHECK_THROWS(cache.publish(std::move(painted),document,0,2,MainPlane));
    cache.publish(PreviewCache::prepare(document,0,2,MainPlane),document,0,2,MainPlane);
    REQUIRE(document.redo()); CHECK(cache.find(document,0,2,MainPlane)==nullptr);
    cache.publish(PreviewCache::prepare(document,0,2,MainPlane),document,0,2,MainPlane);
    REQUIRE(cache.find(document,0,2,MainPlane));
    CHECK(cache.find(document,0,2,MainPlane)->buf==std::vector<uint8_t>{255,99,127,128,254,255});
    const auto *current=cache.find(document,0,2,MainPlane);
    auto wrong_range=PreviewCache::prepare(document,0,1,MainPlane);
    CHECK_THROWS(cache.publish(std::move(wrong_range),document,0,2,MainPlane));
    auto wrong_plane=PreviewCache::prepare(document,0,2,MainPlane);
    CHECK_THROWS(cache.publish(std::move(wrong_plane),document,0,2,sla::LayerMaskPlane::Secondary));
    CHECK(cache.find(document,0,2,MainPlane)==current);
    REQUIRE(document.undo()); CHECK_FALSE(document.can_undo());
    CHECK(document.image(0).buf==std::vector<uint8_t>{0,1,127,0,254,0});
}

TEST_CASE("Cancelled or failed preview preparation preserves the previous frame and masks", "[sla_layer_preview]") {
    auto document=preview_document(); PreviewCache cache;
    cache.publish(PreviewCache::prepare(document,0,0,MainPlane),document,0,0,MainPlane);
    const auto *old=cache.find(document,0,0,MainPlane);
    size_t calls=0;
    PreviewCache::prepare(document,0,2,MainPlane,[&](size_t,size_t){++calls;return true;});
    REQUIRE(calls>=4);
    for(size_t cancel=0;cancel<calls;++cancel) {
        size_t checkpoint=0;
        CHECK_THROWS_AS(PreviewCache::prepare(document,0,2,MainPlane,[&](size_t,size_t){return checkpoint++!=cancel;}),sla::LayerOperationCancelled);
        CHECK(cache.find(document,0,0,MainPlane)==old);
        CHECK_FALSE(document.dirty()); CHECK_FALSE(document.can_undo());
    }
    CHECK_THROWS_AS(PreviewCache::prepare(document,0,2,MainPlane,[](size_t,size_t)->bool{throw std::logic_error("callback failure");}),std::logic_error);
    CHECK_THROWS(PreviewCache::prepare(document,2,1,MainPlane));
    CHECK_THROWS(PreviewCache::prepare(document,0,3,MainPlane));
    CHECK(cache.find(document,0,0,MainPlane)==old);
    cache.publish(PreviewCache::prepare(document,0,2,MainPlane),document,0,2,MainPlane);
    REQUIRE(cache.find(document,0,2,MainPlane));
    CHECK(cache.find(document,0,2,MainPlane)->buf==std::vector<uint8_t>{10,99,127,128,254,255});
}

TEST_CASE("Layer preview preparation performs full PNG validation before publication", "[sla_layer_preview]") {
    auto good=preview_document(); PreviewCache cache;
    cache.publish(PreviewCache::prepare(good,0,0,MainPlane),good,0,0,MainPlane);
    const auto *old=cache.find(good,0,0,MainPlane);
    for(size_t broken_layer:{size_t(0),size_t(1),size_t(2)}) {
        std::vector<sla::EncodedRaster> masks;
        for(size_t i=0;i<3;++i) {
            const auto bytes=good.encoded_layer(i);
            const auto *data=static_cast<const uint8_t*>(bytes.buf);
            std::vector<uint8_t> png(data,data+bytes.sz);
            if(i==broken_layer)png.back()^=1; // IEND CRC, after otherwise valid pixels.
            masks.emplace_back(std::move(png),"png");
        }
        const auto bad=sla::LayerDocument::from_rasters(std::move(masks));
        CHECK_THROWS(PreviewCache::prepare(bad,0,2,MainPlane));
        CHECK(cache.find(good,0,0,MainPlane)==old);
    }
}

TEST_CASE("Layer preview workers reject source changes at a suspended checkpoint", "[sla_layer_preview]") {
    auto document=preview_document(); PreviewCache cache;
    std::promise<void> reached,resume;
    auto wait=resume.get_future();
    auto worker=std::async(std::launch::async,[&] {
        bool stopped=false;
        return PreviewCache::prepare(document,0,2,MainPlane,[&](size_t done,size_t) {
            if(done==1 && !stopped) {stopped=true;reached.set_value();wait.wait();}
            return true;
        });
    });
    reached.get_future().wait();
    // No concurrent buffer access: the worker is parked inside its callback.
    document.paint(2,0,0,0,255); resume.set_value();
    CHECK_THROWS(worker.get()); CHECK(cache.find(document,0,2,MainPlane)==nullptr);
    auto retry=std::async(std::launch::async,[&] {return PreviewCache::prepare(document,0,2,MainPlane);});
    auto prepared=retry.get();
    cache.publish(std::move(prepared),document,0,2,MainPlane);
    REQUIRE(cache.find(document,0,2,MainPlane));
    CHECK(cache.find(document,0,2,MainPlane)->buf==std::vector<uint8_t>{255,99,127,128,254,255});
    REQUIRE(document.undo()); CHECK_FALSE(document.can_undo());
}

TEST_CASE("Native standalone image formats preserve oriented grayscale masks", "[sla_images][sla_layers]") {
    const auto index = GENERATE(0u,1u,2u,3u,4u,5u,6u,7u,8u,9u,10u);
    const auto shape = GENERATE(0u,1u,2u);
    const auto &format = sla::layer_image_formats().at(index);
    CAPTURE(format.id, shape);
    png::ImageGreyscale source;
    source.cols = shape == 0 ? 1 : shape == 1 ? 67 : 257;
    source.rows = shape == 0 ? 1 : shape == 1 ? 73 : 64;
    source.buf.resize(source.cols * source.rows);
    // Odd row strides, all 256 gray values, distinct orientation and smooth
    // gradients let lossless, thresholded and JPEG masks be checked separately.
    for (size_t y=0; y<source.rows; ++y) for (size_t x=0; x<source.cols; ++x)
        source.buf[y*source.cols+x] = uint8_t((x + y*3) % 256);
    const auto pristine = source.buf;
    const auto encoded = sla::encode_layer_image(source, format.id);
    REQUIRE_FALSE(encoded.empty());
    // Encoding must not touch the caller's image (libtiff <= 4.3 applied the predictor in place).
    REQUIRE(source.buf == pristine);
    const auto decoded = sla::decode_layer_image({encoded.data(),encoded.size()}, format.id);
    REQUIRE(decoded.cols == source.cols);
    REQUIRE(decoded.rows == source.rows);
    REQUIRE(decoded.buf.size() == source.buf.size());
    if (format.binary) {
        for (size_t n=0; n<source.buf.size(); ++n)
            REQUIRE(decoded.buf[n] == (source.buf[n] > 0 ? 255 : 0));
    } else if (format.lossy) {
        double error = 0;
        for (size_t n=0; n<source.buf.size(); ++n) error += std::abs(int(decoded.buf[n])-source.buf[n]);
        REQUIRE(error / source.buf.size() < 1.5);
    } else REQUIRE(decoded.buf == source.buf);
    REQUIRE(sla::find_layer_image_format(std::string(".")+format.id) == &format);
    auto truncated = encoded;
    truncated.resize(std::min(size_t(8),encoded.size()/2));
    REQUIRE_THROWS(sla::decode_layer_image({truncated.data(),truncated.size()},format.id));
}

TEST_CASE("PNM binary pixel whitespace and 16-bit values are data", "[sla_images][sla_layers]") {
    const std::string pgm = "P5\r\n# comment\r\n4 1\r\n255\r\n" + std::string("\n\r\t ",4);
    const auto restored = sla::decode_layer_image({pgm.data(),pgm.size()},"pgm");
    REQUIRE(restored.buf == std::vector<uint8_t>{10,13,9,32});
    const std::string wide = "P5\n4 1\n65535\n" + std::string("\0\0\x12\x34\x80\0\xff\xff",8);
    REQUIRE(sla::decode_layer_image({wide.data(),wide.size()},"pgm").buf == std::vector<uint8_t>{0,18,128,255});
    const std::string ascii = "P1\n# bitmap\n5 1\n0 1 0 1 1\n";
    REQUIRE(sla::decode_layer_image({ascii.data(),ascii.size()},"pbm").buf == std::vector<uint8_t>{255,0,255,0,0});
}

TEST_CASE("Layer image encoding rejects dimensions, incomplete pixels and unknown formats", "[sla_images][sla_layers]") {
    png::ImageGreyscale source;source.rows=1;source.cols=32769;
    REQUIRE_THROWS(sla::encode_layer_image(source,"bmp"));
    source.cols=1;
    REQUIRE_THROWS(sla::encode_layer_image(source,"png"));
    source.buf={255};
    REQUIRE_THROWS(sla::encode_layer_image(source,"ctb"));
    REQUIRE(sla::find_layer_image_format("JPEG") != nullptr);
    REQUIRE(sla::find_layer_image_format("ctb") == nullptr);
}

TEST_CASE("Export standalone image fixtures for original UVtools", "[.sla_image_fixtures]") {
    const boost::filesystem::path root("sla-image-fixtures");
    boost::filesystem::create_directories(root);
    png::ImageGreyscale source; source.cols=257; source.rows=73;
    source.buf.resize(source.cols*source.rows);
    for (size_t y=0; y<source.rows; ++y) for (size_t x=0; x<source.cols; ++x)
        source.buf[y*source.cols+x]=uint8_t((x+y*3)%256);
    std::ofstream raw((root/"source.gray").string(),std::ios::binary);
    raw.write(reinterpret_cast<const char *>(source.buf.data()),source.buf.size());raw.close();REQUIRE(raw.good());
    for (const auto &format:sla::layer_image_formats()) {
        const auto bytes=sla::encode_layer_image(source,format.id);
        std::ofstream file((root/(std::string("native.")+format.id)).string(),std::ios::binary);
        file.write(reinterpret_cast<const char *>(bytes.data()),bytes.size());file.close();REQUIRE(file.good());
    }
}

TEST_CASE("Image editing preserves original format and undo until explicitly saved", "[sla_images][sla_layers]") {
    const auto index = GENERATE(0u,1u,2u,3u,4u,5u,6u,7u,8u,9u,10u);
    const auto &format = sla::layer_image_formats().at(index);
    struct Files {
        boost::filesystem::path root=boost::filesystem::unique_path("image-edit-%%%%");
        Files(){boost::filesystem::create_directory(root);}
        ~Files(){boost::system::error_code ignored;boost::filesystem::remove_all(root,ignored);}
    } files;
    png::ImageGreyscale source;source.cols=19;source.rows=17;source.buf.assign(19*17,255);
    const auto encoded=sla::encode_layer_image(source,format.id);
    const auto input=files.root/(std::string("input.")+format.id);
    {std::ofstream stream(input.string(),std::ios::binary);stream.write(reinterpret_cast<const char *>(encoded.data()),encoded.size());}
    auto doc=sla::LayerDocument::load_archive(input.string());
    REQUIRE(doc.archive_format()==format.id);
    REQUIRE(doc.layer_count()==1);
    REQUIRE(doc.image(0).buf==source.buf);
    doc.invert(0);REQUIRE(doc.dirty());REQUIRE(doc.can_undo());
    const auto output=files.root/(std::string("output.")+format.id);
    doc.save_archive(output.string());REQUIRE_FALSE(doc.dirty());
    auto reopened=sla::LayerDocument::load_archive(output.string());
    REQUIRE(reopened.image(0).buf==std::vector<uint8_t>(19*17,0));
    REQUIRE(doc.undo());REQUIRE(doc.dirty());REQUIRE(doc.image(0).buf==source.buf);
    REQUIRE_THROWS(doc.save_archive((files.root/"wrong.ctb").string()));
}

TEST_CASE("Mask range export publishes all layers or nothing without marking a job saved", "[sla_images][sla_layers]") {
    struct Files {
        boost::filesystem::path root=boost::filesystem::unique_path("image-range-%%%%");
        Files(){boost::filesystem::create_directory(root);}
        ~Files(){boost::system::error_code ignored;boost::filesystem::remove_all(root,ignored);}
    } files;
    std::vector<sla::EncodedRaster> masks;
    for(uint8_t value:{0,17,255}) {
        const std::vector<uint8_t> pixels(35,value);
        masks.push_back(sla::PNGRasterEncoder{}(pixels.data(),7,5,1));
    }
    auto doc=sla::LayerDocument::from_rasters(std::move(masks));doc.invert(1);
    const auto cancelled=files.root/"cancelled";
    REQUIRE_FALSE(doc.export_images(cancelled.string(),"pgm",0,2,[](size_t done,size_t){return done<2;}));
    REQUIRE_FALSE(boost::filesystem::exists(cancelled));
    REQUIRE(std::distance(boost::filesystem::directory_iterator(files.root),boost::filesystem::directory_iterator{})==0);
    const auto range=files.root/"completed";
    REQUIRE(doc.export_images(range.string(),"bmp",0,2));REQUIRE(doc.dirty());
    for(size_t n=0;n<3;++n) {
        const auto path=range/(std::string("layer-00000")+std::to_string(n+1)+".bmp");
        REQUIRE(sla::LayerDocument::load_archive(path.string()).image(0).buf==doc.image(n).buf);
    }
    REQUIRE_THROWS(doc.export_images(range.string(),"bmp",0,2));
    REQUIRE_THROWS(doc.export_images((files.root/"bad").string(),"pgm",2,0));
    const auto single=files.root/"one.pgm";
    REQUIRE(doc.export_images(single.string(),"pgm",1,1));
    REQUIRE_FALSE(doc.export_images(single.string(),"pgm",2,2,[](size_t done,size_t){return done==0;}));
    REQUIRE(sla::LayerDocument::load_archive(single.string()).image(0).buf==doc.image(1).buf);
    REQUIRE(doc.dirty());REQUIRE(doc.can_undo());
}

TEST_CASE("Native image importer matches original UVtools image codecs pixel for pixel", "[.sla_image_import_oracle]") {
    const boost::filesystem::path root("sla-image-fixtures");
    for (const auto &format : sla::layer_image_formats()) {
        CAPTURE(format.id);
        const bool sun=std::string(format.id)=="sr" || std::string(format.id)=="ras";
        const auto prefix=sun?"original-rgb.":"original.";
        const auto path=root/(std::string(prefix)+format.id);
        const auto raw=root/(std::string(prefix)+format.id+".gray");
        REQUIRE(boost::filesystem::exists(path));REQUIRE(boost::filesystem::exists(raw));
        std::ifstream file(raw.string(),std::ios::binary);
        const std::vector<uint8_t> expected{std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>()};
        const auto doc=sla::LayerDocument::load_archive(path.string());
        REQUIRE(doc.layer_count()==1);REQUIRE(doc.image(0).cols==257);REQUIRE(doc.image(0).rows==73);
        REQUIRE(doc.image(0).buf==expected);
    }
}

TEST_CASE("PBM preview and undo use the same threshold as the saved mask", "[sla_images][sla_layers]") {
    struct Files {
        boost::filesystem::path file=boost::filesystem::unique_path("pbm-edit-%%%%.pbm");
        ~Files(){boost::system::error_code ignored;boost::filesystem::remove(file,ignored);}
    } files;
    png::ImageGreyscale source;source.rows=1;source.cols=4;source.buf={0,0,255,255};
    const auto encoded=sla::encode_layer_image(source,"pbm");
    {std::ofstream file(files.file.string(),std::ios::binary);file.write(reinterpret_cast<const char *>(encoded.data()),encoded.size());}
    auto doc=sla::LayerDocument::load_archive(files.file.string());
    source.buf={17,128,127,254};doc.replace_image(0,source);
    REQUIRE(doc.image(0).buf==std::vector<uint8_t>{255,255,255,255});
    doc.save_archive(files.file.string());
    REQUIRE(sla::LayerDocument::load_archive(files.file.string()).image(0).buf==doc.image(0).buf);
    REQUIRE(doc.undo());REQUIRE(doc.image(0).buf==std::vector<uint8_t>{0,0,255,255});
    REQUIRE(doc.transform_layers(0,0,sla::LayerDocument::MaskOperation::Fill,127));
    REQUIRE(doc.image(0).buf==std::vector<uint8_t>{255,255,255,255});
}

TEST_CASE("PNM Maxval matches original UVtools ASCII and binary grayscale semantics", "[sla_images][sla_layers]") {
    const std::string binary = "P5\n6 1\n31\n" + std::string("\0\1\17\20\36\37",6);
    REQUIRE(sla::decode_layer_image({binary.data(),binary.size()},"pgm").buf==std::vector<uint8_t>{0,1,15,16,30,31});
    const std::string ascii = "P2\n6 1\n31\n0 1 15 16 30 31\n";
    REQUIRE(sla::decode_layer_image({ascii.data(),ascii.size()},"pgm").buf==std::vector<uint8_t>{0,8,123,131,246,255});
    const std::string wide = "P5\n7 1\n1000\n" + std::string("\0\0\0\1\0\xff\1\0\1\xf4\3\xe7\3\xe8",14);
    REQUIRE(sla::decode_layer_image({wide.data(),wide.size()},"pgm").buf==std::vector<uint8_t>{0,0,0,1,1,3,3});
}
