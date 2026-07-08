#define CATCH_CONFIG_MAIN
#include "catch2/catch.hpp"

#include "axis_mapping.h"

TEST_CASE("no axes detected", "[axis_mapping]")
{
    DWORD axis_count;
    AxisMap axis_map[8];

    build_axis_map({}, axis_count, axis_map);

    REQUIRE(axis_count == 0);
}

TEST_CASE("six linear axes without sliders in natural order", "[axis_mapping]")
{
    DWORD axis_count;
    AxisMap axis_map[8];

    build_axis_map(
        {DIJOFS_X, DIJOFS_Y, DIJOFS_Z, DIJOFS_RX, DIJOFS_RY, DIJOFS_RZ},
        axis_count,
        axis_map
    );

    REQUIRE(axis_count == 6);
    for(DWORD i = 0; i < 6; ++i)
    {
        REQUIRE(axis_map[i].linear_index == i + 1);
        REQUIRE(axis_map[i].axis_index == i + 1);
    }
}

TEST_CASE("non-sequential axis Z and Rz present", "[axis_mapping]")
{
    DWORD axis_count;
    AxisMap axis_map[8];

    build_axis_map({DIJOFS_Z, DIJOFS_RZ}, axis_count, axis_map);

    REQUIRE(axis_count == 2);
    REQUIRE(axis_map[0].linear_index == 1);
    REQUIRE(axis_map[0].axis_index == 3);
    REQUIRE(axis_map[1].linear_index == 2);
    REQUIRE(axis_map[1].axis_index == 6);
}

TEST_CASE("both sliders present", "[axis_mapping]")
{
    DWORD axis_count;
    AxisMap axis_map[8];

    build_axis_map(
        {DIJOFS_SLIDER(0), DIJOFS_SLIDER(1)},
        axis_count,
        axis_map
    );

    REQUIRE(axis_count == 2);
    REQUIRE(axis_map[0].axis_index == 7);
    REQUIRE(axis_map[1].axis_index == 8);
}

TEST_CASE(
    "detection order does not affect resulting axis_index order",
    "[axis_mapping]"
)
{
    DWORD axis_count;
    AxisMap axis_map[8];

    build_axis_map(
        {DIJOFS_SLIDER(1), DIJOFS_RZ, DIJOFS_X, DIJOFS_SLIDER(0)},
        axis_count,
        axis_map
    );

    REQUIRE(axis_count == 4);
    REQUIRE(axis_map[0].axis_index == 1);
    REQUIRE(axis_map[1].axis_index == 6);
    REQUIRE(axis_map[2].axis_index == 7);
    REQUIRE(axis_map[3].axis_index == 8);
}

TEST_CASE("unrecognized offsets are ignored", "[axis_mapping]")
{
    DWORD axis_count;
    AxisMap axis_map[8];

    build_axis_map({DIJOFS_X, 0xDEADBEEF}, axis_count, axis_map);

    REQUIRE(axis_count == 1);
    REQUIRE(axis_map[0].axis_index == 1);
}

TEST_CASE(
    "axis_index_for_offset and offset_for_axis_index round-trip",
    "[axis_mapping]"
)
{
    for(DWORD axis_index = 1; axis_index <= 8; ++axis_index)
    {
        AxisOffset offset = offset_for_axis_index(axis_index);
        REQUIRE(axis_index_for_offset(offset) == axis_index);
    }
}

TEST_CASE("unknown axis_index/offset resolve to -1", "[axis_mapping]")
{
    REQUIRE(offset_for_axis_index(0) == static_cast<AxisOffset>(-1));
    REQUIRE(offset_for_axis_index(9) == static_cast<AxisOffset>(-1));
    REQUIRE(axis_index_for_offset(0xDEADBEEF) == static_cast<DWORD>(-1));
}
