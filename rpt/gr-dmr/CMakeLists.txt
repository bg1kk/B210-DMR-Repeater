# Compiled and written by BG1KK.
# Privatization and closed-source use are strictly forbidden.
# GNU Radio components are copyrighted by their respective developers.
# All other code copyright © BG1KK.
# This copyright statement must be retained.
cmake_minimum_required(VERSION 3.20)
project(gr-dmr VERSION 1.0.0 LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

find_package(Gnuradio 3.10 REQUIRED COMPONENTS runtime pmt)

set(OP25_REPEATER_SOURCE_DIR "" CACHE PATH
    "Path to the OP25 gr-op25_repeater/lib source directory")
if(NOT OP25_REPEATER_SOURCE_DIR)
    set(OP25_REPEATER_SOURCE_DIR
        "$ENV{HOME}/source/op25/op25/gr-op25_repeater/lib")
endif()

foreach(source_file bptc19696.cc hamming.cc golay2087.cc trellis.cc)
    if(NOT EXISTS "${OP25_REPEATER_SOURCE_DIR}/${source_file}")
        message(FATAL_ERROR
            "Missing ${OP25_REPEATER_SOURCE_DIR}/${source_file}; set OP25_REPEATER_SOURCE_DIR")
    endif()
endforeach()

add_library(gnuradio-dmr SHARED
    ../gr_dmr_frame_decoder_impl.cc
    lib/dmr_slot.cc
    ${OP25_REPEATER_SOURCE_DIR}/bptc19696.cc
    ${OP25_REPEATER_SOURCE_DIR}/hamming.cc
    ${OP25_REPEATER_SOURCE_DIR}/golay2087.cc
    ${OP25_REPEATER_SOURCE_DIR}/trellis.cc
)
add_library(gnuradio::gnuradio-dmr ALIAS gnuradio-dmr)

target_compile_features(gnuradio-dmr PUBLIC cxx_std_17)
target_compile_definitions(gnuradio-dmr PRIVATE gnuradio_dmr_EXPORTS)
target_compile_options(gnuradio-dmr PRIVATE -Wall -Wextra -Wpedantic)
target_include_directories(gnuradio-dmr
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/lib
        ${OP25_REPEATER_SOURCE_DIR}
)
target_link_libraries(gnuradio-dmr PUBLIC
    gnuradio::gnuradio-runtime
    gnuradio::gnuradio-pmt
)
set_target_properties(gnuradio-dmr PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION 1
    DEFINE_SYMBOL gnuradio_dmr_EXPORTS
)

install(TARGETS gnuradio-dmr
    EXPORT gnuradio-dmrTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT gnuradio-dmrTargets
    FILE gnuradio-dmrTargets.cmake
    NAMESPACE gnuradio::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/gnuradio-dmr
)

configure_package_config_file(
    cmake/gnuradio-dmrConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/gnuradio-dmrConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/gnuradio-dmr
)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/gnuradio-dmrConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/gnuradio-dmrConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/gnuradio-dmrConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/gnuradio-dmr
)

option(ENABLE_PYTHON "Build Python bindings" ON)
if(ENABLE_PYTHON)
    find_package(Python3 REQUIRED COMPONENTS Interpreter Development.Module)
    find_package(pybind11 CONFIG REQUIRED)
    pybind11_add_module(dmr_python python/dmr_python.cc)
    target_link_libraries(dmr_python PRIVATE gnuradio-dmr)
    install(TARGETS dmr_python
        LIBRARY DESTINATION ${Python3_SITEARCH}/gnuradio/dmr)
    install(FILES python/__init__.py
        DESTINATION ${Python3_SITEARCH}/gnuradio/dmr)
endif()
