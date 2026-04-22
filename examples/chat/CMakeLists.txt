add_executable(chat_example
    main.cpp
)

target_link_libraries(chat_example
    PRIVATE
        campello::net
)

set_target_properties(chat_example PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
