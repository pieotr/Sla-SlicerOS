// SPDX-License-Identifier: AGPL-3.0-or-later
// Opt-in regression: consume genuine pre-fix projects, never synthetic downgrades.
#include "libslic3r/SLAPrint.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/SLA/CalibrationRaster.hpp"
#include "libslic3r/SLA/ProjectArchive.hpp"
#include "libslic3r/Format/ZipperArchiveImport.hpp"
#include <nlohmann/json.hpp>
#include <boost/filesystem.hpp>
#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iterator>
using namespace Slic3r;
namespace {
std::string pm4n_bytes(const boost::filesystem::path &path) {
    std::ifstream stream(path.string(),std::ios::binary);REQUIRE(stream.good());
    return {std::istreambuf_iterator<char>(stream),{}};
}
nlohmann::json pm4n_metadata(const boost::filesystem::path &path) {
    const auto zip=read_zipper_archive(path.string(),{"metadata/sla/layers.json"},{});
    REQUIRE(zip.entries.size()==1);return nlohmann::json::parse(zip.entries.front().buf);
}
}
TEST_CASE("Actual pre-fix PM4n projects retain all Gray8 masks settings and historical recipes",
          "[.sla_pm4n_legacy_reopen]") {
    const char *input=std::getenv("SLA_PM4N_LEGACY_INPUT");
    const char *output=std::getenv("SLA_PM4N_LEGACY_OUTPUT");
    REQUIRE(input!=nullptr);REQUIRE(output!=nullptr);
    size_t total=0;
    for(const std::string producer:{"calibration","model","pcb"}) {
        const auto root=boost::filesystem::path(input)/producer;
        std::vector<boost::filesystem::path> folders;
        for(const auto &entry:boost::filesystem::directory_iterator(root))
            if(boost::filesystem::is_directory(entry.path()) && entry.path().filename().string().find("516-")!=0)
                folders.push_back(entry.path());
        std::sort(folders.begin(),folders.end());REQUIRE(folders.size()==(producer=="pcb"?16:4));
        for(const auto &folder:folders)for(const std::string kind:{"source","edited"}) {
            CAPTURE(producer,folder.string(),kind);++total;
            const auto path=folder/(kind+".sla-project");
            const auto original=pm4n_bytes(path),ini=pm4n_bytes(folder/"source.ini");
            const auto effective=producer=="pcb"?pm4n_bytes(folder/"effective.ini"):std::string{};
            const auto metadata=pm4n_metadata(path);
            REQUIRE(metadata.at("version")== (producer=="calibration"?6:2));
            REQUIRE_FALSE(metadata.at("document").contains("fresh_job"));
            REQUIRE_FALSE(metadata.at("document").contains("mask_contract"));
            REQUIRE(metadata.contains("calibration_recipe")== (producer=="calibration"));
            if(producer=="calibration") {
                CHECK(metadata.at("calibration_recipe").at("precision")==0);
                CHECK(metadata.at("calibration_recipe").at("requested")==nlohmann::json::array({16,47}));
                CHECK(metadata.at("calibration_recipe").at("effective")==nlohmann::json::array({16,47}));
            }
            const auto state=sla::read_project_layers(path.string());REQUIRE(state);REQUIRE(state->settings);
            const bool model_source=producer=="model";
            REQUIRE(state->origin==(model_source?sla::ProjectLayerOrigin::ModelSlice:sla::ProjectLayerOrigin::GeneratedRaster));
            REQUIRE(state->source_fingerprint.empty()==!model_source);
            REQUIRE_FALSE(state->document.fresh_job());REQUIRE_FALSE(state->document.mask_contract_precision());
            REQUIRE(state->document.parameter_capabilities().empty());
            std::vector<std::vector<uint8_t>> masks;bool noncanonical=false;
            for(size_t i=0;i<state->document.layer_count();++i) {
                masks.push_back(state->document.image(i).buf);
                for(uint8_t value:masks.back())noncanonical|=value%17!=0;
            }
            REQUIRE(noncanonical);
            if(kind=="edited") {
                REQUIRE(masks[0].size()>=256);
                for(unsigned value=0;value<256;++value)CHECK(masks[0][value]==uint8_t(value));
                CHECK(state->document.image(0).get(2,2)==128);CHECK(state->document.image(0).get(2,3)==127);
            }
            DynamicPrintConfig config;config.load_from_ini((folder/"source.ini").string(),ForwardCompatibilitySubstitutionRule::Disable);
            CHECK(config.opt_string("sla_archive_format")=="pm4n");
            CHECK(config.opt_string("sla_anycubic_version")== (folder.filename().string().find("auto-")==0?"auto":"515"));
            Model model;
            std::unique_ptr<SLAPrint> restored;
            if(model_source) {
                auto *object=model.add_object("Own rotated precision fixture","",make_cube(4.137,2.313,.15));
                auto *instance=object->add_instance();instance->set_rotation(Vec3d(0.,0.,.371));instance->set_offset(Vec3d(10.,10.,0.));
                REQUIRE(sla::raster_source_fingerprint(model,config)==state->source_fingerprint);
                restored=std::make_unique<SLAPrint>();restored->apply(model,config);
                restored->queue_project_layers(std::make_shared<const sla::ProjectRasterState>(*state));restored->process();
                REQUIRE_FALSE(restored->project_layer_restore_failed());
            } else restored=sla::restore_generated_project(*state);
            REQUIRE(restored);REQUIRE(restored->layer_document());const auto &document=*restored->layer_document();
            CHECK_FALSE(document.fresh_job());CHECK_FALSE(document.mask_contract_precision());
            CHECK(document.parameter_capabilities().empty());CHECK_FALSE(document.dirty());
            CHECK_FALSE(document.can_undo());CHECK_FALSE(document.can_redo());
            CHECK(bool(restored->calibration_recipe())== (producer=="calibration"));
            for(size_t i=0;i<masks.size();++i)CHECK(bool(document.image(i).buf==masks[i]));
            auto editable=document;editable.paint(0,2,2,0,128);CHECK(editable.image(0).get(2,2)==128);
            const auto recaptured=model_source?sla::capture_project_layers(model,config,*restored):
                sla::capture_generated_project(*restored,document);
            CHECK(recaptured.source_fingerprint==state->source_fingerprint);CHECK(bool(recaptured.positions_z==state->positions_z));
            const auto destination=boost::filesystem::path(output)/producer/folder.filename()/(kind+".sla-project");
            boost::filesystem::create_directories(destination.parent_path());REQUIRE_FALSE(boost::filesystem::exists(destination));
            sla::save_layer_project(destination.string(),recaptured);
            CHECK(bool(pm4n_metadata(destination)==metadata));
            const auto reopened=sla::read_project_layers(destination.string());REQUIRE(reopened);
            for(size_t i=0;i<masks.size();++i)CHECK(bool(reopened->document.image(i).buf==masks[i]));
            CHECK(pm4n_bytes(path)==original);CHECK(pm4n_bytes(folder/"source.ini")==ini);
            if(producer=="pcb")CHECK(pm4n_bytes(folder/"effective.ini")==effective);
        }
    }
    CHECK(total==48);
}
