find_package(GTest REQUIRED)

# Test utilities library
add_library(aeron-cluster-test-utils STATIC
    test_utils/mock_aeron.cpp
    test_utils/test_cluster.cpp
    test_utils/message_builders.cpp
)

target_link_libraries(aeron-cluster-test-utils
    PUBLIC
        aeron-cluster-cpp
        GTest::gtest
        GTest::gmock
)

target_include_directories(aeron-cluster-test-utils
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/test_utils
)

# Test executable
add_executable(aeron-cluster-tests
    unit/test_config.cpp
    unit/test_order_types.cpp
    unit/test_sbe_encoder.cpp
    unit/test_session_manager.cpp
    unit/test_cluster_client.cpp
    unit/test_logging.cpp
    integration/test_end_to_end.cpp
    integration/test_failover.cpp
    integration/test_performance.cpp
)

target_link_libraries(aeron-cluster-tests
    PRIVATE
        aeron-cluster-cpp
        aeron-cluster-test-utils
        GTest::gtest_main
        GTest::gmock
)

# Test discovery
include(GoogleTest)
gtest_discover_tests(aeron-cluster-tests
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    PROPERTIES
        ENVIRONMENT "AERON_DIR=${CMAKE_BINARY_DIR}/test_aeron"
)

# Test data
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/data/test_config.json
    ${CMAKE_CURRENT_BINARY_DIR}/data/test_config.json
    COPYONLY
)

# Performance tests (separate executable)
add_executable(aeron-cluster-perf-tests
    performance/throughput_test.cpp
    performance/latency_test.cpp
    performance/memory_test.cpp
)

target_link_libraries(aeron-cluster-perf-tests
    PRIVATE
        aeron-cluster-cpp
        aeron-cluster-test-utils
        GTest::gtest_main
)

# Benchmark with Google Benchmark if available
find_package(benchmark QUIET)
if(benchmark_FOUND)
    add_executable(aeron-cluster-benchmarks
        benchmarks/encoding_benchmark.cpp
        benchmarks/connection_benchmark.cpp
        benchmarks/throughput_benchmark.cpp
    )
    
    target_link_libraries(aeron-cluster-benchmarks
        PRIVATE
            aeron-cluster-cpp
            benchmark::benchmark
    )
endif()
