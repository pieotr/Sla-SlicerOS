///|/ Copyright (c) Prusa Research 2024 Filip Sykala @Jony01
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "SupportPointGenerator.hpp"
#include "SupportOverhangSafetyOffset.hpp"
#include "PeninsulaMinimumOffsetReuse.hpp"
#include "SupportLinkIntersections.hpp"
#include <chrono>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <string_view>
#include <functional>
#include <map>
#include <boost/log/trivial.hpp>

#include "libslic3r/Execution/ExecutionTBB.hpp" // parallel preparation of data for sampling
#include "libslic3r/Execution/Execution.hpp"
#include "libslic3r/KDTreeIndirect.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/ClipperOffsetProfile.hpp"
#include "libslic3r/AABBTreeLines.hpp" // closest point to layer part
#include "libslic3r/AABBMesh.hpp" // move_on_mesh_surface Should be in another file
// SupportIslands
#include "libslic3r/SLA/SupportIslands/UniformSupportIsland.hpp"
#include "libslic3r/SLA/SupportIslands/SampleConfigFactory.hpp"

using namespace Slic3r;
using namespace Slic3r::sla;

namespace {
#ifndef NDEBUG
bool exist_point_in_distance(const Vec3f &p, float distance, const LayerSupportPoints &pts) {
    float distance_sq = sqr(distance);
    return std::any_of(pts.begin(), pts.end(), [&p, distance_sq](const LayerSupportPoint &sp) {
        return (sp.pos - p).squaredNorm() < distance_sq; });
}
#endif // NDEBUG

/// <summary>
/// Struct to store support points in KD tree to fast search for nearest ones.
/// </summary>
class NearPoints
{
    /// <summary>
    /// Structure made for KDTree as function to 
    /// acess support point coordinate by index into global support point storage
    /// </summary>
    struct PointAccessor {
        // multiple trees points into same data storage of all support points
        LayerSupportPoints *m_supports_ptr;
        explicit PointAccessor(LayerSupportPoints *supports_ptr) : m_supports_ptr(supports_ptr) {}
        // accessor to coordinate for kd tree
        const coord_t &operator()(size_t idx, size_t dimension) const {
            return m_supports_ptr->at(idx).position_on_layer[dimension];
        }
    };

    PointAccessor m_points;
    using Tree = KDTreeIndirect<2, coord_t, PointAccessor>;
    Tree m_tree;
public:
    /// <summary>
    /// Constructor get pointer on the global storage of support points
    /// </summary>
    /// <param name="supports_ptr">Pointer on Support vector</param>
    explicit NearPoints(LayerSupportPoints* supports_ptr)
        : m_points(supports_ptr), m_tree(m_points) {}

    NearPoints get_copy(){ 
        NearPoints copy(m_points.m_supports_ptr);
        copy.m_tree = m_tree.get_copy(); // copy tree
        return copy;
    }

    /// <summary>
    /// Remove support points from KD-Tree which lay out of expolygons
    /// </summary>
    /// <param name="shapes">Define area where could be support points</param>
    /// <param name="current_z">Current layer z coordinate 
    /// to prevent remove permanent points in prev layer influence</param>
    void remove_out_of(const ExPolygons &shapes, float current_z) {
        std::vector<size_t> indices = get_indices();
        auto it = std::remove_if(indices.begin(), indices.end(), 
            [&pts = *m_points.m_supports_ptr, &shapes, current_z](size_t point_index) {
                const LayerSupportPoint& lsp = pts.at(point_index);
                if (lsp.is_permanent && lsp.pos.z() >= current_z)
                    return false;
                return !std::any_of(shapes.begin(), shapes.end(), 
                    [&p = lsp.position_on_layer](const ExPolygon &shape) {
                        return shape.contains(p);
                    });
            });
        if (it == indices.end())
            return; // no point to remove
        indices.erase(it, indices.end());
        m_tree.clear();
        m_tree.build(indices); // consume indices
    }

    /// <summary>
    /// Add point new support point into global storage of support points 
    /// and pointer into tree structure of nearest points
    /// </summary>
    /// <param name="point">New added point</param>
    void add(LayerSupportPoint &&point) {
        // IMPROVE: only add to existing tree, do not reconstruct tree
        std::vector<size_t> indices = get_indices();
        LayerSupportPoints &pts = *m_points.m_supports_ptr;
        assert(!exist_point_in_distance(point.pos, point.head_front_radius, pts));
        size_t index = pts.size();
        pts.emplace_back(std::move(point));
        indices.push_back(index);
        m_tree.clear();
        m_tree.build(indices); // consume indices
    }

    using CheckFnc = std::function<bool(const LayerSupportPoint &, const Point&)>;
    /// <summary>
    /// Iterate over support points in 2d radius and search any of fnc with True.
    /// Made for check wheather surrounding supports support current point p.
    /// </summary>
    /// <param name="pos">Center of search circle</param>
    /// <param name="radius">Search circle radius</param>
    /// <param name="fnc">Function to check supports point</param>
    /// <returns>True wheny any of check function return true, otherwise False</returns>
    bool exist_true_in_radius(const Point &pos, coord_t radius, const CheckFnc &fnc) const {
        std::vector<size_t> point_indices = find_nearby_points(m_tree, pos, radius);
        return std::any_of(point_indices.begin(), point_indices.end(), 
            [&points = *m_points.m_supports_ptr, &pos, &fnc](size_t point_index){
                return fnc(points.at(point_index), pos);
            });
    }

    /// <summary>
    /// Merge another tree structure into current one.
    /// Made for connection of two mesh parts.
    /// </summary>
    /// <param name="near_point">Another near points</param>
    void merge(NearPoints &&near_point) {
        // need to have same global storage of support points
        assert(m_points.m_supports_ptr == near_point.m_points.m_supports_ptr);

        // IMPROVE: merge trees instead of rebuild
        std::vector<size_t> indices = get_indices();
        std::vector<size_t> indices2 = near_point.get_indices();
        // merge
        indices.insert(indices.end(),
            std::move_iterator(indices2.begin()), 
            std::move_iterator(indices2.end()));
        // remove duplicit indices - Diamond case
        std::sort(indices.begin(), indices.end());
        auto it = std::unique(indices.begin(), indices.end());
        indices.erase(it, indices.end());
        // rebuild tree
        m_tree.clear();
        m_tree.build(indices); // consume indices
    }

    /// <summary>
    /// Getter on used indices
    /// </summary>
    /// <returns>Current used Indices into m_points</returns>
    std::vector<size_t> get_indices() const {
        std::vector<size_t> indices = m_tree.get_nodes(); // copy
        // nodes in tree contain "max values for size_t" on unused leaf of tree,
        // when count of indices is not exactly power of 2
        auto it = std::remove_if(indices.begin(), indices.end(), 
            [max_index = m_points.m_supports_ptr->size()](size_t i) { return i >= max_index; });
        indices.erase(it, indices.end());
        return indices;
    }
};
using NearPointss = std::vector<NearPoints>;

/// <summary>
/// Intersection of line segment and circle
/// </summary>
/// <param name="p1">Line segment point A, Point lay inside circle</param>
/// <param name="p2">Line segment point B, Point lay outside or on circle</param>
/// <param name="cnt">Circle center point</param>
/// <param name="r2">squared value of Circle Radius (r2 = r*r)</param>
/// <returns>Intersection point</returns>
Point intersection_line_circle(const Point &p1, const Point &p2, const Point &cnt, double r2) {
    // Vector from p1 to p2
    Vec2d dp_d((p2 - p1).cast<double>());
    // Vector from circle center to p1
    Vec2d f_d((p1 - cnt).cast<double>());

    double a = dp_d.squaredNorm();
    double b = 2 * (f_d.x() * dp_d.x() + f_d.y() * dp_d.y());
    double c = f_d.squaredNorm() - r2;

    // Discriminant of the quadratic equation
    double discriminant = b * b - 4 * a * c;

    // No intersection if discriminant is negative
    assert(discriminant >= 0);
    if (discriminant < 0)
        return {}; // No intersection

    // Calculate the two possible values of t (parametric parameter)
    discriminant = sqrt(discriminant);
    double t1 = (-b - discriminant) / (2 * a);

    // Check for valid intersection points within the line segment
    if (t1 >= 0 && t1 <= 1) {
        return {p1.x() + t1 * dp_d.x(), p1.y() + t1 * dp_d.y()};
    }

    // should not be in use
    double t2 = (-b + discriminant) / (2 * a);
    if (t2 >= 0 && t2 <= 1 && t1 != t2) {
        return {p1.x() + t2 * dp_d.x(), p1.y() + t2 * dp_d.y()};
    }
    return {};
}

/// <summary>
/// Move grid from previous layer to current one
/// for given part
/// </summary>
/// <param name="prev_layer_parts">Grids generated in previous layer</param>
/// <param name="part">Current layer part to process</param>
/// <param name="prev_grids">Grids which will be moved to current grid</param>
/// <returns>Grid for given part</returns>
NearPoints create_near_points(
    const LayerParts &prev_layer_parts,
    const LayerPart &part,
    NearPointss &prev_grids
) {
    const LayerParts::const_iterator &prev_part_it = part.prev_parts.front();
    size_t index_of_prev_part = prev_part_it - prev_layer_parts.begin();
    NearPoints near_points = (prev_part_it->next_parts.size() == 1)?
        std::move(prev_grids[index_of_prev_part]) :
        // Need a copy there are multiple parts above previus one
        prev_grids[index_of_prev_part].get_copy(); // copy    

    // merge other grid in case of multiple previous parts
    for (size_t i = 1; i < part.prev_parts.size(); ++i) {
        const LayerParts::const_iterator &prev_part_it = part.prev_parts[i];
        size_t index_of_prev_part = prev_part_it - prev_layer_parts.begin();
        if (prev_part_it->next_parts.size() == 1) {
            near_points.merge(std::move(prev_grids[index_of_prev_part]));
        } else { // Need a copy there are multiple parts above previus one
            NearPoints grid_ = prev_grids[index_of_prev_part].get_copy(); // copy
            near_points.merge(std::move(grid_));
        }
    }
    return near_points;
}

/// <summary>
/// Add support point to near_points when it is neccessary
/// </summary>
/// <param name="part">Current part - keep samples</param>
/// <param name="config">Configuration to sample</param>
/// <param name="near_points">Keep previous sampled suppport points</param>
/// <param name="part_z">current z coordinate of part</param>
/// <param name="maximal_radius">Max distance to seach support for sample</param>
void support_part_overhangs(
    const LayerPart &part,
    const SupportPointGeneratorConfig &config,
    NearPoints &near_points,
    float part_z,
    coord_t maximal_radius
) {
    NearPoints::CheckFnc is_supported = []
    (const LayerSupportPoint &support_point, const Point &p) -> bool {
        // Debug visualization of all sampled outline
        //return false;
        coord_t r = support_point.current_radius;
        Point dp = support_point.position_on_layer - p;
        if (std::abs(dp.x()) > r) return false;
        if (std::abs(dp.y()) > r) return false;
        double r2 = sqr(static_cast<double>(r));
        return dp.cast<double>().squaredNorm() < r2;
    };

    for (const Point &p : part.samples) {
        if (!near_points.exist_true_in_radius(p, maximal_radius, is_supported)) {
            // not supported sample, soo create new support point
            near_points.add(LayerSupportPoint{
                SupportPoint{
                    Vec3f{unscale<float>(p.x()), unscale<float>(p.y()), part_z},
                    /* head_front_radius */ config.head_diameter / 2,
                    SupportPointType::slope
                },
                /* position_on_layer */ p,
                /* radius_curve_index */ 0,
                /* current_radius */ static_cast<coord_t>(scale_(config.support_curve.front().x()))
                });
        }    
    }
}

/// <summary>
/// Sample part as Island
/// Result store to grid
/// </summary>
/// <param name="part">Island to support</param>
/// <param name="near_points">OUT place to store new supports</param>
/// <param name="part_z">z coordinate of part</param>
/// <param name="permanent">z coordinate of part</param>
/// <param name="cfg"></param>
void support_island(const LayerPart &part, NearPoints& near_points, float part_z,
    const Points &permanent, const SupportPointGeneratorConfig &cfg, const ThrowOnCancel &throw_on_cancel) {
    throw_on_cancel();
    SupportIslandPoints samples = uniform_support_island(*part.shape, permanent, cfg.island_configuration, throw_on_cancel);
    for (const SupportIslandPointPtr &sample : samples)
        near_points.add(LayerSupportPoint{
            SupportPoint{
                Vec3f{
                    unscale<float>(sample->point.x()), 
                    unscale<float>(sample->point.y()), 
                    part_z
                },
                /* head_front_radius */ cfg.head_diameter / 2,
                SupportPointType::island
            },
            /* position_on_layer */ sample->point,
            /* radius_curve_index */ 0,
            /* current_radius */ static_cast<coord_t>(scale_(cfg.support_curve.front().x()))
        });
}

void support_peninsulas(const Peninsulas& peninsulas, NearPoints& near_points, float part_z,
    const Points &permanent, const SupportPointGeneratorConfig &cfg, const ThrowOnCancel &throw_on_cancel) {
    for (const Peninsula& peninsula: peninsulas) {
        throw_on_cancel();
        SupportIslandPoints peninsula_supports =
            uniform_support_peninsula(peninsula, permanent, cfg.island_configuration, throw_on_cancel);
        for (const SupportIslandPointPtr &support : peninsula_supports)
            near_points.add(LayerSupportPoint{
                SupportPoint{
                    Vec3f{
                        unscale<float>(support->point.x()), 
                        unscale<float>(support->point.y()), 
                        part_z
                    },
                    /* head_front_radius */ cfg.head_diameter / 2, 
                    SupportPointType::island
                },
                /* position_on_layer */ support->point,
                /* radius_curve_index */ 0,
                /* current_radius */ static_cast<coord_t>(scale_(cfg.support_curve.front().x()))
            });
    }   
}

/// <summary>
/// Copy parts shapes from link to output
/// </summary>
/// <param name="part_links">Links between part of mesh</param>
/// <returns>Collected expolygons from links</returns>
ExPolygons get_shapes(const PartLinks& part_links) {
    ExPolygons out;
    out.reserve(part_links.size());
    for (const PartLink &part_link : part_links)
        out.push_back(*part_link->shape); // copy
    return out;
}

/// <summary>
/// Uniformly sample Polyline,
/// Use first point and each next point is first crosing radius from last added
/// </summary>
/// <param name="b">Begin of polyline points to sample</param>
/// <param name="e">End of polyline points to sample</param>
/// <param name="dist2">Squared distance for sampling</param>
/// <returns>Uniformly distributed points laying on input polygon
/// with exception of first and last point(they are closer than dist2)</returns>
Slic3r::Points sample(Points::const_iterator b, Points::const_iterator e, double dist2) {
    assert(e-b >= 2);
    if (e - b < 2)
        return {}; // at least one line(2 points)

    // IMPROVE1: start of sampling e.g. center of Polyline
    // IMPROVE2: Random offset(To remove align of point between slices)
    // IMPROVE3: Sample small overhangs with memory for last sample(OR in center)
    Slic3r::Points r;
    r.push_back(*b);

    //Points::const_iterator prev_pt = e;
    const Point *prev_pt = nullptr;
    for (Points::const_iterator it = b; it+1 < e; ++it){        
        const Point &pt = *(it+1);
        double p_dist2 = (r.back() - pt).cast<double>().squaredNorm();
        while (p_dist2 > dist2) { // line segment goes out of radius
            if (prev_pt == nullptr)
                prev_pt = &(*it);
            r.push_back(intersection_line_circle(*prev_pt, pt, r.back(), dist2));
            p_dist2 = (r.back() - pt).cast<double>().squaredNorm();
            prev_pt = &r.back();
        }
        prev_pt = nullptr;
    }
    return r;
}

bool contain_point(const Point &p, const Points &sorted_points) {
    auto it = std::lower_bound(sorted_points.begin(), sorted_points.end(), p);
    if (it == sorted_points.end())
        return false;
    ++it; // next point should be same as searched
    if (it == sorted_points.end())
        return false;
    return it->x() == p.x() && it->y() == p.y();
};

#ifndef NDEBUG
bool exist_same_points(const ExPolygon &shape, const Points& prev_points) {
    auto shape_points = to_points(shape);
    return shape_points.end() !=
        std::find_if(shape_points.begin(), shape_points.end(), [&prev_points](const Point &p) {
            return contain_point(p, prev_points);
        });
}
#endif // NDEBUG

Points sample_overhangs(const LayerPart& part, double dist2, const ThrowOnCancel &throw_on_cancel,
    const Polygons *prepared_clip=nullptr) {
    throw_on_cancel();
    const ExPolygon &shape = *part.shape;

    // Collect previous expolygons by links collected in loop before    
    ExPolygons prev_shapes = get_shapes(part.prev_parts);
    assert(!prev_shapes.empty());
    ExPolygons overhangs = prepared_clip ? diff_ex(shape, *prepared_clip, ApplySafetyOffset::No) :
        diff_ex(shape, prev_shapes, ApplySafetyOffset::Yes);
    throw_on_cancel();
    if (overhangs.empty()) // above part is smaller in whole contour
        return {};
    
    Points prev_points = to_points(prev_shapes);
    std::sort(prev_points.begin(), prev_points.end());
    throw_on_cancel();

    // TODO: solve case when shape and prev points has same point
    assert(!exist_same_points(shape, prev_points));
        
    auto sample_overhang = [&prev_points, dist2, &throw_on_cancel](const Polygon &polygon, Points &samples) {
        const Points &pts = polygon.points;
        // first point which is not part of shape
        Points::const_iterator first_bad = pts.end();
        Points::const_iterator start_it = pts.end();
        for (auto it = pts.begin(); it != pts.end(); ++it) {
            if (size_t(it-pts.begin()) % 256 == 0)
                throw_on_cancel();
            const Point &p = *it;
            if (contain_point(p, prev_points)) {
                if (first_bad == pts.end()) {
                    // remember begining
                    first_bad = it;
                }
                if (start_it != pts.end()) {
                    // finish sampling
                    append(samples, sample(start_it, it, dist2));
                    // prepare for new start
                    start_it = pts.end();
                }
            } else if (start_it == pts.end()) {
                start_it = it;
            }
        }

        // sample last segment
        if (start_it == pts.end()) { // tail is without points
            if (first_bad != pts.begin()) // only begining
                append(samples, sample(pts.begin(), first_bad, dist2));
        } else { // tail contain points
            if (first_bad == pts.begin()) { // only tail
                append(samples, sample(start_it, pts.end(), dist2));
            } else if (start_it == pts.begin()) { // whole polygon is overhang
                assert(first_bad == pts.end());
                Points pts2 = pts; // copy
                pts2.push_back(pts.front());
                append(samples, sample(pts2.begin(), pts2.end(), dist2));
            } else { // need connect begining and tail
                Points pts2;
                pts2.reserve((pts.end() - start_it) + 
                             (first_bad - pts.begin()));
                for (auto it = start_it; it < pts.end(); ++it)
                    pts2.push_back(*it);
                for (auto it = pts.begin(); it < first_bad; ++it)
                    pts2.push_back(*it);
                append(samples, sample(pts2.begin(), pts2.end(), dist2));
            }
        }
    };

    Points samples;
    for (const ExPolygon &overhang : overhangs) {
        sample_overhang(overhang.contour, samples);
        for (const Polygon &hole : overhang.holes) {            
            sample_overhang(hole, samples);
        }
    }
    return samples;
}

coord_t calc_influence_radius(float z_distance, const SupportPointGeneratorConfig &config) { 
    float island_support_distance_sq = sqr(config.support_curve.front().x());
    if (!is_approx(config.density_relative, 1.f, 1e-4f)) // exist relative density
        island_support_distance_sq /= config.density_relative;
    float z_distance_sq = sqr(z_distance);
    if (z_distance_sq >= island_support_distance_sq)
        return 0.f;
    // IMPROVE: use curve interpolation instead of sqrt(stored in config).
    // shape of supported area before permanent supports is sphere with radius of island_support_distance
    return static_cast<coord_t>(scale_(std::sqrt(island_support_distance_sq - z_distance_sq)));
}

void prepare_supports_for_layer(LayerSupportPoints &supports, float layer_z, 
    const NearPointss& activ_points, const SupportPointGeneratorConfig &config) {
    auto set_radius = [&config](LayerSupportPoint &support, float radius) {
        if (!is_approx(config.density_relative, 1.f, 1e-4f)) // exist relative density
            radius = std::sqrt(sqr(radius) / config.density_relative);
        support.current_radius = static_cast<coord_t>(scale_(radius));
    };

    std::vector<bool> is_active(supports.size(), {false});
    for (const NearPoints& pts : activ_points) {
        std::vector<size_t> indices = pts.get_indices();
        for (size_t i : indices)
            is_active[i] = true;
    }

    const std::vector<Vec2f>& curve = config.support_curve;
    // calculate support area for each support point as radius
    // IMPROVE: use some offsets of previous supported island
    for (LayerSupportPoint &support : supports) {
        size_t &index = support.radius_curve_index;
        if (index + 1 >= curve.size())
            continue; // already contain maximal radius

        if (!is_active[&support - &supports.front()])
            continue; // Point is not used in any part of the current layer 

        // find current segment
        float diff_z = layer_z - support.pos.z();
        if (diff_z < 0.) {
            // permanent support influence distribution of support points printed before.
            support.current_radius = calc_influence_radius(-diff_z, config);
            continue;
        }
        while ((index + 1) < curve.size() && diff_z > curve[index + 1].y())
            ++index;

        if ((index+1) >= curve.size()) {
            // set maximal radius
            set_radius(support, curve.back().x());
            continue;
        }
        // interpolate radius on input curve
        Vec2f a = curve[index];
        Vec2f b = curve[index+1];
        assert(a.y() <= diff_z && diff_z <= b.y());
        float t = (diff_z - a.y()) / (b.y() - a.y());
        assert(0 <= t && t <= 1);
        set_radius(support, a.x() + t * (b.x() - a.x()));
    }
}

/// <summary>
/// Near points do not have to contain support points out of part.
/// Due to be able support in same area again(overhang above another overhang)
/// Wanted Side effect, it supports thiny part of overhangs
/// </summary>
/// <param name="near_points"></param>
/// <param name="part">current </param>
/// <param name="current_z">Current layer z coordinate 
/// to prevent remove permanent points in prev layer influence</param>
void remove_supports_out_of_part(NearPoints &near_points, const LayerPart &part, float current_z) {
    // Offsetting is made in data preparation - speed up caused by paralelization 
    //ExPolygons extend_shape = offset_ex(*part.shape, config.removing_delta, ClipperLib::jtSquare);
    near_points.remove_out_of(part.extend_shape, current_z);
}

/// <summary>
/// Detect existence of peninsula on current layer part
/// </summary>
/// <param name="part">IN/OUT island part containing peninsulas</param>
/// <param name="min_peninsula_width">minimal width of overhang to become peninsula</param>
/// <param name="self_supported_width">supported distance from mainland</param>
// Diagnostic slots belong to independent jobs; reduce only after the TBB barrier.
struct PeninsulaProfile {
    std::array<int64_t, 9> elapsed_ns{};
    uint64_t previous_links = 0, previous_vertices = 0, current_vertices = 0;
    uint64_t below_lines = 0, candidates = 0, retained = 0;
    uint64_t annotation_queries = 0, candidate_edges = 0;
    bool covered_return = false, tiny_return = false, completed = false;
    // Completed offset work is distinct from the existing interval, which may
    // also include another job's call_once computation and waiting.
    bool minimum_requested = false, minimum_slot = false, minimum_computed = false;
    int64_t minimum_compute_ns = 0;
    ClipperOffsetProfile minimum_kernel;
    uint64_t minimum_output_polygons = 0, minimum_output_vertices = 0;
};

void create_peninsulas(LayerPart &part, const PrepareSupportConfig &config,
    const ThrowOnCancel &throw_on_cancel, PeninsulaProfile *profile = nullptr,
    detail::PeninsulaMinimumOffsetSlot *minimum_offset_slot = nullptr) {
    using Clock = std::chrono::steady_clock;
    Clock::time_point phase_start{};
    if (profile != nullptr)
        phase_start = Clock::now();
    const auto checkpoint = [profile, &phase_start](size_t phase) {
        if (profile != nullptr) {
            const auto now = Clock::now();
            profile->elapsed_ns[phase] += std::chrono::duration_cast<
                std::chrono::nanoseconds>(now - phase_start).count();
            phase_start = now;
        }
    };

    throw_on_cancel();
    assert(config.peninsula_min_width > config.peninsula_self_supported_width);
    const ExPolygon &part_shape = *part.shape;
    const ExPolygons below_shapes = get_shapes(part.prev_parts);
    checkpoint(0);
    if (profile != nullptr) {
        profile->previous_links = part.prev_parts.size();
        const auto vertices = [](const ExPolygon &shape) {
            uint64_t count = shape.contour.points.size();
            for (const Polygon &hole : shape.holes)
                count += hole.points.size();
            return count;
        };
        profile->current_vertices = vertices(part_shape);
        for (const ExPolygon &shape : below_shapes)
            profile->previous_vertices += vertices(shape);
        // Exclude diagnostic counting from the following geometry bucket.
        phase_start = Clock::now();
    }
    // A covered part has no overhang at all. A nonnegative expansion cannot
    // expose any new region, so avoid two costly offsets and edge matching.
    // Use polygon difference, including holes and the union of all linked
    // parts; bounding boxes or matching contour points alone are insufficient.
    if (config.peninsula_min_width >= 0 && config.peninsula_self_supported_width >= 0) {
        const bool covered = diff_ex(part_shape, below_shapes).empty();
        checkpoint(1);
        throw_on_cancel();
        if (covered) {
            if (profile != nullptr)
                profile->covered_return = profile->completed = true;
            return;
        }
    }
    ExPolygons local_minimum_offset;
    if (profile != nullptr) {
        profile->minimum_requested = true;
        profile->minimum_slot = minimum_offset_slot != nullptr;
    }
    const auto compute_minimum_offset = [&] {
        if (profile == nullptr)
            return offset_ex(below_shapes, config.peninsula_min_width, ClipperLib::jtSquare);
        const auto started = Clock::now();
        ExPolygons result = offset_ex_profiled(below_shapes, config.peninsula_min_width,
            profile->minimum_kernel, ClipperLib::jtSquare);
        profile->minimum_compute_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now() - started).count();
        profile->minimum_computed = true;
        profile->minimum_output_polygons = result.size();
        // Count sizes, not point values, after the measured kernel. This does
        // not retain output geometry or introduce a second result cache.
        for (const ExPolygon &shape : result) {
            profile->minimum_output_vertices += shape.contour.points.size();
            for (const Polygon &hole : shape.holes)
                profile->minimum_output_vertices += hole.points.size();
        }
        return result;
    };
    // Repeated immutable inputs share only this pure offset, after each job's
    // covered guard. Downstream differences/annotations remain independent.
    const ExPolygons &below_expanded = minimum_offset_slot != nullptr
        ? minimum_offset_slot->get(compute_minimum_offset, throw_on_cancel)
        : (local_minimum_offset = compute_minimum_offset());
    checkpoint(2);
    throw_on_cancel();
    ExPolygons over_peninsula = diff_ex(part_shape, below_expanded);
    checkpoint(3);
    throw_on_cancel();
    if (over_peninsula.empty()) {
        if (profile != nullptr)
            profile->tiny_return = profile->completed = true;
        return; // only tiny overhangs
    }

    ExPolygons below_self_supported = offset_ex(below_shapes, config.peninsula_self_supported_width, ClipperLib::jtSquare);
    checkpoint(4);
    throw_on_cancel();
    // exist weird edge case, where expand function return empty result
    assert(below_self_supported.empty());

    // exist layer part over peninsula limit
    ExPolygons peninsulas_shape = diff_ex(part_shape, below_self_supported);
    checkpoint(5);
    throw_on_cancel();

    // IMPROVE: Anotate source of diff by ClipperLib_Z
    Lines below_lines = to_lines(below_self_supported);
    auto get_angle = [](const Line &l) {
        Point diff = l.b - l.a;
        if (diff.x() < 0) // Only positive direction X
            diff = -diff;
        return atan2(diff.y(), diff.x());
    };
    std::vector<double> belowe_line_angle; // define direction of line with positive X
    belowe_line_angle.reserve(below_lines.size()); 
    for (const Line& l : below_lines)
        belowe_line_angle.push_back(get_angle(l));
    std::vector<size_t> idx(below_lines.size());
    std::iota(idx.begin(), idx.end(), 0);
    auto is_lower = [&belowe_line_angle](size_t i1, size_t i2) {
        return belowe_line_angle[i1] < belowe_line_angle[i2]; };
    std::sort(idx.begin(), idx.end(), is_lower);
    checkpoint(6);
    if (profile != nullptr) {
        profile->below_lines = below_lines.size();
        profile->candidates = peninsulas_shape.size();
    }
    throw_on_cancel();

    // Check, wheather line exist in set of belowe lines
    // True .. line exist in previous layer (or partialy overlap previous line), connection to land
    // False .. line is made by border of current layer part(peninsula coast)
    auto exist_belowe = [&get_angle, &idx, &below_lines, &belowe_line_angle, profile]
    (const Line &l) {
        if (profile != nullptr)
            ++profile->annotation_queries;
        // It is edge case of expand
        if (below_lines.empty())
            return false;
        // allowed angle epsilon
        const double angle_epsilon = 1e-3; // < 0.06 DEG
        const double paralel_epsilon = scale_(1e-2); // 10 um
        double angle = get_angle(l);
        double low_angle = angle - angle_epsilon;
        bool is_over = false;
        if (low_angle <= -M_PI_2) {
            low_angle += M_PI;
            is_over = true;
        }
        double hi_angle = angle + angle_epsilon;
        if (hi_angle >= M_PI_2) {
            hi_angle -= M_PI;
            is_over = true;
        }
        int mayorit_idx = 0;
        if (Point d = l.a - l.b; 
            abs(d.x()) < abs(d.y()))
            mayorit_idx = 1;

        coord_t low = l.a[mayorit_idx];
        coord_t high = l.b[mayorit_idx];
        if (low > high)
            std::swap(low, high);

        auto is_lower_angle = [&belowe_line_angle](size_t index, double angle) {
            return belowe_line_angle[index] < angle; };
        auto it_idx = std::lower_bound(idx.begin(), idx.end(), low_angle, is_lower_angle);
        if (it_idx == idx.end()) {
            if (is_over) {
                it_idx = idx.begin();
                is_over = false;
            } else {
                return false;
            }
        }
        while (is_over || belowe_line_angle[*it_idx] < hi_angle) {
            if (profile != nullptr)
                ++profile->candidate_edges;
            const Line &l2 = below_lines[*it_idx];
            coord_t l2_low = l2.a[mayorit_idx];
            coord_t l2_high = l2.b[mayorit_idx];
            if (low > high)
                std::swap(low, high);
            if ((l2_high >= low && l2_low <= high) && (
                ((l2.a == l.a && l2.b == l.b) ||(l2.a == l.b && l2.b == l.a)) || // speed up - same line
                l.perp_distance_to(l2.a) < paralel_epsilon)) // check distance of parallel lines
                return true;
            ++it_idx;
            if (it_idx == idx.end()){
                if (is_over) {
                    it_idx = idx.begin();
                    is_over = false;
                } else {
                    break;            
                }
            }
        }
        return false;
    };

    // anotate source of peninsula: overhang VS previous layer  
    for (const ExPolygon &peninsula : peninsulas_shape) {
        throw_on_cancel();
        // Check that peninsula is wide enough(min_peninsula_width)
        // Keep the exact original intersection and early-return ordering.
        if (profile != nullptr)
            phase_start = Clock::now();
        const bool narrow = intersection_ex(ExPolygons{peninsula}, over_peninsula).empty();
        checkpoint(7);
        if (narrow)
            continue; 

        // need to know shape and edges of peninsula
        Lines lines = to_lines(peninsula);
        std::vector<bool> is_outline(lines.size());
        // when line overlap with belowe lines it is not outline
        for (size_t i = 0; i < lines.size(); i++) {
            if (i % 256 == 0)
                throw_on_cancel();
            is_outline[i] = !exist_belowe(lines[i]);
        }
        part.peninsulas.push_back(Peninsula{peninsula, is_outline});
        checkpoint(8);
        if (profile != nullptr)
            ++profile->retained;
    }
    if (profile != nullptr)
        profile->completed = true;
}

struct LayerPartIndex {
    size_t layer_index; // index into layers
    size_t part_index; // index into layer parts
    bool operator<(const LayerPartIndex &other) const {
        return layer_index < other.layer_index ||
            (layer_index == other.layer_index && part_index < other.part_index);
    }
    bool operator==(const LayerPartIndex &other) const {
        return layer_index == other.layer_index && part_index == other.part_index;
    }
};
using SmallPart = std::vector<LayerPartIndex>;
using SmallParts = std::vector<SmallPart>;

std::optional<SmallPart> create_small_part(
    const Layers &layers, const LayerPartIndex& island, float radius_in_mm) {
    // Search for BB with diameter(2*r) distance from part and collect parts
    const LayerPart &part = layers[island.layer_index].parts[island.part_index];
    coord_t radius = static_cast<coord_t>(scale_(radius_in_mm));
    
    // only island part could be source of small part
    assert(part.prev_parts.empty()); 
    // Island bounding box Should be already checked out of function
    assert(part.shape_extent.size().x() <= 2*radius &&
           part.shape_extent.size().y() <= 2*radius );
    
    Point range{radius, radius};
    Point center = part.shape_extent.center();
    BoundingBox range_bb{center - range,center + range};
    // BoundingBox range_bb{ // check exist sphere with radius to overlap model part
    //    part.shape_extent.min - range,
    //    part.shape_extent.max + range};

    /// <summary>
    /// Check parts in layers if they are in range of bounding box
    /// Recursive fast check function without storing any already searched data
    /// NOTE: Small part with holes could slow down checking
    /// </summary>
    /// <param name="check">Index of layer and part</param>
    /// <param name="allowed_depth">Recursion protection</param>
    /// <param name="prev_check">To prevent cycling calls</param>
    std::function<bool(const LayerPartIndex &, size_t, const LayerPartIndex &)> check_parts;
    check_parts = [&range_bb, &check_parts, &layers, &island, radius_in_mm]
    (const LayerPartIndex& check, size_t allowed_depth, const LayerPartIndex& prev_check) -> bool {
        const Layer &check_layer = layers[check.layer_index];
        const LayerPart &check_part = check_layer.parts[check.part_index];
        for (const PartLink &link : check_part.next_parts)
            // part bound is far than diameter from source part
            if (!range_bb.contains(link->shape_extent.min) ||
                !range_bb.contains(link->shape_extent.max))
                return false; // part is too large

        if ((check_layer.print_z - layers[island.layer_index].print_z) > radius_in_mm)
            return false; // parts is large in Z direction

        --allowed_depth;
        if (allowed_depth == 0)
            return true; // break recursion

        // recursively check next layers
        size_t next_layer_i = check.layer_index + 1;
        for (const PartLink& link : check_part.next_parts) {
            size_t next_part_i = link - layers[next_layer_i].parts.cbegin();
            if (next_layer_i == prev_check.layer_index &&
                next_part_i == prev_check.part_index)
                continue; // checked in lambda caller contex
            if (!check_parts({next_layer_i, next_part_i}, allowed_depth, check))
                return false;
        }

        if (check.layer_index == island.layer_index) {
            if (!check_part.prev_parts.empty())
                return false; // prev layers are already checked

            // When model part start from multi islands on the same layer, 
            // Investigate only the first island(lower index into parts).
            if (check.part_index < island.part_index)
                return false; // part is already checked
        }                

        // NOTE: multiple investigation same part seems more relevant 
        // instead of store and search for already checked parts

        // check previous parts
        for (const PartLink &link : check_part.prev_parts) {
            // part bound is far than diameter from source part
            if (!range_bb.contains(link->shape_extent.min) ||
                !range_bb.contains(link->shape_extent.max))
                return false; // part is too large
        }

        for (const PartLink &link : check_part.prev_parts) {
            size_t prev_layer_i = check.layer_index - 1; // exist only when exist prev parts - cant be negative
            size_t prev_part_i = link - layers[prev_layer_i].parts.cbegin();

            if (prev_layer_i == prev_check.layer_index && 
                prev_part_i == prev_check.part_index)
                continue; // checked in lambda caller contex

            // Improve: check only parts which are not already checked
            if (!check_parts({prev_layer_i, prev_part_i}, allowed_depth, check))
                return false;
        }
        return true;
    };

    float layer_height = (island.layer_index == 0) ?
        layers[1].print_z - layers[0].print_z:
        layers[island.layer_index].print_z - layers[island.layer_index-1].print_z;

    assert(layer_height > 0.f);
    float safe_multiplicator = 1.4f; // 40% more layers than radius for zigzag(up & down layer) search
    size_t allowed_depth = static_cast<size_t>(std::ceil((radius_in_mm / layer_height + 1) * safe_multiplicator));
    // Check Bounding boxes and do not create any data - FAST CHECK
    // NOTE: it could check model parts multiple times those there is allowed_depth
    if (!check_parts(island, allowed_depth, island))
        return {};

    SmallPart collected; // sorted by layer_i, part_i
    std::vector<LayerPartIndex> queue_next;
    LayerPartIndex curr = island;
    // create small part by flood fill neighbor parts
    do {
        if (curr.layer_index >= layers.size()){
            if (queue_next.empty())
                break; // finish collecting
            curr = queue_next.back();
            queue_next.pop_back();
        }
        auto collected_it = std::lower_bound(collected.begin(), collected.end(), curr);
        if (collected_it != collected.end() && 
            *collected_it == curr ) // already processed
            continue;
        collected.insert(collected_it, curr); // insert sorted

        const LayerPart &curr_part = layers[curr.layer_index].parts[curr.part_index];
        LayerPartIndex next{layers.size(), 0}; // set to uninitialized value(layer index is out of layers)
        for (const PartLink &link : curr_part.next_parts) {
            size_t next_layer_i = curr.layer_index + 1;
            size_t part_i = link - layers[next_layer_i].parts.begin();
            LayerPartIndex next_{next_layer_i, part_i};
            auto it = std::lower_bound(collected.begin(), collected.end(), next_);
            if (it != collected.end() && *it == next_)
                continue; // already collected

            if (next.layer_index >= layers.size()) // next is uninitialized
                next = next_;
            else
                queue_next.push_back(next_); // insert sorted
        }
        for (const PartLink &link : curr_part.prev_parts) {
            size_t prev_layer_i = curr.layer_index - 1;
            size_t part_i = link - layers[prev_layer_i].parts.begin();
            LayerPartIndex next_{prev_layer_i, part_i};
            auto it = std::lower_bound(collected.begin(), collected.end(), next_);
            if (it != collected.end() && *it == next_)
                continue; // already collected

            if (next.layer_index >= layers.size()) // next is uninitialized
                next = next_;
            else
                queue_next.push_back(next_); // insert sorted
        }
        curr = next;
    } while (true); // NOTE: It is break when queue is empty && curr is invalid

    // Check that whole model part is inside support head sphere
    float print_z = layers[island.layer_index].print_z;
    std::vector<Vec3f> vertices;
    for (const LayerPartIndex &part_id : collected) {
        const Layer &layer = layers[part_id.layer_index];
        double radius_sq = (sqr(radius_in_mm) - sqr(layer.print_z - print_z)) / sqr(SCALING_FACTOR);
        const LayerPart &layer_part = layer.parts[part_id.part_index];
        for (const Point &p : layer_part.shape->contour.points) {
            Vec2d diff2d = (p - center).cast<double>();
            if (sqr(diff2d.x()) + sqr(diff2d.y()) > radius_sq)
                return {}; // part is too large
        }
    }
    return collected;
}

/// <summary>
/// Detection of small parts of support
/// </summary>
SmallParts get_small_parts(const Layers &layers, float radius_in_mm) {
    // collect islands
    coord_t diameter = static_cast<coord_t>(2 * scale_(radius_in_mm));
    std::vector<LayerPartIndex> islands;
    for (size_t layer_i = 0; layer_i < layers.size(); ++layer_i) {
        const Layer &layer = layers[layer_i];
        for (size_t part_i = 0; part_i < layer.parts.size(); ++part_i) {
            const LayerPart &part = layer.parts[part_i];
            if (!part.prev_parts.empty())
                continue; // not island
            if (const Point size = part.shape_extent.size();
                size.x() > diameter || size.y() > diameter)
                continue; // big island
            islands.push_back({layer_i, part_i});
        }
    }

    // multithreaded investigate islands
    std::mutex m; // write access into result
    SmallParts result;
    execution::for_each(ex_tbb, size_t(0), islands.size(),
    [&layers, radius_in_mm, &islands, &result, &m](size_t island_i) {
        std::optional<SmallPart> small_part_opt = create_small_part(layers, islands[island_i], radius_in_mm);
        if (!small_part_opt.has_value())
            return; // no small part
        std::lock_guard lock(m);
        result.push_back(*small_part_opt);
    }, 8 /* gransize */);
    return result;
}

void erase(const SmallParts &small_parts, Layers &layers) {
    // be carefull deleting small parts could invalidate const reference into vector with parts
    // whole layer must be threated at once
    std::vector<LayerPartIndex> to_erase;
    for (const SmallPart &small_part : small_parts) 
        to_erase.insert(to_erase.end(), small_part.begin(), small_part.end());

    auto cmp = [](const LayerPartIndex &a, const LayerPartIndex &b) {
        return a.layer_index < b.layer_index ||
            (a.layer_index == b.layer_index && a.part_index > b.part_index);};
    std::sort(to_erase.begin(), to_erase.end(), cmp); // sort by layer index and part index
    assert(std::unique(to_erase.begin(), to_erase.end()) == to_erase.end());
    size_t erase_to; // without this index
    for (size_t erase_from = 0; erase_from < to_erase.size(); erase_from = erase_to) {
        erase_to = erase_from + 1;
        size_t layer_index = to_erase[erase_from].layer_index;
        while (erase_to < to_erase.size() && 
            to_erase[erase_to].layer_index == layer_index)
            ++erase_to;

        Layer &layer = layers[layer_index];
        LayerParts& parts_ = layer.parts;
        LayerParts layer_parts = parts_; // copy current

        // https://stackoverflow.com/questions/11021764/does-moving-a-vector-invalidate-iterators
        // use swap where const iterator should be guaranteed, instead of move on the end of loop
        std::swap(layer_parts, parts_);  // swap copy into layer parts

        // NOTE: part indices are sorted descent
        for (size_t i = erase_from; i < erase_to; ++i)
            parts_.erase(parts_.begin() + to_erase[i].part_index); // remove parts
        
        if (layer_index > 0) { // not first layer
            Layer& prev_layer = layers[layer_index - 1];
            for (LayerPart& prev_part: prev_layer.parts){
                for (PartLink &next_part : prev_part.next_parts) {
                    size_t part_i = next_part - layer_parts.cbegin();
                    for (size_t i = erase_from; i < erase_to; ++i)
                        if (part_i >= to_erase[i].part_index)
                            --part_i; // index after removed part
                    assert(part_i < parts_.size());
                    next_part = parts_.begin() + part_i;
                }
            }
        }
        if (layer_index < layers.size() - 1) { // not last layer
            Layer& next_layer = layers[layer_index + 1];
            for (LayerPart &next_part : next_layer.parts) {
                for (PartLink &prev_part : next_part.prev_parts) {
                    size_t part_i = prev_part - layer_parts.cbegin();
                    for (size_t i = erase_from; i < erase_to; ++i)
                        if (part_i >= to_erase[i].part_index)
                            --part_i; // index after removed part
                    assert(part_i < parts_.size());
                    prev_part = parts_.begin() + part_i;
                }
            }
        }
    }
}

} // namespace

SupportPointGeneratorData Slic3r::sla::prepare_generator_data(
    std::vector<ExPolygons> &&slices,
    const std::vector<float> &heights,
    const PrepareSupportConfig &config,
    ThrowOnCancel throw_on_cancel,
    StatusFunction statusfn
) {
    // check input
    assert(!slices.empty());
    assert(slices.size() == heights.size());
    if (slices.empty() || slices.size() != heights.size())
        return SupportPointGeneratorData{};

    throw_on_cancel();
    auto phase_started = std::chrono::steady_clock::now();
    const auto log_phase = [&](const char *name) {
        throw_on_cancel();
        const auto now = std::chrono::steady_clock::now();
        BOOST_LOG_TRIVIAL(info) << "Support preparation: " << name << " "
            << std::chrono::duration<double>(now-phase_started).count() << " s";
        phase_started = now;
    };
    // Move input into result
    SupportPointGeneratorData result;
    result.slices = std::move(slices);

    // Allocate empty layers.
    result.layers = Layers(result.slices.size());

    // Generate Extents and SampleLayers
    execution::for_each(ex_tbb, size_t(0), result.slices.size(),
    [&result, &heights, throw_on_cancel](size_t layer_id) {
        throw_on_cancel();

        Layer &layer = result.layers[layer_id];
        layer.print_z = heights[layer_id]; // copy
        const ExPolygons &islands = result.slices[layer_id];
        layer.parts.reserve(islands.size());
        for (const ExPolygon &island : islands) {
            layer.parts.push_back(LayerPart{
                &island, {},
                get_extents(island.contour)
                // sample - only hangout part of expolygon could be known after linking
            });
        }        
    }, 4 /*gransize*/);

    log_phase("bounds");
    // Individual layers can contain very different amounts of geometry. Keep
    // expensive clipping jobs splittable down to one layer instead of leaving
    // a few eight-layer batches on the critical path. Each job writes only its
    // own derived buffers (and the adjacent layer's distinct next_parts field).
    // Bounding-box clipping is the production default. Explicit 0 retains
    // the full kernel for diagnostics and comparisons. Eligibility counters
    // describe that full kernel only; never label its timings as bbox work.
    const char *link_profile_env = std::getenv("SLA_PROFILE_SUPPORT_LINK_REDUCTION");
    const bool link_profile_enabled = link_profile_env && std::string_view(link_profile_env) == "1";
    const char *link_bbox_env = std::getenv("SLA_EXPERIMENT_SUPPORT_LINK_BBOX");
    const bool link_bbox_enabled = link_bbox_env == nullptr || std::string_view(link_bbox_env) != "0";
    if (link_profile_enabled && link_bbox_enabled)
        throw RuntimeError("Support-link bbox experiment cannot be combined with full-kernel reduction profiling");

    // One counter slot belongs to each layer job. Reduction happens only after
    // the existing TBB barrier, with no atomics in the nested pair loop.
    std::vector<detail::SupportLinkReductionProfile> link_profiles(
        link_profile_enabled ? result.layers.size() : 0);
    execution::for_each(ex_tbb, size_t(1), result.slices.size(),
    [&result, &link_profiles, link_profile_enabled, link_bbox_enabled, throw_on_cancel](size_t layer_id) {
        throw_on_cancel();

        LayerParts &parts_above = result.layers[layer_id].parts;
        LayerParts &parts_below = result.layers[layer_id - 1].parts;
        auto *profile = link_profile_enabled ? &link_profiles[layer_id] : nullptr;
        for (auto it_above = parts_above.begin(); it_above < parts_above.end(); ++it_above) {
            for (auto it_below = parts_below.begin(); it_below < parts_below.end(); ++it_below) {
                throw_on_cancel();
                if (profile)
                    ++profile->pairs;
                // Keep the closed bbox rejection and cancellation positions.
                if (!it_above->shape_extent.overlap(it_below->shape_extent)) {
                    if (profile)
                        ++profile->bbox_rejected;
                    continue;
                }

                const auto clip_started = profile ? std::chrono::steady_clock::now() :
                                                    std::chrono::steady_clock::time_point{};
                Polygons polys = link_bbox_enabled ?
                    detail::intersect_support_link_bbox(*it_above->shape, *it_below->shape) :
                    intersection(*it_above->shape, *it_below->shape);
                const auto clip_finished = profile ? std::chrono::steady_clock::now() :
                                                     std::chrono::steady_clock::time_point{};
                throw_on_cancel();
                if (profile) {
                    profile->clip_task_ns_sum += std::chrono::duration_cast<std::chrono::nanoseconds>(
                        clip_finished - clip_started).count();
                    profile->observe(*it_above->shape, *it_below->shape, !polys.empty());
                }
                if (polys.empty())
                    continue;

                // Preserve both owner indices and their append order.
                it_above->prev_parts.push_back(it_below);
                it_below->next_parts.push_back(it_above);
            }
        }
    }, 1 /* independently schedulable clipping layer */);

    BOOST_LOG_TRIVIAL(info) << "Support link intersection kernel=" << (link_bbox_enabled ? "bbox" : "full");
    if (link_profile_enabled) {
        detail::SupportLinkReductionProfile total;
        for (const auto &entry : link_profiles)
            total.append(entry);
        BOOST_LOG_TRIVIAL(info) << "Support link reduction profile: pairs=" << total.pairs
            << " bbox_rejected=" << total.bbox_rejected << " full_calls=" << total.full_calls
            << " nonempty=" << total.nonempty << " subject_vertices=" << total.subject_vertices
            << " clip_vertices=" << total.clip_vertices
            << " retained_clip_vertices=" << total.retained_clip_vertices
            << " reduced_pairs=" << total.reduced_pairs << " fallback_calls=" << total.fallback_calls
            << " clip_task_ns_sum=" << total.clip_task_ns_sum
            << " kernel=full diagnostic_reduction_only=1";
    }
    log_phase("link intersections");
    // erase unsupportable model parts
    SmallParts small_parts = get_small_parts(result.layers, config.minimal_bounding_sphere_radius);
    if(!small_parts.empty()) erase(small_parts, result.layers);

    log_phase("small parts");
    // Sample overhangs part of island
    double sample_distance_in_um = scale_(config.discretize_overhang_step);
    double sample_distance_in_um2 = sample_distance_in_um * sample_distance_in_um;
    execution::for_each(ex_tbb, size_t(1), result.layers.size(),
    [&result, sample_distance_in_um2, throw_on_cancel](size_t layer_id) {
        throw_on_cancel();

        LayerParts &parts = result.layers[layer_id].parts;
        struct ClipEntry { size_t uses=0; bool ready=false, disabled=false; Polygons paths; };
        std::map<detail::PeninsulaPreviousShapes,ClipEntry,detail::PeninsulaPreviousShapesLess> prepared;
        for (const auto &part:parts) {
            throw_on_cancel();
            if (part.prev_parts.empty()) continue;
            detail::PeninsulaPreviousShapes key;
            for (const auto &link:part.prev_parts) key.push_back(link->shape);
            ++prepared[key].uses;
        }
        // Per-layer ownership: no cross-worker references or retained print state.
        // The cap accounts vector capacities, not allocator bookkeeping.
        constexpr size_t cache_budget=8*1024*1024;
        size_t cached_bytes=0;
        for (auto it_part = parts.begin(); it_part < parts.end(); ++it_part) {
            throw_on_cancel();
            if (it_part->prev_parts.empty())
                continue; // island

            // IMPROVE: overhangs could be calculated with Z coordninate
            // soo one will know source shape of point and do not have to search this
            // information Get inspiration at
            // https://github.com/Prusa-Development/PrusaSlicerPrivate/blob/e00c46f070ec3d6fc325640b0dd10511f8acf5f7/src/libslic3r/PerimeterGenerator.cpp#L399
            detail::PeninsulaPreviousShapes key;
            for (const auto &link:it_part->prev_parts) key.push_back(link->shape);
            auto &entry=prepared.at(key);
            Polygons scratch;
            const Polygons *clip=nullptr;
            if (entry.uses>1 && !entry.disabled) {
                if (!entry.ready) {
                    scratch=prepare_support_safety_offset(get_shapes(it_part->prev_parts));
                    throw_on_cancel();
                    size_t bytes=scratch.capacity()*sizeof(Polygon);
                    for (const auto &polygon:scratch) bytes+=polygon.points.capacity()*sizeof(Point);
                    if (bytes<=cache_budget-cached_bytes) {
                        entry.paths=std::move(scratch);entry.ready=true;cached_bytes+=bytes;
                    } else {
                        entry.disabled=true;clip=&scratch;
                    }
                }
                if (entry.ready) clip=&entry.paths;
            }
            it_part->samples = sample_overhangs(*it_part, sample_distance_in_um2, throw_on_cancel,clip);
        }
    }, 1 /* independently schedulable clipping layer */);

    log_phase("overhang sampling");
    // Schedule independent parts rather than serializing all geometry of a
    // busy layer inside one task. Parts and links are stable after erase;
    // each job reads linked shapes and writes only its own peninsula vector.
    std::vector<LayerPart *> peninsula_jobs;
    for (size_t layer_id = 1; layer_id < result.layers.size(); ++layer_id) {
        throw_on_cancel();
        for (auto &part : result.layers[layer_id].parts) {
            throw_on_cancel();
            if (!part.prev_parts.empty())
                peninsula_jobs.push_back(&part);
        }
    }
    const char *reuse_env = std::getenv("SLA_EXPERIMENT_PENINSULA_OFFSET_REUSE");
    // Keep an internal opt-out for exact uncached regression/performance runs;
    // this is phase-local derived geometry, never a persisted print setting.
    const bool reuse_minimum_offsets = reuse_env == nullptr || std::string_view(reuse_env) != "0";
    std::unique_ptr<detail::PeninsulaMinimumOffsetReuse> minimum_offset_reuse;
    if (reuse_minimum_offsets) {
        std::vector<detail::PeninsulaPreviousShapes> inputs;
        inputs.reserve(peninsula_jobs.size());
        for (const LayerPart *part : peninsula_jobs) {
            throw_on_cancel();
            auto &key = inputs.emplace_back();
            key.reserve(part->prev_parts.size());
            for (const PartLink &link : part->prev_parts)
                key.push_back(link->shape);
        }
        minimum_offset_reuse = std::make_unique<detail::PeninsulaMinimumOffsetReuse>(inputs, throw_on_cancel);
    }
    const char *profile_env = std::getenv("SLA_PROFILE_SUPPORT_PENINSULAS");
    const bool profile_peninsulas = profile_env != nullptr && std::string_view(profile_env) == "1";
    std::vector<PeninsulaProfile> peninsula_profiles;
    if (profile_peninsulas)
        peninsula_profiles.resize(peninsula_jobs.size());
    std::vector<double> peninsula_job_seconds(peninsula_jobs.size());
    execution::for_each(ex_tbb, size_t(0), peninsula_jobs.size(),
    [&peninsula_jobs, &peninsula_job_seconds, &peninsula_profiles, &minimum_offset_reuse, profile_peninsulas, &config, throw_on_cancel](size_t job_id) {
        throw_on_cancel();
        const auto started = std::chrono::steady_clock::now();
        create_peninsulas(*peninsula_jobs[job_id], config, throw_on_cancel,
            profile_peninsulas ? &peninsula_profiles[job_id] : nullptr,
            minimum_offset_reuse ? minimum_offset_reuse->slot(job_id) : nullptr);
        peninsula_job_seconds[job_id] = std::chrono::duration<double>(
            std::chrono::steady_clock::now()-started).count();
    }, 1 /* independently schedulable clipping part */);
    if (minimum_offset_reuse) {
        BOOST_LOG_TRIVIAL(info) << "Peninsula minimum-offset reuse: repeated_groups="
            << minimum_offset_reuse->repeated_groups()
            << "; repeated_groups_computed=" << minimum_offset_reuse->computed_after_join()
            << "; repeated_reads_reused=" << minimum_offset_reuse->reused_after_join();
        // No retained geometry beyond this phase and its barrier.
        minimum_offset_reuse.reset();
    }
    const double peninsula_max_seconds = peninsula_job_seconds.empty() ? 0. :
        *std::max_element(peninsula_job_seconds.begin(), peninsula_job_seconds.end());
    BOOST_LOG_TRIVIAL(info) << "Support preparation: peninsula jobs " << peninsula_jobs.size()
        << "; maximum job " << peninsula_max_seconds << " s";

    if (profile_peninsulas) {
        // Emit only metadata after the barrier. Shapes stay immutable, and no
        // extra offset, sorting, copying or contour normalization is performed.
        for (size_t i = 0; i < peninsula_profiles.size(); ++i) {
            const PeninsulaProfile &entry = peninsula_profiles[i];
            if (!entry.minimum_requested)
                continue;
            throw_on_cancel();
            const PartLinks &previous = peninsula_jobs[i]->prev_parts;
            uint64_t holes = 0;
            for (const PartLink &link : previous) {
                throw_on_cancel();
                holes += link->shape->holes.size();
            }
            BOOST_LOG_TRIVIAL(info) << "Peninsula minimum job: id=" << i
                << "; input_polygons=" << previous.size() << "; input_holes=" << holes
                << "; input_vertices=" << entry.previous_vertices
                << "; shared_slot=" << (entry.minimum_slot ? 1 : 0)
                << "; computed=" << (entry.minimum_computed ? 1 : 0)
                << "; output_polygons=" << entry.minimum_output_polygons
                << "; output_vertices=" << entry.minimum_output_vertices
                << "; request_interval_ns=" << entry.elapsed_ns[2]
                << "; compute_interval_ns=" << entry.minimum_compute_ns;
            if (entry.minimum_computed) {
                const auto &kernel = entry.minimum_kernel;
                BOOST_LOG_TRIVIAL(info) << "Peninsula offset kernel: id=" << i
                    << "; outer_ns=" << kernel.elapsed_ns[ClipperOffsetProfile::Outer]
                    << "; holes_ns=" << kernel.elapsed_ns[ClipperOffsetProfile::Holes]
                    << "; combine_ns=" << kernel.elapsed_ns[ClipperOffsetProfile::Combine]
                    << "; union_ns=" << kernel.elapsed_ns[ClipperOffsetProfile::Union]
                    << "; tree_ns=" << kernel.elapsed_ns[ClipperOffsetProfile::Tree]
                    << "; outer_calls=" << kernel.calls[ClipperOffsetProfile::Outer]
                    << "; holes_calls=" << kernel.calls[ClipperOffsetProfile::Holes]
                    << "; combine_calls=" << kernel.calls[ClipperOffsetProfile::Combine]
                    << "; union_calls=" << kernel.calls[ClipperOffsetProfile::Union]
                    << "; tree_calls=" << kernel.calls[ClipperOffsetProfile::Tree]
                    << "; hole_offsets=" << kernel.hole_offsets;
            }
        }
        PeninsulaProfile total;
        size_t completed = 0, covered = 0, tiny = 0, longest = 0;
        for (size_t i = 0; i < peninsula_profiles.size(); ++i) {
            const PeninsulaProfile &entry = peninsula_profiles[i];
            for (size_t j = 0; j < total.elapsed_ns.size(); ++j)
                total.elapsed_ns[j] += entry.elapsed_ns[j];
            total.previous_links += entry.previous_links;
            total.previous_vertices += entry.previous_vertices;
            total.current_vertices += entry.current_vertices;
            total.below_lines += entry.below_lines;
            total.candidates += entry.candidates;
            total.retained += entry.retained;
            total.annotation_queries += entry.annotation_queries;
            total.candidate_edges += entry.candidate_edges;
            completed += entry.completed;
            covered += entry.covered_return;
            tiny += entry.tiny_return;
            if (peninsula_job_seconds[i] > peninsula_job_seconds[longest])
                longest = i;
        }
        BOOST_LOG_TRIVIAL(info) << "Peninsula profile: completed=" << completed
            << "; covered=" << covered << "; tiny=" << tiny
            << "; links=" << total.previous_links << "; prev_vertices=" << total.previous_vertices
            << "; current_vertices=" << total.current_vertices << "; below_lines=" << total.below_lines
            << "; candidates=" << total.candidates << "; retained=" << total.retained
            << "; queries=" << total.annotation_queries << "; candidate_edges=" << total.candidate_edges;
        static const char *names[] = {"copy", "covered_diff", "min_offset", "overhang_diff",
            "self_offset", "peninsula_diff", "line_index", "width_intersection", "annotation"};
        for (size_t j = 0; j < total.elapsed_ns.size(); ++j)
            BOOST_LOG_TRIVIAL(info) << "Peninsula profile bucket: " << names[j]
                << "; task_elapsed_ns=" << total.elapsed_ns[j]
                << "; longest_job_ns=" << (peninsula_profiles.empty() ? 0 : peninsula_profiles[longest].elapsed_ns[j]);

        // Metadata only, after all part jobs: exact ordered pointer sequences
        // preserve multiplicity and are scoped to one immutable config/phase.
        // No geometry cache is constructed and no part geometry is modified.
        const auto reuse_started = std::chrono::steady_clock::now();
        using PreviousShapes = std::vector<const ExPolygon *>;
        struct OrderedShapesLess {
            bool operator()(const PreviousShapes &left, const PreviousShapes &right) const {
                return std::lexicographical_compare(left.begin(), left.end(), right.begin(), right.end(),
                    std::less<const ExPolygon *>{});
            }
        };
        struct ReuseEligibility {
            size_t completed = 0, minimum_offset = 0, self_offset = 0;
        };
        std::map<PreviousShapes, ReuseEligibility, OrderedShapesLess> ordered_inputs;
        for (size_t i = 0; i < peninsula_jobs.size(); ++i) {
            const PeninsulaProfile &entry = peninsula_profiles[i];
            if (!entry.completed)
                continue;
            PreviousShapes previous;
            previous.reserve(peninsula_jobs[i]->prev_parts.size());
            for (const PartLink &link : peninsula_jobs[i]->prev_parts)
                previous.push_back(link->shape);
            ReuseEligibility &eligible = ordered_inputs[std::move(previous)];
            ++eligible.completed;
            // Covered jobs execute neither offset. Tiny jobs execute minimum
            // offset but return before self offset; do not count self reuse.
            if (!entry.covered_return) {
                ++eligible.minimum_offset;
                if (!entry.tiny_return)
                    ++eligible.self_offset;
            }
        }
        size_t repeated_inputs = 0, minimum_calls = 0, minimum_groups = 0, minimum_avoidable = 0;
        size_t self_calls = 0, self_groups = 0, self_avoidable = 0;
        size_t maximum_minimum_group = 0, maximum_self_group = 0;
        for (const auto &group : ordered_inputs) {
            const ReuseEligibility &eligible = group.second;
            repeated_inputs += eligible.completed > 1;
            minimum_calls += eligible.minimum_offset;
            self_calls += eligible.self_offset;
            if (eligible.minimum_offset > 1) {
                ++minimum_groups;
                minimum_avoidable += eligible.minimum_offset - 1;
            }
            if (eligible.self_offset > 1) {
                ++self_groups;
                self_avoidable += eligible.self_offset - 1;
            }
            maximum_minimum_group = std::max(maximum_minimum_group, eligible.minimum_offset);
            maximum_self_group = std::max(maximum_self_group, eligible.self_offset);
        }
        const auto reuse_elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - reuse_started).count();
        BOOST_LOG_TRIVIAL(info) << "Peninsula ordered-input metadata: groups=" << ordered_inputs.size()
            << "; repeated_completed_groups=" << repeated_inputs
            << "; min_calls=" << minimum_calls << "; min_repeated_groups=" << minimum_groups
            << "; min_avoidable_calls=" << minimum_avoidable << "; min_max_group=" << maximum_minimum_group
            << "; self_calls=" << self_calls << "; self_repeated_groups=" << self_groups
            << "; self_avoidable_calls=" << self_avoidable << "; self_max_group=" << maximum_self_group
            << "; metadata_elapsed_ns=" << reuse_elapsed;
    }

    log_phase("peninsulas");
    // Offsetting an exactly repeated shape with the same delta is pure. Track
    // representatives only after small-part removal, when layer part vectors
    // and their shape pointers are stable. The first layer intentionally has
    // no extended shapes and must never become a representative.
    const auto offsets_started = std::chrono::steady_clock::now();
    std::vector<std::vector<LayerPart *>> offset_sources(result.layers.size());
    size_t reused_offsets = 0, total_offsets = 0;
    for (size_t layer_id = 1; layer_id < result.layers.size(); ++layer_id) {
        throw_on_cancel();
        auto &parts = result.layers[layer_id].parts;
        auto &sources = offset_sources[layer_id];
        sources.reserve(parts.size());
        for (size_t part_id = 0; part_id < parts.size(); ++part_id) {
            throw_on_cancel();
            LayerPart *source = &parts[part_id];
            if (layer_id > 1) {
                const auto &previous = result.layers[layer_id - 1].parts;
                // Full ordered contour and hole equality; bounds or matching
                // outer contours alone cannot establish an identical offset.
                if (part_id < previous.size() && *source->shape == *previous[part_id].shape) {
                    source = offset_sources[layer_id - 1][part_id];
                    ++reused_offsets;
                }
            }
            sources.push_back(source);
            ++total_offsets;
        }
    }
    const auto offsets_planned = std::chrono::steady_clock::now();
    // Compute each representative once. A separate synchronous phase below
    // copies completed outputs, so no worker reads a concurrently written shape.
    execution::for_each(ex_tbb, size_t(1), result.layers.size(),
    [&layers = result.layers, &offset_sources, delta = config.removing_delta, throw_on_cancel](size_t layer_id) {
        throw_on_cancel();
        auto &parts = layers[layer_id].parts;
        for (size_t part_id = 0; part_id < parts.size(); ++part_id) {
            throw_on_cancel();
            auto &part = parts[part_id];
            if (offset_sources[layer_id][part_id] != &part)
                continue;
            part.extend_shape = offset_ex(*part.shape, delta, ClipperLib::jtSquare);
            throw_on_cancel();
        }
    }, 1 /* independently schedulable clipping layer */);
    const auto offsets_computed = std::chrono::steady_clock::now();
    execution::for_each(ex_tbb, size_t(1), result.layers.size(),
    [&layers = result.layers, &offset_sources, throw_on_cancel](size_t layer_id) {
        throw_on_cancel();
        auto &parts = layers[layer_id].parts;
        for (size_t part_id = 0; part_id < parts.size(); ++part_id) {
            throw_on_cancel();
            auto &part = parts[part_id];
            const auto *source = offset_sources[layer_id][part_id];
            if (source != &part)
                part.extend_shape = source->extend_shape;
        }
    }, 1);
    const auto offsets_copied = std::chrono::steady_clock::now();
    BOOST_LOG_TRIVIAL(info) << "Support preparation: reused extended offsets "
        << reused_offsets << "/" << total_offsets
        << "; planning " << std::chrono::duration<double>(offsets_planned-offsets_started).count()
        << " s; compute " << std::chrono::duration<double>(offsets_computed-offsets_planned).count()
        << " s; copy " << std::chrono::duration<double>(offsets_copied-offsets_computed).count() << " s";
    log_phase("extended shapes");
    return result;
}

#ifdef USE_ISLAND_GUI_FOR_SETTINGS
#include "libslic3r/NSVGUtils.hpp"
#include "libslic3r/Utils.hpp"
std::vector<Vec2f> load_curve_from_file() {
    std::string filePath = Slic3r::resources_dir() + "/data/sla_support.svg";
    EmbossShape::SvgFile svg_file{filePath};
    NSVGimage *image = init_image(svg_file);
    if (image == nullptr) {
        // In test is not known resource_dir!
        // File is not located soo return DEFAULT permanent radius 5mm is returned
        return {Vec2f{5.f,.0f}, Vec2f{5.f, 1.f}};
    }
    for (NSVGshape *shape_ptr = image->shapes; shape_ptr != NULL; shape_ptr = shape_ptr->next) {
        const NSVGshape &shape = *shape_ptr;
        if (!(shape.flags & NSVG_FLAGS_VISIBLE)) continue; // is visible
        if (shape.fill.type != NSVG_PAINT_NONE) continue; // is not used fill
        if (shape.stroke.type == NSVG_PAINT_NONE) continue; // exist stroke
        if (shape.strokeWidth < 1e-5f) continue; // is visible stroke width
        if (shape.stroke.color != 4278190261) continue; // is red

        // use only first path
        const NSVGpath *path = shape.paths;
        size_t count_points = path->npts;
        assert(count_points > 1);
        --count_points;
        std::vector<Vec2f> points;
        points.reserve(count_points/3+1);
        points.push_back({path->pts[0], path->pts[1]});
        for (size_t i = 0; i < count_points; i += 3) {
            const float *p = &path->pts[i * 2];
            points.push_back({p[6], p[7]});
        }
        assert(points.size() >= 2);
        return points;
    }

    // red curve line is not found
    assert(false);
    return {};
}
#endif // USE_ISLAND_GUI_FOR_SETTINGS

// Processing permanent support points
// Permanent are manualy edited points by user
namespace {
size_t get_index_of_closest_part(const Point &coor, const LayerParts &parts, double max_allowed_distance_sq) {
    size_t count_lines = 0;
    std::vector<size_t> part_lines_ends;
    part_lines_ends.reserve(parts.size());
    for (const LayerPart &part : parts) {
        count_lines += count_points(*part.shape);
        part_lines_ends.push_back(count_lines);
    }
    Linesf lines;
    lines.reserve(count_lines);
    for (const LayerPart &part : parts)
        append(lines, to_linesf({*part.shape}));
    AABBTreeIndirect::Tree<2, double> tree = 
        AABBTreeLines::build_aabb_tree_over_indexed_lines(lines);

    size_t line_idx = std::numeric_limits<size_t>::max();
    Vec2d coor_d = coor.cast<double>();
    Vec2d hit_point;
    [[maybe_unused]] double distance_sq =
        AABBTreeLines::squared_distance_to_indexed_lines(lines, tree, coor_d, line_idx, hit_point);
    
    if (distance_sq >= max_allowed_distance_sq) // point is farer than 1mm from any layer part
        return parts.size(); // this support point should not be used any more

    // Find part index of closest line
    for (size_t part_index = 0; part_index < part_lines_ends.size(); ++part_index)
        if (line_idx < part_lines_ends[part_index]) {
            // check point lais inside prev or next part shape
            // When assert appear check that part index is really the correct one
            assert(union_ex(
                get_shapes(parts[part_index].prev_parts),
                get_shapes(parts[part_index].next_parts))[0].contains(coor));
            return part_index;
        }
    
    assert(false); // not found
    return parts.size();
}

/// <summary>
/// Guess range of layers by its centers
/// NOTE: not valid range for variable layer height but divide space
/// </summary>
/// <param name="layers"></param>
/// <param name="layer_id"></param>
/// <returns>Range of layers</returns>
MinMax<float> get_layer_range(const Layers &layers, size_t layer_id) {
    assert(layer_id < layers.size());
    if (layer_id >= layers.size())
        return MinMax<float>{0., 0.};

    float print_z = layers[layer_id].print_z;
    float min = (layer_id == 0) ? 0.f : (layers[layer_id - 1].print_z + print_z) / 2.f;
    float max = ((layer_id + 1) < layers.size()) ?
        (layers[layer_id + 1].print_z + print_z) / 2.f :
        print_z + (print_z - min); // last layer guess height by prev layer center
    return MinMax<float>{min, max};
}

size_t get_index_of_layer_part(const Point& coor, const LayerParts& parts, double max_allowed_distance_sq) {
    size_t part_index = parts.size();
    // find part for support point
    for (const LayerPart &part : parts) {
        if (part.shape_extent.contains(coor) && part.shape->contains(coor)) {
            // parts do not overlap each other
            assert(part_index >= parts.size());
            part_index = &part - &parts.front();
        }
    }
    if (part_index >= parts.size()) { // support point is not in any part
        part_index = get_index_of_closest_part(coor, parts, max_allowed_distance_sq);
        // if (part_index >= parts.size()) // support is too far from any part
    }
    return part_index;
}

LayerParts::const_iterator get_closest_part(const PartLinks &links, Vec2d &coor) {
    if (links.size() == 1)
        return links.front();

    Point coor_p = coor.cast<coord_t>();
    // Note: layer part MUST not overlap each other
    for (const PartLink &link : links) {
        LayerParts::const_iterator part_it = link;
        if (part_it->shape_extent.contains(coor_p) && 
            part_it->shape->contains(coor_p)) {
            return part_it;
        }
    }

    size_t count_lines = 0;
    std::vector<size_t> part_lines_ends;
    part_lines_ends.reserve(links.size());
    for (const PartLink &link: links) {
        count_lines += count_points(*link->shape);
        part_lines_ends.push_back(count_lines);
    }
    Linesf lines;
    lines.reserve(count_lines);
    for (const PartLink &link : links)
        append(lines, to_linesf({*link->shape}));
    AABBTreeIndirect::Tree<2, double> tree = 
        AABBTreeLines::build_aabb_tree_over_indexed_lines(lines);

    size_t line_idx = std::numeric_limits<size_t>::max();
    Vec2d hit_point;
    [[maybe_unused]] double distance_sq =
        AABBTreeLines::squared_distance_to_indexed_lines(lines, tree, coor, line_idx, hit_point);
    
    // Find part index of closest line
    for (size_t part_index = 0; part_index < part_lines_ends.size(); ++part_index) {
        if (line_idx >= part_lines_ends[part_index])
            continue;
        
        // check point lais inside prev or next part shape
        // When assert appear check that part index is really the correct one
        assert(union_ex(
            get_shapes(links[part_index]->prev_parts),
            get_shapes(links[part_index]->next_parts))[0].contains(coor.cast<coord_t>()));
        coor = hit_point; // update closest point
        return links[part_index];        
    }
    
    assert(false); // not found
    return links.front();
}

struct PartId {
    // index into layers
    size_t layer_id;
    // index into parts of the previously addresed layer.
    size_t part_id;
};

/// <summary>
/// Dive into previous layers a trace influence over layer parts before support point
/// </summary>
/// <param name="part_id">Index of part that point will appear</param>
/// <param name="layer_id">Index of layer where point will appear</param>
/// <param name="p">Permanent support point</param>
/// <param name="layers">All layers</param>
/// <param name="config"></param>
/// <returns>First influence: Layer_index + Part_index</returns>
PartId get_index_of_first_influence(
    const PartId& partid,
    const SupportPoint &p,
    const Point& coor,
    const Layers &layers,
    const SupportPointGeneratorConfig &config) {
    // find layer part for support
    
    float max_influence_distance = std::max(
        2 * p.head_front_radius, 
        config.support_curve.front().x());

    const LayerParts& parts = layers[partid.layer_id].parts;
    LayerParts::const_iterator current_part_it = parts.cbegin() + partid.part_id;
    LayerParts::const_iterator prev_part_it = current_part_it; // stop influence just before island
    Vec2d coor_d = coor.cast<double>();

    auto get_part_id = [&layers](size_t layer_index, const LayerParts::const_iterator& part_it) {
        const LayerParts &parts = layers[layer_index].parts;
        size_t part_index = part_it - parts.cbegin();
        assert(part_index < parts.size());
        return PartId{layer_index, part_index};
    };
    // Detect not propagate into island
    // Island supports has different behavior
    // p.pos.z() - p.head_front_radius >= layer.print_z
    for (size_t i = 0; i <= partid.layer_id; ++i) {
        size_t current_layer_id = partid.layer_id - i;
        const Layer &layer = layers[current_layer_id];
        float z_distance = p.pos.z() - layer.print_z;
        if (z_distance >= max_influence_distance)
            return get_part_id(current_layer_id, current_part_it); // above layer index
        
        const PartLinks &prev_parts = current_part_it->prev_parts;
        if (prev_parts.empty()){
            // Island support
            return (z_distance < p.head_front_radius) ?
                get_part_id(current_layer_id, current_part_it) :
                get_part_id(current_layer_id + 1, prev_part_it);
        }

        prev_part_it = current_part_it;
        current_part_it = get_closest_part(prev_parts, coor_d);
    }

    // It is unreachable!
    // The first layer is always island
    assert(false);
    return PartId{std::numeric_limits<size_t>::max(), std::numeric_limits<size_t>::max()};
}

struct PermanentSupport {
    SupportPoints::const_iterator point_it; // reference to permanent

    // Define wheere layer part when start influene support area
    // When part is island also affect distribution of supports on island
    PartId influence;

    // Position of support point in layer
    PartId part;
    Point layer_position;
};
using PermanentSupports = std::vector<PermanentSupport>;

/// <summary>
/// Prepare permanent supports for layer's parts
/// </summary>
/// <param name="permanent_supports">Permanent supports</param>
/// <param name="layers">Define heights of layers</param>
/// <param name="config">Define how to propagate to previous layers</param>
/// <returns>Supoorts to add into layer parts</returns>
PermanentSupports prepare_permanent_supports(
    const SupportPoints &permanent_supports,
    const Layers &layers,
    const SupportPointGeneratorConfig &config
) {
    // How to propagate permanent support position into previous layers? and how deep? requirements
    // are chained. IMHO it should start togetjer from islands and permanent than propagate over surface

    if (permanent_supports.empty())
        return {};

    // permanent supports MUST be sorted by z
    assert(std::is_sorted(permanent_supports.begin(), permanent_supports.end(),
        [](const SupportPoint &a, const SupportPoint &b) { return a.pos.z() < b.pos.z(); }));

    size_t permanent_index = 0;
    PermanentSupports result;
    for (size_t layer_id = 0; layer_id < layers.size(); ++layer_id) {
        float layer_max_z = get_layer_range(layers, layer_id).max;
        if (permanent_index >= permanent_supports.size())
            break; // no more permanent supports
            
        if (permanent_supports[permanent_index].pos.z() >= layer_max_z)
            continue; // no permanent support for this layer
            
        const Layer &layer = layers[layer_id];
        for (; permanent_index < permanent_supports.size(); ++permanent_index) {
            SupportPoints::const_iterator point_it = permanent_supports.begin()+permanent_index;
            if (point_it->pos.z() > layer_max_z)
                // support point belongs to another layer
                // Points are sorted by z
                break;

            // find layer part for support
            Point coor(static_cast<coord_t>(scale_(point_it->pos.x())),
                       static_cast<coord_t>(scale_(point_it->pos.y())));

            double allowed_distance_sq = std::max(config.max_allowed_distance_sq,
                sqr(scale_(point_it->head_front_radius)));            
            size_t part_index = get_index_of_layer_part(coor, layer.parts, allowed_distance_sq);
            if (part_index >= layer.parts.size()) 
                continue; // support point is not in any part
            
            PartId part_id{layer_id, part_index};
            // find part of first inlfuenced layer and part for this support point
            PartId influence = get_index_of_first_influence(part_id, *point_it, coor, layers, config);
            result.push_back(PermanentSupport{point_it, influence, part_id, coor});
        }
    }

    // sort by layer index and part index
    std::sort(result.begin(), result.end(), [](const PermanentSupport& s1, const PermanentSupport& s2) {
        return s1.influence.layer_id != s2.influence.layer_id ?
            s1.influence.layer_id < s2.influence.layer_id :
            s1.influence.part_id < s2.influence.part_id; });

    return result;
}

bool exist_permanent_support(const PermanentSupports& supports, size_t current_support_index, 
    size_t layer_index, size_t part_index) {
    if (current_support_index >= supports.size())
        return false;

    const PartId &influence = supports[current_support_index].influence;
    assert(influence.layer_id >= layer_index);
    return influence.layer_id == layer_index && 
           influence.part_id == part_index;
}

/// <summary>
/// copy permanent supports into near points 
/// which has influence into current layer part
/// </summary>
/// <param name="near_points">OUTPUT for all permanent supports for this layer and part</param>
/// <param name="supports">source for Copy</param>
/// <param name="support_index">current index into supports</param>
/// <param name="print_z">current layer index</param>
/// <param name="layer_index">current layer index</param>
/// <param name="part_index">current part index</param>
void copy_permanent_supports(NearPoints& near_points, const PermanentSupports& supports, size_t& support_index, 
    float print_z, size_t layer_index, size_t part_index, const SupportPointGeneratorConfig &config) {
    while (exist_permanent_support(supports, support_index, layer_index, part_index)) {
        const PermanentSupport &support = supports[support_index];
        near_points.add(LayerSupportPoint{
            /* SupportPoint */       *support.point_it,
            /* position_on_layer */  support.layer_position, 
            /* radius_curve_index */ 0, // before support point - earlier influence on point distribution
            /* current_radius */     calc_influence_radius(fabs(support.point_it->pos.z() - print_z), config),
            /* active_in_part */ true,
            /* is_permanent */ true
        });

        // NOTE: increment index globaly
        ++support_index;
    }
}

Points get_permanents(const PermanentSupports &supports, size_t support_index, 
    size_t layer_index, size_t part_index) {
    Points result;
    while (exist_permanent_support(supports, support_index, layer_index, part_index)) {
        result.push_back(supports[support_index].layer_position); // copy
        ++support_index; // only local(temporary) increment
    }
    return result;
}

} // namespace

namespace Slic3r::sla {
using namespace Slic3r;

std::vector<Vec2f> create_default_support_curve(){
#ifdef USE_ISLAND_GUI_FOR_SETTINGS
    return {};
#else  // USE_ISLAND_GUI_FOR_SETTINGS
    return std::vector<Vec2f>{
        Vec2f{3.2f, 0.f},
        Vec2f{4.f, 3.9f},
        Vec2f{5.f, 15.f},
        Vec2f{6.f, 40.f},
    };
#endif // USE_ISLAND_GUI_FOR_SETTINGS
}

SampleConfig create_default_island_configuration(float head_diameter_in_mm) {
    return SampleConfigFactory::create(head_diameter_in_mm);
}

LayerSupportPoints generate_support_points(
    const SupportPointGeneratorData &data,
    const SupportPointGeneratorConfig &config,
    ThrowOnCancel throw_on_cancel,
    StatusFunction statusfn
) {
    const Layers &layers = data.layers;
    double increment = 100.0 / static_cast<double>(layers.size());
    double status = 0; // current progress
    int status_int = 0;
#ifdef USE_ISLAND_GUI_FOR_SETTINGS
    // Hack to set curve for testing
    if (config.support_curve.empty())
        const_cast<SupportPointGeneratorConfig &>(config).support_curve = load_curve_from_file();
#endif // USE_ISLAND_GUI_FOR_SETTINGS

    // Maximal radius of supported area of one support point
    double max_support_radius = config.support_curve.back().x();
    // check distance to nearest support points from grid
    coord_t maximal_radius = static_cast<coord_t>(scale_(max_support_radius));

    // Storage for support points used by grid
    LayerSupportPoints result;

    // Index into data.permanent_supports
    size_t permanent_index = 0;
    PermanentSupports permanent_supports =
        prepare_permanent_supports(data.permanent_supports, layers, config);

    // grid index == part in layer index
    NearPointss prev_grids; // same count as previous layer item size
    for (size_t layer_id = 0; layer_id < layers.size(); ++layer_id) {
        const Layer &layer = layers[layer_id];
        prepare_supports_for_layer(result, layer.print_z, prev_grids, config);

        // grid index == part in layer index
        NearPointss grids;
        grids.reserve(layer.parts.size());

        for (const LayerPart &part : layer.parts) {
            size_t part_id = &part - &layer.parts.front();
            if (part.prev_parts.empty()) {   // Island ?
                grids.emplace_back(&result); // only island add new grid
                Points permanent =
                    get_permanents(permanent_supports, permanent_index, layer_id, part_id);
                support_island(part, grids.back(), layer.print_z, permanent, config, throw_on_cancel);
                copy_permanent_supports(
                    grids.back(), permanent_supports, permanent_index, layer.print_z, layer_id,
                    part_id, config
                );
                continue;
            }

            // first layer should have empty prev_part
            assert(layer_id != 0);
            const LayerParts &prev_layer_parts = layers[layer_id - 1].parts;
            NearPoints near_points = create_near_points(prev_layer_parts, part, prev_grids);
            remove_supports_out_of_part(near_points, part, layer.print_z);
            assert(!near_points.get_indices().empty());
            if (!part.peninsulas.empty()) {
                // only get copy of points do not modify permanent_index
                Points permanent =
                    get_permanents(permanent_supports, permanent_index, layer_id, part_id);
                support_peninsulas(part.peninsulas, near_points, layer.print_z, permanent, config, throw_on_cancel);
            }
            copy_permanent_supports(
                near_points, permanent_supports, permanent_index, layer.print_z, layer_id, part_id,
                config
            );
            support_part_overhangs(part, config, near_points, layer.print_z, maximal_radius);
            grids.push_back(std::move(near_points));
        }
        prev_grids = std::move(grids);

        throw_on_cancel();

        int old_status_int = status_int;
        status += increment;
        status_int = static_cast<int>(std::round(status));
        if (old_status_int < status_int)
            statusfn(status_int);
    }
    // Remove permanent supports from result
    // To preserve permanent 3d position it is necessary to append points after move_on_mesh_surface
    result.erase(
        std::remove_if(
            result.begin(), result.end(), [](const LayerSupportPoint &p) { return p.is_permanent; }
        ),
        result.end()
    );
    return result;
}

SupportPoints move_on_mesh_surface(
    const LayerSupportPoints &points,
    const AABBMesh &mesh,
    double allowed_move,
    ThrowOnCancel throw_on_cancel
) {
    SupportPoints pts;
    pts.reserve(points.size());
    for (const LayerSupportPoint &p : points)
        pts.push_back(static_cast<SupportPoint>(p));

    // The function  makes sure that all the points are really exactly placed on the mesh.
    execution::for_each(
        ex_tbb, size_t(0), pts.size(),
        [&pts, &mesh, &throw_on_cancel, allowed_move](size_t idx) {
            if ((idx % 16) == 0)
                // Don't call the following function too often as it flushes CPU write caches due to
                // synchronization primitves.
                throw_on_cancel();

            Vec3f &p = pts[idx].pos;
            Vec3d p_double = p.cast<double>();
            const Vec3d up_vec(0., 0., 1.);
            const Vec3d down_vec(0., 0., -1.);
            // Project the point upward and downward and choose the closer intersection with the mesh.
            AABBMesh::hit_result hit_up = mesh.query_ray_hit(p_double, up_vec);
            AABBMesh::hit_result hit_down = mesh.query_ray_hit(p_double, down_vec);

            bool up = hit_up.is_hit();
            bool down = hit_down.is_hit();
            // no hit means support points lay exactly on triangle surface
            if (!up && !down)
                return;

            AABBMesh::hit_result &hit = (!down || hit_up.distance() < hit_down.distance()) ?
                hit_up :
                hit_down;
            if (hit.distance() <= allowed_move) {
                p[2] += static_cast<float>(hit.distance() * hit.direction()[2]);
                return;
            }

            // big distance means that ray fly over triangle side (space between triangles)
            int triangle_index;
            Vec3d closest_point;
            double distance = mesh.squared_distance(p_double, triangle_index, closest_point);
            if (distance <= std::numeric_limits<float>::epsilon())
                return; // correct coordinate
            p = closest_point.cast<float>();
        },
        64 /* gransize */
    );
    return pts;
}

} // namespace Slic3r::sla
