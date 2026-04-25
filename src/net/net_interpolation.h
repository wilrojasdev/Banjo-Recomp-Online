#ifndef NET_INTERPOLATION_H
#define NET_INTERPOLATION_H

#include <cstdint>
#include <chrono>
#include <array>
#include <mutex>
#include <unordered_map>

#include "net_packets.h"

namespace bknet {

struct PositionSnapshot {
    float x = 0.0f, y = 0.0f, z = 0.0f;
    float yaw = 0.0f;
    float pitch = 0.0f;
    float scale = 1.0f;
    uint32_t map_id = 0;
    uint32_t level_id = 0;
    uint16_t animation_id = 0;
    float anim_timer = 0.0f;
    float anim_duration = 1.0f;
    float anim_subrange_start = 0.0f;
    float anim_subrange_end = 1.0f;
    uint8_t anim_playback_type = 0;
    uint8_t kazooie_flags = 0;
    uint8_t health = 0;
    uint8_t health_total = 0;
    uint8_t lives = 0;
    uint8_t transformation = 0;
    uint8_t bs_state = 0;
    float horizontal_velocity = 0.0f;
    uint8_t carry_kind = 0;  // CarryKind enum (visual-only)
    double timestamp = 0.0;
    bool valid = false;
};

struct InterpolatedState {
    float x = 0.0f, y = 0.0f, z = 0.0f;
    float yaw = 0.0f;
    float pitch = 0.0f;
    float scale = 1.0f;
    uint32_t map_id = 0;
    uint32_t level_id = 0;
    uint16_t animation_id = 0;
    float anim_timer = 0.0f;
    float anim_duration = 1.0f;
    float anim_subrange_start = 0.0f;
    float anim_subrange_end = 1.0f;
    uint8_t anim_playback_type = 0;
    uint8_t kazooie_flags = 0;
    uint8_t health = 0;
    uint8_t transformation = 0;
    uint8_t bs_state = 0;
    float horizontal_velocity = 0.0f;
    uint8_t carry_kind = 0;  // CarryKind enum (visual-only)
    bool active = false;
    bool animation_changed = false;
};

class RemotePlayerInterpolator {
public:
    static constexpr size_t BUFFER_SIZE = 4;
    // Base (floor) delay; effective delay grows adaptively with measured jitter
    // so lossy/WAN links stop snapping to "latest" and can actually interpolate.
    static constexpr double INTERP_DELAY_MIN_SEC = 0.1;
    static constexpr double INTERP_DELAY_MAX_SEC = 0.4;

    void push_snapshot(const PositionSnapshot& snap);
    InterpolatedState interpolate(double current_time) const;
    void reset();

    bool has_data() const { return snapshot_count_ > 0; }
    uint32_t last_map_id() const;

private:
    std::array<PositionSnapshot, BUFFER_SIZE> buffer_{};
    size_t write_index_ = 0;
    size_t snapshot_count_ = 0;
    // Jitter estimate (exp moving avg of |delta_t - expected_tick|).
    double jitter_ema_ = 0.0;
    double last_arrival_dt_ = 0.0;
    double last_push_time_ = 0.0;
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

// --- Enemy interpolation (host-authoritative) ---

struct EnemySnapshot {
    float x = 0.0f, y = 0.0f, z = 0.0f;
    float yaw = 0.0f;
    uint16_t anim_id = 0;
    uint8_t anim_direction = 1;  // 0=back, 1=forward — snapshot of anctrl playback dir
    uint8_t state = 0;           // actor->state low byte (0 = no state to apply)
    float anim_timer = 0.0f;
    double timestamp = 0.0;
    bool valid = false;
};

struct EnemyInterpolatedState {
    float x = 0.0f, y = 0.0f, z = 0.0f;
    float yaw = 0.0f;
    uint16_t spawn_index = 0;
    uint16_t marker_type = 0;
    uint16_t anim_id = 0;
    uint8_t anim_direction = 1;
    uint8_t state = 0;
    float anim_timer = 0.0f;
    bool active = false;
};

class EnemyInterpolator {
public:
    static constexpr size_t BUFFER_SIZE = 4;
    static constexpr double INTERP_DELAY_MIN_SEC = 0.1;
    static constexpr double INTERP_DELAY_MAX_SEC = 0.4;

    uint16_t marker_type = 0;

    void push_snapshot(const EnemySnapshot& snap);
    EnemyInterpolatedState interpolate(double current_time, uint16_t spawn_idx) const;
    void reset();

private:
    std::array<EnemySnapshot, BUFFER_SIZE> buffer_{};
    size_t write_index_ = 0;
    size_t snapshot_count_ = 0;
    double jitter_ema_ = 0.0;
    double last_push_time_ = 0.0;
};

class EnemyInterpolationManager {
public:
    void push_bulk(const EnemyPositionEntry* entries, uint8_t count, uint32_t map_id, double timestamp);
    size_t get_interpolated(EnemyInterpolatedState* out, size_t max_count, double current_time) const;
    void clear();
    uint32_t current_map_id() const { return map_id_; }

private:
    mutable std::mutex mutex_;
    std::unordered_map<uint16_t, EnemyInterpolator> enemies_; // keyed by spawn_index
    uint32_t map_id_ = 0;
};

} // namespace bknet

#endif // NET_INTERPOLATION_H
