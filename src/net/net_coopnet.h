#ifndef NET_COOPNET_H
#define NET_COOPNET_H

#include <cstdint>
#include <functional>
#include <string>
#include <array>
#include <vector>
#include <mutex>

#include "net_packets.h"
#include "libcoopnet.h"

namespace bknet {

struct LobbyInfo {
    uint64_t lobby_id;
    uint64_t owner_id;
    std::string host_name;
    std::string description;
    std::string mode;
    uint16_t player_count;
    uint16_t max_players;
};

// Translated error kinds. Keep in sync with translate_error() in the .cpp
// so UI code can show actionable messages instead of raw numbers.
enum class CoopNetError : int {
    Unknown = 0,
    LobbyNotFound,
    LobbyFull,
    LobbyJoinFailed,
    PasswordIncorrect,
    VersionMismatch,
    PeerFailed,
    IdleTimeout,      // local: peer stopped sending keepalive
    ProtocolMismatch, // local: PROTOCOL_VERSION handshake failed
};

class CoopNetTransport {
public:
    using PacketCallback = std::function<void(uint8_t player_id, const uint8_t* data, size_t size)>;
    using PeerCallback = std::function<void(uint8_t player_id)>;
    using StateCallback = std::function<void()>;
    using LobbyListCallback = std::function<void(const std::vector<LobbyInfo>& lobbies)>;
    using LobbyCreatedCallback = std::function<void(uint64_t lobby_id)>;
    using ErrorCallback = std::function<void(int error_code)>;
    using TypedErrorCallback = std::function<void(CoopNetError kind, const std::string& message)>;

    CoopNetTransport();
    ~CoopNetTransport();

    // Lifecycle
    bool begin(const std::string& server, uint16_t port, const std::string& player_name);
    void shutdown();
    void update();

    // Lobby operations
    bool create_lobby(const std::string& password, const std::string& description, uint16_t max_players,
                      const std::string& host_name);
    bool join_lobby(uint64_t lobby_id, const std::string& password);
    bool leave_lobby();
    void request_lobby_list(const std::string& password = "");

    // Data (P2P)
    void broadcast(const void* data, size_t size);
    void send_to(uint8_t player_id, const void* data, size_t size);

    // State
    bool is_connected() const { return signaling_connected_; }
    bool is_lobby_host() const { return is_host_; }
    bool is_in_lobby() const { return current_lobby_id_ != 0; }
    uint8_t peer_count() const;
    uint8_t local_player_id() const { return local_player_id_; }

    // Callbacks
    void set_packet_callback(PacketCallback cb) { packet_callback_ = cb; }
    void set_connect_callback(PeerCallback cb) { connect_callback_ = cb; }
    void set_disconnect_callback(PeerCallback cb) { disconnect_callback_ = cb; }
    void set_signaling_disconnect_callback(StateCallback cb) { signaling_disconnect_callback_ = cb; }
    void set_lobby_list_callback(LobbyListCallback cb) { lobby_list_callback_ = cb; }
    void set_lobby_created_callback(LobbyCreatedCallback cb) { lobby_created_callback_ = cb; }
    void set_error_callback(ErrorCallback cb) { error_callback_ = cb; }
    void set_typed_error_callback(TypedErrorCallback cb) { typed_error_callback_ = cb; }

    // RTT accessor for UI (milliseconds; 0 = unknown). Thread-safe.
    uint32_t get_peer_rtt_ms(uint8_t player_id) const;

    // Lookup peer_id for a given player_id — used by NetworkManager to send
    // per-peer (Ping, VersionCheck) from the SDL thread.
    uint64_t peer_id_for_player(uint8_t player_id) const;

    // Drop a peer due to application-level reason (idle, version mismatch).
    // Fires disconnect_callback like a normal disconnect.
    void drop_peer(uint8_t player_id, CoopNetError reason);

    // Marks that we successfully received *any* inbound packet from this peer —
    // resets the idle timer. Called by NetworkManager after handle_packet.
    void mark_peer_alive(uint8_t player_id);

    // Check how many seconds since last inbound packet from peer. Returns
    // negative if peer unknown.
    double seconds_since_last_packet(uint8_t player_id) const;

    // Called by NetworkManager::update() on SDL thread to store latest RTT.
    void set_peer_rtt_ms(uint8_t player_id, uint32_t rtt_ms);

private:
    // CoopNet C callback trampolines
    static void on_connected(uint64_t user_id);
    static void on_disconnected(bool intentional);
    static void on_lobby_created(uint64_t lobby_id, const char* game, const char* version,
                                  const char* host_name, const char* mode, uint16_t max_connections);
    static void on_lobby_joined(uint64_t lobby_id, uint64_t user_id, uint64_t owner_id, uint64_t dest_id);
    static void on_lobby_left(uint64_t lobby_id, uint64_t user_id);
    static void on_lobby_list_got(uint64_t lobby_id, uint64_t owner_id, uint16_t connections,
                                   uint16_t max_connections, const char* game, const char* version,
                                   const char* host_name, const char* mode, const char* description);
    static void on_lobby_list_finish();
    static void on_receive(uint64_t from_user_id, const uint8_t* data, uint64_t size);
    static void on_error(enum MPacketErrorNumber error_number, uint64_t tag);
    static void on_peer_connected(uint64_t peer_id);
    static void on_peer_disconnected(uint64_t peer_id);

    // Peer ID mapping: CoopNet uint64_t <-> game player_id (0-3)
    uint8_t assign_player_id(uint64_t peer_id);
    void release_player_id(uint64_t peer_id);
    uint8_t peer_to_player_id(uint64_t peer_id) const;
    uint64_t player_id_to_peer(uint8_t player_id) const;

    static constexpr uint64_t INVALID_PEER = 0;

    std::array<uint64_t, MAX_PLAYERS> player_peers_{}; // player_id -> coopnet peer_id
    uint8_t local_player_id_ = 0;
    uint64_t local_user_id_ = 0;
    uint64_t lobby_owner_id_ = 0;
    uint64_t current_lobby_id_ = 0;
    bool signaling_connected_ = false;
    bool is_host_ = false;
    bool shutdown_pending_ = false;
    // Set by leave_lobby() so on_lobby_left can distinguish a voluntary exit
    // from being booted (host left, server kick). Without it, the unexpected-
    // disconnect callback fired on every leave and triggered phantom reconnects.
    bool voluntary_leave_ = false;

    // Lobby list accumulator
    std::vector<LobbyInfo> lobby_list_buffer_;

    PacketCallback packet_callback_;
    PeerCallback connect_callback_;
    PeerCallback disconnect_callback_;
    StateCallback signaling_disconnect_callback_;
    LobbyListCallback lobby_list_callback_;
    LobbyCreatedCallback lobby_created_callback_;
    ErrorCallback error_callback_;
    TypedErrorCallback typed_error_callback_;

    // Per-peer liveness & latency (indexed by player_id). All access from SDL
    // thread so no additional mutex required beyond the single-threaded
    // invariant already documented. rtt_ms_ is atomic because get_stats() may
    // be read from UI threads other than SDL.
    std::array<double, MAX_PLAYERS> last_packet_time_{}; // seconds since start
    std::array<std::atomic<uint32_t>, MAX_PLAYERS> rtt_ms_{};
    std::array<bool, MAX_PLAYERS> cleanup_done_{}; // guards double-cleanup between on_lobby_left / on_peer_disconnected

    static CoopNetTransport* s_instance_;
};

} // namespace bknet

#endif // NET_COOPNET_H
