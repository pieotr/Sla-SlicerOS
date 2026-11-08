// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "libslic3r/Exception.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace Slic3r::sla {
// Occupied area uses the same nonzero-pixel definition as ordinary SLA
// statistics. One bit per pixel bounds scratch memory to 16 MiB at 128Mpx.
class ExposureMaskUnion {
public:
    explicit ExposureMaskUnion(size_t pixels) : m_pixels(pixels) {
        if (!pixels || pixels > (size_t(128) << 20))
            throw RuntimeError("Exposure union exceeds the raster pixel budget");
        m_bits.resize((pixels + 63) / 64);
    }
    void reset() { std::fill(m_bits.begin(), m_bits.end(), 0); m_count = 0; }
    void append(const std::vector<uint8_t> &mask) {
        if (mask.size() != m_pixels) throw RuntimeError("Exposure union masks have different dimensions");
        for (size_t i = 0; i < mask.size(); ++i) if (mask[i]) {
            const uint64_t bit = uint64_t(1) << (i % 64);
            auto &word = m_bits[i / 64];
            if (!(word & bit)) { word |= bit; ++m_count; }
        }
    }
    size_t nonzero_pixels() const { return m_count; }
private:
    size_t m_pixels, m_count = 0;
    std::vector<uint64_t> m_bits;
};
} // namespace Slic3r::sla
