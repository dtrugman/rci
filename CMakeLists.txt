cmake_minimum_required (VERSION 3.5)

option (BUILD_SHARED_LIBS "Build shared library (.so) instead of static one (.a)" ON)

if (NOT CMAKE_BUILD_TYPE)
    set (CMAKE_BUILD_TYPE Debug)
endif ()
message (STATUS "Build type: ${CMAKE_BUILD_TYPE}")

add_compile_options (-std=c++11 -Wall -Wextra -pedantic -Werror)

include_directories (include)

set (CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/out)
set (CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set (CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

set (ROOT_SOURCE_DIR src)
aux_source_directory (${ROOT_SOURCE_DIR} ROOT_SOURCES)
set (SOURCES ${ROOT_SOURCES})

add_library (rci ${SOURCES})

if (CMAKE_BUILD_TYPE MATCHES Debug)
    message (STATUS "Enabling address sanitizer")
    set (SANITIZE_FLAGS -fno-omit-frame-pointer -fsanitize=address -fsanitize=leak)
    target_compile_options (rci PUBLIC ${SANITIZE_FLAGS})
    target_link_libraries (rci PUBLIC -lasan ${SANITIZE_FLAGS})
endif ()

aux_source_directory (sample/proconn PROCONN_SOURCES)
add_executable (proconn ${PROCONN_SOURCES})
target_include_directories (proconn PRIVATE sample)
target_link_libraries (proconn PRIVATE rci)

set (UNITTEST_SOURCE_DIR test)
aux_source_directory (${UNITTEST_SOURCE_DIR} UNITTEST_SOURCES)
add_executable (unittest ${UNITTEST_SOURCES})
target_link_libraries (unittest PRIVATE rci pthread)
