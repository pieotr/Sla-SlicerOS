#include "LayerEditorContent.hpp"

namespace Slic3r::GUI {
LayerEditorPanel *create_sliced_layer_editor(wxWindow *parent, SLAPrint &source,
    std::function<void(sla::SlicedResultSession &)> apply,
    std::function<bool()> source_available, std::function<void()> back)
{
    std::unique_ptr<sla::SlicedResultSession> session;
    const bool captured = run_background_layers(parent, "Open sliced layers", [&](const sla::LayerProgress &progress) {
        try { session = sla::SlicedResultSession::capture(source, progress); return true; }
        catch (const sla::LayerOperationCancelled &) { return false; }
        catch (const CanceledException &) { return false; }
    });
    if (!captured) return nullptr;
    return new LayerEditorContent(parent, {}, {}, {}, {}, std::move(session),
        std::move(back), std::move(apply), std::move(source_available));
}
std::string choose_layer_file(wxWindow *parent)
{
    wxString archive_filter = "Layer archives, projects and images|*.sla-project;*.SLA-PROJECT;*.sl1;*.sl1s;*.uvj;*.cws;*.zip;*.cxdlp;*.cxdlpv4;*.ctb;*.cbddlp;*.photon;*.CTB;*.CBDDLP;*.PHOTON;*.CXDLP;*.CXDLPV4;*.png;*.PNG;*.jpg;*.jpeg;*.jp2;*.tif;*.tiff;*.bmp;*.pbm;*.pgm;*.sr;*.ras;*.JPG;*.JPEG;*.JP2;*.TIF;*.TIFF;*.BMP;*.PBM;*.PGM;*.SR;*.RAS|All files|*";
    wxString anycubic_masks;
    for (const auto &entry : registered_sla_archives()) if (!anycubic_available_versions(entry.id).empty()) {
        const auto extension = wxString::FromUTF8(entry.ext);
        anycubic_masks += ";*." + extension + ";*." + extension.Upper();
    }
    archive_filter.Replace("|All files|*", anycubic_masks + "|All files|*");
    wxFileDialog open(parent, "Open layer archive", "", "", archive_filter,
                      wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (open.ShowModal() != wxID_OK) return {};
    return into_u8(open.GetPath());
}
namespace {
struct OpenedLayerFile {
    sla::LayerDocument document;
    std::optional<sla::LayerExportProfile> profile;
    std::optional<sla::ProjectSLASettings> settings;
    std::string project_path;
    std::unique_ptr<sla::SlicedResultSession> session;
};
// The temporary generated job is destroyed after capture; only its owned
// context and masks travel to the workspace. Imported documents own their reader.
std::optional<OpenedLayerFile> load_layer_file(wxWindow *parent, const std::string &path)
{
    OpenedLayerFile loaded;
    const bool is_project = boost::algorithm::to_lower_copy(
        boost::filesystem::path(path).extension().string()) == ".sla-project";
    const bool opened = run_background_layers(parent, "Open layer archive", [&](const sla::LayerProgress &progress) {
        try {
            if (is_project) {
                auto project = sla::read_project_layers(path, progress);
                if (!project || project->origin == sla::ProjectLayerOrigin::ModelSlice)
                    throw RuntimeError("Open model projects in the 3D workspace.");
                if (project->origin == sla::ProjectLayerOrigin::GeneratedRaster) {
                    auto job = sla::restore_generated_project(*project);
                    loaded.session = sla::SlicedResultSession::capture(*job, progress);
                } else loaded.document = std::move(project->document);
                loaded.profile = project->export_profile;
                loaded.settings = project->settings;
                loaded.project_path = path;
            } else loaded.document = sla::LayerDocument::load_archive(path, progress);
            return true;
        } catch (const sla::LayerOperationCancelled &) { return false; }
          catch (const CanceledException &) { return false; }
    });
    if (!opened) return std::nullopt;
    return loaded;
}
}
LayerEditorPanel *create_layer_file_editor(wxWindow *parent, const std::string &path,
    std::function<void()> back)
{
    auto loaded = load_layer_file(parent, path);
    if (!loaded) return nullptr;
    return new LayerEditorContent(parent, std::move(loaded->document),
        loaded->profile, std::move(loaded->project_path), loaded->settings,
        std::move(loaded->session), std::move(back));
}
}
