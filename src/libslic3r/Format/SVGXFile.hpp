// SPDX-License-Identifier: AGPL-3.0-or-later
// Native wire schema ported from UVtools FlashForgeSVGXFile.cs. No runtime adapter.
#ifndef SLIC3R_SVGX_FILE_HPP
#define SLIC3R_SVGX_FILE_HPP
#include "SVGXContourPacket.hpp"
#include "libslic3r/GCode/ThumbnailData.hpp"
#include <fstream>
#include <iomanip>
#include <locale>
#include <string>
#include <algorithm>

namespace Slic3r::svgx {
struct FileParameters {
    uint32_t width=0,height=0;
    float display_width=0,display_height=0,machine_z=0,layer_height=0;
    uint16_t bottom_layers=0,transition_layers=0;
    float bottom_exposure=0,exposure=0;
    std::string machine="Unknown",material="Unknown";
};

// UVtools parses centred millimetres as float, adds half-display, multiplies
// by float pixels/mm and truncates. Pick the nearest coordinate that survives
// those operations, rather than rounding to .001 mm and shifting edges.
inline std::vector<float> pixel_coordinates(uint32_t resolution,float display) {
    if(!resolution||resolution>32768||!std::isfinite(display)||display<=0)
        throw ExportError("Invalid SVGX coordinate geometry");
    const float half=display/2.f,ppm=float(resolution)/display;
    if(!std::isfinite(ppm)||ppm<=0)throw ExportError("Invalid SVGX pixel pitch");
    std::vector<float> coordinates(resolution);
    for(uint32_t pixel=0;pixel<resolution;++pixel) {
        float absolute=float(pixel)/ppm,centred=absolute-half;
        auto decoded=[&](){return int(std::clamp(half+centred,0.f,display)*ppm);};
        // At most a few float ULPs are needed for the supported screen sizes.
        for(unsigned attempt=0;decoded()!=int(pixel)&&attempt<16;++attempt) {
            const float direction=decoded()<int(pixel)?INFINITY:-INFINITY;
            absolute=std::nextafter(half+centred,direction);
            const float next=absolute-half;
            centred=next==centred?std::nextafter(centred,direction):next;
        }
        if(!std::isfinite(centred)||decoded()!=int(pixel))
            throw ExportError("SVGX coordinates cannot preserve this screen's pixels");
        coordinates[pixel]=centred;
    }
    return coordinates;
}

inline std::string xml_attribute(const std::string &text) {
    std::string escaped;
    for(unsigned char c:text) {
        switch(c) {
        case '&': escaped+="&amp;";break;
        case '<': escaped+="&lt;";break;
        case '>': escaped+="&gt;";break;
        case '"': escaped+="&quot;";break;
        case '\'': escaped+="&apos;";break;
        default:
            if(c<32)throw ExportError("SVGX names cannot contain control characters");
            escaped+=char(c);
        }
    }
    return escaped;
}

inline float pixel_size_microns(float display,uint32_t resolution) {
    // Match the reference's MathF.Round(display/resolution * 1000, 3).
    const float value=std::nearbyint(display/resolution*1000.f*1000.f)/1000.f;
    if(!std::isfinite(value)||value<=0)throw ExportError("SVGX pixel size is below the reference metadata precision");
    return value;
}

// ThumbnailData and BMP both store bottom-up rows. Convert RGBA to BGR,
// preserving that order; BMP rows are padded to a four-byte boundary.
inline std::vector<uint8_t> bmp_preview(uint32_t width,uint32_t height,const ThumbnailsList &thumbnails) {
    if(!width||!height||width>4096||height>4096)throw ExportError("Invalid SVGX preview dimensions");
    const uint32_t stride=(width*3+3)&~uint32_t(3),size=stride*height;
    std::vector<uint8_t> bytes(54+size,0);
    auto u32=[&](size_t offset,uint32_t value){for(unsigned i=0;i<4;++i)bytes[offset+i]=uint8_t(value>>(i*8));};
    bytes[0]='B';bytes[1]='M';u32(2,uint32_t(bytes.size()));u32(10,54);u32(14,40);
    u32(18,width);u32(22,height);u32(26,0x180001);u32(34,size);u32(38,3780);u32(42,3780);
    const ThumbnailData *source=nullptr;
    for(const auto &thumbnail:thumbnails)
        if(thumbnail.width<=4096&&thumbnail.height<=4096&&thumbnail.is_valid()){source=&thumbnail;break;}
    if(source) {
        const double scale=std::min(double(width)/source->width,double(height)/source->height);
        const uint32_t image_width=std::max(1u,uint32_t(std::lround(source->width*scale)));
        const uint32_t image_height=std::max(1u,uint32_t(std::lround(source->height*scale)));
        const uint32_t left=(width-image_width)/2,bottom=(height-image_height)/2;
        for(uint32_t y=0;y<image_height;++y)for(uint32_t x=0;x<image_width;++x) {
            const size_t source_at=4*(size_t(uint64_t(y)*source->height/image_height)*source->width+
                                       uint64_t(x)*source->width/image_width);
            const size_t destination=54+size_t(bottom+y)*stride+3*(left+x);
            for(unsigned channel=0;channel<3;++channel)
                bytes[destination+channel]=uint8_t((unsigned(source->pixels[source_at+2-channel])*source->pixels[source_at+3]+127)/255);
        }
    }
    return bytes;
}

// The wire writer is separate from profile validation: SVGX has only global
// exposure and uniform layer height, and no motion/wait/PWM fields.
inline void write_file(const std::string &filename,const FileParameters &p,
                       const std::vector<sla::EncodedRaster> &layers,const ThumbnailsList &thumbnails) {
    if(layers.empty()||layers.size()>UINT32_MAX||p.bottom_layers>layers.size()||
       uint64_t(p.width)*p.height>(uint64_t(128)<<20))throw ExportError("Invalid SVGX layer stack");
    for(float value:{p.machine_z,p.layer_height,p.bottom_exposure,p.exposure})
        if(!std::isfinite(value)||value<0)throw ExportError("Invalid SVGX print parameter");
    if(p.layer_height<=0||p.machine_z<=0||double(layers.size())*p.layer_height>p.machine_z+1e-5)
        throw ExportError("SVGX layers exceed machine height");
    const auto machine=xml_attribute(p.machine),material=xml_attribute(p.material);
    const auto xs=pixel_coordinates(p.width,p.display_width),ys=pixel_coordinates(p.height,p.display_height);
    const float pixel_um=std::max(pixel_size_microns(p.display_width,p.width),pixel_size_microns(p.display_height,p.height));
    double volume=0;float minx=0,miny=0,maxx=0,maxy=0;
    // Validation precedes opening the output. Only one decoded contour packet
    // is retained at a time; no second full raster or full-document XML buffer.
    for(const auto &encoded:layers) {
        const auto packet=decode_contours(encoded);
        if(packet.width!=p.width||packet.height!=p.height)throw ExportError("SVGX mask dimensions differ from printer");
        volume+=double(packet.occupied_pixels)*p.display_width/p.width*p.display_height/p.height*p.layer_height/1000.;
        for(const auto &c:packet.contours)for(const auto &[x,y]:c.points) {
            minx=std::min(minx,xs[x]);miny=std::min(miny,ys[y]);maxx=std::max(maxx,xs[x]);maxy=std::max(maxy,ys[y]);
        }
    }
    if(!std::isfinite(float(volume)))throw ExportError("SVGX material volume overflow");
    const auto small=bmp_preview(128,128,thumbnails),large=bmp_preview(200,240,thumbnails);
    std::vector<uint8_t> header(28,0);std::memcpy(header.data(),"DLP-II 1.1\n",11);
    auto u32=[&](size_t offset,uint32_t value){for(unsigned i=0;i<4;++i)header[offset+i]=uint8_t(value>>(i*8));};
    u32(16,28);u32(20,28+uint32_t(small.size()));u32(24,28+uint32_t(small.size()+large.size()));
    std::ofstream out(filename,std::ios::binary|std::ios::trunc);
    if(!out)throw ExportError("Cannot open SVGX output");
    out.imbue(std::locale::classic());out<<std::setprecision(std::numeric_limits<float>::max_digits10);
    for(const auto *bytes:std::initializer_list<const std::vector<uint8_t> *>{&header,&small,&large})
        out.write(reinterpret_cast<const char *>(bytes->data()),bytes->size());
    out<<"<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<svg version=\"1.1\" xmlns=\"http://www.w3.org/2000/svg\">\n"
       <<"<printparams machinename=\""<<machine<<"\" materialname=\""<<material<<"\" layerheight=\""<<p.layer_height
       <<"\" volume=\""<<float(volume)<<"\" layercount=\""<<layers.size()<<"\" lightintensity=\"1\" resolutionx=\""<<p.width
       <<"\" resolutiony=\""<<p.height<<"\" displaywidth=\""<<p.display_width<<"\" displayheight=\""<<p.display_height
       <<"\" machinez=\""<<p.machine_z<<"\">\n<projectiontime attachlayer=\""<<p.bottom_layers
       <<"\" buildinlayer=\""<<p.transition_layers<<"\" attachtime=\""<<p.bottom_exposure<<"\" basetime=\""<<p.exposure
       <<"\"/>\n<projectionadjust x=\"100\" y=\"100\"/>\n<printrange minx=\""<<minx<<"\" miny=\""<<miny
       <<"\" minz=\"0\" maxx=\""<<maxx<<"\" maxy=\""<<maxy<<"\" maxz=\""<<float(layers.size()*double(p.layer_height))
       <<"\"/>\n</printparams>\n<g id=\"background\"></g>\n";
    for(size_t i=0;i<layers.size();++i) {
        const auto packet=decode_contours(layers[i]);float area=0,perimeter=0;
        // Original OpenCV roots appear in reverse scan order; its loop leaves
        // metadata for the last external root (our first), not a total area.
        for(const auto &c:packet.contours)if(c.parent<0) {
            area=float(std::nearbyint(std::cbrt(c.area/pixel_um)*1000.)/1000.);
            perimeter=float(std::nearbyint(c.perimeter/pixel_um*1000.)/1000.);break;
        }
        out<<"<g id=\"layer-"<<i<<"\" area=\""<<area<<"\" perimeter=\""<<perimeter<<"\">\n";
        if(!packet.contours.empty()) {
            out<<"<path style=\"fill:white\" fill-rule=\"evenodd\" d=\"";
            for(const auto &c:packet.contours) {
                const auto &[x,y]=c.points.front();out<<"M "<<xs[x]<<' '<<ys[y]<<" L";
                for(size_t n=1;n<c.points.size();++n)out<<' '<<xs[c.points[n].first]<<' '<<ys[c.points[n].second];
                out<<" Z ";
            }
            out<<"\"/>\n";
        }
        out<<"</g>\n";
    }
    out<<"</svg>\n";out.flush();if(!out)throw ExportError("Cannot finish SVGX output");
}
}
#endif
