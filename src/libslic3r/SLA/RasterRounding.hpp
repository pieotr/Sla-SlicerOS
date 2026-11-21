// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <cfenv>
#include "libslic3r/Exception.hpp"

namespace Slic3r::sla {
// Geometry libraries can change SSE rounding independently of x87. A single
// fegetround() value cannot describe that mixed state. Always set nearest for
// rendering and restore the complete caller environment afterward.
class ScopedNearestRounding {
    std::fenv_t m_previous;
public:
    ScopedNearestRounding() {
        if (std::fegetenv(&m_previous) != 0)
            throw RuntimeError("Cannot save raster floating-point environment");
        if (std::fesetround(FE_TONEAREST) != 0) {
            std::fesetenv(&m_previous);
            throw RuntimeError("Cannot set raster floating-point rounding");
        }
    }
    ~ScopedNearestRounding() { std::fesetenv(&m_previous); }
    ScopedNearestRounding(const ScopedNearestRounding &) = delete;
    ScopedNearestRounding &operator=(const ScopedNearestRounding &) = delete;
};
} // namespace Slic3r::sla
