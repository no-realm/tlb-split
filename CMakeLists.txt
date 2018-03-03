#
# Bareflank Extension: tlb-split

if(ENABLE_BUILD_VMM)
    vmm_extension(
        tlb-split
        DEPENDS eapis
        SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}/src
    )

    vmm_extension(
        tlb-split_main
        DEPENDS tlb-split
        SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}/src/main
    )
endif()
