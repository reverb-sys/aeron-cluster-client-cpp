#!/bin/bash

# Aeron Cluster C++ Client Build Script
# =====================================
# This script provides automated building with various configurations

set -e  # Exit on any error

# Script directory
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

# Default values
BUILD_TYPE="Release"
BUILD_DIR="build"
CLEAN_BUILD=false
BUILD_TESTS=false
BUILD_EXAMPLES=true
BUILD_TOOLS=true
ENABLE_WARNINGS=true
ENABLE_WERROR=false
JOBS=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
VERBOSE=false
INSTALL=false
PACKAGE=false

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Print colored output
print_info() {
    echo -e "${BLUE}[INFO]${NC} $1"
}

print_success() {
    echo -e "${GREEN}[SUCCESS]${NC} $1"
}

print_warning() {
    echo -e "${YELLOW}[WARNING]${NC} $1"
}

print_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# Print usage information
print_usage() {
    cat << EOF
Aeron Cluster C++ Client Build Script

Usage: $0 [options]

Build Options:
  --build-type TYPE     Build type: Debug, Release, RelWithDebInfo, MinSizeRel (default: Release)
  --build-dir DIR       Build directory (default: build)
  --clean              Clean build directory before building
  --jobs NUM           Number of parallel jobs (default: auto-detected)
  
Feature Options:
  --with-tests         Build unit tests
  --without-examples   Skip building examples
  --without-tools      Skip building utility tools
  --with-werror        Treat warnings as errors
  --without-warnings   Disable compiler warnings
  
Actions:
  --install            Install after building
  --package            Create packages after building
  --verbose            Enable verbose build output
  
Utility:
  --help               Show this help message
  --check-deps         Check for required dependencies
  --clean-all          Remove all build directories and exit

Examples:
  $0                                    # Standard release build
  $0 --build-type Debug --with-tests    # Debug build with tests
  $0 --clean --build-type Release --install  # Clean release build and install
  $0 --check-deps                       # Check dependencies only

EOF
}

# Check for required dependencies
check_dependencies() {
    print_info "Checking dependencies..."
    
    local missing_deps=()
    
    # Check for CMake
    if ! command -v cmake &> /dev/null; then
        missing_deps+=("cmake")
    else
        cmake_version=$(cmake --version | head -n1 | grep -o '[0-9]\+\.[0-9]\+\.[0-9]\+')
        print_info "Found CMake $cmake_version"
    fi
    
    # Check for compiler
    if command -v g++ &> /dev/null; then
        gcc_version=$(g++ --version | head -n1 | grep -o '[0-9]\+\.[0-9]\+\.[0-9]\+' | head -n1)
        print_info "Found GCC $gcc_version"
    elif command -v clang++ &> /dev/null; then
        clang_version=$(clang++ --version | head -n1 | grep -o '[0-9]\+\.[0-9]\+\.[0-9]\+' | head -n1)
        print_info "Found Clang $clang_version"
    else
        missing_deps+=("g++ or clang++")
    fi
    
    # Check for pkg-config
    if ! command -v pkg-config &> /dev/null; then
        missing_deps+=("pkg-config")
    fi
    
    # Check for JsonCpp
    if ! pkg-config --exists jsoncpp; then
        missing_deps+=("libjsoncpp-dev")
        print_warning "JsonCpp development package not found"
    else
        jsoncpp_version=$(pkg-config --modversion jsoncpp)
        print_info "Found JsonCpp $jsoncpp_version"
    fi
    
    # Check for Aeron (this is more complex, so just warn if not found)
    if [ ! -d "/usr/local/include/aeron" ] && [ ! -d "/usr/include/aeron" ]; then
        print_warning "Aeron headers not found in standard locations"
        print_warning "Make sure Aeron C++ library is installed"
        print_warning "See: https://github.com/real-logic/aeron for installation instructions"
    else
        print_info "Found Aeron headers"
    fi
    
    if [ ${#missing_deps[@]} -ne 0 ]; then
        print_error "Missing dependencies: ${missing_deps[*]}"
        print_info "To install on Ubuntu/Debian:"
        print_info "  sudo apt-get update"
        print_info "  sudo apt-get install cmake build-essential pkg-config libjsoncpp-dev"
        print_info ""
        print_info "To install on macOS:"
        print_info "  brew install cmake jsoncpp"
        print_info ""
        print_info "For Aeron installation, see:"
        print_info "  https://github.com/real-logic/aeron"
        return 1
    else
        print_success "All dependencies found!"
        return 0
    fi
}

# Clean build directories
clean_all() {
    print_info "Cleaning all build directories..."
    rm -rf "$PROJECT_DIR"/build*
    rm -rf "$PROJECT_DIR"/_build
    rm -rf "$PROJECT_DIR"/out
    print_success "All build directories cleaned"
}

# Parse command line arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --build-type)
            BUILD_TYPE="$2"
            shift 2
            ;;
        --build-dir)
            BUILD_DIR="$2"
            shift 2
            ;;
        --clean)
            CLEAN_BUILD=true
            shift
            ;;
        --with-tests)
            BUILD_TESTS=true
            shift
            ;;
        --without-examples)
            BUILD_EXAMPLES=false
            shift
            ;;
        --without-tools)
            BUILD_TOOLS=false
            shift
            ;;
        --with-werror)
            ENABLE_WERROR=true
            shift
            ;;
        --without-warnings)
            ENABLE_WARNINGS=false
            shift
            ;;
        --jobs)
            JOBS="$2"
            shift 2
            ;;
        --install)
            INSTALL=true
            shift
            ;;
        --package)
            PACKAGE=true
            shift
            ;;
        --verbose)
            VERBOSE=true
            shift
            ;;
        --check-deps)
            check_dependencies
            exit $?
            ;;
        --clean-all)
            clean_all
            exit 0
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

# Validate build type
case $BUILD_TYPE in
    Debug|Release|RelWithDebInfo|MinSizeRel)
        ;;
    *)
        print_error "Invalid build type: $BUILD_TYPE"
        print_info "Valid types: Debug, Release, RelWithDebInfo, MinSizeRel"
        exit 1
        ;;
esac

# Change to project directory
cd "$PROJECT_DIR"

print_info "Building Aeron Cluster C++ Client"
print_info "=================================="
print_info "Build type: $BUILD_TYPE"
print_info "Build directory: $BUILD_DIR"
print_info "Parallel jobs: $JOBS"
print_info "Build tests: $BUILD_TESTS"
print_info "Build examples: $BUILD_EXAMPLES"
print_info "Build tools: $BUILD_TOOLS"
print_info ""

# Check dependencies
if ! check_dependencies; then
    exit 1
fi

# Prepare build directory
FULL_BUILD_DIR="$PROJECT_DIR/$BUILD_DIR"

if [ "$CLEAN_BUILD" = true ] && [ -d "$FULL_BUILD_DIR" ]; then
    print_info "Cleaning build directory..."
    rm -rf "$FULL_BUILD_DIR"
fi

mkdir -p "$FULL_BUILD_DIR"
cd "$FULL_BUILD_DIR"

# Configure CMake
print_info "Configuring with CMake..."

CMAKE_ARGS=(
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
    -DBUILD_EXAMPLES="$BUILD_EXAMPLES"
    -DBUILD_TESTS="$BUILD_TESTS"
    -DBUILD_TOOLS="$BUILD_TOOLS"
    -DENABLE_WARNINGS="$ENABLE_WARNINGS"
    -DENABLE_WERROR="$ENABLE_WERROR"
)

if [ "$VERBOSE" = true ]; then
    CMAKE_ARGS+=(-DCMAKE_VERBOSE_MAKEFILE=ON)
fi

# Run CMake configure
if ! cmake "${CMAKE_ARGS[@]}" ..; then
    print_error "CMake configuration failed"
    exit 1
fi

print_success "CMake configuration completed"

# Build
print_info "Building project..."

MAKE_ARGS=(-j"$JOBS")
if [ "$VERBOSE" = true ]; then
    MAKE_ARGS+=(VERBOSE=1)
fi

if ! make "${MAKE_ARGS[@]}"; then
    print_error "Build failed"
    exit 1
fi

print_success "Build completed successfully"

# Run tests if built
if [ "$BUILD_TESTS" = true ]; then
    print_info "Running tests..."
    if ! ctest -j"$JOBS" --output-on-failure; then
        print_warning "Some tests failed"
    else
        print_success "All tests passed"
    fi
fi

# Install if requested
if [ "$INSTALL" = true ]; then
    print_info "Installing..."
    if ! make install; then
        print_error "Installation failed"
        exit 1
    fi
    print_success "Installation completed"
fi

# Create packages if requested
if [ "$PACKAGE" = true ]; then
    print_info "Creating packages..."
    if ! make package; then
        print_error "Package creation failed"
        exit 1
    fi
    print_success "Package creation completed"
    
    # List created packages
    print_info "Created packages:"
    ls -la *.tar.gz *.deb *.rpm 2>/dev/null || true
fi

# Final summary
print_success "Build process completed!"
print_info ""
print_info "Build summary:"
print_info "  Build type: $BUILD_TYPE"
print_info "  Build directory: $FULL_BUILD_DIR"
if [ "$BUILD_EXAMPLES" = true ]; then
    print_info "  Examples: $FULL_BUILD_DIR/examples/"
fi
if [ "$BUILD_TOOLS" = true ]; then
    print_info "  Tools: $FULL_BUILD_DIR/tools/"
fi
if [ "$BUILD_TESTS" = true ]; then
    print_info "  Tests: Run 'ctest' in build directory"
fi

print_info ""
print_info "To run the basic example:"
print_info "  cd $FULL_BUILD_DIR"
print_info "  ./examples/basic_client_example --help"