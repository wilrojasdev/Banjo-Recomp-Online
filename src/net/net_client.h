#ifndef NET_CLIENT_H
#define NET_CLIENT_H

#include <enet/enet.h>
#include <functional>
#include <string>
#include <cstdint>

#include "net_packets.h"

namespace bknet {

class Client {
public:
    using PacketCallback = std::function<void(uint8_t player_id, const uint8_t* data, size_t size)>;
    using StateCallback = std::function<void()>;

    Client();
    ~Client();

    bool connect(const std::string& ip, uint16_t port);
    void disconnect();
    void update();

    void send(const void* data, size_t size, uint8_t channel, bool reliable);

    bool is_connected() const { return connected_; }
    uint8_t assigned_player_id() const { return assigned_player_id_; }

    void set_packet_callback(PacketCallback cb) { packet_callback_ = std::move(cb); }
    void set_connect_callback(StateCallback cb) { connect_callback_ = std::move(cb); }
    void set_disconnect_callback(StateCallback cb) { disconnect_callback_ = std::move(cb); }

private:
    void on_connect(ENetEvent& event);
    void on_disconnect(ENetEvent& event);
    void on_receive(ENetEvent& event);

    ENetHost* host_ = nullptr;
    ENetPeer* server_peer_ = nullptr;
    bool connected_ = false;
    uint8_t assigned_player_id_ = 0;

    PacketCallback packet_callback_;
    StateCallback connect_callback_;
    StateCallback disconnect_callback_;

    uint16_t next_sequence_ = 0;
};

} // namespace bknet

#endif // NET_CLIENT_H
