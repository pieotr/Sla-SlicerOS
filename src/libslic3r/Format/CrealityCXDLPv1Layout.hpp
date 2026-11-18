// SPDX-License-Identifier: AGPL-3.0-or-later
// Reader/writer-side layout of the Creality CXDLP v1, v2 and v3 binary containers (big endian). The
// file is referenced, never held: this class parses the headers and the layer table once and then
// decodes a layer from its own record.
//
//   "CXSW3DV2" marker (u32 9 + 9 bytes), then
//     v1: layer count (u16)            v2/v3: version (u16 2 or 3), u32 model length + model (NUL
//                                      terminated), layer count (u16)
//   resolution x, y (u16 each); v2/v3: 64 bytes; three previews (116, 290, 290 px RGB565 + CR LF);
//   three UTF-16BE geometry strings (width, height, layer height); 22 bytes of exposure/motion/PWM
//   table: one u32 per layer (the first word of its record) + CR LF
//   v3: two NUL terminated strings and 20 bytes + CR LF
//   per layer: first word (v1: lit pixel count, v2/v3: contour area), u32 lines, lines * 6 bytes
//     (vertical run: y0 13 bits, y1 13 bits, x 14 bits, gray 8 bits), CR LF
//   marker again + u32 (v1: free, v2: XOR and v3: CRC checksum of everything before it)
//
// Format detection keeps the historic heuristic: a v1 file continues with its resolution, so the
// four bytes after the layer count are far above the model length limit of v2/v3.
#ifndef SLIC3R_CREALITY_CXDLPV1_LAYOUT_HPP
#define SLIC3R_CREALITY_CXDLPV1_LAYOUT_HPP

#include <algorithm>
#include <cmath>
#include <locale>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "BinaryLayout.hpp"
#include "CrealityChecksum.hpp"
#include "CrealityContourArea.hpp"
#include "CrealityCXDLPv1Archive.hpp"

namespace Slic3r {

class CrealityCXDLPv1Layout {
public:
    static bool recognizes(const uint8_t *first8) { return std::memcmp(first8, "\0\0\0\tCXSW", 8) == 0; }

    std::string format;
    unsigned version = 1;
    size_t columns = 0, rows = 0, count = 0;
    double width_mm = 0, height_mm = 0; // from the geometry strings (contour areas of v2/v3 depend on them)

    explicit CrealityCXDLPv1Layout(const sla::NativeBytes &bytes, const std::function<void()> &poll = {})
        : m_bytes(bytes), m_in(bytes, "CXDLPv1")
    {
        const size_t size = bytes.size();
        size_t cursor = 0;
        auto require = [&](uint64_t length) { if (length > size - cursor) throw RuntimeError("Truncated CXDLPv1 table"); };
        auto be32 = [&]() { require(4); const auto value = m_in.u32be(cursor); cursor += 4; return value; };
        auto be16 = [&]() { require(2); const auto value = m_in.u16be(cursor); cursor += 2; return value; };
        auto magic = [&]() {
            if (be32() != 9) throw RuntimeError("Invalid CXDLPv1 marker");
            require(9);
            if (!bytes.equals(cursor, "CXSW3DV2\0", 9)) throw RuntimeError("Unsupported CXDLP binary version");
            cursor += 9;
        };
        auto page = [&]() { if (be16() != 0x0d0a) throw RuntimeError("Invalid CXDLPv1 page break"); };
        magic();
        // v1 starts with the count/resolution; v2/v3 insert a version and a length-prefixed model. A valid v1 X
        // resolution makes those four bytes much larger than the bounded model length.
        size_t layer_count = be16();
        const size_t header_probe = cursor;
        const auto model_length = be32();
        cursor = header_probe;
        if ((layer_count == 2 || layer_count == 3) && model_length > 0 && model_length <= 256) {
            version = unsigned(layer_count);
            be32();
            require(model_length);
            if (m_in.u8(cursor + model_length - 1) != 0) throw RuntimeError("Invalid CXDLP printer model");
            cursor += model_length;
            layer_count = be16();
        }
        columns = be16();
        rows = be16();
        if (version >= 2) {
            require(64);
            cursor += 64;
            // The whole-file checksum is verified while streaming the file once.
            uint32_t computed = 0;
            for (size_t at = 0; at < size - 4;) {
                if (poll) poll();
                const auto part = bytes.read(at, std::min<size_t>(size - 4 - at, size_t(4) << 20));
                for (const uint8_t byte : part) computed = sla::creality_checksum_update(computed, byte, version);
                at += part.size();
            }
            if (computed != m_in.u32be(size - 4)) throw RuntimeError("CXDLP checksum mismatch");
        }
        count = layer_count;
        if (!count || !columns || !rows || columns > 16384 || rows > 8192) throw RuntimeError("Invalid CXDLPv1 resolution or layer count");
        for (const size_t preview : {116, 290, 290}) {
            require(preview * preview * 2);
            cursor += preview * preview * 2;
            page();
        }
        double geometry[3]{};
        for (unsigned field = 0; field < 3; ++field) {
            const auto length = be32();
            if (length % 2 || length > 128) throw RuntimeError("Invalid CXDLPv1 geometry string");
            std::string text;
            for (unsigned i = 0; i < length / 2; ++i) {
                const auto c = be16();
                if (c > 127) throw RuntimeError("Invalid CXDLPv1 geometry encoding");
                text.push_back(char(c));
            }
            std::istringstream number(text);
            number.imbue(std::locale::classic());
            double value;
            if (!(number >> value) || !std::isfinite(value) || value <= 0.) throw RuntimeError("Invalid CXDLPv1 geometry");
            number >> std::ws;
            if (!number.eof()) throw RuntimeError("Invalid CXDLPv1 geometry");
            geometry[field] = value;
        }
        width_mm = geometry[0];
        height_mm = geometry[1];
        require(22); // Exposure, motion and PWM are preserved verbatim.
        cursor += 22;
        m_prefix_end = cursor;
        format = version == 1 ? "cxdlpv1" : version == 2 ? "cxdlpv2" : "cxdlp";
        require(uint64_t(count) * 4);
        std::vector<uint32_t> counts(count);
        for (auto &value : counts) value = be32();
        page();
        if (version >= 3) {
            m_informative.first = cursor;
            for (unsigned i = 0; i < 2; ++i) {
                const auto length = be32();
                if (!length || length > 65536) throw RuntimeError("Invalid CXDLP informative string");
                require(length);
                if (m_in.u8(cursor + length - 1) != 0) throw RuntimeError("Invalid CXDLP informative string");
                cursor += length;
            }
            require(20);
            cursor += 20;
            page();
            m_informative.second = cursor;
        }
        m_layers.reserve(count);
        for (size_t layer = 0; layer < count; ++layer) {
            if (poll && layer % 1024 == 0) poll();
            const size_t start = cursor;
            const auto nonzero = be32(), lines = be32();
            if (nonzero != counts[layer] || (version == 1 && nonzero > columns * rows) || uint64_t(lines) * 6 > size - cursor)
                throw RuntimeError("Invalid CXDLPv1 layer table");
            cursor += size_t(lines) * 6;
            page();
            m_layers.push_back({start, size_t(8) + size_t(lines) * 6, nonzero});
        }
        m_footer = cursor;
        magic();
        be32();
        if (cursor != size) throw RuntimeError("CXDLPv1 contains unexpected trailing bytes");
    }

    // Gray raster (row by row) of one layer, validated the way the importer always did.
    std::vector<uint8_t> decode(size_t layer) const
    {
        const auto &record = m_layers.at(layer);
        const auto data = m_bytes.read(record.begin, record.size); // one read: byte access to a store-backed image is paged
        std::vector<uint8_t> pixels(columns * rows, 0);
        uint32_t decoded = 0;
        for (size_t at = 8; at < data.size(); at += 6) {
            const uint8_t *p = data.data() + at;
            const size_t y0 = (((uint32_t(p[0]) << 8) | p[1]) >> 3) & 0x1fff;
            const size_t y1 = (((uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]) >> 6) & 0x1fff;
            const size_t x = ((uint32_t(p[3]) << 8) | p[4]) & 0x3fff;
            if (y0 > y1 || y1 >= rows || x >= columns || !p[5]) throw RuntimeError("Invalid CXDLPv1 run coordinates");
            for (size_t y = y0; y <= y1; ++y) {
                if (pixels[y * columns + x]) throw RuntimeError("Overlapping CXDLPv1 runs");
                pixels[y * columns + x] = p[5];
                ++decoded;
            }
        }
        if (version == 1 && decoded != record.nonzero) throw RuntimeError("CXDLPv1 pixel count mismatch");
        return pixels;
    }

    // Byte range [begin, end) of a layer record (first word to the last run, without its CR LF).
    std::pair<size_t, size_t> record_range(size_t layer) const { return {m_layers.at(layer).begin, m_layers.at(layer).begin + m_layers.at(layer).size}; }

    // The exporter's record of a mask (v2/v3 carry the contour area in the first word).
    std::vector<uint8_t> encode_record(const std::vector<uint8_t> &pixels) const
    {
        const auto packet = CrealityCXDLPv1Archive::encode_mask(pixels.data(), columns, rows, 1);
        const auto *source = static_cast<const uint8_t *>(packet.data());
        std::vector<uint8_t> raw(source, source + packet.size());
        if (version >= 2) {
            const float pitch_x = float(std::round(float(width_mm) / columns * 10000.f) / 10000.f);
            const float pitch_y = float(std::round(float(height_mm) / rows * 10000.f) / 10000.f);
            const double area = sla::creality_largest_contour_area(pixels.data(), columns, rows) * float(pitch_x * pitch_y) * 1000.;
            if (!std::isfinite(area) || area > UINT32_MAX) throw RuntimeError("Edited CXDLP contour area exceeds 32 bits");
            for (unsigned j = 0; j < 4; ++j) raw[j] = uint8_t(uint32_t(area) >> (24 - 8 * j));
        }
        return raw;
    }

    // Writes the file with the given layers re-encoded into `out`, an image built from an empty base
    // (sla::PatchedImage(sla::NativeBytes())): prefix, informative block, unedited records and footer are
    // copied range by range. The table of first words is written last (it needs the edited records),
    // and the v2/v3 checksum is streamed over the finished image.
    void rebuild_to(sla::PatchedImage &out, const sla::NativeEdits &edits, const std::function<void()> &poll = {}) const
    {
        static const std::vector<uint8_t> page_break{13, 10};
        out.append_from(m_bytes, 0, m_prefix_end);
        const uint64_t table_at = out.size();
        out.append(std::vector<uint8_t>(count * 4, 0));
        out.append(page_break);
        if (version >= 3) out.append_from(m_bytes, m_informative.first, m_informative.second - m_informative.first);
        std::vector<uint8_t> table(count * 4);
        for (size_t i = 0; i < count; ++i) {
            if (poll && i % 64 == 0) poll();
            if (!edits.is_edited(i)) {
                const auto &record = m_layers[i];
                const auto first = m_bytes.read(record.begin, 4);
                std::copy(first.begin(), first.end(), table.begin() + i * 4);
                out.append_from(m_bytes, record.begin, record.size + 2); // the record and its CR LF
                continue;
            }
            const auto record = encode_record(edits.pixels(i));
            std::copy(record.begin(), record.begin() + 4, table.begin() + i * 4);
            out.append(record);
            out.append(page_break);
        }
        out.set_range(table_at, table);
        out.append_from(m_bytes, m_footer, m_bytes.size() - m_footer - (version >= 2 ? 4 : 0));
        if (version >= 2) {
            uint32_t checksum = 0;
            out.stream([&](const uint8_t *data, size_t length) {
                for (size_t n = 0; n < length; ++n) checksum = sla::creality_checksum_update(checksum, data[n], version);
            });
            std::vector<uint8_t> tail(4);
            for (unsigned j = 0; j < 4; ++j) tail[j] = uint8_t(checksum >> (24 - 8 * j));
            out.append(tail);
        }
    }

private:
    struct Record { size_t begin, size; uint32_t nonzero; };
    sla::NativeBytes m_bytes;
    binary_layout::Reader m_in;
    std::vector<Record> m_layers; // record start, bytes without the CR LF that follows
    size_t m_prefix_end = 0, m_footer = 0;
    std::pair<size_t, size_t> m_informative{0, 0};
};

} // namespace Slic3r

#endif
