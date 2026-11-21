// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
class wxWindow;
namespace Slic3r::GUI {
// "I printed this file...": book the resin of the current slicing result (or typed values) against a bottle of the
// resin stock. Cancel changes nothing.
void show_resin_printed_dialog(wxWindow *parent);
// "Resin stock...": add, edit and remove bottles. Every change is saved atomically at once.
void show_resin_stock_manager(wxWindow *parent);
}
