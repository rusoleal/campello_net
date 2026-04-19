#include "campello_net/network_log.hpp"

namespace systems::leal::campello_net {

namespace {

LogCallback g_log_callback;

} // anonymous namespace

void set_log_callback(LogCallback cb) {
    g_log_callback = std::move(cb);
}

void network_log(LogLevel level, const std::string& message) {
    if (g_log_callback) {
        g_log_callback(level, message);
    }
}

} // namespace systems::leal::campello_net
