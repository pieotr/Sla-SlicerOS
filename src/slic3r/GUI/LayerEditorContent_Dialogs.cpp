#include "LayerEditorContent.hpp"

namespace Slic3r::GUI {
namespace layer_editor_detail {
void LayerEditorContent::apply_range_operation() {
    if (m_first->GetValue() > m_last->GetValue()) throw RuntimeError("First layer must not exceed last layer");
    TaskLock working(m_working);
    wxDialog progress(this, wxID_ANY, _L("Edit layer range"));
    auto *progress_sizer = new wxBoxSizer(wxVERTICAL);
    auto *status = new wxStaticText(&progress, wxID_ANY, _L("Preparing masks"));
    auto *gauge = new wxGauge(&progress, wxID_ANY, 1000, wxDefaultPosition, wxSize(360, -1));
    auto *abort = new wxButton(&progress, wxID_CANCEL, _L("Cancel"));
    progress_sizer->Add(status, 0, wxALL, 12);
    progress_sizer->Add(gauge, 0, wxEXPAND | wxLEFT | wxRIGHT, 12);
    progress_sizer->Add(abort, 0, wxALIGN_RIGHT | wxALL, 12);
    progress.SetSizerAndFit(progress_sizer); progress.CentreOnParent();
    const auto first = size_t(m_first->GetValue()-1), last = size_t(m_last->GetValue()-1);
    const auto operation = static_cast<sla::LayerDocument::MaskOperation>(m_operation->GetSelection());
    const auto value = uint8_t(m_value->GetValue());
    const auto radius = unsigned(m_radius->GetValue());
    const auto plane=selected_plane();
    std::atomic<size_t> completed{0};
    std::atomic<bool> cancel{false};
    // Only the worker accesses the document until get() completes. The
    // application-modal progress window disables other editing controls.
    // The callback exchanges plain values; wxWidgets stays on its UI thread.
    auto task = std::async(std::launch::async, [&] {
        return m_document.transform_layers(first, last, operation, value,
            [&](size_t done, size_t) {
                completed.store(done, std::memory_order_relaxed);
                return !cancel.load(std::memory_order_relaxed);
            }, radius,plane);
    });
    auto request_cancel = [&] {
        cancel.store(true, std::memory_order_relaxed);
        abort->Disable(); status->SetLabel(_L("Cancelling…"));
    };
    progress.Bind(wxEVT_BUTTON, [&](wxCommandEvent &) { request_cancel(); }, wxID_CANCEL);
    progress.Bind(wxEVT_CLOSE_WINDOW, [&](wxCloseEvent &event) {
        request_cancel(); if (event.CanVeto()) event.Veto();
    });
    // A normal modal event loop also services GTK accessibility and
    // native input while the worker runs. No synchronous Yield loop.
    wxTimer timer(&progress);
    progress.Bind(wxEVT_TIMER, [&](wxTimerEvent &) {
        if (task.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
            timer.Stop(); progress.EndModal(wxID_OK); return;
        }
        const size_t done = completed.load(std::memory_order_relaxed);
        gauge->SetValue(int(std::min(size_t(999), done * 1000 / (last-first+1))));
        if (!cancel.load(std::memory_order_relaxed))
            status->SetLabel(wxString::Format(_L("Layers: %zu / %zu"), done, last-first+1));
    });
    timer.Start(30); progress.ShowModal(); timer.Stop();
    task.get();
}

void LayerEditorContent::show_calibration_recipe() {
    const auto &recipe=*m_print->calibration_recipe();const auto &p=recipe.parameters;
    const wxString patterns[]={_L("XYZ blocks"),_L("Grayscale patches"),_L("Lift area"),"ExposureFinder"};
    wxString summary=_L("Original generation recipe. Current masks may contain later edits.\n\n");
    summary+=(patterns[int(p.pattern)]);
    summary+=wxString::Format(_L("\nPattern X: %.17g mm\nPattern Y: %.17g mm\nHeight: %.17g mm\n"),p.size_x,p.size_y,p.height);
    if(!(recipe.exposure_finder && recipe.exposure_finder->generation))
        summary+=wxString::Format(_L("Spacing / margin: %.17g mm\n"),p.margin);
    if (p.pattern == sla::CalibrationPattern::LiftArea)
        summary += wxString::Format(_L("\nLift size step: %u%% (0 = disabled)\nMinimum lift size: %u%%\nPercentages apply to both X and Y dimensions; decreasing layers are included in total height.\n"),
            p.lift_taper_step,p.lift_taper_min);
    if (recipe.exposure_finder) {
        const auto &table=*recipe.exposure_finder;
        summary+=wxString::Format(_L("\nTest table: %s\nRows: %u\nHeight (mm) | Bottom (s) | Normal (s) | Requested brightness\n"),
            table.table_origin==sla::ExposureFinderTableOrigin::Generated ? _L("Generated") : _L("Manual"),unsigned(table.rows.size()));
        for(size_t i=0;i<std::min(size_t(100),table.rows.size());++i) {
            const auto &row=table.rows[i];
            summary+=wxString::Format("%.3f | %.2f | %.2f | %u%s\n",row.layer_height,row.bottom_exposure,
                row.normal_exposure,unsigned(row.brightness),row.valid()?"":" (invalid)");
        }
        if(table.rows.size()>100)summary+=_L("Showing first 100 rows. The project retains the complete table.\n");
        if(table.schedule) {
            const auto &p=*table.schedule;
            const wxString modes[]={_L("Iterative"),_L("First table row"),_L("Middle table row"),_L("Last table row"),_L("Custom")};
            const wxString exclusions[]={_L("None"),_L("Bottom"),_L("Bottom and base")};
            summary+=wxString::Format(_L("\nExposure schedule\nBase height: %.9g mm\nFeature height: %.9g mm\nBottom layers per test: %u\nBase exposure mode: %s\nCustom base exposure: %.9g s (cure grid 0.01 s)\n"),
                p.base_height,p.features_height,p.bottom_layers,modes[int(p.base_mode)],p.custom_base_exposure);
            summary+=wxString::Format(_L("Multiple exposures: %s\nMultiple brightness: %s\nStored brightness: "),
                p.multiple_exposures?_L("Yes"):_L("No"),p.multiple_brightness?_L("Yes"):_L("No"));
            for(size_t i=0;i<p.brightness.size();++i) {
                if(i)summary+=",";
                summary+=wxString::Format("%u",unsigned(p.brightness[i]));
            }
            summary+=wxString::Format(_L("\nBrightness exclusion: %s\nRepeated-Z settings: %s\nTotal lift override: %.9g mm (%s)\nWait before override: %.9g s (%s)\n"),
                exclusions[int(p.brightness_exclusion)],p.repeated_settings?_L("Enabled"):_L("Disabled"),
                p.repeated_lift_height,p.repeated_lift_enabled?_L("Enabled"):_L("Disabled"),
                p.repeated_wait_before,p.repeated_wait_enabled?_L("Enabled"):_L("Disabled"));
        }
    }
    if(recipe.exposure_finder && recipe.exposure_finder->generation) {
        const auto &g=*recipe.exposure_finder->generation;const auto &p=g.pattern;const auto &l=g.layout;
        summary+=_L("\nPattern geometry\nHoles (px): ");
        for(auto v:p.holes)summary+=wxString::Format("%u ",v);
        if(p.holes.empty())summary+=_L("None");
        summary+=wxString::Format(_L("\nHole shape: %s\nBars (px): "),p.square_holes?_L("Square"):_L("Circle"));
        for(auto v:p.bars)summary+=wxString::Format("%u ",v);
        if(p.bars.empty())summary+=_L("None");
        summary+=_L("\nBullseyes diameter:thickness (px): ");
        for(const auto &ring:p.bullseyes)summary+=wxString::Format("%u:%u ",ring[0],ring[1]);
        if(p.bullseyes.empty())summary+=_L("None");
        summary+=wxString::Format(_L("\nFeature margin: %.9g mm\nBar length: %.9g mm\nBar spacing: %.9g mm\nBar splitter: %d px\nBar fence: %d px\nBar fence offset: %d px\n"),
            p.feature_margin_mm,p.bar_length_mm,p.bar_spacing_mm,p.bar_splitter,p.bar_fence,p.bar_fence_offset);
        summary+=wxString::Format(_L("Bullseye fence: %d px\nBullseye fence offset: %d px\nInvert bullseye quadrants: %s\nCounter triangles: %s\nTriangle fence: %s\nTriangle tip offset: %d px\nStaircase thickness: %u px\nAnti-aliasing: %s\n"),
            p.bullseye_fence,p.bullseye_fence_offset,p.invert_bullseye_quadrants?_L("Yes"):_L("No"),p.counter_triangles?_L("Yes"):_L("No"),p.triangle_fence?_L("Yes"):_L("No"),p.triangle_tip_offset,p.staircase_thickness,p.antialias?_L("Yes"):_L("No"));
        const wxString fonts[]={"Simplex","Plain","Duplex","Complex","Triplex","Complex small","Script simplex","Script complex"};
        summary+=_L("Text: ")+wxString::FromUTF8(p.text)+"\n";
        summary+=wxString::Format(_L("Font: %s%s\nText scale: %.9g (rounded to 0.01)\nText thickness: %d px\n"),
            fonts[p.font_face&15],(p.font_face&16)?_L(" italic"):"",p.text_scale,p.text_thickness);
        summary+=_L("\nLCD layout\nCell columns: ")+(l.cell_columns?wxString::Format("%u px",unsigned(l.cell_columns)):wxString(_L("Auto")));
        summary+=_L("\nCell rows: ")+(l.cell_rows?wxString::Format("%u px",unsigned(l.cell_rows)):wxString(_L("Auto")));
        summary+=wxString::Format(_L("\nMargin X: %.9g mm\nMargin Y: %.9g mm\nGap X: %.9g mm\nGap Y: %.9g mm\nCenter cells: %s\nExtra mirror X: %s\nExtra mirror Y: %s\nBottom erosion: %u px\nChamfer layers: %u\n"),
            l.margin_x_mm,l.margin_y_mm,l.gap_x_mm,l.gap_y_mm,l.center?_L("Yes"):_L("No"),l.mirror_x?_L("Yes"):_L("No"),l.mirror_y?_L("Yes"):_L("No"),g.bottom_erosion,g.chamfer_layers);
    }
    if (!recipe.requested.empty()) {
        summary+="\nRequested -> generated grayscale (0-255):\n";
        for(size_t i=0;i<recipe.requested.size();++i)
            summary+=wxString::Format("%u -> %u\n",unsigned(recipe.requested[i]),unsigned(recipe.effective[i]));
    }
    wxDialog details(this,wxID_ANY,_L("Calibration recipe"),wxDefaultPosition,wxSize(560,440),wxDEFAULT_DIALOG_STYLE|wxRESIZE_BORDER);
    auto *layout=new wxBoxSizer(wxVERTICAL);
    auto *text=new wxTextCtrl(&details,wxID_ANY,summary,wxDefaultPosition,wxDefaultSize,wxTE_MULTILINE|wxTE_READONLY);
    text->SetName("Original calibration recipe");layout->Add(text,1,wxEXPAND|wxALL,10);
    layout->Add(details.CreateSeparatedButtonSizer(wxOK),0,wxEXPAND|wxALL,10);
    details.SetSizer(layout);details.CentreOnParent();details.ShowModal();
}

void LayerEditorContent::capture_selected_settings() {
    const auto config=wxGetApp().preset_bundle->full_config();
    auto settings=sla::capture_project_settings(config);
    size_t columns=config.option("display_pixels_x")->getInt(), rows=config.option("display_pixels_y")->getInt();
    if (config.option("display_orientation")->getInt()==sladoPortrait) std::swap(columns,rows);
    const auto image=m_document.image(0);
    if (columns!=image.cols || rows!=image.rows)
        throw RuntimeError("Selected printer resolution differs from the imported layer raster");
    if (m_export_profile) {
        auto profile=*m_export_profile;
        profile.columns=columns;profile.rows=rows;
        profile.screen_width_mm=config.option("display_width")->getFloat();
        profile.screen_height_mm=config.option("display_height")->getFloat();
        if (config.option("display_orientation")->getInt()==sladoPortrait)
            std::swap(profile.screen_width_mm,profile.screen_height_mm);
        profile.density_g_ml=config.option("material_density")->getFloat();
        profile.cost_per_ml=config.option("bottle_cost")->getFloat()/config.option("bottle_volume")->getFloat();
        profile.max_print_height_mm=config.option("max_print_height")->getFloat();
        profile.validate(image.cols,image.rows);
        m_export_profile=profile;
        m_reopened_profile.reset();
        if (m_use_profile) m_use_profile->SetLabel(_L("Use selected printer and material for resin estimates"));
    }
    m_project_settings=std::move(settings);
    m_settings_require_project=!m_project_revision.has_value();
    if (m_update_profile_summary) m_update_profile_summary();
}

void LayerEditorContent::show_project_settings() {
    if (!m_project_settings) throw RuntimeError("Capture selected SLA settings before reviewing them");
    wxDialog details(this,wxID_ANY,_L("Project SLA settings"),wxDefaultPosition,wxSize(700,600),
        wxDEFAULT_DIALOG_STYLE|wxRESIZE_BORDER);
    auto *layout=new wxBoxSizer(wxVERTICAL);
    layout->Add(new wxStaticText(&details,wxID_ANY,
        _L("Saved project settings. Active presets are unchanged. Imported native layer parameters take precedence over their project defaults.")),0,wxEXPAND|wxALL,10);
    wxString text;
    auto role=[&](const wxString &label,const sla::ProjectPresetSettings &preset) {
        text+=label+": "+wxString::FromUTF8(preset.name)+"\n";
        for (const auto &[key,value]:preset.values)
            text+=wxString::FromUTF8(key)+" = "+wxString::FromUTF8(value)+"\n";
        if(preset.metadata) {
            text+=_L("Profile relationships (resolved values work without the referenced base):\n");
            for(const auto &[key,value]:*preset.metadata)
                text+=wxString::FromUTF8(key)+" = "+wxString::FromUTF8(value)+"\n";
        } else text+=_L("Profile relationships were not stored in this legacy project.\n");
        text+="\n";
    };
    role(_L("Printer"),m_project_settings->printer);role(_L("Material"),m_project_settings->material);role(_L("Process"),m_project_settings->process);
    auto *values=new wxTextCtrl(&details,wxID_ANY,text,wxDefaultPosition,wxDefaultSize,wxTE_MULTILINE|wxTE_READONLY);
    values->SetName("Resolved SLA project settings");layout->Add(values,1,wxEXPAND|wxALL,10);
    layout->Add(details.CreateButtonSizer(wxOK),0,wxEXPAND|wxALL,10);
    details.SetSizer(layout);details.CentreOnParent();details.ShowModal();
}

void LayerEditorContent::show_profile_summary() {
    if (!m_export_profile) m_profile_summary->SetLabel(_L("Resin estimates: original file values"));
    else {
        const auto &profile=*m_export_profile;
        m_profile_summary->SetLabel(wxString::Format(m_reopened_profile ?
            _L("Saved project estimates: %.4g x %.4g mm; %.4g g/ml; %.4g per ml; max Z %.4g mm. Active presets are unchanged.") :
            _L("Resin estimates: %.4g x %.4g mm; %.4g g/ml; %.4g per ml; max Z %.4g mm. Screen dimensions and layer heights are checked on save."),
            profile.screen_width_mm, profile.screen_height_mm, profile.density_g_ml, profile.cost_per_ml, profile.max_print_height_mm));
    }
    m_profile_summary->Wrap(side_width()); refresh();
}

void LayerEditorContent::on_use_profile_toggled() {
    if (m_working) { m_use_profile->SetValue(m_export_profile.has_value()); return; }
    m_export_profile.reset();
    m_profile_summary->SetLabel(_L("Resin estimates: original file values"));
    if (!m_use_profile->GetValue()) { show_profile_summary(); return; }
    try {
        if (m_reopened_profile) {
            m_export_profile=m_reopened_profile;
            show_profile_summary(); return;
        }
        const auto cfg = wxGetApp().preset_bundle->full_config();
        if (cfg.option("printer_technology")->getInt() != ptSLA)
            throw RuntimeError("Select an SLA printer and material before using their resin estimates");
        sla::LayerExportProfile profile;
        profile.columns = cfg.option("display_pixels_x")->getInt();
        profile.rows = cfg.option("display_pixels_y")->getInt();
        profile.screen_width_mm = cfg.option("display_width")->getFloat();
        profile.screen_height_mm = cfg.option("display_height")->getFloat();
        if (cfg.option("display_orientation")->getInt() == sladoPortrait) {
            std::swap(profile.columns, profile.rows);
            std::swap(profile.screen_width_mm, profile.screen_height_mm);
        }
        profile.max_print_height_mm = cfg.option("max_print_height")->getFloat();
        if (!std::isfinite(profile.max_print_height_mm) || profile.max_print_height_mm<=0)
            throw RuntimeError("Selected printer requires a positive finite maximum print height");
        profile.density_g_ml = cfg.option("material_density")->getFloat();
        const double bottle_volume = cfg.option("bottle_volume")->getFloat();
        if (!std::isfinite(bottle_volume) || bottle_volume <= 0)
            throw RuntimeError("Selected material requires a positive finite bottle volume");
        profile.cost_per_ml = cfg.option("bottle_cost")->getFloat()/bottle_volume;
        const auto mask = m_document.image(0);
        profile.validate(mask.cols, mask.rows);
        const auto settings=sla::capture_project_settings(cfg);
        m_export_profile = profile;
        m_project_settings=settings;
        show_profile_summary();
    } catch (const std::exception &error) {
        m_use_profile->SetValue(false);
        show_profile_summary();
        wxMessageBox(wxString::FromUTF8(error.what()), _L("Layer export profile"), wxOK|wxICON_ERROR, this);
    }
}

} // namespace layer_editor_detail
} // namespace Slic3r::GUI
