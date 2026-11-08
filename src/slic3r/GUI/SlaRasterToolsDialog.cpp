#include "SlaRasterToolsDialog.hpp"
#include "RasterGeneratorTask.hpp"
#include "GUI_App.hpp"
#include "Plater.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/SLA/CalibrationRaster.hpp"
#include "libslic3r/SLA/PCBExposure.hpp"
#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>
#include <cmath>
#include <functional>
#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/dialog.h>
#include <wx/filedlg.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
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
} // namespace
void show_pcb_exposure(wxWindow *parent) {
  wxDialog options(parent, wxID_ANY, "PCB exposure", wxDefaultPosition,
                   wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
  auto *root = new wxBoxSizer(wxVERTICAL);
  root->Add(new wxStaticText(
                &options, wxID_ANY,
                "Load copper Gerbers and an optional common Excellon drill file.\n"
                "Artwork stays at its physical size; the selected printer sets "
                "the LCD geometry.\n"
                "Review polarity and mirroring in the layer editor before "
                "exporting."),
            0, wxALL, 12);
  auto *grid = new wxFlexGridSizer(2, 8, 12);
  grid->AddGrowableCol(1);
  auto file_row = [&](const char *label, const char *filter, bool multiple = false) {
    grid->Add(new wxStaticText(&options, wxID_ANY, label), 0,
              wxALIGN_CENTER_VERTICAL);
    auto *row = new wxBoxSizer(wxHORIZONTAL);
    auto *path = new wxTextCtrl(&options, wxID_ANY, "", wxDefaultPosition,
                                multiple ? wxSize(360,72) : wxDefaultSize,
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
  auto *exposure =
      generator_number(options, *grid, "Exposure (s)", 30., .001, 86400., 1.);
  auto *margin =
      generator_number(options, *grid, "LCD margin (mm)", 1., 0., 1000., .1);
  auto *offset_x =
      generator_number(options, *grid, "X offset (mm)", 0., -1000., 1000., .1);
  auto *offset_y =
      generator_number(options, *grid, "Y offset (mm)", 0., -1000., 1000., .1);
  root->Add(grid, 0, wxEXPAND | wxALL, 12);
  auto *merge = new wxCheckBox(&options, wxID_ANY, "Merge Gerber files into one exposure");
  merge->SetValue(true);
  merge->SetToolTip("Unchecked: one exposure layer per file, in list order, with common alignment.");
  root->Add(merge, 0, wxLEFT | wxBOTTOM, 12);
  auto *mirror_x = new wxCheckBox(&options, wxID_ANY, "Mirror artwork in X");
  auto *mirror_y = new wxCheckBox(&options, wxID_ANY, "Mirror artwork in Y");
  auto *invert = new wxCheckBox(&options, wxID_ANY, "Invert exposure polarity");
  for (auto *check : {mirror_x, mirror_y, invert})
    root->Add(check, 0, wxLEFT | wxBOTTOM, 12);
  auto *buttons = options.CreateSeparatedButtonSizer(wxOK | wxCANCEL);
  options.FindWindow(wxID_OK)->SetLabel("Generate PCB layers");
  root->Add(buttons, 0, wxEXPAND | wxALL, 12);
  options.SetSizerAndFit(root);
  options.SetMinSize(wxSize(620, options.GetSize().y));
  options.CentreOnParent();
  while (options.ShowModal() == wxID_OK) {
    try {
      if (gerber->GetValue().empty())
        throw RuntimeError("Choose a copper Gerber file");
      sla::PCBPlacement placement;
      placement.margin = margin->GetValue();
      placement.offset_x = offset_x->GetValue();
      placement.offset_y = offset_y->GetValue();
      placement.mirror_x = mirror_x->GetValue();
      placement.mirror_y = mirror_y->GetValue();
      const auto gerber_path = gerber->GetValue(),
                 drill_path = drill->GetValue();
      const bool invert_mask = invert->GetValue(), merge_files = merge->GetValue();
      const double exposure_seconds = exposure->GetValue();
      const auto configuration = wxGetApp().preset_bundle->full_config();
      auto job =
          run_raster_generator(parent, [=](const std::function<void()> &check) {
            auto config = configuration;
            SLAPrinterConfig printer;
            printer.apply(config, true);
            const double tolerance =
                std::min(printer.display_width.value /
                             printer.display_pixels_x.value,
                         printer.display_height.value /
                             printer.display_pixels_y.value) *
                .25;
            const auto paths = wxStringTokenize(gerber_path, "\n", wxTOKEN_STRTOK);
            if (paths.empty() || paths.size() > 1000)
              throw RuntimeError("Choose 1–1000 copper files, one path per line");
            const auto drill_text = drill_path.empty() ? std::string{} : read_pcb_file(drill_path);
            std::vector<sla::PCBArtwork> boards;
            size_t copper_vertices = 0;
            for (auto path : paths) {
              check();
              path.Trim(true).Trim(false);
              if (path.empty()) continue;
              auto board = sla::parse_gerber(read_pcb_file(path), tolerance, check);
              if (!drill_text.empty()) sla::subtract_excellon(board, drill_text, tolerance, check);
              for (const auto &polygon : board.copper) {
                copper_vertices += polygon.contour.points.size();
                for (const auto &hole : polygon.holes) copper_vertices += hole.points.size();
              }
              if (copper_vertices > 12000000)
                throw RuntimeError("Combined copper exceeds the 12 million vertex budget");
              boards.push_back(std::move(board));
            }
            return sla::create_pcb_exposure_job(config, boards, merge_files, placement,
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
          "rectangular area, optionally decreasing in size at the end."),
      0, wxALL, 12);
  auto *grid = new wxFlexGridSizer(2, 8, 12);
  grid->AddGrowableCol(1);
  grid->Add(new wxStaticText(&options, wxID_ANY, "Pattern"), 0,
            wxALIGN_CENTER_VERTICAL);
  auto *pattern = new wxChoice(&options, wxID_ANY);
  pattern->SetName("Calibration pattern");
  for (const char *label : {"XYZ blocks", "Grayscale patches", "Lift area"})
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
  auto update = [&] {
    const bool grayscale = pattern->GetSelection() == 1;
    const bool lift = pattern->GetSelection() == 2;
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
  pattern->Bind(wxEVT_CHOICE, [&](wxCommandEvent &) { update(); });
  for (auto *control : {steps, first, last, taper_step, taper_min, height}) {
    control->Bind(wxEVT_SPINCTRLDOUBLE, [&](wxSpinDoubleEvent &) { update(); });
    control->Bind(wxEVT_TEXT, [&](wxCommandEvent &event) { update(); event.Skip(); });
  }
  root->Add(grid, 0, wxEXPAND | wxALL, 12);
  root->Add(levels, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);
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
