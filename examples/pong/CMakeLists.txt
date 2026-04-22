add_executable(pong_example
    main.cpp
)

target_link_libraries(pong_example
    PRIVATE
        campello::net
)

set_target_properties(pong_example PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
