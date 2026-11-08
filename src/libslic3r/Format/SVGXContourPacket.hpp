// SPDX-License-Identifier: AGPL-3.0-or-later
// Private cached raster payload; this is not the on-disk SVGX file format.
// LE layout: SGX1 + width/height + uint64 occupied + contour count (24 B).
// Each contour: parent/root/hole, float64 area/perimeter, point count (32 B),
// followed by pairs of uint32 pixel coordinates. No PNG or XML decoding needed.
#ifndef SLIC3R_SVGX_CONTOUR_PACKET_HPP
#define SLIC3R_SVGX_CONTOUR_PACKET_HPP
#include "libslic3r/SLA/PixelContourTree.hpp"
#include "libslic3r/SLA/RasterBase.hpp"
#include <cstring>
#include <limits>
namespace Slic3r::svgx {
struct ContourPacket {
    uint32_t width=0,height=0;
    uint64_t occupied_pixels=0;
    std::vector<sla::PixelContour> contours;
};
inline sla::EncodedRaster encode_contours(const void *pixels,size_t width,size_t height,size_t components) {
    if(components!=1)throw ExportError("SVGX contours require a single-channel mask");
    std::vector<sla::PixelContour> contours;
    const auto stats=sla::trace_pixel_contours(static_cast<const uint8_t *>(pixels),width,height,&contours);
    std::vector<uint8_t> bytes{'S','G','X','1'};
    auto u32=[&](uint32_t value){for(unsigned i=0;i<4;++i)bytes.push_back(uint8_t(value>>(i*8)));};
    auto scalar=[&](double value){static_assert(sizeof(double)==8);uint64_t bits;std::memcpy(&bits,&value,8);u32(uint32_t(bits));u32(uint32_t(bits>>32));};
    u32(uint32_t(width));u32(uint32_t(height));u32(uint32_t(stats.occupied_pixels));u32(uint32_t(stats.occupied_pixels>>32));
    u32(uint32_t(contours.size()));
    for(const auto &c:contours) {
        if(c.points.size()>UINT32_MAX)throw ExportError("SVGX contour exceeds its point-count field");
        u32(uint32_t(c.parent));u32(uint32_t(c.root));u32(c.hole?1:0);scalar(c.area);scalar(c.perimeter);u32(uint32_t(c.points.size()));
        for(const auto &[x,y]:c.points){u32(uint32_t(x));u32(uint32_t(y));}
    }
    return sla::EncodedRaster(std::move(bytes),"svgx-contours");
}
inline ContourPacket decode_contours(const sla::EncodedRaster &raster) {
    const auto *data=static_cast<const uint8_t *>(raster.data());size_t offset=4;
    if(raster.size()<24 || std::memcmp(data,"SGX1",4))throw ExportError("Invalid SVGX contour packet");
    auto u32=[&](){if(raster.size()-offset<4)throw ExportError("Truncated SVGX contour packet");uint32_t value=0;for(unsigned i=0;i<4;++i)value|=uint32_t(data[offset++])<<(i*8);return value;};
    auto scalar=[&](){const uint64_t low=u32(),high=u32(),bits=low|(high<<32);double value;std::memcpy(&value,&bits,8);if(!std::isfinite(value)||value<0)throw ExportError("Invalid SVGX contour metric");return value;};
    ContourPacket packet;packet.width=u32();packet.height=u32();
    const uint64_t low=u32(),high=u32();packet.occupied_pixels=low|(high<<32);
    if(!packet.width||!packet.height||packet.width>32768||packet.height>32768||
       uint64_t(packet.width)*packet.height>(uint64_t(128)<<20)||packet.occupied_pixels>uint64_t(packet.width)*packet.height)
        throw ExportError("Invalid SVGX contour geometry");
    const uint32_t count=u32();if(count>(raster.size()-offset)/40 || count>2ULL*packet.width*packet.height)throw ExportError("Invalid SVGX contour count");
    packet.contours.reserve(count);
    for(uint32_t i=0;i<count;++i) {
        sla::PixelContour c;const uint32_t parent=u32(),root=u32(),hole=u32();
        if((parent!=UINT32_MAX&&parent>=count)||root>=count||hole>1)throw ExportError("Invalid SVGX contour relationship");
        c.parent=parent==UINT32_MAX?-1:int32_t(parent);c.root=int32_t(root);c.hole=hole!=0;c.area=scalar();c.perimeter=scalar();
        const auto points=u32();if(!points||points>(raster.size()-offset)/8)throw ExportError("Invalid SVGX point count");
        c.points.reserve(points);
        for(uint32_t n=0;n<points;++n){const uint32_t x=u32(),y=u32();if(x>=packet.width||y>=packet.height)throw ExportError("SVGX point outside mask");c.points.emplace_back(int(x),int(y));}
        packet.contours.push_back(std::move(c));
    }
    if(offset!=raster.size()||(packet.occupied_pixels==0)!=(count==0))throw ExportError("Unexpected SVGX contour packet data");
    // Linear tree validation also rejects cycles and inconsistent root/hole flags.
    std::vector<uint8_t> state(count,0);std::vector<uint32_t> path;
    for(uint32_t start=0;start<count;++start) {
        uint32_t current=start;path.clear();
        while(state[current]==0) {
            state[current]=1;path.push_back(current);
            const auto parent=packet.contours[current].parent;
            if(parent<0)break;
            current=uint32_t(parent);
        }
        if(packet.contours[path.empty()?current:path.back()].parent>=0 && state[current]==1)
            throw ExportError("Cyclic SVGX contour tree");
        for(auto it=path.rbegin();it!=path.rend();++it) {
            const auto &c=packet.contours[*it];
            if(c.parent<0) {if(c.root!=int32_t(*it)||c.hole)throw ExportError("Invalid SVGX root");}
            else {const auto &p=packet.contours[size_t(c.parent)];if(c.root!=p.root||c.hole==p.hole)throw ExportError("Invalid SVGX contour ancestry");}
            state[*it]=2;
        }
    }
    return packet;
}
}
#endif
