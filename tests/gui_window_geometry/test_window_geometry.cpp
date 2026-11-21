// SPDX-License-Identifier: AGPL-3.0-or-later
// Pure policy tests. The preceding native GUI RED is the actual regression
// reproducer; these tests do not reconstruct the old wx intersection algorithm.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <limits>

#include "slic3r/GUI/WindowGeometry.hpp"
#include "slic3r/GUI/LayerCanvasGeometry.hpp"
#include "slic3r/GUI/LayerRangeSelection.hpp"

using Slic3r::GUI::WindowGeometryMinimum;
using Slic3r::GUI::WindowGeometryRect;
using Slic3r::GUI::clamp_window_to_workarea;
using Slic3r::GUI::layer_canvas_pixel;

TEST_CASE("Layer range mode preserves the independent single layer and interval", "[sla_layer_range]")
{
    using namespace Slic3r::GUI;
    LayerRangeSelection selection(128);
    CHECK(selection.current() == 1);
    CHECK(selection.first() == 1);
    CHECK(selection.last() == 128);
    CHECK_FALSE(selection.range_mode());
    selection.set_bounds(2, 4);
    selection.set_current(17);
    selection.set_range_mode(true);
    CHECK(selection.current() == 17);
    CHECK(selection.first() == 2);
    CHECK(selection.last() == 4);
    selection.set_range_mode(false);
    selection.move_handle(LayerRangeHandle::Last, 23);
    CHECK(selection.current() == 23);
    CHECK(selection.first() == 2);
    CHECK(selection.last() == 4);
    selection.set_range_mode(true);
    CHECK(selection.current() == 23);
    CHECK(selection.first() == 2);
    CHECK(selection.last() == 4);
}

TEST_CASE("Layer range handles cannot cross and keyboard steps saturate", "[sla_layer_range]")
{
    using namespace Slic3r::GUI;
    LayerRangeSelection selection(128);
    selection.set_bounds(2, 4);
    selection.set_range_mode(true);
    selection.move_handle(LayerRangeHandle::First, 99);
    CHECK(selection.first() == 4);
    CHECK(selection.last() == 4);
    CHECK(selection.current() == 4);
    selection.move_handle(LayerRangeHandle::Last, -20);
    CHECK(selection.first() == 4);
    CHECK(selection.last() == 4);
    selection.step_handle(LayerRangeHandle::Last, std::numeric_limits<int>::max());
    CHECK(selection.last() == 128);
    selection.step_handle(LayerRangeHandle::First, std::numeric_limits<int>::min());
    CHECK(selection.first() == 1);
    selection.step_handle(LayerRangeHandle::First, 1);
    CHECK(selection.first() == 2);
    selection.step_handle(LayerRangeHandle::Last, -10);
    CHECK(selection.last() == 118);
    CHECK(selection.current() == 118);
}

TEST_CASE("Vertical layer positions increase upwards with exact endpoints", "[sla_layer_range]")
{
    using namespace Slic3r::GUI;
    CHECK(layer_range_offset(1, 6, 200) == 200);
    CHECK(layer_range_offset(2, 6, 200) == 160);
    CHECK(layer_range_offset(4, 6, 200) == 80);
    CHECK(layer_range_offset(6, 6, 200) == 0);
    CHECK(layer_range_value(200, 6, 200) == 1);
    CHECK(layer_range_value(160, 6, 200) == 2);
    CHECK(layer_range_value(80, 6, 200) == 4);
    CHECK(layer_range_value(0, 6, 200) == 6);
    CHECK(layer_range_value(-100, 6, 200) == 6);
    CHECK(layer_range_value(300, 6, 200) == 1);
    CHECK(layer_range_value(181, 6, 200) == 1);
    CHECK(layer_range_value(179, 6, 200) == 2);
    for (int layer = 1; layer <= 128; ++layer)
        CHECK(layer_range_value(layer_range_offset(layer, 128, 254), 128, 254) == layer);
}

TEST_CASE("Layer range supports one layer and large counts without overflow", "[sla_layer_range]")
{
    using namespace Slic3r::GUI;
    CHECK_THROWS_AS(LayerRangeSelection(0), std::invalid_argument);
    LayerRangeSelection one(1);
    one.set_bounds(-99, 99);
    one.set_range_mode(true);
    one.step_handle(LayerRangeHandle::First, 100);
    CHECK(one.first() == 1);
    CHECK(one.last() == 1);
    CHECK(one.current() == 1);
    CHECK(layer_range_offset(1, 1, 200) == 200);
    CHECK(layer_range_value(0, 1, 200) == 1);
    CHECK(layer_range_value(50, 128, 0) == 1);
    const int largest = std::numeric_limits<int>::max();
    CHECK(layer_range_offset(1, largest, largest) == largest);
    CHECK(layer_range_offset(largest, largest, largest) == 0);
    CHECK(layer_range_value(largest, largest, largest) == 1);
    CHECK(layer_range_value(0, largest, largest) == largest);
    LayerRangeSelection many(largest);
    many.set_bounds(largest, 1);
    CHECK(many.first() == 1);
    CHECK(many.last() == largest);
    many.set_current(largest);
    many.step_handle(LayerRangeHandle::Last, largest);
    CHECK(many.current() == largest);
}

TEST_CASE("Captured brush positions outside the canvas stay on the nearest edge", "[sla_layer_canvas]")
{
    // A 1000 x 720 mask shown at 25%. The old unsigned arithmetic mapped
    // (-1, 50) to (999, 200), and (50, -1) to (200, 719).
    CHECK(layer_canvas_pixel(-1, 250, 1000) == 0);
    CHECK(layer_canvas_pixel(50, 180, 720) == 200);
    CHECK(layer_canvas_pixel(50, 250, 1000) == 200);
    CHECK(layer_canvas_pixel(-1, 180, 720) == 0);
    CHECK(layer_canvas_pixel(-4, 250, 1000) == 0);
    CHECK(layer_canvas_pixel(-4, 180, 720) == 0);
    CHECK(layer_canvas_pixel(250, 250, 1000) == 999);
    CHECK(layer_canvas_pixel(180, 180, 720) == 719);
    CHECK(layer_canvas_pixel(400, 250, 1000) == 999);
    CHECK(layer_canvas_pixel(400, 180, 720) == 719);
}

TEST_CASE("Layer canvas mapping preserves pixels at fit and explicit zoom", "[sla_layer_canvas]")
{
    CHECK(layer_canvas_pixel(0, 333, 1000) == 0);
    CHECK(layer_canvas_pixel(1, 333, 1000) == 3);
    CHECK(layer_canvas_pixel(111, 333, 1000) == 333);
    CHECK(layer_canvas_pixel(332, 333, 1000) == 996);
    CHECK(layer_canvas_pixel(125, 250, 1000) == 500);
    CHECK(layer_canvas_pixel(90, 180, 720) == 360);
    CHECK(layer_canvas_pixel(127, 1000, 1000) == 127);
    CHECK(layer_canvas_pixel(255, 2000, 1000) == 127);
    CHECK(layer_canvas_pixel(256, 2000, 1000) == 128);
    CHECK(layer_canvas_pixel(511, 4000, 1000) == 127);
    CHECK(layer_canvas_pixel(3999, 4000, 1000) == 999);
}

TEST_CASE("Layer canvas mapping handles degenerate and extreme coordinates", "[sla_layer_canvas]")
{
    const int largest = std::numeric_limits<int>::max();
    CHECK(layer_canvas_pixel(std::numeric_limits<int>::min(), 250, 1000) == 0);
    CHECK(layer_canvas_pixel(largest, 250, 1000) == 999);
    CHECK(layer_canvas_pixel(largest / 2, largest, largest) == largest / 2);
    CHECK(layer_canvas_pixel(largest - 1, largest, largest) == largest - 1);
    CHECK(layer_canvas_pixel(largest, largest, largest) == largest - 1);
    CHECK(layer_canvas_pixel(200, 250, 1) == 0);
    CHECK(layer_canvas_pixel(200, 250, 0) == 0);
    CHECK(layer_canvas_pixel(200, 0, 1000) == 0);
    CHECK(layer_canvas_pixel(200, -1, 1000) == 0);
}

namespace {
void require_rect(WindowGeometryRect actual, WindowGeometryRect expected)
{
    CHECK(actual.x == expected.x);
    CHECK(actual.y == expected.y);
    CHECK(actual.width == expected.width);
    CHECK(actual.height == expected.height);
}

void require_contract(WindowGeometryRect original, WindowGeometryRect area,
                      WindowGeometryMinimum minimum, WindowGeometryRect result)
{
    const auto wide = [](int value) { return int64_t(value); };
    const int minimum_width = std::max(1, minimum.width);
    const int minimum_height = std::max(1, minimum.height);
    REQUIRE(area.width > 0);
    REQUIRE(area.height > 0);
    CHECK(result.width >= minimum_width);
    CHECK(result.height >= minimum_height);
    if (minimum_width <= area.width) {
        CHECK(result.width <= area.width);
        CHECK(result.x >= area.x);
        CHECK(wide(result.x) + result.width <= wide(area.x) + area.width);
    } else {
        CHECK(result.width == minimum_width);
        CHECK(result.x == area.x);
    }
    if (minimum_height <= area.height) {
        CHECK(result.height <= area.height);
        CHECK(result.y >= area.y);
        CHECK(wide(result.y) + result.height <= wide(area.y) + area.height);
    } else {
        CHECK(result.height == minimum_height);
        CHECK(result.y == area.y);
    }
    // An already valid window must not be needlessly moved or resized.
    const bool already_valid = original.width >= minimum_width && original.height >= minimum_height &&
        original.x >= area.x && original.y >= area.y &&
        wide(original.x) + original.width <= wide(area.x) + area.width &&
        wide(original.y) + original.height <= wide(area.y) + area.height;
    if (already_valid)
        require_rect(result, original);
    require_rect(clamp_window_to_workarea(result, area, minimum), result);
}
} // namespace

TEST_CASE("Window recovery honors the native minimum before clamping final bounds", "[sla_window_geometry]")
{
    struct Example {
        const char *reason;
        WindowGeometryRect window;
        WindowGeometryRect area;
        WindowGeometryMinimum minimum;
        WindowGeometryRect expected;
    };
    const Example cases[] = {
        {"actual partially visible native regression", {1200,100,912,588}, {0,0,1280,800}, {912,588}, {368,100,912,588}},
        {"valid ordinary window", {100,100,1000,650}, {0,0,1280,800}, {912,588}, {100,100,1000,650}},
        {"exact full workarea", {0,0,1280,800}, {0,0,1280,800}, {912,588}, {0,0,1280,800}},
        {"bottom recovery is independent of x", {100,400,912,588}, {0,0,1280,800}, {912,588}, {100,212,912,588}},
        {"ordinary saved oversize can shrink", {100,100,1500,900}, {0,0,1280,800}, {912,588}, {0,0,1280,800}},
        {"true minimum cannot fit and must stay reachable", {1200,100,1500,900}, {0,0,1280,800}, {1600,1000}, {0,0,1600,1000}},
        {"valid negative monitor coordinates", {-1800,40,912,588}, {-1920,0,1920,1080}, {912,588}, {-1800,40,912,588}},
        {"negative monitor overflow", {-2300,-900,912,588}, {-1920,-1080,1920,1080}, {912,588}, {-1920,-900,912,588}},
        {"client workarea excludes taskbar", {100,0,912,588}, {0,30,1280,770}, {912,588}, {100,30,912,588}},
        {"corrupt requested dimensions use the actual minimum", {42,42,0,-1}, {0,0,1280,800}, {912,588}, {42,42,912,588}},
        {"unspecified native minimum is defensive", {0,0,0,-1}, {0,0,1280,800}, {-1,-1}, {0,0,1,1}},
        {"only one minimum axis cannot fit", {1200,400,1700,650}, {0,0,1280,800}, {1600,588}, {0,150,1600,650}},
    };
    for (const auto &example : cases) {
        CAPTURE(example.reason);
        const auto result = clamp_window_to_workarea(example.window, example.area, example.minimum);
        require_rect(result, example.expected);
        require_contract(example.window, example.area, example.minimum, result);
    }
}

TEST_CASE("Invalid display workareas do not destructively alter stored geometry", "[sla_window_geometry]")
{
    const WindowGeometryRect window{1200,-900,1500,900};
    const WindowGeometryRect invalid[] = {
        {0,0,0,800}, {0,0,1280,0}, {100,-100,-1,800},
        {-1920,-1080,1920,-1},
        {0,0,std::numeric_limits<int>::min(),std::numeric_limits<int>::max()},
    };
    for (const auto &area : invalid) {
        CAPTURE(area.x, area.y, area.width, area.height);
        require_rect(clamp_window_to_workarea(window, area, {912,588}), window);
    }
}

TEST_CASE("Extreme monitor endpoints use widened bounds and representable origins", "[sla_window_geometry]")
{
    const int lo = std::numeric_limits<int>::min();
    const int hi = std::numeric_limits<int>::max();
    struct Example { WindowGeometryRect window, area, expected; WindowGeometryMinimum minimum; };
    const Example cases[] = {
        {{hi,hi,912,588}, {lo,lo,hi,hi}, {-913,-589,912,588}, {912,588}},
        {{lo,lo,912,588}, {hi-100,hi-100,1280,800}, {hi-100,hi-100,912,588}, {912,588}},
        {{hi,hi,912,588}, {hi-100,hi-100,1280,800}, {hi,hi,912,588}, {912,588}},
        {{lo,hi,hi,hi}, {lo,lo,hi,hi}, {lo,lo,hi,hi}, {1,1}},
        {{hi,lo,912,588}, {lo,hi,1,1}, {lo,hi,912,588}, {912,588}},
    };
    for (const auto &example : cases) {
        CAPTURE(example.window.x, example.window.y, example.area.x, example.area.y);
        const auto result = clamp_window_to_workarea(example.window, example.area, example.minimum);
        require_rect(result, example.expected);
        require_contract(example.window, example.area, example.minimum, result);
    }
}

TEST_CASE("Window policy remains contained and idempotent across mixed monitor and minimum domains", "[sla_window_geometry]")
{
    const WindowGeometryRect windows[] = {
        {1200,100,912,588}, {-2300,-900,912,588}, {100,100,1500,900},
        {42,42,0,-1}, {0,0,1,1}, {100,100,1000,650},
    };
    const WindowGeometryRect areas[] = {
        {0,0,1280,800}, {-1920,0,1920,1080}, {-1920,-1080,1920,1080}, {100,30,800,600},
    };
    const WindowGeometryMinimum minimums[] = {{-1,-1}, {912,588}, {1600,1000}, {1,1}};
    for (const auto &window : windows)
        for (const auto &area : areas)
            for (const auto &minimum : minimums) {
                CAPTURE(window.x, window.y, window.width, window.height,
                        area.x, area.y, area.width, area.height, minimum.width, minimum.height);
                require_contract(window, area, minimum, clamp_window_to_workarea(window, area, minimum));
            }
}
