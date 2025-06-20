# Getting Started with Aeron Cluster C++ Client

This guide will help you get up and running with the Aeron Cluster C++ Client in just a few minutes.

## 🚀 Quick Start (5 minutes)

### 1. Clone and Setup
```bash
git clone https://github.com/reverb-sys/aeron-cluster-client-cpp.git
cd aeron-cluster-client-cpp
chmod +x scripts/*.sh
./scripts/setup_project.sh
```

### 2. Build
```bash
./scripts/build.sh
```

#### 2.1 Pre-requisites

In case aeron is not built and you may face this issue
```
CMake Error at CMakeLists.txt:95 (message):
    Aeron client library not found. Please build Aeron first at
    /home/ubuntu/aeron  
```

Use the Aeron setup / install script to setup Aeron

```bash
./scripts/install_aeron.sh
```



### 3. Run Example
```bash
# Make sure Aeron Media Driver is running first
# In another terminal: java -cp aeron-all.jar io.aeron.driver.MediaDriver

./build/examples/basic_client_example --help
./build/examples/basic_client_example
```

## 📋 Prerequisites

### System Requirements
- **OS**: Linux, macOS, or Windows
- **Compiler**: GCC 7+, Clang 6+, or MSVC 2019+
- **CMake**: 3.16 or newer
- **Memory**: 2GB RAM minimum
- **Disk**: 1GB free space

### Dependencies
The setup script will install these automatically:

**Required:**
- CMake 3.16+
- C++17 compatible compiler
- JsonCpp library
- pkg-config
- Aeron C++ library

**Optional:**
- Google Test (for unit tests)
- Doxygen (for documentation)

## 🛠️ Detailed Installation

### Ubuntu/Debian
```bash
# Install system dependencies
sudo apt-get update
sudo apt-get install -y build-essential cmake pkg-config libjsoncpp-dev git

# Clone and build
git clone https://github.com/reverb-sys/aeron-cluster-client-cpp.git
cd aeron-cluster-client-cpp
./scripts/setup_project.sh
./scripts/build.sh --with-tests
```

### macOS
```bash
# Install dependencies via Homebrew
brew install cmake jsoncpp pkg-config

# Clone and build
git clone https://github.com/reverb-sys/aeron-cluster-client-cpp.git
cd aeron-cluster-client-cpp
./scripts/setup_project.sh
./scripts/build.sh --with-tests
```

### Windows (MinGW/MSYS2)
```bash
# Install dependencies
pacman -S mingw-w64-x86_64-cmake mingw-w64-x86_64-jsoncpp mingw-w64-x86_64-pkg-config

# Clone and build
git clone https://github.com/reverb-sys/aeron-cluster-client-cpp.git
cd aeron-cluster-client-cpp
mkdir build && cd build
cmake -G "MinGW Makefiles" ..
make -j4
```

### Windows (Visual Studio)
```bash
# Use vcpkg for dependencies
vcpkg install jsoncpp:x64-windows

# Clone and build
git clone https://github.com/reverb-sys/aeron-cluster-client-cpp.git
cd aeron-cluster-client-cpp
mkdir build && cd build
cmake -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake ..
cmake --build . --config Release
```

## 🎯 First Steps

### 1. Understanding the Architecture

The Aeron Cluster C++ Client provides:
- **ClusterClient**: Main interface for cluster communication
- **SBE Encoding**: Proper Simple Binary Encoding for messages
- **Session Management**: Automatic connection and leader detection
- **Order Publishing**: High-level interface for trading orders

### 2. Basic Usage Example

```cpp
#include <aeron_cluster/cluster_client.hpp>

int main() {
    // Configure the client
    auto config = aeron_cluster::ClusterClientConfigBuilder()
        .withClusterEndpoints({"localhost:9002", "localhost:9102", "localhost:9202"})
        .withAeronDir("/dev/shm/aeron")
        .build();
    
    // Create and connect client
    aeron_cluster::ClusterClient client(config);
    if (!client.connect()) {
        std::cerr << "Failed to connect to cluster" << std::endl;
        return 1;
    }
    
    // Create and publish an order
    auto order = client.createSampleLimitOrder("ETH", "USDC", "BUY", 1.0, 3500.0);
    std::string messageId = client.publishOrder(order);
    
    std::cout << "Published order: " << messageId << std::endl;
    
    // Poll for responses
    while (true) {
        int messages = client.pollMessages(10);
        if (messages == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    
    return 0;
}
```

### 3. Running with Aeron Cluster

Before running your client, you need an Aeron Cluster running:

#### Option A: Use Existing Cluster
If you have an Aeron Cluster already running, just point the client to it:
```cpp
config.cluster_endpoints = {"your-cluster-host:9002", "your-cluster-host:9102"};
```

#### Option B: Start Local Test Cluster
```bash
# Download Aeron all-in-one JAR
wget https://repo1.maven.org/maven2/io/aeron/aeron-all/1.44.1/aeron-all-1.44.1.jar

# Start Media Driver
java -cp aeron-all-1.44.1.jar io.aeron.driver.MediaDriver &

# Start a simple cluster node (for testing)
java -cp aeron-all-1.44.1.jar io.aeron.cluster.ClusterNode &
```

## 🔧 Build Options

### Standard Builds
```bash
# Release build (optimized)
./scripts/build.sh

# Debug build (with debugging symbols)
./scripts/build.sh --build-type Debug

# Build with tests
./scripts/build.sh --with-tests

# Build with all features
./scripts/build.sh --build-type Release --with-tests --with-werror
```

### Advanced Options
```bash
# Clean build
./scripts/build.sh --clean

# Custom build directory
./scripts/build.sh --build-dir my-build

# Parallel jobs
./scripts/build.sh --jobs 8

# Install after building
./scripts/build.sh --install

# Create packages
./scripts/build.sh --package
```

## 📖 Examples

The repository includes several examples:

### 1. Basic Client (`examples/basic_client_example.cpp`)
- Simple connection and order publishing
- Message acknowledgment handling
- Error handling and retry logic

### 2. Order Publishing (`examples/order_publishing_example.cpp`)
- Advanced order types (limit, market, stop)
- Portfolio management integration
- Performance monitoring

### 3. Advanced Features (`examples/advanced_features_example.cpp`)
- Custom message types
- Callback-based message handling
- Connection statistics and monitoring

### Running Examples
```bash
cd build

# Basic example with help
./examples/basic_client_example --help

# Basic example with custom endpoints
./examples/basic_client_example --endpoints localhost:9002,localhost:9102

# Order publishing with debug mode
./examples/order_publishing_example --debug --orders 10

# Advanced features demo
./examples/advanced_features_example --feature callbacks
```

## 🧪 Testing

### Running Tests
```bash
# Build with tests
./scripts/build.sh --with-tests

# Run all tests
cd build && ctest

# Run specific test
./tests/test_sbe_encoding

# Run with verbose output
ctest -V
```

### Manual Testing
```bash
# Test message encoding
./tools/message_inspector --test-encoding

# Monitor cluster connectivity
./tools/cluster_monitor --endpoints localhost:9002,localhost:9102,localhost:9202
```

## 🚨 Troubleshooting

### Common Issues

#### 1. "Failed to connect to cluster"
**Symptoms:** Connection timeouts, no session established
**Solutions:**
- Ensure Aeron Media Driver is running: `ps aux | grep MediaDriver`
- Check cluster endpoints are accessible: `telnet localhost 9002`
- Verify Aeron directory permissions: `ls -la /dev/shm/aeron*`
- Check firewall settings for UDP traffic

#### 2. "Aeron library not found"
**Symptoms:** CMake configuration fails, missing Aeron headers
**Solutions:**
```bash
# Install Aeron manually
git clone https://github.com/real-logic/aeron.git
cd aeron && mkdir build && cd build
cmake .. && make -j4 && sudo make install
```

#### 3. "JsonCpp not found"
**Symptoms:** Link errors, missing json/json.h
**Solutions:**
```bash
# Ubuntu/Debian
sudo apt-get install libjsoncpp-dev

# macOS
brew install jsoncpp

# Windows (vcpkg)
vcpkg install jsoncpp
```

#### 4. Permission Denied on /dev/shm
**Symptoms:** Cannot create Aeron directory
**Solutions:**
```bash
# Create directory with proper permissions
sudo mkdir -p /dev/shm/aeron
sudo chmod 777 /dev/shm/aeron

# Or use alternative directory
export AERON_DIR=/tmp/aeron
mkdir -p /tmp/aeron
```

### Debug Mode

Enable debug logging for detailed information:
```cpp
config.debug_logging = true;
config.enable_hex_dumps = true;
```

Or use debug build:
```bash
./scripts/build.sh --build-type Debug
```

### Getting Help

1. **Check the logs**: Enable debug mode and examine output
2. **Use the tools**: `message_inspector` and `cluster_monitor` 
3. **Read the docs**: See `docs/` directory for detailed documentation
4. **Check examples**: Look at working examples in `examples/`
5. **File an issue**: Create a GitHub issue with full error details

## 🎉 Next Steps

Once you have the basic example working:

1. **Read the API documentation** in `docs/API.md`
2. **Explore advanced examples** in `examples/`
3. **Understand the SBE protocol** in `docs/PROTOCOL.md`
4. **Customize for your use case** - modify order structures, add new message types
5. **Performance tuning** - see `docs/PERFORMANCE.md`
6. **Production deployment** - see `docs/DEPLOYMENT.md`

## 🔗 Resources

- [Aeron Documentation](https://github.com/real-logic/aeron/wiki)
- [SBE Specification](https://github.com/FIXTradingCommunity/fix-simple-binary-encoding)
- [Project Documentation](docs/)
- [Examples and Tutorials](examples/)

---

**Happy coding!** 🚀

If you run into any issues, please check the troubleshooting section above or file an issue on GitHub.
