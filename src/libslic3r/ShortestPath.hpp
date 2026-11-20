///|/ Copyright (c) Prusa Research 2019 - 2023 Vojtěch Bubník @bubnikv, Lukáš Matěna @lukasmatena
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_ShortestPath_hpp_
#define slic3r_ShortestPath_hpp_

#include <vector>

#include "libslic3r.h"
#include "Point.hpp"

namespace Slic3r {

namespace ClipperLib {
    class PolyNode;

    using PolyNodes = std::vector<PolyNode*, PointsAllocator<PolyNode*>>;
}

// Reorder the Clipper polygon tree nodes by the greedy shortest path through the given
// representative points (one point per node). Used by ClipperUtils to give offset results a stable order.
ClipperLib::PolyNodes chain_clipper_polynodes(const Points &points, const ClipperLib::PolyNodes &items);

} // namespace Slic3r

#endif /* slic3r_ShortestPath_hpp_ */
