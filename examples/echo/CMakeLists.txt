add_executable(echo_example
    main.cpp
)

target_link_libraries(echo_example
    PRIVATE
        campello::net
)

set_target_properties(echo_example PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
