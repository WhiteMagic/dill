cmake_minimum_required( VERSION 3.20 )
project( DILL )

set( CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin )
set( CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/lib )
set( CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/lib )

set( CMAKE_CXX_STANDARD 20 )
include_directories(
	${CMAKE_SOURCE_DIR}/src
)

include_directories(
    ${CMAKE_SOURCE_DIR/src/libguarded}
)

add_library( dill SHARED src/dill.cpp )
target_link_libraries( dill dinput8 dxguid )

add_executable( example src/example.cpp )
target_link_libraries( example dill )

add_executable( example2 src/example2.cpp )
target_link_libraries( example2 dill )