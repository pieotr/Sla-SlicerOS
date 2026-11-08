/// Native GR1File/MDLPFile port from local UVtools (AGPLv3).
#include "MakerbaseArchive.hpp"
#include "libslic3r/SLAPrint.hpp"
#include "libslic3r/SLA/PrinterRaster.hpp"
#include "libslic3r/SLA/PrintParameters.hpp"
#include <array>
#include <fstream>
#include <iomanip>
#include <locale>
#include <sstream>

namespace Slic3r {
namespace {
const std::vector<uint8_t> magic{0,0,0,7,'M','K','S','D','L','P',0};
void u16(std::vector<uint8_t> &bytes,uint16_t value) {
    bytes.push_back(uint8_t(value>>8));bytes.push_back(uint8_t(value));
}
void u32(std::vector<uint8_t> &bytes,uint32_t value) {
    for(int shift=24;shift>=0;shift-=8) bytes.push_back(uint8_t(value>>shift));
}
void write(std::ostream &out,const std::vector<uint8_t> &bytes) {
    out.write(reinterpret_cast<const char *>(bytes.data()),bytes.size());
}
uint16_t integral(double value,const char *name) {
    if(!std::isfinite(value) || value<0 || value>UINT16_MAX || std::abs(value-std::round(value))>1e-5)
        throw ExportError(std::string("Makerbase requires an integer 0..65535 for ")+name);
    return uint16_t(std::round(value));
}
void number(std::vector<uint8_t> &bytes,double value,unsigned precision) {
    std::ostringstream stream;stream.imbue(std::locale::classic());
    stream<<std::fixed<<std::setprecision(precision)<<value;
    std::string text=stream.str();
    if(text.find('.')!=std::string::npos) {
        while(text.back()=='0') text.pop_back();
        if(text.back()=='.') text.pop_back();
    }
    u32(bytes,uint32_t(text.size()*2));
    for(const char c:text){bytes.push_back(0);bytes.push_back(uint8_t(c));}
}
float light_off(const SLAPrint &print,bool bottom) {
    const double h=sla::motion_parameter(print,"lift_height",bottom);
    const double speed=sla::motion_parameter(print,"lift_speed",bottom);
    const double retract=sla::motion_parameter(print,"retract_speed",bottom);
    const double move=h>0 ? h*60./speed+h*60./retract : 0.;
    return float(move+sla::motion_parameter(print,"wait_before_cure",bottom));
}
void preview(std::ostream &out,unsigned size,const ThumbnailsList &thumbnails) {
    const ThumbnailData *image=nullptr;
    for(const auto &candidate:thumbnails)
        if(candidate.is_valid() && (!image || candidate.pixels.size()>image->pixels.size())) image=&candidate;
    const double scale=image?std::min(double(size)/image->width,double(size)/image->height):1.;
    const unsigned w=image?std::min(size,std::max(1u,unsigned(std::round(image->width*scale)))):0;
    const unsigned h=image?std::min(size,std::max(1u,unsigned(std::round(image->height*scale)))):0;
    const unsigned left=(size-w)/2,top=(size-h)/2;
    for(unsigned y=0;y<size;++y)for(unsigned x=0;x<size;++x) {
        uint16_t rgb=0;
        if(image && x>=left && x<left+w && y>=top && y<top+h) {
            const unsigned sx=std::min(image->width-1,(x-left)*image->width/w);
            const unsigned sy=std::min(image->height-1,(y-top)*image->height/h);
            const auto *p=image->pixels.data()+(size_t(image->height-1-sy)*image->width+sx)*4;
            const unsigned r=(unsigned(p[0])*p[3]+127)/255,g=(unsigned(p[1])*p[3]+127)/255,b=(unsigned(p[2])*p[3]+127)/255;
            rgb=uint16_t(((r&0xf8)<<8)|((g&0xfc)<<3)|(b>>3));
        }
        out.put(char(rgb>>8));out.put(char(rgb));
    }
    out.put(13);out.put(10);
}
}
sla::EncodedRaster MakerbaseArchive::encode_mask(const void *ptr,size_t width,size_t height,size_t components) {
    if(!ptr || !width || !height || width>32768 || height>32768 ||
       uint64_t(width)*height>(uint64_t(128)<<20) || components!=1)
        throw ExportError("Makerbase requires a nonempty grayscale mask within raster limits");
    const auto *pixels=static_cast<const uint8_t *>(ptr);
    std::vector<uint8_t> bytes(4,0);uint32_t count=0;
    // Read 64 adjacent columns at a time for cache locality, retaining the
    // original column-major line order without a full binary-mask copy.
    for(size_t first=0;first<width;first+=64) {
        const size_t columns=std::min<size_t>(64,width-first);
        std::array<int,64> start;start.fill(-1);
        std::array<std::vector<uint8_t>,64> lines;
        auto close=[&](size_t col,size_t end) {
            u16(lines[col],uint16_t(start[col]));u16(lines[col],uint16_t(end));u16(lines[col],uint16_t(first+col));
            start[col]=-1;++count;
        };
        for(size_t y=0;y<height;++y)for(size_t col=0;col<columns;++col) {
            const bool white=pixels[y*width+first+col]>=128;
            if(white && start[col]<0) start[col]=int(y);
            if(!white && start[col]>=0) close(col,y-1);
        }
        for(size_t col=0;col<columns;++col) {
            if(start[col]>=0) close(col,height-1);
            bytes.insert(bytes.end(),lines[col].begin(),lines[col].end());
        }
    }
    for(unsigned b=0;b<4;++b) bytes[b]=uint8_t(count>>(24-b*8));
    bytes.push_back(13);bytes.push_back(10);
    return sla::EncodedRaster(std::move(bytes),"gr1");
}
std::unique_ptr<sla::RasterBase> MakerbaseArchive::create_raster() const {return sla::create_printer_raster(m_cfg);}
sla::RasterEncoder MakerbaseArchive::get_encoder() const {return encode_mask;}
void MakerbaseArchive::validate_configuration(const SLAPrint &print,bool mdlp) {
    const auto &cfg=print.printer_config();
    if(cfg.display_pixels_x.value<=0 || cfg.display_pixels_y.value<=0 ||
       cfg.display_pixels_x.value>32768 || cfg.display_pixels_y.value>32768 ||
       uint64_t(cfg.display_pixels_x.value)*cfg.display_pixels_y.value>(uint64_t(128)<<20))
        throw ExportError("Invalid Makerbase resolution");
    const bool portrait=cfg.display_orientation.value==sladoPortrait;
    const double width=portrait?cfg.display_height.value:cfg.display_width.value;
    const double height=portrait?cfg.display_width.value:cfg.display_height.value;
    if(!std::isfinite(width)||!std::isfinite(height)||width<=0||height<=0 ||
       std::abs(width-std::round(width*100)/100)>1e-6 || std::abs(height-std::round(height*1000)/1000)>1e-6)
        throw ExportError("Makerbase display width supports 2 decimal places and height 3");
    if(cfg.display_mirror_x.value || cfg.display_mirror_y.value)
        throw ExportError("Makerbase files do not store display mirroring; use a profile without mirroring");
    const double lh=print.default_object_config().layer_height.value;
    if(!std::isfinite(lh)||lh<=0||float(lh)!=float(print.material_config().initial_layer_height.value))
        throw ExportError("Makerbase requires equal positive initial and normal layer heights");
    if(std::abs(lh-std::round(lh*100000)/100000)>1e-7)
        throw ExportError("Makerbase layer height supports 5 decimal places");
    if(print.material_config().sla_transition_layers.value!=0)
        throw ExportError("Makerbase cannot store transition layer exposures");
    integral(print.material_config().exposure_time.value,"normal exposure seconds");
    integral(print.material_config().initial_exposure_time.value,"bottom exposure seconds");
    sla::require_single_stage_motion(print,mdlp?"MDLP":"GR1",!mdlp);
    for(const bool bottom:{false,true}) {
        for(const char *key:{"lift_height","lift_speed","retract_speed","wait_before_cure","light_pwm"}) {
            const double value=sla::motion_parameter(print,key,bottom);
            if(!std::isfinite(value)||value<0 || (std::string(key)=="light_pwm" && value>255))
                throw ExportError(std::string("Invalid Makerbase ")+key);
        }
        if(sla::motion_parameter(print,"lift_height",bottom)>0 &&
           (sla::motion_parameter(print,"lift_speed",bottom)<=0 || sla::motion_parameter(print,"retract_speed",bottom)<=0))
            throw ExportError("Makerbase enabled moves require positive speeds");
        if(mdlp) {
            if(sla::motion_parameter(print,"lift_height",bottom)!=6 ||
               sla::motion_parameter(print,"lift_speed",bottom)!=100 || sla::motion_parameter(print,"retract_speed",bottom)!=100)
                throw ExportError("MDLP has no motion fields; UVtools interprets 6 mm lift and 100 mm/min lift/retract");
        } else for(const char *key:{"lift_height","lift_speed","retract_speed"})
            integral(sla::motion_parameter(print,key,bottom),key);
    }
    const auto normal=integral(light_off(print,false),"light-off seconds");
    if(normal!=integral(light_off(print,true),"bottom light-off seconds"))
        throw ExportError("Makerbase requires a shared bottom/normal light-off delay including movement and wait");
}
void MakerbaseArchive::export_print(const std::string fname,const SLAPrint &print,const ThumbnailsList &thumbnails,const std::string &) {
    validate_configuration(print,m_mdlp);
    if(m_layers.empty() || m_layers.size()!=print.print_layers().size() || m_layers.size()>UINT16_MAX)
        throw ExportError("Makerbase layer count must be 1..65535");
    const double lh=print.default_object_config().layer_height.value;
    for(size_t i=0;i<m_layers.size();++i)
        if(std::abs(double(sla::layer_z(print,i))-lh*(i+1))>1e-5)
            throw ExportError("Makerbase cannot store variable layer heights or Z positions");
    const bool portrait=m_cfg.display_orientation.value==sladoPortrait;
    const uint16_t cols=portrait?m_cfg.display_pixels_y.value:m_cfg.display_pixels_x.value;
    const uint16_t rows=portrait?m_cfg.display_pixels_x.value:m_cfg.display_pixels_y.value;
    auto be16=[](const uint8_t *p){return uint16_t(uint16_t(p[0])<<8|p[1]);};
    for(const auto &mask:m_layers) {
        const auto *p=static_cast<const uint8_t *>(mask.data());
        if(mask.size()<6) throw ExportError("Invalid Makerbase layer packet");
        const uint32_t count=uint32_t(p[0])<<24|uint32_t(p[1])<<16|uint32_t(p[2])<<8|p[3];
        if(uint64_t(count)*6+6!=mask.size() || p[mask.size()-2]!=13 || p[mask.size()-1]!=10)
            throw ExportError("Invalid Makerbase line table");
        for(size_t at=4;at<mask.size()-2;at+=6)
            if(be16(p+at)>be16(p+at+2) || be16(p+at+2)>=rows || be16(p+at+4)>=cols)
                throw ExportError("Makerbase line exceeds printer raster dimensions");
    }
    std::vector<uint8_t> info;
    u16(info,uint16_t(m_layers.size()));u16(info,cols);u16(info,rows);
    number(info,portrait?m_cfg.display_height.value:m_cfg.display_width.value,2);
    number(info,portrait?m_cfg.display_width.value:m_cfg.display_height.value,3);
    number(info,lh,5);
    u16(info,integral(print.material_config().exposure_time.value,"exposure"));
    u16(info,integral(light_off(print,false),"light-off"));
    u16(info,integral(print.material_config().initial_exposure_time.value,"bottom exposure"));
    u16(info,uint16_t(sla::bottom_layers(print,m_layers.size())));
    if(!m_mdlp) {
        for(const auto &field:std::vector<std::pair<const char *,bool>>{{"lift_height",true},{"lift_speed",true},{"lift_height",false},{"lift_speed",false},{"retract_speed",false},{"light_pwm",true},{"light_pwm",false}})
            u16(info,integral(sla::motion_parameter(print,field.first,field.second),field.first));
    }
    std::ofstream out(fname,std::ios::binary|std::ios::trunc);
    if(!out) throw ExportError("Cannot open Makerbase output");
    write(out,magic);preview(out,116,thumbnails);preview(out,290,thumbnails);write(out,info);
    for(const auto &mask:m_layers) out.write(static_cast<const char *>(mask.data()),mask.size());
    write(out,magic);out.flush();if(!out)throw ExportError("Cannot finish Makerbase output");
}
}
