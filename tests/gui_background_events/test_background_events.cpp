#include <catch2/catch_test_macros.hpp>
#include <wx/app.h>
#include <memory>
#include <vector>
#include "slic3r/GUI/SlicingProcessCompletedEvent.hpp"

using namespace Slic3r;
namespace {
wxDEFINE_EVENT(EVT_TEST_PROCESS_COMPLETED, SlicingProcessCompletedEvent);
void drain() { wxAppConsole::GetInstance()->ProcessPendingEvents(); }
SlicingProcessCompletedEvent completion(uint64_t attempt, const sla::ExportGenerationOwner &source,
    SlicingProcessCompletedEvent::StatusType status = SlicingProcessCompletedEvent::Cancelled)
{
    return {EVT_TEST_PROCESS_COMPLETED, 0, status, {}, attempt,
            SLAProcessCompletion{source.input_guard(), source.guard(), true}};
}
}

TEST_CASE("Queued stale and duplicate completion never reaches the accepted attempt callback",
          "[sla_background_events]")
{
    BackgroundProcessAttempts attempts; sla::ExportGenerationOwner source; wxEvtHandler receiver;
    std::vector<uint64_t> delivered, accepted;
    receiver.Bind(EVT_TEST_PROCESS_COMPLETED, [&](SlicingProcessCompletedEvent &event) {
        delivered.push_back(event.attempt_id());
        if (!attempts.consume(event.attempt_id())) return;
        accepted.push_back(event.attempt_id());
    });
    const auto old = attempts.advance();
    auto old_event = completion(old, source);
    wxQueueEvent(&receiver, old_event.Clone());
    const auto current = attempts.advance();
    auto current_event = completion(current, source);
    wxQueueEvent(&receiver, current_event.Clone());
    wxQueueEvent(&receiver, current_event.Clone());
    wxQueueEvent(&receiver, completion(0, source).Clone());
    REQUIRE(accepted.empty()); // Queueing is not synchronous dispatch.
    drain();
    CHECK(delivered == std::vector<uint64_t>{old,current,current,0});
    CHECK(accepted == std::vector<uint64_t>{current});
    CHECK(attempts.current() == current);
}

TEST_CASE("Completion owns its source guards through queueing and detects retired source",
          "[sla_background_events]")
{
    BackgroundProcessAttempts attempts; wxEvtHandler receiver;
    auto source = std::make_unique<sla::ExportGenerationOwner>();
    std::optional<SLAProcessCompletion> delivered;
    receiver.Bind(EVT_TEST_PROCESS_COMPLETED, [&](SlicingProcessCompletedEvent &event) {
        REQUIRE(attempts.consume(event.attempt_id()));
        delivered = event.sla_completion();
    });
    {
        auto event = completion(attempts.advance(), *source);
        auto request_guard = source->guard().with_cancellation(); request_guard.cancel();
        REQUIRE(event.sla_completion()->ready_source->current()); // Not the request cancel flag.
        wxQueueEvent(&receiver, event.Clone());
    }
    source.reset(); // Queue must not dereference the destroyed producer.
    drain();
    REQUIRE(delivered); REQUIRE(delivered->ready_source);
    CHECK(delivered->export_started);
    CHECK_FALSE(delivered->input.current());
    CHECK_FALSE(delivered->ready_source->current());
    sla::ExportGenerationOwner replacement;
    CHECK_FALSE(delivered->input.same_source(replacement.guard()));
}

TEST_CASE("Nested event dispatch retires the old continuation before restoring ready UI",
          "[sla_background_events]")
{
    BackgroundProcessAttempts attempts; sla::ExportGenerationOwner source; wxEvtHandler receiver;
    const auto first = attempts.advance(); std::vector<uint64_t> accepted, continuations;
    receiver.Bind(EVT_TEST_PROCESS_COMPLETED, [&](SlicingProcessCompletedEvent &event) {
        if (!attempts.consume(event.attempt_id())) return;
        accepted.push_back(event.attempt_id());
        if (event.attempt_id() == first) {
            const auto next = attempts.advance();
            wxQueueEvent(&receiver, completion(next,source).Clone());
            drain(); // Real nested wx dispatch, as from a modal/scene callback.
        }
        if (event.attempt_id() == attempts.current()) continuations.push_back(event.attempt_id());
    });
    wxQueueEvent(&receiver, completion(first,source).Clone()); drain();
    CHECK(accepted == std::vector<uint64_t>{first,first+1});
    CHECK(continuations == std::vector<uint64_t>{first+1});
}

TEST_CASE("Incomplete processing has no ready result and new output retires a queued ready result",
          "[sla_background_events]")
{
    auto state = std::make_shared<sla::ExportGenerationState>();
    const sla::ExportInputGuard accepted(state);
    // Producing masks changes output, but not the accepted operator input.
    state->generation.fetch_add(1);
    REQUIRE(accepted.current());
    const sla::ExportGenerationGuard ready(state);
    REQUIRE(accepted.same_source(ready));
    BackgroundProcessAttempts attempts; wxEvtHandler receiver;
    std::vector<bool> has_ready, current;
    receiver.Bind(EVT_TEST_PROCESS_COMPLETED, [&](SlicingProcessCompletedEvent &event) {
        const auto &payload = event.sla_completion(); REQUIRE(payload);
        has_ready.push_back(payload->ready_source.has_value());
        current.push_back(payload->ready_source && payload->ready_source->current());
    });
    const auto attempt = attempts.advance();
    wxQueueEvent(&receiver, SlicingProcessCompletedEvent(EVT_TEST_PROCESS_COMPLETED,0,
        SlicingProcessCompletedEvent::Cancelled,{},attempt,SLAProcessCompletion{accepted,{},false}).Clone());
    wxQueueEvent(&receiver, SlicingProcessCompletedEvent(EVT_TEST_PROCESS_COMPLETED,0,
        SlicingProcessCompletedEvent::Cancelled,{},attempt,SLAProcessCompletion{accepted,ready,true}).Clone());
    state->generation.fetch_add(1); // Reproduction/replacement after queueing.
    drain();
    CHECK(has_ready == std::vector<bool>{false,true});
    CHECK(current == std::vector<bool>{false,false});
    CHECK(accepted.current()); // Output guard is necessary in addition to input.
}
