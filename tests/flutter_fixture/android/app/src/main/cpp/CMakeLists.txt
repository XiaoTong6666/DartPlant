cmake_minimum_required(VERSION 3.22.1)

project(dartplant_flutter_fixture LANGUAGES C CXX ASM)

get_filename_component(DARTPLANT_ROOT
    "${CMAKE_CURRENT_LIST_DIR}/../../../../../../.." ABSOLUTE)
set(DART_SDK_ROOT "${DARTPLANT_ROOT}/../sdk" CACHE PATH "Dart SDK source tree")

set(DARTPLANT_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(DARTPLANT_BUILD_ANDROID_TESTS OFF CACHE BOOL "" FORCE)
set(DARTPLANT_BUILD_ANDROID_MODULE OFF CACHE BOOL "" FORCE)
set(DARTPLANT_BUILD_DOBBY_ADAPTER OFF CACHE BOOL "" FORCE)
set(DARTPLANT_BUILD_FLUTTER_VM_ADAPTER OFF CACHE BOOL "" FORCE)
set(DARTPLANT_BUILD_FLUTTER_VM_ADAPTER_3_4_4 ON CACHE BOOL "" FORCE)
set(DARTPLANT_DART_SDK_ROOT "${DART_SDK_ROOT}" CACHE PATH "" FORCE)
if(NOT DEFINED DARTPLANT_DOBBY_ROOT)
    set(DARTPLANT_DOBBY_ROOT "${DARTPLANT_ROOT}/third_party/dobby" CACHE PATH "" FORCE)
endif()
set(DOBBY_GENERATE_SHARED OFF CACHE BOOL "" FORCE)

if(NOT TARGET dobby)
    add_subdirectory(
        "${DARTPLANT_DOBBY_ROOT}"
        "${CMAKE_CURRENT_BINARY_DIR}/dobby"
    )
endif()

add_subdirectory(
    "${DARTPLANT_ROOT}"
    "${CMAKE_CURRENT_BINARY_DIR}/dartplant"
)

# Keep the root-relocation diagnostics test-only. Production DartPlant builds
# do not emit per-invocation TypeArguments root logs.
target_compile_definitions(dartplant_core PRIVATE DARTPLANT_TYPE_ARGUMENTS_PROOF_LOGGING=1)

add_library(
    dartplant_fixture_bridge
    SHARED
    fixture_bridge.cpp
    fixture_host.cpp
    simple_facade_consumer.cpp
    p6_abi_probe.cpp
    dart_api_adapter.h
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
    "${DARTPLANT_ROOT}/tests/flutter_fixture/.dart_tool/dartplant/generated"
    "${DARTPLANT_ROOT}/include"
    "${DARTPLANT_ROOT}/src"
    "${DARTPLANT_DOBBY_ROOT}/include"
    "${DART_SDK_ROOT}/runtime/include"
)
target_link_libraries(
    dartplant_fixture_bridge
    PRIVATE
    dartplant_core
    dartplant_adapter_flutter_vm_3_4_4
    dobby
    log
    dl
)
target_link_options(
    dartplant_fixture_bridge
    PRIVATE
    -Wl,-z,max-page-size=16384
)
