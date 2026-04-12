#include "net_interpolation.h"
#include <algorithm>
#include <cmath>

namespace bknet {

// --- RemotePlayerInterpolator ---

void RemotePlayerInterpolator::push_snapshot(const PositionSnapshot& snap) {
    std::lock_guard<std::mutex> lock(mutex_);
    buffer_[write_index_] = snap;
    write_index_ = (write_index_ + 1) % BUFFER_SIZE;
    if (snapshot_count_ < BUFFER_SIZE) snapshot_count_++;
}

InterpolatedState RemotePlayerInterpolator::interpolate(double current_time) const {
    std::lock_guard<std::mutex> lock(mutex_);

    InterpolatedState result{};
    if (snapshot_count_ == 0) return result;

    double render_time = current_time - INTERP_DELAY_SEC;

    // Collect valid snapshots sorted by timestamp
    std::array<const PositionSnapshot*, BUFFER_SIZE> sorted{};
    size_t count = 0;
    for (size_t i = 0; i < snapshot_count_; i++) {
        size_t idx = (write_index_ + BUFFER_SIZE - snapshot_count_ + i) % BUFFER_SIZE;
        if (buffer_[idx].valid) {
            sorted[count++] = &buffer_[idx];
        }
    }

    if (count == 0) return result;

    // Helper: copy non-interpolated state from snapshot to result
    auto copy_state = [](InterpolatedState& r, const PositionSnapshot* s) {
        r.x = s->x;
        r.y = s->y;
        r.z = s->z;
        r.yaw = s->yaw;
        r.pitch = s->pitch;
        r.scale = s->scale;
        r.map_id = s->map_id;
        r.animation_id = s->animation_id;
        r.anim_timer = s->anim_timer;
        r.anim_duration = s->anim_duration;
        r.anim_playback_type = s->anim_playback_type;
        r.health = s->health;
        r.transformation = s->transformation;
        r.bs_state = s->bs_state;
        r.horizontal_velocity = s->horizontal_velocity;
        r.active = true;
    };

    // If only one snapshot or render_time is before all snapshots, snap to latest
    if (count == 1 || render_time <= sorted[0]->timestamp) {
        copy_state(result, sorted[count - 1]);
        return result;
    }

    // If render_time is past all snapshots, use latest
    if (render_time >= sorted[count - 1]->timestamp) {
        copy_state(result, sorted[count - 1]);
        return result;
    }

    // Find the two snapshots to interpolate between
    for (size_t i = 0; i < count - 1; i++) {
        const auto* a = sorted[i];
        const auto* b = sorted[i + 1];

        if (render_time >= a->timestamp && render_time <= b->timestamp) {
            double dt = b->timestamp - a->timestamp;
            float t = (dt > 0.0001) ? static_cast<float>((render_time - a->timestamp) / dt) : 0.0f;
            t = std::clamp(t, 0.0f, 1.0f);

            // Interpolate position
            result.x = a->x + (b->x - a->x) * t;
            result.y = a->y + (b->y - a->y) * t;
            result.z = a->z + (b->z - a->z) * t;

            // Interpolate yaw with shortest path
            float yaw_diff = b->yaw - a->yaw;
            if (yaw_diff > 180.0f) yaw_diff -= 360.0f;
            if (yaw_diff < -180.0f) yaw_diff += 360.0f;
            result.yaw = a->yaw + yaw_diff * t;

            result.pitch = a->pitch + (b->pitch - a->pitch) * t;
            result.scale = a->scale + (b->scale - a->scale) * t;

            // Non-interpolated fields: use target snapshot
            result.map_id = b->map_id;
            result.animation_id = b->animation_id;
            result.anim_timer = b->anim_timer;
            result.anim_duration = b->anim_duration;
            result.anim_playback_type = b->anim_playback_type;
            result.health = b->health;
            result.transformation = b->transformation;
            result.bs_state = b->bs_state;
            result.horizontal_velocity = b->horizontal_velocity;
            result.animation_changed = (a->animation_id != b->animation_id);
            result.active = true;
            return result;
        }
    }

    // Fallback: use latest
    copy_state(result, sorted[count - 1]);
    return result;
}

void RemotePlayerInterpolator::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& s : buffer_) s.valid = false;
    write_index_ = 0;
    snapshot_count_ = 0;
}

uint32_t RemotePlayerInterpolator::last_map_id() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (snapshot_count_ == 0) return 0;
    size_t latest = (write_index_ + BUFFER_SIZE - 1) % BUFFER_SIZE;
    return buffer_[latest].map_id;
}

// --- InterpolationManager ---

void InterpolationManager::push_position(uint8_t player_id, float x, float y, float z, float yaw, uint32_t map_id) {
    if (player_id >= MAX_PLAYERS) return;

    PositionSnapshot snap;
    snap.x = x;
    snap.y = y;
    snap.z = z;
    snap.yaw = yaw;
    snap.map_id = map_id;
    snap.timestamp = get_time();
    snap.valid = true;

    players_[player_id].push_snapshot(snap);
}

void InterpolationManager::push_full_state(uint8_t player_id, const PositionSnapshot& snap) {
    if (player_id >= MAX_PLAYERS) return;
    PositionSnapshot s = snap;
    s.timestamp = get_time();
    s.valid = true;
    players_[player_id].push_snapshot(s);
}

InterpolatedState InterpolationManager::get_interpolated(uint8_t player_id) const {
    if (player_id >= MAX_PLAYERS) return {};
    return players_[player_id].interpolate(get_time());
}

void InterpolationManager::remove_player(uint8_t player_id) {
    if (player_id >= MAX_PLAYERS) return;
    players_[player_id].reset();
}

void InterpolationManager::reset() {
    for (auto& p : players_) p.reset();
    start_time_ = std::chrono::steady_clock::now();
}

double InterpolationManager::get_time() const {
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(now - start_time_).count();
}

} // namespace bknet
