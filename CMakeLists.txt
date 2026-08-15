# Compiled and written by BG1KK.
# Privatization and closed-source use are strictly forbidden.
# GNU Radio components are copyrighted by their respective developers.
# All other code copyright © BG1KK.
# This copyright statement must be retained.
cmake_minimum_required(VERSION 3.20)

file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" DMR_B210_SOURCE_VERSION
     LIMIT_COUNT 1)
project(dmr_b210_source VERSION "${DMR_B210_SOURCE_VERSION}" LANGUAGES CXX)

include(GNUInstallDirs)

option(DMR_B210_SOURCE_BUILD_RPT "Build the B210 repeater application" ON)
option(DMR_B210_SOURCE_BUILD_GUI "Build the Pi5 SDL2 GUI" ON)
option(DMR_B210_SOURCE_BUILD_TESTS "Build source-tree unit tests" ON)

set(DMR_B210_DEFAULT_CONFIG_PATH "/etc/dmr-rpt/repeater.yaml"
    CACHE FILEPATH "Production repeater configuration path")
set(DMR_B210_REPEATER_EXAMPLE_CONFIG ""
    CACHE FILEPATH "Repeater YAML used by CTest contract tests")
set(DMR_B210_REPEATER_VECTOR_ROOT ""
    CACHE PATH "828S test vector root used by CTest contract tests")
set(MBELIB_ROOT "" CACHE PATH "mbelib installation or source root")
set(OP25_REPEATER_SOURCE_DIR "" CACHE PATH
    "Path to the OP25 gr-op25_repeater/lib source directory")

if(NOT MBELIB_ROOT AND EXISTS "$ENV{HOME}/.local/include/mbelib.h")
    set(MBELIB_ROOT "$ENV{HOME}/.local" CACHE PATH
        "mbelib installation or source root" FORCE)
endif()
if(NOT OP25_REPEATER_SOURCE_DIR AND
   EXISTS "$ENV{HOME}/deps-src/op25-master/op25/gr-op25_repeater/lib/bptc19696.cc")
    set(OP25_REPEATER_SOURCE_DIR
        "$ENV{HOME}/deps-src/op25-master/op25/gr-op25_repeater/lib"
        CACHE PATH "Path to the OP25 gr-op25_repeater/lib source directory" FORCE)
endif()

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated/dmr_rpt")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/generated/dmr_rpt/build_info.h"
    "#pragma once\n#define DMR_B210_BUILD_SEQUENCE 0\n")

if(DMR_B210_SOURCE_BUILD_RPT OR DMR_B210_SOURCE_BUILD_GUI)
    find_package(yaml-cpp CONFIG REQUIRED)
    if(TARGET yaml-cpp::yaml-cpp)
        set(DMR_B210_YAML_TARGET yaml-cpp::yaml-cpp)
    else()
        set(DMR_B210_YAML_TARGET yaml-cpp)
    endif()
endif()

if(DMR_B210_SOURCE_BUILD_RPT)
    find_package(Gnuradio 3.10 REQUIRED COMPONENTS runtime blocks analog digital filter uhd pmt fft)
    find_package(gnuradio-op25_repeater CONFIG REQUIRED)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LAME REQUIRED IMPORTED_TARGET lame)

    find_path(MBELIB_INCLUDE_DIR mbelib.h
        HINTS "${MBELIB_ROOT}/mbelib" "${MBELIB_ROOT}/include"
              "$ENV{HOME}/.local/include" "$ENV{HOME}/gr-dsd/mbelib" REQUIRED)
    find_library(MBELIB_LIBRARY mbe
        HINTS "${MBELIB_ROOT}/build/mbelib" "${MBELIB_ROOT}/lib"
              "$ENV{HOME}/.local/lib" "$ENV{HOME}/gr-dsd/build/mbelib" REQUIRED)

    set(ENABLE_PYTHON OFF CACHE BOOL "Build gr-dmr Python bindings" FORCE)
    add_subdirectory(rpt/gr-dmr)

    add_library(dmr_b210_repeater_core
        rpt/src/analog_fm.cpp
        rpt/src/audit.cpp
        rpt/src/config.cpp
        rpt/src/dmr_burst.cpp
        rpt/src/event.cpp
        rpt/src/interlock.cpp
        rpt/src/io_status.cpp
        rpt/src/network_protocol.cpp
        rpt/src/recording.cpp
        rpt/src/remote_voice.cpp
        rpt/src/rf_session.cpp
        rpt/src/router.cpp
        rpt/src/rx_signal_calibration.cpp
        rpt/src/sha256.cpp
        rpt/src/short_message.cpp
        rpt/src/vector_manifest.cpp)
    target_include_directories(dmr_b210_repeater_core PUBLIC rpt/include)
    target_compile_features(dmr_b210_repeater_core PUBLIC cxx_std_17)
    target_compile_options(dmr_b210_repeater_core PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(dmr_b210_repeater_core PUBLIC ${DMR_B210_YAML_TARGET})

    add_library(dmr_b210_audio_recording rpt/src/audio_recording_runtime.cpp)
    target_include_directories(dmr_b210_audio_recording
        PUBLIC rpt/include PRIVATE ${MBELIB_INCLUDE_DIR})
    target_compile_features(dmr_b210_audio_recording PUBLIC cxx_std_17)
    target_compile_options(dmr_b210_audio_recording PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(dmr_b210_audio_recording PUBLIC dmr_b210_repeater_core
        PkgConfig::LAME ${MBELIB_LIBRARY})

    add_executable(dmr_b210_rpt
        rpt/src/main.cpp
        rpt/src/hardware_runtime.cpp
        rpt/dmr_direct_framer.cpp)
    target_include_directories(dmr_b210_rpt PRIVATE
        rpt/include rpt/include/dmr_b210
        "${CMAKE_CURRENT_BINARY_DIR}/generated")
    target_compile_features(dmr_b210_rpt PRIVATE cxx_std_17)
    target_compile_options(dmr_b210_rpt PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_definitions(dmr_b210_rpt PRIVATE
        DMR_B210_HAVE_HARDWARE_RUNTIME=1
        DMR_B210_RELEASE_VERSION="${PROJECT_VERSION}"
        DMR_B210_DEFAULT_CONFIG_PATH="${DMR_B210_DEFAULT_CONFIG_PATH}")
    target_link_libraries(dmr_b210_rpt PRIVATE
        dmr_b210_repeater_core dmr_b210_audio_recording
        gnuradio::gnuradio-runtime gnuradio::gnuradio-blocks
        gnuradio::gnuradio-analog gnuradio::gnuradio-digital
        gnuradio::gnuradio-filter gnuradio::gnuradio-uhd
        gnuradio::gnuradio-dmr gnuradio::gnuradio-op25_repeater)
    set_target_properties(dmr_b210_rpt PROPERTIES INSTALL_RPATH "$ORIGIN/../lib")
    install(TARGETS dmr_b210_rpt RUNTIME DESTINATION bin)

    if(DMR_B210_SOURCE_BUILD_TESTS)
        enable_testing()
        foreach(test_name network_protocol_tests repeater_contract_tests)
            add_executable(${test_name} rpt/tests/${test_name}.cpp)
            target_compile_features(${test_name} PRIVATE cxx_std_17)
            target_compile_options(${test_name} PRIVATE -Wall -Wextra -Wpedantic)
            target_link_libraries(${test_name} PRIVATE dmr_b210_repeater_core)
        endforeach()
        add_test(NAME network_protocol_tests COMMAND network_protocol_tests)
        add_executable(rx_signal_calibration_tests
            rpt/tests/rx_signal_calibration_tests.cpp
            rpt/src/rx_signal_calibration.cpp)
        target_compile_features(rx_signal_calibration_tests PRIVATE cxx_std_17)
        target_include_directories(rx_signal_calibration_tests PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/rpt/include)
        target_compile_options(rx_signal_calibration_tests PRIVATE
            -Wall -Wextra -Wpedantic)
        add_test(NAME rx_signal_calibration_tests
            COMMAND rx_signal_calibration_tests)
        add_executable(frontend_conditioning_tests
            rpt/tests/frontend_conditioning_tests.cpp
            rpt/src/io_status.cpp)
        target_compile_features(frontend_conditioning_tests PRIVATE cxx_std_17)
        target_include_directories(frontend_conditioning_tests PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/rpt/include)
        target_compile_options(frontend_conditioning_tests PRIVATE
            -Wall -Wextra -Wpedantic)
        add_test(NAME frontend_conditioning_tests
            COMMAND frontend_conditioning_tests)
        if(EXISTS "${DMR_B210_REPEATER_EXAMPLE_CONFIG}" AND
           IS_DIRECTORY "${DMR_B210_REPEATER_VECTOR_ROOT}")
            add_test(NAME repeater_contract_tests
                COMMAND repeater_contract_tests
                    "${DMR_B210_REPEATER_EXAMPLE_CONFIG}"
                    "${DMR_B210_REPEATER_VECTOR_ROOT}")
        else()
            message(STATUS
                "Repeater contract CTest disabled: set the example config and vector root")
        endif()

        add_executable(audio_recording_tests rpt/tests/audio_recording_tests.cpp)
        target_compile_features(audio_recording_tests PRIVATE cxx_std_17)
        target_compile_options(audio_recording_tests PRIVATE -Wall -Wextra -Wpedantic)
        target_link_libraries(audio_recording_tests PRIVATE dmr_b210_audio_recording)
        add_test(NAME audio_recording_tests COMMAND audio_recording_tests)

        add_executable(dmr_burst_symbol_sampler_tests
            rpt/tests/dmr_burst_symbol_sampler_tests.cpp)
        target_include_directories(dmr_burst_symbol_sampler_tests PRIVATE rpt/include)
        target_compile_features(dmr_burst_symbol_sampler_tests PRIVATE cxx_std_17)
        target_compile_options(dmr_burst_symbol_sampler_tests PRIVATE -Wall -Wextra -Wpedantic)
        target_link_libraries(dmr_burst_symbol_sampler_tests PRIVATE
            gnuradio::gnuradio-runtime gnuradio::gnuradio-blocks)
        add_test(NAME dmr_burst_symbol_sampler_tests
            COMMAND dmr_burst_symbol_sampler_tests)

        add_executable(dmr_short_message_frame_builder_tests
            rpt/tests/dmr_short_message_frame_builder_tests.cpp
            rpt/dmr_short_message_frame_builder.cpp)
        target_include_directories(dmr_short_message_frame_builder_tests PRIVATE
            rpt/include)
        target_compile_features(dmr_short_message_frame_builder_tests PRIVATE cxx_std_17)
        target_compile_options(dmr_short_message_frame_builder_tests PRIVATE
            -Wall -Wextra -Wpedantic)
        add_test(NAME dmr_short_message_frame_builder_tests
            COMMAND dmr_short_message_frame_builder_tests)
    endif()
endif()

if(DMR_B210_SOURCE_BUILD_GUI)
    if(NOT UNIX)
        message(FATAL_ERROR "The Pi5 GUI requires a Linux SDL2 runtime")
    endif()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
    pkg_check_modules(SDL2_TTF REQUIRED IMPORTED_TARGET SDL2_ttf)
    add_executable(dmr_b210_gui gui/main.cpp)
    target_include_directories(dmr_b210_gui PRIVATE
        "${CMAKE_CURRENT_BINARY_DIR}/generated")
    target_compile_features(dmr_b210_gui PRIVATE cxx_std_17)
    target_compile_options(dmr_b210_gui PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_definitions(dmr_b210_gui PRIVATE
        DMR_B210_GUI_VERSION="${PROJECT_VERSION}")
    target_link_libraries(dmr_b210_gui PRIVATE ${DMR_B210_YAML_TARGET}
        PkgConfig::SDL2 PkgConfig::SDL2_TTF)
    install(TARGETS dmr_b210_gui RUNTIME DESTINATION bin)
    if(DMR_B210_SOURCE_BUILD_TESTS)
        enable_testing()
        add_test(NAME gui_contract_tests COMMAND dmr_b210_gui --self-test)
    endif()
endif()
