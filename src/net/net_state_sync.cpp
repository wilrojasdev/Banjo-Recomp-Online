#include "net_state_sync.h"

namespace bknet {

void StateSync::write_local_state(const LocalPlayerSnapshot& snap) {
    std::lock_guard<std::mutex> lock(mutex_);
    snapshot_ = snap;
    snapshot_.frame_counter++;
    snapshot_.valid = true;
}

LocalPlayerSnapshot StateSync::read_local_state() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return snapshot_;
}

bool StateSync::build_position_packet(uint8_t player_id, uint16_t sequence, PlayerPositionPacket& out) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!snapshot_.valid) return false;

    out.header.type = PacketType::PlayerPosition;
    out.header.player_id = player_id;
    out.header.sequence = sequence;
    out.x = snapshot_.position[0];
    out.y = snapshot_.position[1];
    out.z = snapshot_.position[2];
    out.yaw = snapshot_.yaw;
    out.map_id = snapshot_.map_id;
    return true;
}

bool StateSync::build_state_packet(uint8_t player_id, uint16_t sequence, PlayerStatePacket& out) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!snapshot_.valid) return false;

    // Compute diff vs last-sent snapshot so the wire payload advertises which
    // fields actually changed. Receiver can (optionally) apply only dirty
    // fields; until that's wired up on the receive side we still fill every
    // field, so reducing `dirty_flags` is safe — the packet decodes the same
    // way. The win arrives once the receive-side honors the mask.
    uint32_t flags = 0;
    if (!last_snapshot_valid_) {
        flags = 0xFFFFFFFF;
    } else {
        const auto& prev = last_snapshot_;
        const auto& cur = snapshot_;
        if (cur.position[0] != prev.position[0] ||
            cur.position[1] != prev.position[1] ||
            cur.position[2] != prev.position[2]) flags |= DIRTY_POSITION;
        if (cur.yaw != prev.yaw || cur.pitch != prev.pitch) flags |= DIRTY_ROTATION;
        if (cur.animation_id != prev.animation_id ||
            cur.anim_timer != prev.anim_timer ||
            cur.anim_duration != prev.anim_duration ||
            cur.anim_playback_type != prev.anim_playback_type) flags |= DIRTY_ANIMATION;
        if (cur.health != prev.health || cur.health_total != prev.health_total ||
            cur.lives != prev.lives) flags |= DIRTY_HEALTH;
        if (cur.transformation != prev.transformation) flags |= DIRTY_TRANSFORMATION;
        if (cur.map_id != prev.map_id || cur.level_id != prev.level_id) flags |= DIRTY_MAP;
        if (cur.kazooie_flags != prev.kazooie_flags ||
            cur.bs_state != prev.bs_state ||
            cur.horizontal_velocity != prev.horizontal_velocity) flags |= DIRTY_ITEMS;
        if (cur.carry_kind != prev.carry_kind) flags |= DIRTY_CARRY;
    }

    out.header.type = PacketType::PlayerState;
    out.header.player_id = player_id;
    out.header.sequence = sequence;
    out.dirty_flags = flags;
    out.x = snapshot_.position[0];
    out.y = snapshot_.position[1];
    out.z = snapshot_.position[2];
    out.yaw = snapshot_.yaw;
    out.pitch = snapshot_.pitch;
    out.roll = 0.0f;
    out.map_id = snapshot_.map_id;
    out.level_id = snapshot_.level_id;
    out.animation_id = snapshot_.animation_id;
    out.anim_progress = snapshot_.anim_timer;
    out.anim_duration = snapshot_.anim_duration;
    out.anim_subrange_start = snapshot_.anim_subrange_start;
    out.anim_subrange_end = snapshot_.anim_subrange_end;
    out.anim_playback_type = snapshot_.anim_playback_type;
    out.kazooie_flags = snapshot_.kazooie_flags;
    out.health = snapshot_.health;
    out.health_total = snapshot_.health_total;
    out.lives = snapshot_.lives;
    out.eggs = 0;
    out.red_feathers = 0;
    out.gold_feathers = 0;
    out.mumbo_tokens = 0;
    out.transformation = snapshot_.transformation;
    out.bs_state = static_cast<uint32_t>(snapshot_.bs_state);
    out.horizontal_velocity = snapshot_.horizontal_velocity;
    out.carry_kind = snapshot_.carry_kind;

    last_snapshot_ = snapshot_;
    last_snapshot_valid_ = true;
    return true;
}

uint32_t StateSync::get_local_map_id() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return snapshot_.map_id;
}

void StateSync::set_level_id(uint32_t level_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    snapshot_.level_id = level_id;
}


} // namespace bknet
