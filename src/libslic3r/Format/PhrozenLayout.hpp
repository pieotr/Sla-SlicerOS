// SPDX-License-Identifier: AGPL-3.0-or-later
// Reader-side layout of the Phrozen PHZ v2 and Voxelab FDG v2 containers. The
// two share one layer codec and layer record; only the 216-byte header
// differs. Port of the UVtools PHZFile.cs / FDGFile.cs decode paths; the
// writers live in PhrozenArchive.cpp and UVToolsCompatArchive.cpp. All
// integers and floats are little endian.
//
//                      PHZ (magic 0x9FDA83AE)   FDG (magic 0xBD3C7AC8)
//   layer count            40                       8
//   resolution x / y       24 / 28                  24 / 28
//   layer table offset     36                       52
//   XOR key                88                       92
//   volume/weight/cost     128 / 132 / 136          104 / 108 / 112
//
// Layer record (36 bytes): Z, exposure, light-off delay, data address, data
// size, page number (high 32 address bits), 3 unknown words (FDG keeps 84 in
// the first). Layer data is a 7-bit gray RLE: a byte with the top bit set is
// one pixel of gray (b & 0x7f), a byte without it repeats the previous gray
// b more times. The XOR key algorithm differs per family.
#ifndef SLIC3R_PHROZEN_LAYOUT_HPP
#define SLIC3R_PHROZEN_LAYOUT_HPP

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "FDGArchive.hpp"
#include "NativeParameterLayout.hpp"
#include "PhrozenArchive.hpp"
#include "libslic3r/Exception.hpp"
#include "libslic3r/SLA/NativeBytes.hpp"

namespace Slic3r {

class PhrozenLayout {
public:
    enum class Kind { Phz, Fdg };
    struct Spec {
        uint32_t magic;
        size_t count, columns, rows, table, key;
        size_t volume; // f32 volume (ml), then weight (g) and cost
        // Header fields that depend on the layer table (UVtools PHZFile/FDGFile).
        size_t bottom_layers, print_time, overall_height, bed_z, light_off_bottom, light_off, lift_bottom;
        // lift_bottom: f32 height, f32 speed; the normal pair follows, then the retract speed.
    };
    static const Spec &spec(Kind kind)
    {
        static const Spec phz{0x9FDA83AEu, 40, 24, 28, 36, 88, 128, 20, 48, 72, 84, 92, 96, 108};
        static const Spec fdg{0xBD3C7AC8u, 8, 24, 28, 52, 92, 104, 12, 56, 76, 88, 124, 128, 136};
        return kind == Kind::Phz ? phz : fdg;
    }
    static constexpr size_t header_size = 216, record_size = 36;
    // The first four bytes, as read little endian, name the family.
    static bool kind_of_magic(uint32_t magic, Kind &kind)
    {
        if (magic == spec(Kind::Phz).magic) { kind = Kind::Phz; return true; }
        if (magic == spec(Kind::Fdg).magic) { kind = Kind::Fdg; return true; }
        return false;
    }
    static const char *label(Kind kind) { return kind == Kind::Phz ? "PHZ" : "FDG"; }
    size_t volume_offset() const { return spec(m_kind).volume; }

    // What a gray value becomes after being stored. The 7-bit field cannot
    // hold white: 0xff is capped to 0x7c, which reads back as 0xf8. The editor
    // shows exactly this so that what is painted is what is saved.
    static uint8_t quantize(uint8_t value)
    {
        const uint8_t gray = std::min<uint8_t>(value >> 1, 0x7c);
        return uint8_t((gray << 1) | (gray & 1));
    }

    std::string format;
    size_t columns = 0, rows = 0, count = 0, table = 0;
    uint32_t key = 0;

    // `bytes` is the container image: a vector or a store-backed view of the source file.
    PhrozenLayout(const sla::NativeBytes &bytes, Kind kind, const std::function<void()> &poll = {})
        : m_bytes(bytes), m_kind(kind)
    {
        format = kind == Kind::Phz ? "phz" : "fdg";
        const std::string name = label(kind);
        const auto &fields = spec(kind);
        if (bytes.size() < header_size)
            throw RuntimeError(name + " file is truncated: the header needs " + std::to_string(header_size) + " bytes");
        if (integer(0) != fields.magic)
            throw RuntimeError(name + " file has the wrong magic number");
        columns = integer(fields.columns); rows = integer(fields.rows); count = integer(fields.count);
        table = integer(fields.table); key = integer(fields.key);
        if (!columns || !rows || columns > 32768 || rows > 32768 || uint64_t(columns) * rows > 128ULL * 1024 * 1024)
            throw RuntimeError(name + " resolution is zero or exceeds the editor raster limit");
        if (!count || count > 100000)
            throw RuntimeError(name + " layer count is zero or exceeds the editor limit");
        if (table < header_size || table > bytes.size() || (bytes.size() - table) / record_size < count)
            throw RuntimeError(name + " layer table is truncated or outside the file");
        const uint64_t pixels = uint64_t(columns) * rows;
        for (size_t i = 0; i < count; ++i) {
            if (poll && i % 4096 == 0) poll();
            const auto range = payload(i);
            // A run of n pixels takes at most n bytes, so no valid layer exceeds the raster size.
            if (!range.size || range.size > pixels)
                throw RuntimeError(name + " layer " + std::to_string(i + 1) + " has an empty or oversized data block");
            if (range.address > bytes.size() || range.size > bytes.size() - range.address)
                throw RuntimeError(name + " layer " + std::to_string(i + 1) + " data is truncated");
        }
    }

    size_t record(size_t layer) const { return table + record_size * layer; }
    float z(size_t layer) const { return number(record(layer)); }
    float number(size_t at) const
    {
        const uint32_t bits = integer(at);
        float value;
        std::memcpy(&value, &bits, 4);
        return value;
    }
    uint32_t integer(size_t at) const
    {
        if (at > m_bytes.size() || m_bytes.size() - at < 4) throw RuntimeError(std::string(label(m_kind)) + " header field is outside the file");
        return uint32_t(m_bytes[at]) | (uint32_t(m_bytes[at + 1]) << 8) | (uint32_t(m_bytes[at + 2]) << 16) | (uint32_t(m_bytes[at + 3]) << 24);
    }

    struct Payload { uint64_t address; size_t size; };
    // The address is page * 2^32 + the 32-bit offset, as in UVtools.
    Payload payload(size_t layer) const
    {
        const size_t at = record(layer);
        return {(uint64_t(integer(at + 20)) << 32) | integer(at + 12), integer(at + 16)};
    }

    std::vector<uint8_t> decode(size_t layer, const std::function<void()> &poll = {}) const
    {
        const auto range = payload(layer);
        auto rle = m_bytes.read(range.address, range.size);
        crypt(rle, uint32_t(layer));
        return decode_rle(rle, columns * rows, layer, label(m_kind), poll);
    }

    static std::vector<uint8_t> decode_rle(const std::vector<uint8_t> &rle, size_t pixel_count, size_t layer,
                                           const char *family, const std::function<void()> &poll = {})
    {
        const std::string where = std::string(family) + " layer " + std::to_string(layer + 1);
        std::vector<uint8_t> pixels(pixel_count, 0);
        size_t position = 0;
        uint8_t color = 0;
        for (size_t i = 0; i < rle.size(); ++i) {
            if (poll && i % (1 << 20) == 0) poll();
            const uint8_t code = rle[i];
            if (code & 0x80) {
                // Seven bits widened to eight by repeating the last bit; the
                // two top codes mean full white.
                color = uint8_t(((code & 0x7f) << 1) | (code & 1));
                if (color >= 0xfc) color = 0xff;
                if (position >= pixel_count)
                    throw RuntimeError(where + " has corrupted RLE data: it runs past the end of the raster");
                pixels[position++] = color;
            } else {
                if (code > pixel_count - position)
                    throw RuntimeError(where + " has corrupted RLE data: it runs past the end of the raster");
                std::fill_n(pixels.begin() + position, code, color);
                position += code;
            }
        }
        if (position != pixel_count)
            throw RuntimeError(where + " ends short: " + std::to_string(position) + " of " + std::to_string(pixel_count) + " pixels");
        return pixels;
    }

    // Same bytes the exporter writes (runs end at the half-width column, 125
    // max), then the layer-indexed XOR when the file uses one.
    std::vector<uint8_t> encode(const std::vector<uint8_t> &pixels, size_t layer) const
    {
        const auto encoded = PhrozenPHZArchive::encode_mask(pixels.data(), columns, rows, 1);
        const auto *data = static_cast<const uint8_t *>(encoded.data());
        std::vector<uint8_t> payload(data, data + encoded.size());
        crypt(payload, uint32_t(layer));
        return payload;
    }
    // XOR is its own inverse; a zero key leaves the data alone.
    void crypt(std::vector<uint8_t> &data, uint32_t layer) const
    {
        if (m_kind == Kind::Phz) PhrozenPHZArchive::crypt_mask(data, key, layer);
        else FDGArchive::crypt_mask(data, key, layer);
    }

    // ---- Per-layer parameters -------------------------------------------------------------
    // The record stores Z, exposure and light-off delay. Lift and retract are
    // header globals in both families, so they are not per-layer fields.
    static sla::LayerParameterCapabilities parameter_capabilities()
    {
        using F = sla::LayerParameter;
        return native_parameters::capabilities({F::PositionZ, F::Exposure, F::LightOff});
    }
    native_parameters::Row parameter_row(size_t layer) const
    {
        using F = sla::LayerParameter;
        const auto at = record(layer);
        native_parameters::Row row{{F::PositionZ, number(at)}, {F::Exposure, number(at + 4)}, {F::LightOff, number(at + 8)}};
        for (const auto &[field, value] : row)
            if (!std::isfinite(value) || value < 0)
                throw RuntimeError(std::string(label(m_kind)) + " layer " + std::to_string(layer + 1) + " has an invalid parameter");
        return row;
    }
    native_parameters::Table parameter_rows() const
    {
        native_parameters::Table rows;
        rows.reserve(count);
        for (size_t i = 0; i < count; ++i) rows.push_back(parameter_row(i));
        return rows;
    }
    // Machine height the header declares (0 when absent).
    double height_limit() const { return number(spec(m_kind).bed_z); }
    size_t bottom_layers() const { return integer(spec(m_kind).bottom_layers); }
    // The row plus the header globals that the layer's motion uses, so that the
    // print-time model sees the whole cycle.
    native_parameters::Row timed_row(size_t layer, native_parameters::Row row) const
    {
        using F = sla::LayerParameter;
        const auto &fields = spec(m_kind);
        const bool bottom = layer < integer(fields.bottom_layers);
        const size_t lift = fields.lift_bottom + (bottom ? 0 : 8);
        row[F::LiftDistance] = number(lift);
        row[F::LiftSpeed] = number(lift + 4);
        row[F::RetractSpeed] = number(fields.lift_bottom + 16);
        return row;
    }
    // Writes the record fields that differ from the file, the overall height
    // and the print time into `out` (the file patched in place plus appended payloads;
    // record offsets are the same). Returns whether anything was patched.
    bool patch_parameters(sla::PatchedImage &out, const native_parameters::Table &table) const
    {
        using F = sla::LayerParameter;
        if (table.size() != count) throw RuntimeError(std::string(label(m_kind)) + " parameter table has the wrong layer count");
        auto put32 = [&](size_t at, uint32_t value) { out.put32(at, value); };
        auto putf = [&](size_t at, double value) {
            const float number = float(value);
            uint32_t bits;
            std::memcpy(&bits, &number, 4);
            put32(at, bits);
        };
        bool changed = false;
        double old_total = 0, new_total = 0;
        for (size_t i = 0; i < count; ++i) {
            const auto original = parameter_row(i);
            const auto &row = table[i];
            old_total += native_parameters::layer_seconds(timed_row(i, original));
            new_total += native_parameters::layer_seconds(timed_row(i, row));
            const size_t at = record(i);
            for (const auto &[field, offset] : {std::pair{F::PositionZ, size_t(0)}, {F::Exposure, size_t(4)}, {F::LightOff, size_t(8)}})
                if (row.at(field) != original.at(field)) { putf(at + offset, row.at(field)); changed = true; }
        }
        if (!changed) return false;
        const auto &fields = spec(m_kind);
        if (table.back().at(F::PositionZ) != z(count - 1)) putf(fields.overall_height, table.back().at(F::PositionZ));
        put32(fields.print_time, native_parameters::carried_time(integer(fields.print_time), old_total, new_total));
        return true;
    }

private:
    sla::NativeBytes m_bytes;
    Kind m_kind;
};

} // namespace Slic3r

#endif
