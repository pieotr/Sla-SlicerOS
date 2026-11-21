// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "libslic3r/ExPolygon.hpp"

namespace Slic3r {

// Prepare the exact raw safety-offset paths used by diff_ex(..., Yes).
// Preserve path order, multiplicity and winding without unioning the result.
// Pass these paths to diff_ex(..., No): applying the safety offset again
// would change the support geometry. Inputs are borrowed only for this call;
// the returned paths own their coordinates and may be reused while unchanged.
Polygons prepare_support_safety_offset(const ExPolygons &shapes);

} // namespace Slic3r
