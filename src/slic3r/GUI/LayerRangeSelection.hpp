// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef slic3r_GUI_LayerRangeSelection_hpp_
#define slic3r_GUI_LayerRangeSelection_hpp_

#include <algorithm>
#include <cstdint>
#include <stdexcept>

namespace Slic3r::GUI {

enum class LayerRangeHandle { First, Last };

// Selection is view state: changing it must not edit masks or consume Undo.
// Keep the last interval when switching to one layer and back.
class LayerRangeSelection {
    int m_count, m_current = 1, m_first = 1, m_last;
    bool m_range = false;
public:
    explicit LayerRangeSelection(int count) : m_count(count), m_last(count)
    {
        if (count < 1) throw std::invalid_argument("Layer selection requires a nonempty document");
    }
    int count() const { return m_count; }
    int current() const { return m_current; }
    int first() const { return m_first; }
    int last() const { return m_last; }
    bool range_mode() const { return m_range; }
    void set_current(int value) { m_current = std::clamp(value, 1, m_count); }
    void set_range_mode(bool range) { m_range = range; }
    void set_bounds(int first, int last)
    {
        m_first = std::clamp(std::min(first, last), 1, m_count);
        m_last = std::clamp(std::max(first, last), 1, m_count);
    }
    void move_handle(LayerRangeHandle handle, int value)
    {
        if (!m_range) { set_current(value); return; }
        if (handle == LayerRangeHandle::First)
            m_current = m_first = std::clamp(value, 1, m_last);
        else
            m_current = m_last = std::clamp(value, m_first, m_count);
    }
    void step_handle(LayerRangeHandle handle, int delta)
    {
        const int value = !m_range ? m_current : handle == LayerRangeHandle::First ? m_first : m_last;
        move_handle(handle, int(std::clamp(int64_t(value) + delta, int64_t(1), int64_t(m_count))));
    }
};

// Offsets are relative to the top of the usable track. The bottom is layer 1.
// Round to the nearest layer, with widened arithmetic for large documents.
inline int layer_range_offset(int layer, int count, int travel)
{
    if (travel <= 0) return 0;
    if (count <= 1) return travel;
    return int((int64_t(count - std::clamp(layer, 1, count)) * travel + (count - 1) / 2) / (count - 1));
}
inline int layer_range_value(int offset, int count, int travel)
{
    if (count <= 1 || travel <= 0) return 1;
    return count - int((int64_t(std::clamp(offset, 0, travel)) * (count - 1) + travel / 2) / travel);
}

} // namespace Slic3r::GUI
#endif
