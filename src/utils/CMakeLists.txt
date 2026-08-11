add_library(rufus_utils STATIC
    Localization.cpp
    Logger.cpp
    MsgBox.cpp
    Settings.cpp
)
target_include_directories(rufus_utils PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(rufus_utils PUBLIC Qt6::Core Qt6::Widgets)
