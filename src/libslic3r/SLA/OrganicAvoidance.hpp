// Per-layer collision / avoidance areas for the organic growth supports.
//
// Adapted (idea and structure) from PrusaSlicer 2.9 TreeModelVolumes
// (src/libslic3r/Support/TreeModelVolumes.cpp, derived from CuraEngine,
// AGPLv3) but completely free of the FFF print types: the model outline of every
// layer is computed straight from the triangle mesh with slice_mesh_ex().
#ifndef ORGANICAVOIDANCE_HPP
#define ORGANICAVOIDANCE_HPP

#include <cstddef>
#include <functional>
#include <vector>

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/Point.hpp"
#include "admesh/stl.h"

namespace Slic3r { namespace sla {

class OrganicAvoidance
{
public:
    struct Params
    {
        double ground_z  = 0.;  // z of the bed / pad bottom (layer 0)
        double top_z     = 0.;  // highest z which has to be covered
        double step      = 0.3; // layer step [mm]
        double r_min     = 0.5; // smallest branch radius
        double r_max     = 1.8; // largest branch radius
        size_t classes   = 6;   // number of radius classes between r_min and r_max
        double safety    = 0.5; // gap between a branch surface and the model
        double base_radius = 2.; // radius of the pillar base (layers below base_height)
        double base_safety = 0.5;
        double base_height = 1.;
        double max_move  = 0.3; // max horizontal move per layer [mm]
        // class 0: a thin emergency strut for narrow gaps (smaller gap to the model)
        double thin_radius = 0.25;
        double thin_gap    = 0.25;
    };

    // `cancelled` is polled between the stages; if it returns true the
    // construction is abandoned and valid() returns false.
    OrganicAvoidance(const indexed_triangle_set       &model,
                     const Params                    &params,
                     const std::function<bool()>     &cancelled);

    bool   valid() const { return m_valid; }
    size_t layer_count() const { return m_layers; }
    double layer_z(size_t layer) const { return m_p.ground_z + double(layer) * m_p.step; }
    double step() const { return m_p.step; }
    const Params &params() const { return m_p; }

    // Smallest class whose radius is >= r (saturates at the last class).
    // Class 0 is the thin strut, classes 1.. span r_min..r_max.
    size_t radius_class(double r) const;
    double class_radius(size_t rc) const;
    size_t class_count() const { return m_collision.size(); }
    size_t regular_classes() const { return m_collision.size() - 1; }

    // Can a branch of the given class not be centered at `p` on this layer?
    // to_bed: additionally treat the places as blocked from where the bed
    // cannot be reached any more with the maximal move speed.
    bool blocked(size_t layer, size_t rc, bool to_bed, const Vec2d &p) const;

    // Nearest not blocked position within max_dist.
    bool nearest_free(size_t layer, size_t rc, bool to_bed,
                      const Vec2d &p, double max_dist, Vec2d &out) const;

private:
    struct Region
    {
        ExPolygons                polys;
        std::vector<BoundingBox>  bbs;
        void finalize();
        bool contains(const Point &p) const;
    };

    const Region &region(size_t layer, size_t rc, bool to_bed) const;

    Params m_p;
    size_t m_layers = 0;
    bool   m_valid  = false;
    std::vector<std::vector<Region>> m_collision; // [rc][layer]
    std::vector<std::vector<Region>> m_avoid;     // [rc][layer], reaching the bed
};

}} // namespace Slic3r::sla

#endif // ORGANICAVOIDANCE_HPP
