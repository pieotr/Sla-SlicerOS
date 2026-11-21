// SPDX-License-Identifier: AGPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/core.hpp>
#include <boost/log/expressions.hpp>
#include <boost/log/sinks/sync_frontend.hpp>
#include <boost/log/sinks/text_ostream_backend.hpp>
#include <boost/make_shared.hpp>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <thread>
#include "libslic3r/Utils.hpp"
#include "libslic3r/SLAPrint.hpp"
#include "libslic3r/Format/CrealityCXDLPv4.hpp"
#include "libslic3r/Format/CrealityCXDLPv4Layout.hpp"
#include "libslic3r/SLA/EncodedMaskReuse.hpp"
#include "libslic3r/SLA/ExportRequest.hpp"
#include "libslic3r/SLA/ProjectArchive.hpp"
#include "libslic3r/SLA/SlicedResultSession.hpp"
#include "sla_archive_test_config.hpp"

using namespace Slic3r;
namespace {
namespace reuse = sla::encoded_mask_reuse_detail;
struct ReuseSwitch {
    std::string name;
    std::optional<std::string> saved;
    explicit ReuseSwitch(const char *value, const char *variable = "SLA_EXPERIMENT_ENCODED_MASK_REUSE") : name(variable) {
        if (const char *old = std::getenv(name.c_str())) saved = old;
        set(value);
    }
    void set(const char *value) const {
#ifdef _WIN32
        _putenv_s(name.c_str(), value ? value : "");
#else
        if (value) setenv(name.c_str(), value, 1);
        else unsetenv(name.c_str());
#endif
    }
    ~ReuseSwitch() { set(saved ? saved->c_str() : nullptr); }
};
struct ReuseDirectory {
    boost::filesystem::path path = boost::filesystem::unique_path("encoded-mask-reuse-%%%%-%%%%");
    ReuseDirectory() { boost::filesystem::create_directory(path); }
    ~ReuseDirectory() { boost::system::error_code ec; boost::filesystem::remove_all(path, ec); }
};
// Observe the existing production trace, not a counter that could claim a
// cache HIT while the writer still calls encode_layers(). Removing adoption
// must fail these tests even when all snapshot bytes/output remain equal.
struct WriterTrace {
    using Sink = boost::log::sinks::synchronous_sink<boost::log::sinks::text_ostream_backend>;
    unsigned saved_level = get_logging_level();
    std::optional<std::string> saved_environment;
    boost::shared_ptr<std::ostringstream> text = boost::make_shared<std::ostringstream>();
    boost::shared_ptr<Sink> sink = boost::make_shared<Sink>();
    static void environment(const char *value) {
#ifdef _WIN32
        _putenv_s("SLA_TRACE_EXPORT_STAGES", value ? value : "");
#else
        if (value) setenv("SLA_TRACE_EXPORT_STAGES", value, 1);
        else unsetenv("SLA_TRACE_EXPORT_STAGES");
#endif
    }
    WriterTrace() {
        if (const char *old = std::getenv("SLA_TRACE_EXPORT_STAGES")) saved_environment = old;
        sink->locked_backend()->add_stream(text);
        sink->set_formatter(boost::log::expressions::stream << boost::log::expressions::smessage);
        boost::log::core::get()->add_sink(sink);
        environment("1"); set_logging_level(3);
    }
    ~WriterTrace() {
        boost::log::core::get()->remove_sink(sink);
        set_logging_level(saved_level);
        environment(saved_environment ? saved_environment->c_str() : nullptr);
    }
    void expect(size_t reused, size_t encoded, std::optional<size_t> copies = std::nullopt,
                size_t borrowed_png = 0, size_t compared_layers = 0) {
        sink->flush();
        const auto captured = text->str(); INFO(captured);
        std::istringstream lines(captured); std::string line;
        size_t actual_reused = 0, actual_encoded = 0, actual_copies = 0;
        size_t selected_png = 0, consumed_png = 0, compared_png = 0;
        while (std::getline(lines, line)) {
            actual_reused += line.rfind("Layer export masks reused:", 0) == 0;
            actual_encoded += line.rfind("Layer export masks encoded:", 0) == 0;
            actual_copies += line.rfind("CXDLPv4 encoded batch copied:", 0) == 0;
            selected_png += line.rfind("Layer export PNG encoding bypass selected:", 0) == 0;
            consumed_png += line.rfind("Layer export PNG document consumed:", 0) == 0;
            compared_png += line.rfind("SL1 encoded readback chunk:", 0) == 0;
        }
        CHECK(actual_reused == reused); CHECK(actual_encoded == encoded);
        if (copies) CHECK(actual_copies == *copies);
        CHECK(selected_png == borrowed_png); CHECK(consumed_png == borrowed_png);
        CHECK(compared_png == compared_layers); // Each small fixture PNG fits one chunk.
        text->str(std::string{}); text->clear();
    }
};
std::vector<uint8_t> read_bytes(const boost::filesystem::path &path) {
    std::ifstream input(path.string(), std::ios::binary);
    REQUIRE(input.good());
    return {std::istreambuf_iterator<char>(input), {}};
}
std::vector<uint8_t> raster_bytes(const sla::EncodedRaster &raster) {
    const auto *data = static_cast<const uint8_t *>(raster.data());
    return {data, data + raster.size()};
}
std::vector<uint8_t> normalized_cxdlpv4(const boost::filesystem::path &path) {
    auto bytes = read_bytes(path);
    // First validate the untouched original CRC and every native range. Only
    // UTC creation-minute and its already-validated final CRC are normalized.
    const sla::CrealityCXDLPv4Layout layout(bytes);
    const size_t slicer = layout.integer(layout.base + 72);
    REQUIRE(slicer != 0);
    layout.require(slicer + 32, 4);
    std::fill_n(bytes.begin() + slicer + 32, 4, uint8_t(0));
    std::fill_n(bytes.end() - 4, 4, uint8_t(0));
    return bytes;
}
DynamicPrintConfig reuse_configuration() {
    SLAFullPrintConfig full; Test::configure_archive_fixture(full, "cxdlpv4");
    DynamicPrintConfig config; config.apply(full);
    config.set("supports_enable", false); config.set("pad_enable", false);
    config.set("hollowing_enable", false); config.set("support_object_elevation", 0.);
    config.set("sla_bottom_layers", 1); config.set("sla_transition_layers", 0);
    return config;
}
Model reuse_model() {
    Model model;
    auto *object = model.add_object("owned encoding reuse cube", "", make_cube(16., 12., .2));
    // Non-axis-aligned edges cross the pixel grid and exercise actual AGG
    // antialiasing; the public test below requires mid-gray pixels to exist.
    object->rotate(.321, Z);
    object->add_instance()->set_offset(Vec3d(30., 20., 0.));
    return model;
}
std::shared_ptr<const sla::ExportSnapshot> capture(const SLAPrint &print) {
    return std::make_shared<const sla::ExportSnapshot>(print.capture_export_snapshot());
}
void require_pixels(const sla::LayerDocument &expected, const std::string &path) {
    const auto actual = sla::LayerDocument::load_archive(path);
    REQUIRE(actual.layer_count() == expected.layer_count());
    for (size_t i = 0; i < actual.layer_count(); ++i)
        CHECK(actual.image(i).buf == expected.image(i).buf);
}
bool no_staging(const boost::filesystem::path &folder) {
    for (const auto &entry : boost::filesystem::directory_iterator(folder))
        if (entry.path().filename().string().find(".sla-export-") == 0) return false;
    return true;
}
} // namespace

TEST_CASE("Native encoding copy budget is exact and cancellation never changes its source", "[sla_encoded_mask_copy]") {
    std::vector<sla::EncodedRaster> source;
    source.emplace_back(std::vector<uint8_t>{0, 1, 2}, "cxdlpv4rle");
    source.emplace_back(std::vector<uint8_t>{3, 4}, "cxdlpv4rle");
    const auto copy = reuse::copy_cxdlpv4_batch(source, 2, {}, {5, 2});
    REQUIRE(copy); CHECK(copy->payload_bytes == 5); REQUIRE(copy->layers.size() == 2);
    CHECK(raster_bytes(copy->layers[0]) == std::vector<uint8_t>{0, 1, 2});
    CHECK(raster_bytes(copy->layers[1]) == std::vector<uint8_t>{3, 4});
    CHECK(copy->layers[0].data() != source[0].data());
    CHECK_FALSE(reuse::copy_cxdlpv4_batch(source, 2, {}, {4, 2}));
    CHECK_FALSE(reuse::copy_cxdlpv4_batch(source, 2, {}, {6, 1}));
    CHECK_FALSE(reuse::copy_cxdlpv4_batch(source, 2, {}, {0, 2}));
    CHECK_FALSE(reuse::copy_cxdlpv4_batch(source, 1));
    CHECK_FALSE(reuse::copy_cxdlpv4_batch({}, 0));
    source.emplace_back(std::vector<uint8_t>{5}, "png");
    CHECK_FALSE(reuse::copy_cxdlpv4_batch(source, 3));
    source.back() = sla::EncodedRaster{};
    CHECK_FALSE(reuse::copy_cxdlpv4_batch(source, 3));

    std::vector<uint8_t> literal(reuse::copy_chunk_bytes + 17);
    for (size_t i = 0; i < literal.size(); ++i) literal[i] = uint8_t(i % 251);
    std::vector<sla::EncodedRaster> large;
    large.emplace_back(std::vector<uint8_t>(literal), "cxdlpv4rle");
    size_t calls = 0;
    const auto coordinator = std::this_thread::get_id();
    const auto whole = reuse::copy_cxdlpv4_batch(large, 1, [&] {
        CHECK(std::this_thread::get_id() == coordinator); ++calls;
    });
    REQUIRE(whole); CHECK(raster_bytes(whole->layers.front()) == literal);
    // Check every real checkpoint, including the second chunk and final
    // publication check; this makes no assumption about an exact call count.
    REQUIRE(calls >= 2);
    for (size_t fail_at = 1; fail_at <= calls; ++fail_at) {
        size_t observed = 0;
        CHECK_THROWS_AS(reuse::copy_cxdlpv4_batch(large, 1, [&] {
            if (++observed == fail_at) throw CanceledException{};
        }), CanceledException);
        CHECK(observed == fail_at);
        CHECK(raster_bytes(large.front()) == literal);
    }
}

TEST_CASE("CXDLPv4 raw and canonical Gray7 encodings match every value and RLE length boundary", "[sla_encoded_mask_codec]") {
    std::vector<uint8_t> values(256), canonical(256);
    for (size_t i = 0; i < 256; ++i) {
        values[i] = uint8_t(i);
        // Independent literal native alphabet: raw 0/1 become 0; other pairs
        // become the odd representative. Do not derive this expected value
        // from the encoder or the projection function being checked.
        canonical[i] = i < 2 ? 0 : uint8_t(2 * (i / 2) + 1);
        CHECK(sla::project_mask_value(values[i], sla::LayerMaskPrecision::NativeGray7) == canonical[i]);
    }
    CHECK(raster_bytes(CrealityCXDLPv4Archive::encode_mask(values.data(), 256, 1, 1)) ==
          raster_bytes(CrealityCXDLPv4Archive::encode_mask(canonical.data(), 256, 1, 1)));
    for (const size_t run : {size_t(1), size_t(127), size_t(128), size_t(16383), size_t(16384), size_t(2097151), size_t(2097152)}) {
        for (const uint8_t value : {uint8_t(0), uint8_t(1), uint8_t(2), uint8_t(255)}) {
            CAPTURE(run, value);
            const size_t width = std::min<size_t>(4096, run + 1), height = (run + 1 + width - 1) / width;
            std::vector<uint8_t> raw(width * height, value < 128 ? 255 : 0);
            std::fill_n(raw.begin(), run, value);
            auto projected = raw;
            for (auto &byte : projected) byte = byte < 2 ? 0 : uint8_t(2 * (byte / 2) + 1);
            const auto native = CrealityCXDLPv4Archive::encode_mask(raw.data(), width, height, 1);
            CHECK(raster_bytes(native) == raster_bytes(CrealityCXDLPv4Archive::encode_mask(projected.data(), width, height, 1)));
            CHECK(CrealityCXDLPv4Archive::decode_mask(static_cast<const uint8_t *>(native.data()), native.size(), width, height) == projected);
        }
    }
}

TEST_CASE("Actual model process can supply an immutable CXDLPv4 snapshot with identical export", "[sla_encoded_mask_reuse]") {
    // Both entry policies must reach the real writer HIT, including AA and
    // orientation/gamma projection. The unset mode exposes a disabled default.
    const bool use_default = GENERATE(false, true);
    ReuseSwitch enabled(use_default ? nullptr : "1"); ReuseDirectory directory; WriterTrace trace;
    const int variant = GENERATE(0, 1, 2);
    CAPTURE(use_default, variant);
    auto config = reuse_configuration();
    config.option("display_orientation")->setInt(variant == 1 ? sladoPortrait : sladoLandscape);
    config.set("display_mirror_x", variant != 0); config.set("display_mirror_y", variant == 1);
    config.set("gamma_correction", variant == 2 ? 1.8 : 1.);
    auto model = reuse_model(); SLAPrint source;
    bool saw_statistics = false;
    source.set_status_callback([&](const PrintBase::SlicingStatus &status) {
        if (status.text == "Generating statistics") {
            saw_statistics = true;
            // This same-thread callback is before final statistics. A pending
            // raw receipt must not yet authorize encoding reuse.
            if (source.finished()) CHECK(capture(source)->reusable_cxdlpv4_layer_count() == 0);
        }
    });
    source.apply(model, config); source.process();
    REQUIRE(source.finished()); REQUIRE(source.layer_document());
    REQUIRE(saw_statistics);
    const auto rendered = source.layer_document()->image(0);
    REQUIRE(std::any_of(rendered.buf.begin(), rendered.buf.end(), [](uint8_t value) {
        return value > 0 && value < 255;
    }));
    const auto hit = capture(source);
    trace.expect(0, 0, 1); // Real public capture performs one bounded payload copy.
    const auto editor = sla::SlicedResultSession::capture(source);
    trace.expect(0, 0, 0); // Editor capture must not allocate/copy unused native RLE.
    CHECK(editor->configuration() == source.full_print_config());
    CHECK(editor->source_current()); CHECK_FALSE(editor->document().dirty());
    CHECK(editor->document().can_undo() == source.layer_document()->can_undo());
    CHECK(editor->document().can_redo() == source.layer_document()->can_redo());
    REQUIRE(editor->document().layer_count() == source.layer_document()->layer_count());
    for (size_t i = 0; i < editor->document().layer_count(); ++i)
        CHECK(editor->document().image(i).buf == source.layer_document()->image(i).buf);
    REQUIRE(hit->reusable_cxdlpv4_layer_count() == source.layer_document()->layer_count());
    REQUIRE(hit->reusable_cxdlpv4_payload_bytes() > 0);
    CHECK(hit->reusable_cxdlpv4_payload_bytes() <= (size_t(64) << 20));
    const sla::ExportRequest fast(hit, "cxdlpv4", (directory.path / "fast.cxdlpv4").string());
    const auto coordinator = std::this_thread::get_id();
    fast.write([&] { CHECK(std::this_thread::get_id() == coordinator); });
    trace.expect(1, 0, 0); // HIT writer shares the immutable batch; no second copy.
    require_pixels(*source.layer_document(), fast.path());
    {
        ReuseSwitch disabled("0");
        const auto miss = capture(source);
        CHECK(miss->reusable_cxdlpv4_layer_count() == 0);
        CHECK(miss->reusable_cxdlpv4_payload_bytes() == 0);
        sla::ExportRequest(miss, "cxdlpv4", (directory.path / "control.cxdlpv4").string()).write();
        trace.expect(0, 1);
        // A captured batch cannot force reuse when the experiment is disabled
        // between capture and write. Exact output must remain the same.
        sla::ExportRequest(hit, "cxdlpv4", (directory.path / "forced-miss.cxdlpv4").string()).write();
        trace.expect(0, 1);
    }
    CHECK(normalized_cxdlpv4(fast.path()) == normalized_cxdlpv4(directory.path / "control.cxdlpv4"));
    CHECK(normalized_cxdlpv4(fast.path()) == normalized_cxdlpv4(directory.path / "forced-miss.cxdlpv4"));
    const auto alternate = (directory.path / "alternate.sl1").string();
    ReuseSwitch sl1_borrow(nullptr, "SLA_EXPERIMENT_SL1_BORROW_PNG");
    ReuseSwitch sl1_compare(nullptr, "SLA_EXPERIMENT_SL1_COMPARE_ENCODED");
    sla::ExportRequest(hit, "SL1", alternate).write();
    // A CXDLPv4 receipt cannot cross formats; the private SL1 document has
    // its own default PNG path, whose actual consumption must be observed.
    trace.expect(0, 0, 0, 1, source.layer_document()->layer_count());
    require_pixels(*source.layer_document(), alternate);
    CHECK(hit->generation().current()); CHECK(no_staging(directory.path));
}

TEST_CASE("Edited replaced restored and raster-only documents cannot inherit a model encoding receipt", "[sla_encoded_mask_reuse]") {
    ReuseSwitch enabled("1"); ReuseDirectory directory; WriterTrace trace;
    const std::string action = GENERATE("replace-same-revision", "paint", "undo", "redo", "apply-guarded", "raster-only", "restore-project");
    auto config = reuse_configuration(); auto model = reuse_model();
    auto source = std::make_unique<SLAPrint>(); source->apply(model, config); source->process();
    const auto original = capture(*source); REQUIRE(original->reusable_cxdlpv4_layer_count() > 0);
    auto changed = *source->layer_document();
    if (action == "replace-same-revision") {
        const auto geometry = changed.fresh_job()->geometry;
        std::vector<sla::EncodedRaster> masks;
        for (size_t i = 0; i < changed.layer_count(); ++i) {
            auto image = changed.image(i);
            if (i == 0) image.buf.front() = image.buf.front() == 127 ? 255 : 127;
            masks.push_back(sla::PNGRasterEncoder{}(image.buf.data(), geometry.columns, geometry.rows, 1));
        }
        changed = sla::LayerDocument::from_fresh_rasters(std::move(masks), *changed.fresh_job());
        REQUIRE(changed.revision() == 0);
        REQUIRE(changed.image(0).buf != source->layer_document()->image(0).buf);
    }
    if (action == "paint" || action == "undo" || action == "redo" || action == "apply-guarded") {
        changed.paint(0, 0, 0, 1, 127);
        if (action == "undo" || action == "redo") REQUIRE(changed.undo());
        if (action == "redo") REQUIRE(changed.redo());
    }
    if (action == "raster-only") {
        std::vector<double> positions;
        for (const auto &layer : source->print_layers()) positions.push_back(double(layer.level()) * SCALING_FACTOR);
        source = SLAPrint::create_raster_job(config, changed, positions);
    } else if (action == "restore-project") {
        const auto path = (directory.path / "layers.sla-project").string();
        sla::save_layer_project(path, sla::capture_project_layers(model, config, *source));
        const auto saved = sla::read_project_layers(path); REQUIRE(saved);
        source->queue_project_layers(std::make_shared<sla::ProjectRasterState>(*saved));
        source->process(); REQUIRE_FALSE(source->project_layer_restore_failed());
    } else if (action == "apply-guarded") {
        // The editor Apply route checks this exact captured source guard.
        source->replace_layer_document(changed, original->generation());
    } else source->replace_layer_document(changed);
    const auto miss = capture(*source);
    CHECK(miss->reusable_cxdlpv4_layer_count() == 0); CHECK(miss->reusable_cxdlpv4_payload_bytes() == 0);
    CHECK_FALSE(original->generation().current());
    const auto path = (directory.path / "current.cxdlpv4").string();
    sla::ExportRequest(miss, "cxdlpv4", path).write();
    trace.expect(0, 1);
    require_pixels(*source->layer_document(), path);
}

TEST_CASE("Changing format or any encoder configuration retires old requests and metadata-only runs miss", "[sla_encoded_mask_reuse]") {
    ReuseSwitch enabled("1"); ReuseDirectory directory; WriterTrace trace;
    const std::string key = GENERATE("exposure_time", "gamma_correction", "display_mirror_x", "display_width", "display_pixels_x", "sla_archive_format");
    auto config = reuse_configuration(); auto model = reuse_model(); SLAPrint source;
    source.apply(model, config); source.process();
    const auto original = capture(source); REQUIRE(original->reusable_cxdlpv4_layer_count() > 0);
    const auto target = directory.path / "unchanged.cxdlpv4";
    { std::ofstream output(target.string(), std::ios::binary); output << "existing destination"; }
    const auto sentinel = read_bytes(target);
    const sla::ExportRequest old(original, "cxdlpv4", target.string());
    config = source.full_print_config();
    if (key == "sla_archive_format") config.set(key, "SL1");
    else if (key == "display_mirror_x") config.set(key, !config.opt_bool(key));
    else if (key == "display_pixels_x") config.set(key, config.opt_int(key) + 1);
    else config.set(key, config.opt_float(key) + .25);
    source.apply(model, config);
    CHECK_FALSE(original->generation().current()); CHECK_THROWS_AS(old.write(), CanceledException);
    CHECK(read_bytes(target) == sentinel); CHECK(no_staging(directory.path));
    source.process(); REQUIRE(source.finished());
    const auto current = capture(source);
    if (key == "exposure_time" || key == "sla_archive_format") CHECK(current->reusable_cxdlpv4_layer_count() == 0);
    // Full re-rasterization under changed geometry/gamma may establish a NEW
    // valid receipt. That is not reuse of the previous masks/configuration.
    const std::string format = source.full_print_config().opt_string("sla_archive_format");
    const auto output = (directory.path / (format == "SL1" ? "new.sl1" : "new.cxdlpv4")).string();
    ReuseSwitch sl1_borrow(nullptr, "SLA_EXPERIMENT_SL1_BORROW_PNG");
    ReuseSwitch sl1_compare(nullptr, "SLA_EXPERIMENT_SL1_COMPARE_ENCODED");
    sla::ExportRequest(current, format, output).write();
    const bool has_cache = current->reusable_cxdlpv4_layer_count() != 0;
    trace.expect(has_cache ? 1 : 0, has_cache || format == "SL1" ? 0 : 1,
                 std::nullopt, format == "SL1" ? 1 : 0,
                 format == "SL1" ? source.layer_document()->layer_count() : 0);
    require_pixels(*source.layer_document(), output);
}

TEST_CASE("Cache-hit requests retain independent cancellation through completed staged output", "[sla_encoded_mask_reuse]") {
    ReuseSwitch enabled("1"); ReuseDirectory directory; WriterTrace trace;
    auto model = reuse_model(); auto config = reuse_configuration();
    auto source = std::make_unique<SLAPrint>(); source->apply(model, config); source->process();
    const auto snapshot = capture(*source); REQUIRE(snapshot->reusable_cxdlpv4_layer_count() > 0);
    const auto target = directory.path / "destination.cxdlpv4";
    { std::ofstream output(target.string(), std::ios::binary); output << "old destination"; }
    const auto sentinel = read_bytes(target);
    const sla::ExportRequest cancelled(snapshot, "cxdlpv4", target.string());
    const sla::ExportRequest retry(snapshot, "cxdlpv4", (directory.path / "retry.cxdlpv4").string());
    bool saw_complete = false;
    CHECK_THROWS_AS(cancelled.write([&] {
        if (saw_complete) return;
        for (const auto &entry : boost::filesystem::directory_iterator(directory.path)) {
            if (entry.path().filename().string().find(".sla-export-") != 0) continue;
            const auto bytes = read_bytes(entry.path());
            try { const sla::CrealityCXDLPv4Layout layout(bytes); (void)layout; }
            catch (const RuntimeError &) { continue; }
            saw_complete = true; cancelled.cancel(); break;
        }
    }), CanceledException);
    trace.expect(1, 0);
    REQUIRE(saw_complete); CHECK(read_bytes(target) == sentinel); CHECK(no_staging(directory.path));
    CHECK_FALSE(retry.cancelled()); CHECK(snapshot->generation().current());
    retry.write(); trace.expect(1, 0); require_pixels(*source->layer_document(), retry.path());
    source.reset(); CHECK_FALSE(snapshot->generation().current());
    CHECK_THROWS_AS(retry.write(), CanceledException);
}

TEST_CASE("Failed final statistics cannot finalize an encoding receipt before successful retry", "[sla_encoded_mask_reuse][sla_encoded_mask_finalization]") {
    const std::string interruption = GENERATE("statistics-cancel", "statistics-error", "completion-error");
    const bool completion_error = interruption == "completion-error";
    const bool throw_error = interruption != "statistics-cancel";
    CAPTURE(interruption);
    ReuseSwitch enabled("1"); ReuseDirectory directory; WriterTrace trace;
    auto model = reuse_model(); auto config = reuse_configuration(); SLAPrint source;
    bool interrupt = true, reached = false;
    unsigned completions = 0;
    source.set_status_callback([&](const PrintBase::SlicingStatus &status) {
        if (status.text == "Slicing done") ++completions;
        if (interrupt && status.text == (completion_error ? "Slicing done" : "Generating statistics")) {
            reached = true;
            if (throw_error) throw RuntimeError("Owned test interrupts final statistics");
            source.cancel();
        }
    });
    source.apply(model, config);
    if (throw_error) CHECK_THROWS_AS(source.process(), RuntimeError);
    else CHECK_THROWS_AS(source.process(), CanceledException);
    REQUIRE(reached); CHECK(completions == (completion_error ? 1 : 0)); REQUIRE(source.layer_document());
    const auto generation = source.export_generation();
    std::vector<std::vector<uint8_t>> before;
    for (size_t i = 0; i < source.layer_document()->layer_count(); ++i) {
        const auto png = source.layer_document()->encoded_layer(i);
        const auto *data = static_cast<const uint8_t *>(png.buf);
        before.emplace_back(data, data + png.sz);
    }
    // Actual lifecycle cleanup/restart, no private flag mutation or custom
    // state-machine substitute. Done slicing milestones may already exist.
    source.cleanup(); source.restart(); interrupt = false;
    REQUIRE(source.finished());
    const auto unfinished_receipt = capture(source);
    CHECK(unfinished_receipt->reusable_cxdlpv4_layer_count() == 0);
    CHECK(unfinished_receipt->reusable_cxdlpv4_payload_bytes() == 0);
    CHECK(source.export_generation() == generation);
    REQUIRE_NOTHROW(source.process());
    CHECK(completions == (completion_error ? 2 : 1)); REQUIRE(source.finished());
    const auto retry = capture(source);
    REQUIRE(retry->reusable_cxdlpv4_layer_count() == source.layer_document()->layer_count());
    for (size_t i = 0; i < before.size(); ++i) {
        const auto png = source.layer_document()->encoded_layer(i);
        const auto *data = static_cast<const uint8_t *>(png.buf);
        CHECK(std::vector<uint8_t>(data, data + png.sz) == before[i]);
    }
    const auto path = (directory.path / "statistics-retry.cxdlpv4").string();
    sla::ExportRequest(retry, "cxdlpv4", path).write();
    trace.expect(1, 0); require_pixels(*source.layer_document(), path);
}
