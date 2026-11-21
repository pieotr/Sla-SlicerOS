// SPDX-License-Identifier: AGPL-3.0-or-later
#include "LayerOperations.hpp"
#include "LayerMemoryBudget.hpp"
#include "LayerLinkedLines.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cassert>
#include <cmath>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <memory>
#include <set>
#include <stdexcept>

#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>

namespace Slic3r::sla::layerops {

size_t Mask::nonzero_count() const
{
    size_t n = 0;
    for (uint8_t v : px) n += v != 0;
    return n;
}

namespace {

constexpr double kPi = 3.14159265358979323846;

struct Range { size_t first, last; };

Range resolve_range(size_t first, size_t last_requested, size_t count)
{
    if (count == 0) throw std::invalid_argument("There are no layers.");
    const size_t last = std::min(last_requested, count - 1);
    if (first > last) throw std::invalid_argument("The layer range is empty or outside of the layers.");
    return {first, last};
}

void check_uniform(const std::vector<Mask> &layers)
{
    for (const Mask &m : layers) {
        if (m.width != layers.front().width || m.height != layers.front().height)
            throw std::invalid_argument("All layers must have the same resolution.");
        if (m.px.size() != m.width * m.height) throw std::invalid_argument("Layer pixel buffer does not match its resolution.");
    }
}

// An explicit ROI must lie inside the image (OpenCV throws on this too, so silently clamping would hide a bug).
void check_roi(const Rect &roi, size_t w, size_t h)
{
    if (roi.empty()) return;
    if (roi.x < 0 || roi.y < 0 || size_t(roi.right()) > w || size_t(roi.bottom()) > h)
        throw std::invalid_argument("The region of interest is outside of the layer.");
}

Mask crop(const Mask &m, const Rect &r)
{
    Mask out(size_t(r.w), size_t(r.h));
    for (int y = 0; y < r.h; ++y)
        std::memcpy(&out.px[size_t(y) * out.width], &m.px[size_t(r.y + y) * m.width + size_t(r.x)], size_t(r.w));
    return out;
}

// Copies `src` into `dst` at (x, y); the caller guarantees it fits.
void paste(Mask &dst, int x, int y, const Mask &src)
{
    for (size_t row = 0; row < src.height; ++row)
        std::memcpy(&dst.px[(size_t(y) + row) * dst.width + size_t(x)], &src.px[row * src.width], src.width);
}

// Runs g(sub) on the ROI of a copy of `in` (whole image when roi is empty).
template<class G> Mask with_roi(const Mask &in, const Rect &roi, G &&g)
{
    Mask out = in;
    if (roi.empty()) {
        g(out);
    } else {
        Mask sub = crop(in, roi);
        g(sub);
        paste(out, roi.x, roi.y, sub);
    }
    return out;
}

// Runs fn(index) for [first, last] on TBB; reports progress under a lock and throws
// LayerOperationCancelled after the workers finished if the callback asked to stop.
template<class F> void parallel_layers(size_t first, size_t last, const LayerProgress &progress, F &&fn)
{
    const size_t total = last - first + 1;
    check_layer_progress(progress, 0, total);
    std::atomic<size_t> done{0};
    std::atomic<bool>   cancelled{false};
    std::mutex          mtx;
    tbb::parallel_for(tbb::blocked_range<size_t>(first, last + 1), [&](const tbb::blocked_range<size_t> &r) {
        for (size_t i = r.begin(); i != r.end(); ++i) {
            if (cancelled.load()) return;
            fn(i);
            const size_t d = ++done;
            if (progress) {
                std::lock_guard<std::mutex> lock(mtx);
                if (!progress(d, total)) cancelled = true;
            }
        }
    });
    if (cancelled) throw LayerOperationCancelled{};
}

// Generic range op: fn(index, input) -> new mask; results are committed only when every layer finished.
template<class F> void run_per_layer(std::vector<Mask> &layers, size_t first, size_t last, const LayerProgress &progress, F &&fn)
{
    check_uniform(layers);
    const Range r = resolve_range(first, last, layers.size());
    std::vector<Mask> out(r.last - r.first + 1);
    parallel_layers(r.first, r.last, progress, [&](size_t i) { out[i - r.first] = fn(i, layers[i]); });
    for (size_t k = 0; k < out.size(); ++k) layers[r.first + k] = std::move(out[k]);
}

// ---- pixel primitives --------------------------------------------------------------------------

inline uint8_t sat_add(int a, int b) { return uint8_t(std::min(255, a + b)); }
inline uint8_t sat_sub(int a, int b) { return uint8_t(std::max(0, a - b)); }

// Sliding-window min/max over a line with the window [i-r, i+r] clipped to the line. Iterating a 3x3
// rectangle kernel n times with BORDER_REFLECT_101 equals one (2n+1) box clipped at the border.
void sliding_extreme(const uint8_t *in, uint8_t *out, size_t n, int radius, bool is_min)
{
    std::deque<size_t> dq; // indices with monotone values
    size_t             next = 0;
    for (size_t i = 0; i < n; ++i) {
        const size_t hi = std::min(n - 1, i + size_t(radius));
        for (; next <= hi; ++next) {
            while (!dq.empty() && (is_min ? in[dq.back()] >= in[next] : in[dq.back()] <= in[next])) dq.pop_back();
            dq.push_back(next);
        }
        const size_t lo = i >= size_t(radius) ? i - size_t(radius) : 0;
        while (dq.front() < lo) dq.pop_front();
        out[i] = in[dq.front()];
    }
}

Mask box_extreme(const Mask &m, int radius, bool is_min)
{
    if (radius <= 0 || m.px.empty()) return m;
    Mask tmp(m.width, m.height), out(m.width, m.height);
    for (size_t y = 0; y < m.height; ++y) sliding_extreme(&m.px[y * m.width], &tmp.px[y * m.width], m.width, radius, is_min);
    std::vector<uint8_t> col_in(m.height), col_out(m.height);
    for (size_t x = 0; x < m.width; ++x) {
        for (size_t y = 0; y < m.height; ++y) col_in[y] = tmp.px[y * m.width + x];
        sliding_extreme(col_in.data(), col_out.data(), m.height, radius, is_min);
        for (size_t y = 0; y < m.height; ++y) out.px[y * m.width + x] = col_out[y];
    }
    return out;
}
Mask erode(const Mask &m, int iterations) { return box_extreme(m, iterations, true); }
Mask dilate(const Mask &m, int iterations) { return box_extreme(m, iterations, false); }

// Erode/dilate of the ROI of `full`, but looking at the real pixels around the ROI like OpenCV does for
// sub-matrix views (without BORDER_ISOLATED). Matters when the ROI is the model bounds: the model touches the
// ROI edge and must still lose its wall there.
Mask morph_roi(const Mask &full, const Rect &roi, int iterations, bool is_min)
{
    if (roi.empty()) return box_extreme(full, iterations, is_min);
    const int pad = std::max(0, iterations);
    const int x0 = std::max(0, roi.x - pad), y0 = std::max(0, roi.y - pad);
    const int x1 = std::min(int(full.width), roi.right() + pad), y1 = std::min(int(full.height), roi.bottom() + pad);
    const Rect  grown{x0, y0, x1 - x0, y1 - y0};
    const Mask  result = box_extreme(crop(full, grown), iterations, is_min);
    return crop(result, Rect{roi.x - x0, roi.y - y0, roi.w, roi.h});
}
Mask erode_roi(const Mask &full, const Rect &roi, int iterations) { return morph_roi(full, roi, iterations, true); }
Mask dilate_roi(const Mask &full, const Rect &roi, int iterations) { return morph_roi(full, roi, iterations, false); }


struct Affine { double a, b, c, d, e, f; }; // forward map: x' = a x + b y + c ; y' = d x + e y + f

// Bilinear warp with a constant-zero border, same canvas size (cv::warpAffine semantics).
Mask warp_affine(const Mask &src, const Affine &m)
{
    Mask out(src.width, src.height);
    const double det = m.a * m.e - m.b * m.d;
    if (std::abs(det) < 1e-12) return out;
    const double ia = m.e / det, ib = -m.b / det, id = -m.d / det, ie = m.a / det;
    const double ic = -(ia * m.c + ib * m.f), iff = -(id * m.c + ie * m.f);
    const long   W = long(src.width), H = long(src.height);
    auto tap = [&](long x, long y) -> double { return (x < 0 || y < 0 || x >= W || y >= H) ? 0.0 : double(src.px[size_t(y) * src.width + size_t(x)]); };
    for (long y = 0; y < H; ++y)
        for (long x = 0; x < W; ++x) {
            const double sx = ia * double(x) + ib * double(y) + ic;
            const double sy = id * double(x) + ie * double(y) + iff;
            if (sx < -1.0 || sy < -1.0 || sx > double(W) || sy > double(H)) continue;
            const double fx = std::floor(sx), fy = std::floor(sy);
            const long   x0 = long(fx), y0 = long(fy);
            const double tx = sx - fx, ty = sy - fy;
            const double v = (tap(x0, y0) * (1 - tx) + tap(x0 + 1, y0) * tx) * (1 - ty) +
                             (tap(x0, y0 + 1) * (1 - tx) + tap(x0 + 1, y0 + 1) * tx) * ty;
            out.px[size_t(y) * src.width + size_t(x)] = uint8_t(std::clamp(std::lround(v), 0L, 255L));
        }
    return out;
}

Mask transform_from_center(const Mask &m, double xs, double ys)
{
    const double w = double(m.width), h = double(m.height);
    return warp_affine(m, Affine{xs, 0, (w - w * xs) / 2.0, 0, ys, (h - h * ys) / 2.0});
}

// Tiles a pattern from the top-left corner of a w x h canvas.
Mask tile_pattern(const PixelPattern &p, size_t w, size_t h)
{
    Mask out(w, h);
    for (size_t y = 0; y < h; ++y)
        for (size_t x = 0; x < w; ++x) out.px[y * w + x] = p.v[(y % p.rows) * p.cols + (x % p.cols)];
    return out;
}

void fill_rect(Mask &m, long x, long y, long w, long h, uint8_t v)
{
    const long x0 = std::max(0L, x), y0 = std::max(0L, y), x1 = std::min(long(m.width), x + w), y1 = std::min(long(m.height), y + h);
    for (long yy = y0; yy < y1; ++yy)
        for (long xx = x0; xx < x1; ++xx) m.px[size_t(yy) * m.width + size_t(xx)] = v;
}

void fill_circle(Mask &m, long cx, long cy, double r, uint8_t v)
{
    const long x0 = std::max(0L, long(std::floor(double(cx) - r))), x1 = std::min(long(m.width) - 1, long(std::ceil(double(cx) + r)));
    const long y0 = std::max(0L, long(std::floor(double(cy) - r))), y1 = std::min(long(m.height) - 1, long(std::ceil(double(cy) + r)));
    for (long y = y0; y <= y1; ++y)
        for (long x = x0; x <= x1; ++x) {
            const double dx = double(x - cx), dy = double(y - cy);
            if (dx * dx + dy * dy <= r * r) m.px[size_t(y) * m.width + size_t(x)] = v;
        }
}

// Thick segment brush: every pixel within thickness/2 of the segment (round caps), like cv::line with thickness > 1.
void draw_thick_segment(Mask &m, double x0, double y0, double x1, double y1, double thickness, uint8_t v)
{
    const double r = std::max(0.5, thickness / 2.0);
    const long   bx0 = std::max(0L, long(std::floor(std::min(x0, x1) - r))), bx1 = std::min(long(m.width) - 1, long(std::ceil(std::max(x0, x1) + r)));
    const long   by0 = std::max(0L, long(std::floor(std::min(y0, y1) - r))), by1 = std::min(long(m.height) - 1, long(std::ceil(std::max(y0, y1) + r)));
    const double dx = x1 - x0, dy = y1 - y0, len2 = dx * dx + dy * dy;
    for (long y = by0; y <= by1; ++y)
        for (long x = bx0; x <= bx1; ++x) {
            double t = len2 > 0 ? ((double(x) - x0) * dx + (double(y) - y0) * dy) / len2 : 0.0;
            t = std::clamp(t, 0.0, 1.0);
            const double px = x0 + t * dx - double(x), py = y0 + t * dy - double(y);
            if (px * px + py * py <= r * r) m.px[size_t(y) * m.width + size_t(x)] = v;
        }
}

struct Pt { double x, y; };
void draw_polyline(Mask &m, const std::vector<Pt> &pts, bool closed, double thickness, uint8_t v)
{
    for (size_t i = 0; i + 1 < pts.size(); ++i) draw_thick_segment(m, pts[i].x, pts[i].y, pts[i + 1].x, pts[i + 1].y, thickness, v);
    if (closed && pts.size() > 2) draw_thick_segment(m, pts.back().x, pts.back().y, pts.front().x, pts.front().y, thickness, v);
}

// Filled triangle, pixel centres inside (or on) the edges.
void fill_triangle(Mask &m, Pt a, Pt b, Pt c, uint8_t v)
{
    const long x0 = std::max(0L, long(std::floor(std::min({a.x, b.x, c.x})))), x1 = std::min(long(m.width) - 1, long(std::ceil(std::max({a.x, b.x, c.x}))));
    const long y0 = std::max(0L, long(std::floor(std::min({a.y, b.y, c.y})))), y1 = std::min(long(m.height) - 1, long(std::ceil(std::max({a.y, b.y, c.y}))));
    auto edge = [](Pt p, Pt q, double x, double y) { return (q.x - p.x) * (y - p.y) - (q.y - p.y) * (x - p.x); };
    for (long y = y0; y <= y1; ++y)
        for (long x = x0; x <= x1; ++x) {
            const double e0 = edge(a, b, double(x), double(y)), e1 = edge(b, c, double(x), double(y)), e2 = edge(c, a, double(x), double(y));
            if ((e0 >= 0 && e1 >= 0 && e2 >= 0) || (e0 <= 0 && e1 <= 0 && e2 <= 0)) m.px[size_t(y) * m.width + size_t(x)] = v;
        }
}

} // namespace

// ---- public helpers ----------------------------------------------------------------------------

Rect bounding_rect(const Mask &m)
{
    int x0 = int(m.width), y0 = int(m.height), x1 = -1, y1 = -1;
    for (size_t y = 0; y < m.height; ++y)
        for (size_t x = 0; x < m.width; ++x)
            if (m.px[y * m.width + x]) {
                x0 = std::min(x0, int(x)); x1 = std::max(x1, int(x));
                y0 = std::min(y0, int(y)); y1 = std::max(y1, int(y));
            }
    if (x1 < 0) return {};
    return {x0, y0, x1 - x0 + 1, y1 - y0 + 1};
}

namespace {
Rect union_rect(const Rect &a, const Rect &b)
{
    if (a.empty()) return b;
    if (b.empty()) return a;
    const int x0 = std::min(a.x, b.x), y0 = std::min(a.y, b.y), x1 = std::max(a.right(), b.right()), y1 = std::max(a.bottom(), b.bottom());
    return {x0, y0, x1 - x0, y1 - y0};
}
} // namespace

Rect union_bounds(const Rect &a, const Rect &b) { return union_rect(a, b); }
Rect bounding_rect(const std::vector<Mask> &layers)
{
    Rect r;
    for (const Mask &m : layers) r = union_rect(r, bounding_rect(m));
    return r;
}

// ---- kernel infrastructure ----------------------------------------------------------------------
namespace {
std::shared_ptr<const Mask> borrow(const Mask &m) { return std::shared_ptr<const Mask>(std::shared_ptr<void>(), &m); }
} // namespace

StreamLayout batch_layout(const std::vector<Mask> &layers, const std::vector<double> *z_mm)
{
    StreamLayout l;
    l.layer_count = layers.size();
    if (!layers.empty()) { l.width = layers.front().width; l.height = layers.front().height; }
    if (z_mm) l.z_mm = *z_mm;
    l.fetch = [&layers](size_t i) { return borrow(layers.at(i)); };
    l.scan = [&layers](const std::function<void(size_t, const Mask &)> &visit) {
        for (size_t i = 0; i < layers.size(); ++i) visit(i, layers[i]);
    };
    return l;
}

Rect scan_model_bounds(const StreamLayout &l)
{
    Rect r;
    std::mutex mtx;
    l.scan([&](size_t, const Mask &m) {
        const Rect b = bounding_rect(m);
        std::lock_guard<std::mutex> lock(mtx);
        r = union_rect(r, b);
    });
    return r;
}

void run_kernel(std::vector<Mask> &layers, const StreamKernel &k, const LayerProgress &progress)
{
    check_uniform(layers);
    const Range r = resolve_range(k.first, k.last, layers.size());
    const Rect fetch_crop = k.fetch_crop;
    const LayerFetch fetch = [&layers, fetch_crop](size_t i) {
        return fetch_crop.empty() ? borrow(layers.at(i)) : std::make_shared<const Mask>(crop(layers.at(i), fetch_crop));
    };
    std::vector<Mask> out(r.last - r.first + 1);
    std::vector<char> done(out.size(), 0);
    parallel_layers(r.first, r.last, progress, [&](size_t i) {
        if (k.applies && !k.applies(i)) return;
        out[i - r.first] = k.run(i, layers[i], fetch);
        done[i - r.first] = 1;
    });
    for (size_t j = 0; j < out.size(); ++j)
        if (done[j]) layers[r.first + j] = std::move(out[j]);
}

int iteration_chamfer(size_t layer, size_t first, size_t last, int start, int end, bool fade)
{
    fade = fade && first != last && start != end;
    if (!fade) return start;
    const float steps = std::abs((float(start) - float(end)) / (float(last) - float(first)));
    const int   max_it = std::max(start, end);
    const int   it = int(start < end ? float(start) + float(layer - first) * steps : float(start) - float(layer - first) * steps);
    return std::min(std::max(0, it), max_it);
}

PixelPattern parse_pattern_text(const std::string &text, std::string *error)
{
    PixelPattern p;
    auto fail = [&](const std::string &msg) { if (error) *error = msg; return PixelPattern{}; };
    size_t row = 0, pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        const std::string line = text.substr(pos, nl - pos);
        pos = nl + 1;
        std::vector<int> vals;
        size_t i = 0;
        while (i < line.size()) {
            while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) ++i;
            if (i >= line.size()) break;
            size_t j = i;
            while (j < line.size() && !std::isspace(static_cast<unsigned char>(line[j]))) ++j;
            const std::string tok = line.substr(i, j - i);
            if (tok.find_first_not_of("0123456789") != std::string::npos || tok.size() > 3 || std::stoi(tok) > 255)
                return fail(tok + " is a invalid number, use values from 0 to 255");
            vals.push_back(std::stoi(tok));
            i = j;
        }
        if (vals.empty()) continue; // blank lines are skipped like RemoveEmptyEntries
        if (row == 0) p.cols = vals.size();
        else if (vals.size() != p.cols)
            return fail("Row " + std::to_string(row + 1) + " have invalid number of pixels, the pattern must have equal pixel count per line, per defined on line 1");
        for (int v : vals) p.v.push_back(uint8_t(v));
        ++row;
    }
    p.rows = row;
    if (p.v.empty()) { p.rows = p.cols = 0; }
    return p;
}

namespace {
// Rows of a pattern with "B" replaced by the brightness, joined with newlines.
std::string pat(std::initializer_list<const char *> rows, uint8_t b)
{
    std::string out;
    for (const char *row : rows) {
        if (!out.empty()) out += '\n';
        std::string r = row;
        for (size_t p; (p = r.find('B')) != std::string::npos;) r.replace(p, 1, std::to_string(b));
        out += r;
    }
    return out;
}
std::string repeat(const std::string &s, size_t n) { std::string o; for (size_t i = 0; i < n; ++i) o += s; return o; }
std::string trim_ws(std::string s) { while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back(); size_t i = 0; while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i; return s.substr(i); }
} // namespace

PatternTexts generate_dimming_pattern(const std::string &name, uint8_t b)
{
    if (name == "Chessboard") return {pat({"255 B", "B 255"}, b), pat({"B 255", "255 B"}, b)};
    if (name == "Sparse") return {pat({"B 255 255 255", "255 255 B 255"}, b), pat({"255 255 B 255", "B 255 255 255"}, b)};
    if (name == "Crosses")
        return {pat({"B 255 B 255", "255 B 255 255", "B 255 B 255", "255 255 255 255"}, b), pat({"255 255 255 255", "B 255 B 255", "255 B 255 255", "B 255 B 255"}, b)};
    if (name == "Strips") return {pat({"B", "255"}, b), pat({"255", "B"}, b)};
    if (name == "Pyramid")
        return {pat({"255 255 B 255 255 255", "255 B 255 B 255 255", "B 255 B 255 B 255", "255 255 255 255 255 255"}, b),
                pat({"255 B 255 B 255 B", "255 255 B 255 B 255", "255 255 255 B 255 255", "255 255 255 255 255 255"}, b)};
    if (name == "Rhombus")
        return {pat({"255 B 255 255", "B 255 B 255", "255 B 255 255", "255 255 255 255"}, b),
                pat({"255 255 255 255", "255 B 255 255", "B 255 B 255", "255 B 255 255"}, b)};
    if (name == "Hearts")
        return {pat({"255 B 255 B 255 255", "B 255 B 255 B 255", "B 255 255 255 B 255", "255 B 255 B 255 255", "255 255 B 255 255 255", "255 255 255 255 255 255"}, b),
                pat({"255 255 255 255 255 255", "255 255 B 255 B 255", "255 B 255 B 255 B", "255 B 255 255 255 B", "255 255 B 255 B 255", "255 255 255 B 255 255"}, b)};
    if (name == "Slashes") return {pat({"B 255 255", "255 B 255", "255 255 B"}, b), pat({"255 255 B", "255 B 255", "B 255 255"}, b)};
    if (name == "Waves") return {pat({"B 255 255", "255 255 B"}, b), pat({"255 255 B", "B 255 255"}, b)};
    if (name == "Solid") return {std::to_string(b), ""};
    return {};
}

PatternTexts generate_infill_pattern(const std::string &name, unsigned thickness, unsigned spacing)
{
    const std::string z = "0 ", f = "255 ";
    if (name == "Rectilinear") return {trim_ws(repeat("0\n", spacing) + repeat("255\n", spacing)), ""};
    if (name == "Square grid") {
        std::string p1 = trim_ws(repeat(z, spacing) + repeat(f, thickness)) + "\n";
        p1 += repeat(p1, thickness);
        std::string p2 = trim_ws(repeat(f, spacing) + repeat(f, thickness)) + "\n";
        p2 += repeat(p2, thickness);
        return {p1 + trim_ws(p2), ""};
    }
    if (name == "Waves") {
        std::string p1;
        long pos = 0;
        for (int dir = 1; dir >= -1; dir -= 2) {
            while (pos >= 0 && pos <= long(spacing)) {
                p1 += repeat(z, size_t(pos)) + repeat(f, thickness) + repeat(z, spacing - size_t(pos));
                p1 = trim_ws(p1) + '\n';
                pos += dir;
            }
            --pos;
        }
        return {trim_ws(p1), ""};
    }
    if (name == "Lattice") {
        std::string p1, p2;
        const size_t zeros = thickness * 2 >= spacing ? 0 : spacing - thickness * 2;
        auto row = [&](const std::string &s) { return trim_ws(s) + '\n'; };
        for (unsigned i = 0; i < thickness; ++i) p1 += row(repeat(f, thickness) + repeat(z, zeros) + repeat(f, thickness));
        for (size_t i = 0; i < zeros; ++i) p1 += row(repeat(z, spacing));
        for (unsigned i = 0; i < thickness; ++i) p1 += row(repeat(f, thickness) + repeat(z, zeros) + repeat(f, thickness));
        for (unsigned i = 0; i < thickness; ++i) p2 += row(repeat(f, spacing));
        for (size_t i = 0; i < zeros; ++i) p2 += row(repeat(f, thickness) + repeat(z, zeros) + repeat(f, thickness));
        for (unsigned i = 0; i < thickness; ++i) p2 += row(repeat(f, spacing));
        return {trim_ws(p1), trim_ws(p2)};
    }
    return {};
}

// ================================================================================================
// Move
// ================================================================================================

Rect compute_move_destination(const Rect &src, size_t image_w, size_t image_h, Anchor anchor, int ml, int mt, int mr, int mb)
{
    const int W = int(image_w), H = int(image_h);
    Rect dst{0, 0, src.w, src.h};
    switch (anchor) {
    case Anchor::TopLeft:      dst.x = 0;                    dst.y = 0; break;
    case Anchor::TopCenter:    dst.x = W / 2 - src.w / 2;    dst.y = 0; break;
    case Anchor::TopRight:     dst.x = W - src.w;            dst.y = 0; break;
    case Anchor::MiddleLeft:   dst.x = 0;                    dst.y = H / 2 - src.h / 2; break;
    case Anchor::MiddleCenter: dst.x = W / 2 - src.w / 2;    dst.y = H / 2 - src.h / 2; break;
    case Anchor::MiddleRight:  dst.x = W - src.w;            dst.y = H / 2 - src.h / 2; break;
    case Anchor::BottomLeft:   dst.x = 0;                    dst.y = H - src.h; break;
    case Anchor::BottomCenter: dst.x = W / 2 - src.w / 2;    dst.y = H - src.h; break;
    case Anchor::BottomRight:  dst.x = W - src.w;            dst.y = H - src.h; break;
    case Anchor::None:         throw std::invalid_argument("Move needs a concrete anchor.");
    }
    dst.x += ml - mr;
    dst.y += mt - mb;
    return dst;
}

namespace {
bool rect_inside(const Rect &r, size_t w, size_t h)
{
    return !r.empty() && r.x >= 0 && r.y >= 0 && size_t(r.right()) <= w && size_t(r.bottom()) <= h;
}

// Moves/copies a rectangle inside every mask of `masks` (in place, no validation).
void move_in_place(Mask &m, const Rect &src, const Rect &dst, bool cut)
{
    Mask tmp = crop(m, src); // temp copy so overlapping source/destination behave
    if (cut) fill_rect(m, src.x, src.y, src.w, src.h, 0);
    paste(m, dst.x, dst.y, tmp);
}
} // namespace

namespace {
std::string validate_move_geometry(const MoveParams &p, const Rect &src, size_t W, size_t H)
{
    if (src.empty()) return "The layers are empty, there is nothing to move.";
    if (!rect_inside(src, W, H)) return "The source region is outside of the layer.";
    const Rect dst = compute_move_destination(src, W, H, p.anchor, p.margin_left, p.margin_top, p.margin_right, p.margin_bottom);
    if (!rect_inside(dst, W, H))
        return "Your parameters will put the model outside of build plate. Please adjust the location and margins.\n";
    return {};
}
} // namespace

std::string validate_move(const MoveParams &p, const std::vector<Mask> &layers)
{
    if (layers.empty()) return "There are no layers.";
    if (p.anchor == Anchor::None) return "Move needs a concrete anchor.";
    return validate_move_geometry(p, p.selection.roi.empty() ? bounding_rect(layers) : p.selection.roi,
                                  layers.front().width, layers.front().height);
}

StreamKernel move_kernel(const MoveParams &p, const StreamLayout &l)
{
    if (l.layer_count == 0) throw std::invalid_argument("There are no layers.");
    if (p.anchor == Anchor::None) throw std::invalid_argument("Move needs a concrete anchor.");
    const Rect src = p.selection.roi.empty() ? scan_model_bounds(l) : p.selection.roi;
    if (const std::string err = validate_move_geometry(p, src, l.width, l.height); !err.empty()) throw std::invalid_argument(err);
    const Rect dst = compute_move_destination(src, l.width, l.height, p.anchor, p.margin_left, p.margin_top, p.margin_right, p.margin_bottom);
    const Range r = resolve_range(p.selection.first_layer, p.selection.last_layer, l.layer_count);
    StreamKernel k;
    k.first = r.first; k.last = r.last;
    k.run = [src, dst, cut = p.is_cut_move](size_t, const Mask &in, const LayerFetch &) {
        Mask out = in;
        move_in_place(out, src, dst, cut);
        return out;
    };
    return k;
}

void move_layers(std::vector<Mask> &layers, const MoveParams &p, const LayerProgress &progress)
{
    run_kernel(layers, move_kernel(p, batch_layout(layers)), progress);
}

// ================================================================================================
// Resize / Rotate / Flip
// ================================================================================================

std::string validate_resize(const ResizeParams &p)
{
    const double y = p.constrain_xy ? p.x_percent : p.y_percent;
    if (p.x_percent <= 0 || y <= 0) return "X and Y must be greater than 0%.";
    if (p.x_percent == 100 && y == 100) return "X and Y can't both be 100%.";
    return {};
}

StreamKernel resize_kernel(const ResizeParams &p, const StreamLayout &l)
{
    if (const std::string err = validate_resize(p); !err.empty()) throw std::invalid_argument(err);
    const Range r = resolve_range(p.selection.first_layer, p.selection.last_layer, l.layer_count);
    check_roi(p.selection.roi, l.width, l.height);
    const double X = p.x_percent, Y = p.constrain_xy ? p.x_percent : p.y_percent;
    const double count = double(r.last - r.first + 1);
    const double x_step = std::abs(100 - X) / count, y_step = std::abs(100 - Y) / count; // UVtools divides by the layer count
    StreamKernel k;
    k.first = r.first; k.last = r.last;
    k.run = [=](size_t i, const Mask &in, const LayerFetch &) {
        double nx = X, ny = Y;
        if (p.is_fade) {
            const double k = double(i - r.first);
            if (nx != 100) nx = nx < 100 ? nx + k * x_step : nx - k * x_step;
            if (ny != 100) ny = ny < 100 ? ny + k * y_step : ny - k * y_step;
        }
        if (nx == 100 && ny == 100) return in;
        return with_roi(in, p.selection.roi, [&](Mask &t) { t = transform_from_center(t, nx / 100.0, ny / 100.0); });
    };
    return k;
}

void resize_layers(std::vector<Mask> &layers, const ResizeParams &p, const LayerProgress &progress)
{
    check_uniform(layers);
    run_kernel(layers, resize_kernel(p, batch_layout(layers)), progress);
}

std::string validate_rotate(const RotateParams &p)
{
    return p.angle_degrees == 0 ? "An angle of 0 will have no effect" : std::string{};
}

StreamKernel rotate_kernel(const RotateParams &p, const StreamLayout &l)
{
    if (l.layer_count) check_roi(p.selection.roi, l.width, l.height);
    StreamKernel k;
    const Range r = resolve_range(p.selection.first_layer, p.selection.last_layer, l.layer_count);
    k.first = r.first; k.last = r.last;
    if (std::fmod(p.angle_degrees, 360.0) == 0.0) { // UVtools returns early for full turns
        k.applies = [](size_t) { return false; };
        k.run = [](size_t, const Mask &in, const LayerFetch &) { return in; };
        return k;
    }
    // cv::getRotationMatrix2D(centre, -angle): a positive angle turns the image clockwise on screen.
    const double rad = -p.angle_degrees * kPi / 180.0, alpha = std::cos(rad), beta = std::sin(rad);
    k.run = [=](size_t, const Mask &in, const LayerFetch &) {
        return with_roi(in, p.selection.roi, [&](Mask &t) {
            const double cx = double(t.width) / 2.0, cy = double(t.height) / 2.0;
            t = warp_affine(t, Affine{alpha, beta, (1 - alpha) * cx - beta * cy, -beta, alpha, beta * cx + (1 - alpha) * cy});
        });
    };
    return k;
}

void rotate_layers(std::vector<Mask> &layers, const RotateParams &p, const LayerProgress &progress)
{
    check_uniform(layers);
    run_kernel(layers, rotate_kernel(p, batch_layout(layers)), progress);
}

StreamKernel flip_kernel(const FlipParams &p, const StreamLayout &l)
{
    if (l.layer_count) check_roi(p.selection.roi, l.width, l.height);
    const Range r = resolve_range(p.selection.first_layer, p.selection.last_layer, l.layer_count);
    const bool h = p.direction != FlipDirection::Vertical, v = p.direction != FlipDirection::Horizontal;
    StreamKernel k;
    k.first = r.first; k.last = r.last;
    k.run = [=](size_t, const Mask &in, const LayerFetch &) {
        return with_roi(in, p.selection.roi, [&](Mask &t) {
            Mask f(t.width, t.height);
            for (size_t y = 0; y < t.height; ++y)
                for (size_t x = 0; x < t.width; ++x)
                    f.px[y * t.width + x] = t.px[(v ? t.height - 1 - y : y) * t.width + (h ? t.width - 1 - x : x)];
            if (p.make_copy)
                for (size_t i = 0; i < t.px.size(); ++i) t.px[i] = sat_add(t.px[i], f.px[i]);
            else
                t = std::move(f);
        });
    };
    return k;
}

void flip_layers(std::vector<Mask> &layers, const FlipParams &p, const LayerProgress &progress)
{
    check_uniform(layers);
    run_kernel(layers, flip_kernel(p, batch_layout(layers)), progress);
}

// ================================================================================================
// Pattern
// ================================================================================================

PatternLimits compute_pattern_limits(size_t image_w, size_t image_h, const Rect &src)
{
    PatternLimits l{0, 0, 0, 0};
    if (src.empty()) return l;
    l.max_cols = unsigned(image_w / size_t(src.w));
    l.max_rows = unsigned(image_h / size_t(src.h));
    l.max_col_spacing = l.max_cols <= 1 ? 0 : unsigned((image_w - size_t(src.w) * l.max_cols) / l.max_cols);
    l.max_row_spacing = l.max_rows <= 1 ? 0 : unsigned((image_h - size_t(src.h) * l.max_rows) / l.max_rows);
    return l;
}

namespace {
std::string validate_pattern_geometry(const PatternParams &p, const Rect &src, size_t W, size_t H)
{
    std::string sb;
    if (p.cols <= 1 && p.rows <= 1) sb += "Either columns or rows must be greater than 1.\n";
    if (src.empty()) return sb + "The layers are empty, there is nothing to pattern.\n";
    if (!rect_inside(src, W, H)) return sb + "The source region is outside of the layer.\n";
    const size_t vw = p.cols * size_t(src.w) + (p.cols ? p.cols - 1 : 0) * size_t(p.col_spacing);
    const size_t vh = p.rows * size_t(src.h) + (p.rows ? p.rows - 1 : 0) * size_t(p.row_spacing);
    if (p.cols == 0 || p.rows == 0 || vw > W || vh > H)
        sb += "Your parameters will put the object outside of the build plate, please adjust the margins.\n";
    return sb;
}
} // namespace

std::string validate_pattern(const PatternParams &p, const std::vector<Mask> &layers)
{
    if (layers.empty()) return "There are no layers.";
    return validate_pattern_geometry(p, p.selection.roi.empty() ? bounding_rect(layers) : p.selection.roi,
                                     layers.front().width, layers.front().height);
}

StreamKernel pattern_kernel(const PatternParams &p, const StreamLayout &l)
{
    if (l.layer_count == 0) throw std::invalid_argument("There are no layers.");
    const size_t W = l.width, H = l.height;
    const Rect src = p.selection.roi.empty() ? scan_model_bounds(l) : p.selection.roi;
    if (const std::string err = validate_pattern_geometry(p, src, W, H); !err.empty()) throw std::invalid_argument(err);
    const Range r = resolve_range(p.selection.first_layer, p.selection.last_layer, l.layer_count);
    const size_t vw = p.cols * size_t(src.w) + (p.cols - 1) * size_t(p.col_spacing);
    const size_t vh = p.rows * size_t(src.h) + (p.rows - 1) * size_t(p.row_spacing);
    // UVtools replaces the whole layer by the tiled copies (content outside the ROI is dropped).
    const auto tiled = [p, src, W, H, vw, vh](const Mask &in) {
        const Mask tile = crop(in, src);
        Mask       dst(W, H);
        for (unsigned col = 0; col < p.cols; ++col)
            for (unsigned row = 0; row < p.rows; ++row)
                paste(dst, int(col * size_t(src.w) + col * size_t(p.col_spacing) + (W - vw) / 2),
                      int(row * size_t(src.h) + row * size_t(p.row_spacing) + (H - vh) / 2), tile);
        return dst;
    };
    Rect bounds, move_to;
    bool move = false;
    if (p.anchor != Anchor::None) {
        // Model bounds are measured over the whole file after patterning, like the follow-up OperationMove (one extra scan).
        std::mutex mtx;
        l.scan([&](size_t i, const Mask &m) {
            const Rect b = i < r.first || i > r.last ? bounding_rect(m) : bounding_rect(tiled(m));
            std::lock_guard<std::mutex> lock(mtx);
            bounds = union_rect(bounds, b);
        });
        if (!bounds.empty()) { move = true; move_to = compute_move_destination(bounds, W, H, p.anchor, 0, 0, 0, 0); }
    }
    StreamKernel k;
    k.first = r.first; k.last = r.last;
    k.run = [=](size_t, const Mask &in, const LayerFetch &) {
        Mask out = tiled(in);
        if (move) move_in_place(out, bounds, move_to, true);
        return out;
    };
    return k;
}

void pattern_layers(std::vector<Mask> &layers, const PatternParams &p, const LayerProgress &progress)
{
    if (const std::string err = validate_pattern(p, layers); !err.empty()) throw std::invalid_argument(err);
    check_uniform(layers);
    run_kernel(layers, pattern_kernel(p, batch_layout(layers)), progress);
}

// ================================================================================================
// Solidify
// ================================================================================================

namespace {
void solidify_mask(Mask &m, unsigned min_area, AreaCheck check)
{
    const size_t W = m.width, H = m.height, N = W * H;
    if (N == 0) return;
    // Anti-aliasing is cleaned first exactly like UVtools (threshold 127) so grey pixels never form holes.
    auto fg = [&](size_t i) { return m.px[i] > 127; };
    std::vector<uint8_t> outside(N, 0);
    std::vector<uint32_t> stack;
    auto push_bg = [&](size_t x, size_t y) {
        const size_t i = y * W + x;
        if (!fg(i) && !outside[i]) { outside[i] = 1; stack.push_back(uint32_t(i)); }
    };
    for (size_t x = 0; x < W; ++x) { push_bg(x, 0); push_bg(x, H - 1); }
    for (size_t y = 0; y < H; ++y) { push_bg(0, y); push_bg(W - 1, y); }
    while (!stack.empty()) { // 4-connected background flood from the border = the outer background
        const size_t i = stack.back(); stack.pop_back();
        const size_t x = i % W, y = i / W;
        if (x > 0) push_bg(x - 1, y);
        if (x + 1 < W) push_bg(x + 1, y);
        if (y > 0) push_bg(x, y - 1);
        if (y + 1 < H) push_bg(x, y + 1);
    }
    std::vector<int32_t> fg_label; // 8-connected foreground components, built lazily
    auto build_fg_labels = [&]() {
        fg_label.assign(N, -1);
        int32_t next = 0;
        for (size_t s = 0; s < N; ++s) {
            if (!fg(s) || fg_label[s] >= 0) continue;
            fg_label[s] = next; stack.push_back(uint32_t(s));
            while (!stack.empty()) {
                const size_t i = stack.back(); stack.pop_back();
                const long x = long(i % W), y = long(i / W);
                for (long dy = -1; dy <= 1; ++dy)
                    for (long dx = -1; dx <= 1; ++dx) {
                        const long nx = x + dx, ny = y + dy;
                        if (nx < 0 || ny < 0 || nx >= long(W) || ny >= long(H)) continue;
                        const size_t j = size_t(ny) * W + size_t(nx);
                        if (fg(j) && fg_label[j] < 0) { fg_label[j] = next; stack.push_back(uint32_t(j)); }
                    }
            }
            ++next;
        }
    };
    std::vector<uint8_t> seen(N, 0);
    std::vector<uint32_t> hole;
    for (size_t s = 0; s < N; ++s) {
        if (fg(s) || outside[s] || seen[s]) continue;
        hole.clear();
        seen[s] = 1; stack.push_back(uint32_t(s));
        while (!stack.empty()) {
            const size_t i = stack.back(); stack.pop_back();
            hole.push_back(uint32_t(i));
            const size_t x = i % W, y = i / W;
            auto visit = [&](size_t nx, size_t ny) {
                const size_t j = ny * W + nx;
                if (!fg(j) && !seen[j]) { seen[j] = 1; stack.push_back(uint32_t(j)); }
            };
            if (x > 0) visit(x - 1, y);
            if (x + 1 < W) visit(x + 1, y);
            if (y > 0) visit(x, y - 1);
            if (y + 1 < H) visit(x, y + 1);
        }
        // A hole that contains another island has a child contour in UVtools and is left untouched.
        if (fg_label.empty()) build_fg_labels();
        std::set<int32_t> neighbours;
        for (uint32_t i : hole) {
            const long x = long(i % W), y = long(i / W);
            for (long dy = -1; dy <= 1; ++dy)
                for (long dx = -1; dx <= 1; ++dx) {
                    const long nx = x + dx, ny = y + dy;
                    if (nx < 0 || ny < 0 || nx >= long(W) || ny >= long(H)) continue;
                    const int32_t l = fg_label[size_t(ny) * W + size_t(nx)];
                    if (l >= 0) neighbours.insert(l);
                }
        }
        if (neighbours.size() > 1) continue;
        if (min_area >= 1) {
            const size_t area = hole.size();
            if (check == AreaCheck::More ? area < min_area : area > min_area) continue;
        }
        for (uint32_t i : hole) m.px[i] = 255;
    }
}
} // namespace

StreamKernel solidify_kernel(const SolidifyParams &p, const StreamLayout &l)
{
    if (l.layer_count) check_roi(p.selection.roi, l.width, l.height);
    const Range r = resolve_range(p.selection.first_layer, p.selection.last_layer, l.layer_count);
    StreamKernel k;
    k.first = r.first; k.last = r.last;
    k.run = [p](size_t, const Mask &in, const LayerFetch &) {
        return with_roi(in, p.selection.roi, [&](Mask &t) { solidify_mask(t, p.minimum_area, p.area_check); });
    };
    return k;
}

void solidify_layers(std::vector<Mask> &layers, const SolidifyParams &p, const LayerProgress &progress)
{
    check_uniform(layers);
    run_kernel(layers, solidify_kernel(p, batch_layout(layers)), progress);
}

// ================================================================================================
// Threshold
// ================================================================================================

namespace {
std::array<size_t, 256> histogram(const Mask &m)
{
    std::array<size_t, 256> h{};
    for (uint8_t v : m.px) ++h[v];
    return h;
}

int otsu_threshold(const Mask &m)
{
    if (m.px.empty()) return 0;
    const auto   h = histogram(m);
    const double scale = 1.0 / double(m.px.size());
    double mu = 0;
    for (int i = 0; i < 256; ++i) mu += double(i) * double(h[size_t(i)]) * scale;
    double mu1 = 0, q1 = 0, max_sigma = 0;
    int    best = 0;
    for (int i = 0; i < 256; ++i) {
        const double p = double(h[size_t(i)]) * scale;
        mu1 *= q1;
        q1 += p;
        const double q2 = 1.0 - q1;
        if (std::min(q1, q2) < 1.1920929e-7 || std::max(q1, q2) > 1.0 - 1.1920929e-7) continue; // FLT_EPSILON
        mu1 = (mu1 + double(i) * p) / q1;
        const double mu2 = (mu - q1 * mu1) / q2;
        const double sigma = q1 * q2 * (mu1 - mu2) * (mu1 - mu2);
        if (sigma > max_sigma) { max_sigma = sigma; best = i; }
    }
    return best;
}

int triangle_threshold(const Mask &m)
{
    auto hs = histogram(m);
    std::array<double, 256> hist{};
    for (size_t i = 0; i < 256; ++i) hist[i] = double(hs[i]);
    constexpr int N = 256;
    int left = 0, right = 0, max_ind = 0;
    double max = 0;
    for (int i = 0; i < N; ++i) if (hist[size_t(i)] > 0) { left = i; break; }
    if (left > 0) --left;
    for (int i = N - 1; i > 0; --i) if (hist[size_t(i)] > 0) { right = i; break; }
    if (right < N - 1) ++right;
    for (int i = 0; i < N; ++i) if (hist[size_t(i)] > max) { max = hist[size_t(i)]; max_ind = i; }
    bool flipped = false;
    if (max_ind - left < right - max_ind) {
        flipped = true;
        std::reverse(hist.begin(), hist.end());
        left = N - 1 - right;
        max_ind = N - 1 - max_ind;
    }
    int    thresh = left;
    double dist = 0;
    const double a = max, b = double(left - max_ind);
    for (int i = left + 1; i <= max_ind; ++i) {
        const double d = a * double(i) + b * hist[size_t(i)];
        if (d > dist) { dist = d; thresh = i; }
    }
    --thresh;
    if (flipped) thresh = N - 1 - thresh;
    return thresh;
}
} // namespace

void apply_threshold(Mask &m, uint8_t threshold, uint8_t maximum, ThresholdType type)
{
    int t = threshold;
    ThresholdType base = type;
    if (type == ThresholdType::Otsu) { t = otsu_threshold(m); base = ThresholdType::Binary; }
    else if (type == ThresholdType::Triangle) { t = triangle_threshold(m); base = ThresholdType::Binary; }
    t = std::clamp(t, 0, 255);
    for (uint8_t &v : m.px) {
        const bool above = v > t;
        switch (base) {
        case ThresholdType::Binary:    v = above ? maximum : 0; break;
        case ThresholdType::BinaryInv: v = above ? 0 : maximum; break;
        case ThresholdType::Trunc:     v = above ? uint8_t(t) : v; break;
        case ThresholdType::ToZero:    v = above ? v : 0; break;
        case ThresholdType::ToZeroInv: v = above ? 0 : v; break;
        default: break;
        }
    }
}

StreamKernel threshold_kernel(const ThresholdParams &p, const StreamLayout &l)
{
    if (l.layer_count) check_roi(p.selection.roi, l.width, l.height);
    const Range r = resolve_range(p.selection.first_layer, p.selection.last_layer, l.layer_count);
    StreamKernel k;
    k.first = r.first; k.last = r.last;
    k.run = [p](size_t, const Mask &in, const LayerFetch &) {
        return with_roi(in, p.selection.roi, [&](Mask &t) { apply_threshold(t, p.threshold, p.maximum, p.type); });
    };
    return k;
}

void threshold_layers(std::vector<Mask> &layers, const ThresholdParams &p, const LayerProgress &progress)
{
    check_uniform(layers);
    run_kernel(layers, threshold_kernel(p, batch_layout(layers)), progress);
}

// ================================================================================================
// Mask by image (UVtools OperationMask)
std::string validate_mask_image(const MaskImageParams &p, size_t width, size_t height)
{
    std::string sb;
    if (p.image.width == 0 || p.image.height == 0 || p.image.px.size() != p.image.width * p.image.height) sb += "The mask can not be empty.\n";
    else {
        const size_t w = p.selection.roi.empty() ? width : size_t(p.selection.roi.w), h = p.selection.roi.empty() ? height : size_t(p.selection.roi.h);
        if (width && (p.image.width != w || p.image.height != h))
            sb += "The mask image is " + std::to_string(p.image.width) + "x" + std::to_string(p.image.height) + " but the layers (or the region of interest) are "
                + std::to_string(w) + "x" + std::to_string(h) + ".\n";
    }
    return sb;
}

StreamKernel mask_image_kernel(const MaskImageParams &p, const StreamLayout &l)
{
    if (const std::string err = validate_mask_image(p, l.width, l.height); !err.empty()) throw std::invalid_argument(err);
    if (l.layer_count) check_roi(p.selection.roi, l.width, l.height);
    const Range r = resolve_range(p.selection.first_layer, p.selection.last_layer, l.layer_count);
    // Final multiplier table: UVtools multiplies by the (optionally inverted) mask, out = round(in * m / 255).
    auto factor = std::make_shared<std::vector<uint8_t>>(p.image.px);
    if (p.invert) for (uint8_t &v : *factor) v = uint8_t(255 - v);
    StreamKernel k;
    k.first = r.first; k.last = r.last;
    k.run = [p, factor](size_t, const Mask &in, const LayerFetch &) {
        return with_roi(in, p.selection.roi, [&](Mask &t) {
            for (size_t i = 0; i < t.px.size(); ++i) t.px[i] = uint8_t((unsigned(t.px[i]) * unsigned((*factor)[i]) + 127) / 255);
        });
    };
    return k;
}

void mask_image_layers(std::vector<Mask> &layers, const MaskImageParams &p, const LayerProgress &progress)
{
    check_uniform(layers);
    run_kernel(layers, mask_image_kernel(p, batch_layout(layers)), progress);
}

// ================================================================================================
// Pixel dimming
// ================================================================================================

namespace {
PixelPattern fixed_pattern(size_t rows, size_t cols, std::initializer_list<uint8_t> v) { return PixelPattern{rows, cols, std::vector<uint8_t>(v)}; }

Mask invert_mask(Mask m) { for (uint8_t &v : m.px) v = uint8_t(255 - v); return m; }

size_t roi_w(const Rect &roi, const std::vector<Mask> &l) { return roi.empty() ? l.front().width : size_t(roi.w); }
size_t roi_h(const Rect &roi, const std::vector<Mask> &l) { return roi.empty() ? l.front().height : size_t(roi.h); }
} // namespace

std::string validate_pixel_dimming(const PixelDimmingParams &p)
{
    std::string err;
    const PixelPattern a = parse_pattern_text(p.pattern_text, &err);
    if (!err.empty()) return err + "\n";
    const PixelPattern b = parse_pattern_text(p.alternate_pattern_text, &err);
    if (!err.empty()) return err + "\n";
    if (a.empty() && b.empty()) return "Either even or odd pattern must contain a valid matrix.\n";
    if (p.alternate_pattern_per_layers == 0) return "Alternate pattern per layers must be at least 1.\n";
    return {};
}

StreamKernel pixel_dimming_kernel(const PixelDimmingParams &p, const StreamLayout &l)
{
    if (const std::string err = validate_pixel_dimming(p); !err.empty()) throw std::invalid_argument(err);
    const Range r = resolve_range(p.selection.first_layer, p.selection.last_layer, l.layer_count);
    check_roi(p.selection.roi, l.width, l.height);
    std::string ignored;
    PixelPattern a = parse_pattern_text(p.pattern_text, &ignored), b = parse_pattern_text(p.alternate_pattern_text, &ignored);
    if (a.empty()) { // UVtools fallback when only the alternate pattern is given
        a = fixed_pattern(2, 2, {127, 255, 255, 127});
        if (b.empty()) b = fixed_pattern(2, 2, {255, 127, 127, 255});
    }
    if (b.empty()) b = a;
    const size_t tw = p.selection.roi.empty() ? l.width : size_t(p.selection.roi.w);
    const size_t th = p.selection.roi.empty() ? l.height : size_t(p.selection.roi.h);
    // UVtools inverts the tiled pattern, so a pattern value of 255 leaves a pixel untouched.
    const Mask inv_a = invert_mask(tile_pattern(a, tw, th)), inv_b = invert_mask(tile_pattern(b, tw, th));
    StreamKernel k;
    k.first = r.first; k.last = r.last;
    k.run = [p, r, inv_a, inv_b](size_t i, const Mask &in, const LayerFetch &) {
        const int  wall = iteration_chamfer(i, r.first, r.last, int(p.wall_thickness_start), int(p.wall_thickness_end), p.chamfer);
        const Mask &pm = (i / p.alternate_pattern_per_layers) % 2 == 0 ? inv_a : inv_b;
        return with_roi(in, p.selection.roi, [&](Mask &t) {
            const Mask er = erode_roi(in, p.selection.roi, wall), original = t;
            for (size_t k = 0; k < t.px.size(); ++k) {
                const bool apply = p.walls_only ? original.px[k] != 0 : er.px[k] != 0;
                if (apply) t.px[k] = p.lightening_pixels ? sat_add(original.px[k], pm.px[k]) : sat_sub(original.px[k], pm.px[k]);
                if (p.walls_only && er.px[k]) t.px[k] = original.px[k]; // the inside keeps its brightness
            }
        });
    };
    return k;
}

void pixel_dimming_layers(std::vector<Mask> &layers, const PixelDimmingParams &p, const LayerProgress &progress)
{
    if (const std::string err = validate_pixel_dimming(p); !err.empty()) throw std::invalid_argument(err);
    check_uniform(layers);
    run_kernel(layers, pixel_dimming_kernel(p, batch_layout(layers)), progress);
}

// ================================================================================================
// Infill
// ================================================================================================

namespace {
Mask tile_mask(const Mask &tile, size_t w, size_t h)
{
    Mask out(w, h);
    for (size_t y = 0; y < h; ++y)
        for (size_t x = 0; x < w; ++x) out.px[y * w + x] = tile.px[(y % tile.height) * tile.width + (x % tile.width)];
    return out;
}

Mask honeycomb_mask(const InfillParams &p, size_t tw, size_t th)
{
    Mask out(tw, th);
    const double half_sp_d = double(p.infill_spacing) / 2.0;
    const int    half_sp = int(std::nearbyint(half_sp_d)), half_thick = int(p.infill_thickness / 2);
    const int    width = int(std::nearbyint(4 * (half_sp_d / std::sqrt(3.0))));
    const int    cols = int(std::ceil(double(tw) / (double(width) * 0.75f))), rows = int(std::ceil(float(th) / float(p.infill_spacing)));
    auto rnd = [](float v) { return double(std::nearbyint(v)); };
    for (int col = 0; col <= cols; ++col)
        for (int row = 0; row <= rows; ++row) {
            const double x = rnd(float(half_thick) + float(col) * (float(width) * 0.75f));
            double y = double(half_thick + half_sp + row * int(p.infill_spacing));
            if (col % 2 == 1) y += half_sp;
            const std::vector<Pt> pts = {{x, y}, {rnd(float(x) + float(width) * 0.25f), y - half_sp}, {rnd(float(x) + float(width) * 0.75f), y - half_sp},
                                         {x + width, y}, {rnd(float(x) + float(width) * 0.75f), y + half_sp}, {rnd(float(x) + float(width) * 0.25f), y + half_sp}};
            draw_polyline(out, pts, true, double(p.infill_thickness), p.infill_brightness);
        }
    return out;
}

Mask concentric_mask(const InfillParams &p, size_t tw, size_t th)
{
    Mask out(tw, th);
    const double sp = double(p.infill_spacing);
    const Pt     dirs[4] = {{0, -sp}, {sp, 0}, {0, sp}, {-sp, 0}};
    bool         hit[4] = {false, false, false, false};
    double       x = double(tw / 2), y = double(th / 2);
    int          multiplier = 1, position = 0;
    std::vector<Pt> pts{{x, y}};
    while (!(hit[0] && hit[1] && hit[2] && hit[3])) {
        x += dirs[position].x * multiplier;
        y += dirs[position].y * multiplier;
        if (x < 0 || y < 0 || x >= double(tw) || y >= double(th)) hit[position] = true;
        pts.push_back({x, y});
        ++position;
        if (position == 2) ++multiplier;
        else if (position == 4) { ++multiplier; position = 0; }
    }
    draw_polyline(out, pts, false, double(p.infill_thickness), p.infill_brightness);
    return out;
}

// Pillars / cubic cross / alternating cross / star: a square tile of infill_spacing px, tiled from the origin.
Mask cubic_mask(const InfillParams &p, size_t rel_layer, size_t tw, size_t th)
{
    const long sp = long(p.infill_spacing), thk = long(p.infill_thickness);
    const uint8_t c = p.infill_brightness;
    Mask tile(static_cast<size_t>(sp), static_cast<size_t>(sp));
    long long accumulator = 0;
    bool dynamic = false, first = true;
    while (accumulator < (long long)rel_layer) { // which pillar "generation" this layer belongs to
        dynamic = !dynamic;
        first = true;
        accumulator += sp;
        if (accumulator >= (long long)rel_layer) break;
        if (p.reinforce_infill) { first = false; accumulator += thk; }
    }
    if (first) {
        const long t = thk / 2;
        fill_rect(tile, 0, 0, t, t, c);
        fill_rect(tile, sp - t, 0, t, t, c);
        fill_rect(tile, 0, sp - t, t, t, c);
        fill_rect(tile, sp - t, sp - t, t, t, c);
        const long margin = long(int(sp - accumulator + (long long)rel_layer)) - t, inv = long(accumulator - (long long)rel_layer) - t;
        const bool cross = p.type == InfillAlgorithm::CubicCross || (p.type == InfillAlgorithm::CubicCrossAlternating && dynamic) || p.type == InfillAlgorithm::CubicStar;
        const bool star = p.type == InfillAlgorithm::CubicStar || (p.type == InfillAlgorithm::CubicCrossAlternating && !dynamic);
        if (cross) {
            fill_rect(tile, margin, margin, thk, thk, c);
            fill_rect(tile, inv, inv, thk, thk, c);
            fill_rect(tile, margin, inv, thk, thk, c);
            fill_rect(tile, inv, margin, thk, thk, c);
        }
        if (star) {
            fill_rect(tile, margin, -t, thk, thk, c);
            fill_rect(tile, inv, -t, thk, thk, c);
            fill_rect(tile, -t, margin, thk, thk, c);
            fill_rect(tile, -t, inv, thk, thk, c);
            fill_rect(tile, sp - t, margin, thk, thk, c);
            fill_rect(tile, sp - t, inv, thk, thk, c);
            fill_rect(tile, margin, sp - t, thk, thk, c);
            fill_rect(tile, inv, sp - t, thk, thk, c);
        }
    } else {
        // Reinforcement layer: the tile outline, thk px thick, centred on the tile edges.
        const long half = thk / 2;
        fill_rect(tile, 0, 0, sp, half + 1, c);
        fill_rect(tile, 0, sp - 1 - half, sp, half + 1, c);
        fill_rect(tile, 0, 0, half + 1, sp, c);
        fill_rect(tile, sp - 1 - half, 0, half + 1, sp, c);
    }
    return tile_mask(tile, tw, th);
}

Mask waves_mask(const InfillParams &p, size_t rel_layer, double rel_z_mm, size_t layer_w, size_t layer_h, size_t tw, size_t th)
{
    const int radius = std::max(1, int(p.infill_thickness / 2)), sine_h = 100, sine_w = 100;
    bool horizontal = true;
    double acc = 0;
    for (size_t i = 0; i <= rel_layer; ++i) {
        acc += rel_z_mm;
        if (acc >= 2) { horizontal = !horizontal; acc = 0; }
    }
    Mask canvas(layer_w, layer_h);
    std::vector<Pt> pts;
    int max_v = 0;
    const size_t span = horizontal ? layer_w : layer_h;
    for (size_t a = 0; a < span; a += size_t(radius)) {
        const int v = int(std::sin(double(a) / sine_w) * sine_h / 2.0 + sine_h / 2.0 + radius);
        pts.push_back(horizontal ? Pt{double(a), double(v)} : Pt{double(v), double(a)});
        max_v = std::max(max_v, v);
    }
    draw_polyline(canvas, pts, false, double(p.infill_thickness), p.infill_brightness);
    const size_t period = std::min(horizontal ? layer_h : layer_w, size_t(max_v + radius + 2 + int(p.infill_spacing)));
    Mask out(tw, th);
    for (size_t y = 0; y < th; ++y)
        for (size_t x = 0; x < tw; ++x) {
            const size_t sx = horizontal ? x : x % period, sy = horizontal ? y % period : y;
            if (sx < layer_w && sy < layer_h) out.px[y * tw + x] = canvas.px[sy * layer_w + sx];
        }
    return out;
}

Mask gyroid_mask(const InfillParams &p, double z_mm, size_t layer_w, size_t layer_h, size_t tw, size_t th)
{
    Mask out(tw, th);
    const double ratio = 0.0012 / double(p.infill_spacing + p.infill_thickness / 2);
    const double sx = ratio * double(layer_w), sy = ratio * double(layer_h);
    std::vector<double> sinx(tw), cosx(tw);
    for (size_t x = 0; x < tw; ++x) { sinx[x] = std::sin(double(x) * sx); cosx[x] = std::cos(double(x) * sx); }
    const double sin_z = std::sin(z_mm), cos_z = std::cos(z_mm), limit = 0.006 * double(p.infill_thickness);
    for (size_t y = 0; y < th; ++y) {
        const double sin_y = std::sin(double(y) * sy), cos_y = std::cos(double(y) * sy);
        for (size_t x = 0; x < tw; ++x) {
            const double d = sinx[x] * cos_y + sin_y * cos_z + sin_z * cosx[x];
            if (std::abs(d) - limit > 0) continue;
            out.px[y * tw + x] = p.infill_brightness;
        }
    }
    return out;
}

long long to_um(double mm) { return std::llround(mm * 1000.0); }
} // namespace

namespace {
std::string validate_infill_for(const InfillParams &p, size_t layer_count, size_t z_count)
{
    if (layer_count == 0) return "There are no layers.";
    if (z_count != layer_count) return "Every layer needs a Z position.";
    if (p.infill_spacing == 0) return "Infill spacing must be greater than 0.";
    if (p.infill_thickness == 0) return "Infill thickness must be greater than 0.";
    if (p.floor_ceil_thickness_mm < 0) return "Floor/ceiling thickness can't be negative.";
    return {};
}
} // namespace

std::string validate_infill(const InfillParams &p, const std::vector<Mask> &layers, const std::vector<double> &z_mm)
{
    return validate_infill_for(p, layers.size(), z_mm.size());
}

StreamKernel infill_kernel(const InfillParams &p, const StreamLayout &l)
{
    const auto &z_mm = l.z_mm;
    if (const std::string err = validate_infill_for(p, l.layer_count, z_mm.size()); !err.empty()) throw std::invalid_argument(err);
    const Range r = resolve_range(p.selection.first_layer, p.selection.last_layer, l.layer_count);
    check_roi(p.selection.roi, l.width, l.height);
    // UVtools tiles patterns from the top-left corner of the ROI, or of the model volume when there is no ROI.
    const Rect target_rect = p.selection.roi.empty() ? scan_model_bounds(l) : p.selection.roi;
    StreamKernel k;
    k.first = r.first; k.last = r.last;
    if (target_rect.empty()) { // blank model: nothing to fill
        k.applies = [](size_t) { return false; };
        k.run = [](size_t, const Mask &in, const LayerFetch &) { return in; };
        return k;
    }
    const size_t tw = size_t(target_rect.w), th = size_t(target_rect.h), LW = l.width, LH = l.height, count = l.layer_count;
    Mask shared_mask; // layer independent patterns are built once
    if (p.type == InfillAlgorithm::Honeycomb) shared_mask = honeycomb_mask(p, tw, th);
    else if (p.type == InfillAlgorithm::Concentric) shared_mask = concentric_mask(p, tw, th);
    std::vector<long long> z_um(z_mm.size());
    for (size_t i = 0; i < z_um.size(); ++i) z_um[i] = to_um(z_mm[i]);
    const long long thickness_um = to_um(p.floor_ceil_thickness_mm);
    auto rel = [z_um](size_t i) { return i == 0 ? z_um[0] : z_um[i] - z_um[i - 1]; };
    // Window: how many layers below / above any layer of the range the floor / ceiling walk can look at.
    for (size_t idx = r.first; idx <= r.last; ++idx) {
        long long acc = rel(idx);
        if (thickness_um <= acc) continue;
        size_t down = 0, up = 0;
        for (long fl = long(idx) - 1; acc <= thickness_um && fl >= 0; --fl) { ++down; acc += z_um[size_t(fl) + 1] - z_um[size_t(fl)]; }
        k.before = std::max(k.before, down);
        if (acc <= thickness_um) continue;
        acc = rel(idx);
        for (size_t cl = idx + 1; acc <= thickness_um && cl < count; ++cl) { ++up; acc += z_um[cl] - z_um[cl - 1]; }
        k.after = std::max(k.after, up);
    }

    k.fetch_crop = target_rect; // neighbours are only ever read inside the target rectangle
    k.run = [=](size_t idx, const Mask &in, const LayerFetch &fetch) {
        const size_t rel_layer = idx - r.first;
        Mask target = crop(in, target_rect);
        Mask pattern;
        switch (p.type) {
        case InfillAlgorithm::Pillars: case InfillAlgorithm::CubicCross: case InfillAlgorithm::CubicCrossAlternating:
        case InfillAlgorithm::CubicStar: pattern = cubic_mask(p, rel_layer, tw, th); break;
        case InfillAlgorithm::Honeycomb: case InfillAlgorithm::Concentric: pattern = shared_mask; break;
        case InfillAlgorithm::Waves: pattern = waves_mask(p, rel_layer, double(rel(idx)) / 1000.0, LW, LH, tw, th); break;
        case InfillAlgorithm::Gyroid: {
            double zz = 0;
            for (size_t i = r.first; i <= idx; ++i) zz += double(rel(i)) / 1000.0;
            pattern = gyroid_mask(p, zz, LW, LH, tw, th);
            break;
        }
        }
        // Only keep infill where enough solid layers exist above and below (floor / ceiling).
        Mask surface = target;
        long long acc = rel(idx);
        if (thickness_um > acc) {
            for (long fl = long(idx) - 1; acc <= thickness_um && fl >= 0; --fl) {
                const std::shared_ptr<const Mask> other = fetch(size_t(fl)); // keep the layer alive: the cache may evict it meanwhile
                for (size_t k = 0; k < surface.px.size(); ++k) surface.px[k] &= other->px[k];
                acc += z_um[size_t(fl) + 1] - z_um[size_t(fl)];
            }
            if (acc <= thickness_um) return in; // not enough floor: leave the layer solid
            acc = rel(idx);
            for (size_t cl = idx + 1; acc <= thickness_um && cl < count; ++cl) {
                const std::shared_ptr<const Mask> other = fetch(cl);
                for (size_t k = 0; k < surface.px.size(); ++k) surface.px[k] &= other->px[k];
                acc += z_um[cl] - z_um[cl - 1];
            }
            if (acc <= thickness_um) return in;
        }
        const Mask er = erode_roi(in, target_rect, int(p.wall_thickness));
        for (size_t k = 0; k < target.px.size(); ++k)
            if (er.px[k] & surface.px[k]) target.px[k] = pattern.px[k];
        Mask out = in;
        paste(out, target_rect.x, target_rect.y, target);
        return out;
    };
    return k;
}

void infill_layers(std::vector<Mask> &layers, const std::vector<double> &z_mm, const InfillParams &p, const LayerProgress &progress)
{
    if (const std::string err = validate_infill(p, layers, z_mm); !err.empty()) throw std::invalid_argument(err);
    check_uniform(layers);
    run_kernel(layers, infill_kernel(p, batch_layout(layers, &z_mm)), progress);
}

// ================================================================================================
// Pixel arithmetic
// ================================================================================================

std::string validate_pixel_arithmetic(const PixelArithmeticParams &p)
{
    std::string sb;
    const bool have_roi = !p.selection.roi.empty();
    const int  step = int(p.value) + int(std::lround(p.value_step));
    if (p.op == ArithmeticOperator::KeepRegion && !have_roi) sb += "The 'Keep' operator requires selected ROI/masks.\n";
    else if (p.op == ArithmeticOperator::DiscardRegion && !have_roi) sb += "The 'Discard' operator requires selected ROI/masks.\n";
    else if ((p.op == ArithmeticOperator::Add || p.op == ArithmeticOperator::Subtract || p.op == ArithmeticOperator::Maximum ||
              p.op == ArithmeticOperator::BitwiseOr || p.op == ArithmeticOperator::BitwiseXor || p.op == ArithmeticOperator::AbsDiff) && step == 0)
        sb += "This operation by 0 will have no effect.\n";
    else if (p.op == ArithmeticOperator::Divide && p.value == 0) sb += "Can't divide by 0.\n";
    if (p.apply == ArithmeticApply::ModelWalls &&
        ((p.wall_chamfer && p.wall_thickness_start == 0 && p.wall_thickness_end == 0) || (!p.wall_chamfer && p.wall_thickness_start == 0)))
        sb += "The current wall settings will have no effect.\n";
    const bool pattern_visible = p.op != ArithmeticOperator::Threshold && p.op != ArithmeticOperator::BitwiseNot &&
                                 p.op != ArithmeticOperator::KeepRegion && p.op != ArithmeticOperator::DiscardRegion;
    if (p.use_pattern && pattern_visible) {
        std::string err;
        const PixelPattern a = parse_pattern_text(p.pattern_text, &err);
        if (!err.empty()) return sb + err + "\n";
        const PixelPattern b = parse_pattern_text(p.pattern_text_alternate, &err);
        if (!err.empty()) return sb + err + "\n";
        if (a.empty() && b.empty()) return sb + "Either even or odd pattern must contain a valid matrix.\n";
        if (p.pattern_alternate_per_layers == 0) return sb + "Alternate pattern per layers must be at least 1.\n";
    }
    return sb;
}

StreamKernel pixel_arithmetic_kernel(const PixelArithmeticParams &p, const StreamLayout &l)
{
    if (const std::string err = validate_pixel_arithmetic(p); !err.empty()) throw std::invalid_argument(err);
    const Range r = resolve_range(p.selection.first_layer, p.selection.last_layer, l.layer_count);
    const Rect  roi = p.selection.roi;
    check_roi(roi, l.width, l.height);
    const size_t tw = roi.empty() ? l.width : size_t(roi.w), th = roi.empty() ? l.height : size_t(roi.h);
    const bool pattern_visible = p.op != ArithmeticOperator::Threshold && p.op != ArithmeticOperator::BitwiseNot &&
                                 p.op != ArithmeticOperator::KeepRegion && p.op != ArithmeticOperator::DiscardRegion;
    const bool use_pattern = p.use_pattern && pattern_visible;
    Mask pat_a, pat_b;
    if (use_pattern) {
        std::string ignored;
        PixelPattern a = parse_pattern_text(p.pattern_text, &ignored), b = parse_pattern_text(p.pattern_text_alternate, &ignored);
        if (p.pattern_invert) {
            for (uint8_t &v : a.v) v = uint8_t(255 - v);
            for (uint8_t &v : b.v) v = uint8_t(255 - v);
        }
        if (a.empty()) {
            a = fixed_pattern(2, 2, {0, 127, 127, 0});
            if (b.empty()) b = fixed_pattern(2, 2, {127, 0, 0, 127});
        }
        if (b.empty()) b = a;
        pat_a = tile_pattern(a, tw, th);
        pat_b = tile_pattern(b, tw, th);
    }
    const size_t last_file_layer = l.layer_count - 1;
    StreamKernel k;
    k.first = r.first; k.last = r.last;
    // The exposed top skin compares a layer with the next one.
    if (p.apply == ArithmeticApply::ModelSurface || p.apply == ArithmeticApply::ModelSurfaceAndInset) k.after = 1;

    k.run = [=](size_t idx, const Mask &in, const LayerFetch &fetch) {
        if (p.op == ArithmeticOperator::KeepRegion || p.op == ArithmeticOperator::DiscardRegion) {
            if (in.nonzero_count() == 0) return in;
            Mask out = in;
            if (p.op == ArithmeticOperator::KeepRegion) {
                out = Mask(in.width, in.height);
                paste(out, roi.x, roi.y, crop(in, roi));
            } else {
                fill_rect(out, roi.x, roi.y, roi.w, roi.h, 0);
            }
            return out;
        }
        Mask target = roi.empty() ? in : crop(in, roi);
        const Mask original = target;
        // Value for this layer (optionally stepped), as a flat brightness or a tiled pattern.
        uint8_t value = p.value;
        if (p.value_step != 0 && !use_pattern)
            value = uint8_t(std::clamp(std::round(float(p.value) + p.value_step * float(idx - r.first)), 0.0f, 255.0f));
        const int wall = iteration_chamfer(idx, r.first, r.last, int(p.wall_thickness_start), int(p.wall_thickness_end), p.wall_chamfer);
        const Mask *pat = use_pattern ? ((idx / p.pattern_alternate_per_layers) % 2 == 0 ? &pat_a : &pat_b) : nullptr;

        // Which pixels may change.
        std::vector<uint8_t> mask;
        switch (p.apply) {
        case ArithmeticApply::All: break;
        case ArithmeticApply::Model: mask = target.px; break;
        case ArithmeticApply::ModelSurface:
        case ArithmeticApply::ModelSurfaceAndInset: {
            if (idx == last_file_layer) { mask = target.px; break; }
            const Mask next = roi.empty() ? *fetch(idx + 1) : crop(*fetch(idx + 1), roi);
            const Mask er1 = erode_roi(in, roi, 1);
            Mask m(target.width, target.height);
            for (size_t k = 0; k < m.px.size(); ++k) // exposed top skin + one pixel wall
                m.px[k] = sat_add(sat_sub(target.px[k], next.px[k]), sat_sub(target.px[k], er1.px[k]));
            if (p.apply == ArithmeticApply::ModelSurfaceAndInset && wall - 1 > 0) {
                const Mask d = dilate(m, wall - 1);
                for (size_t k = 0; k < m.px.size(); ++k)
                    if (target.px[k]) m.px[k] = d.px[k];
            }
            mask = std::move(m.px);
            break;
        }
        case ArithmeticApply::ModelInner:
            mask = wall <= 0 ? target.px : erode_roi(in, roi, wall).px;
            break;
        case ArithmeticApply::ModelWalls: {
            if (wall <= 0) return in; // no effect
            const Mask er = erode_roi(in, roi, wall);
            mask = target.px;
            for (size_t k = 0; k < mask.size(); ++k) if (er.px[k]) mask[k] = 0;
            break;
        }
        }
        auto allowed = [&](size_t k) { return mask.empty() || mask[k] != 0; };

        // Operators that UVtools skips on an empty layer (they could not change a blank image meaningfully).
        const bool skip_empty = p.op == ArithmeticOperator::Subtract || p.op == ArithmeticOperator::Multiply ||
                                p.op == ArithmeticOperator::Divide || p.op == ArithmeticOperator::Minimum ||
                                p.op == ArithmeticOperator::BitwiseAnd || p.op == ArithmeticOperator::AbsDiff;
        if (skip_empty && in.nonzero_count() == 0) return in;

        if (p.op == ArithmeticOperator::Threshold) {
            apply_threshold(target, p.value, p.threshold_max, p.threshold_type);
            for (size_t k = 0; k < target.px.size(); ++k) if (!allowed(k)) target.px[k] = original.px[k];
        } else {
            for (size_t k = 0; k < target.px.size(); ++k) {
                if (!allowed(k)) continue;
                const int cur = original.px[k], v = pat ? pat->px[k] : value;
                int res = cur;
                switch (p.op) {
                case ArithmeticOperator::Set: res = v; break;
                case ArithmeticOperator::Add: res = std::min(255, cur + v); break;
                case ArithmeticOperator::Subtract: res = std::max(0, cur - v); break;
                case ArithmeticOperator::Multiply: res = int(std::lrintf(float(cur * v) * (1.0f / 255.0f))); break;
                case ArithmeticOperator::Divide: res = v == 0 ? 0 : (cur + v / 2) / v; break; // cv::divide rounds, x/0 = 0
                case ArithmeticOperator::Minimum: res = std::min(cur, v); break;
                case ArithmeticOperator::Maximum: res = std::max(cur, v); break;
                case ArithmeticOperator::BitwiseNot: res = 255 - cur; break;
                case ArithmeticOperator::BitwiseAnd: res = cur & v; break;
                case ArithmeticOperator::BitwiseOr: res = cur | v; break;
                case ArithmeticOperator::BitwiseXor: res = cur ^ v; break;
                case ArithmeticOperator::AbsDiff: res = std::abs(cur - v); break;
                default: break;
                }
                target.px[k] = uint8_t(std::clamp(res, 0, 255));
            }
        }
        if (roi.empty()) return target;
        Mask out = in;
        paste(out, roi.x, roi.y, target);
        return out;
    };
    return k;
}

void pixel_arithmetic_layers(std::vector<Mask> &layers, const PixelArithmeticParams &p, const LayerProgress &progress)
{
    if (const std::string err = validate_pixel_arithmetic(p); !err.empty()) throw std::invalid_argument(err);
    check_uniform(layers);
    run_kernel(layers, pixel_arithmetic_kernel(p, batch_layout(layers)), progress);
}

// ================================================================================================
// Raft relief
// ================================================================================================

namespace {
struct Blob {
    Rect                 bbox;
    size_t               area = 0;       // pixels of the outline-filled region (holes included)
    int                  cx = 0, cy = 0; // centroid of that region
    std::vector<uint8_t> region;         // bbox-sized, 1 = inside the outline
    bool inside(int x, int y) const { return x >= bbox.x && y >= bbox.y && x < bbox.right() && y < bbox.bottom() && region[size_t(y - bbox.y) * size_t(bbox.w) + size_t(x - bbox.x)]; }
};

// 8-connected foreground blobs with their holes filled (the "external contour" view UVtools uses).
std::vector<Blob> find_blobs(const Mask &m, size_t min_area)
{
    std::vector<Blob> blobs;
    std::vector<uint8_t> seen(m.px.size(), 0);
    std::vector<uint32_t> stack, pixels;
    const long W = long(m.width), H = long(m.height);
    for (size_t s = 0; s < m.px.size(); ++s) {
        if (!m.px[s] || seen[s]) continue;
        pixels.clear();
        seen[s] = 1; stack.push_back(uint32_t(s));
        long x0 = W, y0 = H, x1 = -1, y1 = -1;
        while (!stack.empty()) {
            const uint32_t i = stack.back(); stack.pop_back();
            pixels.push_back(i);
            const long x = long(i % size_t(W)), y = long(i / size_t(W));
            x0 = std::min(x0, x); x1 = std::max(x1, x); y0 = std::min(y0, y); y1 = std::max(y1, y);
            for (long dy = -1; dy <= 1; ++dy)
                for (long dx = -1; dx <= 1; ++dx) {
                    const long nx = x + dx, ny = y + dy;
                    if (nx < 0 || ny < 0 || nx >= W || ny >= H) continue;
                    const size_t j = size_t(ny * W + nx);
                    if (m.px[j] && !seen[j]) { seen[j] = 1; stack.push_back(uint32_t(j)); }
                }
        }
        if (pixels.size() < min_area) continue; // cheap pre-filter: filled area is never smaller than the pixel count
        Blob b;
        b.bbox = {int(x0), int(y0), int(x1 - x0 + 1), int(y1 - y0 + 1)};
        const long bw = b.bbox.w + 2, bh = b.bbox.h + 2; // one pixel padding so the flood can walk around the blob
        std::vector<uint8_t> solid(size_t(bw * bh), 0), outside(size_t(bw * bh), 0);
        for (uint32_t i : pixels) solid[size_t((long(i / size_t(W)) - y0 + 1) * bw + (long(i % size_t(W)) - x0 + 1))] = 1;
        std::vector<uint32_t> fs{0};
        outside[0] = 1;
        while (!fs.empty()) {
            const uint32_t i = fs.back(); fs.pop_back();
            const long x = long(i) % bw, y = long(i) / bw;
            const long nb[4][2] = {{x - 1, y}, {x + 1, y}, {x, y - 1}, {x, y + 1}};
            for (auto &n : nb) {
                if (n[0] < 0 || n[1] < 0 || n[0] >= bw || n[1] >= bh) continue;
                const size_t j = size_t(n[1] * bw + n[0]);
                if (!solid[j] && !outside[j]) { outside[j] = 1; fs.push_back(uint32_t(j)); }
            }
        }
        b.region.assign(size_t(b.bbox.w) * size_t(b.bbox.h), 0);
        double sx = 0, sy = 0;
        for (long y = 0; y < b.bbox.h; ++y)
            for (long x = 0; x < b.bbox.w; ++x)
                if (!outside[size_t((y + 1) * bw + x + 1)]) {
                    b.region[size_t(y * b.bbox.w + x)] = 1;
                    ++b.area; sx += double(x + x0); sy += double(y + y0);
                }
        if (b.area < min_area) continue;
        b.cx = int(std::lround(sx / double(b.area)));
        b.cy = int(std::lround(sy / double(b.area)));
        blobs.push_back(std::move(b));
    }
    return blobs;
}

void draw_raft_tabs(Mask &mat, const RaftReliefParams &p)
{
    const int W = int(mat.width), H = int(mat.height);
    const int min_x = 10, max_x = W - 10, min_y = 10, max_y = H - 10;
    const int base_r = int(p.tab_triangle_base / 2), height_r = int(p.tab_triangle_height / 2);
    const Mask original = mat;
    const int dirs[4][2] = {{0, -1}, {1, 0}, {0, 1}, {-1, 0}}; // up, right, down, left
    for (const Blob &blob : find_blobs(original, 10000)) {
        for (int dir = 0; dir < 4; ++dir) {
            bool first_white = false, found = false;
            int x = blob.cx, y = blob.cy;
            while (!found && x >= min_x && x <= max_x && y >= min_y && y <= max_y && x >= blob.bbox.x && x < blob.bbox.right() && y >= blob.bbox.y && y < blob.bbox.bottom()) {
                if (original.at(size_t(x), size_t(y)) > 0) {
                    if (!first_white) { first_white = true; continue; }
                    // "On the contour": a pixel of the blob with a 4-neighbour outside the outline.
                    if (blob.inside(x, y) && (!blob.inside(x - 1, y) || !blob.inside(x + 1, y) || !blob.inside(x, y - 1) || !blob.inside(x, y + 1))) {
                        found = true;
                        break;
                    }
                }
                x += dirs[dir][0];
                y += dirs[dir][1];
            }
            if (!found) continue;
            Pt tri[3];
            switch (dir) {
            case 0: tri[0] = {double(std::max(10, x - base_r)), double(y)}; tri[1] = {double(std::min(W - 10, x + base_r)), double(y)}; tri[2] = {double(x), double(std::max(10, y - height_r))}; break;
            case 1: tri[0] = {double(x), double(std::max(10, y - base_r))}; tri[1] = {double(x), double(std::min(W - 10, y + base_r))}; tri[2] = {double(std::min(W - 10, x + height_r)), double(y)}; break;
            case 2: tri[0] = {double(std::max(10, x - base_r)), double(y)}; tri[1] = {double(std::min(W - 10, x + base_r)), double(y)}; tri[2] = {double(x), double(std::min(H - 10, y + height_r))}; break;
            default: tri[0] = {double(x), double(std::max(10, y - base_r))}; tri[1] = {double(x), double(std::min(W - 10, y + base_r))}; tri[2] = {double(std::max(10, x - height_r)), double(y)}; break;
            }
            // Half ellipse (angles 0..180, rotated 90*dir) reaching into the raft, then the triangle outside.
            const double a = double(base_r), b = double(int(double(base_r) / 1.5));
            const double ax = std::cos(dir * kPi / 2), ay = std::sin(dir * kPi / 2); // rotated x axis
            const double bx = -ay, by = ax;                                         // rotated y axis
            const long reach = long(std::ceil(std::max(a, b))) + 1;
            for (long yy = y - reach; yy <= y + reach; ++yy)
                for (long xx = x - reach; xx <= x + reach; ++xx) {
                    if (xx < 0 || yy < 0 || xx >= W || yy >= H) continue;
                    const double u = double(xx - x) * ax + double(yy - y) * ay, v = double(xx - x) * bx + double(yy - y) * by;
                    if (v >= -1e-9 && b > 0 && (u * u) / (a * a) + (v * v) / (b * b) <= 1.0) mat.at(size_t(xx), size_t(yy)) = 255;
                }
            fill_triangle(mat, tri[0], tri[1], tri[2], p.high_brightness);
        }
    }
}
} // namespace

std::string validate_raft_relief(const RaftReliefParams &p)
{
    std::string sb;
    if (p.type == RaftReliefType::LinkedLines && p.linked_line_thickness < 4) sb += "The link thickness can't be less than 4\n";
    if (p.type == RaftReliefType::Tabs) {
        if (p.tab_triangle_height == 0) sb += "The tab height can't be 0\n";
        if (p.tab_triangle_base == 0) sb += "The tab base can't be 0\n";
        if (p.high_brightness == 0) sb += "The tab brightness can't be 0\n";
    }
    if (p.type == RaftReliefType::Relief && p.hole_diameter + p.hole_spacing == 0) sb += "Hole diameter plus spacing can't be 0\n";
    return sb;
}

std::optional<StreamKernel> raft_relief_kernel(const RaftReliefParams &p, const StreamLayout &l)
{
    if (const std::string err = validate_raft_relief(p); !err.empty()) throw std::invalid_argument(err);
    if (l.layer_count == 0) throw std::invalid_argument("There are no layers.");
    check_roi(p.roi, l.width, l.height);
    auto roi_of = [&](const Mask &m) { return p.roi.empty() ? m : crop(m, p.roi); };

    // Layer where the supports start (the raft ends right below it).
    size_t support_layer = p.mask_layer_index;
    if (support_layer == 0) {
        constexpr size_t kMinSupports = 5, kMaxScan = 1000, kMinSupportArea = 20; // px
        bool found = false;
        const size_t scan = std::min(l.layer_count, kMaxScan);
        for (size_t i = 0; i < scan; ++i) {
            if (find_blobs(roi_of(*l.fetch(i)), kMinSupportArea).size() >= kMinSupports) { support_layer = i; found = true; break; }
        }
        if (!found) return std::nullopt;
    } else if (support_layer >= l.layer_count) {
        throw std::invalid_argument("The mask layer index is outside of the layers.");
    }
    if (p.ignore_first_layers >= support_layer) return std::nullopt;

    const auto support_source = l.fetch(support_layer);
    Mask supports = roi_of(*support_source);
    if (p.dilate_iterations > 0) supports = dilate_roi(*support_source, p.roi, int(p.dilate_iterations));
    const uint8_t color = uint8_t(255 - p.low_brightness);
    Mask pattern;
    if (p.type == RaftReliefType::Relief) {
        const int size = int(p.hole_diameter + p.hole_spacing), radius = int(p.hole_diameter / 2);
        Mask shape(static_cast<size_t>(size), static_cast<size_t>(size));
        fill_circle(shape, size / 2, size / 2, radius, color);
        for (int cx : {0, size}) for (int cy : {0, size}) fill_circle(shape, cx, cy, radius / 2, color);
        pattern = tile_mask(shape, supports.width, supports.height);
    } else if (p.type == RaftReliefType::Dimming) {
        pattern = Mask(supports.width, supports.height, color);
    }

    if (p.type == RaftReliefType::LinkedLines) {
        LinkedLinesParams lp;
        lp.thickness = p.linked_line_thickness; lp.minimum_links = p.linked_minimum_links;
        lp.external_supports = p.linked_external_supports; lp.link_color = p.high_brightness;
        supports = raft_linked_lines(roi_of(*support_source), supports, lp);
    }

    StreamKernel k;
    k.first = p.ignore_first_layers; k.last = support_layer - 1;
    k.run = [p, supports, pattern](size_t, const Mask &in, const LayerFetch &) {
        if (p.type == RaftReliefType::Tabs) {
            Mask out = in;
            draw_raft_tabs(out, p);
            return out;
        }
        return with_roi(in, p.roi, [&](Mask &t) {
            if (p.type == RaftReliefType::Decimate || p.type == RaftReliefType::LinkedLines) { t = supports; return; }
            const Mask er = erode_roi(in, p.roi, int(p.wall_margin));
            const Mask original = t;
            for (size_t k = 0; k < t.px.size(); ++k)
                if (sat_sub(er.px[k], supports.px[k])) t.px[k] = sat_sub(original.px[k], pattern.px[k]);
        });
    };
    return k;
}

bool raft_relief_layers(std::vector<Mask> &layers, const RaftReliefParams &p, const LayerProgress &progress)
{
    if (const std::string err = validate_raft_relief(p); !err.empty()) throw std::invalid_argument(err);
    check_uniform(layers);
    const auto kernel = raft_relief_kernel(p, batch_layout(layers));
    if (!kernel) return false;
    run_kernel(layers, *kernel, progress);
    return true;
}

// ================================================================================================
// Layer list operations
// ================================================================================================

namespace {
double round_height(double mm) { return std::round(mm * 1000.0) / 1000.0; } // UVtools Layer.RoundHeight (3 decimals)

void check_z(const std::vector<Mask> &layers, const std::vector<double> &z)
{
    check_uniform(layers);
    if (z.size() != layers.size()) throw std::invalid_argument("Every layer needs a Z position.");
}
// Height of layer i above the previous one (UVtools RelativePositionZ; the first layer counts from 0).
double relative_z(const std::vector<double> &z, size_t i) { return i == 0 ? z[0] : round_height(z[i] - z[i - 1]); }
} // namespace

std::string validate_clone(const CloneParams &p) { return p.clones == 0 ? "Clones must be a positive number\n" : std::string{}; }

LayerListResult clone_plan(size_t count, std::vector<double> &z_mm, const CloneParams &p, const LayerProgress &progress)
{
    if (const std::string err = validate_clone(p); !err.empty()) throw std::invalid_argument(err);
    if (z_mm.size() != count) throw std::invalid_argument("Every layer needs a Z position.");
    const Range r = resolve_range(p.first_layer, p.last_layer, count);
    const size_t total = (r.last - r.first + 1) * p.clones;
    check_layer_progress(progress, 0, total);
    std::vector<double> out_z;
    LayerListResult res;
    double shift = 0; // accumulated height of the clones inserted so far
    size_t done = 0;
    for (size_t i = 0; i < count; ++i) {
        const double z = p.keep_same_z ? z_mm[i] : round_height(z_mm[i] + shift);
        out_z.push_back(z); res.source_index.push_back(i);
        if (i < r.first || i > r.last) continue;
        double inc = relative_z(z_mm, i);
        if (inc == 0) inc = p.default_layer_height_mm;
        for (unsigned c = 0; c < p.clones; ++c) {
            out_z.push_back(p.keep_same_z ? z_mm[i] : round_height(z + inc * double(c + 1)));
            res.source_index.push_back(i);
            if (!p.keep_same_z) shift += inc;
            ++done;
            check_layer_progress(progress, done, total);
        }
    }
    z_mm = std::move(out_z);
    return res;
}

LayerListResult clone_layers(std::vector<Mask> &layers, std::vector<double> &z_mm, const CloneParams &p, const LayerProgress &progress)
{
    check_z(layers, z_mm);
    LayerListResult res = clone_plan(layers.size(), z_mm, p, progress);
    std::vector<Mask> out_masks;
    out_masks.reserve(res.source_index.size());
    for (size_t source : res.source_index) out_masks.push_back(layers[source]);
    layers = std::move(out_masks);
    return res;
}

namespace {
std::vector<size_t> layers_to_remove(const RemoveParams &p, size_t count, const std::function<size_t(size_t)> &nonzero)
{
    std::vector<size_t> out;
    if (count == 0) return out;
    const size_t last = std::min(p.last_layer, count - 1);
    for (size_t i = p.first_layer; i <= last; ++i) {
        if (p.use_threshold && nonzero(i) > p.pixel_threshold) continue;
        out.push_back(i);
    }
    return out;
}
std::string remove_refusal(size_t removed, size_t count)
{
    if (removed == 0) return "The used values will not remove any layer, please adjust.\n";
    if (removed == count) return "You can't remove all layers from the file. Keep at least one.\n";
    return {};
}
} // namespace

std::string validate_remove(const RemoveParams &p, const std::vector<Mask> &layers)
{
    return remove_refusal(layers_to_remove(p, layers.size(), [&](size_t i) { return layers[i].nonzero_count(); }).size(), layers.size());
}

LayerListResult remove_plan(size_t count, std::vector<double> &z_mm, const RemoveParams &p, const std::function<size_t(size_t)> &nonzero,
                            const LayerProgress &progress)
{
    const std::vector<size_t> removed = layers_to_remove(p, count, nonzero);
    if (const std::string err = remove_refusal(removed.size(), count); !err.empty()) throw std::invalid_argument(err);
    if (z_mm.size() != count) throw std::invalid_argument("Every layer needs a Z position.");
    check_layer_progress(progress, 0, count);
    std::vector<bool> is_removed(count, false);
    for (size_t i : removed) is_removed[i] = true;
    std::vector<double> out_z;
    LayerListResult res;
    double shift = 0; // every removed layer pulls the following layers down by its own height
    for (size_t i = 0; i < count; ++i) {
        if (is_removed[i]) {
            const double rel = relative_z(z_mm, i);
            if (rel > 0) shift += rel;
            continue;
        }
        out_z.push_back(round_height(z_mm[i] - shift));
        res.source_index.push_back(i);
    }
    z_mm = std::move(out_z);
    return res;
}

LayerListResult remove_layers(std::vector<Mask> &layers, std::vector<double> &z_mm, const RemoveParams &p, const LayerProgress &progress)
{
    if (const std::string err = validate_remove(p, layers); !err.empty()) throw std::invalid_argument(err);
    check_z(layers, z_mm);
    LayerListResult res = remove_plan(layers.size(), z_mm, p, [&](size_t i) { return layers[i].nonzero_count(); }, progress);
    std::vector<Mask> out_masks;
    out_masks.reserve(res.source_index.size());
    for (size_t source : res.source_index) out_masks.push_back(std::move(layers[source]));
    layers = std::move(out_masks);
    return res;
}

std::vector<ReHeightPreset> reheight_presets(size_t layer_count, double layer_height_mm)
{
    // Heights are handled in whole micrometres: UVtools rejects results with more than 3 decimals.
    std::vector<ReHeightPreset> out;
    const long long h = std::llround(layer_height_mm * 1000.0);
    constexpr long long kMinUm = 10, kMaxUm = 200; // Layer.MinimumHeight / MaximumHeight
    for (int i = 2; i < 255; ++i) {
        if (h / i < kMinUm) break;                       // "layerHeight / i < MinimumHeight"
        if (h % i != 0) continue;
        out.push_back({false, uint8_t(i), double(h / i) / 1000.0, layer_count * size_t(i)});
    }
    for (int i = 2; i < 255; ++i) {
        if (h * i > kMaxUm) break;
        if (layer_count % size_t(i) != 0) continue;
        out.push_back({true, uint8_t(i), double(h * i) / 1000.0, layer_count / size_t(i)});
    }
    return out;
}

std::string validate_reheight(const ReHeightParams &p, const std::vector<Mask> &layers, const std::vector<double> &z_mm)
{
    if (layers.empty()) return "There are no layers.";
    if (z_mm.size() != layers.size()) return "Every layer needs a Z position.";
    if (p.method == ReHeightMethod::ReHeight) {
        const ReHeightPreset &s = p.preset;
        if (s.modifier < 2) return "No new height was selected.\n";
        const size_t expected = s.is_multiply ? layers.size() / s.modifier : layers.size() * s.modifier;
        if (s.is_multiply && layers.size() % s.modifier != 0) return "The layer count is not divisible by the modifier.\n";
        if (s.layer_count != expected || s.layer_height_mm <= 0) return "No valid configurations, unable to proceed.\n";
        return {};
    }
    if (p.position_z_offset_mm == 0) return "Position offset can't be 0, it have no effect.\n";
    const size_t last = std::min(p.last_layer, layers.size() - 1);
    for (size_t i = p.first_layer; i <= last; ++i)
        if (z_mm[i] + p.position_z_offset_mm < 0)
            return "Offset position by " + std::to_string(p.position_z_offset_mm) + "mm will put layer " + std::to_string(i) + " under 0mm.\n";
    return {};
}

LayerListResult reheight_plan(size_t count, std::vector<double> &z_mm, const ReHeightParams &p, const LayerProgress &progress)
{
    if (count == 0) throw std::invalid_argument("There are no layers.");
    if (z_mm.size() != count) throw std::invalid_argument("Every layer needs a Z position.");
    LayerListResult res;
    if (p.method == ReHeightMethod::OffsetPositionZ) {
        const Range r = resolve_range(p.first_layer, p.last_layer, count);
        check_layer_progress(progress, 0, r.last - r.first + 1);
        for (size_t i = r.first; i <= r.last; ++i) z_mm[i] = round_height(z_mm[i] + p.position_z_offset_mm);
        for (size_t i = 0; i < count; ++i) res.source_index.push_back(i);
        return res;
    }
    const ReHeightPreset &s = p.preset;
    res.source_index.resize(s.layer_count);
    std::vector<double> out_z(s.layer_count);
    if (!s.is_multiply) { // division: every layer is repeated `modifier` times
        check_layer_progress(progress, 0, s.layer_count);
        size_t n = 0;
        for (size_t i = 0; i < count; ++i)
            for (unsigned k = 0; k < s.modifier; ++k, ++n) res.source_index[n] = i;
    } else { // multiplication: `modifier` layers are merged into one (see reheight_merge)
        for (size_t k = 0; k < s.layer_count; ++k) res.source_index[k] = k * s.modifier;
    }
    for (size_t k = 0; k < out_z.size(); ++k) out_z[k] = round_height(s.layer_height_mm * double(k + 1));
    z_mm = std::move(out_z);
    return res;
}

Mask reheight_merge(const std::vector<const Mask *> &group, const ReHeightParams &p)
{
    const ReHeightPreset &s = p.preset;
    Mask sum = *group[0];
    const size_t N = sum.px.size();
    if (p.anti_aliasing == ReHeightAntiAliasing::None) {
        for (unsigned i = 1; i < s.modifier; ++i)
            for (size_t q = 0; q < N; ++q) sum.px[q] = sat_add(sum.px[q], group[i]->px[q]);
    } else if (p.anti_aliasing == ReHeightAntiAliasing::Average) {
        std::vector<unsigned> acc(N);
        for (size_t q = 0; q < N; ++q) acc[q] = group[0]->px[q];
        for (unsigned i = 1; i < s.modifier; ++i)
            for (size_t q = 0; q < N; ++q) acc[q] += group[i]->px[q];
        for (size_t q = 0; q < N; ++q) sum.px[q] = uint8_t(std::min(255L, std::lrintf(float(acc[q]) / float(s.modifier))));
    } else { // Difference: edges that change between layers become partially exposed
        std::vector<uint8_t> xor_sum;
        for (unsigned i = 1; i < s.modifier; ++i) {
            std::vector<uint8_t> x(N);
            for (size_t q = 0; q < N; ++q) x[q] = (group[i - 1]->px[q] ^ group[i]->px[q]) ? uint8_t(255 / s.modifier) : 0;
            if (xor_sum.empty()) xor_sum = std::move(x);
            else for (size_t q = 0; q < N; ++q) xor_sum[q] = sat_add(sat_add(xor_sum[q], xor_sum[q]), x[q]); // UVtools: 2*sum + xor
        }
        for (size_t q = 0; q < N; ++q) sum.px[q] = sat_add(sum.px[q], xor_sum.empty() ? 0 : xor_sum[q]);
    }
    return sum;
}

LayerListResult reheight_layers(std::vector<Mask> &layers, std::vector<double> &z_mm, const ReHeightParams &p, const LayerProgress &progress)
{
    if (const std::string err = validate_reheight(p, layers, z_mm); !err.empty()) throw std::invalid_argument(err);
    check_z(layers, z_mm);
    LayerListResult res = reheight_plan(layers.size(), z_mm, p, progress);
    if (p.method == ReHeightMethod::OffsetPositionZ) return res;
    const ReHeightPreset &s = p.preset;
    std::vector<Mask> out(s.layer_count);
    if (!s.is_multiply) {
        for (size_t n = 0; n < out.size(); ++n) out[n] = layers[res.source_index[n]];
    } else {
        parallel_layers(0, s.layer_count - 1, progress, [&](size_t k) {
            std::vector<const Mask *> group;
            for (unsigned i = 0; i < s.modifier; ++i) group.push_back(&layers[k * s.modifier + i]);
            out[k] = reheight_merge(group, p);
        });
    }
    layers = std::move(out);
    return res;
}

// ---- Layer arithmetic --------------------------------------------------------------------------

bool LayerArithmeticGroup::is_valid() const
{
    return !set_layers.empty() && !operations.empty() && !(set_layers.size() == 1 && operations.size() == 1 && set_layers[0] == operations[0].layer);
}

namespace {
bool parse_uint(const std::string &s, size_t &out)
{
    if (s.empty() || s.find_first_not_of("0123456789") != std::string::npos || s.size() > 9) return false;
    out = size_t(std::stoull(s));
    return true;
}
std::vector<std::string> split_skip_empty(const std::string &s, char sep, bool skip_empty)
{
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos <= s.size()) {
        size_t e = s.find(sep, pos);
        if (e == std::string::npos) e = s.size();
        if (!skip_empty || e > pos) out.push_back(s.substr(pos, e - pos));
        pos = e + 1;
    }
    return out;
}
} // namespace

bool parse_layer_arithmetic(const std::string &sentence, size_t layer_count, std::vector<LayerArithmeticGroup> &out, std::string *error)
{
    out.clear();
    auto fail = [&](const char *msg) { if (error) *error = msg; out.clear(); return false; };
    if (sentence.find_first_not_of(" \t\r\n") == std::string::npos) return fail("The sentence is empty.");
    if (layer_count == 0) return fail("There are no layers.");
    std::string s;
    for (char c : sentence) if (!std::isspace(static_cast<unsigned char>(c))) s += c;
    const size_t last_index = layer_count - 1;
    for (const std::string &part : split_skip_empty(s, ';', true)) {
        LayerArithmeticGroup g;
        const std::vector<std::string> halves = split_skip_empty(part, '=', false);
        std::string operations = halves[0];
        if (halves.size() >= 2) {
            operations = halves[1];
            for (const std::string &layer : split_skip_empty(halves[0], ',', true)) {
                const std::vector<std::string> range = split_skip_empty(layer, ':', true);
                if (range.size() > 1) {
                    size_t start = 0, end = last_index;
                    parse_uint(range[0], start); // a bad start parses as 0, like TryParse
                    if (!parse_uint(range[1], end)) end = last_index;
                    end = std::min(end, last_index);
                    for (size_t i = start; i <= end; ++i)
                        if (std::find(g.set_layers.begin(), g.set_layers.end(), i) == g.set_layers.end()) g.set_layers.push_back(i);
                    continue;
                }
                size_t idx;
                if (!parse_uint(layer, idx) || idx > last_index) continue; // out-of-range targets are ignored (UVtools would throw)
                if (std::find(g.set_layers.begin(), g.set_layers.end(), idx) == g.set_layers.end()) g.set_layers.push_back(idx);
            }
        }
        std::sort(g.set_layers.begin(), g.set_layers.end());
        if (operations.empty()) return fail("Unable to parse the sentence, malformed or incomplete.");
        std::string digits;
        for (char c : operations) {
            if (c >= '0' && c <= '9') { digits += c; continue; }
            const char op = (c == '+' || c == '-' || c == '*' || c == '/' || c == '&' || c == '|' || c == '^' || c == '$') ? c : '\0';
            if (op == '\0' || digits.empty()) continue; // same leniency as UVtools: digits keep accumulating
            size_t idx;
            if (parse_uint(digits, idx) && idx <= last_index) g.operations.push_back({idx, op});
            digits.clear();
        }
        if (!digits.empty()) {
            size_t idx;
            if (parse_uint(digits, idx) && idx <= last_index) g.operations.push_back({idx, '\0'});
        }
        if (g.set_layers.empty() && !g.operations.empty()) g.set_layers.push_back(g.operations[0].layer);
        out.push_back(std::move(g));
    }
    return true;
}

std::string validate_layer_arithmetic(const LayerArithmeticParams &p, size_t layer_count)
{
    std::vector<LayerArithmeticGroup> groups;
    std::string err;
    if (!parse_layer_arithmetic(p.sentence, layer_count, groups, &err)) return err.empty() ? "Unable to parse the sentence, malformed or incomplete.\n" : err + "\n";
    if (std::none_of(groups.begin(), groups.end(), [](const LayerArithmeticGroup &g) { return g.is_valid(); }))
        return "The operation will have no impact and will not be performed.\n";
    return {};
}

void layer_arithmetic(std::vector<Mask> &layers, const LayerArithmeticParams &p, const LayerProgress &progress)
{
    check_uniform(layers);
    if (const std::string err = validate_layer_arithmetic(p, layers.size()); !err.empty()) throw std::invalid_argument(err);
    check_roi(p.roi, layers.front().width, layers.front().height);
    std::vector<LayerArithmeticGroup> groups;
    parse_layer_arithmetic(p.sentence, layers.size(), groups, nullptr);
    // Groups run in order and see each other's results; everything is staged in `changes` until the end.
    std::map<size_t, Mask> changes;
    auto current = [&](size_t i) -> const Mask & { auto it = changes.find(i); return it == changes.end() ? layers[i] : it->second; };
    size_t step = 0;
    for (const LayerArithmeticGroup &g : groups) {
        if (!g.is_valid()) continue;
        check_layer_progress(progress, step++, groups.size());
        Mask result = p.roi.empty() ? current(g.operations[0].layer) : crop(current(g.operations[0].layer), p.roi);
        for (size_t i = 1; i < g.operations.size(); ++i) {
            const Mask img = p.roi.empty() ? current(g.operations[i].layer) : crop(current(g.operations[i].layer), p.roi);
            const char op = g.operations[i - 1].op;
            for (size_t k = 0; k < result.px.size(); ++k) {
                const int a = result.px[k], b = img.px[k];
                int v = a;
                switch (op) {
                case '+': v = std::min(255, a + b); break;
                case '-': v = std::max(0, a - b); break;
                case '*': v = int(std::lrintf(float(a * b) * (1.0f / 255.0f))); break;
                case '/': v = b == 0 ? 0 : (a + b / 2) / b; break;
                case '&': v = a & b; break;
                case '|': v = a | b; break;
                case '^': v = a ^ b; break;
                case '$': v = std::abs(a - b); break;
                default: break;
                }
                result.px[k] = uint8_t(v);
            }
        }
        for (size_t target : g.set_layers) {
            if (p.roi.empty()) { changes[target] = result; continue; }
            Mask m = current(target);
            paste(m, p.roi.x, p.roi.y, result);
            changes[target] = std::move(m);
        }
    }
    check_layer_progress(progress, groups.size(), groups.size());
    for (auto &kv : changes) layers[kv.first] = std::move(kv.second);
}

// Streaming form: valid when the groups do not depend on each other (no group reads a layer an earlier group writes, no
// layer is written twice). Then the result of a target layer is a function of original layers only; operand layers are
// read through `fetch` (one decode each when streaming). The batch function above stays the reference.
StreamKernel layer_arithmetic_kernel(const LayerArithmeticParams &p, const StreamLayout &l)
{
    if (const std::string err = validate_layer_arithmetic(p, l.layer_count); !err.empty()) throw std::invalid_argument(err);
    check_roi(p.roi, l.width, l.height);
    std::vector<LayerArithmeticGroup> groups;
    parse_layer_arithmetic(p.sentence, l.layer_count, groups, nullptr);
    std::map<size_t, size_t> group_of; // target layer -> group index
    std::set<size_t> written;
    for (size_t g = 0; g < groups.size(); ++g) {
        if (!groups[g].is_valid()) continue;
        for (const auto &o : groups[g].operations)
            if (written.count(o.layer)) throw NotStreamable("layer arithmetic: a group reads a layer an earlier group writes");
        for (size_t t : groups[g].set_layers) {
            if (group_of.count(t) && group_of[t] != g) throw NotStreamable("layer arithmetic: a layer is written by two groups");
            group_of[t] = g;
        }
        for (size_t t : groups[g].set_layers) written.insert(t);
    }
    StreamKernel k;
    k.first = group_of.begin()->first; k.last = group_of.rbegin()->first;
    k.random_access = true;
    k.applies = [group_of](size_t i) { return group_of.count(i) != 0; };
    k.run = [groups, group_of, roi = p.roi](size_t i, const Mask &in, const LayerFetch &fetch) {
        const LayerArithmeticGroup &g = groups[group_of.at(i)];
        const auto operand = [&](size_t layer) { return roi.empty() ? Mask(*fetch(layer)) : crop(*fetch(layer), roi); };
        Mask result = operand(g.operations[0].layer);
        for (size_t n = 1; n < g.operations.size(); ++n) {
            const Mask img = operand(g.operations[n].layer);
            const char op = g.operations[n - 1].op;
            for (size_t px = 0; px < result.px.size(); ++px) {
                const int a = result.px[px], b = img.px[px];
                int v = a;
                switch (op) {
                case '+': v = std::min(255, a + b); break;
                case '-': v = std::max(0, a - b); break;
                case '*': v = int(std::lrintf(float(a * b) * (1.0f / 255.0f))); break;
                case '/': v = b == 0 ? 0 : (a + b / 2) / b; break;
                case '&': v = a & b; break;
                case '|': v = a | b; break;
                case '^': v = a ^ b; break;
                case '$': v = std::abs(a - b); break;
                default: break;
                }
                result.px[px] = uint8_t(v);
            }
        }
        if (roi.empty()) return result;
        Mask m = in;
        paste(m, roi.x, roi.y, result);
        return m;
    };
    return k;
}

// ---- Double exposure ---------------------------------------------------------------------------

std::string validate_double_exposure(const DoubleExposureParams &p, const std::vector<double> &z_mm)
{
    if (z_mm.empty()) return "There are no layers.";
    const size_t last = std::min(p.last_layer, z_mm.size() - 1);
    if (p.first_layer > last) return "The layer range is empty or outside of the layers.\n";
    for (size_t i = p.first_layer + 1; i <= last; ++i)
        if (z_mm[i] == z_mm[i - 1])
            return "The selected layer range already have modified layers with same z position, starting at layer " + std::to_string(i) + ". Not safe to continue.\n";
    return {};
}

void double_exposure_pair(const Mask &mat, size_t i, const DoubleExposureParams &p, Mask &first, Mask &second)
{
    const bool bottom = i < p.bottom_layer_count;
    const int fe = int(bottom ? p.first_bottom_erode : p.first_normal_erode), se = int(bottom ? p.second_bottom_erode : p.second_normal_erode);
    const int overlap = int(p.second_layer_difference_overlap_erode);
    first = mat; second = mat; // the second pass starts as a clone of the layer
    if (fe > 0 && fe == se) {
        first = erode(mat, fe);
        second = first;
        if (p.second_layer_difference && overlap > 0) {
            const Mask inner = erode(first, overlap);
            for (size_t k = 0; k < second.px.size(); ++k) second.px[k] = sat_sub(first.px[k], inner.px[k]);
        }
    } else {
        Mask first_mat, second_mat;
        const bool has_first = fe > 0, has_second = se > 0;
        if (has_first) { first_mat = erode(mat, fe); first = first_mat; }
        if (has_second) second_mat = erode(mat, se);
        if (has_first && p.second_layer_difference) {
            // When fe + overlap == se UVtools leaves the second pass unmodified (the original layer).
            if (fe + overlap != se) {
                if (overlap > 0) first_mat = erode(first_mat, overlap);
                const Mask &other = has_second ? second_mat : mat;
                for (size_t k = 0; k < second.px.size(); ++k)
                    second.px[k] = uint8_t(std::abs(int(first_mat.px[k]) - int(other.px[k])));
            }
        } else if (has_second) {
            second = std::move(second_mat);
        }
    }
}

DoubleExposureResult double_exposure_plan(const std::vector<double> &z_mm, const DoubleExposureParams &p, std::vector<double> &out_z,
                                          std::vector<DoubleExposurePiece> &pieces)
{
    if (const std::string err = validate_double_exposure(p, z_mm); !err.empty()) throw std::invalid_argument(err);
    const size_t count = z_mm.size();
    const Range r = resolve_range(p.first_layer, p.last_layer, count);
    DoubleExposureResult res;
    res.bottom_layer_count = p.bottom_layer_count;
    out_z.clear(); pieces.clear();
    auto push = [&](DoubleExposurePiece::Role role, double z, size_t src, double exposure, bool second, double lift, double wait) {
        out_z.push_back(z); pieces.push_back({role, src});
        res.list.source_index.push_back(src); res.exposure_s.push_back(exposure); res.is_second.push_back(second);
        res.lift_mm.push_back(lift); res.wait_before_cure_s.push_back(wait);
    };
    for (size_t i = 0; i < count; ++i) {
        if (i < r.first || i > r.last) { push(DoubleExposurePiece::Role::Copy, z_mm[i], i, -1, false, -1, -1); continue; }
        const bool bottom = i < p.bottom_layer_count;
        if (bottom) ++res.bottom_layer_count;
        push(DoubleExposurePiece::Role::First, z_mm[i], i, bottom ? p.first_bottom_exposure_s : p.first_normal_exposure_s, false, -1, -1);
        const bool own = p.different_settings_for_second_layer;
        push(DoubleExposurePiece::Role::Second, z_mm[i], i, bottom ? p.second_bottom_exposure_s : p.second_normal_exposure_s, true,
             own && p.second_layer_lift_enabled ? p.second_layer_lift_mm : -1.0, own && p.second_layer_wait_enabled ? p.second_layer_wait_s : -1.0);
    }
    return res;
}

DoubleExposureResult double_exposure_layers(std::vector<Mask> &layers, std::vector<double> &z_mm, const DoubleExposureParams &p, const LayerProgress &progress)
{
    if (const std::string err = validate_double_exposure(p, z_mm); !err.empty()) throw std::invalid_argument(err);
    check_z(layers, z_mm);
    const Range r = resolve_range(p.first_layer, p.last_layer, layers.size());
    const size_t count = r.last - r.first + 1;
    std::vector<Mask> firsts(count), seconds(count);
    parallel_layers(r.first, r.last, progress, [&](size_t i) { double_exposure_pair(layers[i], i, p, firsts[i - r.first], seconds[i - r.first]); });
    std::vector<double> out_z;
    std::vector<DoubleExposurePiece> pieces;
    DoubleExposureResult res = double_exposure_plan(z_mm, p, out_z, pieces);
    std::vector<Mask> out_masks;
    out_masks.reserve(pieces.size());
    for (const DoubleExposurePiece &pc : pieces) {
        if (pc.role == DoubleExposurePiece::Role::Copy) out_masks.push_back(std::move(layers[pc.source]));
        else if (pc.role == DoubleExposurePiece::Role::First) out_masks.push_back(std::move(firsts[pc.source - r.first]));
        else out_masks.push_back(std::move(seconds[pc.source - r.first]));
    }
    layers = std::move(out_masks);
    z_mm = std::move(out_z);
    return res;
}

// ---- Change resolution -------------------------------------------------------------------------

double pixel_pitch_ratio(double old_display_mm, size_t old_res, double new_display_mm, size_t new_res)
{
    if (new_res == 0 || old_res == 0 || old_display_mm <= 0 || new_display_mm <= 0) return 1.0;
    const double ratio = (old_display_mm / double(old_res)) / (new_display_mm / double(new_res));
    return std::round(ratio * 1000.0) / 1000.0;
}

std::string validate_change_resolution(const ChangeResolutionParams &p, size_t layer_count, size_t width, size_t height, const Rect &b)
{
    if (layer_count == 0) return "There are no layers.";
    if (p.new_width == 0 || p.new_height == 0) return "The new resolution must be greater than 0.\n";
    std::string sb;
    if (width == p.new_width && height == p.new_height)
        sb += "The new resolution must be different from current resolution (" + std::to_string(p.new_width) + " x " + std::to_string(p.new_height) + ").\n";
    const size_t fw = size_t(p.fix_ratio ? double(b.w) * p.ratio_x : double(b.w)), fh = size_t(p.fix_ratio ? double(b.h) * p.ratio_y : double(b.h));
    if (p.new_width < fw || p.new_height < fh)
        sb += "The new resolution (" + std::to_string(p.new_width) + " x " + std::to_string(p.new_height) + ") is not large enough to hold the model volume (" +
              std::to_string(fw) + " x " + std::to_string(fh) + "), continuing operation would clip the model.\n";
    return sb;
}

std::string validate_change_resolution(const ChangeResolutionParams &p, const std::vector<Mask> &layers)
{
    if (layers.empty()) return "There are no layers.";
    return validate_change_resolution(p, layers.size(), layers.front().width, layers.front().height, p.new_width == 0 || p.new_height == 0 ? Rect{} : bounding_rect(layers));
}

Mask change_resolution_layer(const Mask &in, const Rect &region, const ChangeResolutionParams &p)
{
    Mask dst(p.new_width, p.new_height);
    if (in.nonzero_count() > 0 && !region.empty()) {
        const Mask src = crop(in, region);
        // Centre the model bounds on the new canvas; validation guarantees the bounds fit.
        paste(dst, int((p.new_width - src.width) / 2), int((p.new_height - src.height) / 2), src);
        if (p.fix_ratio && (p.ratio_x != 1.0 || p.ratio_y != 1.0)) dst = transform_from_center(dst, p.ratio_x, p.ratio_y);
    }
    return dst;
}

void change_resolution_layers(std::vector<Mask> &layers, const ChangeResolutionParams &p, const LayerProgress &progress)
{
    if (const std::string err = validate_change_resolution(p, layers); !err.empty()) throw std::invalid_argument(err);
    check_uniform(layers);
    const Rect region = bounding_rect(layers);
    run_per_layer(layers, 0, layers.size() - 1, progress, [&](size_t, const Mask &in) { return change_resolution_layer(in, region, p); });
}


// =====================================================================================================
// Batch 2: per-layer parameter table operations
// =====================================================================================================

namespace {

// MathF.Round(x, digits) of .NET rounds half to even; std::nearbyint does the same in the default rounding mode.
double round_even(double v, int digits)
{
    const double k = std::pow(10.0, digits);
    return std::nearbyint(v * k) / k;
}
// MathF.Round(x, digits, MidpointRounding.AwayFromZero)
double round_away(double v, int digits)
{
    const double k = std::pow(10.0, digits);
    return std::round(v * k) / k;
}
// Math.Clamp semantics: NaN passes through, +-inf is clamped.
double clamp_net(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }
long long um(double mm) { return std::llround(mm * 1000.0); }

void check_table(const std::vector<Mask> &layers, const std::vector<LayerParams> &table)
{
    if (table.size() != layers.size()) throw std::invalid_argument("Every layer needs a print parameter set.");
}

bool has_nonzero(const Mask &m)
{
    for (uint8_t v : m.px) if (v) return true;
    return false;
}

Mask max_masks(const Mask &a, const Mask &b)
{
    Mask out = a;
    for (size_t i = 0; i < out.px.size(); ++i) out.px[i] = std::max(a.px[i], b.px[i]);
    return out;
}

// Mask with 255 where `m` > threshold (cv::threshold BINARY).
Mask binarize(const Mask &m, uint8_t threshold)
{
    Mask out(m.width, m.height);
    for (size_t i = 0; i < m.px.size(); ++i) out.px[i] = m.px[i] > threshold ? 255 : 0;
    return out;
}

// Pixel centre of the model bounds (UVtools BoundingRectangle.Center()): X + Width / 2.
void model_center(const std::vector<Mask> &layers, int &cx, int &cy)
{
    const Rect b = layers.empty() ? Rect{} : bounding_rect(layers);
    cx = b.x + b.w / 2;
    cy = b.y + b.h / 2;
}

// Blank layer with a single dim pixel so that "empty layer" detection does not drop it (UVtools CreateMatWithDummyPixel).
Mask dummy_pixel_mask(size_t w, size_t h, int x, int y, uint8_t brightness)
{
    Mask m(w, h);
    if (x >= 0 && y >= 0 && size_t(x) < w && size_t(y) < h) m.at(size_t(x), size_t(y)) = brightness;
    return m;
}

// cv::GaussianBlur with sigma = 0 on an 8-bit image, BORDER_REFLECT_101, separable, round to nearest.
Mask gaussian_blur(const Mask &m, int ksize)
{
    if (ksize <= 1 || m.px.empty()) return m;
    std::vector<double> k(static_cast<size_t>(ksize), 0.0);
    if (ksize == 3) k = {0.25, 0.5, 0.25};
    else if (ksize == 5) k = {0.0625, 0.25, 0.375, 0.25, 0.0625};
    else if (ksize == 7) k = {0.03125, 0.109375, 0.21875, 0.28125, 0.21875, 0.109375, 0.03125};
    else {
        const double sigma = 0.3 * ((ksize - 1) * 0.5 - 1.0) + 0.8;
        double sum = 0;
        for (int i = 0; i < ksize; ++i) { const double d = i - (ksize - 1) / 2.0; k[size_t(i)] = std::exp(-d * d / (2 * sigma * sigma)); sum += k[size_t(i)]; }
        for (double &v : k) v /= sum;
    }
    const long W = long(m.width), H = long(m.height), r = ksize / 2;
    auto reflect = [](long i, long n) { if (n == 1) return 0L; while (i < 0 || i >= n) i = i < 0 ? -i : 2 * (n - 1) - i; return i; };
    std::vector<double> tmp(m.px.size());
    for (long y = 0; y < H; ++y)
        for (long x = 0; x < W; ++x) {
            double s = 0;
            for (long d = -r; d <= r; ++d) s += k[size_t(d + r)] * m.px[size_t(y * W + reflect(x + d, W))];
            tmp[size_t(y * W + x)] = s;
        }
    Mask out(m.width, m.height);
    for (long y = 0; y < H; ++y)
        for (long x = 0; x < W; ++x) {
            double s = 0;
            for (long d = -r; d <= r; ++d) s += k[size_t(d + r)] * tmp[size_t(reflect(y + d, H) * W + x)];
            out.px[size_t(y * W + x)] = uint8_t(std::clamp(std::lround(s), 0L, 255L));
        }
    return out;
}

Mask open_mask(const Mask &m, int n) { return n > 0 ? dilate(erode(m, n), n) : m; }
Mask close_mask(const Mask &m, int n) { return n > 0 ? erode(dilate(m, n), n) : m; }

LayerParams first_of(const std::vector<LayerParams> &t, bool bottom)
{
    for (const LayerParams &p : t) if (p.is_bottom == bottom) return p;
    return t.empty() ? LayerParams{} : t.front();
}

} // namespace

double LayerParams::lift_height_total() const { return round2(lift_height_mm + lift_height2_mm); }
void   LayerParams::set_lift_height_total(double mm) { lift_height_mm = round2(mm); lift_height2_mm = 0; }
bool   LayerParams::operator==(const LayerParams &o) const
{
    return is_bottom == o.is_bottom && exposure_s == o.exposure_s && wait_before_cure_s == o.wait_before_cure_s &&
           wait_after_cure_s == o.wait_after_cure_s && wait_after_lift_s == o.wait_after_lift_s && lift_height_mm == o.lift_height_mm &&
           lift_speed == o.lift_speed && lift_height2_mm == o.lift_height2_mm && lift_speed2 == o.lift_speed2 &&
           retract_speed == o.retract_speed && retract_height2_mm == o.retract_height2_mm && retract_speed2 == o.retract_speed2 &&
           light_pwm == o.light_pwm;
}

std::vector<size_t> layer_pixel_counts(const std::vector<Mask> &layers)
{
    std::vector<size_t> counts(layers.size());
    tbb::parallel_for(tbb::blocked_range<size_t>(0, layers.size()), [&](const tbb::blocked_range<size_t> &r) {
        for (size_t i = r.begin(); i != r.end(); ++i) counts[i] = layers[i].nonzero_count();
    });
    return counts;
}

// ---- Dynamic lifts ----------------------------------------------------------------------------------

DynamicLiftsParams dynamic_lifts_defaults(const std::vector<LayerParams> &table, DynamicLiftsParams p)
{
    const LayerParams b = first_of(table, true), n = first_of(table, false);
    if (p.smallest_bottom_lift_height <= 0) p.smallest_bottom_lift_height = b.lift_height_total();
    if (p.largest_bottom_lift_height <= 0 || p.largest_bottom_lift_height < p.smallest_bottom_lift_height) p.largest_bottom_lift_height = p.smallest_bottom_lift_height;
    if (p.smallest_lift_height <= 0) p.smallest_lift_height = n.lift_height_total();
    if (p.largest_lift_height <= 0 || p.largest_lift_height < p.smallest_lift_height) p.largest_lift_height = p.smallest_lift_height;
    if (p.slowest_bottom_lift_speed <= 0) p.slowest_bottom_lift_speed = b.lift_speed;
    if (p.fastest_bottom_lift_speed <= 0 || p.fastest_bottom_lift_speed < p.slowest_bottom_lift_speed) p.fastest_bottom_lift_speed = p.slowest_bottom_lift_speed;
    if (p.slowest_lift_speed <= 0) p.slowest_lift_speed = n.lift_speed;
    if (p.fastest_lift_speed <= 0 || p.fastest_lift_speed < p.slowest_lift_speed) p.fastest_lift_speed = p.slowest_lift_speed;
    return p;
}

std::string validate_dynamic_lifts(const DynamicLiftsParams &p)
{
    std::string sb;
    if (p.smallest_bottom_lift_height > p.largest_bottom_lift_height) sb += "Smallest bottom lift height can't be higher than the largest.\n";
    if (p.slowest_bottom_lift_speed > p.fastest_bottom_lift_speed) sb += "Slowest bottom lift speed can't be higher than the fastest.\n";
    if (p.smallest_lift_height > p.largest_lift_height) sb += "Smallest lift height can't be higher than the largest.\n";
    if (p.slowest_lift_speed > p.fastest_lift_speed) sb += "Slowest lift speed can't be higher than the fastest.\n";
    if (p.smallest_bottom_lift_height == p.largest_bottom_lift_height && p.slowest_bottom_lift_speed == p.fastest_bottom_lift_speed &&
        p.smallest_lift_height == p.largest_lift_height && p.slowest_lift_speed == p.fastest_lift_speed)
        sb += "The selected smallest/largest settings are all equal and will not produce a change.\n";
    return sb;
}

void dynamic_lifts(const std::vector<Mask> &layers, std::vector<LayerParams> &table, const DynamicLiftsParams &p, const LayerProgress &progress)
{
    if (const std::string err = validate_dynamic_lifts(p); !err.empty()) throw std::invalid_argument(err);
    check_uniform(layers);
    check_table(layers, table);
    dynamic_lifts_from_counts(layer_pixel_counts(layers), table, p, progress);
}

void dynamic_lifts_from_counts(const std::vector<size_t> &counts, std::vector<LayerParams> &table, const DynamicLiftsParams &p, const LayerProgress &progress)
{
    if (const std::string err = validate_dynamic_lifts(p); !err.empty()) throw std::invalid_argument(err);
    if (counts.empty()) throw std::invalid_argument("There are no layers.");
    if (table.size() != counts.size()) throw std::invalid_argument("Every layer needs a print parameter set.");
    const Range r = resolve_range(p.first_layer, p.last_layer, counts.size());

    // Min / max pixel counts of the non-empty layers of the range, split by bottom / normal (0 when there are none,
    // which makes the divisions below NaN exactly like UVtools' swallowed exceptions leave them at 0).
    uint32_t min_px[2] = {0, 0}, max_px[2] = {0, 0}; // [0] normal, [1] bottom
    bool     seen[2] = {false, false};
    for (size_t i = r.first; i <= r.last; ++i) {
        if (counts[i] == 0) continue;
        const int k = table[i].is_bottom ? 1 : 0;
        const uint32_t c = uint32_t(counts[i]);
        if (!seen[k]) { min_px[k] = max_px[k] = c; seen[k] = true; }
        else { min_px[k] = std::min(min_px[k], c); max_px[k] = std::max(max_px[k], c); }
    }

    std::vector<LayerParams> out = table;
    check_layer_progress(progress, 0, r.last - r.first + 1);
    for (size_t i = r.first; i <= r.last; ++i) {
        // The lift AFTER layer i-1 is what lifts layer i-1 off the FEP, hence the previous layer's area drives it
        // (UVtools: calculateLayer = layer[i - 1]).
        const double area = double(uint32_t(counts[i == 0 ? 0 : i - 1]));
        const bool   bottom = table[i].is_bottom;
        const int    k = bottom ? 1 : 0;
        const double smallest_h = bottom ? p.smallest_bottom_lift_height : p.smallest_lift_height;
        const double largest_h  = bottom ? p.largest_bottom_lift_height : p.largest_lift_height;
        const double slowest_s  = bottom ? p.slowest_bottom_lift_speed : p.slowest_lift_speed;
        const double fastest_s  = bottom ? p.fastest_bottom_lift_speed : p.fastest_lift_speed;
        double height, speed;
        if (p.method == DynamicLiftsMethod::Traditional) {
            height = clamp_net(largest_h * area / double(max_px[k]), smallest_h, largest_h);
            speed  = clamp_net(fastest_s - fastest_s * area / double(max_px[k]), slowest_s, fastest_s);
        } else {
            const double ratio = double(uint32_t(uint32_t(area) - min_px[k])) / double(max_px[k] - min_px[k]); // 0..1
            height = clamp_net(smallest_h + (largest_h - smallest_h) * ratio, smallest_h, largest_h);
            speed  = clamp_net(fastest_s - (fastest_s - slowest_s) * ratio, slowest_s, fastest_s);
        }
        if (!std::isnan(height)) {
            out[i].retract_height2_mm = 0;
            out[i].set_lift_height_total(round_even(height, 1));
            if (!std::isnan(speed)) out[i].lift_speed = round2(round_even(speed, 1));
        }
        check_layer_progress(progress, i - r.first + 1, r.last - r.first + 1);
    }
    table = std::move(out);
}

// ---- Fade exposure time -----------------------------------------------------------------------------

FadeExposureParams fade_exposure_defaults(const std::vector<LayerParams> &table, FadeExposureParams p)
{
    if (p.from_exposure_s <= 0) p.from_exposure_s = first_of(table, true).exposure_s;
    if (p.to_exposure_s <= 0) p.to_exposure_s = first_of(table, false).exposure_s;
    return p;
}

double transition_step_time(double long_s, double short_s, size_t n)
{
    return n == 0 || long_s == short_s ? 0.0 : round_away((long_s - short_s) / double(n + 1), 2);
}

std::string validate_fade_exposure(const FadeExposureParams &p, size_t table_size)
{
    std::string sb;
    if (p.layer_count == 0) sb += "The layer count must be higher than 0.\n";
    if (p.from_exposure_s == p.to_exposure_s) sb += "The starting exposure time can't be the same as the ending exposure time.\n";
    if (p.layer_count > 0 && p.first_layer + p.layer_count > table_size) sb += "The fade range exceeds the number of layers.\n";
    return sb;
}

FadeExposureResult fade_exposure_time(std::vector<LayerParams> &table, const FadeExposureParams &p, const LayerProgress &progress)
{
    if (const std::string err = validate_fade_exposure(p, table.size()); !err.empty()) throw std::invalid_argument(err);
    FadeExposureResult res;
    // UVtools passes (to, from): the step is positive when fading up from `from` to `to`.
    res.increment_s = transition_step_time(p.to_exposure_s, p.from_exposure_s, p.layer_count);
    std::vector<LayerParams> out = table;
    double exposure = p.from_exposure_s;
    check_layer_progress(progress, 0, p.layer_count);
    for (size_t i = 0; i < p.layer_count; ++i) {
        exposure += res.increment_s;
        out[p.first_layer + i].exposure_s = round2(exposure);
        check_layer_progress(progress, i + 1, p.layer_count);
    }
    if (p.transition_type_firmware && p.disable_firmware_transition_layers) {
        res.transition_layer_count_changed = true; res.transition_layer_count = 0;
    } else if (p.bottom_layer_count == p.first_layer) {
        res.transition_layer_count_changed = true; res.transition_layer_count = p.layer_count;
    }
    table = std::move(out);
    return res;
}

// ---- Light bleed compensation -----------------------------------------------------------------------

std::vector<uint8_t> parse_dim_by(const std::string &text)
{
    std::vector<uint8_t> levels;
    size_t pos = 0;
    while (pos <= text.size()) {
        const size_t comma = std::min(text.find(',', pos), text.size());
        const std::string tok = trim_ws(text.substr(pos, comma - pos));
        pos = comma + 1;
        if (tok.empty() || tok.size() > 3 || !std::all_of(tok.begin(), tok.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); })) continue;
        const int v = std::stoi(tok);
        if (v <= 0 || v >= 255) continue; // UVtools skips byte.MinValue / byte.MaxValue and unparsable entries
        levels.push_back(uint8_t(v));
    }
    return levels;
}

std::string validate_light_bleed(const LightBleedParams &p)
{
    std::string sb;
    const std::vector<uint8_t> dims = parse_dim_by(p.dim_by);
    if (dims.empty()) sb += "The dim levels are invalid or not set.\n";
    int sum = 0;
    for (uint8_t d : dims) sum += d;
    if (sum >= 255) sb += "The sum of dim levels are producing black pixels.\n";
    return sb;
}

StreamKernel light_bleed_kernel(const LightBleedParams &p, const StreamLayout &l)
{
    if (const std::string err = validate_light_bleed(p); !err.empty()) throw std::invalid_argument(err);
    const Range r = resolve_range(p.selection.first_layer, p.selection.last_layer, l.layer_count);
    check_roi(p.selection.roi, l.width, l.height);
    const std::vector<uint8_t> dims = parse_dim_by(p.dim_by);
    const Rect  roi = p.selection.roi.empty() ? Rect{0, 0, int(l.width), int(l.height)} : p.selection.roi;
    const size_t W = l.width;
    const bool use_prev = p.lookup != LightBleedLookup::Next, use_next = p.lookup != LightBleedLookup::Previous;
    StreamKernel k;
    k.first = r.first; k.last = r.last;
    // Neighbours are only looked up inside the selected range: dims.size() layers on each side at most.
    k.before = use_prev ? dims.size() : 0;
    k.after  = use_next ? dims.size() : 0;

    k.run = [=](size_t li, const Mask &in, const LayerFetch &fetch) {
        // Neighbours are the original data (the result of one layer never feeds another).
        Mask m = in;
        for (size_t i = 0; i < dims.size(); ++i) {
            std::shared_ptr<const Mask> prev, next;
            if (use_prev && li >= r.first + i + 1) prev = fetch(li - i - 1);
            if (use_next && li + i + 1 <= r.last) next = fetch(li + i + 1);
            if (!prev && !next) break; // nothing more to look at
            for (int y = roi.y; y < roi.bottom(); ++y)
                for (int x = roi.x; x < roi.right(); ++x) {
                    const size_t idx = size_t(y) * W + size_t(x);
                    // Both neighbours are merged with a saturating add, like CvInvoke.Add on the masks.
                    const int neighbour = prev && next ? std::min(255, prev->px[idx] + next->px[idx]) : (prev ? prev->px[idx] : next->px[idx]);
                    bool dim;
                    switch (p.subject) {
                    case LightBleedSubject::Similarities: dim = neighbour != 0; break; // pixel continues from the neighbour
                    case LightBleedSubject::Bridges:      dim = neighbour == 0; break; // nothing below / above: a bridge
                    default:                              dim = true; break;           // Both: one of the two always applies
                    }
                    if (dim) m.px[idx] = sat_sub(m.px[idx], dims[i]);
                }
        }
        return m;
    };
    return k;
}

void light_bleed_compensation_layers(std::vector<Mask> &layers, const LightBleedParams &p, const LayerProgress &progress)
{
    if (const std::string err = validate_light_bleed(p); !err.empty()) throw std::invalid_argument(err);
    check_uniform(layers);
    run_kernel(layers, light_bleed_kernel(p, batch_layout(layers)), progress);
}

// ---- Phased exposure --------------------------------------------------------------------------------

PhasedExposureParams phased_exposure_defaults(const LayerParams &bottom, const LayerParams &normal, bool is_gcode)
{
    PhasedExposureParams p;
    p.phases = {{bottom.exposure_s, 10, normal.exposure_s, 4}, {normal.exposure_s, 0, normal.exposure_s, 0}};
    p.sequential_lift_mm = is_gcode ? 0 : 0.1;
    p.sequential_wait_before_cure_s = is_gcode ? 2 : 0;
    return p;
}

std::string validate_phased_exposure(const PhasedExposureParams &p, const std::vector<double> &z_mm)
{
    std::string sb;
    if (p.phases.size() < 2) sb += "This tool requires at least two phased exposures in order to run.\n";
    if (!z_mm.empty()) {
        const size_t last = std::min(p.last_layer, z_mm.size() - 1);
        for (size_t i = p.first_layer + 1; i <= last; ++i)
            if (z_mm[i] == z_mm[i - 1]) {
                sb += "The selected layer range already have modified layers with same z position, starting at layer " + std::to_string(i) + ". Not safe to continue.\n";
                break;
            }
    }
    for (size_t i = 0; i + 1 < p.phases.size(); ++i) {
        const PhasedExposureLevel &a = p.phases[i], &b = p.phases[i + 1];
        // Mirrors the UVtools comparison literally (including its bottom == normal exposure cross check).
        if (a.bottom_exposure_s == b.exposure_s && a.bottom_iterations == b.bottom_iterations && a.exposure_s == b.exposure_s && a.iterations == b.iterations)
            sb += "Duplicated exposure sequence #" + std::to_string(i + 1) + " == #" + std::to_string(i + 2) + ". Group of entries can't be duplicated.\n";
    }
    return sb;
}

std::vector<PhasedExposurePiece> phased_exposure_layer(const Mask &src, size_t li, double z, const LayerParams &layer_params,
                                                       const PhasedExposureParams &p, const PrinterContext &ctx)
{
    std::vector<PhasedExposurePiece> out;
    if (!has_nonzero(src)) { out.push_back({{}, true, z, layer_params, li}); return out; } // UVtools drops empty layers here; we keep them
    const bool bottom = layer_params.is_bottom;
    unsigned affected = 0;
    const PhasedExposureLevel *last_phase = &p.phases.front();
    for (const PhasedExposureLevel &ph : p.phases) {
        const unsigned iterations = bottom ? ph.bottom_iterations : ph.iterations;
        bool set_mask = false;
        Mask new_mask = src;
        if (iterations > 0) {
            new_mask = erode(src, int(iterations));
            if (!has_nonzero(new_mask)) continue; // would print a black layer: skip this phase
            set_mask = true;
        }
        if (affected > 0 && p.difference_only) {
            // Later phases only cure what the previous phase did not reach: remove the area that the previous
            // phase eroded by (overlap + its iterations).
            const unsigned overlap = p.difference_overlap_iterations + (bottom ? last_phase->bottom_iterations : last_phase->iterations);
            if (overlap > iterations) {
                const Mask overlap_mask = erode(src, int(overlap));
                if (has_nonzero(overlap_mask)) {
                    for (size_t i = 0; i < new_mask.px.size(); ++i) new_mask.px[i] = sat_sub(new_mask.px[i], overlap_mask.px[i]);
                    // UVtools quirk kept on purpose: an empty difference leaves the clone's ORIGINAL mask in place.
                    set_mask = has_nonzero(new_mask);
                }
            }
        }
        PhasedExposurePiece pc{set_mask ? std::move(new_mask) : Mask{}, !set_mask, z, layer_params, li};
        pc.params.exposure_s = round2(bottom ? ph.bottom_exposure_s : ph.exposure_s);
        if (p.different_settings_for_sequential_layers && affected > 0) {
            if (p.sequential_lift_enabled) pc.params.set_lift_height_total(p.sequential_lift_mm);
            if (p.sequential_wait_before_cure_enabled && ctx.can_use_wait_before_cure) pc.params.wait_before_cure_s = round2(p.sequential_wait_before_cure_s);
        }
        out.push_back(std::move(pc));
        ++affected;
        last_phase = &ph;
    }
    if (affected == 0) { out.clear(); out.push_back({{}, true, z, layer_params, li}); } // erosion removed everything: keep the original layer
    return out;
}

PhasedExposureResult phased_exposure_layers(std::vector<Mask> &layers, std::vector<double> &z_mm, std::vector<LayerParams> &table,
                                            const PhasedExposureParams &p, const PrinterContext &ctx, const LayerProgress &progress)
{
    if (const std::string err = validate_phased_exposure(p, z_mm); !err.empty()) throw std::invalid_argument(err);
    check_z(layers, z_mm);
    check_table(layers, table);
    const Range r = resolve_range(p.first_layer, p.last_layer, layers.size());

    std::vector<std::vector<PhasedExposurePiece>> pieces(r.last - r.first + 1);
    parallel_layers(r.first, r.last, progress, [&](size_t li) { pieces[li - r.first] = phased_exposure_layer(layers[li], li, z_mm[li], table[li], p, ctx); });

    std::vector<Mask> om; std::vector<double> oz; std::vector<LayerParams> op;
    PhasedExposureResult res;
    auto push = [&](Mask m, double z, const LayerParams &lp, size_t src) {
        om.push_back(std::move(m)); oz.push_back(z); op.push_back(lp); res.list.source_index.push_back(src);
    };
    for (size_t i = 0; i < r.first; ++i) push(layers[i], z_mm[i], table[i], i);
    for (auto &vec : pieces) for (PhasedExposurePiece &pc : vec) push(pc.original ? layers[pc.source] : std::move(pc.mask), pc.z, pc.params, pc.source);
    for (size_t i = r.last + 1; i < layers.size(); ++i) push(layers[i], z_mm[i], table[i], i);
    for (const LayerParams &lp : op) res.bottom_layer_count += lp.is_bottom;
    layers = std::move(om); z_mm = std::move(oz); table = std::move(op);
    return res;
}

// ---- Raise on print finish --------------------------------------------------------------------------

std::string validate_raise_on_print_finish(const RaiseOnPrintFinishParams &p, const std::vector<double> &z_mm, const PrinterContext &ctx)
{
    std::string sb;
    if (z_mm.empty()) return "There are no layers.\n";
    if (z_mm.size() >= 2) {
        const double last_height = round_height(z_mm.back() - z_mm[z_mm.size() - 2]);
        const double criteria = std::max(0.2, ctx.layer_height_mm); // Layer.MaximumHeightFloat
        if (last_height > criteria)
            sb += "With a difference of " + std::to_string(last_height) + "mm between the last two layers, it looks like this tool had already been applied.\n";
    }
    const double target = p.position_z_mm > 0 ? p.position_z_mm : ctx.machine_z_mm;
    const double print_height = z_mm.back();
    if (target < print_height) sb += "Can't raise to " + std::to_string(target) + "mm, because it's below the maximum print height of " + std::to_string(print_height) + "mm.\n";
    else if (target == print_height) sb += "Raise to " + std::to_string(target) + "mm will have no effect because it's the same height as last layer.\n";
    return sb;
}

RaiseDummyLayer raise_on_print_finish_plan(size_t layer_count, size_t width, size_t height, std::vector<double> &z_mm, std::vector<LayerParams> &table,
                                           const RaiseOnPrintFinishParams &p, const PrinterContext &ctx,
                                           const std::function<void(size_t &, size_t &, bool &)> &last_layer_first_pixel,
                                           const std::function<void(int &, int &)> &model_center_of)
{
    if (const std::string err = validate_raise_on_print_finish(p, z_mm, ctx); !err.empty()) throw std::invalid_argument(err);
    if (z_mm.size() != layer_count) throw std::invalid_argument("Every layer needs a Z position.");
    if (table.size() != layer_count) throw std::invalid_argument("Every layer needs a parameter row.");
    LayerParams lp = table.back();
    const double target = p.position_z_mm > 0 ? p.position_z_mm : ctx.machine_z_mm;
    lp.exposure_s = ctx.is_gcode ? 0.0 : 0.05; // very short exposure; the layer only exists to move the platform
    lp.light_pwm = 0;                            // keep the light off if the printer supports it
    lp.wait_before_cure_s = lp.wait_after_cure_s = lp.wait_after_lift_s = 0;
    if (p.wait_time_s > 0 && ctx.can_use_wait_before_cure) lp.wait_before_cure_s = round2(p.wait_time_s);

    RaiseDummyLayer dummy{width, height, p.output_dummy_pixel, -1, -1, uint8_t(ctx.is_gcode ? 1 : 128)};
    if (p.output_dummy_pixel) {
        int x = -1, y = -1;
        size_t px = 0, py = 0; bool found = false;
        last_layer_first_pixel(px, py, found);
        if (found) { x = int(px); y = int(py); }
        if (x <= 0 || y <= 0) model_center_of(x, y); // UVtools: location <= 0 falls back to the bounds centre
        dummy.x = x; dummy.y = y;
    }
    z_mm.push_back(round_height(target));
    table.push_back(lp);
    return dummy;
}

Mask RaiseDummyLayer::make() const
{
    return dummy_pixel ? dummy_pixel_mask(width, height, x, y, brightness) : Mask(width, height);
}

void raise_on_print_finish(std::vector<Mask> &layers, std::vector<double> &z_mm, std::vector<LayerParams> &table,
                           const RaiseOnPrintFinishParams &p, const PrinterContext &ctx)
{
    if (const std::string err = validate_raise_on_print_finish(p, z_mm, ctx); !err.empty()) throw std::invalid_argument(err);
    check_z(layers, z_mm);
    check_table(layers, table);
    const size_t w = layers.front().width, h = layers.front().height;
    const RaiseDummyLayer dummy = raise_on_print_finish_plan(layers.size(), w, h, z_mm, table, p, ctx,
        [&](size_t &x, size_t &y, bool &found) {
            const Mask &last = layers.back();
            for (size_t i = 0; i < last.px.size(); ++i)
                if (last.px[i]) { x = i % w; y = i / w; found = true; break; }
        },
        [&](int &x, int &y) { model_center(layers, x, y); });
    layers.push_back(dummy.make());
}

// ---- Stir resin -------------------------------------------------------------------------------------

std::string validate_stir_resin(const StirResinParams &p, const PrinterContext &ctx)
{
    std::string sb;
    if (p.stirs == 0) sb += "The number of stirs must be greater than 0.\n";
    if (!ctx.same_layer_z_allowed) {
        const double last = round_height(double(p.stirs) * 0.001);
        if (last >= round2(p.lift_height_mm)) {
            sb += "Your printer and/or file format does not support layers in same position, as so, each stir will increment in height by 0.001mm.\n";
            sb += "> Your current settings (" + std::to_string(p.stirs) + " stirs x 0.001mm) is equal or greater than the defined lift height of the lift height.\n";
            sb += "> Please lower your stir number up to " + std::to_string(int(round2(p.lift_height_mm) / 0.001 - 1)) + " stirs.\n";
        }
        if (p.method == StirMethod::StirAndPrint)
            sb += "Your printer and/or file format is not compatible with the method StirAndPrint. Please select another method.\n";
    }
    if (p.exposure_s < 0 || p.lift_height_mm < 0 || p.lift_speed < 0 || p.retract_speed < 0 || p.wait_time_before_stir_s < 0 || p.wait_time_after_lift_s < 0)
        sb += "Negative values are not allowed.\n";
    return sb;
}

namespace {
StirResinParams resolve_stir(StirResinParams p, const PrinterContext &ctx)
{
    p.exposure_s = round2(p.exposure_s > 0 ? p.exposure_s : (ctx.is_gcode ? 0.0 : 0.01));
    p.lift_height_mm = round2(p.lift_height_mm);
    p.lift_speed = round2(p.lift_speed > 0 ? p.lift_speed : ctx.maximum_speed);
    p.retract_speed = round2(p.retract_speed > 0 ? p.retract_speed : p.lift_speed);
    p.wait_time_before_stir_s = round2(p.wait_time_before_stir_s);
    p.wait_time_after_lift_s = round2(p.wait_time_after_lift_s);
    return p;
}
} // namespace

double stir_resin_seconds(const StirResinParams &in, const PrinterContext &ctx)
{
    const StirResinParams p = resolve_stir(in, ctx);
    return light_off_delay_seconds(p.lift_height_mm, p.lift_speed, p.retract_speed, p.exposure_s + p.wait_time_before_stir_s + p.wait_time_after_lift_s) * double(p.stirs);
}

bool is_dummy_layer(size_t non_zero_pixels, double exposure_s) { return non_zero_pixels <= 1 || exposure_s <= 0.01; }

RaiseDummyLayer stir_resin_plan(size_t layer_count, size_t width, size_t height, std::vector<double> &z_mm, std::vector<LayerParams> &table,
                                const StirResinParams &in, const PrinterContext &ctx, const LayerParams &base_row,
                                const std::function<bool(size_t)> &is_dummy,
                                const std::function<void(size_t &, size_t &, bool &)> &first_layer_first_pixel,
                                const std::function<void(int &, int &)> &model_center_of,
                                std::vector<size_t> &source_index, size_t &first_kept)
{
    if (const std::string err = validate_stir_resin(in, ctx); !err.empty()) throw std::invalid_argument(err);
    if (z_mm.size() != layer_count || table.size() != layer_count) throw std::invalid_argument("Every layer needs a Z position and a parameter row.");
    const StirResinParams p = resolve_stir(in, ctx);
    LayerParams row = base_row;
    row.is_bottom = false;
    row.exposure_s = p.exposure_s;
    row.set_lift_height_total(p.lift_height_mm);
    row.lift_speed = p.lift_speed;
    row.retract_speed = p.retract_speed;
    row.wait_after_cure_s = 0;
    row.wait_after_lift_s = ctx.can_use_wait_after_lift ? p.wait_time_after_lift_s : row.wait_after_lift_s;
    row.light_pwm = ctx.is_gcode ? 0 : 1;
    row.wait_before_cure_s = ctx.can_use_wait_before_cure ? p.wait_time_before_stir_s : row.wait_before_cure_s;

    RaiseDummyLayer dummy{width, height, p.output_dummy_pixel, -1, -1, uint8_t(ctx.is_gcode ? 1 : 128)};
    if (p.output_dummy_pixel && layer_count > 0) {
        int x = -1, y = -1;
        size_t px = 0, py = 0; bool found = false;
        first_layer_first_pixel(px, py, found);
        if (found) { x = int(px); y = int(py); }
        if (x <= 0 || y <= 0) model_center_of(x, y);
        dummy.x = x; dummy.y = y;
    }

    constexpr double kStirZ = 0.001;
    std::vector<double> out_z;
    std::vector<LayerParams> out_table;
    source_index.clear();
    first_kept = 0;
    if (p.method == StirMethod::StirAndPrint)
        while (first_kept < layer_count && is_dummy(first_kept)) ++first_kept; // a previous stir block
    const size_t derive = first_kept < layer_count ? first_kept : 0;
    for (unsigned i = 0; i < p.stirs; ++i) {
        out_z.push_back(ctx.same_layer_z_allowed ? kStirZ : round_height(kStirZ * double(i + 1)));
        out_table.push_back(row);
        source_index.push_back(derive);
    }
    if (p.method == StirMethod::StirAndPrint)
        for (size_t i = first_kept; i < layer_count; ++i) {
            out_z.push_back(z_mm[i]); out_table.push_back(table[i]); source_index.push_back(i);
        }
    z_mm = std::move(out_z);
    table = std::move(out_table);
    return dummy;
}

// ---- Layer import -----------------------------------------------------------------------------------

namespace {
bool import_extends(LayerImportType t)
{
    return t == LayerImportType::Replace || t == LayerImportType::Stack || t == LayerImportType::MergeSum || t == LayerImportType::MergeMax;
}
bool import_needs_equal_resolution(LayerImportType t)
{
    return t != LayerImportType::Insert && t != LayerImportType::Replace && t != LayerImportType::Stack;
}
// Result pixel of a merge-like import: `a` = target, `b` = imported.
uint8_t import_pixel(LayerImportType t, uint8_t a, uint8_t b)
{
    switch (t) {
    case LayerImportType::MergeSum: return uint8_t(std::min(255, int(a) + int(b)));
    case LayerImportType::MergeMax: return std::max(a, b);
    case LayerImportType::Subtract: return uint8_t(std::max(0, int(a) - int(b)));
    case LayerImportType::AbsDiff: return uint8_t(std::abs(int(a) - int(b)));
    case LayerImportType::BitwiseAnd: return uint8_t(a & b);
    case LayerImportType::BitwiseOr: return uint8_t(a | b);
    case LayerImportType::BitwiseXor: return uint8_t(a ^ b);
    default: return b;
    }
}
} // namespace

std::string validate_layer_import(const LayerImportParams &p, size_t layer_count, size_t width, size_t height)
{
    std::string sb;
    if (p.sources.empty()) return "No files to import.\n";
    for (const LayerImportSource &src : p.sources) {
        if (src.layers.empty()) { sb += "The file '" + src.name + "' has no layers.\n"; continue; }
        for (const Mask &m : src.layers)
            if (m.width != src.layers.front().width || m.height != src.layers.front().height || m.px.size() != m.width * m.height) {
                sb += "The layers of '" + src.name + "' do not have one common size.\n";
                break;
            }
    }
    const bool may_append = p.type == LayerImportType::Insert || (p.extend_beyond_layer_count && import_extends(p.type));
    if (p.start_layer > layer_count || (p.start_layer == layer_count && !may_append))
        sb += "The start layer " + std::to_string(p.start_layer) + " is outside of the document (" + std::to_string(layer_count) + " layers).\n";
    if (width == 0 || height == 0) sb += "The document has no resolution.\n";
    return sb;
}

bool layer_import_changes_list(const LayerImportParams &p, size_t layer_count)
{
    if (p.type == LayerImportType::Insert) return true;
    size_t longest = 0;
    for (const auto &src : p.sources) longest = std::max(longest, src.layers.size());
    if (p.extend_beyond_layer_count && import_extends(p.type) && p.start_layer + longest > layer_count) return true;
    return p.discard_unmodified_layers && p.start_layer + longest < layer_count;
}

LayerImportResult layer_import_layers(std::vector<Mask> &layers, std::vector<double> &z_mm, const LayerImportParams &p, const LayerProgress &progress)
{
    check_uniform(layers);
    if (layers.empty()) throw std::invalid_argument("There are no layers.");
    if (z_mm.size() != layers.size()) throw std::invalid_argument("Every layer needs a Z position.");
    const size_t W = layers.front().width, H = layers.front().height;
    if (const std::string err = validate_layer_import(p, layers.size(), W, H); !err.empty()) throw std::invalid_argument(err);

    std::vector<Mask> work = layers;
    std::vector<double> z = z_mm;
    std::vector<size_t> source(work.size());
    for (size_t i = 0; i < source.size(); ++i) source[i] = i;
    const double default_pitch = z.size() > 1 && z.back() > z[z.size() - 2] ? z.back() - z[z.size() - 2] : (z.empty() ? 0.05 : std::max(z.front(), 0.001));

    LayerImportResult result;
    result.total = p.sources.size();
    long last_processed = -1;
    if (p.type == LayerImportType::Stack) {
        MoveParams mp; mp.anchor = Anchor::TopLeft;
        move_layers(work, mp);
    }
    size_t done = 0;
    for (const LayerImportSource &src : p.sources) {
        check_layer_progress(progress, done++, p.sources.size());
        const size_t sw = src.layers.front().width, sh = src.layers.front().height;
        const bool same_resolution = sw == W && sh == H;
        const Rect bounds = bounding_rect(work), fb = bounding_rect(src.layers);
        Rect roi;
        const long n = long(src.layers.size());
        if (import_needs_equal_resolution(p.type) || p.type == LayerImportType::MergeSum || p.type == LayerImportType::MergeMax) {
            if (!same_resolution) continue;
        } else if (!same_resolution && (W < size_t(fb.w) || H < size_t(fb.h))) {
            continue;
        }
        if (p.type == LayerImportType::Stack) {
            int x = 0, y = 0;
            const long right = long(bounds.right()) + long(p.stack_margin) + long(fb.w);
            if (right >= 0 && right < long(W)) x = bounds.right() + int(p.stack_margin);
            else y = bounds.bottom() + int(p.stack_margin);
            if (x + fb.w < 0 || size_t(x + fb.w) >= W) continue; // UVtools IsPixelInsideXBounds: strictly inside
            if (y + fb.h < 0 || size_t(y + fb.h) >= H) continue;
            roi = {x, y, fb.w, fb.h};
        }
        if (p.type == LayerImportType::Insert) {
            // F.LayerCount blank layers are inserted at start; later layers move up in Z.
            const size_t at = p.start_layer;
            const double before = at > 0 ? z[at - 1] : 0.0;
            double step = (at < z.size() ? z[at] - before : default_pitch);
            if (!(step > 0)) step = default_pitch;
            const size_t derive = at > 0 ? at - 1 : 0;
            std::vector<double> ins_z(static_cast<size_t>(n), 0.0);
            for (long k = 0; k < n; ++k) ins_z[size_t(k)] = round_height(before + double(k + 1) * step);
            work.insert(work.begin() + ptrdiff_t(at), size_t(n), Mask(W, H));
            source.insert(source.begin() + ptrdiff_t(at), size_t(n), derive);
            z.insert(z.begin() + ptrdiff_t(at), ins_z.begin(), ins_z.end());
            for (size_t i = at + size_t(n); i < z.size(); ++i) z[i] = round_height(z[i] + double(n) * step);
        } else if (p.extend_beyond_layer_count && import_extends(p.type)) {
            const long diff = long(p.start_layer) + n - long(work.size());
            if (diff > 0) {
                double step = work.size() > 1 && z[work.size() - 1] > z[work.size() - 2] ? z[work.size() - 1] - z[work.size() - 2] : default_pitch;
                const size_t old = work.size();
                for (long k = 0; k < diff; ++k) {
                    work.emplace_back(W, H);
                    source.push_back(old - 1);
                    z.push_back(round_height(z[old - 1] + double(k + 1) * step));
                }
            }
        }
        ++result.imported;
        for (long i = 0; i < n; ++i) {
            const size_t li = p.start_layer + size_t(i);
            if (li >= work.size()) continue;
            const Mask &im = src.layers[size_t(i)];
            Mask &dst = work[li];
            switch (p.type) {
            case LayerImportType::Insert:
            case LayerImportType::Replace:
                if (same_resolution) dst = im;
                else {
                    Mask canvas(W, H);
                    if (!fb.empty()) paste(canvas, int(W / 2) - fb.w / 2, int(H / 2) - fb.h / 2, crop(im, fb));
                    dst = std::move(canvas);
                }
                break;
            case LayerImportType::Stack:
                if (!fb.empty()) paste(dst, roi.x, roi.y, crop(im, fb));
                break;
            default:
                for (size_t k = 0; k < dst.px.size(); ++k) dst.px[k] = import_pixel(p.type, dst.px[k], im.px[k]);
                break;
            }
            last_processed = std::max(last_processed, long(li));
        }
    }
    if (last_processed < 0) {
        throw std::invalid_argument("Nothing was imported: " + std::to_string(p.sources.size() - result.imported) + " of " + std::to_string(p.sources.size()) +
                                    " file(s) do not fit the layers (insufficient or unequal build space).\n");
    }
    if (p.type == LayerImportType::Stack) {
        MoveParams mp; // MiddleCenter
        move_layers(work, mp);
    }
    if (p.discard_unmodified_layers && size_t(last_processed) + 1 < work.size()) {
        work.resize(size_t(last_processed) + 1);
        source.resize(work.size());
        z.resize(work.size());
    }
    check_layer_progress(progress, p.sources.size(), p.sources.size());
    if (result.imported != result.total)
        result.report = "Could not import and fit " + std::to_string(result.total - result.imported) + " out of " + std::to_string(result.total) +
                        " format(s) (Insufficient or unequal build space).";
    result.list.source_index = std::move(source);
    layers = std::move(work);
    z_mm = std::move(z);
    return result;
}

// ---- Edit parameters --------------------------------------------------------------------------------

bool LayerParamEdit::any() const
{
    return position_z_mm || exposure_s || wait_before_cure_s || wait_after_cure_s || wait_after_lift_s || lift_height_mm || lift_speed ||
           lift_height2_mm || lift_speed2 || retract_speed || retract_height2_mm || retract_speed2 || light_pwm;
}

std::string validate_edit_parameters(const EditParametersParams &p)
{
    std::string sb;
    if (!p.edit.any() && !p.bottom.any()) sb += "Nothing changed\nDo some changes or cancel the operation.\n";
    // The range size is unknown for "to the last layer"; treat it as large so the alternating pattern is rejected.
    const uint64_t count = p.last_layer == kLastLayer ? std::numeric_limits<uint64_t>::max() : uint64_t(p.last_layer - p.first_layer + 1);
    if (p.per_layer_override && p.edit.position_z_mm && p.skip_layers > 0 && count > 1 && uint64_t(p.set_layers) + p.skip_layers < count)
        sb += "Can not change the PositionZ in layers with an active alternating pattern.\n";
    for (const LayerParamEdit *e : {&p.edit, &p.bottom}) {
        for (const std::optional<double> *v : {&e->exposure_s, &e->wait_before_cure_s, &e->wait_after_cure_s, &e->wait_after_lift_s, &e->lift_height_mm,
                                              &e->lift_speed, &e->lift_height2_mm, &e->lift_speed2, &e->retract_speed, &e->retract_height2_mm, &e->retract_speed2})
            if (*v && **v < 0) { sb += "Negative values are not allowed.\n"; break; }
    }
    return sb;
}

namespace {
void apply_edit(LayerParams &lp, double *z, const LayerParamEdit &e)
{
    if (e.position_z_mm && z) *z = round2(*e.position_z_mm);
    if (e.exposure_s) lp.exposure_s = round2(*e.exposure_s);
    if (e.wait_before_cure_s) lp.wait_before_cure_s = round2(*e.wait_before_cure_s);
    if (e.wait_after_cure_s) lp.wait_after_cure_s = round2(*e.wait_after_cure_s);
    if (e.wait_after_lift_s) lp.wait_after_lift_s = round2(*e.wait_after_lift_s);
    if (e.lift_height_mm) lp.lift_height_mm = round2(*e.lift_height_mm);
    if (e.lift_speed) lp.lift_speed = round2(*e.lift_speed);
    if (e.lift_height2_mm) lp.lift_height2_mm = round2(*e.lift_height2_mm);
    if (e.lift_speed2) lp.lift_speed2 = round2(*e.lift_speed2);
    if (e.retract_speed) lp.retract_speed = round2(*e.retract_speed);
    if (e.retract_speed2) lp.retract_speed2 = round2(*e.retract_speed2);
    if (e.retract_height2_mm) lp.retract_height2_mm = round2(*e.retract_height2_mm);
    if (e.light_pwm) lp.light_pwm = *e.light_pwm;
    // UVtools RetractHeight2 setter: clamped to [0, total lift] and re-sanitised whenever a lift changes.
    lp.retract_height2_mm = std::clamp(lp.retract_height2_mm, 0.0, lp.lift_height_total());
}
} // namespace

void edit_parameters(std::vector<double> &z_mm, std::vector<LayerParams> &table, const EditParametersParams &p, const LayerProgress &progress)
{
    if (const std::string err = validate_edit_parameters(p); !err.empty()) throw std::invalid_argument(err);
    if (z_mm.size() != table.size()) throw std::invalid_argument("Every layer needs a Z position.");
    const Range r = resolve_range(p.first_layer, p.last_layer, table.size());
    std::vector<LayerParams> out = table;
    std::vector<double> oz = z_mm;
    const size_t total = r.last - r.first + 1;
    check_layer_progress(progress, 0, total);
    if (p.per_layer_override) {
        unsigned set_layers = 0;
        for (size_t i = r.first; i <= r.last; ++i) {
            apply_edit(out[i], &oz[i], p.edit);
            check_layer_progress(progress, i - r.first + 1, total);
            if (p.skip_layers == 0) continue;
            if (++set_layers >= std::max(1u, p.set_layers)) { set_layers = 0; i += p.skip_layers; }
        }
    } else {
        // Global edit: UVtools writes the file level values and propagates them to the layers by bottom / normal.
        for (size_t i = r.first; i <= r.last; ++i) apply_edit(out[i], nullptr, out[i].is_bottom ? p.bottom : p.edit);
    }
    table = std::move(out);
    z_mm = std::move(oz);
}

// ---- Timelapse --------------------------------------------------------------------------------------

namespace {
Range timelapse_range(const TimelapseParams &p, size_t count) { return resolve_range(p.first_layer, p.last_layer, count); }
double timelapse_target(const TimelapseParams &p, const PrinterContext &c) { return p.raise_position_z_mm > 0 ? p.raise_position_z_mm : c.machine_z_mm; }
} // namespace

size_t timelapse_lift_count(const TimelapseParams &p, const std::vector<double> &z_mm, const PrinterContext &ctx)
{
    if (z_mm.empty()) return 0;
    const Range r = timelapse_range(p, z_mm.size());
    const bool extra = p.ensure_last_layer && p.mode == TimelapseMode::VirtualLayer;
    if (p.raise_each_nth_height_mm <= 0) return r.last - r.first + 1;
    const double height = z_mm[r.last] - z_mm[r.first];
    if (height <= 0) return extra ? 1 : 0;
    return size_t(height / std::max(ctx.layer_height_mm, p.raise_each_nth_height_mm)) + (extra ? 1 : 0);
}

std::string validate_timelapse(const TimelapseParams &p, const std::vector<double> &z_mm, const PrinterContext &ctx)
{
    std::string sb;
    if (z_mm.empty()) return "There are no layers.\n";
    if (timelapse_lift_count(p, z_mm, ctx) == 0) sb += "The selected settings does not generate any lift for the timelapse.\n";
    return sb;
}

TimelapseResult timelapse_plan(size_t layer_count, size_t width, size_t height, std::vector<double> &z_mm, std::vector<LayerParams> &table,
                               const TimelapseParams &p, const PrinterContext &ctx, const std::function<void(int &, int &)> &model_center_of,
                               TimelapseVirtualLayer &virtual_layer, const LayerProgress &progress)
{
    if (const std::string err = validate_timelapse(p, z_mm, ctx); !err.empty()) throw std::invalid_argument(err);
    if (z_mm.size() != layer_count) throw std::invalid_argument("Every layer needs a Z position.");
    if (table.size() != layer_count) throw std::invalid_argument("Every layer needs a parameter row.");
    const size_t layers_size = layer_count;
    const Range r = timelapse_range(p, layers_size);
    const double raise_z = timelapse_target(p, ctx);
    const bool custom = p.use_custom_lift;

    std::vector<LayerParams> tb = table; // staged: the caller's table is only replaced at the end
    std::vector<size_t> virtual_layers;
    double checkpoint = z_mm[0];
    const size_t total = r.last - r.first + 1;
    check_layer_progress(progress, 0, total);
    for (size_t i = r.first; i <= r.last; ++i) {
        check_layer_progress(progress, i - r.first, total);
        if (z_mm[i] >= raise_z) break; // passed the target height
        if (p.mode == TimelapseMode::VirtualLayer && p.ensure_last_layer && i == r.last) { virtual_layers.push_back(i + 1); break; }
        if (p.raise_each_nth_height_mm > 0 && round_height(z_mm[i] - checkpoint) < p.raise_each_nth_height_mm) continue;
        checkpoint = z_mm[i];
        LayerParams &lp = tb[i];
        if (p.mode == TimelapseMode::LiftHeight) {
            if (custom) {
                lp.lift_speed = round2(p.lift_speed);
                lp.retract_speed = round2(p.retract_speed);
                if (ctx.can_use_lift_height2) lp.lift_height_mm = round2(p.slow_lift_height_mm);
                if (ctx.can_use_lift_speed2) lp.lift_speed2 = round2(p.lift_speed2);
                if (ctx.can_use_retract_height2) lp.retract_height2_mm = round2(p.slow_retract_height_mm);
                if (ctx.can_use_retract_speed2) lp.retract_speed2 = round2(p.retract_speed2);
            }
            if (ctx.can_use_lift_height2 && (lp.lift_height2_mm > 0 || (custom && p.slow_lift_height_mm > 0))) // two stage lift (TSMC)
                lp.lift_height2_mm = std::max(0.0, round2(raise_z - z_mm[i] - lp.lift_height_mm));
            else
                lp.set_lift_height_total(std::max(lp.lift_height_total(), raise_z - z_mm[i]));
            if (ctx.can_use_wait_after_lift && p.wait_after_lift_s > 0) lp.wait_after_lift_s = round2(p.wait_after_lift_s);
            lp.retract_height2_mm = std::clamp(lp.retract_height2_mm, 0.0, lp.lift_height_total());
        } else {
            virtual_layers.push_back(i);
        }
    }

    TimelapseResult res;
    if (p.mode != TimelapseMode::VirtualLayer || virtual_layers.empty()) {
        table = std::move(tb);
        for (size_t i = 0; i < layers_size; ++i) res.list.source_index.push_back(i);
        res.inserted.assign(layers_size, false);
        return res;
    }

    const double min_lift = custom ? std::min(p.lift_speed, p.lift_speed2) : ctx.min_normal_speed;
    const double min_retract = custom ? std::min(p.retract_speed, p.retract_speed2) : ctx.min_normal_speed;
    const double max_lift = custom ? std::max(p.lift_speed, p.lift_speed2) : ctx.maximum_speed;
    const double max_retract = custom ? std::max(p.retract_speed, p.retract_speed2) : ctx.maximum_speed;

    int cx = 0, cy = 0;
    if (p.output_dummy_pixel) model_center_of(cx, cy);
    virtual_layer = {width, height, p.output_dummy_pixel, cx, cy, uint8_t(ctx.is_gcode ? 1 : 128)};

    LayerParams slow;
    slow.light_pwm = ctx.is_gcode ? 0 : 1;
    slow.exposure_s = ctx.is_gcode ? 0.0 : 0.01;
    slow.set_lift_height_total(ctx.is_gcode ? 0.0 : 0.1);
    slow.lift_speed = round2(min_lift);
    slow.lift_speed2 = round2(max_lift);
    slow.retract_speed = round2(max_retract);
    slow.retract_speed2 = round2(max_retract);
    slow.retract_height2_mm = 0;
    LayerParams photo = slow;
    photo.exposure_s = round2(p.exposure_s > 0 ? p.exposure_s : (ctx.is_gcode ? 0.0 : 0.05));
    photo.lift_speed = round2(max_lift);
    slow.light_pwm = 0; // the slow lift layer must not expose

    std::vector<double> oz = z_mm; std::vector<LayerParams> op = tb;
    res.list.source_index.resize(layers_size);
    for (size_t i = 0; i < layers_size; ++i) res.list.source_index[i] = i;
    res.inserted.assign(layers_size, false);
    double slow_lift_height = custom ? p.slow_lift_height_mm : ctx.global_lift_height_mm;
    size_t inserted_count = 0;
    for (size_t at : virtual_layers) {
        if (at < r.last) {
            // The real layer that follows the photo "lifts" back down: turn its lift into the slow retract.
            LayerParams &next = op[at + inserted_count];
            if (ctx.can_use_retract_height2 && next.retract_height2_mm > 0) next.set_lift_height_total(next.retract_height2_mm);
            next.retract_height2_mm = 0;
            next.lift_speed = round2(max_retract);
            if (custom) {
                next.lift_height_mm = round2(p.slow_retract_height_mm);
                next.retract_speed = round2(min_retract);
                if (ctx.can_use_lift_speed2) next.lift_speed2 = round2(max_lift);
                if (ctx.can_use_retract_speed2) next.retract_speed2 = round2(max_retract);
            }
        }
        const size_t pos = at + inserted_count;
        const size_t src = std::min(at, layers_size - 1);
        LayerParams photo_params = photo;
        oz.insert(oz.begin() + long(pos), round_height(raise_z));
        op.insert(op.begin() + long(pos), photo_params);
        res.list.source_index.insert(res.list.source_index.begin() + long(pos), src); res.inserted.insert(res.inserted.begin() + long(pos), true);
        ++inserted_count; // UVtools forgets this increment when the slow layer is rejected and shifts every later insert by one
        if (slow_lift_height > 0 && at > 0) {
            const double slow_z = round_height(z_mm[at - 1] + slow_lift_height);
            if (slow_z >= round_height(raise_z)) {
                slow_lift_height = 0; // the slow layer must stay below the photo layer, stop adding them
                photo.lift_speed = round2(min_lift);
                continue;
            }
            oz.insert(oz.begin() + long(pos), slow_z);
            op.insert(op.begin() + long(pos), slow);
            res.list.source_index.insert(res.list.source_index.begin() + long(pos), src); res.inserted.insert(res.inserted.begin() + long(pos), true);
            ++inserted_count;
        }
    }
    z_mm = std::move(oz); table = std::move(op);
    return res;
}


Mask TimelapseVirtualLayer::make() const
{
    return dummy_pixel ? dummy_pixel_mask(width, height, x, y, brightness) : Mask(width, height);
}

TimelapseResult timelapse_layers(std::vector<Mask> &layers, std::vector<double> &z_mm, std::vector<LayerParams> &table,
                                 const TimelapseParams &p, const PrinterContext &ctx, const LayerProgress &progress)
{
    if (const std::string err = validate_timelapse(p, z_mm, ctx); !err.empty()) throw std::invalid_argument(err);
    check_z(layers, z_mm);
    check_table(layers, table);
    TimelapseVirtualLayer virtual_layer;
    TimelapseResult res = timelapse_plan(layers.size(), layers.front().width, layers.front().height, z_mm, table, p, ctx,
                                         [&](int &cx, int &cy) { model_center(layers, cx, cy); }, virtual_layer, progress);
    if (std::none_of(res.inserted.begin(), res.inserted.end(), [](bool v) { return v; })) return res;
    const Mask virtual_mask = virtual_layer.make();
    std::vector<Mask> om;
    om.reserve(res.inserted.size());
    for (size_t k = 0; k < res.inserted.size(); ++k) om.push_back(res.inserted[k] ? virtual_mask : layers[res.list.source_index[k]]);
    layers = std::move(om);
    return res;
}

// ---- Dynamic layer height ---------------------------------------------------------------------------

DynamicLayerHeightParams dynamic_layer_height_defaults(const std::vector<LayerParams> &table, const PrinterContext &ctx, DynamicLayerHeightParams p)
{
    if (p.minimum_layer_height_mm < ctx.layer_height_mm) p.minimum_layer_height_mm = ctx.layer_height_mm;
    if (ctx.layer_height_mm * 2 > p.maximum_layer_height_mm) p.maximum_layer_height_mm = std::min(0.2, p.maximum_layer_height_mm * 2);
    if (p.bottom_exposure_s <= 0) p.bottom_exposure_s = first_of(table, true).exposure_s;
    if (p.exposure_s <= 0) p.exposure_s = first_of(table, false).exposure_s;
    return p;
}

std::vector<ExposureItem> dynamic_layer_height_exposure_table(const DynamicLayerHeightParams &p, double file_layer_height_mm)
{
    if (p.exposure_type == ExposureSetType::Manual) return p.manual_table;
    std::vector<ExposureItem> t;
    // Heights are walked in whole micrometres: UVtools uses decimals, doubles would drift.
    const long long step = um(file_layer_height_mm), max_um = um(p.maximum_layer_height_mm);
    if (step <= 0) return t;
    unsigned count = 0;
    for (long long u = step; u <= max_um; u += step, ++count) {
        const double lh = double(u) / 1000.0;
        double bottom = 0, normal = 0;
        if (p.exposure_type == ExposureSetType::Linear) {
            bottom = p.iterate_bottom_exposure ? p.bottom_exposure_s + count * p.bottom_exposure_step_s : p.bottom_exposure_s;
            normal = p.exposure_s + count * p.exposure_step_s;
        } else {
            bottom = p.iterate_bottom_exposure ? p.bottom_exposure_s + p.bottom_exposure_s * count * lh * p.bottom_exposure_step_s : p.bottom_exposure_s;
            normal = p.exposure_s + p.exposure_s * count * lh * p.exposure_step_s;
        }
        t.push_back({lh, round_even(bottom, 2), round_even(normal, 2)});
    }
    return t;
}

std::string validate_dynamic_layer_height(const DynamicLayerHeightParams &p, const std::vector<double> &z_mm, const PrinterContext &ctx)
{
    std::string sb;
    if (ctx.layer_height_mm * 2 > 0.2 + 1e-9)
        return "This file already uses the maximum layer height possible (" + std::to_string(ctx.layer_height_mm) + "mm).\nLayers can not be stacked, please re-slice your file with the lowest layer height of 0.01mm.\n";
    for (size_t i = 1; i < z_mm.size(); ++i)
        if (um(round_height(z_mm[i] - z_mm[i - 1])) != um(ctx.layer_height_mm))
            return "This file contain layer(s) with modified positions, starting at layer " + std::to_string(i) +
                   ".\nThis tool requires sequential layers with equal height.\nIf you ran this tool before, you can't run again.\n";
    if (um(p.minimum_layer_height_mm) < um(ctx.layer_height_mm))
        sb += "Minimum layer height (" + std::to_string(p.minimum_layer_height_mm) + "mm) must be equal or higher than file layer height (" + std::to_string(ctx.layer_height_mm) + "mm)\n";
    if (p.minimum_layer_height_mm > p.maximum_layer_height_mm)
        sb += "Minimum layer height (" + std::to_string(p.minimum_layer_height_mm) + "mm) can't be higher than maximum layer height (" + std::to_string(p.maximum_layer_height_mm) + "mm)\n";
    if (um(ctx.layer_height_mm) >= um(p.maximum_layer_height_mm))
        sb += "Maximum layer height (" + std::to_string(p.maximum_layer_height_mm) + "mm) can't be the same or less than current file layer height (" + std::to_string(ctx.layer_height_mm) + "mm)\n";
    const std::vector<ExposureItem> table = dynamic_layer_height_exposure_table(p, ctx.layer_height_mm);
    const long long step = um(ctx.layer_height_mm), max_um = um(p.maximum_layer_height_mm);
    for (long long u = step; step > 0 && u <= max_um; u += step) {
        const auto it = std::find_if(table.begin(), table.end(), [&](const ExposureItem &e) { return um(e.layer_height_mm) == u; });
        if (it == table.end()) sb += "Layer height " + std::to_string(double(u) / 1000.0) + "mm exposures are missing.\n";
        else if (it->bottom_exposure_s <= 0 || it->exposure_s <= 0)
            sb += "Layer height " + std::to_string(double(u) / 1000.0) + "mm exposures must be a positive value, current: " + std::to_string(it->bottom_exposure_s) + "s/" + std::to_string(it->exposure_s) + "s\n";
    }
    return sb;
}

DynamicLayerHeightResult dynamic_layer_height_layers(std::vector<Mask> &layers, std::vector<double> &z_mm, std::vector<LayerParams> &table,
                                                     const DynamicLayerHeightParams &p, const PrinterContext &ctx, const LayerProgress &progress)
{
    check_z(layers, z_mm);
    check_table(layers, table);
    std::vector<Mask> om;
    DynamicLayerHeightResult res = dynamic_layer_height_plan(
        layers.size(), z_mm, table, p, ctx, [&](size_t i) { return layers[i]; },
        [&](Mask &&m, size_t, bool) { om.push_back(std::move(m)); }, progress);
    layers = std::move(om);
    return res;
}

// The stacking walks the layers strictly in order and only ever needs the layer under the cursor, the next one and the
// stack accumulated so far (sum + XOR of the thresholded layers), so `load` is called about once per layer and nothing
// else of the document is held.
DynamicLayerHeightResult dynamic_layer_height_plan(size_t count, std::vector<double> &z_mm, std::vector<LayerParams> &table,
                                                   const DynamicLayerHeightParams &p, const PrinterContext &ctx,
                                                   const std::function<Mask(size_t)> &load, const DynamicLayerHeightEmit &emit,
                                                   const LayerProgress &progress)
{
    if (z_mm.size() != count) throw std::invalid_argument("Every layer needs a Z position.");
    if (table.size() != count) throw std::invalid_argument("Every layer needs a print parameter set.");
    if (const std::string err = validate_dynamic_layer_height(p, z_mm, ctx); !err.empty()) throw std::invalid_argument(err);
    const Range r = resolve_range(p.first_layer, p.last_layer, count);
    const std::vector<ExposureItem> exposures = dynamic_layer_height_exposure_table(p, ctx.layer_height_mm);

    DynamicLayerHeightResult res;
    res.report.old_layer_count = count;
    res.report.old_print_time_s = estimate_print_time_s(table);

    // Pixels > 127 count as "model" for the similarity test (UVtools cache: threshold 127). With "strip AA" the
    // stacked image itself is the thresholded one.
    struct Cached {
        Mask raw;
        std::optional<Mask> thr;
        const Mask &threshold() { if (!thr) thr = binarize(raw, 127); return *thr; }
    };
    std::map<size_t, std::shared_ptr<Cached>> cache; // the layers under the cursor (at most three)
    auto fetch = [&](size_t i) {
        const auto found = cache.find(i);
        if (found != cache.end()) return found->second;
        while (cache.size() >= 3) cache.erase(cache.begin()); // the walk only moves forward: drop the oldest layer
        auto entry = std::make_shared<Cached>();
        entry->raw = load(i);
        return cache[i] = std::move(entry);
    };
    auto threshold_of = [&](size_t i) { return fetch(i)->threshold(); };
    auto source_of = [&](size_t i) { const auto c = fetch(i); return p.strip_anti_aliasing ? c->threshold() : c->raw; };

    std::vector<double> oz; std::vector<size_t> osrc;
    const long long layer_h_um = um(ctx.layer_height_mm), min_um = um(p.minimum_layer_height_mm), max_um = um(p.maximum_layer_height_mm);
    auto next_z = [&](double h) { return oz.empty() ? h : round_height(oz.back() + h); };
    auto add_new = [&](Mask m, double h, size_t head, bool pristine) {
        if (p.strip_anti_aliasing && p.reconstruct_anti_aliasing) m = gaussian_blur(m, 3);
        res.report.maximum_layer_height_mm = std::max(res.report.maximum_layer_height_mm, h);
        oz.push_back(next_z(h)); osrc.push_back(head);
        emit(std::move(m), head, pristine && !p.strip_anti_aliasing);
    };
    auto reuse = [&](size_t i) {
        Mask m = fetch(i)->raw;
        if (p.strip_anti_aliasing) { m = threshold_of(i); if (p.reconstruct_anti_aliasing) m = gaussian_blur(m, 3); }
        oz.push_back(next_z(ctx.layer_height_mm)); osrc.push_back(i);
        emit(std::move(m), i, !p.strip_anti_aliasing);
    };

    const size_t total = r.last - r.first + 1;
    check_layer_progress(progress, 0, total);
    for (size_t i = 0; i < r.first; ++i) reuse(i);
    size_t li = r.first;
    while (li <= r.last) {
        if (li == r.last) { reuse(li); break; }
        const size_t head = li;
        long long cur_h = layer_h_um;       // current stack height in um
        unsigned stacked = 1, erode_count = 0;
        std::optional<Mask> sum, xor_sum;
        bool pristine = true;               // the stack is still exactly the head layer
        while (true) {
            check_layer_progress(progress, li - r.first, total);
            if (cur_h >= max_um || li == r.last) {
                // Maximum height reached (or the end of the range): close this stack including layer `li`.
                if (!sum) sum = source_of(li);
                ++li;
                break;
            }
            const long long prev_h = cur_h;
            cur_h += layer_h_um;
            const Mask mat1_t = threshold_of(li);
            if (!sum) sum = source_of(li);
            ++li;
            const Mask mat2_t = threshold_of(li);
            Mask diff(mat1_t.width, mat1_t.height);
            for (size_t k = 0; k < diff.px.size(); ++k) diff.px[k] = mat1_t.px[k] != mat2_t.px[k] ? 255 : 0;
            xor_sum = xor_sum ? max_masks(*xor_sum, diff) : diff;
            if (has_nonzero(*xor_sum)) { // the layers differ
                // UVtools erodes `xor_sum` by one pixel on every iteration without feeding the result back, so the
                // outcome is the same on each pass: either it vanishes right away or it never does. Ported literally,
                // including the erode counter being spent across the whole stack.
                const bool vanishes = !has_nonzero(erode(*xor_sum, 1));
                bool meets = false;
                while (erode_count < p.maximum_erodes) {
                    ++erode_count;
                    if (vanishes) { meets = true; break; }
                }
                if (!meets && min_um < cur_h) { cur_h = prev_h; break; } // too different: close the stack WITHOUT layer `li`
                if (erode_count > 0) { sum = max_masks(*sum, source_of(li)); pristine = false; }
            }
            ++stacked;
        }
        if (stacked > 1) res.report.stacked_layers += stacked;
        // Sanity check of UVtools: the rebuilt Z must land on the Z of the last stacked layer.
        if (um(next_z(double(cur_h) / 1000.0)) != um(z_mm[li - 1]))
            throw std::invalid_argument("Model height integrity has been violated at layer " + std::to_string(li) + ", this operation will not proceed.");
        add_new(std::move(*sum), double(cur_h) / 1000.0, head, pristine);
    }
    for (size_t i = r.last + 1; i < count; ++i) reuse(i);

    // Per layer exposure from the height table (bottom / normal by the head layer's flag).
    std::vector<LayerParams> op(osrc.size());
    for (size_t k = 0; k < osrc.size(); ++k) {
        op[k] = table[osrc[k]];
        const double h = k == 0 ? oz[0] : round_height(oz[k] - oz[k - 1]);
        double bottom = p.bottom_exposure_s, normal = p.exposure_s;
        for (const ExposureItem &e : exposures)
            if (um(e.layer_height_mm) == um(h)) { bottom = e.bottom_exposure_s; normal = e.exposure_s; break; }
        op[k].exposure_s = round2(op[k].is_bottom ? bottom : normal);
    }
    res.report.new_layer_count = osrc.size();
    res.report.new_print_time_s = estimate_print_time_s(op);
    res.list.source_index = std::move(osrc);
    z_mm = std::move(oz); table = std::move(op);
    return res;
}

// ---- Lithophane -------------------------------------------------------------------------------------

namespace {

// cv::resize INTER_LINEAR (float weights; OpenCV's fixed point differs by at most one level).
Mask resize_bilinear(const Mask &src, size_t nw, size_t nh)
{
    Mask out(nw, nh);
    if (src.px.empty() || nw == 0 || nh == 0) return out;
    const double sx = double(src.width) / double(nw), sy = double(src.height) / double(nh);
    for (size_t y = 0; y < nh; ++y) {
        const double fy = std::max(0.0, (double(y) + 0.5) * sy - 0.5);
        const size_t y0 = std::min(src.height - 1, size_t(fy)), y1 = std::min(src.height - 1, y0 + 1);
        const double wy = fy - double(y0);
        for (size_t x = 0; x < nw; ++x) {
            const double fx = std::max(0.0, (double(x) + 0.5) * sx - 0.5);
            const size_t x0 = std::min(src.width - 1, size_t(fx)), x1 = std::min(src.width - 1, x0 + 1);
            const double wx = fx - double(x0);
            const double top = src.at(x0, y0) * (1 - wx) + src.at(x1, y0) * wx, bot = src.at(x0, y1) * (1 - wx) + src.at(x1, y1) * wx;
            out.at(x, y) = uint8_t(std::clamp(std::lround(top * (1 - wy) + bot * wy), 0L, 255L));
        }
    }
    return out;
}

// cv::equalizeHist
Mask equalize_hist(const Mask &m)
{
    const std::array<size_t, 256> h = histogram(m);
    size_t i = 0;
    while (i < 256 && h[i] == 0) ++i;
    if (i == 256) return m;
    const size_t total = m.px.size();
    if (h[i] == total) return Mask(m.width, m.height, uint8_t(i)); // flat image: OpenCV fills with that level
    const float scale = 255.f / float(total - h[i]);
    std::array<uint8_t, 256> lut{};
    size_t sum = 0;
    lut[i++] = 0;
    for (; i < 256; ++i) { sum += h[i]; lut[i] = uint8_t(std::clamp(std::lround(float(sum) * scale), 0L, 255L)); }
    Mask out = m;
    for (uint8_t &v : out.px) v = lut[v];
    return out;
}

Mask rotate_mask(const Mask &m, RotateDirection d)
{
    if (d == RotateDirection::None) return m;
    if (d == RotateDirection::Rotate180) { Mask o = m; std::reverse(o.px.begin(), o.px.end()); return o; }
    Mask o(m.height, m.width);
    for (size_t y = 0; y < m.height; ++y)
        for (size_t x = 0; x < m.width; ++x) {
            if (d == RotateDirection::Rotate90Clockwise) o.at(m.height - 1 - y, x) = m.at(x, y);
            else o.at(y, m.width - 1 - x) = m.at(x, y);
        }
    return o;
}

Mask flip_mask(const Mask &m, FlipDirection d)
{
    Mask o(m.width, m.height);
    for (size_t y = 0; y < m.height; ++y)
        for (size_t x = 0; x < m.width; ++x) {
            const size_t sx = d == FlipDirection::Vertical ? x : m.width - 1 - x, sy = d == FlipDirection::Horizontal ? y : m.height - 1 - y;
            o.at(x, y) = m.at(sx, sy);
        }
    return o;
}

// Copies `src` into the centre of `dst` (cv CopyToCenter); the caller guarantees it fits.
void copy_to_center(const Mask &src, Mask &dst) { paste(dst, int((dst.width - src.width) / 2), int((dst.height - src.height) / 2), src); }

} // namespace

Mask lithophane_target_image(const Mask &image, const LithophaneParams &p, const PrinterContext &ctx)
{
    Mask m = image;
    if (p.invert_color) for (uint8_t &v : m.px) v = uint8_t(255 - v);
    const Rect bounds = bounding_rect(m); // crop away the black frame
    if (bounds.empty()) return Mask();
    m = crop(m, bounds);
    const double scale = p.resize_factor_percent / 100.0;
    if (std::abs(scale - 1.0) >= 0.001) m = resize_bilinear(m, size_t(double(m.width) * scale), size_t(double(m.height) * scale));
    if (p.enhance_contrast) m = equalize_hist(m);
    if (p.brightness_gain != 0) {
        // UVtools passes the image as its own mask: pure black background stays black.
        const int g = std::abs(p.brightness_gain);
        for (uint8_t &v : m.px) if (v) v = p.brightness_gain > 0 ? sat_add(v, g) : sat_sub(v, g);
    }
    if (p.gaussian_blur > 0) m = gaussian_blur(m, int(1 + p.gaussian_blur * 2));
    m = open_mask(m, int(p.remove_noise_iterations));
    m = close_mask(m, int(p.gap_closing_iterations));
    m = rotate_mask(m, p.rotate);
    if (p.mirror) m = flip_mask(m, ctx.display_mirror.value_or(FlipDirection::Horizontal));
    return m;
}

std::string validate_lithophane(const LithophaneParams &p, const Mask &image, size_t rx, size_t ry)
{
    std::string sb;
    if (image.px.empty() || !has_nonzero(image)) {
        sb += "Unable to generate the mat from source file, is it a valid image file?\n";
    } else {
        const Rect b = bounding_rect(image);
        if (double(rx) < double(b.w) * p.resize_factor_percent / 100 || double(ry) < double(b.h) * p.resize_factor_percent / 100) {
            const double maxs = std::min(double(rx) * 100.0 / b.w, double(ry) * 100.0 / b.h);
            sb += "The printer resolution is not enough to accomodate the lithophane image, please scale down to a maximum of " + std::to_string(int(maxs)) + "%\n";
        }
    }
    if (p.separate_grayscale_pixels) {
        if (p.start_threshold == p.end_threshold) sb += "Start threshold can't be equal than end threshold (" + std::to_string(p.end_threshold) + ")\n";
        else if (p.start_threshold > p.end_threshold) sb += "Start threshold can't be higher than end threshold\n";
    }
    if (p.layer_height_mm <= 0) sb += "The layer height must be positive.\n";
    return sb;
}

void lithophane_stack(const Mask &image, size_t rx, size_t ry, const LithophaneParams &p, const PrinterContext &ctx,
                      std::vector<Mask> &distinct, std::vector<size_t> &sequence, std::vector<double> &z_mm, std::vector<LayerParams> &table,
                      const LayerProgress &progress)
{
    if (const std::string err = validate_lithophane(p, image, rx, ry); !err.empty()) throw std::invalid_argument(err);
    const Mask mat = lithophane_target_image(image, p, ctx);
    if (mat.px.empty() || mat.width > rx || mat.height > ry) throw std::invalid_argument("The lithophane image does not fit in the printer resolution.\n");

    std::vector<Mask> stack;       // distinct layers
    std::vector<size_t> seq;       // layer order: indices into `stack`
    if (p.separate_grayscale_pixels) {
        // One layer per grey level: the darker the pixel, the earlier (lower) its column ends.
        const unsigned t0 = p.start_threshold, t1 = std::min(255u, p.end_threshold); // [t0, t1) like Parallel.For
        if (t1 <= t0) throw std::invalid_argument("Start threshold can't be higher than end threshold\n");
        std::vector<Mask> by_threshold(t1 - t0);
        parallel_layers(0, by_threshold.size() - 1, progress, [&](size_t k) {
            Mask t = binarize(mat, uint8_t(t0 + k));
            if (!has_nonzero(t)) return;
            if (p.anti_aliasing) t = gaussian_blur(t, 3);
            Mask canvas(rx, ry);
            copy_to_center(t, canvas);
            by_threshold[k] = std::move(canvas);
        });
        for (Mask &m : by_threshold) if (!m.px.empty()) stack.push_back(std::move(m));
        if (stack.empty()) throw std::invalid_argument("Unable to continue due to no threshold layers was generated, either by lack of pixels or by using a short range.\n");
        for (size_t i = 0; i < stack.size(); ++i) seq.push_back(i);

        if (!p.one_layer_per_threshold) {
            const double inc = double(stack.size()) * p.layer_height_mm / std::max(p.layer_height_mm, p.lithophane_height_mm);
            if (inc >= 2) { // more grey levels than layers: merge `n` neighbouring levels into one layer (max)
                const size_t n = size_t(inc);
                std::vector<Mask> packed;
                for (size_t i = 0; i < stack.size(); i += n) {
                    Mask acc = stack[i];
                    for (size_t k = i + 1; k < i + n && k < stack.size(); ++k) acc = max_masks(acc, stack[k]);
                    packed.push_back(std::move(acc));
                }
                stack = std::move(packed);
                seq.clear();
                for (size_t i = 0; i < stack.size(); ++i) seq.push_back(i);
            } else if (inc < 1) { // fewer grey levels than layers: repeat every level
                const size_t n = size_t(std::ceil(1.0 / inc));
                if (n > 1) {
                    seq.clear();
                    for (size_t i = 0; i < stack.size(); ++i) for (size_t k = 0; k < n; ++k) seq.push_back(i);
                }
            }
        }
    } else {
        Mask canvas(rx, ry);
        copy_to_center(mat, canvas);
        if (p.anti_aliasing) canvas = gaussian_blur(canvas, 3);
        // At least one layer: UVtools would produce an empty document for a height below one layer.
        stack.push_back(std::move(canvas));
        seq.assign(std::max<size_t>(1, size_t(p.lithophane_height_mm / p.layer_height_mm)), 0);
    }

    const size_t base_layers = size_t(p.base_thickness_mm / p.layer_height_mm);
    if (p.base_type != LithophaneBase::None && p.base_thickness_mm > 0 && base_layers > 0) {
        Mask base(rx, ry);
        if (p.base_type == LithophaneBase::Square) {
            fill_rect(base, long(rx / 2) - long(mat.width / 2) - long(p.base_margin_px / 2), long(ry / 2) - long(mat.height / 2) - long(p.base_margin_px / 2),
                      long(mat.width) + long(p.base_margin_px), long(mat.height) + long(p.base_margin_px), 255);
        } else {
            Mask silhouette(mat.width, mat.height);
            for (size_t i = 0; i < mat.px.size(); ++i) silhouette.px[i] = mat.px[i] > 1 ? 255 : 0; // threshold(mat, 1)
            silhouette = dilate(silhouette, int(p.base_margin_px));
            copy_to_center(silhouette, base);
        }
        stack.push_back(std::move(base));
        seq.insert(seq.begin(), base_layers, stack.size() - 1);
    }

    distinct = std::move(stack);
    sequence = std::move(seq);
    z_mm.clear(); table.clear();
    for (size_t i = 0; i < sequence.size(); ++i) {
        z_mm.push_back(round_height(p.layer_height_mm * double(i + 1)));
        LayerParams lp;
        lp.is_bottom = i < p.bottom_layer_count;
        lp.exposure_s = round2(lp.is_bottom ? p.bottom_exposure_s : p.normal_exposure_s);
        table.push_back(lp);
    }
}


void lithophane_layers(const Mask &image, size_t rx, size_t ry, const LithophaneParams &p, const PrinterContext &ctx,
                       std::vector<Mask> &layers, std::vector<double> &z_mm, std::vector<LayerParams> &table, const LayerProgress &progress)
{
    std::vector<Mask> distinct;
    std::vector<size_t> sequence;
    lithophane_stack(image, rx, ry, p, ctx, distinct, sequence, z_mm, table, progress);
    layers.clear();
    layers.reserve(sequence.size());
    for (size_t k : sequence) layers.push_back(distinct[k]);
}

// ---- Repair layers ----------------------------------------------------------------------------------

std::string validate_repair_layers(const RepairLayersParams &p)
{
    if (!p.repair_islands && !p.repair_resin_traps && !p.repair_suction_cups && !p.remove_empty_layers) return "You must select at least one repair operation.\n";
    return {};
}

namespace {

// Runs the detector over a layer provider; converts the user cancel request into LayerOperationCancelled.
std::vector<LayerIssue> detect_on(const LayerMaskProvider &provider, const std::vector<double> &z, const IssueDetectionConfig &cfg, const LayerProgress &progress)
{
    std::atomic<bool> cancelled{false};
    try {
        return detect_layer_issues(
            provider, z, cfg,
            [&](size_t d, size_t t) { if (progress && !progress(d, t)) cancelled = true; },
            [&]() { return cancelled.load(); });
    } catch (const std::exception &) {
        if (cancelled) throw LayerOperationCancelled{};
        throw;
    }
}

IssueDetectionConfig all_off(IssueDetectionConfig c)
{
    c.island.enabled = c.overhang.enabled = c.resin_trap.enabled = c.touching_bound.enabled = c.print_height.enabled = c.empty_layer.enabled = false;
    return c;
}

Mask region_mask(const LayerIssuePart &part, size_t w, size_t h)
{
    Mask m(w, h);
    for (const PixelRun &run : part.runs)
        for (int32_t x = run.x0; x < run.x1; ++x)
            if (run.y >= 0 && x >= 0 && size_t(run.y) < h && size_t(x) < w) m.at(size_t(x), size_t(run.y)) = 255;
    return m;
}

// A pixel edit of one layer kept as run-length records (never a mask): zero the runs, or add the captured brightness of
// the island they belong to ("attach islands below").
struct PixelEdit {
    bool                 add = false;
    std::vector<PixelRun> runs;
    std::vector<uint8_t> values; // add: one value per run pixel, in run order
};
void apply_pixel_edit(Mask &m, const PixelEdit &e)
{
    size_t k = 0;
    for (const PixelRun &run : e.runs)
        for (int32_t x = run.x0; x < run.x1; ++x, ++k) {
            if (run.y < 0 || x < 0 || size_t(run.y) >= m.height || size_t(x) >= m.width) continue;
            uint8_t &v = m.at(size_t(x), size_t(run.y));
            v = e.add ? sat_add(v, e.values[k]) : uint8_t(0);
        }
}
struct CircleEdit { long cx, cy; double radius; };

// The working state of one layer = a base layer plus its ordered edits; a few layers are kept decoded (LRU) and every
// new edit is also written through to the cached copy, so a layer is decoded again only after it left the cache.
template<class Edit, class Apply> class EditedLayers {
public:
    EditedLayers(size_t count, size_t capacity, std::function<Mask(size_t)> base, Apply apply)
        : m_edits(count), m_capacity(std::max<size_t>(1, capacity)), m_base(std::move(base)), m_apply(std::move(apply)) {}
    bool edited(size_t layer) const { return !m_edits[layer].empty(); }
    const std::vector<Edit> &edits(size_t layer) const { return m_edits[layer]; }
    // The working state of `layer`. Valid until the next call.
    Mask &get(size_t layer)
    {
        const auto found = m_cache.find(layer);
        if (found != m_cache.end()) { found->second.used = ++m_clock; return found->second.mask; }
        if (m_cache.size() >= m_capacity) {
            auto oldest = m_cache.begin();
            for (auto it = m_cache.begin(); it != m_cache.end(); ++it) if (it->second.used < oldest->second.used) oldest = it;
            m_cache.erase(oldest);
        }
        Slot &slot = m_cache[layer];
        slot.mask = m_base(layer);
        for (const Edit &e : m_edits[layer]) m_apply(slot.mask, e);
        slot.used = ++m_clock;
        return slot.mask;
    }
    // The state of `layer` without keeping it (a copy).
    Mask copy(size_t layer) const
    {
        const auto found = m_cache.find(layer);
        if (found != m_cache.end()) return found->second.mask;
        Mask m = m_base(layer);
        for (const Edit &e : m_edits[layer]) m_apply(m, e);
        return m;
    }
    void add(size_t layer, Edit edit)
    {
        const auto found = m_cache.find(layer);
        if (found != m_cache.end()) m_apply(found->second.mask, edit);
        m_edits[layer].push_back(std::move(edit));
    }
private:
    struct Slot { Mask mask; uint64_t used = 0; };
    std::vector<std::vector<Edit>> m_edits;
    std::map<size_t, Slot>         m_cache;
    size_t                         m_capacity;
    uint64_t                       m_clock = 0;
    std::function<Mask(size_t)>    m_base;
    Apply                          m_apply;
};

} // namespace

RepairLayersResult repair_layers_plan(const RepairIO &io, std::vector<double> &z_mm, const RepairLayersParams &p,
                                      std::vector<RepairRef> &refs, const LayerProgress &progress)
{
    if (const std::string err = validate_repair_layers(p); !err.empty()) throw std::invalid_argument(err);
    const size_t n = io.layer_count, W = io.width, H = io.height;
    if (z_mm.size() != n) throw std::invalid_argument("Every layer needs a Z position.");
    const Range r = resolve_range(p.first_layer, p.last_layer, n);
    check_layer_progress(progress, 0, n);

    // Nothing is committed before the end: cancelling or failing leaves the caller's layers as they were.
    std::vector<double> wz = z_mm;
    RepairLayersResult res;
    res.list.source_index.resize(n);
    refs.resize(n);
    for (size_t i = 0; i < n; ++i) { res.list.source_index[i] = i; refs[i] = {i, false}; }

    const uint64_t per_layer = std::max<uint64_t>(1, uint64_t(W) * H);
    const size_t cache_layers = size_t(std::clamp<uint64_t>(layer_decoded_budget() / per_layer / 4, 2, 16));
    // Edits of steps 1 + 2 (small islands zeroed, islands attached downwards): runs, not masks.
    EditedLayers<PixelEdit, void (*)(Mask &, const PixelEdit &)> work(n, cache_layers, io.load, &apply_pixel_edit);
    auto provider = [&](size_t i) {
        Mask m = io.load(i);
        for (const PixelEdit &e : work.edits(i)) apply_pixel_edit(m, e);
        return LayerMask{m.width, m.height, std::move(m.px)};
    };

    const bool islands_recursive = p.repair_islands && p.remove_islands_below_equal_pixels > 0 && p.remove_islands_recursive_iterations != 1;
    const bool islands_attach = p.repair_islands && p.attach_islands_below_layers > 0;
    IssueDetectionConfig cfg = all_off(p.detection);
    cfg.island.enabled = islands_recursive || islands_attach;
    cfg.resin_trap.enabled = p.repair_resin_traps;
    cfg.resin_trap.detect_suction_cups = p.repair_suction_cups;
    cfg.empty_layer.enabled = p.remove_empty_layers;
    std::vector<LayerIssue> issues = detect_on(provider, wz, cfg, progress);
    if (issues.empty()) return res; // UVtools: nothing detected = nothing repaired (not even gap closing)

    auto is_small_island = [&](const LayerIssue &i) { return i.type == LayerIssue::Type::Island && i.pixel_count <= p.remove_islands_below_equal_pixels; };
    auto runs_of = [](const LayerIssue &issue) {
        std::vector<PixelRun> runs;
        for (const LayerIssuePart &part : issue.parts) runs.insert(runs.end(), part.runs.begin(), part.runs.end());
        return runs;
    };

    // 1. Remove tiny islands. Removing one can orphan the pixels above it, so detection repeats on the next layers.
    if (islands_recursive) {
        const unsigned limit = p.remove_islands_recursive_iterations == 0 ? 65535u : p.remove_islands_recursive_iterations;
        std::vector<LayerIssue> current; // only the small islands matter from here on
        for (const LayerIssue &issue : issues) if (is_small_island(issue)) current.push_back(issue);
        for (unsigned it = 0; it < limit; ++it) {
            std::set<size_t> recompute;
            bool any = false;
            for (const LayerIssue &issue : current) {
                any = true;
                work.add(issue.first_layer, PixelEdit{false, runs_of(issue), {}});
                ++res.islands_removed;
                if (issue.first_layer + 1 < n) recompute.insert(issue.first_layer + 1);
            }
            if (!any) break;
            issues.erase(std::remove_if(issues.begin(), issues.end(), is_small_island), issues.end());
            if (recompute.empty()) break;
            IssueDetectionConfig again = all_off(p.detection);
            again.island.enabled = true;
            again.island.white_list_layers = std::unordered_set<size_t>(recompute.begin(), recompute.end());
            current.clear();
            for (LayerIssue &issue : detect_on(provider, wz, again, {})) if (is_small_island(issue)) current.push_back(std::move(issue));
        }
    }

    // 2. Attach islands that are only separated from the model below by a few layers: copy their pixels downwards.
    if (islands_attach) {
        std::map<size_t, std::vector<size_t>> by_layer; // layer -> indexes into `issues`
        for (size_t k = 0; k < issues.size(); ++k) if (issues[k].type == LayerIssue::Type::Island) by_layer[issues[k].first_layer].push_back(k);
        std::vector<bool> attached(issues.size(), false);
        const int64_t attach = p.attach_islands_below_layers;
        for (const auto &[key, idxs] : by_layer) { // ascending layer order keeps the result deterministic
            // The brightness of the islands as it is before this layer is touched (UVtools copies the layer first).
            std::vector<std::vector<uint8_t>> brightness(idxs.size());
            {
                const Mask &island_layer = work.get(key);
                for (size_t q = 0; q < idxs.size(); ++q)
                    for (const LayerIssuePart &part : issues[idxs[q]].parts)
                        for (const PixelRun &run : part.runs)
                            for (int32_t x = run.x0; x < run.x1; ++x) brightness[q].push_back(island_layer.at(size_t(x), size_t(run.y)));
            }
            const int64_t start = std::max<int64_t>(0, int64_t(key) - 2);
            const int64_t lowest = std::max<int64_t>(0, int64_t(key) - 1 - attach);
            for (size_t q = 0; q < idxs.size(); ++q) {
                const size_t k = idxs[q];
                const LayerIssue &issue = issues[k];
                int64_t found_at = start == 0 ? 0 : -1;
                const double required = std::max(1.0, double(issue.pixel_count) * p.detection.island.required_pixels_to_support_multiplier);
                for (int64_t L = start; L >= lowest && found_at < 0; --L) {
                    const Mask &below = work.get(size_t(L));
                    size_t supporting = 0;
                    for (const LayerIssuePart &part : issue.parts)
                        for (const PixelRun &run : part.runs)
                            for (int32_t x = run.x0; x < run.x1 && found_at < 0; ++x)
                                if (below.at(size_t(x), size_t(run.y)) >= p.detection.island.required_pixel_brightness_to_support && ++supporting >= required)
                                    found_at = L + 1;
                }
                if (found_at < 0) continue;
                for (int64_t L = start + 1; L >= found_at; --L) {
                    if (L < 0 || size_t(L) >= n) continue;
                    work.add(size_t(L), PixelEdit{true, runs_of(issue), brightness[q]});
                }
                attached[k] = true;
                ++res.islands_attached;
            }
        }
        std::vector<LayerIssue> rest;
        for (size_t k = 0; k < issues.size(); ++k) if (!attached[k]) rest.push_back(std::move(issues[k]));
        issues = std::move(rest);
    }

    // 3. Per layer: single pass island removal, resin trap fill, gap closing, noise removal (every layer on its own).
    //    A layer whose pixels did not change keeps the payload of the original.
    const bool per_layer_step = p.repair_islands || p.repair_resin_traps;
    const bool single_pass_islands = p.repair_islands && p.remove_islands_below_equal_pixels > 0 && p.remove_islands_recursive_iterations == 1;
    const bool close_open = p.repair_islands && (p.gap_closing_iterations > 0 || p.noise_removal_iterations > 0);
    std::vector<std::vector<const LayerIssue *>> small_at(n);
    std::vector<std::vector<const LayerIssuePart *>> traps_at(n);
    for (const LayerIssue &issue : issues) {
        if (single_pass_islands && is_small_island(issue)) small_at[issue.first_layer].push_back(&issue);
        if (p.repair_resin_traps && issue.type == LayerIssue::Type::ResinTrap)
            for (const LayerIssuePart &part : issue.parts) if (part.layer < n) traps_at[part.layer].push_back(&part);
    }
    // Suction cups read the result of this step: it is kept as scratch (not quantized) until they are done.
    const bool scratch = p.repair_suction_cups;
    std::vector<int64_t> lit(n, -1);   // lit pixels of the layers computed here (needed by the empty layer removal)
    std::vector<char> is_scratch(n, 0);
    std::vector<size_t> traps(n, 0), small(n, 0);
    io.parallel(0, n - 1, 8, [&](size_t li) {
        const bool in_range = per_layer_step && li >= r.first && li <= r.last;
        const bool process = in_range && (close_open || !small_at[li].empty() || !traps_at[li].empty());
        if (!process && !work.edited(li)) return; // the original layer is the result
        Mask m = io.load(li);
        for (const PixelEdit &e : work.edits(li)) apply_pixel_edit(m, e);
        if (process) {
            for (const LayerIssue *issue : small_at[li]) {
                for (const LayerIssuePart &part : issue->parts) for (const PixelRun &run : part.runs)
                    for (int32_t x = run.x0; x < run.x1; ++x)
                        if (run.y >= 0 && x >= 0 && size_t(run.y) < m.height && size_t(x) < m.width) m.at(size_t(x), size_t(run.y)) = 0;
                ++small[li];
            }
            for (const LayerIssuePart *part : traps_at[li]) {
                const Mask region = region_mask(*part, W, H);
                // Fill the trap and a rim of `overlap` px around it (UVtools strokes the contour that wide).
                const Mask grown = p.resin_traps_overlap_by > 0 ? dilate(region, int(p.resin_traps_overlap_by)) : region;
                for (size_t k = 0; k < m.px.size(); ++k) if (grown.px[k]) m.px[k] = 255;
                ++traps[li];
            }
            if (close_open) {
                m = close_mask(m, int(p.gap_closing_iterations));
                m = open_mask(m, int(p.noise_removal_iterations));
            }
        }
        lit[li] = int64_t(m.nonzero_count());
        refs[li] = io.store(std::move(m), li, scratch);
        is_scratch[li] = scratch && refs[li].emitted;
    });
    for (size_t i = 0; i < n; ++i) { res.resin_traps_filled += traps[i]; res.islands_removed += small[i]; }

    // 4. Suction cups: drill a vent hole from the cup downwards until it reaches an already empty area. The holes are
    //    kept as circles per layer on top of the result of step 3.
    if (p.repair_suction_cups) {
        EditedLayers<CircleEdit, void (*)(Mask &, const CircleEdit &)> drilled(
            n, cache_layers, [&](size_t i) { return io.load_ref(refs[i]); },
            [](Mask &m, const CircleEdit &c) { fill_circle(m, c.cx, c.cy, c.radius, 0); });
        const int d = int(p.suction_cups_vent_hole);
        const double radius = d / 2.0;
        for (const LayerIssue &issue : issues) {
            if (issue.type != LayerIssue::Type::SuctionCup || issue.parts.empty()) continue;
            const LayerIssuePart &part = issue.parts.front();
            double sx = 0, sy = 0; size_t cnt = 0;
            for (const PixelRun &run : part.runs) for (int32_t x = run.x0; x < run.x1; ++x) { sx += x; sy += run.y; ++cnt; }
            if (cnt == 0) continue;
            const long cx = std::lround(sx / double(cnt)), cy = std::lround(sy / double(cnt));
            bool inside = false; // the vent circle must touch the cup, otherwise the centroid lies in a void
            {
                const long y0 = std::max(0L, cy - d), y1 = std::min(long(H) - 1, cy + d), x0 = std::max(0L, cx - d), x1 = std::min(long(W) - 1, cx + d);
                for (const PixelRun &run : part.runs) {
                    if (inside) break;
                    if (run.y < 0 || size_t(run.y) >= H || run.y < y0 || run.y > y1) continue;
                    for (long x = std::max<long>(run.x0, std::max(0L, x0)); x <= std::min<long>(run.x1 - 1, x1); ++x)
                        if (x >= 0 && size_t(x) < W && double((x - cx) * (x - cx) + (run.y - cy) * (run.y - cy)) <= radius * radius) { inside = true; break; }
                }
            }
            if (!inside) continue;
            int drawn = 0;
            for (int64_t L = int64_t(issue.first_layer) - 1; L >= 0; --L) {
                const Mask &m = drilled.get(size_t(L));
                size_t black = 0;
                for (long y = std::max(0L, cy - d / 2); y <= std::min(long(H) - 1, cy + d / 2); ++y)
                    for (long x = std::max(0L, cx - d / 2); x <= std::min(long(W) - 1, cx + d / 2); ++x)
                        if (double((x - cx) * (x - cx) + (y - cy) * (y - cy)) <= radius * radius && m.at(size_t(x), size_t(y)) <= 100) ++black;
                if (double(black) >= double(d) * double(d) / 3.0) { if (drawn == 0) continue; break; } // already drains
                drilled.add(size_t(L), CircleEdit{cx, cy, radius});
                ++drawn;
            }
            if (drawn > 0) ++res.suction_cups_drilled;
        }
        // Final layers: whatever was scratch or drilled is stored again, quantized.
        io.parallel(0, n - 1, 3, [&](size_t li) {
            if (!is_scratch[li] && !drilled.edited(li)) return;
            Mask m = io.load_ref(refs[li]);
            for (const CircleEdit &c : drilled.edits(li)) fill_circle(m, c.cx, c.cy, c.radius, 0);
            lit[li] = int64_t(m.nonzero_count());
            refs[li] = io.store(std::move(m), refs[li].emitted ? std::nullopt : std::optional<size_t>(refs[li].index), false);
        });
    }

    // 5. Empty layers inside the range.
    if (p.remove_empty_layers) {
        RemoveParams rp;
        rp.first_layer = r.first; rp.last_layer = r.last; rp.use_threshold = true; rp.pixel_threshold = 0;
        std::vector<size_t> unknown;
        for (size_t i = r.first; i <= r.last; ++i) if (lit[i] < 0) unknown.push_back(i);
        if (!unknown.empty())
            io.parallel(0, unknown.size() - 1, 2, [&](size_t k) { lit[unknown[k]] = int64_t(io.load_ref(refs[unknown[k]]).nonzero_count()); });
        try {
            const size_t before = n;
            res.list = remove_plan(n, wz, rp, [&](size_t i) { return size_t(lit[i] >= 0 ? lit[i] : int64_t(io.load_ref(refs[i]).nonzero_count())); }, {});
            res.empty_layers_removed = before - res.list.source_index.size();
        } catch (const std::invalid_argument &) {} // nothing to remove, or every layer would go: leave the list alone
        if (res.empty_layers_removed > 0) {
            std::vector<RepairRef> kept(res.list.source_index.size());
            for (size_t k = 0; k < kept.size(); ++k) kept[k] = refs[res.list.source_index[k]];
            refs = std::move(kept);
        }
    }
    z_mm = std::move(wz);
    return res;
}

RepairLayersResult repair_layers(std::vector<Mask> &layers, std::vector<double> &z_mm, const RepairLayersParams &p, const LayerProgress &progress)
{
    if (const std::string err = validate_repair_layers(p); !err.empty()) throw std::invalid_argument(err);
    check_z(layers, z_mm);
    resolve_range(p.first_layer, p.last_layer, layers.size());
    // The same engine as the streamed operation, over vectors: computed layers are kept in `blobs`.
    std::deque<Mask> blobs;
    std::mutex mutex;
    RepairIO io;
    io.layer_count = layers.size(); io.width = layers.front().width; io.height = layers.front().height;
    io.load = [&](size_t i) { return layers[i]; };
    io.store = [&](Mask &&m, std::optional<size_t> source, bool) -> RepairRef {
        if (source && layers[*source].width == m.width && layers[*source].height == m.height && layers[*source].px == m.px) return {*source, false};
        std::lock_guard<std::mutex> lock(mutex);
        blobs.push_back(std::move(m));
        return {blobs.size() - 1, true};
    };
    io.load_ref = [&](RepairRef ref) { std::lock_guard<std::mutex> lock(mutex); return ref.emitted ? blobs[ref.index] : layers[ref.index]; };
    io.parallel = [&](size_t first, size_t last, size_t, const std::function<void(size_t)> &fn) { parallel_layers(first, last, progress, fn); };
    std::vector<RepairRef> refs;
    std::vector<double> wz = z_mm;
    RepairLayersResult res = repair_layers_plan(io, wz, p, refs, progress);
    std::vector<Mask> out;
    out.reserve(refs.size());
    for (const RepairRef &ref : refs) out.push_back(ref.emitted ? blobs[ref.index] : layers[ref.index]);
    layers = std::move(out);
    z_mm = std::move(wz);
    return res;
}



// ---- Calculator -------------------------------------------------------------------------------------

PixelsPerMillimeter pixels_per_millimeter(size_t rx, size_t ry, double dw, double dh)
{
    return {dw > 0 ? round_even(double(rx) / dw, 2) : 0.0, dh > 0 ? round_even(double(ry) / dh, 2) : 0.0};
}

PixelsResult mm_to_pixels(size_t rx, size_t ry, double dw, double dh, double mm)
{
    const PixelsPerMillimeter ppm = pixels_per_millimeter(rx, ry, dw, dh);
    return {round_even(ppm.x * mm, 2), round_even(ppm.y * mm, 2)};
}

double light_off_delay_seconds(double lift, double lift_speed, double retract_speed, double extra)
{
    double t = extra;
    if (lift_speed > 0) t += lift / (lift_speed / 60.0);
    if (retract_speed > 0) t += lift / (retract_speed / 60.0);
    return round_even(t, 2);
}

double light_off_delay_seconds(double lh, double ls, double rs, double extra, double lh2, double ls2, double rh2, double rs2)
{
    double t = extra;
    if (lh > 0 && ls > 0) t += lh / (ls / 60.0);
    if (lh2 > 0 && ls2 > 0) t += lh2 / (ls2 / 60.0);
    if (rh2 > 0 && rs2 > 0) t += rh2 / (rs2 / 60.0);
    const double remaining = lh + lh2 - rh2; // what the second retract stage did not already cover
    if (remaining > 0 && rs > 0) t += remaining / (rs / 60.0);
    return round_even(t, 2);
}

double light_off_delay_seconds(const LayerParams &l, double extra)
{
    return light_off_delay_seconds(l.lift_height_mm, l.lift_speed, l.retract_speed, extra, l.lift_height2_mm, l.lift_speed2, l.retract_height2_mm, l.retract_speed2);
}

double lift_only_seconds(const LayerParams &l, double extra)
{
    double t = extra;
    if (l.lift_height_mm > 0 && l.lift_speed > 0) t += round_even(l.lift_height_mm / (l.lift_speed / 60.0) + extra, 2); // UVtools adds `extra` twice
    if (l.lift_height2_mm > 0 && l.lift_speed2 > 0) t += round_even(l.lift_height2_mm / (l.lift_speed2 / 60.0) + extra, 2);
    return t;
}

double xy_resolution_mm(size_t rx, size_t ry, double dw, double dh)
{
    if (!(dw > 0 || dh > 0) || rx == 0 || ry == 0) return 0;
    return std::max(dw / double(rx), dh / double(ry));
}

double optimal_model_tilt_degrees(size_t rx, size_t ry, double dw, double dh, double layer_height)
{
    const double xy = xy_resolution_mm(rx, ry, dw, dh);
    return xy > 0 ? round_even(std::atan(layer_height / xy) * (180.0 / kPi), 3) : 0.0;
}

double layer_print_time_s(const LayerParams &l)
{
    return round_away(l.wait_before_cure_s + l.exposure_s + l.wait_after_cure_s + l.wait_after_lift_s + light_off_delay_seconds(l, 0), 2);
}

double estimate_print_time_s(const std::vector<LayerParams> &t)
{
    double s = 0;
    for (const LayerParams &l : t) s += layer_print_time_s(l);
    return round_away(s, 2);
}


Mask orient_mask(const Mask &m, std::optional<FlipDirection> flip, RotateDirection rotate)
{
    return rotate_mask(flip ? flip_mask(m, *flip) : m, rotate);
}
Mask resize_mask_bilinear(const Mask &m, size_t w, size_t h) { return resize_bilinear(m, w, h); }
Mask erode_mask(const Mask &m, int iterations) { return erode(m, iterations); }
Mask dilate_mask(const Mask &m, int iterations) { return dilate(m, iterations); }
Mask crop_mask(const Mask &m, const Rect &r) { check_roi(r, m.width, m.height); return r.empty() ? m : crop(m, r); }

} // namespace Slic3r::sla::layerops
