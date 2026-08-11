add_library(rufus_backend STATIC
    DeviceManager.cpp
    PartitionManager.cpp
    Filesystem.cpp
    BootloaderInstaller.cpp
    DriveWriter.cpp
    Mounter.cpp
)
target_include_directories(rufus_backend PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(rufus_backend PUBLIC
    rufus_core
    Qt6::Core
)
