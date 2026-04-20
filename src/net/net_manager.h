#ifndef NET_MANAGER_H
#define NET_MANAGER_H

#include <memory>
#include <cstdint>
#include <functional>
#include <string>
#include <atomic>
#include <deque>
#include <mutex>
#include <unordered_map>

#include "net_packets.h"
#include "net_server.h"
#include "net_client.h"
#include "net_coopnet.h"
#include "net_state_sync.h"
#include "net_interpolation.h"
#include "net_config.h"

namespace bknet {

enum class ConnectionState {
    Disconnected,
    Hosting,
    Connecting,
    Connected,
};

class NetworkManager {
public:
    static NetworkManager& instance();

    bool initialize();
    void shutdown();

    bool host_game();
    bool join_game();
    void disconnect();

    // CoopNet lobby operations
    bool coopnet_begin(const std::string& server, uint16_t port);
    bool coopnet_host_lobby(const std::string& password = "", const std::string& description = "");
    bool coopnet_join_lobby(uint64_t lobby_id, const std::string& password = "");
    void coopnet_leave_lobby();
    void coopnet_request_lobby_list();
    void set_lobby_list_callback(CoopNetTransport::LobbyListCallback cb);
    void set_lobby_created_callback(CoopNetTransport::LobbyCreatedCallback cb);
    void set_coopnet_error_callback(CoopNetTransport::ErrorCallback cb);
    bool is_coopnet_mode() const { return coopnet_ != nullptr; }
    bool is_coopnet_signaling_connected() const { return coopnet_ && coopnet_->is_connected(); }

    // Called every frame from update_gfx
    void update();

    // Called from game thread (via patch) to push local player state
    void push_local_state(float x, float y, float z, float yaw, uint32_t map_id);
    void push_local_full_state(const LocalPlayerSnapshot& snap);

    // Get interpolated remote player state (for ghost rendering)
    InterpolatedState get_remote_player(uint8_t player_id) const;

    // Chat
    void send_chat(const std::string& message);
    void add_system_message(const std::string& message); // Local-only system message (join/leave)

    // World state sync (Phase 3)
    void send_collectible(uint8_t type, uint16_t id, uint8_t collected, uint32_t map_id, uint8_t level_id);
    void send_enemy_death(uint16_t marker_type, uint16_t spawn_index, uint32_t map_id, float px, float py, float pz);
    void send_flag_change(uint8_t flag_type, uint16_t flag_index, uint8_t value, uint32_t map_id);

    // Enemy position sync (host-authoritative)
    void send_enemy_positions(const EnemyPositionEntry* entries, uint8_t count, uint32_t map_id);
    size_t get_enemy_positions(EnemyInterpolatedState* out, size_t max_count) const;

    // Full state sync on join
    void request_full_sync(uint8_t player_id);
    bool should_send_full_sync(uint8_t& out_player_id);
    void send_world_state_full(const uint8_t* data, size_t size, uint8_t target_player);

    // World ownership
    void set_local_level_id(uint32_t level_id);
    void update_player_level(uint8_t player_id, uint32_t level_id);
    bool am_i_world_owner(uint32_t level_id) const;
    uint8_t get_world_owner(uint32_t level_id) const;

    // Centralized kill tracking (HOST is single source of truth)
    struct KillRecord {
        uint16_t marker_type;
        uint16_t spawn_index;
        uint32_t map_id;
    };
    void record_kill(uint32_t level_id, uint16_t marker_type, uint16_t spawn_index, uint32_t map_id);
    void clear_level_kills(uint32_t level_id);

    // Centralized collectible tracking (HOST is single source of truth)
    struct CollectibleRecord {
        uint8_t type;        // COLLECTIBLE_JIGGY, _JINJO, _MUMBO_TOKEN, etc.
        uint16_t id;         // jiggy_id, jinjo bitmask, token_id, etc.
        uint32_t map_id;     // map where it was collected
    };
    void record_collectible(uint32_t level_id, uint8_t type, uint16_t id, uint32_t map_id);
    void clear_level_collectibles(uint32_t level_id);

    // World event queue (received from network, consumed by game thread)
    struct WorldEvent {
        enum Type : uint8_t { COLLECTIBLE, ENEMY, FLAG } type;
        union {
            WorldCollectiblePacket collectible;
            WorldEnemyPacket enemy;
            WorldFlagPacket flag;
        };
    };
    bool pop_world_event(WorldEvent& out);  // Called from game thread
    bool pop_full_state(WorldStateFullPacket& out);  // Called from game thread

    // Host EEPROM snapshot (shipped to each joiner on connect).
    // Host: MIPS polls should_send_host_eeprom() to read hardware EEPROM and
    //       call send_host_eeprom().
    // Join: MIPS polls pop_host_eeprom() to fill the override buffer.
    void request_host_eeprom_send(uint8_t player_id);
    bool should_send_host_eeprom(uint8_t& out_player_id);
    void send_host_eeprom(const uint8_t* eeprom_bytes, size_t size, uint8_t target_player, int16_t current_slot);
    bool pop_host_eeprom(HostEepromPacket& out);

    // Conga orange projectile sync (world owner spawns, broadcasts to others)
    struct CongaOrangeEvent {
        float spawn_x, spawn_y, spawn_z;
        float vel_x, vel_y, vel_z;
    };
    void send_conga_orange(float sx, float sy, float sz, float vx, float vy, float vz, uint32_t map_id);
    bool pop_conga_orange(CongaOrangeEvent& out);

    struct ChatEntry {
        uint8_t player_id;
        std::string message;
        double timestamp; // seconds since start
    };
    static constexpr size_t MAX_CHAT_HISTORY = 20;
    static constexpr double CHAT_DISPLAY_DURATION = 8.0; // seconds to show messages

    std::deque<ChatEntry> get_chat_messages() const;      // recent only (< CHAT_DISPLAY_DURATION)
    std::deque<ChatEntry> get_all_chat_messages() const;  // full history
    bool has_new_messages() const;
    void clear_new_message_flag();
    double get_chat_time() const { return get_time(); }

    // Disconnect detection (set by client disconnect callback, polled by UI thread)
    bool was_unexpectedly_disconnected() const { return unexpected_disconnect_.load(); }
    void clear_disconnect_flag() { unexpected_disconnect_.store(false); }

    // Player roster (names)
    struct PlayerInfo {
        std::string name;
        bool connected = false;
    };
    void set_player_name(uint8_t player_id, const std::string& name);
    PlayerInfo get_player_info(uint8_t player_id) const;
    void clear_player_roster();
    void broadcast_local_name();  // Send PlayerJoinPacket with our name

    // Accessors
    bool is_connected() const { return state_ == ConnectionState::Hosting || state_ == ConnectionState::Connected; }
    bool is_host() const { return state_ == ConnectionState::Hosting || (coopnet_ && coopnet_->is_lobby_host()); }
    ConnectionState get_state() const { return state_; }
    uint8_t local_player_id() const { return local_player_id_; }
    uint8_t player_count() const;

private:
    NetworkManager() = default;
    ~NetworkManager() = default;
    NetworkManager(const NetworkManager&) = delete;
    NetworkManager& operator=(const NetworkManager&) = delete;

    void send_local_state();
    void handle_packet(uint8_t from_player_id, const uint8_t* data, size_t size);

    // Unified send helpers (dispatch to ENet or CoopNet)
    void net_broadcast(const void* data, size_t size, uint8_t channel, bool reliable);
    void net_send_to(uint8_t player_id, const void* data, size_t size, uint8_t channel, bool reliable);
    void handle_position_packet(const PlayerPositionPacket& pkt);
    void handle_state_packet(const PlayerStatePacket& pkt);
    void handle_map_change_packet(const MapChangePacket& pkt);
    void handle_chat_packet(const ChatMessagePacket& pkt);
    void handle_collectible_packet(const WorldCollectiblePacket& pkt);
    void handle_enemy_packet(const WorldEnemyPacket& pkt);
    void handle_enemy_position_packet(const uint8_t* data, size_t size);
    void handle_flag_packet(const WorldFlagPacket& pkt);
    void handle_world_state_full_packet(const WorldStateFullPacket& pkt);
    void handle_host_eeprom_packet(const HostEepromPacket& pkt);
    void handle_ownership_packet(const WorldOwnershipPacket& pkt);
    void handle_conga_orange_packet(const CongaOrangeSpawnPacket& pkt);
    void assign_world_owner(uint32_t level_id, uint8_t player_id);
    void release_world_owner(uint32_t level_id, uint8_t leaving_player_id);
    void send_kill_list_for_level(uint32_t level_id);

    std::unique_ptr<Server> server_;
    std::unique_ptr<Client> client_;
    std::unique_ptr<CoopNetTransport> coopnet_;

    // CoopNet callbacks stored for UI
    CoopNetTransport::LobbyListCallback lobby_list_callback_;
    CoopNetTransport::LobbyCreatedCallback lobby_created_callback_;
    CoopNetTransport::ErrorCallback coopnet_error_callback_;

    StateSync state_sync_;
    InterpolationManager interpolation_;

    ConnectionState state_ = ConnectionState::Disconnected;
    uint8_t local_player_id_ = 0;

    uint16_t send_sequence_ = 0;
    uint32_t frame_counter_ = 0;
    static constexpr uint32_t SEND_INTERVAL_FRAMES = 3; // ~20Hz at 60fps

    bool initialized_ = false;
    std::atomic<bool> unexpected_disconnect_{false};
    std::atomic<bool> initial_sync_done_{false}; // false during roster sync phase after join

    // Chat state
    mutable std::mutex chat_mutex_;
    std::deque<ChatEntry> chat_history_;
    bool new_messages_ = false;

    // World event queue (network thread pushes, game thread pops)
    mutable std::mutex world_mutex_;
    std::deque<WorldEvent> world_events_;

    // Enemy position interpolation (host→join)
    EnemyInterpolationManager enemy_interp_;

    // Full state sync on join
    std::atomic<bool> pending_full_sync_{false};
    uint8_t sync_target_player_ = 0;
    std::deque<WorldStateFullPacket> full_state_queue_;

    // Host EEPROM snapshot (SM64 Coop DX-style save override)
    std::atomic<bool> pending_host_eeprom_{false};
    uint8_t host_eeprom_target_player_ = 0;
    std::deque<HostEepromPacket> host_eeprom_queue_;
    std::deque<CongaOrangeEvent> conga_orange_queue_;
    std::chrono::steady_clock::time_point start_time_ = std::chrono::steady_clock::now();
    double get_time() const;

    // World ownership: level_id -> owner player_id
    mutable std::mutex ownership_mutex_;
    std::unordered_map<uint32_t, uint8_t> world_owner_;
    uint32_t player_levels_[MAX_PLAYERS] = {0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF};
    // Centralized state tracking (HOST authoritative)
    mutable std::mutex kill_mutex_;
    std::unordered_map<uint32_t, std::vector<KillRecord>> level_kills_;
    std::unordered_map<uint32_t, std::vector<CollectibleRecord>> level_collectibles_;
    // Queue of level_ids that need state lists sent (flushed in update())
    std::deque<uint32_t> kill_sync_queue_;
    std::deque<uint32_t> collectible_sync_queue_;

    // Player roster
    mutable std::mutex roster_mutex_;
    PlayerInfo player_roster_[MAX_PLAYERS];

    // Generic packet queue: game thread enqueues, SDL thread sends.
    // ENet is NOT thread-safe — all sends must go through this queue.
    struct QueuedPacket {
        std::vector<uint8_t> data;
        uint8_t channel;
        bool reliable;
        // 0xFF = broadcast to all peers; 0..MAX_PLAYERS-1 = send only to that peer
        uint8_t target_player;
    };
    static constexpr uint8_t BROADCAST_TARGET = 0xFF;
    mutable std::mutex send_queue_mutex_;
    std::deque<QueuedPacket> packet_send_queue_;

    // Thread-safe enqueue (called from game thread)
    void enqueue_packet(const void* data, size_t size, uint8_t channel, bool reliable);
    void enqueue_packet_to(uint8_t target_player, const void* data, size_t size, uint8_t channel, bool reliable);
};

} // namespace bknet

#endif // NET_MANAGER_H
