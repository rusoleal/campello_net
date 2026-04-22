cmake_minimum_required(VERSION 3.25)

project(
    campello_net
    VERSION 0.1.0
    DESCRIPTION "Multiplayer networking library for the Campello game engine"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── Options ──────────────────────────────────────────────────────────────────
option(CAMPELLO_NET_BUILD_TESTS "Build tests" ON)
option(CAMPELLO_NET_BUILD_EXAMPLES "Build examples" ON)
option(CAMPELLO_NET_INSTALL "Generate install targets" ON)

# ── Library target ───────────────────────────────────────────────────────────
add_library(campello_net
    src/transport/udp_transport.cpp
    src/transport/network_simulator.cpp
    src/transport/loopback_transport.cpp
    src/transport/encrypted_transport.cpp
    src/crypto/chachapoly.cpp
    src/crypto/hmac_sha256.cpp
    src/connection_token.cpp
    src/rate_limiter.cpp
    src/network_log.cpp
    src/network_time.cpp
    src/network_clock.cpp
    src/network_manager.cpp
    src/network_entity.cpp
    src/network_replication.cpp
    src/rpc_manager.cpp
    src/prediction/input_buffer.cpp
    src/prediction/lag_compensator.cpp
    src/discovery/lan_discovery.cpp
    src/replication/spatial_interest_manager.cpp
)
add_library(campello::net ALIAS campello_net)

target_include_directories(
    campello_net
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

# Platform detection & compiler flags
if(WIN32)
    target_compile_definitions(campello_net PUBLIC CAMPELLO_NET_PLATFORM_WIN32)
elseif(ANDROID)
    target_compile_definitions(campello_net PUBLIC CAMPELLO_NET_PLATFORM_ANDROID)
elseif(IOS)
    target_compile_definitions(campello_net PUBLIC CAMPELLO_NET_PLATFORM_IOS)
elseif(APPLE)
    target_compile_definitions(campello_net PUBLIC CAMPELLO_NET_PLATFORM_MACOS)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_compile_definitions(campello_net PUBLIC CAMPELLO_NET_PLATFORM_LINUX)
endif()

if(MSVC)
    target_compile_options(
        campello_net
        INTERFACE
            /W4
            /permissive-
            /Zc:__cplusplus
            /Zc:preprocessor
    )
else()
    target_compile_options(
        campello_net
        INTERFACE
            -Wall
            -Wextra
            -Wpedantic
            -Wconversion
            -Wsign-conversion
            -Wshadow
            -Wnon-virtual-dtor
            -Wold-style-cast
            -Wcast-align
            -Wunused
            -Woverloaded-virtual
            -Wdouble-promotion
            -Wformat=2
            -Wimplicit-fallthrough
    )
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
        target_compile_options(campello_net INTERFACE -Wmisleading-indentation -Wduplicated-cond -Wduplicated-branches -Wlogical-op)
    endif()
endif()

# ── Testing ──────────────────────────────────────────────────────────────────
if(CAMPELLO_NET_BUILD_TESTS)
    include(FetchContent)
    FetchContent_Declare(
        Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG        v3.7.0
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(Catch2)
    enable_testing()
    add_subdirectory(tests)
endif()

# ── Examples ─────────────────────────────────────────────────────────────────
if(CAMPELLO_NET_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# ── Documentation ────────────────────────────────────────────────────────────
find_package(Doxygen)
if(DOXYGEN_FOUND)
    add_custom_target(
        campello_net_docs
        COMMAND ${DOXYGEN_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/Doxyfile
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Generating API documentation with Doxygen"
        VERBATIM
    )
endif()

# ── Install ──────────────────────────────────────────────────────────────────
if(CAMPELLO_NET_INSTALL)
    include(GNUInstallDirs)
    install(
        TARGETS campello_net
        EXPORT campello_net-targets
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )
    install(
        DIRECTORY include/
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )
    install(
        EXPORT campello_net-targets
        FILE campello_net-targets.cmake
        NAMESPACE campello::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/campello_net
    )
endif()
