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
#include "net_log.h"

namespace bknet {

// Diagnostic counters. All fields are incremented from the SDL thread; reads
// from UI or other threads should prefer the snapshot accessor which returns
// a consistent copy.
struct NetworkStats {
    uint64_t packets_sent = 0;
    uint64_t bytes_sent = 0;
    uint64_t packets_received = 0;
    uint64_t bytes_received = 0;
    uint64_t packets_dropped_queue = 0;       // enqueue_packet overflow
    uint64_t packets_dropped_stale_seq = 0;   // UDP reorder reject
    uint64_t packets_dropped_rate_limit = 0;  // chat / bandwidth cap
    uint64_t packets_dropped_unknown = 0;     // unknown PacketType
    uint64_t packets_dropped_duplicate = 0;   // dedup ring caught a repeat
    uint64_t fragments_sent = 0;
    uint64_t fragments_completed = 0;
    uint64_t fragments_nacked = 0;            // NACK packets emitted
    uint64_t fragments_retransmitted = 0;     // chunks resent in response
    uint64_t fragments_timed_out = 0;         // reassembly abandoned
    uint64_t reconnect_attempts = 0;
    uint64_t peers_dropped_idle = 0;
    uint64_t peers_dropped_bw = 0;
    uint32_t rtt_ms_by_player[MAX_PLAYERS] = {};
};

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

    // Latency (ms) to a given player — 0 if unknown or self.
    uint32_t get_player_rtt_ms(uint8_t player_id) const;

    // Typed error callback for UI (maps internal errors to user-friendly strings).
    void set_typed_error_callback(CoopNetTransport::TypedErrorCallback cb);

    // Diagnostic counters snapshot (cheap — plain copy under the stats mutex).
    NetworkStats get_stats() const;
    uint32_t get_peer_features(uint8_t player_id) const;

    // World state sync (Phase 3)
    void send_collectible(uint8_t type, uint16_t id, uint8_t collected, uint32_t map_id, uint8_t level_id);
    // state = actor->state captured by the killer right after its dieFunc
    // returned. Receivers apply this directly to actor->state so the local
    // update function plays the death animation naturally (sm64-coop pattern).
    // state=0 means "no death state info" — receiver should despawn directly.
    void send_enemy_death(uint16_t marker_type, uint16_t spawn_index, uint32_t map_id,
                          float px, float py, float pz, uint8_t state);
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
        // True once we've emitted "X joined" in chat with this player's real
        // (non-placeholder) display name. Used to defer the join announce
        // until broadcast_local_name() arrives, so chat doesn't show
        // "Jugador 2 joined" before the real name lands. Reset when the slot
        // disconnects so a re-join re-announces.
        bool announced = false;
    };
    void set_player_name(uint8_t player_id, const std::string& name);
    PlayerInfo get_player_info(uint8_t player_id) const;
    void clear_player_roster();
    void broadcast_local_name();  // Send PlayerJoinPacket with our name
    // Host-side: mark a freshly-connected peer as present in the roster with a
    // placeholder name and emit the "joined" chat message right away. Avoids
    // depending on the joiner's broadcast_local_name() arriving before the UI
    // can reflect their presence.
    void register_remote_player_locally(uint8_t player_id);

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
    // `skip_accounting` = true when re-entering with a reassembled fragment whose
    // chunks were already counted at the wire level; avoids double-billing the
    // BW cap and stats counters.
    void handle_packet(uint8_t from_player_id, const uint8_t* data, size_t size,
                       bool skip_accounting = false);

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

    // Keepalive, ping/pong, version handshake, idle timeout — called once per
    // SDL-thread update() tick.
    void tick_network_health();
    void send_keepalive_to_all();
    void send_ping_to_all();
    void send_version_check(uint8_t player_id);
    void handle_ping_packet(uint8_t from_player_id, const PingPacket& pkt);
    void handle_pong_packet(uint8_t from_player_id, const PongPacket& pkt);
    void handle_version_check(uint8_t from_player_id, const VersionCheckPacket& pkt);

    // Fragment transport helpers. send_chunked fragments a large reliable
    // payload; handle_fragment_chunk reassembles and re-enters the dispatcher.
    void send_chunked(uint8_t target_player, PacketType original_type,
                      const void* data, size_t size);
    void handle_fragment_chunk(uint8_t from_player_id, const FragmentChunkPacket& pkt);

    // zlib wrap/unwrap. wrap_compressed_for_peer returns a compressed envelope
    // when the peer negotiated FEATURE_COMPRESSION and `size` is worth
    // compressing; otherwise returns an empty vector and the caller should
    // send the original payload. handle_compressed_packet inflates the inner
    // payload and re-dispatches it through handle_packet.
    std::vector<uint8_t> wrap_compressed_for_peer(uint8_t target_player,
                                                  const void* data, size_t size);
    void handle_compressed_packet(uint8_t from_player_id,
                                  const uint8_t* data, size_t size);
    void handle_fragment_nack(uint8_t from_player_id, const FragmentNackPacket& pkt);

    // Called from tick_network_health(): emit NACKs for incomplete reassembly
    // groups that have been stalled >2s, and purge stale sender cache entries.
    void tick_fragment_maintenance(double now);

    // Reset any per-peer state that must not leak to a new player landing in
    // the same slot after a disconnect (seq tracking, rate limits, reassembly).
    void reset_peer_state(uint8_t player_id);

    // Per-player sequence check: returns true if `seq` is newer (or first)
    // for the given (player_id, type) pair. Handles 16-bit wrap.
    bool sequence_is_fresh(uint8_t player_id, PacketType type, uint16_t seq);

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

    // Per-joiner ack tracking. Host stamps full_sync_sent_at_ when MIPS hands
    // the packet to the wire and waits for a WorldStateFullAck. If FULLSTATE_ACK
    // is negotiated and no ack arrives within FULL_SYNC_ACK_TIMEOUT, host re-arms
    // pending_full_sync_ for that player up to MAX_FULL_SYNC_RETRIES times.
    static constexpr double  FULL_SYNC_ACK_TIMEOUT = 12.0;
    static constexpr uint8_t MAX_FULL_SYNC_RETRIES = 3;
    std::array<double,  MAX_PLAYERS> full_sync_sent_at_{};   // 0.0 = no send pending ack
    std::array<bool,    MAX_PLAYERS> full_sync_acked_{};
    std::array<uint8_t, MAX_PLAYERS> full_sync_retries_{};
    void handle_world_state_full_ack(uint8_t from_player_id, const WorldStateFullAckPacket& pkt);
    void tick_full_sync_retry(double now);

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
    // Hard cap to prevent unbounded memory growth if CoopNet stalls. Unreliable
    // packets older than this are dropped first; non-critical reliables next;
    // critical reliables (handshake/save/world state) are protected and only
    // ever dropped at twice the cap as a last resort.
    static constexpr size_t MAX_SEND_QUEUE = 512;
    // Returns true for packet types that MUST be delivered for the join/sync
    // pipeline to complete. Inspected from the first byte of a queued payload.
    static bool is_critical_reliable(uint8_t first_byte);
    mutable std::mutex send_queue_mutex_;
    std::deque<QueuedPacket> packet_send_queue_;

    // Thread-safe enqueue (called from game thread)
    void enqueue_packet(const void* data, size_t size, uint8_t channel, bool reliable);
    void enqueue_packet_to(uint8_t target_player, const void* data, size_t size, uint8_t channel, bool reliable);

    // --- Network health state (SDL-thread only) ---
    double last_keepalive_send_ = 0.0;
    double last_ping_send_ = 0.0;
    double last_idle_check_ = 0.0;

    // Per-(player, packet-type) freshness for UDP reorder rejection.
    // Keyed by ((player_id << 8) | packet_type_low_byte). uint16_t store.
    std::unordered_map<uint16_t, uint16_t> last_seen_seq_;

    // Universal duplicate-detection ring buffer (one per peer). Mirrors the
    // sm64coopdx network_player rxSeqIds + rxPacketHash design. Catches
    // duplicates the per-type freshness check misses: same payload arriving
    // twice via the CoopNet mesh, ENet retransmit-after-NACK collisions,
    // and seqId reuse after a peer slot recycles. Indexed by next_idx;
    // wraps after RX_DEDUP_RING bytes.
    static constexpr size_t RX_DEDUP_RING = 256;
    struct RxDedup {
        std::array<uint16_t, RX_DEDUP_RING> seq{};
        std::array<uint32_t, RX_DEDUP_RING> hash{};
        size_t next_idx = 0;
    };
    std::array<RxDedup, MAX_PLAYERS> rx_dedup_{};
    bool is_duplicate_packet(uint8_t from_player_id, uint16_t seq,
                             const uint8_t* data, size_t size);

    // Fragment reassembly buffer per (sender player_id, group_id).
    struct FragmentAssembly {
        uint16_t chunk_count = 0;
        uint16_t chunks_received = 0;
        uint32_t total_size = 0;
        uint8_t original_type = 0;
        double started_at = 0.0;
        double last_chunk_at = 0.0;   // last time a chunk for this group arrived
        double last_nack_at = 0.0;    // throttle NACK emission to once per 2s
        uint8_t nack_attempts = 0;    // stop NACKing after MAX_NACK_ATTEMPTS
        std::vector<bool> received_mask;
        std::vector<uint8_t> buffer;
    };
    // 15 attempts × ~1s throttle = ~15s of recovery before giving up. Combined
    // with the joiner-side grace idle timeout (see tick_network_health) this
    // covers WAN packet-loss bursts during the WorldStateFull/HostEeprom join
    // burst without dropping the peer.
    static constexpr uint8_t MAX_NACK_ATTEMPTS = 15;
    std::unordered_map<uint32_t, FragmentAssembly> fragment_assembly_;

    // Sender-side cache of chunks for potential retransmit. Key = group_id.
    // Chunks are retained until all delivered (we can't know that — approximate
    // via 30s TTL) or the lobby transitions. Entries store raw wire bytes so
    // the retransmit path doesn't re-serialize.
    struct SentFragmentGroup {
        uint8_t target_player = BROADCAST_TARGET;
        uint16_t chunk_count = 0;
        double sent_at = 0.0;
        std::vector<std::vector<uint8_t>> chunks; // raw wire bytes per chunk
    };
    std::unordered_map<uint16_t, SentFragmentGroup> sent_fragments_;
    // Cap in-flight sender-side fragment groups. Beyond this we evict the
    // oldest (by sent_at) before inserting. Each group can hold up to ~65 KB,
    // so 32 caps memory at ~2 MB which is tolerable even during bulk sync.
    static constexpr size_t MAX_SENT_FRAGMENT_GROUPS = 32;

    // Monotonic group_id generator (atomic so future callers from game thread
    // can't tear the value — send_chunked currently runs SDL-only but this
    // removes the foot-gun).
    std::atomic<uint16_t> next_fragment_group_id_{1};

    // Per-player chat rate limit (seconds). Drop chat messages closer than 500ms.
    std::array<double, MAX_PLAYERS> last_chat_time_{};

    // Per-player inbound bandwidth tracker (1s window). Peers exceeding
    // MAX_INBOUND_BPS are dropped with CoopNetError::BandwidthExceeded (treated
    // as a peer-failed equivalent — this protects the host from buggy clients).
    // Steady-state cap raised to 512 KB/s so normal gameplay (32 enemies @
    // 30 Hz + voice + state deltas) doesn't graze the limit on busy maps.
    // During the first JOIN_GRACE_SECONDS after a peer's first packet, the
    // cap is multiplied by JOIN_GRACE_MULTIPLIER so the initial WorldStateFull
    // + HostEeprom + bulk enemies burst can pass without tripping the kick.
    static constexpr uint32_t MAX_INBOUND_BPS = 512 * 1024; // 512 KB/s steady
    static constexpr double   JOIN_GRACE_SECONDS = 10.0;
    static constexpr uint32_t JOIN_GRACE_MULTIPLIER = 4;    // 2 MB/s during burst
    std::array<uint32_t, MAX_PLAYERS> bytes_recv_window_{};
    std::array<double,  MAX_PLAYERS> peer_first_seen_{};    // 0.0 = unset
    double last_bw_window_reset_ = 0.0;

    // Receive-side "last known state" for dirty_flags delta apply. When a
    // PlayerStatePacket arrives with partial dirty_flags, un-dirty fields are
    // filled from this snapshot. Reset on peer disconnect/reassign.
    std::array<PositionSnapshot, MAX_PLAYERS> last_recv_state_{};
    std::array<bool, MAX_PLAYERS> last_recv_state_valid_{};

    // Per-peer negotiated feature bitmap (AND of local & remote SUPPORTED_FEATURES).
    // 0 means the peer hasn't completed the version handshake yet.
    std::array<uint32_t, MAX_PLAYERS> peer_features_{};

    // Stats snapshot. Mutated from SDL thread; read via get_stats() which
    // takes the mutex for a consistent copy.
    mutable std::mutex stats_mutex_;
    NetworkStats stats_{};

    // Reconnection backoff state.
    bool reconnect_pending_ = false;
    double next_reconnect_at_ = 0.0;
    uint32_t reconnect_attempts_ = 0;
    std::string reconnect_server_;
    uint16_t reconnect_port_ = 0;
    // Saved lobby coordinates so the joiner can re-enter the same lobby after
    // signaling reconnects. 0 means "not a lobby joiner" (host or LAN).
    uint64_t reconnect_lobby_id_ = 0;
    std::string reconnect_lobby_password_;
    bool reconnect_relobby_pending_ = false; // signaling came back, need to join

    // Connecting-state watchdog: if we don't transition to Connected within
    // CONNECTING_TIMEOUT seconds, drop to Disconnected with a visible error.
    static constexpr double CONNECTING_TIMEOUT = 25.0;
    double connecting_started_at_ = 0.0; // 0.0 = not in Connecting

    CoopNetTransport::TypedErrorCallback typed_error_callback_;
};

} // namespace bknet

#endif // NET_MANAGER_H
