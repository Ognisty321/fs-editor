cmake_minimum_required(VERSION 3.28)
project(fs-editor VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui GuiPrivate Widgets Concurrent ShaderTools Svg LinguistTools)
qt_standard_project_setup()

# Rust core + cxx bridge, built by cargo through Corrosion.
include(FetchContent)
FetchContent_Declare(Corrosion
    GIT_REPOSITORY https://github.com/corrosion-rs/corrosion.git
    GIT_TAG 1499b14e4906a2890f5cee1547c8848db261753d # v0.6.1
)
FetchContent_MakeAvailable(Corrosion)

set(FS_CARGO_PROFILE release CACHE STRING "cargo profile for the Rust core (release or gui-dev)")
corrosion_import_crate(MANIFEST_PATH ${CMAKE_CURRENT_SOURCE_DIR}/../Cargo.toml
    CRATES fs-bridge CRATE_TYPES staticlib PROFILE ${FS_CARGO_PROFILE})
set(FS_BRIDGE_INCLUDE_DIR ${CMAKE_BINARY_DIR}/bridge-include)
corrosion_set_env_vars(fs_bridge FS_BRIDGE_INCLUDE_DIR=${FS_BRIDGE_INCLUDE_DIR})

# FFmpeg/libass/BestSource prefix from tools/deps (also read by cargo via .cargo/config.toml).
if(NOT FS_DEPS_DIR)
    set(FS_DEPS_DIR $ENV{FS_DEPS_DIR})
endif()
# CI passes D:\a\... ; backslashes would end up as escapes in cmake_install.cmake
file(TO_CMAKE_PATH "${FS_DEPS_DIR}" FS_DEPS_DIR)
if(NOT FS_DEPS_DIR)
    message(FATAL_ERROR "Set FS_DEPS_DIR to the tools/deps prefix (see README)")
endif()

qt_add_executable(fsed WIN32 MACOSX_BUNDLE
    src/main.cpp
    src/bridge.h
    src/mainwindow.h src/mainwindow.cpp
    src/tabpanel.h src/tabpanel.cpp
    src/grid.h src/grid.cpp
    src/editbox.h src/editbox.cpp
    src/shiftpanel.h src/shiftpanel.cpp
    src/rhiquad.h src/rhiquad.cpp
    src/videobox.h src/videobox.cpp
    src/visualtools.h src/visualtools.cpp
    src/audiobox.h src/audiobox.cpp
    src/dialogs.h src/dialogs.cpp
    src/automationhost.h src/automationhost.cpp
    src/colorpicker.h src/colorpicker.cpp
    src/theme.h src/theme.cpp
    src/icons.h src/icons.cpp
)
qt_add_shaders(fsed "shaders"
    PREFIX "/"
    FILES shaders/quad.vert shaders/texture.frag shaders/audio.frag
)
# Polish UI, embedded as :/i18n/fsed_pl.qm; `cmake --build <dir> --target update_translations` refreshes the .ts
qt_add_translations(fsed TS_FILES translations/fsed_pl.ts RESOURCE_PREFIX /i18n NO_GENERATE_PLURALS_TS_FILE
    IMMEDIATE_CALL # deferred, the call breaks on a space in the build path
    LUPDATE_OPTIONS -no-obsolete -locations none)
# Qt's own strings (standard buttons, file dialogs) beside ours: a dev-build or deployed exe has Qt's
# DLLs but no translations folder for QLibraryInfo to find
set(FS_QT_TRANSLATIONS "${QT6_INSTALL_PREFIX}/${QT6_INSTALL_TRANSLATIONS}")
if(EXISTS "${FS_QT_TRANSLATIONS}/qtbase_pl.qm")
    qt_add_resources(fsed "qt_i18n" PREFIX "/i18n" BASE "${FS_QT_TRANSLATIONS}" FILES "${FS_QT_TRANSLATIONS}/qtbase_pl.qm")
endif()
file(GLOB FS_ICONS CONFIGURE_DEPENDS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} icons/*.svg)
qt_add_resources(fsed "icons" PREFIX "/" FILES ${FS_ICONS})
target_include_directories(fsed PRIVATE ${FS_BRIDGE_INCLUDE_DIR})
target_link_directories(fsed PRIVATE ${FS_DEPS_DIR}/lib)
# Native libraries the Rust staticlib needs (cargo:rustc-link-lib of fs-media, cpal's audio backend).
# Attached to fs_bridge so they follow libfs_bridge.a on the link line (GNU ld resolves archives left to right).
target_link_libraries(fs_bridge INTERFACE bestsource xxhash avformat avcodec swscale swresample avutil ass)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(fs_bridge INTERFACE asound)
elseif(APPLE)
    # shortcut: framework list from cpal's coreaudio backend, not yet linked on a real Mac.
    target_link_libraries(fs_bridge INTERFACE "-framework CoreAudio" "-framework AudioToolbox" "-framework CoreFoundation")
endif()
target_link_libraries(fsed PRIVATE fs_bridge Qt6::Core Qt6::Gui Qt6::GuiPrivate Qt6::Widgets Qt6::Concurrent Qt6::Svg)
if(MSVC)
    target_compile_options(fsed PRIVATE /utf-8 /W4 /permissive-)
    # LuaJIT's msvcbuild objects ask for the static CRT; the app uses /MD everywhere else.
    target_link_options(fsed PRIVATE /NODEFAULTLIB:LIBCMT)
    target_link_libraries(fsed PRIVATE dbghelp)
else()
    target_compile_options(fsed PRIVATE -Wall -Wextra)
endif()

# Runtime: media DLLs and the bundled automation tree beside the exe.
if(WIN32)
    file(GLOB FS_DEP_DLLS ${FS_DEPS_DIR}/bin/*.dll)
    add_custom_command(TARGET fsed POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${FS_DEP_DLLS} $<TARGET_FILE_DIR:fsed>
        COMMAND_EXPAND_LISTS)
endif()
add_custom_command(TARGET fsed POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
        ${CMAKE_CURRENT_SOURCE_DIR}/../crates/fs-automation/automation $<TARGET_FILE_DIR:fsed>/automation)

# Install / package: `cmake --install build-windows --prefix dist` then `cpack`.
if(NOT WIN32 AND NOT APPLE)
    set_target_properties(fsed PROPERTIES INSTALL_RPATH "$ORIGIN/../lib")
endif()
install(TARGETS fsed BUNDLE DESTINATION . RUNTIME DESTINATION bin)
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/../crates/fs-automation/automation DESTINATION bin)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/../LICENSE ${CMAKE_CURRENT_SOURCE_DIR}/../THIRD_PARTY.md DESTINATION .)
if(WIN32)
    install(FILES ${FS_DEP_DLLS} DESTINATION bin)
elseif(NOT APPLE)
    install(DIRECTORY ${FS_DEPS_DIR}/lib/ DESTINATION lib FILES_MATCHING PATTERN "*.so*")
endif()
# shortcut: on macOS the vcpkg dylibs are not copied into the bundle yet; add with the macOS package job.
qt_generate_deploy_app_script(TARGET fsed OUTPUT_SCRIPT FS_DEPLOY_SCRIPT NO_UNSUPPORTED_PLATFORM_ERROR)
install(SCRIPT ${FS_DEPLOY_SCRIPT})
if(NOT WIN32 AND NOT APPLE)
    # Unpackers that skip zip symlinks leave 20-byte text stubs ("libQt6Widgets.so.6: file too short"):
    # each library goes in once as a real file under its versioned link name (the soname), dev links dropped.
    # shortcut: assumes one versioned link per library; the CI package check catches a soname this misses.
    install(CODE [[
        file(GLOB fs_libs LIST_DIRECTORIES false "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/*.so*")
        foreach(link IN LISTS fs_libs)
            if(IS_SYMLINK "${link}")
                file(REAL_PATH "${link}" real)
                file(REMOVE "${link}")
                if(NOT link MATCHES "\\.so$" AND EXISTS "${real}")
                    file(RENAME "${real}" "${link}")
                endif()
            endif()
        endforeach()
    ]])
endif()

set(CPACK_PACKAGE_NAME "FS-Editor")
set(CPACK_PACKAGE_VENDOR "FS Editor contributors")
set(CPACK_RESOURCE_FILE_LICENSE ${CMAKE_CURRENT_SOURCE_DIR}/../LICENSE)
set(CPACK_GENERATOR ZIP)
include(CPack)
