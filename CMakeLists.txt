cmake_minimum_required( VERSION 3.4 )
project( DInput )

set( CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin )
set( CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/lib )
set( CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/lib )

set( CMAKE_CXX_STANDARD 11 )
include_directories(
	${CMAKE_SOURCE_DIR}/src
)

add_library( di_listener SHARED src/di_listener.cpp )
target_link_libraries( di_listener dinput8 dxguid )

add_executable( example src/example.cpp )
target_link_libraries( example di_listener )

add_executable( example2 src/example2.cpp )
target_link_libraries( example2 di_listener )