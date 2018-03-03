#
# Bareflank Extension: tlb-split

cmake_minimum_required(VERSION 3.6)
project(tlb-split C CXX)

include(${SOURCE_CMAKE_DIR}/project.cmake)
init_project(
    INCLUDES ${CMAKE_CURRENT_LIST_DIR}/../include
)

add_subdirectory(exit_handler)

# -----------------------------------------------------------------------------
# Install
# -----------------------------------------------------------------------------

install(DIRECTORY ../include/ DESTINATION include/tlb-split)
