///|/ Copyright (c) Prusa Research 2020 - 2023 David Kocík @kocikdav, Lukáš Matěna @lukasmatena, Vojtěch Bubník @bubnikv
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef GUI_PROCESS_HPP
#define GUI_PROCESS_HPP

#include <vector>


class wxWindow;
class wxString;

namespace Slic3r {
namespace GUI {

// Start a new slicer instance, optionally with a file to open.
void start_new_slicer(const wxString *path_to_open = nullptr, bool single_instance = false, bool delete_after_load = false);
void start_new_slicer(const std::vector<wxString>& files, bool single_instance = false, bool delete_after_load = false);

} // namespace GUI
} // namespace Slic3r

#endif // GUI_PROCESS_HPP
