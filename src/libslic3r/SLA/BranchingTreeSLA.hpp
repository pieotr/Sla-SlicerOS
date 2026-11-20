///|/ Copyright (c) Prusa Research 2022 Tomáš Mészáros @tamasmeszaros
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef BRANCHINGTREESLA_HPP
#define BRANCHINGTREESLA_HPP

#include <boost/log/trivial.hpp>
#include <map>
#include <vector>

#include "libslic3r/BranchingTree/BranchingTree.hpp"
#include "SupportTreeBuilder.hpp"

namespace Slic3r { namespace sla {
class SupportTreeBuilder;
struct SupportableMesh;

void create_branching_tree(SupportTreeBuilder& builder, const SupportableMesh &sm);

// Logical (geometry free) result of the branching tree routing: a forest of
// binary trees with positions and radii. Heads, pillars, ground connections
// and anchors are still emitted into the SupportTreeBuilder but the branches
// between the nodes are only recorded here, so a different mesher (e.g. the
// organic one) can produce the actual geometry.
struct TreeSkeleton
{
    struct Node
    {
        Vec3d  pos    = Vec3d::Zero();
        double r      = 0.;
        int    parent = -1;       // index of the node below, -1 for roots
        int    child[2] = {-1, -1};
        bool   anchor = false;    // a root which ends on the model body

        int num_children() const
        {
            return (child[0] >= 0) + (child[1] >= 0);
        }
    };

    std::vector<Node>   nodes;
    std::map<int, int>  key_to_idx; // branchingtree node id -> index in nodes

    // Get (or create) the node belonging to the branching tree node `key`
    int node_for(int key, const Vec3d &pos, double r);

    // Create a new, parentless node which is not a branchingtree node
    int add_anchor_node(const Vec3d &pos, double r);

    // Connect `child` (above) to `parent` (below). Returns false if the child
    // is already connected or the parent has no free slot.
    bool connect(int child, int parent);
};

// Run the branching tree routing, but only record the branches into `skel`.
void create_branching_skeleton(SupportTreeBuilder    &builder,
                               const SupportableMesh &sm,
                               TreeSkeleton          &skel);

}} // namespace Slic3r::sla

#endif // BRANCHINGTREESLA_HPP
