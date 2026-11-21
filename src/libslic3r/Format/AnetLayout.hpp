// SPDX-License-Identifier: AGPL-3.0-or-later
// Reader-side layout of the Anet N4 and N7 formats. Port of the UVtools
// AnetFile.cs decode path; the writer is AnetArchive.cpp. All integers and
// doubles are big endian.
//
//   header: u32-prefixed UTF-16BE strings (version "3", name, description),
//           pixel size and layer height (f64), base layer counts, exposures,
//           bottom layers, lift speed and height (u32), the 260x140 preview
//           (u32 size 72866 + BMP), volume (f64), print time and layer count
//   per layer: lit pixel count, bounding box x/y min/max (u32 each), bit count
//           (u32), then the bit stream
//   end:    u32 support count (0), ignored by UVtools
//
// Bit stream: bit i is bit (i & 7) of byte (i >> 3), values are written most
// significant bit first. 16 bits width, 16 bits height, 1 bit "first run is
// white", then alternating runs (colors flip after each): a 5-bit key k and a
// (k + 1)-bit run length. White is 255, black 0, so a layer is strictly binary
// (anything above 127 is white) and the editor snaps painted gray accordingly.
//
// The file has no machine field: N4 or N7 comes from the extension and the
// resolution (1440x2560 or 2560x1600).
//
// Print parameters: masks-only by design. AnetFile.cs: LayerDef holds the lit pixel count, bounding box and bit stream only; exposure, lift and
// bottom layers are header globals with no PrintParameterPerLayerModifiers.
#ifndef SLIC3R_ANET_LAYOUT_HPP
#define SLIC3R_ANET_LAYOUT_HPP

#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "BinaryLayout.hpp"

namespace Slic3r {

class AnetLayout {
public:
    static bool is_format(const std::string &format) { return format == "n4" || format == "n7"; }
    // Version string "3" as UTF-16BE behind its u32 length of 2.
    static bool recognizes(const uint8_t *first8) { return std::memcmp(first8, "\0\0\0\2\0\x33\0\0", 6) == 0 && first8[6] == 0; }
    static uint8_t quantize(uint8_t value) { return value > 127 ? 255 : 0; }

    std::string format;
    size_t columns = 0, rows = 0, count = 0;

    AnetLayout(const sla::NativeBytes &bytes, const std::string &wanted_format, const std::function<void()> &poll = {})
        : m_bytes(bytes), m_in(bytes, "Anet")
    {
        format = wanted_format;
        if (bytes.size() < 8 || !recognizes(bytes.read(0, 8).data())) throw RuntimeError("Anet file is not version 3 (the version string does not match)");
        uint64_t at = 4 + 2; // version string
        for (const char *what : {"name", "description"}) {
            const uint64_t length = m_in.u32be(at);
            if (length > 65536 || length % 2) throw RuntimeError(std::string("Anet ") + what + " has an invalid length");
            at += 4;
            m_in.need(at, length, what);
            at += length;
        }
        at += 8 + 8 + 4 * 9; // pixel size, layer height, nine u32 settings up to the preview size
        const uint64_t preview = m_in.u32be(at);
        if (preview != 72866) throw RuntimeError("Anet preview size is " + std::to_string(preview) + ", expected 72866");
        at += 4;
        m_in.need(at, preview + 8 + 4 + 4, "the preview");
        at += preview + 8 + 4;
        count = m_in.u32be(at);
        at += 4;
        if (!count || count > binary_layout::max_layers) throw RuntimeError("Anet layer count is zero or exceeds the editor limit");
        m_layers.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            if (poll && i % 1024 == 0) poll();
            const std::string where = "Anet layer " + std::to_string(i + 1);
            m_in.need(at, 24, "a layer header");
            const uint64_t bits = m_in.u32be(at + 20);
            const uint64_t size = 24 + (bits + 7) / 8;
            m_in.need(at, size, "a layer bit stream");
            m_layers.push_back({size_t(at), size_t(size), size_t(bits)});
            if (i == 0) {
                if (bits < 33) throw RuntimeError(where + " bit stream is too short");
                const auto head = m_bytes.read(at, 24 + 5); // 33 bits are guaranteed by the check above
                columns = size_t(read_bits(head, 0, 16)); rows = size_t(read_bits(head, 16, 16));
                binary_layout::check_raster(columns, rows, "Anet");
            }
            at += size;
        }
        m_end = size_t(at);
        // After the layers comes the u32 support count (0); UVtools never reads it, so it may be absent.
        if (bytes.size() - m_end != 0 && bytes.size() - m_end != 4)
            throw RuntimeError("Anet file has " + std::to_string(bytes.size() - m_end) + " unexpected bytes after the last layer");
        // N4 or N7 follows the extension; without one the screen size decides.
        if (!is_format(format)) {
            if (columns == 1440 && rows == 2560) format = "n4";
            else if (columns == 2560 && rows == 1600) format = "n7";
            else throw RuntimeError("Anet file resolution " + std::to_string(columns) + "x" + std::to_string(rows) + " is neither N4 nor N7");
        }
    }

    std::vector<uint8_t> decode(size_t layer) const
    {
        const auto &record = m_layers.at(layer);
        const std::string where = "Anet layer " + std::to_string(layer + 1);
        if (record.bits < 33) throw RuntimeError(where + " bit stream is too short");
        const auto stream = m_bytes.read(record.begin, record.size); // one read: byte access to a store-backed image is paged
        if (read_bits(stream, 0, 16) != columns || read_bits(stream, 16, 16) != rows)
            throw RuntimeError(where + " differs in size from layer 1");
        const size_t total = columns * rows;
        std::vector<uint8_t> pixels(total);
        uint8_t color = read_bits(stream, 32, 1) ? 255 : 0;
        size_t position = 0, bit = 33;
        while (position < total) {
            if (bit + 5 > record.bits) throw RuntimeError(where + " ends short: the bit stream stops after " + std::to_string(position) + " pixels");
            const size_t key = size_t(read_bits(stream, bit, 5));
            if (bit + 5 + key + 1 > record.bits) throw RuntimeError(where + " ends short: the bit stream stops inside a run");
            const uint64_t run = read_bits(stream, bit + 5, key + 1);
            bit += key + 6;
            if (!run) throw RuntimeError(where + " has an empty run");
            if (run > total - position) throw RuntimeError(where + " has corrupted RLE data: it runs past the raster");
            std::fill_n(pixels.begin() + position, size_t(run), color);
            position += size_t(run);
            color = uint8_t(~color);
        }
        return pixels;
    }

    // Byte range [begin, end) of a layer record (24-byte header and bit stream).
    std::pair<size_t, size_t> record_range(size_t layer) const { return {m_layers.at(layer).begin, m_layers.at(layer).begin + m_layers.at(layer).size}; }

    // The exporter's encoding, statistics included.
    std::vector<uint8_t> encode(const std::vector<uint8_t> &pixels) const
    {
        const size_t total = columns * rows;
        Bits bits;
        bits.write(uint32_t(columns), 16);
        bits.write(uint32_t(rows), 16);
        bool previous = pixels.at(0) > 127;
        bits.write(previous ? 1 : 0, 1);
        uint32_t run = 0, white = 0, x0 = uint32_t(columns), y0 = uint32_t(rows), x1 = 0, y1 = 0;
        for (size_t i = 0; i < total; ++i) {
            const bool value = pixels[i] > 127;
            if (value) {
                ++white;
                const uint32_t x = uint32_t(i % columns), y = uint32_t(i / columns);
                x0 = std::min(x0, x); y0 = std::min(y0, y); x1 = std::max(x1, x); y1 = std::max(y1, y);
            }
            if (value != previous) { bits.run(run); run = 0; previous = value; }
            ++run;
        }
        bits.run(run);
        if (!white) x0 = y0 = x1 = y1 = 0;
        std::vector<uint8_t> out;
        for (uint32_t value : {white, x0, y0, x1, y1, bits.position})
            for (int shift = 24; shift >= 0; shift -= 8) out.push_back(uint8_t(value >> shift));
        out.insert(out.end(), bits.bytes.begin(), bits.bytes.end());
        return out;
    }

    // Writes the file with the given layers re-encoded into `out`, an image built from an empty base
    // (sla::PatchedImage(sla::NativeBytes())). Header and tail stay as they are, unedited records are copied
    // range by range; the volume estimate follows the lit-pixel change.
    void rebuild_to(sla::PatchedImage &out, const sla::NativeEdits &edits, const std::function<void()> &poll = {}) const
    {
        out.append_from(m_bytes, 0, m_layers.front().begin);
        uint64_t old_lit = 0, new_lit = 0;
        for (size_t i = 0; i < count; ++i) {
            if (poll && i % 64 == 0) poll();
            const auto &record = m_layers[i];
            const uint32_t before = m_in.u32be(record.begin);
            old_lit += before;
            if (!edits.is_edited(i)) {
                new_lit += before;
                out.append_from(m_bytes, record.begin, record.size);
                continue;
            }
            const auto encoded = encode(edits.pixels(i));
            new_lit += (uint32_t(encoded[0]) << 24) | (uint32_t(encoded[1]) << 16) | (uint32_t(encoded[2]) << 8) | encoded[3];
            out.append(encoded);
        }
        out.append_from(m_bytes, m_end, m_bytes.size() - m_end);
        if (old_lit && new_lit != old_lit) {
            // VolumeMicroL is the f64 right before the print time and layer count.
            const size_t volume = m_layers.front().begin - 8 - 4 - 4;
            const auto field = out.read(volume, 8);
            uint64_t raw = 0;
            for (unsigned n = 0; n < 8; ++n) raw = (raw << 8) | field[n];
            double value;
            std::memcpy(&value, &raw, 8);
            value *= double(new_lit) / double(old_lit);
            if (!std::isfinite(value)) throw RuntimeError("Edited Anet volume overflows");
            std::memcpy(&raw, &value, 8);
            std::vector<uint8_t> patched(8);
            for (unsigned n = 0; n < 8; ++n) patched[n] = uint8_t(raw >> (56 - 8 * n));
            out.set_range(volume, patched);
        }
    }

private:
    struct Record { size_t begin, size, bits; };
    sla::NativeBytes m_bytes;
    binary_layout::Reader m_in;
    std::vector<Record> m_layers;
    size_t m_end = 0;

    struct Bits {
        std::vector<uint8_t> bytes;
        uint32_t position = 0;
        void write(uint32_t value, unsigned width)
        {
            bytes.resize((uint64_t(position) + width + 7) / 8, 0);
            for (unsigned i = 0; i < width; ++i, ++position)
                if ((value >> (width - 1 - i)) & 1) bytes[position / 8] |= uint8_t(1u << (position % 8));
        }
        void run(uint32_t length)
        {
            unsigned key = 0;
            for (uint32_t rest = length - 1; rest; rest >>= 1) ++key;
            write(key, 5);
            write(length, key + 1);
        }
    };
    // `width` bits starting at bit `start` of a layer's stream; `record` holds the whole layer record
    // (its 24-byte header first). The caller has checked that the bits lie inside the record.
    static uint64_t read_bits(const std::vector<uint8_t> &record, size_t start, size_t width)
    {
        uint64_t value = 0;
        for (size_t i = start; i < start + width; ++i) {
            value <<= 1;
            if (record.at(24 + (i >> 3)) & (1u << (i & 7))) value |= 1;
        }
        return value;
    }
};

} // namespace Slic3r

#endif
