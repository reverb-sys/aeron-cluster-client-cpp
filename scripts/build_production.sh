#!/bin/bash

# Production build script for Aeron Cluster C++ Client
set -e

echo "🚀 Building Aeron Cluster C++ Client for Production"
echo "=================================================="

# Configuration
BUILD_TYPE=${BUILD_TYPE:-Release}
INSTALL_PREFIX=${INSTALL_PREFIX:-/usr/local}
ENABLE_DEBUG=${ENABLE_DEBUG:-0}
OPTIMIZE_FOR_SIZE=${OPTIMIZE_FOR_SIZE:-0}

# Create build directory
BUILD_DIR="build_production"
mkdir -p $BUILD_DIR
cd $BUILD_DIR

echo "📋 Build Configuration:"
echo "  Build Type: $BUILD_TYPE"
echo "  Install Prefix: $INSTALL_PREFIX"
echo "  Debug Enabled: $ENABLE_DEBUG"
echo "  Optimize for Size: $OPTIMIZE_FOR_SIZE"
echo ""

# Configure CMake with production optimizations
CMAKE_FLAGS=(
    -DCMAKE_BUILD_TYPE=$BUILD_TYPE
    -DCMAKE_INSTALL_PREFIX=$INSTALL_PREFIX
    -DCMAKE_CXX_STANDARD=17
    -DCMAKE_CXX_STANDARD_REQUIRED=ON
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON
    -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON
    -DCMAKE_LTO=ON
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
)

# Add debug flags if enabled
if [ "$ENABLE_DEBUG" = "1" ]; then
    CMAKE_FLAGS+=(-DCMAKE_BUILD_TYPE=Debug)
    CMAKE_FLAGS+=(-DCMAKE_CXX_FLAGS_DEBUG="-g -O0")
    echo "🐛 Debug build enabled"
fi

# Add size optimization if requested
if [ "$OPTIMIZE_FOR_SIZE" = "1" ]; then
    CMAKE_FLAGS+=(-DCMAKE_CXX_FLAGS_RELEASE="-Os -DNDEBUG -flto")
    echo "📦 Size optimization enabled"
else
    CMAKE_FLAGS+=(-DCMAKE_CXX_FLAGS_RELEASE="-O3 -DNDEBUG -march=native -flto")
    echo "⚡ Performance optimization enabled"
fi

# Configure the build
echo "🔧 Configuring build..."
cmake "${CMAKE_FLAGS[@]}" ..

# Build with parallel jobs
echo "🔨 Building..."
make -j$(nproc)

# Run tests if they exist
if [ -f "tests/unit/test_config" ]; then
    echo "🧪 Running unit tests..."
    make test
fi

# Install
echo "📦 Installing..."
sudo make install

# Create package
echo "📦 Creating package..."
cpack

echo ""
echo "✅ Production build completed successfully!"
echo ""
echo "📋 Installation Summary:"
echo "  Library: $INSTALL_PREFIX/lib/libaeron-cluster-cpp.a"
echo "  Headers: $INSTALL_PREFIX/include/aeron_cluster/"
echo "  Examples: $INSTALL_PREFIX/bin/"
echo ""
echo "🔧 Usage:"
echo "  #include <aeron_cluster/cluster_client.hpp>"
echo "  # Link with: -laeron-cluster-cpp -laeron"
echo ""
echo "🐛 Debug Mode:"
echo "  Set AERON_CLUSTER_DEBUG=1 to enable debug logging"
echo ""
