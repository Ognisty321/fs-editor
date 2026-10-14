cmake_minimum_required(VERSION 3.22)
project(bestsource VERSION 22 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(FFMPEG REQUIRED)
find_package(xxHash CONFIG REQUIRED)

# libp2p: pixel (un)packing used by BestSource's frame export
set(P2P_SOURCES
    ${P2P_SOURCE_DIR}/p2p_api.cpp
    ${P2P_SOURCE_DIR}/v210.cpp)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "(x86_64|AMD64|amd64|i.86|x86)")
    list(APPEND P2P_SOURCES
        ${P2P_SOURCE_DIR}/simd/cpuinfo_x86.cpp
        ${P2P_SOURCE_DIR}/simd/p2p_simd.cpp
        ${P2P_SOURCE_DIR}/simd/p2p_sse41.cpp)
    set(P2P_SIMD ON)
endif()

add_library(bestsource STATIC
    src/audiosource.cpp
    src/bsshared.cpp
    src/gpuhash.cpp
    src/tracklist.cpp
    src/videosource.cpp
    ${P2P_SOURCES})
if(P2P_SIMD)
    target_compile_definitions(bestsource PRIVATE P2P_SIMD)
    if(NOT MSVC)
        set_source_files_properties(${P2P_SOURCE_DIR}/simd/p2p_sse41.cpp PROPERTIES COMPILE_OPTIONS "-msse4.1")
    endif()
endif()
if(MSVC)
    target_compile_options(bestsource PRIVATE /W0 /utf-8 /bigobj)
endif()
target_include_directories(bestsource
    PRIVATE ${P2P_SOURCE_DIR} ${FFMPEG_INCLUDE_DIRS}
    INTERFACE $<INSTALL_INTERFACE:include/bestsource>)
target_link_libraries(bestsource PRIVATE xxHash::xxhash ${FFMPEG_LIBRARIES})

install(TARGETS bestsource EXPORT bestsource-targets ARCHIVE DESTINATION lib)
install(FILES src/audiosource.h src/bsshared.h src/tracklist.h src/version.h src/videosource.h
        DESTINATION include/bestsource)
install(EXPORT bestsource-targets NAMESPACE bestsource:: DESTINATION share/bestsource)
include(CMakePackageConfigHelpers)
configure_package_config_file(bestsource-config.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/bestsource-config.cmake
    INSTALL_DESTINATION share/bestsource)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/bestsource-config.cmake DESTINATION share/bestsource)
