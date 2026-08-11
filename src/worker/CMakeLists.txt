add_library(rufus_worker STATIC
    FormatWorker.cpp
)
target_include_directories(rufus_worker PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)
target_link_libraries(rufus_worker PUBLIC
    rufus_core
    rufus_backend
    Qt6::Core
)
