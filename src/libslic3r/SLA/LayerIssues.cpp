// SPDX-License-Identifier: AGPL-3.0-or-later
#include "LayerIssues.hpp"
#include "LayerMemoryBudget.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <stdexcept>
#include <tbb/parallel_for.h>

#include "libslic3r/PrintBase.hpp" // CanceledException

namespace Slic3r { namespace sla {

namespace {

constexpr size_t k_block_layers = 16; // layers decoded and analysed per parallel block

// ---------------------------------------------------------------- geometry

struct Component
{
    int    x0, y0, x1, y1; // inclusive bbox
    size_t area;
};

// Labels connected foreground (non-zero) components in raster-discovery order.
// labels[i] == 0 is background, component k has label k + 1.
void label_components(const std::vector<uint8_t>& fg, int w, int h, bool diag,
                      std::vector<int32_t>& labels, std::vector<Component>& comps)
{
    labels.assign(size_t(w) * h, 0);
    comps.clear();
    std::vector<int> stack;
    for (int sy = 0; sy < h; ++sy)
        for (int sx = 0; sx < w; ++sx) {
            const size_t s = size_t(sy) * w + sx;
            if (!fg[s] || labels[s]) continue;
            const int32_t id = int32_t(comps.size()) + 1;
            Component c{sx, sy, sx, sy, 0};
            labels[s] = id;
            stack.push_back(int(s));
            while (!stack.empty()) {
                const int  cur = stack.back();
                stack.pop_back();
                const int x = cur % w, y = cur / w;
                ++c.area;
                c.x0 = std::min(c.x0, x); c.x1 = std::max(c.x1, x);
                c.y0 = std::min(c.y0, y); c.y1 = std::max(c.y1, y);
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        if ((dx == 0 && dy == 0) || (!diag && dx != 0 && dy != 0)) continue;
                        const int nx = x + dx, ny = y + dy;
                        if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
                        const size_t n = size_t(ny) * w + nx;
                        if (fg[n] && !labels[n]) { labels[n] = id; stack.push_back(int(n)); }
                    }
            }
            comps.push_back(c);
        }
}

std::vector<PixelRun> runs_of_label(const std::vector<int32_t>& labels, int w, const Component& c,
                                    int32_t label, int ox, int oy)
{
    std::vector<PixelRun> runs;
    for (int y = c.y0; y <= c.y1; ++y)
        for (int x = c.x0; x <= c.x1;) {
            if (labels[size_t(y) * w + x] != label) { ++x; continue; }
            int e = x;
            while (e <= c.x1 && labels[size_t(y) * w + e] == label) ++e;
            runs.push_back({y + oy, x + ox, e + ox});
            x = e;
        }
    return runs;
}

bool runs_intersect(const std::vector<PixelRun>& a, const std::vector<PixelRun>& b)
{
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        const PixelRun &p = a[i], &q = b[j];
        if (p.y < q.y) ++i;
        else if (q.y < p.y) ++j;
        else {
            if (p.x0 < q.x1 && q.x0 < p.x1) return true;
            if (p.x1 < q.x1) ++i; else ++j;
        }
    }
    return false;
}

void merge_bbox(BoundingBox& bb, const BoundingBox& other)
{
    if (!other.defined) return;
    if (!bb.defined) { bb = other; return; }
    bb = BoundingBox(Point(std::min(bb.min.x(), other.min.x()), std::min(bb.min.y(), other.min.y())),
                     Point(std::max(bb.max.x(), other.max.x()), std::max(bb.max.y(), other.max.y())));
}

LayerIssuePart make_part(size_t layer, std::vector<PixelRun> runs, const BoundingBox& bbox, size_t count)
{
    LayerIssuePart p;
    p.layer = layer;
    p.bbox = bbox;
    p.pixel_count = count;
    p.runs = std::move(runs);
    return p;
}

BoundingBox bbox_of(int x0, int y0, int x1, int y1)
{
    return BoundingBox(Point(x0, y0), Point(x1, y1));
}

LayerIssue single_layer_issue(LayerIssue::Type type, LayerIssuePart part)
{
    LayerIssue is;
    is.type = type;
    is.first_layer = is.last_layer = part.layer;
    is.bbox = part.bbox;
    is.pixel_count = part.pixel_count;
    is.parts.push_back(std::move(part));
    return is;
}

// Erodes a binary mask with a cross of radius n px (UVtools: one iteration of a
// (2n+1)x(2n+1) MorphShapes.Cross kernel). Pixels outside the image do not erode.
std::vector<uint8_t> erode_cross(const std::vector<uint8_t>& m, int w, int h, int n)
{
    constexpr int far = 1 << 28;
    std::vector<uint8_t> keep(m.size(), 0);
    // Horizontal pass: a pixel survives when no zero lies within [x-n, x+n] on its row.
    for (int y = 0; y < h; ++y) {
        const uint8_t* row = &m[size_t(y) * w];
        uint8_t*       out = &keep[size_t(y) * w];
        int last_zero = -far;
        for (int x = 0; x < w; ++x) {
            if (!row[x]) last_zero = x;
            out[x] = row[x] && x - last_zero > n;
        }
        int next_zero = far;
        for (int x = w - 1; x >= 0; --x) {
            if (!row[x]) next_zero = x;
            if (next_zero - x <= n) out[x] = 0;
        }
    }
    // Vertical pass on the same mask, combined with AND.
    std::vector<int> zero_at(w, -far);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            if (!m[size_t(y) * w + x]) zero_at[x] = y;
            if (y - zero_at[x] <= n) keep[size_t(y) * w + x] = 0;
        }
    std::fill(zero_at.begin(), zero_at.end(), far);
    for (int y = h - 1; y >= 0; --y)
        for (int x = 0; x < w; ++x) {
            if (!m[size_t(y) * w + x]) zero_at[x] = y;
            if (zero_at[x] - y <= n) keep[size_t(y) * w + x] = 0;
        }
    return keep;
}

// Pixels that grew by more than 127 grey levels versus the previous layer,
// eroded; empty vector when nothing grew. `prev` null means a blank layer.
std::vector<uint8_t> overhang_mask(const uint8_t* cur, const uint8_t* prev, int w, int h, int erode)
{
    std::vector<uint8_t> m(size_t(w) * h, 0);
    bool any = false;
    // UVtools erodes inside the ROI = bounding rectangle of the union of the current and the
    // previous layer, with a Reflect101 border, so pixels on the ROI edge are never eroded from
    // outside. Track that rectangle to reproduce the same overhang extents (verified against
    // `UVtoolsCmd print-issues`, tests/oracle/issues_vs_uvtools.py).
    int x0 = w, y0 = h, x1 = -1, y1 = -1;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const size_t i = size_t(y) * w + x;
            if (cur[i] || (prev && prev[i])) {
                x0 = std::min(x0, x); x1 = std::max(x1, x);
                y0 = std::min(y0, y); y1 = std::max(y1, y);
            }
            const int d = int(cur[i]) - (prev ? int(prev[i]) : 0);
            if (d > 127) { m[i] = 1; any = true; }
        }
    if (!any) return {};
    // Erode the ROI crop; erode_cross treats everything outside its input as non-eroding.
    const int rw = x1 - x0 + 1, rh = y1 - y0 + 1;
    std::vector<uint8_t> roi(size_t(rw) * rh);
    for (int y = 0; y < rh; ++y)
        std::copy_n(&m[size_t(y0 + y) * w + x0], rw, &roi[size_t(y) * rw]);
    roi = erode_cross(roi, rw, rh, std::max(erode, 1));
    std::fill(m.begin(), m.end(), uint8_t(0));
    for (int y = 0; y < rh; ++y)
        std::copy_n(&roi[size_t(y) * rw], rw, &m[size_t(y0 + y) * w + x0]);
    return m;
}

// A layer cropped to a rectangle plus a one pixel black ring, so that "outside"
// air always reaches the frame border.
struct Frame
{
    int ox = 0, oy = 0, w = 0, h = 0; // frame (0,0) is layer pixel (ox,oy)
    std::vector<uint8_t> pix;
};

Frame crop_pad(const LayerMask& m, int x0, int y0, int x1, int y1) // inclusive rect
{
    Frame f;
    f.ox = x0 - 1; f.oy = y0 - 1;
    f.w = x1 - x0 + 3; f.h = y1 - y0 + 3;
    f.pix.assign(size_t(f.w) * f.h, 0);
    if (m.data.empty()) return f;
    for (int y = 0; y < f.h; ++y) {
        const int sy = f.oy + y;
        if (sy < 0 || sy >= int(m.height)) continue;
        for (int x = 0; x < f.w; ++x) {
            const int sx = f.ox + x;
            if (sx >= 0 && sx < int(m.width)) f.pix[size_t(y) * f.w + x] = m.data[size_t(sy) * m.width + sx];
        }
    }
    return f;
}

std::vector<uint8_t> binarize(const std::vector<uint8_t>& pix, uint8_t threshold)
{
    std::vector<uint8_t> out(pix.size());
    for (size_t i = 0; i < pix.size(); ++i) out[i] = threshold > 0 ? pix[i] > threshold : pix[i] != 0;
    return out;
}

// Background pixels (bw == 0) 4-connected to the frame border.
std::vector<uint8_t> outside_background(const std::vector<uint8_t>& bw, int w, int h)
{
    std::vector<uint8_t> out(bw.size(), 0);
    std::vector<int>     stack;
    stack.push_back(0); // the pad ring guarantees (0,0) is background
    out[0] = 1;
    while (!stack.empty()) {
        const int cur = stack.back();
        stack.pop_back();
        const int x = cur % w, y = cur / w;
        const int nb[4][2] = {{x - 1, y}, {x + 1, y}, {x, y - 1}, {x, y + 1}};
        for (const auto& n : nb) {
            if (n[0] < 0 || n[1] < 0 || n[0] >= w || n[1] >= h) continue;
            const size_t idx = size_t(n[1]) * w + n[0];
            if (!bw[idx] && !out[idx]) { out[idx] = 1; stack.push_back(int(idx)); }
        }
    }
    return out;
}

// ---------------------------------------------------------------- hollows

// An enclosed background region of one layer (UVtools negative contour).
struct Hollow
{
    enum class Kind { Unresolved, Trap, Air, Skipped };

    size_t                layer = 0;
    std::vector<PixelRun> runs; // full layer coordinates
    BoundingBox           bbox;
    size_t                area = 0;
    Kind                  kind = Kind::Unresolved;
    bool                  suction = false;
};

bool hollows_intersect(const Hollow& a, const Hollow& b)
{
    if (!a.bbox.overlap(b.bbox)) return false;
    return runs_intersect(a.runs, b.runs);
}

using Group = std::vector<Hollow*>;

// Adds `h` to the group list: joins the groups whose last item is on this or
// the layer above and touches `h`, merging them when several match.
void add_to_groups(std::vector<Group>& groups, Hollow* h)
{
    std::vector<size_t> overlapping;
    for (size_t g = 0; g < groups.size(); ++g) {
        const Hollow* last = groups[g].back();
        if (last->layer != h->layer && last->layer != h->layer + 1) continue;
        if (hollows_intersect(*last, *h)) overlapping.push_back(g);
    }
    if (overlapping.empty()) {
        groups.push_back({h});
    } else if (overlapping.size() == 1) {
        groups[overlapping[0]].push_back(h);
    } else {
        Group combined;
        for (size_t g : overlapping) combined.insert(combined.end(), groups[g].begin(), groups[g].end());
        for (size_t i = overlapping.size(); i-- > 0;) groups.erase(groups.begin() + overlapping[i]);
        combined.push_back(h);
        groups.push_back(std::move(combined));
    }
}

// A new suction hollow turns every trap group touching it into suction cups.
void convert_groups_to_suction(std::vector<Group>& groups, const Hollow& h)
{
    for (size_t g = groups.size(); g-- > 0;) {
        Group& group = groups[g];
        if (group.back()->layer > h.layer + 1) continue; // disconnected by at least one layer
        for (size_t c = group.size(); c-- > 0;) {
            if (group[c]->layer > h.layer + 1) break;
            if (!hollows_intersect(*group[c], h)) continue;
            for (Hollow* item : group) item->suction = true;
            groups.erase(groups.begin() + g);
            break;
        }
    }
}

LayerIssue issue_from_group(LayerIssue::Type type, const Group& group)
{
    LayerIssue is;
    is.type = type;
    is.first_layer = SIZE_MAX;
    is.last_layer = 0;
    for (const Hollow* h : group) {
        is.parts.push_back(make_part(h->layer, h->runs, h->bbox, h->area));
        is.first_layer = std::min(is.first_layer, h->layer);
        is.last_layer = std::max(is.last_layer, h->layer);
        is.pixel_count += h->area;
        merge_bbox(is.bbox, h->bbox);
    }
    std::sort(is.parts.begin(), is.parts.end(), [](const LayerIssuePart& a, const LayerIssuePart& b) {
        if (a.layer != b.layer) return a.layer < b.layer;
        if (a.bbox.min.y() != b.bbox.min.y()) return a.bbox.min.y() < b.bbox.min.y();
        return a.bbox.min.x() < b.bbox.min.x();
    });
    return is;
}

// ---------------------------------------------------------------- per layer

struct LayerResult
{
    bool                   empty = true;
    int                    x0 = 0, y0 = 0, x1 = -1, y1 = -1; // inclusive bbox of non-zero pixels
    std::vector<LayerIssue> issues;
};

bool listed(const std::optional<std::unordered_set<size_t>>& wl, size_t i)
{
    return !wl || wl->count(i) > 0;
}

void find_touching_bounds(const LayerMask& cur, size_t layer, const LayerResult& lr,
                          const TouchingBoundDetectionConfig& cfg, std::vector<LayerIssue>& out)
{
    const int W = int(cur.width), H = int(cur.height);
    const int bx0 = lr.x0, by0 = lr.y0, bx1 = lr.x1 + 1, by1 = lr.y1 + 1; // exclusive right/bottom
    const bool top = by0 <= cfg.margin_top_px, bottom = by1 >= H - cfg.margin_bottom_px;
    const bool left = bx0 <= cfg.margin_left_px, right = bx1 >= W - cfg.margin_right_px;
    std::vector<std::pair<int, int>> pts; // (y, x)
    auto hit = [&](int x, int y) {
        if (x >= 0 && y >= 0 && x < W && y < H && cur.data[size_t(y) * W + x] >= cfg.minimum_pixel_brightness)
            pts.emplace_back(y, x);
    };
    if (top || bottom)
        for (int x = bx0; x < bx1; ++x) {
            if (top) for (int y = by0; y < cfg.margin_top_px; ++y) hit(x, y);
            if (bottom) for (int y = H - cfg.margin_bottom_px; y < by1; ++y) hit(x, y);
        }
    if (left || right)
        for (int y = by0 + cfg.margin_top_px; y < by1 - cfg.margin_bottom_px; ++y) {
            if (left) for (int x = bx0; x < cfg.margin_left_px; ++x) hit(x, y);
            if (right) for (int x = bx1 - cfg.margin_right_px; x < bx1; ++x) hit(x, y);
        }
    if (pts.empty()) return;
    std::sort(pts.begin(), pts.end());
    std::vector<PixelRun> runs;
    int mnx = INT32_MAX, mny = INT32_MAX, mxx = 0, mxy = 0;
    for (const auto& p : pts) {
        mnx = std::min(mnx, p.second); mxx = std::max(mxx, p.second);
        mny = std::min(mny, p.first);  mxy = std::max(mxy, p.first);
        if (!runs.empty() && runs.back().y == p.first && runs.back().x1 == p.second) ++runs.back().x1;
        else runs.push_back({p.first, p.second, p.second + 1});
    }
    out.push_back(single_layer_issue(LayerIssue::Type::TouchingBound,
        make_part(layer, std::move(runs), bbox_of(mnx, mny, mxx, mxy), pts.size())));
}

// Overhang and island detection of layer `i` against the layer below.
void find_islands_and_overhangs(const LayerMask& cur, const uint8_t* prev, size_t layer,
                                const IssueDetectionConfig& cfg, std::vector<LayerIssue>& out)
{
    const int w = int(cur.width), h = int(cur.height);
    const uint8_t* cp = cur.data.data();
    std::vector<uint8_t> oh_mask;
    bool oh_computed = false;
    auto ensure_overhang_mask = [&]() {
        if (!oh_computed) { oh_mask = overhang_mask(cp, prev, w, h, cfg.overhang.erode_iterations); oh_computed = true; }
    };
    std::vector<BoundingBox> overhang_boxes;

    if (cfg.overhang.enabled && listed(cfg.overhang.white_list_layers, layer)) {
        ensure_overhang_mask();
        if (!oh_mask.empty()) {
            std::vector<int32_t> labels;
            std::vector<Component> comps;
            label_components(oh_mask, w, h, true, labels, comps);
            for (size_t k = 0; k < comps.size(); ++k) {
                const Component& c = comps[k];
                // A one pixel wide blob is a degenerate contour (a line): ignore, like UVtools.
                if (c.x0 == c.x1 || c.y0 == c.y1) continue;
                if (c.area < cfg.overhang.required_pixels_to_consider) continue;
                BoundingBox bb = bbox_of(c.x0, c.y0, c.x1, c.y1);
                overhang_boxes.push_back(bb);
                out.push_back(single_layer_issue(LayerIssue::Type::Overhang,
                    make_part(layer, runs_of_label(labels, w, c, int32_t(k + 1), 0, 0), bb, c.area)));
            }
        }
    }

    const IslandDetectionConfig& ic = cfg.island;
    if (!ic.enabled || !listed(ic.white_list_layers, layer)) return;
    std::vector<uint8_t> fg = binarize(cur.data, ic.binary_threshold);
    std::vector<int32_t> labels;
    std::vector<Component> comps;
    label_components(fg, w, h, ic.allow_diagonal_bonds, labels, comps);
    for (size_t k = 0; k < comps.size(); ++k) {
        const Component& c = comps[k];
        if (c.area < ic.required_area_to_process_check_px) continue;
        const int32_t label = int32_t(k + 1);
        size_t points = 0, supporting = 0;
        std::vector<PixelRun> runs;
        for (int y = c.y0; y <= c.y1; ++y)
            for (int x = c.x0; x <= c.x1; ++x) {
                const size_t idx = size_t(y) * w + x;
                if (labels[idx] != label || cp[idx] < ic.required_pixel_brightness_to_process_check) continue;
                ++points;
                if (!runs.empty() && runs.back().y == y && runs.back().x1 == x) ++runs.back().x1;
                else runs.push_back({y, x, x + 1});
                if (prev && prev[idx] >= ic.required_pixel_brightness_to_support) ++supporting;
            }
        if (points == 0) continue;
        const double required = std::max(1.0, double(points) * ic.required_pixels_to_support_multiplier);
        if (double(supporting) >= required) continue; // supported by the layer below

        const BoundingBox bb = bbox_of(c.x0, c.y0, c.x1, c.y1);
        // Weakly supported: keep it only if it also overhangs (UVtools EnhancedDetection).
        if (ic.enhanced_detection && supporting >= 10 && double(supporting) >= required / 4.) {
            if (cfg.overhang.enabled &&
                std::none_of(overhang_boxes.begin(), overhang_boxes.end(),
                             [&](const BoundingBox& o) { return o.overlap(bb); }))
                continue;
            ensure_overhang_mask();
            size_t overhang_px = 0;
            if (!oh_mask.empty())
                for (int y = c.y0; y <= c.y1 && overhang_px < cfg.overhang.required_pixels_to_consider; ++y)
                    for (int x = c.x0; x <= c.x1 && overhang_px < cfg.overhang.required_pixels_to_consider; ++x)
                        if (labels[size_t(y) * w + x] == label && oh_mask[size_t(y) * w + x]) ++overhang_px;
            if (overhang_px < cfg.overhang.required_pixels_to_consider) continue;
        }
        out.push_back(single_layer_issue(LayerIssue::Type::Island, make_part(layer, std::move(runs), bb, points)));
    }
}

void find_hollows(const LayerMask& cur, size_t layer, const LayerResult& lr,
                  const ResinTrapDetectionConfig& rc, std::vector<Hollow>& out)
{
    const Frame f = crop_pad(cur, lr.x0, lr.y0, lr.x1, lr.y1);
    const std::vector<uint8_t> bw = binarize(f.pix, rc.binary_threshold);
    const std::vector<uint8_t> outside = outside_background(bw, f.w, f.h);
    std::vector<uint8_t> enclosed(bw.size());
    for (size_t i = 0; i < bw.size(); ++i) enclosed[i] = !bw[i] && !outside[i];
    std::vector<int32_t> labels;
    std::vector<Component> comps;
    label_components(enclosed, f.w, f.h, false, labels, comps); // background is 4-connected
    for (size_t k = 0; k < comps.size(); ++k) {
        const Component& c = comps[k];
        if (c.area < rc.required_area_to_process_check_px) continue;
        Hollow hl;
        hl.layer = layer;
        hl.runs = runs_of_label(labels, f.w, c, int32_t(k + 1), f.ox, f.oy);
        hl.bbox = bbox_of(c.x0 + f.ox, c.y0 + f.oy, c.x1 + f.ox, c.y1 + f.oy);
        hl.area = c.area;
        out.push_back(std::move(hl));
    }
}

bool is_blank(const LayerMask& m)
{
    return std::all_of(m.data.begin(), m.data.end(), [](uint8_t v) { return v == 0; });
}

} // namespace

std::vector<LayerIssue> detect_layer_issues(const LayerMaskProvider&                   provider,
                                            const std::vector<double>&                 z,
                                            const IssueDetectionConfig&                cfg,
                                            const std::function<void(size_t, size_t)>& progress,
                                            const std::function<bool()>&               cancel)
{
    const size_t count = z.size();
    std::vector<LayerIssue> result;
    if (count == 0) return result;

    const ResinTrapDetectionConfig& rc = cfg.resin_trap;
    const bool need_pixels = cfg.island.enabled || cfg.overhang.enabled || rc.enabled ||
                             cfg.touching_bound.enabled || cfg.empty_layer.enabled;
    const size_t resin_start = std::min(rc.start_layer_index, count);
    const size_t resin_layers = rc.enabled ? count - resin_start : 0;
    const size_t total = (need_pixels ? count : 0) + 2 * resin_layers;
    size_t done = 0;

    std::atomic<bool> canceled{false};
    auto check_cancel = [&]() {
        if (canceled.load()) return true;
        if (cancel && cancel()) canceled.store(true);
        return canceled.load();
    };
    auto throw_if_canceled = [&]() { if (check_cancel()) throw CanceledException(); };
    auto report = [&](size_t n) { done += n; if (progress) progress(done, total); };

    // Print height: Z only, no pixels needed.
    if (cfg.print_height.enabled && cfg.print_height.machine_height_mm > 0.) {
        const double limit = std::round((cfg.print_height.machine_height_mm + cfg.print_height.offset_mm) * 1000.) / 1000.;
        const double top = *std::max_element(z.begin(), z.end());
        if (top > limit) {
            LayerIssue is;
            is.type = LayerIssue::Type::PrintHeight;
            is.first_layer = SIZE_MAX;
            for (size_t i = 0; i < count; ++i)
                if (z[i] > limit) {
                    is.first_layer = std::min(is.first_layer, i);
                    is.last_layer = i;
                    is.parts.push_back(make_part(i, {}, BoundingBox(), 0));
                }
            result.push_back(std::move(is));
        }
    }

    std::vector<LayerResult>        layer_res(count);
    std::vector<std::vector<Hollow>> hollows(count);
    int gx0 = INT32_MAX, gy0 = INT32_MAX, gx1 = -1, gy1 = -1;

    if (need_pixels) {
        const bool first_z_check = count > 0;
        const double first_z = first_z_check ? z[0] : 0.;
        // Block size = layers analysed in parallel. The detector keeps block + 1 decoded masks (the previous layer is the
        // window of islands / overhangs) and every worker needs about nine more layer-sizes of temporaries (labels,
        // erosions), so the block shrinks for huge layers to stay within the decoded-pixel budget.
        size_t block_layers = k_block_layers;
        {
            throw_if_canceled();
            const LayerMask probe = provider(0);
            const uint64_t per_layer = std::max<uint64_t>(1, probe.width * probe.height);
            block_layers = size_t(std::clamp<uint64_t>(layer_decoded_budget() / (per_layer * 10), 1, k_block_layers));
        }
        for (size_t b = 0; b < count; b += block_layers) {
            throw_if_canceled();
            const size_t end = std::min(count, b + block_layers);
            const size_t cache_begin = b > 0 ? b - 1 : 0; // previous layer for the first layer of the block
            std::vector<LayerMask> cache(end - cache_begin);
            tbb::parallel_for(tbb::blocked_range<size_t>(cache_begin, end), [&](const tbb::blocked_range<size_t>& r) {
                for (size_t i = r.begin(); i != r.end(); ++i) {
                    if (check_cancel()) return;
                    LayerMask m = provider(i);
                    if (!m.data.empty() && m.data.size() != m.width * m.height)
                        throw std::runtime_error("layer issue detection: layer mask size mismatch");
                    cache[i - cache_begin] = std::move(m);
                }
            });
            throw_if_canceled();
            tbb::parallel_for(tbb::blocked_range<size_t>(b, end), [&](const tbb::blocked_range<size_t>& r) {
                for (size_t i = r.begin(); i != r.end(); ++i) {
                    if (check_cancel()) return;
                    const LayerMask& cur = cache[i - cache_begin];
                    LayerResult& lr = layer_res[i];
                    if (cur.data.empty() || is_blank(cur)) continue; // lr.empty stays true
                    lr.empty = false;
                    const int W = int(cur.width), H = int(cur.height);
                    lr.x0 = W; lr.y0 = H; lr.x1 = -1; lr.y1 = -1;
                    for (int y = 0; y < H; ++y)
                        for (int x = 0; x < W; ++x)
                            if (cur.data[size_t(y) * W + x]) {
                                lr.x0 = std::min(lr.x0, x); lr.x1 = std::max(lr.x1, x);
                                lr.y0 = std::min(lr.y0, y); lr.y1 = std::max(lr.y1, y);
                            }
                    if (cfg.touching_bound.enabled) find_touching_bounds(cur, i, lr, cfg.touching_bound, lr.issues);
                    // No islands nor overhangs on the first layer or on the plate.
                    if (i > 0 && z[i] > first_z && (cfg.island.enabled || cfg.overhang.enabled)) {
                        const LayerMask& pm = cache[i - 1 - cache_begin];
                        const bool prev_ok = !pm.data.empty() && pm.width == cur.width && pm.height == cur.height;
                        find_islands_and_overhangs(cur, prev_ok ? pm.data.data() : nullptr, i, cfg, lr.issues);
                    }
                    if (rc.enabled && i >= resin_start) find_hollows(cur, i, lr, rc, hollows[i]);
                }
            });
            throw_if_canceled();
            report(end - b);
        }
        for (size_t i = 0; i < count; ++i) {
            const LayerResult& lr = layer_res[i];
            if (lr.empty) continue;
            gx0 = std::min(gx0, lr.x0); gy0 = std::min(gy0, lr.y0);
            gx1 = std::max(gx1, lr.x1); gy1 = std::max(gy1, lr.y1);
            for (const LayerIssue& is : lr.issues) result.push_back(is);
        }
    }

    // Empty layers, classified as starting / loose / ending.
    if (cfg.empty_layer.enabled) {
        size_t first_solid = count, last_solid = 0;
        for (size_t i = 0; i < count; ++i)
            if (!layer_res[i].empty) { first_solid = std::min(first_solid, i); last_solid = i; }
        for (size_t i = 0; i < count; ++i) {
            if (!layer_res[i].empty) continue;
            const bool starting = i < first_solid;
            const bool ending = first_solid != count && i > last_solid;
            const bool ignored = starting ? cfg.empty_layer.ignore_starting
                               : ending   ? cfg.empty_layer.ignore_ending
                                          : cfg.empty_layer.ignore_loose;
            if (!ignored)
                result.push_back(single_layer_issue(LayerIssue::Type::EmptyLayer, make_part(i, {}, BoundingBox(), 0)));
        }
    }

    // Resin traps and suction cups: air propagation up then down (UVtools two pass algorithm).
    if (rc.enabled && gx1 >= 0 && resin_layers > 0) {
        const uint8_t drain = rc.required_black_pixels_to_drain;
        auto make_frame = [&](size_t i) { return crop_pad(provider(i), gx0, gy0, gx1, gy1); };
        auto solid_of = [&](const Frame& f) {
            std::vector<uint8_t> s(f.pix.size());
            for (size_t k = 0; k < s.size(); ++k)
                s[k] = rc.maximum_pixel_brightness_to_drain > 0 ? f.pix[k] > rc.maximum_pixel_brightness_to_drain : f.pix[k] != 0;
            return s;
        };
        // Air of one layer: background connected to the outside that is not solid for the drain threshold.
        auto layer_air = [&](const Frame& f, const std::vector<uint8_t>& solid) {
            std::vector<uint8_t> out = outside_background(binarize(f.pix, rc.binary_threshold), f.w, f.h);
            for (size_t k = 0; k < out.size(); ++k) out[k] = out[k] && !solid[k];
            return out;
        };
        auto overlap = [&](const std::vector<uint8_t>& air, const Frame& f, const Hollow& hl) {
            size_t n = 0;
            for (const PixelRun& r : hl.runs)
                for (int x = r.x0; x < r.x1; ++x) n += air[size_t(r.y - f.oy) * f.w + (x - f.ox)];
            return n;
        };
        auto paint = [&](std::vector<uint8_t>& air, const Frame& f, const Hollow& hl, uint8_t v) {
            for (const PixelRun& r : hl.runs)
                for (int x = r.x0; x < r.x1; ++x) air[size_t(r.y - f.oy) * f.w + (x - f.ox)] = v;
        };
        auto step_air = [&](std::vector<uint8_t>& air, const std::vector<uint8_t>& solid, const std::vector<uint8_t>& lair) {
            for (size_t k = 0; k < air.size(); ++k) air[k] = (air[k] && !solid[k]) || lair[k];
        };

        // Pass 1, bottom to top: hollows not reaching known air are trap candidates.
        std::vector<uint8_t> air;
        for (size_t i = resin_start; i < count; ++i) {
            throw_if_canceled();
            const Frame f = make_frame(i);
            const std::vector<uint8_t> solid = solid_of(f);
            const std::vector<uint8_t> lair = layer_air(f, solid);
            if (i == resin_start) air = lair;
            step_air(air, solid, lair);
            for (Hollow& hl : hollows[i]) {
                const size_t ov = overlap(air, f, hl);
                if (ov == 0) hl.kind = Hollow::Kind::Trap;
                else if (ov >= drain) { hl.kind = Hollow::Kind::Air; paint(air, f, hl, 1); }
                else { hl.kind = Hollow::Kind::Skipped; paint(air, f, hl, 0); } // overlaps too little: treat as solid
            }
            report(1);
        }

        // Pass 2, top to bottom: candidates that reach air from above are suction cups.
        std::vector<Group> groups;
        for (size_t i = count; i-- > resin_start;) {
            throw_if_canceled();
            const Frame f = make_frame(i);
            const std::vector<uint8_t> solid = solid_of(f);
            std::vector<uint8_t> lair = layer_air(f, solid);
            if (i == count - 1) { // top layer: everything open is air
                air.assign(solid.size(), 0);
                for (size_t k = 0; k < air.size(); ++k) air[k] = !solid[k];
            }
            for (const Hollow& hl : hollows[i]) if (hl.kind == Hollow::Kind::Air) paint(lair, f, hl, 1);
            step_air(air, solid, lair);
            for (Hollow& hl : hollows[i]) {
                if (hl.kind != Hollow::Kind::Trap) continue;
                if (overlap(air, f, hl) >= drain) {
                    paint(air, f, hl, 1);
                    hl.suction = true;
                    convert_groups_to_suction(groups, hl);
                } else {
                    paint(air, f, hl, 0);
                    add_to_groups(groups, &hl);
                }
            }
            report(1);
        }

        // Interpolate hollows into multi-layer issues.
        std::vector<Group> trap_groups, suction_groups;
        for (size_t i = count; i-- > 0;)
            for (Hollow& hl : hollows[i]) {
                if (hl.kind != Hollow::Kind::Trap) continue;
                if (hl.suction) {
                    if (rc.detect_suction_cups && hl.area >= rc.required_area_to_consider_suction_cup_px)
                        add_to_groups(suction_groups, &hl);
                } else {
                    add_to_groups(trap_groups, &hl);
                }
            }
        for (const Group& g : trap_groups) {
            const bool on_plate = std::any_of(g.begin(), g.end(), [](const Hollow* h) { return h->layer == 0; });
            if (!on_plate) result.push_back(issue_from_group(LayerIssue::Type::ResinTrap, g));
        }
        for (const Group& g : suction_groups) {
            LayerIssue is = issue_from_group(LayerIssue::Type::SuctionCup, g);
            const size_t f = is.first_layer;
            const double layer_h = f == 0 ? z[0] : z[f] - z[f - 1];
            // UVtools MainIssue.TotalHeight is Layer.RoundHeight(...) (3 decimals) and is compared with
            // the threshold as decimal. A 0.5 mm cup of five 0.1 mm layers must pass a 0.5 mm limit
            // although the Z values come from floats (float noise of ~1e-6 would otherwise reject it).
            const double total_height = std::round((layer_h + z[is.last_layer] - z[f]) * 1000.) / 1000.;
            if (total_height + 1e-9 >= rc.required_height_to_consider_suction_cup_mm)
                result.push_back(std::move(is));
        }
    }

    std::stable_sort(result.begin(), result.end(), [](const LayerIssue& a, const LayerIssue& b) {
        if (a.type != b.type) return a.type < b.type;
        if (a.first_layer != b.first_layer) return a.first_layer < b.first_layer;
        if (a.pixel_count != b.pixel_count) return a.pixel_count > b.pixel_count;
        const int ay = a.bbox.defined ? int(a.bbox.min.y()) : 0, by = b.bbox.defined ? int(b.bbox.min.y()) : 0;
        if (ay != by) return ay < by;
        return (a.bbox.defined ? a.bbox.min.x() : 0) < (b.bbox.defined ? b.bbox.min.x() : 0);
    });
    return result;
}

}} // namespace Slic3r::sla
