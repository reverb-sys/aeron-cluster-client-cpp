# Utility Tools

# Message inspector tool
add_executable(message_inspector
    message_inspector.cpp
)

target_link_libraries(message_inspector
    PRIVATE
        aeron-cluster-cpp
)

#if(ENABLE_WARNINGS)
#    set_target_warnings(message_inspector)
#endif()

# Cluster monitor tool
add_executable(cluster_monitor
    cluster_monitor.cpp
)

target_link_libraries(cluster_monitor
    PRIVATE
        aeron-cluster-cpp
)

#if(ENABLE_WARNINGS)
#    set_target_warnings(cluster_monitor)
#endif()

# Install tools
install(TARGETS 
    message_inspector
    cluster_monitor
    DESTINATION ${CMAKE_INSTALL_BINDIR}
)
