///|/ Copyright (c) Prusa Research 2023 Tomáš Mészáros @tamasmeszaros, Pavel Mikuš @Godrak
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "BinaryRLEEncoderBase.hpp"
#include <cassert>
#include <cstring>

namespace Slic3r::sla {

// Base RLE encoding - generic 8-bit RLE
std::vector<uint8_t> BinaryRLEEncoderBase::encode_scanline(const uint8_t *data, 
                                                            size_t width, 
                                                            const RLEParams &params) const
{
    std::vector<uint8_t> result;
    
    if (width == 0) return result;
    
    size_t i = 0;
    while (i < width) {
        uint8_t current = data[i];
        size_t run_length = 1;
        
        // Count consecutive bytes with same value
        while (i + run_length < width && 
               data[i + run_length] == current && 
               run_length < params.rle_limit) {
            run_length++;
        }
        
        // Decide whether to encode as RLE or literal
        if (run_length > 2 || current == params.rle_byte) {
            // Use RLE encoding
            if (params.two_byte_runs && run_length > 255) {
                // Two-byte run length
                result.push_back(params.rle_byte);
                result.push_back((run_length >> 8) & 0xFF);
                result.push_back(run_length & 0xFF);
            } else {
                // One-byte run length
                result.push_back(params.rle_byte);
                result.push_back(run_length & 0xFF);
            }
            result.push_back(current);
        } else {
            // Write literal bytes
            for (size_t j = 0; j < run_length; j++) {
                result.push_back(current);
            }
        }
        
        i += run_length;
    }
    
    return result;
}

// Base raw data encoding
EncodedRaster BinaryRLEEncoderBase::encode_raw_data(const uint8_t *ptr, 
                                                    size_t w, 
                                                    size_t h,
                                                    size_t num_components,
                                                    const RLEParams &params,
                                                    const std::string &format_ext) const
{
    std::vector<uint8_t> result;
    
    size_t row_bytes = w * num_components;
    
    for (size_t y = 0; y < h; y++) {
        auto scanline = encode_scanline(ptr + y * row_bytes, w, params);
        result.insert(result.end(), scanline.begin(), scanline.end());
    }
    
    return EncodedRaster(std::move(result), format_ext);
}

// ============================================================================
// Chitubox RLE - 8-bit gray, RLE limit 0x7D (125)
// ============================================================================

EncodedRaster ChituboxRLEEncoder::operator()(const void *ptr,
                                            size_t w,
                                            size_t h,
                                            size_t num_components)
{
    std::vector<uint8_t> result;
    const uint8_t *src = reinterpret_cast<const uint8_t *>(ptr);
    size_t row_bytes = w * num_components;
    
    for (size_t y = 0; y < h; y++) {
        const uint8_t *line = src + y * row_bytes;
        
        size_t i = 0;
        while (i < w) {
            uint8_t current = line[i * num_components];
            size_t run_length = 1;
            
            // Count consecutive bytes
            while (i + run_length < w && 
                   line[(i + run_length) * num_components] == current && 
                   run_length < RLE_LIMIT) {
                run_length++;
            }
            
            // Chitubox RLE: 0x00 <count> <value> for runs, or literal bytes
            if (run_length >= 3 || current == 0x00) {
                result.push_back(0x00);              // RLE marker
                result.push_back((uint8_t)run_length);
                result.push_back(current);
            } else {
                // Write literal bytes
                for (size_t j = 0; j < run_length; j++) {
                    result.push_back(line[(i + j) * num_components]);
                }
            }
            
            i += run_length;
        }
    }
    
    return EncodedRaster(std::move(result), "ctb");
}

// ============================================================================
// CTB RLE - variant with larger run limits
// ============================================================================

EncodedRaster CTBRLEEncoder::operator()(const void *ptr,
                                       size_t w,
                                       size_t h,
                                       size_t num_components)
{
    std::vector<uint8_t> result;
    const uint8_t *src = reinterpret_cast<const uint8_t *>(ptr);
    size_t row_bytes = w * num_components;
    
    for (size_t y = 0; y < h; y++) {
        const uint8_t *line = src + y * row_bytes;
        
        size_t i = 0;
        while (i < w) {
            uint8_t current = line[i * num_components];
            size_t run_length = 1;
            
            // Count consecutive bytes
            while (i + run_length < w && 
                   line[(i + run_length) * num_components] == current && 
                   run_length < RLE_LIMIT) {
                run_length++;
            }
            
            // CTB RLE: 0x00 <low_byte> <high_byte> <value> for large runs
            if (run_length >= 3 || current == 0x00) {
                result.push_back(0x00);                    // RLE marker
                result.push_back(run_length & 0xFF);       // Low byte
                result.push_back((run_length >> 8) & 0xFF); // High byte
                result.push_back(current);
            } else {
                // Write literal bytes
                for (size_t j = 0; j < run_length; j++) {
                    result.push_back(line[(i + j) * num_components]);
                }
            }
            
            i += run_length;
        }
    }
    
    return EncodedRaster(std::move(result), "ctb");
}

// ============================================================================
// FDG RLE - 7-bit gray with halfway boundary
// ============================================================================

EncodedRaster FDGRLEEncoder::operator()(const void *ptr,
                                       size_t w,
                                       size_t h,
                                       size_t num_components)
{
    std::vector<uint8_t> result;
    const uint8_t *src = reinterpret_cast<const uint8_t *>(ptr);
    size_t row_bytes = w * num_components;
    size_t half = w / 2;
    
    for (size_t y = 0; y < h; y++) {
        const uint8_t *line = src + y * row_bytes;
        
        for (size_t x = 0; x < w; ) {
            uint8_t current = line[x * num_components];
            size_t run_length = 1;
            
            // Count consecutive bytes, respecting FDG boundary at half-width
            while (x + run_length < w && 
                   line[(x + run_length) * num_components] == current && 
                   run_length < RLE_LIMIT) {
                // Stop if next byte crosses the halfway boundary
                if (x + run_length == half) {
                    break;
                }
                run_length++;
            }
            
            // FDG uses same 0x00 <uint16_t count> <value> format
            if (run_length >= 3 || current == 0x00) {
                result.push_back(0x00);                    // RLE marker
                result.push_back(run_length & 0xFF);       // Low byte of count
                result.push_back((run_length >> 8) & 0xFF); // High byte of count
                result.push_back(gray8_to_gray7(current)); // Convert to 7-bit
            } else {
                // Write literal bytes (converted to 7-bit)
                for (size_t j = 0; j < run_length; j++) {
                    result.push_back(gray8_to_gray7(line[(x + j) * num_components]));
                }
            }
            
            x += run_length;
            
            // At boundary, reset to gray7 = 0xFF
            if (x == half) {
                result.push_back(0x00);  // RLE marker
                result.push_back(0x01);  // Count low byte = 1
                result.push_back(0x00);  // Count high byte = 0
                result.push_back(0xFF);  // Value = 0xFF
            }
        }
    }
    
    return EncodedRaster(std::move(result), "fdg");
}

// ============================================================================
// Anycubic RLE - 16-bit encoding
// ============================================================================

uint16_t AnycubicRLEEncoder::crc16_ansi(const std::vector<uint8_t> &data) {
    uint16_t crc = 0xFFFF;
    
    for (uint8_t byte : data) {
        crc ^= (uint16_t)byte << 8;
        for (int j = 0; j < 8; j++) {
            crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : (crc << 1);
            crc &= 0xFFFF;
        }
    }
    
    return crc;
}

EncodedRaster AnycubicRLEEncoder::operator()(const void *ptr,
                                            size_t w,
                                            size_t h,
                                            size_t num_components)
{
    std::vector<uint8_t> result;
    const uint8_t *src = reinterpret_cast<const uint8_t *>(ptr);
    size_t row_bytes = w * num_components;
    
    std::vector<uint8_t> layer_data;
    
    for (size_t y = 0; y < h; y++) {
        const uint8_t *line = src + y * row_bytes;
        
        size_t i = 0;
        while (i < w) {
            uint8_t current = line[i * num_components];
            size_t run_length = 1;
            
            // Count consecutive bytes
            while (i + run_length < w && 
                   line[(i + run_length) * num_components] == current && 
                   run_length < RLE_LIMIT) {
                run_length++;
            }
            
            // Anycubic 16-bit RLE: depends on whether byte is 0x00
            if (current == 0x00) {
                // Special handling for 0x00
                layer_data.push_back(0x00);
                layer_data.push_back((run_length >> 8) & 0xFF);
                layer_data.push_back(run_length & 0xFF);
            } else if (run_length > 1) {
                // Literal data followed by RLE marker
                layer_data.push_back(current);
                layer_data.push_back(run_length & 0xFF);
            } else {
                // Single literal byte
                layer_data.push_back(current);
            }
            
            i += run_length;
        }
    }
    
    // Add CRC-16-ANSI at the end
    uint16_t crc = crc16_ansi(layer_data);
    layer_data.push_back(crc & 0xFF);
    layer_data.push_back((crc >> 8) & 0xFF);
    
    return EncodedRaster(std::move(layer_data), "pwimg");
}

} // namespace Slic3r::sla
