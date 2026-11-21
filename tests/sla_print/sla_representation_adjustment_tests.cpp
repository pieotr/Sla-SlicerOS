// SPDX-License-Identifier: AGPL-3.0-or-later
// "Ask and adjust": a destination that cannot store a job's settings exactly lists every adjustment in one
// plan; approved adjustments reach only the written file. Strict export lists every problem at once.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <boost/filesystem.hpp>
#include <fstream>
#include "sla_archive_test_config.hpp"
#include "libslic3r/SLAPrint.hpp"
#include "libslic3r/Format/SLAArchiveFormatRegistry.hpp"
#include "libslic3r/SLA/LayerDocument.hpp"
#include "libslic3r/SLA/LayerConversionPlan.hpp"
#include "libslic3r/SLA/ExportRequest.hpp"
#include "libslic3r/SLA/FreshLayerJob.hpp"
#include "libslic3r/PNGReadWrite.hpp"

using namespace Slic3r;
using Catch::Matchers::ContainsSubstring;

namespace {
constexpr size_t layers = 6; // the app reader sniffs v1 by its layer-count word (2..4 reads as a newer version)
struct TempFile {
    boost::filesystem::path path;
    explicit TempFile(const std::string &extension)
        : path(boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("adjust-%%%%-%%%%." + extension)) {}
    ~TempFile() { boost::system::error_code error; boost::filesystem::remove(path, error); }
};

// A material made in stock PrusaSlicer (LCD motion keys at their defaults) on a 1620x2560 printer called "PHOTON MONO".
DynamicPrintConfig user_profile()
{
    SLAFullPrintConfig full;
    Test::configure_archive_fixture(full, "cxdlp");
    full.set("sla_archive_format", std::string("SL1"));
    full.set("supports_enable", false); full.set("pad_enable", false); full.set("hollowing_enable", false);
    DynamicPrintConfig c;
    c.apply(full);
    c.set("printer_model", std::string("PHOTON MONO"));
    c.set("display_pixels_x", 1620); c.set("display_pixels_y", 2560);
    c.set("display_width", 82.62); c.set("display_height", 130.56);
    c.set_deserialize_strict("display_orientation", "landscape");
    c.set("exposure_time", 2.5); c.set("initial_exposure_time", 40.);
    c.set("sla_bottom_layers", 2); c.set("sla_transition_layers", 0);
    for (const char *key : {"sla_wait_before_cure", "sla_bottom_wait_before_cure", "sla_wait_after_cure", "sla_bottom_wait_after_cure",
                            "sla_wait_after_lift", "sla_bottom_wait_after_lift", "sla_lift_height2", "sla_bottom_lift_height2",
                            "sla_retract_height2", "sla_bottom_retract_height2"})
        c.set(key, 0.);
    c.set("sla_lift_height", 6.); c.set("sla_bottom_lift_height", 6.);
    c.set("sla_lift_speed", 80.); c.set("sla_bottom_lift_speed", 60.);
    c.set("sla_retract_speed", 150.); c.set("sla_bottom_retract_speed", 150.);
    c.set("sla_light_pwm", 255); c.set("sla_bottom_light_pwm", 255);
    return c;
}

sla::LayerDocument fresh_document(const DynamicPrintConfig &config)
{
    SLAPrinterConfig printer; printer.apply(config, true);
    SLAMaterialConfig material; material.apply(config, true);
    std::vector<double> positions;
    for (size_t i = 0; i < layers; ++i) positions.push_back(double(i + 1) * config.opt_float("layer_height"));
    auto job = sla::make_fresh_layer_job(printer, material, config.opt_float("layer_height"), positions, sla::FreshLayerProducer::ModelSlice);
    std::vector<uint8_t> pixels(job.geometry.columns * job.geometry.rows, 0);
    for (size_t y = job.geometry.rows / 4; y < job.geometry.rows / 2; ++y)
        for (size_t x = job.geometry.columns / 8; x < job.geometry.columns / 2; ++x) pixels[y * job.geometry.columns + x] = 255;
    std::vector<sla::EncodedRaster> encoded;
    for (size_t i = 0; i < layers; ++i) encoded.push_back(sla::PNGRasterEncoder{}(pixels.data(), job.geometry.columns, job.geometry.rows, 1));
    return sla::LayerDocument::from_fresh_rasters(std::move(encoded), std::move(job));
}

std::vector<std::string> labels(const sla::RepresentationPlan &plan)
{
    std::vector<std::string> result;
    for (const auto &a : plan.adjustments) result.push_back(a.label);
    return result;
}
const sla::RepresentationAdjustment &find(const sla::RepresentationPlan &plan, const std::string &label)
{
    for (const auto &a : plan.adjustments) if (a.label == label) return a;
    FAIL("missing adjustment " << label);
    return plan.adjustments.front();
}
}

TEST_CASE("CXDLP preflight lists every adjustment for the stock-material job", "[sla_representation]")
{
    const auto config = user_profile();
    const auto document = fresh_document(config);
    SECTION("v2/v3: wait, lift speed, retract speed and printer model") {
        const auto plan = sla::plan_export_representation("cxdlp", config, &document);
        CHECK(plan.blockers.empty());
        REQUIRE(labels(plan) == std::vector<std::string>{"Wait before exposure", "Lift speed", "Retract speed", "Printer model"});
        CHECK(find(plan, "Wait before exposure").from == "0");
        CHECK(find(plan, "Wait before exposure").to == "1");
        CHECK(find(plan, "Wait before exposure").keys.size() == 2);
        CHECK(find(plan, "Lift speed").from == "80");
        CHECK(find(plan, "Lift speed").to == "60");
        CHECK(find(plan, "Retract speed").to == "120");
        CHECK(find(plan, "Printer model").to == "CL-60");
        CHECK_THAT(find(plan, "Printer model").reason, ContainsSubstring("1620x2560"));
        CHECK(sla::plan_export_representation("cxdlpv2", config, &document).adjustments.size() == 4);
    }
    SECTION("v1 stores whole seconds and has no printer model") {
        const auto plan = sla::plan_export_representation("cxdlpv1", config, &document);
        CHECK(labels(plan) == std::vector<std::string>{"Exposure", "Wait before exposure"});
        CHECK(find(plan, "Exposure").to == "3");
    }
    SECTION("more fields: lift height, exposure grid, PWM") {
        auto other = config;
        other.set("sla_lift_height", 6.4); other.set("exposure_time", 2.56); other.set("sla_light_pwm", 300);
        other.set("printer_model", std::string("CL-60"));
        const auto plan = sla::plan_export_representation("cxdlp", other, nullptr);
        CHECK(find(plan, "Lift height").to == "6");
        CHECK(find(plan, "Exposure").to == "2.6");
        CHECK(find(plan, "UV power").to == "255");
    }
    SECTION("what no adjustment can fix is listed together with the rest") {
        auto other = config;
        other.set("sla_lift_height2", 2.); other.set("sla_transition_layers", 3);
        other.set("display_pixels_x", 1621); // no Creality machine
        other.set("sla_bottom_wait_before_cure", 5.); // normal and bottom differ truly
        const auto plan = sla::plan_representation("cxdlp", other);
        CHECK(plan.blockers.size() >= 4);
        const auto message = plan.strict_message("CXDLP v2/v3");
        CHECK_THAT(message, ContainsSubstring("transition"));
        CHECK_THAT(message, ContainsSubstring("second lift height"));
        CHECK_THAT(message, ContainsSubstring("1621x2560"));
        CHECK_THAT(message, ContainsSubstring("one Wait before exposure"));
        CHECK_THAT(message, ContainsSubstring("Lift speed 80 mm/min -> 60 mm/min"));
    }
}

TEST_CASE("CXDLP strict export lists all problems; approved adjustments reach only the file", "[sla_representation]")
{
    const auto config = user_profile();
    const auto document = fresh_document(config);
    for (const std::string id : {"cxdlp", "cxdlpv2", "cxdlpv1"}) {
        INFO("format " << id);
        TempFile strict(get_default_extension(id.c_str()));
        try {
            sla::NativeArchiveConversionRequest(document, config, id, strict.path.string()).write();
            FAIL("an unrepresentable job must be refused without approval");
        } catch (const std::exception &e) {
            const std::string message = e.what();
            CHECK_THAT(message, ContainsSubstring("Wait before exposure 0 s -> 1 s"));
            if (id != "cxdlpv1") {
                CHECK_THAT(message, ContainsSubstring("Lift speed 80 mm/min -> 60 mm/min"));
                CHECK_THAT(message, ContainsSubstring("Retract speed 150 mm/min -> 120 mm/min"));
                CHECK_THAT(message, ContainsSubstring("Printer model PHOTON MONO -> CL-60"));
            } else CHECK_THAT(message, ContainsSubstring("Exposure 2.5 s -> 3 s"));
        }
        CHECK_FALSE(boost::filesystem::exists(strict.path));

        const auto plan = sla::plan_export_representation(id, config, &document);
        TempFile adjusted(get_default_extension(id.c_str()));
        REQUIRE_NOTHROW(sla::NativeArchiveConversionRequest(document, config, id, adjusted.path.string(), {}, plan.adjustments).write());
        if (const char *keep = std::getenv("SLA_REPRESENTATION_KEEP")) // oracle runs read the files with UVtools
            boost::filesystem::copy_file(adjusted.path, boost::filesystem::path(keep) / ("adjusted." + std::string(get_default_extension(id.c_str()))),
                                         boost::filesystem::copy_options::overwrite_existing);
        const auto reopened = sla::LayerDocument::load_archive(adjusted.path.string());
        REQUIRE(reopened.layer_count() == layers);
        // Nothing left to adjust: the written values are exactly the planned ones.
        const auto row = reopened.layer_parameters(0);
        using F = sla::LayerParameter;
        if (auto it = row.find(F::WaitBefore); it != row.end()) CHECK(it->second == 1.);
        if (auto it = row.find(F::LiftSpeed); it != row.end() && id != "cxdlpv1") CHECK(it->second == 60.);
        // Header model (v2/v3): offset 4+9+2 = 15 -> big-endian length, then the NUL-terminated name.
        std::ifstream in(adjusted.path.string(), std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(in)), {});
        if (id != "cxdlpv1") CHECK(bytes.find("CL-60") != std::string::npos);
        CHECK(bytes.find("PHOTON MONO") == std::string::npos);
        // The job's own configuration is untouched, so a second approval-less export is still refused.
        TempFile again(get_default_extension(id.c_str()));
        CHECK_THROWS(sla::NativeArchiveConversionRequest(document, config, id, again.path.string()).write());
    }
}

namespace {
// A job in the shape of the format's own fixture (valid geometry), set up like a stock-material job.
DynamicPrintConfig family_profile(const std::string &id)
{
    SLAFullPrintConfig full;
    Test::configure_archive_fixture(full, id);
    full.set("sla_archive_format", std::string("SL1"));
    full.set("supports_enable", false); full.set("pad_enable", false); full.set("hollowing_enable", false);
    if (id != "qdt") full.set("sla_bottom_layers", 2);
    DynamicPrintConfig c;
    c.apply(full);
    return c;
}
struct FamilyCase {
    std::string id;
    std::vector<std::pair<std::string, double>> adjustable;   // settings the container can only store rounded/fixed
    std::vector<std::string> expected;                        // substrings of the strict message for them
    std::vector<std::pair<std::string, double>> blocking;     // settings nothing can fix
    std::string blocker;                                      // substring of the blocker text
};
const std::vector<FamilyCase> &family_cases()
{
    static const std::vector<FamilyCase> cases = {
        {"gr1", {{"exposure_time", 2.5}, {"sla_lift_height", 6.4}, {"sla_lift_speed", 120.5}}, {"Exposure 2.5 s -> 3 s", "Lift height", "Lift speed"},
         {{"sla_lift_height2", 2.}}, "second lift height"},
        {"mdlp", {{"exposure_time", 2.5}, {"sla_lift_height", 8.}}, {"Exposure 2.5 s -> 3 s", "Lift height"},
         {{"sla_transition_layers", 3.}}, "transition"},
        {"n4", {{"exposure_time", 2.5}, {"sla_lift_height", 6.4}}, {"Exposure 2.5 s -> 3 s", "Lift height"},
         {{"sla_wait_before_cure", 1.}, {"sla_bottom_wait_before_cure", 1.}}, "wait before exposure"},
        {"n7", {{"exposure_time", 2.5}, {"sla_lift_height", 6.4}}, {"Exposure 2.5 s -> 3 s", "Lift height"},
         {{"sla_lift_height2", 1.}}, "second lift height"},
        {"qdt", {{"exposure_time", 2.5}, {"sla_lift_height", 8.}}, {"Exposure 2.5 s -> 3 s", "Lift height"},
         {{"sla_wait_after_cure", 2.}}, "wait after exposure"},
        {"svgx", {{"exposure_time", 2.504}, {"sla_lift_height", 8.}}, {"Exposure 2.504 s -> 2.5 s", "Lift height"},
         {{"sla_transition_layers", 2.}}, "transition"},
        {"jxs", {{"exposure_time", 2.504}, {"sla_lift_speed", 80.123}}, {"Exposure 2.504 s -> 2.5 s", "Lift speed"},
         {{"sla_bottom_layers", 70000.}}, "bottom layer count"},
        {"zcode", {{"exposure_time", 2.504}, {"sla_lift_speed", 80.55}}, {"Exposure 2.504 s -> 2.5 s", "Lift speed"},
         {{"sla_light_pwm", 0.}}, "UV power 0"},
        {"zcodex", {{"exposure_time", 2.504}, {"sla_lift_speed", 120.5}, {"sla_bottom_lift_speed", 120.5}}, {"Exposure 2.504 s -> 2.5 s", "Lift speed 120.5 mm/min -> 120 mm/min"},
         {{"sla_lift_height2", 1.}}, "second lift height"},
        {"vdt", {{"exposure_time", 2.504}, {"sla_lift_speed", 80.123}}, {"Exposure 2.504 s -> 2.5 s", "Lift speed"},
         {{"sla_bottom_layers", 70000.}}, "bottom layer count"},
        {"klipper", {{"exposure_time", 2.504}, {"sla_lift_speed", 80.123}}, {"Exposure 2.504 s -> 2.5 s", "Lift speed"},
         {{"sla_light_pwm", 0.}}, "UV power 0"},
    };
    return cases;
}
void set_all(DynamicPrintConfig &c, const std::vector<std::pair<std::string, double>> &values)
{
    for (const auto &[key, value] : values) {
        if (c.option(key)->type() == coInt) c.set(key, int(value)); else c.set(key, value);
    }
}
} // namespace

TEST_CASE("every writer family reports all unrepresentable values in one plan", "[sla_representation]")
{
    for (const auto &item : family_cases()) {
        INFO("format " << item.id);
        // The valid fixture of the format needs nothing.
        const auto valid = family_profile(item.id);
        {
            const auto plan = sla::plan_export_representation(item.id, valid, nullptr);
            INFO(plan.strict_message(item.id));
            CHECK(plan.empty());
        }
        auto adjustable = valid;
        set_all(adjustable, item.adjustable);
        const auto plan = sla::plan_export_representation(item.id, adjustable, nullptr);
        CHECK(plan.blockers.empty());
        CHECK_FALSE(plan.adjustments.empty());
        const auto message = plan.strict_message(sla::representation_format_label(item.id));
        for (const auto &text : item.expected) CHECK_THAT(message, ContainsSubstring(text));

        auto both = adjustable;
        set_all(both, item.blocking);
        const auto combined = sla::plan_export_representation(item.id, both, nullptr);
        CHECK_FALSE(combined.blockers.empty());
        // One refusal carries the adjustable values and the blocker together.
        const auto all = combined.strict_message(sla::representation_format_label(item.id));
        for (const auto &text : item.expected) CHECK_THAT(all, ContainsSubstring(text));
        CHECK_THAT(all, ContainsSubstring(item.blocker));

        // Applying the approved adjustments leaves nothing to adjust.
        auto applied = adjustable;
        sla::apply_representation_adjustments(applied, plan.adjustments);
        const auto after = sla::plan_export_representation(item.id, applied, nullptr);
        INFO(after.strict_message(item.id));
        CHECK(after.empty());
    }
    SECTION("CTB v3 lists the second stage and the rests it has no fields for") {
        auto c = family_profile("ctb_v3");
        CHECK(sla::plan_export_representation("ctb_v3", c, nullptr).empty());
        c.set("sla_lift_height2", 1.); c.set("sla_bottom_wait_after_cure", 2.); c.set("sla_wait_after_lift", 3.);
        const auto plan = sla::plan_export_representation("ctb_v3", c, nullptr);
        CHECK(plan.blockers.size() == 3);
        const auto message = plan.strict_message("Chitubox CTB v3");
        CHECK_THAT(message, ContainsSubstring("normal second lift height"));
        CHECK_THAT(message, ContainsSubstring("bottom wait after exposure"));
        CHECK_THAT(message, ContainsSubstring("normal wait after lift"));
    }
}

TEST_CASE("writer families refuse with the full list and write adjusted values after approval", "[sla_representation]")
{
    for (const auto &item : family_cases()) {
        INFO("format " << item.id);
        auto blocked = family_profile(item.id);
        set_all(blocked, item.adjustable);
        set_all(blocked, item.blocking);
        {
            const auto document = fresh_document(blocked);
            TempFile strict(get_default_extension(item.id.c_str()));
            try {
                sla::NativeArchiveConversionRequest(document, blocked, item.id, strict.path.string()).write();
                FAIL("an unrepresentable job must be refused");
            } catch (const std::exception &e) {
                const std::string message = e.what();
                for (const auto &text : item.expected) CHECK_THAT(message, ContainsSubstring(text));
                CHECK_THAT(message, ContainsSubstring(item.blocker));
            }
            CHECK_FALSE(boost::filesystem::exists(strict.path));
        }
        auto config = family_profile(item.id);
        set_all(config, item.adjustable);
        const auto document = fresh_document(config);
        TempFile strict(get_default_extension(item.id.c_str()));
        try {
            sla::NativeArchiveConversionRequest(document, config, item.id, strict.path.string()).write();
            FAIL("without approval the adjustable values are refused");
        } catch (const std::exception &e) {
            for (const auto &text : item.expected) CHECK_THAT(std::string(e.what()), ContainsSubstring(text));
        }
        CHECK_FALSE(boost::filesystem::exists(strict.path));
        const auto plan = sla::plan_export_representation(item.id, config, &document);
        REQUIRE(plan.blockers.empty());
        TempFile adjusted(get_default_extension(item.id.c_str()));
        REQUIRE_NOTHROW(sla::NativeArchiveConversionRequest(document, config, item.id, adjusted.path.string(), {}, plan.adjustments).write());
        CHECK(boost::filesystem::file_size(adjusted.path) > 0);
        TempFile again(get_default_extension(item.id.c_str()));
        CHECK_THROWS(sla::NativeArchiveConversionRequest(document, config, item.id, again.path.string()).write());
    }
}

TEST_CASE("geometry of the fixed-screen writers is part of the same plan", "[sla_representation]")
{
    auto anet = family_profile("n4");
    anet.set("display_pixels_x", 1620); anet.set("exposure_time", 2.5);
    const auto plan = sla::plan_export_representation("n4", anet, nullptr);
    const auto message = plan.strict_message("Anet N4");
    CHECK_THAT(message, ContainsSubstring("Anet N4 screen"));
    CHECK_THAT(message, ContainsSubstring("Exposure 2.5 s -> 3 s"));
    auto gr1 = family_profile("gr1");
    gr1.set("display_mirror_x", true); gr1.set("display_pixels_x", 40000); gr1.set("display_width", 100.123);
    const auto gr1_message = sla::plan_export_representation("gr1", gr1, nullptr).strict_message("GR1");
    CHECK_THAT(gr1_message, ContainsSubstring("display mirroring"));
    CHECK_THAT(gr1_message, ContainsSubstring("resolution"));
    CHECK_THAT(gr1_message, ContainsSubstring("2 decimals"));
    auto jxs = family_profile("jxs");
    jxs.set("display_mirror_x", true);
    CHECK_THAT(sla::plan_export_representation("jxs", jxs, nullptr).strict_message("JXS"), ContainsSubstring("mirroring"));
    auto qdt = family_profile("qdt");
    qdt.set("display_pixels_x", 3);
    CHECK_THAT(sla::plan_export_representation("qdt", qdt, nullptr).strict_message("QDT"), ContainsSubstring("resolution"));
}
