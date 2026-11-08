#include "CancellablePNGEncoder.hpp"
#include <exception>
#include <cstdio>
///|/ Copyright (c) Prusa Research 2020 - 2022 Tomáš Mészáros @tamasmeszaros
///|/ Copyright (c) 2022 ole00 @ole00
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef SLARASTER_CPP
#define SLARASTER_CPP

#include <libslic3r/SLA/RasterBase.hpp>
#include <libslic3r/SLA/AGGRaster.hpp>
// minz image write:
#include <miniz.h>
#include <algorithm>
#include <cmath>
#include <iterator>
#include <cstdlib>
#include <png.h>
#include <limits>

#include "agg/agg_gamma_functions.h"

namespace Slic3r { namespace sla {

namespace {
struct PNGWriteState {
    std::vector<uint8_t> bytes;
    const std::function<void()> *checkpoint = nullptr;
    std::exception_ptr callback_error;
};
void check_png_cancel(png_structp writer, PNGWriteState &state) {
    if (!state.checkpoint || !*state.checkpoint) return;
    try { (*state.checkpoint)(); }
    catch (...) { state.callback_error=std::current_exception(); }
    // Leave the C++ catch scope before libpng longjmp: skipping its cleanup
    // would leak the active exception and corrupt later exception handling.
    if(state.callback_error)png_error(writer,"SLA PNG encoding cancelled");
}
}
EncodedRaster encode_png_raster(const void *ptr,size_t w,size_t h,size_t num_components,
    const std::function<void()> &check_cancel) {
    const auto checkpoint=check_cancel;
    if(checkpoint)checkpoint();
    if (!ptr || !w || !h || w>32768 || h>32768 || num_components<1 || num_components>4 ||
        uint64_t(w)*h*num_components>(uint64_t(512)<<20))return EncodedRaster({},"png");
    // Mutable state stays on the heap across libpng's error jump. The callback
    // owner is constructed before setjmp and cannot be invalidated by reentry.
    auto state=std::make_unique<PNGWriteState>();state->checkpoint=&checkpoint;
    png_structp png=png_create_write_struct(PNG_LIBPNG_VER_STRING,state.get(),
        [](png_structp writer,png_const_charp message) {
            const auto *output=static_cast<const PNGWriteState *>(png_get_error_ptr(writer));
            if(!output || !output->callback_error)std::fprintf(stderr,"libpng error: %s\n",message);
            png_longjmp(writer,1);
        },nullptr);
    if(!png)return EncodedRaster({},"png");
    png_infop info=png_create_info_struct(png);
    if(!info){png_destroy_write_struct(&png,nullptr);return EncodedRaster({},"png");}
    if(setjmp(png_jmpbuf(png))) {
        png_destroy_write_struct(&png,&info);
        if(state->callback_error)std::rethrow_exception(state->callback_error);
        return EncodedRaster({},"png");
    }
    png_set_write_fn(png,state.get(),[](png_structp writer,png_bytep data,png_size_t size) {
        auto &output=*static_cast<PNGWriteState *>(png_get_io_ptr(writer));bool failed=false;
        try {output.bytes.insert(output.bytes.end(),data,data+size);}
        catch (...) {failed=true;}
        if(failed)png_error(writer,"Cannot allocate encoded SLA PNG");
    },nullptr);
    static constexpr int colors[]{PNG_COLOR_TYPE_GRAY,PNG_COLOR_TYPE_GRAY_ALPHA,PNG_COLOR_TYPE_RGB,PNG_COLOR_TYPE_RGBA};
    png_set_compression_level(png,3);png_set_filter(png,PNG_FILTER_TYPE_BASE,PNG_FILTER_NONE);
    png_set_IHDR(png,info,png_uint_32(w),png_uint_32(h),8,colors[num_components-1],
        PNG_INTERLACE_NONE,PNG_COMPRESSION_TYPE_BASE,PNG_FILTER_TYPE_BASE);
    png_write_info(png,info);
    for(size_t row=0;row<h;++row) {
        check_png_cancel(png,*state);
        png_write_row(png,const_cast<png_bytep>(static_cast<const uint8_t *>(ptr)+row*w*num_components));
    }
    png_write_end(png,info);check_png_cancel(png,*state);
    png_destroy_write_struct(&png,&info);
    return EncodedRaster(std::move(state->bytes),"png");
}
EncodedRaster PNGRasterEncoder::operator()(const void *ptr,size_t w,size_t h,size_t num_components) {
    return encode_png_raster(ptr,w,h,num_components);
}

std::ostream &operator<<(std::ostream &stream, const EncodedRaster &bytes)
{
    stream.write(reinterpret_cast<const char *>(bytes.data()),
                 std::streamsize(bytes.size()));
    
    return stream;
}

EncodedRaster PPMRasterEncoder::operator()(const void *ptr, size_t w, size_t h,
                                           size_t      num_components)
{
    std::vector<uint8_t> buf;
    
    auto header = std::string("P5 ") +
            std::to_string(w) + " " +
            std::to_string(h) + " " + "255 ";
    
    auto sz = w * h * num_components;
    size_t s = sz + header.size();
    
    buf.reserve(s);

    auto buff = reinterpret_cast<const std::uint8_t*>(ptr);
    std::copy(header.begin(), header.end(), std::back_inserter(buf));
    std::copy(buff, buff+sz, std::back_inserter(buf));
    
    return EncodedRaster(std::move(buf), "ppm");
}

std::unique_ptr<RasterBase> create_raster_grayscale_aa(
    const Resolution        &res,
    const PixelDim          &pxdim,
    double                   gamma,
    const RasterBase::Trafo &tr)
{
    std::unique_ptr<RasterBase> rst;
    
    if (gamma == 1.)
        rst = std::make_unique<RasterGrayscaleAA>(res, pxdim, tr, agg::gamma_none());
    else if (gamma > 0)
        rst = std::make_unique<RasterGrayscaleAAGammaPower>(res, pxdim, tr, gamma);
    else
        rst = std::make_unique<RasterGrayscaleAA>(res, pxdim, tr, agg::gamma_threshold(.5));
    
    return rst;
}

} // namespace sla
} // namespace Slic3r

#endif // SLARASTER_CPP
