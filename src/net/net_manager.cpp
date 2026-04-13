#include "net_manager.h"
#include <cstdio>
#include <cstring>
#include <chrono>
#include <algorithm>

namespace bknet {

NetworkManager& NetworkManager::instance() {
    static NetworkManager s_instance;
    return s_instance;
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
        request_full_sync(player_id);
    });

    server_->set_disconnect_callback([this](uint8_t player_id) {
        std::printf("[Network] Player %u left the game\n", player_id);
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
        std::printf("[Network] Lost connection to server\n");
    });

    state_ = ConnectionState::Connecting;

    if (!client_->connect(config.join_ip, config.port)) {
        client_.reset();
        state_ = ConnectionState::Disconnected;
        return false;
    }

    local_player_id_ = client_->assigned_player_id();
    state_ = ConnectionState::Connected;
    interpolation_.reset();

    std::printf("[Network] Joined game as player %u\n", local_player_id_);
    return true;
}

void NetworkManager::disconnect() {
    if (server_) {
        server_->stop();
        server_.reset();
    }
    if (client_) {
        client_->disconnect();
        client_.reset();
    }

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
}

void NetworkManager::update() {
    if (!is_connected()) return;

    // Pump ENet events
    if (server_) server_->update();
    if (client_) client_->update();

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
                if (server_) {
                    server_->broadcast(&pkt, sizeof(pkt), CHANNEL_RELIABLE, true);
                } else if (client_) {
                    client_->send(&pkt, sizeof(pkt), CHANNEL_RELIABLE, true);
                }

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
                if (server_) {
                    server_->broadcast(&pkt, sizeof(pkt), CHANNEL_RELIABLE, true);
                } else if (client_) {
                    client_->send(&pkt, sizeof(pkt), CHANNEL_RELIABLE, true);
                }

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

        // Flush regular queued packets
        while (!packet_send_queue_.empty()) {
            auto& qp = packet_send_queue_.front();
            if (server_) {
                server_->broadcast(qp.data.data(), qp.data.size(), qp.channel, qp.reliable);
            } else if (client_) {
                client_->send(qp.data.data(), qp.data.size(), qp.channel, qp.reliable);
            }
            packet_send_queue_.pop_front();
        }
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
    if (server_) return server_->client_count() + 1;
    if (client_ && client_->is_connected()) return 2;
    return 1;
}

void NetworkManager::send_local_state() {
    // Send full state packet (Phase 2)
    PlayerStatePacket pkt{};
    if (!state_sync_.build_state_packet(local_player_id_, send_sequence_++, pkt)) {
        return;
    }

    if (server_) {
        server_->broadcast(&pkt, sizeof(pkt), CHANNEL_UNRELIABLE, false);
    } else if (client_) {
        client_->send(&pkt, sizeof(pkt), CHANNEL_UNRELIABLE, false);
    }
}

void NetworkManager::handle_packet(uint8_t from_player_id, const uint8_t* data, size_t size) {
    if (size < sizeof(PacketHeader)) return;

    PacketType type = peek_type(data, size);

    switch (type) {
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
                std::printf("[Network] Player %u joined\n", pkt.header.player_id);
            }
            break;
        }
        case PacketType::PlayerLeave: {
            PlayerLeavePacket pkt;
            if (deserialize(data, size, pkt)) {
                interpolation_.remove_player(pkt.header.player_id);
                std::printf("[Network] Player %u left\n", pkt.header.player_id);
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
        // WorldOwnerTransfer and WorldKillResync — no longer used
        // (centralized kill tracking in C++ replaces these)
        default:
            break;
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

    PositionSnapshot snap;
    snap.x = pkt.x;
    snap.y = pkt.y;
    snap.z = pkt.z;
    snap.yaw = pkt.yaw;
    snap.pitch = pkt.pitch;
    snap.scale = 1.0f;
    snap.map_id = pkt.map_id;
    snap.level_id = pkt.level_id;
    snap.animation_id = pkt.animation_id;
    snap.anim_timer = pkt.anim_progress;
    snap.anim_duration = pkt.anim_duration;
    snap.anim_subrange_start = pkt.anim_subrange_start;
    snap.anim_subrange_end = pkt.anim_subrange_end;
    snap.anim_playback_type = pkt.anim_playback_type;
    snap.kazooie_flags = pkt.kazooie_flags;
    snap.health = pkt.health;
    snap.health_total = pkt.health_total;
    snap.transformation = pkt.transformation;
    snap.bs_state = static_cast<uint8_t>(pkt.bs_state);
    snap.horizontal_velocity = pkt.horizontal_velocity;

    interpolation_.push_full_state(pid, snap);
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
    std::string msg(pkt.message, pkt.msg_length);

    {
        std::lock_guard<std::mutex> lock(chat_mutex_);
        chat_history_.push_back({pid, msg, get_time()});
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

void NetworkManager::send_enemy_death(uint16_t marker_type, uint16_t spawn_index, uint32_t map_id, float px, float py, float pz) {
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
    if (!is_connected() || !server_) return;

    // Enqueue for SDL thread (game thread cannot call ENet directly)
    enqueue_packet(data, size, CHANNEL_RELIABLE, true);
    std::printf("[Network] Queued WorldStateFull (%zu bytes) for player %u\n", size, target_player);
}

void NetworkManager::handle_world_state_full_packet(const WorldStateFullPacket& pkt) {
    if (is_host()) return;
    std::lock_guard<std::mutex> lock(world_mutex_);
    full_state_queue_.push_back(pkt);
    std::printf("[Network] Received WorldStateFull from host\n");
}

bool NetworkManager::pop_full_state(WorldStateFullPacket& out) {
    std::lock_guard<std::mutex> lock(world_mutex_);
    if (full_state_queue_.empty()) return false;
    out = full_state_queue_.front();
    full_state_queue_.pop_front();
    return true;
}

// === Thread-safe packet sending ===

void NetworkManager::enqueue_packet(const void* data, size_t size, uint8_t channel, bool reliable) {
    std::lock_guard<std::mutex> lock(send_queue_mutex_);
    QueuedPacket qp;
    qp.data.assign(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size);
    qp.channel = channel;
    qp.reliable = reliable;
    packet_send_queue_.push_back(std::move(qp));
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
    uint8_t current_owner;
    {
        std::lock_guard<std::mutex> lock(ownership_mutex_);
        auto it = world_owner_.find(level_id);
        if (it == world_owner_.end()) return;
        current_owner = it->second;
        if (current_owner != leaving_player_id) return; // Not the owner, nothing to do
    }

    // Find another player on the same level
    uint8_t new_owner = 0xFF;
    {
        std::lock_guard<std::mutex> lock(ownership_mutex_);
        for (uint8_t i = 0; i < MAX_PLAYERS; i++) {
            if (i == leaving_player_id) continue;
            if (player_levels_[i] == level_id) {
                new_owner = i;
                break;
            }
        }
    }

    if (new_owner != 0xFF) {
        // Transfer ownership
        assign_world_owner(level_id, new_owner);
        std::printf("[Ownership] Transferred level %u from player %u to player %u\n",
            level_id, leaving_player_id, new_owner);
    } else {
        // No one left — remove ownership (world state resets)
        {
            std::lock_guard<std::mutex> lock(ownership_mutex_);
            world_owner_.erase(level_id);
        }

        WorldOwnershipPacket pkt{};
        pkt.header.type = PacketType::WorldOwnership;
        pkt.header.player_id = 0;
        pkt.header.sequence = 0;
        pkt.level_id = level_id;
        pkt.owner_player_id = 0xFF; // No owner

        enqueue_packet(&pkt, sizeof(pkt), CHANNEL_RELIABLE, true);

        // Clear centralized kill tracking (enemies will respawn on re-entry)
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

} // namespace bknet
