// Full organic tree supports for SLA (support_tree_type = organic).
#ifndef ORGANICGROWTHSLA_HPP
#define ORGANICGROWTHSLA_HPP

#include <cstddef>
#include <vector>

#include "libslic3r/Point.hpp"
#include "libslic3r/SLA/SupportTreeBuilder.hpp"

namespace Slic3r { namespace sla {

struct SupportableMesh;

struct OrganicGrowthStats
{
    size_t heads       = 0; // valid pinheads after the routing
    size_t support_points = 0; // non duplicate support points (heads before routing)
    size_t dropped     = 0; // heads dropped because their branch could not be routed
    size_t branches    = 0; // emitted tubes (chains between tips / merges / roots)
    size_t merges      = 0; // nodes where two branches joined
    size_t roots       = 0; // trees ending on the ground / pad
    size_t anchors     = 0; // trees ending on the model body
    size_t fallback_ground = 0; // dropped tips rescued by a branching style ground route
    size_t fallback_anchor = 0; // dropped tips rescued by a model anchor
    size_t unroutable  = 0; // heads that could not be routed at all (also dropped by branching)
    double max_anchor_weight = 0.; // largest weight of a growth branch resting on the model
    std::vector<Vec3d>  anchor_pts; // junction points of the anchors of the growth branches
    std::vector<double> anchor_r;
    std::vector<char>   anchor_shortened; // 1: shortened head (last resort), keeps the safety gap
    size_t layers      = 0; // number of growth layers
    size_t nodes       = 0; // growth nodes created
    size_t avoid_moves = 0; // moves forced by the model avoidance areas
    double step        = 0.; // layer step [mm]
    double ms_avoidance = 0., ms_growth = 0., ms_smooth = 0., ms_mesh = 0.;
};

// One branch (chain of nodes between a tip/merge and the next merge/root),
// the final, smoothed polyline with the branch radius for each point.
struct OrganicBranchPath
{
    std::vector<Vec3d>  pts;
    std::vector<double> radii;
};

// Organic tree growth in the spirit of the Cura / PrusaSlicer FFF organic
// supports (TreeSupport.cpp, TreeModelVolumes.cpp, OrganicSupport.cpp), done on
// the SLA support points instead of overhang polygons:
//  * pinheads are placed exactly as for the other tree types;
//  * the tips (heads) descend layer by layer through precomputed per layer
//    collision/avoidance areas of the model (OrganicAvoidance), sliding away from
//    the model with the maximal bridge slope and merging when branches meet;
//  * branches end on the bed (pillar + base) or, when allowed, on the model;
//  * the paths are smoothed with collision checks and emitted as tapered tubes
//    and merge spheres (SupportTreeBuilder::add_support_mesh).
// Does not depend on any FFF type.
void create_organic_growth_tree(SupportTreeBuilder            &builder,
                                const SupportableMesh         &sm,
                                OrganicGrowthStats            *stats = nullptr,
                                std::vector<OrganicBranchPath> *paths = nullptr);

}} // namespace Slic3r::sla

#endif // ORGANICGROWTHSLA_HPP
