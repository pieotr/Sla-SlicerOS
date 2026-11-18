#include "LayerEditorContent.hpp"
#include "libslic3r/SLA/LayerConversionPlan.hpp"
#include "GUI_Utils.hpp"

namespace Slic3r::GUI {
namespace layer_editor_detail {
void LayerEditorContent::update_format_note() {
    const int selection = m_format->GetSelection();
    const char *id = selection == wxNOT_FOUND ? "" : m_formats.at(size_t(selection))->id;
    const bool images_only = std::strcmp(id, "genericzip") == 0 || std::strcmp(id, "vda") == 0;
    const auto &versions = anycubic_available_versions(id);
    uint16_t version = 0;
    wxString version_error;
    if (!versions.empty()) {
        // Read the default from the same registered factory as
        // export, rather than maintaining a second version map.
        try {
            const auto writer = get_archive_entry(id)->wrfactoryfn(m_print->printer_config());
            const auto *anycubic = dynamic_cast<const AnycubicSLAArchive*>(writer.get());
            if (!anycubic) throw std::logic_error("Invalid Anycubic archive registration");
            version = anycubic_resolve_version(id, m_print->printer_config().sla_anycubic_version.value, anycubic->default_version());
        }
        catch (const std::exception &error) { version_error = wxString::FromUTF8(error.what()); }
    }
    const bool machine_table = version >= 516;
    const auto *legacy = machine_table ? nullptr : anycubic_legacy_geometry(id);
    wxString note;
    if (!version_error.empty()) note = version_error;
    else if (images_only) note = "This ZIP format saves masks and geometry. Set exposure and motion on the receiving printer.";
    else if(std::strcmp(id,"klipper")==0 || std::strcmp(id,"klipper_rgb")==0)
        note="Klipper ZIP requires matching MSLA firmware macros for image display, timed UV exposure and layer changes. Configure two-stage lift/retract, PWM and waits in Material Settings > LCD exposure and motion. A positive first lift and retract stage and PWM 1–255 are required by this encoder. RGB uses full-width PNG24 masks.";
    else if(std::strcmp(id,"cwsrgb")==0) note = "RGB.CWS packs three horizontal mask pixels into one PNG pixel. The LCD raster width after orientation must be divisible by three. Full LCD dimensions and all grayscale values are preserved.";
    else if(std::strcmp(id,"cbddlp")==0 || std::strcmp(id,"photon")==0)
        note=wxString::Format("CBDDLP/Photon v2 uses %d AA exposure planes from the printer preset. Counts other than powers of two decode maximum white below 255 and may change pixels on re-encoding. Configure AA in Printer Settings > General > Output.", m_print->printer_config().sla_cbddlp_aa.value);
    else if(std::strcmp(id,"cbddlp_v1")==0 || std::strcmp(id,"photon_v1")==0)
        note="CBDDLP/Photon v1 uses one binary plane: only input value 255 exposes. It cannot store custom lift/retract settings; the decoder assumes normal lift 0.05 mm, bottom lift 6 mm and speeds 100 mm/min. Verify firmware motion separately.";
    else if((std::strcmp(id,"cxdlp")==0 || std::strcmp(id,"cxdlpv2")==0)) note = "CXDLP v2/v3 requires a CL/CT printer model, fixed layer height, integer speeds in mm/s, normal exposure in 0.1 s, integer bottom exposure and a shared wait of at least 1 s. Machine Z is not stored.";
    else if(std::strcmp(id,"cxdlpv4")==0) note = "CXDLPv4 stores 7-bit grayscale: values 0 and 1 are off; other values are quantized. Volume and contour metadata describe the encoded masks.";
    else if(std::strcmp(id,"cxdlpv1")==0) note = "CXDLPv1 requires fixed layer height, integer exposure/motion values and a shared wait of at least 1 second. This format does not store machine Z.";
    else if (version == 518)
        note="Anycubic 518 supports two-stage lift and separate bottom/normal retract speeds. Configure them in Material Settings > LCD exposure and motion. Wait before exposure is shared; custom PWM, after-waits and second retract height are unavailable.";
    else if (machine_table) note = "Anycubic 516/517 supports two-stage lift and separate bottom/normal retract speeds. Configure them in Material Settings > LCD exposure and motion. Screen dimensions and machine Z come from the printer preset. Wait before exposure is shared.";
    else if (legacy) note = wxString::Format(
        "This legacy format requires an oriented screen of %.4g x %.4g mm and Z %.4g mm. Custom dimensions require a format with a machine table.",
        legacy->width, legacy->height, legacy->machine_z);
    else if (!versions.empty() && version < 516)
        note = "PWC 1/515 saves masks, pixel pitch, exposure and single-stage motion, but has no machine table or printer identity. UVtools reports screen dimensions and machine Z as zero. Select PWC 516 or newer in Printer Settings > General > Output to preserve physical dimensions.";
    m_format_note->SetLabel(note);
    m_format_note->Wrap(650); m_format_note->Show(!note.empty()); Layout();
}

void LayerEditorContent::run_export_action() {
    const auto *image_format = !m_print ? sla::find_layer_image_format(m_document.archive_format()) : nullptr;
    const bool png = image_format != nullptr;
    const auto *entry = png ? nullptr : m_print ? m_formats.at(size_t(m_format->GetSelection())) : get_archive_entry(m_document.archive_format().c_str());
    if (!png && !entry) throw RuntimeError("Unknown layer container");
    if (m_print && std::string(entry->id)=="cxdlpv4") {
        const auto error=sla::cxdlpv4_printer_identifier_error(m_print->printer_config().printer_model.value,
            bool(m_document.fresh_job()));
        if (!error.empty()) throw RuntimeError(error+
            "\n\nSet Printer > General > Output > Printer model (firmware), then save the printer preset and try again.");
    }
    auto filename = [&](const std::string &path) {
        return png ? boost::filesystem::path(path).replace_extension(std::string(".")+image_format->id).string() : sla_archive_filename(path, entry->ext);
    };
    const char *project = m_print && m_print->is_raster_job() ? "generated" : "edited";
    const auto name = filename(project);
    const auto filter = png ? wxString::FromUTF8(image_format->description) + " (*." + image_format->id + ")|*." + image_format->id : wxString::FromUTF8(entry->desc) + " (*." + entry->ext + ")|*." + entry->ext;
    wxFileDialog save(this, "Save edited layers", "", wxString::FromUTF8(name), filter,
                      wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
    if (save.ShowModal() == wxID_OK) {
        if (image_format && image_format->lossy && !image_format->binary &&
            wxMessageBox("Saving JPEG may change exposure pixels. Continue?", "Save edited image",
                wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION, this) != wxYES) return;
        const auto path = filename(save.GetPath().ToUTF8().data());
        // The native dialog checks the typed path. A corrected suffix
        // can target a different existing file; confirm that one too.
        if (wxString::FromUTF8(path) != save.GetPath() && boost::filesystem::exists(path) &&
            wxMessageBox("Replace the existing file?\n" + wxString::FromUTF8(path),
                "Save edited layers", wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION, this) != wxYES)
            return;
        TaskLock working(m_working);
        if (m_print) export_through_print(entry, path, project);
        else export_native_archive(path);
    }
}

void LayerEditorContent::export_through_print(const ArchiveEntry *entry, const std::string &path, const char *project) {
    bool times_projected=false;
    if((std::string(entry->id)=="SL1" || std::string(entry->id)=="SL1S") && m_document.fresh_job()) {
        std::optional<sla::SL1ExposureTransitionPreview> transition_plan;
        const bool analyzed = run_background_layers(this, "Check exposure transition",
            [&](const sla::LayerProgress &progress) {
                try {
                    transition_plan = sla::sl1_exposure_transition_preview(m_document, progress);
                    return true;
                } catch (const sla::LayerOperationCancelled &) { return false; }
            });
        if (!analyzed) return;
        times_projected=transition_plan->projected_transition_exposures!=0;
        if(times_projected && wxMessageBox(wxString::Format(
            "SL1 uses a global exposure transition. Exporting will project %llu transition exposures; the largest change is %.6g seconds.\n\nContinue? The original project parameters will be kept.",
            static_cast<unsigned long long>(transition_plan->projected_transition_exposures),transition_plan->maximum_exposure_change),
            "Confirm exposure transition",wxYES_NO|wxNO_DEFAULT|wxICON_QUESTION,this)!=wxYES)return;
    }
    if(std::string(entry->id)=="cws" && m_document.fresh_job()) {
        std::optional<sla::CWSFreshExportPlan> time_plan;
        run_background_write(this,"Check export times",[&] {time_plan=sla::cws_fresh_export_plan(m_document);});
        times_projected=time_plan->projected_time_fields!=0;
        if(time_plan->projected_time_fields && wxMessageBox(wxString::Format(
            "CWS uses whole milliseconds. Exporting will round %llu time values; the largest change is %.6g seconds.\n\nContinue? The original project parameters will be kept.",
            static_cast<unsigned long long>(time_plan->projected_time_fields),time_plan->maximum_time_change),
            "Confirm export times",wxYES_NO|wxNO_DEFAULT|wxICON_QUESTION,this)!=wxYES)return;
    }
    std::optional<sla::LayerDocument> projected_copy;
    const auto &anycubic_versions=anycubic_available_versions(entry->id);
    const bool cxdlpv4=std::string(entry->id)=="cxdlpv4";
    const auto precision=cxdlpv4?sla::LayerMaskPrecision::NativeGray7:sla::LayerMaskPrecision::NativeGray4;
    if(m_document.fresh_job() && (cxdlpv4 || std::any_of(anycubic_versions.begin(),anycubic_versions.end(),
        [](uint16_t version){return version>=516 && version<=518;}))) {
        sla::LayerPaletteLoss loss;
        const bool analyzed=run_background_layers(this,"Check export palette",[&](const sla::LayerProgress &progress) {
            try {loss=sla::layer_palette_loss(m_document,precision,progress);return true;}
            catch(const sla::LayerOperationCancelled &){return false;}
        });
        if(!analyzed)return;
        if(loss.changed_pixels) {
            const auto family=cxdlpv4?wxString("CXDLPv4"):std::string(entry->id)=="pwc"?wxString("PWC"):wxString("Anycubic");
            const auto message=family+wxString::Format(" uses %u grayscale levels. Exporting will change %llu pixels; the largest change is %u of 255.\n\nExport a converted copy? Your original layers and Undo history will be kept.",
                cxdlpv4?128u:16u,static_cast<unsigned long long>(loss.changed_pixels),loss.maximum_change);
            if(wxMessageBox(message,"Convert export palette",wxYES_NO|wxNO_DEFAULT|wxICON_QUESTION,this)!=wxYES)return;
            const bool converted=run_background_layers(this,"Convert export palette",[&](const sla::LayerProgress &progress) {
                try {projected_copy=sla::project_layer_palette_copy(m_document,precision,progress);return true;}
                catch(const sla::LayerOperationCancelled &){return false;}
            });
            if(!converted)return;
        }
    }
    sla::RepresentationApproval approval;
    if(sla::has_representation_rules(entry->id)) {
        auto representation=sla::plan_export_representation(entry->id,m_print->full_print_config(),&m_document);
        if(!representation.blockers.empty()) throw ExportError(representation.strict_message("CXDLP"));
        if(!representation.adjustments.empty()) {
            if(!confirm_export_adjustments(this,"CXDLP",representation.adjustments)) return;
            approval=std::move(representation.adjustments);
        }
    }
    // The session owns a const context. TaskLock + this modal
    // keep the edited document stable until the worker joins.
    // Every attempt has a fresh cancellation flag, while the
    // context's commit mutex protects the existing destination.
    BackgroundRequestOwner<sla::ExportGenerationGuard> request_owner;
    const bool saved=run_background_layers(this, "Export layers", [&](const sla::LayerProgress &progress) {
        try {
            const auto request=std::make_shared<const sla::ExportGenerationGuard>(
                m_print->export_generation_guard().with_cancellation());
            if (!request_owner.bind(request)) return false;
            const auto &document = projected_copy ? *projected_copy : m_document;
            const auto check = [&] { sla::check_layer_progress(progress); };
            const char *snapshot_flag = std::getenv("SLA_EXPERIMENT_SL1_EDITOR_SNAPSHOT");
            // The private SL1 copy permits verified PNG reuse even
            // before Apply. Other writers keep their existing path
            // until their capture cost has been measured separately.
            const bool sl1 = std::string(entry->id) == "SL1" || std::string(entry->id) == "SL1S";
            if (sl1 && (!snapshot_flag || std::string(snapshot_flag) == "1"))
                m_print->export_layer_document_snapshot(document, entry->id,
                    path, {}, project, check, request.get(), approval.empty() ? nullptr : &approval);
            else
                m_print->export_layer_document(document, entry->id,
                    path, {}, project, check, request.get(), approval.empty() ? nullptr : &approval);
            return true; // Publication won; a later Cancel cannot undo it.
        } catch(const sla::LayerOperationCancelled &) { return false; }
        catch(const CanceledException &) { return false; }
    }, [&] { request_owner.cancel(); });
    if (!saved) return;
    if (m_print->is_raster_job() && !projected_copy && !times_projected) {
        m_generated_exported = true; m_document.mark_saved();
        if (!m_project_revision) m_settings_baseline=m_project_settings;
    }
    if (!m_printer_name.empty()) wxGetApp().app_config->set_sla_export_format(m_printer_name, entry->id);
}

void LayerEditorContent::export_native_archive(const std::string &path) {
    BackgroundRequestOwner<sla::NativeArchiveExportRequest> request_owner;
    const bool saved=run_background_layers(this,"Save layer archive",[&](const sla::LayerProgress &progress) {
        try {
            const auto request=std::make_shared<const sla::NativeArchiveExportRequest>(m_document,path,m_export_profile,progress);
            if (!request_owner.bind(request)) return false;
            request->write(progress);
            return true;
        } catch(const sla::LayerOperationCancelled &) { return false; }
        catch(const CanceledException &) { return false; }
    }, [&] { request_owner.cancel(); });
    const auto request=request_owner.request();
    const bool acknowledged = saved && request && request->acknowledge_saved(m_document);
    if (acknowledged && !m_project_revision) {
        m_context_baseline=m_export_profile;
        // Native containers cannot carry every process/preset option.
        if (!m_settings_require_project) m_settings_baseline=m_project_settings;
    }
}

void LayerEditorContent::convert_archive() {
    if(!m_project_settings)
        throw RuntimeError("Capture selected SLA settings before converting to another printer format");
    const auto configuration=sla::restore_project_settings(*m_project_settings);
    wxDialog conversion(this,wxID_ANY,"Convert layer archive",wxDefaultPosition,wxSize(620,260),
        wxDEFAULT_DIALOG_STYLE|wxRESIZE_BORDER);
    auto *layout=new wxBoxSizer(wxVERTICAL);
    auto *description=new wxStaticText(&conversion,wxID_ANY,
        "Destination printer: "+wxString::FromUTF8(m_project_settings->printer.name)+
        "\nPer-layer parameters must be representable by the destination printer format.");
    description->Wrap(570);layout->Add(description,0,wxEXPAND|wxALL,10);
    auto *choice=create_readonly_combo(&conversion,"Destination format");
    std::vector<const ArchiveEntry *> formats;
    const auto source_fields=m_document.parameter_capabilities();
    for(const auto &entry:registered_sla_archives()) {
        const auto schema=sla::parameter_conversion_schema(entry.id);
        if(schema.empty() || !archive_export_unavailable_reason(entry.id).empty())continue;
        if(!std::all_of(source_fields.begin(),source_fields.end(),[&](const auto &source) {
            return std::any_of(schema.begin(),schema.end(),[&](const auto &target){return source.field==target.field;});
        }))continue;
        formats.push_back(&entry);
        choice->Append(wxString::FromUTF8(entry.desc)+" (."+wxString::FromUTF8(entry.ext)+")");
    }
    if(formats.empty())throw RuntimeError("No implemented destination can preserve this source parameter table");
    choice->SetSelection(0);
    const auto preferred=wxGetApp().app_config->sla_export_format(m_project_settings->printer.name);
    for(size_t i=0;i<formats.size();++i)if(preferred==formats[i]->id)choice->SetSelection(int(i));
    choice->SetMinSize(wxSize(420,-1));layout->Add(choice,0,wxEXPAND|wxALL,10);
    auto *actions=conversion.CreateButtonSizer(wxOK|wxCANCEL);
    auto *confirm=dynamic_cast<wxButton *>(conversion.FindWindow(wxID_OK));
    if(confirm)confirm->SetLabel("Convert");
    layout->Add(actions,0,wxEXPAND|wxALL,10);conversion.SetSizer(layout);
    if(conversion.ShowModal()!=wxID_OK)return;
    const auto *entry=formats.at(size_t(choice->GetSelection()));
    const auto default_name=sla_archive_filename("converted",entry->ext);
    wxFileDialog save(this,"Save converted layers","",wxString::FromUTF8(default_name),
        wxString::FromUTF8(entry->desc)+" (*."+wxString::FromUTF8(entry->ext)+")|*."+wxString::FromUTF8(entry->ext),
        wxFD_SAVE|wxFD_OVERWRITE_PROMPT);
    if(save.ShowModal()!=wxID_OK)return;
    const auto typed=into_u8(save.GetPath());const auto path=sla_archive_filename(typed,entry->ext);
    if(path!=typed && boost::filesystem::exists(path) &&
        wxMessageBox("Replace the existing file?\n"+wxString::FromUTF8(path),"Convert archive",
            wxYES_NO|wxNO_DEFAULT|wxICON_QUESTION,this)!=wxYES)return;
    sla::RepresentationApproval approval;
    if(sla::has_representation_rules(entry->id)) {
        auto representation=sla::plan_export_representation(entry->id,configuration,&m_document);
        if(!representation.blockers.empty()) throw ExportError(representation.strict_message("CXDLP"));
        if(!representation.adjustments.empty()) {
            if(!confirm_export_adjustments(this,"CXDLP",representation.adjustments)) return;
            approval=std::move(representation.adjustments);
        }
    }
    TaskLock working(m_working);
    BackgroundRequestOwner<sla::NativeArchiveConversionRequest> request_owner;
    const bool converted=run_background_layers(this,"Convert layers",[&](const sla::LayerProgress &progress) {
        try {
            const auto request=std::make_shared<const sla::NativeArchiveConversionRequest>(m_document,configuration,entry->id,path,progress,approval);
            if (!request_owner.bind(request)) return false;
            request->write(progress);
            return true;
        } catch(const sla::LayerOperationCancelled &) { return false; }
        catch(const CanceledException &) { return false; }
    }, [&] { request_owner.cancel(); });
    if (!converted) return;
    if(!m_project_settings->printer.name.empty())
        wxGetApp().app_config->set_sla_export_format(m_project_settings->printer.name,entry->id);
    // Conversion is an output of this source, not a save/apply of
    // its project or native container. Preserve revision/history.
}

void LayerEditorContent::export_mask_images(bool range) {
    finish_stroke(true);
    const auto first = size_t((range ? m_first : m_layer)->GetValue()-1);
    const auto last = size_t((range ? m_last : m_layer)->GetValue()-1);
    if (first > last) throw RuntimeError("First layer must not exceed last layer");
    // The image format is chosen in an app-owned drop-down (GTK's file-type filter combo closes on button release under
    // GTK3 + XWayland/KWin); the file chooser then shows that single filter. A typed extension of another format wins.
    std::vector<ExportFormatOption> image_options;
    for (const auto &format : sla::layer_image_formats())
        image_options.push_back({ wxString::FromUTF8(format.description) + " (*." + format.id + ")", format.id, format.id });
    int image_index = choose_export_format(this, "Choose the mask image format", image_options, 0);
    if (image_index < 0) return;
    wxFileDialog save(this, first == last ? "Export selected mask" : "Export range to a new mask folder",
        "", wxString("mask.") + image_options[size_t(image_index)].ext, image_options[size_t(image_index)].label + "|*." + image_options[size_t(image_index)].ext,
        wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
    if (save.ShowModal() != wxID_OK) return;
    image_index = export_format_for_typed_name(image_options, image_index, into_u8(save.GetFilename()));
    const auto &format = sla::layer_image_formats().at(size_t(image_index));
    auto output = boost::filesystem::path(into_u8(save.GetPath()));
    output.replace_extension(std::string(".")+format.id);
    if (format.lossy && wxMessageBox(format.binary ?
        "PBM keeps zero black and converts every nonzero value to white, like UVtools. Continue?" :
        "JPEG is lossy and may change exposure pixels. Continue?",
        "Export masks", wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION, this) != wxYES) return;
    if (first != last) {
        output = output.parent_path() / (output.stem().string()+"_layers");
        if (boost::filesystem::exists(output)) throw RuntimeError("Choose a new folder name; the mask export folder already exists");
        if (wxMessageBox(wxString::Format("Export layers %zu–%zu as separate images into a new folder?\n", first+1,last+1) +
            wxString::FromUTF8(output.string()), "Export mask range",
            wxYES_NO | wxICON_QUESTION, this) != wxYES) return;
    } else if (wxString::FromUTF8(output.string()) != save.GetPath() && boost::filesystem::exists(output) &&
        wxMessageBox("Replace the existing mask?\n"+wxString::FromUTF8(output.string()),
            "Export masks", wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION, this) != wxYES) return;
    const auto plane=selected_plane();
    TaskLock working(m_working);
    run_background_layers(this, "Export masks", [&](const auto &progress) {
        return m_document.export_images(output.string(), format.id, first,last,progress,plane);
    });
}

} // namespace layer_editor_detail
} // namespace Slic3r::GUI
