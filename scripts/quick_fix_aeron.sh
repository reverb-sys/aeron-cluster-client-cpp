#!/bin/bash

# Quick Fix for Aeron Dependency
# ===============================
# This script provides a quick solution to get building without full Aeron installation

set -e

# Colors
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

print_info() { echo -e "${BLUE}[INFO]${NC} $1"; }
print_success() { echo -e "${GREEN}[SUCCESS]${NC} $1"; }
print_warning() { echo -e "${YELLOW}[WARNING]${NC} $1"; }

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

print_info "Quick Aeron Dependency Fix"
print_info "=========================="
print_info ""

# Option 1: Install Aeron properly
print_info "Option 1: Install Aeron C++ Library (Recommended)"
print_info "This will download and install the full Aeron library:"
print_info "  chmod +x $SCRIPT_DIR/install_aeron.sh"
print_info "  $SCRIPT_DIR/install_aeron.sh"
print_info ""

# Option 2: Modify CMakeLists.txt to be more flexible
print_info "Option 2: Create a Build without Aeron (Development Only)"
print_info "This creates a mock version for development and testing:"
print_info ""

read -p "Would you like to create a development build without Aeron? (y/N): " response

if [[ "$response" =~ ^[Yy]$ ]]; then
    print_info "Creating development build configuration..."
    
    # Create a mock Aeron header
    mkdir -p "$PROJECT_DIR/third_party/mock_aeron/include"
    
    cat > "$PROJECT_DIR/third_party/mock_aeron/include/Aeron.h" << 'EOF'
#pragma once
// Mock Aeron header for development builds
#include <memory>
#include <string>
#include <functional>
#include <cstdint>

namespace aeron {
    namespace util {
        using index_t = std::int32_t;
    }
    
    namespace logbuffer {
        class Header {
        public:
            std::int64_t sessionId() const { return 12345; }
        };
    }
    
    class AtomicBuffer {
    public:
        AtomicBuffer(uint8_t* data, size_t length) : data_(data), length_(length) {}
        const uint8_t* buffer() const { return data_; }
        size_t capacity() const { return length_; }
    private:
        uint8_t* data_;
        size_t length_;
    };
    
    class ExclusivePublication {
    public:
        bool isConnected() const { return true; }
        std::int64_t offer(AtomicBuffer& buffer, util::index_t offset, util::index_t length) {
            return 1234567890; // Mock position
        }
    };
    
    class Subscription {
    public:
        using fragment_handler_t = std::function<void(AtomicBuffer&, util::index_t, util::index_t, logbuffer::Header&)>;
        
        int poll(fragment_handler_t handler, int fragmentLimit) {
            // Mock: don't actually poll anything
            return 0;
        }
    };
    
    class Context {
    public:
        Context& aeronDir(const std::string& dir) { aeronDir_ = dir; return *this; }
        const std::string& aeronDir() const { return aeronDir_; }
        static std::string defaultAeronDir() { return "/dev/shm/aeron"; }
    private:
        std::string aeronDir_ = "/dev/shm/aeron";
    };
    
    class Aeron {
    public:
        static std::shared_ptr<Aeron> connect(const Context& context) {
            return std::make_shared<Aeron>();
        }
        
        std::int64_t addExclusivePublication(const std::string& channel, std::int32_t streamId) {
            return 1; // Mock publication ID
        }
        
        std::int64_t addSubscription(const std::string& channel, std::int32_t streamId) {
            return 1; // Mock subscription ID
        }
        
        std::shared_ptr<ExclusivePublication> findExclusivePublication(std::int64_t id) {
            return std::make_shared<ExclusivePublication>();
        }
        
        std::shared_ptr<Subscription> findSubscription(std::int64_t id) {
            return std::make_shared<Subscription>();
        }
    };
}
EOF

    # Create a development CMakeLists.txt
    cat > "$PROJECT_DIR/CMakeLists_dev.txt" << 'EOF'
# Development CMakeLists.txt with mock Aeron
cmake_minimum_required(VERSION 3.16)

project(aeron-cluster-cpp
    VERSION 1.0.0
    DESCRIPTION "C++ client library for Aeron Cluster with SBE support"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(BUILD_EXAMPLES "Build example applications" ON)
option(BUILD_TESTS "Build unit tests" OFF)
option(BUILD_TOOLS "Build utility tools" ON)
option(ENABLE_WARNINGS "Enable compiler warnings" ON)

# Find JsonCpp
find_package(PkgConfig REQUIRED)
pkg_check_modules(JSONCPP REQUIRED jsoncpp)

find_package(Threads REQUIRED)

# Set up include directories
set(AERON_CLUSTER_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include")

# Create mock aeron target
add_library(aeron INTERFACE)
target_include_directories(aeron INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/mock_aeron/include"
)

# Create main library
add_library(aeron-cluster-cpp STATIC
    src/cluster_client.cpp
    src/sbe_encoder.cpp
    src/session_manager.cpp
    src/message_handler.cpp
)

target_include_directories(aeron-cluster-cpp
    PUBLIC
        $<BUILD_INTERFACE:${AERON_CLUSTER_INCLUDE_DIR}>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(aeron-cluster-cpp
    PUBLIC
        aeron
        Threads::Threads
    PRIVATE
        ${JSONCPP_LIBRARIES}
)

target_compile_definitions(aeron-cluster-cpp
    PRIVATE
        AERON_CLUSTER_VERSION="${PROJECT_VERSION}"
        MOCK_AERON_BUILD=1
)

# Add warning about mock build
target_compile_definitions(aeron-cluster-cpp
    PUBLIC
        MOCK_AERON_WARNING="This is a development build with mock Aeron - not for production use"
)

if(BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(BUILD_TOOLS)
    add_subdirectory(tools)
endif()

message(STATUS "")
message(STATUS "⚠️  DEVELOPMENT BUILD WITH MOCK AERON")
message(STATUS "=====================================")
message(STATUS "This build uses mock Aeron headers for development only.")
message(STATUS "For production use, install real Aeron library:")
message(STATUS "  ./scripts/install_aeron.sh")
message(STATUS "")
EOF

    # Create build script for development
    cat > "$PROJECT_DIR/scripts/build_dev.sh" << 'EOF'
#!/bin/bash
set -e

PROJECT_DIR="$(dirname "$(dirname "$(realpath "$0")")")"
cd "$PROJECT_DIR"

echo "🔧 Building with mock Aeron (development only)"
echo "=============================================="

mkdir -p build_dev
cd build_dev

cmake -f ../CMakeLists_dev.txt ..
make -j$(nproc 2>/dev/null || echo 4)

echo ""
echo "✅ Development build completed!"
echo "⚠️  This build uses mock Aeron and won't connect to real clusters"
echo "For production builds, install Aeron: ./scripts/install_aeron.sh"
EOF

    chmod +x "$PROJECT_DIR/scripts/build_dev.sh"
    
    print_success "Development build configuration created!"
    print_info ""
    print_info "To build with mock Aeron (development only):"
    print_info "  ./scripts/build_dev.sh"
    print_info ""
    print_warning "This creates a development build that won't work with real Aeron clusters."
    print_warning "For production use, install real Aeron with:"
    print_info "  ./scripts/install_aeron.sh"
    print_info ""

else
    print_info "To install Aeron properly, run:"
    print_info "  chmod +x $SCRIPT_DIR/install_aeron.sh"
    print_info "  $SCRIPT_DIR/install_aeron.sh"
    print_info ""
    print_info "This will download, build, and install Aeron C++ library."
    print_info "The installation will take about 5-10 minutes."
    print_info ""
fi

print_info "After installing Aeron, you can build normally:"
print_info "  ./scripts/build.sh"
print_info ""
print_info "For more help, see:"
print_info "  - GETTING_STARTED.md"
print_info "  - https://github.com/real-logic/aeron"