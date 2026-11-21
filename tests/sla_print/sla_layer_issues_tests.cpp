// SPDX-License-Identifier: AGPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include <atomic>

#include "libslic3r/PrintBase.hpp"
#include "libslic3r/SLA/LayerIssues.hpp"

using namespace Slic3r;
using namespace Slic3r::sla;

namespace {

constexpr size_t W = 40, H = 40;

struct Stack
{
    std::vector<LayerMask> layers;
    std::vector<double>    z;

    explicit Stack(size_t n, double layer_h = 0.1)
    {
        for (size_t i = 0; i < n; ++i) {
            layers.push_back({W, H, std::vector<uint8_t>(W * H, 0)});
            z.push_back(double(i + 1) * layer_h);
        }
    }
    void rect(size_t layer, size_t x0, size_t y0, size_t x1, size_t y1, uint8_t v = 255) // [x0,x1) x [y0,y1)
    {
        for (size_t y = y0; y < y1; ++y)
            for (size_t x = x0; x < x1; ++x) layers[layer].data[y * W + x] = v;
    }
    LayerMaskProvider provider() const
    {
        return [this](size_t i) { return layers[i]; };
    }
};

IssueDetectionConfig only(bool island, bool overhang, bool resin, bool touching, bool height, bool empty)
{
    IssueDetectionConfig c;
    c.island.enabled = island;
    c.overhang.enabled = overhang;
    c.resin_trap.enabled = resin;
    c.touching_bound.enabled = touching;
    c.print_height.enabled = height;
    c.empty_layer.enabled = empty;
    c.overhang.erode_iterations = 2; // the UVtools default of 40 px is larger than the test images
    return c;
}

std::vector<LayerIssue> run(const Stack& s, const IssueDetectionConfig& c)
{
    return detect_layer_issues(s.provider(), s.z, c);
}

size_t count_of(const std::vector<LayerIssue>& v, LayerIssue::Type t)
{
    size_t n = 0;
    for (const auto& i : v) n += i.type == t;
    return n;
}

// Closed hollow box: floor layers 0-2, walls 3-9, optional lid 10-12.
Stack make_box(bool lid)
{
    Stack s(13);
    for (size_t l = 0; l < 3; ++l) s.rect(l, 10, 10, 30, 30);
    for (size_t l = 3; l < 10; ++l) { s.rect(l, 10, 10, 30, 30); s.rect(l, 14, 14, 26, 26, 0); }
    if (lid) for (size_t l = 10; l < 13; ++l) s.rect(l, 10, 10, 30, 30);
    return s;
}

} // namespace

TEST_CASE("Layer issues: floating island and its area threshold", "[sla_layers][sla_layer_issues]")
{
    Stack s(6);
    for (size_t l = 0; l < 5; ++l) s.rect(l, 5, 5, 15, 15);
    s.rect(5, 5, 5, 15, 15);   // supported continuation
    s.rect(5, 25, 25, 35, 35); // floating 10x10 island

    auto issues = run(s, only(true, false, false, false, false, false));
    REQUIRE(issues.size() == 1);
    CHECK(issues[0].type == LayerIssue::Type::Island);
    CHECK(issues[0].first_layer == 5);
    CHECK(issues[0].pixel_count == 100);
    CHECK(issues[0].bbox.min == Point(25, 25));
    CHECK(issues[0].bbox.max == Point(34, 34));

    auto cfg = only(true, false, false, false, false, false);
    cfg.island.required_area_to_process_check_px = 200; // island too small to be processed
    CHECK(run(s, cfg).empty());

    // With the same island supported from below there is no issue.
    Stack sup(6);
    for (size_t l = 0; l < 6; ++l) sup.rect(l, 5, 5, 15, 15);
    CHECK(run(sup, only(true, false, false, false, false, false)).empty());
}

TEST_CASE("Layer issues: overhang beyond the erode margin", "[sla_layers][sla_layer_issues]")
{
    Stack s(6);
    for (size_t l = 0; l < 5; ++l) s.rect(l, 5, 5, 15, 15);
    s.rect(5, 5, 5, 31, 15); // 16 px wide ledge on a 10 px post

    auto issues = run(s, only(false, true, false, false, false, false));
    REQUIRE(issues.size() == 1);
    CHECK(issues[0].type == LayerIssue::Type::Overhang);
    CHECK(issues[0].first_layer == 5);
    CHECK(issues[0].bbox.min.x() >= 15 + 2); // eroded by 2 px from the new area's edge

    // UVtools erodes inside the bounding rectangle of both layers with a Reflect101 border:
    // the ledge touches that rectangle on its right, top and bottom, so only its left edge
    // erodes and even a large erosion keeps it (same result as `UVtoolsCmd print-issues`).
    auto cfg = only(false, true, false, false, false, false);
    cfg.overhang.erode_iterations = 6;
    auto edge = run(s, cfg);
    REQUIRE(edge.size() == 1);
    CHECK(edge[0].bbox.min.x() >= 15 + 6);
    CHECK(edge[0].bbox.max.x() >= 30); // right edge (column 30) lies on the rectangle border: not eroded
}

TEST_CASE("Layer issues: overhang inside the layer rectangle erodes from both sides", "[sla_layers][sla_layer_issues]")
{
    // Two posts, then a bridge spanning them: the new 25 px wide span lies strictly inside the
    // union rectangle horizontally, so it erodes from the left and the right.
    Stack s(6);
    for (size_t l = 0; l < 5; ++l) { s.rect(l, 5, 5, 10, 30); s.rect(l, 36, 5, 40, 30); }
    s.rect(5, 5, 5, 40, 30);
    auto cfg = only(false, true, false, false, false, false);
    cfg.overhang.erode_iterations = 12; // centre column is 13 px from both posts: survives
    REQUIRE(run(s, cfg).size() == 1);
    cfg.overhang.erode_iterations = 13; // now every column is within reach of a post
    CHECK(run(s, cfg).empty());
}

TEST_CASE("Layer issues: empty layers are classified", "[sla_layers][sla_layer_issues]")
{
    Stack s(5);
    s.rect(1, 10, 10, 20, 20);
    s.rect(3, 10, 10, 20, 20);
    // layers 0 (starting), 2 (loose), 4 (ending) are blank.
    auto cfg = only(false, false, false, false, false, true);
    auto issues = run(s, cfg);
    REQUIRE(issues.size() == 3);
    CHECK(issues[0].first_layer == 0);
    CHECK(issues[1].first_layer == 2);
    CHECK(issues[2].first_layer == 4);

    cfg.empty_layer.ignore_starting = true;
    cfg.empty_layer.ignore_ending = true;
    issues = run(s, cfg);
    REQUIRE(issues.size() == 1);
    CHECK(issues[0].first_layer == 2);
}

TEST_CASE("Layer issues: touching bound and print height", "[sla_layers][sla_layer_issues]")
{
    Stack s(4);
    s.rect(0, 10, 10, 20, 20);
    s.rect(1, 0, 10, 8, 30); // touches the left border (UVtools scans rows bbox.top+margin..bbox.bottom-margin)
    auto issues = run(s, only(false, false, false, true, false, false));
    REQUIRE(issues.size() == 1);
    CHECK(issues[0].type == LayerIssue::Type::TouchingBound);
    CHECK(issues[0].first_layer == 1);
    CHECK(issues[0].bbox.min.x() == 0);
    CHECK(issues[0].pixel_count == 5 * 10); // margin of 5 px x 10 rows

    auto cfg = only(false, false, false, false, true, false);
    cfg.print_height.machine_height_mm = 0.25; // layers 2 (0.3) and 3 (0.4) are above
    issues = run(s, cfg);
    REQUIRE(issues.size() == 1);
    CHECK(issues[0].type == LayerIssue::Type::PrintHeight);
    CHECK(issues[0].first_layer == 2);
    CHECK(issues[0].last_layer == 3);
}

TEST_CASE("Layer issues: closed hollow box is a resin trap", "[sla_layers][sla_layer_issues]")
{
    Stack s = make_box(true);
    auto issues = run(s, only(false, false, true, false, false, false));
    REQUIRE(count_of(issues, LayerIssue::Type::ResinTrap) == 1);
    CHECK(count_of(issues, LayerIssue::Type::SuctionCup) == 0);
    const LayerIssue& t = issues[0];
    CHECK(t.first_layer == 3);
    CHECK(t.last_layer == 9);
    CHECK(t.parts.size() == 7);
    CHECK(t.pixel_count == 7 * 12 * 12);
}

TEST_CASE("Layer issues: box open at the top is a suction cup", "[sla_layers][sla_layer_issues]")
{
    Stack s = make_box(false);
    auto issues = run(s, only(false, false, true, false, false, false));
    CHECK(count_of(issues, LayerIssue::Type::ResinTrap) == 0);
    REQUIRE(count_of(issues, LayerIssue::Type::SuctionCup) == 1);
    CHECK(issues[0].first_layer == 3);
    CHECK(issues[0].last_layer == 9);

    auto cfg = only(false, false, true, false, false, false);
    cfg.resin_trap.required_height_to_consider_suction_cup_mm = 2.0; // 0.7 mm cup is too shallow
    CHECK(run(s, cfg).empty());
    cfg.resin_trap.required_height_to_consider_suction_cup_mm = 0.5;
    cfg.resin_trap.detect_suction_cups = false;
    CHECK(run(s, cfg).empty());
}

TEST_CASE("Layer issues: drain hole removes the resin trap", "[sla_layers][sla_layer_issues]")
{
    Stack s = make_box(true);
    s.rect(9, 18, 10, 22, 14, 0); // 4 px slot through the front wall of the last wall layer
    auto issues = run(s, only(false, false, true, false, false, false));
    CHECK(count_of(issues, LayerIssue::Type::ResinTrap) == 0);
}

TEST_CASE("Layer issues: cancellation and deterministic order", "[sla_layers][sla_layer_issues]")
{
    Stack s = make_box(true);
    for (size_t l = 0; l < 5; ++l) s.rect(l, 1, 1, 4, 4); // touching-bound and island noise
    s.rect(5, 33, 33, 38, 38);
    IssueDetectionConfig cfg;
    cfg.overhang.erode_iterations = 2;

    const auto a = run(s, cfg);
    const auto b = run(s, cfg);
    REQUIRE(a.size() == b.size());
    REQUIRE(!a.empty());
    for (size_t i = 0; i < a.size(); ++i) {
        CHECK(a[i].type == b[i].type);
        CHECK(a[i].first_layer == b[i].first_layer);
        CHECK(a[i].pixel_count == b[i].pixel_count);
        if (i > 0) CHECK(a[i - 1].type <= a[i].type);
    }

    std::atomic<int> polls{0};
    CHECK_THROWS_AS(detect_layer_issues(s.provider(), s.z, cfg, {}, [&] { return ++polls > 3; }), CanceledException);

    size_t last_done = 0, last_total = 0;
    detect_layer_issues(s.provider(), s.z, cfg, [&](size_t d, size_t t) { last_done = d; last_total = t; });
    CHECK(last_done == last_total);
}

// ---------------------------------------------------------------------------
// Resin trap / suction cup cases that were compared with UVtools (goal G6,
// tests/oracle/issues_vs_uvtools.py trap_* cases).
// ---------------------------------------------------------------------------

TEST_CASE("Layer issues: suction cup height rule tolerates Z noise like UVtools", "[sla_layers][sla_layer_issues]")
{
    // Cavity of five 0.1 mm layers (3..7) open towards the top: TotalHeight is exactly 0.5 mm in UVtools
    // (RoundHeight to 3 decimals), the Z values of float based archives carry ~1e-6 noise.
    Stack s(8);
    for (size_t l = 0; l < 3; ++l) s.rect(l, 10, 10, 30, 30);
    for (size_t l = 3; l < 8; ++l) { s.rect(l, 10, 10, 30, 30); s.rect(l, 14, 14, 26, 26, 0); }
    for (size_t i = 0; i < s.z.size(); ++i) s.z[i] += (i % 2 ? -1.5e-6 : 1.5e-6); // first cup layer: 0.1 - 3e-6 thick

    auto cfg = only(false, false, true, false, false, false);
    cfg.resin_trap.required_height_to_consider_suction_cup_mm = 0.5;
    CHECK(count_of(run(s, cfg), LayerIssue::Type::SuctionCup) == 1);
    cfg.resin_trap.required_height_to_consider_suction_cup_mm = 0.6; // one layer more than the cup has
    CHECK(count_of(run(s, cfg), LayerIssue::Type::SuctionCup) == 0);
}

TEST_CASE("Layer issues: two sealed cavities are two resin traps", "[sla_layers][sla_layer_issues]")
{
    Stack s(13);
    for (size_t l = 0; l < 3; ++l) s.rect(l, 10, 10, 30, 30);
    for (size_t l = 3; l < 10; ++l) { s.rect(l, 10, 10, 30, 30); s.rect(l, 12, 12, 19, 19, 0); }
    for (size_t l = 5; l < 10; ++l) s.rect(l, 21, 21, 28, 28, 0);
    for (size_t l = 10; l < 13; ++l) s.rect(l, 10, 10, 30, 30);

    const auto issues = run(s, only(false, false, true, false, false, false));
    REQUIRE(count_of(issues, LayerIssue::Type::ResinTrap) == 2);
    CHECK(issues[0].first_layer == 3);
    CHECK(issues[0].last_layer == 9);
    CHECK(issues[1].first_layer == 5);
    CHECK(issues[1].last_layer == 9);
}

TEST_CASE("Layer issues: cavity open towards the build plate is not a trap", "[sla_layers][sla_layer_issues]")
{
    // oracle case trap_open_bottom: the cavity starts on the first layer and is closed above.
    Stack s(10);
    for (size_t l = 0; l < 7; ++l) { s.rect(l, 10, 10, 30, 30); s.rect(l, 14, 14, 26, 26, 0); }
    for (size_t l = 7; l < 10; ++l) s.rect(l, 10, 10, 30, 30);
    CHECK(run(s, only(false, false, true, false, false, false)).empty());
}
