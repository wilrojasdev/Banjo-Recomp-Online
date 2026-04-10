#include "net_client.h"
#include <cstdio>
#include <cstring>

namespace bknet {

Client::Client() = default;

Client::~Client() {
    disconnect();
}

bool Client::connect(const std::string& ip, uint16_t port) {
    host_ = enet_host_create(nullptr, 1, NUM_CHANNELS, 0, 0);
    if (!host_) {
        std::fprintf(stderr, "[NetClient] Failed to create ENet host\n");
        return false;
    }

    ENetAddress address;
    enet_address_set_host(&address, ip.c_str());
    address.port = port;

    server_peer_ = enet_host_connect(host_, &address, NUM_CHANNELS, 0);
    if (!server_peer_) {
        std::fprintf(stderr, "[NetClient] Failed to initiate connection to %s:%u\n", ip.c_str(), port);
        enet_host_destroy(host_);
        host_ = nullptr;
        return false;
    }

    std::printf("[NetClient] Connecting to %s:%u...\n", ip.c_str(), port);

    // Wait up to 5 seconds for connection
    ENetEvent event;
    if (enet_host_service(host_, &event, 5000) > 0 && event.type == ENET_EVENT_TYPE_CONNECT) {
        on_connect(event);

        // Set long timeout so connection survives during game loading
        enet_peer_timeout(server_peer_, 0, 15000, 30000);

        // Wait up to 2 more seconds for PlayerAssignment packet
        for (int i = 0; i < 20 && assigned_player_id_ == 0; i++) {
            if (enet_host_service(host_, &event, 100) > 0) {
                if (event.type == ENET_EVENT_TYPE_RECEIVE) {
                    on_receive(event);
                    enet_packet_destroy(event.packet);
                }
            }
        }

        return true;
    }

    std::fprintf(stderr, "[NetClient] Connection to %s:%u timed out\n", ip.c_str(), port);
    enet_peer_reset(server_peer_);
    enet_host_destroy(host_);
    host_ = nullptr;
    server_peer_ = nullptr;
    return false;
}

void Client::disconnect() {
    if (!host_) return;

    if (server_peer_ && connected_) {
        enet_peer_disconnect(server_peer_, 0);

        // Wait for disconnect acknowledgment
        ENetEvent event;
        bool disconnected = false;
        while (enet_host_service(host_, &event, 1000) > 0) {
            if (event.type == ENET_EVENT_TYPE_DISCONNECT) {
                disconnected = true;
                break;
            }
            if (event.type == ENET_EVENT_TYPE_RECEIVE) {
                enet_packet_destroy(event.packet);
            }
        }

        if (!disconnected) {
            enet_peer_reset(server_peer_);
        }
    }

    enet_host_destroy(host_);
    host_ = nullptr;
    server_peer_ = nullptr;
    connected_ = false;
    assigned_player_id_ = 0;
    std::printf("[NetClient] Disconnected\n");
}

void Client::update() {
    if (!host_) return;

    ENetEvent event;
    while (enet_host_service(host_, &event, 0) > 0) {
        switch (event.type) {
            case ENET_EVENT_TYPE_CONNECT:
                on_connect(event);
                break;
            case ENET_EVENT_TYPE_DISCONNECT:
                on_disconnect(event);
                break;
            case ENET_EVENT_TYPE_RECEIVE:
                on_receive(event);
                enet_packet_destroy(event.packet);
                break;
            case ENET_EVENT_TYPE_NONE:
                break;
        }
    }
}

void Client::send(const void* data, size_t size, uint8_t channel, bool reliable) {
    if (!host_ || !server_peer_ || !connected_) return;

    uint32_t flags = reliable ? ENET_PACKET_FLAG_RELIABLE : ENET_PACKET_FLAG_UNSEQUENCED;
    ENetPacket* packet = enet_packet_create(data, size, flags);
    enet_peer_send(server_peer_, channel, packet);
}

void Client::on_connect(ENetEvent& event) {
    connected_ = true;
    std::printf("[NetClient] Connected to server\n");

    if (connect_callback_) {
        connect_callback_();
    }
}

void Client::on_disconnect(ENetEvent& event) {
    connected_ = false;
    assigned_player_id_ = 0;
    server_peer_ = nullptr;
    std::printf("[NetClient] Disconnected from server\n");

    if (disconnect_callback_) {
        disconnect_callback_();
    }
}

void Client::on_receive(ENetEvent& event) {
    if (event.packet->dataLength < sizeof(PacketHeader)) return;

    PacketType type = peek_type(event.packet->data, event.packet->dataLength);

    // Handle assignment packet internally
    if (type == PacketType::PlayerAssignment) {
        PlayerAssignmentPacket assign;
        if (deserialize(event.packet->data, event.packet->dataLength, assign)) {
            assigned_player_id_ = assign.assigned_player_id;
            std::printf("[NetClient] Assigned player ID: %u (total players: %u)\n",
                        assign.assigned_player_id, assign.current_player_count);
        }
        return;
    }

    // Forward all other packets to callback
    if (packet_callback_) {
        auto& hdr = *reinterpret_cast<const PacketHeader*>(event.packet->data);
        packet_callback_(hdr.player_id, event.packet->data, event.packet->dataLength);
    }
}

} // namespace bknet
