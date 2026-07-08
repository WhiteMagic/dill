#include "axis_mapping.h"

#include <algorithm>
#include <array>

namespace
{
    struct AxisDIOffset
    {
        DWORD                            axis_index;
        AxisOffset                       offset;
    };

    // Mapping from linear axis index to the DIJOYSTATE2 offset.
    const std::array<AxisDIOffset, 8> g_axis_di_offset_lookup =
    {{
        {1, DIJOFS_X},
        {2, DIJOFS_Y},
        {3, DIJOFS_Z},
        {4, DIJOFS_RX},
        {5, DIJOFS_RY},
        {6, DIJOFS_RZ},
        {7, DIJOFS_SLIDER(0)},
        {8, DIJOFS_SLIDER(1)},
    }};
}

DWORD axis_index_for_offset(AxisOffset offset)
{
    const auto it = std::find_if(
        g_axis_di_offset_lookup.cbegin(),
        g_axis_di_offset_lookup.cend(),
        [offset](AxisDIOffset const& slot) { return slot.offset == offset; }
    );
    return it != g_axis_di_offset_lookup.cend()
        ? it->axis_index
        : static_cast<DWORD>(-1);
}

AxisOffset offset_for_axis_index(DWORD axis_index)
{
    const auto it = std::find_if(
        g_axis_di_offset_lookup.cbegin(),
        g_axis_di_offset_lookup.cend(),
        [axis_index](AxisDIOffset const& slot) {
            return slot.axis_index == axis_index;
        }
    );
    return it != g_axis_di_offset_lookup.cend()
        ? it->offset
        : static_cast<AxisOffset>(-1);
}

void build_axis_map(
    std::vector<AxisOffset> const&      detected_offsets,
    DWORD&                              axis_count,
    AxisMap                             (&axis_map)[8]
)
{
    axis_count = 0;
    for(auto& entry : axis_map)
    {
        entry = {0, 0};
    }

    for(auto const& slot : g_axis_di_offset_lookup)
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
