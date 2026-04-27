add_executable(wasm_client_example
    main.cpp
)

target_link_libraries(wasm_client_example
    PRIVATE
        campello::net
)

# Emscripten-specific linker flags
if(CMAKE_SYSTEM_NAME STREQUAL "Emscripten")
    target_link_options(wasm_client_example PRIVATE
        "-sEXPORTED_FUNCTIONS=['_connect_to_server','_send_message','_disconnect','_is_connected','_main']"
        "-sEXPORTED_RUNTIME_METHODS=['ccall','cwrap','UTF8ToString']"
        "-sALLOW_MEMORY_GROWTH=1"
        "-sMODULARIZE=1"
        "-sEXPORT_NAME=CampelloNetModule"
        "-sENVIRONMENT=web"
    )
endif()

set_target_properties(wasm_client_example PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin/wasm_client
)

# Copy index.html into the build output so it can be served alongside the JS/WASM
add_custom_command(
    TARGET wasm_client_example POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_CURRENT_SOURCE_DIR}/index.html
        ${CMAKE_BINARY_DIR}/bin/wasm_client/index.html
)
