// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "ExportSnapshot.hpp"
#include "LayerExportProfile.hpp"

namespace Slic3r::sla {
// A selected output belongs to this request, not to ambient GUI preferences.
// Snapshot data is immutable and cancellation is independently owned.
class ExportRequest {
    std::shared_ptr<const ExportSnapshot> m_snapshot;
    ExportGenerationGuard m_generation;
    std::string m_format, m_path, m_projectname;
    bool m_palette_approved;
public:
    ExportRequest(std::shared_ptr<const ExportSnapshot> snapshot, std::string format,
                  std::string path, std::string projectname = {}, bool palette_approved = false);
    const ExportSnapshot &snapshot() const { return *m_snapshot; }
    const std::string &format() const { return m_format; }
    const std::string &path() const { return m_path; }
    const std::string &projectname() const { return m_projectname; }
    const ExportGenerationGuard &generation() const { return m_generation; }
    bool palette_approved() const { return m_palette_approved; }
    bool cancelled() const { return m_generation.cancelled(); }
    void cancel() const { m_generation.cancel(); }
    // Optional worker checkpoints may cancel/throw, but are never called
    // under the publication mutex. No source SLAPrint pointer is retained.
    void write(const std::function<void()> &checkpoint = {}) const;
};

// Preserving save of an imported container. Format and metadata remain those
// of the captured document; this is separate from converting to another codec.
class NativeArchiveExportRequest {
    std::shared_ptr<const LayerDocument> m_document;
    ExportGenerationGuard m_generation;
    std::string m_path;
    std::optional<LayerExportProfile> m_profile;
    std::shared_ptr<std::atomic<bool>> m_published = std::make_shared<std::atomic<bool>>(false);
public:
    NativeArchiveExportRequest(const LayerDocument &source, std::string path,
        std::optional<LayerExportProfile> profile = {}, const LayerProgress &progress = {});
    const LayerDocument &document() const { return *m_document; }
    const std::string &path() const { return m_path; }
    const ExportGenerationGuard &generation() const { return m_generation; }
    void cancel() const { m_generation.cancel(); }
    bool cancelled() const { return m_generation.cancelled(); }
    void write(const LayerProgress &progress = {}) const;
    bool acknowledge_saved(LayerDocument &source) const;
};

// Conversion owns its accepted printer configuration as well as the source
// snapshot. It produces a copy and never acknowledges a native/project save.
class NativeArchiveConversionRequest {
    std::shared_ptr<const LayerDocument> m_document;
    ExportGenerationGuard m_generation;
    DynamicPrintConfig m_configuration;
    std::string m_format, m_path;
public:
    NativeArchiveConversionRequest(const LayerDocument &source, DynamicPrintConfig configuration,
        std::string format, std::string path, const LayerProgress &progress = {});
    const LayerDocument &document() const { return *m_document; }
    const DynamicPrintConfig &configuration() const { return m_configuration; }
    const std::string &format() const { return m_format; }
    const std::string &path() const { return m_path; }
    const ExportGenerationGuard &generation() const { return m_generation; }
    void cancel() const { m_generation.cancel(); }
    bool cancelled() const { return m_generation.cancelled(); }
    void write(const LayerProgress &progress = {}) const;
};

// Accepted output intent may precede slicing. It owns user choices and can
// bind exactly once to the corresponding ready result, never to another job.
// Copies share cancellation through both the deferred and ready phases.
class DeferredExportRequest {
    struct State;
    std::shared_ptr<State> m_state;
    ExportInputGuard m_input;
    std::string m_format, m_path, m_projectname;
public:
    DeferredExportRequest(ExportInputGuard input, std::string format,
                          std::string path, std::string projectname = {});
    const std::string &format() const { return m_format; }
    const std::string &path() const { return m_path; }
    const std::string &projectname() const { return m_projectname; }
    void check() const;
    void cancel() const;
    bool cancelled() const;
    ExportRequest bind(std::shared_ptr<const ExportSnapshot> snapshot, bool palette_approved = false) const;
};
} // namespace Slic3r::sla
