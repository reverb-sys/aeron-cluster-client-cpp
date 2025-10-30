# Examples

# Basic client example
add_executable(basic_client_example
    basic_client_example.cpp
)

target_link_libraries(basic_client_example
    PRIVATE
        aeron-cluster-cpp
)

#if(ENABLE_WARNINGS)
#    set_target_warnings(basic_client_example)
#endif()

# Order publishing example
add_executable(order_publishing_example
    order_publishing_example.cpp
)

target_link_libraries(order_publishing_example
    PRIVATE
        aeron-cluster-cpp
)

#if(ENABLE_WARNINGS)
#    set_target_warnings(order_publishing_example)
#endif()

# Advanced features example
add_executable(advanced_features_example
    advanced_features_example.cpp
)

target_link_libraries(advanced_features_example
    PRIVATE
        aeron-cluster-cpp
)

#if(ENABLE_WARNINGS)
#    set_target_warnings(advanced_features_example)
#endif()

# Commit/Resume example
add_executable(commit_resume_example
    commit_resume_example.cpp
)

target_link_libraries(commit_resume_example
    PRIVATE
        aeron-cluster-cpp
)

#if(ENABLE_WARNINGS)
#    set_target_warnings(commit_resume_example)
#endif()

#if(ENABLE_WARNINGS)
#    set_target_warnings(production_example)
#endif()

# Pub/Sub Reconnect Test
add_executable(pubsub_reconnect_test
    pubsub_reconnect_test.cpp
)

target_link_libraries(pubsub_reconnect_test
    PRIVATE
        aeron-cluster-cpp
)

#if(ENABLE_WARNINGS)
#    set_target_warnings(pubsub_reconnect_test)
#endif()

# Automatic Signal Handling Example
add_executable(automatic_signal_handling_example
    automatic_signal_handling_example.cpp
)

target_link_libraries(automatic_signal_handling_example
    PRIVATE
        aeron-cluster-cpp
)

#if(ENABLE_WARNINGS)
#    set_target_warnings(automatic_signal_handling_example)
#endif()

# Install examples
install(TARGETS 
    basic_client_example
    order_publishing_example
    advanced_features_example
    commit_resume_example
    pubsub_reconnect_test
    automatic_signal_handling_example
    DESTINATION ${CMAKE_INSTALL_BINDIR}/examples
)
