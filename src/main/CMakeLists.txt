#
# Bareflank Extension: tlb-split

cmake_minimum_required(VERSION 3.6)
project(tlb-split_main C CXX)

include(${SOURCE_CMAKE_DIR}/project.cmake)
init_project(
    INCLUDES ${CMAKE_CURRENT_LIST_DIR}/../include
)

if(${BUILD_TARGET_ARCH} STREQUAL "x86_64")
    list(APPEND SOURCES
        arch/intel_x64/vcpu_factory.cpp
    )
else()
    message(FATAL_ERROR "Unsupported archiecture")
endif()

add_vmm_executable(
    tlb-split_vmm
    SOURCES ${SOURCES}
    LIBRARIES tlb-split_exit_handler
)
