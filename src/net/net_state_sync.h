#ifndef NET_STATE_SYNC_H
#define NET_STATE_SYNC_H

#include <cstdint>
#include <mutex>

#include "net_packets.h"

namespace bknet {

// Full local player state snapshot, written by game thread, read by network thread.
struct LocalPlayerSnapshot {
    float position[3];
    float yaw;
    float pitch;
    float scale;
    uint32_t map_id;
    uint16_t animation_id;
    float anim_timer;
    float anim_duration;
    uint8_t anim_playback_type;
    uint8_t health;
    uint8_t health_total;
    uint8_t lives;
    uint8_t transformation;
    uint8_t bs_state;
    uint32_t frame_counter;
    bool valid;
};

class StateSync {
public:
    // Called from the game thread (via patch) to update local state
    void write_local_state(const LocalPlayerSnapshot& snap);

    // Called from the network thread to read the latest local state
    LocalPlayerSnapshot read_local_state() const;

    // Build a position-only packet (Phase 1 compat)
    bool build_position_packet(uint8_t player_id, uint16_t sequence, PlayerPositionPacket& out) const;

    // Build a full state packet (Phase 2)
    bool build_state_packet(uint8_t player_id, uint16_t sequence, PlayerStatePacket& out) const;

    uint32_t get_local_map_id() const;

private:
    mutable std::mutex mutex_;
    LocalPlayerSnapshot snapshot_{};
};

} // namespace bknet

#endif // NET_STATE_SYNC_H
