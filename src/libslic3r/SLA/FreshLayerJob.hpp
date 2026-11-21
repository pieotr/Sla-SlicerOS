// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "LayerJobParameters.hpp"
#include "LayerProgress.hpp"
#include "LayerRasterGeometry.hpp"
#include "LayerMaskPrecision.hpp"
#include "ExposurePositionSchedule.hpp"
#include <vector>

namespace Slic3r {
class SLAPrinterConfig;
class SLAMaterialConfig;
}

namespace Slic3r::sla {
// Explicit producing context, never inferred from an empty archive format.
// Raster is an intentionally generated job, not an imported standalone PNG.
enum class FreshLayerProducer { Raster, ModelSlice, Calibration, PCB };
// Producer identifies what made the masks; parameter origin determines who
// may refresh the schedule. Old descriptors remain explicit by default.
enum class FreshLayerParameterOrigin { Explicit, AutomaticModel };
struct FreshLayerJob {
    FreshLayerProducer producer = FreshLayerProducer::Raster;
    FreshLayerParameterOrigin parameter_origin = FreshLayerParameterOrigin::Explicit;
    LayerMaskPrecision mask_precision = LayerMaskPrecision::Gray8;
    ExposurePositionPolicy position_policy = ExposurePositionPolicy::IncreasingLayers;
    LayerJobParameters globals;
    std::vector<LayerParameters> rows;
    LayerRasterGeometry geometry;
    void validate(size_t layer_count, const LayerProgress &progress = {}) const;
};
// Shared by slicing and project persistence. Metadata refresh must not require
// rasterization, worker access, PNG decoding or quantization of saved Z values.
FreshLayerJob make_fresh_layer_job(const SLAPrinterConfig &printer,
    const SLAMaterialConfig &material, double layer_height,
    const std::vector<double> &positions_z, FreshLayerProducer producer,
    ExposurePositionPolicy position_policy = ExposurePositionPolicy::IncreasingLayers);
} // namespace Slic3r::sla
