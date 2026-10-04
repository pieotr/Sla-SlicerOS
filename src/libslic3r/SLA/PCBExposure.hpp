#ifndef SLIC3R_SLA_PCB_EXPOSURE_HPP
#define SLIC3R_SLA_PCB_EXPOSURE_HPP
#include "LayerDocument.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/PrintConfig.hpp"
#include <functional>
#include <string>

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
};
// RS-274X standard apertures, linear/circular interpolation and regions.
// Unsupported commands fail explicitly rather than exporting an altered board.
PCBArtwork parse_gerber(const std::string &text,
                        double curve_tolerance_mm = .005,
                        const std::function<void()> &check_cancel = {});
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
} // namespace Slic3r::sla
#endif
