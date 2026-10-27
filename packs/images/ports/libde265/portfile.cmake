# Overlay of vcpkg's libde265 port (vcpkg d46283cf) for Pulse's image preview pack.
# Change: msvc-init-scan-orders.patch. MSVC 14.51 at /O2 reads the first y bound
# of init_scan_orders() from an uninitialised stack slot: half of the diagonal
# scan-position tables stay unset and, depending on stack garbage, de265_init()
# runs past the tables and crashes inside heif.dll's DllMain (0xC0000142 for
# every pack decode, not only HEIF).
vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO strukturag/libde265
    REF "v${VERSION}"
    SHA512 111b85ea9f92cc4a93f1022fc8ac17ef436f9c32698842f041b18ccc382306fbcb5102d44966b059a750bf380c3aeba5dc5124716f253340d91cf9713f90552d
    HEAD_REF master
    PATCHES
        pkgconfig-cxx-linkage.diff
        msvc-init-scan-orders.patch
)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DENABLE_SDL=OFF
)

vcpkg_cmake_install()
vcpkg_cmake_config_fixup(CONFIG_PATH lib/cmake/libde265)
vcpkg_copy_tools(TOOL_NAMES dec265 AUTO_CLEAN)
vcpkg_fixup_pkgconfig()

if(VCPKG_LIBRARY_LINKAGE STREQUAL "static")
    file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/bin" "${CURRENT_PACKAGES_DIR}/debug/bin")
    vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/include/libde265/de265.h" "!defined(LIBDE265_STATIC_BUILD)" "0")
else()
    vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/include/libde265/de265.h" "!defined(LIBDE265_STATIC_BUILD)" "1")
endif()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/COPYING")
