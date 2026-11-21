// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "RasterBase.hpp"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <optional>
#include <vector>
#include <boost/log/trivial.hpp>

namespace Slic3r::sla::encoded_mask_reuse_detail {
// Experimental policy, not a codec/file limit. Large or incompatible batches
// take the normal encoding path. Metadata is separately bounded by max_layers;
// the payload budget does not claim to include allocator/vector overhead.
struct Budget {
    size_t payload_bytes = size_t(64) << 20;
    size_t max_layers = 65536;
};
inline constexpr size_t copy_chunk_bytes = size_t(1) << 20;
inline bool experiment_enabled()
{
    const char *value = std::getenv("SLA_EXPERIMENT_ENCODED_MASK_REUSE");
    // Default enabled; an explicit zero or any unknown spelling keeps the
    // conservative encoder fallback. Only the exact string "1" opts back in.
    return value == nullptr || std::strcmp(value, "1") == 0;
}
struct Batch {
    std::vector<EncodedRaster> layers;
    size_t payload_bytes = 0;
};
// This helper checks copy eligibility, not authenticity of arbitrary RLE.
// Only the private engine receipt may turn a copied batch into a writer input.
// All cancellation runs on the calling/coordinator thread. No partial batch
// escapes if a callback or allocation throws.
inline std::optional<size_t> cxdlpv4_batch_payload_size(const std::vector<EncodedRaster> &source,
    size_t expected_layers, const std::function<void()> &check = {}, Budget budget = {})
{
    const auto checkpoint = [&] { if (check) check(); };
    checkpoint();
    if (!expected_layers || source.size() != expected_layers || source.size() > budget.max_layers)
        return std::nullopt;
    size_t payload = 0;
    for (const auto &layer : source) {
        checkpoint();
        if (!layer.size() || !layer.data() || std::strcmp(layer.extension(), "cxdlpv4rle") != 0 ||
            layer.size() > budget.payload_bytes - payload)
            return std::nullopt;
        payload += layer.size();
    }
    return payload;
}
inline std::optional<Batch> copy_cxdlpv4_batch(const std::vector<EncodedRaster> &source,
    size_t expected_layers, const std::function<void()> &check = {}, Budget budget = {})
{
    const auto payload = cxdlpv4_batch_payload_size(source, expected_layers, check, budget);
    if (!payload) return std::nullopt;
    const auto checkpoint = [&] { if (check) check(); };
    Batch result;
    result.layers.reserve(source.size());
    for (const auto &layer : source) {
        checkpoint();
        std::vector<uint8_t> bytes;
        bytes.reserve(layer.size());
        const auto *begin = static_cast<const uint8_t *>(layer.data());
        for (size_t offset = 0; offset < layer.size();) {
            checkpoint();
            const size_t count = std::min(copy_chunk_bytes, layer.size() - offset);
            bytes.insert(bytes.end(), begin + offset, begin + offset + count);
            offset += count;
        }
        result.layers.emplace_back(std::move(bytes), "cxdlpv4rle");
    }
    checkpoint();
    result.payload_bytes = *payload;
    // Opt-in cost evidence: emitted only after all payload bytes were actually
    // copied and the final cancellation check succeeded.
    if (std::getenv("SLA_TRACE_EXPORT_STAGES"))
        BOOST_LOG_TRIVIAL(info) << "CXDLPv4 encoded batch copied: " << *payload << " bytes";
    return result;
}
} // namespace Slic3r::sla::encoded_mask_reuse_detail
