#pragma once

#include <functional>
#include <string>

namespace systems::leal::campello_net {

/// Compile-time configurable log level for campello_net internals.
///
/// Set `CAMPELLO_NET_MIN_LOG_LEVEL` at compile time to strip noisy logs:
///   - 0 = Verbose (default)
///   - 1 = Info
///   - 2 = Warning
///   - 3 = Error
///
/// Usage:
///   CAMPELLO_NET_LOGI("Client connected: " + std::to_string(id));
enum class LogLevel { Verbose = 0, Info = 1, Warning = 2, Error = 3 };

using LogCallback = std::function<void(LogLevel, const std::string&)>;

/// Install a callback to receive all log messages at or above the compile-time
/// minimum level. Pass nullptr to disable logging.
void set_log_callback(LogCallback cb);

/// Write a log message if @p level is >= the compile-time minimum.
void network_log(LogLevel level, const std::string& message);

namespace detail {

inline void log_if(LogLevel level, const std::string& message) {
    network_log(level, message);
}

} // namespace detail

} // namespace systems::leal::campello_net

// ── Compile-time filtering macros ───────────────────────────────────────────

#if !defined(CAMPELLO_NET_MIN_LOG_LEVEL)
#define CAMPELLO_NET_MIN_LOG_LEVEL 0
#endif

#define CAMPELLO_NET_LOG(level, msg)                                                                                   \
    do {                                                                                                               \
        if (static_cast<int>(level) >= CAMPELLO_NET_MIN_LOG_LEVEL) {                                                   \
            ::systems::leal::campello_net::detail::log_if(level, msg);                                                 \
        }                                                                                                              \
    } while (0)

#define CAMPELLO_NET_LOGV(msg) CAMPELLO_NET_LOG(::systems::leal::campello_net::LogLevel::Verbose, msg)
#define CAMPELLO_NET_LOGI(msg) CAMPELLO_NET_LOG(::systems::leal::campello_net::LogLevel::Info, msg)
#define CAMPELLO_NET_LOGW(msg) CAMPELLO_NET_LOG(::systems::leal::campello_net::LogLevel::Warning, msg)
#define CAMPELLO_NET_LOGE(msg) CAMPELLO_NET_LOG(::systems::leal::campello_net::LogLevel::Error, msg)
