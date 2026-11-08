// SPDX-License-Identifier: AGPL-3.0-or-later
// Internal packet: cache statistics beside PNG while unique masks are encoded.
// This header is never written into a NanoDLP ZIP image entry.
#ifndef SLIC3R_FORMAT_NANODLP_PACKET_HPP
#define SLIC3R_FORMAT_NANODLP_PACKET_HPP
#include "NanoDLPEncoder.hpp"
#include "NanoDLPContourStats.hpp"
#include <cstring>
#include <nlohmann/json.hpp>
namespace Slic3r::nanodlp {
inline sla::EncodedRaster encode_packet(const void *ptr,size_t width,size_t height,size_t components,ImageMode mode)
{
    // The PNG encoder validates dimensions/components before reading pixels.
    const auto png=encode_layer(ptr,width,height,components,mode);
    const auto stats=contour_statistics(static_cast<const uint8_t *>(ptr),width,height);
    const nlohmann::json metadata{{"width",width},{"height",height},{"rgb",mode==ImageMode::RGB},
        {"occupied_pixels",stats.occupied_pixels},{"total_area",stats.total_area},
        {"smallest_area",stats.smallest_area},{"largest_area",stats.largest_area},
        {"external_count",stats.external_count},{"bbox",{stats.min_x,stats.min_y,stats.max_x,stats.max_y}}};
    const auto json=metadata.dump();
    if(json.size()>65536)throw ExportError("NanoDLP raster metadata exceeds packet limit");
    std::vector<uint8_t> bytes{'N','D','P','1'};
    const auto length=uint32_t(json.size());
    for(int shift=24;shift>=0;shift-=8)bytes.push_back(uint8_t(length>>shift));
    bytes.insert(bytes.end(),json.begin(),json.end());
    const auto *data=static_cast<const uint8_t *>(png.data());
    bytes.insert(bytes.end(),data,data+png.size());
    return sla::EncodedRaster(std::move(bytes),"nanodlp-internal");
}
struct PacketView {
    nlohmann::json metadata;
    const uint8_t *png=nullptr;
    size_t png_size=0;
    ContourStats statistics() const {
        ContourStats s;
        s.occupied_pixels=metadata.at("occupied_pixels").get<uint64_t>();
        s.total_area=metadata.at("total_area").get<double>();
        s.smallest_area=metadata.at("smallest_area").get<double>();
        s.largest_area=metadata.at("largest_area").get<double>();
        s.external_count=metadata.at("external_count").get<uint32_t>();
        const auto &b=metadata.at("bbox");
        s.min_x=b.at(0).get<uint32_t>();s.min_y=b.at(1).get<uint32_t>();
        s.max_x=b.at(2).get<uint32_t>();s.max_y=b.at(3).get<uint32_t>();
        return s;
    }
};
inline PacketView inspect_packet(const sla::EncodedRaster &packet)
{
    const auto *bytes=static_cast<const uint8_t *>(packet.data());
    if(packet.size()<8 || std::memcmp(bytes,"NDP1",4)!=0)
        throw ExportError("Invalid NanoDLP raster packet header");
    const uint32_t length=uint32_t(bytes[4])<<24|uint32_t(bytes[5])<<16|uint32_t(bytes[6])<<8|bytes[7];
    if(length==0 || length>65536 || size_t(length)+8+33>packet.size())
        throw ExportError("Invalid NanoDLP raster packet size");
    PacketView view;
    view.metadata=nlohmann::json::parse(bytes+8,bytes+8+length,nullptr,false);
    view.png=bytes+8+length;view.png_size=packet.size()-8-length;
    if(!view.metadata.is_object() || std::memcmp(view.png,"\x89PNG\r\n\x1a\n",8)!=0)
        throw ExportError("Invalid NanoDLP raster packet payload");
    return view;
}
}
#endif
