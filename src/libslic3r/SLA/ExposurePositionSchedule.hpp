// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "libslic3r/Exception.hpp"
#include <cmath>
#include <cstddef>
#include <vector>

namespace Slic3r::sla {
// A mask is an exposure record. In calibration sequences several records may
// cure one physical layer; ordinary mesh layers must remain strictly ordered.
enum class ExposurePositionPolicy { IncreasingLayers, RepeatedExposureHeights };

class ExposurePositionValidator {
public:
    explicit ExposurePositionValidator(ExposurePositionPolicy policy = ExposurePositionPolicy::IncreasingLayers)
        : m_policy(policy) {
        if (policy != ExposurePositionPolicy::IncreasingLayers &&
            policy != ExposurePositionPolicy::RepeatedExposureHeights)
            throw RuntimeError("Unknown exposure position policy");
    }
    // Returns true for the first exposure at a new physical height.
    bool append(double position_z) {
        if (m_count >= 100000 || !std::isfinite(position_z) || position_z <= 0 || position_z < m_previous ||
            (position_z == m_previous && m_policy == ExposurePositionPolicy::IncreasingLayers))
            throw RuntimeError("Exposure positions must be finite, positive and ordered");
        const bool starts_layer = position_z > m_previous;
        ++m_count;
        m_previous = position_z;
        return starts_layer;
    }
private:
    ExposurePositionPolicy m_policy;
    size_t m_count = 0;
    double m_previous = 0;
};

struct PhysicalExposureLayer {
    size_t first_record, past_last_record;
    double position_z, thickness;
};

inline std::vector<PhysicalExposureLayer> group_exposure_positions(
    const std::vector<double> &positions,
    ExposurePositionPolicy policy = ExposurePositionPolicy::IncreasingLayers)
{
    if (positions.empty() || positions.size() > 100000)
        throw RuntimeError("An exposure schedule requires 1–100000 masks");
    ExposurePositionValidator validator(policy);
    std::vector<PhysicalExposureLayer> groups;
    double previous = 0;
    for (size_t i = 0; i < positions.size(); ++i) {
        if (validator.append(positions[i])) {
            groups.push_back({i, i + 1, positions[i], positions[i] - previous});
            previous = positions[i];
        } else groups.back().past_last_record = i + 1;
    }
    return groups;
}

// Native rounding must preserve both physical-layer boundaries and the
// advertised Z grid. Never merge or split exposure groups to make export fit.
// Streaming validation avoids allocating a second parameter table for export.
class ExposurePositionProjectionValidator {
public:
    ExposurePositionProjectionValidator(ExposurePositionPolicy policy, double quantum_mm)
        : m_source(policy), m_represented(policy), m_grid_scale(grid_scale(quantum_mm)) {}
    bool append(double source_z, double represented_z) {
        // A failed validation must not consume a record or advance a height.
        auto source = m_source, represented = m_represented;
        const bool source_start = source.append(source_z);
        if (source_start != represented.append(represented_z))
            throw RuntimeError("Native Z precision changes physical exposure groups");
        double grid = m_previous_grid;
        if (source_start) {
            const double source_grid = std::round(source_z * m_grid_scale);
            grid = std::round(represented_z * m_grid_scale);
            if (!std::isfinite(source_grid) || !std::isfinite(grid) ||
                source_grid != grid || grid <= m_previous_grid)
                throw RuntimeError("Native Z precision collapses or changes the exposure height grid");
        }
        m_source = source; m_represented = represented; m_previous_grid = grid;
        return source_start;
    }
private:
    static double grid_scale(double quantum) {
        if (!std::isfinite(quantum) || quantum <= 0 || !std::isfinite(1. / quantum))
            throw RuntimeError("Invalid native exposure position quantum");
        return 1. / quantum;
    }
    ExposurePositionValidator m_source, m_represented;
    double m_grid_scale, m_previous_grid = 0;
};

inline void require_exposure_position_projection(
    const std::vector<double> &source, const std::vector<double> &represented,
    ExposurePositionPolicy policy, double quantum_mm)
{
    if (source.empty() || source.size() != represented.size())
        throw RuntimeError("Invalid native exposure position projection");
    ExposurePositionProjectionValidator validator(policy, quantum_mm);
    for (size_t i = 0; i < source.size(); ++i) validator.append(source[i], represented[i]);
}
} // namespace Slic3r::sla
