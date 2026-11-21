// SPDX-License-Identifier: AGPL-3.0-or-later
#include "PixelDrawing.hpp"

#include <opencv2/imgproc.hpp>

namespace Slic3r::sla::pixel_drawing {

void filled_circle(uint8_t *data, int rows, int cols, int cx, int cy, int radius, uint8_t value, bool anti_alias)
{
    if (rows <= 0 || cols <= 0 || radius < 0) return;
    cv::Mat image(rows, cols, CV_8UC1, data);
    cv::circle(image, cv::Point(cx, cy), radius, cv::Scalar(value), -1, anti_alias ? cv::LINE_AA : cv::LINE_8);
}

} // namespace Slic3r::sla::pixel_drawing
