// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "LayerParameters.hpp"
#include <cstddef>

namespace Slic3r {
class DynamicPrintConfig;
namespace sla {

// Format-neutral intentions: FIRST/SECOND distances in mm, speeds in mm/min,
// delays in seconds. Defaults remain distinct from each layer's overrides.
// Codec precision, count widths and shared-wait restrictions belong to export.
struct LayerJobParameters {
    double layer_height = 0;
    size_t bottom_layers = 0, transition_layers = 0;
    LayerParameters normal, bottom;

    void validate(size_t layer_count) const;
    void apply_to(DynamicPrintConfig &config) const;
};

} // namespace sla
} // namespace Slic3r
