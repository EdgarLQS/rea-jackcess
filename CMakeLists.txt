cmake_minimum_required(VERSION 3.16)
project(rea_jackcess VERSION 0.1.0 LANGUAGES CXX)

# Pin the declared standard so a newer compiler default (for example C++20 with char8_t
# `path::u8string()`) cannot change what the sources mean.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

option(REA_JACKCESS_BUILD_CLI "Build the rea-jackcess command-line tool" ON)
option(REA_JACKCESS_BUILD_PGEO "Build the PGeo adapter library" ON)
option(REA_JACKCESS_BUILD_FUZZERS "Build the libFuzzer targets (requires Clang)" OFF)
option(REA_JACKCESS_INSTALL "Install libraries, headers, CMake package files and the CLI" ON)

find_package(ZLIB REQUIRED)

add_subdirectory(src)
if(REA_JACKCESS_BUILD_PGEO)
    add_subdirectory(adapters/pgeo)
endif()

if(REA_JACKCESS_BUILD_CLI)
    add_executable(rea_jackcess_cli tools/cli/main.cpp)
    target_sources(rea_jackcess_cli PRIVATE tools/cli/canonical_json.cpp)
    set_target_properties(rea_jackcess_cli PROPERTIES OUTPUT_NAME rea-jackcess CXX_EXTENSIONS OFF)
    target_include_directories(rea_jackcess_cli PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(rea_jackcess_cli PRIVATE rea_jackcess::core)
endif()

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

if(REA_JACKCESS_BUILD_FUZZERS)
    add_subdirectory(fuzz)
endif()

if(REA_JACKCESS_INSTALL)
    install(TARGETS rea_jackcess
        EXPORT rea_jackcessTargets
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
        INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
    )
    if(REA_JACKCESS_BUILD_PGEO)
        install(TARGETS rea_jackcess_pgeo
            EXPORT rea_jackcessTargets
            ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
            LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
            RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
            INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
        )
    endif()
    if(REA_JACKCESS_BUILD_CLI)
        install(TARGETS rea_jackcess_cli RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
    endif()

    install(DIRECTORY "${PROJECT_SOURCE_DIR}/include/rea_jackcess"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
    )

    set(rea_jackcess_cmake_dir "${CMAKE_INSTALL_LIBDIR}/cmake/rea_jackcess")
    install(EXPORT rea_jackcessTargets
        FILE rea_jackcessTargets.cmake
        NAMESPACE rea_jackcess::
        DESTINATION "${rea_jackcess_cmake_dir}"
    )
    configure_package_config_file(
        "${PROJECT_SOURCE_DIR}/cmake/rea_jackcessConfig.cmake.in"
        "${PROJECT_BINARY_DIR}/rea_jackcessConfig.cmake"
        INSTALL_DESTINATION "${rea_jackcess_cmake_dir}"
    )
    write_basic_package_version_file(
        "${PROJECT_BINARY_DIR}/rea_jackcessConfigVersion.cmake"
        VERSION "${PROJECT_VERSION}"
        COMPATIBILITY SameMajorVersion
    )
    install(FILES
        "${PROJECT_BINARY_DIR}/rea_jackcessConfig.cmake"
        "${PROJECT_BINARY_DIR}/rea_jackcessConfigVersion.cmake"
        DESTINATION "${rea_jackcess_cmake_dir}"
    )

    configure_file(
        "${PROJECT_SOURCE_DIR}/cmake/rea_jackcess.pc.in"
        "${PROJECT_BINARY_DIR}/rea_jackcess.pc"
        @ONLY
    )
    install(FILES "${PROJECT_BINARY_DIR}/rea_jackcess.pc"
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig"
    )

    install(FILES LICENSE NOTICE README.md
        DESTINATION "${CMAKE_INSTALL_DOCDIR}"
    )
endif()
