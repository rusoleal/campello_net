add_subdirectory(echo)
add_subdirectory(stress_test)
add_subdirectory(chat)
add_subdirectory(cubes)
add_subdirectory(pong)

if(CMAKE_SYSTEM_NAME STREQUAL "Emscripten")
    add_subdirectory(wasm_client)
endif()
