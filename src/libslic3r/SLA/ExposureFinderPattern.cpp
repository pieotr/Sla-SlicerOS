// SPDX-License-Identifier: AGPL-3.0-or-later
// Native port of UVtools OperationCalibrateExposureFinder.GetLayers/Execute.
// Original algorithm: UVtools contributors, AGPL-3.0; see project LICENSE.
#include "ExposureFinderPattern.hpp"
#include "ExposureFinderDraft.hpp"
#include "HersheyFontData.hpp"
#include "RasterRounding.hpp"
#include "libslic3r/SLAPrint.hpp"
#include "CalibrationRecipe.hpp"
#include <algorithm>
#include <boost/multiprecision/cpp_dec_float.hpp>
#include <charconv>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <opencv2/imgproc.hpp>
#include <sstream>

namespace Slic3r::sla {
namespace {
using PatternDecimal = boost::multiprecision::cpp_dec_float_50;
PatternDecimal decimal(double value) {
    char buffer[64];
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
    if (result.ec != std::errc{})
        throw RuntimeError("Cannot encode ExposureFinder dimension");
    return PatternDecimal(std::string(buffer, result.ptr));
}
PatternDecimal rounded(const PatternDecimal &value, unsigned scale) {
    const PatternDecimal scaled = value * scale;
    PatternDecimal integer = floor(scaled), fraction = scaled - integer;
    if (abs(fraction - PatternDecimal("0.5")) < PatternDecimal("1e-40"))
        fraction = PatternDecimal("0.5");
    if (fraction > PatternDecimal("0.5") ||
        (fraction == PatternDecimal("0.5") && fmod(integer, PatternDecimal(2)) != 0))
        ++integer;
    return integer * (scale == 100 ? PatternDecimal(".01") : PatternDecimal(".001"));
}
cv::Mat view(png::ImageGreyscale &image) {
    return cv::Mat(int(image.rows), int(image.cols), CV_8UC1, image.buf.data());
}
// OpenCV 5 maps the legacy Hershey API to new fonts. Keep the classical
// stroke data/metrics in our source so calibration footprints remain stable.
const int *font_data(int face) {
    if ((face & ~16) < 0 || (face & ~16) > 7)
        throw RuntimeError("Invalid calibration font");
    return hershey::fonts[(face & 15) + ((face & 16) ? 8 : 0)];
}
const char *glyph(const std::string &text, size_t &index, int face) {
    unsigned c = static_cast<unsigned char>(text[index]);
    if (face == 3 && c >= 128) {
        const unsigned next = index + 1 < text.size() ? static_cast<unsigned char>(text[index + 1]) : 0;
        if (c == 0xd0 && next >= 0x90 && next <= 0xbf) {
            ++index;
            c = 127 + next - 0x90;
        } else if (c == 0xd1 && next >= 0x80 && next <= 0x8f) {
            ++index;
            c = 175 + next - 0x80;
        } else {
            while (index + 1 < text.size() && (static_cast<unsigned char>(text[index + 1]) & 0xc0) == 0x80)
                ++index;
            c = '?';
        }
    }
    if (c < 32 || c > (face == 3 ? 190u : 126u))
        c = '?';
    return hershey::glyphs[font_data(face)[c - 32 + 1]];
}
cv::Size native_text_size(const std::string &text, int face, double scale, int thickness, int *baseline) {
    const int *font = font_data(face);
    double width = 0.;
    for (size_t i = 0; i < text.size(); ++i) {
        const char *p = glyph(text, i, face);
        width += (int(p[1]) - int(p[0])) * scale;
    }
    if (baseline)
        *baseline = int(std::lrint((font[0] & 15) * scale + thickness * .5));
    return {int(std::lrint(width + thickness)),
            int(std::lrint((((font[0] >> 4) & 15) + (font[0] & 15)) * scale + (thickness + 1) / 2))};
}
// Classical OpenCV fixed-point Line2 for one-pixel, non-AA Hershey strokes.
// OpenCV 5 changed subpixel rounding in this path; calibration glyphs must
// retain the original endpoint and DDA convention at fractional font scales.
void legacy_thin_line(cv::Mat &image, cv::Point first, cv::Point last, uint8_t color) {
    constexpr int64_t unit = int64_t(1) << 16;
    cv::Point2l a(first.x, first.y), b(last.x, last.y);
    if (!cv::clipLine(cv::Size2l(int64_t(image.cols) * unit, int64_t(image.rows) * unit), a, b))
        return;
    const auto put = [&](int64_t x, int64_t y) {
        if (x >= 0 && x < image.cols && y >= 0 && y < image.rows)
            image.at<uint8_t>(int(y), int(x)) = color;
    };
    const int64_t dx = b.x - a.x, dy = b.y - a.y, ax = std::abs(dx), ay = std::abs(dy);
    if (ax > ay) {
        int64_t signed_dy = dy;
        if (dx < 0) {
            std::swap(a, b);
            signed_dy = -signed_dy;
        }
        const int64_t step = signed_dy * unit / (ax | 1), count = (b.x - a.x) >> 16;
        put((b.x + unit / 2) >> 16, (b.y + unit / 2) >> 16);
        int64_t x = (a.x + unit / 2) >> 16, y = a.y + unit / 2;
        for (int64_t i = 0; i <= count; ++i, ++x, y += step)
            put(x, y >> 16);
    } else {
        int64_t signed_dx = dx;
        if (dy < 0) {
            std::swap(a, b);
            signed_dx = -signed_dx;
        }
        const int64_t step = signed_dx * unit / (ay | 1), count = (b.y - a.y) >> 16;
        put((b.x + unit / 2) >> 16, (b.y + unit / 2) >> 16);
        int64_t x = a.x + unit / 2, y = (a.y + unit / 2) >> 16;
        for (int64_t i = 0; i <= count; ++i, x += step, ++y)
            put(x >> 16, y);
    }
}
void native_put_text(cv::Mat &image, const std::string &text, cv::Point origin, int face, double scale,
                     cv::Scalar color, int thickness, int line_type) {
    constexpr int64_t unit = int64_t(1) << 16;
    const int64_t factor = std::lrint(scale * unit);
    int64_t x = int64_t(origin.x) * unit,
            y = int64_t(origin.y) * unit - int64_t(font_data(face)[0] & 15) * factor;
    for (size_t i = 0; i < text.size(); ++i) {
        const char *p = glyph(text, i, face);
        x -= int64_t(p[0] - 'R') * factor;
        const int64_t advance = int64_t(p[1] - 'R') * factor;
        p += 2;
        std::vector<cv::Point> stroke;
        const auto flush = [&] {
            if (stroke.size() > 1) {
                if (thickness == 1 && line_type == cv::LINE_8) {
                    for (size_t segment = 1; segment < stroke.size(); ++segment)
                        legacy_thin_line(image, stroke[segment - 1], stroke[segment], uint8_t(color[0]));
                } else
                    cv::polylines(image, std::vector<std::vector<cv::Point>>{stroke}, false, color, thickness,
                                  line_type, 16);
            }
            stroke.clear();
        };
        while (*p) {
            if (*p == ' ') {
                flush();
                ++p;
                continue;
            }
            const int64_t px = x + int64_t(p[0] - 'R') * factor, py = y + int64_t(p[1] - 'R') * factor;
            if (px < std::numeric_limits<int>::min() || px > std::numeric_limits<int>::max() ||
                py < std::numeric_limits<int>::min() || py > std::numeric_limits<int>::max())
                throw RuntimeError("ExposureFinder text exceeds fixed-point coordinate limits");
            stroke.emplace_back(int(px), int(py));
            p += 2;
        }
        flush();
        x += advance;
    }
}
std::vector<std::string> lines(std::string text) {
    while (!text.empty() && (text.back() == ' ' || text.back() == '\n' || text.back() == '\r'))
        text.pop_back();
    std::vector<std::string> result;
    std::stringstream stream(text);
    std::string line;
    while (std::getline(stream, line, '\n')) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        result.push_back(line);
    }
    if (result.empty())
        result.emplace_back();
    return result;
}
cv::Size text_size(const std::string &text, int font, double scale, int thickness, int gap = 0) {
    int baseline = 0;
    const auto size = native_text_size(text, font, scale, thickness, &baseline);
    const auto split = lines(text);
    if (split.size() == 1)
        return size;
    int width = 0;
    for (const auto &line : split)
        width = std::max(width, native_text_size(line, font, scale, thickness, &baseline).width);
    return {width, int(split.size()) * (size.height + size.height / 3 + gap) - (size.height / 3 + gap)};
}
void put_text(cv::Mat &image, const std::string &text, cv::Point origin, int font, double scale, int color,
              int thickness, int line_type) {
    const auto split = lines(text);
    int baseline = 0;
    const auto one = native_text_size(text, font, scale, thickness, &baseline);
    for (size_t i = 0; i < split.size(); ++i)
        if (!split[i].empty())
            native_put_text(image, split[i],
                            {origin.x, origin.y + int(i) * (one.height + one.height / 3 + 10)}, font, scale,
                            cv::Scalar(color), thickness, line_type);
}
void rectangle(cv::Mat &image, int x, int y, int w, int h, int color, int thickness, int line_type) {
    // Emgu's Rectangle overload passes the inclusive bottom-right corner;
    // cv::Rect's C++ overload instead subtracts one pixel. Preserve the source
    // convention, including one-pixel bars whose supplied height is zero.
    if (w >= 0 && h >= 0)
        cv::rectangle(image, cv::Point(x, y), cv::Point(x + w, y + h), cv::Scalar(color), thickness,
                      line_type);
}
void validate(const ExposureFinderPatternParameters &p) {
    for (double v : {p.feature_margin_mm, p.bar_length_mm, p.bar_spacing_mm, p.text_scale})
        if (!std::isfinite(v) || v < 0 || v > 1000)
            throw RuntimeError("Invalid ExposureFinder pattern dimension");
    if (p.text_scale <= 0 || p.text_scale > 10 || p.text_thickness < 1 || p.text_thickness > 20 ||
        (p.font_face & ~16) < 0 || (p.font_face & ~16) > 7 || p.text.size() > 4096 ||
        p.text.find('\0') != std::string::npos || p.staircase_thickness > 4096 || p.bar_fence < 0 ||
        p.bar_fence > 255 || p.bullseye_fence < 0 || p.bullseye_fence > 255)
        throw RuntimeError("Invalid ExposureFinder pattern text or fence");
    for (int v : {p.bar_splitter, p.bar_fence_offset, p.bullseye_fence_offset, p.triangle_tip_offset})
        if (v < -127 || v > 127)
            throw RuntimeError("Invalid ExposureFinder pattern offset");
    for (const auto *list : {&p.holes, &p.bars}) {
        if (list->size() > 500 || !std::is_sorted(list->begin(), list->end()) ||
            std::adjacent_find(list->begin(), list->end()) != list->end())
            throw RuntimeError("ExposureFinder feature sizes must be sorted and unique");
        for (unsigned value : *list)
            if (!value || value > 500)
                throw RuntimeError("ExposureFinder feature size outside 1-500 pixels");
    }
    unsigned last = 0;
    if (p.bullseyes.size() > 500)
        throw RuntimeError("Too many ExposureFinder bullseyes");
    for (const auto &ring : p.bullseyes) {
        if (!ring[0] || ring[0] > 500 || !ring[1] || ring[1] > 500 || ring[0] <= last)
            throw RuntimeError("Invalid ExposureFinder bullseye list");
        last = ring[0];
    }
    if (p.holes.empty() && p.bars.empty() && p.bullseyes.empty() && p.text.empty())
        throw RuntimeError("Enable an ExposureFinder pattern feature");
}
} // namespace
ExposureFinderPatternParameters normalize_exposure_finder_pattern_parameters(const ExposureFinderPatternParameters &parameters) {
    if(parameters.text.size()>4096 || parameters.holes.size()>500 || parameters.bars.size()>500 || parameters.bullseyes.size()>500)
        throw RuntimeError("ExposureFinder pattern input exceeds its budget");
    ScopedNearestRounding rounding;
    auto p=parameters;validate(p);
    p.feature_margin_mm=rounded(decimal(p.feature_margin_mm),100).convert_to<double>();
    p.text_scale=std::nearbyint(p.text_scale*100.)/100.;validate(p);return p;
}
void ExposureFinderGenerationSettings::validate() const {
    (void)normalize_exposure_finder_pattern_parameters(pattern);
    if(bottom_erosion>255 || chamfer_layers>255 || layout.cell_columns>32768 || layout.cell_rows>32768)
        throw RuntimeError("Invalid ExposureFinder generation finishing or footprint");
    for(double v:{layout.margin_x_mm,layout.margin_y_mm,layout.gap_x_mm,layout.gap_y_mm})
        if(!std::isfinite(v) || v<0 || v>1000000.)throw RuntimeError("Invalid ExposureFinder layout spacing");
}
ExposureFinderPattern make_exposure_finder_pattern(const LayerRasterGeometry &geometry,
                                                   const ExposureFinderPatternParameters &parameters,
                                                   const std::function<void()> &checkpoint) {
    if (parameters.text.size() > 4096 || parameters.holes.size() > 500 || parameters.bars.size() > 500 ||
        parameters.bullseyes.size() > 500)
        throw RuntimeError("ExposureFinder pattern input exceeds its budget");
    const auto g = geometry;
    auto p = normalize_exposure_finder_pattern_parameters(parameters);
    const auto callback = checkpoint;
    const auto check = [&] {
        if (callback)
            callback();
    };
    check();
    ScopedNearestRounding rounding;
    g.validate();
    validate(p);
    const PatternDecimal xppmm = rounded(PatternDecimal(g.columns) / decimal(g.width_mm), 1000);
    const PatternDecimal yppmm = rounded(PatternDecimal(g.rows) / decimal(g.height_mm), 1000);
    const auto px = [](const PatternDecimal &value) {
        if (value < 0 || value > 32768)
            throw RuntimeError("ExposureFinder pattern exceeds its dimension budget");
        return floor(value).convert_to<int>();
    };
    const int mx = px(xppmm * decimal(p.feature_margin_mm)), my = px(yppmm * decimal(p.feature_margin_mm));
    const int stair = int(p.staircase_thickness), line = p.antialias ? cv::LINE_AA : cv::LINE_8;
    const auto marking = text_size("100u\n20.00s\n3.00s", p.font_face, p.text_scale, p.text_thickness, 10);
    const auto text =
        p.text.empty() ? cv::Size{} : text_size(p.text, p.font_face, p.text_scale, p.text_thickness);
    const int hole_width =
        p.holes.empty() ? 0 : std::max(mx * 2 + int(p.holes.back()), marking.width + mx * 2);
    int holes_height = 0;
    for (auto v : p.holes)
        holes_height += v;
    if (!p.holes.empty())
        holes_height += px(PatternDecimal(p.holes.size() - 1) * decimal(p.feature_margin_mm) * yppmm);
    int bars_height = 0;
    for (auto v : p.bars)
        bars_height += v;
    if (!p.bars.empty()) {
        bars_height += px(PatternDecimal(p.bars.size() + 1) * decimal(p.bar_spacing_mm) * yppmm);
        if (p.bar_fence)
            bars_height = std::max(bars_height, bars_height + p.bar_fence * 2 + p.bar_fence_offset * 2);
    }
    const int diameter = p.bullseyes.empty() ? 0 : int(p.bullseyes.back()[0] + p.bullseyes.back()[1] / 2);
    const int panel = p.bullseyes.empty()
                          ? 0
                          : std::max(diameter, diameter + p.bullseye_fence + p.bullseye_fence_offset * 2),
              radius = diameter / 2;
    int left = stair + my + std::max(bars_height, text.width) + panel,
        right = stair + holes_height + marking.height + my * 2;
    if (bars_height > 0 || text.width > 0)
        left += my;
    const int bar_length = px(decimal(p.bar_length_mm) * xppmm),
              bar_spacing = px(decimal(p.bar_spacing_mm) * yppmm);
    int bar_width = 0, width = mx, height = 60 + my;
    if (!p.bars.empty()) {
        if (bar_length < 2 || bar_spacing < 2)
            throw RuntimeError("ExposureFinder bars require at least two pixels in length and spacing");
        bar_width = bar_length * 2 + p.bar_splitter;
        if (p.bar_fence)
            bar_width = std::max(bar_width, bar_width + p.bar_fence * 2 + p.bar_fence_offset * 2);
        width += bar_width + mx;
    }
    if (text.area())
        width += text.height + mx;
    const int by = left - panel / 2;
    cv::Point positive(0, int(by - marking.height / 3.5));
    width = std::max(width, marking.width + (p.bullseyes.empty() ? mx * 2 : panel + mx * 3));
    left += my + 24;
    positive.x = p.bullseyes.empty() ? width / 2 - marking.width / 2 : mx;
    const int bx = int(width / 1.5);
    if (hole_width)
        width += mx + int(p.holes.back());
    const int negative_width = width;
    width += hole_width;
    height += std::max(left, right + 10);
    if (width <= 0 || height <= 0 || width > 32768 || height > 32768 ||
        uint64_t(width) * height > (uint64_t(128) << 20))
        throw RuntimeError("ExposureFinder pattern exceeds its raster budget");
    ExposureFinderPattern result;
    result.parameters = p;
    result.base = {std::vector<uint8_t>(size_t(width) * height, 255), size_t(height), size_t(width)};
    result.features = {std::vector<uint8_t>(size_t(width) * height, 0), size_t(height), size_t(width)};
    result.positive_marking = {positive.x, positive.y};
    result.staircase_width = width - hole_width;
    if(!p.holes.empty())
        result.negative_marking = {width - hole_width / 2 - marking.width / 3, height - my - marking.height};
    auto base = view(result.base), features = view(result.features);
    rectangle(features, width - hole_width, 0, width, height, 255, -1, line);
    check();
    // The reference compensates holes for the printer's rounded physical pitch.
    const float pitch_x = float(std::nearbyint(double(float(g.width_mm) / float(g.columns)) * 10000) / 10000);
    const float pitch_y = float(std::nearbyint(double(float(g.height_mm) / float(g.rows)) * 10000) / 10000);
    const auto hole = [&](cv::Mat &image, int x, int y, unsigned d, int color, bool square) {
        if (d == 1) {
            if (x >= 0 && x < width && y >= 0 && y < height)
                image.at<uint8_t>(y, x) = uint8_t(color);
            return;
        }
        if (square) {
            rectangle(image, x, y, int(d) - 1, int(d) - 1, color, -1, line);
            return;
        }
        const int r = int(d) / 2;
        const bool pitch_valid = pitch_x > 0 && pitch_y > 0;
        const int rx = pitch_valid && pitch_y > pitch_x ? int(r * (pitch_y / pitch_x)) : r,
                  ry = pitch_valid && pitch_x > pitch_y ? int(r * (pitch_x / pitch_y)) : r;
        if (rx == ry)
            cv::circle(image, {x, y}, rx, cv::Scalar(color), -1, line);
        else
            cv::ellipse(image, {x, y}, {rx, ry}, 0, 0, 360, cv::Scalar(color), -1, line);
    };
    for (int index = 0; index < 2; ++index) {
        auto &image = index ? features : base;
        int y = my + stair;
        for (unsigned d : p.holes) {
            check();
            const int r = int(d) / 2;
            const bool square = p.square_holes || d < 6;
            int x = width - hole_width - mx - int(p.holes.back()) / 2;
            if (square)
                x -= r;
            else
                y += r;
            if (index)
                hole(image, x, y, d, 255, square);
            x = width - hole_width / 2 - (square ? r : 0);
            hole(image, x, y, d, 0, square);
            y += my + (square ? int(d) : r);
        }
    }
    if (!p.bars.empty()) {
        const int sy = stair + my, sx = mx;
        int x = sx + p.bar_fence / 2 + p.bar_fence_offset, y = sy + p.bar_fence / 2 + p.bar_fence_offset;
        for (unsigned bar : p.bars) {
            check();
            rectangle(features, x, y, bar_length - 1, bar_spacing - 1, 255, -1, line);
            y += bar_spacing;
            rectangle(features, x + bar_length + p.bar_splitter, y, bar_length - 1, int(bar) - 1, 255, -1,
                      line);
            y += bar;
        }
        rectangle(features, x, y, bar_length - 1, bar_spacing - 1, 255, -1, line);
        y += bar_spacing;
        if (p.bar_fence)
            rectangle(features, sx - 1, sy - 1, bar_width - p.bar_fence + 1,
                      y - sy + p.bar_fence / 2 + p.bar_fence_offset + 1, 255, p.bar_fence, line);
    }
    if (text.area()) {
        check();
        cv::Mat rotated;
        cv::rotate(features, rotated, cv::ROTATE_90_COUNTERCLOCKWISE);
        native_put_text(rotated, p.text,
                        {stair + my, rotated.rows - bar_width - mx * (bar_width > 0 ? 2 : 1)}, p.font_face,
                        p.text_scale, cv::Scalar(255), p.text_thickness, line);
        cv::rotate(rotated, features, cv::ROTATE_90_CLOCKWISE);
    }
    if (!p.bullseyes.empty()) {
        for (const auto &ring : p.bullseyes) {
            check();
            cv::circle(features, {bx, by}, int(ring[0]) / 2, cv::Scalar(255), int(ring[1]), line);
        }
        const auto invert = [&](cv::Rect roi) {
            if ((roi & cv::Rect(0, 0, width, height)) != roi)
                throw RuntimeError("ExposureFinder bullseye quadrants exceed pattern bounds");
            auto part = features(roi);
            cv::bitwise_not(part, part);
        };
        if (p.invert_bullseye_quadrants) {
            invert({bx, by - radius - 5, radius + 6, radius + 5});
            invert({bx - radius - 5, by, radius + 5, radius + 6});
        }
        if (p.bullseye_fence)
            rectangle(features, bx - radius - 5 - p.bullseye_fence_offset - p.bullseye_fence / 2,
                      by - radius - 5 - p.bullseye_fence_offset - p.bullseye_fence / 2,
                      diameter + 10 + p.bullseye_fence_offset * 2 + p.bullseye_fence,
                      diameter + 10 + p.bullseye_fence_offset * 2 + p.bullseye_fence, 255, p.bullseye_fence,
                      line);
    }
    if (negative_width >= 200 && p.counter_triangles) {
        check();
        const int x = mx, th = 79, tw = (negative_width - x - mx) / 2, q = tw / 4, y = height - my - th + 1,
                  half = y + th / 2, end = height - my + 1;
        if (tw > 5) {
            const std::vector<std::vector<cv::Point>> triangles{
                {{x, y}, {x + tw, half}, {x, end}},
                {{x + tw * 2, y}, {x + tw, half}, {x + tw * 2, end}},
                {{x + tw - q, y}, {x + tw + q, y}, {x + tw, half - p.triangle_tip_offset}},
                {{x + tw - q, end}, {x + tw + q, end}, {x + tw, half + p.triangle_tip_offset}}};
            for (const auto &triangle : triangles) {
                check();
                cv::fillPoly(features, std::vector<std::vector<cv::Point>>{triangle}, cv::Scalar(255), line);
            }
            if (p.triangle_fence)
                rectangle(features, x, y, tw * 2, th, 255, 8, line);
        }
    }
    check();
    return result;
}
ExposureFinderCellRenderer make_exposure_finder_pattern_renderer(const ExposureFinderPattern &pattern,
                                                                 ExposureFinderCellParameters parameters,
                                                                 bool multiple_brightness) {
    validate(pattern.parameters);
    parameters.staircase_width = pattern.parameters.staircase_thickness ? pattern.staircase_width : 0;
    parameters.staircase_thickness = pattern.parameters.staircase_thickness;
    const int staircase_line = pattern.parameters.antialias ? cv::LINE_AA : cv::LINE_8;
    const auto finish = make_exposure_finder_cell_renderer(
        pattern.base, pattern.features, parameters,
        [staircase_line](png::ImageGreyscale &image, size_t first, size_t width, size_t thickness,
                         const std::function<void()> &check) {
            if (check)
                check();
            auto mat = view(image);
            rectangle(mat, int(first), 0, int(width), int(thickness), 255, -1, staircase_line);
            if (check)
                check();
        });
    const auto p = pattern.parameters;
    const auto positive = pattern.positive_marking, negative = pattern.negative_marking;
    return [finish, p, positive, negative, multiple_brightness](const ExposureFinderPart &part,
                                                                const ExposureFinderExposure &record,
                                                                const std::function<void()> &check) {
        auto image = finish(part, record, check);
        auto mat = view(image);
        ScopedNearestRounding rounding;
        std::ostringstream label;
        label.imbue(std::locale::classic());
        label << std::lround(part.layer_height * 1000) << "u\n"
              << std::fixed << std::setprecision(2) << part.label_bottom_exposure << "s\n"
              << part.label_normal_exposure << "s";
        const int line = p.antialias ? cv::LINE_AA : cv::LINE_8;
        if (check)
            check();
        put_text(mat, label.str(), {positive[0], positive[1]}, p.font_face, p.text_scale, 255,
                 p.text_thickness, line);
        if (!p.holes.empty())
            put_text(mat, label.str(), {negative[0], negative[1]}, p.font_face, p.text_scale, 0,
                     p.text_thickness, line);
        if (multiple_brightness)
            native_put_text(mat, std::to_string(part.requested_brightness), {mat.cols / 3, 35},
                            cv::FONT_HERSHEY_DUPLEX, .8, cv::Scalar(255), 2, line);
        if (check)
            check();
        return image;
    };
}
std::unique_ptr<SLAPrint>
create_exposure_finder_calibration(const DynamicPrintConfig &configuration,
                                   const ExposureFinderGenerationParameters &parameters,
                                   const LayerProgress &progress) {
    if (parameters.recipe.rows.size() > 100000 || parameters.pattern.text.size() > 4096 ||
        parameters.pattern.holes.size() > 500 || parameters.pattern.bars.size() > 500 ||
        parameters.pattern.bullseyes.size() > 500 ||
        (parameters.recipe.schedule && parameters.recipe.schedule->brightness.size() > 256))
        throw RuntimeError("ExposureFinder generation input exceeds its budget");
    const auto request = parameters;
    DynamicPrintConfig config;
    config.apply(SLAFullPrintConfig{});
    config.apply(configuration);
    const auto callback = progress;
    ScopedNearestRounding rounding;
    size_t phase = 0;
    const auto check = [&] { check_layer_progress(callback, phase, 100); };
    const auto advance = [&](size_t start, size_t span, size_t done, size_t total) {
        phase = std::max(phase, start + (total ? std::min(done, total) * span / total : 0));
        check();
    };
    check();
    ExposureFinderGenerationSettings{request.pattern,request.layout,request.bottom_erosion,request.chamfer_layers}.validate();
    if (config.option("printer_technology")->getInt() != ptSLA || !request.recipe.schedule)
        throw RuntimeError("ExposureFinder requires an SLA printer and an explicit schedule");
    const int columns = config.opt_int("display_pixels_x"), rows = config.opt_int("display_pixels_y");
    if (columns <= 0 || rows <= 0 || columns > 32768 || rows > 32768 ||
        uint64_t(columns) * rows > (uint64_t(128) << 20))
        throw RuntimeError("Invalid ExposureFinder printer resolution");
    LayerRasterGeometry geometry{size_t(columns), size_t(rows), config.opt_float("display_width"),
                                 config.opt_float("display_height")};
    if (config.option("display_orientation")->getInt() == sladoPortrait) {
        std::swap(geometry.columns, geometry.rows);
        std::swap(geometry.width_mm, geometry.height_mm);
    }
    geometry.validate();
    const auto schedule = plan_exposure_finder_schedule(request.recipe, *request.recipe.schedule,
                                                        [&](size_t done, size_t total) {
                                                            advance(0, 10, done, total);
                                                            return true;
                                                        });
    if (!std::isfinite(config.opt_float("max_print_height")) || config.opt_float("max_print_height") <= 0 ||
        schedule.exposures.back().position_z > config.opt_float("max_print_height"))
        throw RuntimeError("ExposureFinder exceeds the printer build height");
    auto validation_config=config;
    validation_config.set("layer_height",schedule.exposures.front().layer_height);
    validation_config.set("sla_bottom_layers",int(schedule.bottom_records));validation_config.set("sla_transition_layers",0);
    const auto nominal=validate_exposure_finder_configuration(validation_config);
    const auto planned_rows=make_exposure_finder_rows(nominal,schedule,[&](size_t,size_t){check();return true;});
    phase = 10;
    const auto pattern = make_exposure_finder_pattern(geometry, request.pattern, check);
    phase = 20;
    auto layout_parameters = request.layout;
    if ((layout_parameters.cell_columns && layout_parameters.cell_columns != pattern.base.cols) ||
        (layout_parameters.cell_rows && layout_parameters.cell_rows != pattern.base.rows))
        throw RuntimeError("ExposureFinder layout cell differs from the generated pattern");
    layout_parameters.cell_columns = pattern.base.cols;
    layout_parameters.cell_rows = pattern.base.rows;
    layout_parameters.mirror_x = layout_parameters.mirror_x != config.opt_bool("display_mirror_x");
    layout_parameters.mirror_y = layout_parameters.mirror_y != config.opt_bool("display_mirror_y");
    const auto layout = plan_exposure_finder_layout(geometry, schedule.parts.size(), layout_parameters,
                                                    [&](size_t done, size_t total) {
                                                        advance(20, 10, done, total);
                                                        return true;
                                                    });
    ExposureFinderCellParameters cell;
    cell.base_height = request.recipe.schedule->base_height;
    cell.features_height = request.recipe.schedule->features_height;
    cell.bottom_erosion = request.bottom_erosion;
    cell.chamfer_layers = request.chamfer_layers;
    const auto renderer =
        make_exposure_finder_pattern_renderer(pattern, cell, request.recipe.schedule->multiple_brightness);
    const auto document =
        compose_exposure_finder_layers(schedule, layout, renderer, [&](size_t done, size_t total) {
            advance(30, 50, done, total);
            return true;
        });
    std::vector<double> positions;
    for (const auto &exposure : schedule.exposures)
        positions.push_back(exposure.position_z);
    config.set("gamma_correction", 1.);
    config.set("layer_height", schedule.exposures.front().layer_height);
    config.set("sla_bottom_layers", int(schedule.bottom_records));
    config.set("sla_transition_layers", 0);
    phase = 85;
    check();
    auto job = SLAPrint::create_raster_job(config, document, positions, FreshLayerProducer::Calibration,
                                           ExposurePositionPolicy::RepeatedExposureHeights);
    check();
    auto owned = *job->layer_document();
    owned.replace_layer_parameter_table(planned_rows);
    job->replace_layer_document(owned);
    CalibrationRecipe provenance;
    provenance.parameters.pattern=CalibrationPattern::ExposureFinder;
    provenance.parameters.size_x=pattern.base.cols*geometry.width_mm/geometry.columns;
    provenance.parameters.size_y=pattern.base.rows*geometry.height_mm/geometry.rows;
    provenance.parameters.height=positions.back();provenance.parameters.margin=0.;
    provenance.precision=job->layer_document()->fresh_job()->mask_precision;
    auto finder=request.recipe;
    finder.generation=ExposureFinderGenerationSettings{request.pattern,request.layout,request.bottom_erosion,request.chamfer_layers};
    provenance.exposure_finder=std::make_shared<const ExposureFinderRecipe>(std::move(finder));
    job->set_calibration_recipe(std::make_shared<const CalibrationRecipe>(std::move(provenance)));
    check_layer_progress(callback, 100, 100);
    return job;
}
} // namespace Slic3r::sla
