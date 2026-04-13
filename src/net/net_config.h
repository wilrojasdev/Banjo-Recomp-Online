#ifndef NET_CONFIG_H
#define NET_CONFIG_H

#include <cstdint>
#include <string>

namespace bknet {

enum class NetworkMode : int {
    Off = 0,
    Host = 1,
    Join = 2,
};

struct NetworkConfig {
    NetworkMode mode = NetworkMode::Off;
    uint16_t port = 7777;
    std::string join_ip = "127.0.0.1";
    std::string player_name = "Player";
    int save_slot = 0; // 0-2, selected in Host submenu
};

// Global config accessors
NetworkConfig& get_config();
void set_mode(NetworkMode mode);
void set_port(uint16_t port);
void set_join_ip(const std::string& ip);
void set_player_name(const std::string& name);

} // namespace bknet

#endif // NET_CONFIG_H
