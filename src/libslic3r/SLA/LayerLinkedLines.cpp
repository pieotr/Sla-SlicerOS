// SPDX-License-Identifier: AGPL-3.0-or-later
#include "LayerLinkedLines.hpp"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <opencv2/imgproc.hpp>

namespace Slic3r::sla::layerops {
namespace {
cv::Mat view(Mask &m) { return cv::Mat(int(m.height), int(m.width), CV_8UC1, m.px.data()); }

// cv::moments of a contour (polygon moments, Green's theorem; the same arithmetic as OpenCV's contourMoments). OpenCV 5
// moved cv::moments into libopencv_geometry, which we do not link, so M00 / M10 / M01 are computed here.
struct PolygonMoments { double m00 = 0, m10 = 0, m01 = 0; };
PolygonMoments polygon_moments(const std::vector<cv::Point> &pts)
{
    PolygonMoments m;
    if (pts.size() < 2) return m;
    double a00 = 0, a10 = 0, a01 = 0;
    double xi_1 = pts.back().x, yi_1 = pts.back().y;
    for (const cv::Point &p : pts) {
        const double xi = p.x, yi = p.y;
        const double dxy = xi_1 * yi - xi * yi_1, xii_1 = xi_1 + xi, yii_1 = yi_1 + yi;
        a00 += dxy;
        a10 += dxy * xii_1;
        a01 += dxy * yii_1;
        xi_1 = xi; yi_1 = yi;
    }
    if (std::abs(a00) > FLT_EPSILON) {
        const double db1_2 = 0.5, db1_6 = 0.16666666666666666666666666666667;
        if (a00 < 0) { a00 = -a00; a10 = -a10; a01 = -a01; }
        m.m00 = a00 * db1_2; m.m10 = a10 * db1_6; m.m01 = a01 * db1_6;
    }
    return m;
}
} // namespace

Mask raft_linked_lines(const Mask &original, const Mask &dilated, const LinkedLinesParams &p)
{
    Mask supports = dilated; // supportsMat
    if (original.px.empty()) return supports;
    const int thickness = std::max<int>(4, p.thickness);
    const cv::Scalar link_color(p.link_color);

    // contours = FindContours(supportsMatOriginal, RetrType.List); centroid = round(M10/M00), round(M01/M00); (-1,-1) when M00 == 0.
    Mask original_copy = original;
    std::vector<std::vector<cv::Point>> contours;
    cv::findContours(view(original_copy), contours, cv::RETR_LIST, cv::CHAIN_APPROX_SIMPLE);
    const size_t n = contours.size();
    std::vector<cv::Point> centroid(n);
    for (size_t i = 0; i < n; ++i) {
        const PolygonMoments m = polygon_moments(contours[i]);
        centroid[i] = m.m00 == 0 ? cv::Point(-1, -1) : cv::Point(int(std::lround(m.m10 / m.m00)), int(std::lround(m.m01 / m.m00)));
    }
    auto valid = [&](size_t i) { return centroid[i].x >= 0 && centroid[i].y >= 0; }; // !IsAnyNegative

    // Other contours ordered by the distance between centroids (stable, like LINQ OrderBy); the (-1,-1) ones take part too.
    std::vector<std::vector<size_t>> nearest(n);
    for (size_t i = 0; i < n; ++i) {
        std::vector<std::pair<size_t, double>> d;
        for (size_t x = 0; x < n; ++x) {
            if (x == i) continue;
            const double dx = double(centroid[i].x - centroid[x].x), dy = double(centroid[i].y - centroid[x].y);
            d.emplace_back(x, std::sqrt(dx * dx + dy * dy));
        }
        std::stable_sort(d.begin(), d.end(), [](const auto &a, const auto &b) { return a.second < b.second; });
        for (const auto &e : d) nearest[i].push_back(e.first);
    }
    const int links = std::min<int>(p.minimum_links, int(n) - 1);

    Mask redraw;
    if (p.external_supports) redraw = original;
    Mask correction;
    if (p.link_color < 255) correction = supports;

    cv::Mat smat = view(supports), rmat;
    if (p.external_supports) rmat = view(redraw);
    for (size_t i = 0; i < n; ++i) {
        if (!valid(i)) continue;
        if (p.external_supports)
            for (size_t x = 0; x < n; ++x) {
                if (x == i || !valid(x)) continue;
                cv::line(rmat, centroid[i], centroid[x], link_color, 4);
            }
        for (int link = 0; link < links; ++link)
            cv::line(smat, centroid[i], centroid[nearest[i][size_t(link)]], link_color, thickness);
    }

    if (p.external_supports) {
        Mask redraw_copy = redraw;
        std::vector<std::vector<cv::Point>> external;
        cv::findContours(view(redraw_copy), external, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);
        cv::drawContours(smat, external, -1, link_color, thickness);
    }
    if (p.link_color < 255) {
        for (size_t k = 0; k < supports.px.size(); ++k)
            if (correction.px[k]) supports.px[k] = correction.px[k];
    }
    const cv::Mat kernel = cv::getStructuringElement(cv::MORPH_RECT, cv::Size(3, 3));
    cv::morphologyEx(smat, smat, cv::MORPH_CLOSE, kernel, cv::Point(-1, -1), 1, cv::BORDER_REFLECT_101);
    return supports;
}

} // namespace Slic3r::sla::layerops
