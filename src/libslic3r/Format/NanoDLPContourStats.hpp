// SPDX-License-Identifier: AGPL-3.0-or-later
// NanoDLP uses the shared native pixel-contour geometry, without allocating
// path vectors when only its area/bbox statistics are needed.
#ifndef SLIC3R_FORMAT_NANODLP_CONTOUR_STATS_HPP
#define SLIC3R_FORMAT_NANODLP_CONTOUR_STATS_HPP
#include "libslic3r/SLA/PixelContourTree.hpp"
namespace Slic3r::nanodlp {
using ContourStats = sla::PixelContourStats;
inline ContourStats contour_statistics(const uint8_t *pixels,size_t width,size_t height) {
    return sla::trace_pixel_contours(pixels,width,height);
}
}
#endif
