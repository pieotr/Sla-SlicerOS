// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <atomic>
#include <cassert>
#include <cstdint>
#include <exception>
#include <optional>
#include <string>
#include <utility>
#include <wx/event.h>
#include "libslic3r/SLA/ExportGeneration.hpp"
#include "libslic3r/PrintBase.hpp"

namespace Slic3r {
// Written/consumed on the UI thread; the worker only reads current(). Reset
// also advances the token, so queued events from a retired owner are harmless.
class BackgroundProcessAttempts {
    std::atomic<uint64_t> m_current{0};
    uint64_t m_consumed = 0;
public:
    uint64_t advance() { return m_current.fetch_add(1, std::memory_order_acq_rel) + 1; }
    uint64_t current() const { return m_current.load(std::memory_order_acquire); }
    bool consume(uint64_t attempt) {
        if (!attempt || attempt != current() || attempt == m_consumed) return false;
        m_consumed = attempt;
        return true;
    }
};

// Destination strings are allocated before write(), so recording a successful
// atomic publication requires only a non-throwing bool store after its return.
struct SLAExportPublication {
    std::string path;
    std::string format;
    bool committed = false;
};

// Input identity survives raster production, whereas ready_source exists only
// after process() returned successfully. An immutable event copy outlives reset.
struct SLAProcessCompletion {
    sla::ExportInputGuard input;
    std::optional<sla::ExportGenerationGuard> ready_source;
    bool export_started = false;
    std::optional<SLAExportPublication> publication;
};

class SlicingProcessCompletedEvent : public wxEvent
{
public:
	enum StatusType {
		Finished,
		Cancelled,
		Error
	};

	SlicingProcessCompletedEvent(wxEventType eventType, int winid, StatusType status, std::exception_ptr exception,
        uint64_t attempt_id, std::optional<SLAProcessCompletion> sla = {}) :
		wxEvent(winid, eventType), m_status(status), m_exception(exception), m_attempt_id(attempt_id), m_sla(std::move(sla)) {
        if (published_export()) {
            // Lifecycle may remain STATE_CANCELED to wake stop()/stop_internal().
            // A completed atomic publication changes only the user-facing result.
            m_status = Finished;
            if (m_exception) {
                try { std::rethrow_exception(m_exception); }
                catch (const CanceledException &) { /* Cancel lost the commit race. */ }
                catch (...) { m_status = Error; } // Never hide an unrelated failure.
            }
        }
    }
	virtual wxEvent* Clone() const { return new SlicingProcessCompletedEvent(*this); }

    const SLAExportPublication *published_export() const {
        return m_sla && m_sla->publication && m_sla->publication->committed ? &*m_sla->publication : nullptr;
    }
    uint64_t attempt_id() const { return m_attempt_id; }
    const std::optional<SLAProcessCompletion> &sla_completion() const { return m_sla; }
	StatusType 	status()    const { return m_status; }
	bool 		finished()  const { return m_status == Finished; }
	bool 		success()   const { return m_status == Finished; }
	bool 		cancelled() const { return m_status == Cancelled; }
	bool		error() 	const { return m_status == Error; }
	// Unhandled error produced by stdlib or a Win32 structured exception, or unhandled Slic3r's own critical exception.
	bool 		critical_error() const;
	// Critical errors does invalidate plater except CopyFileError.
	bool        invalidate_plater() const;
	// Only valid if error()
	void 		rethrow_exception() const { assert(this->error()); assert(m_exception); std::rethrow_exception(m_exception); }
	// Produce a human readable message to be displayed by a notification or a message box.
	// 2nd parameter defines whether the output should be displayed with a monospace font.
	std::pair<std::string, bool> format_error_message() const;

private:
	StatusType 			m_status;
	std::exception_ptr 	m_exception;
    uint64_t m_attempt_id;
    std::optional<SLAProcessCompletion> m_sla;
};

} // namespace Slic3r
