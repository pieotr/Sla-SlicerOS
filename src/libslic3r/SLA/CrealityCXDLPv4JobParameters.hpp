// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "LayerParameters.hpp"
#include <array>
#include <cstdint>
#include <optional>

namespace Slic3r::sla {
// Native metadata, not a complete editable/exportable LayerJobParameters.
// Six bottom fields have no global slots; their absence is significant.
struct CXDLPv4NativeJobSnapshot {
    double layer_height = 0;
    uint32_t bottom_layers = 0, transition_layers = 0;
    LayerParameters normal, bottom;
    uint32_t raw_per_layer_mode = 0;
    uint32_t parameter_bottom_layers = 0;
    // Slicer-info duplicates are not the authoritative global getters.
    // Keep their original values for diagnostics and later lossless writes.
    std::array<float, 3> mirrors{}; // normal exposure, bottom exposure, after-lift wait
};

// Reference-derived diagnostics, distinct from native author intent. Six
// bottom defaults have no native global slots and originate in a mask row.
struct CXDLPv4ReferenceBottomView {
    CXDLPv4NativeJobSnapshot native;
    uint16_t decoded_bottom_layers = 0;
    LayerParameters virtual_bottom;
    std::optional<size_t> meaningful_bottom_layer;
};
} // namespace Slic3r::sla
