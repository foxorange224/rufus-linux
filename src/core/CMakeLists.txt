add_library(rufus_core STATIC
    ImageHandler.cpp
    Hash.cpp
    BadBlocks.cpp
)
target_include_directories(rufus_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(rufus_core PUBLIC Qt6::Core)
