#pragma once

#include <cstdint>
#include <string_view>

namespace sunrise::core::settings {

enum class Role : std::uint8_t { embedded, client, host, dedicated, invalid };

[[nodiscard]] bool parse_role(std::string_view text, Role& output) noexcept;
[[nodiscard]] Role role() noexcept;
/** Settings select solo, joining client, playing host, or headless dedicated before activation. */
[[nodiscard]] bool configure_role(Role value, bool specified) noexcept;
[[nodiscard]] constexpr bool activates_client_hooks(Role value) noexcept {
    return value == Role::embedded || value == Role::client || value == Role::host;
}
[[nodiscard]] constexpr bool binds_server_ports(Role value) noexcept {
    return value == Role::embedded || value == Role::host || value == Role::dedicated;
}

/** Shared session services can run in the playing host's DLL or a headless dedicated EXE. */
[[nodiscard]] inline bool hosts_session() noexcept {
    return role() == Role::host || role() == Role::dedicated;
}

} // namespace sunrise::core::settings
