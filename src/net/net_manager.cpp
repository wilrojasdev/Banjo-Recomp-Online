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
}

void NetworkManager::update() {
    if (!is_connected()) return;

    // Pump ENet events
    if (server_) server_->update();
    if (client_) client_->update();

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

    PositionSnapshot snap;
    snap.x = pkt.x;
    snap.y = pkt.y;
    snap.z = pkt.z;
    snap.yaw = pkt.yaw;
    snap.pitch = pkt.pitch;
    snap.scale = 1.0f;
    snap.map_id = pkt.map_id;
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
    if (server_) {
        server_->broadcast(&pkt, send_size, CHANNEL_RELIABLE, true);
    } else if (client_) {
        client_->send(&pkt, send_size, CHANNEL_RELIABLE, true);
    }

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

    if (server_) {
        server_->broadcast(&pkt, sizeof(pkt), CHANNEL_RELIABLE, true);
    } else if (client_) {
        client_->send(&pkt, sizeof(pkt), CHANNEL_RELIABLE, true);
    }
}

void NetworkManager::send_enemy_death(uint16_t marker_type, uint16_t spawn_index, uint32_t map_id, float px, float py, float pz) {
    if (!is_connected()) return;

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

    if (server_) {
        server_->broadcast(&pkt, sizeof(pkt), CHANNEL_RELIABLE, true);
    } else if (client_) {
        client_->send(&pkt, sizeof(pkt), CHANNEL_RELIABLE, true);
    }
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

    if (server_) {
        server_->broadcast(&pkt, sizeof(pkt), CHANNEL_RELIABLE, true);
    } else if (client_) {
        client_->send(&pkt, sizeof(pkt), CHANNEL_RELIABLE, true);
    }
}

void NetworkManager::handle_collectible_packet(const WorldCollectiblePacket& pkt) {
    if (pkt.header.player_id == local_player_id_) return;
    std::lock_guard<std::mutex> lock(world_mutex_);
    WorldEvent evt{};
    evt.type = WorldEvent::COLLECTIBLE;
    evt.collectible = pkt;
    world_events_.push_back(evt);
}

void NetworkManager::handle_enemy_packet(const WorldEnemyPacket& pkt) {
    if (pkt.header.player_id == local_player_id_) return;
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

    if (server_) {
        server_->broadcast(&pkt, payload_size, CHANNEL_UNRELIABLE, false);
    } else if (client_) {
        client_->send(&pkt, payload_size, CHANNEL_UNRELIABLE, false);
    }
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

    // Send to specific player only
    server_->send_to(target_player, data, size, CHANNEL_RELIABLE, true);
    std::printf("[Network] Sent WorldStateFull (%zu bytes) to player %u\n", size, target_player);
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

} // namespace bknet
