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

# Set Aeron root directory (separate from our client code)
set(AERON_ROOT /home/ubuntu/aeron)

# Find required packages
find_package(Threads REQUIRED)  # <-- THIS WAS MISSING!
find_package(PkgConfig REQUIRED)

# Find jsoncpp
pkg_check_modules(JSONCPP jsoncpp)

# If pkg-config doesn't find it, try manual search
if(NOT JSONCPP_FOUND)
    find_path(JSONCPP_INCLUDE_DIRS json/json.h
        PATHS
            /usr/include/jsoncpp
            /usr/local/include/jsoncpp
            /usr/include
            /usr/local/include
    )
    
    find_library(JSONCPP_LIBRARIES
        NAMES jsoncpp
        PATHS
            /usr/lib
            /usr/local/lib
            /usr/lib/x86_64-linux-gnu
    )
    
    if(JSONCPP_INCLUDE_DIRS AND JSONCPP_LIBRARIES)
        set(JSONCPP_FOUND TRUE)
        message(STATUS "Found jsoncpp manually: ${JSONCPP_LIBRARIES}")
    else()
        message(FATAL_ERROR "jsoncpp not found. Install with: sudo apt-get install libjsoncpp-dev")
    endif()
else()
    message(STATUS "Found jsoncpp via pkg-config: ${JSONCPP_LIBRARIES}")
endif()

# Find Aeron C++ include directories - check multiple possible locations
set(AERON_CLUSTER_INCLUDE_DIR
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${AERON_ROOT}/aeron-client/src/main/cpp
    ${AERON_ROOT}/aeron-client/src/main/c
    ${AERON_ROOT}/aeron-driver/src/main/c
)

# Also check if headers are in the install location
if(EXISTS "${AERON_ROOT}/cppbuild/Release/include/aeron")
    list(APPEND AERON_CLUSTER_INCLUDE_DIR ${AERON_ROOT}/cppbuild/Release/include)
endif()

if(EXISTS "${AERON_ROOT}/cppbuild/Debug/include/aeron")
    list(APPEND AERON_CLUSTER_INCLUDE_DIR ${AERON_ROOT}/cppbuild/Debug/include)
endif()

# Find Aeron libraries (look in build directory first, then cppbuild)
find_library(AERON_CLIENT_LIBRARY
    NAMES aeron_client aeron
    PATHS
        ${AERON_ROOT}/build/lib
        ${AERON_ROOT}/cppbuild/Release/lib
        ${AERON_ROOT}/cppbuild/Debug/lib
    NO_DEFAULT_PATH
)

find_library(AERON_DRIVER_LIBRARY
    NAMES aeron_driver
    PATHS
        ${AERON_ROOT}/build/lib
        ${AERON_ROOT}/cppbuild/Release/lib
        ${AERON_ROOT}/cppbuild/Debug/lib
    NO_DEFAULT_PATH
)

# Check if we found the libraries
if(NOT AERON_CLIENT_LIBRARY)
    message(FATAL_ERROR "Aeron client library not found. Please build Aeron first at ${AERON_ROOT}")
endif()

message(STATUS "Found Aeron client library: ${AERON_CLIENT_LIBRARY}")
if(AERON_DRIVER_LIBRARY)
    message(STATUS "Found Aeron driver library: ${AERON_DRIVER_LIBRARY}")
endif()

# Debug: Print all include directories we're checking
foreach(dir ${AERON_CLUSTER_INCLUDE_DIR})
    if(EXISTS ${dir})
        message(STATUS "Found include directory: ${dir}")
        if(EXISTS "${dir}/aeron")
            message(STATUS "  - Contains aeron subdirectory")
        endif()
        if(EXISTS "${dir}/Aeron.h")
            message(STATUS "  - Contains Aeron.h directly")
        endif()
    else()
        message(STATUS "Include directory does not exist: ${dir}")
    endif()
endforeach()

# Include directories (updated to include jsoncpp)
include_directories(${AERON_CLUSTER_INCLUDE_DIR})
include_directories(${JSONCPP_INCLUDE_DIRS})

# Create aeron target for linking (since we're not using find_package)
add_library(aeron STATIC IMPORTED)
set_target_properties(aeron PROPERTIES
    IMPORTED_LOCATION ${AERON_CLIENT_LIBRARY}
    INTERFACE_INCLUDE_DIRECTORIES "${AERON_CLUSTER_INCLUDE_DIR}"
)

# Create main library
add_library(aeron-cluster-cpp STATIC
    src/cluster_client.cpp
    src/cluster_client_offer.cpp
    src/sbe_encoder.cpp
    src/session_manager.cpp
    src/message_handler.cpp
    src/config.cpp
    src/order_types.cpp
)

# Set target properties
target_include_directories(aeron-cluster-cpp
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${AERON_CLUSTER_INCLUDE_DIR}
        ${JSONCPP_INCLUDE_DIRS}
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