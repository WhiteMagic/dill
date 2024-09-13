cmake_minimum_required( VERSION 3.4 )
project( DILL )

set( CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin )
set( CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/lib )
set( CMAKE_PYDLL_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/src/python )
set( CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/lib )

set( CMAKE_CXX_STANDARD 11 )
include_directories(
	${CMAKE_SOURCE_DIR}/src
)

add_library( dill SHARED src/dill.cpp )
target_link_libraries( dill dinput8 dxguid )

add_executable( example src/example.cpp )
target_link_libraries( example dill )

add_executable( example2 src/example2.cpp )
target_link_libraries( example2 dill )

add_custom_command(TARGET dill
          POST_BUILD
          COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/dill.dll" ${CMAKE_PYDLL_OUTPUT_DIRECTORY}
)