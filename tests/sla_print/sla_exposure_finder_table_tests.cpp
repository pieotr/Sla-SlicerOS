// SPDX-License-Identifier: AGPL-3.0-or-later
#include "libslic3r/SLA/ExposureFinderTable.hpp"
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "libslic3r/Exception.hpp"
#include "libslic3r/SLA/CalibrationRaster.hpp"
#include "libslic3r/SLA/ProjectArchive.hpp"
#include "libslic3r/Format/ZipperArchiveImport.hpp"
#include "libslic3r/Zipper.hpp"
#include "libslic3r/SLA/ExportSnapshot.hpp"
#include <boost/filesystem.hpp>
#include <nlohmann/json.hpp>
#include <cfenv>
#include <limits>
#include <cstdlib>
#include <fstream>
#include <set>
using namespace Slic3r::sla;
TEST_CASE("ExposureFinder linear tables preserve base and ordered tests", "[sla_exposure_finder_table]") {
    ExposureFinderTableParameters p;
    const auto rows=generate_exposure_finder_table(p);
    REQUIRE(rows.size()==5);
    for(size_t i=0;i<rows.size();++i) {
        CHECK(rows[i].layer_height==.05);CHECK(rows[i].bottom_exposure==15.);
        CHECK(rows[i].normal_exposure==Catch::Approx(2.+i*.2));CHECK(rows[i].valid());
    }
    p.include_base=false;CHECK(generate_exposure_finder_table(p).size()==4);
    p.normal_step=0.;p.bottom_step=0.;CHECK(generate_exposure_finder_table(p).size()==1);
}
TEST_CASE("ExposureFinder multiplier includes height and decimal even rounding", "[sla_exposure_finder_table]") {
    ExposureFinderTableParameters p;p.series=ExposureFinderSeries::Multiplier;
    p.normal_exposure=2.;p.normal_step=.5;p.bottom_exposure=15.;p.bottom_step=.1;
    const auto rows=generate_exposure_finder_table(p);
    REQUIRE(rows.size()==5);
    CHECK(rows[1].normal_exposure==2.05);CHECK(rows[1].bottom_exposure==15.08);
    CHECK(rows[3].bottom_exposure==15.22); // 15.225 -> ties to even, not 15.23.
    p.series=ExposureFinderSeries::Linear;p.normal_exposure=2.675;
    p.normal_step=.005;p.tests=1;
    const auto tied=generate_exposure_finder_table(p);
    REQUIRE(tied.size()==2);CHECK(tied[0].normal_exposure==2.68);
    CHECK(tied[1].normal_exposure==2.68); // Bottom step still distinguishes the row.
}

TEST_CASE("ExposureFinder manual table normalizes sorts and retains invalid rows", "[sla_exposure_finder_table]") {
    const std::vector<ExposureFinderTableRow> input{{.1,15,2,255},{.05,15.225,2.675,255},
        {.0504,15.22,2.68,255},{.05,15.22,2.68,0},{-.0005,0,-.005,255}};
    const auto saved=input,rows=sanitize_exposure_finder_table(input);
    REQUIRE(rows.size()==4);CHECK(input==saved);
    CHECK(rows[0].layer_height==-.001);CHECK_FALSE(rows[0].valid());
    CHECK(rows[1].brightness==0);CHECK_FALSE(rows[1].valid());
    CHECK(rows[2].normal_exposure==2.68);CHECK(rows[2].bottom_exposure==15.22);
    CHECK(rows[3].layer_height==.1);CHECK(rows[3].valid());
    CHECK(sanitize_exposure_finder_table(rows)==rows);
}
TEST_CASE("ExposureFinder table cancellation and resource bounds are transactional", "[sla_exposure_finder_table]") {
    ExposureFinderTableParameters p;p.multiple_layer_heights=true;p.maximum_layer_height=.09;
    const auto expected=generate_exposure_finder_table(p);
    REQUIRE(expected.size()==25);
    for(const size_t stop:{size_t(0),size_t(1),size_t(25)}) {
        bool reached=false;
        CHECK_THROWS_AS(generate_exposure_finder_table(p,[&](size_t done,size_t total){
            if(total==25 && done==stop){reached=true;return false;}return true;
        }),LayerOperationCancelled);
        CHECK(reached);CHECK(generate_exposure_finder_table(p)==expected);
    }
    const std::vector<ExposureFinderTableRow> manual{{.1,15,2},{.05,15,2},{.05,15,2}};
    for(const size_t stop:{size_t(0),size_t(1),size_t(3)}) {
        CHECK_THROWS_AS(sanitize_exposure_finder_table(manual,[&](size_t done,size_t total){return total!=3 || done!=stop;}),LayerOperationCancelled);
        CHECK(manual[0].layer_height==.1);
    }
    for(const double invalid:{0.,-1.,std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()}) {
        auto bad=p;bad.layer_height_step=invalid;
        CHECK_THROWS_AS(generate_exposure_finder_table(bad),Slic3r::RuntimeError);
    }
    p.layer_height_step=.000000001;
    CHECK_THROWS_AS(generate_exposure_finder_table(p),Slic3r::RuntimeError);
    p={};p.tests=256;CHECK_THROWS_AS(generate_exposure_finder_table(p),Slic3r::RuntimeError);
    p={};p.series=static_cast<ExposureFinderSeries>(99);CHECK_THROWS_AS(generate_exposure_finder_table(p),Slic3r::RuntimeError);
    p={};p.normal_exposure=std::numeric_limits<double>::quiet_NaN();CHECK_THROWS_AS(generate_exposure_finder_table(p),Slic3r::RuntimeError);
    p={};p.layer_height=.0004;CHECK_THROWS_AS(generate_exposure_finder_table(p),Slic3r::RuntimeError);
    p={};p.normal_exposure=1000000.;p.normal_step=1.;CHECK_THROWS_AS(generate_exposure_finder_table(p),Slic3r::RuntimeError);
    CHECK_THROWS_AS(sanitize_exposure_finder_table(std::vector<ExposureFinderTableRow>(100001)),Slic3r::RuntimeError);
}
TEST_CASE("ExposureFinder decimal output restores caller rounding and exception flags", "[sla_exposure_finder_table]") {
    struct Restore {std::fenv_t state;Restore(){std::fegetenv(&state);}~Restore(){std::fesetenv(&state);}} restore;
    ExposureFinderTableParameters p;p.normal_exposure=2.675;
    const auto expected=generate_exposure_finder_table(p);
    for(const int mode:{FE_UPWARD,FE_DOWNWARD,FE_TOWARDZERO}) {
        REQUIRE(std::fesetround(mode)==0);std::feclearexcept(FE_ALL_EXCEPT);std::feraiseexcept(FE_INVALID);
        CHECK(generate_exposure_finder_table(p)==expected);
        CHECK(std::fegetround()==mode);CHECK(std::fetestexcept(FE_ALL_EXCEPT)==FE_INVALID);
    }
}
TEST_CASE("ExposureFinder tables match independent reference fixtures", "[sla_exposure_finder_table]") {
    using nlohmann::json;
    json cases=json::array();
    auto record=[&](const ExposureFinderTableParameters &p){
        const auto rows=generate_exposure_finder_table(p);
        json expected=json::array();
        for(const auto &row:rows)expected.push_back({{"layer_height",row.layer_height},{"bottom_exposure",row.bottom_exposure},
            {"normal_exposure",row.normal_exposure},{"brightness",row.brightness},{"valid",row.valid()}});
        cases.push_back({{"name",std::to_string(cases.size())},{"parameters",{
            {"LayerHeight",p.layer_height},{"MultipleLayerHeightMaximum",p.maximum_layer_height},
            {"MultipleLayerHeightStep",p.layer_height_step},{"MultipleLayerHeight",p.multiple_layer_heights},
            {"BottomExposure",p.bottom_exposure},{"NormalExposure",p.normal_exposure},
            {"ExposureGenBottomStep",p.bottom_step},{"ExposureGenNormalStep",p.normal_step},
            {"ExposureGenTests",p.tests},{"ExposureGenIgnoreBaseExposure",!p.include_base},{"ExposureGenType",int(p.series)}}},
            {"rows",expected}});
        CHECK(rows.size()<=100000);
    };
    for(double height:{.001,.025,.05,.1234})for(bool multiple:{false,true})for(bool base:{false,true})
        for(auto series:{ExposureFinderSeries::Linear,ExposureFinderSeries::Multiplier})
            for(unsigned tests:{0u,1u,4u})for(double step:{.005,.2,-.2}) {
                ExposureFinderTableParameters p;p.layer_height=height;p.maximum_layer_height=.2;p.layer_height_step=.025;
                p.multiple_layer_heights=multiple;p.include_base=base;p.series=series;p.tests=tests;
                p.bottom_exposure=15.225;p.normal_exposure=2.675;p.bottom_step=.1;p.normal_step=step;record(p);
            }
    for(bool base:{false,true}) {
        ExposureFinderTableParameters p;p.tests=255;p.include_base=base;record(p);
        p.tests=4;p.maximum_layer_height=.049;p.multiple_layer_heights=true;record(p);
        p.maximum_layer_height=.052;p.layer_height_step=.0004;p.series=ExposureFinderSeries::Multiplier;record(p);
        p.normal_step=-2.;p.bottom_step=-15.;p.series=ExposureFinderSeries::Linear;record(p);
    }
    const std::vector<ExposureFinderTableRow> manual{{.1,15,2,255},{.05,15.225,2.675,255},
        {.0504,15.22,2.68,255},{.05,15.22,2.68,0},{-.0005,0,-.005,255}};
    json input=json::array(),output=json::array();
    for(const auto &row:manual)input.push_back({{"layer_height",row.layer_height},{"bottom_exposure",row.bottom_exposure},
        {"normal_exposure",row.normal_exposure},{"brightness",row.brightness}});
    for(const auto &row:sanitize_exposure_finder_table(manual))output.push_back({{"layer_height",row.layer_height},
        {"bottom_exposure",row.bottom_exposure},{"normal_exposure",row.normal_exposure},{"brightness",row.brightness},{"valid",row.valid()}});
    cases.push_back({{"name","manual"},{"manual",input},{"rows",output}});
    CHECK(cases.size()==297);
    if(const char *path=std::getenv("SLA_EXPOSURE_TABLE_FIXTURES")) {
        std::ofstream file(path);REQUIRE(file.good());file<<cases.dump(2)<<'\n';REQUIRE(file.good());
    }
}

TEST_CASE("ExposureFinder progress cannot mutate its captured table inputs", "[sla_exposure_finder_table]") {
    ExposureFinderTableParameters draft;
    const auto expected=generate_exposure_finder_table(draft);
    const auto actual=generate_exposure_finder_table(draft,[&](size_t,size_t){
        draft.tests=255;draft.normal_step=100.;return true;
    });
    CHECK(actual==expected);
    std::vector<ExposureFinderTableRow> manual{{.1,15,2},{.05,15,2}};
    const auto sorted=sanitize_exposure_finder_table(manual);
    const auto frozen=sanitize_exposure_finder_table(manual,[&](size_t,size_t){manual.clear();return true;});
    CHECK(frozen==sorted);
}

TEST_CASE("ExposureFinder preserves reference base rows at sub-micron height steps", "[sla_exposure_finder_table]") {
    ExposureFinderTableParameters p;p.multiple_layer_heights=true;
    p.maximum_layer_height=.052;p.layer_height_step=.0004;p.series=ExposureFinderSeries::Multiplier;
    const auto rows=generate_exposure_finder_table(p);
    REQUIRE(rows.size()==18);
    CHECK(rows[0]==rows[5]);CHECK(rows[6]==rows[11]);CHECK(rows[12]==rows[17]);
    CHECK(sanitize_exposure_finder_table(rows).size()==15);
    p.include_base=false;CHECK(generate_exposure_finder_table(p).size()==12);
}

TEST_CASE("ExposureFinder recipe schema retains its table and edited masks", "[sla_exposure_finder_recipe]") {
    Slic3r::DynamicPrintConfig config;config.apply(Slic3r::SLAFullPrintConfig{});
    config.option("printer_technology")->setInt(Slic3r::ptSLA);
    config.set("sla_archive_format","uvj");config.set("display_pixels_x",80);config.set("display_pixels_y",40);
    config.set("display_width",100.);config.set("display_height",60.);
    config.set("layer_height",.05);config.set("initial_layer_height",.05);
    Slic3r::sla::CalibrationParameters parameters;parameters.size_x=parameters.size_y=2.;parameters.margin=1.;parameters.height=.1;
    auto job=Slic3r::sla::create_calibration(config,parameters);
    auto edited=*job->layer_document();edited.invert(0);job->replace_layer_document(edited);
    const auto before=job->layer_document()->image(0).buf;
    const char *fixtures=std::getenv("SLA_EXPOSURE_RECIPE_FIXTURES");
    struct Directory {
        boost::filesystem::path path;bool keep;
        ~Directory(){if(!keep){boost::system::error_code error;boost::filesystem::remove_all(path,error);}}
    } directory{fixtures?boost::filesystem::path(fixtures):boost::filesystem::temp_directory_path()/boost::filesystem::unique_path("finder-recipe-%%%%"),fixtures!=nullptr};
    boost::filesystem::create_directories(directory.path);
    const auto base=directory.path/"base.sla-project",source=directory.path/"finder.sla-project";
    Slic3r::sla::save_layer_project(base.string(),Slic3r::sla::capture_generated_project(*job,*job->layer_document()));
    auto entries=Slic3r::read_zipper_archive(base.string(),{"metadata/sla/"},{}).entries;
    const auto entry=std::find_if(entries.begin(),entries.end(),[](const auto &e){return e.fname=="metadata/sla/layers.json";});
    REQUIRE(entry!=entries.end());auto metadata=nlohmann::json::parse(entry->buf);
    auto &recipe=metadata["calibration_recipe"];recipe["version"]=3;recipe["pattern"]=3;
    nlohmann::json rows=nlohmann::json::array();
    for(double exposure:{2.,2.2,2.4,2.6,2.8})rows.push_back({{"layer_height",.05},{"bottom_exposure",15.},{"normal_exposure",exposure},{"brightness",255}});
    recipe["exposure_finder"]={{"version",1},{"table_origin",0},{"parameters",{
        {"layer_height",.05},{"maximum_layer_height",.1},{"layer_height_step",.01},
        {"bottom_exposure",15.},{"normal_exposure",2.},{"bottom_step",0.},{"normal_step",.2},
        {"tests",4},{"multiple_layer_heights",false},{"include_base",true},{"series",0}}},{"rows",rows}};
    const auto text=metadata.dump();entry->buf.assign(text.begin(),text.end());
    {Slic3r::Zipper zip(source.string());for(const auto &e:entries)zip.add_entry(e.fname,e.buf.data(),e.buf.size());zip.finalize();}
    std::optional<Slic3r::sla::ProjectRasterState> state;
    REQUIRE_NOTHROW(state=Slic3r::sla::read_project_layers(source.string()));REQUIRE(state);
    REQUIRE(state->calibration_recipe);CHECK(int(state->calibration_recipe->parameters.pattern)==3);
    CHECK(state->document.image(0).buf==before);
    const auto restored=Slic3r::sla::restore_generated_project(*state);
    CHECK(restored->layer_document()->image(0).buf==before);
}

namespace {
std::unique_ptr<Slic3r::SLAPrint> finder_recipe_job() {
    Slic3r::DynamicPrintConfig config;config.apply(Slic3r::SLAFullPrintConfig{});
    config.option("printer_technology")->setInt(Slic3r::ptSLA);
    config.set("sla_archive_format","uvj");config.set("display_pixels_x",80);config.set("display_pixels_y",40);
    config.set("display_width",100.);config.set("display_height",60.);
    config.set("layer_height",.05);config.set("initial_layer_height",.05);
    Slic3r::sla::CalibrationParameters parameters;parameters.size_x=parameters.size_y=2.;parameters.margin=1.;parameters.height=.1;
    auto job=Slic3r::sla::create_calibration(config,parameters);
    auto table=std::make_shared<ExposureFinderRecipe>();table->rows=generate_exposure_finder_table(table->table_parameters);
    auto recipe=std::make_shared<Slic3r::sla::CalibrationRecipe>(*job->calibration_recipe());
    recipe->parameters.pattern=Slic3r::sla::CalibrationPattern::ExposureFinder;recipe->exposure_finder=table;
    job->set_calibration_recipe(recipe);
    return job;
}
struct FinderDirectory {
    boost::filesystem::path path;
    bool keep;
    explicit FinderDirectory(const char *name) {
        const char *fixtures=std::getenv("SLA_EXPOSURE_RECIPE_FIXTURES");keep=fixtures!=nullptr;
        path=fixtures?boost::filesystem::path(fixtures)/name:boost::filesystem::temp_directory_path()/boost::filesystem::unique_path("finder-owned-%%%%");
        boost::filesystem::create_directories(path);
    }
    ~FinderDirectory(){if(!keep){boost::system::error_code error;boost::filesystem::remove_all(path,error);}}
};
}
TEST_CASE("ExposureFinder owned recipe survives alias edits manual order and project restart", "[sla_exposure_finder_recipe]") {
    using namespace Slic3r;
    auto job=finder_recipe_job();FinderDirectory folder("typed");
    auto table=std::make_shared<ExposureFinderRecipe>(*job->calibration_recipe()->exposure_finder);
    auto recipe=std::make_shared<sla::CalibrationRecipe>(*job->calibration_recipe());recipe->exposure_finder=table;
    job->set_calibration_recipe(recipe);
    const auto guard=job->export_generation_guard();
    const auto snapshot=job->capture_export_snapshot();
    CHECK(job->calibration_recipe()->exposure_finder.get()!=table.get());
    table->rows[0].normal_exposure=99.;recipe->parameters.size_x=999.;
    CHECK(job->calibration_recipe()->exposure_finder->rows[0].normal_exposure==2.);
    CHECK(snapshot.calibration_recipe()->exposure_finder->rows[0].normal_exposure==2.);
    CHECK(job->calibration_recipe()->parameters.size_x==2.);
    CHECK_THROWS_AS(job->set_calibration_recipe(recipe),RuntimeError);CHECK(guard.current());
    auto edited=*job->layer_document();edited.invert(0);job->replace_layer_document(edited);
    const auto before=job->layer_document()->image(0).buf;
    const auto original_rows=job->layer_document()->fresh_job()->rows;
    const auto project=folder.path/"owned.sla-project";
    sla::save_layer_project(project.string(),sla::capture_generated_project(*job,*job->layer_document()));
    const auto state=sla::read_project_layers(project.string());REQUIRE(state);
    REQUIRE(state->calibration_recipe->exposure_finder);
    CHECK(state->calibration_recipe->exposure_finder->rows==generate_exposure_finder_table({}));
    auto restored=sla::restore_generated_project(*state);
    CHECK(restored->layer_document()->image(0).buf==before);
    CHECK(restored->layer_document()->fresh_job()->rows==original_rows);
    REQUIRE_NOTHROW(restored->export_print((folder.path/"owned.uvj").string()));
    // Deliberate manual order, duplicate rows and an invalid draft row survive.
    auto manual=std::make_shared<ExposureFinderRecipe>();manual->table_origin=ExposureFinderTableOrigin::Manual;
    manual->rows={{.1,20,3,127},{.05,15,2,255},{.05,15,2,255},{.05,0,-1,0}};
    auto manual_recipe=std::make_shared<sla::CalibrationRecipe>(*restored->calibration_recipe());manual_recipe->exposure_finder=manual;
    restored->set_calibration_recipe(manual_recipe);
    const auto manual_path=folder.path/"manual.sla-project";
    sla::save_layer_project(manual_path.string(),sla::capture_generated_project(*restored,*restored->layer_document()));
    const auto manual_state=sla::read_project_layers(manual_path.string());REQUIRE(manual_state);
    CHECK(manual_state->calibration_recipe->exposure_finder->rows==manual->rows);
    CHECK(manual_state->document.image(0).buf==before);
    CHECK(manual_state->calibration_recipe->exposure_finder->table_origin==ExposureFinderTableOrigin::Manual);
    auto restarted=sla::restore_generated_project(*manual_state);
    REQUIRE_NOTHROW(restarted->export_print((folder.path/"manual.uvj").string()));
    auto invalid=*manual_state;
    auto bad=std::make_shared<sla::CalibrationRecipe>(*manual_recipe);bad->parameters.pattern=sla::CalibrationPattern::XYZBlocks;
    invalid.calibration_recipe=bad;
    const auto target=folder.path/"keep.sla-project";{std::ofstream out(target.string());out<<"keep";}
    CHECK_THROWS_AS(sla::save_layer_project(target.string(),invalid),RuntimeError);
    {std::ifstream in(target.string());std::string value;in>>value;CHECK(value=="keep");}
    CHECK_THROWS_AS(sla::create_calibration(job->full_print_config(),manual_recipe->parameters),RuntimeError);
}
TEST_CASE("ExposureFinder recipe rejects malformed downgraded or contradictory projects", "[sla_exposure_finder_recipe]") {
    using namespace Slic3r;using Json=nlohmann::json;
    auto job=finder_recipe_job();FinderDirectory folder("hostile");const auto good=folder.path/"good.sla-project";
    sla::save_layer_project(good.string(),sla::capture_generated_project(*job,*job->layer_document()));
    const auto source=read_zipper_archive(good.string(),{"metadata/sla/"},{}).entries;
    const std::vector<std::pair<std::string,std::function<void(Json &)>>> mutations{
        {"root-schema",[](auto &r){r["version"]=1;}},
        {"root-kind",[](auto &r){r["pattern"]=0;}},
        {"missing-finder",[](auto &r){r.erase("exposure_finder");}},
        {"extra",[](auto &r){r["exposure_finder"]["extra"]=1;}},
        {"finder-schema",[](auto &r){r["exposure_finder"]["version"]=2;}},
        {"finder-fraction",[](auto &r){r["exposure_finder"]["version"]=1.0;}},
        {"origin",[](auto &r){r["exposure_finder"]["table_origin"]=2;}},
        {"negative-origin",[](auto &r){r["exposure_finder"]["table_origin"]=-1;}},
        {"bool-origin",[](auto &r){r["exposure_finder"]["table_origin"]=true;}},
        {"fraction-origin",[](auto &r){r["exposure_finder"]["table_origin"]=.5;}},
        {"missing-parameters",[](auto &r){r["exposure_finder"].erase("parameters");}},
        {"extra-parameters",[](auto &r){r["exposure_finder"]["parameters"]["extra"]=1;}},
        {"missing-step",[](auto &r){r["exposure_finder"]["parameters"].erase("normal_step");}},
        {"bool-as-int",[](auto &r){r["exposure_finder"]["parameters"]["include_base"]=1;}},
        {"invalid-series",[](auto &r){r["exposure_finder"]["parameters"]["series"]=2;}},
        {"negative-series",[](auto &r){r["exposure_finder"]["parameters"]["series"]=-1;}},
        {"fraction-series",[](auto &r){r["exposure_finder"]["parameters"]["series"]=.5;}},
        {"bool-tests",[](auto &r){r["exposure_finder"]["parameters"]["tests"]=true;}},
        {"test-overflow",[](auto &r){r["exposure_finder"]["parameters"]["tests"]=256;}},
        {"fraction-tests",[](auto &r){r["exposure_finder"]["parameters"]["tests"]=4.0;}},
        {"zero-step",[](auto &r){r["exposure_finder"]["parameters"]["layer_height_step"]=0;}},
        {"negative-step",[](auto &r){r["exposure_finder"]["parameters"]["layer_height_step"]=-.01;}},
        {"string-number",[](auto &r){r["exposure_finder"]["parameters"]["normal_exposure"]="2";}},
        {"huge-number",[](auto &r){r["exposure_finder"]["parameters"]["normal_exposure"]=1e200;}},
        {"null-number",[](auto &r){r["exposure_finder"]["parameters"]["normal_exposure"]=nullptr;}},
        {"rows-object",[](auto &r){r["exposure_finder"]["rows"]=Json::object();}},
        {"empty-rows",[](auto &r){r["exposure_finder"]["rows"]=Json::array();}},
        {"long-rows",[](auto &r){const auto row=r["exposure_finder"]["rows"][0];r["exposure_finder"]["rows"]=Json::array();for(size_t i=0;i<100001;++i)r["exposure_finder"]["rows"].push_back(row);}},
        {"extra-row-field",[](auto &r){r["exposure_finder"]["rows"][0]["extra"]=1;}},
        {"missing-row-field",[](auto &r){r["exposure_finder"]["rows"][0].erase("normal_exposure");}},
        {"brightness-overflow",[](auto &r){r["exposure_finder"]["rows"][0]["brightness"]=256;}},
        {"brightness-negative",[](auto &r){r["exposure_finder"]["rows"][0]["brightness"]=-1;}},
        {"brightness-fraction",[](auto &r){r["exposure_finder"]["rows"][0]["brightness"]=1.5;}},
        {"brightness-bool",[](auto &r){r["exposure_finder"]["rows"][0]["brightness"]=true;}},
        {"noncanonical-height",[](auto &r){r["exposure_finder"]["rows"][0]["layer_height"]=.0504;}},
        {"stale-auto-row",[](auto &r){r["exposure_finder"]["rows"][0]["normal_exposure"]=2.01;}},
        {"short-auto",[](auto &r){r["exposure_finder"]["rows"].erase(0);}},
        {"duplicate-auto",[](auto &r){r["exposure_finder"]["rows"].push_back(r["exposure_finder"]["rows"][0]);}},
        {"no-valid-manual",[](auto &r){r["exposure_finder"]["table_origin"]=1;for(auto &row:r["exposure_finder"]["rows"])row["normal_exposure"]=0;}},
        {"legacy-taper",[](auto &r){r["lift_taper_step"]=10;}}
    };
    for(const auto &[name,mutate]:mutations) {
        INFO(name);auto entries=source;
        const auto manifest=std::find_if(entries.begin(),entries.end(),[](const auto &e){return e.fname=="metadata/sla/layers.json";});
        REQUIRE(manifest!=entries.end());auto metadata=Json::parse(manifest->buf);mutate(metadata["calibration_recipe"]);
        const auto text=metadata.dump();manifest->buf.assign(text.begin(),text.end());const auto bad=folder.path/(name+".sla-project");
        {Zipper zip(bad.string());for(const auto &e:entries)zip.add_entry(e.fname,e.buf.data(),e.buf.size());zip.finalize();}
        CHECK_THROWS(sla::read_project_layers(bad.string()));REQUIRE(sla::read_project_layers(good.string()));
    }
}

TEST_CASE("ExposureFinder physical schedule preserves bottom base and feature tests", "[sla_exposure_finder_schedule]") {
    ExposureFinderRecipe recipe;recipe.rows=generate_exposure_finder_table(recipe.table_parameters);
    ExposureFinderScheduleParameters p;p.bottom_layers=1;
    const auto plan=plan_exposure_finder_schedule(recipe,p);
    CHECK(plan.physical_layers==4);CHECK(plan.bottom_records==1);
    REQUIRE(plan.exposures.size()==16);
    CHECK(plan.exposures[0].position_z==.05);CHECK(plan.exposures[0].exposure==15.);
    CHECK(plan.exposures[0].parts.size()==5);
    CHECK(plan.exposures[1].position_z==.1);CHECK(plan.exposures[1].exposure==2.);
    CHECK(plan.exposures.back().position_z==.2);CHECK(plan.exposures.back().exposure==2.8);
}

namespace {
nlohmann::json finder_schedule_view(const ExposureFinderSchedule &plan) {
    using nlohmann::json;json rows=json::array();
    for(const auto &exposure:plan.exposures) {
        std::set<int> brightness(exposure.mask_brightness.begin(),exposure.mask_brightness.end());
        rows.push_back({{"position_z",exposure.position_z},{"exposure",exposure.exposure},
            {"brightness",brightness},{"lift_height",exposure.lift_height_override.value_or(6.)},
            {"wait_before",exposure.wait_before_override.value_or(1.)}});
    }
    return {{"bottom_records",plan.bottom_records},{"physical_layers",plan.physical_layers},{"exposures",rows}};
}
}
TEST_CASE("ExposureFinder brightness modifies masks and repeated settings follow the first record", "[sla_exposure_finder_schedule]") {
    ExposureFinderRecipe recipe;recipe.table_parameters.tests=1;recipe.rows=generate_exposure_finder_table(recipe.table_parameters);
    ExposureFinderScheduleParameters p;p.bottom_layers=1;p.multiple_brightness=true;p.brightness={127,255,127,0};
    p.repeated_settings=true;p.repeated_lift_enabled=false;p.repeated_wait_before=.25;
    const auto plan=plan_exposure_finder_schedule(recipe,p);
    REQUIRE(plan.parts.size()==4);REQUIRE(plan.exposures.size()==7);
    CHECK(plan.parts[1].requested_brightness==127);CHECK(plan.parts[1].label_normal_exposure==1.);
    CHECK(plan.parts[1].label_bottom_exposure==15.);
    CHECK(plan.exposures[0].mask_brightness==std::vector<uint8_t>{255,255,255,255});
    CHECK(plan.exposures[0].exposure==15.);
    CHECK_FALSE(plan.exposures[1].wait_before_override);
    CHECK(plan.exposures[2].wait_before_override==std::optional<double>(.25));
    CHECK_FALSE(plan.exposures[2].lift_height_override);
    CHECK(plan.exposures[3].mask_brightness==std::vector<uint8_t>{255,127});
    p.brightness_exclusion=ExposureFinderBrightnessExclusion::None;
    const auto all=plan_exposure_finder_schedule(recipe,p);
    CHECK(all.parts[1].label_bottom_exposure==7.47);
    CHECK(all.exposures[0].mask_brightness==std::vector<uint8_t>{255,127,255,127});
    CHECK(all.exposures[0].exposure==15.);
}
TEST_CASE("ExposureFinder custom base exposure applies with one test on the cure-time grid", "[sla_exposure_finder_schedule]") {
    ExposureFinderRecipe recipe;recipe.table_parameters.tests=0;recipe.rows=generate_exposure_finder_table(recipe.table_parameters);
    ExposureFinderScheduleParameters p;p.bottom_layers=0;p.base_mode=ExposureFinderBaseMode::Custom;p.custom_base_exposure=2.125;
    const auto plan=plan_exposure_finder_schedule(recipe,p);
    REQUIRE(plan.exposures.size()==4);
    CHECK(plan.exposures[0].exposure==2.12);CHECK(plan.exposures[1].exposure==2.12);
    CHECK(plan.exposures[2].exposure==2.);CHECK(plan.exposures[3].exposure==2.);
}
TEST_CASE("ExposureFinder schedule is bounded cancellable and independent of callback drafts", "[sla_exposure_finder_schedule]") {
    ExposureFinderRecipe recipe;recipe.table_parameters.tests=1;recipe.rows=generate_exposure_finder_table(recipe.table_parameters);
    ExposureFinderScheduleParameters p;p.multiple_brightness=true;p.brightness={255,127};
    const auto expected=finder_schedule_view(plan_exposure_finder_schedule(recipe,p));
    for(size_t stop:{size_t(0),size_t(1),size_t(16)}) {
        bool reached=false;
        CHECK_THROWS_AS(plan_exposure_finder_schedule(recipe,p,[&](size_t done,size_t total){
            if(total==16 && done==stop){reached=true;return false;}return true;
        }),LayerOperationCancelled);CHECK(reached);
        CHECK(finder_schedule_view(plan_exposure_finder_schedule(recipe,p))==expected);
    }
    auto draft=recipe;auto options=p;
    const auto captured=plan_exposure_finder_schedule(draft,options,[&](size_t,size_t){draft.rows.clear();options.brightness.clear();return true;});
    CHECK(finder_schedule_view(captured)==expected);
    for(double invalid:{-1.,std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()}) {
        auto bad=p;bad.features_height=invalid;CHECK_THROWS_AS(plan_exposure_finder_schedule(recipe,bad),Slic3r::RuntimeError);
    }
    auto bad=p;bad.base_mode=static_cast<ExposureFinderBaseMode>(5);CHECK_THROWS_AS(plan_exposure_finder_schedule(recipe,bad),Slic3r::RuntimeError);
    bad=p;bad.bottom_layers=65536;CHECK_THROWS_AS(plan_exposure_finder_schedule(recipe,bad),Slic3r::RuntimeError);
    bad=p;bad.features_height=100000.;CHECK_THROWS_AS(plan_exposure_finder_schedule(recipe,bad),Slic3r::RuntimeError);
    bad=p;bad.brightness={0};CHECK_THROWS_AS(plan_exposure_finder_schedule(recipe,bad),Slic3r::RuntimeError);
    bad=p;bad.brightness.assign(257,255);CHECK_THROWS_AS(plan_exposure_finder_schedule(recipe,bad),Slic3r::RuntimeError);
    auto ungridded=recipe;ungridded.table_parameters.multiple_layer_heights=true;
    ungridded.table_parameters.maximum_layer_height=.052;ungridded.table_parameters.layer_height_step=.0004;
    ungridded.rows=generate_exposure_finder_table(ungridded.table_parameters);
    CHECK_THROWS_AS(plan_exposure_finder_schedule(ungridded,p),Slic3r::RuntimeError);
    auto missing=recipe;missing.table_origin=ExposureFinderTableOrigin::Manual;
    missing.table_parameters.multiple_layer_heights=true;missing.table_parameters.maximum_layer_height=.1;
    missing.table_parameters.layer_height_step=.05;
    CHECK_THROWS_AS(plan_exposure_finder_schedule(missing,p),Slic3r::RuntimeError);
}
TEST_CASE("ExposureFinder physical schedules expose original-generator comparison fixtures", "[sla_exposure_finder_schedule]") {
    using nlohmann::json;json cases=json::array();
    auto record=[&](ExposureFinderRecipe recipe,ExposureFinderScheduleParameters p,bool known=false){
        const auto plan=plan_exposure_finder_schedule(recipe,p);const auto &t=recipe.table_parameters;
        json rows=json::array();for(const auto &row:recipe.rows)rows.push_back({{"layer_height",row.layer_height},
            {"bottom_exposure",row.bottom_exposure},{"normal_exposure",row.normal_exposure},{"brightness",row.brightness}});
        std::string levels;for(uint8_t value:p.brightness){if(!levels.empty())levels+=",";levels+=std::to_string(value);}
        cases.push_back({{"name",std::to_string(cases.size())},{"parameters",{
            {"LayerHeight",t.layer_height},{"MultipleLayerHeight",t.multiple_layer_heights},
            {"MultipleLayerHeightMaximum",t.maximum_layer_height},{"MultipleLayerHeightStep",t.layer_height_step},
            {"BottomExposure",t.bottom_exposure},{"NormalExposure",t.normal_exposure},
            {"BaseHeight",p.base_height},{"FeaturesHeight",p.features_height},{"BottomLayers",p.bottom_layers},
            {"MultipleExposures",p.multiple_exposures},{"MultipleBrightness",p.multiple_brightness},
            {"MultipleBrightnessValues",levels},{"MultipleBrightnessExcludeFrom",int(p.brightness_exclusion)},
            {"MultipleExposuresBaseLayersPrintMode",int(p.base_mode)},
            {"MultipleExposuresBaseLayersCustomExposure",p.custom_base_exposure},
            {"DifferentSettingsForSamePositionedLayers",p.repeated_settings},
            {"SamePositionedLayersLiftHeightEnabled",p.repeated_lift_enabled},
            {"SamePositionedLayersWaitTimeBeforeCureEnabled",p.repeated_wait_enabled},
            {"SamePositionedLayersLiftHeight",p.repeated_lift_height},
            {"SamePositionedLayersWaitTimeBeforeCure",p.repeated_wait_before}}},
            {"table",rows},{"expected",finder_schedule_view(plan)},{"part_count",plan.parts.size()},
            {"single_test_custom_base_fix",known}});
        CHECK_FALSE(plan.exposures.empty());
    };
    for(bool multiple:{false,true})for(bool brightness:{false,true})for(unsigned bottom:{0u,1u})for(int exclusion=0;exclusion<3;++exclusion) {
        ExposureFinderRecipe recipe;recipe.table_parameters.tests=2;
        if(multiple){recipe.table_parameters.layer_height=.025;recipe.table_parameters.multiple_layer_heights=true;
            recipe.table_parameters.maximum_layer_height=.05;recipe.table_parameters.layer_height_step=.025;}
        recipe.rows=generate_exposure_finder_table(recipe.table_parameters);
        ExposureFinderScheduleParameters p;p.base_height=.05;p.features_height=.1;p.bottom_layers=bottom;
        p.multiple_brightness=brightness;p.brightness={127,255,127,0};p.brightness_exclusion=ExposureFinderBrightnessExclusion(exclusion);
        record(recipe,p);
    }
    for(unsigned bottom:{0u,1u})for(int mode=0;mode<5;++mode) {
        ExposureFinderRecipe recipe;recipe.table_parameters.tests=2;recipe.rows=generate_exposure_finder_table(recipe.table_parameters);
        ExposureFinderScheduleParameters p;p.base_height=.1;p.features_height=.05;p.bottom_layers=bottom;
        p.base_mode=ExposureFinderBaseMode(mode);p.custom_base_exposure=2.125;record(recipe,p);
    }
    for(bool lift:{false,true})for(bool wait:{false,true}) {
        ExposureFinderRecipe recipe;recipe.table_parameters.tests=2;recipe.rows=generate_exposure_finder_table(recipe.table_parameters);
        ExposureFinderScheduleParameters p;p.bottom_layers=0;p.base_height=.05;p.features_height=.05;
        p.repeated_settings=true;p.repeated_lift_enabled=lift;p.repeated_wait_enabled=wait;
        p.repeated_lift_height=.5;p.repeated_wait_before=.25;record(recipe,p);
    }
    for(bool multiple:{false,true}) {
        ExposureFinderRecipe recipe;recipe.table_parameters.tests=2;
        if(multiple){recipe.table_parameters.multiple_layer_heights=true;recipe.table_parameters.maximum_layer_height=.1;recipe.table_parameters.layer_height_step=.05;}
        recipe.rows=generate_exposure_finder_table(recipe.table_parameters);
        ExposureFinderScheduleParameters p;p.multiple_exposures=false;p.bottom_layers=0;record(recipe,p);
    }
    for(bool adjacent:{false,true}) {
        ExposureFinderRecipe recipe;recipe.table_origin=ExposureFinderTableOrigin::Manual;
        recipe.rows={{.05,15,2,127},{.05,15,2.4,255},{.05,15,2,255}};
        if(adjacent)std::swap(recipe.rows[1],recipe.rows[2]);
        ExposureFinderScheduleParameters p;p.bottom_layers=0;record(recipe,p);
    }
    {
        ExposureFinderRecipe recipe;recipe.table_parameters.tests=2;recipe.rows=generate_exposure_finder_table(recipe.table_parameters);
        ExposureFinderScheduleParameters p;p.bottom_layers=0;p.features_height=0.;record(recipe,p);
    }
    for(bool multiple:{false,true}) {
        ExposureFinderRecipe recipe;recipe.table_parameters.tests=2;recipe.table_parameters.multiple_layer_heights=multiple;
        recipe.table_parameters.maximum_layer_height=.05;recipe.table_parameters.layer_height_step=.0004;
        recipe.rows=generate_exposure_finder_table(recipe.table_parameters);
        ExposureFinderScheduleParameters p;p.bottom_layers=0;record(recipe,p);
    }
    ExposureFinderRecipe one;one.table_parameters.tests=0;one.rows=generate_exposure_finder_table(one.table_parameters);
    ExposureFinderScheduleParameters custom;custom.bottom_layers=0;custom.base_height=.05;custom.features_height=.1;
    custom.base_mode=ExposureFinderBaseMode::Custom;custom.custom_base_exposure=3.25;record(one,custom,true);
    REQUIRE(cases.size()==46);
    if(const char *path=std::getenv("SLA_EXPOSURE_SCHEDULE_FIXTURES")) {
        std::ofstream file(path);REQUIRE(file.good());file<<cases.dump(2)<<'\n';REQUIRE(file.good());
    }
}

TEST_CASE("ExposureFinder schedule restores FP environment on success and cancellation", "[sla_exposure_finder_schedule]") {
    struct Restore {std::fenv_t state;Restore(){std::fegetenv(&state);}~Restore(){std::fesetenv(&state);}} restore;
    ExposureFinderRecipe recipe;recipe.table_parameters.tests=1;recipe.rows=generate_exposure_finder_table(recipe.table_parameters);
    ExposureFinderScheduleParameters p;p.multiple_brightness=true;p.brightness={255,127};
    const auto expected=plan_exposure_finder_schedule(recipe,p);
    for(int mode:{FE_UPWARD,FE_DOWNWARD,FE_TOWARDZERO}) {
        REQUIRE(std::fesetround(mode)==0);std::feclearexcept(FE_ALL_EXCEPT);std::feraiseexcept(FE_INVALID);
        const auto actual=plan_exposure_finder_schedule(recipe,p);
        REQUIRE(actual.exposures.size()==expected.exposures.size());
        for(size_t i=0;i<actual.exposures.size();++i) {
            CHECK(actual.exposures[i].position_z==expected.exposures[i].position_z);
            CHECK(actual.exposures[i].exposure==expected.exposures[i].exposure);
        }
        CHECK(std::fegetround()==mode);CHECK(std::fetestexcept(FE_ALL_EXCEPT)==FE_INVALID);
        CHECK_THROWS_AS(plan_exposure_finder_schedule(recipe,p,[](size_t,size_t){return false;}),LayerOperationCancelled);
        CHECK(std::fegetround()==mode);CHECK(std::fetestexcept(FE_ALL_EXCEPT)==FE_INVALID);
    }
}

TEST_CASE("ExposureFinder schedule descriptor survives project load", "[sla_exposure_schedule_recipe]") {
    using namespace Slic3r;FinderDirectory folder("schedule-schema");auto job=finder_recipe_job();
    const auto base=folder.path/"base.sla-project",output=folder.path/"schedule.sla-project";
    sla::save_layer_project(base.string(),sla::capture_generated_project(*job,*job->layer_document()));
    auto entries=read_zipper_archive(base.string(),{"metadata/sla/"},{}).entries;
    const auto manifest=std::find_if(entries.begin(),entries.end(),[](const auto &e){return e.fname=="metadata/sla/layers.json";});
    REQUIRE(manifest!=entries.end());auto metadata=nlohmann::json::parse(manifest->buf);
    auto &finder=metadata["calibration_recipe"]["exposure_finder"];finder["version"]=2;
    finder["schedule"]={{"base_height",.05},{"features_height",.1},{"custom_base_exposure",3.25},
        {"bottom_layers",0},{"multiple_exposures",true},{"multiple_brightness",true},{"brightness",{127,255,127,0}},
        {"base_mode",4},{"brightness_exclusion",2},{"repeated_settings",true},
        {"repeated_lift_enabled",true},{"repeated_wait_enabled",true},{"repeated_lift_height",.5},{"repeated_wait_before",.25}};
    const auto text=metadata.dump();manifest->buf.assign(text.begin(),text.end());
    {Zipper zip(output.string());for(const auto &e:entries)zip.add_entry(e.fname,e.buf.data(),e.buf.size());zip.finalize();}
    std::optional<sla::ProjectRasterState> state;
    REQUIRE_NOTHROW(state=sla::read_project_layers(output.string()));REQUIRE(state);
    CHECK(state->document.image(0).buf==job->layer_document()->image(0).buf);
}

TEST_CASE("ExposureFinder schedule options are owned persisted and do not regenerate masks", "[sla_exposure_schedule_recipe]") {
    using namespace Slic3r;FinderDirectory folder("schedule-typed");auto job=finder_recipe_job();
    auto finder=std::make_shared<ExposureFinderRecipe>(*job->calibration_recipe()->exposure_finder);
    ExposureFinderScheduleParameters options;options.base_height=.05;options.features_height=.1;
    options.custom_base_exposure=3.25;options.bottom_layers=0;options.base_mode=ExposureFinderBaseMode::Custom;
    options.multiple_brightness=true;options.brightness={127,255,127,0};options.repeated_settings=true;
    options.repeated_lift_height=.5;options.repeated_wait_before=.25;finder->schedule=options;
    auto recipe=std::make_shared<sla::CalibrationRecipe>(*job->calibration_recipe());recipe->exposure_finder=finder;
    job->set_calibration_recipe(recipe);const auto guard=job->export_generation_guard();const auto snapshot=job->capture_export_snapshot();
    finder->schedule->brightness.clear();finder->schedule->base_height=-1.;
    CHECK(job->calibration_recipe()->exposure_finder->schedule->brightness==options.brightness);
    CHECK(snapshot.calibration_recipe()->exposure_finder->schedule->base_height==.05);
    CHECK_THROWS_AS(job->set_calibration_recipe(recipe),RuntimeError);CHECK(guard.current());
    auto edited=*job->layer_document();edited.invert(0);job->replace_layer_document(edited);
    const auto before=job->layer_document()->image(0).buf;
    const auto parameters=job->layer_document()->fresh_job()->rows;
    const auto project=folder.path/"owned.sla-project";
    sla::save_layer_project(project.string(),sla::capture_generated_project(*job,*job->layer_document()));
    const auto state=sla::read_project_layers(project.string());REQUIRE(state);
    REQUIRE(state->calibration_recipe->exposure_finder->schedule);
    const auto &saved=*state->calibration_recipe->exposure_finder;
    CHECK(saved.schedule->brightness==options.brightness);
    CHECK(finder_schedule_view(plan_exposure_finder_schedule(saved,*saved.schedule))==
        finder_schedule_view(plan_exposure_finder_schedule(*job->calibration_recipe()->exposure_finder,options)));
    auto restored=sla::restore_generated_project(*state);
    CHECK(restored->layer_document()->image(0).buf==before);
    CHECK(restored->layer_document()->fresh_job()->rows==parameters);
    REQUIRE_NOTHROW(restored->export_print((folder.path/"owned.uvj").string()));
    // Disabled inputs are persisted exactly rather than erased or defaulted.
    auto manual=std::make_shared<ExposureFinderRecipe>();manual->table_origin=ExposureFinderTableOrigin::Manual;
    manual->rows={{.1,20,3,127},{.05,15,2,255},{.05,15,2,255},{.05,0,-1,0}};
    auto off=options;off.base_height=0;off.bottom_layers=2;off.multiple_exposures=false;
    off.multiple_brightness=false;off.brightness={0,0};off.base_mode=ExposureFinderBaseMode::Last;
    off.brightness_exclusion=ExposureFinderBrightnessExclusion::Bottom;off.repeated_settings=false;
    off.repeated_lift_enabled=false;off.repeated_wait_enabled=false;
    off.repeated_lift_height=.75;off.repeated_wait_before=.5;manual->schedule=off;
    auto manual_recipe=std::make_shared<sla::CalibrationRecipe>(*restored->calibration_recipe());manual_recipe->exposure_finder=manual;
    restored->set_calibration_recipe(manual_recipe);
    const auto manual_path=folder.path/"manual.sla-project";
    sla::save_layer_project(manual_path.string(),sla::capture_generated_project(*restored,*restored->layer_document()));
    const auto reloaded=sla::read_project_layers(manual_path.string());REQUIRE(reloaded);
    const auto &inactive=*reloaded->calibration_recipe->exposure_finder->schedule;
    CHECK(inactive.brightness==off.brightness);CHECK_FALSE(inactive.multiple_exposures);CHECK_FALSE(inactive.multiple_brightness);
    CHECK(inactive.base_mode==off.base_mode);CHECK(inactive.brightness_exclusion==off.brightness_exclusion);
    CHECK_FALSE(inactive.repeated_settings);CHECK_FALSE(inactive.repeated_lift_enabled);CHECK_FALSE(inactive.repeated_wait_enabled);
    CHECK(inactive.repeated_lift_height==.75);CHECK(inactive.repeated_wait_before==.5);
    auto restarted=sla::restore_generated_project(*reloaded);
    REQUIRE_NOTHROW(restarted->export_print((folder.path/"manual.uvj").string()));
    auto invalid=*state;invalid.calibration_recipe=recipe;
    const auto target=folder.path/"keep.sla-project";{std::ofstream out(target.string());out<<"keep";}
    CHECK_THROWS_AS(sla::save_layer_project(target.string(),invalid),RuntimeError);
    {std::ifstream in(target.string());std::string value;in>>value;CHECK(value=="keep");}
}
TEST_CASE("ExposureFinder schedule recipe preserves each mode and legacy absent schedule", "[sla_exposure_schedule_recipe]") {
    using namespace Slic3r;FinderDirectory folder("schedule-modes");auto job=finder_recipe_job();
    const auto old=folder.path/"legacy.sla-project";
    sla::save_layer_project(old.string(),sla::capture_generated_project(*job,*job->layer_document()));
    const auto legacy=sla::read_project_layers(old.string());REQUIRE(legacy);
    CHECK_FALSE(legacy->calibration_recipe->exposure_finder->schedule);
    for(int mode=0;mode<5;++mode)for(int exclusion=0;exclusion<3;++exclusion) {
        auto finder=std::make_shared<ExposureFinderRecipe>(*job->calibration_recipe()->exposure_finder);
        ExposureFinderScheduleParameters p;p.bottom_layers=0;p.base_mode=ExposureFinderBaseMode(mode);
        p.brightness_exclusion=ExposureFinderBrightnessExclusion(exclusion);p.multiple_brightness=true;
        p.brightness={255,127};p.custom_base_exposure=2.125;finder->schedule=p;
        auto recipe=std::make_shared<sla::CalibrationRecipe>(*job->calibration_recipe());recipe->exposure_finder=finder;
        job->set_calibration_recipe(recipe);
        const auto path=folder.path/(std::to_string(mode)+"-"+std::to_string(exclusion)+".sla-project");
        sla::save_layer_project(path.string(),sla::capture_generated_project(*job,*job->layer_document()));
        const auto state=sla::read_project_layers(path.string());REQUIRE(state);
        const auto &restored=*state->calibration_recipe->exposure_finder;
        REQUIRE(restored.schedule);CHECK(restored.schedule->base_mode==p.base_mode);
        CHECK(restored.schedule->brightness_exclusion==p.brightness_exclusion);
        CHECK(restored.schedule->custom_base_exposure==2.125);
        CHECK(finder_schedule_view(plan_exposure_finder_schedule(restored,*restored.schedule))==
            finder_schedule_view(plan_exposure_finder_schedule(*finder,p)));
    }
}
TEST_CASE("ExposureFinder schedule recipe rejects malformed options and downgrade", "[sla_exposure_schedule_recipe]") {
    using namespace Slic3r;using Json=nlohmann::json;
    auto job=finder_recipe_job();FinderDirectory folder("schedule-hostile");
    auto finder=std::make_shared<ExposureFinderRecipe>(*job->calibration_recipe()->exposure_finder);
    finder->schedule=ExposureFinderScheduleParameters{};
    auto recipe=std::make_shared<sla::CalibrationRecipe>(*job->calibration_recipe());recipe->exposure_finder=finder;
    job->set_calibration_recipe(recipe);const auto good=folder.path/"good.sla-project";
    sla::save_layer_project(good.string(),sla::capture_generated_project(*job,*job->layer_document()));
    const auto original=read_zipper_archive(good.string(),{"metadata/sla/"},{}).entries;
    const std::vector<std::pair<std::string,std::function<void(Json &)>>> mutations{
        {"downgrade",[](auto &j){j["version"]=1;}},
        {"schema",[](auto &j){j["version"]=3;}},
        {"float-schema",[](auto &j){j["version"]=2.0;}},
        {"missing",[](auto &j){j.erase("schedule");}},
        {"null",[](auto &j){j["schedule"]=nullptr;}},
        {"extra",[](auto &j){j["schedule"]["extra"]=1;}},
        {"missing-base",[](auto &j){j["schedule"].erase("base_height");}},
        {"negative-base",[](auto &j){j["schedule"]["base_height"]=-1;}},
        {"zero-total",[](auto &j){j["schedule"]["base_height"]=0;j["schedule"]["features_height"]=0;}},
        {"features-null",[](auto &j){j["schedule"]["features_height"]=nullptr;}},
        {"features-string",[](auto &j){j["schedule"]["features_height"]="1";}},
        {"features-bool",[](auto &j){j["schedule"]["features_height"]=true;}},
        {"huge",[](auto &j){j["schedule"]["features_height"]=1e200;}},
        {"budget",[](auto &j){j["schedule"]["features_height"]=100000.;}},
        {"bottom-negative",[](auto &j){j["schedule"]["bottom_layers"]=-1;}},
        {"bottom-large",[](auto &j){j["schedule"]["bottom_layers"]=65536;}},
        {"bottom-fraction",[](auto &j){j["schedule"]["bottom_layers"]=1.5;}},
        {"bottom-bool",[](auto &j){j["schedule"]["bottom_layers"]=true;}},
        {"base-mode",[](auto &j){j["schedule"]["base_mode"]=5;}},
        {"base-negative",[](auto &j){j["schedule"]["base_mode"]=-1;}},
        {"base-mode-bool",[](auto &j){j["schedule"]["base_mode"]=true;}},
        {"brightness-mode",[](auto &j){j["schedule"]["brightness_exclusion"]=3;}},
        {"brightness-mode-fraction",[](auto &j){j["schedule"]["brightness_exclusion"]=1.0;}},
        {"not-boolean",[](auto &j){j["schedule"]["multiple_brightness"]=1;}},
        {"repeated-not-boolean",[](auto &j){j["schedule"]["repeated_settings"]=1;}},
        {"lift-not-boolean",[](auto &j){j["schedule"]["repeated_lift_enabled"]=1;}},
        {"wait-not-boolean",[](auto &j){j["schedule"]["repeated_wait_enabled"]=0;}},
        {"negative-lift",[](auto &j){j["schedule"]["repeated_lift_height"]=-1;}},
        {"negative-wait",[](auto &j){j["schedule"]["repeated_wait_before"]=-1;}},
        {"list-string",[](auto &j){j["schedule"]["brightness"]="255";}},
        {"list-long",[](auto &j){j["schedule"]["brightness"]=Json::array();for(int i=0;i<257;++i)j["schedule"]["brightness"].push_back(255);}},
        {"list-byte-overflow",[](auto &j){j["schedule"]["brightness"]={256};}},
        {"list-byte-negative",[](auto &j){j["schedule"]["brightness"]={-1};}},
        {"list-byte-fraction",[](auto &j){j["schedule"]["brightness"]={127.5};}},
        {"list-byte-bool",[](auto &j){j["schedule"]["brightness"]={true};}},
        {"no-brightness",[](auto &j){j["schedule"]["multiple_brightness"]=true;j["schedule"]["brightness"]={0};}},
        {"invalid-custom",[](auto &j){j["schedule"]["bottom_layers"]=0;j["schedule"]["base_mode"]=4;j["schedule"]["custom_base_exposure"]=0;}},
        {"height-coverage",[](auto &j){j["parameters"]["multiple_layer_heights"]=true;j["parameters"]["maximum_layer_height"]=.1;j["parameters"]["layer_height_step"]=.05;j["table_origin"]=1;}}
    };
    for(const auto &[name,mutate]:mutations) {
        INFO(name);auto entries=original;
        const auto manifest=std::find_if(entries.begin(),entries.end(),[](const auto &e){return e.fname=="metadata/sla/layers.json";});
        REQUIRE(manifest!=entries.end());auto metadata=Json::parse(manifest->buf);mutate(metadata["calibration_recipe"]["exposure_finder"]);
        const auto text=metadata.dump();manifest->buf.assign(text.begin(),text.end());const auto bad=folder.path/(name+".sla-project");
        {Zipper zip(bad.string());for(const auto &e:entries)zip.add_entry(e.fname,e.buf.data(),e.buf.size());zip.finalize();}
        CHECK_THROWS(sla::read_project_layers(bad.string()));REQUIRE(sla::read_project_layers(good.string()));
    }
}


namespace {
// Compare every persisted field independently of the JSON serializer. Requested
// dimensions retain their original doubles even when the renderer rounds them.
void check_finder_generation(const ExposureFinderGenerationSettings &a,
                             const ExposureFinderGenerationSettings &b) {
#define FINDER_PATTERN_FIELD(name) CHECK(a.pattern.name == b.pattern.name)
    FINDER_PATTERN_FIELD(holes); FINDER_PATTERN_FIELD(square_holes); FINDER_PATTERN_FIELD(bars);
    FINDER_PATTERN_FIELD(bullseyes); FINDER_PATTERN_FIELD(feature_margin_mm);
    FINDER_PATTERN_FIELD(bar_length_mm); FINDER_PATTERN_FIELD(bar_spacing_mm);
    FINDER_PATTERN_FIELD(bar_splitter); FINDER_PATTERN_FIELD(bar_fence); FINDER_PATTERN_FIELD(bar_fence_offset);
    FINDER_PATTERN_FIELD(bullseye_fence); FINDER_PATTERN_FIELD(bullseye_fence_offset);
    FINDER_PATTERN_FIELD(invert_bullseye_quadrants); FINDER_PATTERN_FIELD(counter_triangles);
    FINDER_PATTERN_FIELD(triangle_fence); FINDER_PATTERN_FIELD(triangle_tip_offset);
    FINDER_PATTERN_FIELD(staircase_thickness); FINDER_PATTERN_FIELD(antialias);
    FINDER_PATTERN_FIELD(text); FINDER_PATTERN_FIELD(font_face); FINDER_PATTERN_FIELD(text_thickness);
    FINDER_PATTERN_FIELD(text_scale);
#undef FINDER_PATTERN_FIELD
#define FINDER_LAYOUT_FIELD(name) CHECK(a.layout.name == b.layout.name)
    FINDER_LAYOUT_FIELD(cell_columns); FINDER_LAYOUT_FIELD(cell_rows);
    FINDER_LAYOUT_FIELD(margin_x_mm); FINDER_LAYOUT_FIELD(margin_y_mm);
    FINDER_LAYOUT_FIELD(gap_x_mm); FINDER_LAYOUT_FIELD(gap_y_mm);
    FINDER_LAYOUT_FIELD(center); FINDER_LAYOUT_FIELD(mirror_x); FINDER_LAYOUT_FIELD(mirror_y);
#undef FINDER_LAYOUT_FIELD
    CHECK(a.bottom_erosion==b.bottom_erosion); CHECK(a.chamfer_layers==b.chamfer_layers);
}
ExposureFinderGenerationSettings finder_generation_intent() {
    ExposureFinderGenerationSettings g;auto &p=g.pattern;
    p.holes={1,5,9};p.square_holes=false;p.bars={2,4,12};p.bullseyes={{20,2},{60,4}};
    p.feature_margin_mm=.245;p.bar_length_mm=.75;p.bar_spacing_mm=.25;
    p.bar_splitter=-2;p.bar_fence=3;p.bar_fence_offset=-1;p.bullseye_fence=0;p.bullseye_fence_offset=-2;
    p.invert_bullseye_quadrants=true;p.counter_triangles=true;p.triangle_fence=false;p.triangle_tip_offset=-3;
    p.staircase_thickness=3;p.antialias=true;p.text="Łódź / Привет";p.font_face=3;p.text_scale=.805;p.text_thickness=1;
    g.layout.margin_x_mm=.25;g.layout.margin_y_mm=.5;g.layout.gap_x_mm=.75;g.layout.gap_y_mm=.25;
    g.layout.center=false;g.layout.mirror_x=true;g.layout.mirror_y=true;
    g.bottom_erosion=1;g.chamfer_layers=2;return g;
}
}
TEST_CASE("ExposureFinder generation child v3 owns every field without regenerating project masks", "[sla_exposure_generation_recipe]") {
    using namespace Slic3r;
    FinderDirectory folder("generation-typed");auto job=finder_recipe_job();
    auto finder=std::make_shared<ExposureFinderRecipe>(*job->calibration_recipe()->exposure_finder);
    finder->schedule=ExposureFinderScheduleParameters{};finder->generation=finder_generation_intent();
    // Explicit footprint as well as auto footprint must survive persistence.
    for(const bool explicit_cell:{false,true}) {
        finder->generation=finder_generation_intent();
        if(explicit_cell){finder->generation->layout.cell_columns=321;finder->generation->layout.cell_rows=123;}
        const auto expected=*finder->generation;
        auto recipe=std::make_shared<sla::CalibrationRecipe>(*job->calibration_recipe());recipe->exposure_finder=finder;
        job->set_calibration_recipe(recipe);const auto snapshot=job->capture_export_snapshot();
        finder->generation->pattern.text="changed alias";finder->generation->layout.margin_x_mm=123.;
        REQUIRE(job->calibration_recipe()->exposure_finder->generation);
        check_finder_generation(*job->calibration_recipe()->exposure_finder->generation,expected);
        check_finder_generation(*snapshot.calibration_recipe()->exposure_finder->generation,expected);
        auto edited=*job->layer_document();edited.invert(0);job->replace_layer_document(edited);
        const auto pixels=job->layer_document()->image(0).buf;
        const auto rows=job->layer_document()->fresh_job()->rows;
        auto current=job->capture_export_snapshot();
        for(int round=0;round<2;++round) {
            const auto path=folder.path/(std::string(explicit_cell?"explicit":"auto")+std::to_string(round)+".sla-project");
            save_layer_project(path.string(),capture_generated_project(*job,*job->layer_document()));
            const auto entries=read_zipper_archive(path.string(),{"metadata/sla/"},{}).entries;
            const auto manifest=std::find_if(entries.begin(),entries.end(),[](const auto &e){return e.fname=="metadata/sla/layers.json";});
            REQUIRE(manifest!=entries.end());const auto json=nlohmann::json::parse(manifest->buf);
            CHECK(json.at("calibration_recipe").at("exposure_finder").at("version")==3);
            CHECK(json.at("calibration_recipe").at("exposure_finder").at("generation").at("version")==1);
            const auto state=read_project_layers(path.string());REQUIRE(state);
            REQUIRE(state->calibration_recipe->exposure_finder->generation);
            check_finder_generation(*state->calibration_recipe->exposure_finder->generation,expected);
            job=restore_generated_project(*state);
            CHECK(job->layer_document()->image(0).buf==pixels);
            CHECK(job->layer_document()->fresh_job()->rows==rows);
        }
    }
}
TEST_CASE("ExposureFinder generation rejects malformed children and version downgrade", "[sla_exposure_generation_recipe]") {
    using namespace Slic3r;using Json=nlohmann::json;
    FinderDirectory folder("generation-hostile");auto job=finder_recipe_job();
    auto finder=std::make_shared<ExposureFinderRecipe>(*job->calibration_recipe()->exposure_finder);
    finder->schedule=ExposureFinderScheduleParameters{};finder->generation=finder_generation_intent();
    auto recipe=std::make_shared<sla::CalibrationRecipe>(*job->calibration_recipe());recipe->exposure_finder=finder;
    job->set_calibration_recipe(recipe);const auto good=folder.path/"good.sla-project";
    save_layer_project(good.string(),capture_generated_project(*job,*job->layer_document()));
    const auto original=read_zipper_archive(good.string(),{"metadata/sla/"},{}).entries;
    const std::vector<std::pair<std::string,std::function<void(Json &)>>> mutations{
        {"downgrade-v1",[](auto &j){j["version"]=1;}},{"downgrade-v2",[](auto &j){j["version"]=2;}},
        {"missing-generation",[](auto &j){j.erase("generation");}},
        {"missing-schedule",[](auto &j){j.erase("schedule");}},
        {"null-generation",[](auto &j){j["generation"]=nullptr;}},
        {"unknown-version",[](auto &j){j["generation"]["version"]=2;}},
        {"fraction-version",[](auto &j){j["generation"]["version"]=1.0;}},
        {"extra-generation",[](auto &j){j["generation"]["extra"]=0;}},
        {"missing-pattern",[](auto &j){j["generation"].erase("pattern");}},
        {"extra-pattern",[](auto &j){j["generation"]["pattern"]["extra"]=0;}},
        {"missing-layout",[](auto &j){j["generation"].erase("layout");}},
        {"extra-layout",[](auto &j){j["generation"]["layout"]["extra"]=0;}},
        {"erosion-overflow",[](auto &j){j["generation"]["bottom_erosion"]=256;}},
        {"negative-chamfer",[](auto &j){j["generation"]["chamfer_layers"]=-1;}},
        {"fraction-chamfer",[](auto &j){j["generation"]["chamfer_layers"]=.5;}},
        {"boolean-columns",[](auto &j){j["generation"]["layout"]["cell_columns"]=true;}},
        {"huge-rows",[](auto &j){j["generation"]["layout"]["cell_rows"]=32769;}},
        {"layout-negative",[](auto &j){j["generation"]["layout"]["gap_y_mm"]=-.1;}},
        {"layout-boolean",[](auto &j){j["generation"]["layout"]["center"]=1;}},
        {"layout-null",[](auto &j){j["generation"]["layout"]["margin_x_mm"]=nullptr;}},
        {"holes-unsorted",[](auto &j){j["generation"]["pattern"]["holes"]={9,1};}},
        {"holes-duplicate",[](auto &j){j["generation"]["pattern"]["holes"]={1,1};}},
        {"holes-zero",[](auto &j){j["generation"]["pattern"]["holes"]={0};}},
        {"holes-large",[](auto &j){j["generation"]["pattern"]["holes"]={501};}},
        {"holes-list-budget",[](auto &j){j["generation"]["pattern"]["holes"]=Json::array();for(int i=0;i<501;++i)j["generation"]["pattern"]["holes"].push_back(i+1);}},
        {"ring-short",[](auto &j){j["generation"]["pattern"]["bullseyes"]={{20}};}},
        {"ring-order",[](auto &j){j["generation"]["pattern"]["bullseyes"]={{60,4},{20,2}};}},
        {"ring-zero",[](auto &j){j["generation"]["pattern"]["bullseyes"]={{20,0}};}},
        {"negative-dimension",[](auto &j){j["generation"]["pattern"]["bar_length_mm"]=-1;}},
        {"huge-dimension",[](auto &j){j["generation"]["pattern"]["feature_margin_mm"]=1e200;}},
        {"string-dimension",[](auto &j){j["generation"]["pattern"]["text_scale"]=".8";}},
        {"zero-scale",[](auto &j){j["generation"]["pattern"]["text_scale"]=0;}},
        {"invalid-font-gap",[](auto &j){j["generation"]["pattern"]["font_face"]=8;}},
        {"text-long",[](auto &j){j["generation"]["pattern"]["text"]=std::string(4097,'x');}},
        {"text-nul",[](auto &j){j["generation"]["pattern"]["text"]=std::string("a\0b",3);}},
        {"offset-overflow",[](auto &j){j["generation"]["pattern"]["bar_splitter"]=128;}},
        {"offset-unsigned-overflow",[](auto &j){j["generation"]["pattern"]["bar_splitter"]=uint64_t(-1);}},
        {"boolean-antialias",[](auto &j){j["generation"]["pattern"]["antialias"]=1;}}
    };
    for(const auto &[name,mutate]:mutations) {
        INFO(name);auto entries=original;
        const auto manifest=std::find_if(entries.begin(),entries.end(),[](const auto &e){return e.fname=="metadata/sla/layers.json";});
        REQUIRE(manifest!=entries.end());auto metadata=Json::parse(manifest->buf);mutate(metadata["calibration_recipe"]["exposure_finder"]);
        const auto text=metadata.dump();manifest->buf.assign(text.begin(),text.end());const auto path=folder.path/(name+".sla-project");
        {Zipper zip(path.string());for(const auto &e:entries)zip.add_entry(e.fname,e.buf.data(),e.buf.size());zip.finalize();}
        CHECK_THROWS(read_project_layers(path.string()));REQUIRE(read_project_layers(good.string()));
    }
    // Explicit, valid older schemas remain supported without invented provenance.
    for(const int version:{1,2}) {
        auto entries=original;const auto manifest=std::find_if(entries.begin(),entries.end(),[](const auto &e){return e.fname=="metadata/sla/layers.json";});
        REQUIRE(manifest!=entries.end());auto json=Json::parse(manifest->buf);auto &child=json["calibration_recipe"]["exposure_finder"];
        child["version"]=version;child.erase("generation");if(version==1)child.erase("schedule");
        const auto text=json.dump();manifest->buf.assign(text.begin(),text.end());const auto path=folder.path/("legacy-"+std::to_string(version)+".sla-project");
        {Zipper zip(path.string());for(const auto &e:entries)zip.add_entry(e.fname,e.buf.data(),e.buf.size());zip.finalize();}
        const auto state=read_project_layers(path.string());REQUIRE(state);const auto &restored=*state->calibration_recipe->exposure_finder;
        CHECK_FALSE(restored.generation);CHECK(bool(restored.schedule)==(version==2));
        const auto again=folder.path/("legacy-saved-"+std::to_string(version)+".sla-project");
        save_layer_project(again.string(),*state);REQUIRE(read_project_layers(again.string()));
    }
}
