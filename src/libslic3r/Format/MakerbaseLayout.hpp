// SPDX-License-Identifier: AGPL-3.0-or-later
// Reader-side layout of the Makerbase MDLP and GR1 Workshop formats. They
// share one container and differ only in how much print-setting data follows
// the slicer info. Port of the UVtools MDLPFile.cs / GR1File.cs decode paths;
// the writer is MakerbaseArchive.cpp. All integers are big endian.
//
//   0       u32 7 + "MKSDLP\0" (11 bytes)
//   11      116x116 and 290x290 RGB565 previews, each followed by CR LF
//   195127  layer count (u16), resolution x, y (u16), three UTF-16BE strings
//           (display width, display height, layer height; each with a u32 byte
//           length), exposure, light-off, bottom exposure, bottom layers (u16)
//           and, for GR1 only, seven more u16 (lift, speeds and PWM)
//   then    per layer: u32 line count, count * (start y, end y, x as u16), CR LF
//   end     the 11-byte header again
//
// A line is a vertical white segment, so a layer is strictly black and white:
// gray levels cannot be stored (the writer thresholds at 128, UVtools draws
// every line as 255). The editor snaps painted values to 0 or 255 for that
// reason.
//
// Print parameters: masks-only by design. MDLPFile.cs / GR1File.cs: LayerDef is {LineCount, Lines} (geometry); exposure, lift, speeds and PWM are
// header globals and the classes define no PrintParameterPerLayerModifiers.
#ifndef SLIC3R_MAKERBASE_LAYOUT_HPP
#define SLIC3R_MAKERBASE_LAYOUT_HPP

#include <algorithm>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "BinaryLayout.hpp"

namespace Slic3r {

class MakerbaseLayout {
public:
    enum class Kind { Mdlp, Gr1 };
    static bool is_format(const std::string &format) { return format == "mdlp" || format == "gr1"; }
    // First eight bytes: the u32 length 7 and "MKSD".
    static bool recognizes(const uint8_t *first8) { return std::memcmp(first8, "\0\0\0\7MKSD", 8) == 0; }
    static uint8_t quantize(uint8_t value) { return value >= 128 ? 255 : 0; }

    std::string format;
    size_t columns = 0, rows = 0, count = 0;

    MakerbaseLayout(const sla::NativeBytes &bytes, Kind kind, const std::function<void()> &poll = {})
        : m_bytes(bytes), m_in(bytes, kind == Kind::Mdlp ? "MDLP" : "GR1")
    {
        format = kind == Kind::Mdlp ? "mdlp" : "gr1";
        const std::string &name = m_in.label();
        constexpr size_t info_at = 4 + 7 + 290 * 290 * 2 + 116 * 116 * 2 + 4;
        if (bytes.size() < info_at + 6) throw RuntimeError(name + " file is truncated: the header needs " + std::to_string(info_at + 6) + " bytes");
        const auto magic = bytes.read(0, 11);
        if (!recognizes(magic.data()) || magic[8] != 'L' || magic[9] != 'P' || magic[10] != 0)
            throw RuntimeError(name + " file has the wrong magic number");
        count = m_in.u16be(info_at);
        columns = m_in.u16be(info_at + 2);
        rows = m_in.u16be(info_at + 4);
        if (!count) throw RuntimeError(name + " layer count is zero");
        binary_layout::check_raster(columns, rows, name);
        // Display width, display height and layer height as length-prefixed UTF-16BE text.
        uint64_t at = info_at + 6;
        for (int field = 0; field < 3; ++field) {
            const uint64_t length = m_in.u32be(at);
            if (length > 1024 || length % 2) throw RuntimeError(name + " slicer info text has an invalid length");
            at += 4;
            m_in.need(at, length, "slicer info text");
            at += length;
        }
        at += 8 + (kind == Kind::Gr1 ? 14 : 0);
        m_in.need(at, 0, "the slicer info");

        m_layers.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            if (poll && i % 1024 == 0) poll();
            const uint64_t lines = m_in.u32be(at);
            const uint64_t end = at + 4 + lines * 6 + 2;
            if (end > bytes.size()) throw RuntimeError(name + " layer " + std::to_string(i + 1) + " data is truncated");
            m_layers.emplace_back(size_t(at), size_t(end));
            at = end;
        }
        // The file ends with the header again; without it the file was cut short.
        if (bytes.size() - at < 11 || bytes.read(at, 11) != magic)
            throw RuntimeError(name + " file is truncated: the closing header is missing");
    }

    std::vector<uint8_t> decode(size_t layer) const
    {
        const auto [start, end] = m_layers.at(layer);
        const std::string where = m_in.label() + " layer " + std::to_string(layer + 1);
        std::vector<uint8_t> pixels(columns * rows, 0);
        const size_t lines = (end - start - 6) / 6;
        const auto record = m_bytes.read(start, end - start); // one read: byte access to a store-backed image is paged
        auto be16 = [&](size_t at) { return size_t(record[at] << 8 | record[at + 1]); };
        for (size_t line = 0; line < lines; ++line) {
            const size_t at = 4 + line * 6;
            const size_t y0 = be16(at), y1 = be16(at + 2), x = be16(at + 4);
            if (y0 > y1 || y1 >= rows || x >= columns) throw RuntimeError(where + " has a line outside the raster");
            for (size_t y = y0; y <= y1; ++y) pixels[y * columns + x] = 255;
        }
        return pixels;
    }

    // Byte range [begin, end) of a layer record (line count, lines, CR LF).
    std::pair<size_t, size_t> record_range(size_t layer) const { return m_layers.at(layer); }

    // Column-major vertical runs of lit pixels, exactly the exporter's order.
    std::vector<uint8_t> encode(const std::vector<uint8_t> &pixels) const
    {
        std::vector<uint8_t> out(4, 0);
        uint32_t lines = 0;
        auto put16 = [&](size_t value) { out.push_back(uint8_t(value >> 8)); out.push_back(uint8_t(value)); };
        for (size_t x = 0; x < columns; ++x) {
            for (size_t y = 0; y < rows;) {
                if (pixels.at(y * columns + x) < 128) { ++y; continue; }
                size_t end = y;
                while (end + 1 < rows && pixels[(end + 1) * columns + x] >= 128) ++end;
                put16(y); put16(end); put16(x);
                ++lines;
                y = end + 1;
            }
        }
        for (unsigned n = 0; n < 4; ++n) out[n] = uint8_t(lines >> (24 - 8 * n));
        out.push_back(13); out.push_back(10);
        return out;
    }

    // Writes the file with the given layers re-encoded into `out`, an image built from an empty base
    // (sla::PatchedImage(sla::NativeBytes())): header, unedited records and the closing tail are copied
    // range by range.
    void rebuild_to(sla::PatchedImage &out, const sla::NativeEdits &edits, const std::function<void()> &poll = {}) const
    {
        out.append_from(m_bytes, 0, m_layers.front().first);
        for (size_t i = 0; i < count; ++i) {
            if (poll && i % 64 == 0) poll();
            if (!edits.is_edited(i)) out.append_from(m_bytes, m_layers[i].first, m_layers[i].second - m_layers[i].first);
            else out.append(encode(edits.pixels(i)));
        }
        out.append_from(m_bytes, m_layers.back().second, m_bytes.size() - m_layers.back().second);
    }

private:
    sla::NativeBytes m_bytes;
    binary_layout::Reader m_in;
    std::vector<std::pair<size_t, size_t>> m_layers; // record start, end (after CR LF)
};

} // namespace Slic3r

#endif
