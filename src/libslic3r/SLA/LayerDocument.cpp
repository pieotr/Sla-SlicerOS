#include "LayerDocument.hpp"
#include "MaskFilter.hpp"
#include "MedianFilter.hpp"
#include "ColorMask.hpp"
#include "RasterBase.hpp"
#include "libslic3r/miniz_extension.hpp"
#include "libslic3r/Zipper.hpp"
#include "libslic3r/Exception.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/Format/CrealityCXDLPv1Archive.hpp"
#include "libslic3r/Format/CrealityContourArea.hpp"
#include "libslic3r/Format/CrealityChecksum.hpp"
#include "libslic3r/Format/CrealityCXDLPv4.hpp"
#include "libslic3r/Format/CrealityCXDLPv4Layout.hpp"
#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/xml_parser.hpp>
#include <cstring>
#include <boost/algorithm/string.hpp>
#include <algorithm>
#include <set>
#include <map>
#include <limits>
#include <deque>
#include <nlohmann/json.hpp>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <optional>
#include <unordered_map>
#include <string_view>

namespace Slic3r::sla {
namespace {
struct CancelledMaskEdit {};
// Material volume follows actual nonzero pixels and per-layer Z, rather than
// a stale mesh estimate. The bounded cache compares complete immutable PNGs.
double mask_volume_ml(const LayerDocument &document,double width,double height,const std::vector<double> &positions)
{
    if(positions.size()!=document.layer_count()) throw RuntimeError("Archived SLA layer positions are incomplete");
    const auto first=document.image(0);
    const double area=width*height/(double(first.cols)*first.rows);
    std::unordered_map<std::string_view,uint64_t> counts;
    double volume=0.,previous=0.;
    for(size_t i=0;i<positions.size();++i) {
        const double z=positions[i];
        if(!std::isfinite(z) || z<=previous) throw RuntimeError("Archived SLA layer positions must increase");
        const auto encoded=document.encoded_layer(i);
        const std::string_view key(static_cast<const char *>(encoded.buf),encoded.sz);
        const auto found=counts.find(key);
        uint64_t count;
        if(found!=counts.end()) count=found->second;
        else {
            const auto mask=i?document.image(i):png::ImageGreyscale{};
            const auto &image=i?mask:first;
            if(image.cols!=first.cols || image.rows!=first.rows) throw RuntimeError("Archived SLA mask dimensions differ");
            count=std::count_if(image.buf.begin(),image.buf.end(),[](uint8_t value){return value!=0;});
            if(counts.size()<1024) counts.emplace(key,count);
        }
        volume+=count*area*(z-previous);previous=z;
    }
    if(!std::isfinite(volume)) throw RuntimeError("Archived SLA material volume is invalid");
    return volume/1000.;
}
// Separable square-kernel grayscale morphology. A monotonic queue makes
// runtime linear in pixel count, rather than proportional to kernel area.
// Reflect101 matches UVtools' CPU boundary convention, including 1-pixel axes.
std::vector<uint8_t> morphology(const std::vector<uint8_t> &input, size_t width,
                                size_t height, unsigned radius, bool dilate, const std::function<bool()> &cancelled)
{
    auto reflect = [](int coordinate, int length) {
        if (length == 1) return 0;
        const int period = 2 * (length - 1);
        coordinate %= period;
        if (coordinate < 0) coordinate += period;
        return coordinate < length ? coordinate : period - coordinate;
    };
    std::vector<uint8_t> intermediate(input.size()), output(input.size());
    auto pass = [&](const auto &source, auto &target, bool vertical) {
        const int length = int(vertical ? height : width);
        const size_t lines = vertical ? width : height;
        auto offset = [&](size_t line, int position) {
            return vertical ? size_t(position) * width + line : line * width + size_t(position);
        };
        for (size_t line = 0; line < lines; ++line) {
            if (cancelled()) throw CancelledMaskEdit{};
            std::deque<std::pair<int, uint8_t>> queue;
            for (int position = -int(radius); position < length + int(radius); ++position) {
                const uint8_t value = source[offset(line, reflect(position, length))];
                while (!queue.empty() && (dilate ? queue.back().second <= value : queue.back().second >= value))
                    queue.pop_back();
                queue.emplace_back(position, value);
                const int first = position - 2 * int(radius);
                while (queue.front().first < first) queue.pop_front();
                if (position >= int(radius))
                    target[offset(line, position - int(radius))] = queue.front().second;
            }
        }
    };
    pass(input, intermediate, false);
    pass(intermediate, output, true);
    return output;
}
} // namespace
LayerDocument LayerDocument::from_rasters(std::vector<EncodedRaster> layers)
{
    LayerDocument result;
    result.m_entries.reserve(layers.size());
    result.m_layers.reserve(layers.size());
    uint64_t total = 0;
    for (size_t i = 0; i < layers.size(); ++i) {
        const auto &raster = layers[i];
        if (!raster.size()) throw RuntimeError("The sliced document contains an empty raster");
        if (raster.size() > (uint64_t(1) << 30) - total)
            throw RuntimeError("Layer editor document exceeds the 1 GiB memory budget");
        total += raster.size();
        const auto *bytes = static_cast<const uint8_t *>(raster.data());
        result.m_layers.push_back(i);
        result.m_entries.push_back({"layer" + std::to_string(i), {bytes, bytes + raster.size()}});
    }
    return result;
}

LayerDocument LayerDocument::load_sl1(const std::string &path)
{
    auto document = load_archive(path);
    if (document.archive_format() != "SL1") throw RuntimeError("This archive is not SL1");
    return document;
}

LayerDocument LayerDocument::load_archive(const std::string &path)
{
    // Probe the contents: a standalone image is an editable mask, not a
    // printer container. Check dimensions before the PNG decoder allocates.
    boost::nowide::ifstream input(path, std::ios::binary);
    uint8_t signature[8]{};
    input.read(reinterpret_cast<char *>(signature), sizeof(signature));
    const uint8_t cxdlpv1_signature[]{0,0,0,9,'C','X','S','W'};
    if(input.gcount()==8 && std::memcmp(signature,cxdlpv1_signature,8)==0) {
        input.seekg(0,std::ios::end);const auto length=input.tellg();
        if(length<19 || length>std::streamoff(uint64_t(1)<<30)) throw RuntimeError("CXDLPv1 exceeds the editor memory budget or is truncated");
        std::vector<uint8_t> bytes(static_cast<size_t>(length));input.seekg(0);
        input.read(reinterpret_cast<char *>(bytes.data()),bytes.size());if(!input) throw FileIOError("Cannot read CXDLPv1 layers");
        if(bytes[13]==0 && bytes[14]==4) {
            const CrealityCXDLPv4Layout layout(bytes);
            LayerDocument result;result.m_archive_format="cxdlpv4";
            result.m_entries.push_back({"cxdlpv4-original",{}});
            std::unordered_map<std::string_view,size_t> cache;
            uint64_t stored=bytes.size();
            for(size_t i=0;i<layout.layers.size();++i) {
                const auto &layer=layout.layers[i];
                const auto *data=bytes.data()+layer.data+44;const size_t size=layer.length-44;
                const std::string_view key(reinterpret_cast<const char *>(data),size);
                std::vector<uint8_t> encoded_png;
                const auto cached=layout.seed?cache.end():cache.find(key);
                if(cached!=cache.end())encoded_png=result.m_entries[cached->second].bytes;
                else {
                    std::vector<uint8_t> decrypted;
                    if(layout.seed) {decrypted.assign(data,data+size);CrealityCXDLPv4Archive::crypt_mask(decrypted,layout.seed,uint32_t(i));data=decrypted.data();}
                    const auto pixels=CrealityCXDLPv4Archive::decode_mask(data,size,layout.columns,layout.rows);
                    const auto png=PNGRasterEncoder{}(pixels.data(),layout.columns,layout.rows,1);
                    if(!png.size())throw RuntimeError("Cannot encode imported CXDLPv4 mask");
                    const auto *begin=static_cast<const uint8_t *>(png.data());encoded_png.assign(begin,begin+png.size());
                    if(!layout.seed && cache.size()<1024)cache.emplace(key,result.m_entries.size());
                }
                if(encoded_png.size()>(uint64_t(1)<<30)-stored)throw RuntimeError("CXDLPv4 document exceeds the editor memory budget");
                stored+=encoded_png.size();result.m_layers.push_back(result.m_entries.size());
                result.m_entries.push_back({std::to_string(i+1)+".png",std::move(encoded_png)});
            }
            result.m_entries[0].bytes=std::move(bytes);return result;
        }
        size_t cursor=0;
        auto require=[&](size_t size){if(size>bytes.size()-cursor)throw RuntimeError("Truncated CXDLPv1 table");};
        auto be32=[&](){require(4);const auto *p=bytes.data()+cursor;cursor+=4;return(uint32_t(p[0])<<24)|(uint32_t(p[1])<<16)|(uint32_t(p[2])<<8)|p[3];};
        auto be16=[&](){require(2);const auto value=(uint16_t(bytes[cursor])<<8)|bytes[cursor+1];cursor+=2;return value;};
        auto magic=[&](){if(be32()!=9)throw RuntimeError("Invalid CXDLPv1 marker");require(9);if(std::memcmp(bytes.data()+cursor,"CXSW3DV2\0",9)!=0)throw RuntimeError("Unsupported CXDLP binary version");cursor+=9;};
        auto page=[&](){if(be16()!=0x0d0a)throw RuntimeError("Invalid CXDLPv1 page break");};
        magic();
        // v1 starts with the count/resolution; v2/v3 insert a version and a
        // length-prefixed model. A valid v1 X resolution makes those four
        // bytes much larger than the bounded model length.
        uint16_t version=1;
        size_t count=be16();
        const size_t header_probe=cursor;const auto model_length=be32();cursor=header_probe;
        if((count==2 || count==3) && model_length>0 && model_length<=256) {
            version=uint16_t(count);be32();require(model_length);
            if(bytes[cursor+model_length-1]!=0)throw RuntimeError("Invalid CXDLP printer model");
            cursor+=model_length;count=be16();
        }
        const size_t cols=be16(),rows=be16();
        if(version>=2) {require(64);cursor+=64;
            const uint32_t checksum=creality_checksum(bytes.data(),bytes.size()-4,version);
            const auto end=bytes.data()+bytes.size()-4;
            if(checksum!=((uint32_t(end[0])<<24)|(uint32_t(end[1])<<16)|(uint32_t(end[2])<<8)|end[3]))throw RuntimeError("CXDLP checksum mismatch");
        }
        if(!count || !cols || !rows || cols>16384 || rows>8192)throw RuntimeError("Invalid CXDLPv1 resolution or layer count");
        for(const size_t size:{116,290,290}){require(size*size*2);cursor+=size*size*2;page();}
        for(unsigned field=0;field<3;++field) {
            const auto length=be32();if(length%2 || length>128)throw RuntimeError("Invalid CXDLPv1 geometry string");
            std::string text;
            for(unsigned i=0;i<length/2;++i){const auto c=be16();if(c>127)throw RuntimeError("Invalid CXDLPv1 geometry encoding");text.push_back(char(c));}
            std::istringstream number(text);number.imbue(std::locale::classic());double value;
            if(!(number>>value) || !std::isfinite(value) || value<=0.)throw RuntimeError("Invalid CXDLPv1 geometry");
            number>>std::ws;if(!number.eof())throw RuntimeError("Invalid CXDLPv1 geometry");
        }
        require(22);cursor+=22; // Preserve exposure, motion and PWM verbatim.
        LayerDocument result;result.m_archive_format=version==1?"cxdlpv1":version==2?"cxdlpv2":"cxdlp";
        result.m_entries.push_back({"cxdlpv1-prefix",std::vector<uint8_t>(bytes.begin(),bytes.begin()+cursor)});
        result.m_entries.push_back({"cxdlpv1-footer",{}});
        std::vector<uint32_t> counts(count);for(auto &value:counts)value=be32();page();
        if(version>=3) {
            const size_t informative=cursor;
            for(unsigned i=0;i<2;++i) {const auto size=be32();if(!size || size>65536)throw RuntimeError("Invalid CXDLP informative string");require(size);if(bytes[cursor+size-1]!=0)throw RuntimeError("Invalid CXDLP informative string");cursor+=size;}
            require(20);cursor+=20;page();
            result.m_entries.push_back({"cxdlpv3-informative",std::vector<uint8_t>(bytes.begin()+informative,bytes.begin()+cursor)});
        }
        std::unordered_map<std::string_view,size_t> cache;
        uint64_t stored=cursor;
        for(size_t layer=0;layer<count;++layer) {
            const size_t start=cursor;const auto nonzero=be32(),lines=be32();
            if(nonzero!=counts[layer] || (version==1 && nonzero>cols*rows) || uint64_t(lines)*6>bytes.size()-cursor)
                throw RuntimeError("Invalid CXDLPv1 layer table");
            const size_t run_bytes=size_t(lines)*6;
            const std::string_view key(reinterpret_cast<const char *>(bytes.data()+start),run_bytes+8);
            std::vector<uint8_t> png_bytes;
            if(auto existing=cache.find(key);existing!=cache.end()) {
                png_bytes=result.m_entries[existing->second].bytes;cursor+=run_bytes;
            } else {
                std::vector<uint8_t> pixels(cols*rows,0);uint32_t decoded=0;
                for(size_t line=0;line<lines;++line) {
                    const auto *p=bytes.data()+cursor;cursor+=6;
                    const size_t y0=(((uint32_t(p[0])<<8)|p[1])>>3)&0x1fff;
                    const size_t y1=(((uint32_t(p[1])<<16)|(uint32_t(p[2])<<8)|p[3])>>6)&0x1fff;
                    const size_t x=((uint32_t(p[3])<<8)|p[4])&0x3fff;
                    if(y0>y1 || y1>=rows || x>=cols || !p[5])throw RuntimeError("Invalid CXDLPv1 run coordinates");
                    for(size_t y=y0;y<=y1;++y){if(pixels[y*cols+x])throw RuntimeError("Overlapping CXDLPv1 runs");pixels[y*cols+x]=p[5];++decoded;}
                }
                if(version==1 && decoded!=nonzero)throw RuntimeError("CXDLPv1 pixel count mismatch");
                const auto encoded=PNGRasterEncoder{}(pixels.data(),cols,rows,1);
                if(!encoded.size())throw RuntimeError("Cannot encode imported CXDLPv1 mask");
                const auto *data=static_cast<const uint8_t *>(encoded.data());png_bytes.assign(data,data+encoded.size());
                if(cache.size()<1024)cache.emplace(key,result.m_entries.size());
            }
            page();
            if(png_bytes.size()>(uint64_t(1)<<30)-stored)throw RuntimeError("CXDLPv1 masks exceed the editor memory budget");
            stored+=png_bytes.size();result.m_layers.push_back(result.m_entries.size());
            result.m_entries.push_back({std::to_string(layer+1)+".png",std::move(png_bytes)});
        }
        const size_t footer=cursor;magic();be32();
        if(cursor!=bytes.size())throw RuntimeError("CXDLPv1 contains unexpected trailing bytes");
        result.m_entries[1].bytes.assign(bytes.begin()+footer,bytes.end());
        return result;
    }
    if (input.gcount() == 8 && png::is_png({signature, sizeof(signature)})) {
        input.seekg(0, std::ios::end);
        const auto length = input.tellg();
        if (length < 33 || length > (uint64_t(1) << 30))
            throw RuntimeError("PNG file exceeds the editor memory budget or is truncated");
        std::vector<uint8_t> bytes(static_cast<size_t>(length));
        input.seekg(0);
        input.read(reinterpret_cast<char *>(bytes.data()), bytes.size());
        if (!input) throw FileIOError("Cannot read PNG mask");
        auto be32 = [&](size_t offset) {
            return (uint32_t(bytes[offset]) << 24) | (uint32_t(bytes[offset+1]) << 16) |
                   (uint32_t(bytes[offset+2]) << 8) | uint32_t(bytes[offset+3]);
        };
        if (be32(8) != 13 || std::memcmp(bytes.data()+12, "IHDR", 4) != 0)
            throw RuntimeError("PNG mask has no valid IHDR");
        bool image_data = false, image_end = false;
        for (size_t offset = 8; offset < bytes.size();) {
            if (bytes.size() - offset < 12) throw RuntimeError("Truncated PNG chunk");
            const size_t length = be32(offset);
            if (length > bytes.size() - offset - 12) throw RuntimeError("Truncated PNG chunk data");
            if (mz_crc32(0, bytes.data()+offset+4, length+4) != be32(offset+8+length))
                throw RuntimeError("PNG chunk checksum mismatch");
            image_data |= std::memcmp(bytes.data()+offset+4, "IDAT", 4) == 0;
            if (std::memcmp(bytes.data()+offset+4, "IEND", 4) == 0) {
                if (length || offset+12 != bytes.size()) throw RuntimeError("Invalid PNG end chunk");
                image_end = true;
            }
            offset += length + 12;
        }
        if (!image_data || !image_end) throw RuntimeError("Incomplete PNG mask");
        const size_t width = be32(16), height = be32(20);
        if (!width || !height || width > 32768 || height > 32768 || width * height > 128 * 1024 * 1024)
            throw RuntimeError("PNG raster exceeds the editor memory budget");
        if (bytes[24] == 8 && bytes[25] == 0) {
            png::ImageGreyscale decoded;
            if (!png::decode_png({bytes.data(), bytes.size()}, decoded)) throw RuntimeError("Invalid grayscale PNG mask");
        } else {
            if (width * height > 32 * 1024 * 1024)
                throw RuntimeError("Color PNG conversion exceeds the editor memory budget");
            std::vector<unsigned char> rgba;
            unsigned decoded_width = 0, decoded_height = 0;
            if (!png::decode_png(std::string(bytes.begin(), bytes.end()), rgba, decoded_width, decoded_height) ||
                decoded_width != width || decoded_height != height || rgba.size() != width * height * 4)
                throw RuntimeError("Invalid color PNG mask");
            std::vector<uint8_t> gray(width * height);
            for (size_t i = 0; i < gray.size(); ++i) {
                const unsigned luminance = (299 * rgba[4*i] + 587 * rgba[4*i+1] + 114 * rgba[4*i+2] + 500) / 1000;
                // Transparent pixels expose nothing; composite over black.
                gray[i] = uint8_t((luminance * rgba[4*i+3] + 127) / 255);
            }
            const auto encoded = PNGRasterEncoder{}(gray.data(), width, height, 1);
            if (!encoded.size()) throw RuntimeError("Cannot encode grayscale PNG mask");
            const auto *data = static_cast<const uint8_t *>(encoded.data());
            bytes.assign(data, data + encoded.size());
        }
        LayerDocument result;
        result.m_archive_format = "png";
        result.m_layers.push_back(0);
        result.m_entries.push_back({"layer.png", std::move(bytes)});
        return result;
    }
    input.close();
    struct Reader : MZ_Archive {
        explicit Reader(const std::string &p) {
            if (!open_zip_reader(&arch, p)) throw FileIOError(get_errorstr());
        }
        ~Reader() { close_zip_reader(&arch); }
    } zip(path);
    LayerDocument result;
    std::set<std::string> names;
    bool config = false, profile = false;
    // Explicit memory budget until a disk-backed archive store is available.
    constexpr uint64_t limit = uint64_t(1) << 30;
    uint64_t total = 0;
    for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&zip.arch); ++i) {
        mz_zip_archive_file_stat stat;
        if (!mz_zip_reader_file_stat(&zip.arch, i, &stat)) throw FileIOError(zip.get_errorstr());
        if (mz_zip_reader_is_file_a_directory(&zip.arch, i)) continue;
        std::string name = stat.m_filename, lower = boost::algorithm::to_lower_copy(name);
        const bool duplicate = !names.insert(lower).second;
        if (stat.m_uncomp_size > limit - total) throw RuntimeError("Layer editor archive exceeds the 1 GiB memory budget");
        total += stat.m_uncomp_size;
        Entry entry{name, std::vector<uint8_t>(size_t(stat.m_uncomp_size))};
        if (!mz_zip_reader_extract_to_mem(&zip.arch, i, entry.bytes.data(), entry.bytes.size(), 0))
            throw FileIOError(zip.get_errorstr());
        if (duplicate) {
            // Older exports repeated identical thumbnail sizes. Accept only
            // byte-identical preview assets; ambiguous masks/settings still fail.
            const auto previous=std::find_if(result.m_entries.begin(),result.m_entries.end(),
                [&](const Entry &other){return boost::algorithm::iequals(other.name,name);});
            if(boost::algorithm::starts_with(lower,"thumbnail/") && previous!=result.m_entries.end() &&
               previous->bytes==entry.bytes) {total-=stat.m_uncomp_size;continue;}
            throw RuntimeError("Duplicate layer archive entry");
        }
        config |= lower == "config.ini";
        profile |= lower == "prusaslicer.ini";
        // SL1 layers use a top-level prefix followed by a zero-padded index.
        // Keep other PNG assets intact rather than exposing them as print layers.
        bool is_layer = lower.find('/') == std::string::npos && lower.find('\\') == std::string::npos &&
                        boost::algorithm::ends_with(lower, ".png");
        size_t digits = 0;
        if (is_layer)
            for (size_t p = lower.size()-4; p > 0 && lower[p-1] >= '0' && lower[p-1] <= '9'; --p) ++digits;
        if (is_layer && digits >= 5)
            result.m_layers.push_back(result.m_entries.size());
        result.m_entries.push_back(std::move(entry));
    }
    auto uvj_config = std::find_if(result.m_entries.begin(), result.m_entries.end(), [](const Entry &entry) {
        return boost::algorithm::iequals(entry.name, "config.json");
    });
    const auto cws_manifest = std::find_if(result.m_entries.begin(), result.m_entries.end(), [](const Entry &entry) {
        return boost::algorithm::iequals(entry.name, "manifest.xml");
    });
    const auto cws_settings = std::find_if(result.m_entries.begin(), result.m_entries.end(), [](const Entry &entry) {
        return boost::algorithm::iequals(entry.name, "slice.conf");
    });
        auto xml = [](const Entry &entry) {
            if (entry.bytes.size() > 32 * 1024 * 1024) throw RuntimeError("Archive XML exceeds the metadata budget");
            const std::string data(entry.bytes.begin(), entry.bytes.end());
            // Archive XML is data. Do not accept external entities or DTDs.
            if (data.find("<!DOCTYPE") != std::string::npos || data.find("<!ENTITY") != std::string::npos)
                throw RuntimeError("Archive XML must not contain a DTD or entities");
            std::istringstream stream(data);
            boost::property_tree::ptree tree;
            boost::property_tree::read_xml(stream, tree, boost::property_tree::xml_parser::trim_whitespace);
            return tree;
        };
    const auto generic_manifest = std::find_if(result.m_entries.begin(), result.m_entries.end(), [](const Entry &entry) {
        return boost::algorithm::iequals(entry.name, "manifest.uvtools");
    });
    const auto vda_manifest = std::find_if(result.m_entries.begin(), result.m_entries.end(), [](const Entry &entry) {
        return boost::algorithm::iends_with(entry.name, ".xml");
    });
    const auto chitubox_sequence = std::find_if(result.m_entries.begin(), result.m_entries.end(), [](const Entry &entry) {
        return boost::algorithm::iequals(entry.name, "run.gcode");
    });
    if (generic_manifest != result.m_entries.end() && !(config && profile && !result.m_layers.empty()) && uvj_config == result.m_entries.end()) {
        const auto manifest = xml(*generic_manifest);
        const auto columns = manifest.get<int64_t>("Manifest.ResolutionX"), rows = manifest.get<int64_t>("Manifest.ResolutionY");
        if (columns <= 0 || rows <= 0 || columns > 32768 || rows > 32768 || columns * rows > 128 * 1024 * 1024)
            throw RuntimeError("Generic ZIP resolution exceeds the editor memory budget");
        for (const char *key : {"LayerHeight", "DisplayWidth", "DisplayHeight", "MachineZ"}) {
            const double value = manifest.get<double>(std::string("Manifest.")+key);
            if (!std::isfinite(value) || value <= 0.) throw RuntimeError("Invalid Generic ZIP geometry");
        }
        std::map<size_t, size_t> indices;
        for (size_t i = 0; i < result.m_entries.size(); ++i) {
            const auto &entry = result.m_entries[i];
            const boost::filesystem::path path(entry.name);
            if (!boost::algorithm::iequals(path.extension().string(), ".png")) continue;
            const auto stem = path.stem().string();
            if (stem.empty() || !std::all_of(stem.begin(), stem.end(), [](char c) { return c >= '0' && c <= '9'; })) continue;
            if (stem.size() > 10) throw RuntimeError("Invalid Generic ZIP mask index");
            const auto index = std::stoull(stem);
            if (!index || index > 100000 || !indices.emplace(size_t(index), i).second)
                throw RuntimeError("Invalid or duplicate Generic ZIP mask index");
            const auto &bytes = entry.bytes;
            if (bytes.size() < 29 || !png::is_png({bytes.data(), bytes.size()})) throw RuntimeError("Generic ZIP mask is not PNG");
            auto dimension = [&](size_t offset) {
                return (uint32_t(bytes[offset]) << 24) | (uint32_t(bytes[offset+1]) << 16) |
                       (uint32_t(bytes[offset+2]) << 8) | bytes[offset+3];
            };
            if (dimension(16) != columns || dimension(20) != rows || bytes[24] != 8 || bytes[25] != 0)
                throw RuntimeError("Generic ZIP editor requires matching 8-bit grayscale PNG masks");
        }
        result.m_layers.clear();
        for (const auto &[index, entry] : indices) {
            if (index != result.m_layers.size()+1) throw RuntimeError("Generic ZIP masks must be contiguous from one");
            result.m_layers.push_back(entry);
        }
        if (result.m_layers.empty()) throw RuntimeError("Generic ZIP contains no masks");
        result.m_archive_format = "genericzip";
    } else if (cws_manifest != result.m_entries.end() && !(config && profile && !result.m_layers.empty()) && uvj_config == result.m_entries.end()) {
        const auto manifest = xml(*cws_manifest);
        auto find_entry = [&](const std::string &name) -> size_t {
            const auto found = std::find_if(result.m_entries.begin(), result.m_entries.end(), [&](const Entry &entry) {
                return boost::algorithm::iequals(entry.name, name);
            });
            if (found == result.m_entries.end()) throw RuntimeError("CWS manifest references a missing archive entry");
            return size_t(found - result.m_entries.begin());
        };
        const auto slicing = xml(result.m_entries[find_entry(manifest.get<std::string>("manifest.SliceProfile.name"))]);
        find_entry(manifest.get<std::string>("manifest.GCode.name"));
        const auto columns = slicing.get<int64_t>("SliceBuildConfig.XResolution");
        const auto rows = slicing.get<int64_t>("SliceBuildConfig.YResolution");
        if (columns <= 0 || rows <= 0 || columns > 32768 || rows > 32768 || columns * rows > 128 * 1024 * 1024)
            throw RuntimeError("CWS resolution exceeds the editor memory budget");
        result.m_layers.clear();
        std::set<size_t> referenced;
        for (const auto &slice : manifest.get_child("manifest.Slices")) {
            if (slice.first == "<xmlattr>") continue;
            if (slice.first != "Slice") throw RuntimeError("Invalid CWS slice table");
            const auto index = find_entry(slice.second.get<std::string>("name"));
            if (!referenced.insert(index).second) throw RuntimeError("Duplicate CWS layer reference");
            auto &bytes = result.m_entries[index].bytes;
            if (bytes.size() < 29 || !png::is_png({bytes.data(), bytes.size()})) throw RuntimeError("CWS layer is not PNG");
            auto dimension = [&](size_t offset) {
                return (uint32_t(bytes[offset]) << 24) | (uint32_t(bytes[offset+1]) << 16) |
                       (uint32_t(bytes[offset+2]) << 8) | bytes[offset+3];
            };
            const bool xml_rgba=boost::algorithm::iends_with(path,".xml.cws") && bytes[25]==6;
            if (dimension(16) != columns || dimension(20) != rows || bytes[24] != 8 || (bytes[25] != 0 && !xml_rgba))
                throw RuntimeError("CWS masks must match the profile: 8-bit grayscale, or RGBA for XML.CWS");
            if(xml_rgba) {
                png::ImageGreyscale gray;
                if(!png::decode_color_png({bytes.data(),bytes.size()},gray) || gray.cols!=size_t(columns) || gray.rows!=size_t(rows))
                    throw RuntimeError("Invalid XML.CWS RGBA mask");
                const auto encoded=PNGRasterEncoder{}(gray.buf.data(),gray.cols,gray.rows,1);
                if(!encoded.size())throw RuntimeError("Cannot normalize XML.CWS mask");
                const auto new_total=total-bytes.size()+encoded.size();
                if(new_total>limit)throw RuntimeError("XML.CWS exceeds the editor memory budget");
                total=new_total;
                const auto *data=static_cast<const uint8_t *>(encoded.data());
                bytes.assign(data,data+encoded.size());
            }
            result.m_layers.push_back(index);
            if (result.m_layers.size() > 100000) throw RuntimeError("CWS layer count exceeds the editor limit");
        }
        if (result.m_layers.empty()) throw RuntimeError("CWS contains no layer masks");
        result.m_archive_format = boost::algorithm::iends_with(path,".xml.cws") ? "cwsxml" : "cws";
    } else if (cws_settings != result.m_entries.end() && cws_manifest == result.m_entries.end() &&
               !(config && profile && !result.m_layers.empty()) && uvj_config == result.m_entries.end()) {
        // NovaMaker has no XML manifest: slice.conf supplies dimensions/count,
        // and the numeric suffix supplies layer order independently of ZIP order.
        if(cws_settings->bytes.size()>32*1024*1024)throw RuntimeError("NovaMaker settings exceed the metadata budget");
        std::istringstream source(std::string(cws_settings->bytes.begin(),cws_settings->bytes.end()));
        std::map<std::string,std::string> settings;
        for(std::string line;std::getline(source,line);) {
            boost::algorithm::trim(line);
            if(line.empty() || line.front()=='#')continue;
            const auto separator=line.find('=');
            if(separator==std::string::npos)throw RuntimeError("Invalid NovaMaker setting");
            const auto key=boost::algorithm::trim_copy(line.substr(0,separator));
            if(key.empty() || !settings.emplace(key,boost::algorithm::trim_copy(line.substr(separator+1))).second)
                throw RuntimeError("Duplicate or empty NovaMaker setting");
        }
        auto setting=[&](const char *key)->const std::string & {
            const auto found=settings.find(key);
            if(found==settings.end())throw RuntimeError(std::string("Missing NovaMaker setting: ")+key);
            return found->second;
        };
        auto integer=[&](const char *key,size_t maximum) {
            const auto &value=setting(key);
            if(value.empty() || value.size()>10 || !std::all_of(value.begin(),value.end(),[](char c){return c>='0' && c<='9';}))
                throw RuntimeError("NovaMaker dimensions and counts must be integers");
            const auto number=std::stoull(value);
            if(!number || number>maximum)throw RuntimeError("NovaMaker dimensions or count exceed the editor limit");
            return size_t(number);
        };
        const size_t columns=integer("xres",32768),rows=integer("yres",32768),count=integer("layers_num",100000);
        if(columns*rows>128ULL*1024*1024)throw RuntimeError("NovaMaker resolution exceeds the editor memory budget");
        for(const char *key:{"xppm","yppm","thickness"}) {
            std::istringstream number(setting(key));number.imbue(std::locale::classic());double value=0;
            if(!(number>>value) || number.peek()!=std::char_traits<char>::eof() || !std::isfinite(value) || value<=0)
                throw RuntimeError("NovaMaker physical dimensions and thickness must be positive");
        }
        size_t instructions=0;
        std::map<size_t,size_t> layers;
        std::optional<bool> packed_format;
        for(size_t i=0;i<result.m_entries.size();++i) {
            auto &entry=result.m_entries[i];
            const auto name=boost::algorithm::to_lower_copy(entry.name);
            if(boost::algorithm::ends_with(name,".gcode"))++instructions;
            if(name.find('/')!=std::string::npos || name.find('\\')!=std::string::npos || !boost::algorithm::ends_with(name,".png"))continue;
            const size_t end=name.size()-4;
            size_t start=end;
            while(start && name[start-1]>='0' && name[start-1]<='9')--start;
            if(start==end)continue; // Preserve unnumbered previews/unknown assets.
            if(end-start>10)throw RuntimeError("NovaMaker layer index exceeds the editor limit");
            const auto index=std::stoull(name.substr(start,end-start));
            if(index>=count || !layers.emplace(size_t(index),i).second)
                throw RuntimeError("Duplicate or out-of-range NovaMaker layer index");
            auto &bytes=entry.bytes;
            if(bytes.size()<29 || !png::is_png({bytes.data(),bytes.size()}))throw RuntimeError("NovaMaker layer is not PNG");
            auto dimension=[&](size_t offset){return (uint32_t(bytes[offset])<<24)|(uint32_t(bytes[offset+1])<<16)|
                (uint32_t(bytes[offset+2])<<8)|bytes[offset+3];};
            const bool packed=bytes[25]==2;
            if(packed_format && *packed_format!=packed)throw RuntimeError("NovaMaker masks cannot mix grayscale and packed RGB");
            packed_format=packed;
            if((packed && columns%3) || dimension(16)!=(packed?columns/3:columns) || dimension(20)!=rows ||
                bytes[24]!=8 || (bytes[25]!=0 && !packed))
                throw RuntimeError("NovaMaker masks must match slice.conf and use grayscale PNG or packed RGB.CWS");
            png::ImageGreyscale validated;
            if(packed) {
                if(!decode_bgr_packed_mask({bytes.data(),bytes.size()},columns,rows,validated))
                    throw RuntimeError("Invalid RGB.CWS packed mask");
                const auto encoded=PNGRasterEncoder{}(validated.buf.data(),columns,rows,1);
                if(!encoded.size())throw RuntimeError("Cannot normalize RGB.CWS mask");
                const uint64_t new_total=total-bytes.size()+encoded.size();
                if(new_total>limit)throw RuntimeError("RGB.CWS exceeds the editor memory budget");
                total=new_total;
                const auto *data=static_cast<const uint8_t *>(encoded.data());bytes.assign(data,data+encoded.size());
            } else if(!png::decode_png({bytes.data(),bytes.size()},validated))throw RuntimeError("Invalid NovaMaker PNG mask");
        }
        if(instructions!=1 || layers.size()!=count)throw RuntimeError("NovaMaker requires one instruction file and all layers");
        result.m_layers.clear();
        for(const auto &[index,entry]:layers) {
            if(index!=result.m_layers.size())throw RuntimeError("NovaMaker layers must be contiguous from zero");
            result.m_layers.push_back(entry);
        }
        result.m_archive_format=packed_format && *packed_format?"cwsrgb":"cwsnovamaker";
    } else if (chitubox_sequence != result.m_entries.end() && !(config && profile && !result.m_layers.empty()) && uvj_config == result.m_entries.end()) {
        if(chitubox_sequence->bytes.size()>32*1024*1024) throw RuntimeError("Chitubox ZIP sequence exceeds the metadata budget");
        std::istringstream sequence(std::string(chitubox_sequence->bytes.begin(),chitubox_sequence->bytes.end()));
        std::map<std::string,double> header;
        const std::set<std::string> fields{"resolutionX","resolutionY","machineX","machineY","machineZ","layerHeight","totalLayer"};
        std::string line;
        while(std::getline(sequence,line)) {
            boost::algorithm::trim(line);
            if(line.empty() || line.front()!=';') continue;
            const auto separator=line.find(':'); if(separator==std::string::npos) continue;
            const auto key=boost::algorithm::trim_copy(line.substr(1,separator-1)); if(!fields.count(key)) continue;
            std::istringstream value(line.substr(separator+1)); value.imbue(std::locale::classic());
            double number;
            if(!(value>>number) || !std::isfinite(number) || number<=0.) throw RuntimeError("Invalid Chitubox ZIP header");
            value>>std::ws; if(!value.eof() || !header.emplace(key,number).second) throw RuntimeError("Duplicate or invalid Chitubox ZIP field");
        }
        if(header.size()!=fields.size()) throw RuntimeError("Incomplete Chitubox ZIP geometry header");
        const double columns=header.at("resolutionX"), rows=header.at("resolutionY"), count=header.at("totalLayer");
        if(columns!=std::floor(columns) || rows!=std::floor(rows) || columns>32768 || rows>32768 || columns*rows>128*1024*1024 ||
            count!=std::floor(count) || count>100000) throw RuntimeError("Chitubox ZIP exceeds the editor limits");
        std::map<size_t,size_t> indices;
        for(size_t i=0;i<result.m_entries.size();++i) {
            const boost::filesystem::path name(result.m_entries[i].name);
            const auto stem=name.stem().string();
            if(!boost::algorithm::iequals(name.extension().string(),".png") || stem.empty() ||
                !std::all_of(stem.begin(),stem.end(),[](char c){return c>='0' && c<='9';})) continue;
            if(stem.size()>6) throw RuntimeError("Chitubox ZIP layer index exceeds the editor limit");
            const size_t index=std::stoul(stem);
            if(!index || index>size_t(count) || !indices.emplace(index,i).second) throw RuntimeError("Invalid Chitubox ZIP layer index");
            const auto &bytes=result.m_entries[i].bytes;
            if(bytes.size()<29 || !png::is_png({bytes.data(),bytes.size()})) throw RuntimeError("Chitubox ZIP mask is not PNG");
            auto dimension=[&](size_t offset){return(uint32_t(bytes[offset])<<24)|(uint32_t(bytes[offset+1])<<16)|(uint32_t(bytes[offset+2])<<8)|bytes[offset+3];};
            if(dimension(16)!=columns || dimension(20)!=rows || bytes[24]!=8 || bytes[25]!=0)
                throw RuntimeError("Chitubox ZIP editor requires matching 8-bit grayscale PNG masks");
        }
        if(indices.size()!=size_t(count)) throw RuntimeError("Chitubox ZIP layer count mismatch");
        result.m_layers.clear();
        for(size_t i=1;i<=size_t(count);++i) result.m_layers.push_back(indices.at(i));
        result.m_archive_format="chitubox_zip";
    } else if (vda_manifest != result.m_entries.end() && !(config && profile && !result.m_layers.empty()) && uvj_config == result.m_entries.end()) {
        const auto manifest = xml(*vda_manifest);
        int64_t columns = 0, rows = 0; char separator = 0;
        std::istringstream resolution(manifest.get<std::string>("root.Machines.Resolution"));
        if (!(resolution >> columns >> separator >> rows) || separator != '*') throw RuntimeError("Invalid VDA resolution");
        if (resolution.peek() == 'P') resolution.get();
        resolution >> std::ws;
        if (!resolution.eof() || columns <= 0 || rows <= 0 || columns > 32768 || rows > 32768 || columns * rows > 128 * 1024 * 1024)
            throw RuntimeError("VDA resolution exceeds the editor memory budget");
        const auto count = manifest.get<int64_t>("root.Slices.layersCount");
        if (count <= 0 || count > 100000) throw RuntimeError("VDA mask count exceeds the editor limit");
        const double thickness = manifest.get<double>("root.Slices.thickness");
        if (!std::isfinite(thickness) || thickness <= 0.) throw RuntimeError("Invalid VDA layer height");
        result.m_layers.clear();
        std::set<size_t> referenced;
        double previous_z = 0., first_z = 0.;
        for (const auto &layer : manifest.get_child("root.Layers")) {
            if (layer.first == "<xmlattr>") continue;
            if (layer.first != "VDALayer" || layer.second.get<int64_t>("Index") != int64_t(result.m_layers.size()))
                throw RuntimeError("VDA layer indices must be contiguous from zero");
            const double z = layer.second.get<double>("zvalue");
            if (!std::isfinite(z) || z <= previous_z) throw RuntimeError("VDA layer Z must increase");
            previous_z = z;
            if (result.m_layers.empty()) first_z = z;
            const auto name = layer.second.get<std::string>("filename");
            const auto found = std::find_if(result.m_entries.begin(), result.m_entries.end(), [&](const Entry &entry) {
                return boost::algorithm::iequals(entry.name, name);
            });
            if (found == result.m_entries.end()) throw RuntimeError("VDA manifest references a missing mask");
            const auto index = size_t(found-result.m_entries.begin());
            if (!referenced.insert(index).second) throw RuntimeError("Duplicate VDA mask reference");
            const auto &bytes = found->bytes;
            if (bytes.size() < 29 || !png::is_png({bytes.data(), bytes.size()})) throw RuntimeError("VDA mask is not PNG");
            auto dimension = [&](size_t offset) { return (uint32_t(bytes[offset]) << 24) | (uint32_t(bytes[offset+1]) << 16) |
                (uint32_t(bytes[offset+2]) << 8) | bytes[offset+3]; };
            if (dimension(16) != columns || dimension(20) != rows || bytes[24] != 8 || bytes[25] != 0)
                throw RuntimeError("VDA editor requires matching 8-bit grayscale PNG masks");
            result.m_layers.push_back(index);
            if (result.m_layers.size() > size_t(count)) throw RuntimeError("VDA layer count mismatch");
        }
        const double start_height = manifest.get<double>("root.Slices.startHeight"), end_height = manifest.get<double>("root.Slices.endHeight");
        if (result.m_layers.size() != size_t(count) || !std::isfinite(start_height) || !std::isfinite(end_height) ||
            std::abs(start_height-first_z) > 1e-4 || std::abs(end_height-previous_z) > 1e-4)
            throw RuntimeError("VDA layer count or print height mismatch");
        result.m_archive_format = "vda";
    } else if (config && profile && !result.m_layers.empty()) {
        // SL1 also carries config.json; its INI/profile and top-level masks
        // identify the schema before considering vendor-neutral UVJ metadata.
        result.m_archive_format = "SL1";
    } else if (uvj_config != result.m_entries.end()) {
        if (uvj_config->bytes.size() > 32 * 1024 * 1024) throw RuntimeError("UVJ metadata exceeds 32 MiB");
        const auto metadata = nlohmann::json::parse(uvj_config->bytes);
        const auto &size = metadata.at("Properties").at("Size");
        auto positive_integer = [](const auto &value, int64_t maximum) {
            if (!value.is_number_integer()) throw RuntimeError("UVJ dimensions and counts must be integers");
            const auto number = value.template get<int64_t>();
            if (number <= 0 || number > maximum) throw RuntimeError("UVJ dimensions or count outside supported range");
            return size_t(number);
        };
        const size_t columns = positive_integer(size.at("X"), 32768);
        const size_t rows = positive_integer(size.at("Y"), 32768);
        const size_t count = positive_integer(size.at("Layers"), 1000000);
        if (columns * rows > 128 * 1024 * 1024) throw RuntimeError("UVJ raster exceeds the editor memory budget");
        for (const char *axis : {"X", "Y"}) {
            const double mm = size.at("Millimeter").at(axis).template get<double>();
            if (!std::isfinite(mm) || mm <= 0) throw RuntimeError("UVJ display dimensions must be positive");
        }
        result.m_layers.clear();
        for (size_t i = 0; i < result.m_entries.size(); ++i) {
            const auto name = boost::algorithm::to_lower_copy(result.m_entries[i].name);
            if (name.size() == 18 && name.substr(0, 6) == "slice/" && name.substr(14) == ".png" &&
                std::all_of(name.begin()+6, name.begin()+14, [](char c) { return c >= '0' && c <= '9'; }))
                result.m_layers.push_back(i);
        }
        if (result.m_layers.size() != count) throw RuntimeError("UVJ layer count does not match its PNG entries");
        std::sort(result.m_layers.begin(), result.m_layers.end(), [&](size_t a, size_t b) {
            return boost::algorithm::to_lower_copy(result.m_entries[a].name) < boost::algorithm::to_lower_copy(result.m_entries[b].name);
        });
        for (size_t i = 0; i < count; ++i) {
            const auto &entry = result.m_entries[result.m_layers[i]];
            std::ostringstream expected; expected << "slice/" << std::setw(8) << std::setfill('0') << i << ".png";
            if (!boost::algorithm::iequals(entry.name, expected.str())) throw RuntimeError("UVJ layer indices must be contiguous from zero");
            const auto &bytes = entry.bytes;
            if (bytes.size() < 29 || !png::is_png({bytes.data(), bytes.size()})) throw RuntimeError("UVJ layer is not PNG");
            auto dimension = [&](size_t offset) { return (uint32_t(bytes[offset]) << 24) | (uint32_t(bytes[offset+1]) << 16) |
                (uint32_t(bytes[offset+2]) << 8) | uint32_t(bytes[offset+3]); };
            if (dimension(16) != columns || dimension(20) != rows || bytes[24] != 8 || bytes[25] != 0)
                throw RuntimeError("UVJ masks must match metadata and use 8-bit grayscale PNG");
        }
        if (metadata.contains("Layers") && !metadata["Layers"].is_null()) {
            if (!metadata["Layers"].is_array() || metadata["Layers"].size() != count) throw RuntimeError("UVJ parameter table count mismatch");
            double previous_z = 0;
            for (const auto &layer : metadata["Layers"]) {
                const double z = layer.at("Z").template get<double>();
                if (!std::isfinite(z) || z <= previous_z) throw RuntimeError("UVJ layer Z must increase");
                previous_z = z;
            }
        }
        result.m_archive_format = "uvj";
    } else throw RuntimeError("The layer editor supports PNG, SL1, UVJ, grayscale CWS, Generic ZIP and Voxeldance Additive ZIP");
    // CWS order is defined by its manifest, not filenames or ZIP entry order.
    if (result.m_archive_format != "cws" && result.m_archive_format != "genericzip" && result.m_archive_format != "vda" && result.m_archive_format != "chitubox_zip") std::sort(result.m_layers.begin(), result.m_layers.end(), [&](size_t a, size_t b) {
        return boost::algorithm::to_lower_copy(result.m_entries[a].name) <
               boost::algorithm::to_lower_copy(result.m_entries[b].name);
    });
    // Decode only on demand, rather than allocating every full-resolution mask.
    result.image(0);
    return result;
}

png::ReadBuf LayerDocument::encoded_layer(size_t layer) const
{
    const auto &bytes = m_entries.at(m_layers.at(layer)).bytes;
    return {bytes.data(), bytes.size()};
}

png::ImageGreyscale LayerDocument::image(size_t layer) const
{
    const auto &bytes = m_entries.at(m_layers.at(layer)).bytes;
    // Bound decoded allocation before invoking libpng (PNG IHDR width/height).
    if (bytes.size() < 24 || !png::is_png({bytes.data(), bytes.size()}))
        throw RuntimeError("Invalid layer PNG");
    auto u32 = [&](size_t p) { return (uint32_t(bytes[p]) << 24) | (uint32_t(bytes[p+1]) << 16) |
                                    (uint32_t(bytes[p+2]) << 8) | uint32_t(bytes[p+3]); };
    const uint64_t cols = u32(16), rows = u32(20);
    if (!cols || !rows || cols > 32768 || rows > 32768 || cols * rows > 128 * 1024 * 1024)
        throw RuntimeError("Layer dimensions exceed the editor memory budget");
    png::ImageGreyscale result;
    if (!png::decode_png(png::ReadBuf{bytes.data(), bytes.size()}, result))
        throw RuntimeError("Cannot decode grayscale layer PNG");
    return result;
}

void LayerDocument::replace(size_t layer, const png::ImageGreyscale &image)
{
    const size_t index = m_layers.at(layer);
    std::vector<uint8_t> quantized;
    const uint8_t *pixels=image.buf.data();
    if(m_archive_format=="cxdlpv4") {
        quantized=image.buf;
        for(auto &value:quantized)value=value<2?0:uint8_t((value&0xfe)|1);
        pixels=quantized.data();
    }
    const auto encoded = PNGRasterEncoder{}(pixels, image.cols, image.rows, 1);
    const auto *data = static_cast<const uint8_t *>(encoded.data());
    std::vector<uint8_t> after(data, data + encoded.size());
    if (after == m_entries[index].bytes) return;
    commit({{index, m_entries[index].bytes, std::move(after)}});
}

void LayerDocument::commit(std::vector<Change> changes)
{
    if (changes.empty()) return;
    constexpr uint64_t history_limit = 128 * 1024 * 1024;
    uint64_t document_bytes = 0, change_bytes = 0;
    for (const auto &entry : m_entries) document_bytes += entry.bytes.size();
    for (const auto &change : changes) {
        document_bytes = document_bytes - change.before.size() + change.after.size();
        change_bytes += change.before.size() + change.after.size();
    }
    if (document_bytes > (uint64_t(1) << 30))
        throw RuntimeError("Edited document exceeds the 1 GiB memory budget");
    if (change_bytes > history_limit)
        throw RuntimeError("This edit exceeds the 128 MiB history budget. Select fewer layers.");
    const uint64_t next_revision = m_next_revision + 1;
    // Allocate replacement buffers before touching either masks or history.
    std::vector<std::vector<uint8_t>> replacements;
    replacements.reserve(changes.size());
    for (const auto &change : changes) replacements.push_back(change.after);
    m_undo.push_back({std::move(changes), m_revision, next_revision});
    // Bound history by both count and compressed size.
    size_t bytes = 0;
    auto edit_bytes = [](const Edit &edit) {
        size_t result = 0;
        for (const auto &change : edit.changes) result += change.before.size() + change.after.size();
        return result;
    };
    for (const auto &edit : m_undo) bytes += edit_bytes(edit);
    while (m_undo.size() > 1 && (m_undo.size() > 32 || bytes > history_limit)) {
        bytes -= edit_bytes(m_undo.front());
        m_undo.erase(m_undo.begin());
    }
    for (size_t i = 0; i < replacements.size(); ++i)
        m_entries[m_undo.back().changes[i].entry].bytes = std::move(replacements[i]);
    m_redo.clear();
    m_revision = m_next_revision = next_revision;
}

bool LayerDocument::transform_layers(size_t first, size_t last, MaskOperation operation, uint8_t value,
                                    const std::function<bool(size_t, size_t)> &progress, unsigned radius)
{
    if (radius > 4096) throw RuntimeError("Mask filter radius must not exceed 4096 pixels");
    if (first > last || last >= layer_count()) throw RuntimeError("Invalid layer range");
    std::vector<Change> changes;
    const size_t count = last - first + 1;
    uint64_t history_bytes = 0;
    for (size_t layer = first; layer <= last; ++layer) {
        if (progress && !progress(layer-first, count)) return false;
        auto mask = image(layer);
        auto cancelled = [&] { return progress && !progress(layer-first, count); };
        try {
            switch (operation) {
            case MaskOperation::Threshold:
                for (auto &pixel : mask.buf) pixel = pixel >= value ? 255 : 0;
                break;
            case MaskOperation::Invert:
                for (auto &pixel : mask.buf) pixel = uint8_t(255-pixel);
                break;
            case MaskOperation::MirrorHorizontal:
                for (size_t row = 0; row < mask.rows; ++row)
                    std::reverse(mask.buf.begin()+row*mask.cols, mask.buf.begin()+(row+1)*mask.cols);
                break;
            case MaskOperation::MirrorVertical:
                for (size_t row = 0; row < mask.rows/2; ++row)
                    for (size_t col = 0; col < mask.cols; ++col)
                        std::swap(mask.buf[row*mask.cols+col], mask.buf[(mask.rows-1-row)*mask.cols+col]);
                break;
            case MaskOperation::Rotate180: std::reverse(mask.buf.begin(), mask.buf.end()); break;
            case MaskOperation::Fill: std::fill(mask.buf.begin(), mask.buf.end(), value); break;
            case MaskOperation::BoxBlur:
                mask.buf = box_blur(mask.buf,mask.cols,mask.rows,radius,[&] {
                    if (cancelled()) throw CancelledMaskEdit{};
                });
                break;
            case MaskOperation::MedianBlur:
                mask.buf = median_blur(mask.buf,mask.cols,mask.rows,radius,[&] {
                    if (cancelled()) throw CancelledMaskEdit{};
                });
                break;
            case MaskOperation::Erode:
            case MaskOperation::Dilate:
                mask.buf = morphology(mask.buf, mask.cols, mask.rows, radius, operation == MaskOperation::Dilate, cancelled);
                break;
            case MaskOperation::Open:
            case MaskOperation::Close: {
                const bool close = operation == MaskOperation::Close;
                auto first_pass = morphology(mask.buf, mask.cols, mask.rows, radius, close, cancelled);
                mask.buf = morphology(first_pass, mask.cols, mask.rows, radius, !close, cancelled);
                break;
            }
            case MaskOperation::Gradient:
            case MaskOperation::WhiteTopHat:
            case MaskOperation::BlackTopHat: {
                auto lower = morphology(mask.buf, mask.cols, mask.rows, radius, false, cancelled);
                auto upper = morphology(mask.buf, mask.cols, mask.rows, radius, true, cancelled);
                if (operation == MaskOperation::WhiteTopHat)
                    lower = morphology(lower, mask.cols, mask.rows, radius, true, cancelled);
                if (operation == MaskOperation::BlackTopHat)
                    upper = morphology(upper, mask.cols, mask.rows, radius, false, cancelled);
                for (size_t pixel = 0; pixel < mask.buf.size(); ++pixel) {
                    const int a = operation == MaskOperation::WhiteTopHat ? mask.buf[pixel] : upper[pixel];
                    const int b = operation == MaskOperation::BlackTopHat ? mask.buf[pixel] : lower[pixel];
                    mask.buf[pixel] = uint8_t(std::max(0, a-b));
                }
                break;
            }
            default: throw RuntimeError("Unknown layer mask operation");
            }
        } catch (const CancelledMaskEdit &) { return false; }
        if (cancelled()) return false;
        if(m_archive_format=="cxdlpv4")
            for(auto &value:mask.buf)value=value<2?0:uint8_t((value&0xfe)|1);
        const auto encoded = PNGRasterEncoder{}(mask.buf.data(), mask.cols, mask.rows, 1);
        const auto *data = static_cast<const uint8_t *>(encoded.data());
        std::vector<uint8_t> after(data, data + encoded.size());
        const auto index = m_layers[layer];
        if (after != m_entries[index].bytes) {
            history_bytes += after.size() + m_entries[index].bytes.size();
            if (history_bytes > 128 * 1024 * 1024)
                throw RuntimeError("This edit exceeds the 128 MiB history budget. Select fewer layers.");
            changes.push_back({index, m_entries[index].bytes, std::move(after)});
        }
    }
    if (progress && !progress(count, count)) return false;
    commit(std::move(changes));
    return true;
}

void LayerDocument::paint(size_t layer, int x, int y, unsigned radius, uint8_t value)
{
    auto img = image(layer);
    draw_line(img, x, y, x, y, radius, value);
    replace(layer, img);
}

void LayerDocument::replace_image(size_t layer, const png::ImageGreyscale &replacement)
{
    const auto previous = image(layer);
    if (replacement.cols != previous.cols || replacement.rows != previous.rows ||
        replacement.buf.size() != replacement.cols * replacement.rows)
        throw RuntimeError("Edited layer dimensions must match the original raster");
    replace(layer, replacement);
}

void LayerDocument::draw_line(png::ImageGreyscale &img, int x0, int y0, int x1, int y1,
                             unsigned radius, uint8_t value)
{
    if (!img.cols || !img.rows || img.cols > 32768 || img.rows > 32768 ||
        img.buf.size() != img.cols * img.rows || radius > 4096 ||
        x0 < 0 || y0 < 0 || x1 < 0 || y1 < 0 ||
        size_t(x0) >= img.cols || size_t(x1) >= img.cols ||
        size_t(y0) >= img.rows || size_t(y1) >= img.rows)
        throw RuntimeError("Brush position or radius is outside the supported range");
    const int r = int(radius);
    const int dx = std::abs(x1-x0), sx = x0 < x1 ? 1 : -1;
    const int dy = -std::abs(y1-y0), sy = y0 < y1 ? 1 : -1;
    int error = dx + dy;
    for (;;) {
        for (int py = std::max(0, y0-r); py <= std::min(int(img.rows)-1, y0+r); ++py)
            for (int px = std::max(0, x0-r); px <= std::min(int(img.cols)-1, x0+r); ++px)
                if (int64_t(px-x0)*(px-x0) + int64_t(py-y0)*(py-y0) <= int64_t(r)*r)
                    img.buf[size_t(py)*img.cols + size_t(px)] = value;
        if (x0 == x1 && y0 == y1) break;
        const int e2 = 2 * error;
        if (e2 >= dy) { error += dy; x0 += sx; }
        if (e2 <= dx) { error += dx; y0 += sy; }
    }
}
void LayerDocument::threshold(size_t layer, uint8_t cutoff)
{
    auto img = image(layer);
    bool changed = false;
    for (auto &p : img.buf) {
        const uint8_t value = p >= cutoff ? 255 : 0;
        changed |= value != p;
        p = value;
    }
    if (changed) replace(layer, img);
}
void LayerDocument::invert(size_t layer)
{
    auto img = image(layer);
    for (auto &pixel : img.buf) pixel = uint8_t(255 - pixel);
    replace(layer, img);
}
bool LayerDocument::undo()
{
    if (m_undo.empty()) return false;
    // Allocate every replacement and the destination history slot before
    // touching any layer: allocation failure must preserve the whole edit.
    std::vector<std::vector<uint8_t>> replacements;
    for (const auto &change : m_undo.back().changes) replacements.push_back(change.before);
    m_redo.reserve(m_redo.size() + 1);
    auto edit = std::move(m_undo.back()); m_undo.pop_back();
    for (size_t i = 0; i < edit.changes.size(); ++i)
        m_entries[edit.changes[i].entry].bytes = std::move(replacements[i]);
    m_revision = edit.before_revision;
    m_redo.push_back(std::move(edit)); return true;
}
bool LayerDocument::redo()
{
    if (m_redo.empty()) return false;
    std::vector<std::vector<uint8_t>> replacements;
    for (const auto &change : m_redo.back().changes) replacements.push_back(change.after);
    m_undo.reserve(m_undo.size() + 1);
    auto edit = std::move(m_redo.back()); m_redo.pop_back();
    for (size_t i = 0; i < edit.changes.size(); ++i)
        m_entries[edit.changes[i].entry].bytes = std::move(replacements[i]);
    m_revision = edit.after_revision;
    m_undo.push_back(std::move(edit)); return true;
}
void LayerDocument::save_sl1(const std::string &path)
{
    if (m_archive_format != "SL1") throw RuntimeError("This document has no original SL1 metadata");
    save_archive(path);
}

void LayerDocument::save_archive(const std::string &path) {
  if (m_layers.empty())
    throw RuntimeError("Cannot save an empty layer document");
  if (m_archive_format.empty())
    throw RuntimeError("Export a new raster document with its printer profile "
                       "instead of preserving an archive");
  const auto extension = boost::filesystem::path(path).extension().string();
  if (m_archive_format == "png" ? !boost::algorithm::iequals(extension, ".png") :
      m_archive_format == "cxdlpv1" ? !boost::algorithm::iends_with(path,".v1.cxdlp") :
      m_archive_format == "cxdlpv2" ? !boost::algorithm::iends_with(path,".v2.cxdlp") :
      m_archive_format == "cxdlp" ? !boost::algorithm::iequals(extension,".cxdlp") :
      m_archive_format == "cxdlpv4" ? !boost::algorithm::iequals(extension,".cxdlpv4") :
      (m_archive_format == "genericzip" || m_archive_format == "vda" || m_archive_format == "chitubox_zip") ? !boost::algorithm::iequals(extension, ".zip") :
      m_archive_format == "cwsxml" ? !boost::algorithm::iends_with(path,".xml.cws") :
      m_archive_format == "cwsrgb" ? !boost::algorithm::iends_with(path,".rgb.cws") :
      (m_archive_format == "cws" || m_archive_format == "cwsnovamaker") ? !boost::algorithm::iequals(extension, ".cws") : m_archive_format == "uvj"
          ? !boost::algorithm::iequals(extension, ".uvj")
          : !(boost::algorithm::iequals(extension, ".sl1") ||
              boost::algorithm::iequals(extension, ".sl1s")))
    throw RuntimeError(
        "The output extension must match the original layer container");
  const boost::filesystem::path output(path);
  const auto temporary = output.parent_path() / boost::filesystem::unique_path(
                                                    ".sla-edit-%%%%-%%%%.tmp");
  // Legacy archives may omit physical geometry. Recalculate material only
  // when their embedded slicer profile supplies all required dimensions;
  // never infer millimetres from PNG dimensions or the active printer.
  std::optional<double> used_material;
  std::optional<std::string> updated_chitubox;
  if(m_archive_format=="chitubox_zip" && m_revision!=0) {
    const auto entry=std::find_if(m_entries.begin(),m_entries.end(),[](const Entry &entry){return boost::algorithm::iequals(entry.name,"run.gcode");});
    const std::string original(entry->bytes.begin(),entry->bytes.end());
    std::istringstream input(original);
    std::map<std::string,double> fields;
    std::vector<double> positions;
    const std::set<std::string> names{"machineX","machineY","volume","weight","price","PositionZ"};
    for(std::string line;std::getline(input,line);) {
      boost::algorithm::trim(line);
      if(line.empty() || line.front()!=';') continue;
      const auto colon=line.find(':');if(colon==std::string::npos) continue;
      const auto key=boost::algorithm::trim_copy(line.substr(1,colon-1));
      if(!names.count(key)) continue;
      auto text=boost::algorithm::trim_copy(line.substr(colon+1));
      if(key=="PositionZ" && boost::algorithm::ends_with(text,"mm")) text.resize(text.size()-2);
      std::istringstream number(text);number.imbue(std::locale::classic());
      double value;
      if(!(number>>value) || !std::isfinite(value) || value<0.) throw RuntimeError("Invalid Chitubox ZIP material metadata");
      number>>std::ws;if(!number.eof()) throw RuntimeError("Invalid Chitubox ZIP material metadata");
      if(key=="PositionZ") positions.push_back(value);
      else if(!fields.emplace(key,value).second) throw RuntimeError("Duplicate Chitubox ZIP material field");
    }
    // Older external sequences can omit Z comments or material properties.
    // Preserve those fields instead of inventing height, density or resin cost.
    if(positions.size()==m_layers.size() && fields.count("volume")) {
      const double volume=mask_volume_ml(*this,fields.at("machineX"),fields.at("machineY"),positions);
      if(std::abs(volume-fields.at("volume"))>1e-9*std::max(1.,volume)) {
        std::map<std::string,double> replacements{{"volume",volume}};
        if(fields.at("volume")>0.) for(const char *key:{"weight","price"})
          if(fields.count(key)) replacements[key]=fields.at(key)*volume/fields.at("volume");
        std::istringstream lines(original);std::ostringstream updated;
        updated.imbue(std::locale::classic());updated<<std::setprecision(12);
        for(std::string line;std::getline(lines,line);) {
          const auto colon=line.find(':');const auto trimmed=boost::algorithm::trim_copy(line);
          if(colon!=std::string::npos && !trimmed.empty() && trimmed.front()==';') {
            const auto key=boost::algorithm::trim_copy(trimmed.substr(1,trimmed.find(':')-1));
            if(auto value=replacements.find(key);value!=replacements.end()) {
              updated<<line.substr(0,colon+1)<<value->second;
              if(!line.empty() && line.back()=='\r') updated<<'\r';
              updated<<'\n';continue;
            }
          }
          updated<<line<<'\n';
        }
        updated_chitubox=updated.str();
      }
    }
  }
  if (m_archive_format == "SL1" && m_revision != 0) {
    const auto profile = std::find_if(
        m_entries.begin(), m_entries.end(), [](const Entry &entry) {
          return boost::algorithm::iequals(entry.name, "prusaslicer.ini");
        });
    if (profile != m_entries.end()) {
      auto number = [&](const char *key) -> std::optional<double> {
        std::istringstream input(
            std::string(profile->bytes.begin(), profile->bytes.end()));
        for (std::string line; std::getline(input, line);) {
          const auto separator = line.find('=');
          if (separator == std::string::npos ||
              boost::algorithm::trim_copy(line.substr(0, separator)) != key)
            continue;
          std::istringstream value(line.substr(separator + 1));
          value.imbue(std::locale::classic());
          double parsed = 0;
          if (!(value >> parsed) || !std::isfinite(parsed) || parsed <= 0)
            throw RuntimeError(std::string("Invalid archived SLA geometry: ") +
                               key);
          value >> std::ws;
          if (!value.eof())
            throw RuntimeError(std::string("Invalid archived SLA geometry: ") +
                               key);
          return parsed;
        }
        return {};
      };
      const auto width = number("display_width"),
                 height = number("display_height");
      const auto first_height = number("initial_layer_height"),
                 layer_height = number("layer_height");
      if (width && height && first_height && layer_height) {
        double volume = 0.;
        // Repeated calibration PNGs share their nonzero-pixel count.
        // Views borrow immutable compressed buffers; the bounded index
        // does not duplicate masks or retain decoded full-screen images.
        std::unordered_map<std::string_view, uint64_t> counts;
        const auto first = image(0);
        const double pixel_area =
            *width * *height / (double(first.cols) * first.rows);
        for (size_t layer = 0; layer < m_layers.size(); ++layer) {
          const auto bytes = encoded_layer(layer);
          const std::string_view key(static_cast<const char *>(bytes.buf),
                                     bytes.sz);
          auto cached = counts.find(key);
          uint64_t count = 0;
          if (cached != counts.end())
            count = cached->second;
          else {
            const auto mask = image(layer);
            if (mask.cols != first.cols || mask.rows != first.rows)
              throw RuntimeError(
                  "Archived SLA mask dimensions differ between layers");
            count = std::count_if(mask.buf.begin(), mask.buf.end(),
                                  [](uint8_t value) { return value != 0; });
            if (counts.size() < 1024)
              counts.emplace(key, count);
          }
          volume +=
              count * pixel_area * (layer == 0 ? *first_height : *layer_height);
        }
        if (!std::isfinite(volume))
          throw RuntimeError("Archived SLA material volume is invalid");
        used_material = volume / 1000.;
      }
    }
  }
  try {
    if(m_archive_format=="cxdlpv4" && m_revision==0) {
      const auto &original=m_entries.at(0).bytes;
      boost::nowide::ofstream stream(temporary.string(),std::ios::binary|std::ios::trunc);
      stream.write(reinterpret_cast<const char *>(original.data()),original.size());stream.close();
      if(!stream)throw FileIOError("Cannot copy the original CXDLPv4 archive");
    } else if(m_archive_format=="cxdlpv4") {
      const auto &original=m_entries.at(0).bytes;const CrealityCXDLPv4Layout layout(original);
      if(layout.layers.size()!=m_layers.size())throw RuntimeError("CXDLPv4 document layer count changed");
      struct Mask {EncodedRaster raster;uint32_t area,nonzero;};
      std::vector<Mask> masks(m_layers.size());std::unordered_map<std::string_view,size_t> cache;
      std::map<uint32_t,std::vector<size_t>> groups;
      uint64_t encoded_size=0;double volume=0,previous_z=0;bool complete_z=true;
      const float pitch_x=float(std::round(float(layout.width)/layout.columns*10000.f)/10000.f),pitch_y=float(std::round(float(layout.height)/layout.rows*10000.f)/10000.f);
      const double pixel_area=layout.width/layout.columns*layout.height/layout.rows;
      for(size_t i=0;i<m_layers.size();++i) {
        const auto png=encoded_layer(i);const std::string_view key(static_cast<const char *>(png.buf),png.sz);
        if(const auto cached=cache.find(key);cached!=cache.end())masks[i]=masks[cached->second];
        else {
          const auto mask=image(i);
          if(mask.cols!=layout.columns || mask.rows!=layout.rows)throw RuntimeError("Edited CXDLPv4 mask dimensions changed");
          masks[i].raster=CrealityCXDLPv4Archive::encode_mask(mask.buf.data(),mask.cols,mask.rows,1);
          masks[i].nonzero=uint32_t(std::count_if(mask.buf.begin(),mask.buf.end(),[](uint8_t p){return p>=2;}));
          const double area=creality_largest_contour_area(mask.buf.data(),mask.cols,mask.rows)*float(pitch_x*pitch_y)*1000.;
          if(!std::isfinite(area) || area>UINT32_MAX)throw RuntimeError("Edited CXDLPv4 contour area exceeds 32 bits");
          masks[i].area=uint32_t(area);
          if(cache.size()<1024)cache.emplace(key,i);
        }
        if(masks[i].raster.size()>(uint64_t(1)<<30)-encoded_size)throw RuntimeError("Encoded CXDLPv4 layers exceed the memory budget");
        encoded_size+=masks[i].raster.size();groups[layout.layers[i].data].push_back(i);
        const double z=layout.layers[i].z;if(z<=previous_z)complete_z=false;
        volume+=masks[i].nonzero*pixel_area*(z-previous_z);previous_z=z;
      }
      // Replace each addressed payload range once. Shared original payloads
      // may diverge after editing, so emit a separate block for each reference.
      // All documented addresses are relocated; unknown bytes and gaps survive.
      std::vector<uint8_t> output;std::vector<uint32_t> addresses(m_layers.size());
      struct Relocation {size_t end;int64_t delta;};std::vector<Relocation> moves;
      auto append=[&](const uint8_t *data,size_t length) {
        if(length>(uint64_t(1)<<30)-4-output.size())throw RuntimeError("Edited CXDLPv4 file exceeds the memory budget");
        output.insert(output.end(),data,data+length);
      };
      size_t source_position=0;
      for(const auto &[start,references]:groups) {
        append(original.data()+source_position,start-source_position);
        const auto original_size=layout.layers[references.front()].length;
        for(const auto i:references) {
          addresses[i]=uint32_t(output.size());const auto &layer=layout.layers[i];
          append(original.data()+layer.data,44);
          const auto &raster=masks[i].raster;const auto *begin=static_cast<const uint8_t *>(raster.data());
          if(layout.seed) {std::vector<uint8_t> encrypted(begin,begin+raster.size());CrealityCXDLPv4Archive::crypt_mask(encrypted,layout.seed,uint32_t(i));append(encrypted.data(),encrypted.size());}
          else append(begin,raster.size());
        }
        source_position=size_t(start)+original_size;
        moves.push_back({source_position,int64_t(output.size())-int64_t(source_position)});
      }
      append(original.data()+source_position,original.size()-4-source_position);
      auto relocate=[&](size_t old){int64_t delta=0;for(const auto &move:moves){if(old<move.end)break;delta=move.delta;}return size_t(int64_t(old)+delta);};
      auto put32=[&](size_t offset,uint32_t value){if(offset+4>output.size())throw RuntimeError("Invalid edited CXDLPv4 address");for(unsigned j=0;j<4;++j)output[offset+j]=uint8_t(value>>(8*j));};
      auto put_float=[&](size_t offset,double value){const float number=float(value);if(!std::isfinite(number))throw RuntimeError("Edited CXDLPv4 metadata overflow");uint32_t bits;std::memcpy(&bits,&number,4);put32(offset,bits);};
      for(const size_t field:{layout.base+28,layout.base+32,layout.base+40,layout.base+52,layout.base+72}) {
        const auto old=layout.integer(field);if(old)put32(field,uint32_t(relocate(old)));
      }
      for(const size_t field:{layout.base+28,layout.base+40}) {
        const auto preview=layout.integer(field);if(preview)put32(relocate(preview)+8,uint32_t(relocate(layout.integer(preview+8))));
      }
      for(size_t i=0;i<m_layers.size();++i) {
        const size_t row=relocate(layout.table)+40*i;
        put32(row+12,addresses[i]);put32(row+16,uint32_t(44+masks[i].raster.size()));put32(row+28,masks[i].area);
      }
      if(const auto parameters=layout.integer(layout.base+52);parameters && complete_z) {
        const double old_volume=layout.number(parameters+20);const double ml=volume/1000.;
        if(!std::isfinite(old_volume) || old_volume<0)throw RuntimeError("Invalid original CXDLPv4 material volume");
        const size_t target=relocate(parameters);put_float(target+20,ml);
        if(old_volume>0)for(const size_t offset:{24,28}) {
          const double old_value=layout.number(parameters+offset);
          if(!std::isfinite(old_value) || old_value<0)throw RuntimeError("Invalid original CXDLPv4 material statistics");
          put_float(target+offset,old_value*ml/old_volume);
        }
      }
      const uint32_t checksum=creality_checksum(output.data(),output.size(),4);
      for(unsigned i=0;i<4;++i)output.push_back(uint8_t(checksum>>(24-8*i)));
      // Validate the relocated metadata before the atomic replacement.
      const CrealityCXDLPv4Layout verified(output);
      boost::nowide::ofstream stream(temporary.string(),std::ios::binary|std::ios::trunc);
      stream.write(reinterpret_cast<const char *>(output.data()),output.size());stream.close();
      if(!stream)throw FileIOError("Cannot write edited CXDLPv4 layers");
    } else     if(m_archive_format=="cxdlpv1" || m_archive_format=="cxdlpv2" || m_archive_format=="cxdlp") {
      const unsigned version=m_archive_format=="cxdlpv1"?1:m_archive_format=="cxdlpv2"?2:3;
      // Recover physical dimensions from the preserved slicer strings. No
      // active printer profile is needed to recalculate contour areas.
      double width_mm=0,height_mm=0;
      if(version>=2) {
        const auto &prefix=m_entries.at(0).bytes;
        const uint32_t model_length=(uint32_t(prefix.at(15))<<24)|(uint32_t(prefix.at(16))<<16)|(uint32_t(prefix.at(17))<<8)|prefix.at(18);
        size_t position=19+model_length+6+64+2*(116*116+290*290+290*290)+6;
        for(double *value:{&width_mm,&height_mm}) {
          const uint32_t length=(uint32_t(prefix.at(position))<<24)|(uint32_t(prefix.at(position+1))<<16)|(uint32_t(prefix.at(position+2))<<8)|prefix.at(position+3);position+=4;
          std::string text;for(size_t j=0;j<length;j+=2) {text.push_back(char(prefix.at(position+j+1)));}position+=length;
          std::istringstream number(text);number.imbue(std::locale::classic());number>>*value;
        }
      }
      std::vector<EncodedRaster> layers(m_layers.size());
      std::unordered_map<std::string_view,size_t> cache;
      uint64_t total=0;
      for(size_t i=0;i<m_layers.size();++i) {
        const auto bytes=encoded_layer(i);const std::string_view key(static_cast<const char *>(bytes.buf),bytes.sz);
        if(auto previous=cache.find(key);previous!=cache.end()) {
          const auto &source=layers[previous->second];
          if(source.size()>(uint64_t(1)<<30)-total)throw RuntimeError("CXDLPv1 encoding exceeds the memory budget");
          layers[i]=source;
        } else {
          const auto mask=image(i);
          layers[i]=CrealityCXDLPv1Archive::encode_mask(mask.buf.data(),mask.cols,mask.rows,1);
          if(version>=2) {
            const float pitch_x=float(std::round(float(width_mm)/mask.cols*10000.f)/10000.f),pitch_y=float(std::round(float(height_mm)/mask.rows*10000.f)/10000.f);
            const double area=creality_largest_contour_area(mask.buf.data(),mask.cols,mask.rows)*float(pitch_x*pitch_y)*1000.;
            if(!std::isfinite(area) || area>UINT32_MAX)throw RuntimeError("Edited CXDLP contour area exceeds 32 bits");
            const auto *source=static_cast<const uint8_t *>(layers[i].data());std::vector<uint8_t> raw(source,source+layers[i].size());
            for(unsigned j=0;j<4;++j)raw[j]=uint8_t(uint32_t(area)>>(24-8*j));
            layers[i]=EncodedRaster(std::move(raw),"cxdlp");
          }
          if(layers[i].size()>(uint64_t(1)<<30)-total)throw RuntimeError("CXDLPv1 encoding exceeds the memory budget");
          if(cache.size()<1024)cache.emplace(key,i);
        }
        total+=layers[i].size();
      }
      boost::nowide::ofstream stream(temporary.string(),std::ios::binary|std::ios::trunc);
      const auto &prefix=m_entries.at(0).bytes,&footer=m_entries.at(1).bytes;
      stream.write(reinterpret_cast<const char *>(prefix.data()),prefix.size());
      for(const auto &layer:layers)stream.write(static_cast<const char *>(layer.data()),4);
      stream.write("\r\n",2);
      if(version>=3) {const auto &informative=m_entries.at(2).bytes;stream.write(reinterpret_cast<const char *>(informative.data()),informative.size());}
      for(const auto &layer:layers){stream.write(static_cast<const char *>(layer.data()),layer.size());stream.write("\r\n",2);}
      stream.write(reinterpret_cast<const char *>(footer.data()),version==1?footer.size():footer.size()-4);
      stream.close();if(!stream)throw FileIOError("Cannot write edited CXDLP layers");
      if(version>=2) {
        boost::nowide::ifstream checksum_input(temporary.string(),std::ios::binary);
        const auto checksum=creality_checksum(checksum_input,version);
        stream.open(temporary.string(),std::ios::binary|std::ios::app);
        for(unsigned j=0;j<4;++j)stream.put(char(checksum>>(24-8*j)));
        stream.close();if(!stream)throw FileIOError("Cannot write edited CXDLP checksum");
      }
    } else if (m_archive_format == "png") {
      const auto bytes = encoded_layer(0);
      boost::nowide::ofstream stream(temporary.string(), std::ios::binary | std::ios::trunc);
      stream.write(static_cast<const char *>(bytes.buf), bytes.sz);
      stream.close();
      if (!stream) throw FileIOError("Cannot write edited PNG mask");
    } else {
      Zipper zip(temporary.string());
      std::unordered_map<size_t,size_t> mask_indices;
      if (m_archive_format=="cwsxml" || m_archive_format=="cwsrgb")
        for (size_t i=0;i<m_layers.size();++i) mask_indices.emplace(m_layers[i],i);
      for (size_t entry_index=0;entry_index<m_entries.size();++entry_index) {
        const auto &entry=m_entries[entry_index];
        const auto mask=mask_indices.find(entry_index);
        if (mask!=mask_indices.end()) {
          const auto encoded=m_archive_format=="cwsrgb"?encode_bgr_packed_mask(image(mask->second)):encode_rgba_mask(image(mask->second));
          zip.add_entry(entry.name,encoded.data(),encoded.size());
        } else if(updated_chitubox && boost::algorithm::iequals(entry.name,"run.gcode")) {
          zip.add_entry(entry.name);zip<<*updated_chitubox;
        } else if (used_material &&
            boost::algorithm::iequals(entry.name, "config.ini")) {
          std::ostringstream value;
          value.imbue(std::locale::classic());
          value << std::setprecision(12) << *used_material;
          std::istringstream original(
              std::string(entry.bytes.begin(), entry.bytes.end()));
          std::ostringstream updated;
          bool found = false;
          for (std::string line; std::getline(original, line);) {
            const auto separator = line.find('=');
            if (separator != std::string::npos &&
                boost::algorithm::trim_copy(line.substr(0, separator)) ==
                    "usedMaterial") {
              const bool crlf = !line.empty() && line.back() == '\r';
              line = line.substr(0, separator + 1) + " " + value.str() +
                     (crlf ? "\r" : "");
              found = true;
            }
            updated << line << '\n';
          }
          if (!found)
            updated << "usedMaterial = " << value.str() << '\n';
          zip.add_entry(entry.name);
          zip << updated.str();
        } else if (used_material &&
                   boost::algorithm::iequals(entry.name, "config.json")) {
          auto metadata = nlohmann::json::parse(entry.bytes);
          if (!metadata.is_object())
            throw RuntimeError("Invalid archived SLA JSON metadata");
          if (metadata.contains("usedMaterial") &&
              metadata["usedMaterial"].is_string()) {
            std::ostringstream value;
            value.imbue(std::locale::classic());
            value << std::setprecision(12) << *used_material;
            metadata["usedMaterial"] = value.str();
          } else
            metadata["usedMaterial"] = *used_material;
          zip.add_entry(entry.name);
          zip << metadata.dump();
        } else
          zip.add_entry(entry.name, entry.bytes.data(), entry.bytes.size());
      }
      zip.finalize();
    }
    const auto error = rename_file(temporary.string(), output.string());
    if (error)
      throw FileIOError("Cannot replace edited archive: " + error.message());
  } catch (...) {
    boost::system::error_code ignored;
    boost::filesystem::remove(temporary, ignored);
    throw;
  }
  m_saved_revision = m_revision;
}
} // namespace Slic3r::sla
