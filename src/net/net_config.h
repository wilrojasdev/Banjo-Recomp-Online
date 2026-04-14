#ifndef NET_CONFIG_H
#define NET_CONFIG_H

#include <cstdint>
#include <string>

namespace bknet {

enum class NetworkMode : int {
    Off = 0,
    Host = 1,        // ENet LAN host
    Join = 2,        // ENet LAN join
    CoopNetHost = 3, // CoopNet WAN lobby host
    CoopNetJoin = 4, // CoopNet WAN lobby join
};

struct NetworkConfig {
    NetworkMode mode = NetworkMode::Off;
    uint16_t port = 7777;
    std::string join_ip = "127.0.0.1";
    std::string player_name = "Player";
    int save_slot = 0; // 0-2, selected in Host submenu

    // CoopNet settings
    std::string coopnet_server = "localhost";
    uint16_t coopnet_port = 34197;
    uint64_t coopnet_lobby_id = 0;
    std::string lobby_password = "";
    std::string lobby_description = "";
};

// Global config accessors
NetworkConfig& get_config();
void set_mode(NetworkMode mode);
void set_port(uint16_t port);
void set_join_ip(const std::string& ip);
void set_player_name(const std::string& name);
void set_coopnet_server(const std::string& server);
void set_coopnet_port(uint16_t port);

} // namespace bknet

#endif // NET_CONFIG_H
