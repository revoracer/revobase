cmake_minimum_required(VERSION 3.23)

project(
    revobase
    VERSION 0.1.0
    LANGUAGES CXX
)

if(WIN32 OR MSVC)
    message(FATAL_ERROR
        "revobase currently supports Linux and macOS with Clang/GCC-compatible "
        "compilers. Windows/MSVC support is not implemented.")
endif()

option(REVOBASE_BUILD_TESTS "Build revobase C++ tests" ON)
option(REVOBASE_ENABLE_WARNINGS "Enable compiler warnings for revobase targets" ON)
option(REVOBASE_ENABLE_INSTALL "Generate revobase install and package export rules" ${PROJECT_IS_TOP_LEVEL})

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(CMakePackageConfigHelpers)
include(GNUInstallDirs)

find_package(Threads REQUIRED)

function(revobase_add_dependency package_name external_dir target_name)
    if(NOT TARGET "${target_name}")
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/${external_dir}/CMakeLists.txt")
            if(REVOBASE_ENABLE_INSTALL AND "INSTALL_BUNDLED" IN_LIST ARGN)
                add_subdirectory("external/${external_dir}")
            else()
                add_subdirectory("external/${external_dir}" EXCLUDE_FROM_ALL)
            endif()
        else()
            find_package("${package_name}" CONFIG QUIET)
        endif()
    endif()

    if(NOT TARGET "${target_name}")
        message(FATAL_ERROR
            "Missing dependency ${package_name} (${target_name}). "
            "Install it as a CMake package, or initialize the external/${external_dir} submodule.")
    endif()
endfunction()

function(revobase_configure_warnings target_name)
    if(NOT REVOBASE_ENABLE_WARNINGS)
        return()
    endif()

    if(CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
        target_compile_options("${target_name}" PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

set(FMT_DOC OFF CACHE BOOL "" FORCE)
set(FMT_TEST OFF CACHE BOOL "" FORCE)
# Runtime vendored dependencies are installed with revobase when install rules
# are enabled. Catch2 remains test-only below.
set(FMT_INSTALL ${REVOBASE_ENABLE_INSTALL} CACHE BOOL "" FORCE)
revobase_add_dependency(fmt fmt fmt::fmt INSTALL_BUNDLED)

set(SPDLOG_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_TESTS_HO OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_BENCH OFF CACHE BOOL "" FORCE)
set(SPDLOG_INSTALL ${REVOBASE_ENABLE_INSTALL} CACHE BOOL "" FORCE)
set(SPDLOG_FMT_EXTERNAL ON CACHE BOOL "" FORCE)
revobase_add_dependency(spdlog spdlog spdlog::spdlog INSTALL_BUNDLED)

set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
set(JSON_Install ${REVOBASE_ENABLE_INSTALL} CACHE BOOL "" FORCE)
revobase_add_dependency(nlohmann_json nlohmann_json nlohmann_json::nlohmann_json INSTALL_BUNDLED)

add_library(revobase)
add_library(revobase::revobase ALIAS revobase)

file(GLOB_RECURSE REVOBASE_PUBLIC_HEADERS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/include/revobase/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/revobase/*.hpp"
)

file(GLOB_RECURSE REVOBASE_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cxx"
)

target_sources(revobase
    PRIVATE
        ${REVOBASE_SOURCES}
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
        FILES ${REVOBASE_PUBLIC_HEADERS}
)

target_include_directories(revobase
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(revobase
    PUBLIC
        $<BUILD_INTERFACE:fmt::fmt>
        $<BUILD_INTERFACE:spdlog::spdlog>
        $<BUILD_INTERFACE:nlohmann_json::nlohmann_json>
        $<INSTALL_INTERFACE:fmt::fmt>
        $<INSTALL_INTERFACE:spdlog::spdlog>
        $<INSTALL_INTERFACE:nlohmann_json::nlohmann_json>
        Threads::Threads
)

revobase_configure_warnings(revobase)

if(REVOBASE_ENABLE_INSTALL)
    install(TARGETS revobase
        EXPORT revobaseTargets
        FILE_SET HEADERS DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    )

    install(EXPORT revobaseTargets
        NAMESPACE revobase::
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/revobase"
    )

    configure_package_config_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/revobaseConfig.cmake.in"
        "${CMAKE_CURRENT_BINARY_DIR}/revobaseConfig.cmake"
        INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/revobase"
    )

    write_basic_package_version_file(
        "${CMAKE_CURRENT_BINARY_DIR}/revobaseConfigVersion.cmake"
        VERSION "${PROJECT_VERSION}"
        COMPATIBILITY SameMajorVersion
    )

    install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/revobaseConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/revobaseConfigVersion.cmake"
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/revobase"
    )
endif()

if(REVOBASE_BUILD_TESTS)
    include(CTest)
    if(BUILD_TESTING)
        set(REVOBASE_PUBLIC_HEADER_CHECK_SOURCES)
        set(REVOBASE_PUBLIC_HEADER_CHECK_DIR
            "${CMAKE_CURRENT_BINARY_DIR}/revobase_public_header_check"
        )
        file(MAKE_DIRECTORY "${REVOBASE_PUBLIC_HEADER_CHECK_DIR}")

        foreach(header IN LISTS REVOBASE_PUBLIC_HEADERS)
            file(RELATIVE_PATH header_include
                "${CMAKE_CURRENT_SOURCE_DIR}/include"
                "${header}"
            )
            string(MAKE_C_IDENTIFIER "${header_include}" header_id)
            set(header_check_source
                "${REVOBASE_PUBLIC_HEADER_CHECK_DIR}/${header_id}.cpp"
            )
            file(WRITE "${header_check_source}"
                "#include <${header_include}>\n"
                "void revobase_public_header_check_${header_id}() {}\n"
            )
            list(APPEND REVOBASE_PUBLIC_HEADER_CHECK_SOURCES
                "${header_check_source}"
            )
        endforeach()

        add_library(revobase_public_header_check OBJECT
            ${REVOBASE_PUBLIC_HEADER_CHECK_SOURCES}
        )
        target_link_libraries(revobase_public_header_check
            PRIVATE
                revobase::revobase
        )
        revobase_configure_warnings(revobase_public_header_check)

        set(CATCH_INSTALL_DOCS OFF CACHE BOOL "" FORCE)
        set(CATCH_INSTALL_EXTRAS OFF CACHE BOOL "" FORCE)
        set(CATCH_BUILD_TESTING OFF CACHE BOOL "" FORCE)
        revobase_add_dependency(Catch2 Catch2 Catch2::Catch2WithMain)

        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/Catch2/extras/Catch.cmake")
            list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/external/Catch2/extras")
        endif()

        file(GLOB REVOBASE_TEST_SOURCES CONFIGURE_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp"
        )

        if(REVOBASE_TEST_SOURCES)
            add_executable(revobase_tests ${REVOBASE_TEST_SOURCES})
            target_link_libraries(revobase_tests
                PRIVATE
                    revobase::revobase
                    Catch2::Catch2WithMain
            )
            target_include_directories(revobase_tests
                PRIVATE
                    "${CMAKE_CURRENT_SOURCE_DIR}/include/revobase"
            )
            revobase_configure_warnings(revobase_tests)

            include(Catch OPTIONAL RESULT_VARIABLE REVOBASE_CATCH_MODULE)
            if(REVOBASE_CATCH_MODULE)
                catch_discover_tests(revobase_tests)
            else()
                add_test(NAME revobase_tests COMMAND revobase_tests)
            endif()
        endif()
    endif()
endif()
