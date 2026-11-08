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
#include <atomic>
#include <memory>
#include <mutex>
#include <algorithm>
#include <wx/button.h>

namespace Slic3r::GUI {
// Bind a request produced by the worker to cancellation from the UI thread.
// Cancellation before binding is remembered; after binding it uses the
// request's publication lock, rather than waiting for another progress poll.
template<class Request> class BackgroundRequestOwner {
    mutable std::mutex m_mutex;
    std::shared_ptr<const Request> m_request;
    bool m_cancelled = false;
public:
    bool bind(std::shared_ptr<const Request> request) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_request = std::move(request);
        if (m_cancelled) m_request->cancel();
        return !m_cancelled;
    }
    void cancel() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_cancelled = true;
        if (m_request) m_request->cancel();
    }
    std::shared_ptr<const Request> request() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_request;
    }
};

// Cancellation is observed at whole-layer boundaries. The writer stages its
// output and owns rollback; this helper owns only UI and worker lifetime.
inline bool run_background_layers(wxWindow *parent, const wxString &title,
    const std::function<bool(const std::function<bool(size_t,size_t)> &)> &write,
    const std::function<void()> &cancel_write = {})
{
    wxDialog progress(parent, wxID_ANY, title);
    auto *root = new wxBoxSizer(wxVERTICAL);
    auto *status = new wxStaticText(&progress, wxID_ANY, "Preparing masks…");
    auto *gauge = new wxGauge(&progress, wxID_ANY, 1000, wxDefaultPosition, wxSize(360,-1));
    auto *cancel_button = new wxButton(&progress, wxID_CANCEL, "Cancel");
    root->Add(status, 0, wxALL, 12);
    root->Add(gauge, 0, wxEXPAND | wxLEFT | wxRIGHT, 12);
    root->Add(cancel_button, 0, wxALIGN_RIGHT | wxALL, 12);
    progress.SetSizerAndFit(root); progress.CentreOnParent();
    std::atomic<bool> cancel{false};
    std::atomic<size_t> done{0}, total{0};
    auto task = std::async(std::launch::async, [&] {
        return write([&](size_t count, size_t layers) {
            if(layers) {
                total.store(layers, std::memory_order_relaxed);
                done.store(count, std::memory_order_relaxed);
            }
            return !cancel.load(std::memory_order_relaxed);
        });
    });
    auto request_cancel = [&] {
        if (task.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) return;
        if (!cancel.exchange(true, std::memory_order_relaxed) && cancel_write) cancel_write();
        cancel_button->Disable(); status->SetLabel("Cancelling…");
    };
    progress.Bind(wxEVT_BUTTON, [&](wxCommandEvent &) { request_cancel(); }, wxID_CANCEL);
    progress.Bind(wxEVT_CLOSE_WINDOW, [&](wxCloseEvent &event) {
        request_cancel(); if (event.CanVeto()) event.Veto();
    });
    wxTimer timer(&progress);
    progress.Bind(wxEVT_TIMER, [&](wxTimerEvent &) {
        if (task.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
            timer.Stop(); progress.EndModal(wxID_OK); return;
        }
        const auto count = done.load(std::memory_order_relaxed);
        const auto layers = total.load(std::memory_order_relaxed);
        if (layers) gauge->SetValue(int(std::min(size_t(999), count*1000/layers)));
        else gauge->Pulse();
        if (!cancel.load(std::memory_order_relaxed))
            status->SetLabel(layers?wxString::Format("Processed: %zu / %zu",count,layers):wxString("Preparing layers…"));
    });
    timer.Start(30); progress.ShowModal(); timer.Stop();
    return task.get();
}

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
