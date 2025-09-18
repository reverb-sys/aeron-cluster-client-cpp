#!/bin/bash

# Aeron Cluster C++ Client Project Setup Script
# ==============================================
# This script helps set up the complete project structure and dependencies

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

print_info() { echo -e "${BLUE}[INFO]${NC} $1"; }
print_success() { echo -e "${GREEN}[SUCCESS]${NC} $1"; }
print_warning() { echo -e "${YELLOW}[WARNING]${NC} $1"; }
print_error() { echo -e "${RED}[ERROR]${NC} $1"; }

# Script directory
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

print_info "Setting up Aeron Cluster C++ Client Project"
print_info "============================================"

# Detect OS
if [[ "$OSTYPE" == "linux-gnu"* ]]; then
    OS="linux"
    if command -v apt-get &> /dev/null; then
        DISTRO="debian"
    elif command -v yum &> /dev/null; then
        DISTRO="redhat"
    else
        DISTRO="unknown"
    fi
elif [[ "$OSTYPE" == "darwin"* ]]; then
    OS="macos"
elif [[ "$OSTYPE" == "msys" ]] || [[ "$OSTYPE" == "cygwin" ]]; then
    OS="windows"
else
    OS="unknown"
fi

print_info "Detected OS: $OS"

# Create all necessary directories
create_directories() {
    print_info "Creating project directory structure..."
    
    local dirs=(
        "src"
        "include/aeron_cluster"
        "examples"
        "tests"
        "tools"
        "docs"
        "cmake"
        "scripts"
        "build"
    )
    
    cd "$PROJECT_DIR"
    
    for dir in "${dirs[@]}"; do
        if [ ! -d "$dir" ]; then
            mkdir -p "$dir"
            print_info "Created directory: $dir"
        fi
    done
}

# Install system dependencies
install_dependencies() {
    print_info "Installing system dependencies..."
    
    case "$OS" in
        "linux")
            case "$DISTRO" in
                "debian")
                    print_info "Installing packages for Debian/Ubuntu..."
                    sudo apt-get update
                    sudo apt-get install -y \
                        build-essential \
                        cmake \
                        pkg-config \
                        libjsoncpp-dev \
                        git \
                        wget \
                        curl \
                        unzip
                    ;;
                "redhat")
                    print_info "Installing packages for RedHat/CentOS..."
                    sudo yum install -y \
                        gcc-c++ \
                        cmake \
                        pkgconfig \
                        jsoncpp-devel \
                        git \
                        wget \
                        curl \
                        unzip
                    ;;
                *)
                    print_warning "Unknown Linux distribution. Please install manually:"
                    print_info "  - C++ compiler (g++ or clang++)"
                    print_info "  - CMake 3.16+"
                    print_info "  - pkg-config"
                    print_info "  - JsonCpp development package"
                    ;;
            esac
            ;;
        "macos")
            if command -v brew &> /dev/null; then
                print_info "Installing packages via Homebrew..."
                brew install cmake jsoncpp pkg-config
            else
                print_warning "Homebrew not found. Please install dependencies manually:"
                print_info "  - Install Homebrew: https://brew.sh"
                print_info "  - Then run: brew install cmake jsoncpp pkg-config"
            fi
            ;;
        "windows")
            print_warning "Windows detected. Please install dependencies manually:"
            print_info "  - Visual Studio 2019+ or MinGW-w64"
            print_info "  - CMake 3.16+"
            print_info "  - vcpkg package manager recommended"
            print_info "  - Use vcpkg to install: jsoncpp"
            ;;
        *)
            print_warning "Unknown OS. Please install dependencies manually."
            ;;
    esac
}

# Download and build Aeron if not found
install_aeron() {
    print_info "Checking for Aeron installation..."
    
    # Check if Aeron is already installed
    if [ -d "/usr/local/include/aeron" ] || [ -d "/usr/include/aeron" ]; then
        print_success "Aeron already installed"
        return 0
    fi
    
    # Check if there's a local Aeron installation
    if [ -d "$PROJECT_DIR/../aeron" ]; then
        print_info "Found Aeron in parent directory"
        export AERON_ROOT="$PROJECT_DIR/../aeron"
        return 0
    fi
    
    print_warning "Aeron not found. Would you like to download and build it? (y/N)"
    read -r response
    
    if [[ "$response" =~ ^[Yy]$ ]]; then
        local aeron_dir="$PROJECT_DIR/third_party/aeron"
        
        print_info "Downloading Aeron..."
        mkdir -p "$PROJECT_DIR/third_party"
        cd "$PROJECT_DIR/third_party"
        
        if [ ! -d "aeron" ]; then
            git clone https://github.com/real-logic/aeron.git
        fi
        
        cd aeron
        
        print_info "Building Aeron C++ library..."
        mkdir -p build
        cd build
        
        cmake -DCMAKE_BUILD_TYPE=Release -DBUILD_AERON_DRIVER=ON ..
        make -j$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
        
        print_info "Installing Aeron locally..."
        sudo make install
        
        export AERON_ROOT="$aeron_dir"
        
        print_success "Aeron installed successfully"
        cd "$PROJECT_DIR"
    else
        print_warning "Skipping Aeron installation."
        print_info "Please install Aeron manually from: https://github.com/real-logic/aeron"
    fi
}

# Create CMake find modules
create_cmake_modules() {
    print_info "Creating CMake find modules..."
    
    # Create FindAeron.cmake
    cat > "$PROJECT_DIR/cmake/FindAeron.cmake" << 'EOF'
# Find Aeron C++ library
#
# This module defines:
#   Aeron_FOUND - True if Aeron is found
#   Aeron_INCLUDE_DIRS - Include directories for Aeron
#   Aeron_LIBRARIES - Libraries to link against
#   aeron - Imported target for Aeron

find_path(Aeron_INCLUDE_DIR
    NAMES Aeron.h
    PATHS
        ${AERON_ROOT}/include
        /usr/local/include
        /usr/include
    PATH_SUFFIXES aeron
)

find_library(Aeron_LIBRARY
    NAMES aeron
    PATHS
        ${AERON_ROOT}/lib
        /usr/local/lib
        /usr/lib
        /usr/lib/x86_64-linux-gnu
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Aeron
    REQUIRED_VARS Aeron_LIBRARY Aeron_INCLUDE_DIR
)

if(Aeron_FOUND)
    set(Aeron_LIBRARIES ${Aeron_LIBRARY})
    set(Aeron_INCLUDE_DIRS ${Aeron_INCLUDE_DIR})
    
    if(NOT TARGET aeron)
        add_library(aeron UNKNOWN IMPORTED)
        set_target_properties(aeron PROPERTIES
            IMPORTED_LOCATION "${Aeron_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${Aeron_INCLUDE_DIR}"
        )
    endif()
endif()

mark_as_advanced(Aeron_INCLUDE_DIR Aeron_LIBRARY)
EOF

    # Create CompilerWarnings.cmake
    cat > "$PROJECT_DIR/cmake/CompilerWarnings.cmake" << 'EOF'
# Compiler warnings configuration

function(set_target_warnings target_name)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_compile_options(${target_name} PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Wshadow
            -Wformat=2
            -Wcast-align
            -Wconversion
            -Wsign-conversion
            -Wnull-dereference
        )
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        target_compile_options(${target_name} PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Wshadow
            -Wformat=2
            -Wcast-align
            -Wconversion
            -Wsign-conversion
            -Wnull-dereference
        )
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
        target_compile_options(${target_name} PRIVATE
            /W4
            /w14242
            /w14254
            /w14263
            /w14265
            /w14287
            /we4289
            /w14296
            /w14311
            /w14545
            /w14546
            /w14547
            /w14549
            /w14555
            /w14619
            /w14640
            /w14826
            /w14905
            /w14906
            /w14928
        )
    endif()
endfunction()
EOF

    print_success "CMake modules created"
}

# Create example CMakeLists.txt files
create_example_cmake() {
    print_info "Creating example CMakeLists.txt files..."
    
    # Examples CMakeLists.txt
    cat > "$PROJECT_DIR/examples/CMakeLists.txt" << 'EOF'
# Examples

# Basic client example
add_executable(basic_client_example
    basic_client_example.cpp
)

target_link_libraries(basic_client_example
    PRIVATE
        aeron-cluster-cpp
)

if(ENABLE_WARNINGS)
    set_target_warnings(basic_client_example)
endif()

# Order publishing example
add_executable(order_publishing_example
    order_publishing_example.cpp
)

target_link_libraries(order_publishing_example
    PRIVATE
        aeron-cluster-cpp
)

if(ENABLE_WARNINGS)
    set_target_warnings(order_publishing_example)
endif()

# Advanced features example
add_executable(advanced_features_example
    advanced_features_example.cpp
)

target_link_libraries(advanced_features_example
    PRIVATE
        aeron-cluster-cpp
)

if(ENABLE_WARNINGS)
    set_target_warnings(advanced_features_example)
endif()

# Install examples
install(TARGETS 
    basic_client_example
    order_publishing_example
    advanced_features_example
    DESTINATION ${CMAKE_INSTALL_BINDIR}/examples
)
EOF

    # Tests CMakeLists.txt
    cat > "$PROJECT_DIR/tests/CMakeLists.txt" << 'EOF'
# Unit Tests

find_package(GTest QUIET)

if(GTest_FOUND)
    # SBE encoding tests
    add_executable(test_sbe_encoding
        test_sbe_encoding.cpp
    )
    
    target_link_libraries(test_sbe_encoding
        PRIVATE
            aeron-cluster-cpp
            GTest::gtest
            GTest::gtest_main
    )
    
    # Session management tests
    add_executable(test_session_management
        test_session_management.cpp
    )
    
    target_link_libraries(test_session_management
        PRIVATE
            aeron-cluster-cpp
            GTest::gtest
            GTest::gtest_main
    )
    
    # Message parsing tests
    add_executable(test_message_parsing
        test_message_parsing.cpp
    )
    
    target_link_libraries(test_message_parsing
        PRIVATE
            aeron-cluster-cpp
            GTest::gtest
            GTest::gtest_main
    )
    
    # Register tests with CTest
    include(GoogleTest)
    gtest_discover_tests(test_sbe_encoding)
    gtest_discover_tests(test_session_management)
    gtest_discover_tests(test_message_parsing)
    
else()
    message(WARNING "Google Test not found, skipping unit tests")
endif()
EOF

    # Tools CMakeLists.txt
    cat > "$PROJECT_DIR/tools/CMakeLists.txt" << 'EOF'
# Utility Tools

# Message inspector tool
add_executable(message_inspector
    message_inspector.cpp
)

target_link_libraries(message_inspector
    PRIVATE
        aeron-cluster-cpp
)

if(ENABLE_WARNINGS)
    set_target_warnings(message_inspector)
endif()

# Cluster monitor tool
add_executable(cluster_monitor
    cluster_monitor.cpp
)

target_link_libraries(cluster_monitor
    PRIVATE
        aeron-cluster-cpp
)

if(ENABLE_WARNINGS)
    set_target_warnings(cluster_monitor)
endif()

# Install tools
install(TARGETS 
    message_inspector
    cluster_monitor
    DESTINATION ${CMAKE_INSTALL_BINDIR}
)
EOF

    print_success "Example CMakeLists.txt files created"
}

# Create basic source file stubs
create_source_stubs() {
    print_info "Creating source file stubs..."
    
    # Create basic stub files that would need to be implemented
    
    # Session manager header stub
    cat > "$PROJECT_DIR/src/session_manager.hpp" << 'EOF'
#pragma once

#include "aeron_cluster/config.hpp"
#include "aeron_cluster/sbe_messages.hpp"
#include <Aeron.h>
#include <memory>

namespace aeron_cluster {

class SessionManager {
public:
    explicit SessionManager(const ClusterClientConfig& config);
    ~SessionManager();
    
    bool connect(std::shared_ptr<aeron::Aeron> aeron);
    void disconnect();
    bool isConnected() const;
    
    int64_t getSessionId() const;
    int32_t getLeaderMemberId() const;
    
    bool publishMessage(const std::string& topic,
                       const std::string& messageType,
                       const std::string& messageId,
                       const std::string& payload,
                       const std::string& headers);
    
    void handleSessionEvent(const ParseResult& result);
    
private:
    class Impl;
    std::unique_ptr<Impl> pImpl;
};

} // namespace aeron_cluster
EOF

    # Message handlers header stub
    cat > "$PROJECT_DIR/src/message_handler.hpp" << 'EOF'
#pragma once

#include "aeron_cluster/sbe_messages.hpp"

namespace aeron_cluster {

class ClusterClient;

class MessageHandler {
public:
    explicit MessageHandler(ClusterClient::Impl& client);
    ~MessageHandler();
    
    void handleMessage(const ParseResult& result);
    
private:
    class Impl;
    std::unique_ptr<Impl> pImpl;
};

} // namespace aeron_cluster
EOF

    print_success "Source file stubs created"
}

# Create configuration files
create_config_files() {
    print_info "Creating configuration files..."
    
    # .clang-format
    cat > "$PROJECT_DIR/.clang-format" << 'EOF'
---
Language: Cpp
BasedOnStyle: Google
IndentWidth: 4
TabWidth: 4
UseTab: Never
ColumnLimit: 100
BreakBeforeBraces: Attach
AllowShortIfStatementsOnASingleLine: false
AllowShortLoopsOnASingleLine: false
AllowShortFunctionsOnASingleLine: Empty
SortIncludes: true
IncludeBlocks: Regroup
EOF

    # Basic LICENSE file
    cat > "$PROJECT_DIR/LICENSE" << 'EOF'
MIT License

Copyright (c) 2025 Aeron Cluster C++ Contributors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
EOF

    print_success "Configuration files created"
}

# Make scripts executable
make_scripts_executable() {
    print_info "Making scripts executable..."
    
    chmod +x "$PROJECT_DIR/scripts"/*.sh
    
    print_success "Scripts made executable"
}

# Main setup process
main() {
    print_info "Starting project setup..."
    
    # Ask for confirmation on dependency installation
    if [[ "$OS" != "windows" ]]; then
        print_info "This script will install system dependencies. Continue? (y/N)"
        read -r response
        if [[ ! "$response" =~ ^[Yy]$ ]]; then
            print_info "Skipping dependency installation."
            SKIP_DEPS=true
        fi
    fi
    
    create_directories
    
    if [[ "$SKIP_DEPS" != "true" ]]; then
        install_dependencies
        install_aeron
    fi
    
    create_cmake_modules
    create_example_cmake
    create_source_stubs
    create_config_files
    make_scripts_executable
    
    print_success "Project setup completed!"
    print_info ""
    print_info "Next steps:"
    print_info "1. Review the generated project structure"
    print_info "2. Implement the remaining source files in src/"
    print_info "3. Build the project: ./scripts/build.sh"
    print_info "4. Run the example: ./build/examples/basic_client_example"
    print_info ""
    print_info "Project structure created at: $PROJECT_DIR"
    print_info "Build with: cd $PROJECT_DIR && ./scripts/build.sh"
    print_info ""
    
    # Show the created structure
    print_info "Created project structure:"
    if command -v tree &> /dev/null; then
        tree "$PROJECT_DIR" -I 'build*|.git'
    else
        find "$PROJECT_DIR" -type d | head -20 | sort
    fi
}

# Run main function
main "$@"