// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "SL1FreshExportPlan.hpp"
#include "SL1FreshMaterialProfile.hpp"
#include "libslic3r/miniz_extension.hpp"
#include <boost/algorithm/string.hpp>
#include <nlohmann/json.hpp>
#include <functional>
#include <cstring>
#include <cstdlib>
#include <boost/log/trivial.hpp>
#include <map>
#include <sstream>

namespace Slic3r { class SLAPrint; }

namespace Slic3r::sla {
// Verify the actual ZIP before publication. SL1 has globals plus a generated
// schedule, so private project metadata cannot stand in for executable rows.
// Only the export implementation may select encoded equality after its
// private owned-document gate. No public "already validated" switch exists.
class SL1FreshArchiveVerifier {
    friend class ::Slic3r::SLAPrint;
    static void verify(const std::string &path,
    const SL1FreshExportPlan &plan, const LayerDocument &source, const SLAMaterialConfig &material,
    const std::function<void()> &checkpoint, bool compare_encoded)
{
    const auto check = [&] { if (checkpoint) checkpoint(); };
    check();
    struct Zip {
        mz_zip_archive archive{};
        ~Zip() { mz_zip_reader_end(&archive); }
        explicit Zip(const std::string &path) {
            if (!mz_zip_reader_init_file(&archive,path.c_str(),0))
                throw RuntimeError("SL1 cannot reload exported ZIP");
        }
        std::string text(const char *entry) {
            size_t size=0;
            void *data=mz_zip_reader_extract_file_to_heap(&archive,entry,&size,0);
            if (!data) throw RuntimeError(std::string("SL1 missing exported ")+entry);
            const std::string value(static_cast<const char *>(data),size);
            mz_free(data); return value;
        }
    } zip(path);
    auto ini=[](const std::string &text) {
        std::map<std::string,std::string> values;
        std::istringstream input(text);
        for (std::string line;std::getline(input,line);) {
            const auto equal=line.find('='); if (equal==std::string::npos) continue;
            const auto key=boost::trim_copy(line.substr(0,equal));
            if (!values.emplace(key,boost::trim_copy(line.substr(equal+1))).second)
                throw RuntimeError("SL1 duplicate exported INI key: "+key);
        }
        return values;
    };
    const auto config=ini(zip.text("config.ini")), slicer=ini(zip.text("prusaslicer.ini"));
    auto number=[](const auto &values,const char *key) {
        auto found=values.find(key);
        if (found==values.end()) throw RuntimeError(std::string("SL1 missing exported ")+key);
        std::istringstream input(found->second); input.imbue(std::locale::classic());
        double value=0; const bool parsed=bool(input>>value); input>>std::ws;
        if (!parsed || !input.eof() || !std::isfinite(value) || value<0)
            throw RuntimeError(std::string("SL1 invalid exported ")+key);
        return value;
    };
    auto require=[](bool condition,const char *field) {
        if (!condition) throw RuntimeError(std::string("SL1 exported ")+field+" cannot preserve fresh intent");
    };
    using F=LayerParameter;
    require(float(number(config,"layerHeight"))==float(plan.globals.layer_height),"layerHeight");
    require(float(number(config,"expTime"))==float(plan.globals.normal.at(F::Exposure)),"expTime");
    require(float(number(config,"expTimeFirst"))==float(plan.globals.bottom.at(F::Exposure)),"expTimeFirst");
    require(number(config,"numFade")==plan.globals.bottom_layers,"numFade");
    require(number(config,"numSlow")==plan.globals.bottom_layers,"numSlow");
    require(number(config,"numFast")==plan.rows.size()-plan.globals.bottom_layers,"numFast");
    require(float(number(config,"printTime"))==plan.expected_print_time,"printTime");
    require(number(config,"usedMaterial")==plan.material_ml,"usedMaterial");
    require(float(number(slicer,"layer_height"))==float(plan.globals.layer_height),"layer_height");
    require(float(number(slicer,"initial_layer_height"))==float(plan.initial_layer_height),"initial_layer_height");
    require(float(number(slicer,"exposure_time"))==float(plan.globals.normal.at(F::Exposure)),"exposure_time");
    require(float(number(slicer,"initial_exposure_time"))==float(plan.globals.bottom.at(F::Exposure)),"initial_exposure_time");
    require(float(number(slicer,"display_width"))==float(plan.geometry.width_mm),"display_width");
    require(float(number(slicer,"display_height"))==float(plan.geometry.height_mm),"display_height");
    require(number(slicer,"display_pixels_x")==plan.geometry.columns,"display_pixels_x");
    require(number(slicer,"display_pixels_y")==plan.geometry.rows,"display_pixels_y");
    auto note=[&](const std::string &keyword) {
        std::optional<double> result;
        for (const char *key:{"material_notes","printer_notes"}) {
            auto found=slicer.find(key); if (found==slicer.end()) continue;
            std::vector<std::string> lines;
            auto text=found->second; boost::replace_all(text,"\\n","\n");
            boost::split(lines,text,boost::is_any_of("\r\n"));
            for (auto line:lines) {
                boost::trim(line);
                if (!boost::istarts_with(line,keyword+"_")) continue;
                require(!result,"duplicate custom note");
                std::map<std::string,std::string> value{{"value",line.substr(keyword.size()+1)}};
                result=number(value,"value");
            }
        }
        if (!result) throw RuntimeError("SL1 missing exported custom note "+keyword);
        return *result;
    };
    const std::pair<F,const char *> fields[]={
        {F::LightOff,"LightOffDelay"},{F::WaitBefore,"WaitTimeBeforeCure"},
        {F::WaitAfter,"WaitTimeAfterCure"},{F::WaitAfterLift,"WaitTimeAfterLift"},
        {F::LiftDistance,"LiftHeight"},{F::LiftDistance2,"LiftHeight2"},
        {F::LiftSpeed,"LiftSpeed"},{F::LiftSpeed2,"LiftSpeed2"},
        {F::RetractSpeed,"RetractSpeed"},{F::RetractDistance2,"RetractHeight2"},
        {F::RetractSpeed2,"RetractSpeed2"},{F::LightPWM,"LightPWM"}};
    require(note("TransitionLayerCount")==plan.globals.transition_layers,"TransitionLayerCount");
    for (bool bottom:{false,true}) for (const auto &[field,name]:fields)
        require(float(note(std::string(bottom?"Bottom":"")+name))==
            float((bottom?plan.globals.bottom:plan.globals.normal).at(field)),name);
    const auto profile=sl1_fresh_material_profile(plan,material);
    const char *material_keys[]={"material_density","bottle_cost","bottle_volume","bottle_weight"};
    for (size_t i=0;i<profile.size();++i)
        require(float(number(slicer,material_keys[i]))==profile[i],material_keys[i]);
    const auto metadata=nlohmann::json::parse(zip.text("config.json"));
    for (const char *key:{"layerHeight","expTime","expTimeFirst","numFade","numSlow","numFast","usedMaterial","printTime"})
        require(metadata.contains(key) &&
            ((metadata.at(key).is_number() && metadata.at(key).get<double>()==number(config,key)) ||
             (metadata.at(key).is_string() && metadata.at(key).get<std::string>()==config.at(key))),"config.json");
    const auto decoded=LayerDocument::load_archive(path, [&](size_t,size_t) {check();return true;});
    require(decoded.layer_count()==plan.rows.size(),"layer count");
    for (size_t i=0;i<plan.rows.size();++i) {
        check();
        if (compare_encoded) {
            // The complete planner decoded/CRC-checked this exact immutable
            // owned source. Equality includes every PNG byte, so it also
            // proves dimensions/pixels while preserving ZIP readback above.
            const auto actual = decoded.encoded_layer(i), expected = source.encoded_layer(i);
            require(actual.sz == expected.sz, "PNG bytes");
            const auto *actual_bytes = static_cast<const uint8_t *>(actual.buf);
            const auto *expected_bytes = static_cast<const uint8_t *>(expected.buf);
            constexpr size_t chunk_size = size_t(1) << 20;
            for (size_t offset = 0; offset < expected.sz;) {
                check();
                const size_t length = std::min(chunk_size, expected.sz - offset);
                require(std::memcmp(actual_bytes + offset, expected_bytes + offset, length) == 0, "PNG bytes");
                offset += length;
                if (std::getenv("SLA_TRACE_EXPORT_STAGES"))
                    BOOST_LOG_TRIVIAL(info) << "SL1 encoded readback chunk: layer=" << i
                        << "; bytes=" << offset << "/" << expected.sz;
                check();
            }
        } else {
            const auto actual=decoded.image(i), expected=source.image(i);
            require(actual.cols==plan.geometry.columns && actual.rows==plan.geometry.rows &&
                actual.cols==expected.cols && actual.rows==expected.rows && actual.buf==expected.buf,"pixels");
        }
    }
    check();
}
};
} // namespace Slic3r::sla
