# Pulse image preview pack: pulse-imgpack.exe plus the LGPL libheif /
# libde265 DLLs. Dependencies come from vcpkg (manifest in this directory,
# overlay port + triplet next to it); build.ps1 installs them and points
# CMAKE_PREFIX_PATH at the installed tree.
cmake_minimum_required(VERSION 3.25)
cmake_policy(SET CMP0091 NEW)
project(pulse_imgpack LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

find_package(libheif CONFIG REQUIRED)
find_package(OpenEXR CONFIG REQUIRED)
find_package(hwy CONFIG REQUIRED)
find_package(unofficial-brotli CONFIG REQUIRED)
find_package(lcms2 CONFIG REQUIRED)
# SPNGConfig finds zlib through CMake's FindZLIB, which does not know the
# name of vcpkg's static zlib (zs.lib) outside the vcpkg toolchain.
find_library(ZLIB_LIBRARY NAMES zs zlib z REQUIRED)
find_package(SPNG CONFIG REQUIRED)
find_package(WebP CONFIG REQUIRED)
if(TARGET spng::spng_static)
    set(PULSE_SPNG_TARGET spng::spng_static)
else()
    set(PULSE_SPNG_TARGET spng::spng)
endif()

# libjxl installs no CMake package, only static libraries and pkg-config files.
find_path(JXL_INCLUDE_DIR jxl/decode.h REQUIRED)
foreach(lib jxl jxl_threads jxl_cms)
    find_library(JXL_${lib}_LIBRARY NAMES ${lib} REQUIRED)
endforeach()

if(TARGET heif)
    set(PULSE_HEIF_TARGET heif)
else()
    set(PULSE_HEIF_TARGET libheif::heif)
endif()

add_library(imgpack_pixels INTERFACE)
target_include_directories(imgpack_pixels INTERFACE src ../../src/common)

add_executable(pulse-imgpack src/imgpack.cpp)
target_include_directories(pulse-imgpack PRIVATE ${JXL_INCLUDE_DIR})
target_compile_definitions(pulse-imgpack PRIVATE
    UNICODE _UNICODE NOMINMAX SPNG_STATIC
    JXL_STATIC_DEFINE JXL_THREADS_STATIC_DEFINE JXL_CMS_STATIC_DEFINE)
target_compile_options(pulse-imgpack PRIVATE /W3 /EHsc /utf-8 /permissive- /Zc:__cplusplus)
target_link_libraries(pulse-imgpack PRIVATE
    imgpack_pixels
    ${PULSE_HEIF_TARGET}
    ${JXL_jxl_LIBRARY} ${JXL_jxl_threads_LIBRARY} ${JXL_jxl_cms_LIBRARY}
    hwy::hwy unofficial::brotli::brotlidec unofficial::brotli::brotlienc unofficial::brotli::brotlicommon
    lcms2::lcms2
    OpenEXR::OpenEXR
    ${PULSE_SPNG_TARGET} WebP::webp)
# No incremental thunks; static imports load only from the exe folder and System32.
target_link_options(pulse-imgpack PRIVATE /INCREMENTAL:NO /OPT:REF /OPT:ICF /DEPENDENTLOADFLAG:0xA00)

add_executable(imgpack_test src/imgpack_test.cpp)
target_link_libraries(imgpack_test PRIVATE imgpack_pixels)
target_compile_options(imgpack_test PRIVATE /W3 /EHsc /utf-8)

enable_testing()
add_test(NAME imgpack_test COMMAND imgpack_test)
