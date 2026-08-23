cmake_minimum_required(VERSION 3.22.1)

project(dartplant LANGUAGES CXX)

option(DARTPLANT_BUILD_TESTS "Build DartPlant host tests" ON)
option(DARTPLANT_BUILD_ANDROID_TESTS "Build DartPlant Android device tests" OFF)
option(DARTPLANT_BUILD_ANDROID_MODULE "Build the optional LSPosed Native API shared module" OFF)
set(
    DARTPLANT_DOBBY_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/dobby"
    CACHE PATH
    "Path to a Dobby source tree"
)

add_library(
    dartplant_core
    STATIC
    src/core/dartplant.cpp
    src/core/hook_registry.cpp
    src/elf/module_image.cpp
    src/metadata/metadata.cpp
    src/runtime/runtime_profile.cpp
    src/runtime/flutter_snapshot.cpp
    src/runtime/snapshot_index.cpp
    src/runtime/dart_runtime_resolver.cpp
    src/runtime/dart_vm_abi.cpp
    src/runtime/live_vm.cpp
    src/runtime/live_vm_bootstrap.cpp
    src/runtime/vm_adapter.cpp
    src/runtime/dart_dispatch_hook.cpp
    src/runtime/dart_invocation.cpp
    src/runtime/dart_callback_dispatch.cpp
    src/runtime/arm64_callback_stub.cpp
)
set_target_properties(dartplant_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_compile_features(dartplant_core PUBLIC cxx_std_20)
target_compile_options(
    dartplant_core
    PRIVATE
    -Wall
    -Wextra
    -Werror
    -fvisibility=hidden
)
target_include_directories(
    dartplant_core
    PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(dartplant_core PUBLIC ${CMAKE_DL_LIBS})

if(ANDROID)
    enable_language(ASM)
    target_sources(
        dartplant_core
        PRIVATE
        src/runtime/arm64_callback_trampoline.S
    )
    if(NOT ANDROID_ABI STREQUAL "arm64-v8a")
        message(FATAL_ERROR "DartPlant currently supports Android arm64-v8a only")
    endif()

    if(DARTPLANT_BUILD_ANDROID_MODULE)
        add_library(
            dartplant
            SHARED
            src/host/lsposed_module.cpp
        )
        target_compile_features(dartplant PRIVATE cxx_std_20)
        target_compile_options(
            dartplant
            PRIVATE
            -Wall
            -Wextra
            -Werror
            -fvisibility=hidden
        )
        target_include_directories(
            dartplant
            PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}/src
        )
        # DartPlant is also consumed through its public C ABI. Force every object
        # from the static core into the shared library instead of relying on a
        # direct reference from native_init() to pull it from the archive.
        target_link_libraries(
            dartplant
            PRIVATE
            -Wl,--whole-archive
            dartplant_core
            -Wl,--no-whole-archive
            log
            dl
        )
        target_link_options(dartplant PRIVATE -Wl,-z,max-page-size=16384)
    endif()

    if(DARTPLANT_BUILD_ANDROID_TESTS)
        if(NOT EXISTS "${DARTPLANT_DOBBY_ROOT}/CMakeLists.txt")
            message(FATAL_ERROR "DARTPLANT_DOBBY_ROOT must point to a Dobby source tree")
        endif()
        set(DOBBY_GENERATE_SHARED OFF CACHE BOOL "" FORCE)
        add_subdirectory(
            "${DARTPLANT_DOBBY_ROOT}"
            "${CMAKE_CURRENT_BINARY_DIR}/third_party/dobby"
        )

        add_library(
            dartplant_device_fixture
            SHARED
            tests/fixtures/hook_fixture.cc
        )
        target_compile_features(dartplant_device_fixture PRIVATE cxx_std_20)
        target_compile_options(
            dartplant_device_fixture
            PRIVATE
            -Wall
            -Wextra
            -Werror
        )
        target_link_options(
            dartplant_device_fixture
            PRIVATE
            -Wl,-z,max-page-size=16384
        )

        add_executable(
            dartplant_device_tests
            tests/android/device_test_main.cc
        )
        target_compile_features(dartplant_device_tests PRIVATE cxx_std_20)
        target_compile_options(
            dartplant_device_tests
            PRIVATE
            -Wall
            -Wextra
            -Werror
        )
        target_include_directories(
            dartplant_device_tests
            PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${DARTPLANT_DOBBY_ROOT}/include
        )
        target_link_libraries(
            dartplant_device_tests
            PRIVATE
            dartplant_core
            dartplant_device_fixture
            dobby
            log
            dl
        )
        set_target_properties(
            dartplant_device_tests
            PROPERTIES
            BUILD_RPATH "$ORIGIN"
        )
        target_link_options(
            dartplant_device_tests
            PRIVATE
            -Wl,-z,max-page-size=16384
        )
    endif()
endif()

if(DARTPLANT_BUILD_TESTS AND NOT ANDROID)
    enable_testing()
    add_library(dartplant_fixture SHARED tests/fixtures/hook_fixture.cc)
    target_compile_features(dartplant_fixture PRIVATE cxx_std_20)
    target_compile_options(dartplant_fixture PRIVATE -Wall -Wextra -Werror)

    add_executable(
        dartplant_tests
        tests/test_main.cc
        tests/elf_resolver_test.cc
        tests/metadata_test.cc
        tests/hook_registry_test.cc
        tests/runtime_test.cc
        tests/snapshot_index_test.cc
        tests/flutter_snapshot_test.cc
    )
    target_compile_features(dartplant_tests PRIVATE cxx_std_20)
    target_compile_options(dartplant_tests PRIVATE -Wall -Wextra -Werror)
    target_include_directories(
        dartplant_tests
        PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/tests
    )
    target_link_libraries(dartplant_tests PRIVATE dartplant_core ${CMAKE_DL_LIBS})
    target_compile_definitions(
        dartplant_tests
        PRIVATE
        DARTPLANT_FIXTURE_PATH="$<TARGET_FILE:dartplant_fixture>"
    )
    add_dependencies(dartplant_tests dartplant_fixture)
    add_test(NAME dartplant_tests COMMAND dartplant_tests)
endif()
