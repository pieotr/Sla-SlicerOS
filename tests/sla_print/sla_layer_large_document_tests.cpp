// Bounded-memory layer documents: archives whose decoded size or file size exceeds the old fixed 1 GiB
// budget, undo history that spills to a temporary file, and the refusal messages around both.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>
#include <boost/filesystem.hpp>
#include <chrono>
#include <fstream>
#include <thread>
#include <random>
#include "libslic3r/SLAPrint.hpp"
#include "libslic3r/SLA/LayerDocument.hpp"
#include "libslic3r/SLA/LayerMemoryBudget.hpp"
#include "libslic3r/Format/ChituboxLayerLayout.hpp"
#include "libslic3r/Format/CTBEncryptedLayerLayout.hpp"
#include "libslic3r/Format/AnycubicLayerLayout.hpp"
#include "libslic3r/Format/CrealityCXDLPv4Layout.hpp"
#include "libslic3r/Format/PhrozenLayout.hpp"
#include "libslic3r/Format/CrealityCXDLPv1Layout.hpp"
#include "libslic3r/Format/VlareLayout.hpp"
#include "libslic3r/Format/AnetLayout.hpp"
#include "libslic3r/Format/MakerbaseLayout.hpp"
#include "libslic3r/Format/Emake3DLayout.hpp"
#include "libslic3r/Format/SLAArchiveFormatRegistry.hpp"
#include "sla_archive_test_config.hpp"
#include "libslic3r/SLA/RasterBase.hpp"
#include "libslic3r/Zipper.hpp"
using namespace Slic3r;
using Catch::Matchers::ContainsSubstring;

namespace {
struct TempPath {
    boost::filesystem::path path;
    explicit TempPath(const std::string &pattern) : path(boost::filesystem::temp_directory_path() / boost::filesystem::unique_path(pattern)) {}
    ~TempPath() { boost::system::error_code ec; boost::filesystem::remove_all(path, ec); }
    std::string str() const { return path.string(); }
};

// Mostly empty layer with a marker that is unique per layer, so any mix-up is visible.
std::vector<uint8_t> marker_layer(size_t cols, size_t rows, size_t index)
{
    std::vector<uint8_t> px(cols * rows, 0);
    const size_t x0 = 16 + (index * 37) % (cols / 2), y0 = 16 + (index * 91) % (rows / 2);
    for (size_t y = y0; y < y0 + 24; ++y)
        for (size_t x = x0; x < x0 + 40; ++x) px[y * cols + x] = uint8_t(1 + (index * 7 + x + y) % 255);
    return px;
}
std::vector<uint8_t> noise_layer(size_t cols, size_t rows, uint32_t seed)
{
    std::vector<uint8_t> px(cols * rows);
    std::mt19937 rng(seed);
    for (auto &p : px) p = uint8_t(rng());
    return px;
}
void write_sl1(const std::string &path, size_t cols, size_t rows, size_t layers,
               const std::function<std::vector<uint8_t>(size_t)> &make,
               Zipper::e_compression level = Zipper::FAST_COMPRESSION)
{
    Zipper zip(path, level);
    zip.add_entry("config.ini"); zip << "layerHeight = 0.05\n";
    zip.add_entry("prusaslicer.ini"); zip << "display_width = 200\n";
    for (size_t i = 0; i < layers; ++i) {
        const auto px = make(i);
        const auto png = sla::PNGRasterEncoder{}(px.data(), cols, rows, 1);
        char name[32];
        std::snprintf(name, sizeof name, "job%05zu.png", i);
        zip.add_entry(name, png.data(), png.size());
    }
    zip.finalize();
}
sla::LayerDocument small_document(size_t layers, size_t cols, size_t rows, std::vector<std::vector<uint8_t>> &state)
{
    std::vector<sla::EncodedRaster> rasters;
    for (size_t i = 0; i < layers; ++i) {
        state.push_back(noise_layer(cols, rows, uint32_t(i + 1)));
        rasters.push_back(sla::PNGRasterEncoder{}(state.back().data(), cols, rows, 1));
    }
    return sla::LayerDocument::from_rasters(std::move(rasters));
}
void require_state(const sla::LayerDocument &doc, const std::vector<std::vector<uint8_t>> &state)
{
    REQUIRE(doc.layer_count() == state.size());
    for (size_t i = 0; i < state.size(); ++i) REQUIRE(doc.image(i).buf == state[i]);
}
size_t files_in(const boost::filesystem::path &dir)
{
    size_t n = 0;
    for (boost::filesystem::directory_iterator it(dir), end; it != end; ++it) ++n;
    return n;
}
std::vector<std::vector<uint8_t>> encoded_all(const sla::LayerDocument &doc)
{
    std::vector<std::vector<uint8_t>> result;
    for (size_t i = 0; i < doc.layer_count(); ++i) {
        const auto view = doc.encoded_layer(i);
        const auto *data = static_cast<const uint8_t *>(view.buf);
        result.emplace_back(data, data + view.sz);
    }
    return result;
}
const auto invert_all = [](png::ImageGreyscale &image) { for (auto &p : image.buf) p = uint8_t(255 - p); };
}

TEST_CASE("A document with more than 1 GiB of decoded masks opens, edits, undoes and saves",
          "[.][sla_layers_large]")
{
    constexpr size_t cols = 6144, rows = 6144, layers = 32; // 1.125 GiB decoded, a few KiB compressed per layer
    static_assert(uint64_t(cols) * rows * layers > (uint64_t(1) << 30));
    TempPath input("large-decoded-%%%%.sl1"), output("large-decoded-out-%%%%.sl1");
    write_sl1(input.str(), cols, rows, layers, [&](size_t i) { return marker_layer(cols, rows, i); });

    auto doc = sla::LayerDocument::load_archive(input.str());
    REQUIRE(doc.layer_count() == layers);
    for (size_t i = 0; i < layers; ++i) REQUIRE(doc.image(i).buf == marker_layer(cols, rows, i));

    // One undo step per operation, whatever the range.
    REQUIRE(doc.paint_stroke(17, 17, {{100, 100}, {400, 300}}, 20, 255));
    const auto edited = doc.image(17).buf;
    REQUIRE(edited != marker_layer(cols, rows, 17));
    REQUIRE(doc.edit_mask_range(5, 9, sla::LayerMaskPlane::Main, {}, invert_all));
    REQUIRE(doc.undo());
    for (size_t i = 5; i <= 9; ++i) REQUIRE(doc.image(i).buf == marker_layer(cols, rows, i));
    REQUIRE(doc.image(17).buf == edited);
    REQUIRE(doc.undo());
    REQUIRE(doc.image(17).buf == marker_layer(cols, rows, 17));
    REQUIRE_FALSE(doc.can_undo());
    REQUIRE(doc.redo());
    REQUIRE(doc.image(17).buf == edited);

    doc.save_archive(output.str());
    auto reopened = sla::LayerDocument::load_archive(output.str());
    REQUIRE(reopened.layer_count() == layers);
    for (size_t i = 0; i < layers; ++i)
        REQUIRE(reopened.image(i).buf == (i == 17 ? edited : marker_layer(cols, rows, i)));
}

TEST_CASE("The decoded-pixel budget of whole-document operations follows the configured budget",
          "[.][sla_layers_large]")
{
    std::vector<std::vector<uint8_t>> state;
    auto doc = small_document(6, 256, 256, state);
    const sla::LayerDocument::MaskBatchOperation invert = [](std::vector<sla::layerops::Mask> &masks, const sla::LayerProgress &) {
        for (auto &m : masks) for (auto &p : m.px) p = uint8_t(255 - p);
    };
    {   // 6 layers * 64 KiB = 384 KiB decoded
        sla::ScopedLayerBudgets tight({.decoded_bytes = 256 * 1024});
        REQUIRE_THROWS_WITH(doc.apply_mask_batch(sla::LayerMaskPlane::Main, invert),
                            ContainsSubstring("decoded-pixel budget") && !ContainsSubstring("1 GiB"));
        require_state(doc, state);
        REQUIRE_FALSE(doc.can_undo());
    }
    {
        sla::ScopedLayerBudgets roomy({.decoded_bytes = uint64_t(8) << 30});
        REQUIRE(doc.apply_mask_batch(sla::LayerMaskPlane::Main, invert));
        for (size_t i = 0; i < state.size(); ++i)
            for (auto &p : state[i]) p = uint8_t(255 - p);
        require_state(doc, state);
        REQUIRE(doc.undo());
        REQUIRE_FALSE(doc.can_undo());
    }
}

TEST_CASE("Undo history spills to a temporary file and restores byte-exact masks",
          "[.][sla_layers_large][sla_layers_spill]")
{
    TempPath scratch("spill-dir-%%%%");
    boost::filesystem::create_directories(scratch.path);
    const size_t spill_before = sla::layer_spill_files_alive();
    // Everything beyond 4 KiB of history goes to disk; every edit may be large.
    sla::ScopedLayerBudgets budgets({.history_edit_bytes = uint64_t(1) << 30, .history_resident_bytes = 4096,
                                     .temp_directory = scratch.str()});
    std::vector<std::vector<uint8_t>> state;
    auto doc = small_document(12, 192, 160, state);

    std::vector<std::vector<std::vector<uint8_t>>> timeline{state};
    // Range operations over different spans: each is exactly one undo step.
    const std::vector<std::pair<size_t, size_t>> ranges{{0, 11}, {3, 7}, {0, 0}, {5, 11}, {2, 9}, {11, 11}, {0, 5}};
    for (const auto &[first, last] : ranges) {
        REQUIRE(doc.edit_mask_range(first, last, sla::LayerMaskPlane::Main, {}, invert_all));
        for (size_t i = first; i <= last; ++i) for (auto &p : state[i]) p = uint8_t(255 - p);
        timeline.push_back(state);
        require_state(doc, state);
    }
    REQUIRE(doc.paint_stroke(4, 6, {{10, 10}, {150, 120}}, 6, 255));
    for (size_t i = 4; i <= 6; ++i) state[i] = doc.image(i).buf;
    timeline.push_back(state);
    REQUIRE(sla::layer_spill_files_alive() > spill_before);
    // Walk all the way back through the spilled edits (history is capped at 32 steps, we made 8).
    for (size_t step = timeline.size() - 1; step-- > 0;) {
        REQUIRE(doc.undo());
        require_state(doc, timeline[step]);
    }
    REQUIRE_FALSE(doc.can_undo());
    for (size_t step = 1; step < timeline.size(); ++step) {
        REQUIRE(doc.redo());
        require_state(doc, timeline[step]);
    }
    // New edit after undo clears redo and releases its spill files.
    REQUIRE(doc.undo());
    REQUIRE(doc.edit_mask_range(0, 2, sla::LayerMaskPlane::Main, {}, invert_all));
    REQUIRE_FALSE(doc.can_redo());

    // A copy shares spilled history; destroying either must not break the other.
    {
        sla::LayerDocument copy = doc;
        REQUIRE(copy.undo());
    }
    REQUIRE(doc.undo());
    REQUIRE(doc.redo());

    doc = sla::LayerDocument();
    REQUIRE(sla::layer_spill_files_alive() == spill_before);
}

TEST_CASE("Saving a document with spilled history writes the current masks",
          "[.][sla_layers_large][sla_layers_spill]")
{
    TempPath input("spill-save-in-%%%%.sl1"), output("spill-save-out-%%%%.sl1");
    constexpr size_t cols = 200, rows = 150, layers = 9;
    write_sl1(input.str(), cols, rows, layers, [&](size_t i) { return noise_layer(cols, rows, uint32_t(i + 3)); });
    sla::ScopedLayerBudgets budgets({.history_edit_bytes = uint64_t(1) << 30, .history_resident_bytes = 2048});
    auto doc = sla::LayerDocument::load_archive(input.str());
    std::vector<std::vector<uint8_t>> expected;
    for (size_t i = 0; i < layers; ++i) expected.push_back(noise_layer(cols, rows, uint32_t(i + 3)));
    for (size_t round = 0; round < 5; ++round) {
        const size_t first = round, last = round + 3;
        REQUIRE(doc.edit_mask_range(first, last, sla::LayerMaskPlane::Main, {}, invert_all));
        for (size_t i = first; i <= last; ++i) for (auto &p : expected[i]) p = uint8_t(255 - p);
    }
    REQUIRE(doc.undo()); REQUIRE(doc.undo()); REQUIRE(doc.redo()); // rounds 0..3 remain applied
    expected.clear();
    for (size_t i = 0; i < layers; ++i) expected.push_back(noise_layer(cols, rows, uint32_t(i + 3)));
    for (size_t round = 0; round < 4; ++round)
        for (size_t i = round; i <= round + 3; ++i) for (auto &p : expected[i]) p = uint8_t(255 - p);
    doc.save_archive(output.str());
    auto reopened = sla::LayerDocument::load_archive(output.str());
    require_state(reopened, expected);
}

TEST_CASE("A range edit that cannot be spilled is refused cleanly", "[.][sla_layers_large][sla_layers_spill]")
{
    std::vector<std::vector<uint8_t>> state;
    auto doc = small_document(8, 192, 160, state);
    {
        sla::ScopedLayerBudgets none({.history_edit_bytes = uint64_t(1) << 30, .history_resident_bytes = 4096,
                                      .temp_space_bytes = 0});
        REQUIRE_THROWS_WITH(doc.edit_mask_range(0, 7, sla::LayerMaskPlane::Main, {}, invert_all),
                            ContainsSubstring("temporary disk space") && ContainsSubstring("Select fewer layers"));
        require_state(doc, state);
        REQUIRE_FALSE(doc.can_undo());
        REQUIRE_FALSE(doc.dirty());
    }
    {
        sla::ScopedLayerBudgets tiny({.history_edit_bytes = 1024});
        REQUIRE_THROWS_WITH(doc.edit_mask_range(0, 7, sla::LayerMaskPlane::Main, {}, invert_all),
                            ContainsSubstring("history budget"));
        require_state(doc, state);
    }
    // Small edits still work without any disk: old history is dropped instead of spilled.
    sla::ScopedLayerBudgets nodisk({.history_resident_bytes = 200 * 1024, .temp_space_bytes = 0});
    for (int i = 0; i < 6; ++i) REQUIRE(doc.edit_mask_range(0, 1, sla::LayerMaskPlane::Main, {}, invert_all));
    REQUIRE(doc.can_undo());
}

TEST_CASE("Oversized files are refused by the byte budget, truncated ones as truncated",
          "[.][sla_layers_large]")
{
    TempPath file("budget-%%%%.pwc");
    const char header[16] = {'A','N','Y','C','U','B','I','C'};
    {   // Truncated: shorter than the fixed header, whatever the budget is.
        std::ofstream out(file.str(), std::ios::binary);
        out.write(header, sizeof header);
    }
    REQUIRE_THROWS_WITH(sla::LayerDocument::load_archive(file.str()),
                        ContainsSubstring("truncated") && !ContainsSubstring("memory budget"));
    {
        sla::ScopedLayerBudgets tiny({.editor_bytes = 4});
        REQUIRE_THROWS_WITH(sla::LayerDocument::load_archive(file.str()),
                            ContainsSubstring("truncated") && !ContainsSubstring("memory budget"));
    }
    // A sparse file larger than the old 1 GiB limit: refused only if it is really bigger than the budget.
    {
        std::ofstream out(file.str(), std::ios::binary);
        out.write(header, sizeof header);
    }
    boost::filesystem::resize_file(file.path, (uint64_t(1) << 30) + (uint64_t(64) << 20));
    try {
        sla::LayerDocument::load_archive(file.str());
        FAIL("an all-zero container cannot be a valid document");
    } catch (const std::exception &e) {
        // Past the budget check it is rejected as corrupt, not for its size.
        CHECK_THAT(std::string(e.what()), !ContainsSubstring("1 GiB"));
        if (sla::layer_editor_byte_budget() > (uint64_t(1) << 30) + (uint64_t(64) << 20))
            CHECK_THAT(std::string(e.what()), !ContainsSubstring("memory budget"));
    }
    {
        sla::ScopedLayerBudgets tight({.editor_bytes = uint64_t(1) << 30});
        REQUIRE_THROWS_WITH(sla::LayerDocument::load_archive(file.str()),
                            ContainsSubstring("exceeds the editor memory budget") && ContainsSubstring("1024 MiB"));
    }
}

TEST_CASE("A container above 1 GiB of compressed masks opens, edits and saves",
          "[.][sla_layers_slow]")
{
    // 70 incompressible 4096x4096 layers: a real >1 GiB archive (about 40 s to build, 2-3 GiB RAM).
    constexpr size_t cols = 4096, rows = 4096, layers = 70;
    TempPath input("huge-%%%%.sl1"), output("huge-out-%%%%.sl1");
    write_sl1(input.str(), cols, rows, layers, [&](size_t i) { return noise_layer(cols, rows, uint32_t(i + 7)); });
    REQUIRE(boost::filesystem::file_size(input.path) > (uint64_t(1) << 30));
    auto doc = sla::LayerDocument::load_archive(input.str());
    REQUIRE(doc.layer_count() == layers);
    REQUIRE(doc.paint_stroke(33, 33, {{50, 50}, {900, 700}}, 30, 0));
    const auto edited = doc.image(33).buf;
    REQUIRE(doc.undo());
    REQUIRE(doc.image(33).buf == noise_layer(cols, rows, 33 + 7));
    REQUIRE(doc.redo());
    doc.save_archive(output.str());
    auto reopened = sla::LayerDocument::load_archive(output.str());
    REQUIRE(reopened.layer_count() == layers);
    REQUIRE(reopened.image(33).buf == edited);
    REQUIRE(reopened.image(0).buf == noise_layer(cols, rows, 7));
    REQUIRE(reopened.image(layers - 1).buf == noise_layer(cols, rows, uint32_t(layers - 1 + 7)));
}

// ---- Layer payload storage: spill file / source archive references -------------------------------------------

TEST_CASE("A source-referenced document edits, saves over its own file and reopens equal",
          "[.][sla_layers_large][sla_layers_payload]")
{
    constexpr size_t cols = 192, rows = 160, layers = 24;
    TempPath file("payload-stored-%%%%.sl1");
    write_sl1(file.str(), cols, rows, layers, [&](size_t i) { return noise_layer(cols, rows, uint32_t(i + 3)); }, Zipper::NO_COMPRESSION);
    const size_t spill_before = sla::layer_spill_files_alive();
    {
        sla::ScopedLayerBudgets budgets({.payload_resident_bytes = 1});
        auto doc = sla::LayerDocument::load_archive(file.str());
        REQUIRE(doc.references_source_file());
        REQUIRE(doc.stored_payload_bytes() > layers * 1000);
        REQUIRE(doc.resident_payload_bytes() < 4096); // metadata entries only
        for (size_t i = 0; i < layers; ++i) REQUIRE(doc.image(i).buf == noise_layer(cols, rows, uint32_t(i + 3)));

        REQUIRE(doc.paint_stroke(7, 7, {{20, 20}, {100, 90}}, 6, 255));
        REQUIRE(doc.edit_mask_range(10, 12, sla::LayerMaskPlane::Main, {}, invert_all));
        std::vector<std::vector<uint8_t>> expected;
        for (size_t i = 0; i < layers; ++i) expected.push_back(doc.image(i).buf);
        REQUIRE(expected[7] != noise_layer(cols, rows, 7 + 3));

        const auto bytes_before = boost::filesystem::file_size(file.path);
        doc.save_archive(file.str()); // over the archive its layers are read from
        REQUIRE(boost::filesystem::file_size(file.path) != 0);
        (void)bytes_before;
        // The document keeps working after its source file was replaced.
        for (size_t i = 0; i < layers; ++i) REQUIRE(doc.image(i).buf == expected[i]);
        REQUIRE(doc.undo());
        REQUIRE(doc.image(10).buf == noise_layer(cols, rows, 10 + 3));
        REQUIRE(doc.redo());
        REQUIRE(doc.image(10).buf == expected[10]);

        auto reopened = sla::LayerDocument::load_archive(file.str());
        REQUIRE(reopened.layer_count() == layers);
        for (size_t i = 0; i < layers; ++i) REQUIRE(reopened.image(i).buf == expected[i]);
        // Unchanged layers stay byte-faithful (same PNG, not a re-encode).
        const auto original = encoded_all(doc);
        const auto again = encoded_all(reopened);
        REQUIRE(original == again);
    }
    REQUIRE(sla::layer_spill_files_alive() == spill_before);
}

TEST_CASE("Deflated archives spill their layer payloads to an anonymous temp file",
          "[.][sla_layers_large][sla_layers_payload]")
{
    constexpr size_t cols = 192, rows = 160, layers = 24;
    TempPath file("payload-spill-%%%%.sl1"), out("payload-spill-out-%%%%.sl1"), scratch("payload-dir-%%%%");
    boost::filesystem::create_directories(scratch.path);
    write_sl1(file.str(), cols, rows, layers, [&](size_t i) { return noise_layer(cols, rows, uint32_t(i + 11)); });
    const size_t spill_before = sla::layer_spill_files_alive();
    {
        sla::ScopedLayerBudgets budgets({.payload_resident_bytes = 20000, .temp_directory = scratch.str()});
        auto doc = sla::LayerDocument::load_archive(file.str());
        REQUIRE_FALSE(doc.references_source_file());
        REQUIRE(sla::layer_spill_files_alive() == spill_before + 1);
        REQUIRE(files_in(scratch.path) == 0); // unlinked: a crash leaves nothing behind
        REQUIRE(doc.stored_payload_bytes() > 0);
        REQUIRE(doc.resident_payload_bytes() < 3 * 20000);
        for (size_t i = 0; i < layers; ++i) REQUIRE(doc.image(i).buf == noise_layer(cols, rows, uint32_t(i + 11)));
        REQUIRE(doc.paint_stroke(2, 2, {{10, 10}, {90, 80}}, 5, 0));
        const auto edited = doc.image(2).buf;
        doc.save_archive(out.str());
        auto reopened = sla::LayerDocument::load_archive(out.str());
        for (size_t i = 0; i < layers; ++i)
            REQUIRE(reopened.image(i).buf == (i == 2 ? edited : noise_layer(cols, rows, uint32_t(i + 11))));
        REQUIRE(doc.resident_payload_bytes() < 3 * 20000);
    }
    REQUIRE(sla::layer_spill_files_alive() == spill_before);
    REQUIRE(files_in(scratch.path) == 0);
}

TEST_CASE("A source file changed on disk gives a clear error instead of garbage",
          "[.][sla_layers_large][sla_layers_payload]")
{
    constexpr size_t cols = 128, rows = 96, layers = 40;
    TempPath file("payload-changed-%%%%.sl1"), out("payload-changed-out-%%%%.sl1");
    write_sl1(file.str(), cols, rows, layers, [&](size_t i) { return noise_layer(cols, rows, uint32_t(i + 5)); }, Zipper::NO_COMPRESSION);
    sla::ScopedLayerBudgets budgets({.payload_resident_bytes = 1});
    auto doc = sla::LayerDocument::load_archive(file.str());
    REQUIRE(doc.references_source_file());
    REQUIRE(doc.image(39).buf == noise_layer(cols, rows, 39 + 5));
    {   // Another program appends to the archive (size and time change).
        std::this_thread::sleep_for(std::chrono::milliseconds(1100));
        std::ofstream out_file(file.str(), std::ios::binary | std::ios::app);
        out_file << "trailing garbage";
    }
    REQUIRE_THROWS_WITH(doc.image(3), ContainsSubstring("changed or removed")); // layer 3 is not in the small LRU
    REQUIRE_THROWS_WITH(doc.save_archive(out.str()), ContainsSubstring("changed or removed"));
    REQUIRE_FALSE(boost::filesystem::exists(out.path));
    boost::filesystem::remove(file.path);
    REQUIRE_THROWS_WITH(doc.image(4), ContainsSubstring("changed or removed"));
}

TEST_CASE("A streamed whole-document operation under tiny budgets matches the large-budget result",
          "[.][sla_layers_large][sla_layers_payload]")
{
    constexpr size_t cols = 160, rows = 128, layers = 18;
    TempPath file("payload-stream-%%%%.sl1"), scratch("payload-stream-dir-%%%%");
    boost::filesystem::create_directories(scratch.path);
    write_sl1(file.str(), cols, rows, layers, [&](size_t i) { return noise_layer(cols, rows, uint32_t(i + 21)); });
    const size_t spill_before = sla::layer_spill_files_alive();
    const sla::LayerDocument::MaskStreamOperation op = [](sla::layerops::Mask &mask, size_t layer) {
        if (layer % 5 == 4) return; // some layers stay untouched
        for (auto &p : mask.px) p = uint8_t(255 - p);
    };
    auto reference = sla::LayerDocument::load_archive(file.str());
    REQUIRE(reference.apply_mask_stream(sla::LayerMaskPlane::Main, op));
    const auto expected = encoded_all(reference);
    {
        sla::ScopedLayerBudgets tiny({.history_edit_bytes = 1024, .history_resident_bytes = 4096, .payload_resident_bytes = 1,
                                      .temp_directory = scratch.str()});
        auto doc = sla::LayerDocument::load_archive(file.str());
        const auto original = encoded_all(doc);
        REQUIRE(doc.apply_mask_stream(sla::LayerMaskPlane::Main, op));
        REQUIRE(encoded_all(doc) == expected);
        REQUIRE(doc.resident_payload_bytes() < 4096);
        REQUIRE(files_in(scratch.path) == 0);
        // One undo step, byte-exact both ways.
        REQUIRE(doc.undo());
        REQUIRE_FALSE(doc.can_undo());
        REQUIRE(encoded_all(doc) == original);
        REQUIRE(doc.redo());
        REQUIRE(encoded_all(doc) == expected);
        // Saving writes the streamed result.
        TempPath out("payload-stream-out-%%%%.sl1");
        doc.save_archive(out.str());
        REQUIRE(encoded_all(sla::LayerDocument::load_archive(out.str())) == expected);

        // Cancellation mid-stream leaves the document as it was (and no half edit in history).
        auto fresh = sla::LayerDocument::load_archive(file.str());
        const auto before = encoded_all(fresh);
        size_t seen = 0;
        REQUIRE_FALSE(fresh.apply_mask_stream(sla::LayerMaskPlane::Main, op,
                                              [&](size_t, size_t) { return ++seen < 8; }));
        REQUIRE(encoded_all(fresh) == before);
        REQUIRE_FALSE(fresh.can_undo());
        REQUIRE(fresh.apply_mask_stream(sla::LayerMaskPlane::Main, op)); // still usable afterwards
        REQUIRE(encoded_all(fresh) == expected);
    }
    REQUIRE(sla::layer_spill_files_alive() == spill_before + 0);
}

// ---- Binary containers (CTB, CTB AES, GOO, Anycubic) open without holding the file ----------------------------

namespace {
std::vector<uint8_t> slurp(const std::string &path)
{
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}
void spit(const std::string &path, const std::vector<uint8_t> &data)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char *>(data.data()), std::streamsize(data.size()));
}
uint32_t le32(const std::vector<uint8_t> &b, size_t at) { return uint32_t(b[at]) | (uint32_t(b[at + 1]) << 8) | (uint32_t(b[at + 2]) << 16) | (uint32_t(b[at + 3]) << 24); }
uint32_t be32(const std::vector<uint8_t> &b, size_t at) { return (uint32_t(b[at]) << 24) | (uint32_t(b[at + 1]) << 16) | (uint32_t(b[at + 2]) << 8) | b[at + 3]; }

// Native RLE payload of every layer, read straight from the container bytes.
std::vector<std::vector<uint8_t>> native_payloads(const std::string &format, const std::vector<uint8_t> &bytes)
{
    std::vector<std::vector<uint8_t>> result;
    auto take = [&](size_t at, size_t length) { result.emplace_back(bytes.begin() + at, bytes.begin() + at + length); };
    if (format.rfind("ctb_encrypted", 0) == 0) {
        const sla::CTBEncryptedLayerLayout layout(bytes);
        for (size_t i = 0; i < layout.count; ++i) { const auto at = layout.record(i); take(layout.integer(at + 16), layout.integer(at + 24)); }
    } else if (format.rfind("ctb", 0) == 0 || format.rfind("cbddlp", 0) == 0 || format.rfind("photon", 0) == 0) {
        const sla::ChituboxLayerLayout layout(bytes);
        for (size_t i = 0; i < layout.count; ++i) { const auto at = layout.record(i); take(layout.integer(at + 12), layout.integer(at + 16)); }
    } else if (format == "goo") {
        const size_t count = be32(bytes, 195310);
        size_t position = be32(bytes, 195470);
        for (size_t i = 0; i < count; ++i) {
            const size_t length = be32(bytes, position + 66);
            take(position + 70, length);
            position += 70 + length + 2;
        }
    } else if (format == "cxdlpv4") {
        const sla::CrealityCXDLPv4Layout layout(bytes);
        for (const auto &layer : layout.layers) take(layer.data + 44, layer.length - 44);
    } else if (format == "phz" || format == "fdg") {
        const PhrozenLayout layout(bytes, format == "fdg" ? PhrozenLayout::Kind::Fdg : PhrozenLayout::Kind::Phz);
        for (size_t i = 0; i < layout.count; ++i) { const auto range = layout.payload(i); take(range.address, range.size); }
    } else if (format == "osla") {
        // Layer table address/record size/count in the header block at 150; every record starts with the image address.
        const size_t count = le32(bytes, 150 + 71), record = le32(bytes, 150 + 75), table = le32(bytes, 150 + 79);
        for (size_t i = 0; i < count; ++i) { const size_t address = le32(bytes, table + i * record); take(address + 4, le32(bytes, address)); }
    } else if (format == "cxdlpv1" || format == "cxdlpv2" || format == "cxdlp") {
        const CrealityCXDLPv1Layout layout(bytes);
        for (size_t i = 0; i < layout.count; ++i) { const auto range = layout.record_range(i); take(range.first, range.second - range.first); }
    } else if (format == "osf") {
        const VlareLayout layout(bytes);
        for (size_t i = 0; i < layout.count; ++i) { const auto range = layout.record_range(i); take(range.first, range.second - range.first); }
    } else if (format == "n4" || format == "n7") {
        const AnetLayout layout(bytes, format);
        for (size_t i = 0; i < layout.count; ++i) { const auto range = layout.record_range(i); take(range.first, range.second - range.first); }
    } else if (format == "mdlp" || format == "gr1") {
        const MakerbaseLayout layout(bytes, format == "mdlp" ? MakerbaseLayout::Kind::Mdlp : MakerbaseLayout::Kind::Gr1);
        for (size_t i = 0; i < layout.count; ++i) { const auto range = layout.record_range(i); take(range.first, range.second - range.first); }
    } else if (format == "qdt") {
        const Emake3DLayout layout(bytes);
        for (size_t i = 0; i < layout.count; ++i) { const auto range = layout.record_range(i); take(range.first, range.second - range.first); }
    } else if (format.rfind("lgs", 0) == 0) {
        const size_t count = le32(bytes, 164);
        size_t at = 180 + size_t(le32(bytes, 172)) * le32(bytes, 176) * 2;
        if (le32(bytes, 16) == 120) at += 4 + le32(bytes, at) + 2;
        for (size_t i = 0; i < count; ++i) { const size_t length = le32(bytes, at); take(at + 4, length); at += 4 + length; }
    } else {
        const AnycubicLayerLayout layout(bytes, format);
        for (size_t i = 0; i < layout.count; ++i) { const auto at = layout.record(i); take(layout.integer(at), layout.integer(at + 4)); }
    }
    return result;
}

struct BinaryFamily { std::string format, extension; };
std::vector<BinaryFamily> binary_families()
{
    std::vector<BinaryFamily> result;
    for (const char *format : {"ctb_v3", "ctb_v5", "ctb_gktwo", "cbddlp_v1", "ctb_encrypted", "ctb_encrypted_v4", "goo", "pws", "pwmx", "pwc", "cxdlpv4",
                                "phz", "fdg", "osla", "lgs", "lgs30", "lgs120", "lgs4k",
                                "cxdlpv1", "cxdlpv2", "cxdlp", "osf", "n4", "n7", "mdlp", "gr1", "qdt"})
        result.push_back({format, get_archive_entry(format)->ext});
    return result;
}

void export_binary(const BinaryFamily &family, size_t cols, size_t rows, size_t layers, const std::string &path)
{
    // The LGS exporter wants a raster at most as wide as it is tall.
    if (family.format.rfind("lgs", 0) == 0 && cols > rows) std::swap(cols, rows);
    SLAFullPrintConfig base;
    Test::configure_archive_fixture(base, family.format);
    DynamicPrintConfig cfg;
    cfg.apply(base);
    // Anet and QDT only accept their machine's own screen; the document takes that raster.
    const bool machine_screen = family.format == "n4" || family.format == "n7" || family.format == "qdt";
    if (machine_screen) {
        cols = size_t(cfg.opt_int("display_pixels_x")); rows = size_t(cfg.opt_int("display_pixels_y"));
    } else {
        cfg.set("display_pixels_x", int(cols)); cfg.set("display_pixels_y", int(rows));
        if (!anycubic_legacy_geometry(family.format)) { // legacy Anycubic keeps its fixed machine screen
            cfg.set("display_width", double(cols) / 20.); cfg.set("display_height", double(rows) / 20.);
        }
        cfg.option("display_orientation")->setInt(sladoLandscape);
    }
    if (family.format == "pwc") cfg.set("sla_anycubic_version", std::string("518"));
    std::vector<sla::EncodedRaster> pngs;
    std::vector<double> heights;
    for (size_t i = 0; i < layers; ++i) {
        // Black/white runs with a noisy band: realistic RLE, large enough to matter.
        auto px = marker_layer(cols, rows, i);
        std::mt19937 rng(uint32_t(i + 11));
        for (size_t k = 0; k < cols * rows / 40; ++k) px[(size_t(rng()) % rows) * cols + size_t(rng()) % cols] = 255;
        for (auto &p : px) p = p < 128 ? 0 : 255;
        pngs.push_back(sla::PNGRasterEncoder{}(px.data(), cols, rows, 1));
        heights.push_back(.05 * double(i + 1));
    }
    auto job = SLAPrint::create_raster_job(cfg, sla::LayerDocument::from_rasters(std::move(pngs)), heights);
    job->export_print(path);
}
}

TEST_CASE("Binary containers open without holding the file, edit, save new and over the same file",
          "[.][sla_layers_large][sla_layers_native]")
{
    constexpr size_t cols = 1200, rows = 700, layers = 12;
    const auto family = GENERATE(from_range(binary_families()));
    CAPTURE(family.format);
    TempPath dir("native-stream-%%%%");
    boost::filesystem::create_directory(dir.path);
    const std::string source = (dir.path / ("source." + family.extension)).string();
    const std::string copy = (dir.path / ("work." + family.extension)).string();
    const std::string saved = (dir.path / ("saved." + family.extension)).string();
    export_binary(family, cols, rows, layers, source);
    const auto original = slurp(source);
    const auto source_payloads = native_payloads(family.format, original);
    REQUIRE(source_payloads.size() == layers);
    const size_t spill_before = sla::layer_spill_files_alive();
    {
        auto doc = sla::LayerDocument::load_archive(source);
        REQUIRE(doc.layer_count() == layers);
        // The file is referenced, not held: only the small per-layer parameter rows are resident.
        REQUIRE(doc.references_source_file());
        REQUIRE_FALSE(doc.native_image_resident());
        REQUIRE(doc.native_image_bytes() == original.size());
        REQUIRE(doc.resident_payload_bytes() * 8 < original.size());
        std::vector<std::vector<uint8_t>> pixels;
        for (size_t i = 0; i < layers; ++i) pixels.push_back(doc.image(i).buf);

        // Writing back an identical layer is not an edit (the lazily produced PNG is compared by content).
        doc.replace_image(1, doc.image(1));
        REQUIRE_FALSE(doc.dirty());
        REQUIRE_FALSE(doc.can_undo());

        // Untouched save streams the same bytes.
        doc.save_archive(saved);
        REQUIRE(slurp(saved) == original);

        // One edited layer: the others keep their source RLE byte for byte.
        const size_t edit = 5;
        REQUIRE(doc.paint_stroke(edit, edit, {{100, 100}, {300, 250}}, 25, 255));
        const auto edited = doc.image(edit).buf;
        REQUIRE(edited != pixels[edit]);
        doc.save_archive(saved);
        const auto written = slurp(saved);
        const auto written_payloads = native_payloads(family.format, written);
        REQUIRE(written_payloads.size() == layers);
        for (size_t i = 0; i < layers; ++i) {
            if (i == edit) REQUIRE(written_payloads[i] != source_payloads[i]);
            else REQUIRE(written_payloads[i] == source_payloads[i]);
        }
        auto reopened = sla::LayerDocument::load_archive(saved);
        for (size_t i = 0; i < layers; ++i) REQUIRE(reopened.image(i).buf == (i == edit ? edited : pixels[i]));

        // Undo returns to the untouched bytes (the layer is the source decode again).
        REQUIRE(doc.undo());
        doc.save_archive(saved);
        REQUIRE(slurp(saved) == original);
        REQUIRE(doc.redo());
    }
    {
        // Over the very file the layers are read from; the open document keeps working.
        spit(copy, original);
        auto doc = sla::LayerDocument::load_archive(copy);
        std::vector<std::vector<uint8_t>> pixels;
        for (size_t i = 0; i < layers; ++i) pixels.push_back(doc.image(i).buf);
        REQUIRE(doc.paint_stroke(2, 2, {{50, 60}, {400, 300}}, 20, 0));
        REQUIRE(doc.paint_stroke(9, 9, {{70, 80}, {420, 310}}, 12, 255));
        const auto edited2 = doc.image(2).buf, edited9 = doc.image(9).buf;
        doc.save_archive(copy);
        REQUIRE(slurp(copy) != original);
        for (size_t i = 0; i < layers; ++i) REQUIRE(doc.image(i).buf == (i == 2 ? edited2 : i == 9 ? edited9 : pixels[i]));
        auto reopened = sla::LayerDocument::load_archive(copy);
        for (size_t i = 0; i < layers; ++i) REQUIRE(reopened.image(i).buf == (i == 2 ? edited2 : i == 9 ? edited9 : pixels[i]));
        // And again over the same file, now from the reopened document.
        REQUIRE(reopened.paint_stroke(0, 0, {{10, 10}, {200, 90}}, 8, 255));
        const auto edited0 = reopened.image(0).buf;
        reopened.save_archive(copy);
        auto third = sla::LayerDocument::load_archive(copy);
        REQUIRE(third.image(0).buf == edited0);
        REQUIRE(third.image(2).buf == edited2);
    }
    REQUIRE(sla::layer_spill_files_alive() == spill_before);
    // Only the three files this test wrote: no temporary siblings left behind.
    REQUIRE(files_in(dir.path) == 3);
}

TEST_CASE("A binary container changed by another program gives a clear error",
          "[.][sla_layers_large][sla_layers_native]")
{
    constexpr size_t cols = 400, rows = 250, layers = 6;
    const auto family = GENERATE(from_range(binary_families()));
    CAPTURE(family.format);
    TempPath dir("native-changed-%%%%");
    boost::filesystem::create_directory(dir.path);
    const std::string file = (dir.path / ("a." + family.extension)).string(), out = (dir.path / ("b." + family.extension)).string();
    export_binary(family, cols, rows, layers, file);
    auto doc = sla::LayerDocument::load_archive(file);
    REQUIRE(doc.references_source_file());
    REQUIRE_FALSE(doc.image(1).buf.empty());
    {   // Another program appends to the container (size changes at once, no need to wait for the mtime).
        std::ofstream out_file(file, std::ios::binary | std::ios::app);
        out_file << "trailing";
    }
    REQUIRE_THROWS_WITH(doc.image(4), ContainsSubstring("changed or removed"));
    REQUIRE_THROWS_WITH(doc.save_archive(out), ContainsSubstring("changed or removed"));
    REQUIRE_FALSE(boost::filesystem::exists(out));
    REQUIRE(files_in(dir.path) == 1);
}

TEST_CASE("Corrupt layer data is rejected when a binary container is opened",
          "[.][sla_layers_large][sla_layers_native]")
{
    constexpr size_t cols = 300, rows = 200, layers = 4;
    const auto family = GENERATE(from_range(binary_families()));
    CAPTURE(family.format);
    TempPath dir("native-corrupt-%%%%");
    boost::filesystem::create_directory(dir.path);
    const std::string file = (dir.path / ("a." + family.extension)).string();
    export_binary(family, cols, rows, layers, file);
    auto bytes = slurp(file);
    REQUIRE(native_payloads(family.format, bytes).size() == layers);
    // Truncate the file in the middle of the layer data: the table points outside the file.
    bytes.resize(bytes.size() * 2 / 3);
    spit(file, bytes);
    REQUIRE_THROWS(sla::LayerDocument::load_archive(file));
    REQUIRE(files_in(dir.path) == 1);
}

TEST_CASE("ZIP container families keep their layer images out of RAM under a small payload budget",
          "[.][sla_layers_large][sla_layers_native][sla_layers_zip]")
{
    constexpr size_t cols = 900, rows = 500, layers = 10;
    const std::string format = GENERATE(as<std::string>{}, "pwsz", "klipper", "nanodlp_v1", "vdt", "zcode");
    CAPTURE(format);
    const BinaryFamily family{format, get_archive_entry(format.c_str())->ext};
    TempPath dir("zip-spill-%%%%");
    boost::filesystem::create_directory(dir.path);
    const std::string source = (dir.path / ("source." + family.extension)).string();
    const std::string saved = (dir.path / ("saved." + family.extension)).string();
    export_binary(family, cols, rows, layers, source);
    const auto original = slurp(source);
    std::vector<std::vector<uint8_t>> reference;
    {
        auto doc = sla::LayerDocument::load_archive(source);
        REQUIRE(doc.layer_count() == layers);
        for (size_t i = 0; i < layers; ++i) reference.push_back(doc.image(i).buf);
    }
    const size_t spill_before = sla::layer_spill_files_alive();
    {
        sla::LayerBudgetOverride limit; limit.payload_resident_bytes = 4096;
        sla::ScopedLayerBudgets scoped(limit);
        auto doc = sla::LayerDocument::load_archive(source);
        REQUIRE(doc.layer_count() == layers);
        // The layer PNGs sit in the spill file and the archive itself stays in its source file.
        REQUIRE(doc.stored_payload_bytes() > 0);
        REQUIRE(doc.references_source_file());
        REQUIRE_FALSE(doc.native_image_resident());
        REQUIRE(doc.native_image_bytes() == original.size());
        REQUIRE(sla::layer_spill_files_alive() > spill_before);
        for (size_t i = 0; i < layers; ++i) REQUIRE(doc.image(i).buf == reference[i]);
        doc.save_archive(saved);
        REQUIRE(slurp(saved) == original); // unchanged save: the same bytes
        REQUIRE(doc.paint_stroke(4, 4, {{100, 100}, {300, 250}}, 25, 255));
        const auto edited = doc.image(4).buf;
        REQUIRE(edited != reference[4]);
        doc.save_archive(saved);
        auto reopened = sla::LayerDocument::load_archive(saved);
        for (size_t i = 0; i < layers; ++i) REQUIRE(reopened.image(i).buf == (i == 4 ? edited : reference[i]));
        REQUIRE(doc.undo());
        for (size_t i = 0; i < layers; ++i) REQUIRE(doc.image(i).buf == reference[i]);
        doc.save_archive(saved); // save over a copy of the source works with spilled layers too
        REQUIRE(slurp(saved) == original);
        // Saving an edit over the very file the document reads from must not corrupt the references.
        REQUIRE(doc.paint_stroke(6, 6, {{50, 60}, {350, 220}}, 20, 255));
        const auto edited6 = doc.image(6).buf;
        doc.save_archive(source);
        for (size_t i = 0; i < layers; ++i) REQUIRE(doc.image(i).buf == (i == 6 ? edited6 : reference[i]));
        auto over = sla::LayerDocument::load_archive(source);
        for (size_t i = 0; i < layers; ++i) REQUIRE(over.image(i).buf == (i == 6 ? edited6 : reference[i]));
    }
    REQUIRE(sla::layer_spill_files_alive() == spill_before);
    REQUIRE(files_in(dir.path) == 2);
}

TEST_CASE("Generic ZIP families (CWS variants, UVJ, VDA) keep their layer images out of RAM under a small payload budget",
          "[.][sla_layers_large][sla_layers_zip_generic]")
{
    constexpr size_t cols = 900, rows = 500, layers = 10;
    const std::string format = GENERATE(as<std::string>{}, "cws", "cwsxml", "cwsnovamaker", "cwsrgb", "uvj", "vda", "genericzip", "chitubox_zip");
    CAPTURE(format);
    const BinaryFamily family{format, get_archive_entry(format.c_str())->ext};
    TempPath dir("zip-generic-spill-%%%%");
    boost::filesystem::create_directory(dir.path);
    const std::string source = (dir.path / ("source." + family.extension)).string();
    const std::string saved = (dir.path / ("saved." + family.extension)).string();
    export_binary(family, cols, rows, layers, source);
    std::vector<std::vector<uint8_t>> reference;
    {
        auto doc = sla::LayerDocument::load_archive(source);
        REQUIRE(doc.layer_count() == layers);
        for (size_t i = 0; i < layers; ++i) reference.push_back(doc.image(i).buf);
    }
    const size_t spill_before = sla::layer_spill_files_alive();
    {
        sla::LayerBudgetOverride limit; limit.payload_resident_bytes = 4096;
        sla::ScopedLayerBudgets scoped(limit);
        auto doc = sla::LayerDocument::load_archive(source);
        REQUIRE(doc.layer_count() == layers);
        // Layer images are stored (spill file, or referenced in the STORED source); no resident layer bytes.
        REQUIRE(doc.stored_payload_bytes() > 0);
        REQUIRE(doc.resident_payload_bytes() * 8 < boost::filesystem::file_size(source));
        for (size_t i = 0; i < layers; ++i) REQUIRE(doc.image(i).buf == reference[i]);
        doc.save_archive(saved);
        {
            auto same = sla::LayerDocument::load_archive(saved);
            for (size_t i = 0; i < layers; ++i) REQUIRE(same.image(i).buf == reference[i]);
        }
        REQUIRE(doc.paint_stroke(4, 4, {{100, 100}, {300, 250}}, 25, 255));
        const auto edited = doc.image(4).buf;
        REQUIRE(edited != reference[4]);
        doc.save_archive(saved);
        auto reopened = sla::LayerDocument::load_archive(saved);
        for (size_t i = 0; i < layers; ++i) REQUIRE(reopened.image(i).buf == (i == 4 ? edited : reference[i]));
        REQUIRE(doc.undo());
        for (size_t i = 0; i < layers; ++i) REQUIRE(doc.image(i).buf == reference[i]);
    }
    REQUIRE(sla::layer_spill_files_alive() == spill_before);
    REQUIRE(files_in(dir.path) == 2);
}
