#include "net_interpolation.h"
#include <algorithm>
#include <cmath>

namespace bknet {

// --- RemotePlayerInterpolator ---

void RemotePlayerInterpolator::push_snapshot(const PositionSnapshot& snap) {
    std::lock_guard<std::mutex> lock(mutex_);
    // Track inter-arrival jitter vs the expected ~50ms tick (20Hz send rate).
    // jitter_ema_ tracks the deviation from expected spacing; we use it to
    // grow the interpolation delay just enough to absorb real WAN jitter.
    constexpr double expected_dt = 0.05;
    if (last_push_time_ > 0.0) {
        double dt = snap.timestamp - last_push_time_;
        double delta = dt - expected_dt;
        if (delta < 0) delta = -delta;
        jitter_ema_ = 0.8 * jitter_ema_ + 0.2 * delta;
    }
    last_push_time_ = snap.timestamp;

    buffer_[write_index_] = snap;
    write_index_ = (write_index_ + 1) % BUFFER_SIZE;
    if (snapshot_count_ < BUFFER_SIZE) snapshot_count_++;
}

InterpolatedState RemotePlayerInterpolator::interpolate(double current_time) const {
    std::lock_guard<std::mutex> lock(mutex_);

    InterpolatedState result{};
    if (snapshot_count_ == 0) return result;

    // Adaptive delay: min 100ms, scales up as jitter grows so we stay inside
    // the snapshot window on lossy/WAN links and keep interpolating instead
    // of snapping-to-latest (which produces visible rubber-banding).
    double delay = INTERP_DELAY_MIN_SEC + 2.5 * jitter_ema_;
    if (delay > INTERP_DELAY_MAX_SEC) delay = INTERP_DELAY_MAX_SEC;
    double render_time = current_time - delay;

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
        r.level_id = s->level_id;
        r.animation_id = s->animation_id;
        r.anim_timer = s->anim_timer;
        r.anim_duration = s->anim_duration;
        r.anim_subrange_start = s->anim_subrange_start;
        r.anim_subrange_end = s->anim_subrange_end;
        r.anim_playback_type = s->anim_playback_type;
        r.kazooie_flags = s->kazooie_flags;
        r.health = s->health;
        r.transformation = s->transformation;
        r.bs_state = s->bs_state;
        r.horizontal_velocity = s->horizontal_velocity;
        r.carry_kind = s->carry_kind;
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
            result.level_id = b->level_id;
            result.animation_id = b->animation_id;
            result.anim_timer = b->anim_timer;
            result.anim_duration = b->anim_duration;
            result.anim_subrange_start = b->anim_subrange_start;
            result.anim_subrange_end = b->anim_subrange_end;
            result.anim_playback_type = b->anim_playback_type;
            result.kazooie_flags = b->kazooie_flags;
            result.health = b->health;
            result.transformation = b->transformation;
            result.bs_state = b->bs_state;
            result.horizontal_velocity = b->horizontal_velocity;
            result.carry_kind = b->carry_kind;
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

// --- EnemyInterpolator ---

void EnemyInterpolator::push_snapshot(const EnemySnapshot& snap) {
    // Mirror the player jitter EMA so WAN enemies also stop snapping.
    constexpr double expected_dt = 0.05;
    if (last_push_time_ > 0.0) {
        double dt = snap.timestamp - last_push_time_;
        double delta = dt - expected_dt;
        if (delta < 0) delta = -delta;
        jitter_ema_ = 0.8 * jitter_ema_ + 0.2 * delta;
    }
    last_push_time_ = snap.timestamp;

    buffer_[write_index_] = snap;
    write_index_ = (write_index_ + 1) % BUFFER_SIZE;
    if (snapshot_count_ < BUFFER_SIZE) snapshot_count_++;
}

EnemyInterpolatedState EnemyInterpolator::interpolate(double current_time, uint16_t spawn_idx) const {
    EnemyInterpolatedState result{};
    result.spawn_index = spawn_idx;
    result.marker_type = marker_type;

    if (snapshot_count_ == 0) return result;

    double delay = INTERP_DELAY_MIN_SEC + 2.5 * jitter_ema_;
    if (delay > INTERP_DELAY_MAX_SEC) delay = INTERP_DELAY_MAX_SEC;
    double render_time = current_time - delay;

    // Collect valid snapshots sorted by timestamp
    std::array<const EnemySnapshot*, BUFFER_SIZE> sorted{};
    size_t count = 0;
    for (size_t i = 0; i < snapshot_count_; i++) {
        size_t idx = (write_index_ + BUFFER_SIZE - snapshot_count_ + i) % BUFFER_SIZE;
        if (buffer_[idx].valid) {
            sorted[count++] = &buffer_[idx];
        }
    }
    if (count == 0) return result;

    auto copy_snap = [&](const EnemySnapshot* s) {
        result.x = s->x; result.y = s->y; result.z = s->z;
        result.yaw = s->yaw;
        result.anim_id = s->anim_id;
        result.anim_direction = s->anim_direction;
        result.state = s->state;
        result.anim_timer = s->anim_timer;
        result.active = true;
    };

    if (count == 1 || render_time <= sorted[0]->timestamp) {
        copy_snap(sorted[count - 1]);
        return result;
    }
    if (render_time >= sorted[count - 1]->timestamp) {
        copy_snap(sorted[count - 1]);
        return result;
    }

    for (size_t i = 0; i < count - 1; i++) {
        const auto* a = sorted[i];
        const auto* b = sorted[i + 1];
        if (render_time >= a->timestamp && render_time <= b->timestamp) {
            double dt = b->timestamp - a->timestamp;
            float t = (dt > 0.0001) ? static_cast<float>((render_time - a->timestamp) / dt) : 0.0f;
            t = std::clamp(t, 0.0f, 1.0f);

            result.x = a->x + (b->x - a->x) * t;
            result.y = a->y + (b->y - a->y) * t;
            result.z = a->z + (b->z - a->z) * t;

            float yaw_diff = b->yaw - a->yaw;
            if (yaw_diff > 180.0f) yaw_diff -= 360.0f;
            if (yaw_diff < -180.0f) yaw_diff += 360.0f;
            result.yaw = a->yaw + yaw_diff * t;

            // Animation: use target snapshot (don't interpolate anim state)
            result.anim_id = b->anim_id;
            result.anim_direction = b->anim_direction;
            result.state = b->state;
            result.anim_timer = b->anim_timer;

            result.active = true;
            return result;
        }
    }

    copy_snap(sorted[count - 1]);
    return result;
}

void EnemyInterpolator::reset() {
    for (auto& s : buffer_) s.valid = false;
    write_index_ = 0;
    snapshot_count_ = 0;
}

// --- EnemyInterpolationManager ---

void EnemyInterpolationManager::push_bulk(const EnemyPositionEntry* entries, uint8_t count, uint32_t map_id, double timestamp) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Clear on map change
    if (map_id != map_id_) {
        enemies_.clear();
        map_id_ = map_id;
    }

    for (uint8_t i = 0; i < count; i++) {
        auto& interp = enemies_[entries[i].spawn_index];
        interp.marker_type = entries[i].marker_type;

        EnemySnapshot snap;
        snap.x = entries[i].x;
        snap.y = entries[i].y;
        snap.z = entries[i].z;
        snap.yaw = entries[i].yaw;
        snap.anim_id = entries[i].anim_id;
        snap.anim_direction = entries[i].anim_direction;
        snap.state = entries[i].state;
        snap.anim_timer = entries[i].anim_timer;
        snap.timestamp = timestamp;
        snap.valid = true;
        interp.push_snapshot(snap);
    }
}

size_t EnemyInterpolationManager::get_interpolated(EnemyInterpolatedState* out, size_t max_count, double current_time) const {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t written = 0;
    for (const auto& [spawn_idx, interp] : enemies_) {
        if (written >= max_count) break;
        auto state = interp.interpolate(current_time, spawn_idx);
        if (state.active) {
            out[written++] = state;
        }
    }
    return written;
}

void EnemyInterpolationManager::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    enemies_.clear();
    map_id_ = 0;
}

} // namespace bknet
