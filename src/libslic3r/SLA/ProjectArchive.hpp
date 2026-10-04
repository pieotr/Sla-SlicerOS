#ifndef SLIC3R_SLA_PROJECT_ARCHIVE_HPP
#define SLIC3R_SLA_PROJECT_ARCHIVE_HPP
#include "LayerDocument.hpp"
#include <miniz.h>
#include <optional>
#include <string>
#include <vector>

namespace Slic3r {
class Model;
class ConfigBase;
class SLAPrint;
namespace sla {
// Optional project payload. Source geometry/configuration identifies the
// slice these masks belong to; edited PNGs cannot be inferred from meshes.
struct ProjectRasterState {
  LayerDocument document;
  std::vector<double> positions_z;
  std::string source_fingerprint;
};
std::string raster_source_fingerprint(const Model &model,
                                      const ConfigBase &config);
ProjectRasterState capture_project_layers(const Model &model,
                                          const ConfigBase &config,
                                          const SLAPrint &print);
void write_project_layers(mz_zip_archive &archive,
                          const ProjectRasterState &state);
std::optional<ProjectRasterState> read_project_layers(const std::string &path);
// Verifies source, geometry and exact Z positions before changing the print.
// Mismatch is a normal stale-cache result, not a partially applied edit.
bool restore_project_layers(SLAPrint &print, const Model &model,
                            const ConfigBase &config,
                            const ProjectRasterState &state);
} // namespace sla
} // namespace Slic3r
#endif
