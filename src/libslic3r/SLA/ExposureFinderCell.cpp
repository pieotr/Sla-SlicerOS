// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ExposureFinderCell.hpp"
#include "RasterRounding.hpp"
#include <algorithm>
#include <cmath>
#include <deque>
#include <memory>
#include <boost/multiprecision/cpp_dec_float.hpp>
#include <charconv>

namespace Slic3r::sla {
namespace {
using Decimal = boost::multiprecision::cpp_dec_float_50;
Decimal decimal(double value) {
    char buffer[64];
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
    if (result.ec != std::errc{}) throw RuntimeError("Cannot encode ExposureFinder cell height");
    return Decimal(std::string(buffer, result.ptr));
}
int64_t height_ticks(double value) {
    const Decimal scaled = decimal(value) * 100;
    Decimal integer = floor(scaled);
    const Decimal fraction = scaled - integer;
    if (fraction > Decimal("0.5") || (fraction == Decimal("0.5") && fmod(integer, Decimal(2)) != 0)) ++integer;
    return integer.convert_to<int64_t>() * 10;
}
void validate_plane(const png::ImageGreyscale &image) {
    if (image.buf.empty() || !image.cols || !image.rows || image.cols > 32768 || image.rows > 32768 ||
        uint64_t(image.cols) * image.rows > (uint64_t(128) << 20) ||
        image.buf.size() != image.cols * image.rows)
        throw RuntimeError("Invalid ExposureFinder pattern plane");
}
// Repeated 3x3 erosion with Reflect101 has the same extrema as a centered
// square window clipped at the image border. Reflection only repeats samples
// already inside that window. This also covers one-pixel axes and radii larger
// than an axis. A monotonic queue avoids work proportional to kernel area.
void erode(png::ImageGreyscale &image, unsigned radius, const std::function<void()> &check) {
    if (!radius) return;
    std::vector<uint8_t> temporary(image.buf.size());
    const auto pass = [&](const auto &source, auto &target, bool vertical) {
        const size_t length = vertical ? image.rows : image.cols;
        const size_t lines = vertical ? image.cols : image.rows;
        const auto offset = [&](size_t line, size_t position) {
            return vertical ? position * image.cols + line : line * image.cols + position;
        };
        for (size_t line = 0; line < lines; ++line) {
            check();
            std::deque<size_t> queue;
            size_t next = 0;
            for (size_t position = 0; position < length; ++position) {
                if (!(position % 256)) check();
                const size_t end = std::min(length - 1, position + radius);
                while (next <= end) {
                    const auto value = source[offset(line, next)];
                    while (!queue.empty() && source[offset(line, queue.back())] >= value) queue.pop_back();
                    queue.push_back(next++);
                }
                const size_t first = position > radius ? position - radius : 0;
                while (queue.front() < first) queue.pop_front();
                target[offset(line, position)] = source[offset(line, queue.front())];
            }
        }
    };
    pass(image.buf, temporary, false);
    pass(temporary, image.buf, true);
}
struct CellSource {
    png::ImageGreyscale base, features;
    ExposureFinderCellParameters parameters;
    ExposureFinderStaircasePainter painter;
};
} // namespace
ExposureFinderCellRenderer make_exposure_finder_cell_renderer(
    const png::ImageGreyscale &base, const png::ImageGreyscale &features,
    const ExposureFinderCellParameters &parameters, const ExposureFinderStaircasePainter &painter) {
    validate_plane(base); validate_plane(features);
    if (base.cols != features.cols || base.rows != features.rows)
        throw RuntimeError("ExposureFinder pattern planes must have equal dimensions");
    if (!std::isfinite(parameters.base_height) || parameters.base_height < 0 || parameters.base_height > 100000 ||
        !std::isfinite(parameters.features_height) || parameters.features_height < 0 || parameters.features_height > 100000 ||
        parameters.base_height + parameters.features_height <= 0 || parameters.bottom_erosion > 255 ||
        parameters.chamfer_layers > 255 || parameters.staircase_width > base.cols ||
        parameters.staircase_thickness > base.rows ||
        bool(parameters.staircase_width) != bool(parameters.staircase_thickness))
        throw RuntimeError("Invalid ExposureFinder cell finishing settings");
    // A renderer must remain independent of mutable generator/preview inputs.
    if (height_ticks(parameters.base_height) + height_ticks(parameters.features_height) <= 0)
        throw RuntimeError("ExposureFinder cell has no height on the schedule grid");
    const auto source = std::make_shared<const CellSource>(CellSource{base, features, parameters, painter});
    return [source](const ExposureFinderPart &part, const ExposureFinderExposure &record,
                    const std::function<void()> &checkpoint) {
        const auto check = [&] { if (checkpoint) checkpoint(); };
        check(); ScopedNearestRounding rounding;
        const auto &p = source->parameters;
        if (!std::isfinite(part.layer_height) || part.layer_height <= 0 ||
            !std::isfinite(record.position_z) || record.position_z <= 0 ||
            record.layer_height != part.layer_height)
            throw RuntimeError("Invalid ExposureFinder cell exposure height");
        auto image = record.base ? source->base : source->features;
        check();
        // The schedule owns an integer micrometre grid. Integer division and
        // ceil prevent a multiprecision division residual changing a pixel.
        const auto ticks = [](double value) {
            const Decimal scaled = decimal(value) * 1000;
            const Decimal integer = round(scaled);
            // Only absorb internal decimal arithmetic residuals. A distinct
            // finite double near this grid differs by much more than 1e-40.
            if (integer <= 0 || integer > 1000000000 || abs(scaled - integer) > Decimal("1e-40"))
                throw RuntimeError("ExposureFinder cell exposure is off the micrometre grid");
            return integer.convert_to<int64_t>();
        };
        const int64_t tested_height = ticks(part.layer_height), position = ticks(record.position_z);
        const int64_t base_height = height_ticks(p.base_height), feature_height = height_ticks(p.features_height);
        if (position % tested_height || position > base_height + feature_height ||
            record.base != (position <= base_height))
            throw RuntimeError("ExposureFinder cell exposure differs from its pattern height");
        const int64_t layer = position / tested_height;
        if (!record.base && p.staircase_width && feature_height > tested_height &&
            layer != (base_height + feature_height) / tested_height) {
            const uint64_t denominator = uint64_t(feature_height - tested_height);
            const uint64_t numerator = p.staircase_width * uint64_t(tested_height);
            const uint64_t increment = (numerator + denominator - 1) / denominator;
            const int64_t step = layer - base_height / tested_height - 1;
            if (step < 0) throw RuntimeError("ExposureFinder feature exposure precedes its base");
            if (uint64_t(step) <= p.staircase_width / increment) {
                const size_t width = p.staircase_width - size_t(step) * increment;
                if (width) {
                    const size_t first = p.staircase_width - width;
                    if(source->painter)source->painter(image,first,width,p.staircase_thickness,check);
                    else for (size_t y = 0; y < p.staircase_thickness; ++y) {
                        check();
                        std::fill(image.buf.begin() + y * image.cols + first,
                                  image.buf.begin() + y * image.cols + p.staircase_width, uint8_t(255));
                    }
                }
            }
        }
        // These two reference passes are sequential, before text/brightness.
        if (record.bottom) erode(image, p.bottom_erosion, check);
        if (layer < p.chamfer_layers) erode(image, unsigned(p.chamfer_layers - layer), check);
        check(); return image;
    };
}
} // namespace Slic3r::sla
