# Examples

# Basic client example
add_executable(basic_client_example
    basic_client_example.cpp
)

target_link_libraries(basic_client_example
    PRIVATE
        aeron-cluster-cpp
)

if(ENABLE_WARNINGS)
    set_target_warnings(basic_client_example)
endif()

# Order publishing example
add_executable(order_publishing_example
    order_publishing_example.cpp
)

target_link_libraries(order_publishing_example
    PRIVATE
        aeron-cluster-cpp
)

if(ENABLE_WARNINGS)
    set_target_warnings(order_publishing_example)
endif()

# Advanced features example
add_executable(advanced_features_example
    advanced_features_example.cpp
)

target_link_libraries(advanced_features_example
    PRIVATE
        aeron-cluster-cpp
)

if(ENABLE_WARNINGS)
    set_target_warnings(advanced_features_example)
endif()

# Install examples
install(TARGETS 
    basic_client_example
    order_publishing_example
    advanced_features_example
    DESTINATION ${CMAKE_INSTALL_BINDIR}/examples
)
