#include "libslic3r/Format/ChituboxArchive.hpp"
#include "libslic3r/Format/ChituboxLayerLayout.hpp"
#include "libslic3r/Format/SLAArchiveFormatRegistry.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/SLA/LayerDocument.hpp"
#include "libslic3r/SLAPrint.hpp"
#include "sla_archive_test_config.hpp"
#include <array>
#include <boost/filesystem.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <nlohmann/json.hpp>
using namespace Slic3r;

TEST_CASE("CBDDLP and Photon versions decode every AA plane count and preserve layer tables",
          "[sla_cbddlp_versions][sla_reference]") {
    const auto portrait = GENERATE(false, true);
    const std::string family = GENERATE("cbddlp", "photon");
    const char *folder = std::getenv("SLA_CBDDLP_FIXTURES");
    struct Directory {
        boost::filesystem::path path;
        bool keep;
        ~Directory() {
            if (!keep) {
                boost::system::error_code ignored;
                boost::filesystem::remove_all(path, ignored);
            }
        }
    } dir{folder ? boost::filesystem::path(folder) : boost::filesystem::unique_path("cbddlp-aa-%%%%"),
          bool(folder)};
    boost::filesystem::create_directories(dir.path);
    for (unsigned version : {1u, 2u})
        for (unsigned aa = 1; aa <= (version == 1 ? 1u : 16u); ++aa) {
            CAPTURE(family, portrait, version, aa);
            const std::string id = version == 1 ? family + "_v1" : family;
            SLAFullPrintConfig full;
            Test::configure_archive_fixture(full, id);
            full.set("sla_cbddlp_aa", int(aa));
            full.display_orientation.value = portrait ? sladoPortrait : sladoLandscape;
            full.set("display_pixels_x", 257);
            full.set("display_pixels_y", 64);
            full.set("sla_bottom_layers", 1);
            full.set("sla_transition_layers", 1);
            full.set("sla_light_pwm", 200);
            full.set("sla_bottom_light_pwm", 160);
            full.set("max_print_height", 160.);
            full.set("material_density", 1.11);
            full.set("bottle_volume", 500.);
            full.set("bottle_cost", 123.);
            full.set("sla_wait_before_cure", 1.25);
            full.set("sla_bottom_wait_before_cure", 2.5);
            DynamicPrintConfig cfg;
            cfg.apply(full);
            const size_t width = portrait ? 64 : 257, height = portrait ? 257 : 64;
            std::vector<sla::EncodedRaster> masks;
            std::vector<uint8_t> pixels(width * height, 255);
            masks.push_back(sla::PNGRasterEncoder{}(pixels.data(), width, height, 1));
            std::fill(pixels.begin(), pixels.end(), 0);
            masks.push_back(sla::PNGRasterEncoder{}(pixels.data(), width, height, 1));
            for (size_t y = 0; y < height; ++y)
                for (size_t x = 0; x < width; ++x)
                    pixels[y * width + x] = uint8_t((3 * x + 7 * y) % 256);
            const auto gray = sla::PNGRasterEncoder{}(pixels.data(), width, height, 1);
            masks.push_back(gray);
            masks.push_back(gray);
            const auto document = sla::LayerDocument::from_rasters(std::move(masks));
            const auto print = SLAPrint::create_raster_job(cfg, document, {.05, .13, .2, .3});
            const auto stem = std::string(portrait ? "portrait-" : "landscape-") + family + "-v" +
                              std::to_string(version) + "-aa" + std::to_string(aa);
            const auto path = dir.path / (stem + "." + get_archive_entry(id.c_str())->ext);
            ThumbnailData preview;
            preview.set(2, 1);
            preview.pixels = {255, 0, 0, 255, 0, 0, 255, 255};
            print->export_layer_document(document, id, path.string(), {preview}, stem);
            std::ifstream input(path.string(), std::ios::binary);
            const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(input), {}};
            auto u32 = [&](size_t at) {
                REQUIRE(at + 4 <= bytes.size());
                uint32_t v = 0;
                for (unsigned n = 0; n < 4; ++n)
                    v |= uint32_t(bytes[at + n]) << (8 * n);
                return v;
            };
            auto f32 = [&](size_t at) {
                const auto word = u32(at);
                float f;
                std::memcpy(&f, &word, 4);
                return f;
            };
            REQUIRE(u32(0) == 0x12fd0019);
            REQUIRE(u32(4) == version);
            REQUIRE(u32(92) == aa);
            REQUIRE(u32(52) == width);
            REQUIRE(u32(56) == height);
            REQUIRE(u32(68) == 4);
            REQUIRE(u32(84) == (version == 1 ? 0 : 112));
            REQUIRE(u32(88) == 60);
            REQUIRE(u32(60) == (version == 1 ? 112 : 172));
            REQUIRE(u32(u32(60)) == 400);
            REQUIRE(u32(u32(60) + 4) == 300);
            REQUIRE(u32(u32(72)) == 200);
            REQUIRE(u32(u32(72) + 4) == 125);
            REQUIRE(u32(64) == u32(72) + 32 + u32(u32(72) + 12));
            auto imported = sla::LayerDocument::load_archive(path.string());
            REQUIRE(imported.archive_format() == id);
            REQUIRE(imported.layer_count() == 4);
            for (size_t layer = 0; layer < 4; ++layer) {
                const auto source = document.image(layer);
                std::vector<unsigned> levels(source.buf.size(), 0);
                for (unsigned plane = 0; plane < aa; ++plane) {
                    const size_t at = u32(64) + 36 * (plane * 4 + layer);
                    REQUIRE(f32(at) == Catch::Approx((std::array<double, 4>{.05, .13, .2, .3}[layer])));
                    REQUIRE(f32(at + 4) ==
                            Catch::Approx((std::array<double, 4>{20., 11.25, 2.5, 2.5}[layer])));
                    const auto address = u32(at + 12), size = u32(at + 16);
                    REQUIRE(address + uint64_t(size) <= bytes.size());
                    REQUIRE(u32(at + 24) == 36);
                    size_t cursor = 0;
                    for (size_t n = 0; n < size; ++n) {
                        const auto token = bytes[address + n];
                        const unsigned run = token & 127;
                        REQUIRE(run > 0);
                        REQUIRE(run <= 125);
                        REQUIRE(cursor + run <= levels.size());
                        if (token & 128)
                            for (unsigned i = 0; i < run; ++i)
                                ++levels[cursor + i];
                        cursor += run;
                    }
                    REQUIRE(cursor == levels.size());
                }
                const auto decoded = imported.image(layer);
                for (size_t pixel = 0; pixel < levels.size(); ++pixel) {
                    const unsigned step = 256 / aa, gray = source.buf[pixel];
                    unsigned expected = gray == 255 ? aa : std::min(aa - 1, (gray + 1) / step);
                    REQUIRE(levels[pixel] == expected);
                    REQUIRE(decoded.buf[pixel] == (expected ? expected * step - 1 : 0));
                }
            }
            const auto snapshots = dir.path / "editor";
            boost::filesystem::create_directories(snapshots);
            const auto copied = snapshots / ("copy-" + path.filename().string());
            imported.save_archive(copied.string());
            std::ifstream copy_input(copied.string(), std::ios::binary);
            REQUIRE(std::vector<uint8_t>{std::istreambuf_iterator<char>(copy_input), {}} == bytes);
            const auto before = imported.image(0).buf;
            imported.invert(0);
            const auto edited = imported.image(0).buf;
            REQUIRE(std::all_of(edited.begin(), edited.end(), [](uint8_t pixel) { return pixel == 0; }));
            REQUIRE(imported.undo());
            REQUIRE(imported.image(0).buf == before);
            REQUIRE(imported.redo());
            REQUIRE(imported.image(0).buf == edited);
            const auto edited_path = snapshots / ("edited-" + path.filename().string());
            imported.save_archive(edited_path.string());
            const auto reopened = sla::LayerDocument::load_archive(edited_path.string());
            REQUIRE(reopened.image(0).buf == edited);
            for (size_t layer = 1; layer < 4; ++layer)
                REQUIRE(reopened.image(layer).buf == imported.image(layer).buf);
            // Editing one layer must preserve other bitplanes, including the
            // maximum white representation for AA counts that do not divide 256.
            auto gray_edit = imported.image(2);
            gray_edit.buf[0] = 255;
            imported.replace_image(2, gray_edit);
            REQUIRE(imported.image(2).buf[0] == uint8_t(aa * (256 / aa) - 1));
            gray_edit.buf[0] = uint8_t(aa * (256 / aa) - 1);
            REQUIRE(imported.image(2).buf == gray_edit.buf);
            imported.save_archive(edited_path.string());
            REQUIRE(sla::LayerDocument::load_archive(edited_path.string()).image(2).buf ==
                    imported.image(2).buf);
            for (unsigned scenario = 0; scenario < 3; ++scenario) {
                auto invalid = bytes;
                auto put = [&](size_t at, uint32_t value) {
                    for (unsigned n = 0; n < 4; ++n)
                        invalid[at + n] = uint8_t(value >> (8 * n));
                };
                const auto row = u32(64);
                const auto data = u32(row + 12);
                if (scenario == 0)
                    invalid[data] = 0; // Zero-length plane run.
                if (scenario == 1)
                    put(row + 16, 1); // Incomplete complete-mask payload.
                if (scenario == 2)
                    put(row + 12, 0); // Layer overlaps the header.
                const auto corrupt = snapshots / ("invalid-" + path.filename().string());
                {
                    std::ofstream output(corrupt.string(), std::ios::binary | std::ios::trunc);
                    output.write(reinterpret_cast<const char *>(invalid.data()), invalid.size());
                }
                REQUIRE_THROWS(sla::LayerDocument::load_archive(corrupt.string()));
            }
            if (folder) {
                print->export_layer_document(document, "uvj", (dir.path / (stem + ".uvj")).string(), {},
                                             stem);
                std::ofstream info((path.string() + ".json"));
                info << nlohmann::json{{"version", version}, {"aa", aa},         {"family", family},
                                       {"width", width},     {"height", height}, {"source", stem + ".uvj"}}
                            .dump(2)
                     << '\n';
            }
        }
}

TEST_CASE("CBDDLP AA belongs to printer presets and invalid counts are rejected before slicing",
          "[sla_cbddlp_versions][sla_profiles]") {
    REQUIRE(std::find(Preset::sla_printer_options().begin(), Preset::sla_printer_options().end(),
                      "sla_cbddlp_aa") != Preset::sla_printer_options().end());
    SLAFullPrintConfig full;
    Test::configure_archive_fixture(full, "cbddlp");
    const std::vector<uint8_t> pixels(200 * 120, 255);
    auto document = sla::LayerDocument::from_rasters({sla::PNGRasterEncoder{}(pixels.data(), 200, 120, 1)});
    for (int aa : {0, 17}) {
        full.set("sla_cbddlp_aa", aa);
        DynamicPrintConfig config;
        config.apply(full);
        REQUIRE_THROWS(SLAPrint::create_raster_job(config, document, {.05}));
    }
    SLAPrinterConfig printer;
    printer.sla_cbddlp_aa.value = 4;
    DynamicPrintConfig copied;
    copied.apply(printer);
    SLAPrinterConfig restored;
    restored.apply(copied);
    REQUIRE(restored.sla_cbddlp_aa.value == 4);
    REQUIRE(std::string(get_archive_entry("cbddlp_v1")->ext) == "v1.cbddlp");
    REQUIRE(std::string(get_archive_entry("photon_v1")->ext) == "v1.photon");
    REQUIRE(sla_archive_filename("job.photon", "v1.photon") == "job.v1.photon");
}

TEST_CASE("CTB editor preserves addressed metadata and encrypted v1/v2 masks",
          "[sla_chitubox_editor][sla_reference]") {
    const std::string id = GENERATE("ctb_v1", "ctb");
    const bool encrypted = GENERATE(false, true);
    SLAFullPrintConfig full;
    Test::configure_archive_fixture(full, id);
    full.set("sla_bottom_layers", 1);
    full.set("sla_transition_layers", 1);
    DynamicPrintConfig config;
    config.apply(full);
    std::vector<uint8_t> pixels(200 * 120);
    for (size_t i = 0; i < pixels.size(); ++i)
        pixels[i] = uint8_t(i);
    const auto raster = sla::PNGRasterEncoder{}(pixels.data(), 200, 120, 1);
    auto source = sla::LayerDocument::from_rasters({raster, raster});
    auto job = SLAPrint::create_raster_job(config, source, {.05, .13});
    const auto *folder = std::getenv("SLA_CHITUBOX_EDITOR_FIXTURES");
    struct Directory {
        boost::filesystem::path path;
        bool keep;
        ~Directory() {
            if (!keep) {
                boost::system::error_code e;
                boost::filesystem::remove_all(path, e);
            }
        }
    } dir{folder ? boost::filesystem::path(folder) : boost::filesystem::unique_path("ctb-editor-%%%%"),
          bool(folder)};
    boost::filesystem::create_directories(dir.path);
    const auto stem = id + (encrypted ? "-seed" : "-clear");
    const auto path = dir.path / (stem + "." + get_archive_entry(id.c_str())->ext);
    job->export_layer_document(source, id, path.string(), {}, stem);
    std::ifstream in(path.string(), std::ios::binary);
    std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(in), {}};
    if (encrypted) {
        const sla::ChituboxLayerLayout layout(bytes);
        const uint32_t seed = 0xEFBEADDE;
        for (unsigned n = 0; n < 4; ++n)
            bytes[100 + n] = uint8_t(seed >> (8 * n));
        for (size_t layer = 0; layer < 2; ++layer) {
            const auto at = layout.record(layer);
            const auto data = layout.integer(at + 12), length = layout.integer(at + 16);
            std::vector<uint8_t> payload(bytes.begin() + data, bytes.begin() + data + length);
            ctb_encrypted::xor_layer(payload, uint32_t(layer), seed);
            std::copy(payload.begin(), payload.end(), bytes.begin() + data);
        }
        std::ofstream encrypted_file(path.string(), std::ios::binary | std::ios::trunc);
        encrypted_file.write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
    }
    auto document = sla::LayerDocument::load_archive(path.string());
    REQUIRE(document.archive_format() == id);
    REQUIRE(document.layer_count() == 2);
    auto expected = pixels;
    for (auto &value : expected)
        value = value < 2 ? 0 : uint8_t(value | 1);
    REQUIRE(document.image(0).buf == expected);
    REQUIRE(document.image(1).buf == expected);
    const auto copy = dir.path / ("copy-" + path.filename().string());
    document.save_archive(copy.string());
    std::ifstream copied(copy.string(), std::ios::binary);
    REQUIRE(std::vector<uint8_t>{std::istreambuf_iterator<char>(copied), {}} == bytes);
    document.invert(0);
    auto edited = expected;
    for (auto &value : edited) {
        value = 255 - value;
        value = value < 2 ? 0 : uint8_t(value | 1);
    }
    REQUIRE(document.image(0).buf == edited);
    REQUIRE(document.undo());
    REQUIRE(document.image(0).buf == expected);
    REQUIRE(document.redo());
    const auto target = dir.path / ("edited-" + path.filename().string());
    document.save_archive(target.string());
    const auto reopened = sla::LayerDocument::load_archive(target.string());
    REQUIRE(reopened.image(0).buf == edited);
    REQUIRE(reopened.image(1).buf == expected);
    const sla::ChituboxLayerLayout layout(bytes);
    std::ifstream out(target.string(), std::ios::binary);
    const std::vector<uint8_t> saved{std::istreambuf_iterator<char>(out), {}};
    for (size_t i = 0; i < 112; ++i)
        REQUIRE(saved[i] == bytes[i]); // Header and preview addresses survive.
    for (size_t i = 0; i < 12; ++i)
        REQUIRE(saved[layout.record(0) + i] == bytes[layout.record(0) + i]);
    for (const auto &section : layout.metadata) {
        if (section.begin == layout.parameters)
            continue; // Only volume/mass/cost may change.
        if (section.begin == layout.table)
            continue; // Only edited payload address/length change.
        REQUIRE(std::equal(bytes.begin() + section.begin, bytes.begin() + section.end,
                           saved.begin() + section.begin));
    }
    if (folder) {
        std::ofstream expected_file((target.string() + ".json"));
        expected_file << nlohmann::json{{"version", id == "ctb" ? 2 : 1},
                                        {"seed", encrypted},
                                        {"original", path.filename().string()},
                                        {"width", 200},
                                        {"height", 120},
                                        {"edited_pixels", edited},
                                        {"unchanged_pixels", expected}}
                             .dump()
                      << '\n';
    }
    auto truncated = bytes;
    const auto first = layout.record(0);
    for (unsigned n = 0; n < 4; ++n)
        truncated[first + 16 + n] = uint8_t(1u >> (8 * n));
    REQUIRE_THROWS(sla::ChituboxLayerLayout(truncated).decode(0));
    // Reject malformed addressed sections before allocating or decoding masks.
    for (unsigned scenario = 0; scenario < 5; ++scenario) {
        auto invalid = bytes;
        auto put = [&](size_t at, uint32_t value) {
            for (unsigned n = 0; n < 4; ++n)
                invalid[at + n] = uint8_t(value >> (8 * n));
        };
        if (scenario == 0)
            put(52, UINT32_MAX);
        if (scenario == 1)
            put(64, UINT32_MAX);
        if (scenario == 2)
            put(layout.record(0) + 12, 0);
        if (scenario == 3)
            put(layout.record(0) + 16, UINT32_MAX);
        if (scenario == 4)
            put(layout.record(0) + 20, 1);
        REQUIRE_THROWS(sla::ChituboxLayerLayout(invalid));
    }
}

// Development fixture: native import of files produced by the original tool,
// followed by independent PNG hashing against its full-decoder reports.
TEST_CASE("Import original Chitubox fixtures and export canonical PNG masks", "[.sla_chitubox_import]") {
    const char *input = std::getenv("SLA_CHITUBOX_IMPORT_INPUT"),
               *output = std::getenv("SLA_CHITUBOX_IMPORT_OUTPUT");
    REQUIRE(input != nullptr);
    REQUIRE(output != nullptr);
    const boost::filesystem::path destination(output);
    boost::filesystem::create_directories(destination);
    nlohmann::json report = nlohmann::json::array();
    for (const auto &file : boost::filesystem::directory_iterator(input)) {
        const auto extension = file.path().extension().string();
        if (extension != ".ctb" && extension != ".cbddlp" && extension != ".photon")
            continue;
        auto document = sla::LayerDocument::load_archive(file.path().string());
        REQUIRE(document.layer_count() > 1);
        const auto masks = destination / file.path().filename();
        REQUIRE(document.export_images(masks.string(), "png", 0, document.layer_count() - 1));
        const auto copied = destination / (file.path().stem().string() + "-copy." +
                                           get_archive_entry(document.archive_format().c_str())->ext);
        document.save_archive(copied.string());
        std::ifstream a(file.path().string(), std::ios::binary), b(copied.string(), std::ios::binary);
        REQUIRE(std::vector<uint8_t>{std::istreambuf_iterator<char>(a), {}} ==
                std::vector<uint8_t>{std::istreambuf_iterator<char>(b), {}});
        report.push_back({{"filename", boost::filesystem::absolute(file.path()).string()},
                          {"masks", boost::filesystem::absolute(masks).string()},
                          {"layers", document.layer_count()},
                          {"copied_bytes_exact", true}});
    }
    REQUIRE(!report.empty());
    std::ofstream result((destination / "native-import.json").string());
    result << report.dump(2) << '\n';
}
