#pragma once

#include "libslic3r/Exception.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstddef>

namespace Slic3r::sla {

struct CXDLPv4GeneratedCounts {
    uint32_t bottom;
    uint32_t transition;
};

// Generated metadata follows the reference's public exposure view. The caller
// supplies finalized native rows; this projection never rewrites their exposure
// schedule or the source preset. Imported native snapshots retain raw counts.
template<class ExposureAt>
CXDLPv4GeneratedCounts cxdlpv4_generated_counts(size_t layers, int bottom,
                                               int transition, ExposureAt exposure_at)
{
    if (layers == 0 || layers > 100000 || bottom < 0 || transition < 0)
        throw ExportError("Invalid CXDLPv4 layer, bottom or transition count");
    const size_t effective_bottom = std::min(size_t(bottom), layers);
    if (effective_bottom > UINT16_MAX)
        throw ExportError("CXDLPv4 bottom count exceeds the reference uint16 range");
    const size_t maximum = effective_bottom == 0 || layers <= effective_bottom + 1
        ? 0 : layers - effective_bottom - 1;
    if (maximum == 0)
        return {uint32_t(effective_bottom), 0};

    auto public_exposure = [&](size_t i) {
        const float native = float(exposure_at(i));
        const float rounded = std::nearbyint(native * 100.f) / 100.f;
        if (!std::isfinite(rounded) || rounded < 0)
            throw ExportError("Invalid CXDLPv4 transition exposure");
        return rounded;
    };
    size_t parsed = 0;
    float left = public_exposure(effective_bottom);
    for (size_t i = effective_bottom; i + 1 < layers; ++i) {
        const float right = public_exposure(i + 1);
        if (left < right || std::abs(left - right) < .009f)
            break;
        if (++parsed > UINT16_MAX)
            throw ExportError("CXDLPv4 transition run exceeds the reference uint16 range");
        left = right;
    }
    const size_t effective_transition = std::min(parsed ? parsed : size_t(transition), maximum);
    if (effective_transition > UINT16_MAX)
        throw ExportError("CXDLPv4 transition count exceeds the reference uint16 range");
    return {uint32_t(effective_bottom), uint32_t(effective_transition)};
}

} // namespace Slic3r::sla
