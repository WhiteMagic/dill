cmake_minimum_required( VERSION 3.20 )
project( DILL )

set( CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin )
set( CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/lib )
set( CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/lib )

set( CMAKE_CXX_STANDARD 17 )
set( CMAKE_CXX_STANDARD_REQUIRED ON )
include_directories(
	${CMAKE_SOURCE_DIR}/src
)

add_library( dill SHARED src/dill.cpp src/axis_mapping.cpp )
target_link_libraries( dill dinput8 dxguid ole32 )
target_compile_options( dill PRIVATE /Zc:__cplusplus )

add_executable( example src/example.cpp )
target_link_libraries( example dill )

add_executable( example2 src/example2.cpp )
target_link_libraries( example2 dill )

add_executable( dill_tests
	src/dill.cpp
	src/axis_mapping.cpp
	src/catch2/catch_amalgamated.cpp
	tests/test_axis_mapping.cpp
	tests/test_lifecycle.cpp
)
target_link_libraries( dill_tests dinput8 dxguid ole32 )
target_compile_options( dill_tests PRIVATE /Zc:__cplusplus )