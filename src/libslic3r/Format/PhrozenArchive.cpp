///|/ Native port of UVtools.Core/FileFormats/PHZFile.cs (AGPLv3).
#include "PhrozenArchive.hpp"
#include "ChituboxPreview.hpp"
#include "libslic3r/SLAPrint.hpp"
#include "libslic3r/SLA/PrinterRaster.hpp"
#include "libslic3r/SLA/PrintParameters.hpp"
#include <array>
#include <charconv>
#include <cctype>
#include <cstring>
#include <ctime>
#include <fstream>
#include <limits>
#include <map>
#include <boost/uuid/detail/sha1.hpp>

namespace Slic3r {
namespace {
constexpr uint32_t header_size = 216, layer_size = 36;
void u32(std::vector<uint8_t> &buffer, size_t offset, uint32_t value) {
    for (unsigned byte=0;byte<4;++byte) buffer.at(offset+byte)=uint8_t(value>>(byte*8));
}
void f32(std::vector<uint8_t> &buffer, size_t offset, float value) {
    uint32_t bits;std::memcpy(&bits,&value,sizeof bits);u32(buffer,offset,bits);
}
void write(std::ostream &out,const std::vector<uint8_t> &bytes) {
    out.write(reinterpret_cast<const char *>(bytes.data()),bytes.size());
}
float light_off(const SLAPrint &print,bool bottom) {
    const double height=sla::motion_parameter(print,"lift_height",bottom);
    const double speed=sla::motion_parameter(print,"lift_speed",bottom);
    const double retract=sla::motion_parameter(print,"retract_speed",bottom);
    const double movement=height>0?height*60./speed+height*60./retract:0.;
    return float(std::round((movement+sla::motion_parameter(print,"wait_before_cure",bottom))*100.)/100.);
}
uint64_t exposed_pixels(const sla::EncodedRaster &mask,uint64_t expected) {
    const auto *data=static_cast<const uint8_t *>(mask.data());
    uint64_t pixels=0,exposed=0;uint8_t color=0;
    for(size_t i=0;i<mask.size();++i) {
        const uint8_t code=data[i];
        if(code&0x80) color=code&0x7f;
        const unsigned count=(code&0x80)?1:code;
        if(!count || pixels+count>expected) throw ExportError("Invalid PHZ raster run");
        pixels+=count;if(color)exposed+=count;
    }
    if(pixels!=expected)throw ExportError("PHZ mask dimensions do not match the printer");
    return exposed;
}
}
uint32_t PhrozenPHZArchive::parse_encryption_key(const std::string &text) {
    const auto first=text.find_first_not_of(" \t\r\n");
    const auto last=text.find_last_not_of(" \t\r\n");
    if(first==std::string::npos)throw ExportError("PHZ encryption seed must be 0..4294967295 or hexadecimal with 0x prefix");
    const char *begin=text.data()+first,*end=text.data()+last+1;
    int base=10;
    if(end-begin>=2 && begin[0]=='0' && (begin[1]=='x'||begin[1]=='X')){base=16;begin+=2;}
    uint32_t result=0;
    const auto parsed=std::from_chars(begin,end,result,base);
    if(parsed.ec!=std::errc{} || parsed.ptr!=end)
        throw ExportError("PHZ encryption seed must be 0..4294967295 or hexadecimal with 0x prefix");
    return result;
}
void PhrozenPHZArchive::crypt_mask(std::vector<uint8_t> &bytes,uint32_t seed,uint32_t index) {
    if(!seed)return;
    seed%=0x4324u;
    const uint32_t increment=seed*0x34a32231u;
    uint32_t key=(index^0x3fad2212u)*seed*0x4910913du;
    for(size_t i=0;i<bytes.size();++i) {
        bytes[i]^=uint8_t(key>>(8*(i%4)));
        if(i%4==3)key+=increment;
    }
}
sla::EncodedRaster PhrozenPHZArchive::encode_mask(const void *ptr,size_t w,size_t h,size_t components) {
    if(!ptr || !w || !h || w>32768 || h>32768 || uint64_t(w)*h>(uint64_t(128)<<20) || components!=1)
        throw ExportError("PHZ requires a nonempty grayscale mask within raster memory limits");
    const auto *pixels=static_cast<const uint8_t *>(ptr);
    std::vector<uint8_t> bytes;
    for(size_t y=0;y<h;++y)
    for(size_t x=0;x<w;) {
        const uint8_t color=std::min<uint8_t>(pixels[y*w+x]>>1,0x7c);
        const size_t boundary=x<w/2?w/2:w;
        size_t end=x+1;
        while(end<boundary && std::min<uint8_t>(pixels[y*w+end]>>1,0x7c)==color)++end;
        bytes.push_back(color|0x80);
        size_t remaining=end-x-1;
        while(remaining){const size_t run=std::min<size_t>(125,remaining);bytes.push_back(uint8_t(run));remaining-=run;}
        x=end;
    }
    return sla::EncodedRaster(std::move(bytes),"phz");
}
std::unique_ptr<sla::RasterBase> PhrozenPHZArchive::create_raster() const {return sla::create_printer_raster(m_cfg);}
sla::RasterEncoder PhrozenPHZArchive::get_encoder() const {return encode_mask;}
void PhrozenPHZArchive::validate_configuration(const SLAPrint &print) {
    const auto &cfg=print.printer_config();
    parse_encryption_key(cfg.sla_phz_encryption_key.value);
    if(!std::isfinite(cfg.display_width.value)||!std::isfinite(cfg.display_height.value)||
       !std::isfinite(cfg.max_print_height.value)||cfg.display_width.value<=0||cfg.display_height.value<=0||
       cfg.max_print_height.value<=0||cfg.display_pixels_x.value<=0||cfg.display_pixels_y.value<=0||
       cfg.display_pixels_x.value>32768||cfg.display_pixels_y.value>32768||
       uint64_t(cfg.display_pixels_x.value)*cfg.display_pixels_y.value>(uint64_t(128)<<20))
        throw ExportError("Invalid PHZ screen geometry");
    sla::require_single_stage_motion(print,"PHZ v2",true);
    for(const bool bottom:{false,true}) {
        for(const char *key:{"lift_height","lift_speed","retract_speed","wait_before_cure","light_pwm"}) {
            const double value=sla::motion_parameter(print,key,bottom);
            if(!std::isfinite(value)||value<0||(std::string(key)=="light_pwm"&&value>255))
                throw ExportError(std::string("Invalid PHZ ")+key);
        }
        if(sla::motion_parameter(print,"lift_height",bottom)>0 &&
           (sla::motion_parameter(print,"lift_speed",bottom)<=0||sla::motion_parameter(print,"retract_speed",bottom)<=0))
            throw ExportError("PHZ enabled moves require positive speeds");
    }
    for(const unsigned char c:cfg.printer_model.value)
        if(c<32||c>126)throw ExportError("PHZ firmware model name must be printable ASCII");
    if(cfg.printer_model.value.size()>4096)throw ExportError("PHZ firmware model name is too long");
}
void PhrozenPHZArchive::export_print(const std::string fname,const SLAPrint &print,
                                    const ThumbnailsList &thumbnails,const std::string &) {
    validate_configuration(print);
    if(m_layers.empty()||m_layers.size()!=print.print_layers().size()||m_layers.size()>UINT32_MAX)
        throw ExportError("PHZ requires a complete nonempty layer stack");
    const bool portrait=m_cfg.display_orientation.value==sladoPortrait;
    const uint32_t cols=portrait?m_cfg.display_pixels_y.value:m_cfg.display_pixels_x.value;
    const uint32_t rows=portrait?m_cfg.display_pixels_x.value:m_cfg.display_pixels_y.value;
    const double width=portrait?m_cfg.display_height.value:m_cfg.display_width.value;
    const double height=portrait?m_cfg.display_width.value:m_cfg.display_height.value;
    const uint32_t seed=parse_encryption_key(m_cfg.sla_phz_encryption_key.value);
    const std::string model=m_cfg.printer_model.value.empty()?"Unknown":m_cfg.printer_model.value;
    const auto small=encode_chitubox_rgb15(200,125,thumbnails);
    const auto large=encode_chitubox_rgb15(400,300,thumbnails);
    const uint32_t small_offset=header_size,large_offset=small_offset+32+uint32_t(small.size());
    const uint32_t model_offset=large_offset+32+uint32_t(large.size());
    const uint32_t table_offset=model_offset+uint32_t(model.size());
    struct Payload {const sla::EncodedRaster *mask;uint64_t address;bool write=true;};
    std::vector<Payload> payloads;
    std::map<std::array<unsigned,5>,std::vector<size_t>> hashes;
    uint64_t cursor=uint64_t(table_offset)+uint64_t(layer_size)*m_layers.size();
    double volume=0,previous_z=0;
    const uint64_t expected=uint64_t(cols)*rows;
    const double area=width*height/expected;
    for(size_t i=0;i<m_layers.size();++i) {
        const auto &mask=m_layers[i];
        if(mask.size()>UINT32_MAX)throw ExportError("PHZ layer exceeds its size field");
        const double z=sla::layer_z(print,i);
        if(!std::isfinite(z)||z<=previous_z||z>m_cfg.max_print_height.value+1e-5)
            throw ExportError("Invalid PHZ layer Z");
        volume+=exposed_pixels(mask,expected)*area*(z-previous_z)/1000.;previous_z=z;
        Payload payload{&mask,cursor};
        if(!seed) {
            boost::uuids::detail::sha1 hash;hash.process_bytes(mask.data(),mask.size());
            unsigned digest[5];hash.get_digest(digest);
            const std::array<unsigned,5> key{digest[0],digest[1],digest[2],digest[3],digest[4]};
            const auto found=hashes.find(key);
            if(found!=hashes.end())for(const size_t candidate:found->second) {
                const auto &old=payloads[candidate];
                if(old.mask->size()==mask.size()&&!std::memcmp(old.mask->data(),mask.data(),mask.size())) {
                    payload.address=old.address;payload.write=false;break;
                }
            }
            if(payload.write&&hashes.size()<1024)hashes[key].push_back(i);
        }
        if(payload.write) {
            if(cursor>uint64_t(std::numeric_limits<std::streamoff>::max())-mask.size())
                throw ExportError("PHZ exceeds stream address limits");
            cursor+=mask.size();
        }
        payloads.push_back(payload);
    }
    std::vector<uint8_t> header(header_size,0);
    u32(header,0,0x9FDA83AE);u32(header,4,2);
    f32(header,8,print.default_object_config().layer_height.value);
    f32(header,12,print.material_config().exposure_time.value);f32(header,16,print.material_config().initial_exposure_time.value);
    const auto bottom=uint32_t(sla::bottom_layers(print,m_layers.size()));
    u32(header,20,bottom);u32(header,24,cols);u32(header,28,rows);
    u32(header,32,large_offset);u32(header,36,table_offset);u32(header,40,uint32_t(m_layers.size()));
    u32(header,44,small_offset);u32(header,48,sla::lcd_print_time(print));
    u32(header,52,(m_cfg.display_mirror_x.value||m_cfg.display_mirror_y.value)?1:0);u32(header,56,1);
    header[60]=uint8_t(sla::motion_parameter(print,"light_pwm",false));
    header[62]=uint8_t(sla::motion_parameter(print,"light_pwm",true));
    f32(header,72,sla::layer_z(print,m_layers.size()-1));
    f32(header,76,width);f32(header,80,height);f32(header,84,m_cfg.max_print_height.value);u32(header,88,seed);
    f32(header,92,light_off(print,true));f32(header,96,light_off(print,false));u32(header,100,bottom);
    f32(header,108,sla::motion_parameter(print,"lift_height",true));f32(header,112,sla::motion_parameter(print,"lift_speed",true));
    f32(header,116,sla::motion_parameter(print,"lift_height",false));f32(header,120,sla::motion_parameter(print,"lift_speed",false));
    f32(header,124,sla::motion_parameter(print,"retract_speed",false));
    f32(header,128,float(volume));f32(header,132,float(volume*print.material_config().material_density.value));
    f32(header,136,print.material_config().bottle_volume.value>0?float(volume*print.material_config().bottle_cost.value/print.material_config().bottle_volume.value):0.f);
    u32(header,144,model_offset);u32(header,148,uint32_t(model.size()));u32(header,176,28);
    u32(header,180,uint32_t(std::time(nullptr)/60));u32(header,184,1);u32(header,188,0x01060300);
    std::ofstream out(fname,std::ios::binary|std::ios::trunc);
    if(!out)throw ExportError("Cannot open PHZ output");
    write(out,header);
    auto preview=[&](uint32_t address,uint32_t w,uint32_t h,const std::vector<uint8_t> &bytes) {
        std::vector<uint8_t> table(32,0);u32(table,0,w);u32(table,4,h);u32(table,8,address+32);u32(table,12,uint32_t(bytes.size()));
        write(out,table);write(out,bytes);
    };
    preview(small_offset,200,125,small);preview(large_offset,400,300,large);
    out.write(model.data(),model.size());
    for(size_t i=0;i<payloads.size();++i) {
        const auto &p=payloads[i];std::vector<uint8_t> table(layer_size,0);
        f32(table,0,sla::layer_z(print,i));f32(table,4,sla::layer_exposure(print,i));f32(table,8,light_off(print,i<bottom));
        u32(table,12,uint32_t(p.address));u32(table,16,uint32_t(p.mask->size()));u32(table,20,uint32_t(p.address>>32));
        write(out,table);
    }
    for(size_t i=0;i<payloads.size();++i)if(payloads[i].write) {
        const auto &mask=*payloads[i].mask;
        if(seed) {
            const auto *data=static_cast<const uint8_t *>(mask.data());std::vector<uint8_t> encrypted(data,data+mask.size());
            crypt_mask(encrypted,seed,uint32_t(i));write(out,encrypted);
        } else out.write(static_cast<const char *>(mask.data()),mask.size());
    }
    out.flush();if(!out)throw ExportError("Cannot finish PHZ output");
}
} // namespace Slic3r
