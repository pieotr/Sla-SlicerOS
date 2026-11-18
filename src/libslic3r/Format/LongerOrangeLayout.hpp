// SPDX-License-Identifier: AGPL-3.0-or-later
// Reader-side layout of the Longer Orange LGS family (LGS, LGS30, LGS120,
// LGS4K). Port of the UVtools LGSFile.cs decode path; the writer is
// LongerOrangeArchive.cpp. All integers and floats are little endian.
//
//   0     "Longer3D"            32  pixel/mm x, y (f32)
//   16    printer model         36  resolution x, y (f32)
//         10, 30, 120,          164 layer count
//         4000 or 4500 (4K)     172 preview width, height (RGB565, big endian)
//   180   RGB565 preview, [LGS120: u32 size + PNG + 2 padding bytes],
//         then one record per layer: u32 size + RLE
//
// RLE: every byte is one hex digit of a run length in its low nibble and the
// gray level in its high nibble; consecutive bytes with the same high nibble
// form one run, most significant digit first. A pixel is stored as its high
// nibble (gray = nibble * 17). The 4K models store the raster rotated by 90
// degrees clockwise.
//
// UVtools appends a second copy of the layer table after the last record. The
// decoder reads only "layer count" records, so those bytes are carried over
// for an unchanged save and dropped when layers are rewritten.
//
// Print parameters: masks-only by design. LGSFile.cs: LayerDef is {DataSize, EncodedRle} only; PrintParameterModifiers are global (exposure, lift,
// speeds in the header), there is no PrintParameterPerLayerModifiers.
#ifndef SLIC3R_LONGER_ORANGE_LAYOUT_HPP
#define SLIC3R_LONGER_ORANGE_LAYOUT_HPP

#include <algorithm>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "BinaryLayout.hpp"

namespace Slic3r {

class LongerOrangeLayout {
public:
    static constexpr size_t header_size = 180;
    static bool is_format(const std::string &format) { return format == "lgs" || format == "lgs30" || format == "lgs120" || format == "lgs4k"; }
    static bool recognizes(const uint8_t *first8) { return std::memcmp(first8, "Longer3D", 8) == 0; }
    // What a gray value becomes once stored: its high nibble, repeated.
    static uint8_t quantize(uint8_t value) { return uint8_t((value >> 4) * 17); }

    std::string format;
    size_t columns = 0, rows = 0, count = 0;
    uint32_t model = 0;

    explicit LongerOrangeLayout(const sla::NativeBytes &bytes, const std::function<void()> &poll = {})
        : m_bytes(bytes), m_in(bytes, "LGS")
    {
        if (bytes.size() < header_size)
            throw RuntimeError("LGS file is truncated: the header needs " + std::to_string(header_size) + " bytes");
        if (!bytes.equals(0, "Longer3D", 8)) throw RuntimeError("LGS file has the wrong magic number");
        model = m_in.u32(16);
        switch (model) {
        case 10: format = "lgs"; break;
        case 30: format = "lgs30"; break;
        case 120: format = "lgs120"; break;
        case 4000: case 4500: format = "lgs4k"; break;
        default: throw RuntimeError("LGS printer model " + std::to_string(model) + " is not supported");
        }
        // Some plug-in versions store width and height the other way round; UVtools swaps them.
        columns = binary_layout::whole_positive(m_in.f32(36), binary_layout::max_dimension, "LGS", "resolution x");
        rows = binary_layout::whole_positive(m_in.f32(40), binary_layout::max_dimension, "LGS", "resolution y");
        if (columns > rows) std::swap(columns, rows);
        binary_layout::check_raster(columns, rows, "LGS");
        count = m_in.u32(164);
        if (!count || count > binary_layout::max_layers)
            throw RuntimeError("LGS layer count is zero or exceeds the editor limit");
        const uint64_t preview_x = m_in.u32(172), preview_y = m_in.u32(176);
        if (preview_x > 4096 || preview_y > 4096) throw RuntimeError("LGS preview size is out of range");
        uint64_t at = header_size + preview_x * preview_y * 2;
        m_in.need(at, 0, "the preview");
        if (model == 120) {
            const uint64_t png = m_in.u32(at);
            at += 4;
            m_in.need(at, png + 2, "the LGS120 PNG preview");
            at += png + 2;
        }
        m_layers.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            if (poll && i % 4096 == 0) poll();
            const uint64_t size = m_in.u32(at);
            at += 4;
            m_in.need(at, size, "a layer record");
            if (!size) throw RuntimeError("LGS layer " + std::to_string(i + 1) + " has an empty data block");
            m_layers.emplace_back(size_t(at), size_t(size));
            at += size;
        }
    }

    // Gray raster in printer orientation, row by row.
    std::vector<uint8_t> decode(size_t layer) const
    {
        const auto [start, size] = m_layers.at(layer);
        const std::string where = "LGS layer " + std::to_string(layer + 1);
        const size_t total = columns * rows;
        std::vector<uint8_t> wire(total);
        const auto rle = m_bytes.read(start, size); // one read: byte access to a store-backed image is paged
        size_t position = 0;
        for (size_t i = 0; i < size; ++i) {
            const uint8_t nibble = rle[i] >> 4;
            uint64_t run = rle[i] & 15;
            while (i + 1 < size && (rle[i + 1] >> 4) == nibble) {
                ++i;
                run = (run << 4) | (rle[i] & 15);
                if (run > total) throw RuntimeError(where + " has a run longer than the raster");
            }
            if (run > total - position) throw RuntimeError(where + " has corrupted RLE data: it runs past the raster");
            std::fill_n(wire.begin() + position, size_t(run), uint8_t(nibble * 17));
            position += size_t(run);
        }
        if (position != total) throw RuntimeError(where + " ends short: " + std::to_string(position) + " of " + std::to_string(total) + " pixels");
        if (!rotated()) return wire;
        // The wire raster is rows wide and columns tall (rotated clockwise): undo that.
        std::vector<uint8_t> pixels(total);
        for (size_t index = 0; index < total; ++index)
            pixels[(rows - 1 - index % rows) * columns + index / rows] = wire[index];
        return pixels;
    }

    std::vector<uint8_t> encode(const std::vector<uint8_t> &pixels) const
    {
        const size_t total = columns * rows;
        auto level = [&](size_t index) {
            const size_t source = rotated() ? (rows - 1 - index % rows) * columns + index / rows : index;
            return uint8_t(pixels.at(source) & 0xf0);
        };
        std::vector<uint8_t> out;
        for (size_t begin = 0; begin < total;) {
            const uint8_t color = level(begin);
            size_t end = begin + 1;
            while (end < total && level(end) == color) ++end;
            uint8_t digits[8];
            unsigned digit_count = 0;
            size_t run = end - begin;
            do { digits[digit_count++] = uint8_t(color | (run & 15)); run >>= 4; } while (run);
            while (digit_count) out.push_back(digits[--digit_count]);
            begin = end;
        }
        return out;
    }

    // Writes the file with the given layers re-encoded into `out`, an image built from an empty base
    // (sla::PatchedImage(sla::NativeBytes())): header, previews and every other record are copied from
    // the source range by range, edited layers get a fresh record. Whatever follows the last record
    // (UVtools' second table) is dropped. Nothing is held in RAM beyond one record at a time.
    void rebuild_to(sla::PatchedImage &out, const sla::NativeEdits &edits,
                    const std::function<void()> &poll = {}) const
    {
        const size_t first = m_layers.front().first - 4;
        out.append_from(m_bytes, 0, first);
        for (size_t i = 0; i < count; ++i) {
            if (poll && i % 64 == 0) poll();
            if (!edits.is_edited(i)) {
                out.append_from(m_bytes, m_layers[i].first - 4, m_layers[i].second + 4);
                continue;
            }
            const auto encoded = encode(edits.pixels(i));
            if (encoded.size() > UINT32_MAX) throw RuntimeError("Edited LGS layer exceeds the 32-bit size field");
            std::vector<uint8_t> record;
            binary_layout::append32(record, uint32_t(encoded.size()));
            record.insert(record.end(), encoded.begin(), encoded.end());
            out.append(record);
        }
    }

private:
    sla::NativeBytes m_bytes;
    binary_layout::Reader m_in;
    std::vector<std::pair<size_t, size_t>> m_layers; // payload start, size
    bool rotated() const { return model == 4000 || model == 4500; }
};

} // namespace Slic3r

#endif
