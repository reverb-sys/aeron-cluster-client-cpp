cmake_minimum_required(VERSION 3.16)

project(aeron-cluster-cpp
    VERSION 1.0.0
    DESCRIPTION "C++ client library for Aeron Cluster with SBE support"
    LANGUAGES CXX
)

# Set C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Build options
option(BUILD_EXAMPLES "Build example applications" ON)
option(BUILD_TESTS "Build unit tests" OFF)
option(BUILD_TOOLS "Build utility tools" ON)
option(ENABLE_WARNINGS "Enable compiler warnings" ON)
option(ENABLE_WERROR "Treat warnings as errors" OFF)

# Include CMake modules
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# Include compiler warnings setup
if(ENABLE_WARNINGS)
    include(cmake/CompilerWarnings.cmake)
endif()

# Find dependencies
find_package(PkgConfig REQUIRED)

# Find Aeron
find_package(Aeron REQUIRED)
if(NOT Aeron_FOUND)
    message(FATAL_ERROR "Aeron library not found. Please install Aeron C++ library.")
endif()

# Find JsonCpp
pkg_check_modules(JSONCPP REQUIRED jsoncpp)

# Find Threads
find_package(Threads REQUIRED)

# Set up include directories
set(AERON_CLUSTER_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include")

# Create main library
add_library(aeron-cluster-cpp STATIC
    src/cluster_client.cpp
    src/sbe_encoder.cpp
    src/session_manager.cpp
    src/message_handlers.cpp
)

# Set target properties
target_include_directories(aeron-cluster-cpp
    PUBLIC
        $<BUILD_INTERFACE:${AERON_CLUSTER_INCLUDE_DIR}>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Link libraries
target_link_libraries(aeron-cluster-cpp
    PUBLIC
        aeron
        Threads::Threads
    PRIVATE
        ${JSONCPP_LIBRARIES}
)

# Set compile definitions
target_compile_definitions(aeron-cluster-cpp
    PRIVATE
        AERON_CLUSTER_VERSION="${PROJECT_VERSION}"
)

# Apply compiler warnings
if(ENABLE_WARNINGS)
    set_target_warnings(aeron-cluster-cpp)
endif()

# Compiler-specific options
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(aeron-cluster-cpp PRIVATE -Wall -Wextra -Wpedantic)
    if(ENABLE_WERROR)
        target_compile_options(aeron-cluster-cpp PRIVATE -Werror)
    endif()
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    target_compile_options(aeron-cluster-cpp PRIVATE -Wall -Wextra -Wpedantic)
    if(ENABLE_WERROR)
        target_compile_options(aeron-cluster-cpp PRIVATE -Werror)
    endif()
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    target_compile_options(aeron-cluster-cpp PRIVATE /W4)
    if(ENABLE_WERROR)
        target_compile_options(aeron-cluster-cpp PRIVATE /WX)
    endif()
endif()

# Add subdirectories
if(BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(BUILD_TOOLS)
    add_subdirectory(tools)
endif()

# Installation
include(GNUInstallDirs)

# Install library
install(TARGETS aeron-cluster-cpp
    EXPORT aeron-cluster-cpp-targets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

# Install headers
install(DIRECTORY include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING PATTERN "*.hpp"
)

# Install CMake config
install(EXPORT aeron-cluster-cpp-targets
    FILE aeron-cluster-cpp-targets.cmake
    NAMESPACE aeron-cluster-cpp::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/aeron-cluster-cpp
)

# Create config file
include(CMakePackageConfigHelpers)

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/aeron-cluster-cpp-config.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/aeron-cluster-cpp-config.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/aeron-cluster-cpp
)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/aeron-cluster-cpp-config-version.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/aeron-cluster-cpp-config.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/aeron-cluster-cpp-config-version.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/aeron-cluster-cpp
)

# Package configuration
set(CPACK_PACKAGE_NAME "${PROJECT_NAME}")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${PROJECT_DESCRIPTION}")
set(CPACK_PACKAGE_VENDOR "Aeron Cluster C++ Contributors")
set(CPACK_PACKAGE_CONTACT "maintainer@example.com")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
set(CPACK_RESOURCE_FILE_README "${CMAKE_CURRENT_SOURCE_DIR}/README.md")

# Platform-specific packaging
if(WIN32)
    set(CPACK_GENERATOR "ZIP;NSIS")
    set(CPACK_NSIS_MODIFY_PATH ON)
elseif(APPLE)
    set(CPACK_GENERATOR "TGZ;productbuild")
else()
    set(CPACK_GENERATOR "TGZ;DEB;RPM")
    
    # DEB package settings
    set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Aeron Cluster C++ Contributors")
    set(CPACK_DEBIAN_PACKAGE_DEPENDS "libaeron-dev, libjsoncpp-dev")
    set(CPACK_DEBIAN_PACKAGE_SECTION "devel")
    
    # RPM package settings
    set(CPACK_RPM_PACKAGE_GROUP "Development/Libraries")
    set(CPACK_RPM_PACKAGE_REQUIRES "aeron-devel, jsoncpp-devel")
    set(CPACK_RPM_PACKAGE_LICENSE "MIT")
endif()

include(CPack)

# Print build configuration
message(STATUS "")
message(STATUS "Aeron Cluster C++ Build Configuration")
message(STATUS "=====================================")
message(STATUS "Version: ${PROJECT_VERSION}")
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "C++ standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "Compiler: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "Build examples: ${BUILD_EXAMPLES}")
message(STATUS "Build tests: ${BUILD_TESTS}")
message(STATUS "Build tools: ${BUILD_TOOLS}")
message(STATUS "Enable warnings: ${ENABLE_WARNINGS}")
message(STATUS "Warnings as errors: ${ENABLE_WERROR}")
message(STATUS "Install prefix: ${CMAKE_INSTALL_PREFIX}")
message(STATUS "")