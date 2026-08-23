cmake_minimum_required(VERSION 3.22.1)

project(dartplant_flutter_fixture LANGUAGES C CXX ASM)

get_filename_component(DARTPLANT_ROOT
    "${CMAKE_CURRENT_LIST_DIR}/../../../../../../.." ABSOLUTE)
set(DART_SDK_ROOT "${DARTPLANT_ROOT}/../sdk" CACHE PATH "Dart SDK source tree")

set(DARTPLANT_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(DARTPLANT_BUILD_ANDROID_TESTS OFF CACHE BOOL "" FORCE)
set(DARTPLANT_BUILD_ANDROID_MODULE OFF CACHE BOOL "" FORCE)
set(DOBBY_GENERATE_SHARED OFF CACHE BOOL "" FORCE)

add_subdirectory(
    "${DARTPLANT_ROOT}"
    "${CMAKE_CURRENT_BINARY_DIR}/dartplant"
)
add_subdirectory(
    "${DARTPLANT_ROOT}/third_party/dobby"
    "${CMAKE_CURRENT_BINARY_DIR}/dobby"
)

add_library(
    dartplant_fixture_bridge
    SHARED
    fixture_bridge.cpp
    dart_api_adapter.cpp
    dart_raw_bridge.cpp
    standalone_dobby_host.cpp
    "${DART_SDK_ROOT}/runtime/include/dart_api_dl.c"
)
set_source_files_properties(
    "${DART_SDK_ROOT}/runtime/include/dart_api_dl.c"
    PROPERTIES COMPILE_OPTIONS "-Wno-unused-parameter"
)

target_compile_features(dartplant_fixture_bridge PRIVATE cxx_std_20)
target_compile_options(
    dartplant_fixture_bridge
    PRIVATE
    -Wall
    -Wextra
    -Werror
)
target_include_directories(
    dartplant_fixture_bridge
    PRIVATE
    "${DARTPLANT_ROOT}/include"
    "${DARTPLANT_ROOT}/src"
    "${DARTPLANT_ROOT}/third_party/dobby/include"
    "${DART_SDK_ROOT}/runtime/include"
)
target_link_libraries(
    dartplant_fixture_bridge
    PRIVATE
    dartplant_core
    dobby
    log
    dl
)
target_link_options(
    dartplant_fixture_bridge
    PRIVATE
    -Wl,-z,max-page-size=16384
)
