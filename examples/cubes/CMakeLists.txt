add_executable(cubes_example
    main.cpp
)

target_link_libraries(cubes_example
    PRIVATE
        campello::net
)

set_target_properties(cubes_example PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
