#ifndef SLIC3R_SLA_PROJECT_ARCHIVE_HPP
#define SLIC3R_SLA_PROJECT_ARCHIVE_HPP
#include "LayerDocument.hpp"
#include "LayerExportProfile.hpp"
#include "ProjectSettings.hpp"
#include "CalibrationRecipe.hpp"
#include <miniz.h>
#include <optional>
#include <memory>
#include <string>
#include <vector>

namespace Slic3r {
class Model;
class ConfigBase;
class SLAPrint;
namespace sla {
// The origin controls which source identity is meaningful. Imported archives
// retain native heights (when available); they must never impersonate a mesh cache.
enum class ProjectLayerOrigin { ModelSlice, ImportedArchive, GeneratedRaster };
// Optional project payload. Source geometry/configuration identifies the
// slice these masks belong to; edited PNGs cannot be inferred from meshes.
struct ProjectRasterState {
  LayerDocument document;
  std::vector<double> positions_z;
  std::string source_fingerprint;
  // Explicit opt-in context for imported data; never inferred from ambient presets.
  std::optional<LayerExportProfile> export_profile;
  // Missing origin in v1/v2 means ModelSlice for backward compatibility.
  ProjectLayerOrigin origin = ProjectLayerOrigin::ModelSlice;
  std::optional<ProjectSLASettings> settings;
  std::shared_ptr<const CalibrationRecipe> calibration_recipe;
};
std::string raster_source_fingerprint(const Model &model,
                                      const ConfigBase &config);
ProjectRasterState capture_project_layers(const Model &model,
                                          const ConfigBase &config,
                                          const SLAPrint &print);
// Generated projects own their configuration and heights. Ambient presets
// are never read or changed; edited masks are captured explicitly.
ProjectRasterState capture_generated_project(const SLAPrint &print,
                                             const LayerDocument &document);
std::unique_ptr<SLAPrint> restore_generated_project(const ProjectRasterState &state);
// Refresh non-geometric settings of a pending model cache before project save.
// A stale source returns false; validation errors leave the snapshot untouched.
bool refresh_model_project_settings(ProjectRasterState &state, const Model &model,
                                    const ConfigBase &config);
void write_project_layers(mz_zip_archive &archive,
                          const ProjectRasterState &state,
                          const LayerProgress &progress = {});
// Standalone layer project: stage in the target directory, then publish atomically.
// Does not fabricate a 3MF model or mark a document saved before publication.
void save_layer_project(const std::string &path, const ProjectRasterState &state,
                        const LayerProgress &progress = {});
std::optional<ProjectRasterState> read_project_layers(const std::string &path,
                                                      const LayerProgress &progress = {});
// Verifies source, geometry and exact Z positions before changing the print.
// Mismatch is a normal stale-cache result, not a partially applied edit.
bool project_layers_match(const SLAPrint &print, const Model &model,
                          const ConfigBase &config, const ProjectRasterState &state);
bool restore_project_layers(SLAPrint &print, const Model &model,
                            const ConfigBase &config,
                            const ProjectRasterState &state);
} // namespace sla
} // namespace Slic3r
#endif
