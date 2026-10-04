#ifndef SLIC3R_GUI_BACKGROUND_TASK_DIALOG_HPP
#define SLIC3R_GUI_BACKGROUND_TASK_DIALOG_HPP
#include <wx/dialog.h>
#include <wx/gauge.h>
#include <wx/stattext.h>
#include <wx/sizer.h>
#include <wx/timer.h>
#include <future>
#include <chrono>
#include <functional>

namespace Slic3r::GUI {
// For atomic writes without a cancellable codec: keep the event loop alive,
// disable competing edits through a modal dialog, and return only after the
// worker has completed. The callback must not access wxWidgets objects.
inline void run_background_write(wxWindow *parent, const wxString &title,
                                 const std::function<void()> &write,
                                 const wxString &message = "Saving layers…")
{
    wxDialog progress(parent, wxID_ANY, title);
    auto *root = new wxBoxSizer(wxVERTICAL);
    root->Add(new wxStaticText(&progress, wxID_ANY, message), 0, wxALL, 12);
    auto *gauge = new wxGauge(&progress, wxID_ANY, 100, wxDefaultPosition, wxSize(360, -1));
    root->Add(gauge, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);
    progress.SetSizerAndFit(root); progress.CentreOnParent();
    auto task = std::async(std::launch::async, write);
    progress.Bind(wxEVT_CLOSE_WINDOW, [](wxCloseEvent &event) { if (event.CanVeto()) event.Veto(); });
    wxTimer timer(&progress);
    progress.Bind(wxEVT_TIMER, [&](wxTimerEvent &) {
        if (task.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
            timer.Stop(); progress.EndModal(wxID_OK);
        } else gauge->Pulse();
    });
    timer.Start(30); progress.ShowModal(); timer.Stop();
    task.get(); // Propagate any I/O/codec error to the caller's normal GUI handler.
}
}
#endif
