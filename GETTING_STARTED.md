# Getting Started

Welcome to the Aeron Cluster C++ Client! This guide will have you up and running in under 10 minutes.

## 📋 Prerequisites

### System Requirements

| Component | Minimum | Recommended |
|-----------|---------|-------------|
| **OS** | Linux, macOS, Windows | Ubuntu 20.04+, macOS 12+, Windows 10+ |
| **Compiler** | GCC 7+, Clang 6+, MSVC 2019+ | GCC 11+, Clang 14+ |  
| **CMake** | 3.16+ | 3.20+ |
| **RAM** | 2GB | 8GB+ |
| **Disk Space** | 1GB | 2GB+ |

### Required Dependencies

The setup script will install these automatically, but you can install them manually if needed:

- **Aeron C++**: Real Logic's messaging library
- **JsonCpp**: JSON parsing and generation
- **pkg-config**: Build configuration helper

## 🚀 Quick Installation

### Option 1: Automated Setup (Recommended)

```bash
# Download and setup everything
curl -fsSL https://raw.githubusercontent.com/reverb-sys/aeron-cluster-client-cpp/main/scripts/quick_install.sh | bash

# Or manually:
git clone https://github.com/reverb-sys/aeron-cluster-client-cpp.git
cd aeron-cluster-client-cpp
./scripts/setup_project.sh && ./scripts/build.sh
```

### Option 2: Platform-Specific Installation

<details>
<summary><b>🐧 Ubuntu/Debian</b></summary>

```bash
# Install system dependencies
sudo apt-get update && sudo apt-get install -y \
    build-essential cmake pkg-config git \
    libjsoncpp-dev libcurl4-openssl-dev

# Clone and build
git clone https://github.com/reverb-sys/aeron-cluster-client-cpp.git
cd aeron-cluster-client-cpp
./scripts/build.sh --with-tests
```

</details>

<details>
<summary><b>🍎 macOS</b></summary>

```bash
# Install dependencies (requires Homebrew)
brew install cmake jsoncpp pkg-config

# Clone and build
git clone https://github.com/reverb-sys/aeron-cluster-client-cpp.git
cd aeron-cluster-client-cpp
./scripts/build.sh --with-tests
```

</details>

<details>
<summary><b>🪟 Windows</b></summary>

**Using MSYS2/MinGW64:**
```bash
# Install dependencies
pacman -S mingw-w64-x86_64-cmake mingw-w64-x86_64-jsoncpp \
          mingw-w64-x86_64-gcc mingw-w64-x86_64-pkg-config

# Clone and build
git clone https://github.com/reverb-sys/aeron-cluster-client-cpp.git
cd aeron-cluster-client-cpp
mkdir build && cd build
cmake -G "MinGW Makefiles" ..
make -j4
```

**Using Visual Studio:**
```bash
# Install dependencies with vcpkg
vcpkg install jsoncpp:x64-windows

# Clone and build
git clone https://github.com/reverb-sys/aeron-cluster-client-cpp.git
cd aeron-cluster-client-cpp
mkdir build && cd build
cmake -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake ..
cmake --build . --config Release
```

</details>

## ✅ Verify Installation

After installation, verify everything works:

```bash
# Check build succeeded
ls build/examples/basic_client_example

# Run tests
cd build && ctest --output-on-failure

# Check tools
./tools/message_inspector --help
./tools/cluster_monitor --help
```

## 🎯 Your First Application

### Step 1: Create Your Project

```bash
mkdir my-aeron-app && cd my-aeron-app
```

Create `CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.16)
project(MyAeronApp)

set(CMAKE_CXX_STANDARD 17)

# Find the installed Aeron Cluster Client
find_package(PkgConfig REQUIRED)
pkg_check_modules(AERON_CLUSTER_CLIENT REQUIRED aeron-cluster-client)

add_executable(my_app main.cpp)
target_link_libraries(my_app ${AERON_CLUSTER_CLIENT_LIBRARIES})
target_include_directories(my_app PRIVATE ${AERON_CLUSTER_CLIENT_INCLUDE_DIRS})
```

### Step 2: Write Your Application

Create `main.cpp`:

```cpp
#include <aeron_cluster/cluster_client.hpp>
#include <iostream>
#include <thread>
#include <chrono>

int main() {
    std::cout << "🚀 Starting Aeron Cluster Client Demo\n";
    
    try {
        // Configure the client
        aeron_cluster::ClusterClientConfig config;
        config.cluster_endpoints = {
            "localhost:9002", 
            "localhost:9102", 
            "localhost:9202"
        };
        config.response_channel = "aeron:udp?endpoint=localhost:0";
        config.aeron_dir = "/dev/shm/aeron";
        
        // Create and connect
        aeron_cluster::ClusterClient client(config);
        
        std::cout << "🔗 Connecting to cluster...\n";
        if (!client.connect()) {
            std::cerr << "❌ Failed to connect to cluster\n";
            return 1;
        }
        
        std::cout << "✅ Connected! Session ID: " << client.getSessionId() << "\n";
        
        // Create and publish a sample order
        std::cout << "📦 Publishing order...\n";
        
        aeron_cluster::Order order;
        order.id = "demo_order_001";
        order.baseToken = "BTC";
        order.quoteToken = "USD";
        order.side = "BUY";
        order.quantity = 0.01;
        order.limitPrice = 50000.0;
        order.orderType = "LIMIT";
        
        std::string messageId = client.publishOrder(order);
        std::cout << "📨 Order published with ID: " << messageId << "\n";
        
        // Poll for responses
        std::cout << "👂 Listening for responses (5 seconds)...\n";
        auto start = std::chrono::steady_clock::now();
        int messageCount = 0;
        
        while (std::chrono::steady_clock::now() - start < std::chrono::seconds(5)) {
            int polled = client.poll_messages(10);
            messageCount += polled;
            
            if (polled == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
        
        std::cout << "📊 Received " << messageCount << " messages\n";
        std::cout << "🔌 Disconnecting...\n";
        
        client.disconnect();
        std::cout << "✨ Demo completed successfully!\n";
        
    } catch (const std::exception& e) {
        std::cerr << "💥 Error: " << e.what() << "\n";
        return 1;
    }
    
    return 0;
}
```

### Step 3: Build and Run

```bash
mkdir build && cd build
cmake ..
make

# Make sure Aeron Media Driver is running first!
# See "Setting up Aeron" section below

./my_app
```

## 🖥️ Setting Up Aeron

Your application needs an Aeron Media Driver and optionally an Aeron Cluster for testing.

### Quick Test Setup

```bash
# Download Aeron JAR (one-time setup)
wget -O aeron-all.jar \
  "https://repo1.maven.org/maven2/io/aeron/aeron-all/1.44.1/aeron-all-1.44.1.jar"

# Start Media Driver (in background)
java -cp aeron-all.jar io.aeron.driver.MediaDriver &

# Your client can now connect!
```

### Production Cluster Setup

For production use, you'll want a proper Aeron Cluster. See the [Aeron documentation](https://github.com/real-logic/aeron/wiki) for detailed cluster setup instructions.

## 📚 Examples Walkthrough

The repository includes several examples to learn from:

### 1. Basic Client Example

```bash
./build/examples/basic_client_example --help
```

**What it demonstrates:**
- Basic connection establishment
- Session management
- Simple message publishing
- Error handling

### 2. Order Publishing Example

```bash
./build/examples/order_publishing_example --orders 5 --debug
```

**What it demonstrates:**
- Advanced order types (limit, market, stop)
- Batch order processing
- Performance monitoring
- Order acknowledgment handling

### 3. Advanced Features Example

```bash
./build/examples/advanced_features_example --feature callbacks
```

**What it demonstrates:**
- Custom message handlers
- Connection event callbacks
- Statistics and monitoring
- Configuration customization

## 🔧 Build Configuration

### Build Types

```bash
# Development build (with debug symbols)
./scripts/build.sh --build-type Debug

# Release build (optimized)
./scripts/build.sh --build-type Release

# With tests and tools
./scripts/build.sh --with-tests --with-tools

# Clean build
./scripts/build.sh --clean
```

### CMake Options

| Option | Description | Default |
|--------|-------------|---------|
| `BUILD_TESTS` | Build unit tests | `OFF` |
| `BUILD_TOOLS` | Build debugging tools | `ON` |
| `ENABLE_WERROR` | Treat warnings as errors | `OFF` |
| `BUILD_EXAMPLES` | Build example applications | `ON` |

```bash
cmake -DBUILD_TESTS=ON -DENABLE_WERROR=ON ..
```

## 🚨 Troubleshooting

### Common Issues

<details>
<summary><b>❌ "Failed to connect to cluster"</b></summary>

**Symptoms:** Connection timeouts, session not established

**Solutions:**
1. **Check Media Driver**: `ps aux | grep MediaDriver`
2. **Test connectivity**: `telnet localhost 9002`
3. **Check permissions**: `ls -la /dev/shm/aeron*`
4. **Try different directory**:
   ```bash
   export AERON_DIR=/tmp/aeron
   mkdir -p /tmp/aeron
   ```

</details>

<details>
<summary><b>🔍 "Aeron library not found"</b></summary>

**Symptoms:** CMake can't find Aeron, linking errors

**Solutions:**
1. **Manual Aeron install**:
   ```bash
   git clone https://github.com/real-logic/aeron.git
   cd aeron && mkdir build && cd build
   cmake .. && make -j4 && sudo make install
   ```

2. **Set PKG_CONFIG_PATH**:
   ```bash
   export PKG_CONFIG_PATH=/usr/local/lib/pkgconfig:$PKG_CONFIG_PATH
   ```

</details>

<details>
<summary><b>📦 "JsonCpp not found"</b></summary>

**Symptoms:** Missing json/json.h header

**Solutions:**
```bash
# Ubuntu/Debian
sudo apt-get install libjsoncpp-dev

# macOS  
brew install jsoncpp

# Windows (vcpkg)
vcpkg install jsoncpp
```

</details>

### Debug Mode

Enable detailed logging:

```cpp
aeron_cluster::ClusterClientConfig config;
config.debug_logging = true;
config.log_level = aeron_cluster::LogLevel::DEBUG;
```

Or build in debug mode:
```bash
./scripts/build.sh --build-type Debug
```

### Getting Help

1. 📖 **Check the documentation**: [`docs/`](docs/) directory
2. 🔧 **Use debugging tools**: `message_inspector`, `cluster_monitor`
3. 💬 **Ask the community**: [GitHub Discussions](https://github.com/reverb-sys/aeron-cluster-client-cpp/discussions)
4. 🐛 **Report bugs**: [GitHub Issues](https://github.com/reverb-sys/aeron-cluster-client-cpp/issues)

When asking for help, please include:
- Your operating system and version
- Complete error messages
- Steps to reproduce the issue

## 🎓 Next Steps

Now that you have the basics working:

1. **📚 Read the [API documentation](docs/API.md)** to understand all available features
2. **🔍 Explore the [examples](examples/)** for more advanced usage patterns  
3. **⚡ Learn about [performance tuning](docs/PERFORMANCE.md)** for production use
4. **🚀 Plan your [deployment strategy](docs/DEPLOYMENT.md)**
5. **🤝 Consider [contributing](CONTRIBUTING.md)** back to the project!

## 📞 Support

- 💬 **Community**: [GitHub Discussions](https://github.com/reverb-sys/aeron-cluster-client-cpp/discussions)
- 🐛 **Issues**: [GitHub Issues](https://github.com/reverb-sys/aeron-cluster-client-cpp/issues)
- 📧 **Email**: [maintainer@reverb-sys.com](mailto:maintainer@reverb-sys.com)

---

**Happy coding!** 🎉 

If you find this library useful, please ⭐ star the repository on GitHub!