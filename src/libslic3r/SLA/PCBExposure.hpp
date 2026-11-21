#ifndef SLIC3R_SLA_PCB_EXPOSURE_HPP
#define SLIC3R_SLA_PCB_EXPOSURE_HPP
#include "LayerDocument.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/PrintConfig.hpp"
#include <functional>
#include <memory>

namespace Slic3r { class SLAPrint; }
#include <string>
#include <vector>

namespace Slic3r::sla {
// Geometry stays in physical board coordinates (mm) until the shared printer
// rasterizer applies LCD orientation and mirroring exactly once.
struct PCBArtwork {
  ExPolygons copper;
};
struct PCBPlacement {
  bool center = true;
  bool mirror_x = false, mirror_y = false;
  double offset_x = 0., offset_y = 0.;
  double margin = 1.;
  // 1 = physical 1:1 (one Gerber mm is one LCD mm, whatever the pixel pitch).
  // Other values compensate resist or optics error; applied about the board centre.
  double scale = 1.;
  // false thresholds the raster (hard edges, no gray pixels) like UVtools'
  // "enable anti-aliasing" option; true keeps AGG coverage-based gray edges.
  bool antialiasing = true;
};
// RS-274X standard apertures and macros, linear/circular (G74 and G75)
// interpolation, regions, LM/LR/LS aperture transformations, SR step-and-repeat
// and AB block apertures. Relative coordinates (G91 / FS..I) and any other
// unsupported command fail explicitly rather than exporting an altered board.
PCBArtwork parse_gerber(const std::string &text,
                        double curve_tolerance_mm = .005,
                        const std::function<void()> &check_cancel = {});
// Excellon drills, G85 slots and routed paths (G00, M15/M16, G01-G03).
void subtract_excellon(PCBArtwork &board, const std::string &text,
                       double curve_tolerance_mm = .005,
                       const std::function<void()> &check_cancel = {});
LayerDocument rasterize_pcb(const PCBArtwork &board,
                            const SLAPrinterConfig &printer,
                            const PCBPlacement &placement = {},
                            const std::function<void()> &check_cancel = {});
// All input boards share one placement computed from their combined bounds.
// Separate masks retain registration; merging unions copper before rasterizing.
LayerDocument rasterize_pcb_layers(const std::vector<PCBArtwork> &boards,
                                   const SLAPrinterConfig &printer, bool merge,
                                   const PCBPlacement &placement = {},
                                   const std::function<void()> &check_cancel = {});
// Downscaled composition of what rasterize_pcb_layers would produce (same
// placement, mirroring and polarity), for a dialog bitmap. pixels is 8-bit
// grayscale, width_px * height_px, in raster orientation. For unmerged jobs
// layer_index selects the file; layer_count tells how many exist.
struct PCBPreview {
  size_t width_px = 0, height_px = 0;
  std::vector<uint8_t> pixels;
  size_t layer_count = 1, shown_layer = 0;
};
PCBPreview render_pcb_preview(const std::vector<PCBArtwork> &boards,
                              const SLAPrinterConfig &printer, bool merge,
                              const PCBPlacement &placement, bool invert,
                              size_t max_dimension_px, size_t layer_index = 0,
                              const std::function<void()> &check_cancel = {});
// Exposure recipe presets for common photosensitive materials. Only suggestions
// for the exposure field and polarity; nothing is written to resin presets.
struct PCBExposureRecipe {
  std::string name;
  double exposure_seconds = 30.;
  bool invert = false;
  std::string guidance;
};
const std::vector<PCBExposureRecipe> &pcb_exposure_presets();
// Build a stationary exposure job with local overrides, shared registration,
// and explicit seconds. No resin/printer preset is changed by PCB generation.
std::unique_ptr<SLAPrint> create_pcb_exposure_job(
    const DynamicPrintConfig &configuration, const std::vector<PCBArtwork> &boards,
    bool merge, const PCBPlacement &placement, double exposure_seconds,
    bool invert_mask = false, const std::function<void()> &check_cancel = {});
} // namespace Slic3r::sla
#endif
