// SPDX-License-Identifier: AGPL-3.0-or-later
// Byte helpers shared by the reader-side layouts of the binary printer
// formats (LGS, JXS, VDT, MDLP, GR1, QDT, OSLA, OSF, Anet).
//
// WHY bounds-checked accessors: every offset in these files comes from the
// file itself, so a damaged or hostile length must become a RuntimeError with
// the family name, never an out-of-range read.
#ifndef SLIC3R_BINARY_LAYOUT_HPP
#define SLIC3R_BINARY_LAYOUT_HPP

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "libslic3r/Exception.hpp"
#include "libslic3r/SLA/NativeBytes.hpp"

namespace Slic3r::binary_layout {

// Read side over a NativeBytes view: an in-memory vector (restored documents, freshly built
// output) or a store-backed image (the source file the document was opened from, a PatchedImage
// being verified), so a family can be parsed without holding its file in RAM. A std::vector
// converts implicitly (non-owning, it must outlive the reader). `label` names the family.
class Reader {
public:
    Reader(const sla::NativeBytes &bytes, std::string label) : m_bytes(bytes), m_label(std::move(label)) {}
    const sla::NativeBytes &bytes() const { return m_bytes; }
    const std::string &label() const { return m_label; }

    void need(uint64_t at, uint64_t length, const char *what) const
    {
        if (at > m_bytes.size() || length > m_bytes.size() - at)
            throw RuntimeError(m_label + " file is truncated: " + what + " lies beyond the end of the file");
    }
    uint8_t u8(uint64_t at) const { need(at, 1, "a field"); return m_bytes[size_t(at)]; }
    uint16_t u16(uint64_t at) const { need(at, 2, "a field"); return uint16_t(m_bytes[at] | (m_bytes[at + 1] << 8)); }
    uint16_t u16be(uint64_t at) const { need(at, 2, "a field"); return uint16_t((m_bytes[at] << 8) | m_bytes[at + 1]); }
    uint32_t u32(uint64_t at) const
    {
        need(at, 4, "a field");
        return uint32_t(m_bytes[at]) | (uint32_t(m_bytes[at + 1]) << 8) | (uint32_t(m_bytes[at + 2]) << 16) | (uint32_t(m_bytes[at + 3]) << 24);
    }
    uint32_t u32be(uint64_t at) const
    {
        need(at, 4, "a field");
        return (uint32_t(m_bytes[at]) << 24) | (uint32_t(m_bytes[at + 1]) << 16) | (uint32_t(m_bytes[at + 2]) << 8) | m_bytes[at + 3];
    }
    float f32(uint64_t at) const { const uint32_t bits = u32(at); float value; std::memcpy(&value, &bits, 4); return value; }
    // A bounds-checked range in one piece (layer payloads).
    std::vector<uint8_t> read(uint64_t at, uint64_t length, const char *what = "a data block") const
    {
        need(at, length, what);
        return m_bytes.read(at, length);
    }

private:
    sla::NativeBytes m_bytes;
    std::string m_label;
};

// Forward-moving byte access over a NativeBytes view for record scanners: a window of 1 MiB is read
// at a time, so walking a whole file byte by byte costs one read per MiB instead of one per byte.
// Out-of-range access throws like the other readers.
class Window {
public:
    explicit Window(const sla::NativeBytes &bytes) : m_bytes(bytes) {}
    size_t size() const { return m_bytes.size(); }
    uint8_t operator[](uint64_t at) const
    {
        if (at - m_begin >= m_window.size()) {
            if (at >= m_bytes.size()) throw RuntimeError("The container is truncated");
            m_begin = at;
            m_window = m_bytes.read(at, std::min<uint64_t>(uint64_t(1) << 20, m_bytes.size() - at));
        }
        return m_window[size_t(at - m_begin)];
    }

private:
    sla::NativeBytes m_bytes;
    mutable uint64_t m_begin = 0;
    mutable std::vector<uint8_t> m_window;
};

inline void put32(std::vector<uint8_t> &bytes, size_t at, uint32_t value)
{
    for (unsigned n = 0; n < 4; ++n) bytes.at(at + n) = uint8_t(value >> (8 * n));
}
inline void append32(std::vector<uint8_t> &bytes, uint32_t value)
{
    for (unsigned n = 0; n < 4; ++n) bytes.push_back(uint8_t(value >> (8 * n)));
}

// A float field that must hold a whole, positive pixel count.
inline size_t whole_positive(float value, size_t limit, const std::string &label, const char *what)
{
    if (!(value >= 1.f) || value > float(limit) || value != float(uint64_t(value)))
        throw RuntimeError(label + " " + what + " is not a whole number within the editor limits");
    return size_t(value);
}

// Shared sanity limits, the same as the other editor readers.
constexpr size_t max_dimension = 32768;
constexpr uint64_t max_pixels = 128ULL * 1024 * 1024;
constexpr size_t max_layers = 100000;

inline void check_raster(size_t columns, size_t rows, const std::string &label)
{
    if (!columns || !rows || columns > max_dimension || rows > max_dimension || uint64_t(columns) * rows > max_pixels)
        throw RuntimeError(label + " resolution is zero or exceeds the editor raster limit");
}

} // namespace Slic3r::binary_layout

#endif
