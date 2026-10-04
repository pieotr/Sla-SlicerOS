///|/ Copyright (c) Prusa Research 2026
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "CrealityCXDLPv1Archive.hpp"
#include "libslic3r/SLAPrint.hpp"
#include "libslic3r/SLA/PrinterRaster.hpp"
#include "libslic3r/SLA/PrintParameters.hpp"
#include <boost/nowide/fstream.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <sstream>

namespace Slic3r {
namespace {
void write16(std::ostream &output,uint16_t value) {
    const char bytes[]{char(value>>8),char(value)};output.write(bytes,2);
}
void write32(std::ostream &output,uint32_t value) {
    const char bytes[]{char(value>>24),char(value>>16),char(value>>8),char(value)};output.write(bytes,4);
}
uint32_t read32(const uint8_t *bytes) {
    return(uint32_t(bytes[0])<<24)|(uint32_t(bytes[1])<<16)|(uint32_t(bytes[2])<<8)|bytes[3];
}
uint16_t integer(double value,const char *name,bool positive=false) {
    if(!std::isfinite(value) || value<(positive?1.:0.) || value>UINT16_MAX || std::abs(value-std::round(value))>1e-6)
        throw ExportError(std::string("CXDLPv1 requires a ")+(positive?"positive ":"")+"16-bit integer for "+name);
    return uint16_t(std::llround(value));
}
void number_string(std::ostream &output,double value) {
    std::ostringstream text;text.imbue(std::locale::classic());text<<std::setprecision(12)<<value;
    const auto string=text.str();write32(output,uint32_t(string.size()*2));
    for(const char c:string) write16(output,uint8_t(c)); // Numeric ASCII in UTF-16BE, no terminator.
}
} // namespace
sla::EncodedRaster CrealityCXDLPv1Archive::encode_mask(const void *data,size_t width,size_t height,size_t components) {
    if(components!=1 || width>16384 || height>8192 || !width || !height)
        throw ExportError("CXDLPv1 mask exceeds its 14-bit X / 13-bit Y coordinates");
    const auto *pixels=static_cast<const uint8_t *>(data);
    std::vector<uint8_t> bytes(8,0);uint32_t nonzero=0,lines=0;
    for(size_t x=0;x<width;++x) for(size_t y=0;y<height;) {
        const uint8_t gray=pixels[y*width+x];
        if(!gray){++y;continue;}
        const size_t start=y++;
        while(y<height && pixels[y*width+x]==gray) ++y;
        const size_t end=y-1;nonzero+=uint32_t(y-start);++lines;
        bytes.insert(bytes.end(),{uint8_t(start>>5),uint8_t((start<<3)|(end>>10)),
            uint8_t(end>>2),uint8_t((end<<6)|(x>>8)),uint8_t(x),gray});
    }
    for(unsigned i=0;i<4;++i){bytes[i]=uint8_t(nonzero>>(24-8*i));bytes[i+4]=uint8_t(lines>>(24-8*i));}
    return sla::EncodedRaster(std::move(bytes),"cxdlp");
}
namespace {
void preview(std::ostream &output,unsigned size,const ThumbnailData *source) {
    std::vector<uint16_t> pixels(size_t(size)*size,0);
    if(source) {
        const double scale=std::min(double(size)/source->width,double(size)/source->height);
        const size_t width=std::max<size_t>(1,std::lround(source->width*scale)),height=std::max<size_t>(1,std::lround(source->height*scale));
        const size_t left=(size-width)/2,top=(size-height)/2;
        for(size_t y=0;y<height;++y) for(size_t x=0;x<width;++x) {
            const size_t sx=std::min<size_t>(source->width-1,x*source->width/width);
            const size_t sy=source->height-1-std::min<size_t>(source->height-1,y*source->height/height);
            const auto *rgba=source->pixels.data()+4*(sy*source->width+sx);
            const unsigned r=(unsigned(rgba[0])*rgba[3]+127)/255,g=(unsigned(rgba[1])*rgba[3]+127)/255,b=(unsigned(rgba[2])*rgba[3]+127)/255;
            pixels[(top+y)*size+left+x]=uint16_t(((r>>3)<<11)|((g>>2)<<5)|(b>>3));
        }
    }
    for(const uint16_t pixel:pixels) write16(output,pixel);
    output.write("\r\n",2);
}
}

std::unique_ptr<sla::RasterBase> CrealityCXDLPv1Archive::create_raster() const
{
    return sla::create_printer_raster(m_cfg);
}

sla::RasterEncoder CrealityCXDLPv1Archive::get_encoder() const
{
    return encode_mask;
}

void CrealityCXDLPv1Archive::export_print(const std::string     fname,
                                           const SLAPrint       &print,
                                           const ThumbnailsList &thumbnails,
                                           const std::string    &)
{
    // UVtools CrealityCXDLPv1File: sequential big-endian tables, vertical
    // inclusive grayscale runs, three RGB565 previews and a validated footer.
    size_t columns=m_cfg.display_pixels_x.value,rows=m_cfg.display_pixels_y.value;
    double width=m_cfg.display_width.value,height=m_cfg.display_height.value;
    if(m_cfg.display_orientation.value==sladoPortrait){std::swap(columns,rows);std::swap(width,height);}
    if(columns>16384 || rows>8192 || m_layers.empty() || m_layers.size()>UINT16_MAX || m_layers.size()!=print.print_layers().size())
        throw ExportError("CXDLPv1 resolution or layer count exceeds the schema limits");
    if(std::abs(width*100-std::round(width*100))>1e-6 || std::abs(height*1000-std::round(height*1000))>1e-6)
        throw ExportError("CXDLPv1 display width requires 2 decimal places and height 3");
    sla::require_single_stage_motion(print,"CXDLPv1",true);
    const auto &material=print.material_config();
    if(material.sla_transition_layers.value!=0)
        throw ExportError("CXDLPv1 cannot store transition exposures");
    const double layer_height=print.default_object_config().layer_height.value;
    for(size_t i=0;i<m_layers.size();++i)
        if(std::abs(double(print.print_layers()[i].level())*SCALING_FACTOR-(i+1)*layer_height)>1e-5)
            throw ExportError("CXDLPv1 cannot store a different first layer height or variable Z");
    auto param=[&](const char *key,bool bottom=false){return sla::motion_parameter(print,key,bottom);};
    if(param("wait_before_cure")!=param("wait_before_cure",true))
        throw ExportError("CXDLPv1 requires the same normal and bottom wait before exposure");
    const std::array<uint16_t,11> settings{
        integer(material.exposure_time.value,"exposure",true),integer(param("wait_before_cure"),"wait before exposure",true),
        integer(material.initial_exposure_time.value,"bottom exposure",true),uint16_t(sla::bottom_layers(print,m_layers.size())),
        integer(param("lift_height",true),"bottom lift height",true),integer(param("lift_speed",true),"bottom lift speed",true),
        integer(param("lift_height"),"lift height",true),integer(param("lift_speed"),"lift speed",true),
        integer(param("retract_speed"),"retract speed",true),integer(param("light_pwm",true),"bottom UV PWM"),integer(param("light_pwm"),"UV PWM")};
    for(const auto &layer:m_layers) {
        if(layer.size()<8) throw ExportError("CXDLPv1 raster is incomplete");
        const auto *bytes=static_cast<const uint8_t *>(layer.data());
        if(layer.size()!=8+uint64_t(read32(bytes+4))*6) throw ExportError("CXDLPv1 raster table size mismatch");
    }
    boost::nowide::ofstream output(fname,std::ios::binary|std::ios::trunc);
    if(!output) throw FileIOError("Cannot write CXDLPv1 output");
    write32(output,9);output.write("CXSW3DV2\0",9);
    write16(output,uint16_t(m_layers.size()));write16(output,uint16_t(columns));write16(output,uint16_t(rows));
    const auto valid=std::find_if(thumbnails.begin(),thumbnails.end(),[](const ThumbnailData &image){return image.is_valid() && image.width<=4096 && image.height<=4096;});
    const ThumbnailData *thumbnail=valid==thumbnails.end()?nullptr:&*valid;
    preview(output,116,thumbnail);preview(output,290,thumbnail);preview(output,290,thumbnail);
    number_string(output,width);number_string(output,height);number_string(output,layer_height);
    for(const auto value:settings) write16(output,value);
    for(const auto &layer:m_layers) write32(output,read32(static_cast<const uint8_t *>(layer.data())));
    output.write("\r\n",2);
    for(const auto &layer:m_layers){output.write(static_cast<const char *>(layer.data()),layer.size());output.write("\r\n",2);}
    write32(output,9);output.write("CXSW3DV2\0",9);write32(output,7);
    output.close();if(!output) throw FileIOError("Cannot finish CXDLPv1 output");
}

}
