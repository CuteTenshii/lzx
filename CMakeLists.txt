cmake_minimum_required(VERSION 3.21)

project(lzx VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(LZX_BUILD_TOOLS "Build the lzxzip command-line tool" ON)
option(LZX_BUILD_TESTS "Build the unit tests" ON)
option(LZX_BUILD_ARK_PLUGIN "Build the Ark backend for zip archives that use XMemCompress" OFF)

include(GNUInstallDirs)

find_package(ZLIB REQUIRED)

if(LZX_BUILD_ARK_PLUGIN AND WIN32)
    message(FATAL_ERROR "The Ark backend builds only on Linux and other Unix systems; turn off LZX_BUILD_ARK_PLUGIN")
endif()

# Also true for clang-cl, which takes MSVC's options.
if(MSVC)
    add_compile_options(/W4 /permissive- /utf-8)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -Wpedantic -Wconversion -Wshadow)
endif()

# Windows finds DLLs next to the executable, so keep the library's DLL beside
# the tool and the tests.
if(WIN32 AND PROJECT_IS_TOP_LEVEL AND NOT CMAKE_RUNTIME_OUTPUT_DIRECTORY)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/bin)
endif()

add_library(lzx
    include/lzx/lzx.h
    include/lzx/zip.h
    include/lzx/zip_writer.h
    src/decoder.cpp
    src/decoder.h
    src/encoder.cpp
    src/platform.cpp
    src/platform.h
    src/zip.cpp
    src/zip_writer.cpp
)
add_library(lzx::lzx ALIAS lzx)
target_include_directories(lzx
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE src
)
target_link_libraries(lzx PRIVATE ZLIB::ZLIB)
# The headers carry no export macros; with BUILD_SHARED_LIBS on Windows, CMake
# exports every symbol of the DLL instead.
set_target_properties(lzx PROPERTIES POSITION_INDEPENDENT_CODE ON WINDOWS_EXPORT_ALL_SYMBOLS ON)

install(TARGETS lzx EXPORT lzxTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(FILES include/lzx/lzx.h include/lzx/zip.h include/lzx/zip_writer.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/lzx)
install(EXPORT lzxTargets NAMESPACE lzx:: FILE lzxTargets.cmake DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/lzx)

include(CMakePackageConfigHelpers)
configure_package_config_file(cmake/lzxConfig.cmake.in ${CMAKE_CURRENT_BINARY_DIR}/lzxConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/lzx
)
write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/lzxConfigVersion.cmake COMPATIBILITY SameMinorVersion)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/lzxConfig.cmake ${CMAKE_CURRENT_BINARY_DIR}/lzxConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/lzx
)

if(LZX_BUILD_TOOLS)
    add_executable(lzxzip tools/lzxzip.cpp)
    target_link_libraries(lzxzip PRIVATE lzx::lzx $<$<PLATFORM_ID:Windows>:shell32>)
    install(TARGETS lzxzip RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

if(LZX_BUILD_ARK_PLUGIN)
    add_subdirectory(ark)
endif()

if(LZX_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
