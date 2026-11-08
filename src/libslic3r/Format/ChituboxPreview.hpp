// UVtools RGB15 previews shared by PHZ and Creality CXDLPv4 (AGPLv3).
#pragma once
#include "libslic3r/GCode/ThumbnailData.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>
namespace Slic3r {
// Original UVtools RGB15 runs: bit 5 marks a following 12-bit repeat
// count (run minus one). Split before 4096; two pixels cost the same as a run.
inline std::vector<uint8_t> compress_chitubox_rgb15(const std::vector<uint8_t> &raw)
{
    std::vector<uint8_t> encoded;encoded.reserve(raw.size());
    auto word=[&](uint16_t value){encoded.push_back(uint8_t(value));encoded.push_back(uint8_t(value>>8));};
    for(size_t cursor=0;cursor<raw.size();) {
        const uint16_t color=uint16_t(raw[cursor])|(uint16_t(raw[cursor+1])<<8);
        size_t run=1;
        while(run<4095 && cursor+2*run<raw.size() && raw[cursor+2*run]==uint8_t(color) &&
              raw[cursor+2*run+1]==uint8_t(color>>8))++run;
        if(run<=2)for(size_t i=0;i<run;++i)word(color);
        else {word(color|0x20);word(uint16_t((run-1)|0x3000));}
        cursor+=2*run;
    }
    return encoded;
}

// UVtools RGB15 reserves bit 5 for the RLE marker, BETWEEN green and
// blue. Raw pixels must clear it: RRRRRGGGGG0BBBBB, little endian.
// Keep the aspect ratio and convert bottom-up renderer RGBA over black.
inline std::vector<uint8_t> encode_chitubox_rgb15(uint32_t width,uint32_t height,const ThumbnailsList &thumbnails)
{
    std::vector<uint8_t> output(size_t(width)*height*2,0);
    const auto source=std::find_if(thumbnails.begin(),thumbnails.end(),[](const ThumbnailData &image){return image.is_valid() && image.width<=4096 && image.height<=4096;});
    if(source==thumbnails.end()) return compress_chitubox_rgb15(output);
    const double scale=std::min(double(width)/source->width,double(height)/source->height);
    const size_t image_width=std::max<size_t>(1,std::lround(source->width*scale)),image_height=std::max<size_t>(1,std::lround(source->height*scale));
    const size_t left=(width-image_width)/2,top=(height-image_height)/2;
    for(size_t y=0;y<image_height;++y)for(size_t x=0;x<image_width;++x) {
        const size_t sx=std::min<size_t>(source->width-1,x*source->width/image_width);
        const size_t sy=source->height-1-std::min<size_t>(source->height-1,y*source->height/image_height);
        const auto *rgba=source->pixels.data()+4*(sy*source->width+sx);
        const unsigned r=(unsigned(rgba[0])*rgba[3]+127)/255,g=(unsigned(rgba[1])*rgba[3]+127)/255,b=(unsigned(rgba[2])*rgba[3]+127)/255;
        const uint16_t pixel=uint16_t(((r>>3)<<11)|((g>>3)<<6)|(b>>3));
        const size_t offset=2*((top+y)*width+left+x);
        output[offset]=uint8_t(pixel);output[offset+1]=uint8_t(pixel>>8);
    }
    return compress_chitubox_rgb15(output);
}

} // namespace Slic3r
