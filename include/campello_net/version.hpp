#pragma once

#include "campello_net/detail/config.hpp"

#include <string_view>

namespace systems::leal::campello_net {

/// @return Semantic version string (e.g., "0.2.0").
[[nodiscard]] constexpr std::string_view version_string() noexcept {
    return "0.2.0";
}

/// @return Major version number.
[[nodiscard]] constexpr int version_major() noexcept {
    return CAMPELLO_NET_VERSION_MAJOR;
}

/// @return Minor version number.
[[nodiscard]] constexpr int version_minor() noexcept {
    return CAMPELLO_NET_VERSION_MINOR;
}

/// @return Patch version number.
[[nodiscard]] constexpr int version_patch() noexcept {
    return CAMPELLO_NET_VERSION_PATCH;
}

} // namespace systems::leal::campello_net
