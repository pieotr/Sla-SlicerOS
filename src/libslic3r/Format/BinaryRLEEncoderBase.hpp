///|/ Copyright (c) Prusa Research 2023 Tomáš Mészáros @tamasmeszaros, Pavel Mikuš @Godrak
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/

#pragma once

#include <vector>
#include <cstdint>
#include <memory>
#include "libslic3r/SLA/RasterBase.hpp"

namespace Slic3r::sla {

// Base RLE encoder for binary format layers
// Works with raw pixel buffers (as received from RasterEncoder pattern)
class BinaryRLEEncoderBase {
public:
    // RLE encoding parameters
    struct RLEParams {
        uint8_t rle_byte;           // Byte that signals RLE run
        uint32_t rle_limit;         // Max run length (e.g., 0x7D for Chitubox)
        bool two_byte_runs;         // Use 2-byte encoding for runs > 255
        uint8_t bit_depth;          // Bits per pixel (1, 4, 7, 8)
    };
    
    virtual ~BinaryRLEEncoderBase() = default;
    
    // Encode a single scanline with RLE
    virtual std::vector<uint8_t> encode_scanline(const uint8_t *data, 
                                                   size_t width, 
                                                   const RLEParams &params) const;
    
    // Encode raw pixel buffer with RLE
    virtual EncodedRaster encode_raw_data(const uint8_t *ptr, 
                                          size_t w, 
                                          size_t h,
                                          size_t num_components,
                                          const RLEParams &params,
                                          const std::string &format_ext) const;
};

// Chitubox RLE encoder (8-bit pixels, RLE limit 0x7D)
struct ChituboxRLEEncoder {
    static constexpr uint8_t RLE_BYTE = 0x00;
    static constexpr uint32_t RLE_LIMIT = 0x7D;  // 125 pixels max
    
    EncodedRaster operator()(const void *ptr,
                            size_t w,
                            size_t h,
                            size_t num_components);
};

// FDG RLE encoder (7-bit pixels with special boundary handling)
// Uses transformation: gray7_value = (gray8_value >> 1) | (gray8_value & 1)
struct FDGRLEEncoder {
    static constexpr uint8_t RLE_BYTE = 0x00;
    static constexpr uint32_t RLE_LIMIT = 0x7FFF; // 32K pixels max
    
    // Convert 8-bit gray to 7-bit FDG format
    static uint8_t gray8_to_gray7(uint8_t gray8) {
        return (gray8 >> 1) | (gray8 & 1);
    }
    
    // FDG has special handling at halfway point (width/2)
    EncodedRaster operator()(const void *ptr,
                            size_t w,
                            size_t h,
                            size_t num_components);
};

// CTB RLE encoder (variant of Chitubox with different structure)
struct CTBRLEEncoder {
    static constexpr uint32_t RLE_LIMIT = 0xFFFF;  // 64K pixels max
    
    EncodedRaster operator()(const void *ptr,
                            size_t w,
                            size_t h,
                            size_t num_components);
};

// Anycubic RLE encoder (uses 16-bit RLE with CRC-16)
struct AnycubicRLEEncoder {
    static constexpr uint16_t RLE_BYTE = 0x0000;
    static constexpr uint32_t RLE_LIMIT = 0xFFFF;  // 64K pixels max
    
    // Anycubic uses 16-bit encoding
    EncodedRaster operator()(const void *ptr,
                            size_t w,
                            size_t h,
                            size_t num_components);
    
    static uint16_t crc16_ansi(const std::vector<uint8_t> &data);
};

} // namespace Slic3r::sla
