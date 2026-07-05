#pragma once

#define WIN32_LEAN_AND_MEAN
#define DIRECTINPUT_VERSION 0x0800

#include <vector>

#include <windows.h>
#include <dinput.h>

#include "dill.h"

//! Byte offset of a single axis object within a DIJOYSTATE2 struct
using AxisOffset = DWORD;

/**
 * \brief Maps a DIJOYSTATE2 axis offset to DILL's axis_index.
 *
 * \param offset DIJOYSTATE2 byte offset, e.g. DIJOFS_X
 * \return axis_index (1-8), or 0 if unrecognized
 */
DWORD axis_index_for_offset(AxisOffset offset);

/**
 * \brief Maps a DILL axis_index to its DIJOYSTATE2 byte offset.
 *
 * \param axis_index DILL axis index (1-8)
 * \return DIJOYSTATE2 byte offset, or 0 if out of range
 */
AxisOffset offset_for_axis_index(DWORD axis_index);

/**
 * \brief Builds axis_count and axis_map from the axis offsets detected on
 *        a device.
 *
 * \param detected_offsets axis offsets found via device object enumeration
 * \param axis_count set to the number of recognized axes
 * \param axis_map populated for [0, axis_count); remaining entries zeroed
 */
void build_axis_map(
    std::vector<AxisOffset> const&      detected_offsets,
    DWORD&                              axis_count,
    AxisMap                             (&axis_map)[8]
);
