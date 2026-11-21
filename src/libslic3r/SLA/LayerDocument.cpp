#include "LayerDocument.hpp"
#include "PixelDrawing.hpp"
#include "libslic3r/Execution/ExecutionTBB.hpp"
#include <exception>
#include <mutex>
#include <map>
#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>
#include <tbb/task_arena.h>
#include "LayerMaskValidation.hpp"
#include "LayerMemoryBudget.hpp"
#include "ExposurePositionSchedule.hpp"
#include "FreshLayerMaterialPricing.hpp"
#include "LayerImageFile.hpp"
#include "MaskFilter.hpp"
#include "MedianFilter.hpp"
#include "ColorMask.hpp"
#include "RasterBase.hpp"
#include "libslic3r/miniz_extension.hpp"
#include "libslic3r/Zipper.hpp"
#include "libslic3r/Exception.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/Format/CrealityCXDLPv1Archive.hpp"
#include "libslic3r/Format/CrealityCXDLPv1Layout.hpp"
#include "libslic3r/Format/CrealityContourArea.hpp"
#include "libslic3r/Format/CrealityChecksum.hpp"
#include "libslic3r/Format/CrealityCXDLPv4.hpp"
#include "libslic3r/Format/CrealityCXDLPv4Layout.hpp"
#include "libslic3r/Format/ChituboxLayerLayout.hpp"
#include "libslic3r/Format/CTBEncryptedLayerLayout.hpp"
#include "libslic3r/Format/AnycubicLayerLayout.hpp"
#include "libslic3r/Format/AnycubicZipLayout.hpp"
#include "libslic3r/Format/KlipperLayout.hpp"
#include "libslic3r/Format/NanoDLPLayout.hpp"
#include "libslic3r/Format/PhrozenLayout.hpp"
#include "libslic3r/Format/ZCodeLayout.hpp"
#include "libslic3r/Format/LongerOrangeLayout.hpp"
#include "libslic3r/Format/UniformationLayout.hpp"
#include "libslic3r/Format/VoxeldanceLayout.hpp"
#include "libslic3r/Format/MakerbaseLayout.hpp"
#include "libslic3r/Format/Emake3DLayout.hpp"
#include "libslic3r/Format/VlareLayout.hpp"
#include "libslic3r/Format/OpenSlaLayout.hpp"
#include "libslic3r/Format/AnetLayout.hpp"
#include "libslic3r/Format/SvgxLayout.hpp"
#include "LayerExportProfile.hpp"
#include "ChituboxJobOverlay.hpp"
#include "ChituboxJobReader.hpp"
#include "ChituboxLayerParameters.hpp"
#include "CTBEncryptedLayerParameters.hpp"
#include "AnycubicLayerParameters.hpp"
#include "CrealityCXDLPv4JobReader.hpp"
#include "CrealityCXDLPv4ReferenceBottom.hpp"
#include "CrealityCXDLPv4MaskReader.hpp"
#include "libslic3r/Format/SLAArchiveFormatRegistry.hpp"
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
#include <chrono>
#include <cstdlib>
#include <boost/log/trivial.hpp>
#include <array>
#include <unordered_map>
#include <string_view>

// Defined in UVToolsCompatArchive.cpp without a public header.
namespace Slic3r { std::vector<uint8_t> encode_goo_rle(const uint8_t *src, size_t pixel_count); }

namespace Slic3r::sla {
LayerParameterCapabilities parameter_conversion_schema(const std::string &destination)
{
    if(destination=="ctb_v3")return chitubox_parameter_capabilities(3);
    if(destination=="ctb_v4" || destination=="ctb_v5" || destination=="ctb_gktwo" ||
       destination=="ctb_encrypted_v4" || destination=="ctb_encrypted")
        return chitubox_parameter_capabilities(4);
    return {};
}
namespace {
struct CancelledMaskEdit {};
std::vector<uint8_t> encode_parameters(const LayerParameters &parameters)
{
    nlohmann::json json=nlohmann::json::object();
    for (const auto &[field,value]:parameters) json[std::to_string(int(field))]=value;
    const auto text=json.dump();return {text.begin(),text.end()};
}
LayerParameters decode_parameters(const std::vector<uint8_t> &bytes)
{
    if (bytes.size()>65536) throw RuntimeError("Layer parameter entry exceeds its budget");
    const auto json=nlohmann::json::parse(bytes);
    if (!json.is_object()) throw RuntimeError("Invalid layer parameter entry");
    LayerParameters result;
    for (auto it=json.begin();it!=json.end();++it) {
        const auto field=std::stoi(it.key());
        if (it.key()!=std::to_string(field) || !it.value().is_number())
            throw RuntimeError("Invalid canonical layer parameter key or value");
        const auto value=it.value().get<double>();
        if (field<0 || field>int(LayerParameter::LightPWM) || !std::isfinite(value) || value<0)
            throw RuntimeError("Invalid canonical layer parameter");
        result.emplace(static_cast<LayerParameter>(field),value);
    }
    return result;
}

// Material volume follows actual nonzero pixels and per-layer Z, rather than
// a stale mesh estimate. The bounded cache compares complete immutable PNGs.
double mask_volume_ml(const LayerDocument &document,double width,double height,const std::vector<double> &positions,const LayerProgress &progress)
{
    if(positions.size()!=document.layer_count()) throw RuntimeError("Archived SLA layer positions are incomplete");
    const auto first=document.image(0);
    const double area=width*height/(double(first.cols)*first.rows);
    std::unordered_map<std::string_view,uint64_t> counts;
    double volume=0.,previous=0.;
    for(size_t i=0;i<positions.size();++i) {
        check_layer_progress(progress,i,positions.size());
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

// The editor keeps the whole source container in memory. A file shorter than
// its fixed header is damaged, while a file above the budget is valid but too
// large; report those separately so the user knows which action applies.

void check_archive_length(std::streamoff length, std::streamoff minimum, const char *kind)
{
    if (length < 0)
        throw FileIOError(std::string("Cannot read ") + kind + " file");
    if (length < minimum)
        throw RuntimeError(std::string(kind) + " file is truncated: " + std::to_string(static_cast<long long>(length)) +
                           " bytes, the header needs at least " + std::to_string(static_cast<long long>(minimum)));
    if (uint64_t(length) > layer_editor_byte_budget())
        throw RuntimeError(std::string(kind) + " file (" + layer_mib(uint64_t(length)) + ") exceeds the editor memory budget (" +
                           layer_mib(layer_editor_byte_budget()) + ", derived from the free RAM). Close other applications and retry.");
}

// ---------------------------------------------------------------------------
// GOO / PRZ container (UVtools GooFile.cs). PRZ is the same container; only
// the writer differs: it must not use colour-difference chunks.
// All integers and floats are big endian.
// ---------------------------------------------------------------------------
constexpr size_t goo_header_size = 195477;      // fixed header incl. both previews
constexpr size_t goo_layer_def_size = 70;       // LayerDef fields before the RLE data
constexpr size_t goo_footer_size = 11;          // 3 padding bytes + 8-byte magic
constexpr size_t goo_layer_count_offset = 195310;
constexpr size_t goo_resolution_offset = 195314; // u16 x, u16 y
constexpr size_t goo_volume_offset = 195450;     // f32 volume, then grams, then cost
constexpr size_t goo_layer_def_address_offset = 195470;
constexpr size_t goo_def_z_offset = 6;           // LayerDef.PositionZ (f32)
constexpr size_t goo_def_delimiter_offset = 64;
constexpr size_t goo_def_length_offset = 66;
constexpr uint8_t goo_magic[8] = {0x07, 0, 0, 0, 'D', 'L', 'P', 0};

bool is_goo_format(const std::string &format) { return format == "goo" || format == "prz"; }
// Native-container families whose masks are decoded into editor PNGs while the
// complete original file is kept in entry 0 (see load_archive_impl).
bool is_anycubic_zip_layer_format(const std::string &format) { return AnycubicZipLayout::is_format(format); }
bool is_klipper_layer_format(const std::string &format) { return KlipperLayout::is_format(format); }
bool is_nanodlp_layer_format(const std::string &format) { return NanoDLPLayout::is_format(format); }
bool is_zcode_layer_format(const std::string &format) { return ZCodeLayout::is_format(format); }
bool is_phrozen_layer_format(const std::string &format) { return format == "phz" || format == "fdg"; }
PhrozenLayout::Kind phrozen_kind(const std::string &format) { return format == "fdg" ? PhrozenLayout::Kind::Fdg : PhrozenLayout::Kind::Phz; }

// ---------------------------------------------------------------------------
// Binary families with a *Layout.hpp reader (LGS, ...). Each keeps the complete
// original file in entry 0 and decodes one layer at a time from it. Adding a
// family means adding it to the three helpers below plus one probe in
// load_archive_impl; saving, quantisation and the extension rule follow.
// ---------------------------------------------------------------------------
bool is_native_binary_layer_format(const std::string &format)
{
    return LongerOrangeLayout::is_format(format) || UniformationLayout::is_format(format) || VoxeldanceLayout::is_format(format) ||
           MakerbaseLayout::is_format(format) || Emake3DLayout::is_format(format) ||
           VlareLayout::is_format(format) || OpenSlaLayout::is_format(format) ||
           AnetLayout::is_format(format) || SvgxLayout::is_format(format);
}

// What a gray value becomes once stored in the file. The editor shows exactly
// this so that what is painted is what is saved.
uint8_t quantize_native_binary(const std::string &format, uint8_t value)
{
    if (LongerOrangeLayout::is_format(format)) return LongerOrangeLayout::quantize(value);
    if (MakerbaseLayout::is_format(format)) return MakerbaseLayout::quantize(value);
    if (Emake3DLayout::is_format(format)) return Emake3DLayout::quantize(value);
    if (VlareLayout::is_format(format)) return VlareLayout::quantize(value);
    if (AnetLayout::is_format(format)) return AnetLayout::quantize(value);
    if (SvgxLayout::is_format(format)) return SvgxLayout::quantize(value);
    return value;
}

// ---------------------------------------------------------------------------
// Per-layer parameters of the wave 7-8 families. A family joins this list when
// its container really stores per-layer values; the parameter rows are then
// kept as small JSON entries beside the masks (like CTB and Anycubic), so they
// share Undo/Redo, project persistence and the editor panel. The rows are read
// from the untouched file in entry 0, which stays the reference for "edited?".
// ZCode (RSA-framed lcd.gcode), ZCodex (ResinGCodeData) and JXS (control.json
// action list) keep their per-layer program as G-code-like text and join the
// list too. Containers that keep only global values (LGS, MDLP, GR1, OSF, Anet,
// NanoDLP: UVtools has no per-layer modifiers for them), that store no
// parameters at all (QDT) or that cannot be rewritten (SVGX) have no rows and
// their parameters stay read-only by design.
// ---------------------------------------------------------------------------
enum class NativeFamily { None, Phrozen, OpenSla, Vdt, AnycubicZip, Klipper, ZCode, ZCodex, Jxs };

NativeFamily native_family(const std::string &format)
{
    if (format == "phz" || format == "fdg") return NativeFamily::Phrozen;
    if (OpenSlaLayout::is_format(format)) return NativeFamily::OpenSla;
    if (VoxeldanceLayout::is_format(format)) return NativeFamily::Vdt;
    if (AnycubicZipLayout::is_format(format)) return NativeFamily::AnycubicZip;
    if (KlipperLayout::is_format(format)) return NativeFamily::Klipper;
    if (format == "zcode") return NativeFamily::ZCode;
    if (format == "zcodex") return NativeFamily::ZCodex;
    if (UniformationLayout::is_format(format)) return NativeFamily::Jxs;
    return NativeFamily::None;
}

struct NativeTable {
    native_parameters::Table rows;
    double height_limit = 0; // machine height the container declares; 0 = unknown
    size_t bottom_layers = 0; // the container's bottom layer count (0 = unknown)
};

// Rows of the container in `bytes`, or nothing when it is not shaped the way
// our writers produce it (then the document simply stays without parameters:
// no guessing at foreign layouts).
std::optional<NativeTable> read_native_table(const std::string &format, const NativeBytes &image)
{
    try {
        // Every family reads straight from the (possibly store-backed) image; ZIP families through MemoryZip's store reader.
        NativeTable table;
        const auto family = native_family(format);
        switch (family) {
        case NativeFamily::Phrozen: {
            const PhrozenLayout layout(image, phrozen_kind(format));
            table.rows = layout.parameter_rows(); table.height_limit = layout.height_limit(); table.bottom_layers = layout.bottom_layers();
            break;
        }
        case NativeFamily::OpenSla: {
            const OpenSlaLayout layout(image);
            table.rows = layout.parameter_rows(); table.height_limit = layout.height_limit(); table.bottom_layers = layout.bottom_layers();
            break;
        }
        case NativeFamily::Vdt: {
            const MemoryZip zip(image, "VDT");
            const VoxeldanceLayout layout(zip);
            table.rows = layout.parameter_rows(); table.height_limit = layout.height_limit(); table.bottom_layers = layout.bottom_layers();
            break;
        }
        case NativeFamily::AnycubicZip: {
            const MemoryZip zip(image, "Anycubic ZIP");
            const AnycubicZipLayout layout(zip, format);
            table.rows = layout.parameter_rows();
            // Z lives in two files; edit it only when they agree (see AnycubicZipLayout::scene_matches).
            if (!layout.scene_matches(table.rows)) return std::nullopt;
            table.height_limit = layout.height_limit(); table.bottom_layers = layout.bottom_layers();
            break;
        }
        case NativeFamily::Klipper: {
            const MemoryZip zip(image, "Klipper ZIP");
            const KlipperLayout layout(zip);
            table.rows = layout.parameter_rows(); table.height_limit = layout.height_limit(); table.bottom_layers = layout.bottom_layers();
            break;
        }
        case NativeFamily::ZCode: case NativeFamily::ZCodex: {
            const auto kind = family == NativeFamily::ZCode ? ZCodeLayout::Kind::ZCode : ZCodeLayout::Kind::ZCodex;
            const MemoryZip zip(image, family == NativeFamily::ZCode ? "ZCode" : "ZCodex");
            const ZCodeLayout layout(zip, kind);
            table.rows = layout.parameter_rows(); table.height_limit = layout.height_limit(); table.bottom_layers = layout.bottom_layers();
            break;
        }
        case NativeFamily::Jxs: {
            const MemoryZip zip(image, "JXS");
            const UniformationLayout layout(zip);
            table.rows = layout.parameter_rows(); table.height_limit = layout.height_limit(); table.bottom_layers = layout.bottom_layers();
            break;
        }
        case NativeFamily::None: return std::nullopt;
        }
        return table;
    } catch (const RuntimeError &) {
        return std::nullopt;
    }
}

// Bottom layer count of a native-family container; nothing for the other formats.
std::optional<size_t> native_bottom_layers(const std::string &format, const NativeBytes &bytes)
{
    if (native_family(format) == NativeFamily::None) return std::nullopt;
    const auto table = read_native_table(format, bytes);
    if (!table) return std::nullopt;
    return table->bottom_layers;
}

LayerParameterCapabilities native_capabilities(NativeFamily family)
{
    switch (family) {
    case NativeFamily::Phrozen:     return PhrozenLayout::parameter_capabilities();
    case NativeFamily::OpenSla:     return OpenSlaLayout::parameter_capabilities();
    case NativeFamily::Vdt:         return VoxeldanceLayout::parameter_capabilities();
    case NativeFamily::AnycubicZip: return AnycubicZipLayout::parameter_capabilities();
    case NativeFamily::Klipper:     return KlipperLayout::parameter_capabilities();
    case NativeFamily::ZCode:       return ZCodeLayout::parameter_capabilities(ZCodeLayout::Kind::ZCode);
    case NativeFamily::ZCodex:      return ZCodeLayout::parameter_capabilities(ZCodeLayout::Kind::ZCodex);
    case NativeFamily::Jxs:         return UniformationLayout::parameter_capabilities();
    case NativeFamily::None:        break;
    }
    return {};
}

// The value the container reads back after `value` was stored in `field`.
double native_canonical(NativeFamily family, LayerParameter field, double value)
{
    switch (family) {
    case NativeFamily::Vdt:         return VoxeldanceLayout::canonical(field, value);
    case NativeFamily::AnycubicZip: return AnycubicZipLayout::canonical(field, value);
    case NativeFamily::Klipper:     return KlipperLayout::canonical(field, value);
    case NativeFamily::ZCode:       return ZCodeLayout::canonical(ZCodeLayout::Kind::ZCode, field, value);
    case NativeFamily::ZCodex:      return ZCodeLayout::canonical(ZCodeLayout::Kind::ZCodex, field, value);
    case NativeFamily::Jxs:         return UniformationLayout::canonical(field, value);
    case NativeFamily::OpenSla:
        return field == LayerParameter::LightPWM ? value : native_parameters::as_float(value);
    case NativeFamily::Phrozen:     return native_parameters::as_float(value);
    case NativeFamily::None:        break;
    }
    return value;
}

// Rules the container's own writer enforces, so that an edit can never produce
// a file that the exporter would have refused.
void validate_native_row(NativeFamily family, const LayerParameters &row, double previous_z = 0)
{
    using F = LayerParameter;
    switch (family) {
    case NativeFamily::Phrozen:
        break;
    case NativeFamily::OpenSla:
        native_parameters::require_pwm(row, "OSLA");
        native_parameters::require_moves_have_speed(row, "OSLA");
        // The exporter's program also moves to Z when nothing is lifted, and needs a speed for it.
        if (native_parameters::value_of(row, F::LiftDistance) + native_parameters::value_of(row, F::LiftDistance2) == 0 &&
            native_parameters::value_of(row, F::LiftSpeed) <= 0)
            throw RuntimeError("OSLA movement requires a positive lift speed");
        break;
    case NativeFamily::Vdt:
        native_parameters::require_pwm(row, "VDT");
        native_parameters::require_moves_have_speed(row, "VDT");
        break;
    case NativeFamily::AnycubicZip:
        if (native_parameters::value_of(row, F::LiftDistance) > 0 && native_parameters::value_of(row, F::LiftSpeed) <= 0)
            throw RuntimeError("Anycubic ZIP: an active lift needs a positive speed");
        break;
    case NativeFamily::Klipper:
        native_parameters::require_pwm(row, "Klipper");
        native_parameters::require_moves_have_speed(row, "Klipper");
        KlipperLayout::check_storable(row);
        break;
    case NativeFamily::ZCode:
        native_parameters::require_moves_have_speed(row, "ZCode");
        ZCodeLayout::check_storable(ZCodeLayout::Kind::ZCode, row);
        break;
    case NativeFamily::ZCodex:
        ZCodeLayout::check_storable(ZCodeLayout::Kind::ZCodex, row);
        ZCodeLayout::check_step(ZCodeLayout::Kind::ZCodex, row, previous_z);
        break;
    case NativeFamily::Jxs:
        native_parameters::require_pwm(row, "JXS");
        native_parameters::require_moves_have_speed(row, "JXS");
        UniformationLayout::check_storable(row);
        break;
    case NativeFamily::None:
        break;
    }
}

template<class L, class = void> struct has_parameter_rows : std::false_type {};
template<class L> struct has_parameter_rows<L, std::void_t<decltype(std::declval<const L &>().parameter_rows())>> : std::true_type {};

// The full table of the document, or nothing when it stores none.
std::optional<native_parameters::Table> document_table(const LayerDocument &document)
{
    if (document.parameter_capabilities().empty()) return std::nullopt;
    native_parameters::Table table;
    table.reserve(document.layer_count());
    for (size_t i = 0; i < document.layer_count(); ++i) table.push_back(document.layer_parameters(i));
    return table;
}

// ---------------------------------------------------------------------------
// Shared pieces of the bounded-memory native families (the container is referenced, never held).
// A Layout reads a NativeBytes view and offers format/count/columns/rows and decode(layer).
// ---------------------------------------------------------------------------
// Layer store of a native layout: layer N is `layout->decode(N)` encoded as the editable PNG.
template<class Layout>
std::shared_ptr<DerivedLayerStore> derived_png_store(std::shared_ptr<const Layout> layout, std::string failure)
{
    return std::make_shared<DerivedLayerStore>([layout, failure](uint64_t number) {
        const auto pixels = layout->decode(number);
        const auto png = PNGRasterEncoder{}(pixels.data(), layout->columns, layout->rows, 1);
        if (!png.size()) throw RuntimeError(failure);
        const auto *begin = static_cast<const uint8_t *>(png.data());
        return std::vector<uint8_t>(begin, begin + png.size());
    });
}

// Streams `image` to `temporary` in 4 MiB pieces (cancellable).
template<class Image>
void stream_image_to_file(const Image &image, uint64_t size, const std::string &temporary, const std::string &failure,
                          const LayerProgress &progress)
{
    boost::nowide::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
    for (uint64_t at = 0; at < size;) {
        check_layer_progress(progress);
        const auto part = image.read(at, std::min<uint64_t>(size - at, uint64_t(4) << 20));
        stream.write(reinterpret_cast<const char *>(part.data()), std::streamsize(part.size()));
        if (!stream) throw FileIOError(failure);
        at += part.size();
    }
    stream.close();
    if (!stream) throw FileIOError(failure);
}

// Saves a binary family kept whole in entry 0. An unchanged document is copied
// byte for byte; otherwise only the layers whose mask differs are re-encoded
// (Layout::rebuild) and the result is proven to read back as the edited masks
// before it can replace the previous file.
template<class Layout, class... Variant>
void save_native_binary(const LayerDocument &document, const std::vector<uint8_t> &original, bool unchanged,
                        const char *kind, const std::string &temporary, const LayerProgress &progress, Variant... variant)
{
    const std::string failure = std::string("Cannot write edited ") + kind + " layers";
    auto write = [&](const std::vector<uint8_t> &data) {
        boost::nowide::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        for (size_t at = 0; at < data.size();) {
            check_layer_progress(progress);
            const size_t chunk = std::min(size_t(1) << 20, data.size() - at);
            stream.write(reinterpret_cast<const char *>(data.data() + at), std::streamsize(chunk));
            if (!stream) throw FileIOError(failure);
            at += chunk;
        }
        stream.close();
        if (!stream) throw FileIOError(failure);
    };
    if (unchanged) { write(original); return; }
    const std::function<void()> poll = [&]{ check_layer_progress(progress); };
    const Layout layout(original, variant..., poll);
    const size_t layers = document.layer_count();
    if (layout.count != layers) throw RuntimeError(std::string(kind) + " document layer count changed");
    std::map<size_t, std::vector<uint8_t>> edited;
    for (size_t i = 0; i < layers; ++i) {
        check_layer_progress(progress, i, layers);
        auto mask = document.image(i);
        if (mask.cols != layout.columns || mask.rows != layout.rows)
            throw RuntimeError(std::string("Edited ") + kind + " mask dimensions changed");
        if (mask.buf == layout.decode(i)) continue; // keeps its original record
        edited.emplace(i, std::move(mask.buf));
    }
    // Families with per-layer parameters write the document's rows as well.
    std::optional<native_parameters::Table> table;
    if constexpr (has_parameter_rows<Layout>::value) table = document_table(document);
    std::vector<uint8_t> output;
    if constexpr (has_parameter_rows<Layout>::value) output = layout.rebuild(edited, table ? &*table : nullptr);
    else output = layout.rebuild(edited);
    const Layout verified(output, variant..., poll);
    if (verified.count != layers) throw RuntimeError(std::string("Edited ") + kind + " layer table is inconsistent");
    if constexpr (has_parameter_rows<Layout>::value)
        if (table && verified.parameter_rows() != *table)
            throw RuntimeError(std::string("Edited ") + kind + " parameters do not read back as saved");
    for (const auto &[layer, pixels] : edited) {
        check_layer_progress(progress, layer, layers);
        if (verified.decode(layer) != pixels)
            throw RuntimeError(std::string("Edited ") + kind + " layer " + std::to_string(layer + 1) + " does not read back as saved");
    }
    write(output);
}


// Saves a binary family that is referenced, not held (original = the source image). An unchanged document
// streams the source byte for byte. Otherwise the layers that really differ from their source decode are
// found first (a layer still in its pristine state is not even decoded), `Layout::rebuild_to` writes the
// new container into a PatchedImage that spills to disk, and the result is proven to read back as the
// edited masks before it can replace the previous file. Nothing here grows with the file size.
template<class Layout, class... Variant>
void save_native_layout(const LayerDocument &document, const NativeBytes &original, bool unchanged, const char *kind,
                        const std::string &temporary, const LayerProgress &progress,
                        const std::function<bool(size_t)> &pristine, Variant... variant)
{
    const std::string failure = std::string("Cannot write edited ") + kind + " layers";
    if (unchanged) { stream_image_to_file(original, original.size(), temporary, failure, progress); return; }
    const std::function<void()> poll = [&]{ check_layer_progress(progress); };
    const Layout layout(original, variant..., poll);
    const size_t layers = document.layer_count();
    if (layout.count != layers) throw RuntimeError(std::string(kind) + " document layer count changed");
    NativeEdits edits;
    edits.edited.assign(layers, false);
    edits.pixels = [&](size_t layer) { return document.image(layer).buf; };
    for (size_t i = 0; i < layers; ++i) {
        check_layer_progress(progress, i, layers);
        if (pristine(i)) continue; // still the decode of its source record
        const auto mask = document.image(i);
        if (mask.cols != layout.columns || mask.rows != layout.rows)
            throw RuntimeError(std::string("Edited ") + kind + " mask dimensions changed");
        if (mask.buf == layout.decode(i)) continue; // keeps its original record
        edits.edited[i] = true;
    }
    // Families with per-layer parameters write the document's rows as well.
    std::optional<native_parameters::Table> table;
    if constexpr (has_parameter_rows<Layout>::value) table = document_table(document);
    PatchedImage output{NativeBytes()};
    if constexpr (has_parameter_rows<Layout>::value) layout.rebuild_to(output, edits, table ? &*table : nullptr, poll);
    else layout.rebuild_to(output, edits, poll);
    const Layout verified(output.view(), variant..., poll);
    if (verified.count != layers) throw RuntimeError(std::string("Edited ") + kind + " layer table is inconsistent");
    if constexpr (has_parameter_rows<Layout>::value)
        if (table && verified.parameter_rows() != *table)
            throw RuntimeError(std::string("Edited ") + kind + " parameters do not read back as saved");
    for (size_t i = 0; i < layers; ++i) {
        if (!edits.edited[i]) continue;
        check_layer_progress(progress, i, layers);
        if (verified.decode(i) != edits.pixels(i))
            throw RuntimeError(std::string("Edited ") + kind + " layer " + std::to_string(i + 1) + " does not read back as saved");
    }
    stream_image_to_file(output, output.size(), temporary, failure, progress);
}

// Saves a ZIP family kept whole in entry 0. Unchanged documents are copied
// byte for byte (compression and entry order included). Otherwise the archive
// is rewritten in its original order with only the edited layer images
// replaced; every other entry (scripts, previews, vendor files) is carried over.
// `Layout` provides count/columns/rows, layer_entry(i), decode(i) and encode(pixels).
template<class Layout, class Open>
void save_native_zip(const LayerDocument &document, const NativeBytes &original, bool unchanged,
                     const char *kind, const std::string &temporary, Zipper::e_compression compression,
                     const Open &open, const LayerProgress &progress)
{
    const std::string failure = std::string("Cannot write edited ") + kind;
    if (unchanged) {
        boost::nowide::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        for (size_t at = 0; at < original.size();) {
            check_layer_progress(progress);
            const size_t chunk = std::min(size_t(4) << 20, original.size() - at);
            const auto part = original.read(at, chunk);
            stream.write(reinterpret_cast<const char *>(part.data()), std::streamsize(chunk));
            if (!stream) throw FileIOError(failure);
            at += chunk;
        }
        stream.close();
        if (!stream) throw FileIOError(failure);
        return;
    }
    const MemoryZip source(original, kind);
    const Layout layout = open(source);
    const size_t layers = document.layer_count();
    if (layout.count != layers) throw RuntimeError(std::string(kind) + " document layer count changed");
    std::map<std::string, std::vector<uint8_t>> replacements;
    std::map<std::string, std::vector<uint8_t>> expected; // entry -> pixels, for the readback proof
    // Families with per-layer parameters also refresh the statistics their
    // manifests keep, which needs every layer's lit area before and after.
    std::optional<native_parameters::Table> table;
    if constexpr (has_parameter_rows<Layout>::value) table = document_table(document);
    std::vector<native_parameters::MaskSummary> old_masks, new_masks;
    for (size_t i = 0; i < layers; ++i) {
        check_layer_progress(progress, i, layers);
        const auto mask = document.image(i);
        if (mask.cols != layout.columns || mask.rows != layout.rows)
            throw RuntimeError(std::string("Edited ") + kind + " mask dimensions changed");
        const auto original = layout.decode(i);
        const bool same = mask.buf == original;
        if (table) {
            old_masks.push_back(native_parameters::summarize(original, layout.columns, layout.rows));
            new_masks.push_back(same ? old_masks.back() : native_parameters::summarize(mask.buf, layout.columns, layout.rows));
        }
        // Layers that still match the file keep their original bytes.
        if (same) continue;
        replacements.emplace(layout.layer_entry(i), layout.encode(mask.buf));
        expected.emplace(layout.layer_entry(i), mask.buf);
    }
    if constexpr (has_parameter_rows<Layout>::value)
        if (table)
            for (auto &[name, bytes] : layout.parameter_replacements(*table, old_masks, new_masks))
                replacements[name] = std::move(bytes);
    {
        Zipper zip(temporary, compression);
        source.rewrite(zip, replacements, [&]{ check_layer_progress(progress); });
        zip.finalize();
    }
    // Prove the rebuilt archive decodes to exactly the edited pixels before it
    // can replace the previous file.
    // The verification reads the written file back through a store: the rebuilt archive is never resident.
    const SourceFileStore rebuilt_store(temporary);
    check_archive_length(std::streamoff(rebuilt_store.size()), 22, kind);
    const NativeBytes rebuilt(rebuilt_store, rebuilt_store.size());
    const MemoryZip verified_zip(rebuilt, kind);
    const Layout verified = open(verified_zip);
    if (verified.count != layers) throw RuntimeError(std::string("Edited ") + kind + " layer table is inconsistent");
    if constexpr (has_parameter_rows<Layout>::value)
        if (table && verified.parameter_rows() != *table)
            throw RuntimeError(std::string("Edited ") + kind + " parameters do not read back as saved");
    for (size_t i = 0; i < layers; ++i) {
        const auto wanted = expected.find(verified.layer_entry(i));
        if (wanted == expected.end()) continue;
        check_layer_progress(progress, i, layers);
        if (verified.decode(i) != wanted->second)
            throw RuntimeError(std::string("Edited ") + kind + " layer " + std::to_string(i + 1) + " does not read back as saved");
    }
}

void save_native_binary_dispatch(const LayerDocument &document, const std::string &format, const NativeBytes &original,
                                 bool unchanged, const std::string &temporary, const LayerProgress &progress)
{
    if (UniformationLayout::is_format(format))
        return save_native_zip<UniformationLayout>(document, original, unchanged, "JXS", temporary, Zipper::FAST_COMPRESSION,
            [](const MemoryZip &zip) { return UniformationLayout(zip); }, progress);
    if (SvgxLayout::is_format(format)) {
        // Vector contours: only an untouched document can be saved (see SvgxLayout.hpp).
        if (!unchanged) throw RuntimeError(SvgxLayout::read_only_reason());
        return save_native_binary<SvgxLayout>(document, original.to_vector(), true, "SVGX", temporary, progress);
    }
    if (VoxeldanceLayout::is_format(format))
        return save_native_zip<VoxeldanceLayout>(document, original, unchanged, "VDT", temporary, Zipper::FAST_COMPRESSION,
            [](const MemoryZip &zip) { return VoxeldanceLayout(zip); }, progress);
    throw RuntimeError("Unsupported native layer format " + format);
}

// The first 8 bytes are "Vx.y" plus the first 4 magic bytes.
bool is_goo_signature(const uint8_t *signature)
{
    return signature[0] == 'V' && std::memcmp(signature + 4, goo_magic, 4) == 0;
}

uint32_t goo_be32(const uint8_t *p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
float goo_bef32(const uint8_t *p) { const uint32_t bits = goo_be32(p); float v; std::memcpy(&v, &bits, 4); return v; }
void goo_put_bef32(uint8_t *p, float v)
{
    uint32_t bits; std::memcpy(&bits, &v, 4);
    for (unsigned i = 0; i < 4; ++i) p[i] = uint8_t(bits >> (24 - 8 * i));
}

// Validates every address and length before anything is allocated from them.
struct GooLayout {
    struct Layer { size_t def, data, length; };
    size_t columns = 0, rows = 0, layer_def_address = 0, footer = 0;
    std::vector<Layer> layers;
    NativeBytes bytes;

    size_t pixel_count() const { return columns * rows; }
    float z(size_t layer) const { const auto v = bytes.read(layers[layer].def + goo_def_z_offset, 4); return goo_bef32(v.data()); }
    float number(size_t at) const { const auto v = bytes.read(at, 4); return goo_bef32(v.data()); }
    uint32_t be32_at(size_t at) const { const auto v = bytes.read(at, 4); return goo_be32(v.data()); }
    std::vector<uint8_t> payload(size_t layer) const { return bytes.read(layers[layer].data, layers[layer].length); }

    GooLayout(const NativeBytes &source, const std::function<void()> &tick) : bytes(source)
    {
        if (bytes.size() < goo_header_size + goo_footer_size)
            throw RuntimeError("GOO file is truncated: the header needs " + std::to_string(goo_header_size + goo_footer_size) + " bytes");
        {
            const auto head = bytes.read(0, 12);
            if (head[0] != 'V' || std::memcmp(head.data() + 4, goo_magic, 8) != 0)
                throw RuntimeError("Invalid GOO header magic");
        }
        const size_t count = be32_at(goo_layer_count_offset);
        const auto resolution = bytes.read(goo_resolution_offset, 4);
        columns = (size_t(resolution[0]) << 8) | resolution[1];
        rows = (size_t(resolution[2]) << 8) | resolution[3];
        if (!count || count > 100000)
            throw RuntimeError("Invalid GOO layer count " + std::to_string(count));
        if (!columns || !rows || columns * rows > 128 * 1024 * 1024)
            throw RuntimeError("Invalid GOO resolution " + std::to_string(columns) + "x" + std::to_string(rows));
        layer_def_address = be32_at(goo_layer_def_address_offset);
        if (layer_def_address < goo_header_size || layer_def_address > bytes.size() - goo_footer_size)
            throw RuntimeError("Invalid GOO layer table address " + std::to_string(layer_def_address));
        // Each layer needs its definition and the two-byte data delimiter at least.
        if (count > (bytes.size() - layer_def_address - goo_footer_size) / (goo_layer_def_size + 2))
            throw RuntimeError("GOO layer table is truncated: " + std::to_string(count) + " layers do not fit in the file");
        layers.reserve(count);
        const size_t limit = bytes.size() - goo_footer_size;
        size_t position = layer_def_address;
        for (size_t i = 0; i < count; ++i) {
            tick();
            if (limit - std::min(limit, position) < goo_layer_def_size)
                throw RuntimeError("GOO layer " + std::to_string(i + 1) + " definition is truncated");
            const auto def = bytes.read(position, goo_layer_def_size);
            if (def[goo_def_delimiter_offset] != 0x0D || def[goo_def_delimiter_offset + 1] != 0x0A)
                throw RuntimeError("GOO layer " + std::to_string(i + 1) + " definition delimiter is missing");
            const size_t length = goo_be32(def.data() + goo_def_length_offset);
            const size_t data = position + goo_layer_def_size;
            if (length > limit - data || limit - data - length < 2)
                throw RuntimeError("GOO layer " + std::to_string(i + 1) + " data is truncated");
            const auto delimiter = bytes.read(data + length, 2);
            if (delimiter[0] != 0x0D || delimiter[1] != 0x0A)
                throw RuntimeError("GOO layer " + std::to_string(i + 1) + " data delimiter is missing");
            layers.push_back({position, data, length});
            position = data + length + 2;
        }
        footer = position;
        if (!bytes.equals(bytes.size() - 8, goo_magic, 8))
            throw RuntimeError("GOO footer magic mismatch");
    }
};

// Decodes one layer into `pixels` (pixel_count bytes). Mirrors UVtools
// LayerDef.DecodeImage but rejects runs that would leave the raster.
void decode_goo_layer(const uint8_t *rle, size_t length, uint8_t *pixels, size_t pixel_count, size_t layer)
{
    const std::string name = "GOO layer " + std::to_string(layer + 1);
    std::memset(pixels, 0, pixel_count);
    if (length <= 3) return; // UVtools treats this as an empty layer
    if (rle[0] != 0x55) throw RuntimeError(name + " RLE magic is invalid");
    const size_t last = length - 1; // checksum byte
    uint8_t checksum = 0;
    for (size_t i = 1; i < last; ++i) checksum += rle[i];
    if (uint8_t(~checksum) != rle[last]) throw RuntimeError(name + " RLE checksum mismatch");
    uint8_t color = 0;
    size_t position = 0;
    auto need = [&](size_t index, size_t extra) { if (index + extra >= last) throw RuntimeError(name + " RLE chunk is truncated"); };
    for (size_t i = 1; i < last; ++i) {
        const uint8_t b0 = rle[i];
        const unsigned type = b0 >> 6;
        uint64_t stride = 0;
        if (type == 2) { // colour difference relative to the previous colour
            const unsigned kind = (b0 >> 4) & 3;
            const uint8_t diff = b0 & 0x0F;
            color = uint8_t(kind < 2 ? color + diff : color - diff);
            if (kind & 1) { need(i, 1); stride = rle[++i]; }
            else stride = 1;
        } else {
            if (type == 0) color = 0;
            else if (type == 3) color = 255;
            else { need(i, 1); color = rle[++i]; }
            const unsigned extra = (b0 >> 4) & 3;
            need(i, extra);
            uint64_t high = 0;
            for (unsigned k = 1; k <= extra; ++k) high = (high << 8) | rle[i + k];
            stride = (high << 4) | (b0 & 0x0F);
            i += extra;
        }
        if (stride > pixel_count - position) throw RuntimeError(name + " RLE run exceeds the raster");
        std::memset(pixels + position, color, size_t(stride));
        position += size_t(stride);
    }
}

// PRZ files must not contain colour-difference chunks (UVtools disables them
// for .prz), so runs are written as plain black/gray/white chunks only.
std::vector<uint8_t> encode_goo_plain_rle(const uint8_t *src, size_t pixel_count)
{
    std::vector<uint8_t> rle{0x55};
    for (size_t i = 0; i < pixel_count;) {
        size_t end = i + 1;
        while (end < pixel_count && src[end] == src[i]) ++end;
        for (uint64_t run = end - i; run > 0;) {
            const uint32_t part = uint32_t(std::min<uint64_t>(run, 0xFFFFFFFu)); // 28-bit stride limit
            const size_t first = rle.size();
            rle.push_back(src[i] == 0xFF ? 0xC0 : src[i] ? 0x40 : 0x00);
            if (src[i] && src[i] != 0xFF) rle.push_back(src[i]);
            rle[first] |= uint8_t(part & 0x0F);
            const unsigned extra = part <= 0x0F ? 0 : part <= 0xFFF ? 1 : part <= 0xFFFFF ? 2 : 3;
            rle[first] |= uint8_t(extra << 4);
            for (unsigned k = extra; k > 0; --k) rle.push_back(uint8_t(part >> (4 + 8 * (k - 1))));
            run -= part;
        }
        i = end;
    }
    uint8_t checksum = 0;
    for (size_t i = 1; i < rle.size(); ++i) checksum += rle[i];
    rle.push_back(uint8_t(~checksum));
    return rle;
}
} // namespace
namespace {
constexpr uint64_t payload_disk_margin = uint64_t(256) << 20;
}
std::shared_ptr<HistorySpill> LayerDocument::payload_spill()
{
    if (!m_payload_spill) m_payload_spill = std::make_shared<HistorySpill>();
    return m_payload_spill;
}
void LayerDocument::store_payload(Entry &entry, std::vector<uint8_t> bytes, uint64_t &resident)
{
    if (resident + bytes.size() > layer_payload_resident_budget()) {
        try {
            const auto space = layer_temp_space_available();
            if (!space || *space > bytes.size() + payload_disk_margin) {
                auto spill = payload_spill();
                const uint64_t at = spill->append(bytes);
                entry.stored = StoredBytes{spill, at, bytes.size()};
                entry.bytes.clear();
                return;
            }
        } catch (const std::exception &) {} // no disk: keep it in RAM; the document budget still applies
    }
    resident += bytes.size();
    entry.stored = {};
    entry.bytes = std::move(bytes);
}
void LayerDocument::spill_payloads(const std::set<size_t> *keep)
{
    const uint64_t budget = layer_payload_resident_budget();
    uint64_t resident = 0;
    for (const auto *list : {&m_layers, &m_secondary_layers})
        for (size_t index : *list) resident += m_entries[index].bytes.size();
    if (resident <= budget) return;
    try {
        std::set<size_t> seen;
        for (const auto *list : {&m_layers, &m_secondary_layers})
            for (size_t index : *list) {
                if (resident <= budget) return;
                auto &entry = m_entries[index];
                if (entry.stored || entry.bytes.empty() || (keep && keep->count(index)) || !seen.insert(index).second) continue;
                const auto space = layer_temp_space_available();
                if (space && *space < entry.bytes.size() + payload_disk_margin) return;
                auto spill = payload_spill();
                const uint64_t at = spill->append(entry.bytes);
                entry.stored = StoredBytes{spill, at, entry.bytes.size()};
                resident -= entry.bytes.size();
                std::vector<uint8_t>().swap(entry.bytes);
            }
    } catch (const std::exception &) {} // payloads stay resident
}
uint64_t LayerDocument::resident_payload_bytes() const
{
    uint64_t result = 0;
    for (const auto &entry : m_entries) result += entry.bytes.size();
    return result;
}
uint64_t LayerDocument::stored_payload_bytes() const
{
    uint64_t result = 0;
    for (const auto &entry : m_entries) if (entry.stored) result += entry.stored.len;
    return result;
}
LayerDocument LayerDocument::from_rasters(std::vector<EncodedRaster> layers, const LayerProgress &progress)
{
    check_layer_progress(progress, 0, layers.size());
    LayerDocument result;
    result.m_entries.reserve(layers.size());
    result.m_layers.reserve(layers.size());
    uint64_t total = 0;
    for (size_t i = 0; i < layers.size(); ++i) {
        const auto &raster = layers[i];
        if (!raster.size()) throw RuntimeError("The sliced document contains an empty raster");
        if (raster.size() > layer_editor_byte_budget() - total)
            throw RuntimeError("Layer editor document exceeds the editor memory budget");
        total += raster.size();
        const auto *bytes = static_cast<const uint8_t *>(raster.data());
        result.m_layers.push_back(i);
        // Bound the interval between cancellation checks even for a large PNG.
        Entry entry{"layer" + std::to_string(i), {}};
        entry.bytes.reserve(raster.size());
        constexpr size_t chunk_size = size_t(1) << 20;
        for (size_t offset = 0; offset < raster.size();) {
            check_layer_progress(progress, offset, raster.size());
            const size_t end = std::min(raster.size(), offset + chunk_size);
            entry.bytes.insert(entry.bytes.end(), bytes + offset, bytes + end);
            offset = end;
        }
        result.m_entries.push_back(std::move(entry));
        // The by-value input is owned by this factory. Release each copied PNG
        // immediately instead of retaining a second complete document until return.
        layers[i] = EncodedRaster{};
        check_layer_progress(progress, i + 1, layers.size());
    }
    return result;
}

void FreshLayerJob::validate(size_t layer_count, const LayerProgress &progress) const
{
    check_layer_progress(progress, 0, layer_count);
    if (mask_precision != LayerMaskPrecision::Gray8 && mask_precision != LayerMaskPrecision::NativeGray7 && mask_precision != LayerMaskPrecision::NativeGray4 && !mask_bitplanes(mask_precision))
        throw RuntimeError("Unknown source mask precision");
    if (parameter_origin != FreshLayerParameterOrigin::Explicit &&
        parameter_origin != FreshLayerParameterOrigin::AutomaticModel)
        throw RuntimeError("Unknown fresh parameter origin");
    if (parameter_origin == FreshLayerParameterOrigin::AutomaticModel && producer != FreshLayerProducer::ModelSlice)
        throw RuntimeError("Automatic model parameters require model-produced masks");
    if (producer != FreshLayerProducer::Raster && producer != FreshLayerProducer::ModelSlice &&
        producer != FreshLayerProducer::Calibration && producer != FreshLayerProducer::PCB)
        throw RuntimeError("Unknown fresh layer producer");
    if (!layer_count || layer_count > 100000 || rows.size() != layer_count)
        throw RuntimeError("Fresh job requires one complete parameter row per mask");
    geometry.validate();
    if (geometry.columns > 32768 || geometry.rows > 32768 ||
        geometry.columns * geometry.rows > (size_t(128) << 20))
        throw RuntimeError("Fresh job screen exceeds the raster budget");
    globals.validate(layer_count);
    using F = LayerParameter;
    ExposurePositionValidator positions(position_policy);
    if (position_policy == ExposurePositionPolicy::RepeatedExposureHeights &&
        (parameter_origin != FreshLayerParameterOrigin::Explicit ||
         (producer != FreshLayerProducer::Raster && producer != FreshLayerProducer::Calibration)))
        throw RuntimeError("Repeated exposures require explicit raster or calibration intentions");
    size_t checked_rows = 0;
    for (const auto &row : rows) {
        check_layer_progress(progress, checked_rows++, layer_count);
        if (row.size() != 14) throw RuntimeError("Incomplete fresh layer parameter row");
        // The v1 descriptor schema owns these fourteen stable enum fields.
        for (int field = int(F::PositionZ); field <= int(F::LightPWM); ++field) {
            const auto found = row.find(F(field));
            if (found == row.end() || !std::isfinite(found->second) || found->second < 0)
                throw RuntimeError("Fresh layer parameters must be finite and nonnegative");
        }
        const double z = row.at(F::PositionZ);
        positions.append(z);
        const double total = row.at(F::LiftDistance) + row.at(F::LiftDistance2);
        if (!std::isfinite(total) || row.at(F::RetractDistance2) > total)
            throw RuntimeError("Fresh retract distance exceeds finite lift distance");
        const double pwm = row.at(F::LightPWM);
        if (pwm > 255 || pwm != std::floor(pwm)) throw RuntimeError("Fresh light PWM must be an integer from 0 to 255");
        for (const auto &[distance, speed] : std::array<std::pair<double,F>,4>{
                std::pair<double,F>{row.at(F::LiftDistance),F::LiftSpeed},
                {row.at(F::LiftDistance2),F::LiftSpeed2},
                {total-row.at(F::RetractDistance2),F::RetractSpeed},
                {row.at(F::RetractDistance2),F::RetractSpeed2}})
            if (distance > 0 && row.at(speed) <= 0)
                throw RuntimeError("Fresh moving phases require positive speeds");
    }
    check_layer_progress(progress, layer_count, layer_count);
}

void LayerDocument::refresh_automatic_model_parameters(FreshLayerJob job)
{
    if (!m_fresh_job || m_fresh_job->parameter_origin != FreshLayerParameterOrigin::AutomaticModel ||
        job.parameter_origin != FreshLayerParameterOrigin::AutomaticModel)
        throw RuntimeError("Only automatic model intentions may refresh from model settings");
    // A preset/target change must not reinterpret stored pixels or history.
    job.mask_precision=m_fresh_job->mask_precision;
    job.validate(layer_count());
    const auto &before=m_fresh_job->geometry;
    if (before.columns!=job.geometry.columns || before.rows!=job.geometry.rows ||
        before.width_mm!=job.geometry.width_mm || before.height_mm!=job.geometry.height_mm)
        throw RuntimeError("Model metadata refresh cannot change raster geometry");
    for (size_t i=0;i<layer_count();++i)
        if (m_fresh_job->rows[i].at(LayerParameter::PositionZ)!=job.rows[i].at(LayerParameter::PositionZ))
            throw RuntimeError("Model metadata refresh cannot change layer positions");
    const auto &globals = m_fresh_job->globals;
    if (job.producer == m_fresh_job->producer && job.position_policy == m_fresh_job->position_policy &&
        job.globals.layer_height == globals.layer_height && job.globals.bottom_layers == globals.bottom_layers &&
        job.globals.transition_layers == globals.transition_layers && job.globals.normal == globals.normal &&
        job.globals.bottom == globals.bottom && job.rows == m_fresh_job->rows)
        return;
    // Redo keeps the user's requested fields. Its preceding values must come
    // from the refreshed automatic schedule, including earlier redo edits.
    // Otherwise Undo after Redo can restore an obsolete preset value.
    struct RebasedBefore { size_t edit, change; std::vector<uint8_t> bytes; };
    std::vector<RebasedBefore> rebased;
    std::map<std::pair<size_t, LayerParameter>, double> projected;
    for (size_t edit = m_redo.size(); edit-- > 0;) {
        for (size_t change = 0; change < m_redo[edit].changes.size(); ++change) {
            const auto &delta = m_redo[edit].changes[change];
            if (delta.target != Change::Target::FreshRow) continue;
            LayerParameters before;
            for (const auto &[field, value] : decode_parameters(delta.after)) {
                const auto key = std::make_pair(delta.entry, field);
                const auto found = projected.find(key);
                before[field] = found == projected.end() ? job.rows.at(delta.entry).at(field) : found->second;
                projected[key] = value;
            }
            rebased.push_back({edit, change, encode_parameters(before)});
        }
    }
    // All allocation completes before publishing either the schedule or its
    // history. No PNG or full metadata table is copied for history rebasing.
    m_export_source.invalidate();
    m_fresh_job=std::move(job);
    for (auto &before : rebased)
        m_redo[before.edit].changes[before.change].before = std::move(before.bytes);
}

LayerDocument LayerDocument::snapshot(const LayerProgress &progress) const
{
    LayerDocument result;
    check_layer_progress(progress, 0, m_entries.size());
    result.m_entries.reserve(m_entries.size());
    for (const auto &entry : m_entries) {
        result.m_entries.push_back(entry);
        check_layer_progress(progress, result.m_entries.size(), m_entries.size());
    }
    result.m_layers = m_layers;
    result.m_secondary_layers = m_secondary_layers;
    result.m_layer_parameters = m_layer_parameters;
    result.m_fresh_job = m_fresh_job;
    result.m_mask_precision = m_mask_precision;
    result.m_archive_format = m_archive_format;
    result.m_revision = m_revision;
    result.m_saved_revision = m_saved_revision;
    result.m_next_revision = m_next_revision;
    result.m_source_store = m_source_store;
    result.m_native_layers = m_native_layers;
    check_layer_progress(progress, m_entries.size(), m_entries.size());
    return result;
}

LayerDocument LayerDocument::from_precision_rasters(std::vector<EncodedRaster> layers,
    LayerMaskPrecision precision, const LayerProgress &progress)
{
    if (precision != LayerMaskPrecision::NativeGray4 && precision != LayerMaskPrecision::NativeBitplanes1)
        throw RuntimeError("Unsupported independent source mask precision");
    const auto callback=progress;
    auto result=from_rasters(std::move(layers),callback);
    result.m_mask_precision=precision;
    result.validate_and_capture_mask_statistics(callback);
    check_layer_progress(callback,result.layer_count(),result.layer_count());
    return result;
}

LayerDocument LayerDocument::from_generated_rasters(std::vector<EncodedRaster> layers, FreshLayerJob job, const LayerProgress &progress)
{
    job.validate(layers.size(), progress);
    auto result=from_rasters(std::move(layers), progress);
    if (job.mask_precision != LayerMaskPrecision::Gray8) {
        std::vector<uint8_t> previous_source, previous_projection;
        for (size_t layer=0;layer<result.layer_count();++layer) {
            check_layer_progress(progress, layer, result.layer_count());
            auto &bytes=result.m_entries[result.m_layers[layer]].bytes;
            // Flat sections often repeat. Reuse only after exact PNG-byte
            // comparison, with two bounded compressed buffers, not a cache
            // of uncompressed full-screen images.
            if (bytes==previous_source) { bytes=previous_projection;continue; }
            auto image=result.image(layer); // Checks PNG dimensions before decode.
            if (image.cols!=job.geometry.columns || image.rows!=job.geometry.rows)
                throw RuntimeError("Generated mask dimensions differ from source geometry");
            check_layer_progress(progress, layer, result.layer_count());
            previous_source=bytes;
            bool changed=false;
            constexpr size_t chunk_size = size_t(1) << 20;
            for (size_t offset = 0; offset < image.buf.size();) {
                check_layer_progress(progress, offset, image.buf.size());
                const size_t end = std::min(image.buf.size(), offset + chunk_size);
                for (size_t i = offset; i < end; ++i) {
                    auto &value = image.buf[i];
                    const auto projected = project_mask_value(value, job.mask_precision);
                    changed = changed || projected != value;
                    value = projected;
                }
                offset = end;
            }
            check_layer_progress(progress, image.buf.size(), image.buf.size());
            if (changed) {
                const auto encoded=PNGRasterEncoder{}(image.buf.data(),image.cols,image.rows,1);
                const auto *begin=static_cast<const uint8_t *>(encoded.data());
                bytes.assign(begin,begin+encoded.size());
            }
            previous_projection=bytes;
        }
    }
    result.m_fresh_job=std::move(job);
    result.validate_and_capture_mask_statistics(progress);
    check_layer_progress(progress, result.layer_count(), result.layer_count());
    return result;
}

LayerDocument LayerDocument::from_fresh_rasters(std::vector<EncodedRaster> layers, FreshLayerJob job, const LayerProgress &progress)
{
    job.validate(layers.size(), progress);
    auto result = from_rasters(std::move(layers), progress);
    result.m_fresh_job = std::move(job);
    result.validate_and_capture_mask_statistics(progress);
    check_layer_progress(progress, result.layer_count(), result.layer_count());
    return result;
}

LayerDocument LayerDocument::load_sl1(const std::string &path, const LayerProgress &progress)
{
    auto document = load_archive(path, progress);
    if (document.archive_format() != "SL1") throw RuntimeError("This archive is not SL1");
    return document;
}

struct LayerDocument::ChituboxConversionReadback {
    const LayerDocument &source;
    const std::string &format;
    ExportGenerationGuard generation;
    std::vector<LayerMaskStatistics> statistics;

    void verify(size_t layer, const std::vector<uint8_t> &pixels,
        size_t columns, size_t rows, const LayerProgress &progress)
    {
        generation.check();
        if (layer != statistics.size())
            throw RuntimeError("Converted Chitubox mask verification is out of order");
        // This decoded source dies before the caller encodes the destination
        // PNG. Keep the same two-mask peak as the former separate comparison.
        const auto expected = source.image(layer);
        if (expected.cols != columns || expected.rows != rows || expected.buf != pixels)
            throw RuntimeError("Destination grayscale precision cannot preserve the source exposure pixels");
        size_t nonzero = 0;
        for (size_t offset = 0; offset < pixels.size();) {
            check_layer_progress(progress, layer, source.layer_count());
            const size_t count = std::min(pixels.size()-offset, size_t(1)<<20);
            nonzero += validate_and_count_mask_chunk(pixels.data()+offset,
                count, LayerMaskPrecision::Gray8).nonzero_pixels;
            offset += count;
        }
        generation.check();
        statistics.push_back({columns, rows, nonzero});
    }
};

LayerDocument LayerDocument::load_converted_chitubox(const std::string &path,
    const std::string &format, const LayerDocument &source,
    std::vector<LayerMaskStatistics> &statistics, const LayerProgress &progress)
{
    ChituboxConversionReadback readback{source, format, source.export_generation_guard(), {}};
    readback.statistics.reserve(source.layer_count());
    const auto guarded_progress = [&](size_t done, size_t total) {
        readback.generation.check();
        check_layer_progress(progress, done, total);
        readback.generation.check();
        return true;
    };
    auto result = load_archive_impl(path, guarded_progress, &readback);
    readback.generation.check();
    if (readback.statistics.size() != source.layer_count())
        throw RuntimeError("Converted Chitubox mask verification is incomplete");
    // Publish derived counts only after the complete editable document loads.
    statistics = std::move(readback.statistics);
    return result;
}

LayerDocument LayerDocument::load_archive(const std::string &path, const LayerProgress &progress)
{
    auto result = load_archive_impl(path, progress, nullptr);
    result.attach_native_parameters();
    result.spill_payloads();
    return result;
}

// Adds one parameter entry per layer for the families whose containers store
// per-layer values. A container that is not in the shape our writers produce
// is left without rows: its parameters stay read-only rather than guessed.
void LayerDocument::attach_native_parameters()
{
    if (native_family(m_archive_format) == NativeFamily::None || !m_layer_parameters.empty() || m_entries.empty() ||
        !m_secondary_layers.empty())
        return;
    const auto table = read_native_table(m_archive_format, native_original());
    if (!table || table->rows.size() != layer_count()) return;
    uint64_t stored = 0;
    for (const auto &entry : m_entries) stored += entry.bytes.size();
    for (size_t i = 0; i < table->rows.size(); ++i) {
        auto encoded = encode_parameters(table->rows[i]);
        stored += encoded.size();
        if (stored > (uint64_t(1) << 31)) throw RuntimeError("Layer parameters exceed the editor memory budget");
        m_layer_parameters.push_back(m_entries.size());
        m_entries.push_back({std::to_string(i) + "-parameters.json", std::move(encoded)});
    }
}

NativeBytes LayerDocument::native_original() const
{
    const auto &entry = m_entries.at(0);
    if (entry.stored) return NativeBytes(entry.stored.store, entry.stored.len);
    return NativeBytes(entry.bytes);
}

LayerDocument LayerDocument::load_archive_impl(const std::string &path,
    const LayerProgress &progress, ChituboxConversionReadback *readback)
{
    check_layer_progress(progress);
    auto read_bytes = [&](std::istream &stream, std::vector<uint8_t> &bytes) {
        for(size_t at=0;at<bytes.size();) {
            check_layer_progress(progress);
            const size_t length=std::min(size_t(1)<<20,bytes.size()-at);
            stream.read(reinterpret_cast<char *>(bytes.data()+at),length);
            if(!stream)throw FileIOError("Cannot read layer archive");at+=length;
        }
        check_layer_progress(progress);
    };
    // Binary containers are opened without reading them: the document references the file (entry 0 and
    // every layer's RLE) and produces a layer's PNG only when it is asked for.
    struct NativeOpen { std::shared_ptr<SourceFileStore> source; NativeBytes image; };
    auto open_native = [&]() {
        NativeOpen opened;
        opened.source = std::make_shared<SourceFileStore>(path);
        opened.image = NativeBytes(opened.source, opened.source->size());
        return opened;
    };
    auto lazy_layer = [](const std::shared_ptr<ByteStore> &derived, uint64_t number, const std::string &name,
                         const std::vector<uint8_t> &pixels, size_t columns, size_t rows) {
        Entry entry{name, {}};
        entry.stored = StoredBytes{derived, number, 0};
        entry.statistics = LayerMaskStatistics{columns, rows, size_t(std::count_if(pixels.begin(), pixels.end(), [](uint8_t v) { return v != 0; }))};
        return entry;
    };
    auto bind_native = [](LayerDocument &result, const NativeOpen &opened, std::shared_ptr<ByteStore> derived) {
        result.m_entries[0].stored = StoredBytes{opened.source, 0, opened.source->size()};
        result.m_source_store = opened.source;
        result.m_native_layers = std::move(derived);
    };
    // Generic open of a native layout (Format/*Layout.hpp over NativeBytes with format/count/columns/rows
    // and decode(layer)): the file is referenced, every layer is decoded once to validate it and count its
    // lit pixels, and its PNG is produced lazily from the layer's own record. `make(image, poll)` builds the
    // layout. Returns the finished document (entry 0 = the whole source file).
    auto open_native_layout = [&](const char *kind, std::streamoff minimum, std::istream &stream, auto make) {
        stream.clear(); stream.seekg(0, std::ios::end);
        check_archive_length(stream.tellg(), minimum, kind);
        const auto opened = open_native();
        const std::function<void()> poll = [&]{ check_layer_progress(progress); };
        using Layout = std::decay_t<decltype(make(opened.image, poll))>;
        const auto layout_ptr = std::make_shared<const Layout>(make(opened.image, poll));
        const auto &layout = *layout_ptr;
        LayerDocument result;
        result.m_archive_format = layout.format;
        result.m_entries.push_back({layout.format + "-original", {}});
        auto derived = derived_png_store(layout_ptr, std::string("Cannot encode imported ") + kind + " mask");
        for (size_t i = 0; i < layout.count; ++i) {
            check_layer_progress(progress, i, layout.count);
            const auto pixels = layout.decode(i);
            result.m_layers.push_back(result.m_entries.size());
            result.m_entries.push_back(lazy_layer(derived, i, std::to_string(i + 1) + ".png", pixels, layout.columns, layout.rows));
        }
        check_layer_progress(progress, layout.count, layout.count);
        bind_native(result, opened, derived);
        return result;
    };
    // Probe the contents: a standalone image is an editable mask, not a
    // printer container. Check dimensions before the PNG decoder allocates.
    boost::nowide::ifstream input(path, std::ios::binary);
    uint8_t signature[8]{};
    input.read(reinterpret_cast<char *>(signature), sizeof(signature));
    const uint32_t magic=uint32_t(signature[0])|(uint32_t(signature[1])<<8)|(uint32_t(signature[2])<<16)|(uint32_t(signature[3])<<24);
    if (readback && (input.gcount() != 8 || (magic != 0x12fd0086 && magic != 0x12fd0106)))
        throw RuntimeError("Converted container is not a plain Chitubox archive");
    if (input.gcount() == 8 && std::memcmp(signature, "ANYCUBIC", 8) == 0) {
        input.seekg(0, std::ios::end); const auto length = input.tellg();
        check_archive_length(length, 48, "Anycubic");
        const auto opened = open_native(); const auto &bytes = opened.image;
        std::string format = boost::filesystem::path(path).extension().string();
        if (!format.empty()) format.erase(0, 1);
        boost::algorithm::to_lower(format);
        const auto layout_ptr = std::make_shared<const AnycubicLayerLayout>(bytes, format, [&]{check_layer_progress(progress);});
        const auto &layout = *layout_ptr;
        LayerDocument result; result.m_archive_format = format;
        result.m_entries.push_back({"anycubic-original", {}});
        // Layer numbers: 0..count-1 main images, count..2*count-1 independent subimages.
        auto derived = std::make_shared<DerivedLayerStore>([layout_ptr](uint64_t number) {
            const auto &l = *layout_ptr;
            const bool sub = number >= l.count;
            const auto pixels = sub ? l.decode_subimage(number - l.count) : l.decode(number);
            const auto png = PNGRasterEncoder{}(pixels.data(), l.columns, l.rows, 1);
            if (!png.size()) throw RuntimeError("Anycubic masks exceed editor limits");
            const auto *data = static_cast<const uint8_t *>(png.data());
            return std::vector<uint8_t>(data, data + png.size());
        });
        double position_z=0;
        uint64_t stored = 0;
        for (size_t i = 0; i < layout.count; ++i) {
            check_layer_progress(progress, i, layout.count);
            const auto pixels = layout.decode(i, [&]{check_layer_progress(progress, i, layout.count);});
            std::vector<uint8_t> secondary;
            if (layout.subtable && !layout.subimage_is_shared(i)) {
                // Full Open in the reference ignores this independent image.
                // Validate its own RLE before publishing the main-layer view.
                secondary=layout.decode_subimage(i,[&]{check_layer_progress(progress,i,layout.count);});
                if (size_t(std::count_if(secondary.begin(),secondary.end(),[](uint8_t value){return value!=0;}))!=layout.integer(layout.subrecord(i)+8))
                    throw RuntimeError("Anycubic subimage pixel count does not match its mask");
            }
            result.m_layers.push_back(result.m_entries.size());
            result.m_entries.push_back(lazy_layer(derived, i, std::to_string(i)+".png", pixels, layout.columns, layout.rows));
            if (layout.subtable) {
                if (layout.subimage_is_shared(i)) result.m_secondary_layers.push_back(result.m_layers.back());
                else {
                    result.m_secondary_layers.push_back(result.m_entries.size());
                    result.m_entries.push_back(lazy_layer(derived, layout.count + i, std::to_string(i)+"-sub.png", secondary, layout.columns, layout.rows));
                }
            }
            position_z+=layout.number(layout.record(i)+20);
            const auto parameters=anycubic_layer_parameters(layout,i,position_z);
            auto encoded=encode_parameters(parameters);
            if (encoded.size()>layer_editor_byte_budget()-stored) throw RuntimeError("Anycubic parameters exceed editor budget");
            stored+=encoded.size();result.m_layer_parameters.push_back(result.m_entries.size());
            result.m_entries.push_back({std::to_string(i)+"-parameters.json",std::move(encoded)});
        }
        check_layer_progress(progress, layout.count, layout.count);
        bind_native(result, opened, derived); return result;
    }
    if (input.gcount()==8 && magic==0x12fd0107) {
        input.seekg(0,std::ios::end);const auto length=input.tellg();
        if(length<336)throw RuntimeError("CTBEncrypted file is truncated");
        const auto opened=open_native();const auto &bytes=opened.image;
        const auto layout_ptr=std::make_shared<const CTBEncryptedLayerLayout>(bytes,[&]{check_layer_progress(progress);});
        const auto &layout=*layout_ptr;
        LayerDocument result;result.m_archive_format=layout.version==4?"ctb_encrypted_v4":"ctb_encrypted";
        result.m_entries.push_back({"ctb-encrypted-original",{}});uint64_t stored=0;
        auto derived=derived_png_store(layout_ptr,"CTBEncrypted masks exceed editor budget");
        for(size_t i=0;i<layout.count;++i) {
            check_layer_progress(progress,i,layout.count);
            const auto pixels=layout.decode(i,[&]{check_layer_progress(progress,i,layout.count);});
            result.m_layers.push_back(result.m_entries.size());
            result.m_entries.push_back(lazy_layer(derived,i,std::to_string(i)+".png",pixels,layout.columns,layout.rows));
        }
        for(size_t i=0;i<layout.count;++i) {
            auto encoded=encode_parameters(ctb_encrypted_layer_parameters(layout,i));
            if(encoded.size()>layer_editor_byte_budget()-stored)throw RuntimeError("CTBEncrypted parameters exceed editor budget");
            stored+=encoded.size();result.m_layer_parameters.push_back(result.m_entries.size());
            result.m_entries.push_back({std::to_string(i)+"-parameters.json",std::move(encoded)});
        }
        check_layer_progress(progress,layout.count,layout.count);
        bind_native(result,opened,derived);return result;
    }
    if(input.gcount()==8 && (magic==0x12fd0019 || magic==0x12fd0086 || magic==0x12fd0106 || magic==0xff220810)) {
        input.seekg(0,std::ios::end);const auto length=input.tellg();
        check_archive_length(length, 112, "Chitubox");
        const auto opened=open_native();const auto &bytes=opened.image;
        const auto layout_ptr=std::make_shared<const ChituboxLayerLayout>(bytes,[&]{check_layer_progress(progress);});
        const auto &layout=*layout_ptr;LayerDocument result;
        const std::string family=layout.binary?(boost::algorithm::iends_with(path,".photon")?"photon":"cbddlp"):"ctb";
        result.m_archive_format = layout.magic == 0xff220810 ? "ctb_gktwo" :
            layout.version >= 3 ? "ctb_v" + std::to_string(layout.version) :
            family + (layout.version == 1 ? "_v1" : "");
        if (readback && (layout.binary || layout.version < 3 ||
            result.m_archive_format != readback->format || layout.count != readback->source.layer_count()))
            throw RuntimeError("Converted Chitubox format or layer count differs from the request");
        result.m_entries.push_back({"chitubox-original",{}});uint64_t stored=0;
        auto derived=derived_png_store(layout_ptr,"Cannot encode imported Chitubox mask");
        for(size_t i=0;i<layout.count;++i) {
            check_layer_progress(progress,i,layout.count);
            // Every layer's RLE is decoded once to validate it (and to compare a conversion's masks); the
            // PNG is produced when the layer is first used.
            const auto pixels=layout.decode(i,[&]{check_layer_progress(progress,i,layout.count);});
            if (readback) readback->verify(i, pixels, layout.columns, layout.rows, progress);
            result.m_layers.push_back(result.m_entries.size());
            result.m_entries.push_back(lazy_layer(derived,i,std::to_string(i)+".png",pixels,layout.columns,layout.rows));
        }
        if(layout.version>=3) for(size_t i=0;i<layout.count;++i) {
            auto encoded=encode_parameters(chitubox_layer_parameters(layout,i));
            if(encoded.size()>layer_editor_byte_budget()-stored)throw RuntimeError("Chitubox parameters exceed editor budget");
            stored+=encoded.size();result.m_layer_parameters.push_back(result.m_entries.size());
            result.m_entries.push_back({std::to_string(i)+"-parameters.json",std::move(encoded)});
        }
        check_layer_progress(progress,layout.count,layout.count);
        bind_native(result,opened,derived);return result;
    }
    const uint8_t cxdlpv1_signature[]{0,0,0,9,'C','X','S','W'};
    if(input.gcount()==8 && std::memcmp(signature,cxdlpv1_signature,8)==0) {
        input.seekg(0,std::ios::end);const auto length=input.tellg();
        check_archive_length(length, 19, "CXDLPv1");
        uint8_t head[15]{};
        input.seekg(0);input.read(reinterpret_cast<char *>(head),15);
        if(input.gcount()==15 && head[13]==0 && head[14]==4) {
            // CXDLPv4 is addressed: opened without reading the file (the checksum is verified while streaming it).
            const auto opened=open_native();
            const auto layout_ptr=std::make_shared<const CrealityCXDLPv4Layout>(opened.image);
            const auto &layout=*layout_ptr;
            LayerDocument result;result.m_archive_format="cxdlpv4";
            result.m_entries.push_back({"cxdlpv4-original",{}});
            auto derived=std::make_shared<DerivedLayerStore>([layout_ptr](uint64_t number) {
                const auto pixels=cxdlpv4_layer_image(*layout_ptr,number);
                const auto png=PNGRasterEncoder{}(pixels.buf.data(),layout_ptr->columns,layout_ptr->rows,1);
                if(!png.size())throw RuntimeError("Cannot encode imported CXDLPv4 mask");
                const auto *begin=static_cast<const uint8_t *>(png.data());return std::vector<uint8_t>(begin,begin+png.size());
            });
            for(size_t i=0;i<layout.layers.size();++i) {
                check_layer_progress(progress,i,layout.layers.size());
                const auto pixels=cxdlpv4_layer_image(layout,i,[&]{check_layer_progress(progress,i,layout.layers.size());});
                result.m_layers.push_back(result.m_entries.size());
                result.m_entries.push_back(lazy_layer(derived,i,std::to_string(i+1)+".png",pixels.buf,layout.columns,layout.rows));
            }
            check_layer_progress(progress,layout.layers.size(),layout.layers.size());
            bind_native(result,opened,derived);return result;
        }
        // CXDLP v1-v3: referenced like the other native binaries (the checksum of v2/v3 is verified while streaming).
        return open_native_layout("CXDLPv1", 19, input,
            [](const NativeBytes &image, const std::function<void()> &poll) { return CrealityCXDLPv1Layout(image, poll); });
    }
    PhrozenLayout::Kind phrozen_family = PhrozenLayout::Kind::Phz;
    if (input.gcount() == 8 && PhrozenLayout::kind_of_magic(magic, phrozen_family))
        return open_native_layout(PhrozenLayout::label(phrozen_family), PhrozenLayout::header_size, input,
            [&](const NativeBytes &image, const std::function<void()> &poll) { return PhrozenLayout(image, phrozen_family, poll); });
    // Binary families with a Format/*Layout.hpp reader: the whole file stays in
    // entry 0 (unchanged saves are byte-identical) and each mask is decoded once.
    auto import_binary = [&](const char *kind, auto open) {
        input.clear(); input.seekg(0, std::ios::end); const auto length = input.tellg();
        check_archive_length(length, 16, kind);
        std::vector<uint8_t> bytes(static_cast<size_t>(length)); input.seekg(0); read_bytes(input, bytes);
        const std::function<void()> poll = [&]{ check_layer_progress(progress); };
        const auto layout = open(bytes, poll);
        LayerDocument result;
        result.m_archive_format = layout.format;
        result.m_entries.push_back({layout.format + "-original", {}});
        uint64_t stored = bytes.size();
        for (size_t i = 0; i < layout.count; ++i) {
            check_layer_progress(progress, i, layout.count);
            const auto pixels = layout.decode(i);
            const auto png = PNGRasterEncoder{}(pixels.data(), layout.columns, layout.rows, 1);
            if (!png.size()) throw RuntimeError(std::string("Cannot encode imported ") + kind + " mask");
            if (png.size() > layer_editor_byte_budget() - stored) throw RuntimeError(std::string(kind) + " masks exceed editor limits");
            stored += png.size();
            const auto *data = static_cast<const uint8_t *>(png.data());
            result.m_layers.push_back(result.m_entries.size());
            result.m_entries.push_back({std::to_string(i + 1) + ".png", {data, data + png.size()}});
        }
        check_layer_progress(progress, layout.count, layout.count);
        result.m_entries[0].bytes = std::move(bytes);
        return result;
    };
    if (input.gcount() == 8 && LongerOrangeLayout::recognizes(signature))
        return open_native_layout("LGS", 16, input,
            [](const NativeBytes &image, const std::function<void()> &poll) { return LongerOrangeLayout(image, poll); });
    // OSF has no magic number, so its header pattern must be backed by the extension.
    if (input.gcount() == 8 && VlareLayout::recognizes(signature) &&
        boost::algorithm::iequals(boost::filesystem::path(path).extension().string(), ".osf"))
        return open_native_layout("OSF", 16, input,
            [](const NativeBytes &image, const std::function<void()> &poll) { return VlareLayout(image, poll); });
    // Anet has no magic number either: the extension names the machine and the version string confirms it.
    if (input.gcount() == 8 && AnetLayout::recognizes(signature)) {
        std::string extension = boost::filesystem::path(path).extension().string();
        boost::algorithm::to_lower(extension);
        if (extension == ".n4" || extension == ".n7")
            return open_native_layout("Anet", 16, input, [&](const NativeBytes &image, const std::function<void()> &poll) {
                return AnetLayout(image, extension.substr(1), poll);
            });
    }
    if (input.gcount() == 8 && OpenSlaLayout::recognizes(signature))
        return open_native_layout("OSLA", 16, input,
            [](const NativeBytes &image, const std::function<void()> &poll) { return OpenSlaLayout(image, poll); });
    if (input.gcount() == 8 && SvgxLayout::recognizes(signature))
        return import_binary("SVGX", [](const std::vector<uint8_t> &b, const std::function<void()> &poll) { return SvgxLayout(b, poll); });
    if (input.gcount() == 8 && Emake3DLayout::recognizes(signature))
        return open_native_layout("QDT", 16, input,
            [](const NativeBytes &image, const std::function<void()> &poll) { return Emake3DLayout(image, poll); });
    if (input.gcount() == 8 && MakerbaseLayout::recognizes(signature)) {
        // MDLP and GR1 share a magic; the extension says which one the file claims to be,
        // and the other reading is tried only when the extension names neither.
        std::string extension = boost::filesystem::path(path).extension().string();
        boost::algorithm::to_lower(extension);
        const auto first = extension == ".gr1" ? MakerbaseLayout::Kind::Gr1 : MakerbaseLayout::Kind::Mdlp;
        const auto second = first == MakerbaseLayout::Kind::Gr1 ? MakerbaseLayout::Kind::Mdlp : MakerbaseLayout::Kind::Gr1;
        const bool known = extension == ".gr1" || extension == ".mdlp";
        return open_native_layout(first == MakerbaseLayout::Kind::Gr1 ? "GR1" : "MDLP", 16, input,
            [&](const NativeBytes &image, const std::function<void()> &poll) {
                try { return MakerbaseLayout(image, first, poll); }
                catch (const RuntimeError &) { if (known) throw; }
                return MakerbaseLayout(image, second, poll);
            });
    }
    if (input.gcount() == 8 && is_goo_signature(signature)) {
        input.seekg(0, std::ios::end); const auto length = input.tellg();
        check_archive_length(length, goo_header_size + goo_footer_size, "GOO");
        const auto opened = open_native(); const auto &bytes = opened.image;
        const auto layout_ptr = std::make_shared<const GooLayout>(bytes, [&]{check_layer_progress(progress);});
        const auto &layout = *layout_ptr;
        LayerDocument result;
        result.m_archive_format = boost::algorithm::iends_with(path, ".prz") ? "prz" : "goo";
        result.m_entries.push_back({"goo-original", {}});
        auto derived = std::make_shared<DerivedLayerStore>([layout_ptr](uint64_t number) {
            const auto &l = *layout_ptr;
            std::vector<uint8_t> pixels(l.pixel_count());
            const auto rle = l.payload(number);
            decode_goo_layer(rle.data(), rle.size(), pixels.data(), pixels.size(), number);
            const auto png = PNGRasterEncoder{}(pixels.data(), l.columns, l.rows, 1);
            if (!png.size()) throw RuntimeError("Cannot encode imported GOO mask");
            const auto *begin = static_cast<const uint8_t *>(png.data());
            return std::vector<uint8_t>(begin, begin + png.size());
        });
        std::vector<uint8_t> pixels(layout.pixel_count());
        for (size_t i = 0; i < layout.layers.size(); ++i) {
            check_layer_progress(progress, i, layout.layers.size());
            const auto rle = layout.payload(i);
            decode_goo_layer(rle.data(), rle.size(), pixels.data(), pixels.size(), i);
            result.m_layers.push_back(result.m_entries.size());
            result.m_entries.push_back(lazy_layer(derived, i, std::to_string(i + 1) + ".png", pixels, layout.columns, layout.rows));
        }
        check_layer_progress(progress, layout.layers.size(), layout.layers.size());
        bind_native(result, opened, derived); return result;
    }
    if (input.gcount() == 8 && png::is_png({signature, sizeof(signature)})) {
        input.seekg(0, std::ios::end);
        const auto length = input.tellg();
        check_archive_length(length, 33, "PNG");
        std::vector<uint8_t> bytes(static_cast<size_t>(length));
        input.seekg(0);
        read_bytes(input,bytes);
        auto be32 = [&](size_t offset) {
            return (uint32_t(bytes[offset]) << 24) | (uint32_t(bytes[offset+1]) << 16) |
                   (uint32_t(bytes[offset+2]) << 8) | uint32_t(bytes[offset+3]);
        };
        if (be32(8) != 13 || std::memcmp(bytes.data()+12, "IHDR", 4) != 0)
            throw RuntimeError("PNG mask has no valid IHDR");
        bool image_data = false, image_end = false;
        for (size_t offset = 8; offset < bytes.size();) {
            check_layer_progress(progress,0,1);
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
        check_layer_progress(progress,1,1);return result;
    }
    // Other ImageFile extensions are decoded once and kept as lossless PNG
    // internally, so undo/redo never repeats a lossy JPEG conversion.
    if (const auto *format = find_layer_image_format(boost::filesystem::path(path).extension().string())) {
        input.clear(); input.seekg(0, std::ios::end);
        const auto length = input.tellg();
        if (length <= 0 || length > std::streamoff(layer_editor_byte_budget()))
            throw RuntimeError("Layer image exceeds the editor memory budget or is empty");
        std::vector<uint8_t> bytes(static_cast<size_t>(length));
        input.seekg(0); read_bytes(input,bytes);
        const auto image = decode_layer_image({bytes.data(), bytes.size()}, format->id);
        auto result = from_rasters({PNGRasterEncoder{}(image.buf.data(), image.cols, image.rows, 1)});
        result.m_archive_format = format->id;
        check_layer_progress(progress,1,1);return result;
    }
    // ZIP families that are kept whole in entry 0 and decoded layer by layer
    // (Anycubic ZIP: PW0 masks; Klipper: PNG masks that must be written back in
    // the file's own colour type) are recognised by their manifest entries.
    if (input.gcount() >= 2 && signature[0] == 'P' && signature[1] == 'K') {
        std::set<std::string> zip_names;
        // A damaged ZIP is reported by the generic importer, not guessed at here.
        const bool listed = zip_entry_names(path, zip_names);
        const bool anycubic_zip = listed && AnycubicZipLayout::recognizes(zip_names);
        const bool klipper_zip = listed && !anycubic_zip && KlipperLayout::recognizes(zip_names);
        const bool nanodlp_zip = listed && !anycubic_zip && !klipper_zip && NanoDLPLayout::recognizes(zip_names);
        const bool zcode_zip = listed && !anycubic_zip && !klipper_zip && !nanodlp_zip && ZCodeLayout::recognizes(zip_names, ZCodeLayout::Kind::ZCode);
        const bool zcodex_zip = listed && !anycubic_zip && !klipper_zip && !nanodlp_zip && !zcode_zip &&
                                ZCodeLayout::recognizes(zip_names, ZCodeLayout::Kind::ZCodex);
        const bool jxs_zip = listed && !anycubic_zip && !klipper_zip && !nanodlp_zip && !zcode_zip && !zcodex_zip &&
                             UniformationLayout::recognizes(zip_names);
        const bool vdt_zip = listed && !anycubic_zip && !klipper_zip && !nanodlp_zip && !zcode_zip && !zcodex_zip && !jxs_zip &&
                             VoxeldanceLayout::recognizes(zip_names);
        if (anycubic_zip || klipper_zip || nanodlp_zip || zcode_zip || zcodex_zip || jxs_zip || vdt_zip) {
            const char *kind = anycubic_zip ? "Anycubic ZIP" : klipper_zip ? "Klipper ZIP" : nanodlp_zip ? "NanoDLP ZIP" :
                               zcode_zip ? "ZCode" : zcodex_zip ? "ZCodex" : jxs_zip ? "JXS" : "VDT";
            input.clear(); input.seekg(0, std::ios::end); const auto length = input.tellg();
            check_archive_length(length, 22, kind);
            // The archive stays in its file: entry 0 references it and the layouts read through the store.
            const auto opened = open_native();
            std::string extension = boost::filesystem::path(path).extension().string();
            if (!extension.empty()) extension.erase(0, 1);
            boost::algorithm::to_lower(extension);
            LayerDocument result;
            // Entry 0 is the untouched file: an unchanged save copies it verbatim
            // and an edited save rewrites only the changed layer images.
            result.m_entries.push_back({anycubic_zip ? "anycubic-zip-original" : klipper_zip ? "klipper-zip-original" :
                                        nanodlp_zip ? "nanodlp-zip-original" : zcode_zip ? "zcode-original" :
                                        zcodex_zip ? "zcodex-original" :
                                        jxs_zip ? "jxs-original" : "vdt-original", {}});
            uint64_t resident_layers = 0;
            {
                const MemoryZip zip(opened.image, kind);
                auto import_layers = [&](const auto &layout) {
                    result.m_archive_format = layout.format;
                    for (size_t i = 0; i < layout.count; ++i) {
                        check_layer_progress(progress, i, layout.count);
                        const auto pixels = layout.decode(i);
                        const auto png = PNGRasterEncoder{}(pixels.data(), layout.columns, layout.rows, 1);
                        if (!png.size()) throw RuntimeError(std::string("Cannot encode imported ") + kind + " mask");
                        const auto *data = static_cast<const uint8_t *>(png.data());
                        result.m_layers.push_back(result.m_entries.size());
                        // Layer images beyond the resident payload budget go to the document's spill file (the saves read
                        // layers through image(), never the entry vector); the file itself stays whole in entry 0.
                        Entry entry{std::to_string(i) + ".png", {}};
                        result.store_payload(entry, std::vector<uint8_t>(data, data + png.size()), resident_layers);
                        if (resident_layers > layer_editor_byte_budget()) throw RuntimeError(std::string(kind) + " masks exceed editor limits");
                        result.m_entries.push_back(std::move(entry));
                    }
                };
                if (anycubic_zip) import_layers(AnycubicZipLayout(zip, extension));
                else if (klipper_zip) import_layers(KlipperLayout(zip));
                else if (nanodlp_zip) import_layers(NanoDLPLayout(zip));
                else if (jxs_zip) import_layers(UniformationLayout(zip));
                else if (vdt_zip) import_layers(VoxeldanceLayout(zip));
                else import_layers(ZCodeLayout(zip, zcode_zip ? ZCodeLayout::Kind::ZCode : ZCodeLayout::Kind::ZCodex));
            }
            check_layer_progress(progress, result.m_layers.size(), result.m_layers.size());
            result.m_entries[0].stored = StoredBytes{opened.source, 0, opened.source->size()};
            result.m_source_store = opened.source;
            return result;
        }
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
    // `limit` bounds the RAM held by archive entries; SL1-style layer images beyond the resident
    // payload budget go to the document's spill file (or, STORED, stay in the source archive).
    const uint64_t limit = layer_editor_byte_budget();
    uint64_t total = 0, resident_layers = 0;
    std::shared_ptr<SourceFileStore> source;
    // Name-based pre-pass: which family's layer PNGs may leave RAM. It mirrors the precedence of the
    // schema decision after the loop (SL1, UVJ, Generic, CWS manifest, NovaMaker, Chitubox, VDA).
    enum class FamilySpill { None, Uvj, Cws, NovaMaker, Vda, Generic, Chitubox } family_spill = FamilySpill::None;
    {
        std::set<std::string> lowers;
        bool sl1_layer = false, any_xml = false;
        for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&zip.arch); ++i) {
            mz_zip_archive_file_stat stat;
            if (!mz_zip_reader_file_stat(&zip.arch, i, &stat) || mz_zip_reader_is_file_a_directory(&zip.arch, i)) continue;
            const auto lower = boost::algorithm::to_lower_copy(std::string(stat.m_filename));
            lowers.insert(lower);
            any_xml |= boost::algorithm::ends_with(lower, ".xml");
            if (lower.find('/') == std::string::npos && lower.find('\\') == std::string::npos && boost::algorithm::ends_with(lower, ".png")) {
                size_t digits = 0;
                for (size_t p = lower.size()-4; p > 0 && lower[p-1] >= '0' && lower[p-1] <= '9'; --p) ++digits;
                sl1_layer |= digits >= 5;
            }
        }
        const auto has = [&](const char *n) { return lowers.count(n) != 0; };
        if (!(has("config.ini") && has("prusaslicer.ini") && sl1_layer)) {
            if (has("config.json")) family_spill = FamilySpill::Uvj;
            else if (has("manifest.uvtools")) family_spill = FamilySpill::Generic;
            else if (has("manifest.xml")) family_spill = FamilySpill::Cws;
            else if (has("slice.conf")) family_spill = FamilySpill::NovaMaker;
            else if (has("run.gcode")) family_spill = FamilySpill::Chitubox;
            else if (any_xml) family_spill = FamilySpill::Vda;
        }
    }
    const auto family_layer_image = [&](const std::string &lower) {
        if (!boost::algorithm::ends_with(lower, ".png")) return false;
        switch (family_spill) {
        case FamilySpill::Uvj: // slice/NNNNNNNN.png
            return lower.size() == 18 && lower.compare(0, 6, "slice/") == 0 &&
                   std::all_of(lower.begin()+6, lower.begin()+14, [](char c) { return c >= '0' && c <= '9'; });
        case FamilySpill::Cws: case FamilySpill::Vda: case FamilySpill::Generic: case FamilySpill::Chitubox: return true;
        case FamilySpill::NovaMaker: { // top-level, numbered (other assets are preserved untouched)
            if (lower.find('/') != std::string::npos || lower.find('\\') != std::string::npos) return false;
            return lower.size() > 4 && lower[lower.size()-5] >= '0' && lower[lower.size()-5] <= '9';
        }
        default: return false;
        }
    };
    for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&zip.arch); ++i) {
        check_layer_progress(progress,i,mz_zip_reader_get_num_files(&zip.arch));
        mz_zip_archive_file_stat stat;
        if (!mz_zip_reader_file_stat(&zip.arch, i, &stat)) throw FileIOError(zip.get_errorstr());
        if (mz_zip_reader_is_file_a_directory(&zip.arch, i)) continue;
        std::string name = stat.m_filename, lower = boost::algorithm::to_lower_copy(name);
        const bool duplicate = !names.insert(lower).second;
        // SL1 layers use a top-level prefix followed by a zero-padded index.
        // Keep other PNG assets intact rather than exposing them as print layers.
        bool is_layer = lower.find('/') == std::string::npos && lower.find('\\') == std::string::npos &&
                        boost::algorithm::ends_with(lower, ".png");
        size_t digits = 0;
        if (is_layer)
            for (size_t p = lower.size()-4; p > 0 && lower[p-1] >= '0' && lower[p-1] <= '9'; --p) ++digits;
        // Once config.ini and prusaslicer.ini were seen the SL1 schema wins over every other container
        // kind, so its layer images can leave RAM while the archive is still being read.
        const bool spillable = !duplicate && ((config && profile && is_layer && digits >= 5) || family_layer_image(lower));
        Entry entry{name, {}};
        struct Extract {
            std::vector<uint8_t> &data;const LayerProgress &progress;size_t index,total;bool cancelled=false;
            std::exception_ptr error;
        };
        auto extract_into = [&](std::vector<uint8_t> &data) {
            data.assign(size_t(stat.m_uncomp_size), 0);
            Extract extract{data,progress,i,mz_zip_reader_get_num_files(&zip.arch),false,{}};
            // Never throw through miniz's C callback. Abort extraction first,
            // then propagate cancellation/errors with normal C++ cleanup.
            auto copy=[](void *opaque,mz_uint64 at,const void *chunk,size_t length)->size_t {
                auto &state=*static_cast<Extract *>(opaque);
                try {
                    if(state.progress && !state.progress(state.index,state.total)){state.cancelled=true;return 0;}
                    if(at>state.data.size() || length>state.data.size()-at)throw RuntimeError("ZIP extraction exceeds the entry bounds");
                    if(length)std::memcpy(state.data.data()+size_t(at),chunk,length);return length;
                } catch(...){state.error=std::current_exception();return 0;}
            };
            const bool extracted=mz_zip_reader_extract_to_callback(&zip.arch,i,copy,&extract,0);
            if(extract.error)std::rethrow_exception(extract.error);
            if(extract.cancelled)throw LayerOperationCancelled{};
            if(!extracted)throw FileIOError(zip.get_errorstr());
        };
        if (spillable) {
            bool referenced = false;
            // STORED entries are already uncompressed at rest: reference them in the source file.
            if (stat.m_method == 0 && stat.m_comp_size == stat.m_uncomp_size && stat.m_uncomp_size > 0) {
                try {
                    if (!source) source = std::make_shared<SourceFileStore>(path);
                    const auto header = source->read(uint64_t(stat.m_local_header_ofs), 30);
                    const auto u16 = [&](size_t at) { return size_t(header[at]) | (size_t(header[at+1]) << 8); };
                    const bool local = header[0] == 'P' && header[1] == 'K' && header[2] == 3 && header[3] == 4;
                    if (local && !(u16(6) & 1)) {
                        const uint64_t at = uint64_t(stat.m_local_header_ofs) + 30 + u16(26) + u16(28);
                        if (at + stat.m_comp_size <= source->size()) {
                            entry.stored = StoredBytes{source, at, uint64_t(stat.m_comp_size)};
                            referenced = true;
                            result.m_source_store = source;
                        }
                    }
                } catch (const RuntimeError &) { referenced = false; }
            }
            if (!referenced) {
                std::vector<uint8_t> data;
                extract_into(data);
                result.store_payload(entry, std::move(data), resident_layers);
                if (resident_layers > limit) throw RuntimeError("Layer editor archive exceeds the editor memory budget");
            }
            // Only top-level numbered masks were ever pre-listed here; the family code rebuilds m_layers.
            if (is_layer && digits >= 5) result.m_layers.push_back(result.m_entries.size());
            result.m_entries.push_back(std::move(entry));
            continue;
        }
        if (stat.m_uncomp_size > limit - total) throw RuntimeError("Layer editor archive exceeds the editor memory budget");
        total += stat.m_uncomp_size;
        extract_into(entry.bytes);
        if (duplicate) {
            // Older exports repeated identical thumbnail sizes. Accept only
            // byte-identical preview assets; ambiguous masks/settings still fail.
            const auto previous=std::find_if(result.m_entries.begin(),result.m_entries.end(),
                [&](const Entry &other){return boost::algorithm::iequals(other.name,name);});
            if(boost::algorithm::starts_with(lower,"thumbnail/") && previous!=result.m_entries.end() &&
               previous->same_as(entry.bytes)) {total-=stat.m_uncomp_size;continue;}
            throw RuntimeError("Duplicate layer archive entry");
        }
        config |= lower == "config.ini";
        profile |= lower == "prusaslicer.ini";
        if (is_layer && digits >= 5)
            result.m_layers.push_back(result.m_entries.size());
        result.m_entries.push_back(std::move(entry));
    }
    // First `count` bytes of an entry without loading a spilled or source-referenced payload whole.
    const auto peek = [](const Entry &entry, size_t count) {
        if (entry.stored) return entry.stored.store->read(entry.stored.at, std::min<uint64_t>(count, entry.stored.len));
        return std::vector<uint8_t>(entry.bytes.begin(), entry.bytes.begin() + std::min<size_t>(count, entry.bytes.size()));
    };
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
            check_layer_progress(progress,i,result.m_entries.size());
            const auto &entry = result.m_entries[i];
            const boost::filesystem::path path(entry.name);
            if (!boost::algorithm::iequals(path.extension().string(), ".png")) continue;
            const auto stem = path.stem().string();
            if (stem.empty() || !std::all_of(stem.begin(), stem.end(), [](char c) { return c >= '0' && c <= '9'; })) continue;
            if (stem.size() > 10) throw RuntimeError("Invalid Generic ZIP mask index");
            const auto index = std::stoull(stem);
            if (!index || index > 100000 || !indices.emplace(size_t(index), i).second)
                throw RuntimeError("Invalid or duplicate Generic ZIP mask index");
            const auto bytes = peek(entry, 29);
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
            check_layer_progress(progress);
            if (slice.first == "<xmlattr>") continue;
            if (slice.first != "Slice") throw RuntimeError("Invalid CWS slice table");
            const auto index = find_entry(slice.second.get<std::string>("name"));
            if (!referenced.insert(index).second) throw RuntimeError("Duplicate CWS layer reference");
            auto &layer_entry = result.m_entries[index];
            const auto bytes = peek(layer_entry, 29);
            if (bytes.size() < 29 || !png::is_png({bytes.data(), bytes.size()})) throw RuntimeError("CWS layer is not PNG");
            auto dimension = [&](size_t offset) {
                return (uint32_t(bytes[offset]) << 24) | (uint32_t(bytes[offset+1]) << 16) |
                       (uint32_t(bytes[offset+2]) << 8) | bytes[offset+3];
            };
            const bool xml_rgba=boost::algorithm::iends_with(path,".xml.cws") && bytes[25]==6;
            if (dimension(16) != columns || dimension(20) != rows || bytes[24] != 8 || (bytes[25] != 0 && !xml_rgba))
                throw RuntimeError("CWS masks must match the profile: 8-bit grayscale, or RGBA for XML.CWS");
            if(xml_rgba) {
                // One layer at a time: decode, keep only the gray PNG (it may itself leave RAM).
                png::ImageGreyscale gray;
                {
                    const auto full=layer_entry.load();
                    if(!png::decode_color_png({full->data(),full->size()},gray) || gray.cols!=size_t(columns) || gray.rows!=size_t(rows))
                        throw RuntimeError("Invalid XML.CWS RGBA mask");
                }
                const auto encoded=PNGRasterEncoder{}(gray.buf.data(),gray.cols,gray.rows,1);
                if(!encoded.size())throw RuntimeError("Cannot normalize XML.CWS mask");
                const auto *data=static_cast<const uint8_t *>(encoded.data());
                result.store_payload(layer_entry,std::vector<uint8_t>(data,data+encoded.size()),resident_layers);
                if(resident_layers>limit)throw RuntimeError("XML.CWS exceeds the editor memory budget");
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
            check_layer_progress(progress,i,result.m_entries.size());
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
            const auto bytes=peek(entry,29);
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
            const auto full=entry.load();
            if(packed) {
                if(!decode_bgr_packed_mask({full->data(),full->size()},columns,rows,validated))
                    throw RuntimeError("Invalid RGB.CWS packed mask");
                const auto encoded=PNGRasterEncoder{}(validated.buf.data(),columns,rows,1);
                if(!encoded.size())throw RuntimeError("Cannot normalize RGB.CWS mask");
                const auto *data=static_cast<const uint8_t *>(encoded.data());
                result.store_payload(entry,std::vector<uint8_t>(data,data+encoded.size()),resident_layers);
                if(resident_layers>limit)throw RuntimeError("RGB.CWS exceeds the editor memory budget");
            } else if(!png::decode_png({full->data(),full->size()},validated))throw RuntimeError("Invalid NovaMaker PNG mask");
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
            check_layer_progress(progress,i,result.m_entries.size());
            const boost::filesystem::path name(result.m_entries[i].name);
            const auto stem=name.stem().string();
            if(!boost::algorithm::iequals(name.extension().string(),".png") || stem.empty() ||
                !std::all_of(stem.begin(),stem.end(),[](char c){return c>='0' && c<='9';})) continue;
            if(stem.size()>6) throw RuntimeError("Chitubox ZIP layer index exceeds the editor limit");
            const size_t index=std::stoul(stem);
            if(!index || index>size_t(count) || !indices.emplace(index,i).second) throw RuntimeError("Invalid Chitubox ZIP layer index");
            const auto bytes=peek(result.m_entries[i],29);
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
            check_layer_progress(progress);
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
            const auto bytes = peek(*found, 29);
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
            check_layer_progress(progress,i,result.m_entries.size());
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
            const auto bytes = peek(entry, 29);
            if (bytes.size() < 29 || !png::is_png({bytes.data(), bytes.size()})) throw RuntimeError("UVJ layer is not PNG");
            auto dimension = [&](size_t offset) { return (uint32_t(bytes[offset]) << 24) | (uint32_t(bytes[offset+1]) << 16) |
                (uint32_t(bytes[offset+2]) << 8) | uint32_t(bytes[offset+3]); };
            if (dimension(16) != columns || dimension(20) != rows || bytes[24] != 8 || bytes[25] != 0)
                throw RuntimeError("UVJ masks must match metadata and use 8-bit grayscale PNG");
        }
        if (metadata.contains("Layers") && !metadata["Layers"].is_null()) {
            if (!metadata["Layers"].is_array() || metadata["Layers"].size() != count) throw RuntimeError("UVJ parameter table count mismatch");
            ExposurePositionValidator positions(ExposurePositionPolicy::RepeatedExposureHeights);
            for (const auto &layer : metadata["Layers"]) {
                const double z = layer.at("Z").template get<double>();
                positions.append(z);
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
    check_layer_progress(progress);result.image(0);
    check_layer_progress(progress,result.layer_count(),result.layer_count());return result;
}

void LayerDocument::validate_and_capture_mask_statistics(const LayerProgress &progress)
{
    // Reuse counts derived during mandatory palette validation. The SSE2
    // implementation is measured; other targets keep the old path until
    // their fused scan has been profiled. Neither path relaxes validation.
    const char *flag = std::getenv("SLA_EXPERIMENT_VALIDATED_MASK_STATS");
#if defined(__SSE2__)
    constexpr bool default_reuse = true;
#else
    constexpr bool default_reuse = false;
#endif
    const bool reuse = flag ? std::strcmp(flag, "1") == 0 : default_reuse;
    const bool profile = std::getenv("SLA_PROFILE_MASK_STATISTICS") != nullptr;
    const auto started = profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    std::vector<std::optional<LayerMaskStatistics>> derived;
    validate_persisted_state(progress, reuse ? &derived : nullptr);
    // Publish only after all masks, palettes and metadata have passed. The
    // factory still owns this unpublished document if cancellation throws.
    check_layer_progress(progress, layer_count(), layer_count());
    size_t cached = 0;
    for (size_t i = 0; i < derived.size(); ++i) {
        m_entries[i].statistics = derived[i];
        cached += bool(derived[i]);
    }
    if (profile)
        BOOST_LOG_TRIVIAL(info) << "SLA mask validation: reuse=" << reuse << "; layers=" << layer_count()
            << "; cached=" << cached << "; seconds="
            << std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
}

void LayerDocument::validate_persisted_state(const LayerProgress &progress,
    std::vector<std::optional<LayerMaskStatistics>> *derived) const
{
    check_layer_progress(progress, 0, layer_count());
    if (m_layers.empty() || m_layers.size()>100000 || m_entries.size()>500000)
        throw RuntimeError("Invalid persisted document size");
    if (derived) derived->assign(m_entries.size(), std::nullopt);
    if (!m_archive_format.empty() && !get_archive_entry(m_archive_format.c_str()) &&
        !find_layer_image_format(m_archive_format))
        throw RuntimeError("Unknown persisted document format");
    std::set<size_t> used;
    for (const auto entry:m_layers)
        if (entry>=m_entries.size() || !used.insert(entry).second)
            throw RuntimeError("Invalid persisted main mask index");
    if (!m_secondary_layers.empty() && m_secondary_layers.size()!=layer_count())
        throw RuntimeError("Invalid persisted secondary mask count");
    for (size_t i=0;i<m_secondary_layers.size();++i) {
        const auto entry=m_secondary_layers[i];
        if (entry>=m_entries.size() || (entry!=m_layers[i] && !used.insert(entry).second))
            throw RuntimeError("Invalid persisted secondary mask relationship");
    }
    const bool anycubic=!anycubic_available_versions(m_archive_format).empty();
    const bool chitubox=is_chitubox_layer_format(m_archive_format);
    const bool encrypted=is_ctb_encrypted_layer_format(m_archive_format);
    const auto native=native_family(m_archive_format);
    // A native-family document may also have no rows (container not in our writer's shape).
    // Chitubox v1/v2 containers (CBDDLP, Photon, v1/v2 CTB) carry no per-layer motion table, so the
    // loader never creates parameter rows for them; such documents are legitimately read-only.
    const bool legacy_chitubox = chitubox && m_layer_parameters.empty() && !m_entries.empty() &&
        !native_original().empty() && !used.count(0) && ChituboxLayerLayout(native_original()).version < 3;
    if ((native!=NativeFamily::None && m_layer_parameters.empty()) || legacy_chitubox) {
        // read-only document: nothing to validate beyond the shared checks
    } else if (anycubic || chitubox || encrypted || native!=NativeFamily::None) {
        if (used.count(0) || m_layer_parameters.size()!=layer_count())
            throw RuntimeError("Missing persisted native source or parameters");
    } else if (!m_layer_parameters.empty())
        throw RuntimeError("Unexpected persisted layer parameters");
    for (const auto entry:m_layer_parameters)
        if (!entry || entry>=m_entries.size() || !used.insert(entry).second)
            throw RuntimeError("Invalid persisted parameter index");
    if (m_mask_precision) {
        if (*m_mask_precision!=LayerMaskPrecision::NativeGray4 && *m_mask_precision!=LayerMaskPrecision::NativeBitplanes1)
            throw RuntimeError("Unsupported independent source mask precision");
        if (m_fresh_job || !m_archive_format.empty() || !m_layer_parameters.empty() || !m_secondary_layers.empty())
            throw RuntimeError("Independent mask precision cannot shadow native or fresh intentions");
    }
    if (m_fresh_job || m_mask_precision) {
        if (!m_archive_format.empty() || !m_layer_parameters.empty() || !m_secondary_layers.empty())
            throw RuntimeError("Fresh intentions cannot impersonate a native archive or parameter table");
        if (m_fresh_job) m_fresh_job->validate(layer_count(), progress);
        const auto precision=m_fresh_job ? m_fresh_job->mask_precision : *m_mask_precision;
        size_t columns=m_fresh_job ? m_fresh_job->geometry.columns : 0;
        size_t rows=m_fresh_job ? m_fresh_job->geometry.rows : 0;
        std::unordered_map<std::string_view,LayerMaskStatistics> checked_palettes;
        // Check every PNG header without decoding all full-resolution masks.
        // Project loading/publication separately checks complete CRC/decode.
        for (size_t layer = 0; layer < layer_count(); ++layer) {
            check_layer_progress(progress, layer, layer_count());
            const auto bytes = encoded_layer(layer);
            if (bytes.sz < 33 || !png::is_png(bytes)) throw RuntimeError("Source masks must be grayscale PNG rasters");
            const auto *data = static_cast<const uint8_t *>(bytes.buf);
            auto word = [&](size_t offset) {
                return (uint32_t(data[offset]) << 24) | (uint32_t(data[offset+1]) << 16) |
                    (uint32_t(data[offset+2]) << 8) | data[offset+3];
            };
            if (!columns) {columns=word(16);rows=word(20);}
            if (!columns || !rows || columns>32768 || rows>32768 || columns*rows>128*1024*1024 ||
                word(8) != 13 || std::memcmp(data+12,"IHDR",4) != 0 || data[24] != 8 || data[25] != 0 ||
                word(16) != columns || word(20) != rows)
                throw RuntimeError("Source mask dimensions differ from the declared raster geometry");
            if (precision != LayerMaskPrecision::Gray8) {
                const std::string_view key(reinterpret_cast<const char *>(data),bytes.sz);
                const auto found = checked_palettes.find(key);
                if (found != checked_palettes.end()) {
                    if (derived) (*derived)[m_layers[layer]] = found->second;
                    continue;
                }
                const auto mask=image(layer);
                LayerMaskStatistics statistics{mask.cols, mask.rows, 0};
                constexpr size_t chunk_size = size_t(1) << 20;
                for (size_t offset = 0; offset < mask.buf.size();) {
                    check_layer_progress(progress, offset, mask.buf.size());
                    const size_t end = std::min(mask.buf.size(), offset + chunk_size);
                    if (derived) {
                        const auto checked = validate_and_count_mask_chunk(mask.buf.data() + offset, end - offset, precision);
                        if (!checked.valid) throw RuntimeError("Source mask pixels violate declared precision");
                        statistics.nonzero_pixels += checked.nonzero_pixels;
                    } else if (!mask_chunk_matches_precision(mask.buf.data() + offset, end - offset, precision))
                        throw RuntimeError("Source mask pixels violate declared precision");
                    offset = end;
                }
                check_layer_progress(progress, mask.buf.size(), mask.buf.size());
                if (derived) (*derived)[m_layers[layer]] = statistics;
                if (checked_palettes.size()<1024) checked_palettes.emplace(key,statistics);
            }
        }
    }
    if (m_layer_parameters.empty()) return;
    // Parse the immutable source independently of the stored parameter JSON.
    std::vector<LayerParameters> originals;
    double height_limit=0;
    const auto raster=image(0);
    auto geometry=[&](size_t columns,size_t rows,size_t count) {
        if(count!=layer_count() || columns!=raster.cols || rows!=raster.rows)
            throw RuntimeError("Persisted native count or raster geometry mismatch");
    };
    if(native!=NativeFamily::None) {
        if(m_entries.empty() || native_original().empty() || !m_secondary_layers.empty())
            throw RuntimeError("Missing persisted native source or parameters");
        const auto source=read_native_table(m_archive_format,native_original());
        if(!source || source->rows.size()!=layer_count())
            throw RuntimeError("Persisted native parameter table does not match its source container");
        originals=source->rows;height_limit=source->height_limit;
    } else if(encrypted) {
        const CTBEncryptedLayerLayout layout(native_original());
        geometry(layout.columns,layout.rows,layout.count);
        if(m_archive_format!=(layout.version==4?"ctb_encrypted_v4":"ctb_encrypted") || !m_secondary_layers.empty())
            throw RuntimeError("Persisted encrypted format or planes mismatch");
        height_limit=layout.parameter(20);
        for(size_t i=0;i<layer_count();++i)originals.push_back(ctb_encrypted_layer_parameters(layout,i));
    } else if(chitubox) {
        const ChituboxLayerLayout layout(native_original());
        geometry(layout.columns,layout.rows,layout.count);
        const std::string format=layout.magic==0xff220810?"ctb_gktwo":"ctb_v"+std::to_string(layout.version);
        if(m_archive_format!=format || !m_secondary_layers.empty())
            throw RuntimeError("Persisted Chitubox format or planes mismatch");
        height_limit=layout.number(16);
        for(size_t i=0;i<layer_count();++i)originals.push_back(chitubox_layer_parameters(layout,i));
    } else {
        const AnycubicLayerLayout layout(native_original(),m_archive_format);
        geometry(layout.columns,layout.rows,layout.count);
        // The native writer preserves the source plane graph. A forged graph
        // would otherwise display a secondary edit that cannot be saved.
        if(bool(layout.subtable)!=!m_secondary_layers.empty())
            throw RuntimeError("Persisted Anycubic secondary mask count mismatch");
        if(layout.subtable)
            for(size_t i=0;i<layer_count();++i)
                if(mask_is_shared(i)!=layout.subimage_is_shared(i))
                    throw RuntimeError("Persisted Anycubic mask sharing mismatch");
        if(layout.version>=516)height_limit=layout.number(layout.integer(44)+144);
        else if(const auto *known=anycubic_legacy_geometry(m_archive_format))height_limit=known->machine_z;
        double z=0;
        for(size_t i=0;i<layer_count();++i){z+=layout.number(layout.record(i)+20);originals.push_back(anycubic_layer_parameters(layout,i,z));}
    }
    const auto capabilities=parameter_capabilities();double previous=0;
    for(size_t i=0;i<layer_count();++i) {
        const auto values=layer_parameters(i);
        if(values.size()!=originals[i].size())throw RuntimeError("Persisted parameter fields mismatch");
        for(const auto &[field,value]:values) {
            const auto cap=std::find_if(capabilities.begin(),capabilities.end(),[&](const auto &c){return c.field==field;});
            if(cap==capabilities.end() || !originals[i].count(field) || !std::isfinite(value) || value<0 ||
               value>std::numeric_limits<float>::max() || (!cap->editable && value!=originals[i].at(field)))
                throw RuntimeError("Invalid persisted layer parameter");
            // Authored snapshots contain the canonical values already normalized
            // by the editor. Reject values that native save would silently round.
            // Unedited source Z may be an accumulated relative float sum, which
            // is retained exactly instead of being rounded to an absolute float.
            if(cap->editable && value!=originals[i].at(field)) {
                const double canonical=native!=NativeFamily::None?native_canonical(native,field,value):
                    anycubic && field==LayerParameter::LiftSpeed?
                    double(float(value/60.))*60.:
                    !anycubic && field==LayerParameter::LiftDistance && values.count(LayerParameter::LiftDistance2)?
                    double(float(value+values.at(LayerParameter::LiftDistance2)))-values.at(LayerParameter::LiftDistance2):
                    double(float(value));
                if(canonical!=value)throw RuntimeError("Persisted parameter is not canonical for its native field");
            }

        }
        const double z=values.at(LayerParameter::PositionZ);
        if(z<=0 || z<previous || (z==previous && m_archive_format!="uvj"))
            throw RuntimeError("Persisted layer Z must remain positive and ordered");
        if(std::isfinite(height_limit) && height_limit>0 && !layer_z_within_native_limit(z,height_limit))
            throw RuntimeError("Persisted layer Z exceeds the printer height");
        previous=z;
        if(native!=NativeFamily::None)validate_native_row(native,values,previous);
        else if(anycubic)validate_anycubic_motion(values);else validate_chitubox_parameters(values);
    }
}

std::optional<LayerRasterGeometry> LayerDocument::raster_geometry() const
{
    std::optional<LayerRasterGeometry> result;
    if (is_chitubox_layer_format(m_archive_format)) {
        const ChituboxLayerLayout layout(native_original());
        result = LayerRasterGeometry{layout.columns, layout.rows, layout.number(8), layout.number(12)};
    } else if (is_ctb_encrypted_layer_format(m_archive_format)) {
        const CTBEncryptedLayerLayout layout(native_original());
        result = LayerRasterGeometry{layout.columns, layout.rows, layout.parameter(12), layout.parameter(16)};
    } else if (!anycubic_available_versions(m_archive_format).empty()) {
        const AnycubicLayerLayout layout(native_original(), m_archive_format);
        if (const auto physical = layout.physical_dimensions())
            result = LayerRasterGeometry{layout.columns, layout.rows, physical->first, physical->second};
    }
    if (result) result->validate();
    return result;
}

std::optional<CXDLPv4NativeJobSnapshot> LayerDocument::cxdlpv4_native_job() const
{
    if (m_archive_format != "cxdlpv4") return std::nullopt;
    const CrealityCXDLPv4Layout layout(native_original(),false);
    return cxdlpv4_native_globals(layout);
}

std::optional<CXDLPv4ReferenceBottomView> LayerDocument::cxdlpv4_reference_bottom(const LayerProgress &progress) const
{
    if (m_archive_format != "cxdlpv4") return std::nullopt;
    check_layer_progress(progress);
    // Validate the original native container once, while reading current masks
    // from canonical entries. No per-field CRC passes or stale mask cache.
    const CrealityCXDLPv4Layout layout(native_original(),false);
    return cxdlpv4_reference_bottom_view(layout, [&](size_t i) { return image(i); }, progress);
}

std::optional<LayerJobParameters> LayerDocument::job_parameters() const
{
    if (m_archive_format == "ctb_v3" || m_archive_format == "ctb_v4" || m_archive_format == "ctb_v5") {
        const ChituboxLayerLayout layout(native_original());
        if (m_archive_format != "ctb_v" + std::to_string(layout.version))
            throw RuntimeError("CTB source version mismatch");
        return chitubox_job_parameters(layout);
    }

    if (is_ctb_encrypted_layer_format(m_archive_format)) {
        using F = LayerParameter;
        LayerJobParameters actual;
        uint8_t flag = 0; unsigned enabled = 0;
        const CTBEncryptedLayerLayout layout(native_original());
        flag = layout.settings.at(171); enabled = 0x40;
        if (layout.parameter(180) != layout.parameter(216) ||
            layout.parameter(156) != layout.parameter(220) || layout.parameter(184) != layout.parameter(220))
            throw RuntimeError("Conflicting duplicate AES global waits");
        actual.layer_height = layout.parameter(36);
        actual.bottom_layers = layout.setting(52); actual.transition_layers = layout.setting(188);
        for (bool bottom : {false, true}) {
            auto &row = bottom ? actual.bottom : actual.normal;
            row = {{F::Exposure, layout.parameter(bottom ? 44 : 40)},
                {F::LiftDistance, double(layout.parameter(bottom ? 84 : 92)) - layout.parameter(bottom ? 132 : 140)},
                {F::LiftDistance2, layout.parameter(bottom ? 132 : 140)},
                {F::LiftSpeed, layout.parameter(bottom ? 88 : 96)}, {F::LiftSpeed2, layout.parameter(bottom ? 136 : 144)},
                {F::RetractSpeed, layout.parameter(bottom ? 192 : 100)}, {F::RetractSpeed2, layout.parameter(bottom ? 196 : 152)},
                {F::RetractDistance2, layout.parameter(bottom ? 228 : 148)},
                {F::WaitBefore, layout.parameter(216)}, {F::WaitAfter, layout.parameter(224)},
                {F::WaitAfterLift, layout.parameter(220)}, {F::LightOff, layout.parameter(bottom ? 116 : 48)},
                {F::LightPWM, double(uint16_t(layout.settings.at(bottom ? 126 : 124)) |
                    (uint16_t(layout.settings.at(bottom ? 127 : 125)) << 8))}};
        }
        validate_chitubox_job(actual, m_layers.size());
        if (flag != 0 && flag != enabled)
            throw RuntimeError("Unknown or mismatched source per-layer settings mode");
        if (flag == 0) {
            // Inspect the original raw rows, not edited document overrides. A
            // disabled table with conflicting values has ambiguous firmware meaning.
            const auto check_rows = [&](const auto &layout, const auto &read_row) {
                for (size_t i = 0; i < m_layers.size(); ++i) {
                    const auto row = read_row(layout, i);
                    const auto &defaults = i < actual.bottom_layers ? actual.bottom : actual.normal;
                    for (const auto &[field, value] : defaults)
                        if (row.at(field) != value)
                            throw RuntimeError("Disabled source per-layer table conflicts with global defaults");
                }
            };
            check_rows(CTBEncryptedLayerLayout(native_original()), ctb_encrypted_layer_parameters);
        }
        return actual;
    }
    if (anycubic_available_versions(m_archive_format).empty()) return std::nullopt;
    const AnycubicLayerLayout layout(native_original(), m_archive_format);
    if (layout.version != 516 && layout.version != 517) return std::nullopt;
    using F = LayerParameter;
    LayerJobParameters result;
    result.layer_height = layout.number(layout.header + 20);
    if (layout.integer(layout.header + 12) >= 96 && layout.integer(layout.header + 108) != 0)
        throw RuntimeError("Source intelligent exposure mode cannot be preserved by this destination");
    const double count = layout.number(layout.header + 36);
    const auto transition = layout.integer(layout.header + 88);
    if (!std::isfinite(count) || count < 0 || count != std::floor(count) || count > layout.count ||
        transition > layout.count - size_t(count) || layout.integer(layout.header + 92) != 0)
        throw RuntimeError("Source global layer counts or transition mode cannot be preserved");
    result.bottom_layers = size_t(count); result.transition_layers = transition;
    if (!std::isfinite(result.layer_height) || result.layer_height <= 0)
        throw RuntimeError("Invalid source global layer height");
    const auto extra = layout.integer(40);
    if (extra) layout.named(extra, "EXTRA", 72);
    const bool advanced = layout.integer(layout.header + 12) >= 84 && layout.integer(layout.header + 96) != 0;
    if (advanced && !extra) throw RuntimeError("Source advanced global motion is missing EXTRA");
    for (bool bottom : {false, true}) {
        auto &values = bottom ? result.bottom : result.normal;
        values = {{F::Exposure, layout.number(layout.header + (bottom ? 32 : 24))},
            {F::WaitBefore, layout.number(layout.header + 28)}, {F::WaitAfter, 0},
            {F::WaitAfterLift, 0}, {F::LightOff, 0}, {F::LightPWM, 255}, {F::RetractDistance2, 0}};
        const size_t base = extra + (bottom ? 20 : 48);
        values[F::LiftDistance] = advanced ? layout.number(base) : layout.number(layout.header + 40);
        values[F::LiftDistance2] = advanced ? layout.number(base + 12) : 0;
        values[F::LiftSpeed] = double(advanced ? layout.number(base + 4) : layout.number(layout.header + 44)) * 60;
        values[F::LiftSpeed2] = double(advanced ? layout.number(base + 16) : layout.number(layout.header + 44)) * 60;
        values[F::RetractSpeed] = double(advanced ? layout.number(base + 20) : layout.number(layout.header + 48)) * 60;
        values[F::RetractSpeed2] = double(advanced ? layout.number(base + 8) : layout.number(layout.header + 48)) * 60;
        for (const auto &[field, value] : values)
            if (!std::isfinite(value) || value < 0 || !std::isfinite(float(value)))
                throw RuntimeError("Invalid source global exposure or motion");
        if ((values.at(F::LiftDistance) > 0 && values.at(F::LiftSpeed) <= 0) ||
            (values.at(F::LiftDistance2) > 0 && values.at(F::LiftSpeed2) <= 0) ||
            (values.at(F::LiftDistance) + values.at(F::LiftDistance2) > 0 && values.at(F::RetractSpeed) <= 0))
            throw RuntimeError("Invalid source global movement speed");
    }
    result.validate(m_layers.size());
    const auto per_layer_mode = layout.integer(layout.header + 80);
    if (per_layer_mode > 1) throw RuntimeError("Unknown source Anycubic per-layer settings mode");
    if (per_layer_mode == 0) {
        for (size_t i = 0; i < m_layers.size(); ++i) {
            const auto at = layout.record(i);
            const auto &defaults = i < result.bottom_layers ? result.bottom : result.normal;
            if (layout.number(at + 16) != defaults.at(F::Exposure) ||
                layout.number(at + 8) != float(defaults.at(F::LiftDistance) + defaults.at(F::LiftDistance2)) ||
                double(layout.number(at + 12)) * 60 != defaults.at(F::LiftSpeed))
                throw RuntimeError("Disabled source Anycubic per-layer table conflicts with global defaults");
        }
    }
    return result;
}

namespace {
void require_same_job_parameters(const LayerJobParameters &actual, const LayerJobParameters &expected)
{
    using F = LayerParameter;
    if (actual.layer_height != double(float(expected.layer_height)) || actual.bottom_layers != expected.bottom_layers ||
        actual.transition_layers != expected.transition_layers)
        throw RuntimeError("Destination cannot preserve source global layer height or counts");
    for (bool bottom : {false, true}) {
        const auto &source = bottom ? expected.bottom : expected.normal;
        const auto &destination = bottom ? actual.bottom : actual.normal;
        for (const auto &[field, value] : source) {
            const double represented = field == F::LiftDistance ?
                double(float(value + source.at(F::LiftDistance2))) - double(float(source.at(F::LiftDistance2))) : double(float(value));
            if (!std::isfinite(represented) || destination.at(field) != represented)
                throw RuntimeError("Destination cannot preserve a source global exposure or motion parameter");
        }
    }
}
} // namespace

void LayerDocument::require_job_parameters(const LayerJobParameters &expected) const
{
    const auto actual = job_parameters();
    if (!actual) throw RuntimeError("Destination has no native global parameter table");
    require_same_job_parameters(*actual, expected);
}

void LayerDocument::verify_converted_chitubox(const std::string &path,
    const std::vector<LayerParameters> &table, const LayerJobParameters *globals,
    const LayerProgress &progress) const
{
    const auto check = [&] { check_layer_progress(progress); };
    check();
    if (m_archive_format != "ctb_v3" && m_archive_format != "ctb_v4" && m_archive_format != "ctb_v5")
        throw RuntimeError("Native conversion readback requires ordinary CTB v3, v4 or v5");

    boost::nowide::ifstream input(path, std::ios::binary | std::ios::ate);
    const auto length = input.tellg();
    check_archive_length(length, 112, "Chitubox");
    std::vector<uint8_t> bytes(static_cast<size_t>(length));
    input.seekg(0);
    for (size_t at = 0; at < bytes.size();) {
        check();
        const size_t chunk = std::min(size_t(1) << 20, bytes.size() - at);
        input.read(reinterpret_cast<char *>(bytes.data() + at), chunk);
        if (!input) throw FileIOError("Cannot read converted Chitubox archive");
        at += chunk;
    }
    check();
    const ChituboxLayerLayout baseline(native_original(), check), actual(bytes, check);
    // Cipher interpretation depends on version, seed, geometry and layer
    // index. Equal bytes alone cannot establish equal images without these.
    if (actual.magic != baseline.magic || actual.version != baseline.version ||
        actual.columns != baseline.columns || actual.rows != baseline.rows ||
        actual.count != baseline.count || actual.planes != baseline.planes || actual.seed != baseline.seed ||
        table.size() != actual.count || m_layers.size() != actual.count)
        throw RuntimeError("Converted Chitubox raster identity changed during parameter transfer");
    for (size_t field : {8u, 12u, 16u})
        if (actual.integer(field) != baseline.integer(field))
            throw RuntimeError("Converted Chitubox physical geometry changed during parameter transfer");

    const auto actual_globals = chitubox_job_parameters(actual, check);
    if (globals) require_same_job_parameters(actual_globals, *globals);
    for (size_t i = 0; i < actual.count; ++i) {
        check_layer_progress(progress, i, actual.count);
        if (chitubox_layer_parameters(actual, i) != table[i])
            throw RuntimeError("Native destination cannot preserve the complete converted parameter table");
        for (unsigned plane = 0; plane < actual.planes; ++plane) {
            const auto source_record = baseline.record(i, plane), target_record = actual.record(i, plane);
            const auto source_data = baseline.integer(source_record + 12), target_data = actual.integer(target_record + 12);
            const auto size = actual.integer(target_record + 16);
            if (size != baseline.integer(source_record + 16))
                throw RuntimeError("Converted Chitubox mask length changed during parameter transfer");
            // Both layouts validated bounds and section overlap. The private
            // bootstrap has also passed full RLE decode and source-pixel
            // comparison. Equality at the same cipher index therefore proves
            // the final image without decoding/encoding an extra PNG copy.
            for (size_t offset = 0; offset < size;) {
                check_layer_progress(progress, i, actual.count);
                const auto chunk = std::min(size_t(65536), size_t(size) - offset);
                const auto source_chunk = baseline.bytes.read(size_t(source_data) + offset, chunk);
                if (std::memcmp(source_chunk.data(), bytes.data() + target_data + offset, chunk) != 0)
                    throw RuntimeError("Converted Chitubox mask changed during parameter transfer");
                offset += chunk;
            }
        }
    }
    check_layer_progress(progress, actual.count, actual.count);
}

LayerParameterCapabilities LayerDocument::parameter_capabilities() const
{
    if (m_fresh_job) {
        auto capabilities = parameter_conversion_schema("ctb_v4");
        for (auto &capability : capabilities)
            if (capability.field == LayerParameter::PositionZ) capability.editable = false;
        return capabilities;
    }
    if (m_layer_parameters.empty()) return {};
    if (const auto family = native_family(m_archive_format); family != NativeFamily::None)
        return native_capabilities(family);
    if(is_ctb_encrypted_layer_format(m_archive_format))
        return ctb_encrypted_parameter_capabilities(CTBEncryptedLayerLayout(native_original()));
    if(is_chitubox_layer_format(m_archive_format)) {
        const unsigned version=m_archive_format=="ctb_v3"?3:4;
        return chitubox_parameter_capabilities(version);
    }
    return anycubic_parameter_capabilities(layer_parameters(0));
}
LayerParameters LayerDocument::layer_parameters(size_t layer) const
{
    m_layers.at(layer);
    if (m_fresh_job) return m_fresh_job->rows.at(layer);
    if (m_layer_parameters.empty()) return {};
    return decode_parameters(m_entries.at(m_layer_parameters.at(layer)).bytes);
}
bool LayerDocument::has_convertible_parameter_table() const
{
    return !m_layer_parameters.empty() && native_family(m_archive_format) == NativeFamily::None;
}
bool LayerDocument::has_parameter_edits() const
{
    if (m_layer_parameters.empty()) return false;
    if (native_family(m_archive_format) != NativeFamily::None) {
        const auto source = read_native_table(m_archive_format, native_original());
        if (!source) return false;
        for (size_t i = 0; i < layer_count(); ++i)
            if (layer_parameters(i) != source->rows.at(i)) return true;
        return false;
    }
    if(is_ctb_encrypted_layer_format(m_archive_format)) {
        const CTBEncryptedLayerLayout source(native_original());
        for(size_t i=0;i<layer_count();++i)
            if(layer_parameters(i)!=ctb_encrypted_layer_parameters(source,i))return true;
        return false;
    }
    if(is_chitubox_layer_format(m_archive_format)) {
        const ChituboxLayerLayout source(native_original());
        for(size_t i=0;i<layer_count();++i)
            if(layer_parameters(i)!=chitubox_layer_parameters(source,i))return true;
        return false;
    }
    const AnycubicLayerLayout source(native_original(),m_archive_format);
    double z=0;
    for (size_t i=0;i<layer_count();++i) {
        z+=source.number(source.record(i)+20);const auto values=layer_parameters(i);
        if(values!=anycubic_layer_parameters(source,i,z))return true;
    }
    return false;
}
void LayerDocument::replace_layer_parameter_table(const std::vector<LayerParameters> &table)
{
    if (m_fresh_job) {
        if (table.size() != layer_count()) throw RuntimeError("An owned table requires one complete row per record");
        const auto capabilities = parameter_capabilities();
        std::vector<Change> changes;
        for (size_t i = 0; i < table.size(); ++i) {
            if (table[i].size() != m_fresh_job->rows[i].size()) throw RuntimeError("Incomplete owned parameter row");
            LayerParameters before, after;
            for (const auto &cap : capabilities) {
                const auto found = table[i].find(cap.field);
                if (found == table[i].end() || !std::isfinite(found->second) || found->second < 0)
                    throw RuntimeError("Invalid owned parameter row");
                const double original = m_fresh_job->rows[i].at(cap.field);
                if (found->second == original) continue;
                if (!cap.editable) throw RuntimeError("Owned layer Z requires a geometry operation");
                before[cap.field] = original; after[cap.field] = found->second;
            }
            if (!after.empty()) changes.push_back({i, encode_parameters(before), encode_parameters(after), Change::Target::FreshRow});
        }
        if (changes.empty()) return;
        if (m_fresh_job->parameter_origin == FreshLayerParameterOrigin::AutomaticModel)
            changes.push_back({0, {uint8_t(FreshLayerParameterOrigin::AutomaticModel)},
                {uint8_t(FreshLayerParameterOrigin::Explicit)}, Change::Target::FreshOrigin});
        commit(std::move(changes));
        return;
    }
    if(m_layer_parameters.empty() || table.size()!=layer_count())
        throw RuntimeError("A parameter table requires one complete canonical row per native layer");
    std::vector<Change> changes;changes.reserve(table.size());
    for(size_t i=0;i<table.size();++i) {
        const auto entry=m_layer_parameters.at(i);auto after=encode_parameters(table[i]);
        if(after!=m_entries[entry].bytes)
            changes.push_back({entry,m_entries[entry].bytes,std::move(after)});
    }
    // Only small JSON buffers are swapped for validation. Native source bytes,
    // masks and history are neither copied nor changed by the tentative table.
    auto swap_rows=[&] {for(auto &change:changes)m_entries[change.entry].bytes.swap(change.after);};
    swap_rows();
    try { validate_persisted_state(); }
    catch(...) {swap_rows();throw;}
    swap_rows();
    commit(std::move(changes));
}
void LayerDocument::set_layer_parameters(size_t layer,const LayerParameters &patch)
{
    m_layers.at(layer);
    if (patch.empty()) return;
    if (m_fresh_job) {
        const auto capabilities = parameter_capabilities();
        LayerParameters before, after;
        for (const auto &[field, value] : patch) {
            const auto cap = std::find_if(capabilities.begin(), capabilities.end(),
                [&](const auto &c) { return c.field == field && c.editable; });
            if (cap == capabilities.end() || !std::isfinite(value) || value < 0)
                throw RuntimeError("Invalid or read-only owned layer parameter");
            const double original = m_fresh_job->rows.at(layer).at(field);
            if (value != original) { before[field] = original; after[field] = value; }
        }
        if (after.empty()) return;
        std::vector<Change> changes;
        changes.push_back({layer, encode_parameters(before), encode_parameters(after), Change::Target::FreshRow});
        if (m_fresh_job->parameter_origin == FreshLayerParameterOrigin::AutomaticModel)
            changes.push_back({0, {uint8_t(FreshLayerParameterOrigin::AutomaticModel)},
                {uint8_t(FreshLayerParameterOrigin::Explicit)}, Change::Target::FreshOrigin});
        commit(std::move(changes));
        return;
    }
    auto values=layer_parameters(layer);const auto capabilities=parameter_capabilities();
    for (const auto &[field,value]:patch) {
        const auto found=std::find_if(capabilities.begin(),capabilities.end(),[&](const auto &c){return c.field==field && c.editable;});
        if (found==capabilities.end()) throw RuntimeError("This format cannot edit the requested per-layer field yet");
        const bool anycubic_speed=!anycubic_available_versions(m_archive_format).empty() && field==LayerParameter::LiftSpeed;
        const double encoded_value=anycubic_speed?value/60.:value;
        if(!std::isfinite(value) || value<0 || value>std::numeric_limits<float>::max() || encoded_value>std::numeric_limits<float>::max())
            throw RuntimeError("Layer parameters must be finite nonnegative values representable by the format");
        // Normalize mm/min through the actual Anycubic mm/s float field,
        // so save/reopen and no-op comparisons use the same represented value.
        if(native_family(m_archive_format)!=NativeFamily::None)
            values[field]=native_canonical(native_family(m_archive_format),field,value);
        else if(anycubic_speed)
            values[field]=double(float(value/60.))*60.;
        else values[field]=double(float(value));
        if(values[field]>std::numeric_limits<float>::max())
            throw RuntimeError("Layer speed overflows the canonical unit range");
    }
    if (values.empty()) return;
    if ((is_chitubox_layer_format(m_archive_format) || is_ctb_encrypted_layer_format(m_archive_format)) &&
        values.count(LayerParameter::LiftDistance2) &&
        (patch.count(LayerParameter::LiftDistance) || patch.count(LayerParameter::LiftDistance2))) {
        // CTB stores total lift and second phase as floats. Normalize their
        // difference, rather than rounding first independently: the UI and
        // project snapshot must already contain what native readback returns.
        // Use the requested first phase to avoid an extra rounding step.
        const auto first=patch.count(LayerParameter::LiftDistance)?patch.at(LayerParameter::LiftDistance):
            values.at(LayerParameter::LiftDistance);
        const auto second=values.at(LayerParameter::LiftDistance2);
        if(first+second>std::numeric_limits<float>::max())
            throw RuntimeError("Total lift distance exceeds the format range");
        const auto represented_first=double(float(first+second))-second;
        if(represented_first<0)
            throw RuntimeError("Represented total lift is smaller than its second phase");
        values[LayerParameter::LiftDistance]=represented_first;
    }
    const double z=values.at(LayerParameter::PositionZ);
    const double previous=layer ? layer_parameters(layer-1).at(LayerParameter::PositionZ) : 0.;
    if (z<=0 || (m_archive_format=="uvj" ? z<previous : z<=previous) ||
        (layer+1<layer_count() && (m_archive_format=="uvj" ?
            z>layer_parameters(layer+1).at(LayerParameter::PositionZ) :
            z>=layer_parameters(layer+1).at(LayerParameter::PositionZ))))
        throw RuntimeError("Layer Z must increase strictly between its neighbours");
    double limit=0;
    if(const auto family=native_family(m_archive_format);family!=NativeFamily::None) {
        // Only a Z edit needs the machine height, and reading it parses the container:
        // exposure and motion edits in a loop over layers must not pay that per call.
        if(patch.count(LayerParameter::PositionZ)){
            const auto source=read_native_table(m_archive_format,native_original());
            if(!source)throw RuntimeError("The container no longer matches the parameter table");
            limit=source->height_limit;
        }
        validate_native_row(family,values,previous);
        // A Z edit also changes the thickness of the next layer (ZCodex checks its lift against it).
        if(patch.count(LayerParameter::PositionZ) && layer+1<layer_count())
            validate_native_row(family,layer_parameters(layer+1),z);
    } else
    if(is_ctb_encrypted_layer_format(m_archive_format)) {
        const CTBEncryptedLayerLayout source(native_original());limit=source.parameter(20);
        validate_chitubox_parameters(values);
    } else
    if(is_chitubox_layer_format(m_archive_format)) {
        const ChituboxLayerLayout source(native_original());limit=source.number(16);
        validate_chitubox_parameters(values);
    } else {
        const AnycubicLayerLayout source(native_original(),m_archive_format);
        validate_anycubic_motion(values);
        if(source.version>=516)limit=source.number(source.integer(44)+144);
        else if(const auto *geometry=anycubic_legacy_geometry(m_archive_format))limit=geometry->machine_z;
    }
    if(std::isfinite(limit) && limit>0 && !layer_z_within_native_limit(z,limit))throw RuntimeError("Layer Z exceeds the printer height");
    const auto index=m_layer_parameters.at(layer);auto after=encode_parameters(values);
    if (after!=m_entries[index].bytes) commit({{index,m_entries[index].bytes,std::move(after)}});
}
size_t LayerDocument::layer_entry(size_t layer, LayerMaskPlane plane) const
{
    if (plane==LayerMaskPlane::Main) return m_layers.at(layer);
    if (plane==LayerMaskPlane::Secondary && !m_secondary_layers.empty()) return m_secondary_layers.at(layer);
    throw RuntimeError("This document has no selected mask plane");
}
bool LayerDocument::mask_is_shared(size_t layer) const
{
    const auto main=m_layers.at(layer);
    return !m_secondary_layers.empty() && m_secondary_layers.at(layer)==main;
}
LayerDocument::EncodedView LayerDocument::encoded_layer(size_t layer, LayerMaskPlane plane) const
{
    return EncodedView(m_entries.at(layer_entry(layer,plane)).load());
}

png::ImageGreyscale LayerDocument::image(size_t layer, LayerMaskPlane plane) const
{
    const auto loaded = m_entries.at(layer_entry(layer,plane)).load();
    const auto &bytes = *loaded;
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

std::array<uint8_t,256> LayerDocument::edit_value_projection() const
{
    std::vector<uint8_t> values(256);
    for (size_t value=0;value<values.size();++value) values[value]=uint8_t(value);
    quantize_mask(values);
    std::array<uint8_t,256> result;
    std::copy(values.begin(),values.end(),result.begin());
    return result;
}

void LayerDocument::quantize_mask(std::vector<uint8_t> &pixels) const
{
    if (m_mask_precision) {
        for (auto &value:pixels) value=project_mask_value(value,*m_mask_precision);
        return;
    }
    if (m_fresh_job) {
        for (auto &value : pixels)
            value=project_mask_value(value,m_fresh_job->mask_precision);
        return;
    }
    const bool chitubox = is_ctb_mask_format(m_archive_format);
    if (!anycubic_available_versions(m_archive_format).empty()) {
        const AnycubicLayerLayout layout(native_original(), m_archive_format);
        for (auto &value : pixels) value = layout.quantize(value);
    } else if(chitubox) {
        const bool binary = is_chitubox_binary_format(m_archive_format);
        const auto original=native_original();
        const unsigned planes = !binary || boost::algorithm::iends_with(m_archive_format,"_v1") ? 1 : original.at(92);
        for(auto &value:pixels)value=ChituboxLayerLayout::quantize(value,binary,planes);
    } else if(is_anycubic_zip_layer_format(m_archive_format)) {
        // The file stores four-bit PW0 gray; any other level would not survive a save.
        for(auto &value:pixels)value=AnycubicZipLayout::quantize(value);
    } else if(is_phrozen_layer_format(m_archive_format)) {
        // Seven-bit RLE gray (PHZ and FDG); white is stored as 0xf8.
        for(auto &value:pixels)value=PhrozenLayout::quantize(value);
    } else if(is_native_binary_layer_format(m_archive_format)) {
        for(auto &value:pixels)value=quantize_native_binary(m_archive_format,value);
    } else if(m_archive_format=="cxdlpv4" || m_archive_format=="pbm")
        for(auto &value:pixels)value=m_archive_format=="pbm"?uint8_t(value?255:0):value<2?0:uint8_t(value|1);
}

void LayerDocument::replace(size_t layer, const png::ImageGreyscale &image, LayerMaskPlane plane)
{
    const size_t index = layer_entry(layer,plane);
    std::vector<uint8_t> quantized;const uint8_t *pixels=image.buf.data();
    if (m_mask_precision || m_fresh_job || !anycubic_available_versions(m_archive_format).empty() || m_archive_format=="cxdlpv4" ||
        m_archive_format=="pbm" || is_ctb_mask_format(m_archive_format) || is_anycubic_zip_layer_format(m_archive_format) ||
        is_phrozen_layer_format(m_archive_format) || is_native_binary_layer_format(m_archive_format)) {
        quantized=image.buf;quantize_mask(quantized);pixels=quantized.data();
    }
    const auto encoded = PNGRasterEncoder{}(pixels, image.cols, image.rows, 1);
    const auto *data = static_cast<const uint8_t *>(encoded.data());
    std::vector<uint8_t> after(data, data + encoded.size());
    if (m_entries[index].same_as(after)) return;
    commit({{index, m_entries[index].copy(), std::move(after)}});
}

std::vector<LayerDocument::Replacement> LayerDocument::prepare_replacements(
    const std::vector<Change> &changes, bool after) const
{
    std::vector<Replacement> replacements;
    replacements.reserve(changes.size());
    for (const auto &change : changes) {
        const auto &data = after ? change.after : change.before;
        Replacement replacement;
        if (change.target == Change::Target::Entry) {
            m_entries.at(change.entry);
            // A spilled payload is referenced in place (no read, no RAM): the entry points into the
            // history file, which stays alive as long as any entry or edit uses it.
            if (change.spill) replacement.stored = after ? StoredBytes{change.spill, change.after_at, change.after_len}
                                                         : StoredBytes{change.spill, change.before_at, change.before_len};
            else replacement.bytes = data;
        } else if (change.target == Change::Target::FreshRow) {
            if (!m_fresh_job) throw RuntimeError("Owned history requires source intentions");
            replacement.values = decode_parameters(data);
            for (const auto &[field, value] : replacement.values)
                m_fresh_job->rows.at(change.entry).at(field);
        } else {
            if (!m_fresh_job || data.size() != 1 || data[0] > 1)
                throw RuntimeError("Invalid owned history origin");
            replacement.origin = FreshLayerParameterOrigin(data[0]);
        }
        replacements.push_back(std::move(replacement));
    }
    return replacements;
}
void LayerDocument::apply_metadata(const std::vector<Change> &changes, const std::vector<Replacement> &replacements)
{
    for (size_t i = 0; i < changes.size(); ++i) {
        if (changes[i].target == Change::Target::FreshRow)
            for (const auto &[field, value] : replacements[i].values)
                m_fresh_job->rows[changes[i].entry].at(field) = value;
        else if (changes[i].target == Change::Target::FreshOrigin)
            m_fresh_job->parameter_origin = replacements[i].origin;
    }
}
void LayerDocument::validate_metadata_edit(const std::vector<Change> &changes, const std::vector<Replacement> &replacements)
{
    if (std::none_of(changes.begin(), changes.end(), [](const auto &c) { return c.target != Change::Target::Entry; })) return;
    // Allocate the rollback before changing values. Only edited fields are
    // copied; PNG buffers and the rest of a potentially large table stay put.
    std::vector<Replacement> rollback(changes.size());
    for (size_t i = 0; i < changes.size(); ++i) {
        if (changes[i].target == Change::Target::FreshRow)
            for (const auto &[field, value] : replacements[i].values)
                rollback[i].values[field] = m_fresh_job->rows.at(changes[i].entry).at(field);
        else if (changes[i].target == Change::Target::FreshOrigin)
            rollback[i].origin = m_fresh_job->parameter_origin;
    }
    apply_metadata(changes, replacements);
    try { m_fresh_job->validate(layer_count()); }
    catch (...) { apply_metadata(changes, rollback); throw; }
    apply_metadata(changes, rollback);
}
void LayerDocument::apply_replacements(const std::vector<Change> &changes, std::vector<Replacement> &replacements)
{
    apply_metadata(changes, replacements);
    for (size_t i = 0; i < changes.size(); ++i)
        if (changes[i].target == Change::Target::Entry) {
            auto &entry = m_entries[changes[i].entry];
            entry.statistics.reset();
            if (replacements[i].stored) { entry.bytes.clear(); entry.bytes.shrink_to_fit(); entry.stored = replacements[i].stored; }
            else entry.set(std::move(replacements[i].bytes));
        }
    std::set<size_t> keep;
    for (const auto &change : changes) if (change.target == Change::Target::Entry) keep.insert(change.entry);
    spill_payloads(&keep);
}
namespace {
constexpr uint64_t history_disk_margin = uint64_t(256) << 20;
}
void LayerDocument::trim_undo_history()
{
    // Bound history by count; size is bounded by spilling cold payloads to a temporary file and,
    // only when that is impossible (no space, no temp directory), by dropping the oldest edits.
    while (m_undo.size() > 32) m_undo.erase(m_undo.begin());
    spill_cold_history();
    const auto resident = [](const Edit &edit) {
        uint64_t result = 0;
        for (const auto &change : edit.changes) result += change.before.size() + change.after.size();
        if (edit.structure)
            for (const auto &entry : edit.structure->entries) result += entry.bytes.size(); // stored payloads are not resident
        return result;
    };
    uint64_t bytes = 0;
    for (const auto &edit : m_undo) bytes += resident(edit);
    for (const auto &edit : m_redo) bytes += resident(edit);
    const uint64_t limit = layer_history_resident_budget();
    while (m_undo.size() > 1 && bytes > limit) {
        bytes -= resident(m_undo.front());
        m_undo.erase(m_undo.begin());
    }
}
void LayerDocument::spill_cold_history()
{
    const uint64_t limit = layer_history_resident_budget();
    const auto payload = [](const Edit &edit) {
        uint64_t result = 0;
        for (const auto &change : edit.changes)
            if (change.target == Change::Target::Entry) result += change.before.size() + change.after.size();
        return result;
    };
    const auto resident = [&](const Edit &edit) {
        uint64_t result = payload(edit);
        for (const auto &change : edit.changes)
            if (change.target != Change::Target::Entry) result += change.before.size() + change.after.size();
        if (edit.structure)
            for (const auto &entry : edit.structure->entries) result += entry.bytes.size(); // stored payloads are not resident
        return result;
    };
    uint64_t total = 0;
    for (const auto &edit : m_undo) total += resident(edit);
    for (const auto &edit : m_redo) total += resident(edit);
    if (total <= limit) return;
    // Farthest from the current state first; the edits Undo/Redo would hit next go last.
    std::vector<Edit *> order;
    for (size_t i = 0; i + 1 < m_undo.size(); ++i) order.push_back(&m_undo[i]);
    for (size_t i = 0; i + 1 < m_redo.size(); ++i) order.push_back(&m_redo[i]);
    if (!m_undo.empty()) order.push_back(&m_undo.back());
    if (!m_redo.empty()) order.push_back(&m_redo.back());
    for (Edit *edit : order) {
        if (total <= limit) break;
        const uint64_t bytes = payload(*edit);
        if (bytes == 0) continue;
        try {
            const auto space = layer_temp_space_available();
            if (space && *space < bytes + history_disk_margin) break;
            auto file = std::make_shared<HistorySpill>();
            std::vector<std::array<uint64_t, 2>> offsets(edit->changes.size());
            for (size_t i = 0; i < edit->changes.size(); ++i) {
                const auto &change = edit->changes[i];
                if (change.target != Change::Target::Entry || change.spill) continue;
                offsets[i] = {file->append(change.before), file->append(change.after)};
            }
            // Publish only after every payload reached the disk.
            for (size_t i = 0; i < edit->changes.size(); ++i) {
                auto &change = edit->changes[i];
                if (change.target != Change::Target::Entry || change.spill) continue;
                change.before_at = offsets[i][0]; change.before_len = change.before.size();
                change.after_at = offsets[i][1];  change.after_len = change.after.size();
                change.spill = file;
                std::vector<uint8_t>().swap(change.before);
                std::vector<uint8_t>().swap(change.after);
            }
            total -= bytes;
        } catch (const std::exception &) { break; } // keep the payload in RAM; trim_undo_history drops if needed
    }
}
void LayerDocument::add_streamed_change(std::vector<Change> &changes, std::shared_ptr<HistorySpill> &file,
                                        uint64_t &resident, Change change) const
{
    const auto flush = [&](Change &c) {
        c.before_at = file->append(c.before); c.before_len = c.before.size();
        c.after_at = file->append(c.after);   c.after_len = c.after.size();
        c.spill = file;
        std::vector<uint8_t>().swap(c.before);
        std::vector<uint8_t>().swap(c.after);
    };
    const uint64_t bytes = change.before.size() + change.after.size();
    if (!file && resident + bytes > layer_history_resident_budget()) {
        const auto space = layer_temp_space_available();
        if (space && *space < resident + bytes + history_disk_margin)
            throw RuntimeError("Not enough temporary disk space in " + layer_temp_directory() + " for the undo history of this operation (" +
                               layer_mib(*space) + " free). Free disk space or select fewer layers.");
        file = std::make_shared<HistorySpill>();
        for (auto &pending : changes) flush(pending);
        resident = 0;
    }
    if (file) flush(change); else resident += bytes;
    changes.push_back(std::move(change));
}
void LayerDocument::exchange_structure(std::shared_ptr<Structure> &stored)
{
    // Build the incoming layout first (copy when a document copy still shares it) so a failed
    // allocation leaves the live layout intact.
    Structure incoming = stored.use_count() == 1 ? std::move(*stored) : *stored;
    auto current = std::make_shared<Structure>();
    current->entries = std::move(m_entries);
    current->layers = std::move(m_layers);
    current->secondary_layers = std::move(m_secondary_layers);
    current->layer_parameters = std::move(m_layer_parameters);
    current->fresh_job = std::move(m_fresh_job);
    m_entries = std::move(incoming.entries);
    m_layers = std::move(incoming.layers);
    m_secondary_layers = std::move(incoming.secondary_layers);
    m_layer_parameters = std::move(incoming.layer_parameters);
    m_fresh_job = std::move(incoming.fresh_job);
    stored = std::move(current);
}
void LayerDocument::commit(std::vector<Change> changes)
{
    if (changes.empty()) return;
    const uint64_t history_limit = layer_history_edit_budget();
    // Only RAM-resident bytes are bounded here: payloads already in the history file (streamed
    // whole-document operations) or in the document's spill file cost disk, not memory.
    uint64_t document_bytes = 0, change_bytes = 0;
    for (const auto &entry : m_entries) document_bytes += entry.bytes.size();
    for (const auto &change : changes) {
        if (change.target == Change::Target::Entry)
            document_bytes = document_bytes - std::min<uint64_t>(document_bytes, m_entries.at(change.entry).bytes.size()) + change.after.size();
        change_bytes += change.before.size() + change.after.size();
    }
    if (document_bytes > layer_editor_byte_budget())
        throw RuntimeError("Edited document exceeds the editor memory budget");
    if (change_bytes > history_limit)
        throw RuntimeError("This edit exceeds the history budget (" + layer_mib(history_limit) + "). Select fewer layers.");
    if (change_bytes > layer_history_resident_budget()) {
        // The undo payload of a large edit lives on disk; refuse before touching anything.
        const auto space = layer_temp_space_available();
        if (space && *space < change_bytes + history_disk_margin)
            throw RuntimeError("Not enough temporary disk space in " + layer_temp_directory() + " for the undo history of this edit (needs " +
                               layer_mib(change_bytes) + ", " + layer_mib(*space) + " free). Select fewer layers or free disk space.");
    }
    const uint64_t next_revision = m_next_revision + 1;
    // Allocate replacement buffers before touching either masks or history.
    auto replacements = prepare_replacements(changes, true);
    validate_metadata_edit(changes, replacements);
    m_undo.reserve(m_undo.size() + 1);
    m_export_source.invalidate();
    m_undo.push_back({std::move(changes), m_revision, next_revision});
    trim_undo_history();
    apply_replacements(m_undo.back().changes, replacements);
    m_redo.clear();
    m_revision = m_next_revision = next_revision;
}

bool LayerDocument::transform_layers(size_t first, size_t last, MaskOperation operation, uint8_t value,
                                    const std::function<bool(size_t, size_t)> &progress, unsigned radius, LayerMaskPlane plane)
{
    if (radius > 4096) throw RuntimeError("Mask filter radius must not exceed 4096 pixels");
    if (first > last || last >= layer_count()) throw RuntimeError("Invalid layer range");
    std::vector<Change> changes;
    const size_t count = last - first + 1;
    uint64_t history_bytes = 0;
    for (size_t layer = first; layer <= last; ++layer) {
        if (progress && !progress(layer-first, count)) return false;
        auto mask = image(layer,plane);
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
        quantize_mask(mask.buf);
        const auto encoded = PNGRasterEncoder{}(mask.buf.data(), mask.cols, mask.rows, 1);
        const auto *data = static_cast<const uint8_t *>(encoded.data());
        std::vector<uint8_t> after(data, data + encoded.size());
        const auto index = layer_entry(layer,plane);
        if (!m_entries[index].same_as(after)) {
            history_bytes += after.size() + m_entries[index].size();
            if (history_bytes > layer_history_edit_budget())
                throw RuntimeError("This edit exceeds the history budget (" + layer_mib(layer_history_edit_budget()) + "). Select fewer layers.");
            changes.push_back({index, m_entries[index].copy(), std::move(after)});
        }
    }
    if (progress && !progress(count, count)) return false;
    commit(std::move(changes));
    return true;
}

png::ImageGreyscale LayerDocument::overlay(size_t first, size_t last, LayerMaskPlane plane,
                                           const LayerProgress &progress) const
{
    if (first > last || last >= layer_count()) throw RuntimeError("Invalid layer range");
    check_layer_progress(progress, 0, last-first+1);
    auto result = image(first, plane);
    for (size_t layer = first+1; layer <= last; ++layer) {
        check_layer_progress(progress, layer-first, last-first+1);
        const auto next = image(layer, plane);
        if (next.cols != result.cols || next.rows != result.rows)
            throw RuntimeError("Overlay requires equal layer dimensions");
        for (size_t i = 0; i < result.buf.size(); ++i)
            result.buf[i] = std::max(result.buf[i], next.buf[i]);
    }
    check_layer_progress(progress, last-first+1, last-first+1);
    return result;
}

bool LayerDocument::paint_stroke(size_t first, size_t last, const std::vector<BrushPoint> &points,
                                unsigned radius, uint8_t value, LayerMaskPlane plane,
                                const LayerProgress &progress, BrushShape shape)
{
    if (points.empty()) { if (first > last || last >= layer_count()) throw RuntimeError("Invalid layer range"); return true; }
    return edit_mask_range(first, last, plane, progress, [&](png::ImageGreyscale &mask) {
        auto previous = points.front();
        for (const auto &point : points) {
            if (shape == BrushShape::Round)
                draw_line(mask, previous.x, previous.y, point.x, point.y, radius, value);
            else
                draw_stroke(mask, previous.x, previous.y, point.x, point.y, radius, value, shape);
            previous = point;
        }
    });
}

bool LayerDocument::paint_shape(size_t first, size_t last, const ShapeOp &op,
                                LayerMaskPlane plane, const LayerProgress &progress)
{
    return edit_mask_range(first, last, plane, progress,
        [&](png::ImageGreyscale &mask) { apply_shape(mask, op); });
}

bool LayerDocument::edit_mask_range(size_t first, size_t last, LayerMaskPlane plane,
                                    const LayerProgress &progress,
                                    const std::function<void(png::ImageGreyscale &)> &edit)
{
    if (first > last || last >= layer_count()) throw RuntimeError("Invalid layer range");
    const auto generation = export_generation_guard();
    std::vector<Change> changes;
    size_t history_bytes = 0;
    const size_t count = last-first+1;
    const auto checkpoint = [&](size_t completed) {
        const auto check_source = [&] {
            if (!generation.current())
                throw RuntimeError("Layer document changed during brush operation");
        };
        check_source();
        const bool proceed = !progress || progress(completed, count);
        check_source();
        return proceed;
    };
    const auto batch_width = [&](size_t start) {
        // Use actual PNG headers, not printer metadata, for the working-set
        // estimate. Six pixel buffers cover decoder/encoder scratch, growing
        // compressed vectors and their copies; allow another 2 MiB per worker.
        // The existing 128 MiB single-mask limit still applies to serial work.
        constexpr size_t budget = size_t(512) << 20;
        size_t width = 0, reserved = 0;
        for (size_t layer = start; layer <= last && width < 4; ++layer) {
            const auto bytes = encoded_layer(layer, plane);
            // A malformed future header stops lookahead, not the operation.
            // image() will report its error after that layer's callback, so an
            // earlier Cancel keeps the same precedence as serial processing.
            if (bytes.sz < 33 || !png::is_png(bytes)) break;
            const auto *data = static_cast<const uint8_t *>(bytes.buf);
            const auto word = [&](size_t at) {
                return (uint32_t(data[at]) << 24) | (uint32_t(data[at+1]) << 16) |
                    (uint32_t(data[at+2]) << 8) | uint32_t(data[at+3]);
            };
            const uint64_t columns = word(16), rows = word(20);
            if (word(8) != 13 || std::memcmp(data+12, "IHDR", 4) != 0 ||
                data[24] != 8 || data[25] != 0 || !columns || !rows ||
                columns > 32768 || rows > 32768 || columns*rows > (uint64_t(128) << 20))
                break;
            const size_t pixels = size_t(columns*rows);
            if (pixels < (size_t(1) << 20)) break;
            const size_t estimate = pixels*6 + (size_t(2) << 20);
            if (estimate > budget-reserved) break;
            reserved += estimate;
            ++width;
        }
        return std::max(size_t(1), width);
    };
    struct PreparedMask {
        std::vector<uint8_t> bytes;
        std::exception_ptr error;
    };
    for (size_t layer = first; layer <= last;) {
        if (!checkpoint(layer-first)) return false;
        const size_t width = batch_width(layer);
        std::vector<PreparedMask> prepared(width);
        const auto prepare = [&](size_t offset) {
            try {
                auto mask = image(layer+offset, plane);
                edit(mask);
                quantize_mask(mask.buf);
                const auto encoded = PNGRasterEncoder{}(mask.buf.data(), mask.cols, mask.rows, 1);
                if (encoded.size() == 0) throw RuntimeError("Cannot encode edited layer PNG");
                const auto *bytes = static_cast<const uint8_t *>(encoded.data());
                prepared[offset].bytes.assign(bytes, bytes+encoded.size());
            } catch (...) { prepared[offset].error = std::current_exception(); }
        };
        // Workers only read document state and own their output. Callbacks and
        // history stay on the coordinator, after all borrowed data are released.
        if (width == 1) prepare(0);
        else {
            tbb::task_arena arena{int(width)};
            arena.execute([&] {
                execution::for_each(ExecutionTBB{}, size_t(0), width, prepare);
            });
        }
        for (size_t offset = 0; offset < width; ++offset) {
            if (offset != 0 && !checkpoint(layer+offset-first)) return false;
            auto &item = prepared[offset];
            if (item.error) std::rethrow_exception(item.error);
            const auto index = layer_entry(layer+offset, plane);
            if (!m_entries[index].same_as(item.bytes)) {
                history_bytes += item.bytes.size()+m_entries[index].size();
                if (history_bytes > layer_history_edit_budget())
                    throw RuntimeError("This edit exceeds the history budget (" + layer_mib(layer_history_edit_budget()) + "). Select fewer layers.");
                changes.push_back({index, m_entries[index].copy(), std::move(item.bytes)});
            }
        }
        layer += width;
    }
    if (!checkpoint(count)) return false;
    commit(std::move(changes));
    return true;
}

namespace {
bool valid_raster(const png::ImageGreyscale &img)
{
    return img.cols && img.rows && img.cols <= 32768 && img.rows <= 32768 &&
        img.buf.size() == img.cols * img.rows;
}
} // namespace

size_t LayerDocument::draw_stroke(png::ImageGreyscale &img, int x0, int y0, int x1, int y1,
                                  unsigned radius, uint8_t value, BrushShape shape)
{
    if (!valid_raster(img) || radius > 4096) throw RuntimeError("Brush radius or raster is outside the supported range");
    const int r = int(radius), cols = int(img.cols), rows = int(img.rows);
    // Cheap reject: the swept square's bounding box misses the image entirely.
    if (std::max(x0, x1)+r < 0 || std::min(x0, x1)-r >= cols ||
        std::max(y0, y1)+r < 0 || std::min(y0, y1)-r >= rows)
        return 0;
    // draw_line rejects out-of-image endpoints, so spans are built here for both shapes.
    std::vector<int> half_width(radius + 1, r);
    if (shape == BrushShape::Round) {
        int extent = r;
        for (int d = 0; d <= r; ++d) {
            while (int64_t(extent)*extent + int64_t(d)*d > int64_t(r)*r) --extent;
            half_width[size_t(d)] = extent;
        }
    }
    const int first_row = std::max(0, std::min(y0, y1)-r), last_row = std::min(rows-1, std::max(y0, y1)+r);
    struct Span { int first, last; };
    std::vector<Span> spans(size_t(last_row-first_row+1), {cols, -1});
    const int dx = std::abs(x1-x0), sx = x0 < x1 ? 1 : -1;
    const int dy = -std::abs(y1-y0), sy = y0 < y1 ? 1 : -1;
    int error = dx + dy, x = x0, y = y0;
    for (;;) {
        for (int row = std::max(first_row, y-r); row <= std::min(last_row, y+r); ++row) {
            const int w = half_width[size_t(std::abs(row-y))];
            auto &span = spans[size_t(row-first_row)];
            span.first = std::min(span.first, std::max(0, x-w));
            span.last = std::max(span.last, std::min(cols-1, x+w));
        }
        if (x == x1 && y == y1) break;
        const int twice = 2*error;
        if (twice >= dy) { error += dy; x += sx; }
        if (twice <= dx) { error += dx; y += sy; }
    }
    size_t written = 0;
    for (int row = first_row; row <= last_row; ++row) {
        const auto &span = spans[size_t(row-first_row)];
        for (int col = span.first; col <= span.last; ++col, ++written)
            img.buf[size_t(row)*img.cols + size_t(col)] = value;
    }
    return written;
}

size_t LayerDocument::fill_rectangle(png::ImageGreyscale &img, int x0, int y0, int x1, int y1, uint8_t value)
{
    if (!valid_raster(img)) throw RuntimeError("Unsupported raster");
    const int left = std::max(0, std::min(x0, x1)), right = std::min(int(img.cols)-1, std::max(x0, x1));
    const int top = std::max(0, std::min(y0, y1)), bottom = std::min(int(img.rows)-1, std::max(y0, y1));
    if (left > right || top > bottom) return 0;
    for (int row = top; row <= bottom; ++row)
        std::fill(img.buf.begin() + ptrdiff_t(size_t(row)*img.cols + size_t(left)),
                  img.buf.begin() + ptrdiff_t(size_t(row)*img.cols + size_t(right) + 1), value);
    return size_t(right-left+1) * size_t(bottom-top+1);
}

size_t LayerDocument::fill_ellipse(png::ImageGreyscale &img, int x0, int y0, int x1, int y1, uint8_t value)
{
    if (!valid_raster(img)) throw RuntimeError("Unsupported raster");
    const int lx = std::min(x0, x1), hx = std::max(x0, x1), ly = std::min(y0, y1), hy = std::max(y0, y1);
    // Test pixel centres against the box-inscribed ellipse in exact integers:
    // (2dx)^2*B^2 + (2dy)^2*A^2 <= A^2*B^2 with A, B the box width/height in pixels.
    const int64_t A = int64_t(hx)-lx+1, B = int64_t(hy)-ly+1;
    size_t written = 0;
    for (int row = std::max(0, ly); row <= std::min(int(img.rows)-1, hy); ++row) {
        const int64_t ey = 2*int64_t(row) - (int64_t(ly)+hy);
        for (int col = std::max(0, lx); col <= std::min(int(img.cols)-1, hx); ++col) {
            const int64_t ex = 2*int64_t(col) - (int64_t(lx)+hx);
            if (ex*ex*B*B + ey*ey*A*A <= A*A*B*B) {
                img.buf[size_t(row)*img.cols + size_t(col)] = value;
                ++written;
            }
        }
    }
    return written;
}

size_t LayerDocument::flood_fill(png::ImageGreyscale &img, int sx, int sy, uint8_t value)
{
    if (!valid_raster(img)) throw RuntimeError("Unsupported raster");
    if (sx < 0 || sy < 0 || size_t(sx) >= img.cols || size_t(sy) >= img.rows) return 0;
    const uint8_t target = img.buf[size_t(sy)*img.cols + size_t(sx)];
    if (target == value) return 0; // nothing changes; also avoids endless revisits
    // Scanline fill with an explicit stack: no recursion depth limit on 8K+ masks.
    // Writing `value` while filling marks pixels as visited (value != target).
    size_t written = 0;
    std::vector<std::pair<int,int>> stack{{sx, sy}};
    const auto at = [&](int x, int y) -> uint8_t & { return img.buf[size_t(y)*img.cols + size_t(x)]; };
    while (!stack.empty()) {
        auto [x, y] = stack.back(); stack.pop_back();
        if (at(x, y) != target) continue;
        int left = x, right = x;
        while (left > 0 && at(left-1, y) == target) --left;
        while (right+1 < int(img.cols) && at(right+1, y) == target) ++right;
        for (int i = left; i <= right; ++i) at(i, y) = value;
        written += size_t(right-left+1);
        for (const int ny : {y-1, y+1}) {
            if (ny < 0 || ny >= int(img.rows)) continue;
            bool in_run = false;
            for (int i = left; i <= right; ++i) {
                const bool match = at(i, ny) == target;
                if (match && !in_run) stack.push_back({i, ny});
                in_run = match;
            }
        }
    }
    return written;
}

size_t LayerDocument::apply_shape(png::ImageGreyscale &img, const ShapeOp &op)
{
    switch (op.tool) {
    case ShapeTool::Line: return draw_stroke(img, op.x0, op.y0, op.x1, op.y1, op.radius, op.value, op.brush);
    case ShapeTool::Rectangle: return fill_rectangle(img, op.x0, op.y0, op.x1, op.y1, op.value);
    case ShapeTool::Ellipse: return fill_ellipse(img, op.x0, op.y0, op.x1, op.y1, op.value);
    case ShapeTool::Fill: return flood_fill(img, op.x0, op.y0, op.value);
    case ShapeTool::Text:
        if (!op.text) throw RuntimeError("The Text tool needs a text");
        return draw_text(img, op.x0, op.y0, *op.text, op.value);
    }
    return 0;
}

size_t LayerDocument::draw_text(png::ImageGreyscale &img, int x, int y, const hershey_text::TextSpec &spec, uint8_t value)
{
    if (!valid_raster(img)) throw RuntimeError("Unsupported raster");
    if (const std::string error = hershey_text::validate_text(spec); !error.empty()) {
        std::string text = error;
        while (!text.empty() && text.back() == '\n') text.pop_back();
        throw RuntimeError(text);
    }
    try {
        return hershey_text::put_text(img.buf.data(), int(img.rows), int(img.cols), x, y, spec, value);
    } catch (const std::invalid_argument &e) {
        throw RuntimeError(e.what());
    }
}

// UVtools FileFormat.DrawModifications (Supports / DrainHole). OpenCV's filled circle is used for both the "does the
// column touch material" test and the drawing, so the result equals UVtools' on the same pixels. Pixels are square:
// UVtools' PixelsToNormalizedPitch is the identity there.
namespace {
size_t count_in_circle(const png::ImageGreyscale &img, int cx_roi, int cy_roi, int roi_x, int roi_y, int size, int radius,
                       uint8_t circle_value, const std::function<bool(uint8_t)> &lit)
{
    const int w = std::min(size, int(img.cols) - roi_x), h = std::min(size, int(img.rows) - roi_y);
    if (w <= 0 || h <= 0) return 0;
    std::vector<uint8_t> mask(size_t(w) * size_t(h), 0);
    pixel_drawing::filled_circle(mask.data(), h, w, cx_roi, cy_roi, radius, circle_value, false);
    size_t count = 0;
    for (int row = 0; row < h; ++row)
        for (int col = 0; col < w; ++col) {
            const uint8_t m = mask[size_t(row) * size_t(w) + size_t(col)];
            if (m && lit(img.buf[size_t(roi_y + row) * img.cols + size_t(roi_x + col)])) ++count;
        }
    return count;
}
} // namespace

LayerDocument::ColumnStep LayerDocument::support_layer_step(png::ImageGreyscale &img, int x, int y, size_t layer_index,
                                                            unsigned drawn_layers, const SupportSpec &spec)
{
    if (!valid_raster(img)) throw RuntimeError("Unsupported raster");
    if (spec.tip == 0 || spec.pillar == 0 || spec.base == 0) throw RuntimeError("The support diameters must be greater than 0");
    const int radius = int((layer_index > 10 ? std::min(spec.tip + drawn_layers, spec.pillar) : spec.base) / 2);
    const int tip_radius = int(spec.tip / 2);
    const int xs = std::max(0, x - tip_radius), ys = std::max(0, y - tip_radius);
    // bitwise AND of the layer pixel with the circle brightness (a grey pixel 0x0F & 0xF0 is not counted)
    const uint8_t brightness = spec.brightness;
    const size_t white = count_in_circle(img, tip_radius, tip_radius, xs, ys, int(spec.tip), tip_radius, brightness,
                                         [brightness](uint8_t v) { return (v & brightness) != 0; });
    // the AND uses the circle's value, so count with the same brightness on both sides
    if (double(white) >= double(spec.tip) * double(spec.tip) / 3.0)
        return drawn_layers == 0 ? ColumnStep::Skipped : ColumnStep::Finished;
    pixel_drawing::filled_circle(img.buf.data(), int(img.rows), int(img.cols), x, y, radius, spec.brightness, spec.anti_alias);
    return ColumnStep::Drew;
}

LayerDocument::ColumnStep LayerDocument::drain_layer_step(png::ImageGreyscale &img, int x, int y, unsigned drawn_layers, const DrainSpec &spec)
{
    if (!valid_raster(img)) throw RuntimeError("Unsupported raster");
    if (spec.diameter == 0) throw RuntimeError("The drain hole diameter must be greater than 0");
    const int radius = int(spec.diameter / 2);
    const int xs = std::max(0, x - radius), ys = std::max(0, y - radius);
    const size_t black = count_in_circle(img, radius, radius, xs, ys, int(spec.diameter), radius, 255, [](uint8_t v) { return v <= 100; });
    if (double(black) >= double(spec.diameter) * double(spec.diameter) / 3.0)
        return drawn_layers == 0 ? ColumnStep::Skipped : ColumnStep::Finished;
    pixel_drawing::filled_circle(img.buf.data(), int(img.rows), int(img.cols), x, y, radius, 0, spec.anti_alias);
    return ColumnStep::Drew;
}

bool LayerDocument::dig_column(size_t clicked_layer, int x, int y, const std::variant<SupportSpec, DrainSpec> &spec,
                               LayerMaskPlane plane, const LayerProgress &progress)
{
    if (clicked_layer >= layer_count()) throw RuntimeError("The layer is outside the document");
    if (clicked_layer == 0) return true; // UVtools ignores clicks on the first layer
    const auto generation = export_generation_guard();
    const size_t total = clicked_layer;
    std::vector<Change> changes;
    size_t history_bytes = 0;
    unsigned drawn = 0;
    for (size_t step = 0; step < total; ++step) {
        const size_t layer = clicked_layer - 1 - step;
        if (!generation.current()) throw RuntimeError("Layer document changed during the operation");
        if (progress && !progress(step, total)) return false;
        auto mask = image(layer, plane);
        if (x < 0 || y < 0 || size_t(x) >= mask.cols || size_t(y) >= mask.rows)
            throw RuntimeError("The point is outside of the layer");
        const ColumnStep result = std::visit([&](const auto &s) {
            if constexpr (std::is_same_v<std::decay_t<decltype(s)>, SupportSpec>) return support_layer_step(mask, x, y, layer, drawn, s);
            else return drain_layer_step(mask, x, y, drawn, s);
        }, spec);
        if (result == ColumnStep::Finished) break;
        if (result == ColumnStep::Skipped) continue;
        ++drawn;
        quantize_mask(mask.buf);
        const auto encoded = PNGRasterEncoder{}(mask.buf.data(), mask.cols, mask.rows, 1);
        if (encoded.size() == 0) throw RuntimeError("Cannot encode edited layer PNG");
        const auto *bytes = static_cast<const uint8_t *>(encoded.data());
        std::vector<uint8_t> blob(bytes, bytes + encoded.size());
        const auto index = layer_entry(layer, plane);
        if (m_entries[index].same_as(blob)) continue;
        history_bytes += blob.size() + m_entries[index].size();
        if (history_bytes > layer_history_edit_budget())
            throw RuntimeError("This edit exceeds the history budget (" + layer_mib(layer_history_edit_budget()) + ").");
        changes.push_back({index, m_entries[index].copy(), std::move(blob)});
    }
    if (progress && !progress(total, total)) return false;
    if (!generation.current()) throw RuntimeError("Layer document changed during the operation");
    commit(std::move(changes));
    return true;
}

void LayerDocument::paint(size_t layer, int x, int y, unsigned radius, uint8_t value, LayerMaskPlane plane)
{
    auto img = image(layer,plane);
    draw_line(img, x, y, x, y, radius, value);
    replace(layer, img,plane);
}

void LayerDocument::replace_image(size_t layer, const png::ImageGreyscale &replacement, LayerMaskPlane plane)
{
    const auto previous = image(layer,plane);
    if (replacement.cols != previous.cols || replacement.rows != previous.rows ||
        replacement.buf.size() != replacement.cols * replacement.rows)
        throw RuntimeError("Edited layer dimensions must match the original raster");
    replace(layer, replacement,plane);
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
    if (radius == 0) {
        // A one-pixel line needs neither circle extents nor row storage.
        for (;;) {
            img.buf[size_t(y0)*img.cols + size_t(x0)] = value;
            if (x0 == x1 && y0 == y1) return;
            const int twice = 2 * error;
            if (twice >= dy) { error += dy; x0 += sx; }
            if (twice <= dx) { error += dx; y0 += sy; }
        }
    }

    // Exact inclusive disk extents: x*x + y*y <= r*r. The cursor only moves
    // inward, so all extents take O(radius) integer work, without sqrt/rounding.
    std::vector<int> half_width(radius + 1);
    int extent = r;
    for (int distance = 0; distance <= r; ++distance) {
        while (int64_t(extent)*extent + int64_t(distance)*distance > int64_t(r)*r)
            --extent;
        half_width[size_t(distance)] = extent;
    }
    const int first_row = std::max(0, std::min(y0, y1)-r);
    const int last_row = std::min(int(img.rows)-1, std::max(y0, y1)+r);
    struct Span { int first, last; };
    std::vector<Span> spans(size_t(last_row-first_row+1), {int(img.cols), -1});
    for (;;) {
        for (int row = std::max(first_row, y0-r); row <= std::min(last_row, y0+r); ++row) {
            const int width = half_width[size_t(std::abs(row-y0))];
            auto &span = spans[size_t(row-first_row)];
            span.first = std::min(span.first, std::max(0, x0-width));
            span.last = std::max(span.last, std::min(int(img.cols)-1, x0+width));
        }
        if (x0 == x1 && y0 == y1) break;
        const int twice = 2 * error;
        if (twice >= dy) { error += dy; x0 += sx; }
        if (twice <= dx) { error += dx; y0 += sy; }
    }
    // Within ONE Bresenham line, centers contributing to a row are consecutive
    // and adjacent in X. Their nonempty disk intervals therefore touch/overlap.
    // The union is exactly one span, so write each destination pixel once.
    // Do not merge spans this way across an arbitrary multi-segment stroke:
    // a loop or U-shaped stroke must leave its interior untouched.
    for (int row = first_row; row <= last_row; ++row) {
        const auto &span = spans[size_t(row-first_row)];
        if (span.first <= span.last) {
            auto begin = img.buf.begin() + size_t(row)*img.cols + size_t(span.first);
            std::fill_n(begin, size_t(span.last-span.first+1), value);
        }
    }
}
void LayerDocument::threshold(size_t layer, uint8_t cutoff, LayerMaskPlane plane)
{
    auto img = image(layer,plane);
    bool changed = false;
    for (auto &p : img.buf) {
        const uint8_t value = p >= cutoff ? 255 : 0;
        changed |= value != p;
        p = value;
    }
    if (changed) replace(layer, img,plane);
}
void LayerDocument::invert(size_t layer, LayerMaskPlane plane)
{
    auto img = image(layer,plane);
    for (auto &pixel : img.buf) pixel = uint8_t(255 - pixel);
    replace(layer, img,plane);
}
// ---- UVtools-style layer operations bridge ------------------------------------------------------
namespace {
// Decoded working set limit for whole-document operations (bytes of 8-bit pixels).


// Maps a sub-phase (done/total) onto [base, base+span] of a 0..100 overall progress scale so the
// user sees one monotone bar over decode -> operate -> encode.
LayerProgress phase_progress(const LayerProgress &overall, size_t base, size_t span)
{
    if (!overall) return {};
    return [overall, base, span](size_t done, size_t total) {
        return overall(base + (total ? span * std::min(done, total) / total : 0), 100);
    };
}
layerops::Mask to_mask(const png::ImageGreyscale &image)
{
    layerops::Mask mask(image.cols, image.rows);
    mask.px = image.buf;
    return mask;
}
uint64_t pixel_hash(const std::vector<uint8_t> &px)
{
    return std::hash<std::string_view>{}(std::string_view(reinterpret_cast<const char *>(px.data()), px.size()));
}
// Most common positive Z step (rounded to 1 nm): the nominal layer height of the edited result.
std::optional<double> dominant_layer_height(const std::vector<double> &z_mm)
{
    std::map<long long, size_t> votes;
    for (size_t i = 1; i < z_mm.size(); ++i) {
        const double step = z_mm[i] - z_mm[i - 1];
        if (step > 1e-9) ++votes[std::llround(step * 1e6)];
    }
    if (votes.empty()) return std::nullopt;
    const auto best = std::max_element(votes.begin(), votes.end(),
        [](const auto &a, const auto &b) { return a.second < b.second; });
    return double(best->first) / 1e6;
}
} // namespace

bool LayerDocument::apply_mask_batch(LayerMaskPlane plane, const MaskBatchOperation &op, const LayerProgress &progress)
{
    if (!op) throw RuntimeError("No layer operation was given");
    const size_t count = layer_count();
    if (count == 0) throw RuntimeError("The document has no layers");
    const auto generation = export_generation_guard();
    try {
        check_layer_progress(progress, 0, 100);
        // Decode phase (0..30 %). The budget is checked after the first layer from its size.
        std::vector<layerops::Mask> masks;
        masks.reserve(count);
        std::vector<uint64_t> original_hash;
        original_hash.reserve(count);
        uint64_t decoded_bytes = 0;
        const auto decode_progress = phase_progress(progress, 0, 30);
        for (size_t layer = 0; layer < count; ++layer) {
            check_layer_progress(decode_progress, layer, count);
            masks.push_back(to_mask(image(layer, plane)));
            decoded_bytes += masks.back().px.size();
            if (layer == 0 && masks.back().px.size() * uint64_t(count) > layer_decoded_budget())
                throw RuntimeError("The document is too large for a whole-document operation (over the decoded-pixel budget of "+layer_mib(layer_decoded_budget())+")");
            if (decoded_bytes > layer_decoded_budget())
                throw RuntimeError("The document is too large for a whole-document operation (over the decoded-pixel budget of "+layer_mib(layer_decoded_budget())+")");
            original_hash.push_back(pixel_hash(masks.back().px));
        }
        std::vector<std::pair<size_t, size_t>> original_size;
        for (const auto &m : masks) original_size.emplace_back(m.width, m.height);
        check_layer_progress(progress, 30, 100);
        op(masks, phase_progress(progress, 30, 50));
        if (masks.size() != count)
            throw RuntimeError("This operation changes the layer count; it must be applied as a layer-list operation");
        // Encode phase (80..100 %): only changed layers are re-encoded and stored in history.
        std::vector<Change> changes;
        std::shared_ptr<HistorySpill> history_file;
        uint64_t history_resident = 0;
        for (size_t layer = 0; layer < count; ++layer) {
            check_layer_progress(progress, 80 + 20 * layer / count, 100);
            auto &mask = masks[layer];
            if (mask.width != original_size[layer].first || mask.height != original_size[layer].second)
                throw RuntimeError("This operation changes the mask size; use Change resolution as a layer-list operation");
            if (pixel_hash(mask.px) == original_hash[layer]) continue;
            quantize_mask(mask.px);
            const auto encoded = PNGRasterEncoder{}(mask.px.data(), mask.width, mask.height, 1);
            if (encoded.size() == 0) throw RuntimeError("Cannot encode edited layer PNG");
            const auto *bytes = static_cast<const uint8_t *>(encoded.data());
            std::vector<uint8_t> after(bytes, bytes + encoded.size());
            const auto index = layer_entry(layer, plane);
            if (m_entries[index].same_as(after)) continue;
            add_streamed_change(changes, history_file, history_resident, {index, m_entries[index].copy(), std::move(after)});
        }
        if (!generation.current()) throw RuntimeError("Layer document changed during the operation");
        check_layer_progress(progress, 100, 100);
        commit(std::move(changes));
        return true;
    } catch (const LayerOperationCancelled &) {
        return false;
    }
}

bool LayerDocument::apply_mask_stream(LayerMaskPlane plane, const MaskStreamOperation &op, const LayerProgress &progress)
{
    if (!op) throw RuntimeError("No layer operation was given");
    return apply_mask_window(plane, {}, [&op](layerops::Mask &mask, size_t layer, const MaskStreamContext &) { op(mask, layer); }, progress);
}

// The streaming engine. Layers are processed in chunks of T consecutive layers: the chunk's originals (plus the
// `before`/`after` neighbours) are decoded in parallel into a cache that slides along the document, `op` runs on a copy
// of each layer in parallel, the changed ones are encoded in parallel and then stored in layer order (serially) into one
// undo step whose payloads go to the history spill. Nothing is touched before the final commit, so cancelling or failing
// leaves the document as it was. Memory: (T + before + after + ~7 T) decoded layers, T chosen from the decoded budget.
bool LayerDocument::apply_mask_window(LayerMaskPlane plane, const MaskWindowSpec &spec, const MaskWindowOperation &op,
                                      const LayerProgress &progress)
{
    if (!op) throw RuntimeError("No layer operation was given");
    const size_t count = layer_count();
    if (count == 0) throw RuntimeError("The document has no layers");
    const size_t first = spec.first, last = std::min(spec.last, count - 1);
    if (first > last) throw RuntimeError("The layer range is empty or outside of the layers.");
    const size_t total = last - first + 1;
    const auto generation = export_generation_guard();
    try {
        check_layer_progress(progress, 0, total);
        const auto wants = [&](size_t layer) { return !spec.applies || spec.applies(layer); };
        // Layers whose result is needed: every layer of the range the operation applies to.
        size_t probe = first;
        while (probe <= last && !wants(probe)) ++probe;
        std::vector<Change> changes;
        std::shared_ptr<HistorySpill> file;
        uint64_t resident = 0;
        if (probe <= last) {
            const auto bytes_of = [&](size_t layer) {
                const auto loaded = m_entries.at(layer_entry(layer, plane)).load();
                const auto &b = *loaded;
                if (b.size() < 24) throw RuntimeError("Invalid layer PNG");
                const auto u32 = [&](size_t at) { return (uint64_t(b[at]) << 24) | (uint64_t(b[at + 1]) << 16) | (uint64_t(b[at + 2]) << 8) | uint64_t(b[at + 3]); };
                return u32(16) * u32(20);
            };
            const uint64_t per_layer = std::max<uint64_t>(1, bytes_of(probe));
            const uint64_t budget_layers = layer_decoded_budget() / per_layer;
            const uint64_t window = uint64_t(spec.before) + spec.after;
            // A cropped window keeps only the crop of every neighbour: count it in full-layer equivalents.
            const bool cropped = !spec.neighbour_crop.empty();
            const uint64_t window_cost = cropped ? (window * uint64_t(spec.neighbour_crop.w) * uint64_t(spec.neighbour_crop.h) + per_layer - 1) / per_layer : window;
            // chunk T: window cache (T + before + after) + copy, result and operation temporaries (~7 T) within the budget.
            const uint64_t room = budget_layers > window_cost ? (budget_layers - window_cost) / 8 : 0;
            if (room == 0)
                throw RuntimeError("The document is too large for this operation: it needs " + std::to_string(window_cost + 8) +
                                   " layers decoded at once (a neighbour window of " + std::to_string(window + 1) + " layers), over the decoded-pixel budget of " +
                                   layer_mib(layer_decoded_budget()));
            const size_t chunk = size_t(std::min<uint64_t>({room, 16, uint64_t(std::max(1, tbb::this_task_arena::max_concurrency())), total}));
            std::map<size_t, std::shared_ptr<const layerops::Mask>> cache;  // original layers, ascending (cropped mode: the chunk)
            std::map<size_t, std::shared_ptr<const layerops::Mask>> ncache; // cropped mode: neighbour crops of the window
            const layerops::Rect crop = spec.neighbour_crop;
            const layerops::LayerFetch fetch = [&](size_t layer) -> std::shared_ptr<const layerops::Mask> {
                if (layer >= count) throw RuntimeError("The operation asked for a layer outside of the document");
                const auto &store = cropped ? ncache : cache;
                const auto found = store.find(layer);
                if (found != store.end()) return found->second;
                auto full = to_mask(image(layer, plane));
                if (cropped) return std::make_shared<const layerops::Mask>(layerops::crop_mask(full, crop));
                return std::make_shared<const layerops::Mask>(std::move(full));
            };
            const MaskStreamContext context{count, &fetch};
            for (size_t c = first; c <= last; c += chunk) {
                const size_t end = std::min(last + 1, c + chunk);
                for (size_t i = c; i < end; ++i) check_layer_progress(progress, i - first, total);
                const size_t lo = c > spec.before ? c - spec.before : 0, hi = std::min(count - 1, end - 1 + spec.after);
                if (cropped) {
                    cache.clear();
                    ncache.erase(ncache.begin(), ncache.lower_bound(lo));
                    std::vector<size_t> need;
                    for (size_t i = lo; i <= hi; ++i)
                        if ((i >= c && i < end && wants(i)) || !ncache.count(i)) need.push_back(i);
                    std::vector<std::shared_ptr<const layerops::Mask>> full(need.size()), part(need.size());
                    tbb::parallel_for(tbb::blocked_range<size_t>(0, need.size()), [&](const tbb::blocked_range<size_t> &r) {
                        for (size_t k = r.begin(); k != r.end(); ++k) {
                            auto m = std::make_shared<const layerops::Mask>(to_mask(image(need[k], plane)));
                            if (!ncache.count(need[k])) part[k] = std::make_shared<const layerops::Mask>(layerops::crop_mask(*m, crop));
                            if (need[k] >= c && need[k] < end && wants(need[k])) full[k] = std::move(m);
                        }
                    });
                    for (size_t k = 0; k < need.size(); ++k) {
                        if (part[k]) ncache[need[k]] = std::move(part[k]);
                        if (full[k]) cache[need[k]] = std::move(full[k]);
                    }
                } else {
                cache.erase(cache.begin(), cache.lower_bound(lo));
                std::vector<size_t> missing;
                for (size_t i = lo; i <= hi; ++i)
                    if (!cache.count(i) && ((i >= c && i < end && wants(i)) || spec.before || spec.after)) missing.push_back(i);
                std::vector<std::shared_ptr<const layerops::Mask>> decoded(missing.size());
                tbb::parallel_for(tbb::blocked_range<size_t>(0, missing.size()), [&](const tbb::blocked_range<size_t> &r) {
                    for (size_t k = r.begin(); k != r.end(); ++k)
                        decoded[k] = std::make_shared<const layerops::Mask>(to_mask(image(missing[k], plane)));
                });
                for (size_t k = 0; k < missing.size(); ++k) cache[missing[k]] = std::move(decoded[k]);
                }
                std::vector<std::vector<uint8_t>> encoded(end - c);
                tbb::parallel_for(tbb::blocked_range<size_t>(c, end), [&](const tbb::blocked_range<size_t> &r) {
                    for (size_t layer = r.begin(); layer != r.end(); ++layer) {
                        if (!wants(layer)) continue;
                        const auto original = cache.at(layer);
                        const uint64_t before_hash = pixel_hash(original->px);
                        layerops::Mask mask = *original;
                        op(mask, layer, context);
                        if (mask.width != original->width || mask.height != original->height)
                            throw RuntimeError("This operation changes the mask size; use Change resolution as a layer-list operation");
                        if (pixel_hash(mask.px) == before_hash) continue;
                        quantize_mask(mask.px);
                        const auto png = PNGRasterEncoder{}(mask.px.data(), mask.width, mask.height, 1);
                        if (png.size() == 0) throw RuntimeError("Cannot encode edited layer PNG");
                        const auto *bytes = static_cast<const uint8_t *>(png.data());
                        encoded[layer - c].assign(bytes, bytes + png.size());
                    }
                });
                for (size_t layer = c; layer < end; ++layer) {
                    auto &after = encoded[layer - c];
                    if (after.empty()) continue;
                    const auto index = layer_entry(layer, plane);
                    if (m_entries[index].same_as(after)) continue;
                    add_streamed_change(changes, file, resident, {index, m_entries[index].copy(), std::move(after)});
                }
            }
        }
        if (!generation.current()) throw RuntimeError("Layer document changed during the operation");
        check_layer_progress(progress, total, total);
        commit(std::move(changes));
        return true;
    } catch (const LayerOperationCancelled &) {
        return false;
    }
}

bool LayerDocument::scan_masks(LayerMaskPlane plane, const std::function<void(size_t, const layerops::Mask &)> &visit,
                               const LayerProgress &progress) const
{
    if (!visit) throw RuntimeError("No layer visitor was given");
    const size_t count = layer_count();
    try {
        check_layer_progress(progress, 0, count);
        // Each visited layer is the decoded image plus its mask copy and decoder scratch (~3 layers): the chunk stays within the decoded budget.
        uint64_t per_layer = 1;
        if (count) {
            const auto first = m_entries.at(layer_entry(0, plane)).load();
            const auto &b = *first;
            const auto u32 = [&](size_t at) { return (uint64_t(b[at]) << 24) | (uint64_t(b[at + 1]) << 16) | (uint64_t(b[at + 2]) << 8) | uint64_t(b[at + 3]); };
            if (b.size() >= 24) per_layer = std::max<uint64_t>(1, u32(16) * u32(20));
        }
        const size_t chunk = size_t(std::max<uint64_t>(1, std::min<uint64_t>({16, uint64_t(std::max(1, tbb::this_task_arena::max_concurrency())), layer_decoded_budget() / (per_layer * 3)})));
        for (size_t c = 0; c < count; c += chunk) {
            const size_t end = std::min(count, c + chunk);
            check_layer_progress(progress, c, count);
            tbb::parallel_for(tbb::blocked_range<size_t>(c, end), [&](const tbb::blocked_range<size_t> &r) {
                for (size_t layer = r.begin(); layer != r.end(); ++layer) visit(layer, to_mask(image(layer, plane)));
            });
        }
        check_layer_progress(progress, count, count);
        return true;
    } catch (const LayerOperationCancelled &) {
        return false;
    }
}

bool LayerDocument::apply_mask_kernel(LayerMaskPlane plane, const std::vector<double> &z_mm, const MaskKernelFactory &factory,
                                      const LayerProgress &progress, const MaskBatchOperation &fallback)
{
    if (!factory) throw RuntimeError("No layer operation was given");
    const size_t count = layer_count();
    if (count == 0) throw RuntimeError("The document has no layers");
    layerops::StreamLayout layout;
    layout.layer_count = count;
    layout.z_mm = z_mm;
    {
        const auto first = image(0, plane);
        layout.width = size_t(first.cols); layout.height = size_t(first.rows);
    }
    layout.fetch = [this, plane](size_t layer) { return std::make_shared<const layerops::Mask>(to_mask(image(layer, plane))); };
    layout.scan = [this, plane, &progress](const std::function<void(size_t, const layerops::Mask &)> &visit) {
        if (!scan_masks(plane, visit, progress)) throw LayerOperationCancelled{};
    };
    try {
        const layerops::StreamKernel kernel = factory(layout);
        MaskWindowSpec spec;
        spec.first = kernel.first; spec.last = kernel.last; spec.before = kernel.before; spec.after = kernel.after;
        spec.applies = kernel.applies;
        spec.neighbour_crop = kernel.fetch_crop;
        return apply_mask_window(plane, spec, [&kernel](layerops::Mask &mask, size_t layer, const MaskStreamContext &ctx) {
            mask = kernel.run(layer, mask, *ctx.fetch);
        }, progress);
    } catch (const LayerOperationCancelled &) {
        return false;
    } catch (const layerops::NotStreamable &) {
        if (!fallback) throw;
        return apply_mask_batch(plane, fallback, progress);
    }
}
std::string LayerDocument::layer_list_refusal(bool changes_resolution) const
{
    if (m_layers.empty()) return "The document has no layers.";
    if (!m_archive_format.empty())
        return "This document was opened from a '" + m_archive_format + "' file. Its layer count, heights and resolution "
               "are stored in native headers that are written back unchanged, so they cannot be edited without "
               "corrupting the file. Edit pixels instead, or slice a model and use the Sliced editor.";
    bool plain = m_secondary_layers.empty() && m_layer_parameters.empty() && m_entries.size() == m_layers.size();
    for (size_t i = 0; plain && i < m_layers.size(); ++i) plain = m_layers[i] == i;
    if (!plain) return "This document has an unsupported internal layout for layer-list operations.";
    if (changes_resolution && m_fresh_job)
        return "Changing the resolution is not available for sliced or generated documents: their pixel size is tied to "
               "the printer's display. Change the printer profile and slice again.";
    return {};
}

bool LayerDocument::apply_layer_list_operation(const LayerListOperation &op, bool changes_resolution,
                                               double fallback_layer_height_mm, const LayerProgress &progress)
{
    if (!op) throw RuntimeError("No layer operation was given");
    if (const auto reason = layer_list_refusal(changes_resolution); !reason.empty()) throw RuntimeError(reason);
    if (!(fallback_layer_height_mm > 0) || !std::isfinite(fallback_layer_height_mm))
        throw RuntimeError("The fallback layer height must be positive");
    const size_t count = layer_count();
    const auto generation = export_generation_guard();
    using F = LayerParameter;
    try {
        check_layer_progress(progress, 0, 100);
        std::vector<layerops::Mask> masks;
        masks.reserve(count);
        std::vector<uint64_t> original_hash;
        std::vector<std::pair<size_t, size_t>> original_size;
        std::vector<double> z_mm(count);
        uint64_t decoded_bytes = 0;
        const auto decode_progress = phase_progress(progress, 0, 30);
        for (size_t layer = 0; layer < count; ++layer) {
            check_layer_progress(decode_progress, layer, count);
            masks.push_back(to_mask(image(layer)));
            decoded_bytes += masks.back().px.size();
            if (decoded_bytes > layer_decoded_budget())
                throw RuntimeError("The document is too large for a layer operation (over the decoded-pixel budget of "+layer_mib(layer_decoded_budget())+")");
            original_hash.push_back(pixel_hash(masks.back().px));
            original_size.emplace_back(masks.back().width, masks.back().height);
            z_mm[layer] = m_fresh_job ? m_fresh_job->rows[layer].at(F::PositionZ) : fallback_layer_height_mm * double(layer + 1);
        }
        const size_t old_bottom = m_fresh_job ? m_fresh_job->globals.bottom_layers : 0;
        const size_t old_transition = m_fresh_job ? m_fresh_job->globals.transition_layers : 0;
        check_layer_progress(progress, 30, 100);
        const LayerListChange change = op(masks, z_mm, phase_progress(progress, 30, 50));
        const size_t out = masks.size();
        commit_layer_list(change, z_mm, count, out, [&](size_t i) {
            auto &mask = masks[i];
            const size_t src = change.list.source_index[i];
            Entry entry{"layer" + std::to_string(i), {}};
            const bool unchanged = mask.width == original_size[src].first && mask.height == original_size[src].second &&
                                   pixel_hash(mask.px) == original_hash[src];
            if (unchanged) {
                const auto &from = m_entries[m_layers[src]];
                entry.bytes = from.bytes; entry.stored = from.stored; // keeps the exact original PNG
                entry.statistics = m_entries[m_layers[src]].statistics;
            } else {
                quantize_mask(mask.px);
                const auto encoded = PNGRasterEncoder{}(mask.px.data(), mask.width, mask.height, 1);
                if (encoded.size() == 0) throw RuntimeError("Cannot encode edited layer PNG");
                const auto *bytes = static_cast<const uint8_t *>(encoded.data());
                entry.bytes.assign(bytes, bytes + encoded.size());
            }
            return entry;
        }, old_bottom, old_transition, generation, progress);
        return true;
    } catch (const LayerOperationCancelled &) {
        return false;
    }
}

void LayerDocument::commit_layer_list(const LayerListChange &change, const std::vector<double> &z_mm, size_t count, size_t out,
                                      const std::function<Entry(size_t)> &make_entry, size_t old_bottom, size_t old_transition,
                                      const ExportGenerationGuard &generation, const LayerProgress &progress)
{
    using F = LayerParameter;
    const auto &source = change.list.source_index;
    if (out == 0 || z_mm.size() != out || source.size() != out)
        throw RuntimeError("The operation returned an inconsistent layer list");
    for (size_t s : source)
        if (s >= count) throw RuntimeError("The operation returned an invalid source layer");
    for (const auto *over : {&change.exposure_s, &change.lift_mm, &change.wait_before_cure_s})
        if (!over->empty() && over->size() != out)
            throw RuntimeError("The operation returned inconsistent per-layer parameters");
    if (!change.row_patch.empty() && change.row_patch.size() != out)
        throw RuntimeError("The operation returned inconsistent per-layer parameters");

    // Build the new layout off to the side; the live document is untouched until commit.
    auto next = std::make_shared<Structure>();
    next->entries.reserve(out);
    uint64_t total_bytes = 0;
    for (size_t i = 0; i < out; ++i) {
        check_layer_progress(progress, 80 + 15 * i / out, 100);
        Entry entry = make_entry(i);
        total_bytes += entry.size();
        next->entries.push_back(std::move(entry));
        next->layers.push_back(i);
    }
    if (total_bytes > layer_editor_byte_budget()) throw RuntimeError("Edited document exceeds the editor memory budget");
    if (m_fresh_job) {
        FreshLayerJob job = *m_fresh_job;
        job.rows.clear();
        bool repeated_z = false;
        for (size_t i = 0; i < out; ++i) {
            LayerParameters row = m_fresh_job->rows[source[i]]; // per-layer parameters follow their layer
            row[F::PositionZ] = z_mm[i];
            if (!change.row_patch.empty())
                for (const auto &[field, value] : change.row_patch[i]) {
                    // A field the row does not carry cannot be stored: refuse instead of inventing it.
                    if (!row.count(field)) throw RuntimeError("This document cannot store a per-layer value the operation produced");
                    row[field] = value;
                }
            if (!change.exposure_s.empty() && change.exposure_s[i] >= 0) row[F::Exposure] = change.exposure_s[i];
            if (!change.wait_before_cure_s.empty() && change.wait_before_cure_s[i] >= 0)
                row[F::WaitBefore] = change.wait_before_cure_s[i];
            if (!change.lift_mm.empty() && change.lift_mm[i] >= 0) {
                // Overrides are the TOTAL lift: keep one lift phase so the table stays valid.
                row[F::LiftDistance]  = change.lift_mm[i];
                row[F::LiftDistance2] = 0;
                row[F::RetractDistance2] = std::min(row[F::RetractDistance2], change.lift_mm[i]);
            }
            if (i > 0 && z_mm[i] <= z_mm[i - 1]) repeated_z = true;
            job.rows.push_back(std::move(row));
        }
        if (repeated_z) job.position_policy = ExposurePositionPolicy::RepeatedExposureHeights;
        // Edited counts/heights must never be overwritten by automatic model parameter refresh.
        job.parameter_origin = FreshLayerParameterOrigin::Explicit;
        if (change.bottom_layer_count) job.globals.bottom_layers = std::min(*change.bottom_layer_count, out);
        else job.globals.bottom_layers = size_t(std::count_if(source.begin(), source.end(),
                                                [&](size_t s) { return s < old_bottom; }));
        job.globals.transition_layers = std::min(old_transition, out - job.globals.bottom_layers);
        if (const auto height = dominant_layer_height(z_mm)) job.globals.layer_height = *height;
        try { job.validate(out); }
        catch (const RuntimeError &error) {
            throw RuntimeError(std::string("The result cannot be stored in this document: ") + error.what());
        }
        next->fresh_job = std::move(job);
    }
    if (!generation.current()) throw RuntimeError("Layer document changed during the operation");
    check_layer_progress(progress, 100, 100);

    // Commit: the old layout becomes the undo payload. Reserve history slots first so the
    // swap below cannot fail half-way.
    m_undo.reserve(m_undo.size() + 1);
    const uint64_t next_revision = m_next_revision + 1;
    m_export_source.invalidate();
    std::shared_ptr<Structure> stored = std::move(next); // currently holds the NEW layout
    exchange_structure(stored);                          // live <- new layout, stored <- old layout
    m_undo.push_back({{}, m_revision, next_revision, std::move(stored)});
    trim_undo_history();
    m_redo.clear();
    spill_payloads();
    m_revision = m_next_revision = next_revision;
}

namespace {
// Fields of LayerParams that map 1:1 onto canonical layer parameters. LightOff has no LayerParams
// counterpart; the operations never touch it, so it simply stays as stored.
using ParamMember = double layerops::LayerParams::*;
const std::array<std::pair<LayerParameter, ParamMember>, 11> &parameter_members()
{
    using F = LayerParameter;
    using P = layerops::LayerParams;
    static const std::array<std::pair<LayerParameter, ParamMember>, 11> members{{
        {F::Exposure, &P::exposure_s}, {F::LiftDistance, &P::lift_height_mm}, {F::LiftSpeed, &P::lift_speed},
        {F::WaitBefore, &P::wait_before_cure_s}, {F::WaitAfter, &P::wait_after_cure_s},
        {F::WaitAfterLift, &P::wait_after_lift_s}, {F::LiftDistance2, &P::lift_height2_mm},
        {F::LiftSpeed2, &P::lift_speed2}, {F::RetractSpeed, &P::retract_speed},
        {F::RetractDistance2, &P::retract_height2_mm}, {F::RetractSpeed2, &P::retract_speed2}}};
    return members;
}
const char *parameter_field_name(LayerParameter field)
{
    switch (field) {
    case LayerParameter::PositionZ: return "layer Z position";
    case LayerParameter::Exposure: return "exposure time";
    case LayerParameter::LiftDistance: return "lift distance";
    case LayerParameter::LiftSpeed: return "lift speed";
    case LayerParameter::LightOff: return "light-off delay";
    case LayerParameter::WaitBefore: return "wait before cure";
    case LayerParameter::WaitAfter: return "wait after cure";
    case LayerParameter::WaitAfterLift: return "wait after lift";
    case LayerParameter::LiftDistance2: return "second lift distance";
    case LayerParameter::LiftSpeed2: return "second lift speed";
    case LayerParameter::RetractSpeed: return "retract speed";
    case LayerParameter::RetractDistance2: return "second retract distance";
    case LayerParameter::RetractSpeed2: return "second retract speed";
    case LayerParameter::LightPWM: return "light PWM";
    }
    return "parameter";
}
layerops::LayerParams to_layer_params(const LayerParameters &row, bool is_bottom)
{
    layerops::LayerParams params;
    params.is_bottom = is_bottom;
    for (const auto &[field, member] : parameter_members())
        if (const auto found = row.find(field); found != row.end()) params.*member = found->second;
    if (const auto found = row.find(LayerParameter::LightPWM); found != row.end())
        params.light_pwm = uint8_t(std::clamp(std::llround(found->second), 0LL, 255LL));
    return params;
}
// Fields whose value differs between two parameter sets (the operations round to 2 decimals, so
// anything below 1e-9 is noise from the floating point round trip).
LayerParameters parameter_patch(const layerops::LayerParams &before, const layerops::LayerParams &after)
{
    LayerParameters patch;
    for (const auto &[field, member] : parameter_members())
        if (std::abs(before.*member - after.*member) > 1e-9) patch[field] = after.*member;
    if (before.light_pwm != after.light_pwm) patch[LayerParameter::LightPWM] = double(after.light_pwm);
    return patch;
}
// Refuses (with the field named) a changed value the format cannot store.
void require_storable(const LayerParameters &patch, const LayerParameters &row,
                      const LayerParameterCapabilities &capabilities, size_t layer)
{
    for (const auto &[field, value] : patch) {
        (void) value;
        const auto cap = std::find_if(capabilities.begin(), capabilities.end(),
                                      [&](const auto &c) { return c.field == field; });
        const std::string where = " (layer " + std::to_string(layer + 1) + ")";
        if (cap == capabilities.end() || !row.count(field))
            throw RuntimeError(std::string("This format cannot store the ") + parameter_field_name(field) +
                               " per layer, so the operation cannot be applied" + where + ".");
        if (!cap->editable)
            throw RuntimeError(std::string("The ") + parameter_field_name(field) + " of this document is read-only" +
                               (field == LayerParameter::PositionZ ?
                                    "; use a layer-list operation (Change layer height, Clone, ...) to move layers" : "") +
                               where + ".");
    }
}
} // namespace

std::string LayerDocument::parameter_operation_refusal() const
{
    if (m_layers.empty()) return "The document has no layers.";
    if (!m_fresh_job && m_layer_parameters.empty())
        return "This document stores no per-layer print parameters (exposure, lifts, waits), so parameter "
               "operations have nothing to edit. Open a printer file with a per-layer table, or slice a model.";
    if (parameter_capabilities().empty())
        return "This format has no editable per-layer parameters.";
    return {};
}

std::vector<layerops::LayerParams> LayerDocument::parameter_table() const
{
    std::vector<layerops::LayerParams> table;
    if (!parameter_operation_refusal().empty()) return table;
    size_t bottom = 0;
    if (m_fresh_job) bottom = m_fresh_job->globals.bottom_layers;
    else if (const auto native = native_bottom_layers(m_archive_format, native_original())) bottom = *native;
    else if (const auto job = job_parameters()) bottom = job->bottom_layers;
    table.reserve(layer_count());
    for (size_t i = 0; i < layer_count(); ++i) table.push_back(to_layer_params(layer_parameters(i), i < bottom));
    return table;
}

layerops::PrinterContext LayerDocument::parameter_printer_context(double fallback_layer_height_mm) const
{
    layerops::PrinterContext context;
    context.layer_height_mm = fallback_layer_height_mm;
    if (m_fresh_job && m_fresh_job->globals.layer_height > 0) context.layer_height_mm = m_fresh_job->globals.layer_height;
    const auto capabilities = parameter_capabilities();
    // An optional field the format cannot store must not be produced by an operation.
    auto can = [&](LayerParameter field) {
        return std::any_of(capabilities.begin(), capabilities.end(),
                           [&](const auto &c) { return c.field == field && c.editable; });
    };
    context.can_use_lift_height2 = can(LayerParameter::LiftDistance2);
    context.can_use_lift_speed2 = can(LayerParameter::LiftSpeed2);
    context.can_use_retract_height2 = can(LayerParameter::RetractDistance2);
    context.can_use_retract_speed2 = can(LayerParameter::RetractSpeed2);
    context.can_use_wait_after_lift = can(LayerParameter::WaitAfterLift);
    context.can_use_wait_before_cure = can(LayerParameter::WaitBefore);
    return context;
}

bool LayerDocument::apply_parameter_operation(const ParameterOperation &op, bool needs_masks,
                                              double fallback_layer_height_mm, const LayerProgress &progress)
{
    if (!op) throw RuntimeError("No layer operation was given");
    if (const auto reason = parameter_operation_refusal(); !reason.empty()) throw RuntimeError(reason);
    if (!(fallback_layer_height_mm > 0) || !std::isfinite(fallback_layer_height_mm))
        throw RuntimeError("The fallback layer height must be positive");
    const size_t count = layer_count();
    const auto generation = export_generation_guard();
    try {
        check_layer_progress(progress, 0, 100);
        const auto before = parameter_table();
        std::vector<LayerParameters> rows(count);
        std::vector<double> z_mm(count);
        for (size_t i = 0; i < count; ++i) {
            rows[i] = layer_parameters(i);
            z_mm[i] = rows[i].count(LayerParameter::PositionZ) ? rows[i].at(LayerParameter::PositionZ)
                                                              : fallback_layer_height_mm * double(i + 1);
        }
        const auto original_z = z_mm;
        std::vector<layerops::Mask> masks;
        std::vector<uint64_t> original_hash;
        if (needs_masks) {
            masks.reserve(count);
            uint64_t decoded_bytes = 0;
            const auto decode_progress = phase_progress(progress, 0, 40);
            for (size_t layer = 0; layer < count; ++layer) {
                check_layer_progress(decode_progress, layer, count);
                masks.push_back(to_mask(image(layer)));
                decoded_bytes += masks.back().px.size();
                if (decoded_bytes > layer_decoded_budget())
                    throw RuntimeError("The document is too large for this operation (over the decoded-pixel budget of "+layer_mib(layer_decoded_budget())+")");
                original_hash.push_back(pixel_hash(masks.back().px));
            }
        }
        auto table = before;
        const LayerListChange change = op(masks, z_mm, table, phase_progress(progress, 40, 50));
        const bool identity = change.list.source_index.empty() ||
            (change.list.source_index.size() == count &&
             [&] { for (size_t i = 0; i < count; ++i) if (change.list.source_index[i] != i) return false; return true; }());
        if (!identity || table.size() != count || z_mm.size() != count || (needs_masks && masks.size() != count))
            throw RuntimeError("This operation changes the layer list; it must be applied as a layer-list operation");
        for (size_t i = 0; needs_masks && i < count; ++i)
            if (pixel_hash(masks[i].px) != original_hash[i])
                throw RuntimeError("This operation changes pixels; it must be applied as a pixel operation");
        if (change.bottom_layer_count) {
            std::optional<size_t> native;
            if (!m_fresh_job) native = native_bottom_layers(m_archive_format, native_original());
            const size_t current = m_fresh_job ? m_fresh_job->globals.bottom_layers :
                                   native ? *native : job_parameters() ? job_parameters()->bottom_layers : 0;
            if (*change.bottom_layer_count != current)
                throw RuntimeError("The bottom layer count of this document cannot be changed by a parameter edit.");
        }
        check_layer_progress(progress, 60, 100);
        const auto capabilities = parameter_capabilities();
        std::vector<LayerParameters> next = rows;
        bool any = false;
        for (size_t i = 0; i < count; ++i) {
            auto patch = parameter_patch(before[i], table[i]);
            if (std::abs(z_mm[i] - original_z[i]) > 1e-9) patch[LayerParameter::PositionZ] = z_mm[i];
            if (patch.empty()) continue;
            require_storable(patch, rows[i], capabilities, i);
            for (const auto &[field, value] : patch) next[i][field] = value;
            any = true;
        }
        if (!generation.current()) throw RuntimeError("Layer document changed during the operation");
        check_layer_progress(progress, 100, 100);
        if (!any) return true; // nothing differs from the stored values: no empty undo step
        replace_layer_parameter_table(next); // one commit = one undo step; validates the whole table
        return true;
    } catch (const LayerOperationCancelled &) {
        return false;
    }
}

bool LayerDocument::apply_parameter_list_operation(const ParameterOperation &op, double fallback_layer_height_mm,
                                                   const LayerProgress &progress)
{
    if (!op) throw RuntimeError("No layer operation was given");
    if (!m_fresh_job)
        throw RuntimeError("Layer-list parameter operations need a sliced or generated document: files opened from a "
                           "container keep their layer count and heights in native headers.");
    if (const auto reason = parameter_operation_refusal(); !reason.empty()) throw RuntimeError(reason);
    const auto before = parameter_table();
    const auto capabilities = parameter_capabilities();
    return apply_layer_list_operation(
        [&](std::vector<layerops::Mask> &masks, std::vector<double> &z_mm, const LayerProgress &inner) {
            auto table = before;
            LayerListChange change = op(masks, z_mm, table, inner);
            const size_t out = masks.size();
            if (table.size() != out || z_mm.size() != out || change.list.source_index.size() != out)
                throw RuntimeError("The operation returned an inconsistent layer list");
            change.row_patch.assign(out, {});
            for (size_t i = 0; i < out; ++i) {
                const size_t source = change.list.source_index[i];
                if (source >= before.size()) throw RuntimeError("The operation returned an invalid source layer");
                change.row_patch[i] = parameter_patch(before[source], table[i]);
                require_storable(change.row_patch[i], m_fresh_job->rows[source], capabilities, i);
            }
            if (!change.bottom_layer_count)
                change.bottom_layer_count = size_t(std::count_if(table.begin(), table.end(),
                                                                 [](const auto &p) { return p.is_bottom; }));
            return change;
        },
        false, fallback_layer_height_mm, progress);
}

bool LayerDocument::run_list_stream(const ListStreamOperation &op, bool changes_resolution, bool with_table,
                                    double fallback_layer_height_mm, const LayerProgress &progress, bool pixels_only)
{
    if (!op) throw RuntimeError("No layer operation was given");
    if (pixels_only) {
        if (m_layers.empty()) throw RuntimeError("The document has no layers.");
    } else if (const auto reason = layer_list_refusal(changes_resolution); !reason.empty()) throw RuntimeError(reason);
    if (!(fallback_layer_height_mm > 0) || !std::isfinite(fallback_layer_height_mm))
        throw RuntimeError("The fallback layer height must be positive");
    if (with_table) {
        if (!m_fresh_job)
            throw RuntimeError("Layer-list parameter operations need a sliced or generated document: files opened from a "
                               "container keep their layer count and heights in native headers.");
        if (const auto reason = parameter_operation_refusal(); !reason.empty()) throw RuntimeError(reason);
    }
    using F = LayerParameter;
    const size_t count = layer_count();
    const auto generation = export_generation_guard();
    try {
        check_layer_progress(progress, 0, 100);
        ListStreamContext ctx;
        ctx.layer_count = count;
        {
            const auto loaded = m_entries.at(m_layers.at(0)).load();
            const auto &b = *loaded;
            if (b.size() < 24) throw RuntimeError("Invalid layer PNG");
            const auto u32 = [&](size_t at) { return (size_t(b[at]) << 24) | (size_t(b[at + 1]) << 16) | (size_t(b[at + 2]) << 8) | size_t(b[at + 3]); };
            ctx.width = u32(16); ctx.height = u32(20);
        }
        ctx.z_mm.resize(count);
        for (size_t layer = 0; layer < count; ++layer) {
            if (m_fresh_job) { ctx.z_mm[layer] = m_fresh_job->rows[layer].at(F::PositionZ); continue; }
            const auto row = pixels_only ? layer_parameters(layer) : LayerParameters{};
            ctx.z_mm[layer] = row.count(F::PositionZ) ? row.at(F::PositionZ) : fallback_layer_height_mm * double(layer + 1);
        }
        const auto before_table = with_table ? parameter_table() : std::vector<layerops::LayerParams>{};
        ctx.table = before_table;
        ctx.progress = phase_progress(progress, 0, 80);
        const size_t old_bottom = m_fresh_job ? m_fresh_job->globals.bottom_layers : 0;
        const size_t old_transition = m_fresh_job ? m_fresh_job->globals.transition_layers : 0;

        std::mutex mutex;
        std::deque<Entry> emitted; // stable references: layers are read back while others are being stored
        uint64_t resident = 0;
        ctx.load = [this](size_t layer) {
            if (layer >= m_layers.size()) throw RuntimeError("The operation asked for a layer outside of the document");
            return to_mask(image(layer));
        };
        const auto store = [&](layerops::Mask &&mask, std::optional<size_t> source, bool scratch) -> LayerRef {
            if (source) {
                const auto original = to_mask(image(*source));
                if (mask.width == original.width && mask.height == original.height && mask.px == original.px) return {*source, false};
            }
            if (!scratch) quantize_mask(mask.px);
            const auto encoded = PNGRasterEncoder{}(mask.px.data(), mask.width, mask.height, 1);
            if (encoded.size() == 0) throw RuntimeError("Cannot encode edited layer PNG");
            const auto *bytes = static_cast<const uint8_t *>(encoded.data());
            Entry entry{"", {}};
            std::lock_guard<std::mutex> lock(mutex);
            store_payload(entry, std::vector<uint8_t>(bytes, bytes + encoded.size()), resident);
            emitted.push_back(std::move(entry));
            return {emitted.size() - 1, true};
        };
        ctx.emit = [&](layerops::Mask &&mask, std::optional<size_t> source) { return store(std::move(mask), source, false); };
        ctx.emit_scratch = [&](layerops::Mask &&mask, std::optional<size_t> source) { return store(std::move(mask), source, true); };
        ctx.load_ref = [&](LayerRef ref) -> layerops::Mask {
            if (!ref.emitted) return ctx.load(ref.index);
            std::shared_ptr<const std::vector<uint8_t>> bytes;
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (ref.index >= emitted.size()) throw RuntimeError("The operation asked for a layer that was not stored");
                bytes = emitted[ref.index].load();
            }
            png::ImageGreyscale decoded;
            if (!png::decode_png(png::ReadBuf{bytes->data(), bytes->size()}, decoded)) throw RuntimeError("Cannot decode a stored layer");
            return to_mask(decoded);
        };
        ctx.parallel = [&](size_t first, size_t last, size_t buffers, const std::function<void(size_t)> &fn) {
            if (first > last) return;
            const uint64_t per_layer = std::max<uint64_t>(1, uint64_t(ctx.width) * ctx.height);
            const uint64_t budget_layers = layer_decoded_budget() / per_layer;
            const uint64_t need = std::max<uint64_t>(1, buffers);
            if (budget_layers < need)
                throw RuntimeError("The document is too large for this operation: one layer needs " + std::to_string(need) +
                                   " decoded layers at once, over the decoded-pixel budget of " + layer_mib(layer_decoded_budget()));
            const size_t chunk = size_t(std::min<uint64_t>({budget_layers / need, 16, uint64_t(std::max(1, tbb::this_task_arena::max_concurrency())), last - first + 1}));
            const size_t total = last - first + 1;
            for (size_t c = first; c <= last; c += chunk) {
                check_layer_progress(ctx.progress, c - first, total);
                const size_t end = std::min(last + 1, c + chunk);
                tbb::parallel_for(tbb::blocked_range<size_t>(c, end), [&](const tbb::blocked_range<size_t> &r) {
                    for (size_t i = r.begin(); i != r.end(); ++i) fn(i);
                });
            }
            check_layer_progress(ctx.progress, total, total);
        };
        ListStreamResult result = op(ctx);
        if (pixels_only) {
            if (result.refs.size() != count) throw RuntimeError("This operation changes the layer count; it must be applied as a layer-list operation");
            std::vector<Change> changes;
            std::shared_ptr<HistorySpill> history_file;
            uint64_t history_resident = 0;
            for (size_t layer = 0; layer < count; ++layer) {
                const LayerRef &ref = result.refs[layer];
                if (!ref.emitted) {
                    if (ref.index != layer) throw RuntimeError("This operation changes the layer list; it must be applied as a layer-list operation");
                    continue;
                }
                if (ref.index >= emitted.size()) throw RuntimeError("The operation returned an invalid layer reference");
                const auto index = layer_entry(layer, LayerMaskPlane::Main);
                std::vector<uint8_t> after = *emitted[ref.index].load();
                if (m_entries[index].same_as(after)) continue;
                add_streamed_change(changes, history_file, history_resident, {index, m_entries[index].copy(), std::move(after)});
            }
            if (!generation.current()) throw RuntimeError("Layer document changed during the operation");
            check_layer_progress(progress, 100, 100);
            commit(std::move(changes));
            return true;
        }
        const size_t out = result.refs.size();
        if (out == 0 || result.z_mm.size() != out || result.change.list.source_index.size() != out ||
            (with_table && result.table.size() != out))
            throw RuntimeError("The operation returned an inconsistent layer list");
        for (const auto &ref : result.refs)
            if (ref.index >= (ref.emitted ? emitted.size() : count)) throw RuntimeError("The operation returned an invalid layer reference");
        if (with_table) {
            const auto capabilities = parameter_capabilities();
            auto &change = result.change;
            change.row_patch.assign(out, {});
            for (size_t i = 0; i < out; ++i) {
                const size_t source = change.list.source_index[i];
                if (source >= before_table.size()) throw RuntimeError("The operation returned an invalid source layer");
                change.row_patch[i] = parameter_patch(before_table[source], result.table[i]);
                require_storable(change.row_patch[i], m_fresh_job->rows[source], capabilities, i);
            }
            if (!change.bottom_layer_count)
                change.bottom_layer_count = size_t(std::count_if(result.table.begin(), result.table.end(),
                                                                 [](const auto &p) { return p.is_bottom; }));
        }
        std::vector<size_t> uses(emitted.size(), 0);
        for (const auto &ref : result.refs) if (ref.emitted) ++uses[ref.index];
        uint64_t copy_resident = resident;
        commit_layer_list(result.change, result.z_mm, count, out, [&](size_t i) {
            const LayerRef &ref = result.refs[i];
            if (ref.emitted) {
                Entry entry = --uses[ref.index] == 0 ? std::move(emitted[ref.index]) : emitted[ref.index];
                entry.name = "layer" + std::to_string(i);
                return entry;
            }
            const auto &from = m_entries[m_layers[ref.index]];
            Entry entry{"layer" + std::to_string(i), {}};
            entry.statistics = from.statistics;
            if (from.stored) entry.stored = from.stored; // keeps the exact original PNG
            else store_payload(entry, from.bytes, copy_resident);
            return entry;
        }, old_bottom, old_transition, generation, progress);
        return true;
    } catch (const LayerOperationCancelled &) {
        return false;
    }
}

bool LayerDocument::apply_layer_list_stream(const ListStreamOperation &op, bool changes_resolution,
                                            double fallback_layer_height_mm, const LayerProgress &progress)
{
    return run_list_stream(op, changes_resolution, false, fallback_layer_height_mm, progress);
}

bool LayerDocument::apply_pixel_list_stream(const ListStreamOperation &op, double fallback_layer_height_mm, const LayerProgress &progress)
{
    return run_list_stream(op, false, false, fallback_layer_height_mm, progress, true);
}

bool LayerDocument::apply_parameter_list_stream(const ListStreamOperation &op, double fallback_layer_height_mm,
                                                const LayerProgress &progress)
{
    return run_list_stream(op, false, true, fallback_layer_height_mm, progress);
}

LayerDocument LayerDocument::from_parameter_masks(const FreshLayerJob &tmpl, const std::vector<layerops::Mask> &masks,
                                                  const std::vector<double> &z_mm,
                                                  const std::vector<layerops::LayerParams> &table,
                                                  const LayerProgress &progress)
{
    const size_t count = masks.size();
    if (count == 0 || z_mm.size() != count || table.size() != count)
        throw RuntimeError("The generated layers, heights and parameters do not match");
    if (tmpl.rows.empty()) throw RuntimeError("The template document stores no per-layer parameters");
    FreshLayerJob job = tmpl;
    job.producer = FreshLayerProducer::Raster;
    job.parameter_origin = FreshLayerParameterOrigin::Explicit;
    const LayerParameters base = tmpl.rows.front();
    const auto base_params = to_layer_params(base, false);
    job.rows.clear();
    std::vector<EncodedRaster> encoded;
    encoded.reserve(count);
    bool repeated_z = false;
    for (size_t i = 0; i < count; ++i) {
        check_layer_progress(progress, i, count);
        const auto &mask = masks[i];
        if (mask.width != tmpl.geometry.columns || mask.height != tmpl.geometry.rows)
            throw RuntimeError("A generated layer does not match the printer resolution of the template");
        LayerParameters row = base;
        for (const auto &[field, value] : parameter_patch(base_params, table[i])) row[field] = value;
        row[LayerParameter::PositionZ] = z_mm[i];
        if (i > 0 && z_mm[i] <= z_mm[i - 1]) repeated_z = true;
        job.rows.push_back(std::move(row));
        auto png = PNGRasterEncoder{}(mask.px.data(), mask.width, mask.height, 1);
        if (png.size() == 0) throw RuntimeError("Cannot encode generated layer PNG");
        encoded.push_back(std::move(png));
    }
    if (repeated_z) job.position_policy = ExposurePositionPolicy::RepeatedExposureHeights;
    job.globals.bottom_layers = size_t(std::count_if(table.begin(), table.end(), [](const auto &p) { return p.is_bottom; }));
    job.globals.transition_layers = std::min(job.globals.transition_layers, count - job.globals.bottom_layers);
    if (const auto height = dominant_layer_height(z_mm)) job.globals.layer_height = *height;
    return from_generated_rasters(std::move(encoded), std::move(job), progress);
}

bool LayerDocument::undo()
{
    if (m_undo.empty()) return false;
    // Allocate every replacement and the destination history slot before
    // touching any layer: allocation failure must preserve the whole edit.
    auto replacements = prepare_replacements(m_undo.back().changes, false);
    validate_metadata_edit(m_undo.back().changes, replacements);
    m_redo.reserve(m_redo.size() + 1);
    m_export_source.invalidate();
    if (m_undo.back().structure) exchange_structure(m_undo.back().structure);
    auto edit = std::move(m_undo.back()); m_undo.pop_back();
    apply_replacements(edit.changes, replacements);
    m_revision = edit.before_revision;
    m_redo.push_back(std::move(edit)); return true;
}
bool LayerDocument::redo()
{
    if (m_redo.empty()) return false;
    auto replacements = prepare_replacements(m_redo.back().changes, true);
    validate_metadata_edit(m_redo.back().changes, replacements);
    m_undo.reserve(m_undo.size() + 1);
    m_export_source.invalidate();
    if (m_redo.back().structure) exchange_structure(m_redo.back().structure);
    auto edit = std::move(m_redo.back()); m_redo.pop_back();
    apply_replacements(edit.changes, replacements);
    m_revision = edit.after_revision;
    m_undo.push_back(std::move(edit)); return true;
}
bool LayerDocument::export_images(const std::string &path, const std::string &format,
                                  size_t first, size_t last,
                                  const std::function<bool(size_t, size_t)> &progress, LayerMaskPlane plane) const
{
    if (first > last || last >= layer_count()) throw RuntimeError("Invalid layer image export range");
    const auto *entry = find_layer_image_format(format);
    if (!entry) throw RuntimeError("Unknown layer image format");
    const boost::filesystem::path output(path);
    const bool range = first != last;
    if (range && boost::filesystem::exists(output))
        throw RuntimeError("Choose a new folder for the mask range; existing folders are preserved");
    if (!range && !boost::algorithm::iequals(output.extension().string(), std::string(".")+entry->id))
        throw RuntimeError("The output extension must match the image format");
    const auto temporary = output.parent_path() / boost::filesystem::unique_path(".sla-masks-%%%%-%%%%.tmp");
    // Directory publication is one rename. Failure/cancel during staging never
    // leaves a partial range next to the user's previously exported masks.
    try {
        if (range) boost::filesystem::create_directory(temporary);
        const size_t count = last-first+1;
        for (size_t n=0; n<count; ++n) {
            if (progress && !progress(n,count)) { boost::filesystem::remove_all(temporary); return false; }
            const auto encoded = encode_layer_image(image(first+n,plane), entry->id);
            std::ostringstream name;
            name << "layer-" << std::setfill('0') << std::setw(6) << first+n+1 << '.' << entry->id;
            const auto target = range ? temporary / name.str() : temporary;
            boost::nowide::ofstream stream(target.string(), std::ios::binary | std::ios::trunc);
            stream.write(reinterpret_cast<const char *>(encoded.data()), encoded.size());
            stream.close();
            if (!stream) throw FileIOError("Cannot write layer image export");
        }
        if (progress && !progress(count,count)) { boost::filesystem::remove_all(temporary); return false; }
        if (range) {
            // Recheck just before publishing; a folder created during staging
            // must never be replaced, even if it is empty.
            if (boost::filesystem::exists(output)) throw RuntimeError("The mask export folder already exists");
            boost::filesystem::rename(temporary,output);
        } else if (const auto error = Slic3r::rename_file(temporary.string(), output.string()); error)
            throw FileIOError("Cannot publish layer image export: " + error.message());
        return true;
    } catch (...) {
        boost::system::error_code ignored; boost::filesystem::remove_all(temporary,ignored);
        throw;
    }
}

void LayerDocument::save_sl1(const std::string &path, const LayerProgress &progress)
{
    if (m_archive_format != "SL1") throw RuntimeError("This document has no original SL1 metadata");
    save_archive(path, progress);
}

void LayerDocument::save_archive(const std::string &path, const LayerProgress &progress,
    const LayerExportProfile *profile, const LayerJobParameters *globals)
{
    const auto generation = export_generation_guard();
    const auto guarded_progress = [&](size_t done, size_t total) {
        generation.check();
        check_layer_progress(progress, done, total);
        generation.check();
        return true;
    };
    save_archive_impl(path, guarded_progress, profile, globals, nullptr, nullptr, &generation);
    mark_saved_if_current(generation);
}

bool LayerDocument::mark_saved_if_current(const ExportGenerationGuard &generation)
{
    const auto state = m_export_source.m_state;
    if (!state) return false;
    std::lock_guard<std::mutex> lock(state->commit_mutex);
    if (!ExportInputGuard(state).same_source(generation) || !generation.current()) return false;
    m_saved_revision = m_revision;
    return true;
}

void LayerDocument::save_converted_ctb_encrypted(const std::string &path,
    const std::vector<LayerParameters> &table, const LayerJobParameters *globals,
    const FreshLayerMaterialPricing *pricing, const LayerProgress &progress)
{
    check_layer_progress(progress);
    if (!is_ctb_encrypted_layer_format(m_archive_format) || table.size()!=layer_count())
        throw RuntimeError("Converted AES table must match a new native container");
    const auto schema=chitubox_parameter_capabilities(4);
    const CTBEncryptedLayerLayout layout(native_original());
    double previous=0;
    size_t checked=0;
    for (const auto &row:table) {
        check_layer_progress(progress,checked++,table.size());
        if (row.size()!=schema.size()) throw RuntimeError("Converted AES requires a complete parameter row");
        for (const auto &capability:schema) {
            const auto found=row.find(capability.field);
            if (found==row.end() || !std::isfinite(found->second) || found->second<0 ||
                found->second>std::numeric_limits<float>::max())
                throw RuntimeError("Converted AES field is missing or outside its native range");
            const double canonical=capability.field==LayerParameter::LiftDistance ?
                double(float(found->second+row.at(LayerParameter::LiftDistance2)))-
                    row.at(LayerParameter::LiftDistance2) : double(float(found->second));
            if (canonical!=found->second) throw RuntimeError("Converted AES field is not native-canonical");
        }
        validate_chitubox_parameters(row);
        const double z=row.at(LayerParameter::PositionZ);
        if (z<=previous || !layer_z_within_native_limit(z,layout.parameter(20)))
            throw RuntimeError("Converted AES layer Z is outside the selected printer");
        previous=z;
    }
    save_archive_impl(path, progress, nullptr, globals, &table, pricing);
    mark_saved();
}

void LayerDocument::require_converted_chitubox_masks(const ChituboxConversionMasks &masks) const
{
    if ((m_archive_format != "ctb_v3" && m_archive_format != "ctb_v4" && m_archive_format != "ctb_v5") ||
        masks.owner != this || masks.revision != m_revision || masks.layers.size() != layer_count() ||
        !export_input_guard().same_source(masks.generation))
        throw RuntimeError("Converted Chitubox masks do not belong to this container revision");
    masks.generation.check();
}

void LayerDocument::save_converted_chitubox(const std::string &path,
    const std::vector<LayerParameters> &table, const LayerJobParameters *globals,
    const FreshLayerMaterialPricing *pricing, ChituboxConversionMasks masks,
    const LayerProgress &progress)
{
    check_layer_progress(progress);
    require_converted_chitubox_masks(masks);
    replace_layer_parameter_table(table);
    // This operation touches parameter entries only. Rebind after that known
    // transition, before any callback can invalidate the owned destination.
    masks.revision = m_revision;
    masks.generation = export_generation_guard();
    const auto guarded_progress = [&](size_t done, size_t total) {
        masks.generation.check();
        check_layer_progress(progress, done, total);
        masks.generation.check();
        return true;
    };
    save_archive_impl(path, guarded_progress, nullptr, globals, nullptr, pricing,
        &masks.generation, &masks);
    mark_saved_if_current(masks.generation);
}

void LayerDocument::save_archive_impl(const std::string &path, const LayerProgress &progress,
    const LayerExportProfile *profile, const LayerJobParameters *globals,
    const std::vector<LayerParameters> *converted_table, const FreshLayerMaterialPricing *pricing,
    const ExportGenerationGuard *generation, const ChituboxConversionMasks *converted_masks) const {
  check_layer_progress(progress);
  if (converted_masks) require_converted_chitubox_masks(*converted_masks);
  if (pricing) pricing->validate();
  if (globals) {
    validate_chitubox_job(*globals, m_layers.size());
    if (m_archive_format == "ctb_v3") {
      // An overlay owns only validated source sections. Vector bounds cannot
      // distinguish a missing PrintParameters address from header offset 0,
      // nor authorize repairing contradictory source defaults implicitly.
      (void)job_parameters();
      validate_chitubox_v3_job(*globals, m_layers.size());
    }
    else if (!is_ctb_encrypted_layer_format(m_archive_format) && m_archive_format != "ctb_v4" && m_archive_format != "ctb_v5")
      throw RuntimeError("Native global overlay requires CTB v3/v4/v5 or AES v4/v5");
  }
  if (profile && anycubic_available_versions(m_archive_format).empty())
    throw RuntimeError("Explicit resin-estimate profiles are not yet supported for this layer container");
  if (m_layers.empty())
    throw RuntimeError("Cannot save an empty layer document");
  if (m_archive_format.empty())
    throw RuntimeError("Export a new raster document with its printer profile "
                       "instead of preserving an archive");
  const auto extension = boost::filesystem::path(path).extension().string();
  const auto *image_format = find_layer_image_format(m_archive_format);
  // The CTB version lives in the header, not in the file name: Chitubox and printers name every version
  // plain ".ctb". Accept the exact registry suffix (".v5.ctb") or plain ".ctb", but refuse a name that
  // claims a different CTB variant (e.g. "part.v3.ctb" for a v5 document).
  const auto ctb_save_name_matches = [](const std::string &format, const std::string &file) {
    const std::string ext = get_archive_entry(format.c_str())->ext;
    if (boost::algorithm::iends_with(file, "." + ext)) return true;
    if (!boost::algorithm::iends_with(ext, "ctb") || !boost::algorithm::iends_with(file, ".ctb")) return false;
    const std::string stem = file.substr(0, file.size() - 4);
    for (const char *marker : {".v3", ".v4", ".v5", ".gktwo", ".encrypted", ".cbddlp", ".photon"})
      if (boost::algorithm::iends_with(stem, marker)) return false;
    return true;
  };

  if (!anycubic_available_versions(m_archive_format).empty() ? !boost::algorithm::iequals(extension, "."+m_archive_format) : image_format ? !boost::algorithm::iequals(extension, std::string(".")+image_format->id) :
      (is_ctb_mask_format(m_archive_format)) ? !ctb_save_name_matches(m_archive_format, path) :
      m_archive_format == "cxdlpv1" ? !boost::algorithm::iends_with(path,".v1.cxdlp") :
      m_archive_format == "cxdlpv2" ? !boost::algorithm::iends_with(path,".v2.cxdlp") :
      m_archive_format == "cxdlp" ? !boost::algorithm::iequals(extension,".cxdlp") :
      m_archive_format == "cxdlpv4" ? !boost::algorithm::iequals(extension,".cxdlpv4") :
      is_goo_format(m_archive_format) ? !boost::algorithm::iequals(extension,"."+m_archive_format) :
      is_anycubic_zip_layer_format(m_archive_format) ? !boost::algorithm::iequals(extension, "."+m_archive_format) :
      is_phrozen_layer_format(m_archive_format) ? !boost::algorithm::iequals(extension, "."+m_archive_format) :
      is_zcode_layer_format(m_archive_format) ? !boost::algorithm::iequals(extension, "."+m_archive_format) :
      is_native_binary_layer_format(m_archive_format) ? !boost::algorithm::iequals(extension, "."+m_archive_format) :
      is_nanodlp_layer_format(m_archive_format) ? !NanoDLPLayout::extension_matches(m_archive_format, path) :
      m_archive_format == "klipper_rgb" ? !boost::algorithm::iends_with(path, ".rgb.zip") :
      m_archive_format == "klipper" ? (!boost::algorithm::iequals(extension, ".zip") || boost::algorithm::iends_with(path, ".rgb.zip")) :
      (m_archive_format == "genericzip" || m_archive_format == "vda" || m_archive_format == "chitubox_zip") ? !boost::algorithm::iequals(extension, ".zip") :
      m_archive_format == "cwsxml" ? !boost::algorithm::iends_with(path,".xml.cws") :
      m_archive_format == "cwsrgb" ? !boost::algorithm::iends_with(path,".rgb.cws") :
      (m_archive_format == "cws" || m_archive_format == "cwsnovamaker") ? !boost::algorithm::iequals(extension, ".cws") : m_archive_format == "uvj"
          ? !boost::algorithm::iequals(extension, ".uvj")
          : !(boost::algorithm::iequals(extension, ".sl1") ||
              boost::algorithm::iequals(extension, ".sl1s")))
    throw RuntimeError(
        "The output extension must match the original layer container");
  auto write_bytes=[&](std::ostream &stream,const void *data,size_t length) {
    const auto *bytes=static_cast<const char *>(data);
    for(size_t at=0;at<length;) {
      check_layer_progress(progress);const size_t chunk=std::min(size_t(1)<<20,length-at);
      stream.write(bytes+at,chunk);if(!stream)throw FileIOError("Cannot write edited layer archive");at+=chunk;
    }
    check_layer_progress(progress);
  };
  const boost::filesystem::path output(path);
  const auto temporary = output.parent_path() / boost::filesystem::unique_path(
                                                    ".sla-edit-%%%%-%%%%.tmp");
  // Streams a rewritten container (original + patches + appended layers) to the temporary file in chunks.
  auto write_image=[&](const PatchedImage &image,const char *error) {
    boost::nowide::ofstream stream(temporary.string(),std::ios::binary|std::ios::trunc);
    image.stream([&](const uint8_t *data,size_t length){write_bytes(stream,data,length);});
    stream.close();if(!stream)throw FileIOError(error);
  };
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
      const double volume=mask_volume_ml(*this,fields.at("machineX"),fields.at("machineY"),positions,progress);
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
          check_layer_progress(progress,layer,m_layers.size());
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
    if (!anycubic_available_versions(m_archive_format).empty()) {
      const auto original = native_original();
      const AnycubicLayerLayout layout(original, m_archive_format, [&]{check_layer_progress(progress);});
      if (profile) {
        profile->validate(layout.columns, layout.rows);
        if (const auto dimensions = layout.physical_dimensions(); dimensions &&
            (std::abs(dimensions->first-profile->screen_width_mm) > .005 ||
             std::abs(dimensions->second-profile->screen_height_mm) > .005))
          throw RuntimeError("Selected printer screen dimensions differ from the imported Anycubic machine");
      }
      PatchedImage output(original);
      auto put32 = [&](size_t at, uint32_t value) { output.put32(at, value); };
      auto put_float = [&](size_t at, double value) { const float f = float(value); if (!std::isfinite(f)) throw RuntimeError("Anycubic metadata overflow"); uint32_t bits; std::memcpy(&bits, &f, 4); put32(at, bits); };
      double old_integral = 0, new_integral = 0, previous_z=0, time_delta=0;
      bool changed = false, masks_changed=false, parameters_changed=false, z_changed=false;
      size_t left = layout.columns, top = layout.rows, right = 0, bottom = 0;
      for (size_t i = 0; i < m_layers.size(); ++i) {
        check_layer_progress(progress, i, m_layers.size());
        // A layer that still is the decode of its source RLE needs no comparison: its bytes are kept.
        const bool pristine = native_layer_pristine(m_layers[i], i);
        const auto &known = m_entries[m_layers[i]].statistics;
        const bool counted = pristine && known && known->columns == layout.columns && known->rows == layout.rows;
        const auto old_pixels = counted ? std::vector<uint8_t>() : layout.decode(i, [&]{check_layer_progress(progress);});
        const auto pixels = pristine ? png::ImageGreyscale{} : image(i);
        if (!pristine && (pixels.cols != layout.columns || pixels.rows != layout.rows)) throw RuntimeError("Edited Anycubic dimensions changed");
        const size_t old_count = counted ? size_t(known->nonzero_pixels) : size_t(std::count_if(old_pixels.begin(), old_pixels.end(), [](uint8_t value){return value != 0;}));
        const size_t new_count = pristine ? old_count : std::count_if(pixels.buf.begin(), pixels.buf.end(), [](uint8_t value){return value != 0;});
        if (layout.model) for (size_t y = 0; y < (pristine ? 0 : pixels.rows); ++y) {
          check_layer_progress(progress);
          for (size_t x = 0; x < pixels.cols; ++x) if (pixels.buf[y*pixels.cols+x]) {
            left = std::min(left, x); top = std::min(top, y);
            right = std::max(right, x+1); bottom = std::max(bottom, y+1);
          }
        }
        const auto at = layout.record(i);const auto parameters=layer_parameters(i);
        const double z=parameters.at(LayerParameter::PositionZ),height=z-previous_z;
        if(profile)profile->validate_height(z);
        const double exposure=parameters.at(LayerParameter::Exposure);previous_z=z;
        if (!std::isfinite(height) || height<=0 || float(height)<=0)
          throw RuntimeError("Edited Anycubic layer heights must increase and remain representable");
        if (float(height)!=layout.number(at+20)) {changed=z_changed=true;parameters_changed=true;put_float(at+20,height);}
        if (float(exposure)!=layout.number(at+16)) {parameters_changed=true;put_float(at+16,exposure);}
        if(float(parameters.at(LayerParameter::LiftDistance))!=layout.number(at+8)) {
            parameters_changed=true;put_float(at+8,parameters.at(LayerParameter::LiftDistance));
        }
        if(float(parameters.at(LayerParameter::LiftSpeed)/60.)!=layout.number(at+12)) {
            parameters_changed=true;put_float(at+12,parameters.at(LayerParameter::LiftSpeed)/60.);
        }
        // Global EXTRA context stays unchanged. Use total lift once in the
        // estimate, preserving unknown fixed overhead in the original header.
        time_delta+=anycubic_layer_seconds(parameters)-anycubic_layer_seconds(anycubic_layer_parameters(layout,i,z));
        old_integral += old_count * double(layout.number(at+20));
        new_integral += new_count * double(float(height));
        if (pristine || pixels.buf == old_pixels) continue;
        changed = masks_changed = true;
        const auto payload = layout.encode(pixels.buf, [&]{check_layer_progress(progress);});
        if (output.size()+payload.size() > UINT32_MAX) throw RuntimeError("Edited Anycubic exceeds the 4 GiB address range");
        const uint32_t address = uint32_t(output.size());
        output.append(payload);
        put32(at, address); put32(at+4, uint32_t(payload.size())); put32(at+24, uint32_t(new_count));
        if (layout.subimage_is_shared(i)) {
          const auto sub=layout.subrecord(i);
          put32(sub,address);put32(sub+4,uint32_t(payload.size()));put32(sub+8,uint32_t(new_count));
        }
        // An independent SUBIMGS mask is a separate image: painting the main
        // plane must preserve its pointer, pixels, count and opaque padding.
        // Explicit secondary edits are written in the separate pass below.
      }
      if (parameters_changed) {
        put32(layout.header+80,1); // Enable the documented per-layer settings.
        if (const auto estimate=layout.integer(layout.header+84)) {
          const double seconds=std::ceil(double(estimate)+time_delta);
          if (!std::isfinite(seconds) || seconds<0 || seconds>UINT32_MAX)
            throw RuntimeError("Edited Anycubic print-time estimate exceeds the format range");
          put32(layout.header+84,uint32_t(seconds));
        } // Zero is an unknown estimate, not zero motion overhead.
        if (layout.model && z_changed) put_float(layout.model+44,previous_z);
      }
      // Secondary masks have no documented independent volume/exposure fields.
      // Preserve main statistics unless a linked/main mask actually changed.
      if (layout.subtable) for (size_t i=0;i<m_layers.size();++i) {
        check_layer_progress(progress,i,m_layers.size());
        if (mask_is_shared(i)) continue; // Already written together with main.
        if (native_layer_pristine(m_secondary_layers.at(i),layout.count+i)) continue; // keeps its source RLE
        const auto mask=image(i,LayerMaskPlane::Secondary);
        const auto old=layout.decode_subimage(i,[&]{check_layer_progress(progress);});
        if (mask.cols!=layout.columns || mask.rows!=layout.rows)
          throw RuntimeError("Edited Anycubic secondary dimensions changed");
        if (mask.buf==old) continue;
        const auto payload=layout.encode(mask.buf,[&]{check_layer_progress(progress);});
        if (output.size()+payload.size()>UINT32_MAX) throw RuntimeError("Edited Anycubic exceeds the 4 GiB address range");
        const auto sub=layout.subrecord(i);const auto address=uint32_t(output.size());
        output.append(payload);
        put32(sub,address);put32(sub+4,uint32_t(payload.size()));
        put32(sub+8,uint32_t(std::count_if(mask.buf.begin(),mask.buf.end(),[](uint8_t value){return value!=0;})));
      }
      // Preserve legacy geometry and arbitrary per-layer parameters. The ratio
      // of pixel-height integrals cancels pixel area even without a MACHINE table.
      if (!profile && changed && old_integral > 0) for (size_t offset : {52u, 68u, 72u}) {
        const double value = layout.number(layout.header+offset);
        if (!std::isfinite(value) || value < 0) throw RuntimeError("Invalid Anycubic original resin statistics");
        put_float(layout.header+offset, value*new_integral/old_integral);
      }
      if (!profile && changed && new_integral > 0 &&
          (old_integral == 0 || layout.number(layout.header+52) == 0) &&
          (masks_changed || layout.physical_dimensions().has_value())) {
        // No source volume exists to scale. Weight/cost cannot be recovered
        // from zero statistics without a material profile; retain those fields.
        put_float(layout.header+52, new_integral*layout.pixel_area()/1000.);
      }
      if (profile) {
        const double volume = new_integral*profile->pixel_area()/1000.;
        put_float(layout.header+52, volume);
        put_float(layout.header+68, volume*profile->density_g_ml);
        put_float(layout.header+72, volume*profile->cost_per_ml);
      }
      if (masks_changed && layout.model) {
        const auto machine = layout.integer(44);
        layout.named(machine, "MACHINE", 148);
        const double width = layout.number(machine+136), height = layout.number(machine+140);
        if (!std::isfinite(width) || !std::isfinite(height) || width <= 0 || height <= 0)
          throw RuntimeError("Cannot update Anycubic model bounds without valid machine dimensions");
        auto coordinate = [](double value) { return std::round(value*10000)/10000; };
        put_float(layout.model+16, right ? coordinate(left*width/layout.columns-width/2) : 0);
        put_float(layout.model+20, right ? coordinate(top*height/layout.rows-height/2) : 0);
        put_float(layout.model+28, right ? coordinate(right*width/layout.columns-width/2) : 0);
        put_float(layout.model+32, right ? coordinate(bottom*height/layout.rows-height/2) : 0);
      }
      const AnycubicLayerLayout verified(output.view(), m_archive_format);
      write_image(output, "Cannot write edited Anycubic layers");
    } else
    if (is_ctb_encrypted_layer_format(m_archive_format)) {
      const auto original=native_original();
      const CTBEncryptedLayerLayout layout(original,[&]{check_layer_progress(progress);});
      PatchedImage output(original);auto settings=layout.settings;bool changed=false,parameters_changed=false,z_changed=false;double volume=0,previous_z=0,time_delta=0;
      const bool tilting=ctb_encrypted_tilting_vat(layout);
      auto put=[](auto &&data,size_t at,uint32_t value){for(unsigned n=0;n<4;++n)data.at(at+n)=uint8_t(value>>(8*n));};
      auto scalar=[&](size_t at,double value) {
          const float f=float(value);if(!std::isfinite(f))throw RuntimeError("CTBEncrypted resin statistics overflow");
          uint32_t bits;std::memcpy(&bits,&f,4);put(settings,at,bits);
      };
      bool increasing_z=true;
      for(size_t i=0;i<m_layers.size();++i) {
        check_layer_progress(progress,i,m_layers.size());const auto record=layout.record(i);
        const bool pristine=native_layer_pristine(m_layers[i],i);
        png::ImageGreyscale mask;size_t nonzero=0;
        if(pristine) {
          const auto &statistics=m_entries[m_layers[i]].statistics;
          if(statistics && statistics->columns==layout.columns && statistics->rows==layout.rows)nonzero=statistics->nonzero_pixels;
          else {const auto decoded=layout.decode(i,[&]{check_layer_progress(progress,i,m_layers.size());});nonzero=std::count_if(decoded.begin(),decoded.end(),[](uint8_t value){return value!=0;});}
        } else {
          mask=image(i);
          if(mask.cols!=layout.columns || mask.rows!=layout.rows)throw RuntimeError("CTBEncrypted mask dimensions changed");
          nonzero=std::count_if(mask.buf.begin(),mask.buf.end(),[](uint8_t value){return value!=0;});
        }
        if(!pristine && mask.buf!=layout.decode(i,[&]{check_layer_progress(progress,i,m_layers.size());})) {
          changed=true;const auto raster=chitubox::Gray7RasterEncoder{}(mask.buf.data(),mask.cols,mask.rows,1);
          const auto *begin=static_cast<const uint8_t *>(raster.data());std::vector<uint8_t> payload(begin,begin+raster.size());
          ctb_encrypted::xor_layer(payload,uint32_t(i),layout.seed,[&]{check_layer_progress(progress,i,m_layers.size());});
          uint32_t aes_offset=layout.integer(record+32),aes_length=layout.integer(record+36);
          if(aes_length) {
            // Match the reference writer's unencrypted fallback for small,
            // unaligned masks: its decoder otherwise silently clones a layer.
            if((payload.size()<512 && payload.size()%16) || aes_offset>payload.size())aes_offset=aes_length=0;
            else aes_length=uint32_t(std::min(size_t(aes_length),(payload.size()-aes_offset)/16*16));
            if(aes_length) {
              const auto encrypted=ctb_encrypted::aes_cbc({payload.begin()+aes_offset,payload.begin()+aes_offset+aes_length},true,
                  [&]{check_layer_progress(progress,i,m_layers.size());});
              std::copy(encrypted.begin(),encrypted.end(),payload.begin()+aes_offset);
            }
          }
          if(output.size()+payload.size()+88>UINT32_MAX)throw RuntimeError("CTBEncrypted edited file exceeds the 4 GiB address range");
          const auto at=uint32_t(output.size());output.append(original.read(record,88));
          put(output,layout.table+16*i,at);put(output,at+16,at+88);put(output,at+24,uint32_t(payload.size()));
          put(output,at+32,aes_offset);put(output,at+36,aes_length);output.append(payload);
        }
        const auto values=converted_table ? converted_table->at(i) : layer_parameters(i);
        const auto before=ctb_encrypted_layer_parameters(layout,i);
        if(values!=before) {
          parameters_changed=true;
          if (!converted_table)
            time_delta+=ctb_encrypted_layer_seconds(values,tilting)-ctb_encrypted_layer_seconds(before,tilting);
          const auto at=output.get32(layout.table+16*i);
          auto number=[&](size_t offset,double value) {
            const float f=float(value);if(!std::isfinite(f))throw RuntimeError("CTBEncrypted layer metadata overflow");
            uint32_t bits;std::memcpy(&bits,&f,4);put(output,at+offset,bits);
          };
          using F=LayerParameter;
          for(auto [field,offset]:std::map<F,size_t>{{F::PositionZ,4},{F::Exposure,8},{F::LightOff,12},
              {F::LiftSpeed,44},{F::LiftDistance2,48},{F::LiftSpeed2,52},{F::RetractSpeed,56},
              {F::RetractDistance2,60},{F::RetractSpeed2,64},{F::WaitAfter,68},
              {F::WaitAfterLift,72},{F::WaitBefore,76},{F::LightPWM,80}})
            if(values.at(field)!=before.at(field))number(offset,values.at(field));
          if(values.at(F::LiftDistance)!=before.at(F::LiftDistance) || values.at(F::LiftDistance2)!=before.at(F::LiftDistance2))
            number(40,values.at(F::LiftDistance)+values.at(F::LiftDistance2));
        }
        const double z=values.at(LayerParameter::PositionZ);if(z!=before.at(LayerParameter::PositionZ))z_changed=true;
        if(z<=previous_z)increasing_z=false;
        volume+=nonzero*
            double(layout.parameter(12))*layout.parameter(16)/(double(layout.columns)*layout.rows)*(z-previous_z)/1000.;previous_z=z;
      }
      if(changed || z_changed || pricing) {
        if(!increasing_z)throw RuntimeError("Cannot update CTBEncrypted statistics for non-increasing Z");
        const double old_volume=layout.parameter(104);
        if(!std::isfinite(old_volume) || old_volume<0)throw RuntimeError("Invalid original CTBEncrypted volume");
        scalar(104,volume);
        if (pricing) {
            const auto totals=pricing->native_totals(volume);
            scalar(104,totals[0]);scalar(108,totals[1]);scalar(112,totals[2]);
        } else if(old_volume>0)for(size_t at:{108u,112u}) {
            const double old_value=layout.parameter(at);
            if(!std::isfinite(old_value) || old_value<0)throw RuntimeError("Invalid original CTBEncrypted material statistics");
            scalar(at,old_value*volume/old_volume);
        }
      }
      if(parameters_changed) {
        settings.at(171)=0x40;
        if(z_changed)scalar(32,previous_z);
        // Native edits retain unknown preparation overhead; a converted job
        // uses its full final estimate below, never this bootstrap delta.
        if (!converted_table) if(const auto original_time=layout.setting(76)) {
          const double seconds=std::ceil(original_time+time_delta);
          if(!std::isfinite(seconds) || seconds<0 || seconds>UINT32_MAX)throw RuntimeError("Edited print time exceeds the format range");
          put(settings,76,uint32_t(seconds));
        }
      }
      if (globals) overlay_encrypted_job(settings, *globals);
      if (converted_table) {
        // A conversion is a new job. Estimate from final stored globals and
        // every final row; temporary rounded process defaults are not identity.
        const auto model_at=layout.setting(160),model_size=layout.setting(164);
        const auto model_bytes=model_size ? original.read(model_at,model_size) : std::vector<uint8_t>();
        const std::string model(model_bytes.begin(),model_bytes.end());
        const bool final_tilting=ctb_encrypted_tilting_motion(model,
            CTBEncryptedLayerLayout::number(settings,36),
            std::max(CTBEncryptedLayerLayout::number(settings,92)-
                     CTBEncryptedLayerLayout::number(settings,140),0.f),
            CTBEncryptedLayerLayout::number(settings,96));
        float estimate=300.f;
        for (const auto &row:*converted_table) estimate+=float(ctb_encrypted_layer_seconds(row,final_tilting));
        if (!std::isfinite(estimate) || double(estimate)>UINT32_MAX)
            throw RuntimeError("Converted AES print time exceeds the native range");
        put(settings,76,uint32_t(estimate));
        settings.at(171)=0x40;
      }
      if(changed || parameters_changed || globals || converted_table || pricing) {
        const auto encrypted=ctb_encrypted::aes_cbc(std::move(settings),true,[&]{check_layer_progress(progress);});
        output.set_range(layout.settings_offset,encrypted);
        // The checksum is unchanged, so its AES(SHA256(checksum)) signature is
        // unchanged too. Move the entire original trailer, including unknown
        // trailing data, after the appended records rather than ending on RLE.
        if(changed) {
        const size_t prefix=layout.version==5?8:0,tail=layout.signature_offset-prefix;
        if(output.size()+(original.size()-tail)>UINT32_MAX)throw RuntimeError("CTBEncrypted edited file exceeds the 4 GiB address range");
        put(output,24,uint32_t(output.size()+prefix));output.append_from_base(tail,original.size()-tail);
        }
      }
      const CTBEncryptedLayerLayout verified(output.view());
      if (pricing) {
          const auto totals=pricing->native_totals(volume);
          for (size_t i=0;i<totals.size();++i)
              if (verified.parameter(104+4*i)!=totals[i])
                  throw RuntimeError("CTBEncrypted native totals cannot preserve fresh material pricing");
      }
      write_image(output,"Cannot write edited CTBEncrypted layers");
    } else
    if(is_chitubox_layer_format(m_archive_format)) {
      const auto original=native_original();const ChituboxLayerLayout layout(original,[&]{check_layer_progress(progress);});
      PatchedImage output(original);double volume=0,previous_z=0;bool changed=false,complete_z=true,parameters_changed=false,z_changed=false;double time_delta=0;
      auto put32=[&](size_t at,uint32_t value){output.put32(at,value);};
      auto put_float=[&](size_t at,double value){const float f=float(value);if(!std::isfinite(f))throw RuntimeError("Edited Chitubox metadata overflow");uint32_t word;std::memcpy(&word,&f,4);put32(at,word);};
      for(size_t i=0;i<m_layers.size();++i) {
        check_layer_progress(progress,i,m_layers.size());
        size_t nonzero_pixels = 0;
        if (converted_masks) {
          const auto &statistics = converted_masks->layers.at(i);
          if (statistics.columns != layout.columns || statistics.rows != layout.rows ||
              statistics.nonzero_pixels > uint64_t(layout.columns) * layout.rows)
            throw RuntimeError("Converted Chitubox mask statistics differ from native geometry");
          nonzero_pixels = statistics.nonzero_pixels;
        } else {
          // Ordinary native edits still compare and re-encode actual pixels.
          // Only the private, unchanged conversion bootstrap has this proof.
          // A layer that still is the decode of its source RLE keeps those bytes: nothing to compare or encode.
          const bool pristine=native_layer_pristine(m_layers[i],i);
          png::ImageGreyscale mask;
          if(pristine) {
            const auto &statistics=m_entries[m_layers[i]].statistics;
            if(statistics && statistics->columns==layout.columns && statistics->rows==layout.rows)nonzero_pixels=statistics->nonzero_pixels;
            else {
              const auto decoded=layout.decode(i,[&]{check_layer_progress(progress,i,m_layers.size());});
              nonzero_pixels=std::count_if(decoded.begin(),decoded.end(),[](uint8_t value){return value!=0;});
            }
          } else {
            mask=image(i);if(mask.cols!=layout.columns || mask.rows!=layout.rows)throw RuntimeError("Edited Chitubox mask dimensions changed");
            nonzero_pixels = std::count_if(mask.buf.begin(),mask.buf.end(),[](uint8_t value){return value!=0;});
          }
          if(!pristine && mask.buf!=layout.decode(i,[&]{check_layer_progress(progress,i,m_layers.size());})) {
            changed=true;
            for(unsigned plane=0;plane<layout.planes;++plane) {
              const auto payload=layout.encode(mask.buf,i,plane);
              const size_t prefix = layout.version >= 3 ? 84 : 0;
              if (output.size()+payload.size()+prefix > UINT32_MAX)
                  throw RuntimeError("Edited Chitubox output exceeds the 4 GiB address range");
              const auto at = layout.record(i,plane);
              const size_t extended = output.size();
              if (prefix) {
                  const auto old = layout.extended_record(i);
                  output.append(original.read(old,prefix));
              }
              const auto address = uint32_t(output.size());
              put32(at+12,address);put32(at+16,uint32_t(payload.size()));
              if (prefix) {
                  // Keep the duplicated definition consistent, preserving unknown
                  // bytes and all motion/wait/PWM fields in the extended record.
                  put32(extended+12,address);put32(extended+16,uint32_t(payload.size()));
                  put32(extended+36,uint32_t(prefix+payload.size()));
              }
              output.append(payload);
            }
          }
        }
        double z=layout.number(layout.record(i));
        if(!m_layer_parameters.empty()) {
          using F=LayerParameter;
          const auto values=layer_parameters(i),before=chitubox_layer_parameters(layout,i);
          if(values!=before) {
            parameters_changed=true;
            time_delta+=chitubox_layer_seconds(values)-chitubox_layer_seconds(before);
            const auto at=layout.record(i),ex=size_t(output.get32(at+12));
            const auto extended=ex-84;
            for(auto [field,offset]:std::map<F,size_t>{{F::PositionZ,0},{F::Exposure,4},{F::LightOff,8}})
              if(values.at(field)!=before.at(field)) {put_float(at+offset,values.at(field));put_float(extended+offset,values.at(field));}
            for(auto [field,offset]:std::map<F,size_t>{{F::LiftSpeed,44},{F::RetractSpeed,56},{F::LightPWM,80}})
              if(values.at(field)!=before.at(field))put_float(extended+offset,values.at(field));
            if(values.at(F::LiftDistance)!=before.at(F::LiftDistance) ||
                (layout.version>=4 && values.at(F::LiftDistance2)!=before.at(F::LiftDistance2)))
              put_float(extended+40,values.at(F::LiftDistance)+(layout.version>=4?values.at(F::LiftDistance2):0.));
            if(layout.version>=4) for(auto [field,offset]:std::map<F,size_t>{{F::LiftDistance2,48},{F::LiftSpeed2,52},
                {F::RetractDistance2,60},{F::RetractSpeed2,64},{F::WaitAfter,68},{F::WaitAfterLift,72},{F::WaitBefore,76}})
              if(values.at(field)!=before.at(field))put_float(extended+offset,values.at(field));
          }
          z=values.at(F::PositionZ);if(z!=before.at(F::PositionZ))z_changed=true;
        }
        if(z<=previous_z)complete_z=false;
        volume+=nonzero_pixels*
            double(layout.number(8))*layout.number(12)/(double(layout.columns)*layout.rows)*(z-previous_z)/1000.;previous_z=z;
      }
      if(parameters_changed) {
        const auto slicer=layout.integer(104);output.set(slicer+39,uint8_t(layout.version*0x10));
        if(z_changed)put_float(28,previous_z);
        if(const auto original_time=layout.integer(76)) {
          const double seconds=std::ceil(original_time+time_delta);
          if(!std::isfinite(seconds) || seconds<0 || seconds>UINT32_MAX)throw RuntimeError("Edited print time exceeds the format range");
          put32(76,uint32_t(seconds));
        }
      }
      if((changed || z_changed || pricing) && layout.parameters) {
        if(!complete_z)throw RuntimeError("Cannot update resin statistics for non-increasing Chitubox Z");
        const double old_volume=layout.number(layout.parameters+20);
        if(!std::isfinite(old_volume) || old_volume<0)throw RuntimeError("Invalid original Chitubox resin volume");
        put_float(layout.parameters+20,volume);
        if (pricing) {
          const auto totals=pricing->native_totals(volume);
          for (size_t i=0;i<totals.size();++i) put_float(layout.parameters+20+4*i,totals[i]);
        } else if(old_volume>0)for(size_t field:{24u,28u}) {
          const double old_value=layout.number(layout.parameters+field);
          if(!std::isfinite(old_value) || old_value<0)throw RuntimeError("Invalid original Chitubox resin statistics");
          put_float(layout.parameters+field,old_value*volume/old_volume);
        }
      }
      if (globals) overlay_chitubox_job(output, layout, *globals);
      const ChituboxLayerLayout verified(output.view());
      if (pricing) {
          const auto totals=pricing->native_totals(volume);
          if (!verified.parameters) throw RuntimeError("Fresh Chitubox material totals require native parameter fields");
          for (size_t i=0;i<totals.size();++i)
              if (verified.number(verified.parameters+20+4*i)!=totals[i])
                  throw RuntimeError("Chitubox native totals cannot preserve fresh material pricing");
      }
      write_image(output,"Cannot write edited Chitubox layers");
    } else     if(m_archive_format=="cxdlpv4" && m_revision==0) {
      const auto original=native_original();
      boost::nowide::ofstream stream(temporary.string(),std::ios::binary|std::ios::trunc);
      for(uint64_t at=0;at<original.size();) {
        const auto part=original.read(at,std::min<uint64_t>(original.size()-at,uint64_t(4)<<20));
        write_bytes(stream,part.data(),part.size());at+=part.size();
      }
      stream.close();
      if(!stream)throw FileIOError("Cannot copy the original CXDLPv4 archive");
    } else if(m_archive_format=="cxdlpv4") {
      const auto original=native_original();const CrealityCXDLPv4Layout layout(original,false);
      if(layout.layers.size()!=m_layers.size())throw RuntimeError("CXDLPv4 document layer count changed");
      // `raster` is empty for a layer that still is the decode of its source RLE: its source block is copied as is.
      struct Mask {EncodedRaster raster;uint32_t area=0,nonzero=0;bool keep=false;};
      std::vector<Mask> masks(m_layers.size());std::unordered_map<std::string_view,size_t> cache;
      std::map<uint32_t,std::vector<size_t>> groups;
      double volume=0,previous_z=0;bool complete_z=true;
      const float pitch_x=float(std::round(float(layout.width)/layout.columns*10000.f)/10000.f),pitch_y=float(std::round(float(layout.height)/layout.rows*10000.f)/10000.f);
      const double pixel_area=layout.width/layout.columns*layout.height/layout.rows;
      for(size_t i=0;i<m_layers.size();++i) {
        check_layer_progress(progress,i,m_layers.size());
        if(native_layer_pristine(m_layers[i],i)) {
          const auto source=cxdlpv4_layer_image(layout,i,[&]{check_layer_progress(progress);});
          masks[i].keep=true;
          masks[i].nonzero=uint32_t(std::count_if(source.buf.begin(),source.buf.end(),[](uint8_t p){return p>=2;}));
          masks[i].area=layout.integer(layout.table+40*i+28); // the unchanged mask keeps its stored contour area
        } else {
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
        }
        groups[layout.layers[i].data].push_back(i);
        const double z=layout.layers[i].z;if(z<=previous_z)complete_z=false;
        volume+=masks[i].nonzero*pixel_area*(z-previous_z);previous_z=z;
      }
      // Replace each addressed payload range once. Shared original payloads
      // may diverge after editing, so emit a separate block for each reference.
      // All documented addresses are relocated; unknown bytes and gaps survive.
      // The new file is built sequentially (source ranges copied, edited layers encoded); only the
      // relocated addresses are patched afterwards.
      static const std::vector<uint8_t> no_bytes;
      PatchedImage output{NativeBytes(no_bytes)};
      std::vector<uint32_t> addresses(m_layers.size());
      struct Relocation {size_t end;int64_t delta;};std::vector<Relocation> moves;
      auto append=[&](const uint8_t *data,size_t length) {
        if(output.size()+length>UINT32_MAX)throw RuntimeError("Edited CXDLPv4 file exceeds the 4 GiB address range");
        output.append(data,length);
      };
      auto copy_source=[&](uint64_t at,uint64_t length) {
        if(output.size()+length>UINT32_MAX)throw RuntimeError("Edited CXDLPv4 file exceeds the 4 GiB address range");
        for(uint64_t done=0;done<length;) {
          check_layer_progress(progress);
          const uint64_t part=std::min<uint64_t>(length-done,uint64_t(4)<<20);
          const auto chunk=original.read(at+done,part);output.append(chunk);done+=part;
        }
      };
      size_t source_position=0;
      for(const auto &[start,references]:groups) {
        copy_source(source_position,start-source_position);
        const auto original_size=layout.layers[references.front()].length;
        for(const auto i:references) {
          check_layer_progress(progress,i,m_layers.size());
          addresses[i]=uint32_t(output.size());const auto &layer=layout.layers[i];
          if(masks[i].keep)copy_source(layer.data,layer.length);
          else {
            copy_source(layer.data,44);
            const auto &raster=masks[i].raster;const auto *begin=static_cast<const uint8_t *>(raster.data());
            if(layout.seed) {std::vector<uint8_t> encrypted(begin,begin+raster.size());CrealityCXDLPv4Archive::crypt_mask(encrypted,layout.seed,uint32_t(i));append(encrypted.data(),encrypted.size());}
            else append(begin,raster.size());
          }
        }
        source_position=size_t(start)+original_size;
        moves.push_back({source_position,int64_t(output.size())-int64_t(source_position)});
      }
      copy_source(source_position,original.size()-4-source_position);
      auto relocate=[&](size_t old){int64_t delta=0;for(const auto &move:moves){if(old<move.end)break;delta=move.delta;}return size_t(int64_t(old)+delta);};
      auto put32=[&](size_t offset,uint32_t value){if(offset+4>output.size())throw RuntimeError("Invalid edited CXDLPv4 address");output.put32(offset,value);};
      auto put_float=[&](size_t offset,double value){const float number=float(value);if(!std::isfinite(number))throw RuntimeError("Edited CXDLPv4 metadata overflow");uint32_t bits;std::memcpy(&bits,&number,4);put32(offset,bits);};
      for(const size_t field:{layout.base+28,layout.base+32,layout.base+40,layout.base+52,layout.base+72}) {
        const auto old=layout.integer(field);if(old)put32(field,uint32_t(relocate(old)));
      }
      for(const size_t field:{layout.base+28,layout.base+40}) {
        const auto preview=layout.integer(field);if(preview)put32(relocate(preview)+8,uint32_t(relocate(layout.integer(preview+8))));
      }
      for(size_t i=0;i<m_layers.size();++i) {
        const size_t row=relocate(layout.table)+40*i;
        put32(row+12,addresses[i]);
        put32(row+16,masks[i].keep?layout.layers[i].length:uint32_t(44+masks[i].raster.size()));
        put32(row+28,masks[i].area);
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
      uint32_t checksum=0;
      output.stream([&](const uint8_t *data,size_t length){for(size_t n=0;n<length;++n)checksum=creality_checksum_update(checksum,data[n],4);});
      for(unsigned i=0;i<4;++i){const uint8_t byte=uint8_t(checksum>>(24-8*i));output.append(&byte,1);}
      // Validate the relocated metadata before the atomic replacement.
      const CrealityCXDLPv4Layout verified(output.view());
      write_image(output,"Cannot write edited CXDLPv4 layers");
    } else if (is_phrozen_layer_format(m_archive_format)) {
      const auto original = native_original();
      const auto kind = phrozen_kind(m_archive_format);
      const std::string family = PhrozenLayout::label(kind);
      if (m_revision == 0) {
        // Untouched: byte-identical copy of the source, streamed.
        stream_image_to_file(original, original.size(), temporary.string(), "Cannot write edited " + family + " layers", progress);
      } else {
        const PhrozenLayout layout(original, kind, [&]{ check_layer_progress(progress); });
        if (layout.count != m_layers.size()) throw RuntimeError(family + " document layer count changed");
        // The source with the edits patched in and the new payloads appended (the tail spills to disk).
        PatchedImage output(original);
        std::vector<size_t> edited; // layers whose payload was replaced, for the readback proof
        // Thickness-weighted lit-pixel totals scale the stored volume, mass and cost.
        double old_weighted = 0, new_weighted = 0, old_pixels = 0, new_pixels = 0, previous_z = 0, new_previous_z = 0;
        bool z_valid = true;
        // Edited per-layer parameters (Z, exposure, light-off) are written into the records below;
        // the Z of the table also sets the thickness the lit area is weighted by.
        const auto table = document_table(*this);
        const auto lit = [](const std::vector<uint8_t> &p) { return double(std::count_if(p.begin(), p.end(), [](uint8_t v) { return v != 0; })); };
        for (size_t i = 0; i < m_layers.size(); ++i) {
          check_layer_progress(progress, i, m_layers.size());
          const double z = layout.z(i), thickness = z - previous_z;
          const double new_z = table ? table->at(i).at(LayerParameter::PositionZ) : z, new_thickness = new_z - new_previous_z;
          if (!std::isfinite(z) || thickness <= 0 || !std::isfinite(new_z) || new_thickness <= 0) z_valid = false;
          previous_z = z; new_previous_z = new_z;
          // A layer that still is the decode of its source record keeps it; its lit count was taken at open.
          const auto &known = m_entries[m_layers[i]].statistics;
          const bool pristine = native_layer_pristine(m_layers[i], i);
          png::ImageGreyscale mask;
          std::vector<uint8_t> old;
          double lit_old, lit_new;
          bool same = pristine;
          if (pristine && known && known->columns == layout.columns && known->rows == layout.rows) lit_old = lit_new = double(known->nonzero_pixels);
          else {
            old = layout.decode(i, [&]{ check_layer_progress(progress, i, m_layers.size()); });
            lit_old = lit(old);
            if (pristine) lit_new = lit_old;
            else {
              mask = image(i);
              if (mask.cols != layout.columns || mask.rows != layout.rows) throw RuntimeError("Edited " + family + " mask dimensions changed");
              same = mask.buf == old;
              lit_new = same ? lit_old : lit(mask.buf);
            }
          }
          old_pixels += lit_old; new_pixels += lit_new; old_weighted += lit_old * thickness; new_weighted += lit_new * new_thickness;
          if (same) continue; // keeps its original payload and record
          // New data goes to the end of the file; every other layer, record and
          // header byte stays as it was (including layers sharing the old data).
          const auto payload = layout.encode(mask.buf, i);
          if (payload.size() > UINT32_MAX) throw RuntimeError("Edited " + family + " layer exceeds the 4 GiB length range");
          const uint64_t address = output.append(payload);
          const size_t record = layout.record(i);
          output.put32(record + 12, uint32_t(address)); output.put32(record + 16, uint32_t(payload.size())); output.put32(record + 20, uint32_t(address >> 32));
          edited.push_back(i);
        }
        if (table) layout.patch_parameters(output, *table);
        const double old_total = z_valid ? old_weighted : old_pixels, new_total = z_valid ? new_weighted : new_pixels;
        const float old_volume = layout.number(layout.volume_offset());
        if (std::isfinite(old_volume) && old_volume > 0 && old_total > 0 && new_total != old_total)
          for (size_t offset : {layout.volume_offset(), layout.volume_offset() + 4, layout.volume_offset() + 8}) {
            const float scaled = float(double(layout.number(offset)) * new_total / old_total);
            if (!std::isfinite(scaled)) throw RuntimeError("Edited " + family + " material statistics overflow");
            uint32_t bits; std::memcpy(&bits, &scaled, 4); output.put32(offset, bits);
          }
        // Prove the rebuilt file decodes to the edited pixels before it can replace the old one.
        const PhrozenLayout verified(output.view(), kind, [&]{ check_layer_progress(progress); });
        if (verified.count != m_layers.size()) throw RuntimeError("Edited " + family + " layer table is inconsistent");
        if (table && verified.parameter_rows() != *table) throw RuntimeError("Edited " + family + " parameters do not read back as saved");
        for (const size_t layer : edited)
          if (verified.decode(layer, [&]{ check_layer_progress(progress); }) != image(layer).buf)
            throw RuntimeError("Edited " + family + " layer " + std::to_string(layer + 1) + " does not read back as saved");
        write_image(output, ("Cannot write edited " + family + " layers").c_str());
      }
    } else if (LongerOrangeLayout::is_format(m_archive_format)) {
      save_native_layout<LongerOrangeLayout>(*this, native_original(), m_revision == 0, "LGS", temporary.string(), progress,
          [&](size_t i) { return native_layer_pristine(m_layers[i], i); });
    } else if (OpenSlaLayout::is_format(m_archive_format)) {
      save_native_layout<OpenSlaLayout>(*this, native_original(), m_revision == 0, "OSLA", temporary.string(), progress,
          [&](size_t i) { return native_layer_pristine(m_layers[i], i); });
    } else if (AnetLayout::is_format(m_archive_format)) {
      save_native_layout<AnetLayout>(*this, native_original(), m_revision == 0, "Anet", temporary.string(), progress,
          [&](size_t i) { return native_layer_pristine(m_layers[i], i); }, m_archive_format);
    } else if (VlareLayout::is_format(m_archive_format)) {
      save_native_layout<VlareLayout>(*this, native_original(), m_revision == 0, "OSF", temporary.string(), progress,
          [&](size_t i) { return native_layer_pristine(m_layers[i], i); });
    } else if (Emake3DLayout::is_format(m_archive_format)) {
      save_native_layout<Emake3DLayout>(*this, native_original(), m_revision == 0, "QDT", temporary.string(), progress,
          [&](size_t i) { return native_layer_pristine(m_layers[i], i); });
    } else if (MakerbaseLayout::is_format(m_archive_format)) {
      const auto kind = m_archive_format == "mdlp" ? MakerbaseLayout::Kind::Mdlp : MakerbaseLayout::Kind::Gr1;
      save_native_layout<MakerbaseLayout>(*this, native_original(), m_revision == 0, kind == MakerbaseLayout::Kind::Mdlp ? "MDLP" : "GR1",
          temporary.string(), progress, [&](size_t i) { return native_layer_pristine(m_layers[i], i); }, kind);
    } else if(is_goo_format(m_archive_format)) {
      const auto original=native_original();
      std::vector<uint8_t> head;      // everything before the first layer definition
      const GooLayout layout(original,[&]{check_layer_progress(progress);});
      if(layout.layers.size()!=m_layers.size())throw RuntimeError("GOO document layer count changed");
      boost::nowide::ofstream stream(temporary.string(),std::ios::binary|std::ios::trunc);
      if(m_revision==0) {
        // Untouched: byte-identical copy of the source, streamed.
        for(uint64_t at=0;at<original.size();) {
          const auto part=original.read(at,std::min<uint64_t>(original.size()-at,uint64_t(4)<<20));
          write_bytes(stream,part.data(),part.size());at+=part.size();
        }
      } else {
        // PRZ forbids colour-difference chunks; GOO uses the shared writer encoder.
        const bool diff_chunks=m_archive_format!="prz";
        // The header carries the (scaled) volume, so it is patched in a small copy and written first.
        head=original.read(0,layout.layer_def_address);
        std::vector<uint8_t> source_pixels(layout.pixel_count());
        // Weighted lit-pixel totals (thickness * pixels) scale the stored volume.
        double old_weighted=0,new_weighted=0,old_pixels=0,new_pixels=0,previous_z=0;bool z_valid=true;
        // Layer bodies are produced one at a time into a temporary body file; the header (volume) is known only after the
        // last layer, so the body is spooled and appended after the finished header.
        HistorySpill body;uint64_t body_size=0;
        std::vector<uint8_t> chunk;
        auto spool=[&](const uint8_t *data,size_t length) {
          chunk.insert(chunk.end(),data,data+length);
          if(chunk.size()>=(size_t(8)<<20)) {body.append(chunk);body_size+=chunk.size();chunk.clear();}
        };
        for(size_t i=0;i<m_layers.size();++i) {
          check_layer_progress(progress,i,m_layers.size());
          const auto &layer=layout.layers[i];
          const bool pristine=native_layer_pristine(m_layers[i],i);
          png::ImageGreyscale mask;
          bool same=pristine;double lit_old=0;
          const auto &known=m_entries[m_layers[i]].statistics;
          if(pristine && known && known->columns==layout.columns && known->rows==layout.rows)lit_old=double(known->nonzero_pixels);
          else {
            const auto rle=layout.payload(i);
            decode_goo_layer(rle.data(),rle.size(),source_pixels.data(),source_pixels.size(),i);
            lit_old=double(std::count_if(source_pixels.begin(),source_pixels.end(),[](uint8_t p){return p!=0;}));
            if(!pristine) {
              mask=image(i);
              if(mask.cols!=layout.columns || mask.rows!=layout.rows)throw RuntimeError("Edited GOO mask dimensions changed");
              // Re-encode only layers whose pixels differ; others keep their original bytes.
              same=mask.buf==source_pixels;
            }
          }
          const double z=layout.z(i),thickness=z-previous_z;
          if(!std::isfinite(z) || thickness<=0)z_valid=false;
          previous_z=z;
          const double lit_new=same?lit_old:double(std::count_if(mask.buf.begin(),mask.buf.end(),[](uint8_t p){return p!=0;}));
          old_pixels+=lit_old;new_pixels+=lit_new;old_weighted+=lit_old*thickness;new_weighted+=lit_new*thickness;
          auto def=original.read(layer.def,layer.data-layer.def);
          if(same) {
            spool(def.data(),def.size());
            const auto rle=original.read(layer.data,layer.length);spool(rle.data(),rle.size());
          } else {
            const auto rle=diff_chunks?encode_goo_rle(mask.buf.data(),mask.buf.size()):encode_goo_plain_rle(mask.buf.data(),mask.buf.size());
            if(rle.size()>UINT32_MAX)throw RuntimeError("Edited GOO layer exceeds the 4 GiB length range");
            for(unsigned j=0;j<4;++j)def[goo_def_length_offset+j]=uint8_t(rle.size()>>(24-8*j));
            spool(def.data(),def.size());spool(rle.data(),rle.size());
          }
          const uint8_t delimiter[2]{0x0D,0x0A};spool(delimiter,2);
        }
        if(!chunk.empty()){body.append(chunk);body_size+=chunk.size();chunk.clear();}
        // The stored volume, mass and cost follow the lit area. Only a ratio is
        // used, so no physical display size or unit assumption is needed.
        const double old_total=z_valid?old_weighted:old_pixels,new_total=z_valid?new_weighted:new_pixels;
        const float old_volume=layout.number(goo_volume_offset);
        if(std::isfinite(old_volume) && old_volume>0 && old_total>0 && new_total!=old_total)
          for(size_t offset:{goo_volume_offset,goo_volume_offset+4,goo_volume_offset+8}) {
            const float scaled=float(double(layout.number(offset))*new_total/old_total);
            if(!std::isfinite(scaled))throw RuntimeError("Edited GOO material statistics overflow");
            goo_put_bef32(head.data()+offset,scaled);
          }
        write_bytes(stream,head.data(),head.size());
        for(uint64_t at=0;at<body_size;) {
          const auto part=body.read(at,std::min<uint64_t>(body_size-at,uint64_t(8)<<20));
          write_bytes(stream,part.data(),part.size());at+=part.size();
        }
        const auto footer=original.read(layout.footer,original.size()-layout.footer);
        write_bytes(stream,footer.data(),footer.size());
      }
      stream.close();if(!stream)throw FileIOError("Cannot write edited GOO layers");
      if(m_revision!=0) {
        // Validate the rebuilt tables before the atomic replacement.
        const auto written=std::make_shared<SourceFileStore>(temporary.string());
        const GooLayout verified(NativeBytes(written,written->size()),[&]{check_layer_progress(progress);});
        if(verified.layers.size()!=m_layers.size())throw RuntimeError("Edited GOO layer table is inconsistent");
      }
    } else if (m_archive_format == "cxdlpv1" || m_archive_format == "cxdlpv2" || m_archive_format == "cxdlp") {
      save_native_layout<CrealityCXDLPv1Layout>(*this, native_original(), m_revision == 0, "CXDLP", temporary.string(), progress,
          [&](size_t i) { return native_layer_pristine(m_layers[i], i); });
    } else if (image_format) {
      if (m_layers.size() != 1) throw RuntimeError("A standalone image cannot contain a layer stack");
      const auto encoded = encode_layer_image(image(0), image_format->id);
      const png::ReadBuf bytes{encoded.data(), encoded.size()};
      boost::nowide::ofstream stream(temporary.string(), std::ios::binary | std::ios::trunc);
      write_bytes(stream,bytes.buf,bytes.sz);
      stream.close();
      if (!stream) throw FileIOError("Cannot write edited layer image");
    } else if (is_anycubic_zip_layer_format(m_archive_format)) {
      save_native_zip<AnycubicZipLayout>(*this, native_original(), m_revision == 0, "Anycubic ZIP", temporary.string(),
          Zipper::FAST_COMPRESSION, [&](const MemoryZip &zip) { return AnycubicZipLayout(zip, m_archive_format); }, progress);
    } else if (is_native_binary_layer_format(m_archive_format)) {
      save_native_binary_dispatch(*this, m_archive_format, native_original(), m_revision == 0, temporary.string(), progress);
    } else if (is_zcode_layer_format(m_archive_format)) {
      const auto kind = m_archive_format == "zcode" ? ZCodeLayout::Kind::ZCode : ZCodeLayout::Kind::ZCodex;
      save_native_zip<ZCodeLayout>(*this, native_original(), m_revision == 0, kind == ZCodeLayout::Kind::ZCode ? "ZCode" : "ZCodex",
          temporary.string(), Zipper::FAST_COMPRESSION, [&](const MemoryZip &zip) { return ZCodeLayout(zip, kind); }, progress);
    } else if (is_nanodlp_layer_format(m_archive_format)) {
      save_native_zip<NanoDLPLayout>(*this, native_original(), m_revision == 0, "NanoDLP ZIP", temporary.string(),
          Zipper::FAST_COMPRESSION, [&](const MemoryZip &zip) { return NanoDLPLayout(zip); }, progress);
    } else if (is_klipper_layer_format(m_archive_format)) {
      // The Klipper writer stores entries uncompressed; keep that for the rewrite.
      save_native_zip<KlipperLayout>(*this, native_original(), m_revision == 0, "Klipper ZIP", temporary.string(),
          Zipper::NO_COMPRESSION, [&](const MemoryZip &zip) { return KlipperLayout(zip); }, progress);
    } else {
      Zipper zip(temporary.string());
      std::unordered_map<size_t,size_t> mask_indices;
      if (m_archive_format=="cwsxml" || m_archive_format=="cwsrgb")
        for (size_t i=0;i<m_layers.size();++i) mask_indices.emplace(m_layers[i],i);
      for (size_t entry_index=0;entry_index<m_entries.size();++entry_index) {
        check_layer_progress(progress,entry_index,m_entries.size());
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
          {const auto payload=entry.load();zip.add_entry(entry.name, payload->data(), payload->size());}
      }
      zip.finalize();
    }
    check_layer_progress(progress,m_layers.size(),m_layers.size());
    // Saving over the archive this document reads its layers from: stop depending on the path.
    detach_source(output.string());
    if (generation) generation->publish(temporary.string(), output.string());
    else if (const auto error = rename_file(temporary.string(), output.string()); error)
      throw FileIOError("Cannot replace edited archive: " + error.message());
  } catch (...) {
    boost::system::error_code ignored;
    boost::filesystem::remove(temporary, ignored);
    throw;
  }
}
} // namespace Slic3r::sla
