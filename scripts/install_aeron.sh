#!/bin/bash

# Aeron Installation Script
# =========================
# This script downloads and installs Aeron C++ library

set -e

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

print_info() { echo -e "${BLUE}[INFO]${NC} $1"; }
print_success() { echo -e "${GREEN}[SUCCESS]${NC} $1"; }
print_warning() { echo -e "${YELLOW}[WARNING]${NC} $1"; }
print_error() { echo -e "${RED}[ERROR]${NC} $1"; }

# Configuration
AERON_VERSION="1.44.1"
INSTALL_PREFIX="/usr/local"
BUILD_TYPE="Release"
PARALLEL_JOBS=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
INSTALL_SYSTEM=true
CLEAN_BUILD=false

print_usage() {
    cat << EOF
Aeron C++ Installation Script

Usage: $0 [options]

Options:
  --version VERSION     Aeron version to install (default: $AERON_VERSION)
  --prefix PATH         Installation prefix (default: $INSTALL_PREFIX)
  --build-type TYPE     Build type: Debug, Release, RelWithDebInfo (default: $BUILD_TYPE)
  --jobs NUM           Parallel build jobs (default: auto-detected)
  --local              Install to local directory instead of system
  --clean              Clean any existing build
  --help               Show this help message

Examples:
  $0                              # Install latest version system-wide
  $0 --local --prefix ~/aeron     # Install to home directory
  $0 --version 1.42.0 --clean     # Install specific version with clean build

EOF
}

# Parse command line arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --version)
            AERON_VERSION="$2"
            shift 2
            ;;
        --prefix)
            INSTALL_PREFIX="$2"
            shift 2
            ;;
        --build-type)
            BUILD_TYPE="$2"
            shift 2
            ;;
        --jobs)
            PARALLEL_JOBS="$2"
            shift 2
            ;;
        --local)
            INSTALL_SYSTEM=false
            shift
            ;;
        --clean)
            CLEAN_BUILD=true
            shift
            ;;
        --help)
            print_usage
            exit 0
            ;;
        *)
            print_error "Unknown option: $1"
            print_usage
            exit 1
            ;;
    esac
done

print_info "Aeron C++ Installation Script"
print_info "============================="
print_info "Version: $AERON_VERSION"
print_info "Install prefix: $INSTALL_PREFIX"
print_info "Build type: $BUILD_TYPE"
print_info "Parallel jobs: $PARALLEL_JOBS"
print_info "System install: $INSTALL_SYSTEM"
print_info ""

# Detect OS
if [[ "$OSTYPE" == "linux-gnu"* ]]; then
    OS="linux"
elif [[ "$OSTYPE" == "darwin"* ]]; then
    OS="macos"
else
    OS="unknown"
fi

print_info "Detected OS: $OS"

# Check dependencies
check_dependencies() {
    print_info "Checking dependencies..."
    
    local missing_deps=()
    
    # Check for essential tools
    for tool in git cmake make; do
        if ! command -v $tool &> /dev/null; then
            missing_deps+=($tool)
        fi
    done
    
    # Check for compiler
    if ! command -v g++ &> /dev/null && ! command -v clang++ &> /dev/null; then
        missing_deps+=("g++ or clang++")
    fi
    
    # Check for Java (needed for Aeron build)
    if ! command -v java &> /dev/null; then
        missing_deps+=("java")
    fi
    
    if [ ${#missing_deps[@]} -ne 0 ]; then
        print_error "Missing dependencies: ${missing_deps[*]}"
        print_info ""
        print_info "To install on Ubuntu/Debian:"
        print_info "  sudo apt-get update"
        print_info "  sudo apt-get install -y git cmake build-essential default-jdk"
        print_info ""
        print_info "To install on macOS:"
        print_info "  brew install git cmake"
        print_info "  # Java should be pre-installed"
        print_info ""
        return 1
    else
        print_success "All dependencies found"
        return 0
    fi
}

# Download Aeron source
download_aeron() {
    print_info "Downloading Aeron source..."
    
    local aeron_dir="aeron-$AERON_VERSION"
    
    if [ -d "$aeron_dir" ]; then
        if [ "$CLEAN_BUILD" = true ]; then
            print_info "Cleaning existing directory..."
            rm -rf "$aeron_dir"
        else
            print_info "Using existing directory: $aeron_dir"
            cd "$aeron_dir"
            return 0
        fi
    fi
    
    # Download from GitHub
    local download_url="https://github.com/real-logic/aeron/archive/refs/tags/${AERON_VERSION}.tar.gz"
    
    print_info "Downloading from: $download_url"
    
    if command -v wget &> /dev/null; then
        wget -O "aeron-${AERON_VERSION}.tar.gz" "$download_url"
    elif command -v curl &> /dev/null; then
        curl -L -o "aeron-${AERON_VERSION}.tar.gz" "$download_url"
    else
        print_error "Neither wget nor curl found. Please install one of them."
        return 1
    fi
    
    print_info "Extracting archive..."
    tar -xzf "aeron-${AERON_VERSION}.tar.gz"
    
    cd "$aeron_dir"
    print_success "Aeron source downloaded and extracted"
}

# Build Aeron
build_aeron() {
    print_info "Building Aeron C++ library..."
    
    # Create build directory
    local build_dir="build"
    if [ "$CLEAN_BUILD" = true ] && [ -d "$build_dir" ]; then
        rm -rf "$build_dir"
    fi
    
    mkdir -p "$build_dir"
    cd "$build_dir"
    
    # Configure with CMake
    print_info "Configuring with CMake..."
    
    local cmake_args=(
        -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
        -DCMAKE_INSTALL_PREFIX="$INSTALL_PREFIX"
        -DBUILD_AERON_DRIVER=ON
        -DBUILD_AERON_ARCHIVE_API=ON
        -DBUILD_AERON_ALL=OFF
        -DAERON_TESTS=OFF
        -DAERON_SYSTEM_TESTS=OFF
        -DAERON_BUILD_SAMPLES=OFF
        -DAERON_BUILD_BENCHMARKS=OFF
    )
    
    if ! cmake "${cmake_args[@]}" ..; then
        print_error "CMake configuration failed"
        return 1
    fi
    
    print_success "CMake configuration completed"
    
    # Build
    print_info "Building (this may take several minutes)..."
    if ! make -j"$PARALLEL_JOBS"; then
        print_error "Build failed"
        return 1
    fi
    
    print_success "Build completed successfully"
}

# Install Aeron
install_aeron() {
    print_info "Installing Aeron..."
    
    if [ "$INSTALL_SYSTEM" = true ]; then
        if ! sudo make install; then
            print_error "Installation failed"
            return 1
        fi
        
        # Update library cache on Linux
        if [ "$OS" = "linux" ]; then
            sudo ldconfig
        fi
        
        print_success "Aeron installed system-wide to $INSTALL_PREFIX"
    else
        if ! make install; then
            print_error "Installation failed"
            return 1
        fi
        
        print_success "Aeron installed locally to $INSTALL_PREFIX"
        print_info "You may need to set environment variables:"
        print_info "  export CMAKE_PREFIX_PATH=$INSTALL_PREFIX:\$CMAKE_PREFIX_PATH"
        print_info "  export LD_LIBRARY_PATH=$INSTALL_PREFIX/lib:\$LD_LIBRARY_PATH"
        print_info "  export PATH=$INSTALL_PREFIX/bin:\$PATH"
    fi
}

# Verify installation
verify_installation() {
    print_info "Verifying installation..."
    
    # Check for headers
    local header_paths=(
        "$INSTALL_PREFIX/include/aeron/Aeron.h"
        "$INSTALL_PREFIX/include/aeron/concurrent/AtomicBuffer.h"
    )
    
    for header in "${header_paths[@]}"; do
        if [ ! -f "$header" ]; then
            print_warning "Header not found: $header"
            return 1
        fi
    done
    
    # Check for libraries
    local lib_paths=(
        "$INSTALL_PREFIX/lib/libaeron.a"
        "$INSTALL_PREFIX/lib/libaeron.so"
        "$INSTALL_PREFIX/lib/libaeron.dylib"
    )
    
    local found_lib=false
    for lib in "${lib_paths[@]}"; do
        if [ -f "$lib" ]; then
            found_lib=true
            print_info "Found library: $lib"
            break
        fi
    done
    
    if [ "$found_lib" = false ]; then
        print_warning "No Aeron library found in $INSTALL_PREFIX/lib"
        return 1
    fi
    
    print_success "Installation verified successfully"
    return 0
}

# Create a simple test program
create_test_program() {
    print_info "Creating test program..."
    
    cat > aeron_test.cpp << 'EOF'
#include <Aeron.h>
#include <iostream>

int main() {
    try {
        aeron::Context context;
        std::cout << "✅ Aeron headers found and compilable" << std::endl;
        std::cout << "📦 Aeron version: " << aeron::Context::defaultAeronDir() << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "❌ Error: " << e.what() << std::endl;
        return 1;
    }
}
EOF

    # Try to compile the test program
    print_info "Compiling test program..."
    
    local compile_cmd="g++ -std=c++17 -I$INSTALL_PREFIX/include -L$INSTALL_PREFIX/lib -laeron aeron_test.cpp -o aeron_test"
    
    if $compile_cmd 2>/dev/null; then
        print_success "Test program compiled successfully"
        
        # Try to run it
        if [ "$INSTALL_SYSTEM" = false ]; then
            export LD_LIBRARY_PATH="$INSTALL_PREFIX/lib:$LD_LIBRARY_PATH"
        fi
        
        if ./aeron_test; then
            print_success "Test program runs successfully"
        else
            print_warning "Test program compilation succeeded but execution failed"
            print_info "You may need to set LD_LIBRARY_PATH or install system-wide"
        fi
    else
        print_warning "Test program compilation failed"
        print_info "This might indicate missing dependencies or configuration issues"
    fi
    
    # Clean up
    rm -f aeron_test.cpp aeron_test
}

# Main installation process
main() {
    local start_dir="$(pwd)"
    local work_dir="/tmp/aeron-install-$$"
    
    # Create temporary work directory
    mkdir -p "$work_dir"
    cd "$work_dir"
    
    # Ensure cleanup on exit
    trap "cd '$start_dir'; rm -rf '$work_dir'" EXIT
    
    print_info "Working in temporary directory: $work_dir"
    print_info ""
    
    # Check dependencies
    if ! check_dependencies; then
        exit 1
    fi
    
    # Download source
    if ! download_aeron; then
        exit 1
    fi
    
    # Build
    if ! build_aeron; then
        exit 1
    fi
    
    # Install
    if ! install_aeron; then
        exit 1
    fi
    
    # Verify
    if ! verify_installation; then
        print_warning "Installation completed but verification failed"
        print_info "You may still be able to use Aeron, but there might be issues"
    fi
    
    # Create test program
    create_test_program
    
    print_info ""
    print_success "Aeron installation completed!"
    print_info ""
    print_info "Next steps:"
    print_info "1. Try building your project again: ./scripts/build.sh"
    print_info "2. If using local install, set environment variables as shown above"
    print_info "3. Check the Aeron documentation: https://github.com/real-logic/aeron"
    print_info ""
    print_info "To start the Aeron Media Driver:"
    if [ "$INSTALL_SYSTEM" = true ]; then
        print_info "  sudo $INSTALL_PREFIX/bin/aeronmd"
    else
        print_info "  $INSTALL_PREFIX/bin/aeronmd"
    fi
    print_info ""
}

# Run main function
main "$@"