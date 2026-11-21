#ifndef ORGANICTREESLA_HPP
#define ORGANICTREESLA_HPP

#include <cstddef>

#include "libslic3r/SLA/BranchingTreeSLA.hpp"
#include "libslic3r/SLA/SupportTreeBuilder.hpp"

namespace Slic3r { namespace sla {

struct SupportableMesh;

// Statistics of the last organic tree generation (optional output, mainly for
// tests and diagnostics).
struct OrganicTreeStats
{
    size_t heads          = 0; // valid pinheads
    size_t branches       = 0; // smooth tubes (one per skeleton edge)
    size_t merges         = 0; // nodes where two branches join
    size_t roots          = 0; // trees ending on the ground / pad
    size_t anchors        = 0; // trees ending on the model body
    size_t smoothed_nodes = 0; // skeleton nodes moved by the relaxation
    size_t curved         = 0; // branches emitted as smooth curves
    size_t straight       = 0; // branches that had to fall back to straight
};

// Organic (smooth, tapering, merging) tree supports.
//
// Heads, pillars, ground connections and model anchors are produced exactly as
// in the branching tree (so the pad, the support point editing and the model
// avoidance rules keep working), but the branches between them are relaxed
// (Laplacian smoothing with collision checks against the model), swept along
// Hermite curves with a continuously tapering radius and emitted as closed
// tube meshes into the builder (SupportTreeBuilder::add_support_mesh).
void create_organic_tree(SupportTreeBuilder    &builder,
                         const SupportableMesh &sm,
                         OrganicTreeStats      *stats = nullptr);

}} // namespace Slic3r::sla

#endif // ORGANICTREESLA_HPP
