#ifndef NET_INTERPOLATION_H
#define NET_INTERPOLATION_H

#include <cstdint>
#include <chrono>
#include <array>
#include <mutex>

#include "net_packets.h"

namespace bknet {

struct PositionSnapshot {
    float x = 0.0f, y = 0.0f, z = 0.0f;
    float yaw = 0.0f;
    float pitch = 0.0f;
    float scale = 1.0f;
    uint32_t map_id = 0;
    uint16_t animation_id = 0;
    float anim_timer = 0.0f;
    float anim_duration = 1.0f;
    uint8_t anim_playback_type = 0;
    uint8_t health = 0;
    uint8_t health_total = 0;
    uint8_t lives = 0;
    uint8_t transformation = 0;
    uint8_t bs_state = 0;
    float horizontal_velocity = 0.0f;
    double timestamp = 0.0;
    bool valid = false;
};

struct InterpolatedState {
    float x = 0.0f, y = 0.0f, z = 0.0f;
    float yaw = 0.0f;
    float pitch = 0.0f;
    float scale = 1.0f;
    uint32_t map_id = 0;
    uint16_t animation_id = 0;
    float anim_timer = 0.0f;
    float anim_duration = 1.0f;
    uint8_t anim_playback_type = 0;
    uint8_t health = 0;
    uint8_t transformation = 0;
    uint8_t bs_state = 0;
    float horizontal_velocity = 0.0f;
    bool active = false;
    bool animation_changed = false;
};

class RemotePlayerInterpolator {
public:
    static constexpr size_t BUFFER_SIZE = 4;
    static constexpr double INTERP_DELAY_SEC = 0.1; // 100ms interpolation delay

    void push_snapshot(const PositionSnapshot& snap);
    InterpolatedState interpolate(double current_time) const;
    void reset();

    bool has_data() const { return snapshot_count_ > 0; }
    uint32_t last_map_id() const;

private:
    std::array<PositionSnapshot, BUFFER_SIZE> buffer_{};
    size_t write_index_ = 0;
    size_t snapshot_count_ = 0;
    mutable std::mutex mutex_;
};

class InterpolationManager {
public:
    void push_position(uint8_t player_id, float x, float y, float z, float yaw, uint32_t map_id);
    void push_full_state(uint8_t player_id, const PositionSnapshot& snap);
    InterpolatedState get_interpolated(uint8_t player_id) const;
    void remove_player(uint8_t player_id);
    void reset();

    double get_time() const;

private:
    std::array<RemotePlayerInterpolator, MAX_PLAYERS> players_{};
    std::chrono::steady_clock::time_point start_time_ = std::chrono::steady_clock::now();
};

} // namespace bknet

#endif // NET_INTERPOLATION_H
