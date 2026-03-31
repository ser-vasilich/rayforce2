cmake_minimum_required(VERSION 3.15)
project(rayforce LANGUAGES C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(RAYFORCE_PORTABLE "Build portable binary (no -march=native)" OFF)
option(RAYFORCE_WERROR "Treat warnings as errors" OFF)

# Collect all source files under src/
file(GLOB_RECURSE RAYFORCE_SOURCES CONFIGURE_DEPENDS "src/**/*.c")
list(FILTER RAYFORCE_SOURCES EXCLUDE REGEX "src/lang/repl\\.c$")

# Static library
add_library(rayforce_static STATIC ${RAYFORCE_SOURCES})
set_target_properties(rayforce_static PROPERTIES OUTPUT_NAME rayforce)
target_include_directories(rayforce_static PUBLIC include PRIVATE src)

# Shared library
add_library(rayforce_lib SHARED ${RAYFORCE_SOURCES})
set_target_properties(rayforce_lib PROPERTIES OUTPUT_NAME rayforce)
target_include_directories(rayforce_lib PUBLIC include PRIVATE src)

# Platform-specific compiler flags
if(MSVC)
    set(RAYFORCE_WARNINGS /W4 /WX)
    set(RAYFORCE_DEBUG_FLAGS /Od /Zi /fsanitize=address)
    set(RAYFORCE_RELEASE_FLAGS /O2 /DNDEBUG /arch:AVX2)
else()
    set(RAYFORCE_WARNINGS -Wall -Wextra)
    if(RAYFORCE_WERROR)
        list(APPEND RAYFORCE_WARNINGS -Werror)
    endif()
    set(RAYFORCE_DEBUG_FLAGS -O0 -g -fsanitize=address,undefined)
    if(RAYFORCE_PORTABLE)
        set(RAYFORCE_RELEASE_FLAGS -O3 -mtune=generic -DNDEBUG)
    else()
        set(RAYFORCE_RELEASE_FLAGS -O3 -march=native -DNDEBUG)
    endif()
endif()

# Apply flags to both library targets
foreach(tgt rayforce_static rayforce_lib)
    target_compile_options(${tgt} PRIVATE ${RAYFORCE_WARNINGS})
    target_compile_options(${tgt} PRIVATE
        $<$<CONFIG:Debug>:${RAYFORCE_DEBUG_FLAGS}>
        $<$<CONFIG:Release>:${RAYFORCE_RELEASE_FLAGS}>
    )
endforeach()

# Link sanitizer runtime for debug builds (GCC/Clang)
if(NOT MSVC)
    foreach(tgt rayforce_static rayforce_lib)
        target_link_options(${tgt} PRIVATE
            $<$<CONFIG:Debug>:-fsanitize=address,undefined>
        )
    endforeach()
endif()

# Platform-specific link libraries
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(rayforce_static PUBLIC m pthread)
    target_link_libraries(rayforce_lib PUBLIC m pthread)
endif()

# Rayfall REPL binary
# Version and build info for REPL banner
set(RAYFORCE_VERSION "0.1.0")
execute_process(
    COMMAND git rev-parse --short HEAD
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE RAYFORCE_GIT_COMMIT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
if(NOT RAYFORCE_GIT_COMMIT)
    set(RAYFORCE_GIT_COMMIT "unknown")
endif()
string(TIMESTAMP RAYFORCE_BUILD_DATE "%Y-%m-%d" UTC)

# Version defines for library targets (banner in src/app/repl.c)
foreach(tgt rayforce_static rayforce_lib)
    target_compile_definitions(${tgt} PRIVATE
        RAYFORCE_VERSION="${RAYFORCE_VERSION}"
        RAYFORCE_GIT_COMMIT="${RAYFORCE_GIT_COMMIT}"
        RAYFORCE_BUILD_DATE="${RAYFORCE_BUILD_DATE}")
endforeach()

add_executable(rayforce src/lang/repl.c)
target_link_libraries(rayforce PRIVATE rayforce_static)
target_include_directories(rayforce PRIVATE include src)
target_compile_definitions(rayforce PRIVATE
    RAYFORCE_VERSION="${RAYFORCE_VERSION}"
    RAYFORCE_GIT_COMMIT="${RAYFORCE_GIT_COMMIT}"
    RAYFORCE_BUILD_DATE="${RAYFORCE_BUILD_DATE}")
target_compile_options(rayforce PRIVATE ${RAYFORCE_WARNINGS})
target_compile_options(rayforce PRIVATE
    $<$<CONFIG:Debug>:${RAYFORCE_DEBUG_FLAGS}>
    $<$<CONFIG:Release>:${RAYFORCE_RELEASE_FLAGS}>
)
if(NOT MSVC)
    target_link_options(rayforce PRIVATE
        $<$<CONFIG:Debug>:-fsanitize=address,undefined>
    )
endif()

# Fuzz targets (requires clang with libFuzzer)
option(RAYFORCE_FUZZ "Build fuzz testing targets" OFF)
if(RAYFORCE_FUZZ)
    file(GLOB FUZZ_SOURCES CONFIGURE_DEPENDS "fuzz/*.c")
    foreach(fuzz_src ${FUZZ_SOURCES})
        get_filename_component(fuzz_name ${fuzz_src} NAME_WE)
        add_executable(${fuzz_name} ${fuzz_src})
        target_link_libraries(${fuzz_name} PRIVATE rayforce_static)
        target_include_directories(${fuzz_name} PRIVATE include src)
        target_compile_options(${fuzz_name} PRIVATE
            -fsanitize=fuzzer,address,undefined -g -O1)
        target_link_options(${fuzz_name} PRIVATE
            -fsanitize=fuzzer,address,undefined)
    endforeach()
endif()

# Benchmark targets
option(RAYFORCE_BENCH "Build benchmark executables" OFF)
if(RAYFORCE_BENCH)
    file(GLOB BENCH_SOURCES CONFIGURE_DEPENDS "bench/*.c")
    foreach(bench_src ${BENCH_SOURCES})
        get_filename_component(bench_name ${bench_src} NAME_WE)
        add_executable(${bench_name} ${bench_src})
        target_link_libraries(${bench_name} PRIVATE rayforce_static)
        target_include_directories(${bench_name} PRIVATE include src)
        target_compile_options(${bench_name} PRIVATE ${RAYFORCE_RELEASE_FLAGS})
    endforeach()
endif()

# Example targets
option(RAYFORCE_EXAMPLES "Build example executables" OFF)
if(RAYFORCE_EXAMPLES)
    file(GLOB EXAMPLE_SOURCES CONFIGURE_DEPENDS "examples/*.c")
    foreach(ex_src ${EXAMPLE_SOURCES})
        get_filename_component(ex_name ${ex_src} NAME_WE)
        add_executable(example_${ex_name} ${ex_src})
        target_link_libraries(example_${ex_name} PRIVATE rayforce_static)
        target_include_directories(example_${ex_name} PRIVATE include src)
        if(NOT MSVC)
            target_link_options(example_${ex_name} PRIVATE
                $<$<CONFIG:Debug>:-fsanitize=address,undefined>
            )
        endif()
    endforeach()
endif()

# Test executable
enable_testing()
file(GLOB TEST_SOURCES CONFIGURE_DEPENDS "test/*.c")
if(TEST_SOURCES)
    add_executable(test_rayforce ${TEST_SOURCES})
    target_link_libraries(test_rayforce PRIVATE rayforce_static)
    target_include_directories(test_rayforce PRIVATE include src test)
    target_compile_options(test_rayforce PRIVATE ${RAYFORCE_WARNINGS})
    target_compile_options(test_rayforce PRIVATE
        $<$<CONFIG:Debug>:${RAYFORCE_DEBUG_FLAGS}>
        $<$<CONFIG:Release>:${RAYFORCE_RELEASE_FLAGS}>
    )
    if(NOT MSVC)
        target_link_options(test_rayforce PRIVATE
            $<$<CONFIG:Debug>:-fsanitize=address,undefined>
        )
    endif()
    add_test(NAME test_rayforce COMMAND test_rayforce)
endif()

# Install targets
include(GNUInstallDirs)
install(TARGETS rayforce_lib rayforce_static
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
install(FILES include/rayforce.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

# pkg-config
configure_file(rayforce.pc.in rayforce.pc @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/rayforce.pc
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig
)
