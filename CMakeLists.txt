cmake_minimum_required(VERSION 3.21)
project(PasteOrbitNative LANGUAGES CXX)

set(PASTEORBIT_VERSION "2.5.2" CACHE STRING "PasteOrbit application version")
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets Sql Concurrent Network)
include(FetchContent)
FetchContent_Declare(Qlementine
    GIT_REPOSITORY https://github.com/oclero/qlementine.git
    GIT_TAG 209e549c415f1d828883f9f21a633eb535f9d67d
    GIT_SHALLOW FALSE
)
FetchContent_MakeAvailable(Qlementine)
set_target_properties(qlementinestyleplugin PROPERTIES EXCLUDE_FROM_ALL TRUE)
add_subdirectory(src)
