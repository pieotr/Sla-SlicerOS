///|/ Copyright (c) Prusa Research 2020 - 2023 Oleksandra Iushchenko @YuSanka, Vojtěch Bubník @bubnikv, Tomáš Mészáros @tamasmeszaros, Lukáš Matěna @lukasmatena, Enrico Turri @enricoturri1966
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "DoubleSliderForLayers.hpp"

#include <boost/format.hpp>
#include <cmath>
#include <algorithm>
#include <array>
#include <cstdio>

#include "libslic3r/SLA/StatisticsPresentation.hpp"
#include "libslic3r/format.hpp" // -> format()
#include "I18N.hpp"
#include "ImGuiWrapper.hpp"
#include "libslic3r/libslic3r.h"
#include "slic3r/GUI/ImGuiDoubleSlider.hpp"
#include "slic3r/GUI/ImGuiPureWrap.hpp"
#include "slic3r/GUI/RulerForDoubleSlider.hpp"

#ifndef IMGUI_DEFINE_MATH_OPERATORS
#define IMGUI_DEFINE_MATH_OPERATORS
#endif
#include <imgui/imgui_internal.h>

using namespace Slic3r;
using Slic3r::format;

namespace DoubleSlider {

DSForLayers::DSForLayers(   int lowerValue,
                            int higherValue,
                            int minValue,
                            int maxValue,
                            bool allow_editing) :
    m_allow_editing(allow_editing)
{
    Init(lowerValue, higherValue, minValue, maxValue, "layers_slider", false);
    m_ctrl.ShowLabelOnMouseMove(true);

    m_ctrl.set_get_label_on_move_cb([this](int pos) {
        m_pos_on_move = pos; 
        return m_show_estimated_times ? get_label(pos, ltEstimatedTime) : "";
    });
    m_ctrl.set_extra_draw_cb([this](const ImRect& draw_rc) {return draw_ticks(draw_rc); });
}

void DSForLayers::SetLayersTimes(const std::vector<double>& layers_times, bool excludes_printer_setup)
{ 
    m_time_excludes_printer_setup = excludes_printer_setup;
    m_layers_times = layers_times;
}

using namespace ImGui;

void DSForLayers::draw_ticks(const ImRect& slideable_region)
{
    if (m_show_ruler)
        draw_ruler(slideable_region);
}

void DSForLayers::draw_ruler(const ImRect& slideable_region)
{
    if (m_values.empty())
        return;

    const double step = double(slideable_region.GetHeight()) / (m_ctrl.GetMaxPos() - m_ctrl.GetMinPos());

    if (!m_ruler.valid())
        m_ruler.init(m_values, step);

    const float inner_x         = 11.f * m_scale;
    const float long_outer_x    = 17.f * m_scale;
    const float short_outer_x   = 14.f * m_scale;
    const float tick_width      = float(int(1.0f * m_scale +0.5f));
    const float label_height    = m_imgui->GetTextureCustomRect(ImGui::DSRevert)->Height;

    constexpr ImU32 tick_clr = IM_COL32(255, 255, 255, 255);

    const float x_center = slideable_region.GetCenter().x;

    double max_val = 0.;
    for (const auto& val : m_ruler.max_values)
        if (max_val < val)
            max_val = val;

    if (m_show_ruler_bg) {
        // draw ruler BG
        ImRect bg_rect = slideable_region;
        bg_rect.Expand(ImVec2(0.f, long_outer_x));
        bg_rect.Min.x -= tick_width;
        bg_rect.Max.x = m_ctrl.GetCtrlPos().x + GetWidth();
        bg_rect.Min.y = m_ctrl.GetCtrlPos().y + label_height;
        bg_rect.Max.y = m_ctrl.GetCtrlPos().y + GetHeight() - label_height;
        const ImU32 bg_color = ImGui::ColorConvertFloat4ToU32(ImVec4(0.13f, 0.13f, 0.13f, 0.5f));

        ImGui::RenderFrame(bg_rect.Min, bg_rect.Max, bg_color, false, 2.f * m_ctrl.rounding());
    }

    auto get_tick_pos = [this, slideable_region](int tick) -> float {
        return m_ctrl.GetPositionInRect(tick, slideable_region);
    };

    auto draw_text = [max_val, x_center, label_height,  long_outer_x, this](const int tick, const float tick_pos)
    {
        ImVec2 start = ImVec2(x_center + long_outer_x + 1, tick_pos - (0.5f * label_height));
        std::string label = get_label(tick, ltHeight, max_val > 100.0 ? "%1$.1f" : "%1$.2f");
        ImGui::RenderText(start, label.c_str());
    };

    auto draw_tick = [x_center, tick_width, inner_x](const float tick_pos, const float outer_x)
    {
        ImRect tick_right = ImRect(x_center + inner_x, tick_pos - tick_width, x_center + outer_x, tick_pos);
        ImGui::RenderFrame(tick_right.Min, tick_right.Max, tick_clr, false);
    };

    auto draw_short_ticks = [this, short_outer_x, draw_tick, get_tick_pos](double& current_tick, int max_tick) 
    {
        if (m_ruler.short_step <= 0.0)
            return;
        while (current_tick < max_tick) {
            float pos = get_tick_pos(lround(current_tick));
            draw_tick(pos, short_outer_x);
            current_tick += m_ruler.short_step;
            if (current_tick > m_ctrl.GetMaxPos())
                break;
        }
    };

    double short_tick = NaNd;
    int tick = 0;
    double value = 0.0;
    size_t sequence = 0;
    float prev_y_pos = -1.f;
    int values_size = (int)m_values.size();

    const float label_shift = 0.5f * label_height;

    if (m_ruler.long_step < 0) {
        // sequential print when long_step wasn't detected because of a lot of printed objects 
        if (m_ruler.max_values.size() > 1) {
            float last_pos = get_tick_pos(m_ctrl.GetMaxPos());
            while (tick <= m_ctrl.GetMaxPos() && sequence < m_ruler.count()) {
                // draw just ticks with max value
                value = m_ruler.max_values[sequence];
                short_tick = tick;

                for (; tick < values_size; tick++) {
                    if (m_values[tick] == value)
                        break;
                    if (m_values[tick] > value) {
                        if (tick > 0)
                            tick--;
                        break;
                    }
                }
                if (tick > m_ctrl.GetMaxPos())
                    break;

                float pos = get_tick_pos(tick);
                draw_tick(pos, long_outer_x);
                if (prev_y_pos < 0 || pos == last_pos || (prev_y_pos - pos >= label_shift && pos - last_pos >= label_shift)) {
                    draw_text(tick, pos);
                    prev_y_pos = pos;
                }
                draw_short_ticks(short_tick, tick);

                sequence++;
                tick++;
            }
        }
        // very short object or some non-trivial ruler with non-regular step (see https://github.com/prusa3d/PrusaSlicer/issues/7263)
        else {
            if (step < 1) // step less then 1 px indicates very tall object with non-regular laayer step (probably in vase mode)
                return;
            for (size_t tick = 1; tick < m_values.size(); tick++) {
                float pos = get_tick_pos(tick);
                draw_tick(pos, long_outer_x);
                draw_text(tick, pos);
            }
        }
    }
    else {
        std::vector<int> last_positions; 
        if (m_ruler.count() == 1)
            last_positions.emplace_back(m_ctrl.GetMaxPos());
        else {
            // fill last positions for each object in sequential print
            last_positions.reserve(m_ruler.count());

            int tick = 0;
            double value = 0.0;
            size_t sequence = 0;

            while (tick <= m_ctrl.GetMaxPos()) {
                value += m_ruler.long_step;

                if (sequence < m_ruler.count() && value > m_ruler.max_values[sequence])
                    value = m_ruler.max_values[sequence];

                for (; tick < values_size; tick++) {
                    if (m_values[tick] == value)
                        break;
                    if (m_values[tick] > value) {
                        if (tick > 0)
                            tick--;
                        break;
                    }
                }
                if (tick > m_ctrl.GetMaxPos())
                    break;

                if (sequence < m_ruler.count() && value == m_ruler.max_values[sequence]) {
                    last_positions.emplace_back(tick);
                    value = 0.0;
                    sequence++;
                    tick++;
                }
            }
        }

        float last_pos = get_tick_pos(last_positions[sequence]);

        while (tick <= m_ctrl.GetMaxPos()) {
            value += m_ruler.long_step;

            if (sequence < m_ruler.count() && value > m_ruler.max_values[sequence])
                value = m_ruler.max_values[sequence];

            short_tick = tick;

            for (; tick < values_size; tick++) {
                if (m_values[tick] == value)
                    break;
                if (m_values[tick] > value) {
                    if (tick > 0)
                        tick--;
                    break;
                }
            }
            if (tick > m_ctrl.GetMaxPos())
                break;

            float pos = get_tick_pos(tick);
            draw_tick(pos, long_outer_x);
            if (prev_y_pos < 0 || pos == last_pos || (prev_y_pos - pos >= label_shift && pos - last_pos >= label_shift) ) {
                draw_text(tick, pos);
                prev_y_pos = pos;
            }

            draw_short_ticks(short_tick, tick);

            if (sequence < m_ruler.count() && value == m_ruler.max_values[sequence]) {
                value = 0.0;
                sequence++;
                tick++;

                if (sequence < m_ruler.count())
                    last_pos = get_tick_pos(last_positions[sequence]);
            }
        }
        // short ticks from the last tick to the end 
        draw_short_ticks(short_tick, m_ctrl.GetMaxPos());
    }

    // draw mose move line
    if (m_pos_on_move > 0) {
        float line_pos = get_tick_pos(m_pos_on_move);

        ImRect move_line = ImRect(x_center + 0.75f * inner_x, line_pos - tick_width, x_center + 1.5f * long_outer_x, line_pos);
        ImGui::RenderFrame(move_line.Min, move_line.Max, ImGui::ColorConvertFloat4ToU32(ImGuiPureWrap::COL_ORANGE_LIGHT), false);
        m_pos_on_move = -1;
    }
}

void DSForLayers::render_menu()
{
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 10.0f) * m_scale);
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 4.0f * m_scale);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, { 1, ImGui::GetStyle().ItemSpacing.y });
    ImGui::PushStyleVar(ImGuiStyleVar_::ImGuiStyleVar_ChildRounding, 4.0f * m_scale);

    ImGui::PushStyleColor(ImGuiCol_::ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));

    if (m_show_cog_menu)
        ImGui::OpenPopup("cog_menu_popup");

    render_cog_menu();

    ImGui::PopStyleColor(1);
    ImGui::PopStyleVar(4);

    ImGuiContext& context = *GImGui;
    if (context.IO.MouseReleased[0])
        m_show_cog_menu = false;
}

void DSForLayers::render_cog_menu()
{
    const ImVec2 icon_sz = ImVec2(14, 14);
    if (ImGui::BeginPopup("cog_menu_popup")) {
        if (ImGuiPureWrap::menu_item_with_icon(_u8L("Jump to height").c_str(), "Shift+G")) {
            jump_to_value();
        }
        if (ImGuiPureWrap::menu_item_with_icon(_u8L("Show estimated print time on hover").c_str(), "", icon_sz, 0, m_show_estimated_times)) {
            m_show_estimated_times = !m_show_estimated_times;
            if (m_cb_change_app_config)
                m_cb_change_app_config("show_estimated_times_in_dbl_slider", m_show_estimated_times ? "1" : "0");
        }
        if (ImGuiPureWrap::begin_menu(_u8L("Ruler").c_str())) {
            if (ImGuiPureWrap::menu_item_with_icon(_u8L("Show").c_str(), "", icon_sz, 0, m_show_ruler)) {
                m_show_ruler = !m_show_ruler;
                if (m_show_ruler)
                    m_imgui->set_requires_extra_frame();
                if (m_cb_change_app_config)
                    m_cb_change_app_config("show_ruler_in_dbl_slider", m_show_ruler ? "1" : "0");
            }

            if (ImGuiPureWrap::menu_item_with_icon(_u8L("Show background").c_str(), "", icon_sz, 0, m_show_ruler_bg)) {
                m_show_ruler_bg = !m_show_ruler_bg;
                if (m_cb_change_app_config)
                    m_cb_change_app_config("show_ruler_bg_in_dbl_slider", m_show_ruler_bg ? "1" : "0");
            }

            ImGuiPureWrap::end_menu();
        }

        ImGui::EndPopup();
    }
}

bool DSForLayers::render_button(const wchar_t btn_icon, const wchar_t btn_icon_hovered, const std::string& label_id, const ImVec2& pos, FocusedItem focus, int tick /*= -1*/)
{
    ImGui::PushStyleVar(ImGuiStyleVar_::ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_::ImGuiStyleVar_FramePadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_::ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));

    ImGui::PushStyleColor(ImGuiCol_::ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_Text));

    int windows_flag =    ImGuiWindowFlags_NoTitleBar
                        | ImGuiWindowFlags_NoCollapse
                        | ImGuiWindowFlags_NoMove
                        | ImGuiWindowFlags_NoResize
                        | ImGuiWindowFlags_NoScrollbar
                        | ImGuiWindowFlags_NoScrollWithMouse
                        | ImGuiWindowFlags_NoFocusOnAppearing;

    ImGuiPureWrap::set_next_window_pos(pos.x, pos.y, ImGuiCond_Always);
    std::string win_name = label_id + "##btn_win";
    ImGuiPureWrap::begin(win_name, windows_flag);

    ImGuiContext& g = *GImGui;

    m_focus = focus;
    std::string tooltip = m_allow_editing ? get_tooltip(tick) : "";
    ImGui::SetCursorPos(ImVec2(0, 0));
    const bool ret = m_imgui->image_button(g.HoveredWindow == g.CurrentWindow ? btn_icon_hovered : btn_icon, tooltip, false);

    ImGuiPureWrap::end();

    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(3);

    return ret;
}

bool DSForLayers::render_jump_to_window(const ImVec2& pos, double* active_value, double min_z, double max_z)
{
    if (!m_show_get_jump_value)
        return false;

    const std::string   msg_text    = _u8L("Enter the height you want to jump to") + ":";
    const std::string   win_name    = _u8L("Jump to height") + "##btn_win";
    const ImVec2        msg_size    = ImGui::CalcTextSize(msg_text.c_str(), NULL, true);

    const float         ctrl_pos_x  = msg_size.x + 15 * m_scale;
    const float         ctrl_width  = 50.f * m_scale;

    ImGui::SetNextWindowPos(pos, ImGuiCond_Always);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, { 12.0f, 8.0f });

    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.13f, 0.13f, 0.13f, 0.8f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_Text));

    ImGuiWindowFlags windows_flag =   ImGuiWindowFlags_NoCollapse
                                    | ImGuiWindowFlags_NoMove
                                    | ImGuiWindowFlags_NoResize
                                    | ImGuiWindowFlags_NoScrollbar
                                    | ImGuiWindowFlags_NoScrollWithMouse;

    ImGui::Begin(win_name.c_str(), &m_show_get_jump_value, windows_flag);

    ImGui::AlignTextToFramePadding();
    ImGui::Text("%s", msg_text.c_str());
    ImGui::SameLine(ctrl_pos_x);
    ImGui::PushItemWidth(ctrl_width);

    ImGui::InputDouble("##jump_to", active_value, 0.0, 0.0, "%.2f", ImGuiInputTextFlags_CharsDecimal | ImGuiInputTextFlags_AutoSelectAll);
    //check if Enter was pressed
    bool enter_pressed = ImGui::IsItemDeactivatedAfterEdit();

    //check out of range
    bool disable_ok = *active_value < min_z || *active_value > max_z;

    ImGui::Text("%s", "");
    ImGui::SameLine(ctrl_pos_x);

    if (disable_ok) {
        ImGui::PushItemFlag(ImGuiItemFlags_Disabled, true);
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.5f);
    }

    bool ok_pressed = ImGui::Button("OK##jump_to", ImVec2(ctrl_width, 0.f));

    if (disable_ok) {
        ImGui::PopItemFlag();
        ImGui::PopStyleVar();
    }

    ImGui::End();

    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(3);

    return enter_pressed || ok_pressed;
}

void DSForLayers::Render(const int canvas_width, const int canvas_height, float extra_scale/* = 0.1f*/, float offset /*= 0.f*/)
{
    if (!m_ctrl.IsShown())
        return;
    m_scale = extra_scale * 0.1f * m_em;

    m_ruler.set_scale(m_scale);

    const float action_btn_sz   = m_imgui->GetTextureCustomRect(ImGui::DSRevert)->Height;
    const float tick_icon_side  = m_imgui->GetTextureCustomRect(ImGui::DSRevert)->Height;

    ImVec2 pos;

    float VERTICAL_SLIDER_WIDTH = m_show_ruler ? 125.f : 105.0f;
    // Hover labels share the slider's clipped ImGui window. Reserve space for
    // the complete SLA elapsed time, including scientific notation, before
    // placing that window; widening only the painted label still clips it.
    if (m_show_estimated_times && !m_layers_times.empty() && m_scale > 0.f) {
        const std::string elapsed = sla::statistics_time(m_layers_times.back());
        VERTICAL_SLIDER_WIDTH = std::max(VERTICAL_SLIDER_WIDTH,
            ImGui::CalcTextSize(elapsed.c_str()).x / m_scale + 80.f);
    }

    pos.x = canvas_width - VERTICAL_SLIDER_WIDTH * m_scale - tick_icon_side;
    pos.y = 1.5f * action_btn_sz + offset;
    if (m_allow_editing)
        pos.y += 2.f;

    ImVec2 size = ImVec2(VERTICAL_SLIDER_WIDTH * m_scale, canvas_height - 4.f * action_btn_sz - offset);

    m_ctrl.Init(pos, size, m_scale, m_show_ruler);
    if (m_ctrl.render()) {
        // request one more frame if value was changes with mouse wheel
        if (GImGui->IO.MouseWheel != 0.0f)
            m_imgui->set_requires_extra_frame();
        process_thumb_move();

        // discard all getters dialogs
        m_show_get_jump_value = false;
    }

    // draw action buttons

    const float groove_center_x = m_ctrl.GetGrooveRect().GetCenter().x;

    ImVec2 btn_pos = ImVec2(groove_center_x - 0.5f * action_btn_sz, pos.y - 0.75f * action_btn_sz);

    btn_pos.y += 0.5f * action_btn_sz + size.y;
    const bool is_one_layer = m_ctrl.IsCombineThumbs();
    if (render_button(is_one_layer ? ImGui::Lock : ImGui::Unlock, is_one_layer ? ImGui::LockHovered : ImGui::UnlockHovered, "one_layer", btn_pos, fiOneLayerIcon))
        ChangeOneLayerLock();

    btn_pos.y += 1.2f * action_btn_sz;
    if (render_button(ImGui::DSSettings, ImGui::DSSettingsHovered, "settings", btn_pos, fiCogIcon)) {
        m_show_cog_menu = true;
    }

    render_menu();

    if (render_jump_to_window(ImVec2(0.5f * canvas_width, 0.5f*canvas_height), &m_jump_to_value,
                              m_values[m_ctrl.GetMinPos()], m_values[m_ctrl.GetMaxPos()]))
        process_jump_to_value();
}

void DSForLayers::force_ruler_update()
{
    m_ruler.invalidate();
}

static std::string short_and_splitted_time(const std::string& time)
{
    // Parse the dhms time format.
    int days = 0;
    int hours = 0;
    int minutes = 0;
    int seconds = 0;
    if (time.find('d') != std::string::npos)
        ::sscanf(time.c_str(), "%dd %dh %dm %ds", &days, &hours, &minutes, &seconds);
    else if (time.find('h') != std::string::npos)
        ::sscanf(time.c_str(), "%dh %dm %ds", &hours, &minutes, &seconds);
    else if (time.find('m') != std::string::npos)
        ::sscanf(time.c_str(), "%dm %ds", &minutes, &seconds);
    else if (time.find('s') != std::string::npos)
        ::sscanf(time.c_str(), "%ds", &seconds);

    // Format the dhm time.
    auto get_d = [days]()   { return format(_u8L("%1%d"), days); };
    auto get_h = [hours]()  { return format(_u8L("%1%h"), hours); };
    auto get_m = [minutes](){ return format(_u8L("%1%m"), minutes); };
    auto get_s = [seconds](){ return format(_u8L("%1%s"), seconds); };

    if (days > 0)
        return format("%1%%2%\n%3%", get_d(), get_h(), get_m());
    if (hours > 0) {
        if (hours < 10 && minutes < 10 && seconds < 10)
            return format("%1%%2%%3%", get_h(), get_m(), get_s());
        if (hours > 10 && minutes > 10 && seconds > 10)
            return format("%1%\n%2%\n%3%", get_h(), get_m(), get_s());
        if ((minutes < 10 && seconds > 10) || (minutes > 10 && seconds < 10))
            return format("%1%\n%2%%3%", get_h(), get_m(), get_s());
        return format("%1%%2%\n%3%", get_h(), get_m(), get_s());
    }
    if (minutes > 0) {
        if (minutes > 10 && seconds > 10)
            return format("%1%\n%2%", get_m(), get_s());
        return format("%1%%2%", get_m(), get_s());
    }
    return get_s();
}

std::string DSForLayers::get_label(int pos, LabelType label_type, const std::string& fmt/* = "%1$.2f"*/) const
{
    const size_t value = pos;

    if (m_values.empty())
        return format("%1%", pos);
    if (value >= m_values.size())
        return "ErrVal";

    if (label_type == ltEstimatedTime) {
        if (value >= m_layers_times.size()) return "";
        const double seconds = m_layers_times[value];
        const auto text = sla::statistics_time(seconds);
        return sla::statistics_time_is_dhms(seconds) ? short_and_splitted_time(text) : text;
    }
    std::string str = format(fmt, m_values[value]);
    if (label_type == ltHeight)
        return str;
    if (label_type == ltHeightWithLayer)
        return format("%1%\n(%2%)", str, value + 1);

    return "";
}

void DSForLayers::ChangeOneLayerLock()
{
    m_ctrl.CombineThumbs(!m_ctrl.IsCombineThumbs()); 
    process_thumb_move();
}

std::string DSForLayers::get_tooltip(int tick/*=-1*/)
{
    if (m_focus == fiNone)
        return "";
    if (m_focus == fiOneLayerIcon)
        return _u8L("One layer mode");
    if (m_focus == fiCogIcon)
        return (boost::format(_u8L("Jump to height %s\n"
                                   "or Set ruler mode")) % "(Shift + G)").str();

    return m_time_excludes_printer_setup ?
        _u8L("Elapsed layer schedule time; printer setup is excluded.") :
        _u8L("Estimated elapsed printing time at this layer.");
}

void DSForLayers::jump_to_value()
{
    //Init "jump to value";
    m_show_get_jump_value = true;
    m_jump_to_value = m_values[m_ctrl.GetActivePos()];

    m_imgui->set_requires_extra_frame();
}

void DSForLayers::process_jump_to_value()
{
    auto it = std::lower_bound(m_values.begin(), m_values.end(), m_jump_to_value - 1e-6);
    if (it == m_values.end())
        return;
    const int tick_value = int(it - m_values.begin());
    if (tick_value > 0) {
        m_show_get_jump_value = false;

        if (m_ctrl.IsActiveHigherThumb())
            SetHigherPos(tick_value);
        else
            SetLowerPos(tick_value);
    }
}

} // DoubleSlider
