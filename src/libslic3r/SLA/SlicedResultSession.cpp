// SPDX-License-Identifier: AGPL-3.0-or-later
#include "SlicedResultSession.hpp"

namespace Slic3r::sla {
SlicedResultSession::SlicedResultSession(std::unique_ptr<SLAPrint> context,
    LayerDocument document, ExportGenerationGuard source)
    : m_context(std::move(context)), m_document(std::move(document)), m_source(std::move(source))
{
    m_document.mark_saved();
}

std::unique_ptr<SlicedResultSession> SlicedResultSession::capture(const SLAPrint &source,
                                                               const LayerProgress &progress)
{
    const auto snapshot = source.capture_export_snapshot_impl({}, progress, false);
    if (!snapshot.document())
        throw RuntimeError("This result contains vector layers. Choose a raster format to edit pixels.");
    const auto check = [&] {
        snapshot.generation().check();
        check_layer_progress(progress);
        snapshot.generation().check();
    };
    check();
    LayerDocument editable;
    {
        // Export snapshots deliberately omit Undo/Redo. An editor must retain
        // committed history when reopened, while binding that copy to the same
        // result as its captured configuration and grid. Never read a borrowed
        // document across a source mutation; copying is bracketed by checks.
        std::scoped_lock<std::mutex> lock(source.state_mutex());
        check();
        if (!source.m_layer_document)
            throw RuntimeError("The sliced layers are no longer available");
        editable = *source.m_layer_document;
        check();
    }
    const auto &document = *snapshot.document();
    std::vector<double> positions;
    positions.reserve(snapshot.levels().size());
    for (auto level : snapshot.levels()) positions.push_back(double(level) * SCALING_FACTOR);
    auto context = SLAPrint::create_raster_job_impl(snapshot.configuration(), document, positions,
        std::nullopt, document.fresh_job() ? document.fresh_job()->position_policy :
            ExposurePositionPolicy::IncreasingLayers,
        &snapshot.statistics(), &snapshot.generation(), check);
    // The raster factory normalizes the first height. The session must retain
    // the captured configuration and exact integer grid, just as frozen export.
    context->m_full_print_config = snapshot.configuration();
    context->m_material_config.apply(snapshot.configuration(), true);
    context->m_printer_input.clear();
    for (auto level : snapshot.levels()) context->m_printer_input.emplace_back(level);
    context->m_print_statistics = snapshot.statistics();
    context->m_placeholder_parser.apply_config(snapshot.placeholders());
    context->m_calibration_recipe = snapshot.calibration_recipe();
    for (const auto &metadata : snapshot.objects()) {
        check();
        auto *object = context->m_model.add_object();
        object->name = metadata.name;
        auto *descriptor = new SLAPrintObject(context.get(), object);
        context->m_objects.push_back(descriptor);
        descriptor->config_apply(metadata.configuration);
    }
    context->m_raster_job = snapshot.is_raster_job();
    context->m_frozen_export_ready = true;
    check();
    // Construction is guarded against concurrent invalidation/cancellation.
    // After capture, the session owns its data independently of source lifetime.
    context->m_frozen_export_guard.reset();
    context->m_frozen_export_checkpoint = {};
    return std::unique_ptr<SlicedResultSession>(new SlicedResultSession(
        std::move(context), std::move(editable), snapshot.generation()));
}

void SlicedResultSession::apply(SLAPrint &source)
{
    source.replace_layer_document(m_document, m_source);
    m_source = source.export_generation_guard();
    m_document.mark_saved();
}
} // namespace Slic3r::sla
