// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "libslic3r/SLA/LayerProgress.hpp"
#include <atomic>
#include <chrono>
#include <future>
#include <exception>
#include <optional>
#include <stdexcept>
#include <utility>
#include <wx/button.h>
#include <wx/dialog.h>
#include <wx/gauge.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/thread.h>
#include <wx/timer.h>

namespace Slic3r::GUI {

// Call on the main thread with an immutable captured draft/configuration.
// generate runs on a worker and may only publish progress through its callback;
// it must not access wx windows. Cancellation is cooperative: the modal remains
// alive until the worker unwinds, and no partial result is returned.
template<class Result, class Generate>
std::optional<Result> run_raster_task(wxWindow *parent, const wxString &title, Generate generate)
{
    if (!wxIsMainThread())
        throw std::logic_error("Raster generation dialog requires the main thread");
    wxDialog dialog(parent, wxID_ANY, title);
    auto *root = new wxBoxSizer(wxVERTICAL);
    auto *label = new wxStaticText(&dialog, wxID_ANY, "Preparing exposure masks…");
    auto *gauge = new wxGauge(&dialog, wxID_ANY, 100, wxDefaultPosition, wxSize(360, -1));
    auto *abort = new wxButton(&dialog, wxID_CANCEL, "Cancel");
    root->Add(label, 0, wxALL, 12);
    root->Add(gauge, 0, wxEXPAND | wxLEFT | wxRIGHT, 12);
    root->Add(abort, 0, wxALIGN_RIGHT | wxALL, 12);
    dialog.SetSizerAndFit(root);
    dialog.CentreOnParent();

    // Declared before the future: all callback state outlives its worker even
    // when launching the modal or an event handler throws.
    std::atomic<bool> cancelled{false};
    std::atomic<size_t> completed{0}, total{0};
    const sla::LayerProgress progress = [&](size_t done, size_t count) {
        completed.store(done, std::memory_order_relaxed);
        total.store(count, std::memory_order_relaxed);
        return !cancelled.load(std::memory_order_relaxed);
    };
    auto task = std::async(std::launch::async, [&, generate = std::move(generate)]() mutable -> Result {
        sla::check_layer_progress(progress);
        auto result = generate(progress);
        sla::check_layer_progress(progress);
        return result;
    });
    struct CancelOnUnwind {
        std::atomic<bool> &flag;
        int initial_exceptions = std::uncaught_exceptions();
        ~CancelOnUnwind() {
            if (std::uncaught_exceptions() > initial_exceptions)
                flag.store(true, std::memory_order_relaxed);
        }
    } cancel_on_unwind{cancelled};
    const auto request_cancel = [&] {
        cancelled.store(true, std::memory_order_relaxed);
        abort->Disable();
        label->SetLabel("Cancelling…");
    };
    dialog.Bind(wxEVT_BUTTON, [&](wxCommandEvent &) { request_cancel(); }, wxID_CANCEL);
    dialog.Bind(wxEVT_CLOSE_WINDOW, [&](wxCloseEvent &event) {
        request_cancel();
        if (event.CanVeto()) event.Veto();
        // Do not let the default close handler destroy worker callback state.
    });
    dialog.Bind(wxEVT_CHAR_HOOK, [&](wxKeyEvent &event) {
        if (event.GetKeyCode() == WXK_ESCAPE) request_cancel();
        else event.Skip();
    });
    wxTimer timer(&dialog);
    dialog.Bind(wxEVT_TIMER, [&](wxTimerEvent &) {
        if (task.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
            timer.Stop();
            if (dialog.IsModal()) dialog.EndModal(wxID_OK);
            return;
        }
        if (cancelled.load(std::memory_order_relaxed)) return;
        const auto count = total.load(std::memory_order_relaxed);
        const auto done = completed.load(std::memory_order_relaxed);
        if (count) {
            // Reports may restart for a new phase. The two relaxed counters
            // need not be a coherent pair; clamp for safe cosmetic display.
            const int percent = done >= count ? 100 : int(100.L * done / count);
            gauge->SetValue(percent);
        } else gauge->Pulse();
    });
    try {
        if (!timer.Start(30)) throw std::runtime_error("Could not start raster progress timer");
        dialog.ShowModal();
        timer.Stop();
    } catch (...) {
        cancelled.store(true, std::memory_order_relaxed);
        timer.Stop();
        task.wait();
        throw;
    }
    // An external modal termination also cancels rather than publishing work
    // whose progress window was dismissed. Normal completion is already ready.
    if (task.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready)
        cancelled.store(true, std::memory_order_relaxed);
    try {
        auto result = task.get(); // Joins before callback state/dialog destruction.
        if (cancelled.load(std::memory_order_relaxed)) return std::nullopt;
        return std::optional<Result>(std::move(result));
    } catch (const sla::LayerOperationCancelled &) {
        return std::nullopt;
    }
    // Other exceptions propagate only after get() has joined the worker.
}
} // namespace Slic3r::GUI
