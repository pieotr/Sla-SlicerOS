#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <wx/app.h>
#include <boost/filesystem.hpp>
#include <fstream>
#include <iterator>
#include <mutex>
#include <algorithm>
#include <vector>
#include "slic3r/GUI/SlicingProcessCompletedEvent.hpp"
#include "libslic3r/SLA/ExportRequest.hpp"
#include "libslic3r/SLA/LayerDocument.hpp"
#include "libslic3r/SLAPrint.hpp"
#include "libslic3r/Format/SLAArchiveFormatRegistry.hpp"
#include "sla_print/sla_archive_test_config.hpp"

using namespace Slic3r;
namespace {
wxDEFINE_EVENT(EVT_TEST_PUBLICATION_COMPLETED, SlicingProcessCompletedEvent);
struct Directory {
    boost::filesystem::path path = boost::filesystem::unique_path("event-publication-%%%%%%%%");
    Directory() { boost::filesystem::create_directory(path); }
    ~Directory() { boost::system::error_code error; boost::filesystem::remove_all(path,error); }
};
std::vector<uint8_t> read(const boost::filesystem::path &path) {
    std::ifstream stream(path.string(),std::ios::binary);
    return {std::istreambuf_iterator<char>(stream),{}};
}
}

TEST_CASE("Real publication followed by Cancel queues Finished without changing cancellation lifecycle",
          "[sla_background_events][sla_background_publication]")
{
    const std::string format = GENERATE("SL1","cxdlpv4"); CAPTURE(format);
    Directory dir; SLAFullPrintConfig full; Test::configure_archive_fixture(full,format);
    full.set("sla_transition_layers",0); DynamicPrintConfig config; config.apply(full);
    std::vector<uint8_t> pixels(full.display_pixels_x.value*full.display_pixels_y.value,0);
    std::fill(pixels.begin(),pixels.begin()+pixels.size()/2,255);
    auto document = sla::LayerDocument::from_rasters({sla::PNGRasterEncoder{}(
        pixels.data(),full.display_pixels_x.value,full.display_pixels_y.value,1)});
    auto source = SLAPrint::create_raster_job(config,document,{.05});
    auto snapshot = std::make_shared<const sla::ExportSnapshot>(source->capture_export_snapshot());
    const auto target = dir.path/("committed."+std::string(get_archive_entry(format.c_str())->ext));
    { std::ofstream stream(target.string()); stream << "old"; }
    sla::DeferredExportRequest intent(source->export_input_guard(),format,target.string());
    const auto request = intent.bind(snapshot);
    SLAProcessCompletion payload{source->export_input_guard(),source->export_generation_guard(),true,
                                SLAExportPublication{request.path(),request.format(),false}};
    enum Step { Publication };
    PrintState<Step,1> finalization;
    // This isolated milestone owns its lock. The real worker's stop/join
    // handshake is covered separately by the diagnostic GUI scenario.
    std::mutex finalization_mutex;
    REQUIRE(finalization.set_started(Publication,finalization_mutex,[&]{source->throw_if_canceled();}));
    REQUIRE_NOTHROW(request.write());
    payload.publication->committed = true;
    const auto committed = read(target);
    REQUIRE(committed != std::vector<uint8_t>{'o','l','d'});
    REQUIRE(sla::LayerDocument::load_archive(target.string()).image(0).buf == pixels);
    intent.cancel(); source->cancel(); // The UI stop arrives after guarded rename.
    std::exception_ptr failure;
    try { finalization.set_done(Publication,finalization_mutex,[&]{source->throw_if_canceled();}); }
    catch (...) { failure = std::current_exception(); }
    REQUIRE(failure);
    CHECK(source->canceled());
    CHECK_FALSE(finalization.is_done(Publication,finalization_mutex));
    CHECK_THROWS_AS(std::rethrow_exception(failure),CanceledException);
    BackgroundProcessAttempts attempts; const auto attempt = attempts.advance();
    wxEvtHandler receiver; std::optional<SlicingProcessCompletedEvent> delivered;
    receiver.Bind(EVT_TEST_PUBLICATION_COMPLETED,[&](SlicingProcessCompletedEvent &event) {
        REQUIRE(attempts.consume(event.attempt_id())); delivered.emplace(event);
    });
    {
        SlicingProcessCompletedEvent event(EVT_TEST_PUBLICATION_COMPLETED,0,
            SlicingProcessCompletedEvent::Cancelled,failure,attempt,payload);
        wxQueueEvent(&receiver,event.Clone());
    }
    payload.publication->path = "unrelated-new-output.sl1";
    wxAppConsole::GetInstance()->ProcessPendingEvents();
    REQUIRE(delivered); CHECK(delivered->success()); CHECK_FALSE(delivered->cancelled());
    REQUIRE(delivered->published_export());
    CHECK(delivered->published_export()->path == target.string());
    CHECK(delivered->published_export()->format == format);
    CHECK(source->canceled()); // Event outcome did not rewrite the stop/join signal.
    CHECK(read(target) == committed);
    CHECK(std::distance(boost::filesystem::directory_iterator(dir.path),boost::filesystem::directory_iterator{}) == 1);
}

TEST_CASE("Publication outcome never hides another error or turns a prepared destination into success",
          "[sla_background_events][sla_background_publication]")
{
    sla::ExportGenerationOwner source;
    SLAProcessCompletion payload{source.input_guard(),source.guard(),true,
                                SLAExportPublication{"owned.sl1","SL1",false}};
    const auto cancellation = std::make_exception_ptr(CanceledException{});
    SlicingProcessCompletedEvent uncommitted(EVT_TEST_PUBLICATION_COMPLETED,0,
        SlicingProcessCompletedEvent::Cancelled,cancellation,1,payload);
    CHECK(uncommitted.cancelled()); CHECK_FALSE(uncommitted.published_export());
    payload.publication->committed = true;
    SlicingProcessCompletedEvent late_without_exception(EVT_TEST_PUBLICATION_COMPLETED,0,
        SlicingProcessCompletedEvent::Cancelled,{},1,payload);
    CHECK(late_without_exception.success());
    const auto real_error = std::make_exception_ptr(RuntimeError("after publication: reporting failed"));
    SlicingProcessCompletedEvent failed(EVT_TEST_PUBLICATION_COMPLETED,0,
        SlicingProcessCompletedEvent::Cancelled,real_error,1,payload);
    CHECK(failed.error()); CHECK_FALSE(failed.success()); REQUIRE(failed.published_export());
    CHECK_THROWS_AS(failed.rethrow_exception(),RuntimeError);
    // Old success is still rejected by the same gate before Plater::stop().
    BackgroundProcessAttempts attempts; const auto old = attempts.advance();
    SlicingProcessCompletedEvent published(EVT_TEST_PUBLICATION_COMPLETED,0,
        SlicingProcessCompletedEvent::Cancelled,cancellation,old,payload);
    attempts.advance(); CHECK(published.success()); CHECK_FALSE(attempts.consume(published.attempt_id()));
}
