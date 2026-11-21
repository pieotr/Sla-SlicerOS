// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ColorMask.hpp"
#include <png.h>
#include <cstring>
#include <csetjmp>

namespace Slic3r::sla {
EncodedRaster encode_bgr_packed_mask(const png::ImageGreyscale &image) {
    if (!image.cols || !image.rows || image.cols>32768 || image.rows>32768 ||
        image.cols*image.rows>128ULL*1024*1024 || image.buf.size()!=image.cols*image.rows || image.cols%3)
        throw RuntimeError("RGB.CWS requires an LCD raster width divisible by three after orientation");
    auto rgb=image.buf;
    for(size_t i=0;i<rgb.size();i+=3)std::swap(rgb[i],rgb[i+2]);
    auto encoded=PNGRasterEncoder{}(rgb.data(),image.cols/3,image.rows,3);
    if(!encoded.size())throw RuntimeError("Cannot encode RGB.CWS packed mask");
    return encoded;
}

bool decode_bgr_packed_mask(const png::ReadBuf &input,size_t columns,size_t rows,png::ImageGreyscale &output) {
    if(!input.buf || input.sz<8 || !columns || columns%3 || !rows || columns>32768 || rows>32768 ||
        columns*rows>128ULL*1024*1024 || !png::is_png(input))return false;
    struct Descriptor {
        png_structp png=png_create_read_struct(PNG_LIBPNG_VER_STRING,nullptr,nullptr,nullptr);
        png_infop info=png?png_create_info_struct(png):nullptr;
        ~Descriptor(){if(png)png_destroy_read_struct(&png,info?&info:nullptr,nullptr);}
    } descriptor;
    struct Reader {const png::ReadBuf &input;size_t position=0;} reader{input};
    if(!descriptor.png || !descriptor.info)return false;
    if(setjmp(png_jmpbuf(descriptor.png))){output.buf.clear();return false;}
    png_set_read_fn(descriptor.png,&reader,[](png_structp state,png_bytep destination,png_size_t size) {
        auto &source=*static_cast<Reader *>(png_get_io_ptr(state));
        if(size>source.input.sz-source.position)png_error(state,"Truncated RGB.CWS PNG");
        std::memcpy(destination,static_cast<const uint8_t *>(source.input.buf)+source.position,size);
        source.position+=size;
    });
    png_set_crc_action(descriptor.png,PNG_CRC_ERROR_QUIT,PNG_CRC_ERROR_QUIT);
    png_read_info(descriptor.png,descriptor.info);
    if(png_get_image_width(descriptor.png,descriptor.info)!=columns/3 ||
        png_get_image_height(descriptor.png,descriptor.info)!=rows ||
        png_get_bit_depth(descriptor.png,descriptor.info)!=8 ||
        png_get_color_type(descriptor.png,descriptor.info)!=PNG_COLOR_TYPE_RGB)return false;
    const int passes=png_set_interlace_handling(descriptor.png);
    png_read_update_info(descriptor.png,descriptor.info);
    if(png_get_channels(descriptor.png,descriptor.info)!=3 ||
        png_get_rowbytes(descriptor.png,descriptor.info)!=columns)return false;
    // Decode directly into the final grayscale-sized storage, including all
    // Adam7 passes. No full RGBA working image and no luminance conversion.
    output.cols=columns;output.rows=rows;output.buf.resize(columns*rows);
    for(int pass=0;pass<passes;++pass)
        for(size_t y=0;y<rows;++y)png_read_row(descriptor.png,output.buf.data()+y*columns,nullptr);
    png_read_end(descriptor.png,descriptor.info);
    for(size_t i=0;i<output.buf.size();i+=3)std::swap(output.buf[i],output.buf[i+2]);
    return true;
}
} // namespace Slic3r::sla
