#include "net_config.h"

namespace bknet {

static NetworkConfig s_config{};

NetworkConfig& get_config() {
    return s_config;
}

void set_mode(NetworkMode mode) {
    s_config.mode = mode;
}

void set_port(uint16_t port) {
    if (port >= 1024 && port <= 65535) {
        s_config.port = port;
    }
}

void set_join_ip(const std::string& ip) {
    s_config.join_ip = ip;
}

void set_player_name(const std::string& name) {
    if (!name.empty() && name.size() <= 31) {
        s_config.player_name = name;
    }
}

void set_coopnet_server(const std::string& server) {
    if (!server.empty()) {
        s_config.coopnet_server = server;
    }
}

void set_coopnet_port(uint16_t port) {
    if (port >= 1024 && port <= 65535) {
        s_config.coopnet_port = port;
    }
}

} // namespace bknet
