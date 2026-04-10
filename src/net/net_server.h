#ifndef NET_SERVER_H
#define NET_SERVER_H

#include <enet/enet.h>
#include <array>
#include <functional>
#include <cstdint>

#include "net_packets.h"

namespace bknet {

class Server {
public:
    using PacketCallback = std::function<void(uint8_t player_id, const uint8_t* data, size_t size)>;

    Server();
    ~Server();

    bool start(uint16_t port);
    void stop();
    void update();

    void broadcast(const void* data, size_t size, uint8_t channel, bool reliable);
    void send_to(uint8_t player_id, const void* data, size_t size, uint8_t channel, bool reliable);

    bool is_running() const { return host_ != nullptr; }
    uint8_t client_count() const;

    void set_packet_callback(PacketCallback cb) { packet_callback_ = std::move(cb); }

    using ConnectionCallback = std::function<void(uint8_t player_id)>;
    void set_connect_callback(ConnectionCallback cb) { connect_callback_ = std::move(cb); }
    void set_disconnect_callback(ConnectionCallback cb) { disconnect_callback_ = std::move(cb); }

private:
    void on_connect(ENetEvent& event);
    void on_disconnect(ENetEvent& event);
    void on_receive(ENetEvent& event);

    uint8_t find_free_slot() const;
    uint8_t peer_to_player_id(ENetPeer* peer) const;

    ENetHost* host_ = nullptr;
    std::array<ENetPeer*, MAX_PLAYERS - 1> clients_{}; // slots 1-3 (0 is host)

    PacketCallback packet_callback_;
    ConnectionCallback connect_callback_;
    ConnectionCallback disconnect_callback_;

    uint16_t next_sequence_ = 0;
};

} // namespace bknet

#endif // NET_SERVER_H
