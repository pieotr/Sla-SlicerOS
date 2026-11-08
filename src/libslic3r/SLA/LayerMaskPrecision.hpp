// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <cstdint>
#include <algorithm>
#include <stdexcept>

namespace Slic3r::sla {
// Source precision survives target changes. These stable identifiers describe
// an exact, idempotent palette, not the target currently selected for export.
enum class LayerMaskPrecision { Gray8, NativeGray7, NativeBitplanes1=2, NativeBitplanes16=17, NativeGray4=18 };
inline unsigned mask_bitplanes(LayerMaskPrecision precision)
{
    const int id=int(precision);
    return id>=2 && id<=17 ? unsigned(id-1) : 0;
}
inline LayerMaskPrecision bitplane_mask_precision(unsigned planes)
{
    if(!planes || planes>16)throw std::invalid_argument("Mask bitplanes must be from 1 to 16");
    return LayerMaskPrecision(planes+1);
}
// Canonical levels are planeCount*floor(256/AA)-1. Non-power AA therefore
// has a maximum below255. Keep those levels stable through editing/restart.
inline unsigned mask_plane_count(uint8_t value,unsigned planes)
{
    const unsigned step=256/planes;
    return value ? std::min(planes,(unsigned(value)+1)/step) : 0;
}
inline uint8_t project_mask_value(uint8_t value, LayerMaskPrecision precision)
{
    if(precision==LayerMaskPrecision::NativeGray4)return uint8_t((value>>4)*17);
    if(const unsigned planes=mask_bitplanes(precision)) {
        const unsigned count=mask_plane_count(value,planes);
        return count?uint8_t(count*(256/planes)-1):0;
    }
    return precision == LayerMaskPrecision::NativeGray7
        ? (value < 2 ? uint8_t(0) : uint8_t((value & 254) | 1)) : value;
}
} // namespace Slic3r::sla
