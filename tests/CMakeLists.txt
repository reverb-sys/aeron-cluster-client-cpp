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
