# Aeron Cluster Client for C++

[![License](https://img.shields.io/badge/License-Apache%202.0-blue.svg)](LICENSE)
[![Build Status](https://github.com/reverb-sys/aeron-cluster-client-cpp/workflows/CI/badge.svg)](https://github.com/reverb-sys/aeron-cluster-client-cpp/actions)
[![Documentation](https://img.shields.io/badge/docs-latest-brightgreen.svg)](https://reverb-sys.github.io/aeron-cluster-client-cpp/)
[![Release](https://img.shields.io/github/v/release/reverb-sys/aeron-cluster-client-cpp)](https://github.com/reverb-sys/aeron-cluster-client-cpp/releases)

A high-performance C++ client library for [Aeron Cluster](https://github.com/real-logic/aeron), providing low-latency, fault-tolerant communication with clustered services. Built for financial trading systems, real-time applications, and distributed computing environments requiring microsecond-level performance.

## Features

- 🚀 **High Performance**: Zero-copy message passing with microsecond latencies
- 🔧 **Simple Binary Encoding (SBE)**: Efficient message serialization
- 🛡️ **Fault Tolerant**: Automatic leader detection and failover
- 🔄 **Session Management**: Robust connection handling and recovery
- 📊 **Order Management**: Built-in support for trading order workflows
- ⚡ **Lock-Free**: Thread-safe, non-blocking operations
- 🎯 **Production Ready**: Memory-safe with comprehensive error handling

## Quick Start

### Prerequisites

- **Compiler**: GCC 7+, Clang 6+, or MSVC 2019+
- **CMake**: 3.16+
- **OS**: Linux, macOS, Windows

### Installation

```bash
git clone https://github.com/reverb-sys/aeron-cluster-client-cpp.git
cd aeron-cluster-client-cpp
chmod +x scripts/*.sh
./scripts/setup_project.sh
./scripts/build.sh
```

### Basic Usage

```cpp
#include <aeron_cluster/cluster_client.hpp>

int main() {
    // Configure the client
    auto config = aeron_cluster::ClusterClientConfigBuilder()
        .with_cluster_endpoints({"localhost:9002", "localhost:9102", "localhost:9202"})
        .build();
    
    // Create and connect
    aeron_cluster::ClusterClient client(config);
    if (!client.connect()) {
        return 1;
    }
    
    // Publish an order
    auto order = client.create_sample_limit_order("BTC", "USD", "BUY", 1.0, 50000.0);
    std::string messageId = client.publish_order(order);
    
    // Poll for responses
    while (client.poll_messages(10) >= 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    
    return 0;
}
```

### Run the Example

```bash
# Start Aeron Media Driver first
java -cp aeron-all.jar io.aeron.driver.MediaDriver &

# Run the basic example
./build/examples/basic_client_example

# List all arguments supported by basic_client_example
./build/examples/basic_client_example --help

# can be run in either publisher or subscriber mode
```

## Documentation

- 📚 [API Reference](docs/API.md)
- 🏗️ [Architecture Guide](docs/ARCHITECTURE.md)
- 🚀 [Performance Tuning](docs/PERFORMANCE.md)
- 📋 [Message Protocol](docs/PROTOCOL.md)
- 🔧 [Configuration Options](docs/CONFIGURATION.md)
- 🚢 [Deployment Guide](docs/DEPLOYMENT.md)

## Building from Source

### System Dependencies

<details>
<summary><strong>Ubuntu/Debian</strong></summary>

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake pkg-config libjsoncpp-dev git
```
</details>

<details>
<summary><strong>macOS</strong></summary>

```bash
brew install cmake jsoncpp pkg-config
```
</details>

<details>
<summary><strong>Windows (vcpkg)</strong></summary>

```bash
vcpkg install jsoncpp:x64-windows
```
</details>

### Build Options

```bash
# Release build (default)
./scripts/build.sh

# Debug build
./scripts/build.sh --build-type Debug

# Build with tests
./scripts/build.sh --with-tests

# Clean build
./scripts/build.sh --clean
```

### CMake Integration

```cmake
# Add to your CMakeLists.txt
find_package(AeronClusterClient REQUIRED)
target_link_libraries(your_target AeronClusterClient::client)
```

## Examples

| Example | Description |
|---------|-------------|
| [`basic_client`](examples/basic_client_example.cpp) | Simple connection and message publishing |
| [`order_publishing`](examples/order_publishing_example.cpp) | Advanced order management workflows |
| [`advanced_features`](examples/advanced_features_example.cpp) | Custom message types and callbacks |

```bash
# Run examples with help
./build/examples/basic_client_example --help

# Custom cluster endpoints
./build/examples/basic_client_example --endpoints localhost:9002,localhost:9102
```

## Testing

```bash
# Build with tests
./scripts/build.sh --with-tests

# Run test suite
cd build && ctest

# Run specific test
./tests/test_sbe_encoding

# Verbose output
ctest -V
```

## Performance Benchmarks

| Metric | Value |
|--------|-------|
| Latency (99th percentile) | < 10μs |
| Throughput | > 1M messages/sec |
| Memory overhead | < 1MB per connection |
| CPU usage | < 5% at 100K msg/sec |

*Benchmarks run on AWS c5.large with dedicated tenancy*

## Troubleshooting

### Common Issues

<details>
<summary><strong>"Failed to connect to cluster"</strong></summary>

- Ensure Aeron Media Driver is running: `ps aux | grep MediaDriver`
- Check cluster endpoints: `telnet localhost 9002`
- Verify Aeron directory permissions: `ls -la /dev/shm/aeron*`
- Check firewall settings for UDP traffic
</details>

<details>
<summary><strong>"Aeron library not found"</strong></summary>

```bash
# Install Aeron manually
git clone https://github.com/real-logic/aeron.git
cd aeron && mkdir build && cd build
cmake .. && make -j$(nproc) && sudo make install
```
</details>

<details>
<summary><strong>Permission denied on /dev/shm</strong></summary>

```bash
sudo mkdir -p /dev/shm/aeron
sudo chmod 777 /dev/shm/aeron
# Or use alternative: export AERON_DIR=/tmp/aeron
```
</details>

Enable debug logging for detailed diagnostics:
```cpp
config.debug_logging = true;
```

## Contributing

We welcome contributions! Please see our [Contributing Guide](CONTRIBUTING.md) for details.

### Development Setup

```bash
git clone https://github.com/reverb-sys/aeron-cluster-client-cpp.git
cd aeron-cluster-client-cpp
./scripts/setup_project.sh
./scripts/build.sh --with-tests --build-type Debug
```

### Code Style

- Follow [Google C++ Style Guide](https://google.github.io/styleguide/cppguide.html)
- Run `clang-format` before submitting
- Include tests for new features
- Update documentation as needed

### Reporting Issues

- Use the [issue template](.github/ISSUE_TEMPLATE.md)
- Include system information and error logs
- Provide minimal reproduction case

## Roadmap

- [ ] **v1.1**: WebSocket gateway support
- [ ] **v1.2**: Metrics and monitoring integration
- [ ] **v1.3**: Python bindings
- [ ] **v2.0**: Multi-cluster support

See our [project board](https://github.com/reverb-sys/aeron-cluster-client-cpp/projects) for detailed progress.

## License

This project is licensed under the Apache License 2.0 - see the [LICENSE](LICENSE) file for details.

## Acknowledgments

- [Aeron](https://github.com/real-logic/aeron) - High-performance messaging
- [SBE](https://github.com/real-logic/simple-binary-encoding) - Simple Binary Encoding
- All our [contributors](https://github.com/reverb-sys/aeron-cluster-client-cpp/graphs/contributors)

## Support

- 📖 [Documentation](https://reverb-sys.github.io/aeron-cluster-client-cpp/)
- 💬 [Discussions](https://github.com/reverb-sys/aeron-cluster-client-cpp/discussions)
- 🐛 [Issues](https://github.com/reverb-sys/aeron-cluster-client-cpp/issues)
- 📧 Email: support@reverb-sys.com

---

<div align="center">

**[Getting Started](#GETTING_STARTED.md)** • **[Documentation](docs/)** • **[Examples](examples/)** • **[Contributing](CONTRIBUTING.md)**

Made with ❤️ by the Reverb Systems team

</div>
