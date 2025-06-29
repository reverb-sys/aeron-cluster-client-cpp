# Aeron Cluster C++ Client

[![Build Status](https://img.shields.io/github/actions/workflow/status/reverb-sys/aeron-cluster-client-cpp/ci.yml?branch=main)](https://github.com/reverb-sys/aeron-cluster-client-cpp/actions)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![C++](https://img.shields.io/badge/C%2B%2B-17-blue.svg)](https://isocpp.org/)
[![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20macOS%20%7C%20Windows-lightgrey.svg)](https://github.com/reverb-sys/aeron-cluster-client-cpp)

A high-performance, production-ready C++ client library for [Real Logic's Aeron Cluster](https://github.com/real-logic/aeron) with comprehensive SBE (Simple Binary Encoding) support.

## ✨ Features

- 🚀 **High Performance**: >100k messages/second throughput with <1ms latency
- 🔄 **Full SBE Compliance**: Compatible with Go/Java Aeron implementations
- 🎯 **Session Management**: Automatic leader detection and failover
- 📦 **Order Publishing**: Built-in support for trading order workflows
- 🛡️ **Production Ready**: Comprehensive error handling and recovery
- 🔧 **Modern C++**: C++17 with clean, extensible architecture
- ⚡ **Easy Integration**: CMake build system with minimal dependencies

## 🚀 Quick Start

```bash
# Clone and build
git clone https://github.com/reverb-sys/aeron-cluster-client-cpp.git
cd aeron-cluster-client-cpp
./scripts/build.sh

# Run example (requires Aeron Media Driver)
./build/examples/basic_client_example
```

### Simple Usage

```cpp
#include <aeron_cluster/cluster_client.hpp>

int main() {
    // Configure client
    aeron_cluster::ClusterClientConfig config;
    config.cluster_endpoints = {"localhost:9002", "localhost:9102", "localhost:9202"};
    
    // Connect and publish
    aeron_cluster::ClusterClient client(config);
    if (client.connect()) {
        auto order = client.create_sample_limit_order("ETH", "USDC", "BUY", 1.0, 3500.0);
        client.publish_order(order);
    }
    
    return 0;
}
```

## 📚 Documentation

| Resource | Description |
|----------|-------------|
| [Getting Started](GETTING_STARTED.md) | Step-by-step setup guide |
| [API Reference](docs/API.md) | Complete API documentation |
| [Protocol Details](docs/PROTOCOL.md) | SBE protocol specification |
| [Examples](examples/) | Working code examples |
| [Troubleshooting](docs/TROUBLESHOOTING.md) | Common issues and solutions |

## 🏗️ Installation

### Prerequisites

- **Compiler**: GCC 7+, Clang 6+, or MSVC 2019+
- **CMake**: 3.16 or later
- **Dependencies**: Aeron C++, JsonCpp

### Ubuntu/Debian

```bash
sudo apt-get install cmake build-essential libjsoncpp-dev
git clone https://github.com/reverb-sys/aeron-cluster-client-cpp.git
cd aeron-cluster-client-cpp
./scripts/setup_project.sh && ./scripts/build.sh
```

### macOS

```bash
brew install cmake jsoncpp
git clone https://github.com/reverb-sys/aeron-cluster-client-cpp.git
cd aeron-cluster-client-cpp
./scripts/setup_project.sh && ./scripts/build.sh
```

See [GETTING_STARTED.md](GETTING_STARTED.md) for detailed installation instructions on all platforms.

## 🎯 Examples

### Basic Connection
```cpp
aeron_cluster::ClusterClient client(config);
client.connect();
std::cout << "Session ID: " << client.getSessionId() << std::endl;
```

### Order Publishing
```cpp
aeron_cluster::Order order{
    .id = "order_001",
    .baseToken = "BTC",
    .quoteToken = "USD",
    .side = "BUY",
    .quantity = 0.1,
    .limitPrice = 50000.0,
    .orderType = "LIMIT"
};

std::string messageId = client.publishOrder(order);
```

### Message Handling
```cpp
client.set_message_handler([](const auto& message) {
    std::cout << "Received: " << message.payload << std::endl;
});

while (client.isConnected()) {
    client.poll_messages(100);
}
```

More examples available in the [`examples/`](examples/) directory.

## 🧪 Testing

```bash
# Build with tests
./scripts/build.sh --with-tests

# Run test suite
cd build && ctest -V

# Run specific tests
./tests/test_sbe_encoding
./tests/test_session_management
```

## 🔧 Tools

The library includes helpful debugging and monitoring tools:

- **Message Inspector**: Debug SBE message encoding/decoding
- **Cluster Monitor**: Real-time cluster health monitoring

```bash
./tools/message_inspector --file message.bin
./tools/cluster_monitor --endpoints localhost:9002,localhost:9102
```

## 📊 Performance

Benchmarks on modern hardware (Intel i7, 32GB RAM):

| Metric | Value |
|--------|-------|
| Connection Time | <100ms |
| Message Throughput | >100k orders/sec |
| End-to-End Latency | <1ms (localhost) |
| Memory Usage | <50MB typical |

## 🤝 Contributing

We welcome contributions from the community! Please see our [Contributing Guide](CONTRIBUTING.md) for details.

### Development Setup

```bash
git clone https://github.com/reverb-sys/aeron-cluster-client-cpp.git
cd aeron-cluster-client-cpp
./scripts/setup_project.sh --dev
./scripts/build.sh --with-tests --build-type Debug
```

### Code Style

We use `clang-format` for consistent formatting:

```bash
./scripts/format.sh
```

## 🐛 Issues and Support

- 🐛 **Bug Reports**: [GitHub Issues](https://github.com/reverb-sys/aeron-cluster-client-cpp/issues)
- 💬 **Discussions**: [GitHub Discussions](https://github.com/reverb-sys/aeron-cluster-client-cpp/discussions)
- 📖 **Documentation**: Browse the [`docs/`](docs/) directory
- ❓ **Questions**: Use GitHub Discussions for usage questions

When reporting issues, please include:
- Operating system and version
- Compiler version
- Complete error messages
- Minimal reproduction code

## 🗺️ Roadmap

- [ ] **Enhanced Protocol Support**: Additional SBE message types
- [ ] **Connection Pooling**: Multi-session management
- [ ] **Metrics Integration**: Prometheus/OpenTelemetry support
- [ ] **Language Bindings**: Python and Node.js wrappers
- [ ] **Docker Examples**: Containerized deployment examples
- [ ] **Performance Optimizations**: Zero-copy operations, custom allocators

## 📄 License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.

## 📈 Project Status

This library is actively maintained and used in production environments. We follow semantic versioning and provide migration guides for breaking changes.

- **Stability**: Production ready
- **Maintenance**: Active development
- **Community**: Growing user base
- **Testing**: Comprehensive test coverage

---

<div align="center">

**⭐ Star this repository if you find it useful!**

[Report Bug](https://github.com/reverb-sys/aeron-cluster-client-cpp/issues) • [Request Feature](https://github.com/reverb-sys/aeron-cluster-client-cpp/issues) • [Join Discussion](https://github.com/reverb-sys/aeron-cluster-client-cpp/discussions)

</div>

---

*This library is not affiliated with Real Logic Ltd. It is an independent implementation based on the open Aeron Cluster protocol.*