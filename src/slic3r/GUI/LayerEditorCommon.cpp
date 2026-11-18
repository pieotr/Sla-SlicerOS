#include "LayerEditorInternal.hpp"

namespace Slic3r::GUI {
namespace layer_editor_detail {
// wxDataViewCtrl is a GtkTreeView on GTK: its rows and cells are exposed through ATK/AT-SPI, but the
// wx window name is not. Publish it as the accessible name of the tree view (no-op off GTK).
void set_dataview_accessible_name(wxWindow *view, const char *name)
{
    set_accessible_name(view, wxString::FromUTF8(name), wxString(), AccessibleRole::Default, true);
}
// ---- Layer issue detection helpers (UI independent) ----------------------
const char *issue_type_label(sla::LayerIssue::Type type)
{
    switch (type) {
    case sla::LayerIssue::Type::Island:        return "Island";
    case sla::LayerIssue::Type::Overhang:      return "Overhang";
    case sla::LayerIssue::Type::ResinTrap:     return "Resin trap";
    case sla::LayerIssue::Type::SuctionCup:    return "Suction cup";
    case sla::LayerIssue::Type::TouchingBound: return "Touching bound";
    case sla::LayerIssue::Type::PrintHeight:   return "Print height";
    case sla::LayerIssue::Type::EmptyLayer:    return "Empty layer";
    }
    return "?";
}
// Z of the top of every layer, mm. Formats without per-layer Z fall back to a
// constant pitch; that only affects the suction-cup height rule and print height.
std::vector<double> issue_layer_positions_z(const sla::LayerDocument &document)
{
    std::vector<double> z(document.layer_count());
    double pitch = 0.05;
    if (const auto job = document.job_parameters(); job && job->layer_height > 0) pitch = job->layer_height;
    for (size_t i = 0; i < z.size(); ++i) {
        const auto parameters = document.layer_parameters(i);
        const auto it = parameters.find(sla::LayerParameter::PositionZ);
        z[i] = it != parameters.end() ? it->second : pitch * double(i + 1);
    }
    return z;
}
} // namespace layer_editor_detail
} // namespace Slic3r::GUI
