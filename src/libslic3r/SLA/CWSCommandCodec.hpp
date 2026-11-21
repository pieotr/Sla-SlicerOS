// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "FreshLayerJob.hpp"
#include "libslic3r/Exception.hpp"
#include <array>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <string>
#include <vector>

namespace Slic3r::sla {
// Executable commands, independently of absent/derived CWS global getters.
// Speeds belonging to an inactive phase are intentionally absent here.
struct CWSMove { double distance=0, speed=0; };
struct CWSCommandRow {
    double position_z=0;
    bool z_advance_commit=false;
    std::vector<CWSMove> lift, retract;
    std::array<uint32_t,3> waits_ms{}; // after lift, before cure, after cure
    uint32_t exposure_ms=0;
    unsigned pwm=0;
};
namespace cws_command_detail {
[[noreturn]] inline void fail(size_t layer, const char *field, const char *reason)
{
    throw RuntimeError("CWS cannot preserve layer "+std::to_string(layer+1)+"."+field+": "+reason);
}
inline uint32_t milliseconds(double seconds, size_t layer, const char *field)
{
    const double value=seconds*1000., rounded=std::round(value);
    const double tolerance=4*std::numeric_limits<double>::epsilon()*std::max(1.,std::abs(value));
    if (!std::isfinite(value) || value<0 || value>UINT32_MAX || std::abs(value-rounded)>tolerance)
        fail(layer,field,"outside exact uint32 millisecond range");
    return uint32_t(rounded);
}
inline void native_positive(double value, size_t layer, const char *field)
{
    if (!std::isfinite(value) || value<=0 || !std::isfinite(float(value)) || float(value)<=0)
        fail(layer,field,"movement requires a finite positive native value");
}
inline std::string decimal(double value)
{
    // UVtools' command regex does not accept exponents. 64 decimal places
    // cover every positive native float with full double significant digits.
    std::ostringstream out;out.imbue(std::locale::classic());
    out<<std::fixed<<std::setprecision(64)<<value;
    auto text=out.str();while(text.back()=='0')text.pop_back();
    if(text.back()=='.')text.pop_back();
    return text;
}
inline uint32_t unsigned_number(const std::string &text)
{
    if(text.empty())throw RuntimeError("CWS command requires an unsigned integer");
    uint64_t value=0;
    for(char c:text) {
        if(c<'0' || c>'9' || value>(UINT32_MAX-unsigned(c-'0'))/10)
            throw RuntimeError("Invalid CWS command integer");
        value=value*10+unsigned(c-'0');
    }
    return uint32_t(value);
}
inline bool prefix(const std::string &value,const char *start)
{ return value.rfind(start,0)==0; }
} // namespace cws_command_detail

inline std::string cws_encode_command_rows(const FreshLayerJob &job)
{
    using F=LayerParameter;using namespace cws_command_detail;
    job.validate(job.rows.size());
    std::ostringstream out;out.imbue(std::locale::classic());
    out<<"G21\nG91\nM17\nM106 S0\n;<Slice> Blank\nG28 Z\nM400\n";
    double previous=0;
    for(size_t i=0;i<job.rows.size();++i) {
        const auto &row=job.rows[i];auto p=[&](F f){return row.at(f);};
        if(p(F::LightOff)!=0)fail(i,"LightOff","independent light-off delay has no command field");
        native_positive(p(F::PositionZ),i,"PositionZ");
        const auto exposure=milliseconds(p(F::Exposure),i,"Exposure");
        const auto before=milliseconds(p(F::WaitBefore),i,"WaitBefore");
        const auto after=milliseconds(p(F::WaitAfter),i,"WaitAfter");
        const auto after_lift=milliseconds(p(F::WaitAfterLift),i,"WaitAfterLift");
        const double lift=p(F::LiftDistance),lift2=p(F::LiftDistance2),total=lift+lift2;
        out<<";LAYER_START:"<<i<<"\n;PositionZ:"<<decimal(p(F::PositionZ))<<"mm\n";
        auto move=[&](double distance,double speed,const char *field) {
            if(distance==0)return 0.;
            native_positive(std::abs(distance),i,field);native_positive(speed,i,field);
            const double seconds=std::abs(distance)*60/speed;
            if(!std::isfinite(seconds))fail(i,field,"movement duration is not finite");
            out<<"G1 Z"<<decimal(distance)<<" F"<<decimal(speed)<<'\n';return seconds;
        };
        auto sync=[&](double seconds) {
            if(!std::isfinite(seconds) || seconds<0 || seconds*1000.>UINT32_MAX)
                fail(i,"MotionSync","outside uint32 millisecond range");
            out<<";<Delay> 0"<<uint32_t(std::round(seconds*1000.))<<"\nM400\n";
        };
        const double delta=p(F::PositionZ)-previous;
        double duration=0;
        if(lift>0) {
            duration=move(delta+lift,p(F::LiftSpeed),"LiftSpeed");
            duration+=move(lift2,p(F::LiftSpeed2),"LiftSpeed2");
        } else duration=move(delta+lift2,p(lift2>0?F::LiftSpeed2:F::LiftSpeed),"PositionZ");
        sync(duration);
        // With no peel/retract, UVtools otherwise classifies cure delays as
        // after-lift waits. Commit the Z-only prelude, then resume the same
        // image index. Comments change parser state, never physical motion.
        if(total==0)out<<";CWS_Z_ADVANCE_COMMIT:"<<i<<"\n;LAYER_START:"<<i<<'\n';
        if(after_lift)out<<";<Delay> "<<after_lift<<'\n';
        duration=move(-(total-p(F::RetractDistance2)),p(F::RetractSpeed),"RetractSpeed");
        duration+=move(-p(F::RetractDistance2),p(F::RetractSpeed2),"RetractSpeed2");
        sync(duration);
        out<<";<Delay> "<<before<<"\n;<Slice> "<<i<<'\n';
        // A dark exposure still consumes the requested timer interval.
        // PWM controls light power; it must not silently remove a delay.
        if(exposure)
            out<<"M106 S"<<unsigned(p(F::LightPWM))<<"\n;<Delay> "<<exposure<<"\nM106 S0\n";
        out<<";<Slice> Blank\n";
        if(after)out<<";<Delay> "<<after<<'\n';
        out<<";LAYER_END:"<<i<<'\n';previous=p(F::PositionZ);
        if(out.tellp()>std::streamoff(64*1024*1024))throw RuntimeError("CWS command stream exceeds the 64 MiB budget");
    }
    out<<"M106 S0\n;<Slice> Blank\nM400\nM18\n;<Completed>\n";
    return out.str();
}

// Strict reader for our owned relative-command dialect. It derives final Z
// from movements, not PositionZ comments or the source descriptor. General
// arbitrary CWS import/editing is a separate reader capability.
inline std::vector<CWSCommandRow> cws_decode_command_rows(const std::string &commands)
{
    using namespace cws_command_detail;
    if(commands.size()>64*1024*1024)throw RuntimeError("CWS command stream exceeds the 64 MiB budget");
    std::istringstream input(commands);input.imbue(std::locale::classic());
    std::vector<std::string> lines;std::string line;
    while(std::getline(input,line)) { if(!line.empty() && line.back()=='\r')line.pop_back();lines.push_back(line); }
    std::vector<CWSCommandRow> rows;CWSCommandRow row;
    bool units=false,relative=false,in_layer=false,exposing=false,pulse_seen=false,completed=false;
    unsigned stage=0;long double position=0;bool commit_pending=false;
    auto invalid=[](){throw RuntimeError("Malformed owned CWS command sequence");};
    for(size_t n=0;n<lines.size();++n) {
        const auto &text=lines[n];
        if(text.empty())continue;
        if(text=="G21") {if(in_layer || units)invalid();units=true;continue;}
        if(text=="G91") {if(in_layer || relative)invalid();relative=true;continue;}
        if(text=="G90" || text=="G20")invalid();
        if(prefix(text,";CWS_Z_ADVANCE_COMMIT:")) {
            if(!in_layer || stage!=1 || row.z_advance_commit || row.lift.size()!=1 || !row.retract.empty() ||
                row.waits_ms!=std::array<uint32_t,3>{} || pulse_seen ||
                unsigned_number(text.substr(22))!=rows.size() || n+1>=lines.size() ||
                lines[n+1]!=";LAYER_START:"+std::to_string(rows.size()))invalid();
            commit_pending=true;continue;
        }
        if(prefix(text,";LAYER_START:")) {
            if(in_layer && commit_pending) {
                if(unsigned_number(text.substr(13))!=rows.size())invalid();
                commit_pending=false;row.z_advance_commit=true;continue;
            }
            if(in_layer || completed || !units || !relative || rows.size()>=100000 ||
                unsigned_number(text.substr(13))!=rows.size())invalid();
            in_layer=true;row={};stage=0;exposing=false;pulse_seen=false;continue;
        }
        if(prefix(text,";LAYER_END:")) {
            if(!in_layer || stage!=4 || exposing || unsigned_number(text.substr(11))!=rows.size() ||
                !std::isfinite(double(position)) || position<= (rows.empty()?0:rows.back().position_z))invalid();
            row.position_z=double(position);rows.push_back(std::move(row));in_layer=false;continue;
        }
        if(!in_layer) {
            if(text==";<Completed>") {if(completed || rows.empty())invalid();completed=true;continue;}
            if(completed)invalid();
            if(text=="M17" || text=="M18" || text=="M400" || text=="M106 S0" || text==";<Slice> Blank")continue;
            if(text=="G28 Z") {if(!rows.empty())invalid();position=0;continue;}
            if(prefix(text,";(" ) || prefix(text,"; SLA-Slicer"))continue;
            invalid();
        }
        if(prefix(text,";PositionZ:"))continue;
        if(text=="M400")continue;
        if(prefix(text,"G1 Z")) {
            if(stage>1)invalid();
            std::istringstream values(text.substr(4));values.imbue(std::locale::classic());
            double distance=0,speed=0;char f=0;std::string trailing;
            if(!(values>>distance>>f>>speed) || f!='F' || values>>trailing || !std::isfinite(distance) ||
                !std::isfinite(speed) || !std::isfinite(float(distance)) || !std::isfinite(float(speed)) ||
                float(std::abs(distance))<=0 || float(speed)<=0 || (stage==0?distance<0:distance>0))invalid();
            auto &moves=stage==0?row.lift:row.retract;
            if(moves.size()>=2)invalid();
            moves.push_back({distance,speed});position+=distance;continue;
        }
        if(prefix(text,";<Delay> ")) {
            const auto token=text.substr(9);
            const uint32_t ms=unsigned_number(token);
            const bool sync=!token.empty() && token.front()=='0' &&
                (token.size()>1 || (n+1<lines.size() && lines[n+1]=="M400"));
            if(sync) {
                if(stage>1 || (stage==0 && row.lift.empty()) || n+1>=lines.size() || lines[n+1]!="M400")invalid();
                ++stage;++n;continue;
            }
            uint32_t *target=nullptr;
            if(stage==1)target=&row.waits_ms[0];
            else if(stage==2)target=&row.waits_ms[1];
            else if(stage==3 && exposing)target=&row.exposure_ms;
            else if(stage==4)target=&row.waits_ms[2];
            else invalid();
            if(ms>UINT32_MAX-*target)invalid();
            *target+=ms;continue;
        }
        if(prefix(text,";<Slice> ")) {
            if(text==";<Slice> Blank") {if(stage!=3 || exposing)invalid();stage=4;}
            else {if(stage!=2 || unsigned_number(text.substr(9))!=rows.size())invalid();stage=3;}
            continue;
        }
        if(prefix(text,"M106 S")) {
            const auto pwm=unsigned_number(text.substr(6));
            if(stage!=3 || pwm>255)invalid();
            if(!pulse_seen) {row.pwm=pwm;exposing=pulse_seen=true;}
            else {if(pwm || !exposing || !row.exposure_ms)invalid();exposing=false;}
            continue;
        }
        invalid();
    }
    if(in_layer || !completed || rows.empty())invalid();
    return rows;
}
} // namespace Slic3r::sla
