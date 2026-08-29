cmake_minimum_required(VERSION 3.22.1)

project(dartplant LANGUAGES CXX)

option(DARTPLANT_BUILD_TESTS "Build DartPlant host tests" ${PROJECT_IS_TOP_LEVEL})
option(DARTPLANT_BUILD_ANDROID_TESTS "Build DartPlant Android device tests" OFF)
option(DARTPLANT_BUILD_ANDROID_MODULE "Build the optional LSPosed Native API shared module" OFF)
option(DARTPLANT_BUILD_DOBBY_ADAPTER "Build the optional Dobby HostApi adapter" OFF)
option(DARTPLANT_BUILD_SHADOWHOOK_ADAPTER "Build the optional ShadowHook HostApi adapter" OFF)
set(
    DARTPLANT_DOBBY_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/dobby"
    CACHE PATH
    "Path to a Dobby source tree"
)
set(
    DARTPLANT_SHADOWHOOK_ROOT
    ""
    CACHE PATH
    "Path to a ShadowHook source tree when building the optional adapter"
)

add_library(
    dartplant_abi
    STATIC
    src/abi/representation.cpp
    src/abi/evidence_solver.cpp
    src/abi/calling_convention.cpp
)
set_target_properties(dartplant_abi PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_features(dartplant_abi PUBLIC cxx_std_20)
target_compile_options(dartplant_abi PRIVATE -Wall -Wextra -Werror -fvisibility=hidden)
target_include_directories(dartplant_abi PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)

add_library(
    dartplant_core
    STATIC
    src/core/dartplant.cpp
    src/core/hook_registry.cpp
    src/elf/module_image.cpp
    src/metadata/metadata.cpp
    src/abi/runtime_evidence.cpp
    src/abi/value_codec.cpp
    src/runtime/flutter_snapshot.cpp
    src/runtime/snapshot_index.cpp
    src/runtime/dart_runtime_resolver.cpp
    src/runtime/default_runtime.cpp
    src/vm/profiles.cpp
    src/vm/live_vm.cpp
    src/vm/live_vm_bootstrap.cpp
    src/vm/object_bridge.cpp
    src/interceptor/runtime_hook.cpp
    src/interceptor/invocation.cpp
    src/interceptor/callback_dispatch.cpp
    src/interceptor/arm64_bridge.cpp
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
target_link_libraries(dartplant_core PRIVATE dartplant_abi)

if(DARTPLANT_BUILD_DOBBY_ADAPTER)
    if(NOT TARGET dobby)
        if(NOT EXISTS "${DARTPLANT_DOBBY_ROOT}/CMakeLists.txt")
            message(FATAL_ERROR "DARTPLANT_DOBBY_ROOT must point to a Dobby source tree")
        endif()
        # Seed Dobby's option in this directory scope without rewriting a
        # parent project's cache when DartPlant is consumed via add_subdirectory.
        set(DOBBY_GENERATE_SHARED OFF)
        add_subdirectory(
            "${DARTPLANT_DOBBY_ROOT}"
            "${CMAKE_CURRENT_BINARY_DIR}/third_party/dobby-adapter"
        )
    endif()
    add_library(dartplant_adapter_dobby STATIC adapters/dobby/dobby_host.cpp)
    target_compile_features(dartplant_adapter_dobby PUBLIC cxx_std_20)
    target_compile_options(dartplant_adapter_dobby PRIVATE -Wall -Wextra -Werror)
    target_include_directories(
        dartplant_adapter_dobby
        PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
        PRIVATE ${DARTPLANT_DOBBY_ROOT}/include
    )
    target_link_libraries(dartplant_adapter_dobby PUBLIC dartplant_core PRIVATE dobby)
endif()

if(DARTPLANT_BUILD_SHADOWHOOK_ADAPTER)
    add_library(dartplant_adapter_shadowhook STATIC adapters/shadowhook/shadowhook_host.cpp)
    target_compile_features(dartplant_adapter_shadowhook PUBLIC cxx_std_20)
    target_compile_options(dartplant_adapter_shadowhook PRIVATE -Wall -Wextra -Werror)
    target_include_directories(dartplant_adapter_shadowhook PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
    if(TARGET shadowhook)
        # Consume include/link usage requirements from a parent-provided target.
    elseif(EXISTS "${DARTPLANT_SHADOWHOOK_ROOT}/shadowhook/src/main/cpp/include/shadowhook.h")
        target_include_directories(
            dartplant_adapter_shadowhook
            PRIVATE ${DARTPLANT_SHADOWHOOK_ROOT}/shadowhook/src/main/cpp/include
        )
    else()
        message(FATAL_ERROR
            "Provide a CMake target named shadowhook or set DARTPLANT_SHADOWHOOK_ROOT to a ShadowHook source tree")
    endif()
    # DartPlant never configures/builds ShadowHook itself. A parent target can
    # supply a source/prebuilt implementation; otherwise the plain library
    # name is resolved by the final Android link step.
    target_link_libraries(dartplant_adapter_shadowhook PUBLIC dartplant_core PRIVATE shadowhook)
endif()

if(ANDROID)
    enable_language(ASM)
    target_sources(
        dartplant_core
        PRIVATE
        src/interceptor/arm64_bridge.S
    )
    if(NOT ANDROID_ABI STREQUAL "arm64-v8a")
        message(FATAL_ERROR "DartPlant currently supports Android arm64-v8a only")
    endif()

    if(DARTPLANT_BUILD_ANDROID_MODULE)
        add_library(
            dartplant
            SHARED
            adapters/lsposed/lsposed_module.cpp
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
        # Vector stores NativeOnModuleLoaded without an unregister API. Keep the
        # callback and its refresh worker code resident for process lifetime.
        target_link_options(dartplant PRIVATE -Wl,-z,max-page-size=16384 -Wl,-z,nodelete)
    endif()

    if(DARTPLANT_BUILD_ANDROID_TESTS)
        if(NOT TARGET dobby)
            if(NOT EXISTS "${DARTPLANT_DOBBY_ROOT}/CMakeLists.txt")
                message(FATAL_ERROR "DARTPLANT_DOBBY_ROOT must point to a Dobby source tree")
            endif()
            set(DOBBY_GENERATE_SHARED OFF CACHE BOOL "" FORCE)
            add_subdirectory(
                "${DARTPLANT_DOBBY_ROOT}"
                "${CMAKE_CURRENT_BINARY_DIR}/third_party/dobby"
            )
        endif()
        if(NOT TARGET dartplant_adapter_dobby)
            add_library(dartplant_adapter_dobby STATIC adapters/dobby/dobby_host.cpp)
            target_compile_features(dartplant_adapter_dobby PUBLIC cxx_std_20)
            target_compile_options(dartplant_adapter_dobby PRIVATE -Wall -Wextra -Werror)
            target_include_directories(
                dartplant_adapter_dobby
                PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
                PRIVATE ${DARTPLANT_DOBBY_ROOT}/include
            )
            target_link_libraries(dartplant_adapter_dobby PUBLIC dartplant_core PRIVATE dobby)
        endif()

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
            tests/android/device_bridge_probe.S
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
            ${CMAKE_CURRENT_SOURCE_DIR}
            ${DARTPLANT_DOBBY_ROOT}/include
        )
        target_link_libraries(
            dartplant_device_tests
            PRIVATE
            dartplant_core
            dartplant_adapter_dobby
            dartplant_device_fixture
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
    # Keep bundled Capstone configuration local to this directory scope. With
    # CMP0077 NEW (CMake >= 3.13), these normal variables seed Capstone's
    # option() calls without rewriting a parent project's cache.
    set(CAPSTONE_ARCHITECTURE_DEFAULT OFF)
    set(CAPSTONE_AARCH64_SUPPORT ON)
    set(CAPSTONE_BUILD_TESTS OFF)
    set(CAPSTONE_BUILD_CSTOOL OFF)
    set(CAPSTONE_INSTALL OFF)
    add_subdirectory(third_party/capstone EXCLUDE_FROM_ALL)

    add_library(
        dartplant_aot_abi_analyzer
        STATIC
        tools/aot-abi-analyzer/aot_abi_analyzer.cpp
    )
    target_compile_features(dartplant_aot_abi_analyzer PUBLIC cxx_std_20)
    target_compile_options(dartplant_aot_abi_analyzer PRIVATE -Wall -Wextra -Werror)
    target_compile_definitions(
        dartplant_aot_abi_analyzer
        PRIVATE
        CAPSTONE_AARCH64_COMPAT_HEADER
    )
    target_include_directories(
        dartplant_aot_abi_analyzer
        PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/tools/aot-abi-analyzer
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(dartplant_aot_abi_analyzer PUBLIC dartplant_abi capstone)

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
        tests/live_vm_signature_test.cc
        tests/snapshot_index_test.cc
        tests/flutter_snapshot_test.cc
        tests/abi_contract_test.cc
        tests/calling_convention_test.cc
        tests/aot_abi_analyzer_test.cc
        tests/shadowhook_adapter_test.cc
        adapters/shadowhook/shadowhook_host.cpp
    )
    target_compile_features(dartplant_tests PRIVATE cxx_std_20)
    target_compile_options(dartplant_tests PRIVATE -Wall -Wextra -Werror)
    target_include_directories(
        dartplant_tests
        PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/tests
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fakes
    )
    target_link_libraries(
        dartplant_tests
        PRIVATE
        dartplant_core
        dartplant_aot_abi_analyzer
        ${CMAKE_DL_LIBS}
    )
    target_compile_definitions(
        dartplant_tests
        PRIVATE
        DARTPLANT_FIXTURE_PATH="$<TARGET_FILE:dartplant_fixture>"
    )
    add_dependencies(dartplant_tests dartplant_fixture)
    add_test(NAME dartplant_tests COMMAND dartplant_tests)
endif()
