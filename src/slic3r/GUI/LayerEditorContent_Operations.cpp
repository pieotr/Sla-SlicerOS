#include "LayerEditorContent.hpp"

namespace Slic3r::GUI {
namespace layer_editor_detail {
bool LayerEditorContent::context_changed() const
{
    if (m_export_profile.has_value() != m_context_baseline.has_value()) return true;
    if (!m_export_profile) return false;
    const auto &a=*m_export_profile, &b=*m_context_baseline;
    return a.columns!=b.columns || a.rows!=b.rows ||
        a.screen_width_mm!=b.screen_width_mm || a.screen_height_mm!=b.screen_height_mm ||
        a.density_g_ml!=b.density_g_ml || a.cost_per_ml!=b.cost_per_ml ||
        a.max_print_height_mm!=b.max_print_height_mm;
}

bool LayerEditorContent::unsaved_changes() const
{
    // Native export and project persistence have independent baselines.
    // Undo can return to the saved project even after exporting another revision.
    return m_stroke || m_settings_require_project || context_changed() || !(m_project_settings==m_settings_baseline) ||
        (is_model_result() && m_document.revision()!=m_model_project_revision) || (m_project_revision ?
        m_document.revision()!=*m_project_revision : m_document.dirty());
}

void LayerEditorContent::edit_layer_parameters(bool range)
{
    finish_stroke(true);
    const auto layer=size_t((range ? m_first->GetValue() : m_layer->GetValue())-1);
    const auto last=range ? size_t(m_last->GetValue()-1) : layer;
    if (layer > last || last >= m_document.layer_count()) throw RuntimeError("Invalid parameter range");
    const auto values=m_document.layer_parameters(layer);
    wxDialog editor(this,wxID_ANY,range ? wxString::Format("Layer parameters — range %zu–%zu",layer+1,last+1) :
        wxString::Format("Layer parameters — layer %zu",layer+1));
    auto *root=new wxBoxSizer(wxVERTICAL);
    root->Add(new wxStaticText(&editor,wxID_ANY,
        m_document.fresh_job() ?
        "Exposure and movement apply to this record. Z comes from generated layers and is read-only here.\nChanges keep the current layer schedule independent of preset updates." :
        "Parameters apply to this print layer and its mask planes. Read-only fields cannot be changed in this format."),0,wxALL,10);
    if (range) root->Add(new wxStaticText(&editor,wxID_ANY,
        "Values shown come from the first selected record. Only fields you change are applied to the range.\nOther settings and masks are preserved. The whole range is one Undo step."),0,wxALL,10);
    auto *grid=new wxFlexGridSizer(2,8,8);
    std::map<sla::LayerParameter,std::pair<wxTextCtrl *,wxString>> fields;
    for (const auto &capability:m_document.parameter_capabilities()) {
        grid->Add(new wxStaticText(&editor,wxID_ANY,wxString::FromUTF8(capability.label)+
            " ("+wxString::FromUTF8(capability.unit)+")"),0,wxALIGN_CENTER_VERTICAL);
        const auto text=wxString::Format("%.9g",values.at(capability.field));
        auto *input=new wxTextCtrl(&editor,wxID_ANY,text,wxDefaultPosition,wxDefaultSize,
            capability.editable ? 0 : wxTE_READONLY);
        input->SetName(wxString::FromUTF8(capability.label));grid->Add(input,1,wxEXPAND);
        if (capability.editable) fields.emplace(capability.field,std::make_pair(input,text));
    }
    grid->AddGrowableCol(1);root->Add(grid,1,wxEXPAND|wxALL,10);
    root->Add(editor.CreateButtonSizer(wxOK|wxCANCEL),0,wxEXPAND|wxALL,10);
    static_cast<wxButton *>(editor.FindWindow(wxID_OK))->SetLabel("Apply parameters");
    editor.SetSizerAndFit(root);editor.CentreOnParent();
    while (editor.ShowModal()==wxID_OK) {
        try {
            sla::LayerParameters patch;
            for (const auto &[field,input]:fields) if (input.first->GetValue()!=input.second) {
                double value=0;
                if (!input.first->GetValue().ToDouble(&value)) throw RuntimeError("Enter a numeric layer parameter");
                patch.emplace(field,value);
            }
            if (range && !patch.empty()) {
                auto table=m_document.fresh_job()->rows;
                for (size_t i=layer; i<=last; ++i)
                    for (const auto &[field,value]:patch) table[i][field]=value;
                m_document.replace_layer_parameter_table(table);
            } else if (!range) m_document.set_layer_parameters(layer,patch);
            return;
        } catch (const std::exception &error) {
            wxMessageBox(wxString::FromUTF8(error.what()),"Layer parameters",wxOK|wxICON_ERROR,&editor);
        }
    }
}

layerops::Mask LayerEditorContent::read_layer_mask(size_t layer) const
{
    const auto image = m_document.image(layer, selected_plane());
    layerops::Mask mask(image.cols, image.rows);
    mask.px = image.buf;
    return mask;
}

std::string LayerEditorContent::operation_list_refusal(bool changes_resolution) const
{
    // Applying a model result writes the document back into the print, whose layer count is
    // fixed by the slicing levels; a different layer list could not be applied.
    if (is_model_result())
        return "Layer count, heights and resolution of a sliced model result are fixed by the slicing. "
               "Pixel operations are available here; to clone, remove or re-height layers export the layers "
               "and open them as a layer file, or change the layer height and slice again.";
    return m_document.layer_list_refusal(changes_resolution);
}

std::vector<double> LayerEditorContent::operation_layer_z() const
{
    std::vector<double> z(m_document.layer_count());
    const bool has_rows = m_document.fresh_job() || m_document.parameter_operation_refusal().empty();
    for (size_t i = 0; i < z.size(); ++i) {
        const auto row = has_rows ? m_document.layer_parameters(i) : sla::LayerParameters{};
        const auto found = row.find(sla::LayerParameter::PositionZ);
        z[i] = found != row.end() ? found->second : kFallbackLayerHeightMm * double(i + 1);
    }
    return z;
}

RunOutcome LayerEditorContent::open_lithophane_as_new_document(const LayerOpsJob &job)
{
    if (!m_document.fresh_job())
        throw RuntimeError("The lithophane needs printer geometry. Slice a model or open a generated layer project first, "
                           "then build the lithophane from there.");
    std::vector<layerops::Mask> layers; std::vector<double> z; std::vector<layerops::LayerParams> table;
    bool built = false;
    {
        TaskLock working(m_working);
        built = run_background_layers(this, "Build lithophane", [&](const sla::LayerProgress &progress) {
            try { job.lithophane(layers, z, table, progress); return true; }
            catch (const sla::LayerOperationCancelled &) { return false; }
        });
    }
    if (!built) return RunOutcome::Cancelled;
    auto document = sla::LayerDocument::from_parameter_masks(*m_document.fresh_job(), layers, z, table);
    wxFileDialog save(this, "Save the lithophane layer project", "", "lithophane.sla-project",
                      "SLA layer project (*.sla-project)|*.sla-project", wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
    if (save.ShowModal() != wxID_OK) return RunOutcome::Cancelled;
    const auto path = boost::filesystem::path(into_u8(save.GetPath())).replace_extension(".sla-project").string();
    sla::ProjectRasterState state{std::move(document), {}, "", std::nullopt, sla::ProjectLayerOrigin::ImportedArchive, std::nullopt};
    TaskLock working(m_working);
    const bool saved = run_background_layers(this, "Save layer project", [&](const sla::LayerProgress &progress) {
        try { sla::save_layer_project(path, state, progress); return true; }
        catch (const sla::LayerOperationCancelled &) { return false; }
    });
    if (!saved) return RunOutcome::Cancelled;
    wxTheApp->CallAfter([path] { if (auto *plater = wxGetApp().plater()) plater->open_layer_file(path); });
    return RunOutcome::Elsewhere;
}

RunOutcome LayerEditorContent::run_operation_job(const LayerOpsJob &job)
{
    if (m_working) return RunOutcome::Cancelled;
    finish_stroke(true);
    m_op_preview.reset();
    sla::LayerDocument::ParameterOperation param_op = job.param_op;
    bool param_list = job.param_changes_list, param_masks = job.param_needs_masks;
    sla::LayerDocument::ListStreamOperation stream = job.stream;
    bool stream_params = job.stream_params;
    if (job.lithophane) {
        // In place only when the layer list may change (generated documents, not a model result).
        if (!operation_list_refusal(false).empty() || !m_document.fresh_job() || !m_document.parameter_operation_refusal().empty())
            return open_lithophane_as_new_document(job);
        if (job.lithophane_stream) {
            // Streamed in place: the stack is encoded layer by layer (identical layers once), nothing is decoded.
            const auto table = m_document.parameter_table();
            const auto first_of = [&](bool bottom) {
                for (const auto &row : table) if (row.is_bottom == bottom) return row;
                return table.empty() ? layerops::LayerParams{} : table.front();
            };
            stream = job.lithophane_stream(first_of(true), first_of(false));
            stream_params = true;
        }
        const auto generate = job.lithophane;
        param_list = true; param_masks = true;
        if (!stream) param_op = [generate](std::vector<layerops::Mask> &masks, std::vector<double> &z, std::vector<layerops::LayerParams> &table,
                              const sla::LayerProgress &progress) {
            const size_t old_count = masks.size();
            std::vector<layerops::Mask> layers; std::vector<double> heights; std::vector<layerops::LayerParams> params;
            generate(layers, heights, params, progress);
            masks = std::move(layers); z = std::move(heights); table = std::move(params);
            // New layers inherit the unmodelled fields (speeds, waits) of the nearest old layer.
            sla::LayerDocument::LayerListChange change;
            for (size_t i = 0; i < masks.size(); ++i) change.list.source_index.push_back(std::min(i, old_count - 1));
            return change;
        };
    }
    bool committed = false;
    std::exception_ptr failure;
    {
        TaskLock working(m_working);
        try {
            committed = run_background_layers(this, job.title, [&](const auto &progress) {
                if (job.document_op) return job.document_op(m_document, kFallbackLayerHeightMm, progress);
                if (stream && job.stream_pixels) return m_document.apply_pixel_list_stream(stream, kFallbackLayerHeightMm, progress);
                if (stream)
                    return stream_params ? m_document.apply_parameter_list_stream(stream, kFallbackLayerHeightMm, progress) :
                                           m_document.apply_layer_list_stream(stream, job.changes_resolution, kFallbackLayerHeightMm, progress);
                if (param_op)
                    return param_list ?
                        m_document.apply_parameter_list_operation(param_op, kFallbackLayerHeightMm, progress) :
                        m_document.apply_parameter_operation(param_op, param_masks, kFallbackLayerHeightMm, progress);
                return job.list_op ?
                    m_document.apply_layer_list_operation(job.list_op, job.changes_resolution, kFallbackLayerHeightMm, progress) :
                    job.kernel ? m_document.apply_mask_kernel(selected_plane(), job.kernel_z, job.kernel, progress, job.mask_op) :
                                 m_document.apply_mask_batch(selected_plane(), job.mask_op, progress);
            });
        } catch (...) { failure = std::current_exception(); }
    }
    m_preview_cancelled = false;
    try { refresh(); } catch (...) {}
    if (failure) std::rethrow_exception(failure);
    if (committed && m_apply_button) update_source_status();
    return committed ? RunOutcome::Committed : RunOutcome::Cancelled;
}

bool LayerEditorContent::run_export_job(const LayerExportJob &job)
{
    if (m_working) return false;
    finish_stroke(true);
    const auto z = operation_layer_z();
    auto table = m_document.parameter_table(); // empty when the document stores no parameters
    const auto plane = selected_plane();
    bool done = false;
    std::exception_ptr failure;
    {
        TaskLock working(m_working);
        try {
            done = run_background_layers(this, job.title, [&](const sla::LayerProgress &progress) {
                try {
                    // Layers are decoded one at a time on demand (fetch) or visited once (scan): no whole-document decode.
                    ExportData data;
                    data.z_mm = z; data.table = table;
                    const size_t count = m_document.layer_count();
                    data.layers.layer_count = count;
                    if (count) {
                        const auto first = m_document.image(0, plane);
                        data.layers.width = size_t(first.cols); data.layers.height = size_t(first.rows);
                    }
                    data.layers.z_mm = z;
                    data.layers.fetch = [this, plane](size_t layer) {
                        auto image = m_document.image(layer, plane);
                        auto mask = std::make_shared<layerops::Mask>(size_t(image.cols), size_t(image.rows));
                        mask->px = std::move(image.buf);
                        return std::shared_ptr<const layerops::Mask>(std::move(mask));
                    };
                    data.layers.scan = [this, plane, &progress](const std::function<void(size_t, const layerops::Mask &)> &visit) {
                        const bool done = m_document.scan_masks(plane, [&](size_t layer, const layerops::Mask &mask) { visit(layer, mask); }, progress);
                        if (!done) throw sla::LayerOperationCancelled{};
                    };
                    job.run(data, progress);
                    return true;
                } catch (const sla::LayerOperationCancelled &) { return false; }
            });
        } catch (...) { failure = std::current_exception(); }
    }
    if (failure) std::rethrow_exception(failure);
    return done;
}

LayerOpsHost LayerEditorContent::make_operations_host()
{
    LayerOpsHost host;
    host.layer_count = [this] { return m_document.layer_count(); };
    host.current_layer = [this] { return size_t(m_layer->GetValue() - 1); };
    host.selected_range = [this] {
        return m_paint_range->GetValue() ? std::make_pair(size_t(m_first->GetValue() - 1), size_t(m_last->GetValue() - 1)) :
            std::make_pair(size_t(m_layer->GetValue() - 1), size_t(m_layer->GetValue() - 1));
    };
    host.last_rectangle = [this] { return m_last_rectangle; };
    host.mask_size = [this] {
        const auto image = m_document.image(size_t(m_layer->GetValue() - 1), selected_plane());
        return std::make_pair(image.cols, image.rows);
    };
    host.layer_z_mm = [this] { return operation_layer_z(); };
    host.bottom_layer_count = [this] {
        if (m_document.fresh_job()) return m_document.fresh_job()->globals.bottom_layers;
        const auto job = m_document.job_parameters();
        return job ? job->bottom_layers : size_t(0);
    };
    host.parameter_refusal = [this] { return m_document.parameter_operation_refusal(); };
    host.parameter_table = [this] { return m_document.parameter_table(); };
    host.printer_context = [this] { return m_document.parameter_printer_context(kFallbackLayerHeightMm); };
    host.display_size_mm = [this]() -> std::optional<std::pair<double, double>> {
        const auto geometry = m_document.raster_geometry();
        if (!geometry || !(geometry->width_mm > 0) || !(geometry->height_mm > 0)) return std::nullopt;
        return std::make_pair(geometry->width_mm, geometry->height_mm);
    };
    host.run_export = [this](const LayerExportJob &job) { return run_export_job(job); };
    host.list_refusal = [this](bool resolution) { return operation_list_refusal(resolution); };
    host.read_current_layer = [this] { return read_layer_mask(size_t(m_layer->GetValue() - 1)); };
    host.show_preview = [this](const layerops::Mask *mask) {
        if (m_working) return;
        if (mask) {
            png::ImageGreyscale image;
            image.cols = mask->width; image.rows = mask->height; image.buf = mask->px;
            m_op_preview = std::move(image);
        } else m_op_preview.reset();
        try { refresh(); } catch (const std::exception &error) {
            m_op_preview.reset();
            wxMessageBox(wxString::FromUTF8(error.what()), "Layer editor", wxOK | wxICON_ERROR, this);
        }
    };
    host.run = [this](const LayerOpsJob &job) { return run_operation_job(job); };
    host.on_layer_list_changed = [this] { CallAfter([this] { perform([this] { open_operations_dialog(); }); }); };
    return host;
}

void LayerEditorContent::open_operations_dialog()
{
    finish_stroke(true);
    if (m_ops_dialog) { m_ops_dialog->Destroy(); m_ops_dialog = nullptr; }
    m_ops_dialog = create_layer_operations_dialog(this, make_operations_host());
    m_ops_dialog->CentreOnParent();
    m_ops_dialog->Show();
}

bool LayerEditorContent::save_layer_project_impl(bool save_as)
{
    if (is_model_result()) throw RuntimeError("Save model layers with the 3D project");
    finish_stroke(true);
    const boost::filesystem::path previous(m_layer_project_path);
    std::string path = m_layer_project_path;
    if (save_as || path.empty()) {
        wxFileDialog save(this, "Save layer project", wxString::FromUTF8(previous.parent_path().string()),
            previous.empty() ? "layers.sla-project" : wxString::FromUTF8(previous.filename().string()),
            "SLA layer project (*.sla-project)|*.sla-project", wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
        if (save.ShowModal() != wxID_OK) return false;
        path = boost::filesystem::path(into_u8(save.GetPath())).replace_extension(".sla-project").string();
        if (wxString::FromUTF8(path) != save.GetPath() && boost::filesystem::exists(path) &&
            wxMessageBox("Replace the existing layer project?\n" + wxString::FromUTF8(path),
                "Save layer project", wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION, this) != wxYES)
            return false;
    }
    auto state = m_print ? sla::capture_generated_project(*m_print, m_document) :
        sla::ProjectRasterState{m_document, {}, "", m_export_profile, sla::ProjectLayerOrigin::ImportedArchive, m_project_settings};
    state.settings = m_project_settings;
    TaskLock working(m_working);
    const bool saved = run_background_layers(this, "Save layer project", [&](const sla::LayerProgress &progress) {
        try { sla::save_layer_project(path, state, progress); return true; }
        catch (const sla::LayerOperationCancelled &) { return false; }
    });
    // Advance both the remembered path and the baseline only after publish.
    if (saved) {
        m_layer_project_path = path;
        m_project_revision = m_document.revision();
        m_context_baseline = m_export_profile;
        m_settings_baseline = m_project_settings;
        m_settings_require_project = false;
        m_document.mark_saved();
        if (auto *frame = wxGetApp().mainframe) frame->remember_layer_document(path);
    }
    return saved;
}

bool LayerEditorContent::save_project(bool save_as)
{
    bool saved = false;
    perform([&] { saved = save_layer_project_impl(save_as); });
    return saved;
}

void LayerEditorContent::apply_changes()
{
    if (m_working) throw RuntimeError("Wait for the layer operation to finish");
    finish_stroke(true);
    if (!m_document.dirty()) return;
    if (!source_current())
        throw RuntimeError("The sliced result changed. Load its current layers before applying edits.");
    if (!m_apply_source) throw RuntimeError("The layer editor has no model Apply target");
    m_apply_source(*m_session);
    if (auto *plater = wxGetApp().plater()) plater->on_sla_layer_document_changed();
    refresh();
}

bool LayerEditorContent::matches_project_snapshot(const sla::ExportGenerationGuard &document_source,
                              uint64_t revision) const
{
    // Use document identity plus its content revision, not current() on
    // the model source: metadata invalidation must not lose applied PNGs.
    // Undo/Redo can return to the exact applied revision; newer edits and
    // replacement sessions cannot reuse that receipt.
    return is_model_result() && !m_working && !m_stroke && !m_document.dirty() &&
        !m_settings_require_project && !context_changed() &&
        m_project_settings == m_settings_baseline && m_document.revision() == revision &&
        m_document.export_input_guard().same_source(document_source);
}

void LayerEditorContent::acknowledge_model_project_saved(uint64_t revision,
    const sla::ExportGenerationGuard *document_source)
{
    const bool matching_snapshot = document_source && matches_project_snapshot(*document_source, revision);
    if (!is_model_result() || (!source_current() && !matching_snapshot) || m_working || m_stroke ||
        m_document.revision() != revision) return;
    m_model_project_revision = revision;
    update_source_status();
}

bool LayerEditorContent::confirm_discard()
{
    if (m_working) return false;
    if (!unsaved_changes() && !(m_print && m_print->is_raster_job() && !m_generated_exported && !m_project_revision)) return true;
    if (m_session && !m_print->is_raster_job() && !source_current())
        return wxMessageBox("Discard the older sliced edits? They are separate from the current 3D project. Exported files will be kept.",
            "Layer editor", wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION, this) == wxYES;
    return wxMessageBox(m_project_revision ? "Close without saving changes to the layer project? Exported files will be kept." :
        m_print && m_print->is_raster_job() ? "Discard generated layers that have not been saved or exported?" :
        m_print ? "Close without saving these layer edits in the 3D project? Exported files will be kept." :
        "Discard unsaved layer edits or export context?", "Layer editor", wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION, this) == wxYES;
}
} // namespace layer_editor_detail
} // namespace Slic3r::GUI
