///|/ Copyright (c) Prusa Research 2022 - 2023 Tomáš Mészáros @tamasmeszaros
///|/ Copyright (c) 2023 Mimoja @Mimoja
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "SLAArchiveWriter.hpp"

#include "SLAArchiveFormatRegistry.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Exception.hpp"
#include "libslic3r/SLA/LayerDocument.hpp"
#include <unordered_map>
#include <string_view>
#include <atomic>

namespace Slic3r {
void SLAArchiveWriter::encode_layers(const sla::LayerDocument &document, const std::function<void()> &check_cancel)
{
    const auto poll = [&] { if (check_cancel) check_cancel(); };
    poll();
    std::vector<sla::EncodedRaster> encoded(document.layer_count());
    const auto encoder = get_document_encoder(document);
    if (!encoder) throw RuntimeError("The selected SLA format has no raster encoder");
    if (encoded.empty()) throw RuntimeError("Cannot encode an empty layer document");
    const auto first = document.image(0);
    const uint64_t pixels = uint64_t(first.cols) * first.rows;
    const int workers = int(std::clamp<uint64_t>((uint64_t(512) << 20) / std::max<uint64_t>(1, pixels * 4), 1, 8));
    std::vector<size_t> representative(encoded.size()), copies(encoded.size(), 0), unique;
    std::unordered_map<std::string_view, size_t> masks;
    for (size_t i = 0; i < encoded.size(); ++i) {
        poll();
        const auto png = document.encoded_layer(i);
        const std::string_view key{static_cast<const char *>(png.buf), png.sz};
        const auto found = masks.find(key);
        if (found != masks.end()) representative[i] = found->second;
        else {
            representative[i] = i; unique.push_back(i);
            if (masks.size() < 1024) masks.emplace(key, i);
        }
        ++copies[representative[i]];
    }
    // Compare full immutable PNG bytes, not just hashes. Calibration stacks
    // commonly repeat one mask thousands of times. Validate/decode a distinct
    // mask once, then reuse its encoding for every identical layer.
    const bool png_container = encoder.target<sla::PNGRasterEncoder>() != nullptr;
    constexpr uint64_t encoded_budget = uint64_t(1) << 30;
    std::atomic<uint64_t> total_bytes{0};
    tbb::task_arena arena(workers);
    // Arbitrary checkpoints run on the coordinator, never encoder workers.
    for (size_t first_index=0; first_index<unique.size(); first_index+=size_t(workers)) {
      poll();
      const auto end=std::min(first_index+size_t(workers),unique.size());
      arena.execute([&] { execution::for_each(ExecutionTBB{}, first_index, end, [&](size_t index) {
        const size_t i = unique[index];
        const auto loaded = i == 0 ? png::ImageGreyscale{} : document.image(i);
        const auto &image = i == 0 ? first : loaded;
        if (image.cols != first.cols || image.rows != first.rows)
            throw RuntimeError("All layers must have the same raster dimensions");
        if (png_container) {
            const auto png = document.encoded_layer(i);
            const auto *begin = static_cast<const uint8_t *>(png.buf);
            encoded[i] = sla::EncodedRaster(std::vector<uint8_t>(begin, begin + png.sz), "png");
        } else encoded[i] = encoder(image.buf.data(), image.cols, image.rows, 1);
        if (encoded[i].size() == 0) throw RuntimeError("The SLA mask encoder failed");
        if (encoded[i].size() > encoded_budget / copies[i])
            throw RuntimeError("Encoded layers exceed the 1 GiB memory budget");
        const uint64_t bytes = uint64_t(encoded[i].size()) * copies[i];
        if (total_bytes.fetch_add(bytes, std::memory_order_relaxed) + bytes > encoded_budget)
            throw RuntimeError("Encoded layers exceed the 1 GiB memory budget");
      }); });
      poll();
    }
    for (size_t i = 0; i < encoded.size(); ++i) {
        poll();
        if (representative[i] != i) encoded[i] = encoded[representative[i]];
    }
    poll();
    m_layers.swap(encoded);
}

namespace {
// Keep unsupported presets loadable. Report the problem during slicing/export,
// where the application's job error handling can present it to the user.
class UnavailableArchive final : public SLAArchiveWriter {
    std::string m_reason;
    std::unique_ptr<sla::RasterBase> create_raster() const override { throw RuntimeError(m_reason); }
    sla::RasterEncoder get_encoder() const override { throw RuntimeError(m_reason); }
public:
    explicit UnavailableArchive(std::string reason) : m_reason(std::move(reason)) {}
    void export_print(const std::string, const SLAPrint &, const ThumbnailsList &,
                      const std::string &) override { throw RuntimeError(m_reason); }
};
}

std::unique_ptr<SLAArchiveWriter>
SLAArchiveWriter::create(const std::string &archtype, const SLAPrinterConfig &cfg)
{
    std::unique_ptr<SLAArchiveWriter> ret;
    if (get_archive_entry(archtype.c_str())) {
        const auto reason = archive_export_unavailable_reason(archtype.c_str());
        if (!reason.empty())
            return std::make_unique<UnavailableArchive>(reason);
    }
    auto factory = get_writer_factory(archtype.c_str());

    if (factory) {
        // Conversion may select a different format than the active printer.
        // Give the writer its actual target, including version-specific IDs.
        auto target_config = cfg;
        target_config.sla_archive_format.value = archtype;
        ret = factory(target_config);
    }

    return ret;
}

} // namespace Slic3r
