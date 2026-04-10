#include "net_server.h"
#include <cstdio>
#include <cstring>

namespace bknet {

Server::Server() {
    clients_.fill(nullptr);
}

Server::~Server() {
    stop();
}

bool Server::start(uint16_t port) {
    ENetAddress address;
    address.host = ENET_HOST_ANY;
    address.port = port;

    host_ = enet_host_create(&address, MAX_PLAYERS - 1, NUM_CHANNELS, 0, 0);
    if (!host_) {
        std::fprintf(stderr, "[NetServer] Failed to create ENet host on port %u\n", port);
        return false;
    }

    std::printf("[NetServer] Listening on port %u\n", port);
    return true;
}

void Server::stop() {
    if (!host_) return;

    // Disconnect all clients gracefully
    for (auto& client : clients_) {
        if (client) {
            enet_peer_disconnect(client, 0);
            client = nullptr;
        }
    }

    // Flush pending disconnects
    ENetEvent event;
    while (enet_host_service(host_, &event, 500) > 0) {
        if (event.type == ENET_EVENT_TYPE_RECEIVE) {
            enet_packet_destroy(event.packet);
        }
    }

    enet_host_destroy(host_);
    host_ = nullptr;
    std::printf("[NetServer] Stopped\n");
}

void Server::update() {
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

void Server::broadcast(const void* data, size_t size, uint8_t channel, bool reliable) {
    if (!host_) return;

    uint32_t flags = reliable ? ENET_PACKET_FLAG_RELIABLE : ENET_PACKET_FLAG_UNSEQUENCED;
    ENetPacket* packet = enet_packet_create(data, size, flags);
    enet_host_broadcast(host_, channel, packet);
}

void Server::send_to(uint8_t player_id, const void* data, size_t size, uint8_t channel, bool reliable) {
    if (!host_ || player_id == 0 || player_id >= MAX_PLAYERS) return;

    ENetPeer* peer = clients_[player_id - 1];
    if (!peer) return;

    uint32_t flags = reliable ? ENET_PACKET_FLAG_RELIABLE : ENET_PACKET_FLAG_UNSEQUENCED;
    ENetPacket* packet = enet_packet_create(data, size, flags);
    enet_peer_send(peer, channel, packet);
}

uint8_t Server::client_count() const {
    uint8_t count = 0;
    for (const auto& client : clients_) {
        if (client) count++;
    }
    return count;
}

void Server::on_connect(ENetEvent& event) {
    uint8_t slot = find_free_slot();
    if (slot == 0) {
        // No free slots
        enet_peer_disconnect(event.peer, 0);
        std::printf("[NetServer] Rejected connection: server full\n");
        return;
    }

    clients_[slot - 1] = event.peer;
    event.peer->data = reinterpret_cast<void*>(static_cast<uintptr_t>(slot));

    // Long timeout so connection survives during game loading
    enet_peer_timeout(event.peer, 0, 15000, 30000);

    // Send player assignment
    PlayerAssignmentPacket assign{};
    assign.header.type = PacketType::PlayerAssignment;
    assign.header.player_id = 0; // from server
    assign.header.sequence = next_sequence_++;
    assign.assigned_player_id = slot;
    assign.current_player_count = static_cast<uint8_t>(client_count() + 1); // +1 for host

    send_to(slot, &assign, sizeof(assign), CHANNEL_RELIABLE, true);

    // Notify other clients about the new player
    PlayerJoinPacket join{};
    join.header.type = PacketType::PlayerJoin;
    join.header.player_id = slot;
    join.header.sequence = next_sequence_++;

    for (uint8_t i = 0; i < MAX_PLAYERS - 1; i++) {
        if (clients_[i] && clients_[i] != event.peer) {
            send_to(i + 1, &join, sizeof(join), CHANNEL_RELIABLE, true);
        }
    }

    std::printf("[NetServer] Player %u connected\n", slot);

    if (connect_callback_) {
        connect_callback_(slot);
    }
}

void Server::on_disconnect(ENetEvent& event) {
    uint8_t player_id = peer_to_player_id(event.peer);
    if (player_id == 0) return;

    clients_[player_id - 1] = nullptr;

    // Notify remaining clients
    PlayerLeavePacket leave{};
    leave.header.type = PacketType::PlayerLeave;
    leave.header.player_id = player_id;
    leave.header.sequence = next_sequence_++;
    leave.reason = 0; // disconnect

    broadcast(&leave, sizeof(leave), CHANNEL_RELIABLE, true);

    std::printf("[NetServer] Player %u disconnected\n", player_id);

    if (disconnect_callback_) {
        disconnect_callback_(player_id);
    }
}

void Server::on_receive(ENetEvent& event) {
    uint8_t player_id = peer_to_player_id(event.peer);
    if (player_id == 0) return;

    // Forward packet to all other clients (relay server model)
    for (uint8_t i = 0; i < MAX_PLAYERS - 1; i++) {
        if (clients_[i] && clients_[i] != event.peer) {
            bool reliable = (event.channelID == CHANNEL_RELIABLE);
            send_to(i + 1, event.packet->data, event.packet->dataLength,
                    event.channelID, reliable);
        }
    }

    // Also deliver to local host via callback
    if (packet_callback_) {
        packet_callback_(player_id, event.packet->data, event.packet->dataLength);
    }
}

uint8_t Server::find_free_slot() const {
    for (uint8_t i = 0; i < MAX_PLAYERS - 1; i++) {
        if (!clients_[i]) return i + 1;
    }
    return 0; // no free slot
}

uint8_t Server::peer_to_player_id(ENetPeer* peer) const {
    return static_cast<uint8_t>(reinterpret_cast<uintptr_t>(peer->data));
}

} // namespace bknet
