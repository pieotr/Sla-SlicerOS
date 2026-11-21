// SPDX-License-Identifier: AGPL-3.0-or-later
#include "LayerBlur.hpp"

#include <algorithm>
#include <cstring>
#include <opencv2/imgproc.hpp>
#include <sstream>
#include <stdexcept>

namespace Slic3r::sla::layerops {
namespace {

cv::Mat view(Mask &m) { return cv::Mat(int(m.height), int(m.width), CV_8UC1, m.px.data()); }
cv::Mat view(const Mask &m) { return cv::Mat(int(m.height), int(m.width), CV_8UC1, const_cast<uint8_t *>(m.px.data())); }

// UVtools KernelConfiguration.GenerateKernelFromText.
struct Kernel { int rows = 0, cols = 0; std::vector<uint8_t> v; };

bool parse_kernel(const BlurParams &p, Kernel &k, std::string &error)
{
    if (p.kernel_text.empty()) { error = "Invalid kernel: Kernel can't be empty.\n"; return false; }
    std::vector<std::string> lines;
    {
        std::string cur;
        for (char c : p.kernel_text) { if (c == '\n') { lines.push_back(cur); cur.clear(); } else cur += c; }
        lines.push_back(cur);
    }
    for (size_t row = 0; row < lines.size(); ++row) {
        std::string line = lines[row];
        const auto b = line.find_first_not_of(" \t\r");
        const auto e = line.find_last_not_of(" \t\r");
        line = b == std::string::npos ? std::string() : line.substr(b, e - b + 1);
        std::vector<std::string> cells;
        std::string cur;
        for (char c : line) { if (c == ' ') { cells.push_back(cur); cur.clear(); } else cur += c; }
        cells.push_back(cur);
        if (row == 0) { k.cols = int(cells.size()); k.rows = int(lines.size()); k.v.assign(size_t(k.rows) * size_t(k.cols), 0); }
        else if (int(cells.size()) != k.cols) {
            error = "Invalid kernel: Row " + std::to_string(row + 1) + " have invalid number of columns, the matrix must have equal columns count per line, per defined on line 1\n";
            return false;
        }
        for (size_t col = 0; col < cells.size(); ++col) {
            const std::string &c = cells[col];
            bool ok = !c.empty() && c.size() <= 3 && std::all_of(c.begin(), c.end(), [](char ch) { return ch >= '0' && ch <= '9'; });
            const int val = ok ? std::stoi(c) : 0;
            if (!ok || val > 255) { error = "Invalid kernel: " + c + " is a invalid number, use values from 0 to 255\n"; return false; }
            k.v[row * size_t(k.cols) + col] = uint8_t(val);
        }
    }
    if (k.cols <= p.anchor_x || k.rows <= p.anchor_y) {
        error = "Invalid kernel: Anchor position X/Y can't be higher or equal than kernel columns/rows\nPlease fix the values.\n";
        return false;
    }
    return true;
}

} // namespace

std::string validate_blur(const BlurParams &p)
{
    std::string sb;
    if (p.algorithm == BlurAlgorithm::StackBlur || p.algorithm == BlurAlgorithm::GaussianBlur || p.algorithm == BlurAlgorithm::MedianBlur)
        if (p.size % 2 != 1) sb += "Size must be a odd number.\n";
    const bool sized = p.algorithm != BlurAlgorithm::Pyramid && p.algorithm != BlurAlgorithm::Filter2D;
    if (sized && (p.size == 0 || p.size > 4095)) sb += "Size must be between 1 and 4095.\n";
    if (p.algorithm == BlurAlgorithm::BoxBlur && sized && p.size > 0) {
        if (p.anchor_x >= int(p.size) || p.anchor_y >= int(p.size) || p.anchor_x < -1 || p.anchor_y < -1)
            sb += "Invalid kernel: Anchor position X/Y can't be higher or equal than kernel columns/rows\nPlease fix the values.\n";
    }
    if (p.algorithm == BlurAlgorithm::Filter2D) {
        Kernel k; std::string err;
        if (!parse_kernel(p, k, err)) sb += err;
        else if (p.anchor_x < -1 || p.anchor_y < -1) sb += "Invalid kernel: Anchor position X/Y can't be negative (except -1 = centre).\n";
    }
    return sb;
}

Mask blur_mask(const Mask &in, const BlurParams &p)
{
    if (const std::string err = validate_blur(p); !err.empty()) throw std::invalid_argument(err);
    Mask out(in.width, in.height);
    if (in.px.empty()) return out;
    const cv::Mat src = view(in);
    cv::Mat dst = view(out);
    const int k = int(p.size);
    switch (p.algorithm) {
    case BlurAlgorithm::StackBlur:
        if (k == 1) { out = in; break; }
        cv::stackBlur(src, dst, cv::Size(k, k));
        break;
    case BlurAlgorithm::BoxBlur:
        cv::blur(src, dst, cv::Size(k, k), cv::Point(p.anchor_x, p.anchor_y), cv::BORDER_DEFAULT);
        break;
    case BlurAlgorithm::Pyramid: {
        cv::Mat down, up;
        cv::pyrDown(src, down);
        cv::pyrUp(down, up, src.size());
        up.copyTo(dst);
        break;
    }
    case BlurAlgorithm::MedianBlur:
        if (k == 1) { out = in; break; }
        cv::medianBlur(src, dst, k);
        break;
    case BlurAlgorithm::GaussianBlur:
        cv::GaussianBlur(src, dst, cv::Size(k, k), 0);
        break;
    case BlurAlgorithm::Filter2D: {
        Kernel kern; std::string err;
        parse_kernel(p, kern, err);
        cv::Mat km(kern.rows, kern.cols, CV_8UC1, kern.v.data());
        cv::filter2D(src, dst, -1, km, cv::Point(p.anchor_x, p.anchor_y), 0, cv::BORDER_DEFAULT);
        break;
    }
    }
    return out;
}

StreamKernel blur_kernel(const BlurParams &p, const StreamLayout &l)
{
    if (const std::string err = validate_blur(p); !err.empty()) throw std::invalid_argument(err);
    if (l.layer_count == 0) throw std::invalid_argument("There are no layers.");
    const Rect roi = p.selection.roi;
    if (!roi.empty() && (roi.x < 0 || roi.y < 0 || size_t(roi.right()) > l.width || size_t(roi.bottom()) > l.height))
        throw std::invalid_argument("The region of interest is outside of the layer.");
    const size_t last = std::min(p.selection.last_layer, l.layer_count - 1);
    if (p.selection.first_layer > last) throw std::invalid_argument("The layer range is empty or outside of the layers.");
    StreamKernel k;
    k.first = p.selection.first_layer; k.last = last;
    k.run = [p](size_t, const Mask &in, const LayerFetch &) {
        // OpenCV filters a ROI sub-matrix with the parent's pixels as border: blur the whole layer, keep only the ROI.
        Mask out = blur_mask(in, p);
        if (!p.selection.roi.empty()) {
            const Rect r = p.selection.roi;
            Mask merged = in;
            for (int y = 0; y < r.h; ++y)
                std::memcpy(&merged.px[size_t(r.y + y) * merged.width + size_t(r.x)], &out.px[size_t(r.y + y) * out.width + size_t(r.x)], size_t(r.w));
            return merged;
        }
        return out;
    };
    return k;
}

void blur_layers(std::vector<Mask> &layers, const BlurParams &p, const LayerProgress &progress)
{
    run_kernel(layers, blur_kernel(p, batch_layout(layers)), progress);
}

} // namespace Slic3r::sla::layerops
