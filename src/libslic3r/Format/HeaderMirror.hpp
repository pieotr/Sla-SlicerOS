#pragma once

#include "libslic3r/PrintConfig.hpp"
#include <cstdint>

namespace Slic3r {

// Mirror/flip flag stored in an SLA archive header (metadata only; the layer
// images are mirrored separately by display_mirror_x/y and never touched here).
struct HeaderMirror {
    bool x = false, y = false;
    bool any() const { return x || y; }
    // 0 none | 1 horizontal | 2 vertical | 3 both (OSLA / OSF / Klipper encoding).
    uint8_t bits() const { return uint8_t((x ? 1 : 0) | (y ? 2 : 0)); }
};

// `auto_x/auto_y` is the value the format has always written (a constant or
// display_mirror_*); sla_archive_header_mirror = auto keeps it exactly.
inline HeaderMirror resolve_header_mirror(const SLAPrinterConfig &cfg, bool auto_x, bool auto_y)
{
    switch (cfg.sla_archive_header_mirror.value) {
    case slahmNone:       return {false, false};
    case slahmHorizontal: return {true, false};
    case slahmVertical:   return {false, true};
    case slahmBoth:       return {true, true};
    case slahmAuto:
    default:              return {auto_x, auto_y};
    }
}

} // namespace Slic3r
