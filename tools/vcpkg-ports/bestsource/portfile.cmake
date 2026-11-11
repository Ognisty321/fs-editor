# BestSource R23 plus its libp2p dependency (no releases upstream, pinned
# to a commit). Built with our own CMake script instead of meson so no
# pkg-config is needed on Windows; produces one static library.
vcpkg_check_linkage(ONLY_STATIC_LIBRARY)

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO vapoursynth/bestsource
    REF bca32dac44794d93f3e580f9573c58c4a0e7671f # R23
    SHA512 f5ce479032a3812121d69e117c1ff8894ddebf014ac3cee6875ac1cf823562dc9d88515cc26259e025e1315928d0e65d869b88dcb251a58dd00ce0433da5f580
    HEAD_REF master
)

vcpkg_from_github(
    OUT_SOURCE_PATH P2P_PATH
    REPO sekrit-twc/libp2p
    REF 869fa993041f9f3af7d9ac8b10158920c6ddce66
    SHA512 b5a575648db9b74cd27c5feb1740050864f2329d65c7b05744faf9200a20efb5cb3a374856c4139bb49630dca021aac7377a4b0df7a263abaa33c429a6dbd505
    HEAD_REF master
)

file(COPY "${CMAKE_CURRENT_LIST_DIR}/CMakeLists.txt" DESTINATION "${SOURCE_PATH}")
file(COPY "${CMAKE_CURRENT_LIST_DIR}/bestsource-config.cmake.in" DESTINATION "${SOURCE_PATH}")

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS "-DP2P_SOURCE_DIR=${P2P_PATH}"
)
vcpkg_cmake_install()
vcpkg_cmake_config_fixup(PACKAGE_NAME bestsource CONFIG_PATH share/bestsource)

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
