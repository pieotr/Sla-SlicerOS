/// Native port of UVtools.Core/FileFormats/LGSFile.cs (AGPLv3).
#include "LongerOrangeArchive.hpp"
#include "libslic3r/SLAPrint.hpp"
#include "libslic3r/SLA/PrinterRaster.hpp"
#include "libslic3r/SLA/PrintParameters.hpp"
#include <cstring>
#include <fstream>
#include <limits>

namespace Slic3r {
namespace {
constexpr size_t header_size = 180;
void u32(std::vector<uint8_t> &buffer, size_t at, uint32_t value) {
    for (unsigned byte=0; byte<4; ++byte) buffer.at(at+byte)=uint8_t(value>>(byte*8));
}
void f32(std::vector<uint8_t> &buffer, size_t at, float value) {
    uint32_t bits; std::memcpy(&bits,&value,sizeof bits); u32(buffer,at,bits);
}
float machine_z(uint32_t model) {
    switch(model) {
    case 10: return 140; case 30: return 170; case 120: return 150;
    case 4000: case 4500: return 190;
    default: throw ExportError("Unknown Longer Orange model");
    }
}
// Renderer RGBA is bottom-up. Produce top-down RGB, preserving aspect ratio
// with black margins; neither preview affects layer orientation or masks.
std::vector<uint8_t> preview_rgb(unsigned width,unsigned height,const ThumbnailsList &thumbnails) {
    std::vector<uint8_t> rgb(size_t(width)*height*3,0);
    const ThumbnailData *image=nullptr;
    for(const auto &candidate:thumbnails)
        if(candidate.is_valid() && (!image || candidate.pixels.size()>image->pixels.size())) image=&candidate;
    if(!image) return rgb;
    const double scale=std::min(double(width)/image->width,double(height)/image->height);
    const unsigned w=std::min(width,std::max(1u,unsigned(std::round(image->width*scale))));
    const unsigned h=std::min(height,std::max(1u,unsigned(std::round(image->height*scale))));
    const unsigned left=(width-w)/2,top=(height-h)/2;
    for(unsigned y=0;y<h;++y) for(unsigned x=0;x<w;++x) {
        const unsigned sx=std::min(image->width-1,x*image->width/w);
        const unsigned sy=std::min(image->height-1,y*image->height/h);
        const auto *source=image->pixels.data()+(size_t(image->height-1-sy)*image->width+sx)*4;
        const size_t offset=(size_t(y+top)*width+x+left)*3;
        for(unsigned c=0;c<3;++c) rgb[offset+c]=uint8_t((unsigned(source[c])*source[3]+127)/255);
    }
    return rgb;
}
void write(std::ostream &out,const std::vector<uint8_t> &data) {
    out.write(reinterpret_cast<const char *>(data.data()),data.size());
}
}
uint32_t LongerOrangeArchive::model_for_format(const std::string &format,const std::string &model) {
    if(format=="lgs") return 10;
    if(format=="lgs30") return 30;
    if(format=="lgs120") return 120;
    if(format=="lgs4k") return model=="4000"?4000:4500;
    return 0;
}
sla::EncodedRaster LongerOrangeArchive::encode_mask(const void *ptr,size_t width,size_t height,size_t components,bool rotate) {
    if(!ptr || !width || !height || width>32768 || height>32768 ||
       uint64_t(width)*height>(uint64_t(128)<<20) || components!=1)
        throw ExportError("LGS requires a nonempty grayscale mask within raster memory limits");
    const auto *pixels=static_cast<const uint8_t *>(ptr);
    const size_t length=width*height;
    auto color_at=[&](size_t index) {
        // 4K wire order is a 90 degree clockwise rotation. The decoder rotates
        // back. Canonical masks already include printer raster transforms.
        const size_t source=rotate ? (height-1-index%height)*width+index/height : index;
        return uint8_t(pixels[source]&0xf0);
    };
    std::vector<uint8_t> bytes;
    for(size_t begin=0;begin<length;) {
        const uint8_t color=color_at(begin);
        size_t end=begin+1;
        while(end<length && color_at(end)==color) ++end;
        uint32_t run=uint32_t(end-begin);
        uint8_t digits[8]; unsigned count=0;
        do { digits[count++]=uint8_t(color|(run&15));run>>=4; } while(run);
        while(count) bytes.push_back(digits[--count]);
        begin=end;
    }
    return sla::EncodedRaster(std::move(bytes),"lgs");
}
std::unique_ptr<sla::RasterBase> LongerOrangeArchive::create_raster() const {
    return sla::create_printer_raster(m_cfg);
}
sla::RasterEncoder LongerOrangeArchive::get_encoder() const {
    const bool rotate=m_model==4000 || m_model==4500;
    return [rotate](const void *ptr,size_t w,size_t h,size_t c) { return encode_mask(ptr,w,h,c,rotate); };
}
void LongerOrangeArchive::validate_configuration(const SLAPrint &print,uint32_t model) {
    const auto &cfg=print.printer_config();
    const bool portrait=cfg.display_orientation.value==sladoPortrait;
    const int width=portrait?cfg.display_pixels_y.value:cfg.display_pixels_x.value;
    const int height=portrait?cfg.display_pixels_x.value:cfg.display_pixels_y.value;
    if(width<=0 || height<=0 || width>height || width>32768 || height>32768 ||
       uint64_t(width)*height>(uint64_t(128)<<20) ||
       !std::isfinite(cfg.display_width.value) || !std::isfinite(cfg.display_height.value) ||
       cfg.display_width.value<=0 || cfg.display_height.value<=0)
        throw ExportError("LGS requires positive screen geometry with output pixel width <= height; adjust display orientation");
    if(!cfg.display_mirror_x.value || cfg.display_mirror_y.value)
        throw ExportError("LGS requires horizontal display mirroring and no vertical mirroring");
    if(cfg.max_print_height.value!=machine_z(model))
        throw ExportError("LGS printer height must match its model: 10=140, 30=170, 120=150, 4K=190 mm");
    const double layer_height=print.default_object_config().layer_height.value;
    if(!std::isfinite(layer_height) || layer_height<=0 ||
       float(layer_height)!=float(print.material_config().initial_layer_height.value))
        throw ExportError("LGS requires the same positive initial and normal layer height");
    if(print.material_config().sla_transition_layers.value!=0)
        throw ExportError("LGS cannot store transition layer exposures");
    sla::require_single_stage_motion(print,"LGS",false);
    const float retract=sla::motion_parameter(print,"lift_speed",false);
    for(const bool bottom:{false,true}) {
        if(sla::motion_parameter(print,"retract_speed",bottom)!=retract)
            throw ExportError("LGS retract speed must equal the normal lift speed (no separate retract field)");
        for(const char *key:{"lift_height","lift_speed","wait_before_cure"}) {
            const float value=sla::motion_parameter(print,key,bottom);
            if(!std::isfinite(value) || value<0 || (std::string(key)=="lift_speed" && value<=0))
                throw ExportError(std::string("Invalid LGS ")+key);
        }
    }
}
void LongerOrangeArchive::export_print(const std::string fname,const SLAPrint &print,
                                      const ThumbnailsList &thumbnails,const std::string &) {
    validate_configuration(print,m_model);
    if(m_layers.empty() || m_layers.size()!=print.print_layers().size() || m_layers.size()>UINT32_MAX)
        throw ExportError("LGS requires a complete nonempty layer stack");
    const double lh=print.default_object_config().layer_height.value;
    for(size_t i=0;i<m_layers.size();++i)
        if(std::abs(double(sla::layer_z(print,i))-lh*(i+1))>1e-5)
            throw ExportError("LGS cannot store variable layer heights or Z positions");
    const size_t bottom=sla::bottom_layers(print,m_layers.size());
    if(bottom>UINT16_MAX) throw ExportError("LGS bottom layer count exceeds uint16");
    float bottom_height=float(bottom)*float(lh);
    // UVtools derives a uint16 count by truncating a float quotient. Avoid a
    // one-layer loss when multiplication/division rounds just below the integer.
    if(bottom && bottom_height/float(lh)<float(bottom))
        bottom_height=std::nextafter(bottom_height,std::numeric_limits<float>::infinity());
    const bool portrait=m_cfg.display_orientation.value==sladoPortrait;
    const float cols=portrait?m_cfg.display_pixels_y.value:m_cfg.display_pixels_x.value;
    const float rows=portrait?m_cfg.display_pixels_x.value:m_cfg.display_pixels_y.value;
    const float width=portrait?m_cfg.display_height.value:m_cfg.display_width.value;
    const float height=portrait?m_cfg.display_width.value:m_cfg.display_height.value;
    std::vector<uint8_t> header(header_size,0);
    std::memcpy(header.data(),"Longer3D",8);
    u32(header,8,1);u32(header,12,1);u32(header,16,m_model);u32(header,24,34);
    f32(header,28,cols/width);f32(header,32,rows/height);
    f32(header,36,cols);f32(header,40,rows);f32(header,44,float(lh));
    f32(header,48,print.material_config().exposure_time.value*1000);
    f32(header,52,print.material_config().initial_exposure_time.value*1000);f32(header,56,10);
    f32(header,60,sla::motion_parameter(print,"wait_before_cure",false)*1000);
    f32(header,64,sla::motion_parameter(print,"wait_before_cure",true)*1000);
    f32(header,68,bottom_height);f32(header,72,.6f);
    f32(header,76,sla::motion_parameter(print,"lift_height",true));f32(header,80,sla::motion_parameter(print,"lift_height",false));
    f32(header,84,sla::motion_parameter(print,"lift_speed",false));f32(header,88,sla::motion_parameter(print,"lift_speed",false));
    f32(header,92,sla::motion_parameter(print,"lift_speed",true));f32(header,96,sla::motion_parameter(print,"lift_speed",true));
    for(const auto &field:std::vector<std::pair<size_t,float>>{{100,5},{104,60},{108,10},{112,600},{116,600},{120,2},{124,.2f},{128,60},{132,1},{136,6},{140,150},{144,1001}})
        f32(header,field.first,field.second);
    f32(header,148,machine_z(m_model));u32(header,164,uint32_t(m_layers.size()));
    u32(header,168,4);u32(header,172,120);u32(header,176,150);
    const auto rgb=preview_rgb(120,150,thumbnails);
    std::vector<uint8_t> preview;preview.reserve(rgb.size()/3*2);
    for(size_t i=0;i<rgb.size();i+=3) {
        const uint16_t word=uint16_t((uint16_t(rgb[i]&0xf8)<<8)|(uint16_t(rgb[i+1]&0xfc)<<3)|(rgb[i+2]>>3));
        preview.push_back(uint8_t(word>>8));preview.push_back(uint8_t(word));
    }
    sla::EncodedRaster png;
    if(m_model==120) {
        const auto large=preview_rgb(1200,1600,thumbnails);
        png=sla::PNGRasterEncoder{}(large.data(),1200,1600,3);
        if(png.size()>UINT32_MAX) throw ExportError("LGS120 preview exceeds uint32");
    }
    // Check every extent before truncating an existing destination.
    for(const auto &mask:m_layers) {
        if(!mask.size() || mask.size()>UINT32_MAX) throw ExportError("Invalid LGS layer size");
        const auto *data=static_cast<const uint8_t *>(mask.data());
        const uint64_t expected=uint64_t(cols)*uint64_t(rows);
        uint64_t pixels=0;
        for(size_t i=0;i<mask.size();) {
            const uint8_t color=data[i]&0xf0;
            uint64_t run=0;
            do {
                if(run>(expected>>4)) throw ExportError("Invalid LGS run length");
                run=(run<<4)|(data[i++]&15);
            } while(i<mask.size() && (data[i]&0xf0)==color);
            if(!run || run>expected-pixels) throw ExportError("Invalid LGS mask extent");
            pixels+=run;
        }
        if(pixels!=expected) throw ExportError("LGS mask dimensions do not match the printer");
    }
    std::ofstream out(fname,std::ios::binary|std::ios::trunc);
    if(!out) throw ExportError("Cannot open LGS output");
    write(out,header);write(out,preview);
    std::vector<uint8_t> length(4);
    if(m_model==120) {
        u32(length,0,uint32_t(png.size()));write(out,length);
        out.write(static_cast<const char *>(png.data()),png.size());out.put(0);out.put(0);
    }
    for(const auto &mask:m_layers) {
        u32(length,0,uint32_t(mask.size()));write(out,length);
        out.write(static_cast<const char *>(mask.data()),mask.size());
    }
    // Original UVtools appends a second table after freeing its buffers. The
    // decoder consumes only LayerCount records; native output ends there.
    out.flush();if(!out) throw ExportError("Cannot finish LGS output");
}
}
