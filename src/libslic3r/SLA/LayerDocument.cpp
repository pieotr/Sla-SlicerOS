#include "LayerDocument.hpp"
#include "libslic3r/Execution/ExecutionTBB.hpp"
#include <exception>
#include "LayerMaskValidation.hpp"
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
#include "libslic3r/Format/CrealityContourArea.hpp"
#include "libslic3r/Format/CrealityChecksum.hpp"
#include "libslic3r/Format/CrealityCXDLPv4.hpp"
#include "libslic3r/Format/CrealityCXDLPv4Layout.hpp"
#include "libslic3r/Format/ChituboxLayerLayout.hpp"
#include "libslic3r/Format/CTBEncryptedLayerLayout.hpp"
#include "libslic3r/Format/AnycubicLayerLayout.hpp"
#include "LayerExportProfile.hpp"
#include "ChituboxJobOverlay.hpp"
#include "ChituboxJobReader.hpp"
#include "ChituboxLayerParameters.hpp"
#include "CTBEncryptedLayerParameters.hpp"
#include "AnycubicLayerParameters.hpp"
#include "CrealityCXDLPv4JobReader.hpp"
#include "CrealityCXDLPv4ReferenceBottom.hpp"
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
} // namespace
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
        if (raster.size() > (uint64_t(1) << 30) - total)
            throw RuntimeError("Layer editor document exceeds the 1 GiB memory budget");
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
    return load_archive_impl(path, progress, nullptr);
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
        if (length < 48 || length > std::streamoff(uint64_t(1)<<30)) throw RuntimeError("Anycubic file exceeds editor limits");
        std::vector<uint8_t> bytes(static_cast<size_t>(length)); input.seekg(0); read_bytes(input, bytes);
        std::string format = boost::filesystem::path(path).extension().string();
        if (!format.empty()) format.erase(0, 1);
        boost::algorithm::to_lower(format);
        const AnycubicLayerLayout layout(bytes, format, [&]{check_layer_progress(progress);});
        LayerDocument result; result.m_archive_format = format;
        result.m_entries.push_back({"anycubic-original", {}});
        uint64_t stored = bytes.size();double position_z=0;
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
            const auto png = PNGRasterEncoder{}(pixels.data(), layout.columns, layout.rows, 1);
            if (!png.size() || png.size() > (uint64_t(1)<<30)-stored) throw RuntimeError("Anycubic masks exceed editor limits");
            stored += png.size(); const auto *data = static_cast<const uint8_t *>(png.data());
            result.m_layers.push_back(result.m_entries.size());
            result.m_entries.push_back({std::to_string(i)+".png", {data, data+png.size()}});
            if (layout.subtable) {
                if (layout.subimage_is_shared(i)) result.m_secondary_layers.push_back(result.m_layers.back());
                else {
                    const auto encoded=PNGRasterEncoder{}(secondary.data(),layout.columns,layout.rows,1);
                    if (!encoded.size() || encoded.size()>(uint64_t(1)<<30)-stored)
                        throw RuntimeError("Anycubic secondary masks exceed editor limits");
                    stored+=encoded.size();const auto *begin=static_cast<const uint8_t *>(encoded.data());
                    result.m_secondary_layers.push_back(result.m_entries.size());
                    result.m_entries.push_back({std::to_string(i)+"-sub.png",{begin,begin+encoded.size()}});
                }
            }
            position_z+=layout.number(layout.record(i)+20);
            const auto parameters=anycubic_layer_parameters(layout,i,position_z);
            auto encoded=encode_parameters(parameters);
            if (encoded.size()>(uint64_t(1)<<30)-stored) throw RuntimeError("Anycubic parameters exceed editor budget");
            stored+=encoded.size();result.m_layer_parameters.push_back(result.m_entries.size());
            result.m_entries.push_back({std::to_string(i)+"-parameters.json",std::move(encoded)});
        }
        check_layer_progress(progress, layout.count, layout.count);
        result.m_entries[0].bytes = std::move(bytes); return result;
    }
    if (input.gcount()==8 && magic==0x12fd0107) {
        input.seekg(0,std::ios::end);const auto length=input.tellg();
        if(length<336 || length>std::streamoff(uint64_t(1)<<30))throw RuntimeError("CTBEncrypted file exceeds editor budget or is truncated");
        std::vector<uint8_t> bytes(static_cast<size_t>(length));input.seekg(0);read_bytes(input,bytes);
        const CTBEncryptedLayerLayout layout(bytes,[&]{check_layer_progress(progress);});
        LayerDocument result;result.m_archive_format=layout.version==4?"ctb_encrypted_v4":"ctb_encrypted";
        result.m_entries.push_back({"ctb-encrypted-original",{}});uint64_t stored=bytes.size();
        for(size_t i=0;i<layout.count;++i) {
            check_layer_progress(progress,i,layout.count);
            const auto pixels=layout.decode(i,[&]{check_layer_progress(progress,i,layout.count);});
            const auto png=PNGRasterEncoder{}(pixels.data(),layout.columns,layout.rows,1);
            if(!png.size() || png.size()>(uint64_t(1)<<30)-stored)throw RuntimeError("CTBEncrypted masks exceed editor budget");
            stored+=png.size();const auto *begin=static_cast<const uint8_t *>(png.data());
            result.m_layers.push_back(result.m_entries.size());
            result.m_entries.push_back({std::to_string(i)+".png",{begin,begin+png.size()}});
        }
        for(size_t i=0;i<layout.count;++i) {
            auto encoded=encode_parameters(ctb_encrypted_layer_parameters(layout,i));
            if(encoded.size()>(uint64_t(1)<<30)-stored)throw RuntimeError("CTBEncrypted parameters exceed editor budget");
            stored+=encoded.size();result.m_layer_parameters.push_back(result.m_entries.size());
            result.m_entries.push_back({std::to_string(i)+"-parameters.json",std::move(encoded)});
        }
        check_layer_progress(progress,layout.count,layout.count);
        result.m_entries[0].bytes=std::move(bytes);return result;
    }
    if(input.gcount()==8 && (magic==0x12fd0019 || magic==0x12fd0086 || magic==0x12fd0106 || magic==0xff220810)) {
        input.seekg(0,std::ios::end);const auto length=input.tellg();
        if(length<112 || length>std::streamoff(uint64_t(1)<<30))throw RuntimeError("Chitubox file exceeds the editor budget or is truncated");
        std::vector<uint8_t> bytes(static_cast<size_t>(length));input.seekg(0);
        read_bytes(input,bytes);
        const ChituboxLayerLayout layout(bytes,[&]{check_layer_progress(progress);});LayerDocument result;
        const std::string family=layout.binary?(boost::algorithm::iends_with(path,".photon")?"photon":"cbddlp"):"ctb";
        result.m_archive_format = layout.magic == 0xff220810 ? "ctb_gktwo" :
            layout.version >= 3 ? "ctb_v" + std::to_string(layout.version) :
            family + (layout.version == 1 ? "_v1" : "");
        if (readback && (layout.binary || layout.version < 3 ||
            result.m_archive_format != readback->format || layout.count != readback->source.layer_count()))
            throw RuntimeError("Converted Chitubox format or layer count differs from the request");
        result.m_entries.push_back({"chitubox-original",{}});uint64_t stored=bytes.size();
        std::unordered_map<std::string,size_t> cache;
        for(size_t i=0;i<layout.count;++i) {
            check_layer_progress(progress,i,layout.count);
            std::string key;
            for(unsigned plane=0;plane<layout.planes;++plane) {
                const auto at=layout.record(i,plane);
                key.append(reinterpret_cast<const char *>(bytes.data()+at+12),8);
            }
            std::vector<uint8_t> encoded_png;
            // Seeded CTB uses a different cipher stream for each layer index.
            const bool reusable=layout.binary || !layout.seed;
            const auto cached=reusable?cache.find(key):cache.end();
            if(cached!=cache.end()) {
                encoded_png=result.m_entries[cached->second].bytes;
                if (readback) {
                    // Shared native bytes do not prove that two SOURCE layers
                    // match. Compare each source against the cached real mask.
                    png::ImageGreyscale pixels;
                    if (!png::decode_png({encoded_png.data(),encoded_png.size()}, pixels) ||
                        pixels.cols != layout.columns || pixels.rows != layout.rows)
                        throw RuntimeError("Cannot decode cached converted Chitubox mask");
                    readback->verify(i, pixels.buf, pixels.cols, pixels.rows, progress);
                }
            } else {
                const auto pixels=layout.decode(i,[&]{check_layer_progress(progress,i,layout.count);});
                if (readback) readback->verify(i, pixels, layout.columns, layout.rows, progress);
                const auto png=PNGRasterEncoder{}(pixels.data(),layout.columns,layout.rows,1);
                if(!png.size())throw RuntimeError("Cannot encode imported Chitubox mask");
                const auto *begin=static_cast<const uint8_t *>(png.data());encoded_png.assign(begin,begin+png.size());
                if(reusable && cache.size()<1024)cache.emplace(std::move(key),result.m_entries.size());
            }
            if(encoded_png.size()>(uint64_t(1)<<30)-stored)throw RuntimeError("Chitubox layers exceed the editor memory budget");
            stored+=encoded_png.size();result.m_layers.push_back(result.m_entries.size());
            result.m_entries.push_back({std::to_string(i)+".png",std::move(encoded_png)});
        }
        if(layout.version>=3) for(size_t i=0;i<layout.count;++i) {
            auto encoded=encode_parameters(chitubox_layer_parameters(layout,i));
            if(encoded.size()>(uint64_t(1)<<30)-stored)throw RuntimeError("Chitubox parameters exceed editor budget");
            stored+=encoded.size();result.m_layer_parameters.push_back(result.m_entries.size());
            result.m_entries.push_back({std::to_string(i)+"-parameters.json",std::move(encoded)});
        }
        check_layer_progress(progress,layout.count,layout.count);
        result.m_entries[0].bytes=std::move(bytes);return result;
    }
    const uint8_t cxdlpv1_signature[]{0,0,0,9,'C','X','S','W'};
    if(input.gcount()==8 && std::memcmp(signature,cxdlpv1_signature,8)==0) {
        input.seekg(0,std::ios::end);const auto length=input.tellg();
        if(length<19 || length>std::streamoff(uint64_t(1)<<30)) throw RuntimeError("CXDLPv1 exceeds the editor memory budget or is truncated");
        std::vector<uint8_t> bytes(static_cast<size_t>(length));input.seekg(0);
        read_bytes(input,bytes);
        if(bytes[13]==0 && bytes[14]==4) {
            const CrealityCXDLPv4Layout layout(bytes);
            LayerDocument result;result.m_archive_format="cxdlpv4";
            result.m_entries.push_back({"cxdlpv4-original",{}});
            std::unordered_map<std::string_view,size_t> cache;
            uint64_t stored=bytes.size();
            for(size_t i=0;i<layout.layers.size();++i) {
                check_layer_progress(progress,i,layout.layers.size());
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
            check_layer_progress(progress,layout.layers.size(),layout.layers.size());
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
            check_layer_progress(progress,layer,count);
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
                    if(line%4096==0)check_layer_progress(progress,layer,count);
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
        check_layer_progress(progress,count,count);return result;
    }
    if (input.gcount() == 8 && png::is_png({signature, sizeof(signature)})) {
        input.seekg(0, std::ios::end);
        const auto length = input.tellg();
        if (length < 33 || length > (uint64_t(1) << 30))
            throw RuntimeError("PNG file exceeds the editor memory budget or is truncated");
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
        if (length <= 0 || length > std::streamoff(uint64_t(1) << 30))
            throw RuntimeError("Layer image exceeds the editor memory budget or is empty");
        std::vector<uint8_t> bytes(static_cast<size_t>(length));
        input.seekg(0); read_bytes(input,bytes);
        const auto image = decode_layer_image({bytes.data(), bytes.size()}, format->id);
        auto result = from_rasters({PNGRasterEncoder{}(image.buf.data(), image.cols, image.rows, 1)});
        result.m_archive_format = format->id;
        check_layer_progress(progress,1,1);return result;
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
        check_layer_progress(progress,i,mz_zip_reader_get_num_files(&zip.arch));
        mz_zip_archive_file_stat stat;
        if (!mz_zip_reader_file_stat(&zip.arch, i, &stat)) throw FileIOError(zip.get_errorstr());
        if (mz_zip_reader_is_file_a_directory(&zip.arch, i)) continue;
        std::string name = stat.m_filename, lower = boost::algorithm::to_lower_copy(name);
        const bool duplicate = !names.insert(lower).second;
        if (stat.m_uncomp_size > limit - total) throw RuntimeError("Layer editor archive exceeds the 1 GiB memory budget");
        total += stat.m_uncomp_size;
        Entry entry{name, std::vector<uint8_t>(size_t(stat.m_uncomp_size))};
        struct Extract {
            Entry &entry;const LayerProgress &progress;size_t index,total;bool cancelled=false;
            std::exception_ptr error;
        } extract{entry,progress,i,mz_zip_reader_get_num_files(&zip.arch),false,{}};
        // Never throw through miniz's C callback. Abort extraction first,
        // then propagate cancellation/errors with normal C++ cleanup.
        auto copy=[](void *opaque,mz_uint64 at,const void *data,size_t length)->size_t {
            auto &state=*static_cast<Extract *>(opaque);
            try {
                if(state.progress && !state.progress(state.index,state.total)){state.cancelled=true;return 0;}
                if(at>state.entry.bytes.size() || length>state.entry.bytes.size()-at)throw RuntimeError("ZIP extraction exceeds the entry bounds");
                if(length)std::memcpy(state.entry.bytes.data()+size_t(at),data,length);return length;
            } catch(...){state.error=std::current_exception();return 0;}
        };
        const bool extracted=mz_zip_reader_extract_to_callback(&zip.arch,i,copy,&extract,0);
        if(extract.error)std::rethrow_exception(extract.error);
        if(extract.cancelled)throw LayerOperationCancelled{};
        if(!extracted)throw FileIOError(zip.get_errorstr());
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
            check_layer_progress(progress);
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
            check_layer_progress(progress,i,result.m_entries.size());
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
            const auto &bytes = entry.bytes;
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
    if (anycubic || chitubox || encrypted) {
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
    if(encrypted) {
        const CTBEncryptedLayerLayout layout(m_entries[0].bytes);
        geometry(layout.columns,layout.rows,layout.count);
        if(m_archive_format!=(layout.version==4?"ctb_encrypted_v4":"ctb_encrypted") || !m_secondary_layers.empty())
            throw RuntimeError("Persisted encrypted format or planes mismatch");
        height_limit=layout.parameter(20);
        for(size_t i=0;i<layer_count();++i)originals.push_back(ctb_encrypted_layer_parameters(layout,i));
    } else if(chitubox) {
        const ChituboxLayerLayout layout(m_entries[0].bytes);
        geometry(layout.columns,layout.rows,layout.count);
        const std::string format=layout.magic==0xff220810?"ctb_gktwo":"ctb_v"+std::to_string(layout.version);
        if(m_archive_format!=format || !m_secondary_layers.empty())
            throw RuntimeError("Persisted Chitubox format or planes mismatch");
        height_limit=layout.number(16);
        for(size_t i=0;i<layer_count();++i)originals.push_back(chitubox_layer_parameters(layout,i));
    } else {
        const AnycubicLayerLayout layout(m_entries[0].bytes,m_archive_format);
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
                const double canonical=anycubic && field==LayerParameter::LiftSpeed?
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
        if(anycubic)validate_anycubic_motion(values);else validate_chitubox_parameters(values);
    }
}

std::optional<LayerRasterGeometry> LayerDocument::raster_geometry() const
{
    std::optional<LayerRasterGeometry> result;
    if (is_chitubox_layer_format(m_archive_format)) {
        const ChituboxLayerLayout layout(m_entries.at(0).bytes);
        result = LayerRasterGeometry{layout.columns, layout.rows, layout.number(8), layout.number(12)};
    } else if (is_ctb_encrypted_layer_format(m_archive_format)) {
        const CTBEncryptedLayerLayout layout(m_entries.at(0).bytes);
        result = LayerRasterGeometry{layout.columns, layout.rows, layout.parameter(12), layout.parameter(16)};
    } else if (!anycubic_available_versions(m_archive_format).empty()) {
        const AnycubicLayerLayout layout(m_entries.at(0).bytes, m_archive_format);
        if (const auto physical = layout.physical_dimensions())
            result = LayerRasterGeometry{layout.columns, layout.rows, physical->first, physical->second};
    }
    if (result) result->validate();
    return result;
}

std::optional<CXDLPv4NativeJobSnapshot> LayerDocument::cxdlpv4_native_job() const
{
    if (m_archive_format != "cxdlpv4") return std::nullopt;
    const CrealityCXDLPv4Layout layout(m_entries.at(0).bytes);
    return cxdlpv4_native_globals(layout);
}

std::optional<CXDLPv4ReferenceBottomView> LayerDocument::cxdlpv4_reference_bottom(const LayerProgress &progress) const
{
    if (m_archive_format != "cxdlpv4") return std::nullopt;
    check_layer_progress(progress);
    // Validate the original native container once, while reading current masks
    // from canonical entries. No per-field CRC passes or stale mask cache.
    const CrealityCXDLPv4Layout layout(m_entries.at(0).bytes);
    return cxdlpv4_reference_bottom_view(layout, [&](size_t i) { return image(i); }, progress);
}

std::optional<LayerJobParameters> LayerDocument::job_parameters() const
{
    if (m_archive_format == "ctb_v3" || m_archive_format == "ctb_v4" || m_archive_format == "ctb_v5") {
        const ChituboxLayerLayout layout(m_entries.at(0).bytes);
        if (m_archive_format != "ctb_v" + std::to_string(layout.version))
            throw RuntimeError("CTB source version mismatch");
        return chitubox_job_parameters(layout);
    }

    if (is_ctb_encrypted_layer_format(m_archive_format)) {
        using F = LayerParameter;
        const auto &bytes = m_entries.at(0).bytes;
        LayerJobParameters actual;
        uint8_t flag = 0; unsigned enabled = 0;
        const CTBEncryptedLayerLayout layout(bytes);
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
            check_rows(CTBEncryptedLayerLayout(bytes), ctb_encrypted_layer_parameters);
        }
        return actual;
    }
    if (anycubic_available_versions(m_archive_format).empty()) return std::nullopt;
    const AnycubicLayerLayout layout(m_entries.at(0).bytes, m_archive_format);
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
    if (length < 112 || length > std::streamoff(uint64_t(1) << 30))
        throw RuntimeError("Chitubox file exceeds the editor budget or is truncated");
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
    const ChituboxLayerLayout baseline(m_entries.at(0).bytes, check), actual(bytes, check);
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
                if (std::memcmp(baseline.bytes.data() + source_data + offset,
                        bytes.data() + target_data + offset, chunk) != 0)
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
    if(is_ctb_encrypted_layer_format(m_archive_format))
        return ctb_encrypted_parameter_capabilities(CTBEncryptedLayerLayout(m_entries.at(0).bytes));
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
bool LayerDocument::has_parameter_edits() const
{
    if (m_layer_parameters.empty()) return false;
    if(is_ctb_encrypted_layer_format(m_archive_format)) {
        const CTBEncryptedLayerLayout source(m_entries.at(0).bytes);
        for(size_t i=0;i<layer_count();++i)
            if(layer_parameters(i)!=ctb_encrypted_layer_parameters(source,i))return true;
        return false;
    }
    if(is_chitubox_layer_format(m_archive_format)) {
        const ChituboxLayerLayout source(m_entries.at(0).bytes);
        for(size_t i=0;i<layer_count();++i)
            if(layer_parameters(i)!=chitubox_layer_parameters(source,i))return true;
        return false;
    }
    const AnycubicLayerLayout source(m_entries.at(0).bytes,m_archive_format);
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
        if(anycubic_speed)
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
    if(is_ctb_encrypted_layer_format(m_archive_format)) {
        const CTBEncryptedLayerLayout source(m_entries.at(0).bytes);limit=source.parameter(20);
        validate_chitubox_parameters(values);
    } else
    if(is_chitubox_layer_format(m_archive_format)) {
        const ChituboxLayerLayout source(m_entries.at(0).bytes);limit=source.number(16);
        validate_chitubox_parameters(values);
    } else {
        const AnycubicLayerLayout source(m_entries.at(0).bytes,m_archive_format);
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
png::ReadBuf LayerDocument::encoded_layer(size_t layer, LayerMaskPlane plane) const
{
    const auto &bytes = m_entries.at(layer_entry(layer,plane)).bytes;
    return {bytes.data(), bytes.size()};
}

png::ImageGreyscale LayerDocument::image(size_t layer, LayerMaskPlane plane) const
{
    const auto &bytes = m_entries.at(layer_entry(layer,plane)).bytes;
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
        const AnycubicLayerLayout layout(m_entries.at(0).bytes, m_archive_format);
        for (auto &value : pixels) value = layout.quantize(value);
    } else if(chitubox) {
        const bool binary = is_chitubox_binary_format(m_archive_format);
        const auto &original=m_entries.at(0).bytes;
        const unsigned planes = !binary || boost::algorithm::iends_with(m_archive_format,"_v1") ? 1 : original.at(92);
        for(auto &value:pixels)value=ChituboxLayerLayout::quantize(value,binary,planes);
    } else if(m_archive_format=="cxdlpv4" || m_archive_format=="pbm")
        for(auto &value:pixels)value=m_archive_format=="pbm"?uint8_t(value?255:0):value<2?0:uint8_t(value|1);
}

void LayerDocument::replace(size_t layer, const png::ImageGreyscale &image, LayerMaskPlane plane)
{
    const size_t index = layer_entry(layer,plane);
    std::vector<uint8_t> quantized;const uint8_t *pixels=image.buf.data();
    if (m_mask_precision || m_fresh_job || !anycubic_available_versions(m_archive_format).empty() || m_archive_format=="cxdlpv4" ||
        m_archive_format=="pbm" || is_ctb_mask_format(m_archive_format)) {
        quantized=image.buf;quantize_mask(quantized);pixels=quantized.data();
    }
    const auto encoded = PNGRasterEncoder{}(pixels, image.cols, image.rows, 1);
    const auto *data = static_cast<const uint8_t *>(encoded.data());
    std::vector<uint8_t> after(data, data + encoded.size());
    if (after == m_entries[index].bytes) return;
    commit({{index, m_entries[index].bytes, std::move(after)}});
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
            m_entries.at(change.entry); replacement.bytes = data;
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
            entry.bytes = std::move(replacements[i].bytes);
        }
}
void LayerDocument::commit(std::vector<Change> changes)
{
    if (changes.empty()) return;
    constexpr uint64_t history_limit = 128 * 1024 * 1024;
    uint64_t document_bytes = 0, change_bytes = 0;
    for (const auto &entry : m_entries) document_bytes += entry.bytes.size();
    for (const auto &change : changes) {
        if (change.target == Change::Target::Entry)
            document_bytes = document_bytes - change.before.size() + change.after.size();
        change_bytes += change.before.size() + change.after.size();
    }
    if (document_bytes > (uint64_t(1) << 30))
        throw RuntimeError("Edited document exceeds the 1 GiB memory budget");
    if (change_bytes > history_limit)
        throw RuntimeError("This edit exceeds the 128 MiB history budget. Select fewer layers.");
    const uint64_t next_revision = m_next_revision + 1;
    // Allocate replacement buffers before touching either masks or history.
    auto replacements = prepare_replacements(changes, true);
    validate_metadata_edit(changes, replacements);
    m_undo.reserve(m_undo.size() + 1);
    m_export_source.invalidate();
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
                                const LayerProgress &progress)
{
    if (first > last || last >= layer_count()) throw RuntimeError("Invalid layer range");
    if (points.empty()) return true;
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
                auto previous = points.front();
                for (const auto &point : points) {
                    draw_line(mask, previous.x, previous.y, point.x, point.y, radius, value);
                    previous = point;
                }
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
            if (item.bytes != m_entries[index].bytes) {
                history_bytes += item.bytes.size()+m_entries[index].bytes.size();
                if (history_bytes > 128*1024*1024)
                    throw RuntimeError("This edit exceeds the 128 MiB history budget. Select fewer layers.");
                changes.push_back({index, m_entries[index].bytes, std::move(item.bytes)});
            }
        }
        layer += width;
    }
    if (!checkpoint(count)) return false;
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
bool LayerDocument::undo()
{
    if (m_undo.empty()) return false;
    // Allocate every replacement and the destination history slot before
    // touching any layer: allocation failure must preserve the whole edit.
    auto replacements = prepare_replacements(m_undo.back().changes, false);
    validate_metadata_edit(m_undo.back().changes, replacements);
    m_redo.reserve(m_redo.size() + 1);
    m_export_source.invalidate();
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
    const CTBEncryptedLayerLayout layout(m_entries.at(0).bytes);
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
  if (!anycubic_available_versions(m_archive_format).empty() ? !boost::algorithm::iequals(extension, "."+m_archive_format) : image_format ? !boost::algorithm::iequals(extension, std::string(".")+image_format->id) :
      (is_ctb_mask_format(m_archive_format)) ?
          !boost::algorithm::iends_with(path,std::string(".")+get_archive_entry(m_archive_format.c_str())->ext) :
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
      const auto &original = m_entries.at(0).bytes;
      const AnycubicLayerLayout layout(original, m_archive_format, [&]{check_layer_progress(progress);});
      if (profile) {
        profile->validate(layout.columns, layout.rows);
        if (const auto dimensions = layout.physical_dimensions(); dimensions &&
            (std::abs(dimensions->first-profile->screen_width_mm) > .005 ||
             std::abs(dimensions->second-profile->screen_height_mm) > .005))
          throw RuntimeError("Selected printer screen dimensions differ from the imported Anycubic machine");
      }
      std::vector<uint8_t> output = original;
      auto put32 = [&](size_t at, uint32_t value) { for (unsigned n = 0; n < 4; ++n) output.at(at+n) = uint8_t(value>>(8*n)); };
      auto put_float = [&](size_t at, double value) { const float f = float(value); if (!std::isfinite(f)) throw RuntimeError("Anycubic metadata overflow"); uint32_t bits; std::memcpy(&bits, &f, 4); put32(at, bits); };
      double old_integral = 0, new_integral = 0, previous_z=0, time_delta=0;
      bool changed = false, masks_changed=false, parameters_changed=false, z_changed=false;
      size_t left = layout.columns, top = layout.rows, right = 0, bottom = 0;
      for (size_t i = 0; i < m_layers.size(); ++i) {
        check_layer_progress(progress, i, m_layers.size());
        const auto pixels = image(i);
        const auto old_pixels = layout.decode(i, [&]{check_layer_progress(progress);});
        if (pixels.cols != layout.columns || pixels.rows != layout.rows) throw RuntimeError("Edited Anycubic dimensions changed");
        const auto old_count = std::count_if(old_pixels.begin(), old_pixels.end(), [](uint8_t value){return value != 0;});
        const auto new_count = std::count_if(pixels.buf.begin(), pixels.buf.end(), [](uint8_t value){return value != 0;});
        if (layout.model) for (size_t y = 0; y < pixels.rows; ++y) {
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
        if (pixels.buf == old_pixels) continue;
        changed = masks_changed = true;
        const auto payload = layout.encode(pixels.buf, [&]{check_layer_progress(progress);});
        if (payload.size() > (uint64_t(1)<<30)-output.size()) throw RuntimeError("Edited Anycubic exceeds editor limits");
        const uint32_t address = uint32_t(output.size());
        output.insert(output.end(), payload.begin(), payload.end());
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
        const auto mask=image(i,LayerMaskPlane::Secondary);
        const auto old=layout.decode_subimage(i,[&]{check_layer_progress(progress);});
        if (mask.cols!=layout.columns || mask.rows!=layout.rows)
          throw RuntimeError("Edited Anycubic secondary dimensions changed");
        if (mask.buf==old) continue;
        const auto payload=layout.encode(mask.buf,[&]{check_layer_progress(progress);});
        if (payload.size()>(uint64_t(1)<<30)-output.size()) throw RuntimeError("Edited Anycubic exceeds editor limits");
        const auto sub=layout.subrecord(i);const auto address=uint32_t(output.size());
        output.insert(output.end(),payload.begin(),payload.end());
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
      const AnycubicLayerLayout verified(output, m_archive_format);
      boost::nowide::ofstream stream(temporary.string(), std::ios::binary|std::ios::trunc);
      write_bytes(stream, output.data(), output.size()); stream.close();
      if (!stream) throw FileIOError("Cannot write edited Anycubic layers");
    } else
    if (is_ctb_encrypted_layer_format(m_archive_format)) {
      const auto &original=m_entries.at(0).bytes;
      const CTBEncryptedLayerLayout layout(original,[&]{check_layer_progress(progress);});
      auto output=original,settings=layout.settings;bool changed=false,parameters_changed=false,z_changed=false;double volume=0,previous_z=0,time_delta=0;
      const bool tilting=ctb_encrypted_tilting_vat(layout);
      auto put=[](std::vector<uint8_t> &data,size_t at,uint32_t value){for(unsigned n=0;n<4;++n)data.at(at+n)=uint8_t(value>>(8*n));};
      auto scalar=[&](size_t at,double value) {
          const float f=float(value);if(!std::isfinite(f))throw RuntimeError("CTBEncrypted resin statistics overflow");
          uint32_t bits;std::memcpy(&bits,&f,4);put(settings,at,bits);
      };
      bool increasing_z=true;
      for(size_t i=0;i<m_layers.size();++i) {
        check_layer_progress(progress,i,m_layers.size());const auto mask=image(i);const auto record=layout.record(i);
        if(mask.cols!=layout.columns || mask.rows!=layout.rows)throw RuntimeError("CTBEncrypted mask dimensions changed");
        if(mask.buf!=layout.decode(i,[&]{check_layer_progress(progress,i,m_layers.size());})) {
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
          if(payload.size()+88>(uint64_t(1)<<30)-output.size())throw RuntimeError("CTBEncrypted edited file exceeds editor budget");
          const auto at=uint32_t(output.size());output.insert(output.end(),original.begin()+record,original.begin()+record+88);
          put(output,layout.table+16*i,at);put(output,at+16,at+88);put(output,at+24,uint32_t(payload.size()));
          put(output,at+32,aes_offset);put(output,at+36,aes_length);output.insert(output.end(),payload.begin(),payload.end());
        }
        const auto values=converted_table ? converted_table->at(i) : layer_parameters(i);
        const auto before=ctb_encrypted_layer_parameters(layout,i);
        if(values!=before) {
          parameters_changed=true;
          if (!converted_table)
            time_delta+=ctb_encrypted_layer_seconds(values,tilting)-ctb_encrypted_layer_seconds(before,tilting);
          const auto at=CTBEncryptedLayerLayout::integer(output,layout.table+16*i);
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
        volume+=std::count_if(mask.buf.begin(),mask.buf.end(),[](uint8_t value){return value!=0;})*
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
        const std::string model(model_size ? reinterpret_cast<const char *>(original.data()+model_at) : "",model_size);
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
        std::copy(encrypted.begin(),encrypted.end(),output.begin()+layout.settings_offset);
        // The checksum is unchanged, so its AES(SHA256(checksum)) signature is
        // unchanged too. Move the entire original trailer, including unknown
        // trailing data, after the appended records rather than ending on RLE.
        if(changed) {
        const size_t prefix=layout.version==5?8:0,tail=layout.signature_offset-prefix;
        if(original.size()-tail>(uint64_t(1)<<30)-output.size())throw RuntimeError("CTBEncrypted trailer exceeds editor budget");
        put(output,24,uint32_t(output.size()+prefix));output.insert(output.end(),original.begin()+tail,original.end());
        }
      }
      const CTBEncryptedLayerLayout verified(output);
      if (pricing) {
          const auto totals=pricing->native_totals(volume);
          for (size_t i=0;i<totals.size();++i)
              if (verified.parameter(104+4*i)!=totals[i])
                  throw RuntimeError("CTBEncrypted native totals cannot preserve fresh material pricing");
      }
      boost::nowide::ofstream stream(temporary.string(),std::ios::binary|std::ios::trunc);
      write_bytes(stream,output.data(),output.size());stream.close();
      if(!stream)throw FileIOError("Cannot write edited CTBEncrypted layers");
    } else
    if(is_chitubox_layer_format(m_archive_format)) {
      const auto &original=m_entries.at(0).bytes;const ChituboxLayerLayout layout(original,[&]{check_layer_progress(progress);});
      std::vector<uint8_t> output=original;double volume=0,previous_z=0;bool changed=false,complete_z=true,parameters_changed=false,z_changed=false;double time_delta=0;
      auto put32=[&](size_t at,uint32_t value){for(unsigned n=0;n<4;++n)output.at(at+n)=uint8_t(value>>(8*n));};
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
          const auto mask=image(i);if(mask.cols!=layout.columns || mask.rows!=layout.rows)throw RuntimeError("Edited Chitubox mask dimensions changed");
          nonzero_pixels = std::count_if(mask.buf.begin(),mask.buf.end(),[](uint8_t value){return value!=0;});
          if(mask.buf!=layout.decode(i,[&]{check_layer_progress(progress,i,m_layers.size());})) {
            changed=true;
            for(unsigned plane=0;plane<layout.planes;++plane) {
              const auto payload=layout.encode(mask.buf,i,plane);
              const size_t prefix = layout.version >= 3 ? 84 : 0;
              if (payload.size()+prefix > (uint64_t(1)<<30)-output.size())
                  throw RuntimeError("Edited Chitubox output exceeds the memory budget");
              const auto at = layout.record(i,plane);
              const size_t extended = output.size();
              if (prefix) {
                  const auto old = layout.extended_record(i);
                  output.insert(output.end(),original.begin()+old,original.begin()+old+prefix);
              }
              const auto address = uint32_t(output.size());
              put32(at+12,address);put32(at+16,uint32_t(payload.size()));
              if (prefix) {
                  // Keep the duplicated definition consistent, preserving unknown
                  // bytes and all motion/wait/PWM fields in the extended record.
                  put32(extended+12,address);put32(extended+16,uint32_t(payload.size()));
                  put32(extended+36,uint32_t(prefix+payload.size()));
              }
              output.insert(output.end(),payload.begin(),payload.end());
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
            const auto at=layout.record(i),ex=size_t(output.at(at+12)) |
                (size_t(output.at(at+13))<<8) | (size_t(output.at(at+14))<<16) | (size_t(output.at(at+15))<<24);
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
        const auto slicer=layout.integer(104);output.at(slicer+39)=uint8_t(layout.version*0x10);
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
      const ChituboxLayerLayout verified(output);
      if (pricing) {
          const auto totals=pricing->native_totals(volume);
          if (!verified.parameters) throw RuntimeError("Fresh Chitubox material totals require native parameter fields");
          for (size_t i=0;i<totals.size();++i)
              if (verified.number(verified.parameters+20+4*i)!=totals[i])
                  throw RuntimeError("Chitubox native totals cannot preserve fresh material pricing");
      }
      boost::nowide::ofstream stream(temporary.string(),std::ios::binary|std::ios::trunc);
      write_bytes(stream,output.data(),output.size());stream.close();
      if(!stream)throw FileIOError("Cannot write edited Chitubox layers");
    } else     if(m_archive_format=="cxdlpv4" && m_revision==0) {
      const auto &original=m_entries.at(0).bytes;
      boost::nowide::ofstream stream(temporary.string(),std::ios::binary|std::ios::trunc);
      write_bytes(stream,original.data(),original.size());stream.close();
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
        check_layer_progress(progress,i,m_layers.size());
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
        check_layer_progress(progress,i,m_layers.size());
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
      write_bytes(stream,output.data(),output.size());stream.close();
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
        check_layer_progress(progress,i,m_layers.size());
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
      write_bytes(stream,prefix.data(),prefix.size());
      for(const auto &layer:layers)stream.write(static_cast<const char *>(layer.data()),4);
      stream.write("\r\n",2);
      if(version>=3) {const auto &informative=m_entries.at(2).bytes;write_bytes(stream,informative.data(),informative.size());}
      for(const auto &layer:layers){write_bytes(stream,layer.data(),layer.size());stream.write("\r\n",2);}
      write_bytes(stream,footer.data(),version==1?footer.size():footer.size()-4);
      stream.close();if(!stream)throw FileIOError("Cannot write edited CXDLP layers");
      if(version>=2) {
        boost::nowide::ifstream checksum_input(temporary.string(),std::ios::binary);
        const auto checksum=creality_checksum(checksum_input,version);
        stream.open(temporary.string(),std::ios::binary|std::ios::app);
        for(unsigned j=0;j<4;++j)stream.put(char(checksum>>(24-8*j)));
        stream.close();if(!stream)throw FileIOError("Cannot write edited CXDLP checksum");
      }
    } else if (image_format) {
      if (m_layers.size() != 1) throw RuntimeError("A standalone image cannot contain a layer stack");
      const auto encoded = encode_layer_image(image(0), image_format->id);
      const png::ReadBuf bytes{encoded.data(), encoded.size()};
      boost::nowide::ofstream stream(temporary.string(), std::ios::binary | std::ios::trunc);
      write_bytes(stream,bytes.buf,bytes.sz);
      stream.close();
      if (!stream) throw FileIOError("Cannot write edited layer image");
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
          zip.add_entry(entry.name, entry.bytes.data(), entry.bytes.size());
      }
      zip.finalize();
    }
    check_layer_progress(progress,m_layers.size(),m_layers.size());
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
