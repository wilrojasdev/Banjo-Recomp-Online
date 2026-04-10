#ifndef NET_MANAGER_H
#define NET_MANAGER_H

#include <memory>
#include <cstdint>
#include <functional>
#include <string>
#include <deque>
#include <mutex>

#include "net_packets.h"
#include "net_server.h"
#include "net_client.h"
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

    // Called every frame from update_gfx
    void update();

    // Called from game thread (via patch) to push local player state
    void push_local_state(float x, float y, float z, float yaw, uint32_t map_id);
    void push_local_full_state(const LocalPlayerSnapshot& snap);

    // Get interpolated remote player state (for ghost rendering)
    InterpolatedState get_remote_player(uint8_t player_id) const;

    // Chat
    void send_chat(const std::string& message);

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

    // Accessors
    bool is_connected() const { return state_ == ConnectionState::Hosting || state_ == ConnectionState::Connected; }
    bool is_host() const { return state_ == ConnectionState::Hosting; }
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
    void handle_position_packet(const PlayerPositionPacket& pkt);
    void handle_state_packet(const PlayerStatePacket& pkt);
    void handle_map_change_packet(const MapChangePacket& pkt);
    void handle_chat_packet(const ChatMessagePacket& pkt);

    std::unique_ptr<Server> server_;
    std::unique_ptr<Client> client_;

    StateSync state_sync_;
    InterpolationManager interpolation_;

    ConnectionState state_ = ConnectionState::Disconnected;
    uint8_t local_player_id_ = 0;

    uint16_t send_sequence_ = 0;
    uint32_t frame_counter_ = 0;
    static constexpr uint32_t SEND_INTERVAL_FRAMES = 3; // ~20Hz at 60fps

    bool initialized_ = false;

    // Chat state
    mutable std::mutex chat_mutex_;
    std::deque<ChatEntry> chat_history_;
    bool new_messages_ = false;
    std::chrono::steady_clock::time_point start_time_ = std::chrono::steady_clock::now();
    double get_time() const;
};

} // namespace bknet

#endif // NET_MANAGER_H
