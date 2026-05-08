#include "net_manager.h"
#include "net_playerlist_ui.h"
#include "../locale/locale.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <thread>
#include <algorithm>
/* miniz provides zlib-compatible names (compress2/uncompress/compressBound)
 * by default. Vendored in N64ModernRuntime/thirdparty/miniz and linked
 * PUBLIC by librecomp, so available everywhere — including Windows CI
 * which has no system zlib in its toolchain include path. */
#include <miniz.h>

namespace bknet {

// Global log level storage (declared extern in net_log.h). Default = Warn.
// Override at startup via env: `BKNET_LOG_LEVEL=3 <binary>` for Debug.
std::atomic<int> g_log_level{static_cast<int>(LogLevel::Warn)};

namespace {
    struct LogLevelInit {
        LogLevelInit() {
            if (const char* e = std::getenv("BKNET_LOG_LEVEL")) {
                int v = std::atoi(e);
                if (v >= 0 && v <= 3) g_log_level.store(v);
            }
        }
    };
    static LogLevelInit s_log_init;
}

NetworkManager& NetworkManager::instance() {
    static NetworkManager s_instance;
    return s_instance;
}

NetworkStats NetworkManager::get_stats() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    NetworkStats out = stats_;
    if (coopnet_) {
        for (uint8_t i = 0; i < MAX_PLAYERS; i++) {
            out.rtt_ms_by_player[i] = coopnet_->get_peer_rtt_ms(i);
        }
    }
    return out;
}

uint32_t NetworkManager::get_peer_features(uint8_t player_id) const {
    if (player_id >= MAX_PLAYERS) return 0;
    return peer_features_[player_id];
}

bool NetworkManager::initialize() {
    if (initialized_) return true;

    if (enet_initialize() != 0) {
        std::fprintf(stderr, "[Network] Failed to initialize ENet\n");
        return false;
    }

    initialized_ = true;
    std::printf("[Network] ENet initialized\n");
    return true;
}

void NetworkManager::shutdown() {
    disconnect();

    if (initialized_) {
        enet_deinitialize();
        initialized_ = false;
        std::printf("[Network] ENet shut down\n");
    }
}

bool NetworkManager::host_game() {
    if (!initialized_) return false;
    disconnect();

    const auto& config = get_config();
    server_ = std::make_unique<Server>();

    server_->set_packet_callback([this](uint8_t player_id, const uint8_t* data, size_t size) {
        handle_packet(player_id, data, size);
    });

    server_->set_connect_callback([this](uint8_t player_id) {
        std::printf("[Network] Player %u joined the game\n", player_id);
        request_host_eeprom_send(player_id);  // MIPS will read EEPROM and ship via send_host_eeprom
        request_full_sync(player_id);
        // Register the joiner in the host's roster immediately with a placeholder
        // name + system message. The joiner's broadcast_local_name() will arrive
        // shortly and update the name silently (handle_packet skips re-announce).
        register_remote_player_locally(player_id);
        // Tell other clients about the new slot with a proper placeholder until
        // broadcast_local_name() supplies the real display name.
        {
            std::string ph = banjo::locale::tr_format("chat.player_placeholder", "n",
                std::to_string(player_id + 1));
            for (uint8_t i = 1; i < MAX_PLAYERS; i++) {
                if (i == player_id) continue;
                auto info = get_player_info(i);
                if (!info.connected) continue;
                PlayerJoinPacket announce{};
                announce.header.type = PacketType::PlayerJoin;
                announce.header.player_id = player_id;
                announce.header.sequence = send_sequence_++;
                std::strncpy(announce.player_name, ph.c_str(), 31);
                announce.player_name[31] = '\0';
                server_->send_to(i, &announce, sizeof(announce), CHANNEL_RELIABLE, true);
            }
        }
        // Send host's name directly to the new player
        {
            const auto& config = get_config();
            PlayerJoinPacket host_pkt{};
            host_pkt.header.type = PacketType::PlayerJoin;
            host_pkt.header.player_id = 0; // host
            host_pkt.header.sequence = send_sequence_++;
            std::strncpy(host_pkt.player_name, config.player_name.c_str(), 31);
            host_pkt.player_name[31] = '\0';
            server_->send_to(player_id, &host_pkt, sizeof(host_pkt), CHANNEL_RELIABLE, true);
        }
        // Send existing players' names to the new joiner
        for (uint8_t i = 1; i < MAX_PLAYERS; i++) {
            if (i == player_id) continue;
            auto info = get_player_info(i);
            if (!info.connected) continue;
            PlayerJoinPacket njp{};
            njp.header.type = PacketType::PlayerJoin;
            njp.header.player_id = i;
            njp.header.sequence = send_sequence_++;
            std::strncpy(njp.player_name, info.name.c_str(), 31);
            njp.player_name[31] = '\0';
            server_->send_to(player_id, &njp, sizeof(njp), CHANNEL_RELIABLE, true);
        }
    });

    server_->set_disconnect_callback([this](uint8_t player_id) {
        std::printf("[Network] Player %u left the game\n", player_id);
        std::string leave_name;
        bool was_announced = false;
        {
            std::lock_guard<std::mutex> lock(roster_mutex_);
            if (player_id < MAX_PLAYERS) {
                leave_name = player_roster_[player_id].name;
                was_announced = player_roster_[player_id].announced;
                player_roster_[player_id].connected = false;
                player_roster_[player_id].announced = false;
            }
        }
        if (leave_name.empty()) {
            leave_name = banjo::locale::tr_format("chat.player_placeholder", "n",
                std::to_string(player_id + 1));
        }
        // Only announce "X left" if we previously announced "X joined" — keeps
        // chat consistent when a peer drops before broadcast_local_name lands.
        if (was_announced) {
            add_system_message(banjo::locale::tr_format("chat.player_left", "name", leave_name));
        }
        interpolation_.remove_player(player_id);
        // Release world ownership for this player's level
        uint32_t level;
        {
            std::lock_guard<std::mutex> lock(ownership_mutex_);
            level = player_levels_[player_id];
            player_levels_[player_id] = 0xFFFFFFFF;
        }
        if (level != 0xFFFFFFFF) {
            release_world_owner(level, player_id);
        }
    });

    if (!server_->start(config.port)) {
        server_.reset();
        return false;
    }

    local_player_id_ = 0;
    state_ = ConnectionState::Hosting;
    interpolation_.reset();
    clear_player_roster();
    set_player_name(0, config.player_name);
    initial_sync_done_.store(true); // Host is always "synced"

    std::printf("[Network] Hosting game on port %u\n", config.port);
    return true;
}

bool NetworkManager::join_game() {
    if (!initialized_) return false;
    disconnect();

    const auto& config = get_config();
    client_ = std::make_unique<Client>();

    client_->set_packet_callback([this](uint8_t player_id, const uint8_t* data, size_t size) {
        handle_packet(player_id, data, size);
    });

    client_->set_connect_callback([this]() {
        state_ = ConnectionState::Connected;
        std::printf("[Network] Connected to server\n");
    });

    client_->set_disconnect_callback([this]() {
        state_ = ConnectionState::Disconnected;
        interpolation_.reset();
        unexpected_disconnect_.store(true);
        std::printf("[Network] Lost connection to server\n");
    });

    state_ = ConnectionState::Connecting;
    clear_player_roster();
    initial_sync_done_.store(false); // Don't show join messages during roster sync

    if (!client_->connect(config.join_ip, config.port)) {
        client_.reset();
        state_ = ConnectionState::Disconnected;
        return false;
    }

    local_player_id_ = client_->assigned_player_id();
    state_ = ConnectionState::Connected;
    interpolation_.reset();
    set_player_name(local_player_id_, config.player_name);
    broadcast_local_name();

    std::printf("[Network] Joined game as player %u\n", local_player_id_);
    return true;
}

// === CoopNet lobby operations ===

bool NetworkManager::coopnet_begin(const std::string& server, uint16_t port) {
    if (coopnet_ && coopnet_->is_connected()) return true;

    disconnect();

    // Remember target so tick_network_health() can retry the signaling
    // connection with exponential backoff if we drop unexpectedly.
    reconnect_server_ = server;
    reconnect_port_ = port;
    reconnect_pending_ = false;
    reconnect_attempts_ = 0;
    next_reconnect_at_ = 0.0;

    const auto& config = get_config();
    coopnet_ = std::make_unique<CoopNetTransport>();

    coopnet_->set_packet_callback([this](uint8_t player_id, const uint8_t* data, size_t size) {
        handle_packet(player_id, data, size);
    });

    // Don't set signaling_disconnect_callback here — only set it after we're in a lobby
    // to avoid race conditions during initial connection

    if (lobby_list_callback_) coopnet_->set_lobby_list_callback(lobby_list_callback_);
    if (lobby_created_callback_) coopnet_->set_lobby_created_callback(lobby_created_callback_);
    if (coopnet_error_callback_) coopnet_->set_error_callback(coopnet_error_callback_);
    if (typed_error_callback_) coopnet_->set_typed_error_callback(typed_error_callback_);

    if (!coopnet_->begin(server, port, config.player_name)) {
        coopnet_.reset();
        return false;
    }

    // Non-blocking: coopnet_->update() is called from NetworkManager::update() on SDL thread
    // Connection status is checked via is_coopnet_signaling_connected()
    std::printf("[CoopNet] Connection initiated to %s:%u (async)\n", server.c_str(), port);
    return true;
}

bool NetworkManager::coopnet_host_lobby(const std::string& password, const std::string& description) {
    if (!coopnet_ || !coopnet_->is_connected()) return false;

    coopnet_->set_connect_callback([this](uint8_t player_id) {
        std::printf("[CoopNet] Player %u joined the game\n", player_id);
        // Version handshake first — if the joiner is on an incompatible build,
        // the handler will drop them before any other state is shipped.
        send_version_check(player_id);
        request_host_eeprom_send(player_id);
        request_full_sync(player_id);
        // Register the joiner in the host's roster immediately with a placeholder
        // name + system message + flash the player list. Joiner's broadcast_local_name()
        // arrives shortly and updates the name silently (handle_packet skips re-announce).
        register_remote_player_locally(player_id);
        // Notify other in-lobby clients (CoopNet mesh) with a non-empty name so
        // they never apply the empty-packet → "Player" fallback before
        // broadcast_local_name() arrives.
        {
            std::string ph = banjo::locale::tr_format("chat.player_placeholder", "n",
                std::to_string(player_id + 1));
            for (uint8_t i = 1; i < MAX_PLAYERS; i++) {
                if (i == player_id) continue;
                auto info = get_player_info(i);
                if (!info.connected) continue;
                PlayerJoinPacket announce{};
                announce.header.type = PacketType::PlayerJoin;
                announce.header.player_id = player_id;
                announce.header.sequence = send_sequence_++;
                std::strncpy(announce.player_name, ph.c_str(), 31);
                announce.player_name[31] = '\0';
                net_send_to(i, &announce, sizeof(announce), CHANNEL_RELIABLE, true);
            }
        }
        // Send host's name directly to the new player
        {
            const auto& config = get_config();
            PlayerJoinPacket host_pkt{};
            host_pkt.header.type = PacketType::PlayerJoin;
            host_pkt.header.player_id = 0;
            host_pkt.header.sequence = send_sequence_++;
            std::strncpy(host_pkt.player_name, config.player_name.c_str(), 31);
            host_pkt.player_name[31] = '\0';
            net_send_to(player_id, &host_pkt, sizeof(host_pkt), CHANNEL_RELIABLE, true);
        }
        // Send existing players' names to the new joiner
        for (uint8_t i = 1; i < MAX_PLAYERS; i++) {
            if (i == player_id) continue;
            auto info = get_player_info(i);
            if (!info.connected) continue;
            PlayerJoinPacket njp{};
            njp.header.type = PacketType::PlayerJoin;
            njp.header.player_id = i;
            njp.header.sequence = send_sequence_++;
            std::strncpy(njp.player_name, info.name.c_str(), 31);
            njp.player_name[31] = '\0';
            net_send_to(player_id, &njp, sizeof(njp), CHANNEL_RELIABLE, true);
        }
    });

    coopnet_->set_disconnect_callback([this](uint8_t player_id) {
        BKNET_LOG(Info, "Player %u left the game", player_id);
        std::string leave_name;
        bool was_announced = false;
        {
            std::lock_guard<std::mutex> lock(roster_mutex_);
            if (player_id < MAX_PLAYERS) {
                leave_name = player_roster_[player_id].name;
                was_announced = player_roster_[player_id].announced;
                player_roster_[player_id].connected = false;
                player_roster_[player_id].announced = false;
            }
        }
        if (leave_name.empty()) {
            leave_name = banjo::locale::tr_format("chat.player_placeholder", "n",
                std::to_string(player_id + 1));
        }
        if (was_announced) {
            add_system_message(banjo::locale::tr_format("chat.player_left", "name", leave_name));
        }
        interpolation_.remove_player(player_id);
        reset_peer_state(player_id);
        uint32_t level;
        {
            std::lock_guard<std::mutex> lock(ownership_mutex_);
            level = player_levels_[player_id];
            player_levels_[player_id] = 0xFFFFFFFF;
        }
        if (level != 0xFFFFFFFF) {
            release_world_owner(level, player_id);
        }
    });

    coopnet_->set_signaling_disconnect_callback([this]() {
        state_ = ConnectionState::Disconnected;
        interpolation_.reset();
        unexpected_disconnect_.store(true);
        // Schedule a reconnect (tick_network_health drives the backoff).
        if (!reconnect_server_.empty()) {
            reconnect_pending_ = true;
            next_reconnect_at_ = get_time() + 1.0;
        }
        std::printf("[CoopNet] Lost connection\n");
    });

    if (!coopnet_->create_lobby(password, description, MAX_PLAYERS, get_config().player_name)) {
        return false;
    }

    local_player_id_ = 0;
    state_ = ConnectionState::Hosting;
    interpolation_.reset();
    clear_player_roster();
    set_player_name(0, get_config().player_name);
    initial_sync_done_.store(true);

    std::printf("[CoopNet] Hosting lobby\n");
    return true;
}

bool NetworkManager::coopnet_join_lobby(uint64_t lobby_id, const std::string& password) {
    if (!coopnet_ || !coopnet_->is_connected()) return false;

    coopnet_->set_disconnect_callback([this](uint8_t player_id) {
        if (player_id == 0) {
            state_ = ConnectionState::Disconnected;
            interpolation_.reset();
            unexpected_disconnect_.store(true);
        } else {
            std::string leave_name;
            bool was_announced = false;
            {
                std::lock_guard<std::mutex> lock(roster_mutex_);
                if (player_id < MAX_PLAYERS) {
                    leave_name = player_roster_[player_id].name;
                    was_announced = player_roster_[player_id].announced;
                    player_roster_[player_id].connected = false;
                    player_roster_[player_id].announced = false;
                }
            }
            if (leave_name.empty()) {
                leave_name = banjo::locale::tr_format("chat.player_placeholder", "n",
                    std::to_string(player_id + 1));
            }
            if (was_announced) {
                add_system_message(banjo::locale::tr_format("chat.player_left", "name", leave_name));
            }
            interpolation_.remove_player(player_id);
            reset_peer_state(player_id);
            BKNET_LOG(Info, "Player %u (%s) left", player_id, leave_name.c_str());
        }
    });

    coopnet_->set_signaling_disconnect_callback([this]() {
        state_ = ConnectionState::Disconnected;
        interpolation_.reset();
        unexpected_disconnect_.store(true);
        if (!reconnect_server_.empty()) {
            reconnect_pending_ = true;
            // Joiners with a remembered lobby_id should re-join after signaling
            // is restored. Hosts can't auto-rehost (they'd get a new lobby_id),
            // so the flag stays false for them.
            if (reconnect_lobby_id_ != 0) {
                reconnect_relobby_pending_ = true;
            }
            next_reconnect_at_ = get_time() + 1.0;
        }
        std::printf("[CoopNet] Lost connection\n");
    });

    if (!coopnet_->join_lobby(lobby_id, password)) {
        return false;
    }

    // Remember coordinates so a signaling reconnect can re-enter the same
    // lobby instead of stranding the joiner with signaling-only recovery.
    reconnect_lobby_id_ = lobby_id;
    reconnect_lobby_password_ = password;
    reconnect_relobby_pending_ = false;

    state_ = ConnectionState::Connecting;
    connecting_started_at_ = get_time();
    std::printf("[CoopNet] Joining lobby %llu (async)\n", (unsigned long long)lobby_id);
    return true;
}

void NetworkManager::coopnet_leave_lobby() {
    if (coopnet_) {
        coopnet_->leave_lobby();
    }
}

void NetworkManager::coopnet_request_lobby_list() {
    if (coopnet_) {
        coopnet_->request_lobby_list(get_config().lobby_password);
    }
}

void NetworkManager::set_lobby_list_callback(CoopNetTransport::LobbyListCallback cb) {
    lobby_list_callback_ = cb;
    if (coopnet_) coopnet_->set_lobby_list_callback(cb);
}

void NetworkManager::set_lobby_created_callback(CoopNetTransport::LobbyCreatedCallback cb) {
    lobby_created_callback_ = cb;
    if (coopnet_) coopnet_->set_lobby_created_callback(cb);
}

void NetworkManager::set_coopnet_error_callback(CoopNetTransport::ErrorCallback cb) {
    coopnet_error_callback_ = cb;
    if (coopnet_) coopnet_->set_error_callback(cb);
}

void NetworkManager::set_typed_error_callback(CoopNetTransport::TypedErrorCallback cb) {
    typed_error_callback_ = cb;
    if (coopnet_) coopnet_->set_typed_error_callback(cb);
}

uint32_t NetworkManager::get_player_rtt_ms(uint8_t player_id) const {
    if (player_id >= MAX_PLAYERS || player_id == local_player_id_) return 0;
    if (coopnet_) return coopnet_->get_peer_rtt_ms(player_id);
    return 0;
}

// === Unified send helpers ===

void NetworkManager::net_broadcast(const void* data, size_t size, uint8_t channel, bool reliable) {
    if (coopnet_) {
        coopnet_->broadcast(data, size);
    } else if (server_) {
        server_->broadcast(data, size, channel, reliable);
    } else if (client_) {
        client_->send(data, size, channel, reliable);
    }
    std::lock_guard<std::mutex> lock(stats_mutex_);
    stats_.packets_sent++;
    stats_.bytes_sent += size;
}

void NetworkManager::net_send_to(uint8_t player_id, const void* data, size_t size, uint8_t channel, bool reliable) {
    if (coopnet_) {
        coopnet_->send_to(player_id, data, size);
    } else if (server_) {
        server_->send_to(player_id, data, size, channel, reliable);
    }
    std::lock_guard<std::mutex> lock(stats_mutex_);
    stats_.packets_sent++;
    stats_.bytes_sent += size;
}

void NetworkManager::disconnect() {
    if (coopnet_) {
        coopnet_->shutdown();
        coopnet_.reset();
    }
    if (server_) {
        server_->stop();
        server_.reset();
    }
    if (client_) {
        client_->disconnect();
        client_.reset();
    }

    // Intentional disconnect — cancel any pending reconnection attempts.
    reconnect_pending_ = false;
    reconnect_attempts_ = 0;
    reconnect_server_.clear();
    reconnect_lobby_id_ = 0;
    reconnect_lobby_password_.clear();
    reconnect_relobby_pending_ = false;
    connecting_started_at_ = 0.0;

    // Reset rolling network state so a fresh session starts clean.
    last_seen_seq_.clear();
    fragment_assembly_.clear();
    sent_fragments_.clear();
    last_chat_time_.fill(0.0);
    bytes_recv_window_.fill(0);
    last_bw_window_reset_ = 0.0;
    peer_features_.fill(0);
    last_recv_state_valid_.fill(false);
    for (auto& s : last_recv_state_) s = {};
    last_keepalive_send_ = 0.0;
    last_ping_send_ = 0.0;
    last_idle_check_ = 0.0;
    { std::lock_guard<std::mutex> lock(stats_mutex_); stats_ = {}; }

    state_ = ConnectionState::Disconnected;
    local_player_id_ = 0;
    interpolation_.reset();
    frame_counter_ = 0;
    {
        std::lock_guard<std::mutex> lock(ownership_mutex_);
        world_owner_.clear();
        for (int i = 0; i < MAX_PLAYERS; i++) player_levels_[i] = 0xFFFFFFFF;
        // owner_transfer_queue_ removed (centralized kill tracking)
    }
    {
        std::lock_guard<std::mutex> lock(send_queue_mutex_);
        packet_send_queue_.clear();
        kill_sync_queue_.clear();
        collectible_sync_queue_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(kill_mutex_);
        level_kills_.clear();
        level_collectibles_.clear();
    }
    clear_player_roster();
    initial_sync_done_.store(false);
}

void NetworkManager::update() {
    // CoopNet needs update() even before is_connected() (for signaling handshake)
    if (coopnet_) {
        coopnet_->update();
        // Check if joiner got PlayerAssignment (async join completion)
        if (state_ == ConnectionState::Connecting && coopnet_->local_player_id() != 0) {
            local_player_id_ = coopnet_->local_player_id();
            state_ = ConnectionState::Connected;
            connecting_started_at_ = 0.0; // disarm watchdog
            interpolation_.reset();
            set_player_name(local_player_id_, get_config().player_name);
            broadcast_local_name();
            // As a fresh joiner, immediately send a version handshake to the host.
            // Host will respond if its version matches; if ours is wrong, host
            // drops us. This closes the "silent desync on update" footgun.
            send_version_check(0);
            std::printf("[CoopNet] Joined game as player %u\n", local_player_id_);
        }
        // Reconnection attempt timer (runs even when not connected).
        tick_network_health();
    }

    if (!is_connected() && !coopnet_) return;

    // Pump network events
    if (!coopnet_) {
        if (server_) server_->update();
        if (client_) client_->update();
    }

    // Flush ALL queued packets from game thread (ENet only safe from SDL thread)
    {
        std::lock_guard<std::mutex> lock(send_queue_mutex_);

        // Flush pending kill list syncs (send kill records as death events)
        while (!kill_sync_queue_.empty()) {
            uint32_t level_id = kill_sync_queue_.front();
            kill_sync_queue_.pop_front();

            std::vector<KillRecord> kills;
            {
                std::lock_guard<std::mutex> klock(kill_mutex_);
                auto it = level_kills_.find(level_id);
                if (it != level_kills_.end()) kills = it->second;
            }

            for (const auto& k : kills) {
                WorldEnemyPacket pkt{};
                pkt.header.type = PacketType::WorldEnemy;
                pkt.header.player_id = 0xFF; // Special: resync, not from a real player
                pkt.header.sequence = send_sequence_++;
                pkt.marker_type = k.marker_type;
                pkt.spawn_index = k.spawn_index;
                pkt.map_id = k.map_id;
                pkt.alive = 0;
                pkt.health = 0;

                // Send to remote players via network
                net_broadcast(&pkt, sizeof(pkt), CHANNEL_RELIABLE, true);

                // Also queue locally so the HOST's own game thread processes it
                {
                    std::lock_guard<std::mutex> wlock(world_mutex_);
                    WorldEvent evt{};
                    evt.type = WorldEvent::ENEMY;
                    evt.enemy = pkt;
                    world_events_.push_back(evt);
                }
            }
            if (!kills.empty()) {
                std::printf("[KillTrack] Sent %zu kills for level %u\n", kills.size(), level_id);
            }
        }

        // Flush pending collectible list syncs
        while (!collectible_sync_queue_.empty()) {
            uint32_t level_id = collectible_sync_queue_.front();
            collectible_sync_queue_.pop_front();

            std::vector<CollectibleRecord> colls;
            {
                std::lock_guard<std::mutex> klock(kill_mutex_);
                auto it = level_collectibles_.find(level_id);
                if (it != level_collectibles_.end()) colls = it->second;
            }

            for (const auto& c : colls) {
                WorldCollectiblePacket pkt{};
                pkt.header.type = PacketType::WorldCollectible;
                pkt.header.player_id = 0xFE; // Special: resync (silent despawn)
                pkt.header.sequence = send_sequence_++;
                pkt.collectible_type = c.type;
                pkt.collectible_id = c.id;
                pkt.collected = 1;
                pkt.map_id = c.map_id;
                pkt.level_id = static_cast<uint8_t>(level_id);
                pkt.pos_x = 0.0f;
                pkt.pos_y = 0.0f;
                pkt.pos_z = 0.0f;

                // Send to remote players
                net_broadcast(&pkt, sizeof(pkt), CHANNEL_RELIABLE, true);

                // Also queue locally (HOST needs it too)
                {
                    std::lock_guard<std::mutex> wlock(world_mutex_);
                    WorldEvent evt{};
                    evt.type = WorldEvent::COLLECTIBLE;
                    evt.collectible = pkt;
                    world_events_.push_back(evt);
                }
            }
            if (!colls.empty()) {
                std::printf("[CollTrack] Sent %zu collectibles for level %u\n", colls.size(), level_id);
            }
        }

        // Flush regular queued packets. Per-peer compression: targeted sends
        // get wrapped in a CompressedPacket envelope when the peer negotiated
        // FEATURE_COMPRESSION and the payload is worth compressing. Broadcasts
        // are not compressed here (would need per-peer fan-out — broadcasts
        // are mostly small unreliable position updates anyway).
        while (!packet_send_queue_.empty()) {
            auto& qp = packet_send_queue_.front();
            if (qp.target_player == BROADCAST_TARGET) {
                net_broadcast(qp.data.data(), qp.data.size(), qp.channel, qp.reliable);
            } else {
                auto wrapped = wrap_compressed_for_peer(qp.target_player,
                                                        qp.data.data(), qp.data.size());
                if (!wrapped.empty()) {
                    net_send_to(qp.target_player, wrapped.data(), wrapped.size(),
                                qp.channel, qp.reliable);
                } else {
                    net_send_to(qp.target_player, qp.data.data(), qp.data.size(),
                                qp.channel, qp.reliable);
                }
            }
            packet_send_queue_.pop_front();
        }
    }

    // Mark initial sync as done after first update (roster packets already processed)
    if (!initial_sync_done_.load() && is_connected()) {
        initial_sync_done_.store(true);
    }

    // Send local state at ~20Hz
    frame_counter_++;
    if (frame_counter_ % SEND_INTERVAL_FRAMES == 0) {
        send_local_state();
    }
}

void NetworkManager::push_local_state(float x, float y, float z, float yaw, uint32_t map_id) {
    LocalPlayerSnapshot snap{};
    snap.position[0] = x;
    snap.position[1] = y;
    snap.position[2] = z;
    snap.yaw = yaw;
    snap.map_id = map_id;
    state_sync_.write_local_state(snap);
}

void NetworkManager::push_local_full_state(const LocalPlayerSnapshot& snap) {
    state_sync_.write_local_state(snap);
}

void NetworkManager::set_local_level_id(uint32_t level_id) {
    state_sync_.set_level_id(level_id);
    update_player_level(local_player_id_, level_id);
}

InterpolatedState NetworkManager::get_remote_player(uint8_t player_id) const {
    if (player_id == local_player_id_) return {};
    return interpolation_.get_interpolated(player_id);
}

uint8_t NetworkManager::player_count() const {
    if (coopnet_) return coopnet_->peer_count();
    if (server_) return server_->client_count() + 1;
    if (client_ && client_->is_connected()) {
        std::lock_guard<std::mutex> lock(roster_mutex_);
        uint8_t count = 0;
        for (uint8_t i = 0; i < MAX_PLAYERS; i++) {
            if (player_roster_[i].connected) count++;
        }
        return count > 0 ? count : 1;
    }
    return 1;
}

void NetworkManager::send_local_state() {
    // Send full state packet (Phase 2)
    PlayerStatePacket pkt{};
    if (!state_sync_.build_state_packet(local_player_id_, send_sequence_++, pkt)) {
        return;
    }

    net_broadcast(&pkt, sizeof(pkt), CHANNEL_UNRELIABLE, false);
}

void NetworkManager::handle_packet(uint8_t from_player_id, const uint8_t* data, size_t size,
                                    bool skip_accounting) {
    if (size < sizeof(PacketHeader)) {
        BKNET_LOG(Warn, "Malformed packet from p%u: %zu bytes < header (%zu)",
                  from_player_id, size, sizeof(PacketHeader));
        return;
    }
    // Reject any packet whose sender identity we can't trust. Malformed or
    // spoofed player_ids could OOB-index arrays downstream.
    if (from_player_id >= MAX_PLAYERS) {
        BKNET_LOG(Warn, "Packet from invalid player_id %u (max=%u) dropped",
                  from_player_id, MAX_PLAYERS);
        return;
    }

    PacketType type = peek_type(data, size);

    if (!skip_accounting) {
        // Stats: one inbound packet accounted for (bandwidth cap applies below).
        {
            std::lock_guard<std::mutex> lock(stats_mutex_);
            stats_.packets_received++;
            stats_.bytes_received += size;
        }

        // Per-peer inbound bandwidth cap (1s sliding window). A peer that floods
        // past MAX_INBOUND_BPS gets dropped — protects host from a buggy/hostile
        // client saturating the link.
        double now_secs = get_time();
        if (now_secs - last_bw_window_reset_ >= 1.0) {
            bytes_recv_window_.fill(0);
            last_bw_window_reset_ = now_secs;
        }
        if (from_player_id != local_player_id_) {
            // Stamp first-packet time per peer so the grace-period multiplier
            // covers the join burst (WorldStateFull + HostEeprom + bulk enemies).
            // Cleared in reset_peer_state when a peer disconnects so a slot
            // reuse gets its own grace window.
            if (peer_first_seen_[from_player_id] == 0.0) {
                peer_first_seen_[from_player_id] = now_secs;
            }
            uint32_t cap = MAX_INBOUND_BPS;
            if (now_secs - peer_first_seen_[from_player_id] < JOIN_GRACE_SECONDS) {
                cap *= JOIN_GRACE_MULTIPLIER;
            }
            bytes_recv_window_[from_player_id] += static_cast<uint32_t>(size);
            if (bytes_recv_window_[from_player_id] > cap) {
                BKNET_LOG(Warn, "Player %u exceeded BW cap (%u B/s, cap=%u) — dropping",
                          from_player_id, bytes_recv_window_[from_player_id], cap);
                {
                    std::lock_guard<std::mutex> lock(stats_mutex_);
                    stats_.peers_dropped_bw++;
                    stats_.packets_dropped_rate_limit++;
                }
                if (coopnet_) coopnet_->drop_peer(from_player_id, CoopNetError::PeerFailed);
                return;
            }
        }

        // Liveness: any valid-sender inbound packet (including keepalive) resets
        // the idle timer for that peer. CoopNet side already stamps on receive,
        // but doing it here too means direct-ENet paths also get the benefit if
        // we later add idle detection there.
        if (coopnet_ && from_player_id != local_player_id_) {
            coopnet_->mark_peer_alive(from_player_id);
        }

        // Drop out-of-order high-frequency packets so a late UDP datagram can't
        // overwrite a newer position/state already applied.
        //
        // Identity: `from_player_id` is the peer that delivered this packet —
        // when the host relays a P1 packet to P3, that's the host (=0), but
        // the *originator* is in `hdr_peek.player_id` (=1). Sequence/dedup
        // keying must follow the originator, otherwise relay traffic poisons
        // the host's `last_seen_seq_` slot and the host's own packets get
        // rejected as stale. This was an asymmetric bug that hit only the
        // last joiner (the only one in a position to receive relays for an
        // already-established peer before its own state stream started).
        PacketHeader hdr_peek;
        std::memcpy(&hdr_peek, data, sizeof(hdr_peek));
        uint8_t origin_id = hdr_peek.player_id < MAX_PLAYERS
                                ? hdr_peek.player_id
                                : from_player_id;
        if (!sequence_is_fresh(origin_id, type, hdr_peek.sequence)) {
            BKNET_LOG(Debug, "Stale seq from origin p%u (delivered by p%u) type=0x%02X seq=%u",
                      origin_id, from_player_id, static_cast<unsigned>(type),
                      hdr_peek.sequence);
            std::lock_guard<std::mutex> lock(stats_mutex_);
            stats_.packets_dropped_stale_seq++;
            return;
        }

        // Universal duplicate detection. Skip for self-describing seq=0
        // packets (KeepAlive/Ping/Pong/etc.) — duplicates of those are
        // harmless and they all share seq=0 which would alias as duplicates.
        // Keyed by origin (header.player_id), same reasoning as above.
        if (hdr_peek.sequence != 0 &&
            is_duplicate_packet(origin_id, hdr_peek.sequence, data, size)) {
            BKNET_LOG(Debug, "Duplicate packet from origin p%u (delivered by p%u) type=0x%02X seq=%u",
                      origin_id, from_player_id, static_cast<unsigned>(type),
                      hdr_peek.sequence);
            std::lock_guard<std::mutex> lock(stats_mutex_);
            stats_.packets_dropped_duplicate++;
            return;
        }
    }

    switch (type) {
        case PacketType::KeepAlive:
            // Presence is the payload. mark_peer_alive already ran above.
            return;
        case PacketType::Ping: {
            PingPacket pkt;
            if (deserialize(data, size, pkt)) handle_ping_packet(from_player_id, pkt);
            return;
        }
        case PacketType::Pong: {
            PongPacket pkt;
            if (deserialize(data, size, pkt)) handle_pong_packet(from_player_id, pkt);
            return;
        }
        case PacketType::VersionCheck: {
            VersionCheckPacket pkt;
            if (deserialize(data, size, pkt)) handle_version_check(from_player_id, pkt);
            return;
        }
        case PacketType::Compressed: {
            handle_compressed_packet(from_player_id, data, size);
            return;
        }
        case PacketType::FragmentChunk: {
            FragmentChunkPacket pkt;
            // Fragment packets are variable-size on the wire (trailing chunks
            // can be short), so accept anything with a full fixed header.
            size_t header_size = offsetof(FragmentChunkPacket, data);
            if (size >= header_size) {
                std::memcpy(&pkt, data, std::min(size, sizeof(pkt)));
                handle_fragment_chunk(from_player_id, pkt);
            }
            return;
        }
        case PacketType::FragmentNack: {
            FragmentNackPacket pkt;
            if (deserialize(data, size, pkt)) handle_fragment_nack(from_player_id, pkt);
            return;
        }
        case PacketType::PlayerPosition: {
            PlayerPositionPacket pkt;
            if (deserialize(data, size, pkt)) {
                handle_position_packet(pkt);
            }
            break;
        }
        case PacketType::PlayerState: {
            PlayerStatePacket pkt;
            if (deserialize(data, size, pkt)) {
                handle_state_packet(pkt);
            }
            break;
        }
        case PacketType::MapChange: {
            MapChangePacket pkt;
            if (deserialize(data, size, pkt)) {
                handle_map_change_packet(pkt);
            }
            break;
        }
        case PacketType::ChatMessage: {
            ChatMessagePacket pkt{};
            if (size >= sizeof(PacketHeader) + 1) {
                std::memcpy(&pkt, data, std::min(size, sizeof(pkt)));
                handle_chat_packet(pkt);
            }
            break;
        }
        case PacketType::PlayerJoin: {
            PlayerJoinPacket pkt;
            if (deserialize(data, size, pkt)) {
                pkt.player_name[31] = '\0'; // safety
                std::string name(pkt.player_name);
                // Drop empty-name packets entirely (e.g. ENet net_server.cpp's
                // zero-init announcement). Previously these were promoted to
                // "Player" and then guarded against — but that guard also
                // rejected a joiner whose actual configured name *is* "Player",
                // which is the default and very common. Treating empty as
                // "no update" eliminates the ambiguity at the source.
                if (name.empty()) break;

                // Localized placeholder ("Jugador 2", "Player 2", ...) recognition:
                // the host fans out the placeholder to other peers when somebody
                // joins, before broadcast_local_name() arrives. We must (a) not
                // overwrite a real name we already have, and (b) suppress the
                // chat "X joined" announce until the real name lands.
                bool is_placeholder = false;
                if (pkt.header.player_id < MAX_PLAYERS) {
                    std::string ph = banjo::locale::tr_format("chat.player_placeholder", "n",
                        std::to_string(pkt.header.player_id + 1));
                    if (name == ph) {
                        is_placeholder = true;
                        std::lock_guard<std::mutex> lock(roster_mutex_);
                        const std::string& ex = player_roster_[pkt.header.player_id].name;
                        if (player_roster_[pkt.header.player_id].connected && !ex.empty() &&
                            ex != ph) {
                            break;
                        }
                    }
                }

                // Was this player already in our roster, and have we already
                // announced their join in chat? We track these separately:
                // `connected` is set by register_remote_player_locally on the
                // host (for UI purposes), while `announced` is only set once
                // we've shown "X joined" with the real display name.
                bool was_connected = false;
                bool was_announced = false;
                {
                    std::lock_guard<std::mutex> lock(roster_mutex_);
                    if (pkt.header.player_id < MAX_PLAYERS) {
                        was_connected = player_roster_[pkt.header.player_id].connected;
                        was_announced = player_roster_[pkt.header.player_id].announced;
                    }
                }

                set_player_name(pkt.header.player_id, name);

                // CoopNet: raw relay in on_receive() uses juice_send per peer but
                // late joiners can still miss names if a link stalls. Re-fan-out
                // real display names through NetworkManager so each client gets
                // our normal reliable path (and optional compression).
                if (coopnet_ && is_host() && pkt.header.player_id != local_player_id_) {
                    if (!is_placeholder) {
                        for (uint8_t i = 1; i < MAX_PLAYERS; ++i) {
                            if (i == pkt.header.player_id) continue;
                            auto pi = get_player_info(i);
                            if (!pi.connected) continue;
                            net_send_to(i, &pkt, sizeof(pkt), CHANNEL_RELIABLE, true);
                        }
                    }
                }

                // Show "X joined" exactly once, using the REAL name. Skip when:
                //   - it's our own slot,
                //   - we haven't finished the initial roster sync yet,
                //   - we've already announced this slot,
                //   - the incoming name is just the localized placeholder
                //     (host fan-out before broadcast_local_name arrives).
                if (pkt.header.player_id != local_player_id_ &&
                    initial_sync_done_.load() &&
                    !was_announced &&
                    !is_placeholder) {
                    add_system_message(banjo::locale::tr_format("chat.player_joined", "name", name));
                    std::lock_guard<std::mutex> lock(roster_mutex_);
                    if (pkt.header.player_id < MAX_PLAYERS) {
                        player_roster_[pkt.header.player_id].announced = true;
                    }
                }
                std::printf("[Network] Player %u joined as '%s' (sync_done=%d, was_connected=%d, was_announced=%d, placeholder=%d)\n",
                    pkt.header.player_id, name.c_str(),
                    initial_sync_done_.load() ? 1 : 0, was_connected ? 1 : 0,
                    was_announced ? 1 : 0, is_placeholder ? 1 : 0);
            }
            break;
        }
        case PacketType::PlayerLeave: {
            PlayerLeavePacket pkt;
            if (deserialize(data, size, pkt)) {
                // Get name before clearing
                std::string leave_name;
                bool was_announced = false;
                {
                    std::lock_guard<std::mutex> lock(roster_mutex_);
                    if (pkt.header.player_id < MAX_PLAYERS) {
                        leave_name = player_roster_[pkt.header.player_id].name;
                        was_announced = player_roster_[pkt.header.player_id].announced;
                        player_roster_[pkt.header.player_id].connected = false;
                        player_roster_[pkt.header.player_id].announced = false;
                    }
                }
                if (leave_name.empty()) {
                    leave_name = banjo::locale::tr_format("chat.player_placeholder", "n",
                        std::to_string(pkt.header.player_id + 1));
                }
                interpolation_.remove_player(pkt.header.player_id);
                if (was_announced) {
                    add_system_message(banjo::locale::tr_format("chat.player_left", "name", leave_name));
                }
                std::printf("[Network] Player %u (%s) left\n", pkt.header.player_id, leave_name.c_str());
            }
            break;
        }
        case PacketType::WorldCollectible: {
            WorldCollectiblePacket pkt;
            if (deserialize(data, size, pkt)) {
                handle_collectible_packet(pkt);
            }
            break;
        }
        case PacketType::WorldEnemy: {
            WorldEnemyPacket pkt;
            if (deserialize(data, size, pkt)) {
                handle_enemy_packet(pkt);
            }
            break;
        }
        case PacketType::WorldObject: {
            WorldFlagPacket pkt;
            if (deserialize(data, size, pkt)) {
                handle_flag_packet(pkt);
            }
            break;
        }
        case PacketType::WorldStateFull: {
            WorldStateFullPacket pkt;
            if (deserialize(data, size, pkt)) {
                handle_world_state_full_packet(pkt);
            }
            break;
        }
        case PacketType::WorldStateFullAck: {
            WorldStateFullAckPacket pkt;
            if (deserialize(data, size, pkt)) {
                handle_world_state_full_ack(from_player_id, pkt);
            }
            break;
        }
        case PacketType::HostEeprom: {
            HostEepromPacket pkt;
            if (deserialize(data, size, pkt)) {
                handle_host_eeprom_packet(pkt);
            }
            break;
        }
        case PacketType::EnemyPositionBulk: {
            handle_enemy_position_packet(data, size);
            break;
        }
        case PacketType::WorldOwnership: {
            WorldOwnershipPacket pkt;
            if (deserialize(data, size, pkt)) {
                handle_ownership_packet(pkt);
            }
            break;
        }
        case PacketType::CongaOrangeSpawn: {
            CongaOrangeSpawnPacket pkt;
            if (deserialize(data, size, pkt)) {
                handle_conga_orange_packet(pkt);
            }
            break;
        }
        // WorldOwnerTransfer and WorldKillResync — no longer used
        // (centralized kill tracking in C++ replaces these)
        default: {
            {
                std::lock_guard<std::mutex> lock(stats_mutex_);
                stats_.packets_dropped_unknown++;
            }
            BKNET_LOG(Warn, "Unknown PacketType 0x%02X from player %u (protocol mismatch?)",
                      static_cast<unsigned>(type), from_player_id);
            break;
        }
    }
}

void NetworkManager::handle_position_packet(const PlayerPositionPacket& pkt) {
    uint8_t pid = pkt.header.player_id;
    if (pid == local_player_id_ || pid >= MAX_PLAYERS) return;

    interpolation_.push_position(pid, pkt.x, pkt.y, pkt.z, pkt.yaw, pkt.map_id);
}

void NetworkManager::handle_state_packet(const PlayerStatePacket& pkt) {
    uint8_t pid = pkt.header.player_id;
    if (pid == local_player_id_ || pid >= MAX_PLAYERS) return;

    update_player_level(pid, pkt.level_id);

    // Merge-apply: start from the last fully-known snapshot for this player,
    // then overwrite fields whose dirty bits are set in this packet. If we
    // don't have a baseline yet, or the sender sent everything (full-apply
    // shortcut 0xFFFFFFFF), apply the whole packet.
    PositionSnapshot snap{};
    const bool full_apply =
        !last_recv_state_valid_[pid] || pkt.dirty_flags == 0xFFFFFFFFu;
    if (!full_apply) {
        snap = last_recv_state_[pid];
    }

    const uint8_t bs_before = snap.bs_state;

    if (full_apply || (pkt.dirty_flags & DIRTY_POSITION)) {
        snap.x = pkt.x; snap.y = pkt.y; snap.z = pkt.z;
    }
    if (full_apply || (pkt.dirty_flags & DIRTY_ROTATION)) {
        snap.yaw = pkt.yaw; snap.pitch = pkt.pitch;
    }
    if (full_apply || (pkt.dirty_flags & DIRTY_MAP)) {
        snap.map_id = pkt.map_id;
        snap.level_id = pkt.level_id;
    }
    if (full_apply || (pkt.dirty_flags & DIRTY_ITEMS)) {
        snap.kazooie_flags = pkt.kazooie_flags;
        snap.bs_state = static_cast<uint8_t>(pkt.bs_state);
        snap.horizontal_velocity = pkt.horizontal_velocity;
    }
    /* BS changes almost always imply a new AnimCtrl clip on the sender. Dirty
     * flags are computed independently — a packet can list DIRTY_ITEMS without
     * DIRTY_ANIMATION if floats/id matched the *previous* sent snapshot while
     * our merged baseline still had stale anim fields. Always take animation
     * from the packet payload when bs_state actually changes. */
    const bool bs_changed =
        !full_apply && (pkt.dirty_flags & DIRTY_ITEMS) && (snap.bs_state != bs_before);

    if (full_apply || (pkt.dirty_flags & DIRTY_ANIMATION) || bs_changed) {
        snap.animation_id = pkt.animation_id;
        snap.anim_timer = pkt.anim_progress;
        snap.anim_duration = pkt.anim_duration;
        snap.anim_subrange_start = pkt.anim_subrange_start;
        snap.anim_subrange_end = pkt.anim_subrange_end;
        snap.anim_playback_type = pkt.anim_playback_type;
    }
    if (full_apply || (pkt.dirty_flags & DIRTY_HEALTH)) {
        snap.health = pkt.health;
        snap.health_total = pkt.health_total;
    }
    if (full_apply || (pkt.dirty_flags & DIRTY_TRANSFORMATION)) {
        snap.transformation = pkt.transformation;
    }
    if (full_apply || (pkt.dirty_flags & DIRTY_CARRY)) {
        snap.carry_kind = pkt.carry_kind;
    }
    // scale isn't in any flag group — always take it (sender hardcodes 1.0f)
    if (full_apply) snap.scale = 1.0f;

    last_recv_state_[pid] = snap;
    last_recv_state_valid_[pid] = true;

    interpolation_.push_full_state(pid, snap);
}

void NetworkManager::add_system_message(const std::string& message) {
    std::lock_guard<std::mutex> lock(chat_mutex_);
    chat_history_.push_back({0xFF, message, get_time()}); // 0xFF = system
    if (chat_history_.size() > MAX_CHAT_HISTORY) chat_history_.pop_front();
    new_messages_ = true;
}

void NetworkManager::send_chat(const std::string& message) {
    if (!is_connected() || message.empty()) return;

    ChatMessagePacket pkt{};
    pkt.header.type = PacketType::ChatMessage;
    pkt.header.player_id = local_player_id_;
    pkt.header.sequence = send_sequence_++;
    pkt.msg_length = static_cast<uint8_t>(std::min(message.size(), CHAT_MAX_LENGTH - 1));
    std::memcpy(pkt.message, message.c_str(), pkt.msg_length);
    pkt.message[pkt.msg_length] = '\0';

    // Add to own chat history
    {
        std::lock_guard<std::mutex> lock(chat_mutex_);
        chat_history_.push_back({local_player_id_, message, get_time()});
        if (chat_history_.size() > MAX_CHAT_HISTORY) chat_history_.pop_front();
        new_messages_ = true;
    }

    size_t send_size = sizeof(PacketHeader) + 1 + pkt.msg_length + 1;
    enqueue_packet(&pkt, send_size, CHANNEL_RELIABLE, true);

    std::printf("[Chat] P%u: %s\n", local_player_id_, message.c_str());
}

void NetworkManager::handle_chat_packet(const ChatMessagePacket& pkt) {
    uint8_t pid = pkt.header.player_id;
    if (pid >= MAX_PLAYERS) return; // malformed / spoof
    if (pkt.msg_length >= CHAT_MAX_LENGTH) return; // oversized

    // Per-player rate limit: 2 msg/sec. A buggy/malicious client can't spam
    // the chat history and blow the 20-entry ring.
    double now = get_time();
    if (now - last_chat_time_[pid] < 0.5) {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_.packets_dropped_rate_limit++;
        return;
    }
    last_chat_time_[pid] = now;

    std::string msg(pkt.message, pkt.msg_length);

    {
        std::lock_guard<std::mutex> lock(chat_mutex_);
        chat_history_.push_back({pid, msg, now});
        if (chat_history_.size() > MAX_CHAT_HISTORY) chat_history_.pop_front();
        new_messages_ = true;
    }

    std::printf("[Chat] P%u: %s\n", pid, msg.c_str());
}

std::deque<NetworkManager::ChatEntry> NetworkManager::get_chat_messages() const {
    std::lock_guard<std::mutex> lock(chat_mutex_);
    // Return only recent messages
    std::deque<ChatEntry> recent;
    double now = get_time();
    for (const auto& e : chat_history_) {
        if (now - e.timestamp < CHAT_DISPLAY_DURATION) {
            recent.push_back(e);
        }
    }
    return recent;
}

std::deque<NetworkManager::ChatEntry> NetworkManager::get_all_chat_messages() const {
    std::lock_guard<std::mutex> lock(chat_mutex_);
    return chat_history_;
}

bool NetworkManager::has_new_messages() const {
    std::lock_guard<std::mutex> lock(chat_mutex_);
    return new_messages_;
}

void NetworkManager::clear_new_message_flag() {
    std::lock_guard<std::mutex> lock(chat_mutex_);
    new_messages_ = false;
}

double NetworkManager::get_time() const {
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(now - start_time_).count();
}

void NetworkManager::handle_map_change_packet(const MapChangePacket& pkt) {
    uint8_t pid = pkt.header.player_id;
    if (pid == local_player_id_ || pid >= MAX_PLAYERS) return;

    std::printf("[Network] Player %u changed to map %u\n", pid, pkt.new_map_id);
    // Ghost actor management will handle this via map_id changes in interpolated state
}

// === World state sync (Phase 3) ===

void NetworkManager::send_collectible(uint8_t type, uint16_t id, uint8_t collected, uint32_t map_id, uint8_t level_id) {
    if (!is_connected()) return;

    // Record in centralized tracking
    if (collected) {
        record_collectible(static_cast<uint32_t>(level_id), type, id, map_id);
    }

    // Get local player position for proximity-based despawn
    auto snap = state_sync_.read_local_state();

    WorldCollectiblePacket pkt{};
    pkt.header.type = PacketType::WorldCollectible;
    pkt.header.player_id = local_player_id_;
    pkt.header.sequence = send_sequence_++;
    pkt.collectible_type = type;
    pkt.collectible_id = id;
    pkt.collected = collected;
    pkt.map_id = map_id;
    pkt.level_id = level_id;
    pkt.pos_x = snap.position[0];
    pkt.pos_y = snap.position[1];
    pkt.pos_z = snap.position[2];

    enqueue_packet(&pkt, sizeof(pkt), CHANNEL_RELIABLE, true);
}

void NetworkManager::send_enemy_death(uint16_t marker_type, uint16_t spawn_index, uint32_t map_id, float px, float py, float pz, uint8_t state) {
    if (!is_connected()) return;

    // Record locally in centralized kill tracking
    {
        uint32_t local_level;
        {
            std::lock_guard<std::mutex> lock(ownership_mutex_);
            local_level = player_levels_[local_player_id_];
        }
        if (local_level != 0xFFFFFFFF) {
            record_kill(local_level, marker_type, spawn_index, map_id);
        }
    }

    WorldEnemyPacket pkt{};
    pkt.header.type = PacketType::WorldEnemy;
    pkt.header.player_id = local_player_id_;
    pkt.header.sequence = send_sequence_++;
    pkt.marker_type = marker_type;
    pkt.spawn_index = spawn_index;
    pkt.map_id = map_id;
    pkt.alive = 0;
    pkt.health = 0;
    pkt.state = state;
    pkt.pos_x = px;
    pkt.pos_y = py;
    pkt.pos_z = pz;

    enqueue_packet(&pkt, sizeof(pkt), CHANNEL_RELIABLE, true);
}

void NetworkManager::send_flag_change(uint8_t flag_type, uint16_t flag_index, uint8_t value, uint32_t map_id) {
    if (!is_connected()) return;

    WorldFlagPacket pkt{};
    pkt.header.type = PacketType::WorldObject;
    pkt.header.player_id = local_player_id_;
    pkt.header.sequence = send_sequence_++;
    pkt.flag_type = flag_type;
    pkt.flag_index = flag_index;
    pkt.value = value;
    pkt.map_id = map_id;

    enqueue_packet(&pkt, sizeof(pkt), CHANNEL_RELIABLE, true);
}

// === Conga orange projectile sync ===

void NetworkManager::send_conga_orange(float sx, float sy, float sz, float vx, float vy, float vz, uint32_t map_id) {
    if (!is_connected()) return;

    CongaOrangeSpawnPacket pkt{};
    pkt.header.type = PacketType::CongaOrangeSpawn;
    pkt.header.player_id = local_player_id_;
    pkt.header.sequence = send_sequence_++;
    pkt.map_id = map_id;
    pkt.spawn_x = sx;
    pkt.spawn_y = sy;
    pkt.spawn_z = sz;
    pkt.vel_x = vx;
    pkt.vel_y = vy;
    pkt.vel_z = vz;

    enqueue_packet(&pkt, sizeof(pkt), CHANNEL_RELIABLE, true);
}

void NetworkManager::handle_conga_orange_packet(const CongaOrangeSpawnPacket& pkt) {
    if (pkt.header.player_id >= MAX_PLAYERS) return;
    if (pkt.header.player_id == local_player_id_) return;
    std::lock_guard<std::mutex> lock(world_mutex_);
    CongaOrangeEvent evt{};
    evt.spawn_x = pkt.spawn_x;
    evt.spawn_y = pkt.spawn_y;
    evt.spawn_z = pkt.spawn_z;
    evt.vel_x = pkt.vel_x;
    evt.vel_y = pkt.vel_y;
    evt.vel_z = pkt.vel_z;
    conga_orange_queue_.push_back(evt);
}

bool NetworkManager::pop_conga_orange(CongaOrangeEvent& out) {
    std::lock_guard<std::mutex> lock(world_mutex_);
    if (conga_orange_queue_.empty()) return false;
    out = conga_orange_queue_.front();
    conga_orange_queue_.pop_front();
    return true;
}

void NetworkManager::handle_collectible_packet(const WorldCollectiblePacket& pkt) {
    if (pkt.header.player_id == local_player_id_) return;

    // Record in centralized tracking (resync packets use player_id 0xFE)
    if (pkt.collected && pkt.header.player_id < MAX_PLAYERS) {
        record_collectible(static_cast<uint32_t>(pkt.level_id),
                           pkt.collectible_type, pkt.collectible_id, pkt.map_id);
    }

    std::lock_guard<std::mutex> lock(world_mutex_);
    WorldEvent evt{};
    evt.type = WorldEvent::COLLECTIBLE;
    evt.collectible = pkt;
    world_events_.push_back(evt);
}

void NetworkManager::handle_enemy_packet(const WorldEnemyPacket& pkt) {
    if (pkt.header.player_id == local_player_id_) return;

    // Record kill in centralized tracking (HOST is source of truth)
    // Skip resync packets (player_id 0xFF) — they're already in level_kills_
    if (pkt.alive == 0 && pkt.header.player_id < MAX_PLAYERS) {
        uint8_t sender = pkt.header.player_id;
        uint32_t sender_level;
        {
            std::lock_guard<std::mutex> lock(ownership_mutex_);
            sender_level = player_levels_[sender];
        }
        if (sender_level != 0xFFFFFFFF) {
            record_kill(sender_level, pkt.marker_type, pkt.spawn_index, pkt.map_id);
        }
    }

    std::lock_guard<std::mutex> lock(world_mutex_);
    WorldEvent evt{};
    evt.type = WorldEvent::ENEMY;
    evt.enemy = pkt;
    world_events_.push_back(evt);
}

void NetworkManager::handle_flag_packet(const WorldFlagPacket& pkt) {
    if (pkt.header.player_id == local_player_id_) return;
    std::lock_guard<std::mutex> lock(world_mutex_);
    WorldEvent evt{};
    evt.type = WorldEvent::FLAG;
    evt.flag = pkt;
    world_events_.push_back(evt);
}

bool NetworkManager::pop_world_event(WorldEvent& out) {
    std::lock_guard<std::mutex> lock(world_mutex_);
    if (world_events_.empty()) return false;
    out = world_events_.front();
    world_events_.pop_front();
    return true;
}

// === Enemy position sync (host-authoritative) ===

void NetworkManager::send_enemy_positions(const EnemyPositionEntry* entries, uint8_t count, uint32_t map_id) {
    if (!is_connected() || count == 0) return;

    // Build variable-size packet
    size_t payload_size = sizeof(PacketHeader) + sizeof(uint32_t) + 4 + count * sizeof(EnemyPositionEntry);
    EnemyPositionBulkPacket pkt{};
    pkt.header.type = PacketType::EnemyPositionBulk;
    pkt.header.player_id = local_player_id_;
    pkt.header.sequence = send_sequence_++;
    pkt.map_id = map_id;
    pkt.enemy_count = count;
    std::memcpy(pkt.enemies, entries, count * sizeof(EnemyPositionEntry));

    enqueue_packet(&pkt, payload_size, CHANNEL_UNRELIABLE, false);
}

void NetworkManager::handle_enemy_position_packet(const uint8_t* data, size_t size) {
    // Minimum size: header + map_id + count + pad
    if (size < sizeof(PacketHeader) + 8) return;

    PacketHeader hdr;
    std::memcpy(&hdr, data, sizeof(hdr));
    if (hdr.player_id == local_player_id_) return;

    uint32_t map_id;
    std::memcpy(&map_id, data + sizeof(PacketHeader), sizeof(uint32_t));
    uint8_t enemy_count = data[sizeof(PacketHeader) + 4];

    size_t entries_offset = sizeof(PacketHeader) + 8; // header + map_id + count + pad
    size_t expected_size = entries_offset + enemy_count * sizeof(EnemyPositionEntry);
    if (size < expected_size || enemy_count > MAX_ENEMIES_PER_PACKET) return;

    const auto* entries = reinterpret_cast<const EnemyPositionEntry*>(data + entries_offset);
    enemy_interp_.push_bulk(entries, enemy_count, map_id, get_time());
}

size_t NetworkManager::get_enemy_positions(EnemyInterpolatedState* out, size_t max_count) const {
    return enemy_interp_.get_interpolated(out, max_count, get_time());
}

// === Full state sync on join ===

void NetworkManager::request_full_sync(uint8_t player_id) {
    sync_target_player_ = player_id;
    pending_full_sync_.store(true);
    if (player_id < MAX_PLAYERS) {
        // Reset ack tracking for this slot. Retries reset to 0 only on the
        // initial request (not on a retry-driven re-arm — that path already
        // increments full_sync_retries_ before re-arming).
        full_sync_sent_at_[player_id] = 0.0;
        full_sync_acked_[player_id] = false;
        full_sync_retries_[player_id] = 0;
    }
    std::printf("[Network] Full state sync requested for player %u\n", player_id);
}

bool NetworkManager::should_send_full_sync(uint8_t& out_player_id) {
    if (pending_full_sync_.load()) {
        out_player_id = sync_target_player_;
        pending_full_sync_.store(false);
        return true;
    }
    return false;
}

void NetworkManager::send_world_state_full(const uint8_t* data, size_t size, uint8_t target_player) {
    if (!is_connected() || (!server_ && !coopnet_)) return;

    // Fragment large payloads: a single dropped packet would otherwise trigger
    // full-packet retransmit and risk join timeout on lossy WAN. Chunks are
    // reliable individually, so ENet/CoopNet retries only the missing piece.
    // Targeted send: WorldStateFull is only meaningful to the newly-joined peer.
    // Broadcasting it to peers already in the session re-applies state (visual/
    // logic glitches) and multiplies the burst by player count, which can trip
    // MAX_INBOUND_BPS on receivers and force a kick.
    if (size > FRAGMENT_CHUNK_PAYLOAD) {
        send_chunked(target_player, PacketType::WorldStateFull, data, size);
    } else {
        enqueue_packet_to(target_player, data, size, CHANNEL_RELIABLE, true);
    }
    // Stamp send time so tick_full_sync_retry can detect a missing ack.
    if (target_player < MAX_PLAYERS) {
        full_sync_sent_at_[target_player] = get_time();
        full_sync_acked_[target_player] = false;
    }
    std::printf("[Network] Queued WorldStateFull (%zu bytes) for player %u\n", size, target_player);
}

void NetworkManager::handle_world_state_full_packet(const WorldStateFullPacket& pkt) {
    if (is_host()) return;
    {
        std::lock_guard<std::mutex> lock(world_mutex_);
        full_state_queue_.push_back(pkt);
    }
    std::printf("[Network] Received WorldStateFull from host\n");

    // Echo ack to host so it knows the bulk transfer landed and won't retry.
    // Sent unconditionally; host gates retries on its own peer_features_ check
    // so an old host without FULLSTATE_ACK simply ignores this packet.
    if (coopnet_) {
        WorldStateFullAckPacket ack{};
        ack.header.type = PacketType::WorldStateFullAck;
        ack.header.player_id = local_player_id_;
        ack.header.sequence = 0;
        ack.map_id = pkt.map_id;
        coopnet_->send_to(0, &ack, sizeof(ack));
    }
}

void NetworkManager::handle_world_state_full_ack(uint8_t from_player_id, const WorldStateFullAckPacket& pkt) {
    if (!is_host()) return;
    if (from_player_id >= MAX_PLAYERS) return;
    full_sync_acked_[from_player_id] = true;
    full_sync_sent_at_[from_player_id] = 0.0;
    BKNET_LOG(Info, "WorldStateFullAck from player %u (map=%u) — join sync confirmed",
              from_player_id, pkt.map_id);
}

bool NetworkManager::pop_full_state(WorldStateFullPacket& out) {
    std::lock_guard<std::mutex> lock(world_mutex_);
    if (full_state_queue_.empty()) return false;
    out = full_state_queue_.front();
    full_state_queue_.pop_front();
    return true;
}

void NetworkManager::tick_full_sync_retry(double now) {
    // Re-arm pending_full_sync_ for any joiner whose last send didn't ack
    // within FULL_SYNC_ACK_TIMEOUT. Skips peers without FULLSTATE_ACK so
    // older clients fall back to the original fire-and-forget behaviour.
    // Skips while another sync is already pending so we don't clobber the
    // active target_player.
    if (pending_full_sync_.load()) return;
    for (uint8_t pid = 1; pid < MAX_PLAYERS; pid++) {
        if (full_sync_acked_[pid]) continue;
        if (full_sync_sent_at_[pid] == 0.0) continue;
        if ((peer_features_[pid] & FEATURE_FULLSTATE_ACK) == 0) {
            // Peer doesn't support ack — assume delivered, stop watching.
            full_sync_sent_at_[pid] = 0.0;
            continue;
        }
        if (now - full_sync_sent_at_[pid] < FULL_SYNC_ACK_TIMEOUT) continue;
        if (full_sync_retries_[pid] >= MAX_FULL_SYNC_RETRIES) {
            BKNET_LOG(Warn,
                      "WorldStateFull retries exhausted for player %u — peer likely stale",
                      pid);
            full_sync_sent_at_[pid] = 0.0; // stop spamming the log
            continue;
        }
        full_sync_retries_[pid]++;
        BKNET_LOG(Warn, "No WorldStateFull ack from player %u after %.1fs — retry %u/%u",
                  pid, now - full_sync_sent_at_[pid],
                  full_sync_retries_[pid], MAX_FULL_SYNC_RETRIES);
        sync_target_player_ = pid;
        full_sync_sent_at_[pid] = 0.0; // re-stamp on next send
        pending_full_sync_.store(true);
        break; // one retry per tick — host-EEPROM is independent
    }
}

// === Host EEPROM snapshot (SM64 Coop DX-style save override) ===

void NetworkManager::request_host_eeprom_send(uint8_t player_id) {
    host_eeprom_target_player_ = player_id;
    pending_host_eeprom_.store(true);
    std::printf("[Network] Host EEPROM send requested for player %u\n", player_id);
}

bool NetworkManager::should_send_host_eeprom(uint8_t& out_player_id) {
    if (pending_host_eeprom_.load()) {
        out_player_id = host_eeprom_target_player_;
        pending_host_eeprom_.store(false);
        return true;
    }
    return false;
}

void NetworkManager::send_host_eeprom(const uint8_t* eeprom_bytes, size_t size, uint8_t target_player, int16_t current_slot) {
    if (!is_connected() || (!server_ && !coopnet_)) return;

    HostEepromPacket pkt{};
    pkt.header.type = PacketType::HostEeprom;
    pkt.header.player_id = local_player_id_;
    pkt.header.sequence = send_sequence_++;
    pkt.current_slot = current_slot;
    size_t copy = (size < HOST_EEPROM_SIZE) ? size : HOST_EEPROM_SIZE;
    std::memcpy(pkt.eeprom, eeprom_bytes, copy);

    // Targeted send: HostEeprom is meaningful only to the newly-joined peer.
    // The payload is ~2 KB — always fragment so a single lost packet triggers
    // only a small chunk retransmit instead of redelivering the whole blob
    // and risking a join timeout.
    send_chunked(target_player, PacketType::HostEeprom, &pkt, sizeof(pkt));
    std::printf("[Network] Queued HostEeprom (%zu bytes, slot=%d) for player %u\n",
                sizeof(pkt), (int)current_slot, target_player);
}

void NetworkManager::handle_host_eeprom_packet(const HostEepromPacket& pkt) {
    if (is_host()) return;  // host never applies its own EEPROM from wire
    std::lock_guard<std::mutex> lock(world_mutex_);
    host_eeprom_queue_.push_back(pkt);
    std::printf("[Network] Received HostEeprom (%zu bytes)\n", sizeof(pkt));
}

bool NetworkManager::pop_host_eeprom(HostEepromPacket& out) {
    std::lock_guard<std::mutex> lock(world_mutex_);
    if (host_eeprom_queue_.empty()) return false;
    out = host_eeprom_queue_.front();
    host_eeprom_queue_.pop_front();
    return true;
}

// === Thread-safe packet sending ===

void NetworkManager::reset_peer_state(uint8_t player_id) {
    if (player_id >= MAX_PLAYERS) return;
    // Clear anything keyed by player_id that could leak to a new occupant of
    // the slot: seq tracking, rate limits, inbound BW window, reassembly and
    // negotiated features, cached "last state" used by dirty-delta apply.
    last_chat_time_[player_id] = 0.0;
    bytes_recv_window_[player_id] = 0;
    peer_first_seen_[player_id] = 0.0;
    peer_features_[player_id] = 0;
    full_sync_sent_at_[player_id] = 0.0;
    full_sync_acked_[player_id] = false;
    full_sync_retries_[player_id] = 0;
    rx_dedup_[player_id] = RxDedup{};
    last_recv_state_valid_[player_id] = false;
    last_recv_state_[player_id] = {};

    // Wipe per-player sequence entries.
    for (auto it = last_seen_seq_.begin(); it != last_seen_seq_.end(); ) {
        uint8_t pid = static_cast<uint8_t>(it->first >> 8);
        if (pid == player_id) it = last_seen_seq_.erase(it);
        else ++it;
    }

    // Drop any in-flight fragment reassembly from this sender.
    for (auto it = fragment_assembly_.begin(); it != fragment_assembly_.end(); ) {
        uint8_t pid = static_cast<uint8_t>(it->first >> 16);
        if (pid == player_id) it = fragment_assembly_.erase(it);
        else ++it;
    }

    BKNET_LOG(Info, "Reset per-peer state for player %u", player_id);
}

// Static helper: must be answered without instance state because it's called
// from the locked backpressure loop on every queued payload. Listed here so
// the criteria for "do not drop" lives in one place.
bool NetworkManager::is_critical_reliable(uint8_t first_byte) {
    auto t = static_cast<PacketType>(first_byte);
    switch (t) {
        case PacketType::PlayerAssignment:    // joiner identity — without it, no session
        case PacketType::VersionCheck:        // handshake — drop = stuck handshake
        case PacketType::WorldStateFull:      // initial world snapshot
        case PacketType::WorldStateFullAck:   // host gates retries on this
        case PacketType::HostEeprom:          // save data, ~2 KB join burst
        case PacketType::FragmentChunk:       // a missing chunk strands the whole bulk
        case PacketType::FragmentNack:        // recovery path for the above
        case PacketType::Compressed:          // wrapper may carry any of the above
            return true;
        default:
            return false;
    }
}

void NetworkManager::enqueue_packet(const void* data, size_t size, uint8_t channel, bool reliable) {
    enqueue_packet_to(BROADCAST_TARGET, data, size, channel, reliable);
}

void NetworkManager::enqueue_packet_to(uint8_t target_player, const void* data, size_t size, uint8_t channel, bool reliable) {
    std::lock_guard<std::mutex> lock(send_queue_mutex_);

    // Backpressure: if the queue is huge (CoopNet stalled or game thread
    // spamming), drop in priority order:
    //   1. oldest unreliable packet
    //   2. oldest non-critical reliable
    //   3. (only at 2× cap) oldest critical reliable, as a last resort
    // This protects the join/sync pipeline (PlayerAssignment, WorldStateFull,
    // HostEeprom, fragment chunks…) from being silently discarded under load.
    if (packet_send_queue_.size() >= MAX_SEND_QUEUE) {
        bool dropped = false;
        // Pass 1: drop oldest unreliable.
        for (auto it = packet_send_queue_.begin(); it != packet_send_queue_.end(); ++it) {
            if (!it->reliable) {
                packet_send_queue_.erase(it);
                dropped = true;
                break;
            }
        }
        // Pass 2: drop oldest non-critical reliable.
        if (!dropped) {
            for (auto it = packet_send_queue_.begin(); it != packet_send_queue_.end(); ++it) {
                if (it->data.empty()) continue;
                if (!is_critical_reliable(it->data[0])) {
                    packet_send_queue_.erase(it);
                    dropped = true;
                    break;
                }
            }
        }
        // Pass 3: only at 2× cap (catastrophic backlog), drop a critical too.
        if (!dropped && packet_send_queue_.size() >= MAX_SEND_QUEUE * 2) {
            BKNET_LOG(Warn, "Send queue overflow (%zu) — dropping oldest CRITICAL reliable",
                      packet_send_queue_.size());
            packet_send_queue_.pop_front();
            dropped = true;
        }
        if (dropped) {
            std::lock_guard<std::mutex> lock(stats_mutex_);
            stats_.packets_dropped_queue++;
        }
    }

    QueuedPacket qp;
    qp.data.assign(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size);
    qp.channel = channel;
    qp.reliable = reliable;
    qp.target_player = target_player;
    packet_send_queue_.push_back(std::move(qp));
}

// === Network health: keepalive, ping/pong, idle timeout, reconnection ===

void NetworkManager::tick_network_health() {
    if (!coopnet_) return;

    double now = get_time();

    // Send keepalives every 10s per peer.
    if (now - last_keepalive_send_ >= 10.0) {
        last_keepalive_send_ = now;
        if (is_connected()) {
            send_keepalive_to_all();
        }
    }

    // Ping every 5s for RTT sampling.
    if (now - last_ping_send_ >= 5.0) {
        last_ping_send_ = now;
        if (is_connected()) {
            send_ping_to_all();
        }
    }

    // Idle timeout check once per second. 30s without any inbound data =
    // consider the peer dead and drop it explicitly (otherwise libcoopnet
    // can hold a stale slot for several minutes).
    //
    // During the join grace window the peer is still resolving fragmented
    // WorldStateFull/HostEeprom and may have brief stalls under WAN loss
    // (NACK retries take ~15s to exhaust). Use a longer idle limit there so
    // a slow join doesn't get kicked while the recovery path is still working.
    if (now - last_idle_check_ >= 1.0) {
        last_idle_check_ = now;
        if (is_connected()) {
            for (uint8_t pid = 1; pid < MAX_PLAYERS; pid++) {
                if (pid == local_player_id_) continue;
                double idle = coopnet_->seconds_since_last_packet(pid);
                double first_seen = peer_first_seen_[pid];
                bool in_grace = first_seen > 0.0 &&
                                (now - first_seen) < JOIN_GRACE_SECONDS;
                double idle_limit = in_grace ? 60.0 : 30.0;
                if (idle > idle_limit) {
                    BKNET_LOG(Warn, "Player %u idle for %.1fs (limit=%.0fs%s) — dropping",
                              pid, idle, idle_limit, in_grace ? ", joining" : "");
                    {
                        std::lock_guard<std::mutex> lock(stats_mutex_);
                        stats_.peers_dropped_idle++;
                    }
                    coopnet_->drop_peer(pid, CoopNetError::IdleTimeout);
                }
            }
        }
    }

    // Fragment NACK + sender cache TTL.
    tick_fragment_maintenance(now);

    // Resend WorldStateFull to joiners that didn't ack within the timeout.
    if (is_host()) {
        tick_full_sync_retry(now);
    }

    // Connecting-state watchdog: a joiner who lost PlayerAssignment (or whose
    // P2P never converged) would otherwise sit in Connecting forever. After
    // CONNECTING_TIMEOUT seconds, drop to Disconnected with an error so the UI
    // can surface it instead of pretending the connect is still in flight.
    if (state_ == ConnectionState::Connecting && connecting_started_at_ > 0.0
        && now - connecting_started_at_ > CONNECTING_TIMEOUT) {
        BKNET_LOG(Warn, "Connecting watchdog fired after %.1fs — giving up",
                  now - connecting_started_at_);
        state_ = ConnectionState::Disconnected;
        connecting_started_at_ = 0.0;
        unexpected_disconnect_.store(true);
        if (typed_error_callback_) {
            typed_error_callback_(CoopNetError::PeerFailed,
                                  "Connecting timed out");
        }
        // Stop the auto-rejoin loop too — the user should choose to retry.
        reconnect_relobby_pending_ = false;
        reconnect_pending_ = false;
    }

    // Reconnection with exponential backoff. Triggered by signaling_disconnect_
    // callback setting reconnect_pending_.
    if (reconnect_pending_ && now >= next_reconnect_at_ && !coopnet_->is_connected()) {
        reconnect_attempts_++;
        {
            std::lock_guard<std::mutex> lock(stats_mutex_);
            stats_.reconnect_attempts++;
        }
        BKNET_LOG(Info, "Reconnect attempt %u to %s:%u",
                  reconnect_attempts_, reconnect_server_.c_str(), reconnect_port_);
        // Exponential backoff capped at 30s: 2,4,8,16,30,30...
        double delay = std::min(30.0, static_cast<double>(1u << std::min<uint32_t>(reconnect_attempts_, 5u)));
        next_reconnect_at_ = now + delay;
        // CoopNetTransport::begin() refuses if its static s_instance_ is still
        // pointing to us (from a previous session). shutdown() clears it and
        // zeroes the socket state; the callback registrations (stored in this
        // instance, not in s_instance_) survive across the cycle.
        coopnet_->shutdown();
        coopnet_->begin(reconnect_server_, reconnect_port_, get_config().player_name);
        // Clear pending once signaling re-opens — we poll that next tick.
        if (coopnet_->is_connected()) {
            reconnect_pending_ = false;
            reconnect_attempts_ = 0;
        }
    }

    // Once signaling is back up, re-enter the saved lobby (joiner only).
    // Without this, signaling recovery left the player stranded outside the
    // game even though network connectivity was restored.
    if (reconnect_relobby_pending_ && coopnet_ && coopnet_->is_connected()
        && reconnect_lobby_id_ != 0) {
        reconnect_relobby_pending_ = false;
        BKNET_LOG(Info, "Signaling restored — rejoining lobby %llu",
                  static_cast<unsigned long long>(reconnect_lobby_id_));
        coopnet_join_lobby(reconnect_lobby_id_, reconnect_lobby_password_);
    }
}

void NetworkManager::send_keepalive_to_all() {
    KeepAlivePacket pkt{};
    pkt.header.type = PacketType::KeepAlive;
    pkt.header.player_id = local_player_id_;
    pkt.header.sequence = 0; // keepalive is self-describing
    // Direct SDL-thread send — we're called from update() which already runs here.
    if (coopnet_) coopnet_->broadcast(&pkt, sizeof(pkt));
}

void NetworkManager::send_ping_to_all() {
    if (!coopnet_) return;
    PingPacket pkt{};
    pkt.header.type = PacketType::Ping;
    pkt.header.player_id = local_player_id_;
    pkt.header.sequence = 0;
    auto now = std::chrono::steady_clock::now();
    pkt.timestamp_us = std::chrono::duration_cast<std::chrono::microseconds>(
        now.time_since_epoch()).count();
    coopnet_->broadcast(&pkt, sizeof(pkt));
}

void NetworkManager::send_version_check(uint8_t player_id) {
    if (!coopnet_) return;
    VersionCheckPacket pkt{};
    pkt.header.type = PacketType::VersionCheck;
    pkt.header.player_id = local_player_id_;
    pkt.header.sequence = 0;
    pkt.protocol_version = PROTOCOL_VERSION;
    pkt.feature_flags = SUPPORTED_FEATURES;
    coopnet_->send_to(player_id, &pkt, sizeof(pkt));
}

void NetworkManager::handle_ping_packet(uint8_t from_player_id, const PingPacket& pkt) {
    if (!coopnet_ || from_player_id >= MAX_PLAYERS || from_player_id == local_player_id_) return;
    PongPacket pong{};
    pong.header.type = PacketType::Pong;
    pong.header.player_id = local_player_id_;
    pong.header.sequence = 0;
    pong.ping_timestamp_us = pkt.timestamp_us;
    auto now = std::chrono::steady_clock::now();
    pong.pong_timestamp_us = std::chrono::duration_cast<std::chrono::microseconds>(
        now.time_since_epoch()).count();
    coopnet_->send_to(from_player_id, &pong, sizeof(pong));
}

void NetworkManager::handle_pong_packet(uint8_t from_player_id, const PongPacket& pkt) {
    if (!coopnet_ || from_player_id >= MAX_PLAYERS) return;
    auto now = std::chrono::steady_clock::now();
    uint64_t now_us = std::chrono::duration_cast<std::chrono::microseconds>(
        now.time_since_epoch()).count();
    if (now_us <= pkt.ping_timestamp_us) return; // clock weirdness
    uint64_t rtt_us = now_us - pkt.ping_timestamp_us;
    uint32_t rtt_ms = static_cast<uint32_t>(rtt_us / 1000);

    // EMA smooth (alpha 0.3). Start from raw sample if no prior.
    uint32_t prev = coopnet_->get_peer_rtt_ms(from_player_id);
    uint32_t smoothed = prev == 0 ? rtt_ms
                                  : static_cast<uint32_t>(prev * 0.7 + rtt_ms * 0.3);
    coopnet_->set_peer_rtt_ms(from_player_id, smoothed);
}

void NetworkManager::handle_version_check(uint8_t from_player_id, const VersionCheckPacket& pkt) {
    if (!coopnet_ || from_player_id >= MAX_PLAYERS) return;
    if (pkt.protocol_version != PROTOCOL_VERSION) {
        BKNET_LOG(Error, "Version mismatch with player %u: got %u, expected %u — dropping",
                  from_player_id, pkt.protocol_version, PROTOCOL_VERSION);
        coopnet_->drop_peer(from_player_id, CoopNetError::ProtocolMismatch);
        return;
    }
    // Store negotiated feature intersection — downgrades gracefully if peer
    // lacks a feature we offer (e.g. old builds without FragmentNack).
    uint32_t common = pkt.feature_flags & SUPPORTED_FEATURES;
    peer_features_[from_player_id] = common;
    BKNET_LOG(Info, "Version check OK with player %u (v%u, features=0x%X common=0x%X)",
              from_player_id, pkt.protocol_version, pkt.feature_flags, common);
}

// === Sequence freshness for UDP reorder rejection ===

bool NetworkManager::sequence_is_fresh(uint8_t player_id, PacketType type, uint16_t seq) {
    // Only apply to high-frequency unreliable packets where reorder matters.
    // Reliable packets are already ordered by ENet/CoopNet per channel.
    if (type != PacketType::PlayerPosition &&
        type != PacketType::PlayerState &&
        type != PacketType::EnemyPositionBulk) {
        return true;
    }
    uint16_t key = (static_cast<uint16_t>(player_id) << 8) | static_cast<uint8_t>(type);
    auto it = last_seen_seq_.find(key);
    if (it == last_seen_seq_.end()) {
        last_seen_seq_[key] = seq;
        return true;
    }
    int16_t delta = static_cast<int16_t>(seq - it->second);
    if (delta <= 0) return false; // stale or duplicate
    it->second = seq;
    return true;
}

// === Duplicate-detection ring ===

bool NetworkManager::is_duplicate_packet(uint8_t from_player_id, uint16_t seq,
                                          const uint8_t* data, size_t size) {
    if (from_player_id >= MAX_PLAYERS) return false;
    // FNV-1a 32-bit over the full wire payload. Cheap, no collisions worth
    // worrying about within a 256-entry window.
    uint32_t h = 0x811c9dc5u;
    for (size_t i = 0; i < size; i++) {
        h ^= data[i];
        h *= 0x01000193u;
    }
    auto& ring = rx_dedup_[from_player_id];
    for (size_t i = 0; i < RX_DEDUP_RING; i++) {
        if (ring.seq[i] == seq && ring.hash[i] == h) {
            return true;
        }
    }
    ring.seq[ring.next_idx] = seq;
    ring.hash[ring.next_idx] = h;
    ring.next_idx = (ring.next_idx + 1) % RX_DEDUP_RING;
    return false;
}

// === zlib wrap/unwrap ===

std::vector<uint8_t> NetworkManager::wrap_compressed_for_peer(uint8_t target_player,
                                                              const void* data, size_t size) {
    // Skip wrap if peer doesn't negotiate compression, payload is too small to
    // be worth it, or target is BROADCAST (we'd need per-peer wrapping for
    // that; broadcasts here are typically small unreliable position updates).
    if (target_player >= MAX_PLAYERS) return {};
    if (size < COMPRESSION_THRESHOLD) return {};
    if ((peer_features_[target_player] & FEATURE_COMPRESSION) == 0) return {};

    // compressBound gives the worst-case output size; we then trim. Cap at
    // 256 KB so a malicious "decompression bomb" header from a peer can't
    // make us allocate forever — our packets stay well under this in practice.
    uLong bound = compressBound(static_cast<uLong>(size));
    if (bound > 256 * 1024) return {};

    std::vector<uint8_t> out(sizeof(CompressedPacketHeader) + bound);
    auto* hdr = reinterpret_cast<CompressedPacketHeader*>(out.data());
    hdr->header.type = PacketType::Compressed;
    hdr->header.player_id = local_player_id_;
    hdr->header.sequence = send_sequence_++;
    hdr->original_size = static_cast<uint32_t>(size);

    uLongf dest_len = bound;
    int rc = compress2(out.data() + sizeof(CompressedPacketHeader), &dest_len,
                       static_cast<const Bytef*>(data),
                       static_cast<uLong>(size),
                       Z_BEST_SPEED); // fast path; bandwidth, not CPU, is the limit
    if (rc != Z_OK) {
        BKNET_LOG(Warn, "zlib compress failed (rc=%d, size=%zu) — sending raw", rc, size);
        return {};
    }
    // If compression didn't actually save anything (already-compressed data,
    // tiny dirty deltas), send raw — adds 12B + zlib overhead otherwise.
    if (dest_len + sizeof(CompressedPacketHeader) >= size) {
        return {};
    }
    hdr->compressed_size = static_cast<uint32_t>(dest_len);
    out.resize(sizeof(CompressedPacketHeader) + dest_len);
    return out;
}

void NetworkManager::handle_compressed_packet(uint8_t from_player_id,
                                              const uint8_t* data, size_t size) {
    if (size < sizeof(CompressedPacketHeader)) {
        BKNET_LOG(Warn, "Compressed packet from p%u too small (%zu)", from_player_id, size);
        return;
    }
    CompressedPacketHeader hdr;
    std::memcpy(&hdr, data, sizeof(hdr));
    // Sanity caps on declared sizes — protects against malformed/hostile
    // headers that would otherwise allocate huge buffers or read OOB.
    if (hdr.original_size == 0 || hdr.original_size > 256 * 1024) return;
    if (hdr.compressed_size == 0 ||
        hdr.compressed_size > size - sizeof(CompressedPacketHeader)) return;

    std::vector<uint8_t> inflated(hdr.original_size);
    uLongf dest_len = hdr.original_size;
    int rc = uncompress(inflated.data(), &dest_len,
                        data + sizeof(CompressedPacketHeader),
                        static_cast<uLong>(hdr.compressed_size));
    if (rc != Z_OK || dest_len != hdr.original_size) {
        BKNET_LOG(Warn, "zlib uncompress failed (rc=%d, decoded=%lu/%u) from p%u",
                  rc, dest_len, hdr.original_size, from_player_id);
        return;
    }
    // Re-dispatch the inner packet. skip_accounting=true so we don't double-
    // count bytes (the outer wrapper already paid the BW window cost).
    handle_packet(from_player_id, inflated.data(), inflated.size(), /*skip_accounting=*/true);
}

// === Chunked fragment transport ===

void NetworkManager::send_chunked(uint8_t target_player, PacketType original_type,
                                   const void* data, size_t size) {
    // Try to compress the full payload BEFORE fragmenting so the chunk count
    // drops with the byte count. The receiver reassembles into the wrapped
    // CompressedPacket and handle_packet detects type=Compressed → unwrap →
    // re-dispatch the inner original_type. Big win for HostEeprom (2 KB) and
    // WorldStateFull (~600 B-1 KB) which compress 3-4× on flag bitfields.
    std::vector<uint8_t> compressed = wrap_compressed_for_peer(target_player, data, size);
    if (!compressed.empty()) {
        BKNET_LOG(Info, "Compressed chunked payload type=0x%02X: %zu → %zu bytes",
                  static_cast<unsigned>(original_type), size, compressed.size());
        data = compressed.data();
        size = compressed.size();
        original_type = PacketType::Compressed;
    }
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    // Atomic so future game-thread callers don't tear the counter. Skip 0
    // (treated as "unknown" in the reassembly maps).
    uint16_t group = next_fragment_group_id_.fetch_add(1, std::memory_order_relaxed);
    if (group == 0) group = next_fragment_group_id_.fetch_add(1, std::memory_order_relaxed);

    uint16_t chunk_count = static_cast<uint16_t>(
        (size + FRAGMENT_CHUNK_PAYLOAD - 1) / FRAGMENT_CHUNK_PAYLOAD);
    if (chunk_count == 0) chunk_count = 1;

    // Cache each chunk's wire bytes so a later FragmentNack can retransmit
    // individual pieces without re-serializing the original payload.
    SentFragmentGroup cache;
    cache.target_player = target_player;
    cache.chunk_count = chunk_count;
    cache.sent_at = get_time();
    cache.chunks.resize(chunk_count);

    for (uint16_t i = 0; i < chunk_count; i++) {
        size_t offset = i * FRAGMENT_CHUNK_PAYLOAD;
        size_t chunk_size = std::min<size_t>(FRAGMENT_CHUNK_PAYLOAD, size - offset);

        FragmentChunkPacket pkt{};
        pkt.header.type = PacketType::FragmentChunk;
        pkt.header.player_id = local_player_id_;
        pkt.header.sequence = send_sequence_++;
        pkt.group_id = group;
        pkt.chunk_index = i;
        pkt.chunk_count = chunk_count;
        pkt.chunk_size = static_cast<uint16_t>(chunk_size);
        pkt.original_type = static_cast<uint8_t>(original_type);
        pkt.total_size = static_cast<uint32_t>(size);
        std::memcpy(pkt.data, bytes + offset, chunk_size);

        // Only send the populated prefix — this drops ~1KB per chunk on the
        // wire for short trailing chunks.
        size_t wire_size = offsetof(FragmentChunkPacket, data) + chunk_size;

        // Cache the exact wire bytes for retransmit.
        cache.chunks[i].assign(reinterpret_cast<const uint8_t*>(&pkt),
                               reinterpret_cast<const uint8_t*>(&pkt) + wire_size);

        if (target_player == BROADCAST_TARGET) {
            enqueue_packet(&pkt, wire_size, CHANNEL_RELIABLE, true);
        } else {
            enqueue_packet_to(target_player, &pkt, wire_size, CHANNEL_RELIABLE, true);
        }
    }

    // Cap sender-side cache so a host blasting chunked payloads to multiple
    // joiners can't accumulate unbounded memory before the 30s TTL sweeps.
    // On overflow, evict the oldest group by sent_at timestamp.
    if (sent_fragments_.size() >= MAX_SENT_FRAGMENT_GROUPS) {
        auto oldest = sent_fragments_.begin();
        for (auto it = sent_fragments_.begin(); it != sent_fragments_.end(); ++it) {
            if (it->second.sent_at < oldest->second.sent_at) oldest = it;
        }
        BKNET_LOG(Warn, "sent_fragments_ cap hit — evicting group %u", oldest->first);
        sent_fragments_.erase(oldest);
    }
    sent_fragments_[group] = std::move(cache);
    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_.fragments_sent += chunk_count;
    }
    BKNET_LOG(Info, "Fragmented %zu-byte payload (type=0x%02X) into %u chunks group=%u",
              size, static_cast<unsigned>(original_type), chunk_count, group);
}

void NetworkManager::handle_fragment_nack(uint8_t from_player_id, const FragmentNackPacket& pkt) {
    auto it = sent_fragments_.find(pkt.group_id);
    if (it == sent_fragments_.end()) {
        // Cache expired or we never sent this group — nothing to do.
        BKNET_LOG(Warn, "NACK from p%u for unknown group %u", from_player_id, pkt.group_id);
        return;
    }
    if (pkt.chunk_count != it->second.chunk_count) return; // mismatched group

    uint32_t resent = 0;
    for (uint16_t i = 0; i < pkt.chunk_count && i < it->second.chunk_count; i++) {
        bool missing = (pkt.missing_bitmap[i / 8] >> (i % 8)) & 0x1;
        if (!missing) continue;
        const auto& bytes = it->second.chunks[i];
        if (bytes.empty()) continue;
        // Retransmit to the requesting peer specifically (even if original was
        // broadcast) — only that peer needs it.
        enqueue_packet_to(from_player_id, bytes.data(), bytes.size(),
                          CHANNEL_RELIABLE, true);
        resent++;
    }
    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_.fragments_retransmitted += resent;
    }
    BKNET_LOG(Info, "Retransmitted %u chunks to p%u for group %u",
              resent, from_player_id, pkt.group_id);
}

void NetworkManager::tick_fragment_maintenance(double now) {
    // Receiver side: emit NACKs for groups with missing chunks. The stall
    // threshold (how long to wait before assuming a chunk is lost) and the
    // throttle between NACKs both scale with the peer's measured RTT so we
    // don't NACK prematurely on WAN nor wait too long on LAN. Mirrors the
    // sm64coopdx adaptive-resend pattern (packet_reliable.c get_max_elapsed_time).
    for (auto& [key, asm_state] : fragment_assembly_) {
        if (asm_state.chunks_received == asm_state.chunk_count) continue;

        uint8_t sender = static_cast<uint8_t>(key >> 16);
        uint16_t group = static_cast<uint16_t>(key & 0xFFFF);

        // Compute adaptive intervals from RTT. coopnet_ may not be set in
        // pure-ENet mode (LAN) — fall back to 60ms (a typical LAN ping).
        double rtt_s = 0.06;
        if (coopnet_) {
            uint32_t rtt_ms = coopnet_->get_peer_rtt_ms(sender);
            if (rtt_ms > 0) rtt_s = rtt_ms / 1000.0;
        }
        // Stall threshold: 3× RTT, but at least 0.5s to absorb jitter and
        // never less than half the original 1s safety floor on a near-zero
        // RTT measurement.
        double stall_threshold = std::max(0.5, rtt_s * 3.0);
        // NACK throttle scales with attempt count so a peer that's truly slow
        // gets backed off (matches coopdx's attempts² behaviour, capped at 8s
        // so we still recover within the 30s sender-cache TTL).
        double nack_throttle = std::min(8.0,
            std::max(rtt_s * 2.0, 0.5) * (1.0 + asm_state.nack_attempts * 0.5));

        if (now - asm_state.last_chunk_at < stall_threshold) continue;
        if (now - asm_state.last_nack_at < nack_throttle) continue;
        if (asm_state.nack_attempts >= MAX_NACK_ATTEMPTS) continue; // give up gracefully

        // Only NACK if peer declares support for it.
        if ((peer_features_[sender] & FEATURE_FRAGMENT_NACK) == 0) continue;

        FragmentNackPacket nack{};
        nack.header.type = PacketType::FragmentNack;
        nack.header.player_id = local_player_id_;
        nack.header.sequence = 0;
        nack.group_id = group;
        nack.chunk_count = asm_state.chunk_count;
        // Pack missing-chunk bitmap; cap at bitmap capacity.
        size_t max_bits = FRAGMENT_NACK_BITMAP_BYTES * 8;
        for (uint16_t i = 0; i < asm_state.chunk_count && i < max_bits; i++) {
            if (!asm_state.received_mask[i]) {
                nack.missing_bitmap[i / 8] |= static_cast<uint8_t>(1u << (i % 8));
            }
        }
        enqueue_packet_to(sender, &nack, sizeof(nack), CHANNEL_RELIABLE, true);
        asm_state.last_nack_at = now;
        asm_state.nack_attempts++;
        {
            std::lock_guard<std::mutex> lock(stats_mutex_);
            stats_.fragments_nacked++;
        }
        BKNET_LOG(Info, "NACK → p%u group %u attempt %u/%u (%u/%u rcvd)",
                  sender, group, asm_state.nack_attempts, MAX_NACK_ATTEMPTS,
                  asm_state.chunks_received, asm_state.chunk_count);
        if (asm_state.nack_attempts == MAX_NACK_ATTEMPTS) {
            BKNET_LOG(Warn, "NACK retry cap hit for group %u from p%u — "
                      "waiting for 30s timeout", group, sender);
        }
    }

    // Sender side: purge cache entries older than 30s (assume delivered).
    for (auto it = sent_fragments_.begin(); it != sent_fragments_.end(); ) {
        if (now - it->second.sent_at > 30.0) {
            it = sent_fragments_.erase(it);
        } else {
            ++it;
        }
    }
}

void NetworkManager::handle_fragment_chunk(uint8_t from_player_id, const FragmentChunkPacket& pkt) {
    if (from_player_id >= MAX_PLAYERS) return;
    if (pkt.chunk_count == 0 || pkt.chunk_index >= pkt.chunk_count) return;
    if (pkt.chunk_size > FRAGMENT_CHUNK_PAYLOAD) return;
    if (pkt.total_size > 64 * 1024) return; // sanity cap: 64 KB per group

    double now = get_time();
    uint32_t key = (static_cast<uint32_t>(from_player_id) << 16) | pkt.group_id;
    auto& asm_state = fragment_assembly_[key];
    if (asm_state.chunk_count == 0) {
        asm_state.chunk_count = pkt.chunk_count;
        asm_state.total_size = pkt.total_size;
        asm_state.original_type = pkt.original_type;
        asm_state.started_at = now;
        asm_state.last_chunk_at = now;
        asm_state.received_mask.assign(pkt.chunk_count, false);
        asm_state.buffer.assign(pkt.total_size, 0);
    }
    if (pkt.chunk_count != asm_state.chunk_count ||
        pkt.total_size != asm_state.total_size) {
        BKNET_LOG(Warn, "Fragment mismatch from p%u group %u", from_player_id, pkt.group_id);
        fragment_assembly_.erase(key);
        return;
    }
    asm_state.last_chunk_at = now;
    if (asm_state.received_mask[pkt.chunk_index]) return; // duplicate chunk

    size_t offset = static_cast<size_t>(pkt.chunk_index) * FRAGMENT_CHUNK_PAYLOAD;
    if (offset + pkt.chunk_size > asm_state.total_size) return;
    std::memcpy(asm_state.buffer.data() + offset, pkt.data, pkt.chunk_size);
    asm_state.received_mask[pkt.chunk_index] = true;
    asm_state.chunks_received++;

    if (asm_state.chunks_received == asm_state.chunk_count) {
        BKNET_LOG(Info, "Reassembled %u-byte payload (type=0x%02X) from p%u",
                  asm_state.total_size, asm_state.original_type, from_player_id);
        {
            std::lock_guard<std::mutex> lock(stats_mutex_);
            stats_.fragments_completed++;
        }
        // Dispatch the reassembled packet through the normal handler. Skip
        // accounting — the wire chunks were already counted against BW/stats;
        // re-counting the reassembled payload would inflate both.
        std::vector<uint8_t> buf = std::move(asm_state.buffer);
        fragment_assembly_.erase(key);
        handle_packet(from_player_id, buf.data(), buf.size(), /*skip_accounting=*/true);
    }

    // Age out stale in-flight reassemblies (>30s) to avoid leaks.
    for (auto it = fragment_assembly_.begin(); it != fragment_assembly_.end(); ) {
        if (now - it->second.started_at > 30.0) {
            BKNET_LOG(Warn, "Discarding stale fragment group (%u/%u chunks rcvd)",
                      it->second.chunks_received, it->second.chunk_count);
            {
                std::lock_guard<std::mutex> lock(stats_mutex_);
                stats_.fragments_timed_out++;
            }
            it = fragment_assembly_.erase(it);
        } else {
            ++it;
        }
    }
}

// === World ownership ===

void NetworkManager::update_player_level(uint8_t player_id, uint32_t level_id) {
    if (!is_connected() || player_id >= MAX_PLAYERS) return;

    uint32_t old_level;
    bool needs_release = false;
    bool needs_assign = false;

    {
        std::lock_guard<std::mutex> lock(ownership_mutex_);
        old_level = player_levels_[player_id];
        if (old_level == level_id) return; // No change
        player_levels_[player_id] = level_id;

        if (!is_host()) return;

        // Check what ownership actions are needed (but don't call them under lock)
        if (old_level != 0xFFFFFFFF) {
            auto it = world_owner_.find(old_level);
            needs_release = (it != world_owner_.end() && it->second == player_id);
        }
        if (level_id != 0xFFFFFFFF) {
            needs_assign = (world_owner_.find(level_id) == world_owner_.end());
        }
    }
    // Now call outside the lock to avoid deadlock

    if (needs_release) {
        release_world_owner(old_level, player_id);
    }
    if (needs_assign) {
        assign_world_owner(level_id, player_id);
    }

    // When a player enters a level, re-sync state:
    if (is_host() && level_id != 0xFFFFFFFF) {
        // For remote players: send collectible scores
        if (player_id != local_player_id_) {
            request_full_sync(player_id);
            std::printf("[Ownership] Requested full sync for player %u entering level %u\n",
                player_id, level_id);
        }

        // Send centralized kill + collectible lists for this level
        send_kill_list_for_level(level_id);

        // Queue collectible resync
        {
            std::lock_guard<std::mutex> lock(send_queue_mutex_);
            collectible_sync_queue_.push_back(level_id);
        }
    }
}

void NetworkManager::assign_world_owner(uint32_t level_id, uint8_t player_id) {
    {
        std::lock_guard<std::mutex> lock(ownership_mutex_);
        world_owner_[level_id] = player_id;
    }

    std::printf("[Ownership] Player %u is now owner of level %u\n", player_id, level_id);

    WorldOwnershipPacket pkt{};
    pkt.header.type = PacketType::WorldOwnership;
    pkt.header.player_id = 0;
    pkt.header.sequence = 0;
    pkt.level_id = level_id;
    pkt.owner_player_id = player_id;

    enqueue_packet(&pkt, sizeof(pkt), CHANNEL_RELIABLE, true);
}

void NetworkManager::release_world_owner(uint32_t level_id, uint8_t leaving_player_id) {
    // Determine replacement + mutate world_owner_ atomically to avoid TOCTOU:
    // a concurrent update_player_level() could otherwise change player_levels_[]
    // between the check and the find-replacement loop.
    uint8_t new_owner = 0xFF;
    bool no_one_left = false;
    {
        std::lock_guard<std::mutex> lock(ownership_mutex_);
        auto it = world_owner_.find(level_id);
        if (it == world_owner_.end()) return;
        if (it->second != leaving_player_id) return;

        for (uint8_t i = 0; i < MAX_PLAYERS; i++) {
            if (i == leaving_player_id) continue;
            if (player_levels_[i] == level_id) {
                new_owner = i;
                break;
            }
        }

        if (new_owner != 0xFF) {
            world_owner_[level_id] = new_owner;
        } else {
            world_owner_.erase(level_id);
            no_one_left = true;
        }
    }

    // Broadcast outside the lock (enqueue_packet takes send_queue_mutex_).
    WorldOwnershipPacket pkt{};
    pkt.header.type = PacketType::WorldOwnership;
    pkt.header.player_id = 0;
    pkt.header.sequence = 0;
    pkt.level_id = level_id;
    pkt.owner_player_id = (new_owner != 0xFF) ? new_owner : 0xFF;
    enqueue_packet(&pkt, sizeof(pkt), CHANNEL_RELIABLE, true);

    if (new_owner != 0xFF) {
        std::printf("[Ownership] Transferred level %u from player %u to player %u\n",
            level_id, leaving_player_id, new_owner);
        // Re-send kill list so the new owner's local killed_on_map[] tracking
        // stays consistent with the centralized registry (fix for bug #4:
        // pending kills orphaned when owner disconnects).
        send_kill_list_for_level(level_id);
    } else if (no_one_left) {
        clear_level_kills(level_id);
        std::printf("[Ownership] Level %u has no players — state reset\n", level_id);
    }
}

bool NetworkManager::am_i_world_owner(uint32_t level_id) const {
    std::lock_guard<std::mutex> lock(ownership_mutex_);
    auto it = world_owner_.find(level_id);
    if (it == world_owner_.end()) return false;
    return it->second == local_player_id_;
}

uint8_t NetworkManager::get_world_owner(uint32_t level_id) const {
    std::lock_guard<std::mutex> lock(ownership_mutex_);
    auto it = world_owner_.find(level_id);
    if (it == world_owner_.end()) return 0xFF;
    return it->second;
}

void NetworkManager::handle_ownership_packet(const WorldOwnershipPacket& pkt) {
    std::lock_guard<std::mutex> lock(ownership_mutex_);
    if (pkt.owner_player_id == 0xFF) {
        world_owner_.erase(pkt.level_id);
        std::printf("[Ownership] Level %u owner cleared\n", pkt.level_id);
    } else {
        world_owner_[pkt.level_id] = pkt.owner_player_id;
        std::printf("[Ownership] Level %u owner set to player %u\n", pkt.level_id, pkt.owner_player_id);
    }
}


// === Centralized kill tracking ===

void NetworkManager::record_kill(uint32_t level_id, uint16_t marker_type, uint16_t spawn_index, uint32_t map_id) {
    std::lock_guard<std::mutex> lock(kill_mutex_);
    auto& kills = level_kills_[level_id];

    // Dedup
    for (const auto& k : kills) {
        if (k.marker_type == marker_type && k.spawn_index == spawn_index) return;
    }
    kills.push_back({marker_type, spawn_index, map_id});
    std::printf("[KillTrack] Recorded kill: level=%u marker=0x%X spawn=%u map=%u (total=%zu)\n",
        level_id, marker_type, spawn_index, map_id, kills.size());
}

void NetworkManager::clear_level_kills(uint32_t level_id) {
    std::lock_guard<std::mutex> lock(kill_mutex_);
    level_kills_.erase(level_id);
    std::printf("[KillTrack] Cleared kills for level %u\n", level_id);
}

void NetworkManager::send_kill_list_for_level(uint32_t level_id) {
    // Queue the level_id for the SDL thread to send kill list
    // (game thread cannot call ENet directly)
    std::lock_guard<std::mutex> lock(send_queue_mutex_);
    kill_sync_queue_.push_back(level_id);
    std::printf("[KillTrack] Queued kill sync for level %u\n", level_id);
}

// === Centralized collectible tracking ===

void NetworkManager::record_collectible(uint32_t level_id, uint8_t type, uint16_t id, uint32_t map_id) {
    std::lock_guard<std::mutex> lock(kill_mutex_); // reuse same mutex
    auto& colls = level_collectibles_[level_id];

    // Dedup by type+id
    for (const auto& c : colls) {
        if (c.type == type && c.id == id) return;
    }
    colls.push_back({type, id, map_id});
    std::printf("[CollTrack] Recorded: level=%u type=%u id=%u map=%u (total=%zu)\n",
        level_id, type, id, map_id, colls.size());
}

void NetworkManager::clear_level_collectibles(uint32_t level_id) {
    std::lock_guard<std::mutex> lock(kill_mutex_);
    level_collectibles_.erase(level_id);
}

// === Player roster ===

void NetworkManager::set_player_name(uint8_t player_id, const std::string& name) {
    if (player_id >= MAX_PLAYERS) return;
    std::lock_guard<std::mutex> lock(roster_mutex_);
    player_roster_[player_id].name = name;
    player_roster_[player_id].connected = true;
    std::printf("[Roster] Player %u name set to '%s'\n", player_id, name.c_str());
}

NetworkManager::PlayerInfo NetworkManager::get_player_info(uint8_t player_id) const {
    if (player_id >= MAX_PLAYERS) return {};
    std::lock_guard<std::mutex> lock(roster_mutex_);
    return player_roster_[player_id];
}

void NetworkManager::clear_player_roster() {
    std::lock_guard<std::mutex> lock(roster_mutex_);
    for (uint8_t i = 0; i < MAX_PLAYERS; i++) {
        player_roster_[i] = {};
    }
}

void NetworkManager::register_remote_player_locally(uint8_t player_id) {
    if (player_id >= MAX_PLAYERS || player_id == local_player_id_) return;

    std::string placeholder = banjo::locale::tr_format("chat.player_placeholder", "n",
        std::to_string(player_id + 1));

    bool was_connected = false;
    {
        std::lock_guard<std::mutex> lock(roster_mutex_);
        was_connected = player_roster_[player_id].connected;
        if (!was_connected) {
            player_roster_[player_id].name = placeholder;
            player_roster_[player_id].connected = true;
            // announced stays false — handle_packet emits "X joined" in chat
            // when broadcast_local_name() lands with the real display name.
        }
    }

    if (was_connected) return;

    // No add_system_message here: emitting with the placeholder ("Jugador 2
    // joined") and never editing the chat history would freeze the wrong
    // name in the log. The handle_packet PlayerJoin path announces once the
    // real name arrives.
    std::printf("[Roster] Pre-registered player %u (placeholder name)\n", player_id);
}

void NetworkManager::broadcast_local_name() {
    if (!is_connected()) return;

    const auto& config = get_config();
    PlayerJoinPacket pkt{};
    pkt.header.type = PacketType::PlayerJoin;
    pkt.header.player_id = local_player_id_;
    pkt.header.sequence = send_sequence_++;
    std::strncpy(pkt.player_name, config.player_name.c_str(), 31);
    pkt.player_name[31] = '\0';

    // CoopNet joiner: unicast to host first (always mapped after assignment) so
    // the host roster updates even if mesh PeerSend is still partial; mesh
    // broadcast still runs (PeerSend now succeeds if any peer accepts).
    if (coopnet_ && !is_host()) {
        enqueue_packet_to(0, &pkt, sizeof(pkt), CHANNEL_RELIABLE, true);
    }

    enqueue_packet(&pkt, sizeof(pkt), CHANNEL_RELIABLE, true);
    std::printf("[Roster] Broadcast name '%s' as player %u\n", config.player_name.c_str(), local_player_id_);
}

} // namespace bknet
