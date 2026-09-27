add_executable(tst_lzx tst_lzx.cpp check.h lzx_test_encoder.h zip_builder.h)
target_link_libraries(tst_lzx PRIVATE lzx::lzx ZLIB::ZLIB)
add_test(NAME tst_lzx COMMAND tst_lzx)

# Checks every entry of real archives; set LZX_SAMPLE_DIR to a directory of
# .zip files to run it, otherwise it reports as skipped.
add_executable(tst_realdata tst_realdata.cpp)
target_link_libraries(tst_realdata PRIVATE lzx::lzx)
add_test(NAME tst_realdata COMMAND tst_realdata)
set_tests_properties(tst_realdata PROPERTIES SKIP_RETURN_CODE 77)

if(TARGET kerfuffle_lzxzip)
    find_package(Qt6 REQUIRED COMPONENTS Widgets Test)
    find_package(KF6CoreAddons REQUIRED)
    find_package(KF6FileMetaData REQUIRED)
    add_executable(tst_arkplugin tst_arkplugin.cpp zip_builder.h lzx_test_encoder.h)
    set_target_properties(tst_arkplugin PROPERTIES AUTOMOC ON)
    target_compile_definitions(tst_arkplugin PRIVATE
        LZX_TEST_PLUGIN_FILE="$<TARGET_FILE:kerfuffle_lzxzip>")
    target_link_libraries(tst_arkplugin PRIVATE lzx::lzx kerfuffle_ark Qt6::Test ZLIB::ZLIB)
    add_dependencies(tst_arkplugin kerfuffle_lzxzip)
    add_test(NAME tst_arkplugin COMMAND tst_arkplugin)
endif()
