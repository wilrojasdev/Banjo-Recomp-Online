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

    out.header.type = PacketType::PlayerState;
    out.header.player_id = player_id;
    out.header.sequence = sequence;
    out.dirty_flags = 0xFFFFFFFF; // send all fields for now
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
