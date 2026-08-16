cmake_minimum_required(VERSION 3.22)
project(rufus
    VERSION 1.0.3
    DESCRIPTION "Rufus - Create bootable USB drives (Linux port)"
    LANGUAGES CXX C
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Widgets Concurrent LinguistTools)

option(RUFUS_BUILD_TESTS "Build smoke/integration tests (require root + a loop device)" OFF)
if(RUFUS_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

add_subdirectory(src)
