// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ExportRequest.hpp"
#include "libslic3r/Format/SLAArchiveFormatRegistry.hpp"

namespace Slic3r::sla {
namespace {
ExportGenerationGuard request_guard(const std::shared_ptr<const ExportSnapshot> &snapshot)
{
    if (!snapshot) throw RuntimeError("Export request requires a snapshot");
    return snapshot->generation().with_cancellation();
}
std::string request_format(const std::string &format)
{
    const auto *entry = get_archive_entry(format.c_str());
    if (!entry) throw ExportError("Unknown SLA request format: " + format);
    const auto reason = archive_export_unavailable_reason(entry->id);
    if (!reason.empty()) throw ExportError(reason);
    return entry->id;
}
}
ExportRequest::ExportRequest(std::shared_ptr<const ExportSnapshot> snapshot, std::string format,
    std::string path, std::string projectname, bool palette_approved)
    : m_snapshot(std::move(snapshot)), m_generation(request_guard(m_snapshot)),
      m_format(std::move(format)), m_path(std::move(path)), m_projectname(std::move(projectname)),
      m_palette_approved(palette_approved)
{
    m_format = request_format(m_format);
    if (m_path.empty()) throw ExportError("Export request requires an output path");
}
void ExportRequest::write(const std::function<void()> &checkpoint) const
{
    m_generation.check();
    if (checkpoint) checkpoint();
    m_generation.check();
    SLAPrint::write_export_snapshot(*m_snapshot, m_generation, m_format, m_path, m_projectname, m_palette_approved, checkpoint);
}
NativeArchiveExportRequest::NativeArchiveExportRequest(const LayerDocument &source, std::string path,
    std::optional<LayerExportProfile> profile, const LayerProgress &progress)
    : m_generation(source.export_generation_guard().with_cancellation()),
      m_path(std::move(path)), m_profile(std::move(profile))
{
    m_generation.check();
    if (m_path.empty()) throw ExportError("Native save requires an output path");
    if (source.archive_format().empty()) throw ExportError("Native save requires an imported container");
    const auto capture_progress = [&](size_t done, size_t total) {
        m_generation.check();
        check_layer_progress(progress, done, total);
        m_generation.check();
        return true;
    };
    m_document = std::make_shared<const LayerDocument>(source.snapshot(capture_progress));
    m_generation.check();
}
void NativeArchiveExportRequest::write(const LayerProgress &progress) const
{
    const auto write_progress = [&](size_t done, size_t total) {
        m_generation.check();
        if (progress && !progress(done, total)) {
            m_generation.cancel();
            throw LayerOperationCancelled{};
        }
        m_generation.check();
        return true;
    };
    m_generation.check();
    m_document->save_archive_impl(m_path, write_progress, m_profile ? &*m_profile : nullptr,
        nullptr, nullptr, nullptr, &m_generation);
    // Publication is the linearization point. A later cancel/source change
    // cannot turn a completed rename into an unperformed write.
    m_published->store(true, std::memory_order_release);
}
bool NativeArchiveExportRequest::acknowledge_saved(LayerDocument &source) const
{
    return m_published->load(std::memory_order_acquire) && source.mark_saved_if_current(m_generation);
}

NativeArchiveConversionRequest::NativeArchiveConversionRequest(const LayerDocument &source,
    DynamicPrintConfig configuration, std::string format, std::string path, const LayerProgress &progress)
    : m_generation(source.export_generation_guard().with_cancellation()),
      m_configuration(std::move(configuration)), m_format(request_format(format)), m_path(std::move(path))
{
    m_generation.check();
    if (m_path.empty()) throw ExportError("Conversion requires an output path");
    const auto capture_progress = [&](size_t done, size_t total) {
        m_generation.check();check_layer_progress(progress, done, total);m_generation.check();return true;
    };
    m_document = std::make_shared<const LayerDocument>(source.snapshot(capture_progress));
    m_generation.check();
}
void NativeArchiveConversionRequest::write(const LayerProgress &progress) const
{
    const auto guarded_progress = [&](size_t done, size_t total) {
        m_generation.check();
        if (progress && !progress(done, total)) {
            m_generation.cancel();
            throw LayerOperationCancelled{};
        }
        m_generation.check();
        return true;
    };
    m_generation.check();
    SLAPrint::convert_layer_archive_owned(m_configuration, *m_document, m_format, m_path,
        m_generation, guarded_progress);
}

bool ExportInputGuard::same_source(const ExportGenerationGuard &output) const
{
    return m_state && m_state == output.m_state;
}

struct DeferredExportRequest::State {
    std::mutex mutex;
    std::atomic<bool> cancelled{false};
    std::optional<ExportRequest> ready;
};
DeferredExportRequest::DeferredExportRequest(ExportInputGuard input, std::string format,
    std::string path, std::string projectname)
    : m_state(std::make_shared<State>()), m_input(std::move(input)),
      m_format(request_format(format)), m_path(std::move(path)), m_projectname(std::move(projectname))
{
    if (m_path.empty()) throw ExportError("Export request requires an output path");
    check();
}
void DeferredExportRequest::check() const
{
    m_input.check();
    if (cancelled()) throw CanceledException{};
}
bool DeferredExportRequest::cancelled() const
{
    return m_state->cancelled.load(std::memory_order_acquire);
}
void DeferredExportRequest::cancel() const
{
    std::lock_guard<std::mutex> lock(m_state->mutex);
    if (m_state->ready) m_state->ready->cancel();
    m_state->cancelled.store(true, std::memory_order_release);
}
ExportRequest DeferredExportRequest::bind(std::shared_ptr<const ExportSnapshot> snapshot, bool palette_approved) const
{
    std::lock_guard<std::mutex> lock(m_state->mutex);
    check();
    if (!snapshot) throw ExportError("Deferred export requires a ready snapshot");
    if (!m_input.same_source(snapshot->generation())) throw ExportError("Export result belongs to another source");
    if (m_state->ready) throw ExportError("Export request is already bound to a result");
    const auto path = snapshot->statistics().finalize_output_path(m_path);
    ExportRequest ready(snapshot, m_format, path, m_projectname, palette_approved);
    // Capture may race an external mutation. A result accepted between two
    // input changes must never become the output of the earlier intent.
    check();
    ready.generation().check();
    m_state->ready = ready;
    return ready;
}
} // namespace Slic3r::sla
