// SPDX-License-Identifier: AGPL-3.0-or-later
// UVtools suggestions backend (SLA/LayerSuggestions): synthetic jobs, expected values worked out
// from the UVtools.Core/Suggestions sources.
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <atomic>

#include "libslic3r/PrintBase.hpp"
#include "libslic3r/SLA/LayerSuggestions.hpp"

using namespace Slic3r;
using namespace Slic3r::sla;
using Catch::Approx;

namespace {

// 0.05 mm layers, `bottom` bottom layers at 30 s, the rest at 2.5 s, waits initialised to `wait`.
SuggestionJob make_job(size_t layers, size_t bottom = 4, double layer_h = 0.05, double wait = 0.)
{
    SuggestionJob job;
    job.layer_height_mm = layer_h;
    job.bottom_layer_count = bottom;
    job.bottom_exposure_s = 30;
    job.normal_exposure_s = 2.5;
    for (size_t i = 0; i < layers; ++i) {
        job.z_mm.push_back(double(i + 1) * layer_h);
        job.exposure_s.push_back(i < bottom ? 30. : 2.5);
        job.wait_before_s.push_back(wait);
        job.wait_after_s.push_back(wait);
    }
    job.resolution_x = 1000;
    job.resolution_y = 600;
    job.pixel_width_mm = job.pixel_height_mm = 0.05;
    return job;
}

// Every layer carries `pixels` lit pixels in the box x:[x0,x1] y:[y0,y1].
SuggestionLayerStatsProvider uniform_stats(size_t pixels, int x0 = 300, int y0 = 200, int x1 = 499, int y1 = 299)
{
    return [=](size_t) {
        SuggestionLayerStats s;
        s.nonzero_pixels = pixels;
        s.bbox = BoundingBox(Point(x0, y0), Point(x1, y1));
        return s;
    };
}

void apply(SuggestionJob &job, const SuggestionResult &r)
{
    for (const SuggestedLayerChange &c : r.layer_changes) {
        auto &column = c.field == LayerParameter::Exposure ? job.exposure_s
                     : c.field == LayerParameter::WaitBefore ? job.wait_before_s : job.wait_after_s;
        column.at(c.layer) = c.after;
    }
}

} // namespace

TEST_CASE("Suggestions: bottom layer count", "[sla_layers][sla_suggestions]")
{
    // 0.25 mm target / 0.05 mm layers = 5 layers, inside [3, 7].
    SuggestionJob job = make_job(40, 5);
    auto r = suggest_bottom_layer_count(job);
    CHECK(r.id == "BottomLayerCount");
    CHECK(r.status == SuggestionStatus::Applied);
    CHECK(!r.bottom_layer_count);
    CHECK(r.layer_changes.empty());

    job = make_job(40, 10); // 0.5 mm: above the 0.4 mm limit
    r = suggest_bottom_layer_count(job);
    REQUIRE(r.status == SuggestionStatus::NotApplied);
    REQUIRE(r.bottom_layer_count);
    CHECK(r.bottom_layer_count->first == 10);
    CHECK(r.bottom_layer_count->second == 5);
    CHECK(r.message.find("(10)") != std::string::npos);
    CHECK(!r.confirmation.empty());

    job = make_job(40, 1); // 0.05 mm: below the 0.07 mm limit
    CHECK(suggest_bottom_layer_count(job).status == SuggestionStatus::NotApplied);

    BottomLayerCountSuggestionConfig exact;
    exact.apply_when = SuggestionApplyWhen::Different;
    job = make_job(40, 4); // 0.2 mm is inside the limits but not the target
    CHECK(suggest_bottom_layer_count(job).status == SuggestionStatus::Applied);
    r = suggest_bottom_layer_count(job, exact);
    REQUIRE(r.status == SuggestionStatus::NotApplied);
    CHECK(r.bottom_layer_count->second == 5);

    job = make_job(40, 3, 0.1); // 0.1 mm layers: ceil(0.25 / 0.1) = 3 layers
    r = suggest_bottom_layer_count(job, exact);
    CHECK(r.status == SuggestionStatus::NotApplied); // 0.3 mm != 0.25 mm target height
    CHECK(r.bottom_layer_count->second == 3);

    job = make_job(40, 10);
    job.can_set_bottom_layer_count = false;
    r = suggest_bottom_layer_count(job);
    CHECK(r.status == SuggestionStatus::Unavailable);
    CHECK(!r.unavailable_reason.empty());

    BottomLayerCountSuggestionConfig invalid;
    invalid.minimum_bottom_layer_count = 9;
    CHECK(suggest_bottom_layer_count(make_job(40, 5), invalid).status == SuggestionStatus::Unavailable);
}

TEST_CASE("Suggestions: layer height is informational", "[sla_layers][sla_suggestions]")
{
    auto r = suggest_layer_height(make_job(20));
    CHECK(r.status == SuggestionStatus::Applied);
    CHECK(r.informational_only);
    CHECK(r.layer_changes.empty());

    r = suggest_layer_height(make_job(20, 4, 0.02)); // thinner than 0.03 mm
    CHECK(r.status == SuggestionStatus::NotApplied);
    CHECK(r.informational_only);
    CHECK(r.layer_changes.empty());

    r = suggest_layer_height(make_job(20, 4, 0.123)); // three decimals, only two allowed
    CHECK(r.status == SuggestionStatus::NotApplied);

    LayerHeightSuggestionConfig three_digits;
    three_digits.maximum_decimal_digits = 3;
    CHECK(suggest_layer_height(make_job(20, 4, 0.075), three_digits).status == SuggestionStatus::Applied);
    CHECK(suggest_layer_height(make_job(20, 4, 0.075)).status == SuggestionStatus::NotApplied);

    SuggestionJob irregular = make_job(10);
    for (size_t i = 3; i < 10; ++i) irregular.z_mm[i] += 0.15; // one 0.2 mm layer in the middle
    r = suggest_layer_height(irregular);
    CHECK(r.status == SuggestionStatus::NotApplied);
}

TEST_CASE("Suggestions: transition layers", "[sla_layers][sla_suggestions]")
{
    // 30 s bottom, 2.5 s normal, no ramp: step 2 s gives round(27.5 / 2 - 1) = 13, clamped to 10 layers.
    SuggestionJob job = make_job(20, 3);
    auto r = suggest_transition_layer_count(job);
    REQUIRE(r.status == SuggestionStatus::NotApplied);
    REQUIRE(r.transition_layer_count);
    CHECK(r.transition_layer_count->first == 0);
    CHECK(r.transition_layer_count->second == 10);
    // Software transition: ten exposures from 27.5 s down to 5 s, 2.5 s s per step.
    REQUIRE(r.layer_changes.size() == 10);
    CHECK(r.layer_changes.front().layer == 3);
    CHECK(r.layer_changes.front().after == Approx(27.5));
    CHECK(r.layer_changes.back().layer == 12);
    CHECK(r.layer_changes.back().after == Approx(5.0));
    CHECK(r.layer_changes.front().field == LayerParameter::Exposure);

    // The proposal, applied and recorded as the file's transition count, satisfies the suggestion.
    apply(job, r);
    job.transition_layer_count = 10;
    CHECK(suggest_transition_layer_count(job).status == SuggestionStatus::Applied);

    // Firmware transitions are a printer setting: the count changes, layers stay untouched.
    SuggestionJob firmware = make_job(20, 3);
    firmware.transition_in_firmware = true;
    r = suggest_transition_layer_count(firmware);
    REQUIRE(r.status == SuggestionStatus::NotApplied);
    CHECK(r.layer_changes.empty());
    CHECK(r.transition_layer_count->second == 10);

    // A job that cannot hold a transition counts as applied (UVtools).
    SuggestionJob flat = make_job(20, 3);
    flat.bottom_exposure_s = flat.normal_exposure_s;
    CHECK(suggest_transition_layer_count(flat).status == SuggestionStatus::Applied);
    CHECK(suggest_transition_layer_count(make_job(5, 3)).status == SuggestionStatus::Applied); // too few layers

    SuggestionJob no_exposure = make_job(20, 3);
    no_exposure.exposure_s.clear();
    CHECK(suggest_transition_layer_count(no_exposure).status == SuggestionStatus::Unavailable);
}

TEST_CASE("Suggestions: wait time after cure", "[sla_layers][sla_suggestions]")
{
    // Fixed 7 s bottom / 1 s normal.
    SuggestionJob job = make_job(12, 4, 0.05, 0.);
    auto r = suggest_wait_time_after_cure(job);
    REQUIRE(r.status == SuggestionStatus::NotApplied);
    REQUIRE(r.layer_changes.size() == 12);
    CHECK(r.layer_changes[0].field == LayerParameter::WaitAfter);
    CHECK(r.layer_changes[3].after == Approx(7.));
    CHECK(r.layer_changes[4].after == Approx(1.));
    CHECK(r.message.find("7/1s") != std::string::npos);

    apply(job, r);
    CHECK(suggest_wait_time_after_cure(job).status == SuggestionStatus::Applied);
    job.wait_after_s[6] += 0.05; // within the 0.1 s tolerance
    CHECK(suggest_wait_time_after_cure(job).status == SuggestionStatus::Applied);
    job.wait_after_s[6] += 0.5;
    CHECK(suggest_wait_time_after_cure(job).status == SuggestionStatus::NotApplied);

    // Proportional: 2 s of wait per 6 s of exposure, limited to [3, 20] s bottom and [1, 12] s normal.
    WaitTimeAfterCureSuggestionConfig proportional;
    proportional.set_type = WaitTimeAfterCureSuggestionConfig::SetType::ProportionalExposure;
    proportional.proportional_wait_s = 2;
    proportional.proportional_exposure_s = 6;
    SuggestionJob p = make_job(8, 2, 0.05, 0.);
    p.exposure_s = {30, 30, 9, 9, 9, 9, 9, 9};
    r = suggest_wait_time_after_cure(p, proportional);
    REQUIRE(r.status == SuggestionStatus::NotApplied);
    CHECK(r.layer_changes[0].after == Approx(10.)); // 30 * 2 / 6
    CHECK(r.layer_changes[2].after == Approx(3.));  //  9 * 2 / 6

    // A blank layer (<= 1 lit pixel) is a dummy layer: it must have no wait at all.
    SuggestionJob dummy = make_job(6, 2, 0.05, 1.);
    dummy.wait_after_s[0] = dummy.wait_after_s[1] = 7.;
    dummy.stats = [](size_t layer) { SuggestionLayerStats s; s.nonzero_pixels = layer == 5 ? 0 : 100; return s; };
    r = suggest_wait_time_after_cure(dummy);
    REQUIRE(r.status == SuggestionStatus::NotApplied);
    REQUIRE(r.layer_changes.size() == 1);
    CHECK(r.layer_changes[0].layer == 5);
    CHECK(r.layer_changes[0].after == Approx(0.));

    SuggestionJob none = make_job(6);
    none.wait_after_s.clear();
    CHECK(suggest_wait_time_after_cure(none).status == SuggestionStatus::Unavailable);

    WaitTimeAfterCureSuggestionConfig invalid;
    invalid.minimum_normal_s = 5; invalid.maximum_normal_s = 2;
    CHECK(suggest_wait_time_after_cure(make_job(6), invalid).status == SuggestionStatus::Unavailable);
}

TEST_CASE("Suggestions: wait time before cure, fixed ramp", "[sla_layers][sla_suggestions]")
{
    // Layers up to Z = 1 mm (20 layers of 0.05 mm) count as bottom: 15 s. Then eight ramp layers
    // down by round((15 - 2) / 9, 2) = 1.44 s per layer, finally the 2 s normal wait.
    WaitTimeBeforeCureSuggestionConfig fixed;
    fixed.set_type = WaitTimeBeforeCureSuggestionConfig::SetType::Fixed;
    SuggestionJob job = make_job(40, 4, 0.05, 0.);
    auto r = suggest_wait_time_before_cure(job, fixed);
    REQUIRE(r.status == SuggestionStatus::NotApplied);
    REQUIRE(r.layer_changes.size() == 40);
    CHECK(r.layer_changes[0].field == LayerParameter::WaitBefore);
    CHECK(r.layer_changes[19].after == Approx(15.));
    CHECK(r.layer_changes[20].after == Approx(13.56));
    CHECK(r.layer_changes[21].after == Approx(12.12));
    CHECK(r.layer_changes[28].after == Approx(2.04));
    CHECK(r.layer_changes[29].after == Approx(2.));
    CHECK(r.layer_changes[39].after == Approx(2.));
    CHECK(!r.prepend_blank_first_layer);

    apply(job, r);
    CHECK(suggest_wait_time_before_cure(job, fixed).status == SuggestionStatus::Applied);

    // Chitubox / CTB / GOO may take the near-empty first layer; the extra layer counts as bottom.
    SuggestionJob chitubox = make_job(40, 4, 0.05, 0.);
    chitubox.can_prepend_blank_layer = true;
    chitubox.stats = uniform_stats(1000);
    r = suggest_wait_time_before_cure(chitubox, fixed);
    CHECK(r.prepend_blank_first_layer);
    REQUIRE(r.bottom_layer_count);
    CHECK(r.bottom_layer_count->second == 5);

    SuggestionJob none = make_job(10);
    none.wait_before_s.clear();
    CHECK(suggest_wait_time_before_cure(none, fixed).status == SuggestionStatus::Unavailable);
}

TEST_CASE("Suggestions: wait time before cure, proportional to area", "[sla_layers][sla_suggestions]")
{
    // 1,000,000 px * 0.0025 mm2 = 2500 mm2 per layer. Normal: 2500 * 10 s / 10000 mm2 = 2.5 s;
    // bottom: 2500 * 30 / 10000 = 7.5 s, raised to the 10 s minimum.
    // The "previous layer" limit (+-1 s) works on the already updated neighbour, layer after layer.
    SuggestionJob job = make_job(30, 4, 0.05, 0.);
    job.stats = uniform_stats(1000000);
    auto r = suggest_wait_time_before_cure(job);
    REQUIRE(r.status == SuggestionStatus::NotApplied);
    std::vector<double> proposed = job.wait_before_s;
    for (const auto &c : r.layer_changes) proposed[c.layer] = c.after;
    CHECK(proposed[0] == Approx(10.));
    CHECK(proposed[19] == Approx(10.)); // layers up to Z = 1 mm are bottom layers
    CHECK(proposed[20] == Approx(4.));  // 2.5 s wanted, but at most 1 s below the previous 10 s, then capped at 4 s
    CHECK(proposed[21] == Approx(3.));
    CHECK(proposed[22] == Approx(2.5));
    CHECK(proposed[29] == Approx(2.5));

    apply(job, r);
    CHECK(suggest_wait_time_before_cure(job).status == SuggestionStatus::Applied);

    // Pixel mode: 5,000,000 px give the full 10 s of the normal reference.
    WaitTimeBeforeCureSuggestionConfig pixels;
    pixels.set_type = WaitTimeBeforeCureSuggestionConfig::SetType::ProportionalLayerPixels;
    pixels.bottom_height_mm = 0;
    pixels.proportional_normal_max_difference_s = 0; // no neighbour limit
    pixels.maximum_normal_s = 20;
    SuggestionJob px = make_job(10, 2, 0.05, 0.);
    px.stats = uniform_stats(2500000);
    r = suggest_wait_time_before_cure(px, pixels);
    REQUIRE(r.status == SuggestionStatus::NotApplied);
    CHECK(r.layer_changes.back().after == Approx(5.)); // 2.5e6 * 10 / 5e6
    CHECK(r.layer_changes[0].after == Approx(15.)); // bottom: 2.5e6 * 30 / 5e6 inside [10, 30]

    // Blank layers get no wait at all.
    SuggestionJob blank = make_job(10, 2, 0.05, 0.);
    blank.stats = [](size_t layer) { SuggestionLayerStats s; s.nonzero_pixels = layer == 4 ? 0 : 1000000; return s; };
    r = suggest_wait_time_before_cure(blank, pixels);
    std::vector<double> waits(10, 0.);
    for (const auto &c : r.layer_changes) waits[c.layer] = c.after;
    CHECK(waits[4] == Approx(0.));
    CHECK(waits[5] > 0.);

    // The area rules need masks and a pixel size.
    SuggestionJob no_masks = make_job(10);
    CHECK(suggest_wait_time_before_cure(no_masks).status == SuggestionStatus::Unavailable);
    SuggestionJob no_pixel = make_job(10);
    no_pixel.stats = uniform_stats(10);
    no_pixel.pixel_width_mm = 0;
    CHECK(suggest_wait_time_before_cure(no_pixel).status == SuggestionStatus::Unavailable);
}

TEST_CASE("Suggestions: model position", "[sla_layers][sla_suggestions]")
{
    // Model x:[300,499] y:[200,299] on a 1000 x 600 screen: inside the corner window [50, 300] px.
    SuggestionJob job = make_job(5);
    job.stats = uniform_stats(100, 300, 200, 499, 299);
    auto r = suggest_model_position(job);
    CHECK(r.status == SuggestionStatus::Applied);
    CHECK(!r.model_move);
    CHECK(r.message.find("X=300") != std::string::npos);

    // Moved to x:[400,599]: not near any corner. Random picks the nearest corner (top right here).
    job.stats = uniform_stats(100, 400, 200, 599, 299);
    r = suggest_model_position(job);
    REQUIRE(r.status == SuggestionStatus::NotApplied);
    REQUIRE(r.model_move);
    CHECK(r.model_move->anchor == "top_right");
    CHECK(r.model_move->dx_px == 300);   // right edge to 1000 - 100 = 900
    CHECK(r.model_move->dy_px == -100);  // top edge to the 100 px margin
    CHECK(r.model_move->after.min.x() == 700);
    CHECK(r.model_move->after.max.x() == 899);

    ModelPositionSuggestionConfig top_left;
    top_left.anchor = ModelPositionSuggestionConfig::Anchor::TopLeft;
    r = suggest_model_position(job, top_left);
    REQUIRE(r.model_move);
    CHECK(r.model_move->dx_px == -300);
    CHECK(r.model_move->dy_px == -100);

    ModelPositionSuggestionConfig bottom_left;
    bottom_left.anchor = ModelPositionSuggestionConfig::Anchor::BottomLeft;
    r = suggest_model_position(job, bottom_left);
    REQUIRE(r.model_move);
    CHECK(r.model_move->anchor == "bottom_left");
    CHECK(r.model_move->after.max.y() == 499); // bottom edge 100 px above the 600 px screen edge

    // "Different" demands the exact margin even though the window would accept the position.
    ModelPositionSuggestionConfig exact;
    exact.apply_when = SuggestionApplyWhen::Different;
    job.stats = uniform_stats(100, 300, 200, 499, 299);
    CHECK(suggest_model_position(job, exact).status == SuggestionStatus::NotApplied);
    job.stats = uniform_stats(100, 100, 100, 299, 199);
    CHECK(suggest_model_position(job, exact).status == SuggestionStatus::Applied);

    // A model too wide for two margins is centred on that axis: (1000 - 950) / 2 = 25 px.
    job.stats = uniform_stats(100, 25, 100, 974, 199);
    CHECK(suggest_model_position(job).status == SuggestionStatus::Applied);
    job.stats = uniform_stats(100, 0, 0, 949, 99);
    r = suggest_model_position(job, top_left);
    REQUIRE(r.model_move);
    CHECK(r.model_move->dx_px == 25);

    // Whole screen filled, empty job, no masks.
    job.stats = uniform_stats(100, 0, 0, 999, 599);
    CHECK(suggest_model_position(job).status == SuggestionStatus::Applied);
    job.stats = [](size_t) { return SuggestionLayerStats{}; };
    CHECK(suggest_model_position(job).status == SuggestionStatus::Unavailable);
    job.stats = nullptr;
    CHECK(suggest_model_position(job).status == SuggestionStatus::Unavailable);
}

TEST_CASE("Suggestions: mask statistics helper", "[sla_layers][sla_suggestions]")
{
    const auto stats = layer_stats_from_masks([](size_t layer) {
        LayerMask mask;
        mask.width = 10; mask.height = 8;
        if (layer == 0) return mask; // blank layer
        mask.data.assign(80, 0);
        mask.data[2 * 10 + 3] = 255;
        mask.data[5 * 10 + 7] = 1;
        return mask;
    });
    const auto blank = stats(0);
    CHECK(blank.nonzero_pixels == 0);
    CHECK(!blank.bbox.defined);
    const auto lit = stats(1);
    CHECK(lit.nonzero_pixels == 2);
    REQUIRE(lit.bbox.defined);
    CHECK(lit.bbox.min == Point(3, 2));
    CHECK(lit.bbox.max == Point(7, 5));
}

TEST_CASE("Suggestions: evaluate all", "[sla_layers][sla_suggestions]")
{
    SuggestionJob job = make_job(30, 8, 0.05, 0.);
    std::atomic<int> calls{0};
    job.stats = [&](size_t) {
        ++calls;
        SuggestionLayerStats s;
        s.nonzero_pixels = 1000000;
        s.bbox = BoundingBox(Point(400, 200), Point(599, 299));
        return s;
    };

    SuggestionConfig config;
    auto results = evaluate_suggestions(job, config);
    REQUIRE(results.size() == 6);
    const char *ids[] = {"BottomLayerCount", "LayerHeight", "TransitionLayerCount", "WaitTimeBeforeCure", "WaitTimeAfterCure", "ModelPosition"};
    for (size_t i = 0; i < 6; ++i) CHECK(results[i].id == ids[i]);
    CHECK(calls == 30); // masks are decoded once for all suggestions
    for (const auto &r : results) {
        CHECK(!r.message.empty());
        CHECK(!r.description.empty());
    }
    CHECK(results[0].status == SuggestionStatus::NotApplied); // 8 layers exceed the maximum of 7
    CHECK(results[3].status == SuggestionStatus::NotApplied);
    CHECK(results[4].status == SuggestionStatus::NotApplied);
    CHECK(results[5].status == SuggestionStatus::NotApplied);

    // Nothing is applied automatically: the job is untouched.
    CHECK(job.wait_before_s[5] == 0.);
    CHECK(job.bottom_layer_count == 8);

    config.wait_time_before_cure.enabled = false;
    config.model_position.enabled = false;
    CHECK(evaluate_suggestions(job, config).size() == 4);

    CHECK_THROWS_AS(evaluate_suggestions(job, SuggestionConfig{}, [] { return true; }), CanceledException);
}
