// SPDX-License-Identifier: AGPL-3.0-or-later
// Raft relief "Linked lines" (UVtools OperationRaftRelief.cs, RaftReliefTypes.LinkedLines): the supports of the
// first support layer are joined by lines so the raft can be replaced by "supports + links". OpenCV is private to the
// translation unit (findContours / moments / line / drawContours), the interface is Mask in / Mask out.
#pragma once

#include "LayerOperations.hpp"

namespace Slic3r::sla::layerops {

struct LinkedLinesParams {
    uint8_t thickness = 26;           // UVtools LinkedLineThickness (>= 4)
    uint8_t minimum_links = 4;        // UVtools LinkedMinimumLinks
    bool    external_supports = true; // UVtools LinkedExternalSupports: also link every support to every other one
    uint8_t link_color = 255;         // UVtools HighBrightness
};

// `original` = the support layer (ROI), `dilated` = the same after DilateIterations (equal to `original` when 0).
// Returns the image the raft layers are replaced by (same size).
Mask raft_linked_lines(const Mask &original, const Mask &dilated, const LinkedLinesParams &);

} // namespace Slic3r::sla::layerops
