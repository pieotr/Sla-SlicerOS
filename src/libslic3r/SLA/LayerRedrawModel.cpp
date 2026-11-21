// SPDX-License-Identifier: AGPL-3.0-or-later
#include "LayerRedrawModel.hpp"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>
#include <opencv2/imgproc.hpp>
#include <stdexcept>

namespace Slic3r::sla::layerops {
namespace {
Mask crop_rect(const Mask &m, const Rect &r)
{
    if (r.empty()) return m;
    Mask out(size_t(r.w), size_t(r.h));
    for (int y = 0; y < r.h; ++y) std::memcpy(&out.px[size_t(y) * out.width], &m.px[size_t(r.y + y) * m.width + size_t(r.x)], size_t(r.w));
    return out;
}
void paste_rect(Mask &dst, const Rect &r, const Mask &src)
{
    if (r.empty()) { dst = src; return; }
    for (int y = 0; y < r.h; ++y) std::memcpy(&dst.px[size_t(r.y + y) * dst.width + size_t(r.x)], &src.px[size_t(y) * src.width], size_t(r.w));
}
uint8_t sat(int v) { return uint8_t(std::min(255, std::max(0, v))); }
const char *operator_name(RedrawOperator op)
{
    switch (op) {
    case RedrawOperator::Set: return "Set"; case RedrawOperator::Add: return "Add"; case RedrawOperator::Subtract: return "Subtract";
    case RedrawOperator::Multiply: return "Multiply"; case RedrawOperator::Divide: return "Divide"; case RedrawOperator::Minimum: return "Minimum";
    case RedrawOperator::Maximum: return "Maximum"; case RedrawOperator::AbsDiff: return "AbsDiff";
    }
    return "";
}
} // namespace

std::string validate_redraw_model(const RedrawModelParams &p)
{
    std::string sb;
    if (p.type == RedrawType::Model || p.contact_points_only) {
        const std::string name = operator_name(p.op);
        switch (p.op) {
        case RedrawOperator::Add: case RedrawOperator::Subtract: case RedrawOperator::Maximum: case RedrawOperator::AbsDiff:
            if (p.brightness == 0) sb += name + " with a brightness of 0 yield no result, please use a value larger than 0.\\n";
            break;
        case RedrawOperator::Divide:
            if (p.brightness == 0) sb += name + " with a brightness of 0 is not valid, please use a value larger than 0.\\n";
            else if (p.brightness == 1) sb += name + " with a brightness of 1 yield no result, please use a value larger than 1.\\n";
            break;
        default: break;
        }
    }
    return sb;
}

bool redraw_model_layer(Mask &full_layer, const Mask &body_layer, const Mask *body_next, const RedrawModelParams &p)
{
    if (full_layer.width != body_layer.width || full_layer.height != body_layer.height)
        throw std::invalid_argument("The model-only file has another resolution than the document.");
    Mask full = crop_rect(full_layer, p.roi);
    const Mask body = crop_rect(body_layer, p.roi);
    Mask supports(full.width, full.height);
    bool modified = false;
    const size_t n = full.px.size();
    if (p.type == RedrawType::Supports && p.contact_points_only) {
        if (!body_next) return false;
        for (size_t i = 0; i < n; ++i) supports.px[i] = sat(int(full.px[i]) - int(body.px[i]));
        const Mask next = crop_rect(*body_next, p.roi);
        Mask copy = supports;
        std::vector<std::vector<cv::Point>> contours;
        cv::findContours(cv::Mat(int(copy.height), int(copy.width), CV_8UC1, copy.px.data()), contours, cv::RETR_LIST, cv::CHAIN_APPROX_SIMPLE);
        if (contours.empty()) return false;
        cv::Mat fmat(int(full.height), int(full.width), CV_8UC1, full.px.data());
        for (size_t c = 0; c < contours.size(); ++c) {
            // cv::boundingRect lives in libopencv_geometry (OpenCV 5), which is not linked: the bounds of the points.
            int bx0 = INT_MAX, by0 = INT_MAX, bx1 = INT_MIN, by1 = INT_MIN;
            for (const cv::Point &pt : contours[c]) { bx0 = std::min(bx0, pt.x); by0 = std::min(by0, pt.y); bx1 = std::max(bx1, pt.x); by1 = std::max(by1, pt.y); }
            const cv::Rect box(bx0, by0, bx1 - bx0 + 1, by1 - by0 + 1);
            bool found = false;
            for (int y = box.y; y < box.y + box.height && !found; ++y)
                for (int x = box.x; x < box.x + box.width; ++x) {
                    const size_t pos = size_t(y) * full.width + size_t(x);
                    if (p.ignore_contactless_pixels) {
                        if (supports.px[pos] <= 10) continue;
                        if (next.px[pos] <= 0) continue;
                        modified = true;
                        full.px[pos] = p.brightness;
                    } else {
                        if (supports.px[pos] <= 100) continue;
                        if (next.px[pos] <= 150) continue;
                        cv::drawContours(fmat, contours, int(c), cv::Scalar(p.brightness), -1, cv::LINE_AA);
                        modified = true;
                        found = true;
                        break;
                    }
                }
        }
    } else {
        for (size_t i = 0; i < n; ++i)
            supports.px[i] = p.type == RedrawType::Supports ? sat(int(full.px[i]) - int(body.px[i])) : uint8_t(full.px[i] & body.px[i]);
        std::vector<uint8_t> pattern(n, 0);
        for (size_t i = 0; i < n; ++i) if (supports.px[i]) pattern[i] = p.brightness;
        for (size_t i = 0; i < n; ++i) {
            const int f = full.px[i], q = pattern[i];
            switch (p.op) {
            case RedrawOperator::Set:      if (f) full.px[i] = uint8_t(q); break;                       // copyTo(full, mask = full)
            case RedrawOperator::Add:      if (supports.px[i]) full.px[i] = sat(f + q); break;
            case RedrawOperator::Subtract: if (supports.px[i]) full.px[i] = sat(f - q); break;
            case RedrawOperator::Multiply: full.px[i] = uint8_t((unsigned(f) * unsigned(q) + 127) / 255); break;
            case RedrawOperator::Divide:   full.px[i] = q == 0 ? 0 : sat(int(std::nearbyint(double(f) / double(q)))); break;
            case RedrawOperator::Minimum:  full.px[i] = uint8_t(std::min(f, q)); break;
            case RedrawOperator::Maximum:  full.px[i] = uint8_t(std::max(f, q)); break;
            case RedrawOperator::AbsDiff:  full.px[i] = uint8_t(std::abs(f - q)); break;
            }
        }
        modified = true;
    }
    if (modified) paste_rect(full_layer, p.roi, full);
    return modified;
}

std::string validate_redraw_model_files(const RedrawModelParams &p, size_t full_count, size_t width, size_t height, size_t body_count, size_t body_width, size_t body_height)
{
    std::string sb = validate_redraw_model(p);
    if (body_count == 0) sb += "The model-only file has no layers.\\n";
    else if (body_count > full_count)
        sb += "The model-only file has more layers (" + std::to_string(body_count) + ") than the document (" + std::to_string(full_count) + ").\\n";
    if (body_width != width || body_height != height)
        sb += "The model-only file is " + std::to_string(body_width) + "x" + std::to_string(body_height) + " but the document is " + std::to_string(width) + "x" + std::to_string(height) + ".\\n";
    if (!p.roi.empty() && (p.roi.x < 0 || p.roi.y < 0 || size_t(p.roi.right()) > width || size_t(p.roi.bottom()) > height)) sb += "The region of interest is outside of the layer.\\n";
    return sb;
}

StreamKernel redraw_model_kernel(const RedrawModelParams &p, const StreamLayout &l, size_t body_count, const RedrawBodyFetch &body_at)
{
    if (l.layer_count == 0) throw std::invalid_argument("There are no layers.");
    if (const std::string err = validate_redraw_model_files(p, l.layer_count, l.width, l.height, body_count, l.width, l.height); !err.empty()) throw std::invalid_argument(err);
    const size_t start = l.layer_count - body_count;
    StreamKernel k;
    k.first = start; k.last = l.layer_count - 1;
    k.run = [p, start, body_count, body_at](size_t layer, const Mask &in, const LayerFetch &) {
        Mask out = in;
        if (in.nonzero_count() == 0) return out; // an empty layer (raft gap, ...) has nothing to redraw
        const size_t j = layer - start;
        const std::shared_ptr<const Mask> body = body_at(j);
        std::shared_ptr<const Mask> next;
        if (j + 1 < body_count) next = body_at(j + 1);
        redraw_model_layer(out, *body, next.get(), p);
        return out;
    };
    return k;
}

void redraw_model_layers(std::vector<Mask> &full, const std::vector<Mask> &body, const RedrawModelParams &p, const LayerProgress &progress)
{
    if (full.empty()) throw std::invalid_argument("There are no layers.");
    const StreamLayout layout = batch_layout(full);
    const auto kernel = redraw_model_kernel(p, layout, body.size(), [&body](size_t j) { return std::shared_ptr<const Mask>(&body[j], [](const Mask *) {}); });
    run_kernel(full, kernel, progress);
}

} // namespace Slic3r::sla::layerops
