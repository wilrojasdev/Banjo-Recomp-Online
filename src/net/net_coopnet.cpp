#include "net_coopnet.h"
#include "libcoopnet.h"
#include <juice/juice.h>
#include <cstdio>
#include <cstring>

namespace bknet {

CoopNetTransport* CoopNetTransport::s_instance_ = nullptr;

CoopNetTransport::CoopNetTransport() {
    player_peers_.fill(INVALID_PEER);
}

CoopNetTransport::~CoopNetTransport() {
    shutdown();
}

bool CoopNetTransport::begin(const std::string& server, uint16_t port, const std::string& player_name) {
    if (s_instance_ != nullptr) {
        std::fprintf(stderr, "[CoopNet] Another instance is already active\n");
        return false;
    }
    s_instance_ = this;

    // Setup callbacks
    std::memset(&gCoopNetCallbacks, 0, sizeof(gCoopNetCallbacks));
    gCoopNetCallbacks.OnConnected = on_connected;
    gCoopNetCallbacks.OnDisconnected = on_disconnected;
    gCoopNetCallbacks.OnLobbyCreated = on_lobby_created;
    gCoopNetCallbacks.OnLobbyJoined = on_lobby_joined;
    gCoopNetCallbacks.OnLobbyLeft = on_lobby_left;
    gCoopNetCallbacks.OnLobbyListGot = on_lobby_list_got;
    gCoopNetCallbacks.OnLobbyListFinish = on_lobby_list_finish;
    gCoopNetCallbacks.OnReceive = on_receive;
    gCoopNetCallbacks.OnError = on_error;
    gCoopNetCallbacks.OnPeerConnected = on_peer_connected;
    gCoopNetCallbacks.OnPeerDisconnected = on_peer_disconnected;

    // Suppress libjuice STUN datagram spam (DEBUG/VERBOSE)
    juice_set_log_level(JUICE_LOG_LEVEL_WARN);

    CoopNetRc rc = coopnet_begin(server.c_str(), port, player_name.c_str(), 0);
    if (rc != COOPNET_OK) {
        std::fprintf(stderr, "[CoopNet] Failed to connect to signaling server %s:%u\n",
                     server.c_str(), port);
        s_instance_ = nullptr;
        return false;
    }

    shutdown_pending_ = false;
    std::printf("[CoopNet] Connecting to signaling server %s:%u...\n", server.c_str(), port);
    return true;
}

void CoopNetTransport::shutdown() {
    if (s_instance_ != this) return;

    if (current_lobby_id_ != 0) {
        coopnet_lobby_leave(current_lobby_id_);
        current_lobby_id_ = 0;
    }

    coopnet_shutdown();
    shutdown_pending_ = true;
    // Process the shutdown
    coopnet_update();

    signaling_connected_ = false;
    is_host_ = false;
    local_user_id_ = 0;
    lobby_owner_id_ = 0;
    player_peers_.fill(INVALID_PEER);
    s_instance_ = nullptr;

    std::printf("[CoopNet] Shut down\n");
}

void CoopNetTransport::update() {
    if (s_instance_ != this) return;
    coopnet_update();
}

bool CoopNetTransport::create_lobby(const std::string& password, const std::string& description, uint16_t max_players) {
    if (!signaling_connected_) return false;

    CoopNetRc rc = coopnet_lobby_create(
        "BK64-Online",     // game
        "1.0",             // version
        "",                // hostName (will use player name from begin)
        "coop",            // mode
        max_players,
        password.c_str(),
        description.c_str()
    );

    if (rc != COOPNET_OK) {
        std::fprintf(stderr, "[CoopNet] Failed to create lobby\n");
        return false;
    }

    is_host_ = true;
    local_player_id_ = 0;
    player_peers_[0] = local_user_id_; // Host is player 0
    std::printf("[CoopNet] Creating lobby...\n");
    return true;
}

bool CoopNetTransport::join_lobby(uint64_t lobby_id, const std::string& password) {
    if (!signaling_connected_) return false;

    CoopNetRc rc = coopnet_lobby_join(lobby_id, password.c_str());
    if (rc != COOPNET_OK) {
        std::fprintf(stderr, "[CoopNet] Failed to join lobby %llu\n",
                     (unsigned long long)lobby_id);
        return false;
    }

    is_host_ = false;
    std::printf("[CoopNet] Joining lobby %llu...\n", (unsigned long long)lobby_id);
    return true;
}

bool CoopNetTransport::leave_lobby() {
    if (current_lobby_id_ == 0) return false;

    CoopNetRc rc = coopnet_lobby_leave(current_lobby_id_);
    current_lobby_id_ = 0;
    is_host_ = false;
    player_peers_.fill(INVALID_PEER);

    std::printf("[CoopNet] Left lobby\n");
    return rc == COOPNET_OK;
}

void CoopNetTransport::request_lobby_list(const std::string& password) {
    if (!signaling_connected_) return;

    lobby_list_buffer_.clear();
    coopnet_lobby_list_get("BK64-Online", password.c_str());
    std::printf("[CoopNet] Requesting lobby list (password='%s')...\n", password.c_str());
}

void CoopNetTransport::broadcast(const void* data, size_t size) {
    if (current_lobby_id_ == 0) return;
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    const uint64_t len = static_cast<uint64_t>(size);
    if (is_host_) {
        // Host already has every peer_id (assigned in on_peer_connected),
        // so we can explicitly unicast to each. This is more reliable than
        // the library broadcast in flaky-NAT scenarios — PlayerAssignment
        // (also unicast) arrives even when state broadcasts don't.
        for (uint8_t i = 0; i < MAX_PLAYERS; i++) {
            uint64_t peer = player_peers_[i];
            if (peer != INVALID_PEER && peer != local_user_id_) {
                coopnet_send_to(peer, bytes, len);
            }
        }
    } else {
        // Non-host joiners may not yet know other joiners' peer_ids — the
        // mapping is learned lazily on first inbound packet (see on_receive).
        // Use library broadcast so first-contact still happens.
        coopnet_send(bytes, len);
    }
}

void CoopNetTransport::send_to(uint8_t player_id, const void* data, size_t size) {
    uint64_t peer_id = player_id_to_peer(player_id);
    if (peer_id == INVALID_PEER) return;
    coopnet_send_to(peer_id, static_cast<const uint8_t*>(data), static_cast<uint64_t>(size));
}

uint8_t CoopNetTransport::peer_count() const {
    uint8_t count = 0;
    for (uint8_t i = 0; i < MAX_PLAYERS; i++) {
        if (player_peers_[i] != INVALID_PEER) count++;
    }
    return count;
}

// === Peer ID mapping ===

uint8_t CoopNetTransport::assign_player_id(uint64_t peer_id) {
    // Idempotent: libcoopnet can fire on_peer_connected multiple times per
    // peer (once per ICE candidate / connectivity check). Return the existing
    // slot if the peer is already assigned, otherwise take the first free one.
    for (uint8_t i = 1; i < MAX_PLAYERS; i++) {
        if (player_peers_[i] == peer_id) return i;
    }
    for (uint8_t i = 1; i < MAX_PLAYERS; i++) {
        if (player_peers_[i] == INVALID_PEER) {
            player_peers_[i] = peer_id;
            return i;
        }
    }
    return 0xFF; // No free slot
}

void CoopNetTransport::release_player_id(uint64_t peer_id) {
    for (uint8_t i = 0; i < MAX_PLAYERS; i++) {
        if (player_peers_[i] == peer_id) {
            player_peers_[i] = INVALID_PEER;
            return;
        }
    }
}

uint8_t CoopNetTransport::peer_to_player_id(uint64_t peer_id) const {
    for (uint8_t i = 0; i < MAX_PLAYERS; i++) {
        if (player_peers_[i] == peer_id) return i;
    }
    return 0xFF;
}

uint64_t CoopNetTransport::player_id_to_peer(uint8_t player_id) const {
    if (player_id >= MAX_PLAYERS) return INVALID_PEER;
    return player_peers_[player_id];
}

// === Static callback trampolines ===

void CoopNetTransport::on_connected(uint64_t user_id) {
    if (!s_instance_) return;
    s_instance_->signaling_connected_ = true;
    s_instance_->local_user_id_ = user_id;
    std::printf("[CoopNet] Connected to signaling server (userId=%llu)\n",
                (unsigned long long)user_id);
}

void CoopNetTransport::on_disconnected(bool intentional) {
    if (!s_instance_) return;
    s_instance_->signaling_connected_ = false;
    std::printf("[CoopNet] Disconnected from signaling server (intentional=%d)\n", intentional);

    if (!intentional && s_instance_->signaling_disconnect_callback_) {
        s_instance_->signaling_disconnect_callback_();
    }
}

void CoopNetTransport::on_lobby_created(uint64_t lobby_id, const char* game, const char* version,
                                         const char* host_name, const char* mode, uint16_t max_connections) {
    if (!s_instance_) return;
    s_instance_->current_lobby_id_ = lobby_id;
    s_instance_->lobby_owner_id_ = s_instance_->local_user_id_;
    std::printf("[CoopNet] Lobby created: id=%llu game=%s\n",
                (unsigned long long)lobby_id, game);

    if (s_instance_->lobby_created_callback_) {
        s_instance_->lobby_created_callback_(lobby_id);
    }
}

void CoopNetTransport::on_lobby_joined(uint64_t lobby_id, uint64_t user_id, uint64_t owner_id, uint64_t dest_id) {
    if (!s_instance_) return;

    s_instance_->current_lobby_id_ = lobby_id;
    s_instance_->lobby_owner_id_ = owner_id;

    if (user_id == s_instance_->local_user_id_) {
        // We just joined the lobby
        s_instance_->is_host_ = (owner_id == s_instance_->local_user_id_);
        if (!s_instance_->is_host_) {
            // We're a joiner — host will assign our player_id via PlayerAssignment packet
            // after P2P connection is established
            std::printf("[CoopNet] Joined lobby %llu (owner=%llu), waiting for P2P...\n",
                        (unsigned long long)lobby_id, (unsigned long long)owner_id);
        }
    } else {
        // Another user joined the lobby — P2P will be established automatically
        std::printf("[CoopNet] User %llu joined lobby %llu\n",
                    (unsigned long long)user_id, (unsigned long long)lobby_id);
    }
}

void CoopNetTransport::on_lobby_left(uint64_t lobby_id, uint64_t user_id) {
    if (!s_instance_) return;

    if (user_id == s_instance_->local_user_id_) {
        // We were removed from the lobby (host left, or we left voluntarily)
        // Notify disconnect for all peers that were connected
        for (uint8_t i = 0; i < MAX_PLAYERS; i++) {
            if (s_instance_->player_peers_[i] != INVALID_PEER && i != s_instance_->local_player_id_) {
                std::printf("[CoopNet] Peer player %u disconnected (lobby closed)\n", i);
                if (s_instance_->disconnect_callback_) {
                    s_instance_->disconnect_callback_(i);
                }
            }
        }

        s_instance_->current_lobby_id_ = 0;
        s_instance_->is_host_ = false;
        s_instance_->player_peers_.fill(INVALID_PEER);
        std::printf("[CoopNet] Left lobby %llu\n", (unsigned long long)lobby_id);

        // If we didn't leave voluntarily, signal unexpected disconnect
        if (s_instance_->signaling_disconnect_callback_) {
            s_instance_->signaling_disconnect_callback_();
        }
    } else {
        // Another peer left — on_peer_disconnected may NOT fire (forced disconnect),
        // so handle it here
        uint8_t player_id = s_instance_->peer_to_player_id(user_id);
        if (player_id != 0xFF) {
            std::printf("[CoopNet] Player %u left lobby\n", player_id);
            s_instance_->release_player_id(user_id);
            if (s_instance_->disconnect_callback_) {
                s_instance_->disconnect_callback_(player_id);
            }
        } else {
            std::printf("[CoopNet] User %llu left lobby %llu\n",
                        (unsigned long long)user_id, (unsigned long long)lobby_id);
        }
    }
}

void CoopNetTransport::on_lobby_list_got(uint64_t lobby_id, uint64_t owner_id, uint16_t connections,
                                          uint16_t max_connections, const char* game, const char* version,
                                          const char* host_name, const char* mode, const char* description) {
    if (!s_instance_) return;

    LobbyInfo info{};
    info.lobby_id = lobby_id;
    info.owner_id = owner_id;
    info.host_name = host_name ? host_name : "";
    info.description = description ? description : "";
    info.mode = mode ? mode : "";
    info.player_count = connections;
    info.max_players = max_connections;

    s_instance_->lobby_list_buffer_.push_back(info);
}

void CoopNetTransport::on_lobby_list_finish() {
    if (!s_instance_) return;

    std::printf("[CoopNet] Lobby list complete: %zu lobbies\n",
                s_instance_->lobby_list_buffer_.size());

    if (s_instance_->lobby_list_callback_) {
        s_instance_->lobby_list_callback_(s_instance_->lobby_list_buffer_);
    }
}

void CoopNetTransport::on_receive(uint64_t from_user_id, const uint8_t* data, uint64_t size) {
    if (!s_instance_ || size == 0) return;

    uint8_t player_id = s_instance_->peer_to_player_id(from_user_id);

    // Special case: if we're not yet assigned and this is a PlayerAssignment packet
    if (!s_instance_->is_host_ && size >= sizeof(PacketHeader)) {
        PacketType type = static_cast<PacketType>(data[0]);
        if (type == PacketType::PlayerAssignment && size >= sizeof(PlayerAssignmentPacket)) {
            PlayerAssignmentPacket pkt;
            std::memcpy(&pkt, data, sizeof(pkt));
            s_instance_->local_player_id_ = pkt.assigned_player_id;

            // Map the host peer (the sender, who is player 0)
            // We know the host's peer_id from lobby_owner_id_
            s_instance_->player_peers_[0] = from_user_id;
            s_instance_->player_peers_[pkt.assigned_player_id] = s_instance_->local_user_id_;

            std::printf("[CoopNet] Assigned player_id=%u by host\n", pkt.assigned_player_id);
            return; // Don't forward assignment packet to game logic
        }
    }

    if (player_id == 0xFF) {
        // P2P mesh: non-host peers connect directly but only the host assigns
        // player_ids. Learn the mapping from the packet's own header — every
        // packet (PlayerPosition, PlayerState, PlayerJoin, ...) carries the
        // sender's player_id. Without this, data from non-host peers is
        // silently dropped and only host+self render.
        if (size < sizeof(PacketHeader)) return;
        PacketHeader hdr;
        std::memcpy(&hdr, data, sizeof(hdr));
        if (hdr.player_id == 0 || hdr.player_id >= MAX_PLAYERS ||
            s_instance_->player_peers_[hdr.player_id] != INVALID_PEER) {
            std::fprintf(stderr, "[CoopNet] Received data from unknown peer %llu\n",
                         (unsigned long long)from_user_id);
            return;
        }
        s_instance_->player_peers_[hdr.player_id] = from_user_id;
        player_id = hdr.player_id;
        std::printf("[CoopNet] Learned peer mapping: %llu -> player %u\n",
                    (unsigned long long)from_user_id, hdr.player_id);
    }

    if (s_instance_->packet_callback_) {
        s_instance_->packet_callback_(player_id, data, static_cast<size_t>(size));
    }

    // If we're the host, relay to other peers (emulate server relay behavior)
    if (s_instance_->is_host_) {
        for (uint8_t i = 1; i < MAX_PLAYERS; i++) {
            uint64_t peer = s_instance_->player_peers_[i];
            if (peer != INVALID_PEER && peer != from_user_id) {
                coopnet_send_to(peer, data, size);
            }
        }
    }
}

void CoopNetTransport::on_error(enum MPacketErrorNumber error_number, uint64_t tag) {
    if (!s_instance_) return;
    std::fprintf(stderr, "[CoopNet] Error: %d (tag=%llu)\n",
                 static_cast<int>(error_number), (unsigned long long)tag);

    if (s_instance_->error_callback_) {
        s_instance_->error_callback_(static_cast<int>(error_number));
    }
}

void CoopNetTransport::on_peer_connected(uint64_t peer_id) {
    if (!s_instance_) return;

    std::printf("[CoopNet] P2P peer connected: %llu\n", (unsigned long long)peer_id);

    if (s_instance_->is_host_) {
        // Skip if this peer is already assigned — libcoopnet fires this event
        // multiple times per peer and we don't want to resend assignment/join
        // packets or trigger connect_callback more than once.
        if (s_instance_->peer_to_player_id(peer_id) != 0xFF) {
            return;
        }
        // Host assigns a player_id to this peer
        uint8_t assigned_id = s_instance_->assign_player_id(peer_id);
        if (assigned_id == 0xFF) {
            std::fprintf(stderr, "[CoopNet] No free player slot for peer %llu\n",
                         (unsigned long long)peer_id);
            coopnet_unpeer(peer_id);
            return;
        }

        // Send PlayerAssignment to the new peer
        PlayerAssignmentPacket pkt{};
        pkt.header.type = PacketType::PlayerAssignment;
        pkt.header.player_id = 0; // From host
        pkt.header.sequence = 0;
        pkt.assigned_player_id = assigned_id;

        coopnet_send_to(peer_id, reinterpret_cast<const uint8_t*>(&pkt), sizeof(pkt));

        std::printf("[CoopNet] Assigned player_id=%u to peer %llu\n",
                    assigned_id, (unsigned long long)peer_id);

        // Notify host's game logic
        if (s_instance_->connect_callback_) {
            s_instance_->connect_callback_(assigned_id);
        }

        // Broadcast PlayerJoin to other peers
        PlayerJoinPacket join_pkt{};
        join_pkt.header.type = PacketType::PlayerJoin;
        join_pkt.header.player_id = assigned_id;
        join_pkt.header.sequence = 0;

        for (uint8_t i = 1; i < MAX_PLAYERS; i++) {
            uint64_t other_peer = s_instance_->player_peers_[i];
            if (other_peer != INVALID_PEER && other_peer != peer_id) {
                coopnet_send_to(other_peer,
                    reinterpret_cast<const uint8_t*>(&join_pkt), sizeof(join_pkt));
            }
        }
    } else {
        // Non-host client. The peer could be host, or another joiner in a
        // 3+ player lobby — libcoopnet establishes a full P2P mesh.
        // We learn the peer_id->player_id mapping via PlayerAssignment (host)
        // or from the first data packet's header (other joiners, see on_receive).
        if (peer_id == s_instance_->lobby_owner_id_) {
            std::printf("[CoopNet] P2P connection to host established\n");
        } else {
            std::printf("[CoopNet] P2P connection to peer %llu established\n",
                        (unsigned long long)peer_id);
        }
    }
}

void CoopNetTransport::on_peer_disconnected(uint64_t peer_id) {
    if (!s_instance_) return;

    uint8_t player_id = s_instance_->peer_to_player_id(peer_id);
    if (player_id == 0xFF) return;

    std::printf("[CoopNet] P2P peer disconnected: %llu (player_id=%u)\n",
                (unsigned long long)peer_id, player_id);

    s_instance_->release_player_id(peer_id);

    if (s_instance_->disconnect_callback_) {
        s_instance_->disconnect_callback_(player_id);
    }

    // If we're host, broadcast PlayerLeave
    if (s_instance_->is_host_) {
        PlayerLeavePacket pkt{};
        pkt.header.type = PacketType::PlayerLeave;
        pkt.header.player_id = player_id;
        pkt.header.sequence = 0;

        for (uint8_t i = 1; i < MAX_PLAYERS; i++) {
            uint64_t other = s_instance_->player_peers_[i];
            if (other != INVALID_PEER) {
                coopnet_send_to(other,
                    reinterpret_cast<const uint8_t*>(&pkt), sizeof(pkt));
            }
        }
    }

    // If the host disconnected, we lost connection
    if (!s_instance_->is_host_ && player_id == 0) {
        if (s_instance_->signaling_disconnect_callback_) {
            s_instance_->signaling_disconnect_callback_();
        }
    }
}

} // namespace bknet
