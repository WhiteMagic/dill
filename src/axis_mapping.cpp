#include "axis_mapping.h"

#include <algorithm>
#include <iterator>

namespace
{
    struct AxisSlot
    {
        DWORD                            axis_index;
        AxisOffset                       offset;
    };

    // Canonical axis_index <-> DIJOYSTATE2 offset table.
    const AxisSlot g_axis_slots[8] =
    {
        {1, DIJOFS_X},
        {2, DIJOFS_Y},
        {3, DIJOFS_Z},
        {4, DIJOFS_RX},
        {5, DIJOFS_RY},
        {6, DIJOFS_RZ},
        {7, DIJOFS_SLIDER(0)},
        {8, DIJOFS_SLIDER(1)},
    };
}

DWORD axis_index_for_offset(AxisOffset offset)
{
    const auto it = std::find_if(
        std::begin(g_axis_slots),
        std::end(g_axis_slots),
        [offset](AxisSlot const& slot) { return slot.offset == offset; }
    );
    return it != std::end(g_axis_slots) ? it->axis_index : 0;
}

AxisOffset offset_for_axis_index(DWORD axis_index)
{
    const auto it = std::find_if(
        std::begin(g_axis_slots),
        std::end(g_axis_slots),
        [axis_index](AxisSlot const& slot) {
            return slot.axis_index == axis_index;
        }
    );
    return it != std::end(g_axis_slots) ? it->offset : 0;
}

void build_axis_map(
    std::vector<AxisOffset> const& detected_offsets,
    DWORD& axis_count,
    AxisMap (&axis_map)[8]
)
{
    axis_count = 0;
    for(auto& entry : axis_map)
    {
        entry = {0, 0};
    }

    for(auto const& slot : g_axis_slots)
    {
        const bool detected = std::find(
            detected_offsets.cbegin(),
            detected_offsets.cend(),
            slot.offset
        ) != detected_offsets.cend();

        if(detected)
        {
            axis_map[axis_count] = {axis_count + 1, slot.axis_index};
            ++axis_count;
        }
    }
}
