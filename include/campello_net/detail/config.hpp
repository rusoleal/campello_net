#pragma once

#include <cstddef>
#include <cstdint>

// ── Platform detection (fallback if CMake did not define) ──────────────────
#if !defined(CAMPELLO_NET_PLATFORM_WIN32) && !defined(CAMPELLO_NET_PLATFORM_LINUX) &&                                  \
    !defined(CAMPELLO_NET_PLATFORM_MACOS) && !defined(CAMPELLO_NET_PLATFORM_IOS) &&                                    \
    !defined(CAMPELLO_NET_PLATFORM_ANDROID)
#if defined(_WIN32)
#define CAMPELLO_NET_PLATFORM_WIN32
#elif defined(__ANDROID__)
#define CAMPELLO_NET_PLATFORM_ANDROID
#elif defined(__APPLE__)
#include <TargetConditionals.h>
#if TARGET_OS_IPHONE
#define CAMPELLO_NET_PLATFORM_IOS
#else
#define CAMPELLO_NET_PLATFORM_MACOS
#endif
#elif defined(__linux__)
#define CAMPELLO_NET_PLATFORM_LINUX
#endif
#endif

// ── Export macros ────────────────────────────────────────────────────────────
#ifdef CAMPELLO_NET_PLATFORM_WIN32
#ifdef CAMPELLO_NET_BUILD_SHARED
#define CAMPELLO_NET_API __declspec(dllexport)
#else
#define CAMPELLO_NET_API
#endif
#else
#define CAMPELLO_NET_API
#endif

// ── Version ──────────────────────────────────────────────────────────────────
#define CAMPELLO_NET_VERSION_MAJOR 0
#define CAMPELLO_NET_VERSION_MINOR 1
#define CAMPELLO_NET_VERSION_PATCH 0
