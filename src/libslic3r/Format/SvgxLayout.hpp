// SPDX-License-Identifier: AGPL-3.0-or-later
// Reader-side layout of the Flashforge SVGX format, for viewing and analysis.
// Port of the UVtools FlashForgeSVGXFile.cs decode path; the writer is
// FlashforgeArchive.cpp / SVGXFile.hpp.
//
//   0    "DLP-II 1.1\n", preview 1 address, preview 2 address, SVG address
//        (u32 little endian at 16, 20, 24), two BMP previews
//   SVG  <printparams resolutionx resolutiony displaywidth displayheight
//        layercount ...>, then one <g id="layer-N"> per layer holding
//        <path fill-rule="evenodd" d="M x y L x y ... Z M ..."/>
//
// Coordinates are millimetres centred on the screen. UVtools turns them into
// pixels as (int)(clamp(half display + mm, 0, display) * resolution / display),
// in single precision, and fills the polygons with the even-odd rule.
//
// READ-ONLY. A layer here is a set of vector contours, not pixels. Re-tracing
// an edited bitmap would change the contour metadata (area, perimeter, print
// range, material volume) that the exporter derives with its own contour
// tracer, so a pixel edit cannot be written back faithfully and the document
// refuses to save edited SVGX. An unchanged document is copied byte for byte.
//
// Rasterising is done here (no OpenCV dependency): polygon edges are drawn
// with Bresenham lines and the interior is filled by an even-odd scanline pass
// that includes both span ends. For the exporter's pixel contours (vertices on
// boundary pixels) this reproduces the original mask exactly; for polygons
// with arbitrary float vertices from other slicers an edge pixel can differ
// from OpenCV's fixed-point rasteriser by one pixel.
#ifndef SLIC3R_SVGX_LAYOUT_HPP
#define SLIC3R_SVGX_LAYOUT_HPP

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "BinaryLayout.hpp"

namespace Slic3r {

class SvgxLayout {
public:
    static bool is_format(const std::string &format) { return format == "svgx"; }
    static bool recognizes(const uint8_t *first8) { return std::memcmp(first8, "DLP-II 1", 8) == 0; }
    // Layers are black and white: anything lit is white.
    static uint8_t quantize(uint8_t value) { return value ? 255 : 0; }
    static const char *read_only_reason()
    {
        return "SVGX layers are vector contours: edited pixels cannot be written back exactly, so edited SVGX files are not "
               "saved. Export the edited layers to another format instead.";
    }

    std::string format = "svgx";
    size_t columns = 0, rows = 0, count = 0;

    explicit SvgxLayout(const std::vector<uint8_t> &bytes, const std::function<void()> &poll = {}) : m_bytes(bytes), m_in(bytes, "SVGX")
    {
        if (bytes.size() < 28) throw RuntimeError("SVGX file is truncated: the header needs 28 bytes");
        if (std::memcmp(bytes.data(), "DLP-II 1.1\n", 11) != 0) throw RuntimeError("SVGX file has the wrong magic number");
        const size_t document = m_in.u32(24);
        if (document < 28 || document >= bytes.size()) throw RuntimeError("SVGX document address is outside the file");
        const std::string_view text(reinterpret_cast<const char *>(bytes.data()) + document, bytes.size() - document);
        if (text.find("<!DOCTYPE") != std::string_view::npos || text.find("<!ENTITY") != std::string_view::npos)
            throw RuntimeError("SVGX document must not contain a DTD or entities");
        scan(text, document, poll);
        if (!m_closed) throw RuntimeError("SVGX file is truncated: the closing </svg> tag is missing");
        if (!m_have_params) throw RuntimeError("SVGX document has no printparams element");
        if (!columns || !rows || !(m_width > 0) || !(m_height > 0))
            throw RuntimeError("SVGX file does not contain a resolution and display size, which are needed to generate the layer images");
        binary_layout::check_raster(columns, rows, "SVGX");
        if (!count || count > binary_layout::max_layers) throw RuntimeError("SVGX layer count is zero or exceeds the editor limit");
        for (const auto &[layer, paths] : m_groups)
            if (layer >= count) throw RuntimeError("SVGX layer group " + std::to_string(layer) + " is beyond its declared " + std::to_string(count) + " layers");
    }

    std::vector<uint8_t> decode(size_t layer) const
    {
        std::vector<uint8_t> pixels(columns * rows, 0);
        const auto found = m_groups.find(layer);
        if (found == m_groups.end()) return pixels;
        const float half_x = m_width / 2.f, half_y = m_height / 2.f;
        const float ppmm_x = float(columns) / m_width, ppmm_y = float(rows) / m_height;
        std::vector<std::vector<Point>> polygons;
        std::vector<Point> points;
        auto flush = [&]() { if (!points.empty()) { polygons.push_back(std::move(points)); points.clear(); } };
        for (const auto &[begin, length] : found->second) {
            const auto tokens = split_spaces(std::string_view(reinterpret_cast<const char *>(m_bytes.data()) + begin, length));
            for (size_t i = 0; i < tokens.size(); ++i) {
                const auto token = tokens[i];
                if (token == "M" || token == "Z") { flush(); continue; }
                if (token.size() == 1 && !(token[0] >= '0' && token[0] <= '9')) continue; // "L" and other one-letter commands
                if (i + 1 >= tokens.size()) break;
                float mm_x, mm_y;
                if (!parse_float(token, mm_x)) continue;
                if (!parse_float(tokens[++i], mm_y)) continue;
                const float absolute_x = std::clamp(half_x + mm_x, 0.f, m_width), absolute_y = std::clamp(half_y + mm_y, 0.f, m_height);
                points.push_back({int(absolute_x * ppmm_x), int(absolute_y * ppmm_y)});
            }
            flush();
        }
        fill_even_odd(polygons, pixels);
        return pixels;
    }

    // Present so the shared save helper compiles; an edit can never be rebuilt (see the header comment).
    std::vector<uint8_t> rebuild(const std::map<size_t, std::vector<uint8_t>> &) const { throw RuntimeError(read_only_reason()); }

private:
    struct Point { int x, y; };
    const std::vector<uint8_t> &m_bytes;
    binary_layout::Reader m_in;
    float m_width = 0, m_height = 0;
    bool m_have_params = false, m_closed = false;
    // layer number -> (byte offset, length) of each path's "d" attribute
    std::map<size_t, std::vector<std::pair<size_t, size_t>>> m_groups;

    static bool parse_float(std::string_view text, float &value)
    {
        const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
        return result.ec == std::errc() && result.ptr == text.data() + text.size() && std::isfinite(value);
    }
    static size_t whole(std::string_view text, const char *what)
    {
        size_t value = 0;
        const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
        if (text.empty() || result.ec != std::errc() || result.ptr != text.data() + text.size())
            throw RuntimeError(std::string("SVGX ") + what + " is not a whole number");
        return value;
    }
    static std::vector<std::string_view> split_spaces(std::string_view text)
    {
        std::vector<std::string_view> tokens;
        size_t i = 0;
        while (i < text.size()) {
            while (i < text.size() && (text[i] == ' ' || text[i] == '\t' || text[i] == '\r' || text[i] == '\n')) ++i;
            const size_t begin = i;
            while (i < text.size() && !(text[i] == ' ' || text[i] == '\t' || text[i] == '\r' || text[i] == '\n')) ++i;
            if (i > begin) tokens.push_back(text.substr(begin, i - begin));
        }
        return tokens;
    }

    // One pass over the XML tags: the printparams attributes and, per layer
    // group, where each path's "d" attribute lies. Text content is not used.
    void scan(std::string_view text, size_t base, const std::function<void()> &poll)
    {
        bool in_layer = false;
        size_t layer = 0, tags = 0;
        for (size_t at = text.find('<'); at != std::string_view::npos; at = text.find('<', at)) {
            if (poll && ++tags % 4096 == 0) poll();
            if (text.compare(at, 4, "<!--") == 0) {
                const auto end = text.find("-->", at + 4);
                if (end == std::string_view::npos) throw RuntimeError("SVGX document has an unterminated comment");
                at = end + 3;
                continue;
            }
            const auto close = text.find('>', at);
            if (close == std::string_view::npos) throw RuntimeError("SVGX document is truncated inside a tag");
            std::string_view tag = text.substr(at + 1, close - at - 1);
            const size_t tag_offset = at + 1;
            at = close + 1;
            if (tag.empty() || tag[0] == '?' || tag[0] == '!') continue;
            if (tag[0] == '/') {
                if (tag.substr(1, 1) == "g") in_layer = false;
                if (tag == "/svg") m_closed = true;
                continue;
            }
            const bool self_closing = tag.back() == '/';
            size_t name_end = 0;
            while (name_end < tag.size() && !std::isspace(static_cast<unsigned char>(tag[name_end])) && tag[name_end] != '/') ++name_end;
            const auto name = tag.substr(0, name_end);
            auto attribute = [&](std::string_view wanted, size_t *offset = nullptr) -> std::string_view {
                size_t p = name_end;
                while (p < tag.size()) {
                    while (p < tag.size() && (std::isspace(static_cast<unsigned char>(tag[p])) || tag[p] == '/')) ++p;
                    const size_t key_begin = p;
                    while (p < tag.size() && tag[p] != '=' && !std::isspace(static_cast<unsigned char>(tag[p]))) ++p;
                    const auto key = tag.substr(key_begin, p - key_begin);
                    while (p < tag.size() && std::isspace(static_cast<unsigned char>(tag[p]))) ++p;
                    if (p >= tag.size() || tag[p] != '=') { if (p >= tag.size()) break; continue; }
                    ++p;
                    while (p < tag.size() && std::isspace(static_cast<unsigned char>(tag[p]))) ++p;
                    if (p >= tag.size() || (tag[p] != '"' && tag[p] != '\'')) break;
                    const char quote = tag[p++];
                    const size_t value_begin = p;
                    while (p < tag.size() && tag[p] != quote) ++p;
                    if (p >= tag.size()) break;
                    if (key == wanted) {
                        if (offset) *offset = base + tag_offset + value_begin;
                        return tag.substr(value_begin, p - value_begin);
                    }
                    ++p;
                }
                return {};
            };
            if (name == "printparams") {
                if (m_have_params) throw RuntimeError("SVGX document has two printparams elements");
                m_have_params = true;
                columns = whole_or_zero(attribute("resolutionx"), "resolutionx");
                rows = whole_or_zero(attribute("resolutiony"), "resolutiony");
                count = whole_or_zero(attribute("layercount"), "layercount");
                if (!parse_float(attribute("displaywidth"), m_width)) m_width = 0;
                if (!parse_float(attribute("displayheight"), m_height)) m_height = 0;
            } else if (name == "g") {
                in_layer = false;
                const auto id = attribute("id");
                if (id.substr(0, 6) == "layer-" && !self_closing) {
                    layer = whole(id.substr(6), "layer id");
                    if (m_groups.count(layer)) throw RuntimeError("SVGX has two groups for layer " + std::to_string(layer));
                    m_groups[layer];
                    in_layer = true;
                } else if (id.substr(0, 6) == "layer-") {
                    layer = whole(id.substr(6), "layer id");
                    m_groups[layer];
                }
            } else if (name == "path" && in_layer) {
                size_t offset = 0;
                const auto d = attribute("d", &offset);
                if (!d.empty()) m_groups[layer].emplace_back(offset, d.size());
            }
        }
    }
    static size_t whole_or_zero(std::string_view text, const char *what) { return text.empty() ? 0 : whole(text, what); }

    // Bresenham line, clipped to the raster.
    static void line(Point a, Point b, size_t columns, size_t rows, std::vector<uint8_t> &pixels)
    {
        const int dx = std::abs(b.x - a.x), dy = -std::abs(b.y - a.y);
        const int sx = a.x < b.x ? 1 : -1, sy = a.y < b.y ? 1 : -1;
        int error = dx + dy;
        for (;;) {
            if (a.x >= 0 && a.y >= 0 && size_t(a.x) < columns && size_t(a.y) < rows) pixels[size_t(a.y) * columns + size_t(a.x)] = 255;
            if (a.x == b.x && a.y == b.y) break;
            const int doubled = 2 * error;
            if (doubled >= dy) { error += dy; a.x += sx; }
            if (doubled <= dx) { error += dx; a.y += sy; }
        }
    }

    // Even-odd fill over all polygons together (they form one path), with the
    // polygon outlines drawn as well so one-pixel features survive.
    void fill_even_odd(const std::vector<std::vector<Point>> &polygons, std::vector<uint8_t> &pixels) const
    {
        struct Edge { int y_top, y_bottom; double x_top, slope; };
        std::vector<Edge> edges;
        for (const auto &polygon : polygons) {
            const size_t n = polygon.size();
            if (n == 1) { line(polygon[0], polygon[0], columns, rows, pixels); continue; }
            for (size_t i = 0; i < n; ++i) {
                const Point a = polygon[i], b = polygon[(i + 1) % n];
                line(a, b, columns, rows, pixels);
                if (a.y == b.y) continue;
                const Point top = a.y < b.y ? a : b, bottom = a.y < b.y ? b : a;
                edges.push_back({top.y, bottom.y, double(top.x), double(bottom.x - top.x) / double(bottom.y - top.y)});
            }
        }
        if (edges.empty()) return;
        std::sort(edges.begin(), edges.end(), [](const Edge &l, const Edge &r) { return l.y_top < r.y_top; });
        std::vector<size_t> active;
        std::vector<int> crossings;
        size_t next = 0;
        const int first = std::max(0, edges.front().y_top);
        for (int y = first; y < int(rows); ++y) {
            while (next < edges.size() && edges[next].y_top <= y) active.push_back(next++);
            // Half-open in y so a vertex shared by two edges is counted once.
            active.erase(std::remove_if(active.begin(), active.end(), [&](size_t e) { return edges[e].y_bottom <= y; }), active.end());
            if (active.empty() && next >= edges.size()) break;
            crossings.clear();
            for (size_t e : active) crossings.push_back(int(std::lround(edges[e].x_top + edges[e].slope * double(y - edges[e].y_top))));
            std::sort(crossings.begin(), crossings.end());
            for (size_t k = 0; k + 1 < crossings.size(); k += 2) {
                const int from = std::max(0, crossings[k]), to = std::min(int(columns) - 1, crossings[k + 1]);
                for (int x = from; x <= to; ++x) pixels[size_t(y) * columns + size_t(x)] = 255;
            }
        }
    }
};

} // namespace Slic3r

#endif
