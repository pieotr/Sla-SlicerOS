// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "ExportSnapshot.hpp"

namespace Slic3r::sla {
// An editing session owns the accepted result and its writer configuration.
// It can outlive the scene/job; only Apply requires the original live result.
// Capture/Apply are coordinated by the caller's ready-result/UI ownership.
class SlicedResultSession {
    std::unique_ptr<SLAPrint> m_context;
    LayerDocument m_document;
    ExportGenerationGuard m_source;
    SlicedResultSession(std::unique_ptr<SLAPrint> context, LayerDocument document,
                        ExportGenerationGuard source);
public:
    static std::unique_ptr<SlicedResultSession> capture(const SLAPrint &source,
                                                       const LayerProgress &progress = {});
    LayerDocument &document() { return m_document; }
    const LayerDocument &document() const { return m_document; }
    const SLAPrint &context() const { return *m_context; }
    const DynamicPrintConfig &configuration() const { return m_context->full_print_config(); }
    bool source_current() const { return m_source.current(); }
    // Does not store the source pointer. Failed/stale Apply preserves the edit
    // history and baseline; a successful Apply allows later edits/Apply again.
    void apply(SLAPrint &source);
};
} // namespace Slic3r::sla
