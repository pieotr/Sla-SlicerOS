// Shared scene-to-raster contract for grayscale SLA archive writers.
#ifndef SLIC3R_SLA_PRINTER_RASTER_HPP
#define SLIC3R_SLA_PRINTER_RASTER_HPP
#include <cmath>
#include "RasterBase.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Exception.hpp"

namespace Slic3r::sla {
inline std::unique_ptr<RasterBase> create_printer_raster(const SLAPrinterConfig &cfg)
{
    const double width = cfg.display_width.value, height = cfg.display_height.value;
    const int cols = cfg.display_pixels_x.value, rows = cfg.display_pixels_y.value;
    if (!std::isfinite(width) || !std::isfinite(height) || width <= 0. || height <= 0. ||
        cols <= 0 || rows <= 0 || cols > 32768 || rows > 32768 ||
        uint64_t(cols) * uint64_t(rows) > 128ULL * 1024 * 1024)
        throw RuntimeError("Invalid SLA display dimensions or resolution");
    const auto orientation = cfg.display_orientation.value == sladoPortrait ?
        RasterBase::roPortrait : RasterBase::roLandscape;
    // AGGRaster scales scene coordinates before rotating them. Pitches must
    // retain scene-axis order even when output rows and columns are swapped.
    const PixelDim pitch{width / cols, height / rows};
    Resolution resolution{size_t(cols), size_t(rows)};
    if (orientation == RasterBase::roPortrait)
        std::swap(resolution.width_px, resolution.height_px);
    const RasterBase::Trafo transform{orientation,
        {cfg.display_mirror_x.value, cfg.display_mirror_y.value}};
    return create_raster_grayscale_aa(resolution, pitch, cfg.gamma_correction.value, transform);
}
}
#endif
