// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "CrealityChecksum.hpp"
#include "libslic3r/Exception.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace Slic3r::sla {
// Addressed v4 sections differ completely from the sequential v1/v2/v3
// tables. Keep a validated layout so an editor can relocate payloads while
// preserving the original previews, settings, extended parameters and gaps.
struct CrealityCXDLPv4Layout {
    struct Range {size_t start,end;};
    struct Layer {uint32_t data,length;float z;};
    const std::vector<uint8_t> &bytes;
    size_t base=0,table=0;
    uint32_t columns=0,rows=0,seed=0;
    double width=0,height=0;
    std::vector<Layer> layers;
    std::vector<Range> sections;

    void require(size_t offset,uint64_t length) const {
        if(bytes.size()<4 || offset>bytes.size()-4 || length>bytes.size()-4-offset)
            throw RuntimeError("CXDLPv4 section lies outside the file");
    }
    uint32_t integer(size_t offset) const {
        require(offset,4);const auto *p=bytes.data()+offset;
        return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);
    }
    float number(size_t offset) const {
        const uint32_t bits=integer(offset);float value;std::memcpy(&value,&bits,4);return value;
    }
    void section(size_t offset,uint64_t length) {
        require(offset,length);sections.push_back({offset,offset+size_t(length)});
    }
    explicit CrealityCXDLPv4Layout(const std::vector<uint8_t> &source):bytes(source) {
        require(0,19);
        if(std::memcmp(bytes.data(),"\0\0\0\x09" "CXSW3DV2\0\0\x04",15)!=0)
            throw RuntimeError("Not a CXDLPv4 header");
        const auto *model=bytes.data()+15;
        const uint32_t length=(uint32_t(model[0])<<24)|(uint32_t(model[1])<<16)|(uint32_t(model[2])<<8)|model[3];
        if(!length || length>256)throw RuntimeError("Invalid CXDLPv4 printer model length");
        require(19,length+80);
        if(bytes[19+length-1]!=0)throw RuntimeError("Invalid CXDLPv4 printer model terminator");
        base=19+length;section(0,base+80);
        columns=uint16_t(bytes[base])|(uint16_t(bytes[base+1])<<8);
        rows=uint16_t(bytes[base+2])|(uint16_t(bytes[base+3])<<8);
        if(!columns || !rows || columns>32768 || rows>32768 || uint64_t(columns)*rows>(uint64_t(128)<<20))
            throw RuntimeError("CXDLPv4 resolution exceeds the raster limits");
        width=number(base+4);height=number(base+8);
        if(!std::isfinite(width) || !std::isfinite(height) || width<=0 || height<=0)
            throw RuntimeError("Invalid CXDLPv4 physical screen dimensions");
        const auto *checksum=bytes.data()+bytes.size()-4;
        const uint32_t stored=(uint32_t(checksum[0])<<24)|(uint32_t(checksum[1])<<16)|(uint32_t(checksum[2])<<8)|checksum[3];
        if(creality_checksum(bytes.data(),bytes.size()-4,4)!=stored)throw RuntimeError("CXDLPv4 checksum mismatch");
        table=integer(base+32);const uint32_t count=integer(base+36);seed=integer(base+68);
        if(!count || count>100000)throw RuntimeError("Invalid CXDLPv4 layer count");
        section(table,uint64_t(count)*40);
        for(const size_t field:{base+28,base+40}) {
            const auto preview=integer(field);if(!preview)continue;
            section(preview,32);
            const uint32_t x=integer(preview),y=integer(preview+4),data=integer(preview+8),size=integer(preview+12);
            if(!x || !y || x>4096 || y>4096 || data<preview+32)
                throw RuntimeError("Invalid CXDLPv4 preview header");
            section(data,size);
            uint64_t decoded=0;
            for(size_t cursor=data;cursor<size_t(data)+size;) {
                if(data+uint64_t(size)-cursor<2)throw RuntimeError("Truncated CXDLPv4 preview pixel");
                const uint16_t color=uint16_t(bytes[cursor])|(uint16_t(bytes[cursor+1])<<8);cursor+=2;
                uint32_t run=1;
                if(color&0x20) {
                    if(data+uint64_t(size)-cursor<2)throw RuntimeError("Truncated CXDLPv4 preview run");
                    run+=uint32_t(bytes[cursor])|((uint32_t(bytes[cursor+1])&15)<<8);cursor+=2;
                }
                decoded+=run;if(decoded>uint64_t(x)*y)throw RuntimeError("CXDLPv4 preview exceeds its dimensions");
            }
            if(decoded!=uint64_t(x)*y)throw RuntimeError("Incomplete CXDLPv4 preview");
        }
        if(const auto parameters=integer(base+52)) {
            const auto size=integer(base+56);if(size<68)throw RuntimeError("Incomplete CXDLPv4 print parameters");section(parameters,size);
        }
        if(const auto slicer=integer(base+72)) {
            const auto size=integer(base+76);if(size<76)throw RuntimeError("Incomplete CXDLPv4 slicer information");section(slicer,size);
        }
        std::sort(sections.begin(),sections.end(),[](const Range &a,const Range &b){return a.start<b.start || (a.start==b.start && a.end<b.end);});
        for(size_t i=1;i<sections.size();++i)
            if(sections[i].start<sections[i-1].end && (sections[i].start!=sections[i-1].start || sections[i].end!=sections[i-1].end))
                throw RuntimeError("CXDLPv4 metadata sections overlap");
        layers.reserve(count);
        for(size_t i=0;i<count;++i) {
            const size_t row=table+40*i;const uint32_t data=integer(row+12),size=integer(row+16);
            if(size<44 || integer(row+20)!=0)throw RuntimeError("Unsupported CXDLPv4 layer type or incomplete parameters");
            require(data,size);
            for(const auto &metadata:sections)
                if(data<metadata.end && uint64_t(data)+size>metadata.start)
                    throw RuntimeError("CXDLPv4 layer overlaps a metadata section");
            const float z=number(row);
            if(!std::isfinite(z) || z<0)throw RuntimeError("Invalid CXDLPv4 layer Z");
            for(const size_t offset:{row+4,row+8})
                if(!std::isfinite(number(offset)) || number(offset)<0)throw RuntimeError("Invalid CXDLPv4 layer exposure or delay");
            for(unsigned field=0;field<11;++field) {
                const float value=number(data+4*field);
                if(!std::isfinite(value) || value<0 || (field==10 && value>255))
                    throw RuntimeError("Invalid CXDLPv4 extended layer parameter");
            }
            layers.push_back({data,size,z});
        }
        std::vector<Range> payloads;
        for(const auto &layer:layers)payloads.push_back({layer.data,size_t(layer.data)+layer.length});
        std::sort(payloads.begin(),payloads.end(),[](const Range &a,const Range &b){return a.start<b.start || (a.start==b.start && a.end<b.end);});
        for(size_t i=1;i<payloads.size();++i)
            if(payloads[i].start<payloads[i-1].end && (payloads[i].start!=payloads[i-1].start || payloads[i].end!=payloads[i-1].end))
                throw RuntimeError("CXDLPv4 payload ranges overlap");
    }
};
} // namespace Slic3r::sla
