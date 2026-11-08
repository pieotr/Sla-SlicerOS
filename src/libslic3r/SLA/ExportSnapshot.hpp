// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "libslic3r/SLAPrint.hpp"

namespace Slic3r::sla {
// Effective object settings needed by writers, with no borrowed ModelObject
// pointers, meshes or edit history.
struct ExportObjectMetadata {
    std::string name;
    SLAPrintObjectConfig configuration;
};
struct ExportVectorLayer {
    coord_t level;
    ExPolygons contours;
};
// Immutable capture of a ready SLA result. Pixel jobs retain their canonical
// document; vector-only jobs retain contours without SliceRecord references.
class ExportSnapshot {
    friend class Slic3r::SLAPrint;
    explicit ExportSnapshot(ExportGenerationGuard generation) : m_generation(std::move(generation)) {}
    ExportGenerationGuard m_generation;
    DynamicPrintConfig m_configuration;
    DynamicConfig m_placeholders;
    std::optional<LayerDocument> m_document;
    std::vector<coord_t> m_levels;
    std::vector<ExportVectorLayer> m_vector_layers;
    std::vector<ExportObjectMetadata> m_objects;
    SLAPrintStatistics m_statistics;
    std::shared_ptr<const CalibrationRecipe> m_recipe;
    ThumbnailsList m_thumbnails;
    bool m_raster_job = false;
    // Optional, immutable CXDLPv4 RLE only. This is never serialized into a
    // layer project, and never replaces the authoritative canonical document.
    std::shared_ptr<const std::vector<EncodedRaster>> m_cxdlpv4_encoding;
    size_t m_cxdlpv4_encoding_bytes = 0;
public:
    // Observational diagnostics, not a public cache-installation capability.
    size_t reusable_cxdlpv4_layer_count() const { return m_cxdlpv4_encoding ? m_cxdlpv4_encoding->size() : 0; }
    size_t reusable_cxdlpv4_payload_bytes() const { return m_cxdlpv4_encoding_bytes; }
    const ExportGenerationGuard &generation() const { return m_generation; }
    const DynamicPrintConfig &configuration() const { return m_configuration; }
    const DynamicConfig &placeholders() const { return m_placeholders; }
    const std::optional<LayerDocument> &document() const { return m_document; }
    const std::vector<coord_t> &levels() const { return m_levels; }
    const std::vector<ExportVectorLayer> &vector_layers() const { return m_vector_layers; }
    const std::vector<ExportObjectMetadata> &objects() const { return m_objects; }
    const SLAPrintStatistics &statistics() const { return m_statistics; }
    const std::shared_ptr<const CalibrationRecipe> &calibration_recipe() const { return m_recipe; }
    const ThumbnailsList &thumbnails() const { return m_thumbnails; }
    bool is_raster_job() const { return m_raster_job; }
};
} // namespace Slic3r::sla
