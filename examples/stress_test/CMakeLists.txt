add_executable(stress_test_example
    main.cpp
)

target_link_libraries(stress_test_example
    PRIVATE
        campello::net
)

set_target_properties(stress_test_example PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
