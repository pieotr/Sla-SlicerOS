#include "SlaRasterToolsDialog.hpp"
#include "RasterGeneratorTask.hpp"
#include "GUI_App.hpp"
#include "Widgets/ComboBox.hpp"
#include "GUI_Utils.hpp"
#include "Plater.hpp"
#include "Tab.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/SLA/CalibrationRaster.hpp"
#include "libslic3r/SLA/PCBExposure.hpp"
#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>
#include <cmath>
#include <functional>
#include <memory>
#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/dialog.h>
#include <wx/filedlg.h>
#include <wx/image.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/statbmp.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/tokenzr.h>

namespace Slic3r::GUI {

namespace {
using RasterGenerator =
    std::function<std::unique_ptr<SLAPrint>(const std::function<void()> &)>;
std::unique_ptr<SLAPrint>
run_raster_generator(wxWindow *parent, const RasterGenerator &generate) {
  auto result = run_raster_task<std::unique_ptr<SLAPrint>>(parent, "Generating layers",
      [generate](const sla::LayerProgress &progress) {
        const std::function<void()> check = [&progress] { sla::check_layer_progress(progress); };
        return generate(check);
      });
  return result ? std::move(*result) : std::unique_ptr<SLAPrint>{};
}
std::string read_pcb_file(const wxString &path) {
  const boost::filesystem::path file{path.ToUTF8().data()};
  const auto size = boost::filesystem::file_size(file);
  if (size > 64 * 1024 * 1024)
    throw RuntimeError("PCB input exceeds the 64 MiB limit");
  boost::nowide::ifstream input(file.string(), std::ios::binary);
  std::string text(size, '\0');
  input.read(text.data(), std::streamsize(size));
  if (!input)
    throw RuntimeError("Could not read PCB input");
  return text;
}
wxSpinCtrlDouble *generator_number(wxDialog &dialog, wxFlexGridSizer &grid,
                                   const char *label, double value,
                                   double minimum, double maximum,
                                   double increment) {
  grid.Add(new wxStaticText(&dialog, wxID_ANY, label), 0,
           wxALIGN_CENTER_VERTICAL);
  auto *control = new wxSpinCtrlDouble(&dialog, wxID_ANY);
  control->SetRange(minimum, maximum);
  control->SetIncrement(increment);
  control->SetDigits(3);
  control->SetValue(value);
  control->SetName(label);
  grid.Add(control, 1, wxEXPAND);
  return control;
}

// --- PCB: persisted options -------------------------------------------------
// Options survive between sessions in the application config (section
// sla_pcb_exposure), so a repeated PCB run starts from the last used values.
constexpr const char *pcb_section = "sla_pcb_exposure";
std::string pcb_setting(const char *key, const std::string &fallback) {
  const auto *config = wxGetApp().app_config;
  return config && config->has(pcb_section, key) ? config->get(pcb_section, key) : fallback;
}
void store_pcb_setting(const char *key, const std::string &value) {
  if (auto *config = wxGetApp().app_config) config->set(pcb_section, key, value);
}
double pcb_number(const char *key, double fallback) {
  try { return std::stod(pcb_setting(key, "")); } catch (const std::exception &) { return fallback; }
}
bool pcb_flag(const char *key, bool fallback) {
  const std::string value = pcb_setting(key, "");
  return value.empty() ? fallback : value == "1";
}

// Parsed copper is reused between the preview and the generation when the
// files did not change, so the user does not wait for the parser twice.
using PcbBoards = std::shared_ptr<const std::vector<sla::PCBArtwork>>;
std::string pcb_cache_key(const wxString &gerbers, const wxString &drill, double tolerance_mm) {
  std::string key = std::to_string(tolerance_mm) + "|" + drill.ToStdString() + "|";
  const auto stamp = [&key](const wxString &path) {
    boost::system::error_code error;
    const boost::filesystem::path file{path.ToUTF8().data()};
    const auto time = boost::filesystem::last_write_time(file, error);
    key += path.ToStdString() + "@" + std::to_string(error ? 0 : long(time)) + ";";
  };
  const auto paths = wxStringTokenize(gerbers, "\n", wxTOKEN_STRTOK);
  for (const auto &path : paths) stamp(wxString(path).Trim(true).Trim(false));
  if (!drill.empty()) stamp(drill);
  return key;
}
PcbBoards load_pcb_boards(const wxString &gerbers, const wxString &drill, double tolerance_mm,
                          const std::function<void()> &check) {
  const auto paths = wxStringTokenize(gerbers, "\n", wxTOKEN_STRTOK);
  if (paths.empty() || paths.size() > 1000)
    throw RuntimeError("Choose 1–1000 copper files, one path per line");
  const auto drill_text = drill.empty() ? std::string{} : read_pcb_file(drill);
  auto boards = std::make_shared<std::vector<sla::PCBArtwork>>();
  size_t copper_vertices = 0;
  for (auto path : paths) {
    check();
    path.Trim(true).Trim(false);
    if (path.empty()) continue;
    auto board = sla::parse_gerber(read_pcb_file(path), tolerance_mm, check);
    if (!drill_text.empty()) sla::subtract_excellon(board, drill_text, tolerance_mm, check);
    for (const auto &polygon : board.copper) {
      copper_vertices += polygon.contour.points.size();
      for (const auto &hole : polygon.holes) copper_vertices += hole.points.size();
    }
    if (copper_vertices > 12000000)
      throw RuntimeError("Combined copper exceeds the 12 million vertex budget");
    boards->push_back(std::move(board));
  }
  return boards;
}
// A quarter of the smaller pixel keeps arcs smoother than the raster resolves.
double pcb_curve_tolerance_mm(const SLAPrinterConfig &printer) {
  return std::min(printer.display_width.value / printer.display_pixels_x.value,
                  printer.display_height.value / printer.display_pixels_y.value) * .25;
}
wxBitmap pcb_preview_bitmap(const sla::PCBPreview &preview) {
  auto *rgb = static_cast<unsigned char *>(std::malloc(preview.pixels.size() * 3));
  for (size_t i = 0; i < preview.pixels.size(); ++i)
    rgb[i * 3] = rgb[i * 3 + 1] = rgb[i * 3 + 2] = preview.pixels[i];
  wxImage image(int(preview.width_px), int(preview.height_px), rgb, false);  // takes ownership
  return wxBitmap(image);
}
} // namespace

void show_pcb_exposure(wxWindow *parent) {
  wxDialog options(parent, wxID_ANY, "Expose PCB", wxDefaultPosition,
                   wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
  auto *root = new wxBoxSizer(wxVERTICAL);
  {
    const auto printer_config = wxGetApp().preset_bundle->full_config();
    SLAPrinterConfig printer;
    printer.apply(printer_config, true);
    // Artwork is scaled with the real pitch of each axis, so 10 mm of copper is
    // 10 mm on the LCD even when the pixels are not square.
    const wxString pitch = wxString::Format(
        "Printer pixel pitch: %.2f x %.2f um (X x Y). Artwork keeps its physical 1:1 size.",
        1000. * printer.display_width.value / std::max(1, printer.display_pixels_x.value),
        1000. * printer.display_height.value / std::max(1, printer.display_pixels_y.value));
    root->Add(new wxStaticText(
                  &options, wxID_ANY,
                  "1. Choose copper Gerbers and an optional Excellon drill file.\n"
                  "2. Pick an exposure recipe and check the preview.\n"
                  "3. Generate: the layers open in the Sliced workspace, ready to export in the "
                  "current printer's native format.\n" + pitch),
              0, wxALL, 12);
  }
  auto *body = new wxBoxSizer(wxHORIZONTAL);
  auto *left = new wxBoxSizer(wxVERTICAL);
  auto *grid = new wxFlexGridSizer(2, 8, 12);
  grid->AddGrowableCol(1);
  auto file_row = [&](const char *label, const char *filter, bool multiple = false) {
    grid->Add(new wxStaticText(&options, wxID_ANY, label), 0,
              wxALIGN_CENTER_VERTICAL);
    auto *row = new wxBoxSizer(wxHORIZONTAL);
    auto *path = new wxTextCtrl(&options, wxID_ANY, "", wxDefaultPosition,
                                multiple ? wxSize(300,72) : wxDefaultSize,
                                multiple ? wxTE_MULTILINE : 0);
    path->SetName(label);
    auto *browse = new wxButton(&options, wxID_ANY, "Browse");
    row->Add(path, 1, wxRIGHT, 6);
    row->Add(browse);
    grid->Add(row, 1, wxEXPAND);
    browse->Bind(wxEVT_BUTTON, [&, path, label, filter, multiple](wxCommandEvent &) {
      wxFileDialog open(&options, label, "", "", filter,
                        wxFD_OPEN | wxFD_FILE_MUST_EXIST | (multiple ? wxFD_MULTIPLE : 0));
      if (open.ShowModal() == wxID_OK) {
        wxArrayString paths;
        open.GetPaths(paths);
        wxString selected;
        for (const auto &file : paths) {
          if (!selected.empty()) selected += "\n";
          selected += file;
        }
        path->SetValue(selected);
      }
    });
    return path;
  };
  auto *gerber =
      file_row("Copper Gerber",
               "Gerber files|*.gbr;*.ger;*.gko;*.gtl;*.gto;*.gts;*.gbl;*.gbo;*.gbs;*.gml;*.GBR|All files|*", true);
  gerber->SetToolTip("Select multiple files with Browse, or enter one path per line. All files share the same physical coordinates.");
  auto *drill = file_row("Drill file (optional)",
                         "Excellon files|*.drl;*.xln;*.DRL|All files|*");
  // Paths are stored with '|' because config values are single-line.
  {
    wxString saved_paths = wxString::FromUTF8(pcb_setting("gerber_paths", ""));
    saved_paths.Replace("|", "\n");
    gerber->ChangeValue(saved_paths);
    drill->ChangeValue(wxString::FromUTF8(pcb_setting("drill_path", "")));
  }
  // Exposure recipe: a preset fills exposure and polarity; both stay editable.
  grid->Add(new wxStaticText(&options, wxID_ANY, "Exposure recipe"), 0, wxALIGN_CENTER_VERTICAL);
  auto *recipe = create_readonly_combo(&options);
  recipe->SetName("Exposure recipe");
  recipe->Append("Custom");
  for (const auto &preset : sla::pcb_exposure_presets()) recipe->Append(wxString::FromUTF8(preset.name));
  recipe->SetSelection(std::clamp(int(pcb_number("recipe", 0.)), 0, int(sla::pcb_exposure_presets().size())));
  grid->Add(recipe, 1, wxEXPAND);
  auto *exposure =
      generator_number(options, *grid, "Exposure (s)", pcb_number("exposure", 30.), .001, 86400., 1.);
  auto *margin =
      generator_number(options, *grid, "LCD margin (mm)", pcb_number("margin", 1.), 0., 1000., .1);
  auto *offset_x =
      generator_number(options, *grid, "X offset (mm)", pcb_number("offset_x", 0.), -1000., 1000., .1);
  auto *offset_y =
      generator_number(options, *grid, "Y offset (mm)", pcb_number("offset_y", 0.), -1000., 1000., .1);
  auto *scale =
      generator_number(options, *grid, "Size scale (%)", pcb_number("scale_percent", 100.), 10., 1000., .1);
  scale->SetToolTip("100 = physical 1:1. Change only to compensate measured resist shrinkage or optics error.");
  left->Add(grid, 0, wxEXPAND | wxALL, 12);
  auto *recipe_help = new wxStaticText(&options, wxID_ANY, "");
  left->Add(recipe_help, 0, wxLEFT | wxRIGHT | wxBOTTOM, 12);
  auto add_check = [&](const char *label, bool value, const char *tip = nullptr) {
    auto *check = new wxCheckBox(&options, wxID_ANY, label);
    check->SetValue(value);
    if (tip) check->SetToolTip(tip);
    left->Add(check, 0, wxLEFT | wxBOTTOM, 12);
    return check;
  };
  auto *merge = add_check("Merge Gerber files into one exposure", pcb_flag("merge", true),
                          "Unchecked: one exposure layer per file, in list order, with common alignment.");
  auto *antialiasing = add_check("Anti-aliasing (gray edge pixels)", pcb_flag("antialiasing", true),
                                 "Unchecked gives hard black/white edges; use it for films that react to any gray.");
  auto *mirror_x = add_check("Mirror artwork in X", pcb_flag("mirror_x", false));
  auto *mirror_y = add_check("Mirror artwork in Y", pcb_flag("mirror_y", false));
  auto *invert = add_check("Invert exposure polarity", pcb_flag("invert", false));
  body->Add(left, 1, wxEXPAND);

  // Preview: downscaled composition of the real mask, rendered on a worker.
  auto *right = new wxBoxSizer(wxVERTICAL);
  constexpr int preview_size = 360;
  wxBitmap placeholder(preview_size, preview_size);
  {
    wxMemoryDC dc(placeholder);
    dc.SetBackground(*wxBLACK_BRUSH);
    dc.Clear();
  }
  auto *preview_image = new wxStaticBitmap(&options, wxID_ANY, placeholder);
  preview_image->SetName("PCB preview");
  auto *preview_status = new wxStaticText(&options, wxID_ANY, "No preview yet.");
  auto *preview_layer = new wxSpinCtrl(&options, wxID_ANY, "1", wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 1, 1000, 1);
  preview_layer->SetToolTip("File shown when files are not merged.");
  preview_layer->SetName("Preview file number");
  Slic3r::GUI::set_accessible_name(preview_layer, "Preview file number", "File shown when files are not merged.");
  auto *preview_button = new wxButton(&options, wxID_ANY, "Update preview");
  right->Add(new wxStaticText(&options, wxID_ANY, "Preview of the mask (white = exposed)"), 0, wxBOTTOM, 6);
  right->Add(preview_image, 0, wxBOTTOM, 6);
  right->Add(preview_status, 0, wxBOTTOM, 6);
  auto *layer_row = new wxBoxSizer(wxHORIZONTAL);
  layer_row->Add(new wxStaticText(&options, wxID_ANY, "Preview file"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
  layer_row->Add(preview_layer, 0, wxRIGHT, 12);
  layer_row->Add(preview_button, 0);
  right->Add(layer_row, 0);
  body->Add(right, 0, wxALL, 12);
  root->Add(body, 1, wxEXPAND);

  auto *buttons = options.CreateSeparatedButtonSizer(wxOK | wxCANCEL);
  options.FindWindow(wxID_OK)->SetLabel("Expose PCB (generate layers)");
  root->Add(buttons, 0, wxEXPAND | wxALL, 12);
  options.SetSizerAndFit(root);
  options.SetMinSize(wxSize(900, options.GetSize().y));
  options.CentreOnParent();

  PcbBoards cached_boards;
  std::string cached_key;
  auto update_recipe_text = [&] {
    const int index = recipe->GetSelection();
    recipe_help->SetLabel(index <= 0 ? wxString("Custom values. Run a short exposure step test on your film first.")
        : wxString::FromUTF8(sla::pcb_exposure_presets()[size_t(index - 1)].guidance) +
          "\nThese are starting points, not guarantees: expose a test strip and adjust.");
    recipe_help->Wrap(420);
    options.Layout();
  };
  auto mark_stale = [&] { preview_status->SetLabel("Preview is out of date. Press Update preview."); };
  recipe->Bind(wxEVT_COMBOBOX, [&](wxCommandEvent &) {
    const int index = recipe->GetSelection();
    if (index > 0) {
      const auto &preset = sla::pcb_exposure_presets()[size_t(index - 1)];
      exposure->SetValue(preset.exposure_seconds);
      invert->SetValue(preset.invert);
    }
    update_recipe_text();
    mark_stale();
  });
  update_recipe_text();
  for (auto *control : {margin, offset_x, offset_y, scale})
    control->Bind(wxEVT_SPINCTRLDOUBLE, [&](wxSpinDoubleEvent &) { mark_stale(); });
  for (auto *check : {merge, antialiasing, mirror_x, mirror_y, invert})
    check->Bind(wxEVT_CHECKBOX, [&](wxCommandEvent &) { mark_stale(); });
  gerber->Bind(wxEVT_TEXT, [&](wxCommandEvent &event) { mark_stale(); event.Skip(); });
  drill->Bind(wxEVT_TEXT, [&](wxCommandEvent &event) { mark_stale(); event.Skip(); });

  const auto read_placement = [&] {
    sla::PCBPlacement placement;
    placement.margin = margin->GetValue();
    placement.offset_x = offset_x->GetValue();
    placement.offset_y = offset_y->GetValue();
    placement.scale = scale->GetValue() / 100.;
    placement.mirror_x = mirror_x->GetValue();
    placement.mirror_y = mirror_y->GetValue();
    placement.antialiasing = antialiasing->GetValue();
    return placement;
  };
  const auto save_options = [&] {
    const auto flag = [](bool value) { return std::string(value ? "1" : "0"); };
    wxString paths = gerber->GetValue();
    paths.Replace("\n", "|");
    store_pcb_setting("gerber_paths", paths.ToStdString());
    store_pcb_setting("drill_path", drill->GetValue().ToStdString());
    store_pcb_setting("recipe", std::to_string(recipe->GetSelection()));
    store_pcb_setting("exposure", std::to_string(exposure->GetValue()));
    store_pcb_setting("margin", std::to_string(margin->GetValue()));
    store_pcb_setting("offset_x", std::to_string(offset_x->GetValue()));
    store_pcb_setting("offset_y", std::to_string(offset_y->GetValue()));
    store_pcb_setting("scale_percent", std::to_string(scale->GetValue()));
    store_pcb_setting("merge", flag(merge->GetValue()));
    store_pcb_setting("antialiasing", flag(antialiasing->GetValue()));
    store_pcb_setting("mirror_x", flag(mirror_x->GetValue()));
    store_pcb_setting("mirror_y", flag(mirror_y->GetValue()));
    store_pcb_setting("invert", flag(invert->GetValue()));
  };

  preview_button->Bind(wxEVT_BUTTON, [&](wxCommandEvent &) {
    try {
      if (gerber->GetValue().empty()) throw RuntimeError("Choose a copper Gerber file");
      const auto configuration = wxGetApp().preset_bundle->full_config();
      SLAPrinterConfig printer;
      printer.apply(configuration, true);
      const wxString gerber_paths = gerber->GetValue(), drill_path = drill->GetValue();
      const double tolerance = pcb_curve_tolerance_mm(printer);
      const std::string key = pcb_cache_key(gerber_paths, drill_path, tolerance);
      const PcbBoards known = key == cached_key ? cached_boards : PcbBoards{};
      const auto placement = read_placement();
      const bool merge_files = merge->GetValue(), invert_mask = invert->GetValue();
      const size_t layer = size_t(std::max(1, preview_layer->GetValue())) - 1;
      struct PreviewResult { PcbBoards boards; sla::PCBPreview preview; };
      auto result = run_raster_task<PreviewResult>(&options, "Rendering PCB preview",
          [=](const sla::LayerProgress &progress) {
            const std::function<void()> check = [&progress] { sla::check_layer_progress(progress); };
            PreviewResult out;
            out.boards = known ? known : load_pcb_boards(gerber_paths, drill_path, tolerance, check);
            out.preview = sla::render_pcb_preview(*out.boards, printer, merge_files, placement, invert_mask,
                                                  preview_size, layer, check);
            return out;
          });
      if (!result) return;  // cancelled by the user
      cached_boards = result->boards;
      cached_key = key;
      preview_image->SetBitmap(pcb_preview_bitmap(result->preview));
      Slic3r::GUI::set_accessible_name(preview_image, "PCB preview",
          wxString::Format("PCB exposure preview, file %d of %d", int(layer) + 1, int(result->preview.layer_count)),
          Slic3r::GUI::AccessibleRole::Image);
      preview_status->SetLabel(wxString::Format("%u x %u px preview of %u mask layer(s).",
          unsigned(result->preview.width_px), unsigned(result->preview.height_px), unsigned(result->preview.layer_count)));
      preview_layer->SetRange(1, int(result->preview.layer_count));
      preview_layer->Enable(!merge_files);
      save_options();
      options.Layout();
    } catch (const std::exception &error) {
      preview_status->SetLabel("Preview failed.");
      wxMessageBox(wxString::FromUTF8(error.what()), "PCB exposure", wxOK | wxICON_ERROR, &options);
    }
  });

  while (options.ShowModal() == wxID_OK) {
    try {
      if (gerber->GetValue().empty())
        throw RuntimeError("Choose a copper Gerber file");
      const auto placement = read_placement();
      const auto gerber_path = gerber->GetValue(),
                 drill_path = drill->GetValue();
      const bool invert_mask = invert->GetValue(), merge_files = merge->GetValue();
      const double exposure_seconds = exposure->GetValue();
      const auto configuration = wxGetApp().preset_bundle->full_config();
      SLAPrinterConfig printer;
      printer.apply(configuration, true);
      const double tolerance = pcb_curve_tolerance_mm(printer);
      const std::string key = pcb_cache_key(gerber_path, drill_path, tolerance);
      const PcbBoards known = key == cached_key ? cached_boards : PcbBoards{};
      save_options();
      auto job =
          run_raster_generator(parent, [=](const std::function<void()> &check) {
            const PcbBoards boards = known ? known : load_pcb_boards(gerber_path, drill_path, tolerance, check);
            return sla::create_pcb_exposure_job(configuration, *boards, merge_files, placement,
                                                exposure_seconds, invert_mask, check);
          });
      if (!job)
        continue;
      auto *plater = wxGetApp().plater();
      if (!plater) throw RuntimeError("The Sliced workspace is unavailable");
      if (!plater->open_generated_layers(*job))
        continue; // Preserve the filled form when replacement is declined.
      return;
    } catch (const std::exception &error) {
      wxMessageBox(wxString::FromUTF8(error.what()), "PCB exposure",
                   wxOK | wxICON_ERROR, &options);
    }
  }
}
namespace {
// --- Apply result to profile -------------------------------------------------
// Settings a calibration can improve. They are edited through the open
// Material/Printer tab (Tab::load_config), exactly like typing in the field:
// the preset turns dirty, so the user can still discard it with the revert arrow.
struct ProfileTarget { const char *key, *label, *unit; int digits; };
constexpr ProfileTarget profile_targets[] = {
    {"exposure_time", "Normal exposure time (material)", "s", 2},
    {"initial_exposure_time", "Bottom exposure time (material)", "s", 2},
    {"sla_lift_height", "Lift height (material)", "mm", 2},
    {"sla_bottom_lift_height", "Bottom lift height (material)", "mm", 2},
    {"elefant_foot_compensation", "Elephant foot compensation (printer)", "mm", 3},
};
Tab *tab_with_option(const char *key) {
  for (const auto type : {Preset::TYPE_SLA_MATERIAL, Preset::TYPE_PRINTER}) {
    Tab *tab = wxGetApp().get_tab(type);
    if (tab && tab->m_config && tab->m_config->has(key)) return tab;
  }
  return nullptr;
}

void show_apply_result(wxWindow *parent) {
  wxDialog dialog(parent, wxID_ANY, "Apply calibration result to profile", wxDefaultPosition, wxDefaultSize,
                  wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
  auto *root = new wxBoxSizer(wxVERTICAL);
  auto *intro = new wxStaticText(&dialog, wxID_ANY,
      "Enter the best cell or value from your printed test. The matching field of the edited preset is "
      "updated after you confirm the old and new value. The preset stays modified (not saved): use the revert "
      "arrow in Material or Printer Settings to discard the change.");
  intro->Wrap(520);
  root->Add(intro, 0, wxALL, 12);
  auto *grid = new wxFlexGridSizer(2, 8, 12);
  grid->AddGrowableCol(1);
  grid->Add(new wxStaticText(&dialog, wxID_ANY, "Result of"), 0, wxALIGN_CENTER_VERTICAL);
  auto *source = create_readonly_combo(&dialog);
  for (const char *label : {"Exposure finder (best exposure time)", "Grayscale patches (best level)",
                            "Elephant foot (best erode cell)", "Lift height (lowest lift that peels)"})
    source->Append(label);
  source->SetSelection(0);
  grid->Add(source, 1, wxEXPAND);
  const auto config = wxGetApp().preset_bundle->full_config();
  const auto number = [&](wxStaticText *&label, double value, double minimum, double maximum, double step, int digits) {
    label = new wxStaticText(&dialog, wxID_ANY, "");
    grid->Add(label, 0, wxALIGN_CENTER_VERTICAL);
    auto *control = new wxSpinCtrlDouble(&dialog, wxID_ANY);
    control->SetRange(minimum, maximum); control->SetIncrement(step); control->SetDigits(digits); control->SetValue(value);
    grid->Add(control, 1, wxEXPAND);
    return control;
  };
  wxStaticText *label_a = nullptr, *label_b = nullptr, *label_value = nullptr;
  auto *input_a = number(label_a, config.opt_float("exposure_time"), .001, 86400., .1, 3);
  auto *input_b = number(label_b, config.opt_float("exposure_time"), .001, 86400., .1, 3);
  auto *value = number(label_value, config.opt_float("exposure_time"), 0., 86400., .01, 3);
  grid->Add(new wxStaticText(&dialog, wxID_ANY, "Write to"), 0, wxALIGN_CENTER_VERTICAL);
  auto *target = create_readonly_combo(&dialog);
  for (const auto &entry : profile_targets) target->Append(entry.label);
  target->SetSelection(0);
  grid->Add(target, 1, wxEXPAND);
  root->Add(grid, 0, wxEXPAND | wxLEFT | wxRIGHT, 12);
  auto *current = new wxStaticText(&dialog, wxID_ANY, "");
  root->Add(current, 0, wxALL, 12);
  auto *buttons = dialog.CreateSeparatedButtonSizer(wxOK | wxCANCEL);
  dialog.FindWindow(wxID_OK)->SetLabel("Review and apply");
  root->Add(buttons, 0, wxEXPAND | wxALL, 12);
  dialog.SetSizerAndFit(root);
  dialog.CentreOnParent();

  const double pitch_x = config.opt_float("display_width") / std::max(1, config.opt_int("display_pixels_x"));
  const double pitch_y = config.opt_float("display_height") / std::max(1, config.opt_int("display_pixels_y"));
  // Recomputes the proposed value from the inputs; it stays editable afterwards.
  const auto propose = [&] {
    try {
      switch (source->GetSelection()) {
      case 0: value->SetValue(input_a->GetValue()); break;
      case 1: value->SetValue(sla::grayscale_equivalent_exposure_s(input_b->GetValue(), unsigned(std::lround(input_a->GetValue())))); break;
      case 2: value->SetValue(sla::elephant_foot_compensation_mm(unsigned(std::lround(input_a->GetValue())), pitch_x, pitch_y)); break;
      default: value->SetValue(input_a->GetValue() + input_b->GetValue()); break;
      }
    } catch (const std::exception &) {
      // Input outside the conversion's range (e.g. gray level 0): keep the last proposal.
    }
  };
  const auto show_current = [&] {
    const auto &entry = profile_targets[std::max(0, target->GetSelection())];
    const Tab *tab = tab_with_option(entry.key);
    current->SetLabel(tab ? wxString::Format("Edited preset \"%s\": %s is now %.*f %s.",
                                             wxString::FromUTF8(tab->m_presets->get_edited_preset().name), entry.key,
                                             entry.digits, tab->m_config->opt_float(entry.key), entry.unit)
                          : wxString("This setting is not available in the current presets."));
    current->Wrap(520);
    dialog.Layout(); dialog.Fit();
  };
  const auto configure_source = [&] {
    static const struct { const char *a, *b; double a_value, b_value; int target; bool show_b; } sources[] = {
        {"Best exposure time (s)", "", 2.5, 0., 0, false},
        {"Best grayscale level (1-255)", "Exposure used for the test (s)", 128., 2.5, 0, true},
        {"Best cell: erode iterations (E:)", "", 2., 0., 4, false},
        {"Lowest lift height that peels (mm)", "Safety margin (mm)", 6., 1.5, 2, true},
    };
    const auto &entry = sources[std::max(0, source->GetSelection())];
    label_a->SetLabel(entry.a); label_b->SetLabel(entry.b);
    label_b->Show(entry.show_b); input_b->Show(entry.show_b);
    label_value->SetLabel("Proposed value (editable)");
    input_a->SetRange(source->GetSelection() == 1 ? 1. : 0., source->GetSelection() == 1 ? 255. : 86400.);
    input_a->SetDigits(source->GetSelection() == 1 || source->GetSelection() == 2 ? 0 : 3);
    input_a->SetValue(entry.a_value);
    input_b->SetValue(source->GetSelection() == 1 ? config.opt_float("exposure_time") : entry.b_value);
    target->SetSelection(entry.target);
    propose();
    show_current();
  };
  source->Bind(wxEVT_COMBOBOX, [&](wxCommandEvent &) { configure_source(); });
  target->Bind(wxEVT_COMBOBOX, [&](wxCommandEvent &) { show_current(); });
  for (auto *control : {input_a, input_b}) {
    control->Bind(wxEVT_SPINCTRLDOUBLE, [&](wxSpinDoubleEvent &) { propose(); });
    control->Bind(wxEVT_TEXT, [&](wxCommandEvent &event) { propose(); event.Skip(); });
  }
  configure_source();

  while (dialog.ShowModal() == wxID_OK) {
    const auto &entry = profile_targets[std::max(0, target->GetSelection())];
    Tab *tab = tab_with_option(entry.key);
    if (!tab) {
      wxMessageBox("This setting is not available in the current presets.", "Apply result", wxOK | wxICON_ERROR, &dialog);
      continue;
    }
    const double old_value = tab->m_config->opt_float(entry.key);
    const double scale = std::pow(10., entry.digits);
    const double new_value = std::round(value->GetValue() * scale) / scale;
    // Stay inside the printer's exposure limits instead of creating an invalid preset.
    const char *limit_key = std::string(entry.key) == "exposure_time" ? "max_exposure_time"
                          : std::string(entry.key) == "initial_exposure_time" ? "max_initial_exposure_time" : nullptr;
    if (limit_key && config.has(limit_key) && config.opt_float(limit_key) > 0. && new_value > config.opt_float(limit_key)) {
      wxMessageBox(wxString::Format("%.2f s is above the printer limit of %.2f s (%s). Raise the limit first or pick another value.",
                                    new_value, config.opt_float(limit_key), limit_key),
                   "Apply result", wxOK | wxICON_WARNING, &dialog);
      continue;
    }
    if (std::abs(new_value - old_value) < .5 / scale) {
      wxMessageBox("The new value equals the current one; nothing to change.", "Apply result", wxOK | wxICON_INFORMATION, &dialog);
      continue;
    }
    const wxString preset_name = wxString::FromUTF8(tab->m_presets->get_edited_preset().name);
    const wxString question = wxString::Format(
        "Change in preset \"%s\":\n\n%s\n%.*f %s  ->  %.*f %s\n\nThe preset becomes modified but is not saved. "
        "Discard it with the revert arrow in the settings tab.\n\nApply?",
        preset_name, wxString(entry.label), entry.digits, old_value, entry.unit, entry.digits, new_value, entry.unit);
    if (wxMessageBox(question, "Confirm profile change", wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION, &dialog) != wxYES)
      continue;
    DynamicPrintConfig change;
    change.set_key_value(entry.key, new ConfigOptionFloat(new_value));
    tab->load_config(change);
    return;
  }
}
} // namespace

void show_calibration_generator(wxWindow *parent) {
  wxDialog options(parent, wxID_ANY, "Printer calibration", wxDefaultPosition,
                   wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
  auto *root = new wxBoxSizer(wxVERTICAL);
  root->Add(
      new wxStaticText(
          &options, wxID_ANY,
          "Generate native calibration layers for the selected SLA printer.\n"
          "Exposure, layer height and movement come from the active "
          "material/process presets.\n"
          "XYZ: nine solid blocks. Grayscale: ordered patches. Lift: one "
          "rectangular area, optionally decreasing in size at the end.\n"
          "XYZ accuracy, Tolerance and Lift height are ports of the UVtools tests with their own "
          "motion/exposure fields. After printing, use \"Apply result to profile\"."),
      0, wxALL, 12);
  auto *grid = new wxFlexGridSizer(2, 8, 12);
  grid->AddGrowableCol(1);
  grid->Add(new wxStaticText(&options, wxID_ANY, "Pattern"), 0,
            wxALIGN_CENTER_VERTICAL);
  auto *pattern = create_readonly_combo(&options);
  pattern->SetName("Calibration pattern");
  for (const char *label : {"XYZ blocks", "Grayscale patches", "Lift area", "Elephant foot", "Blooming effect",
                            "Stress tower", "XYZ accuracy (hollow, labelled)", "Tolerance", "Lift height",
                            "External tests (reference)"})
    pattern->Append(label);
  pattern->SetSelection(0);
  grid->Add(pattern, 1, wxEXPAND);
  auto *sx =
      generator_number(options, *grid, "Pattern X (mm)", 10., .001, 1000., 1.);
  auto *sy =
      generator_number(options, *grid, "Pattern Y (mm)", 10., .001, 1000., 1.);
  auto *height = generator_number(options, *grid, "Test height (mm)", 1., .001,
                                  1000., .05);
  auto *margin = generator_number(options, *grid, "Spacing / margin (mm)", 2.,
                                  0., 1000., .1);
  auto *steps =
      generator_number(options, *grid, "Grayscale steps", 16., 2., 256., 1.);
  auto *first =
      generator_number(options, *grid, "First grayscale", 0., 0., 255., 1.);
  auto *last =
      generator_number(options, *grid, "Last grayscale", 255., 0., 255., 1.);
  auto *taper_step = generator_number(options, *grid, "Lift size step (%)", 0., 0., 99., 1.);
  auto *taper_min = generator_number(options, *grid, "Minimum lift size (%)", 10., 1., 99., 1.);
  taper_step->SetDigits(0); taper_min->SetDigits(0);
  taper_step->SetToolTip("0 keeps all layers at full size. Each final layer reduces both X and Y by this percentage of the original dimensions.");
  taper_min->SetToolTip("Smallest allowed percentage of the original X and Y dimensions. Total test height includes the decreasing layers.");
  // Pixel-domain UVtools calibrations (selection 3-5) have their own parameter
  // grids; the classic grid above is hidden while one of them is selected.
  const auto add_number = [&](wxFlexGridSizer &target, const char *label, double value, double minimum,
                              double maximum, double increment, int digits, const char *tip) {
    auto *control = generator_number(options, target, label, value, minimum, maximum, increment);
    control->SetDigits(digits);
    if (tip) control->SetToolTip(tip);
    return control;
  };
  const auto add_check = [&](wxFlexGridSizer &target, const char *label, bool value) {
    target.Add(new wxStaticText(&options, wxID_ANY, label), 0, wxALIGN_CENTER_VERTICAL);
    auto *control = new wxCheckBox(&options, wxID_ANY, "");
    control->SetValue(value);
    target.Add(control, 1, wxEXPAND);
    return control;
  };
  const auto make_grid = [&] {
    auto *sizer = new wxFlexGridSizer(2, 8, 12);
    sizer->AddGrowableCol(1);
    return sizer;
  };
  auto *ef_grid = make_grid(), *bloom_grid = make_grid(), *tower_grid = make_grid();
  const sla::ElephantFootParameters ef_defaults;
  auto *ef_bottom = add_number(*ef_grid, "Bottom layers", 0., 0., 10000., 1., 0, "0 = 1.0 mm of layers (UVtools default).");
  auto *ef_normal = add_number(*ef_grid, "Normal layers", 0., 0., 10000., 1., 0, "0 = 3.5 mm of layers (UVtools default).");
  auto *ef_scale = add_number(*ef_grid, "Part scale", ef_defaults.part_scale, .1, 50., .1, 2, "1 = 250 px wide part.");
  auto *ef_margin = add_number(*ef_grid, "Part margin (px)", ef_defaults.margin_px, 0., 10000., 1., 0, nullptr);
  auto *ef_original = add_check(*ef_grid, "Output original (ORI) part", ef_defaults.output_original);
  auto *ef_extrude = add_check(*ef_grid, "Extrude text", ef_defaults.extrude_text);
  auto *ef_text_h = add_number(*ef_grid, "Text height (mm)", ef_defaults.text_height_mm, 0., 1000., .1, 2, "Raised label height above the part.");
  auto *ef_aa = add_check(*ef_grid, "Anti-aliasing", ef_defaults.antialiasing);
  auto *ef_erode = add_check(*ef_grid, "Erode bottom layers", ef_defaults.erode);
  auto *ef_e_start = add_number(*ef_grid, "Erode start (iterations)", ef_defaults.erode_start, 0., 255., 1., 0, nullptr);
  auto *ef_e_end = add_number(*ef_grid, "Erode end (iterations)", ef_defaults.erode_end, 0., 255., 1., 0, nullptr);
  auto *ef_e_step = add_number(*ef_grid, "Erode step", ef_defaults.erode_step, 1., 255., 1., 0, nullptr);
  auto *ef_dim = add_check(*ef_grid, "Dim bottom wall", ef_defaults.dimming);
  auto *ef_d_wall = add_number(*ef_grid, "Wall thickness (px)", ef_defaults.dimming_wall_px, 0., 255., 1., 0, nullptr);
  auto *ef_d_start = add_number(*ef_grid, "Wall brightness start (0-255)", ef_defaults.dimming_start, 0., 255., 1., 0, nullptr);
  auto *ef_d_end = add_number(*ef_grid, "Wall brightness end (0-255)", ef_defaults.dimming_end, 0., 255., 1., 0, nullptr);
  auto *ef_d_step = add_number(*ef_grid, "Wall brightness step", ef_defaults.dimming_step, 1., 255., 1., 0, nullptr);
  const sla::BloomingParameters bloom_defaults;
  auto *bl_bottom = add_number(*bloom_grid, "Bottom layers", bloom_defaults.bottom_layers, 1., 10000., 1., 0, nullptr);
  auto *bl_lr = add_number(*bloom_grid, "Left/right margin (px)", bloom_defaults.left_right_margin_px, 0., 32768., 1., 0, nullptr);
  auto *bl_tb = add_number(*bloom_grid, "Top/bottom margin (px)", bloom_defaults.top_bottom_margin_px, 0., 32768., 1., 0, nullptr);
  auto *bl_start = add_number(*bloom_grid, "Wait before cure start (s)", bloom_defaults.wait_start_s, 0., 600., .5, 2, nullptr);
  auto *bl_step = add_number(*bloom_grid, "Wait increment (s)", bloom_defaults.wait_increment_s, .05, 600., .1, 2, nullptr);
  auto *bl_count = add_number(*bloom_grid, "Object count", bloom_defaults.object_count, 1., 255., 1., 0, "The test makes count + 1 squares.");
  auto *bl_diameter = add_number(*bloom_grid, "Object size (px)", bloom_defaults.object_diameter_px, 20., 32768., 1., 0, nullptr);
  auto *bl_margin = add_number(*bloom_grid, "Object margin (px)", bloom_defaults.object_margin_px, 0., 32768., 1., 0, nullptr);
  auto *bl_height = add_number(*bloom_grid, "Object height (mm)", bloom_defaults.object_height_mm, 0., 1000., .5, 2, nullptr);
  const sla::StressTowerParameters tower_defaults;
  auto *st_diameter = add_number(*tower_grid, "Base diameter (mm)", tower_defaults.base_diameter_mm, .1, 1000., 1., 2, nullptr);
  auto *st_base = add_number(*tower_grid, "Base height (mm)", tower_defaults.base_height_mm, 0., 1000., .5, 2, nullptr);
  auto *st_body = add_number(*tower_grid, "Body height (mm)", tower_defaults.body_height_mm, 0., 1000., 1., 2, nullptr);
  auto *st_ceil = add_number(*tower_grid, "Ceiling height (mm)", tower_defaults.ceil_height_mm, 0., 1000., .5, 2, nullptr);
  auto *st_chamfer = add_number(*tower_grid, "Chamfer layers", tower_defaults.chamfer_layers, 0., 255., 1., 0, nullptr);
  auto *st_aa = add_check(*tower_grid, "Anti-aliasing", tower_defaults.antialiasing);
  auto *st_spirals = add_number(*tower_grid, "Spirals", tower_defaults.spirals, 1., 255., 1., 0, nullptr);
  auto *st_sp_d = add_number(*tower_grid, "Spiral diameter (mm)", tower_defaults.spiral_diameter_mm, .05, 1000., .5, 2, nullptr);
  auto *st_angle = add_number(*tower_grid, "Spiral angle per layer (deg)", tower_defaults.spiral_angle_step_deg, 0., 360., .1, 2, nullptr);
  tower_grid->Add(new wxStaticText(&options, wxID_ANY, "Spiral direction"), 0, wxALIGN_CENTER_VERTICAL);
  auto *st_direction = create_readonly_combo(&options);
  for (const char *label : {"Clockwise", "Alternate", "Both"}) st_direction->Append(label);
  st_direction->SetSelection(int(tower_defaults.spiral_direction));
  tower_grid->Add(st_direction, 1, wxEXPAND);
  // XYZ accuracy (UVtools): hollow labelled blocks at up to nine positions.
  auto *xyz_grid = make_grid(), *tol_grid = make_grid(), *lift_grid = make_grid();
  const sla::XYZAccuracyParameters xyz_defaults;
  auto *xy_bottom = add_number(*xyz_grid, "Bottom layers (0 = material preset)", 0., 0., 10000., 1., 0, nullptr);
  auto *xy_tb = add_number(*xyz_grid, "Top/bottom margin (px)", xyz_defaults.top_bottom_margin_px, 0., 32768., 1., 0, nullptr);
  auto *xy_lr = add_number(*xyz_grid, "Left/right margin (px)", xyz_defaults.left_right_margin_px, 0., 32768., 1., 0, nullptr);
  auto *xy_x = add_number(*xyz_grid, "Block X (mm)", xyz_defaults.x_mm, .01, 1000., 1., 2, "Truncated to whole pixels; the summary shows the real size.");
  auto *xy_y = add_number(*xyz_grid, "Block Y (mm)", xyz_defaults.y_mm, .01, 1000., 1., 2, nullptr);
  auto *xy_z = add_number(*xyz_grid, "Block Z (mm)", xyz_defaults.z_mm, .01, 1000., 1., 2, "Truncated to whole layers.");
  auto *xy_relief = add_check(*xyz_grid, "Center hole relief", xyz_defaults.center_hole_relief);
  auto *xy_hollow = add_check(*xyz_grid, "Hollow blocks", xyz_defaults.hollow);
  auto *xy_wall = add_number(*xyz_grid, "Wall thickness (mm)", xyz_defaults.wall_thickness_mm, 0., 1000., .5, 2, nullptr);
  auto *xy_drain = add_number(*xyz_grid, "Drain slot (mm)", xyz_defaults.drain_hole_mm, 0., 1000., .5, 2,
                              "Slot cut across hollow blocks right above the bottom layers so resin can drain. 0 = none.");
  auto *xy_aa = add_check(*xyz_grid, "Anti-aliasing", xyz_defaults.antialiasing);
  std::array<wxCheckBox *, 9> xy_positions{};
  {
    xyz_grid->Add(new wxStaticText(&options, wxID_ANY, "Block positions (top row first)"), 0, wxALIGN_CENTER_VERTICAL);
    auto *positions = new wxGridSizer(3, 4, 4);
    static const char *names[9] = {"TL", "TC", "TR", "ML", "MC", "MR", "BL", "BC", "BR"};
    for (size_t i = 0; i < 9; ++i) {
      xy_positions[i] = new wxCheckBox(&options, wxID_ANY, names[i]);
      xy_positions[i]->SetValue(xyz_defaults.objects[i]);
      positions->Add(xy_positions[i]);
    }
    xyz_grid->Add(positions, 1, wxEXPAND);
  }
  // Tolerance (UVtools): female ring + male parts stepping around the hole size.
  const sla::ToleranceParameters tol_defaults;
  auto *tl_bottom = add_number(*tol_grid, "Bottom layers (0 = material preset)", 0., 0., 10000., 1., 0, nullptr);
  auto *tl_z = add_number(*tol_grid, "Test height (mm)", tol_defaults.z_mm, .01, 1000., 1., 2, nullptr);
  auto *tl_tb = add_number(*tol_grid, "Top/bottom margin (px)", tol_defaults.top_bottom_margin_px, 0., 32768., 1., 0, nullptr);
  auto *tl_lr = add_number(*tol_grid, "Left/right margin (px)", tol_defaults.left_right_margin_px, 0., 32768., 1., 0, nullptr);
  auto *tl_chamfer = add_number(*tol_grid, "Chamfer layers", tol_defaults.chamfer_layers, 0., 255., 1., 0, nullptr);
  auto *tl_erode = add_number(*tol_grid, "Erode bottom layers (iterations)", tol_defaults.erode_bottom_iterations, 0., 255., 1., 0, nullptr);
  tol_grid->Add(new wxStaticText(&options, wxID_ANY, "Shape"), 0, wxALIGN_CENTER_VERTICAL);
  auto *tl_shape = create_readonly_combo(&options);
  for (const char *label : {"Circle", "Square"}) tl_shape->Append(label);
  tl_shape->SetSelection(int(tol_defaults.shape));
  tol_grid->Add(tl_shape, 1, wxEXPAND);
  auto *tl_margin = add_number(*tol_grid, "Part margin (px)", tol_defaults.part_margin_px, 0., 32768., 1., 0, nullptr);
  auto *tl_same = add_check(*tol_grid, "Output same-size part", tol_defaults.output_same_diameter_part);
  auto *tl_fuse = add_check(*tol_grid, "Fuse parts (each part inside its own ring)", tol_defaults.fuse_parts);
  auto *tl_aa = add_check(*tol_grid, "Anti-aliasing", tol_defaults.antialiasing);
  auto *tl_female = add_number(*tol_grid, "Female diameter (mm)", tol_defaults.female_diameter_mm, .1, 1000., 1., 2, nullptr);
  auto *tl_hole = add_number(*tol_grid, "Female hole diameter (mm)", tol_defaults.female_hole_diameter_mm, .1, 1000., 1., 2, nullptr);
  auto *tl_thin_n = add_number(*tol_grid, "Thinner models", tol_defaults.thinner_models, 0., 1000., 1., 0, nullptr);
  auto *tl_thin_o = add_number(*tol_grid, "Thinner offset (mm)", tol_defaults.thinner_offset_mm, -100., 100., .05, 2, nullptr);
  auto *tl_thin_s = add_number(*tol_grid, "Thinner step (mm)", tol_defaults.thinner_step_mm, -100., 100., .05, 2, nullptr);
  auto *tl_thick_n = add_number(*tol_grid, "Thicker models", tol_defaults.thicker_models, 0., 1000., 1., 0, nullptr);
  auto *tl_thick_o = add_number(*tol_grid, "Thicker offset (mm)", tol_defaults.thicker_offset_mm, -100., 100., .05, 2, nullptr);
  auto *tl_thick_s = add_number(*tol_grid, "Thicker step (mm)", tol_defaults.thicker_step_mm, -100., 100., .05, 2, nullptr);
  // Lift height (UVtools): motion fields start from the active material preset.
  const auto &preset_config = wxGetApp().preset_bundle->full_config();
  const auto preset_value = [&](const char *key, double fallback) {
    return preset_config.has(key) ? preset_config.opt_float(key) : fallback;
  };
  const sla::LiftHeightParameters lift_defaults;
  auto *lf_bottom = add_number(*lift_grid, "Bottom layers", lift_defaults.bottom_layers, 0., 10000., 1., 0, nullptr);
  auto *lf_normal = add_number(*lift_grid, "Normal layers", lift_defaults.normal_layers, 0., 10000., 1., 0, nullptr);
  auto *lf_b_exp = add_number(*lift_grid, "Bottom exposure (s)", preset_value("initial_exposure_time", 30.), .01, 86400., 1., 2, nullptr);
  auto *lf_exp = add_number(*lift_grid, "Normal exposure (s)", preset_value("exposure_time", 2.5), .01, 86400., .1, 2, nullptr);
  auto *lf_b_lift = add_number(*lift_grid, "Bottom lift height (mm)", preset_value("sla_bottom_lift_height", 6.), 0., 1000., .5, 2, nullptr);
  auto *lf_lift = add_number(*lift_grid, "Lift height (mm)", preset_value("sla_lift_height", 6.), 0., 1000., .5, 2,
                             "Print the test with different lift heights and keep the lowest that still peels cleanly.");
  auto *lf_b_speed = add_number(*lift_grid, "Bottom lift speed (mm/min)", preset_value("sla_bottom_lift_speed", 60.), .1, 100000., 5., 1, nullptr);
  auto *lf_speed = add_number(*lift_grid, "Lift speed (mm/min)", preset_value("sla_lift_speed", 80.), .1, 100000., 5., 1, nullptr);
  auto *lf_retract = add_number(*lift_grid, "Retract speed (mm/min)", preset_value("sla_retract_speed", 150.), .1, 100000., 5., 1, nullptr);
  auto *lf_lr = add_number(*lift_grid, "Left/right margin (px)", lift_defaults.left_right_margin_px, 0., 32768., 1., 0, nullptr);
  auto *lf_tb = add_number(*lift_grid, "Top/bottom margin (px)", lift_defaults.top_bottom_margin_px, 0., 32768., 1., 0, nullptr);
  auto *lf_decrease = add_check(*lift_grid, "Decrease image size at the end", lift_defaults.decrease_image);
  auto *lf_factor = add_number(*lift_grid, "Decrease step (%)", lift_defaults.decrease_factor_pct, 1., 99., 1., 0, nullptr);
  auto *lf_min = add_number(*lift_grid, "Minimum size (%)", lift_defaults.minimum_factor_pct, 1., 99., 1., 0, nullptr);
  auto *levels = new wxTextCtrl(&options, wxID_ANY, "", wxDefaultPosition,
      wxSize(440, 110), wxTE_MULTILINE | wxTE_READONLY);
  levels->SetName("Generated grayscale levels");
  auto read_parameters = [&] {
    sla::CalibrationParameters parameters;
    parameters.pattern = static_cast<sla::CalibrationPattern>(pattern->GetSelection());
    parameters.size_x = sx->GetValue(); parameters.size_y = sy->GetValue();
    parameters.height = height->GetValue(); parameters.margin = margin->GetValue();
    parameters.grayscale_steps = unsigned(std::lround(steps->GetValue()));
    parameters.grayscale_first = uint8_t(std::lround(first->GetValue()));
    parameters.grayscale_last = uint8_t(std::lround(last->GetValue()));
    if (parameters.pattern == sla::CalibrationPattern::LiftArea) {
      parameters.lift_taper_step = unsigned(std::lround(taper_step->GetValue()));
      parameters.lift_taper_min = unsigned(std::lround(taper_min->GetValue()));
    }
    return parameters;
  };
  const auto read_elephant = [&] {
    sla::ElephantFootParameters p;
    p.bottom_layers = unsigned(std::lround(ef_bottom->GetValue())); p.normal_layers = unsigned(std::lround(ef_normal->GetValue()));
    p.part_scale = ef_scale->GetValue(); p.margin_px = unsigned(std::lround(ef_margin->GetValue()));
    p.output_original = ef_original->GetValue(); p.extrude_text = ef_extrude->GetValue();
    p.text_height_mm = ef_text_h->GetValue(); p.antialiasing = ef_aa->GetValue();
    p.erode = ef_erode->GetValue(); p.erode_start = unsigned(std::lround(ef_e_start->GetValue()));
    p.erode_end = unsigned(std::lround(ef_e_end->GetValue())); p.erode_step = unsigned(std::lround(ef_e_step->GetValue()));
    p.dimming = ef_dim->GetValue(); p.dimming_wall_px = unsigned(std::lround(ef_d_wall->GetValue()));
    p.dimming_start = unsigned(std::lround(ef_d_start->GetValue())); p.dimming_end = unsigned(std::lround(ef_d_end->GetValue()));
    p.dimming_step = unsigned(std::lround(ef_d_step->GetValue()));
    return p;
  };
  const auto read_blooming = [&] {
    sla::BloomingParameters p;
    p.bottom_layers = unsigned(std::lround(bl_bottom->GetValue()));
    p.left_right_margin_px = unsigned(std::lround(bl_lr->GetValue())); p.top_bottom_margin_px = unsigned(std::lround(bl_tb->GetValue()));
    p.wait_start_s = bl_start->GetValue(); p.wait_increment_s = bl_step->GetValue();
    p.object_count = unsigned(std::lround(bl_count->GetValue()));
    p.object_diameter_px = unsigned(std::lround(bl_diameter->GetValue())); p.object_margin_px = unsigned(std::lround(bl_margin->GetValue()));
    p.object_height_mm = bl_height->GetValue();
    return p;
  };
  const auto read_tower = [&] {
    sla::StressTowerParameters p;
    p.base_diameter_mm = st_diameter->GetValue(); p.base_height_mm = st_base->GetValue();
    p.body_height_mm = st_body->GetValue(); p.ceil_height_mm = st_ceil->GetValue();
    p.chamfer_layers = unsigned(std::lround(st_chamfer->GetValue())); p.antialiasing = st_aa->GetValue();
    p.spirals = unsigned(std::lround(st_spirals->GetValue())); p.spiral_diameter_mm = st_sp_d->GetValue();
    p.spiral_angle_step_deg = st_angle->GetValue();
    p.spiral_direction = static_cast<sla::StressTowerSpiral>(std::max(0, st_direction->GetSelection()));
    return p;
  };
  const auto count_of = [](wxSpinCtrlDouble *control) { return unsigned(std::lround(control->GetValue())); };
  const auto read_xyz = [&] {
    sla::XYZAccuracyParameters p;
    p.bottom_layers = count_of(xy_bottom);
    p.top_bottom_margin_px = count_of(xy_tb); p.left_right_margin_px = count_of(xy_lr);
    p.x_mm = xy_x->GetValue(); p.y_mm = xy_y->GetValue(); p.z_mm = xy_z->GetValue();
    p.center_hole_relief = xy_relief->GetValue(); p.hollow = xy_hollow->GetValue();
    p.wall_thickness_mm = xy_wall->GetValue(); p.drain_hole_mm = xy_drain->GetValue();
    p.antialiasing = xy_aa->GetValue();
    for (size_t i = 0; i < 9; ++i) p.objects[i] = xy_positions[i]->GetValue();
    return p;
  };
  const auto read_tolerance = [&] {
    sla::ToleranceParameters p;
    p.bottom_layers = count_of(tl_bottom); p.z_mm = tl_z->GetValue();
    p.top_bottom_margin_px = count_of(tl_tb); p.left_right_margin_px = count_of(tl_lr);
    p.chamfer_layers = count_of(tl_chamfer); p.erode_bottom_iterations = count_of(tl_erode);
    p.shape = static_cast<sla::ToleranceShape>(std::max(0, tl_shape->GetSelection()));
    p.part_margin_px = count_of(tl_margin);
    p.output_same_diameter_part = tl_same->GetValue(); p.fuse_parts = tl_fuse->GetValue();
    p.antialiasing = tl_aa->GetValue();
    p.female_diameter_mm = tl_female->GetValue(); p.female_hole_diameter_mm = tl_hole->GetValue();
    p.thinner_models = count_of(tl_thin_n); p.thinner_offset_mm = tl_thin_o->GetValue(); p.thinner_step_mm = tl_thin_s->GetValue();
    p.thicker_models = count_of(tl_thick_n); p.thicker_offset_mm = tl_thick_o->GetValue(); p.thicker_step_mm = tl_thick_s->GetValue();
    return p;
  };
  const auto read_lift = [&] {
    sla::LiftHeightParameters p;
    p.bottom_layers = count_of(lf_bottom); p.normal_layers = count_of(lf_normal);
    p.left_right_margin_px = count_of(lf_lr); p.top_bottom_margin_px = count_of(lf_tb);
    p.decrease_image = lf_decrease->GetValue();
    p.decrease_factor_pct = count_of(lf_factor); p.minimum_factor_pct = count_of(lf_min);
    p.bottom_exposure_s = lf_b_exp->GetValue(); p.normal_exposure_s = lf_exp->GetValue();
    p.bottom_lift_height_mm = lf_b_lift->GetValue(); p.lift_height_mm = lf_lift->GetValue();
    p.bottom_lift_speed_mm_min = lf_b_speed->GetValue(); p.lift_speed_mm_min = lf_speed->GetValue();
    p.retract_speed_mm_min = lf_retract->GetValue();
    return p;
  };
  auto update = [&] {
    const int selection = pattern->GetSelection();
    const bool grayscale = selection == 1;
    const bool lift = selection == 2;
    const bool pixel_pattern = selection >= 3;
    root->Show(grid, !pixel_pattern);
    root->Show(ef_grid, selection == 3);
    root->Show(bloom_grid, selection == 4);
    root->Show(tower_grid, selection == 5);
    root->Show(xyz_grid, selection == 6);
    root->Show(tol_grid, selection == 7);
    root->Show(lift_grid, selection == 8);
    if (pixel_pattern) {
      bool valid = true;
      wxString summary;
      try {
        const auto &config = wxGetApp().preset_bundle->full_config();
        if (selection == 3) {
          const auto plan = sla::elephant_foot_plan(config, read_elephant());
          summary = wxString::Format("%u parts (%u requested), part %d x %d px, %u bottom + %u normal + %u text layers.\n",
              unsigned(plan.cells.size()), unsigned(plan.requested_objects), plan.shape_width_px, plan.shape_height_px,
              plan.bottom_layers, plan.normal_layers, plan.text_layers);
          if (plan.cells.size() < plan.requested_objects) summary += "Some parts do not fit the screen and are skipped.\n";
          summary += "How to read: the printed part whose bottom is flush with the layers above it (no wider foot) "
                     "has the best compensation. E: = erode iterations, W/B = wall width (px) and brightness of the bottom "
                     "layers, ORI = uncompensated part. Repeat after changing printer, resin or exposure.";
        } else if (selection == 4) {
          const auto plan = sla::blooming_plan(config, read_blooming());
          summary = wxString::Format("%u of %u squares fit the screen, %u layers per square + %u text layers (%u masks).\n",
              unsigned(plan.objects.size()), unsigned(plan.requested_objects), unsigned(plan.object_layers),
              unsigned(plan.text_layers), unsigned(plan.total_layers));
          summary += "How to read: every square is cured at the same height with a different wait time before cure (label W:). "
                     "Measure the printed squares; the one closest to its designed size has the best wait time. "
                     "Requires UVJ or CXDLPv4 export (repeated-height exposures).";
        } else if (selection == 6) {
          const auto parameters = read_xyz();
          const auto plan = sla::xyz_accuracy_plan(config, parameters);
          summary = wxString::Format("%u block(s), %d x %d px each = %.3f x %.3f x %.3f mm real size "
                                     "(requested %.2f x %.2f x %.2f), %u layers (%u bottom, %u with drain slot).\n",
              plan.object_count, plan.block_x_px, plan.block_y_px, plan.real_x_mm, plan.real_y_mm, plan.real_z_mm,
              parameters.x_mm, parameters.y_mm, parameters.z_mm, plan.total_layers, plan.bottom_layers, plan.vent_layers);
          summary += "How to read: measure each printed block in X, Y and Z with calipers and compare with the real "
                     "size above. Scale factor = real size / measured size, per axis (apply it as model scaling or in "
                     "printer correction). Blocks at different positions show uneven light or screen distortion. "
                     "Repeat after changing printer, resin or exposure.";
        } else if (selection == 7) {
          const auto plan = sla::tolerance_plan(config, read_tolerance());
          summary = wxString::Format("%u of %u objects fit, %u layers (%u bottom). Female ring %d x %d px, hole %d x %d px.\n",
              unsigned(plan.parts.size()), unsigned(plan.requested_objects), plan.total_layers, plan.bottom_layers,
              plan.female_x_px, plan.female_y_px, plan.hole_x_px, plan.hole_y_px);
          if (plan.parts.size() + (read_tolerance().fuse_parts ? 0 : 1) < plan.requested_objects)
            summary += "Some parts do not fit the screen and are skipped.\n";
          summary += "How to read: each male part is labelled with its size offset from the hole (mm). Try the parts in the "
                     "printed ring: the largest one that slides in without force gives the real clearance; use it to "
                     "set the design tolerance for mating parts. Repeat after changing printer, resin or exposure.";
        } else if (selection == 8) {
          const auto plan = sla::lift_height_plan(config, read_lift());
          summary = wxString::Format("%u bottom + %u normal + %u decreasing layers = %.2f mm, block %d x %d px.\n",
              plan.bottom_layers, plan.normal_layers, plan.decrease_layers, plan.total_height_mm,
              plan.block_width_px, plan.block_height_px);
          summary += "How to read: run the print with a force gauge or watch the peel. The large block needs the most "
                     "lift; note the layer (size %) where it peels cleanly. Take the lowest lift height that works and "
                     "add 1-2 mm for safety. The motion values above are written into the generated file only; "
                     "resin and printer presets stay unchanged.";
        } else if (selection == 9) {
          valid = false;  // reference only, nothing to generate
          for (const auto &test : sla::external_calibration_tests())
            summary += wxString::FromUTF8(test.name + "\n  " + test.purpose + "\n  " + test.address + "\n  Native: " +
                                          test.native_alternative + "\n\n");
          summary += "UVtools only opens these pages. This application is offline, so the addresses are listed as "
                     "text to open on another device; nothing is downloaded.";
        } else {
          const auto plan = sla::stress_tower_plan(config, read_tower());
          summary = wxString::Format("%u layers (%u base + %u body + %u ceiling), base radius %d px, spiral radius %d px.\n",
              plan.total_layers, plan.base_layers, plan.body_layers, plan.ceil_layers, plan.base_radius_px, plan.spiral_radius_px);
          summary += "How to read: inspect the printed tower along its height for layer shifts, broken spirals and "
                     "surface artifacts; they show where the printer motion or exposure reaches its limit.";
        }
      } catch (const std::exception &error) {
        summary = wxString::FromUTF8(error.what()); valid = false;
      }
      levels->ChangeValue(summary);
      levels->Enable(true);
      if (auto *generate = options.FindWindow(wxID_OK)) generate->Enable(valid);
      options.Layout(); options.Fit();
      return;
    }
    for (auto *control : {steps, first, last})
      control->Enable(grayscale);
    taper_step->Enable(lift);
    taper_min->Enable(lift && taper_step->GetValue() > 0);
    bool valid = true;
    wxString summary;
    if (grayscale) {
      try {
        const auto plan = sla::calibration_grayscale_plan(
            wxGetApp().preset_bundle->full_config(), read_parameters());
        summary = wxString::Format("Generated mask levels: %u distinct / %u patches.\nRequested -> generated (0-255):\n",
            unsigned(plan.distinct_levels), unsigned(plan.effective.size()));
        for (size_t i = 0; i < plan.requested.size(); ++i)
          summary += wxString::Format("%u -> %u%s", unsigned(plan.requested[i]),
              unsigned(plan.effective[i]), (i + 1) % 4 == 0 ? "\n" : "    ");
        if (plan.has_duplicates()) {
          summary += "\nLevels repeat: reduce steps or widen the grayscale range.";
          valid = false;
        } else if (plan.changed_levels)
          summary += "\nThe generated palette changes some requested levels.";
        summary += "\nChanging the export format later may require another palette conversion.";
      } catch (const std::exception &error) {
        summary = wxString::FromUTF8(error.what()); valid = false;
      }
    }
    if (lift) {
      try {
        const auto plan = sla::calibration_lift_plan(
            wxGetApp().preset_bundle->full_config(), read_parameters());
        summary = wxString::Format("%u full-size layers + %u decreasing layers.\nTotal height includes both parts.\n",
            unsigned(plan.base_layers), unsigned(plan.factors.size()));
        if (plan.factors.empty()) summary += "Size reduction disabled (step = 0).";
        else {
          summary += "Final X and Y sizes (% of original dimensions): ";
          for (unsigned factor : plan.factors) summary += wxString::Format("%u%% ", factor);
          summary += "\nBoth sides decrease: 90% X and Y gives 81% area.\nExposure and movement use the active presets.";
        }
      } catch (const std::exception &error) {
        summary = wxString::FromUTF8(error.what()); valid = false;
      }
    }
    levels->ChangeValue(summary);
    levels->Enable(grayscale || lift);
    if (auto *generate = options.FindWindow(wxID_OK)) generate->Enable(valid);
  };
  pattern->Bind(wxEVT_COMBOBOX, [&](wxCommandEvent &) { update(); });
  for (auto *control : {ef_bottom, ef_normal, ef_scale, ef_margin, ef_text_h, ef_e_start, ef_e_end, ef_e_step, ef_d_wall,
                        ef_d_start, ef_d_end, ef_d_step, bl_bottom, bl_lr, bl_tb, bl_start, bl_step, bl_count, bl_diameter,
                        bl_margin, bl_height, st_diameter, st_base, st_body, st_ceil, st_chamfer, st_spirals, st_sp_d,
                        st_angle}) {
    control->Bind(wxEVT_SPINCTRLDOUBLE, [&](wxSpinDoubleEvent &) { update(); });
    control->Bind(wxEVT_TEXT, [&](wxCommandEvent &event) { update(); event.Skip(); });
  }
  for (auto *control : {ef_original, ef_extrude, ef_aa, ef_erode, ef_dim, st_aa})
    control->Bind(wxEVT_CHECKBOX, [&](wxCommandEvent &) { update(); });
  st_direction->Bind(wxEVT_COMBOBOX, [&](wxCommandEvent &) { update(); });
  tl_shape->Bind(wxEVT_COMBOBOX, [&](wxCommandEvent &) { update(); });
  for (auto *control : {xy_bottom, xy_tb, xy_lr, xy_x, xy_y, xy_z, xy_wall, xy_drain, tl_bottom, tl_z, tl_tb, tl_lr,
                        tl_chamfer, tl_erode, tl_margin, tl_female, tl_hole, tl_thin_n, tl_thin_o, tl_thin_s, tl_thick_n,
                        tl_thick_o, tl_thick_s, lf_bottom, lf_normal, lf_b_exp, lf_exp, lf_b_lift, lf_lift, lf_b_speed,
                        lf_speed, lf_retract, lf_lr, lf_tb, lf_factor, lf_min}) {
    control->Bind(wxEVT_SPINCTRLDOUBLE, [&](wxSpinDoubleEvent &) { update(); });
    control->Bind(wxEVT_TEXT, [&](wxCommandEvent &event) { update(); event.Skip(); });
  }
  for (auto *control : {xy_relief, xy_hollow, xy_aa, tl_same, tl_fuse, tl_aa, lf_decrease})
    control->Bind(wxEVT_CHECKBOX, [&](wxCommandEvent &) { update(); });
  for (auto *control : xy_positions)
    control->Bind(wxEVT_CHECKBOX, [&](wxCommandEvent &) { update(); });
  for (auto *control : {steps, first, last, taper_step, taper_min, height}) {
    control->Bind(wxEVT_SPINCTRLDOUBLE, [&](wxSpinDoubleEvent &) { update(); });
    control->Bind(wxEVT_TEXT, [&](wxCommandEvent &event) { update(); event.Skip(); });
  }
  root->Add(grid, 0, wxEXPAND | wxALL, 12);
  root->Add(ef_grid, 0, wxEXPAND | wxALL, 12);
  root->Add(bloom_grid, 0, wxEXPAND | wxALL, 12);
  root->Add(tower_grid, 0, wxEXPAND | wxALL, 12);
  root->Add(xyz_grid, 0, wxEXPAND | wxALL, 12);
  root->Add(tol_grid, 0, wxEXPAND | wxALL, 12);
  root->Add(lift_grid, 0, wxEXPAND | wxALL, 12);
  root->Add(levels, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);
  auto *apply_result = new wxButton(&options, wxID_ANY, "Apply result to profile...");
  apply_result->SetToolTip("After printing a test, enter the best cell or value and write it to the edited preset.");
  apply_result->Bind(wxEVT_BUTTON, [&](wxCommandEvent &) { show_apply_result(&options); });
  root->Add(apply_result, 0, wxLEFT | wxRIGHT | wxBOTTOM, 12);
  auto *buttons = options.CreateSeparatedButtonSizer(wxOK | wxCANCEL);
  options.FindWindow(wxID_OK)->SetLabel("Generate calibration layers");
  root->Add(buttons, 0, wxEXPAND | wxALL, 12);
  update();
  options.SetSizerAndFit(root);
  options.CentreOnParent();
  while (options.ShowModal() == wxID_OK) {
    try {
      auto config = wxGetApp().preset_bundle->full_config();
      if (config.opt_string("sla_archive_format") == "SL1SVG")
        config.set("sla_archive_format", std::string("uvj"));
      const int selection = pattern->GetSelection();
      const auto ef = read_elephant();
      const auto bloom = read_blooming();
      const auto tower = read_tower();
      if (selection >= 3 && selection <= 8) {
        // Parameters are re-read here so the worker owns an immutable copy.
        const auto xyz = selection == 6 ? read_xyz() : sla::XYZAccuracyParameters{};
        const auto tolerance = selection == 7 ? read_tolerance() : sla::ToleranceParameters{};
        const auto lift = selection == 8 ? read_lift() : sla::LiftHeightParameters{};
        // Pixel-domain generators: parameters are validated in the worker too.
        auto job = run_raster_generator(parent, [=](const std::function<void()> &check) {
          check();
          if (selection == 3) return sla::create_elephant_foot_calibration(config, ef, check);
          if (selection == 4) return sla::create_blooming_calibration(config, bloom, check);
          if (selection == 6) return sla::create_xyz_accuracy_calibration(config, xyz, check);
          if (selection == 7) return sla::create_tolerance_calibration(config, tolerance, check);
          if (selection == 8) return sla::create_lift_height_calibration(config, lift, check);
          return sla::create_stress_tower_calibration(config, tower, check);
        });
        if (!job) continue;
        auto *plater = wxGetApp().plater();
        if (!plater) throw RuntimeError("The Sliced workspace is unavailable");
        if (!plater->open_generated_layers(*job)) continue;
        return;
      }
      const auto parameters = read_parameters();
      if (parameters.pattern == sla::CalibrationPattern::GrayscalePatches) {
        const auto plan = sla::calibration_grayscale_plan(config, parameters);
        if (plan.has_duplicates()) { update(); continue; }
        if (plan.changed_levels && wxMessageBox(
            "The generated palette changes some requested grayscale levels.\n"
            "Use the generated levels shown in the table for this calibration?",
            "Grayscale calibration", wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION,
            &options) != wxYES)
          continue;
      }
      auto job =
          run_raster_generator(parent, [=](const std::function<void()> &check) {
            check();
            return sla::create_calibration(config, parameters);
          });
      if (!job)
        continue;
      auto *plater = wxGetApp().plater();
      if (!plater) throw RuntimeError("The Sliced workspace is unavailable");
      if (!plater->open_generated_layers(*job))
        continue; // Preserve the filled form when replacement is declined.
      return;
    } catch (const std::exception &error) {
      wxMessageBox(wxString::FromUTF8(error.what()), "Printer calibration",
                   wxOK | wxICON_ERROR, &options);
    }
  }
}

} // namespace Slic3r::GUI
