add_library(rufus_ui STATIC
    MainWindow.cpp
    AboutDialog.cpp
    LogDialog.cpp
)
target_include_directories(rufus_ui PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)
target_link_libraries(rufus_ui PUBLIC
    rufus_core
    rufus_worker
    Qt6::Core
    Qt6::Widgets
    Qt6::Concurrent
)
