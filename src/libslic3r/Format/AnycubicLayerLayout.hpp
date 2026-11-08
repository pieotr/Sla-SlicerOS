// Native table/mask reader for local UVtools AnycubicFile versions 1–518.
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "AnycubicSLA.hpp"
#include "libslic3r/Exception.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <optional>
#include <vector>

namespace Slic3r {
struct AnycubicLayerLayout {
    const std::vector<uint8_t> &bytes;
    uint32_t version, header, table, count, columns, rows, planes, subtable = 0, model = 0;
    bool binary;
    void require(size_t at, uint64_t length) const {
        if (at > bytes.size() || length > bytes.size() - at)
            throw RuntimeError("Anycubic section lies outside the file");
    }
    uint32_t integer(size_t at) const {
        require(at, 4);
        return uint32_t(bytes[at]) | (uint32_t(bytes[at+1])<<8) |
               (uint32_t(bytes[at+2])<<16) | (uint32_t(bytes[at+3])<<24);
    }
    float number(size_t at) const {
        const auto word = integer(at); float value; std::memcpy(&value, &word, 4); return value;
    }
    void named(size_t at, const char *name, size_t minimum) const {
        require(at, minimum);
        if (std::memcmp(bytes.data()+at, name, std::strlen(name)) != 0)
            throw RuntimeError("Invalid Anycubic table marker");
    }
    size_t record(size_t i) const { return size_t(table) + 20 + i * 32; }
    size_t subrecord(size_t i) const { return size_t(subtable) + 24 + i * 44; }
    // Physical area cannot be inferred from the scalar header pitch for an
    // anisotropic LCD. Prefer MACHINE, or the format's fixed legacy device.
    std::optional<std::pair<double, double>> physical_dimensions() const {
        double width = 0, height = 0;
        if (version >= 516) {
            const auto machine = integer(44);
            named(machine, "MACHINE", 148);
            width = number(machine+136); height = number(machine+140);
        } else if (const auto *geometry = anycubic_legacy_geometry(extension)) {
            width = geometry->width; height = geometry->height;
        } else return std::nullopt;
        if (!std::isfinite(width) || !std::isfinite(height) || width <= 0 || height <= 0)
            throw RuntimeError("Cannot calculate Anycubic resin volume without physical screen dimensions");
        return std::pair{width, height};
    }
    double pixel_area() const {
        const auto dimensions = physical_dimensions();
        if (!dimensions) throw RuntimeError("Cannot calculate Anycubic resin volume without physical screen dimensions");
        return dimensions->first*dimensions->second/(double(columns)*rows);
    }
    explicit AnycubicLayerLayout(const std::vector<uint8_t> &source, const std::string &format,
                                const std::function<void()> &check = {}) : bytes(source), binary(format == "pws"), extension(format) {
        require(0, 48);
        if (std::memcmp(bytes.data(), "ANYCUBIC", 8) != 0) throw RuntimeError("Not an Anycubic archive");
        version = integer(12);
        const auto &versions = anycubic_available_versions(format);
        if (std::find(versions.begin(), versions.end(), version) == versions.end())
            throw RuntimeError("Anycubic version does not match the filename extension");
        header = integer(20); table = integer(36);
        named(header, "HEADER", 96); named(table, "LAYERDEF", 20);
        columns = integer(header+60); rows = integer(header+64); planes = integer(header+56);
        count = integer(table+16);
        if (!columns || !rows || columns > 32768 || rows > 32768 ||
            uint64_t(columns)*rows > (uint64_t(128)<<20) || !count || count > 100000 || !planes || planes > 16)
            throw RuntimeError("Anycubic dimensions, count or AA exceed editor limits");
        require(table, 20 + uint64_t(count)*32);
        if (version >= 516) {
            require(0, version >= 518 ? 64 : version >= 517 ? 56 : 52);
            if (version >= 517 && integer(52)) { model = integer(52); named(model, "MODEL", 48); }
            if (version >= 518 && integer(56)) {
                subtable = integer(56); named(subtable, "SUBIMGS", 24);
                if (integer(subtable+16) != count) throw RuntimeError("Anycubic subimage count differs from layers");
                require(subtable, 24 + uint64_t(count)*44);
            }
        }
        const auto image_begin = integer(version >= 516 ? 48 : 44);
        require(image_begin, 0);
        auto overlaps = [](size_t begin, size_t end, size_t metadata_begin, size_t metadata_end) {
            return begin < metadata_end && metadata_begin < end;
        };
        struct Range { size_t begin,end; };
        std::vector<Range> payloads;
        auto validate_mask=[&](size_t at) {
            const size_t begin=integer(at),length=integer(at+4);require(begin,length);
            const size_t end=begin+length;
            if (!length || (image_begin && begin<image_begin) ||
                overlaps(begin,end,0,version>=518?64:version>=517?56:version>=516?52:48) ||
                overlaps(begin,end,header,size_t(header)+96) ||
                overlaps(begin,end,table,size_t(table)+20+size_t(count)*32) ||
                (model && overlaps(begin,end,model,size_t(model)+48)) ||
                (subtable && overlaps(begin,end,subtable,size_t(subtable)+24+size_t(count)*44)))
                throw RuntimeError("Anycubic mask overlaps metadata or has no data");
            payloads.push_back({begin,end});
        };
        for (size_t i=0;i<count;++i) {
            if (check) check();
            const auto at=record(i);validate_mask(at);
            if (!std::isfinite(number(at+20)) || number(at+20)<=0 ||
                !std::isfinite(number(at+16)) || number(at+16)<0)
                throw RuntimeError("Invalid Anycubic layer height or exposure");
            if (subtable) {
                const auto sub=subrecord(i);validate_mask(sub);
                if (integer(sub+8)>uint64_t(columns)*rows) throw RuntimeError("Invalid Anycubic subimage pixel count");
            }
        }
        std::sort(payloads.begin(),payloads.end(),[](const Range &a,const Range &b) {
            return a.begin<b.begin || (a.begin==b.begin && a.end<b.end);
        });
        for(size_t i=1;i<payloads.size();++i)
            if(payloads[i].begin<payloads[i-1].end &&
               (payloads[i].begin!=payloads[i-1].begin || payloads[i].end!=payloads[i-1].end))
                throw RuntimeError("Anycubic mask payloads overlap");

    }
    uint8_t quantize(uint8_t value) const {
        if (!binary) return uint8_t((value >> 4) * 17);
        // PWS is a sum of AA bitplanes, then integer 256/AA scaling minus one.
        const unsigned level = unsigned(std::lround(double(value)*planes/255));
        return level ? uint8_t(level*(256/planes)-1) : 0;
    }
    bool subimage_is_shared(size_t layer) const {
        if(layer>=count)throw RuntimeError("Anycubic layer index outside table");
        return subtable && integer(subrecord(layer))==integer(record(layer)) &&
               integer(subrecord(layer)+4)==integer(record(layer)+4);
    }
    std::vector<uint8_t> decode(size_t layer,const std::function<void()> &check={}) const {
        if(layer>=count)throw RuntimeError("Anycubic layer index outside table");
        return decode_record(record(layer),check);
    }
    std::vector<uint8_t> decode_subimage(size_t layer,const std::function<void()> &check={}) const {
        if(!subtable || layer>=count)throw RuntimeError("Anycubic subimage outside table");
        return decode_record(subrecord(layer),check);
    }
    std::vector<uint8_t> decode_record(size_t at,const std::function<void()> &check) const {
        size_t cursor=integer(at),end=cursor+integer(at+4);
        std::vector<uint8_t> pixels(size_t(columns)*rows, 0);
        for (unsigned plane = 0; plane < (binary ? planes : 1); ++plane) {
            size_t position = 0;
            while (position < pixels.size()) {
                if (check) check();
                if (cursor >= end) throw RuntimeError("Truncated Anycubic mask");
                const uint8_t byte = bytes[cursor++];
                unsigned run = binary ? byte & 127 : byte & 15;
                const uint8_t color = binary ? byte >> 7 : byte >> 4;
                if (!binary && (color == 0 || color == 15)) {
                    if (cursor >= end) throw RuntimeError("Truncated Anycubic long run");
                    run = (run << 8) | bytes[cursor++];
                }
                if (!run || run > pixels.size()-position) throw RuntimeError("Anycubic run exceeds the layer");
                if (binary) {
                    if (color) for (size_t i = position; i < position+run; ++i) ++pixels[i];
                } else std::fill(pixels.begin()+position, pixels.begin()+position+run, uint8_t(color*17));
                position += run;
            }
        }
        if (cursor != end) throw RuntimeError("Unexpected bytes after Anycubic mask planes");
        if (binary) for (auto &pixel : pixels) pixel = pixel ? uint8_t(pixel*(256/planes)-1) : 0;
        return pixels;
    }
    std::vector<uint8_t> encode(const std::vector<uint8_t> &pixels, const std::function<void()> &check = {}) const {
        if (pixels.size() != size_t(columns)*rows) throw RuntimeError("Edited Anycubic dimensions changed");
        std::vector<uint8_t> output;
        for (unsigned plane = 0; plane < (binary ? planes : 1); ++plane) {
            auto color = [&](size_t i) { return binary ? unsigned((unsigned(pixels[i])+1)/(256/planes) > plane) : unsigned(pixels[i]>>4); };
            for (size_t at = 0; at < pixels.size();) {
                if (check) check();
                const unsigned value = color(at), limit = binary ? 127 : value == 0 || value == 15 ? 4095 : 15;
                size_t run = 1;
                while (run < limit && at+run < pixels.size() && color(at+run) == value) ++run;
                if (binary) output.push_back(uint8_t((value ? 128 : 0) | run));
                else if (value == 0 || value == 15) { output.push_back(uint8_t((value<<4) | (run>>8))); output.push_back(uint8_t(run)); }
                else output.push_back(uint8_t((value<<4) | run));
                at += run;
            }
        }
        return output;
    }
private:
    std::string extension;
};
}
