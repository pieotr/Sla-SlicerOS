// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "LayerMaskPrecision.hpp"
#include <algorithm>
#include <cstddef>
#if defined(__SSE2__)
#include <emmintrin.h>
#endif

namespace Slic3r::sla {
// Byte membership only: the caller validates the declared enum separately and
// retains PNG decoding, integrity/geometry checks and chunk cancellation. Empty
// chunks and unknown identifiers preserve project_mask_value's old behavior;
// this function must not be used as an enum-validity check.
inline bool mask_chunk_matches_precision(const uint8_t *data, std::size_t size,
                                         LayerMaskPrecision precision)
{
    if (size == 0 || precision == LayerMaskPrecision::Gray8)
        return true;
    if (precision == LayerMaskPrecision::NativeGray7)
        return std::all_of(data, data + size, [](uint8_t value) {
            return value == 0 || (value > 1 && (value & 1) != 0);
        });
    if (precision == LayerMaskPrecision::NativeGray4) {
        // Caller bounds this scan to 1 MiB between cancellation checkpoints.
        // Equal nibbles encode exactly 0,17,...,255; OR permits vectorization.
        unsigned violations = 0;
        for (std::size_t index = 0; index < size; ++index) {
            const auto value = data[index];
            violations |= unsigned((value >> 4) ^ (value & 15));
        }
        return violations == 0;
    }
    return std::all_of(data, data + size, [precision](uint8_t value) {
        return project_mask_value(value, precision) == value;
    });
}
struct MaskChunkStatistics {
    bool valid;
    std::size_t nonzero_pixels; // Complete only when valid is true.
};
// Share the zero comparison between palette validation and pixel counting.
// SSE2 accumulates in 64-bit lanes; the portable path bounds its 32-bit sums
// to 1 MiB. The caller owns PNG integrity/geometry checks and cancellation.
inline MaskChunkStatistics validate_and_count_mask_chunk(const uint8_t *data,
    std::size_t size, LayerMaskPrecision precision)
{
    std::size_t total = 0;
#if defined(__SSE2__)
    if (precision == LayerMaskPrecision::NativeGray7 || precision == LayerMaskPrecision::NativeGray4 ||
        precision == LayerMaskPrecision::Gray8) {
        const auto zero = _mm_setzero_si128();
        const auto ones = _mm_set1_epi8(1);
        const auto nibbles = _mm_set1_epi8(15);
        auto violations = zero;
        auto counts = zero;
        std::size_t offset = 0;
        for (; size - offset >= 16; offset += 16) {
            const auto values = _mm_loadu_si128(reinterpret_cast<const __m128i *>(data + offset));
            const auto zeros = _mm_cmpeq_epi8(values, zero);
            counts = _mm_add_epi64(counts, _mm_sad_epu8(_mm_andnot_si128(zeros, ones), zero));
            if (precision == LayerMaskPrecision::NativeGray7) {
                const auto even = _mm_cmpeq_epi8(_mm_and_si128(values, ones), zero);
                const auto invalid = _mm_andnot_si128(zeros, _mm_or_si128(even, _mm_cmpeq_epi8(values, ones)));
                violations = _mm_or_si128(violations, invalid);
            } else if (precision == LayerMaskPrecision::NativeGray4) {
                const auto difference = _mm_xor_si128(_mm_and_si128(values, nibbles),
                    _mm_and_si128(_mm_srli_epi16(values, 4), nibbles));
                violations = _mm_or_si128(violations, difference);
            }
        }
        if (_mm_movemask_epi8(_mm_cmpeq_epi8(violations, zero)) != 0xffff) return {false, 0};
        alignas(16) uint64_t sums[2];
        _mm_store_si128(reinterpret_cast<__m128i *>(sums), counts);
        total = std::size_t(sums[0] + sums[1]);
        for (; offset < size; ++offset) {
            const auto value = data[offset];
            if (project_mask_value(value, precision) != value) return {false, total};
            total += value != 0;
        }
        return {true, total};
    }
#endif
    for (std::size_t offset = 0; offset < size;) {
        const std::size_t end = offset + std::min(size - offset, std::size_t(1) << 20);
        uint32_t nonzero = 0, violations = 0;
        if (precision == LayerMaskPrecision::NativeGray7) {
            for (std::size_t i = offset; i < end; ++i) {
                const auto value = data[i];
                nonzero += uint32_t(value != 0);
                violations |= uint32_t(value == 1) | (uint32_t(value != 0) & uint32_t((value & 1) == 0));
            }
        } else if (precision == LayerMaskPrecision::NativeGray4) {
            for (std::size_t i = offset; i < end; ++i) {
                const auto value = data[i];
                nonzero += uint32_t(value != 0);
                violations |= uint32_t((value >> 4) ^ (value & 15));
            }
        } else if (precision == LayerMaskPrecision::Gray8) {
            for (std::size_t i = offset; i < end; ++i) nonzero += uint32_t(data[i] != 0);
        } else {
            uint8_t invalid[256];
            for (unsigned value = 0; value < 256; ++value)
                invalid[value] = uint8_t(project_mask_value(uint8_t(value), precision) != value);
            for (std::size_t i = offset; i < end; ++i) {
                const auto value = data[i];
                nonzero += uint32_t(value != 0);
                violations |= invalid[value];
            }
        }
        total += nonzero;
        if (violations) return {false, total};
        offset = end;
    }
    return {true, total};
}
} // namespace Slic3r::sla
