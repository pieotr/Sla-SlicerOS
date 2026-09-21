#include <catch2/catch_test_macros.hpp>
#include <boost/filesystem.hpp>
#include "libslic3r/SLA/LayerDocument.hpp"
#include "libslic3r/SLA/RasterBase.hpp"
#include "libslic3r/Zipper.hpp"
#include "libslic3r/Format/ZipperArchiveImport.hpp"
using namespace Slic3r;

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
