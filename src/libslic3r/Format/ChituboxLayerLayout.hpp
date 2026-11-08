// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "CTBEncryptedCrypto.hpp"
#include "ChituboxFormat.hpp"
#include "ChituboxRasterEncoder.hpp"
#include "libslic3r/SLA/LayerProgress.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace Slic3r::sla {
// Original addressed ChituboxFile v1–v5 layout (including GKtwo).
// Editing appends replacement payloads and
// changes only their table pointers and material statistics. Unknown sections,
// previews, gaps, seed and all per-layer parameters retain their original bytes.
struct ChituboxLayerLayout {
    struct Range {
        size_t begin, end;
    };
    const std::vector<uint8_t> &bytes;
    uint32_t magic, version, columns, rows, count, planes, seed, table, parameters;
    bool binary;
    std::vector<Range> metadata;
    void require(size_t at, uint64_t length) const {
        if (at > bytes.size() || length > bytes.size() - at)
            throw RuntimeError("Chitubox section lies outside the file");
    }
    uint32_t integer(size_t at) const {
        require(at, 4);
        return uint32_t(bytes[at]) | (uint32_t(bytes[at + 1]) << 8) | (uint32_t(bytes[at + 2]) << 16) |
               (uint32_t(bytes[at + 3]) << 24);
    }
    float number(size_t at) const {
        const auto bits = integer(at);
        float f;
        std::memcpy(&f, &bits, 4);
        return f;
    }
    void section(size_t at, uint64_t length) {
        require(at, length);
        metadata.push_back({at, at + size_t(length)});
    }
    size_t record(size_t layer, unsigned plane = 0) const {
        return table + 36 * (size_t(plane) * count + layer);
    }
    // V3+ duplicates the main layer definition immediately before the RLE.
    size_t extended_record(size_t layer) const {
        const auto data = integer(record(layer) + 12);
        if (data < 84) throw RuntimeError("Truncated Chitubox extended layer record");
        return size_t(data) - 84;
    }
    explicit ChituboxLayerLayout(const std::vector<uint8_t> &source, const std::function<void()> &check = {}) : bytes(source) {
        if(check)check();
        section(0, 112);
        magic = integer(0);
        binary = magic == 0x12fd0019;
        if (!binary && magic != 0x12fd0086 && magic != 0x12fd0106 && magic != 0xff220810)
            throw RuntimeError("Not a Chitubox layer archive");
        version = integer(4);
        if (!version || version > (binary ? 2u : 5u) || (magic == 0xff220810 && version != 4))
            throw RuntimeError("Chitubox editor requires a native reader for this version");
        columns = integer(52);
        rows = integer(56);
        count = integer(68);
        planes = binary ? (version == 1 ? 1 : integer(92)) : 1;
        table = integer(64);
        seed = integer(100);
        parameters = integer(84);
        if (!columns || !rows || columns > 32768 || rows > 32768 ||
            uint64_t(columns) * rows > (uint64_t(128) << 20) || !count || count > 100000 || !planes ||
            planes > 16)
            throw RuntimeError("Chitubox dimensions, layer count or AA exceed the editor limits");
        for (size_t field : {8u, 12u})
            if (!std::isfinite(number(field)) || number(field) <= 0)
                throw RuntimeError("Invalid Chitubox physical dimensions");
        section(table, uint64_t(count) * planes * 36);
        if (parameters) {
            if (version == 1 || integer(88) < 60)
                throw RuntimeError("Invalid Chitubox print parameters");
            section(parameters, integer(88));
        }
        if (const auto slicer = integer(104)) {
            const auto size = integer(108);
            section(slicer, size);
            // Protect addressed strings/tables, not just the immediate header.
            if (size >= 36 && integer(slicer + 32))
                section(integer(slicer + 28), integer(slicer + 32));
            if (version >= 4) {
                if (size < 68 || !integer(slicer + 64))
                    throw RuntimeError("Missing Chitubox v4 print parameters");
                const auto v4 = integer(slicer + 64);
                section(v4, 464);
                if (integer(v4 + 76)) section(integer(v4 + 72), integer(v4 + 76));
                if (version >= 5 && integer(v4 + 80)) {
                    const auto resin = integer(v4 + 80);
                    section(resin, 40);
                    for (auto [address, length] : {std::pair<size_t,size_t>{8,28},{16,12},{24,20}})
                        if (integer(resin + length))
                            section(integer(resin + address), integer(resin + length));
                }
            }
        } else if (version >= 3)
            throw RuntimeError("Missing Chitubox slicer information");
        if (version >= 3) {
            for (size_t i = 0; i < count; ++i) {
                if (check && i % 256 == 0) check();
                const auto at = record(i);
                if (integer(at + 20)) throw RuntimeError("Unsupported Chitubox payload page");
                const auto extended = extended_record(i);
                section(extended, 84);
                // UVtools takes Z/exposure/light-off from the duplicate. The
                // editor uses the main table for statistics; ambiguous input
                // must be rejected rather than silently choosing one copy.
                for (size_t field : {0u, 4u, 8u})
                    if (integer(extended + field) != integer(at + field))
                        throw RuntimeError("Inconsistent Chitubox extended layer definition");
                for (size_t field = 40; field <= 80; field += 4)
                    if (!std::isfinite(number(extended + field)) || number(extended + field) < 0)
                        throw RuntimeError("Invalid Chitubox extended layer parameter");
            }
        }
        for (size_t field : {60u, 72u}) {
            const auto at = integer(field);
            if (!at)
                continue;
            section(at, 32);
            const auto width = integer(at), height = integer(at + 4), data = integer(at + 8),
                       length = integer(at + 12);
            if (!width || !height || width > 4096 || height > 4096)
                throw RuntimeError("Invalid Chitubox preview dimensions");
            section(data, length);
            uint64_t pixels = 0;
            size_t next_check=data;
            for (size_t cursor = data; cursor < size_t(data) + length;) {
                if(check && cursor>=next_check){check();next_check=cursor+65536;}
                require(cursor, 2);
                if (size_t(data) + length - cursor < 2)
                    throw RuntimeError("Truncated Chitubox preview");
                const auto color = uint16_t(bytes[cursor]) | (uint16_t(bytes[cursor + 1]) << 8);
                cursor += 2;
                unsigned run = 1;
                if (color & 32) {
                    if (size_t(data) + length - cursor < 2)
                        throw RuntimeError("Truncated Chitubox preview run");
                    run += bytes[cursor] | ((bytes[cursor + 1] & 15) << 8);
                    cursor += 2;
                }
                pixels += run;
                if (pixels > uint64_t(width) * height)
                    throw RuntimeError("Chitubox preview exceeds its dimensions");
            }
            if (pixels != uint64_t(width) * height)
                throw RuntimeError("Incomplete Chitubox preview");
        }
        auto less = [](const Range &a, const Range &b) {
            return a.begin < b.begin || (a.begin == b.begin && a.end < b.end);
        };
        std::sort(metadata.begin(), metadata.end(), less);
        for (size_t i = 1; i < metadata.size(); ++i)
            if (metadata[i].begin < metadata[i - 1].end &&
                (metadata[i].begin != metadata[i - 1].begin || metadata[i].end != metadata[i - 1].end))
                throw RuntimeError("Chitubox metadata sections overlap");
        std::vector<Range> payloads;
        for (unsigned plane = 0; plane < planes; ++plane)
            for (size_t i = 0; i < count; ++i) {
                if(check && i%256==0)check();
                const auto at = record(i, plane);
                const auto data = integer(at + 12), length = integer(at + 16);
                if (integer(at + 20) || !length)
                    throw RuntimeError("Unsupported Chitubox payload page or empty mask");
                require(data, length);
                const Range range{data, size_t(data) + length};
                // Sections are sorted and disjoint (identical shared ranges
                // allowed). Binary search avoids scanning every extended
                // record for every layer, which would be quadratic in count.
                const auto section = std::lower_bound(metadata.begin(), metadata.end(), range.begin,
                    [](const Range &item, size_t begin) { return item.end <= begin; });
                if (section != metadata.end() && section->begin < range.end)
                    throw RuntimeError("Chitubox layer overlaps metadata");
                for (size_t field : {0u, 4u, 8u}) {
                    const float value = number(at + field);
                    if (!std::isfinite(value) || value < 0)
                        throw RuntimeError("Invalid Chitubox layer parameter");
                    if (plane && integer(at + field) != integer(record(i) + field))
                        throw RuntimeError("Inconsistent Chitubox AA layer parameters");
                }
                payloads.push_back(range);
            }
        std::sort(payloads.begin(), payloads.end(), less);
        for (size_t i = 1; i < payloads.size(); ++i)
            if (payloads[i].begin < payloads[i - 1].end &&
                (payloads[i].begin != payloads[i - 1].begin || payloads[i].end != payloads[i - 1].end))
                throw RuntimeError("Chitubox layer payloads overlap");
    }
    std::vector<uint8_t> decode(size_t layer, const std::function<void()> &check = {}) const {
        if (check) check();
        if (layer >= count) throw RuntimeError("Chitubox layer index outside the table");
        if (!binary) {
            const auto at=record(layer);const auto data=integer(at+12),length=integer(at+16);
            std::vector<uint8_t> payload(bytes.begin()+data,bytes.begin()+data+length);
            ctb_encrypted::xor_layer(payload,uint32_t(layer),seed,check);
            return chitubox::decode_gray7(payload,size_t(columns)*rows,check);
        }
        std::vector<uint8_t> output(size_t(columns)*rows,0);
        for (unsigned plane=0;plane<planes;++plane) {
            const auto at=record(layer,plane);const auto data=integer(at+12),length=integer(at+16);
            size_t pixel=0,next_check=data;
            for (size_t cursor=data;cursor<size_t(data)+length;++cursor) {
                if (check && cursor>=next_check) {check();next_check=cursor+65536;}
                const auto token=bytes[cursor];const unsigned run=token&127;
                if (!run || run>output.size()-pixel) throw RuntimeError("Chitubox AA run exceeds raster dimensions");
                if (token&128) for (size_t n=0;n<run;++n) ++output[pixel+n];
                pixel+=run;
            }
            if (pixel!=output.size()) throw RuntimeError("Incomplete Chitubox AA mask");
        }
        for (size_t i=0;i<output.size();++i) {
            if (check && i%65536==0) check();
            auto &pixel=output[i];pixel=pixel?uint8_t(pixel*(256/planes)-1):0;
        }
        if (check) check();
        return output;
    }
    static uint8_t quantize(uint8_t value, bool binary, unsigned planes) {
        if (!binary)
            return value < 2 ? 0 : uint8_t(value | 1);
        const unsigned step = 256 / planes;
        if (value && (unsigned(value) + 1) % step == 0 && (unsigned(value) + 1) / step <= planes)
            return value;
        const unsigned count =
            value == 255 ? planes : std::min(planes - 1, (unsigned(value) + 1) / (256 / planes));
        return count ? uint8_t(count * (256 / planes) - 1) : 0;
    }
    // Recover plane counts from already quantized edited pixels; applying the
    // original threshold a second time would destroy white for non-power AA.
    std::vector<uint8_t> encode(const std::vector<uint8_t> &pixels, size_t layer, unsigned plane) const {
        if (pixels.size() != size_t(columns) * rows)
            throw RuntimeError("Edited Chitubox dimensions changed");
        if (!binary) {
            const auto raster = chitubox::Gray7RasterEncoder{}(pixels.data(), columns, rows, 1);
            const auto *begin = static_cast<const uint8_t *>(raster.data());
            std::vector<uint8_t> result(begin, begin + raster.size());
            if (seed)
                ctb_encrypted::xor_layer(result, uint32_t(layer), seed);
            return result;
        }
        auto white = [&](size_t i) {
            const unsigned count = pixels[i] ? (unsigned(pixels[i]) + 1) / (256 / planes) : 0;
            return plane ? count >= plane : count == planes;
        };
        std::vector<uint8_t> result;
        for (size_t i = 0; i < pixels.size();) {
            const bool exposed = white(i);
            size_t end = i + 1;
            while (end < pixels.size() && end - i < 125 && white(end) == exposed)
                ++end;
            result.push_back(uint8_t((exposed ? 128 : 0) | (end - i)));
            i = end;
        }
        return result;
    }
};
} // namespace Slic3r::sla
