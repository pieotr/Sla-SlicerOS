// SPDX-License-Identifier: AGPL-3.0-or-later
// Reader-side layout of the Vlare Open File Format (OSF v4). Port of the
// UVtools OSFFile.cs decode path; the writer is OSFArchive.cpp. All integers
// are big endian.
//
//   0   header length (u32, offset of the first layer), version (u16, 4),
//       image log (u8, 2)
//   7   four previews, each a u24 size followed by RGB565 data
//       then the 126-byte settings table that ends at "header length":
//         resolution x, y (u16), pixel size (u16, um * 100), mirror (u8),
//         PWM bottom/normal (u8), AA, distortion, delayed (u8 each),
//         layer count (u32), parameter sets (u16), ...
//   then per layer: mark 0D 0A (0D 0B when the layer holds supports only),
//       number of RLE elements (u32), first row "start y" (u16), the elements.
//
// RLE element: one byte whose top seven bits are the gray level; bit 0 set
// means a run follows as a 1, 2, 3 or 4 byte length (top bits of the first
// byte select the width: 0xxxxxxx, 10xxxxxx, 110xxxxx, 1110xxxx). Without
// bit 0 it is a single pixel. Gray is 7 bit, widened as (g | 1) unless zero.
// UVtools finds the next layer by the sequence 0D 0A/0B where an element would
// start, which is why a run of 0x0c pixels with length 10 or 11 is written in
// the two-byte form.
//
// Print parameters: masks-only by design. OSFFile.cs: OSFLayerDef is {Mark, NumberOfLines, StartY, EncodedRle}; exposure, lifts, waits and PWM
// are header globals (and the parameter-set count is global) with no PrintParameterPerLayerModifiers.
#ifndef SLIC3R_VLARE_LAYOUT_HPP
#define SLIC3R_VLARE_LAYOUT_HPP

#include <algorithm>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "BinaryLayout.hpp"

namespace Slic3r {

class VlareLayout {
public:
    static constexpr size_t settings_size = 126;
    static bool is_format(const std::string &format) { return format == "osf"; }
    // No magic number: the first seven bytes are the header length, version 4 and image log 2.
    static bool recognizes(const uint8_t *first8)
    {
        return first8[4] == 0 && first8[5] == 4 && first8[6] == 2;
    }
    // 7-bit gray stored, widened to 8 bit again.
    static uint8_t quantize(uint8_t value)
    {
        const uint8_t seven = value & 0xfe;
        return seven ? uint8_t(seven | 1) : uint8_t(0);
    }

    std::string format = "osf";
    size_t columns = 0, rows = 0, count = 0;

    explicit VlareLayout(const sla::NativeBytes &bytes, const std::function<void()> &poll = {}) : m_bytes(bytes), m_in(bytes, "OSF")
    {
        if (bytes.size() < 7 + settings_size) throw RuntimeError("OSF file is truncated: the header needs at least " + std::to_string(7 + settings_size) + " bytes");
        m_header = m_in.u32be(0);
        if (!recognizes(bytes.read(0, 8).data())) throw RuntimeError("OSF file is not version 4 (version/image log fields do not match)");
        uint64_t at = 7;
        for (int preview = 0; preview < 4; ++preview) {
            m_in.need(at, 3, "a preview size");
            const uint64_t size = (uint64_t(m_in.u8(at)) << 16) | (uint64_t(m_in.u8(at + 1)) << 8) | m_in.u8(at + 2);
            at += 3;
            m_in.need(at, size, "a preview");
            at += size;
        }
        if (at + settings_size != m_header || m_header > bytes.size())
            throw RuntimeError("OSF header length does not match its previews and settings table");
        const size_t settings = size_t(at);
        columns = m_in.u16be(settings);
        rows = m_in.u16be(settings + 2);
        binary_layout::check_raster(columns, rows, "OSF");
        count = m_in.u32be(settings + 12);
        if (!count || count > binary_layout::max_layers) throw RuntimeError("OSF layer count is zero or exceeds the editor limit");

        m_layers.reserve(count);
        const binary_layout::Window scan(bytes); // the element scan walks the file byte by byte
        size_t position = m_header;
        for (size_t layer = 0; layer < count; ++layer) {
            if (poll && layer % 1024 == 0) poll();
            const std::string where = "OSF layer " + std::to_string(layer + 1);
            m_in.need(position, 8, "a layer header");
            const uint32_t mark = m_in.u16be(position);
            if (mark != 0x0D0A && mark != 0x0D0B) throw RuntimeError(where + " does not start with a layer mark");
            const size_t lines = m_in.u32be(position + 2);
            const size_t start_y = m_in.u16be(position + 6);
            if (start_y > rows) throw RuntimeError(where + " starts below the raster");
            const size_t data = position + 8;
            size_t cursor = data, elements = 0;
            while (lines && cursor < bytes.size()) {
                const uint8_t code = scan[cursor];
                if (code & 1) {
                    m_in.need(cursor, 2, "an RLE run");
                    const uint8_t first = scan[cursor + 1];
                    // The next layer begins where an element could start with 0D 0A / 0D 0B.
                    if (code == 0x0D && (first == 0x0A || first == 0x0B)) break;
                    cursor += 2 + length_extra(first, where);
                    m_in.need(cursor, 0, "an RLE run");
                } else {
                    ++cursor;
                }
                ++elements;
            }
            if (elements != lines)
                throw RuntimeError(where + " declares " + std::to_string(lines) + " RLE elements but holds " + std::to_string(elements));
            m_layers.push_back({position, data, cursor, start_y});
            position = cursor;
        }
        if (position != bytes.size()) throw RuntimeError("OSF file has " + std::to_string(bytes.size() - position) + " unexpected trailing bytes");
    }

    std::vector<uint8_t> decode(size_t layer) const
    {
        const auto &record = m_layers.at(layer);
        const std::string where = "OSF layer " + std::to_string(layer + 1);
        const auto rle = m_bytes.read(record.data, record.end - record.data); // one read: byte access to a store-backed image is paged
        auto byte = [&](size_t at) {
            if (at >= rle.size()) throw RuntimeError(where + " has corrupted RLE data: a run length lies beyond the record");
            return rle[at];
        };
        const size_t total = columns * rows;
        std::vector<uint8_t> pixels(total, 0);
        size_t position = record.start_y * columns;
        for (size_t n = 0; n < rle.size(); ++n) {
            uint8_t code = rle[n];
            uint64_t stride = 1;
            if (code & 1) {
                code &= 0xfe;
                const uint8_t first = byte(++n);
                if ((first & 0x80) == 0) stride = first;
                else if ((first & 0xc0) == 0x80) { stride = (uint64_t(first & 0x3f) << 8) + byte(n + 1); n += 1; }
                else if ((first & 0xe0) == 0xc0) { stride = (uint64_t(first & 0x1f) << 16) + (uint64_t(byte(n + 1)) << 8) + byte(n + 2); n += 2; }
                else { stride = (uint64_t(first & 0x0f) << 24) + (uint64_t(byte(n + 1)) << 16) + (uint64_t(byte(n + 2)) << 8) + byte(n + 3); n += 3; }
            }
            if (stride > total - position) throw RuntimeError(where + " has corrupted RLE data: it runs past the raster");
            if (code) std::fill_n(pixels.begin() + position, size_t(stride), uint8_t(code | 1));
            position += size_t(stride);
        }
        return pixels;
    }

    // Byte range [begin, end) of a layer record (mark to the last element).
    std::pair<size_t, size_t> record_range(size_t layer) const { return {m_layers.at(layer).begin, m_layers.at(layer).end}; }

    // The exporter's encoding: one element per run of equal 7-bit gray over the whole raster.
    std::vector<uint8_t> encode(const std::vector<uint8_t> &pixels) const
    {
        const size_t total = columns * rows;
        std::vector<uint8_t> runs;
        uint32_t lines = 0;
        for (size_t first = 0; first < total;) {
            const uint8_t color = pixels.at(first) & 0xfe;
            size_t last = first + 1;
            while (last < total && (pixels[last] & 0xfe) == color) ++last;
            const size_t length = last - first;
            ++lines;
            runs.push_back(uint8_t(color | (length > 1 ? 1 : 0)));
            if (length > 1) {
                const bool marker = color == 0x0c && (length == 10 || length == 11);
                if (length <= 0x7f && !marker) runs.push_back(uint8_t(length));
                else if (length <= 0x3fff) { runs.push_back(uint8_t(0x80 | (length >> 8))); runs.push_back(uint8_t(length)); }
                else if (length <= 0x1fffff) {
                    runs.push_back(uint8_t(0xc0 | (length >> 16))); runs.push_back(uint8_t(length >> 8)); runs.push_back(uint8_t(length));
                } else {
                    runs.push_back(uint8_t(0xe0 | (length >> 24))); runs.push_back(uint8_t(length >> 16));
                    runs.push_back(uint8_t(length >> 8)); runs.push_back(uint8_t(length));
                }
            }
            first = last;
        }
        std::vector<uint8_t> layer = {0x0d, 0x0a, uint8_t(lines >> 24), uint8_t(lines >> 16), uint8_t(lines >> 8), uint8_t(lines), 0, 0};
        layer.insert(layer.end(), runs.begin(), runs.end());
        return layer;
    }

    // Writes the file with the given layers re-encoded into `out`, an image built from an empty base
    // (sla::PatchedImage(sla::NativeBytes())): the header and every unedited layer are copied range by range.
    void rebuild_to(sla::PatchedImage &out, const sla::NativeEdits &edits, const std::function<void()> &poll = {}) const
    {
        out.append_from(m_bytes, 0, m_header);
        for (size_t i = 0; i < count; ++i) {
            if (poll && i % 64 == 0) poll();
            if (!edits.is_edited(i)) out.append_from(m_bytes, m_layers[i].begin, m_layers[i].end - m_layers[i].begin);
            else out.append(encode(edits.pixels(i)));
        }
    }

private:
    struct Record { size_t begin, data, end, start_y; };
    sla::NativeBytes m_bytes;
    binary_layout::Reader m_in;
    size_t m_header = 0;
    std::vector<Record> m_layers;

    // Bytes that follow the first length byte of a run.
    static size_t length_extra(uint8_t first, const std::string &where)
    {
        if ((first & 0x80) == 0) return 0;
        if ((first & 0xc0) == 0x80) return 1;
        if ((first & 0xe0) == 0xc0) return 2;
        if ((first & 0xf0) == 0xe0) return 3;
        throw RuntimeError(where + " has corrupted RLE data: invalid run length prefix");
    }
};

} // namespace Slic3r

#endif
