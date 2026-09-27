find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Widgets)
find_package(KF6CoreAddons REQUIRED)
find_package(KF6FileMetaData REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBZIP REQUIRED IMPORTED_TARGET libzip)

set(CMAKE_AUTOMOC ON)

# Ark does not install its kerfuffle headers, so the plugin compiles against
# the Ark sources of the installed version and links the installed library.
if(NOT KERFUFFLE_LIBRARY)
    foreach(prefix IN LISTS CMAKE_PREFIX_PATH CMAKE_SYSTEM_PREFIX_PATH)
        file(GLOB candidates "${prefix}/lib*/libkerfuffle.so.*.*.*")
        if(candidates)
            list(GET candidates 0 found)
            set(KERFUFFLE_LIBRARY "${found}" CACHE FILEPATH "Ark's installed libkerfuffle")
            break()
        endif()
    endforeach()
endif()
if(NOT KERFUFFLE_LIBRARY)
    message(FATAL_ERROR "libkerfuffle not found; install Ark or set KERFUFFLE_LIBRARY")
endif()
string(REGEX MATCH "[0-9]+\\.[0-9]+\\.[0-9]+$" detected_ark_version "${KERFUFFLE_LIBRARY}")
set(LZX_ARK_VERSION "${detected_ark_version}" CACHE STRING "Ark version the plugin is built for")
set(LZX_ARK_SOURCE_DIR "" CACHE PATH "Ark source tree matching LZX_ARK_VERSION; fetched when empty")

if(NOT LZX_ARK_SOURCE_DIR)
    include(FetchContent)
    # A SOURCE_SUBDIR without a CMakeLists.txt makes this a download only;
    # only the headers are needed, not a build of Ark.
    FetchContent_Declare(ark
        GIT_REPOSITORY https://invent.kde.org/utilities/ark.git
        GIT_TAG v${LZX_ARK_VERSION}
        GIT_SHALLOW TRUE
        SOURCE_SUBDIR kerfuffle-headers
    )
    FetchContent_MakeAvailable(ark)
    set(LZX_ARK_SOURCE_DIR "${ark_SOURCE_DIR}")
endif()
if(NOT EXISTS "${LZX_ARK_SOURCE_DIR}/kerfuffle/archiveinterface.h")
    message(FATAL_ERROR "${LZX_ARK_SOURCE_DIR} is not an Ark source tree")
endif()

# Ark generates this header at build time; only the export macro is needed.
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/kerfuffle_export.h"
    "#pragma once\n#define KERFUFFLE_EXPORT __attribute__((visibility(\"default\")))\n")

# Global so the plugin's tests can link kerfuffle too.
add_library(kerfuffle_ark SHARED IMPORTED GLOBAL)
set_target_properties(kerfuffle_ark PROPERTIES
    IMPORTED_LOCATION "${KERFUFFLE_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_BINARY_DIR};${LZX_ARK_SOURCE_DIR}/kerfuffle"
)
target_link_libraries(kerfuffle_ark INTERFACE KF6::CoreAddons KF6::FileMetaData Qt6::Widgets)

# Offer the same compression methods as Ark's libzip plugin, which does the
# writing; newer methods depend on the libzip version, as in Ark.
set(ZIP_CM_ZSTD_JSON_LINE "")
set(ZIP_CM_LZMA_JSON_LINE "")
set(ZIP_CM_XZ_JSON_LINE "")
if(LIBZIP_VERSION VERSION_GREATER_EQUAL "1.8.0")
    set(ZIP_CM_ZSTD_JSON_LINE "\"Zstd\": \"Zstd\",")
    set(ZIP_CM_LZMA_JSON_LINE "\"LZMA\": \"LZMA\",")
    set(ZIP_CM_XZ_JSON_LINE "\"XZ\": \"XZ\",")
endif()
configure_file(kerfuffle_lzxzip.json.in ${CMAKE_CURRENT_BINARY_DIR}/kerfuffle_lzxzip.json)

add_library(kerfuffle_lzxzip MODULE lzxzipplugin.cpp lzxzipplugin.h)
target_link_libraries(kerfuffle_lzxzip PRIVATE lzx::lzx kerfuffle_ark)
set_target_properties(kerfuffle_lzxzip PROPERTIES
    PREFIX ""
    # Tests load the plugin from here, laid out the way Ark looks for it.
    LIBRARY_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/plugins/kerfuffle
)

set(LZX_QT_PLUGIN_DIR "${QT6_INSTALL_PLUGINS}" CACHE PATH
    "Directory, relative to the install prefix, that holds Qt's plugin folders")
install(TARGETS kerfuffle_lzxzip LIBRARY DESTINATION ${LZX_QT_PLUGIN_DIR}/kerfuffle)
