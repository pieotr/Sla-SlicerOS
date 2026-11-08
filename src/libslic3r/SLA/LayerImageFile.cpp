// Standalone layer images. Native codecs only; UVtools is a development oracle.
#include "LayerImageFile.hpp"
#include "RasterBase.hpp"
#include "libslic3r/Exception.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <csetjmp>
#include <limits>
#include <memory>
#include "sla_jpeg/jpeglib.h"
#include <tiffio.h>
#include <openjpeg.h>

namespace Slic3r::sla {
namespace {
constexpr size_t byte_limit = size_t(1) << 30;
void dimensions(size_t width, size_t height) {
    if (!width || !height || width > 32768 || height > 32768 || width * height > (size_t(128) << 20))
        throw RuntimeError("Layer image dimensions exceed the editor memory budget");
}
uint8_t gray(unsigned r, unsigned g, unsigned b) {
    return uint8_t((4899 * r + 9617 * g + 1868 * b + 8192) >> 14);
}
struct Bytes {
    const uint8_t *data;
    size_t size;
    uint32_t integer(size_t at, unsigned count, bool big = false) const {
        if (at > size || count > size - at) throw RuntimeError("Truncated layer image");
        uint32_t value = 0;
        for (unsigned n = 0; n < count; ++n)
            value |= uint32_t(data[at + n]) << (8 * (big ? count - n - 1 : n));
        return value;
    }
};
void integer(std::vector<uint8_t> &bytes, size_t at, uint32_t value, unsigned count, bool big = false) {
    for (unsigned n = 0; n < count; ++n) bytes.at(at + n) = uint8_t(value >> (8 * (big ? count - n - 1 : n)));
}
std::vector<uint8_t> bmp_encode(const png::ImageGreyscale &image) {
    const size_t stride = (image.cols + 3) & ~size_t(3);
    std::vector<uint8_t> bytes(1078 + stride * image.rows, 0);
    bytes[0] = 'B'; bytes[1] = 'M';
    integer(bytes, 2, uint32_t(bytes.size()), 4); integer(bytes, 10, 1078, 4);
    integer(bytes, 14, 40, 4); integer(bytes, 18, uint32_t(image.cols), 4);
    integer(bytes, 22, uint32_t(image.rows), 4); integer(bytes, 26, 1, 2); integer(bytes, 28, 8, 2);
    integer(bytes, 34, uint32_t(stride * image.rows), 4); integer(bytes, 46, 256, 4);
    for (unsigned n = 0; n < 256; ++n) bytes[54 + 4*n] = bytes[55 + 4*n] = bytes[56 + 4*n] = uint8_t(n);
    for (size_t y = 0; y < image.rows; ++y)
        std::copy_n(image.buf.data() + y * image.cols, image.cols, bytes.data() + 1078 + (image.rows - y - 1) * stride);
    return bytes;
}
png::ImageGreyscale bmp_decode(Bytes bytes) {
    if (bytes.integer(0, 2) != 0x4d42) throw RuntimeError("Invalid BMP signature");
    const uint32_t dib = bytes.integer(14, 4);
    if (dib != 12 && dib < 40) throw RuntimeError("Unsupported BMP header");
    if (dib > bytes.size - 14) throw RuntimeError("Truncated BMP header");
    const int64_t width = dib == 12 ? bytes.integer(18, 2) : int32_t(bytes.integer(18, 4));
    const int64_t signed_height = dib == 12 ? bytes.integer(20, 2) : int32_t(bytes.integer(22, 4));
    const unsigned bits = bytes.integer(dib == 12 ? 24 : 28, 2);
    const auto planes = bytes.integer(dib == 12 ? 22 : 26, 2);
    const unsigned compression = dib == 12 ? 0 : bytes.integer(30, 4);
    if (width <= 0 || !signed_height || planes != 1 ||
        (bits != 1 && bits != 4 && bits != 8 && bits != 16 && bits != 24 && bits != 32))
        throw RuntimeError("Unsupported BMP pixel layout");
    const size_t height = size_t(signed_height < 0 ? -signed_height : signed_height);
    dimensions(size_t(width), height);
    if (compression != 0 && compression != 1 && compression != 2 && compression != 3 && compression != 6)
        throw RuntimeError("Unsupported BMP compression");
    const size_t offset = bytes.integer(10, 4), palette_offset = 14 + dib;
    if (offset > bytes.size || offset < palette_offset) throw RuntimeError("Invalid BMP data offset");
    const size_t palette_step = dib == 12 ? 3 : 4;
    const size_t colors = bits <= 8 ? (dib == 12 || !bytes.integer(46, 4) ? size_t(1) << bits : bytes.integer(46, 4)) : 0;
    if (colors > 256 || colors * palette_step > offset - palette_offset)
        throw RuntimeError("Incomplete BMP palette");
    auto palette = [&](unsigned index) {
        if (index >= colors) throw RuntimeError("BMP pixel exceeds its palette");
        const auto p = bytes.data + palette_offset + index * palette_step;
        return gray(p[2], p[1], p[0]);
    };
    png::ImageGreyscale image; image.cols = size_t(width); image.rows = height;
    image.buf.assign(image.cols * height, colors ? palette(0) : 0);
    auto put = [&](size_t x, size_t row, unsigned index) {
        if (x >= image.cols || row >= height) throw RuntimeError("BMP run exceeds its dimensions");
        image.buf[(signed_height < 0 ? row : height - row - 1) * image.cols + x] = palette(index);
    };
    if (compression == 1 || compression == 2) {
        if ((compression == 1 && bits != 8) || (compression == 2 && bits != 4) || signed_height < 0)
            throw RuntimeError("Invalid BMP RLE layout");
        size_t cursor = offset, x = 0, y = 0; bool ended = false;
        while (cursor < bytes.size && !ended) {
            const unsigned count = bytes.integer(cursor++, 1), value = bytes.integer(cursor++, 1);
            if (count) {
                for (unsigned n = 0; n < count; ++n) put(x++, y, bits == 8 ? value : (n % 2 ? value & 15 : value >> 4));
            } else if (!value) { x = 0; ++y; }
            else if (value == 1) ended = true;
            else if (value == 2) {
                x += bytes.integer(cursor++, 1); y += bytes.integer(cursor++, 1);
                if (x > image.cols || y >= height) throw RuntimeError("Invalid BMP RLE delta");
            } else {
                const size_t length = bits == 8 ? value : (value + 1) / 2;
                for (unsigned n = 0; n < value; ++n) {
                    const unsigned v = bytes.integer(cursor + (bits == 8 ? n : n/2), 1);
                    put(x++, y, bits == 8 ? v : (n % 2 ? v & 15 : v >> 4));
                }
                cursor += length + (length & 1);
            }
        }
        if (!ended) throw RuntimeError("Incomplete BMP RLE image");
        return image;
    }
    if ((compression == 3 || compression == 6) && bits != 16 && bits != 32)
        throw RuntimeError("Invalid BMP bitfields");
    const size_t stride = ((image.cols * bits + 31) / 32) * 4;
    if (height > (bytes.size - offset) / stride) throw RuntimeError("Truncated BMP pixels");
    std::array<uint32_t,3> masks{bits == 16 ? 0x7c00u : 0xff0000u, bits == 16 ? 0x3e0u : 0xff00u, bits == 16 ? 0x1fu : 0xffu};
    if (compression == 3 || compression == 6) {
        if ((dib == 40 && offset < (compression == 6 ? 70u : 66u)) || (dib != 40 && dib < 52))
            throw RuntimeError("Incomplete BMP channel masks");
        for (unsigned n = 0; n < 3; ++n) masks[n] = bytes.integer(54 + 4*n, 4);
        for (unsigned n = 0; n < 3; ++n) {
            const auto mask = masks[n];
            unsigned shift = 0;
            if (!mask) throw RuntimeError("Invalid BMP channel mask");
            while (!(mask & (uint32_t(1) << shift))) ++shift;
            const auto field = mask >> shift;
            if ((field & (field+1)) || (bits == 16 && mask > 0xffff) ||
                (mask & masks[(n+1)%3]) || (mask & masks[(n+2)%3]))
                throw RuntimeError("Invalid BMP channel mask");
        }
    }
    auto component = [](uint32_t pixel, uint32_t mask) {
        if (!mask) throw RuntimeError("Invalid BMP channel mask");
        unsigned shift = 0; while (!(mask & 1)) { mask >>= 1; ++shift; }
        return unsigned((uint64_t((pixel >> shift) & mask) * 255) / mask);
    };
    for (size_t y = 0; y < height; ++y) for (size_t x = 0; x < image.cols; ++x) {
        const size_t p = offset + y * stride;
        uint8_t value;
        if (bits <= 8) {
            const auto byte = bytes.integer(p + x * bits / 8, 1);
            value = palette((byte >> (8 - bits - (x * bits % 8))) & ((1u << bits) - 1));
        } else if (bits == 24) {
            const auto pixel = bytes.data + p + 3*x; value = gray(pixel[2], pixel[1], pixel[0]);
        } else {
            const auto pixel = bytes.integer(p + x * bits / 8, bits/8);
            value = gray(component(pixel, masks[0]), component(pixel, masks[1]), component(pixel, masks[2]));
        }
        image.buf[(signed_height < 0 ? y : height-y-1) * image.cols + x] = value;
    }
    return image;
}
bool whitespace(uint8_t c) { return c == ' ' || c == '\n' || c == '\r' || c == '\t' || c == '\f' || c == '\v'; }
struct PnmReader {
    Bytes bytes; size_t cursor = 0;
    std::string token() {
        for (;;) {
            while (cursor < bytes.size && whitespace(bytes.data[cursor])) ++cursor;
            if (cursor == bytes.size || bytes.data[cursor] != '#') break;
            while (cursor < bytes.size && bytes.data[cursor] != '\n') ++cursor;
        }
        const size_t start = cursor;
        while (cursor < bytes.size && !whitespace(bytes.data[cursor]) && bytes.data[cursor] != '#') ++cursor;
        if (start == cursor || cursor-start > 32) throw RuntimeError("Invalid PNM header or sample");
        return std::string(reinterpret_cast<const char *>(bytes.data + start), cursor-start);
    }
    unsigned number() {
        const auto text = token(); uint64_t value = 0;
        for (char c : text) {
            if (c < '0' || c > '9' || value > UINT32_MAX / 10) throw RuntimeError("Invalid PNM integer");
            value = value * 10 + unsigned(c-'0');
        }
        if (value > UINT32_MAX) throw RuntimeError("PNM integer overflow");
        return unsigned(value);
    }
    void raster_separator() {
        if (cursor >= bytes.size || !whitespace(bytes.data[cursor])) throw RuntimeError("Missing PNM raster separator");
        if (bytes.data[cursor++] == '\r' && cursor < bytes.size && bytes.data[cursor] == '\n') ++cursor;
        // Never skip arbitrary whitespace: the first raster byte may be 0x0a.
    }
};
std::vector<uint8_t> pnm_encode(const png::ImageGreyscale &image, bool binary) {
    const std::string header = std::string(binary ? "P4\n" : "P5\n") + std::to_string(image.cols) + " " + std::to_string(image.rows) + (binary ? "\n" : "\n255\n");
    std::vector<uint8_t> bytes(header.begin(), header.end());
    if (!binary) { bytes.insert(bytes.end(), image.buf.begin(), image.buf.end()); return bytes; }
    const size_t offset = bytes.size(), stride = (image.cols+7)/8;
    bytes.resize(offset + stride * image.rows, 0);
    for (size_t y = 0; y < image.rows; ++y) for (size_t x = 0; x < image.cols; ++x)
        if (image.buf[y*image.cols+x] == 0) bytes[offset+y*stride+x/8] |= uint8_t(128 >> (x%8));
    return bytes;
}
png::ImageGreyscale pnm_decode(Bytes bytes) {
    PnmReader reader{bytes}; const auto magic = reader.token();
    if (magic.size() != 2 || magic[0] != 'P' || magic[1] < '1' || magic[1] > '6') throw RuntimeError("Invalid PNM signature");
    const unsigned kind = unsigned(magic[1]-'0');
    png::ImageGreyscale image; image.cols = reader.number(); image.rows = reader.number(); dimensions(image.cols, image.rows);
    const unsigned maximum = kind == 1 || kind == 4 ? 1 : reader.number();
    if (!maximum || maximum > 65535) throw RuntimeError("Unsupported PNM sample range");
    if (kind >= 4) reader.raster_separator();
    image.buf.resize(image.cols * image.rows);
    if (kind == 4) {
        const size_t stride = (image.cols+7)/8;
        if (image.rows > (bytes.size-reader.cursor)/stride) throw RuntimeError("Truncated PBM pixels");
        for (size_t y = 0; y < image.rows; ++y) for (size_t x = 0; x < image.cols; ++x)
            image.buf[y*image.cols+x] = bytes.data[reader.cursor+y*stride+x/8] & (128 >> (x%8)) ? 0 : 255;
        return image;
    }
    auto sample = [&]() {
        unsigned value;
        if (kind <= 3) value = reader.number();
        else { const unsigned size = maximum > 255 ? 2 : 1; value = bytes.integer(reader.cursor, size, true); reader.cursor += size; }
        if (value > maximum) throw RuntimeError("PNM sample exceeds its maximum");
        // Match Imread Grayscale: binary 8-bit PNM retains stored samples;
        // ASCII 8-bit PNM scales Maxval, while 16-bit PNM takes the high byte.
        return maximum > 255 ? value >> 8 : kind <= 3 ? value * 255 / maximum : value;
    };
    for (auto &pixel : image.buf) {
        if (kind == 1) pixel = uint8_t(255 - sample());
        else if (kind == 3 || kind == 6) { const auto r = sample(), g = sample(), b = sample(); pixel = gray(r,g,b); }
        else pixel = uint8_t(sample());
    }
    return image;
}
std::vector<uint8_t> sun_encode(const png::ImageGreyscale &image) {
    const size_t stride = (image.cols+1) & ~size_t(1);
    // OpenCV's Sun raster decoder returns black for 8-bit files without
    // a palette (including files its own writer creates). An explicit linear
    // palette preserves grayscale in both UVtools and independent decoders.
    constexpr size_t raster_offset = 32+768;
    std::vector<uint8_t> bytes(raster_offset+stride*image.rows,0);
    integer(bytes,0,0x59a66a95,4,true); integer(bytes,4,uint32_t(image.cols),4,true); integer(bytes,8,uint32_t(image.rows),4,true);
    integer(bytes,12,8,4,true); integer(bytes,16,uint32_t(stride*image.rows),4,true); integer(bytes,20,1,4,true);
    integer(bytes,24,1,4,true); integer(bytes,28,768,4,true);
    for (size_t n=0;n<256;++n) bytes[32+n]=bytes[32+256+n]=bytes[32+512+n]=uint8_t(n);
    for (size_t y=0;y<image.rows;++y) std::copy_n(image.buf.data()+y*image.cols,image.cols,bytes.data()+raster_offset+y*stride);
    return bytes;
}
png::ImageGreyscale sun_decode(Bytes bytes) {
    if (bytes.integer(0,4,true)!=0x59a66a95) throw RuntimeError("Invalid Sun raster signature");
    png::ImageGreyscale image; image.cols=bytes.integer(4,4,true); image.rows=bytes.integer(8,4,true); dimensions(image.cols,image.rows);
    const auto bits=bytes.integer(12,4,true),type=bytes.integer(20,4,true),map=bytes.integer(24,4,true),mapsize=bytes.integer(28,4,true);
    if ((bits!=1&&bits!=8&&bits!=24&&bits!=32)||type>3||map>1||mapsize>768||mapsize>bytes.size-32)
        throw RuntimeError("Unsupported Sun raster layout");
    const size_t offset=32+mapsize,stride=((image.cols*bits+15)/16)*2,expected=stride*image.rows;
    if (expected>byte_limit) throw RuntimeError("Sun raster working image exceeds the memory budget");
    std::vector<uint8_t> expanded;
    const uint8_t *raster=bytes.data+offset;
    if (type==2) {
        expanded.reserve(expected);
        for (size_t at=offset;at<bytes.size&&expanded.size()<expected;) {
            uint8_t value=uint8_t(bytes.integer(at++,1));size_t count=1;
            if (value==128) {
                const auto run=bytes.integer(at++,1);
                if (run) { count=run+1;value=uint8_t(bytes.integer(at++,1)); }
            }
            if (count>expected-expanded.size()) throw RuntimeError("Sun raster RLE exceeds its dimensions");
            expanded.insert(expanded.end(),count,value);
        }
        if (expanded.size()!=expected) throw RuntimeError("Incomplete Sun raster RLE");
        raster=expanded.data();
    } else if (expected>bytes.size-offset) throw RuntimeError("Truncated Sun raster pixels");
    image.buf.resize(image.cols*image.rows);
    for (size_t y=0;y<image.rows;++y) for (size_t x=0;x<image.cols;++x) {
        const auto *row=raster+y*stride;uint8_t value;
        if (bits<=8) {
            const unsigned index=bits==8?row[x]:((row[x/8]>>(7-x%8))&1);
            if (map==1&&mapsize) {
                if (mapsize%3||index>=mapsize/3) throw RuntimeError("Invalid Sun raster palette");
                value=gray(bytes.data[32+index],bytes.data[32+mapsize/3+index],bytes.data[32+2*mapsize/3+index]);
            } else value=bits==8?uint8_t(index):(index?0:255);
        } else {
            const auto *pixel=row+x*(bits/8)+(bits==32?1:0);
            value=type==3?gray(pixel[0],pixel[1],pixel[2]):gray(pixel[2],pixel[1],pixel[0]);
        }
        image.buf[y*image.cols+x]=value;
    }
    return image;
}

// Shared bounded memory stream for TIFF and JPEG 2000. C callbacks report an
// error instead of letting a C++ allocation exception escape through a codec.
struct Memory {
    Bytes input{nullptr,0}; std::vector<uint8_t> output; size_t cursor=0; bool writing=false,failed=false;
    size_t size() const {return writing?output.size():input.size;}
    size_t read(void *buffer,size_t count) {
        count=std::min(count,size()-std::min(cursor,size()));
        if(count)std::memcpy(buffer,(writing?output.data():input.data)+cursor,count);
        cursor+=count;return count;
    }
    bool seek(int64_t offset,int origin) {
        const uint64_t base=origin==SEEK_SET?0:origin==SEEK_CUR?cursor:origin==SEEK_END?size():UINT64_MAX;
        if(base==UINT64_MAX||(offset<0&&uint64_t(-(offset+1))+1>base)||
           (offset>=0&&uint64_t(offset)>UINT64_MAX-base))return false;
        const uint64_t target=offset<0?base-(uint64_t(-(offset+1))+1):base+uint64_t(offset);
        if(target>(writing?byte_limit:size()))return false;
        cursor=size_t(target);return true;
    }
    bool write(const void *buffer,size_t count) noexcept {
        if(count>byte_limit-std::min(cursor,byte_limit)){failed=true;return false;}
        try {if(cursor+count>output.size())output.resize(cursor+count);if(count)std::memcpy(output.data()+cursor,buffer,count);cursor+=count;return true;}
        catch(...){failed=true;return false;}
    }
};
TIFF *tiff_stream(Memory &memory) {
    return TIFFClientOpen("layer image",memory.writing?"w":"r",&memory,
        [](thandle_t h,void *b,tmsize_t n)->tmsize_t{return n<0?0:tmsize_t(static_cast<Memory *>(h)->read(b,size_t(n)));},
        [](thandle_t h,void *b,tmsize_t n)->tmsize_t{return n<0||!static_cast<Memory *>(h)->write(b,size_t(n))?-1:n;},
        [](thandle_t h,toff_t offset,int origin)->toff_t{auto &m=*static_cast<Memory *>(h);return m.seek(int64_t(offset),origin)?toff_t(m.cursor):toff_t(-1);},
        [](thandle_t)->int{return 0;},[](thandle_t h)->toff_t{return static_cast<Memory *>(h)->size();},nullptr,nullptr);
}
std::vector<uint8_t> tiff_encode(const png::ImageGreyscale &image) {
    Memory memory;memory.writing=true;
    {std::unique_ptr<TIFF,decltype(&TIFFClose)> file(tiff_stream(memory),TIFFClose);
    if(!file)throw RuntimeError("Cannot create TIFF image");
    TIFFSetField(file.get(),TIFFTAG_IMAGEWIDTH,uint32_t(image.cols));TIFFSetField(file.get(),TIFFTAG_IMAGELENGTH,uint32_t(image.rows));
    TIFFSetField(file.get(),TIFFTAG_BITSPERSAMPLE,8);TIFFSetField(file.get(),TIFFTAG_SAMPLESPERPIXEL,1);
    TIFFSetField(file.get(),TIFFTAG_PHOTOMETRIC,PHOTOMETRIC_MINISBLACK);TIFFSetField(file.get(),TIFFTAG_PLANARCONFIG,PLANARCONFIG_CONTIG);
    TIFFSetField(file.get(),TIFFTAG_COMPRESSION,COMPRESSION_LZW);TIFFSetField(file.get(),TIFFTAG_PREDICTOR,PREDICTOR_HORIZONTAL);
    TIFFSetField(file.get(),TIFFTAG_ORIENTATION,ORIENTATION_TOPLEFT);
    TIFFSetField(file.get(),TIFFTAG_ROWSPERSTRIP,TIFFDefaultStripSize(file.get(),0));
    for(size_t y=0;y<image.rows;++y)
        if(TIFFWriteScanline(file.get(),const_cast<uint8_t *>(image.buf.data()+y*image.cols),uint32_t(y),0)<0)
            throw RuntimeError("Cannot encode TIFF scanline");
    if(!TIFFWriteDirectory(file.get()))throw RuntimeError("Cannot finish TIFF directory");}
    if(memory.failed)throw RuntimeError("TIFF output exceeds the memory budget");
    return std::move(memory.output);
}
png::ImageGreyscale tiff_decode(Bytes bytes) {
    Memory memory;memory.input=bytes;
    std::unique_ptr<TIFF,decltype(&TIFFClose)> file(tiff_stream(memory),TIFFClose);
    if(!file)throw RuntimeError("Invalid TIFF image");
    uint32_t width=0,height=0;TIFFGetField(file.get(),TIFFTAG_IMAGEWIDTH,&width);TIFFGetField(file.get(),TIFFTAG_IMAGELENGTH,&height);dimensions(width,height);
    if(TIFFNumberOfDirectories(file.get())!=1)throw RuntimeError("Multi-page TIFF requires a stack importer; no pages were discarded");
    png::ImageGreyscale image;image.cols=width;image.rows=height;image.buf.resize(size_t(width)*height);
    uint16_t bits=0,samples=0,photo=0,orientation=0,sample_format=0;
    TIFFGetFieldDefaulted(file.get(),TIFFTAG_BITSPERSAMPLE,&bits);TIFFGetFieldDefaulted(file.get(),TIFFTAG_SAMPLESPERPIXEL,&samples);
    TIFFGetFieldDefaulted(file.get(),TIFFTAG_PHOTOMETRIC,&photo);TIFFGetFieldDefaulted(file.get(),TIFFTAG_ORIENTATION,&orientation);
    TIFFGetFieldDefaulted(file.get(),TIFFTAG_SAMPLEFORMAT,&sample_format);
    if(samples==1&&(bits==8||bits==16)&&sample_format==SAMPLEFORMAT_UINT&&
       (photo==PHOTOMETRIC_MINISBLACK||photo==PHOTOMETRIC_MINISWHITE)&&!TIFFIsTiled(file.get())&&orientation==ORIENTATION_TOPLEFT) {
        const auto size=TIFFScanlineSize(file.get());if(size<0||uint64_t(size)>byte_limit||uint64_t(size)<uint64_t(width)*(bits/8))throw RuntimeError("Invalid TIFF scanline size");
        std::vector<uint8_t> row(static_cast<size_t>(size));
        for(uint32_t y=0;y<height;++y) {
            if(TIFFReadScanline(file.get(),row.data(),y,0)<0)throw RuntimeError("Truncated TIFF scanline");
            for(uint32_t x=0;x<width;++x) {
                unsigned value=row[x];if(bits==16){uint16_t word;std::memcpy(&word,row.data()+2*x,2);value=word>>8;}
                image.buf[size_t(y)*width+x]=photo==PHOTOMETRIC_MINISWHITE?uint8_t(255-value):uint8_t(value);
            }
        }
    } else {
        if(image.buf.size()>(size_t(128)<<20))throw RuntimeError("TIFF color conversion exceeds the memory budget");
        std::vector<uint32_t> rgba(image.buf.size());
        if(!TIFFReadRGBAImageOriented(file.get(),width,height,rgba.data(),ORIENTATION_TOPLEFT,1))throw RuntimeError("Unsupported or damaged TIFF pixels");
        for(size_t i=0;i<rgba.size();++i)image.buf[i]=gray(TIFFGetR(rgba[i]),TIFFGetG(rgba[i]),TIFFGetB(rgba[i]));
    }
    return image;
}
struct JpegError {jpeg_error_mgr base;jmp_buf jump;char message[JMSG_LENGTH_MAX]{};};
void jpeg_failure(j_common_ptr common) {
    auto *error=reinterpret_cast<JpegError *>(common->err);common->err->format_message(common,error->message);longjmp(error->jump,1);
}
// State modified by libjpeg lives on the heap: setjmp/longjmp must not read
// indeterminate automatic variables, and every error path releases the codec.
struct JpegEncoder {
    jpeg_compress_struct codec{};
    JpegError error{};
    unsigned char *bytes = nullptr;
    unsigned long size = 0;
    ~JpegEncoder() { if (codec.mem) jpeg_destroy_compress(&codec); std::free(bytes); }
};
struct JpegDecoder {
    jpeg_decompress_struct codec{};
    JpegError error{};
    png::ImageGreyscale image;
    ~JpegDecoder() { if (codec.mem) jpeg_destroy_decompress(&codec); }
};
std::vector<uint8_t> jpeg_encode(const png::ImageGreyscale &image) {
    auto state = std::make_unique<JpegEncoder>();
    auto &codec = state->codec;
    codec.err = jpeg_std_error(&state->error.base);
    state->error.base.error_exit = jpeg_failure;
    if (setjmp(state->error.jump))
        throw RuntimeError(std::string("JPEG encoding failed: ") + state->error.message);
    jpeg_create_compress(&codec);
    jpeg_mem_dest(&codec, &state->bytes, &state->size);
    codec.image_width = JDIMENSION(image.cols);
    codec.image_height = JDIMENSION(image.rows);
    codec.input_components = 1;
    codec.in_color_space = JCS_GRAYSCALE;
    jpeg_set_defaults(&codec);
    jpeg_set_quality(&codec, 95, TRUE);
    jpeg_start_compress(&codec, TRUE);
    while (codec.next_scanline < codec.image_height) {
        auto *row = const_cast<JSAMPLE *>(image.buf.data() + size_t(codec.next_scanline) * image.cols);
        jpeg_write_scanlines(&codec, &row, 1);
    }
    jpeg_finish_compress(&codec);
    if (state->size > byte_limit) throw RuntimeError("JPEG output exceeds the memory budget");
    return {state->bytes, state->bytes + state->size};
}
png::ImageGreyscale jpeg_decode(Bytes bytes) {
    auto state = std::make_unique<JpegDecoder>();
    auto &codec = state->codec;
    codec.err = jpeg_std_error(&state->error.base);
    state->error.base.error_exit = jpeg_failure;
    // Do not silently repair truncated masks by filling missing scanlines.
    state->error.base.emit_message = [](j_common_ptr common, int level) {
        if (level < 0) jpeg_failure(common);
    };
    if (setjmp(state->error.jump))
        throw RuntimeError(std::string("JPEG decoding failed: ") + state->error.message);
    jpeg_create_decompress(&codec);
    jpeg_mem_src(&codec, bytes.data, static_cast<unsigned long>(bytes.size));
    jpeg_read_header(&codec, TRUE);
    dimensions(codec.image_width, codec.image_height);
    codec.out_color_space = JCS_GRAYSCALE;
    jpeg_start_decompress(&codec);
    state->image.cols = codec.output_width;
    state->image.rows = codec.output_height;
    state->image.buf.resize(state->image.cols * state->image.rows);
    while (codec.output_scanline < codec.output_height) {
        auto *row = state->image.buf.data() + size_t(codec.output_scanline) * state->image.cols;
        jpeg_read_scanlines(&codec, &row, 1);
    }
    jpeg_finish_decompress(&codec);
    return std::move(state->image);
}
opj_stream_t *jp2_stream(Memory &memory) {
    auto *stream=opj_stream_create(8192,!memory.writing);if(!stream)return nullptr;
    opj_stream_set_user_data(stream,&memory,nullptr);opj_stream_set_user_data_length(stream,memory.size());
    opj_stream_set_read_function(stream,[](void *b,OPJ_SIZE_T n,void *h)->OPJ_SIZE_T {const auto count=static_cast<Memory *>(h)->read(b,n);return count?count:OPJ_SIZE_T(-1);});
    opj_stream_set_write_function(stream,[](void *b,OPJ_SIZE_T n,void *h)->OPJ_SIZE_T {return static_cast<Memory *>(h)->write(b,n)?n:OPJ_SIZE_T(-1);});
    opj_stream_set_skip_function(stream,[](OPJ_OFF_T n,void *h)->OPJ_OFF_T{return static_cast<Memory *>(h)->seek(n,SEEK_CUR)?n:-1;});
    opj_stream_set_seek_function(stream,[](OPJ_OFF_T n,void *h)->OPJ_BOOL{return static_cast<Memory *>(h)->seek(n,SEEK_SET)?OPJ_TRUE:OPJ_FALSE;});
    return stream;
}
void jp2_messages(const char *message,void *context) noexcept {
    auto &out = *static_cast<std::string *>(context);
    try {
        if (message && out.size() < 4096)
            out.append(message,std::min(size_t(4096)-out.size(),std::strlen(message)));
    } catch (...) { /* A diagnostic allocation must not unwind through C. */ }
}
std::vector<uint8_t> jp2_encode(const png::ImageGreyscale &mask) {
    opj_cparameters_t parameters;opj_set_default_encoder_parameters(&parameters);
    parameters.tcp_numlayers=1;parameters.cp_disto_alloc=1;parameters.tcp_rates[0]=0;parameters.irreversible=0;
    unsigned resolutions=1;for(size_t size=std::min(mask.cols,mask.rows);size>1;size>>=1)++resolutions;
    parameters.numresolution=int(std::min(6u,resolutions));
    opj_image_cmptparm_t component{};component.dx=component.dy=1;component.w=uint32_t(mask.cols);component.h=uint32_t(mask.rows);component.prec=8;
    std::unique_ptr<opj_image_t,decltype(&opj_image_destroy)> image(opj_image_create(1,&component,OPJ_CLRSPC_GRAY),opj_image_destroy);
    std::unique_ptr<opj_codec_t,decltype(&opj_destroy_codec)> codec(opj_create_compress(OPJ_CODEC_JP2),opj_destroy_codec);
    if(!image||!codec)throw RuntimeError("Cannot allocate JPEG 2000 codec");
    image->x1=uint32_t(mask.cols);image->y1=uint32_t(mask.rows);
    for(size_t i=0;i<mask.buf.size();++i)image->comps[0].data[i]=mask.buf[i];
    std::string errors;opj_set_error_handler(codec.get(),jp2_messages,&errors);
    Memory memory;memory.writing=true;std::unique_ptr<opj_stream_t,decltype(&opj_stream_destroy)> stream(jp2_stream(memory),opj_stream_destroy);
    if(!stream||!opj_setup_encoder(codec.get(),&parameters,image.get())||!opj_start_compress(codec.get(),image.get(),stream.get())||
       !opj_encode(codec.get(),stream.get())||!opj_end_compress(codec.get(),stream.get())||memory.failed)
        throw RuntimeError("JPEG 2000 encoding failed: "+errors);
    return std::move(memory.output);
}
png::ImageGreyscale jp2_decode(Bytes bytes) {
    std::unique_ptr<opj_codec_t,decltype(&opj_destroy_codec)> codec(opj_create_decompress(OPJ_CODEC_JP2),opj_destroy_codec);
    Memory memory;memory.input=bytes;std::unique_ptr<opj_stream_t,decltype(&opj_stream_destroy)> stream(jp2_stream(memory),opj_stream_destroy);
    if(!codec||!stream)throw RuntimeError("Cannot allocate JPEG 2000 decoder");
    std::string errors;opj_set_error_handler(codec.get(),jp2_messages,&errors);opj_dparameters_t parameters;opj_set_default_decoder_parameters(&parameters);
    opj_image_t *raw=nullptr;
    if(!opj_setup_decoder(codec.get(),&parameters)||!opj_read_header(stream.get(),codec.get(),&raw)) {
        if (raw) opj_image_destroy(raw);
        throw RuntimeError("Invalid JPEG 2000 header: "+errors);
    }
    std::unique_ptr<opj_image_t,decltype(&opj_image_destroy)> image(raw,opj_image_destroy);
    if(image->x1<=image->x0||image->y1<=image->y0)throw RuntimeError("Invalid JPEG 2000 bounds");
    png::ImageGreyscale result;result.cols=image->x1-image->x0;result.rows=image->y1-image->y0;dimensions(result.cols,result.rows);
    uint64_t component_bytes = 0;
    if (image->color_space == OPJ_CLRSPC_SYCC || image->color_space == OPJ_CLRSPC_EYCC || image->color_space == OPJ_CLRSPC_CMYK)
        throw RuntimeError("This JPEG 2000 color space requires a dedicated color converter");
    if (!image->numcomps || image->numcomps > 4) throw RuntimeError("Unsupported JPEG 2000 channels");
    for (unsigned n = 0; n < image->numcomps; ++n) {
        const auto &c = image->comps[n];
        dimensions(c.w, c.h);
        if (!c.dx || !c.dy || c.dx > 32768 || c.dy > 32768 || !c.prec || c.prec > 31 ||
            c.x0 != image->x0 || c.y0 != image->y0 ||
            c.w != (result.cols+c.dx-1)/c.dx || c.h != (result.rows+c.dy-1)/c.dy)
            throw RuntimeError("Unsupported JPEG 2000 component layout");
        component_bytes += uint64_t(c.w) * c.h * sizeof(OPJ_INT32);
    }
    if (component_bytes > byte_limit) throw RuntimeError("JPEG 2000 exceeds the memory budget");
    if(!opj_decode(codec.get(),stream.get(),image.get())||!opj_end_decompress(codec.get(),stream.get()))
        throw RuntimeError("JPEG 2000 decoding failed: "+errors);
    auto sample=[&](unsigned channel,size_t x,size_t y) {
        const auto &c=image->comps[channel];
        if(!c.data||!c.w||!c.h||!c.dx||!c.dy||!c.prec||c.prec>31)throw RuntimeError("Unsupported JPEG 2000 component");
        const auto sx=std::min(size_t(c.w-1),x/c.dx),sy=std::min(size_t(c.h-1),y/c.dy);
        int64_t v=c.data[sy*c.w+sx];if(c.sgnd)v+=int64_t(1)<<(c.prec-1);
        v=std::clamp(v,int64_t(0),(int64_t(1)<<c.prec)-1);
        return unsigned(c.prec>8?v>>(c.prec-8):v*255/((1<<c.prec)-1));
    };
    result.buf.resize(result.cols*result.rows);
    for(size_t y=0;y<result.rows;++y)for(size_t x=0;x<result.cols;++x)
        result.buf[y*result.cols+x]=image->numcomps<3?uint8_t(sample(0,x,y)):
            gray(sample(0,x,y),sample(1,x,y),sample(2,x,y));
    return result;
}
}
const std::array<LayerImageFormat,11> &layer_image_formats() {
    static const std::array<LayerImageFormat,11> formats{{
        {"png","PNG mask",false,false},{"jpg","JPEG mask",true,false},{"jpeg","JPEG mask",true,false},
        {"jp2","JPEG 2000 mask",false,false},{"tif","TIFF mask",false,false},{"tiff","TIFF mask",false,false},
        {"bmp","Bitmap mask",false,false},{"pbm","PBM black/white mask",true,true},{"pgm","PGM grayscale mask",false,false},
        {"sr","Sun raster mask",false,false},{"ras","Sun raster mask",false,false}}};
    return formats;
}
const LayerImageFormat *find_layer_image_format(const std::string &extension) {
    std::string normalized=extension;if(!normalized.empty()&&normalized.front()=='.')normalized.erase(0,1);
    for(char &c:normalized)if(c>='A'&&c<='Z')c=char(c-'A'+'a');
    for(const auto &format:layer_image_formats())if(normalized==format.id)return &format;
    return nullptr;
}
std::vector<uint8_t> encode_layer_image(const png::ImageGreyscale &image,const std::string &format) {
    dimensions(image.cols,image.rows);if(image.buf.size()!=image.cols*image.rows)throw RuntimeError("Incomplete layer image pixels");
    const auto *entry=find_layer_image_format(format);if(!entry)throw RuntimeError("Unknown layer image format");
    const std::string id=entry->id;
    if(id=="png") {const auto encoded=PNGRasterEncoder{}(image.buf.data(),image.cols,image.rows,1);const auto *bytes=static_cast<const uint8_t *>(encoded.data());return {bytes,bytes+encoded.size()};}
    if(id=="bmp")return bmp_encode(image);
    if(id=="pbm"||id=="pgm")return pnm_encode(image,id=="pbm");
    if(id=="sr"||id=="ras")return sun_encode(image);
    if(id=="jpg"||id=="jpeg")return jpeg_encode(image);
    if(id=="tif"||id=="tiff")return tiff_encode(image);
    return jp2_encode(image);
}
png::ImageGreyscale decode_layer_image(png::ReadBuf input,const std::string &format) {
    if(!input.buf||!input.sz||input.sz>byte_limit)throw RuntimeError("Empty layer image or file exceeds the memory budget");
    const auto *entry=find_layer_image_format(format);if(!entry)throw RuntimeError("Unknown layer image format");
    const std::string id=entry->id;const Bytes bytes{static_cast<const uint8_t *>(input.buf),input.sz};
    if(id=="png") {
        if (bytes.size < 33 || !png::is_png(input) || bytes.integer(8,4,true) != 13 ||
            bytes.integer(12,4,true) != 0x49484452) throw RuntimeError("Invalid PNG header");
        dimensions(bytes.integer(16,4,true),bytes.integer(20,4,true));
        png::ImageGreyscale image;if(!png::decode_png(input,image)&&!png::decode_color_png(input,image))throw RuntimeError("Unsupported or damaged PNG mask");return image;}
    if(id=="bmp")return bmp_decode(bytes);
    if(id=="pbm"||id=="pgm")return pnm_decode(bytes);
    if(id=="sr"||id=="ras")return sun_decode(bytes);
    if(id=="jpg"||id=="jpeg")return jpeg_decode(bytes);
    if(id=="tif"||id=="tiff")return tiff_decode(bytes);
    return jp2_decode(bytes);
}
}
