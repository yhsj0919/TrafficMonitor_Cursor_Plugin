cmake_minimum_required(VERSION 3.21)

set(CURSOR_USAGE_VERSION "0.1.0" CACHE STRING "CursorUsage plugin version")

project(CursorUsage VERSION "${CURSOR_USAGE_VERSION}" LANGUAGES C CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)
FetchContent_Declare(
    sqlite_amalgamation
    URL https://www.sqlite.org/2024/sqlite-amalgamation-3460100.zip
)
FetchContent_GetProperties(sqlite_amalgamation)
if(NOT sqlite_amalgamation_POPULATED)
    FetchContent_Populate(sqlite_amalgamation)
endif()

if(NOT EXISTS "${sqlite_amalgamation_SOURCE_DIR}/sqlite3.c")
    message(FATAL_ERROR "SQLite amalgamation not found after FetchContent")
endif()

add_library(sqlite3 STATIC "${sqlite_amalgamation_SOURCE_DIR}/sqlite3.c")
set_target_properties(sqlite3 PROPERTIES LINKER_LANGUAGE C)
target_include_directories(sqlite3 PUBLIC "${sqlite_amalgamation_SOURCE_DIR}")
target_compile_definitions(sqlite3 PRIVATE
    SQLITE_OMIT_LOAD_EXTENSION
    SQLITE_THREADSAFE=1
    SQLITE_DQS=0
)
if(MSVC)
    target_compile_options(sqlite3 PRIVATE /W0)
else()
    target_compile_options(sqlite3 PRIVATE -w)
endif()

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/CursorUsageVersion.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/CursorUsageVersion.h"
    @ONLY
)

set(TRAFFICMONITOR_INCLUDE_DIR "" CACHE PATH "Directory containing TrafficMonitor PluginInterface.h")

if(NOT TRAFFICMONITOR_INCLUDE_DIR)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/TrafficMonitor/include/PluginInterface.h")
        set(TRAFFICMONITOR_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/TrafficMonitor/include")
    elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../TrafficMonitor/include/PluginInterface.h")
        set(TRAFFICMONITOR_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../TrafficMonitor/include")
    else()
        message(FATAL_ERROR "PluginInterface.h not found. Set TRAFFICMONITOR_INCLUDE_DIR or checkout TrafficMonitor into this source tree.")
    endif()
endif()

add_library(CursorUsageCore STATIC
    src/CursorUsageCore.cpp
    src/JsonLite.cpp
    src/CursorUsageConfig.cpp
    src/CursorSqlite.cpp
    src/CursorSafeStorage.cpp
)

target_include_directories(CursorUsageCore PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_BINARY_DIR}/generated"
)

target_compile_definitions(CursorUsageCore PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)

if(MSVC)
    target_compile_options(CursorUsageCore PRIVATE /utf-8)
endif()

target_link_libraries(CursorUsageCore PUBLIC winhttp sqlite3 crypt32 bcrypt)

add_library(CursorUsage SHARED
    src/CursorUsagePlugin.cpp
    src/CursorUsageFetcher.cpp
    src/CursorUsageOptions.cpp
    src/CursorUsage.rc
)

target_include_directories(CursorUsage PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${TRAFFICMONITOR_INCLUDE_DIR}"
)

target_compile_definitions(CursorUsage PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)

if(MSVC)
    target_compile_options(CursorUsage PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/utf-8>)
endif()

target_link_libraries(CursorUsage PRIVATE
    CursorUsageCore
    comdlg32
    shlwapi
    winhttp
)

set_target_properties(CursorUsage PROPERTIES
    PREFIX ""
    OUTPUT_NAME "CursorUsage"
)

add_executable(CursorUsageTests
    tests/CursorUsageCoreTests.cpp
    tests/CursorUsageConfigTests.cpp
    src/CursorUsageFetcher.cpp
)

target_compile_definitions(CursorUsageTests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)

target_link_libraries(CursorUsageTests PRIVATE CursorUsageCore ws2_32)

if(MSVC)
    target_compile_options(CursorUsageTests PRIVATE /utf-8)
endif()

enable_testing()
add_test(NAME CursorUsageTests COMMAND CursorUsageTests)
