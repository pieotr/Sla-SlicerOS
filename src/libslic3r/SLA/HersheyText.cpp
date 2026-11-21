// SPDX-License-Identifier: AGPL-3.0-or-later
// Stroke renderer extracted from ExposureFinderPattern.cpp (itself a port of OpenCV's putText); see HersheyText.hpp.
#include "HersheyText.hpp"
#include "HersheyFontData.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <vector>

#include <opencv2/imgproc.hpp>

namespace Slic3r::sla::hershey_text {
namespace {

const int *font_data(int face)
{
    if ((face & ~16) < 0 || (face & ~16) > 7) throw std::invalid_argument("Invalid font face");
    return hershey::fonts[(face & 15) + ((face & 16) ? 8 : 0)];
}

// Glyph of the byte at `index` (Complex = face 3 also knows the Cyrillic range, UTF-8 encoded); advances `index` over
// a multi-byte character.
const char *glyph(const std::string &text, size_t &index, int face)
{
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
            while (index + 1 < text.size() && (static_cast<unsigned char>(text[index + 1]) & 0xc0) == 0x80) ++index;
            c = '?';
        }
    }
    if (c < 32 || c > (face == 3 ? 190u : 126u)) c = '?';
    return hershey::glyphs[font_data(face)[c - 32 + 1]];
}

// Classical OpenCV fixed-point Line2 for one-pixel, non-AA Hershey strokes. OpenCV 5 changed subpixel rounding in this
// path; calibration glyphs must retain the original endpoint and DDA convention at fractional font scales.
void legacy_thin_line(cv::Mat &image, cv::Point first, cv::Point last, uint8_t color)
{
    constexpr int64_t unit = int64_t(1) << 16;
    cv::Point2l a(first.x, first.y), b(last.x, last.y);
    if (!cv::clipLine(cv::Size2l(int64_t(image.cols) * unit, int64_t(image.rows) * unit), a, b)) return;
    const auto put = [&](int64_t x, int64_t y) {
        if (x >= 0 && x < image.cols && y >= 0 && y < image.rows) image.at<uint8_t>(int(y), int(x)) = color;
    };
    const int64_t dx = b.x - a.x, dy = b.y - a.y, ax = std::abs(dx), ay = std::abs(dy);
    if (ax > ay) {
        int64_t signed_dy = dy;
        if (dx < 0) { std::swap(a, b); signed_dy = -signed_dy; }
        const int64_t step = signed_dy * unit / (ax | 1), count = (b.x - a.x) >> 16;
        put((b.x + unit / 2) >> 16, (b.y + unit / 2) >> 16);
        int64_t x = (a.x + unit / 2) >> 16, y = a.y + unit / 2;
        for (int64_t i = 0; i <= count; ++i, ++x, y += step) put(x, y >> 16);
    } else {
        int64_t signed_dx = dx;
        if (dy < 0) { std::swap(a, b); signed_dx = -signed_dx; }
        const int64_t step = signed_dx * unit / (ay | 1), count = (b.y - a.y) >> 16;
        put((b.x + unit / 2) >> 16, (b.y + unit / 2) >> 16);
        int64_t x = a.x + unit / 2, y = (a.y + unit / 2) >> 16;
        for (int64_t i = 0; i <= count; ++i, x += step, ++y) put(x >> 16, y);
    }
}

// UVtools / .NET: Trim(), TrimEnd() on whitespace.
std::string trim(const std::string &s, bool both)
{
    size_t b = 0, e = s.size();
    auto blank = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; };
    if (both) while (b < e && blank(s[b])) ++b;
    while (e > b && blank(s[e - 1])) --e;
    return s.substr(b, e - b);
}

std::vector<std::string> split_lines(const std::string &text)
{
    std::vector<std::string> lines;
    std::string cur;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n') continue; // "\r\n" breaks once
        if (text[i] == '\n' || text[i] == '\r') { lines.push_back(cur); cur.clear(); }
        else cur += text[i];
    }
    lines.push_back(cur);
    return lines;
}

bool blank(const std::string &s) { return trim(s, true).empty(); }

// UVtools PutTextExtended on a raw buffer (no rotation).
void put_block(uint8_t *data, int rows, int cols, int org_x, int org_y, const TextSpec &spec, int value)
{
    // text.TrimEnd('\n', '\r', ' ')
    size_t end = spec.text.size();
    while (end > 0 && (spec.text[end - 1] == '\n' || spec.text[end - 1] == '\r' || spec.text[end - 1] == ' ')) --end;
    const std::string text = spec.text.substr(0, end);
    std::vector<std::string> lines = split_lines(text);
    if (lines.size() <= 1) {
        if (!text.empty()) draw_line(data, rows, cols, text, org_x, org_y, spec.face, spec.scale, value, spec.thickness, spec.line_type, spec.mirror);
        return;
    }
    const int height = text_extent(text, spec.face, spec.scale, spec.thickness).height;
    const int line_gap = height / 3 + spec.line_gap_offset;
    for (auto &l : lines) l = trim(l, spec.alignment != Alignment::None);
    std::vector<int> widths(lines.size(), 0);
    int width = 0;
    if (spec.alignment == Alignment::Center || spec.alignment == Alignment::Right)
        for (size_t i = 0; i < lines.size(); ++i) {
            if (blank(lines[i])) continue;
            widths[i] = text_extent(lines[i], spec.face, spec.scale, spec.thickness).width;
            width = std::max(width, widths[i]);
        }
    for (size_t i = 0; i < lines.size(); ++i) {
        if (blank(lines[i])) continue;
        int x = org_x;
        if (spec.alignment == Alignment::Center) x = org_x + (width - widths[i]) / 2;
        else if (spec.alignment == Alignment::Right) x = org_x + width - widths[i];
        const int shift = int(i) * (line_gap + height);
        draw_line(data, rows, cols, lines[i], x, spec.mirror ? org_y - shift : org_y + shift, spec.face, spec.scale, value, spec.thickness, spec.line_type, spec.mirror);
    }
}

} // namespace

bool valid_face(int face) { return (face & ~16) >= 0 && (face & ~16) <= 7; }

Extent text_extent(const std::string &text, int face, double scale, int thickness)
{
    const int *font = font_data(face);
    double width = 0.;
    for (size_t i = 0; i < text.size(); ++i) {
        const char *p = glyph(text, i, face);
        width += (int(p[1]) - int(p[0])) * scale;
    }
    Extent e;
    e.baseline = int(std::lrint((font[0] & 15) * scale + thickness * .5));
    e.width    = int(std::lrint(width + thickness));
    e.height   = int(std::lrint((((font[0] >> 4) & 15) + (font[0] & 15)) * scale + (thickness + 1) / 2));
    return e;
}

void draw_line(uint8_t *data, int rows, int cols, const std::string &line, int x0, int y0, int face, double scale, int value,
               int thickness, LineType line_type, bool flip_vertical, bool legacy_thin, size_t row_stride)
{
    cv::Mat image(rows, cols, CV_8UC1, data, row_stride ? row_stride : size_t(cols));
    constexpr int64_t unit = int64_t(1) << 16;
    const int64_t factor = std::lrint(scale * unit);
    const int64_t vfactor = flip_vertical ? -factor : factor; // bottomLeftOrigin: vscale = -vscale
    int64_t x = int64_t(x0) * unit, y = int64_t(y0) * unit - int64_t(font_data(face)[0] & 15) * vfactor;
    const cv::Scalar color(value);
    for (size_t i = 0; i < line.size(); ++i) {
        const char *p = glyph(line, i, face);
        x -= int64_t(p[0] - 'R') * factor;
        const int64_t advance = int64_t(p[1] - 'R') * factor;
        p += 2;
        std::vector<cv::Point> stroke;
        const auto flush = [&] {
            if (stroke.size() > 1) {
                if (legacy_thin && thickness == 1 && line_type == LineType::EightConnected) {
                    for (size_t segment = 1; segment < stroke.size(); ++segment)
                        legacy_thin_line(image, stroke[segment - 1], stroke[segment], uint8_t(value));
                } else
                    cv::polylines(image, std::vector<std::vector<cv::Point>>{stroke}, false, color, thickness, int(line_type), 16);
            }
            stroke.clear();
        };
        while (*p) {
            if (*p == ' ') { flush(); ++p; continue; }
            const int64_t px = x + int64_t(p[0] - 'R') * factor, py = y + int64_t(p[1] - 'R') * vfactor;
            if (px < std::numeric_limits<int>::min() || px > std::numeric_limits<int>::max() ||
                py < std::numeric_limits<int>::min() || py > std::numeric_limits<int>::max())
                throw std::runtime_error("Text exceeds fixed-point coordinate limits");
            stroke.emplace_back(int(px), int(py));
            p += 2;
        }
        flush();
        x += advance;
    }
}

std::string validate_text(const TextSpec &s)
{
    std::string sb;
    if (trim(s.text, true).empty()) sb += "The text can not be empty.\n";
    if (!valid_face(s.face)) sb += "Unknown font face.\n";
    if (!std::isfinite(s.scale) || s.scale < 0.2 || s.scale > 200) sb += "The font scale must be between 0.2 and 200.\n";
    if (s.thickness < 1 || s.thickness > 255) sb += "The thickness must be between 1 and 255.\n";
    if (!std::isfinite(s.angle)) sb += "The angle must be a number.\n";
    if (s.text.size() > 4096) sb += "The text is too long.\n";
    return sb;
}

Extent block_extent(const TextSpec &s)
{
    size_t e = s.text.size();
    while (e > 0 && (s.text[e - 1] == '\n' || s.text[e - 1] == '\r' || s.text[e - 1] == ' ')) --e;
    const std::string text = s.text.substr(0, e);
    const Extent one = text_extent(text, s.face, s.scale, s.thickness);
    std::vector<std::string> lines = split_lines(text);
    if (lines.size() <= 1) return one;
    const int gap = one.height / 3 + s.line_gap_offset;
    Extent out;
    out.height = int(lines.size()) * (gap + one.height) - gap;
    for (auto &l : lines) {
        l = trim(l, s.alignment != Alignment::None);
        if (blank(l)) continue;
        out.width = std::max(out.width, text_extent(l, s.face, s.scale, s.thickness).width);
    }
    out.baseline = one.baseline;
    return out;
}

size_t put_text(uint8_t *data, int rows, int cols, int x, int y, const TextSpec &spec, uint8_t value)
{
    if (rows <= 0 || cols <= 0) return 0;
    const std::vector<uint8_t> before(data, data + size_t(rows) * size_t(cols));
    const double turns = std::fmod(spec.angle, 360.0);
    if (turns == 0.0) {
        put_block(data, rows, cols, x, y, spec, value);
    } else {
        // Draw upright on a zero scratch canvas around the origin, then rotate the picture and its coverage into the image.
        const Extent ext = block_extent(spec);
        const int reach = std::max(ext.width, ext.height) * 2 + 4 * spec.thickness + 8; // generous: baseline, descent, mirror, lines
        const int side = 2 * reach + 1;
        std::vector<uint8_t> scratch(size_t(side) * size_t(side), 0), coverage(size_t(side) * size_t(side), 0);
        put_block(scratch.data(), side, side, reach, reach, spec, value);
        put_block(coverage.data(), side, side, reach, reach, spec, 255);
        // exact sin / cos for multiples of 90 degrees (positive angle = clockwise in image coordinates)
        double c = std::cos(turns * M_PI / 180.0), s = std::sin(turns * M_PI / 180.0);
        const double q = turns / 90.0;
        if (q == std::floor(q)) {
            const int k = ((int(q) % 4) + 4) % 4;
            c = k == 0 ? 1 : k == 2 ? -1 : 0;
            s = k == 1 ? 1 : k == 3 ? -1 : 0;
        }
        const long span = long(std::ceil(reach * 1.5)) + 2;
        for (long yy = std::max<long>(0, y - span); yy <= std::min<long>(rows - 1, y + span); ++yy)
            for (long xx = std::max<long>(0, x - span); xx <= std::min<long>(cols - 1, x + span); ++xx) {
                const double dx = double(xx - x), dy = double(yy - y);
                const double sx = reach + c * dx + s * dy, sy = reach - s * dx + c * dy; // inverse rotation
                const double fx = std::floor(sx + 1e-9), fy = std::floor(sy + 1e-9);
                const double tx = std::fabs(sx - std::round(sx)) < 1e-9 ? 0 : sx - fx, ty = std::fabs(sy - std::round(sy)) < 1e-9 ? 0 : sy - fy;
                const long ix = long(std::fabs(sx - std::round(sx)) < 1e-9 ? std::round(sx) : fx), iy = long(std::fabs(sy - std::round(sy)) < 1e-9 ? std::round(sy) : fy);
                auto sample = [&](const std::vector<uint8_t> &img, long px, long py) -> double {
                    return px < 0 || py < 0 || px >= side || py >= side ? 0.0 : double(img[size_t(py) * size_t(side) + size_t(px)]);
                };
                auto bilinear = [&](const std::vector<uint8_t> &img) {
                    return sample(img, ix, iy) * (1 - tx) * (1 - ty) + sample(img, ix + 1, iy) * tx * (1 - ty) +
                           sample(img, ix, iy + 1) * (1 - tx) * ty + sample(img, ix + 1, iy + 1) * tx * ty;
                };
                const double cov = bilinear(coverage);
                if (cov <= 0.0) continue;
                const double alpha = cov / 255.0;
                uint8_t &dst = data[size_t(yy) * size_t(cols) + size_t(xx)];
                if (alpha >= 1.0 - 1e-9 || spec.line_type != LineType::AntiAlias) {
                    // fully covered (or aliased text): the glyph value replaces the pixel
                    dst = alpha >= 0.5 ? value : dst;
                } else {
                    dst = uint8_t(std::lround(double(dst) * (1.0 - alpha) + double(value) * alpha));
                }
            }
    }
    size_t changed = 0;
    for (size_t i = 0; i < before.size(); ++i) changed += before[i] != data[i];
    return changed;
}

} // namespace Slic3r::sla::hershey_text
