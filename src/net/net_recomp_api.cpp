#include <cstring>
#include "recomp.h"
#include "librecomp/helpers.hpp"
#include "net_manager.h"
#include "net_chat.h"

// These functions are called from game patches (game thread) via REGISTER_FUNC.

// NetFullState struct layout (must match patches/network_patches.c):
// Offset  Field                    Size
// 0x00    f32 x                    4
// 0x04    f32 y                    4
// 0x08    f32 z                    4
// 0x0C    f32 yaw                  4
// 0x10    f32 pitch                4
// 0x14    f32 scale                4
// 0x18    u32 map_id               4
// 0x1C    u16 animation_id         2
// 0x1E    (pad)                    2
// 0x20    f32 anim_timer           4
// 0x24    f32 anim_duration        4
// 0x28    u8  anim_playback_type   1
// 0x29    u8  health               1
// 0x2A    u8  health_total         1
// 0x2B    u8  lives                1
// 0x2C    u8  transformation       1
// 0x2D    u8  bs_state             1
// 0x2E    u8  kazooie_flags        1
// 0x2F    u8  _pad                 1
// 0x30    f32 horizontal_velocity  4
// 0x34    f32 anim_subrange_start  4
// 0x38    f32 anim_subrange_end    4
// Total: 0x3C (60 bytes)

static inline float read_f32(uint8_t* rdram, gpr addr, int offset) {
    u32 raw = MEM_W(offset, addr);
    float val;
    std::memcpy(&val, &raw, sizeof(float));
    return val;
}

static inline void write_f32(uint8_t* rdram, gpr addr, int offset, float val) {
    u32 raw;
    std::memcpy(&raw, &val, sizeof(u32));
    MEM_W(offset, addr) = raw;
}

// Push full local player state from game thread.
// Args: NetFullState* state
extern "C" void recomp_net_push_full_state(uint8_t* rdram, recomp_context* ctx) {
    gpr state_ptr = ctx->r4;

    bknet::LocalPlayerSnapshot snap{};
    snap.position[0] = read_f32(rdram, state_ptr, 0x00);
    snap.position[1] = read_f32(rdram, state_ptr, 0x04);
    snap.position[2] = read_f32(rdram, state_ptr, 0x08);
    snap.yaw         = read_f32(rdram, state_ptr, 0x0C);
    snap.pitch        = read_f32(rdram, state_ptr, 0x10);
    snap.scale        = read_f32(rdram, state_ptr, 0x14);
    snap.map_id       = MEM_W(0x18, state_ptr);
    snap.animation_id = static_cast<uint16_t>(MEM_HU(0x1C, state_ptr));
    snap.anim_timer     = read_f32(rdram, state_ptr, 0x20);
    snap.anim_duration  = read_f32(rdram, state_ptr, 0x24);
    snap.anim_playback_type = MEM_BU(0x28, state_ptr);
    snap.health       = MEM_BU(0x29, state_ptr);
    snap.health_total = MEM_BU(0x2A, state_ptr);
    snap.lives        = MEM_BU(0x2B, state_ptr);
    snap.transformation = MEM_BU(0x2C, state_ptr);
    snap.bs_state     = MEM_BU(0x2D, state_ptr);
    snap.kazooie_flags = MEM_BU(0x2E, state_ptr);
    snap.horizontal_velocity = read_f32(rdram, state_ptr, 0x30);
    snap.anim_subrange_start = read_f32(rdram, state_ptr, 0x34);
    snap.anim_subrange_end   = read_f32(rdram, state_ptr, 0x38);

    bknet::NetworkManager::instance().push_local_full_state(snap);
}

// Backwards-compat: position-only push (Phase 1)
extern "C" void recomp_net_push_local_state(uint8_t* rdram, recomp_context* ctx) {
    gpr state_ptr = ctx->r4;

    bknet::LocalPlayerSnapshot snap{};
    snap.position[0] = read_f32(rdram, state_ptr, 0x00);
    snap.position[1] = read_f32(rdram, state_ptr, 0x04);
    snap.position[2] = read_f32(rdram, state_ptr, 0x08);
    snap.yaw         = read_f32(rdram, state_ptr, 0x0C);
    snap.map_id       = MEM_W(0x10, state_ptr);

    bknet::NetworkManager::instance().push_local_full_state(snap);
}

// Returns 1 if networking is active, 0 otherwise.
extern "C" void recomp_net_is_connected(uint8_t* rdram, recomp_context* ctx) {
    _return(ctx, bknet::NetworkManager::instance().is_connected() ? 1u : 0u);
}

// Get remote player's interpolated state.
// Args: u32 player_id, NetFullState* out
// Returns: u32 (1 if active, 0 if not)
extern "C" void recomp_net_get_remote_state(uint8_t* rdram, recomp_context* ctx) {
    u32 player_id = static_cast<u32>(ctx->r4);
    gpr out_ptr = ctx->r5;

    auto state = bknet::NetworkManager::instance().get_remote_player(static_cast<uint8_t>(player_id));

    if (state.active) {
        write_f32(rdram, out_ptr, 0x00, state.x);
        write_f32(rdram, out_ptr, 0x04, state.y);
        write_f32(rdram, out_ptr, 0x08, state.z);
        write_f32(rdram, out_ptr, 0x0C, state.yaw);
        write_f32(rdram, out_ptr, 0x10, state.pitch);
        write_f32(rdram, out_ptr, 0x14, state.scale);
        MEM_W(0x18, out_ptr) = state.map_id;
        MEM_HU(0x1C, out_ptr) = state.animation_id;
        write_f32(rdram, out_ptr, 0x20, state.anim_timer);
        write_f32(rdram, out_ptr, 0x24, state.anim_duration);
        MEM_BU(0x28, out_ptr) = state.anim_playback_type;
        MEM_BU(0x29, out_ptr) = state.health;
        MEM_BU(0x2A, out_ptr) = 0; // health_total
        MEM_BU(0x2B, out_ptr) = 0; // lives
        MEM_BU(0x2C, out_ptr) = state.transformation;
        MEM_BU(0x2D, out_ptr) = state.bs_state;
        MEM_BU(0x2E, out_ptr) = state.kazooie_flags;
        write_f32(rdram, out_ptr, 0x30, state.horizontal_velocity);
        write_f32(rdram, out_ptr, 0x34, state.anim_subrange_start);
        write_f32(rdram, out_ptr, 0x38, state.anim_subrange_end);
        _return(ctx, 1u);
    } else {
        _return(ctx, 0u);
    }
}

// Get number of connected remote players.
extern "C" void recomp_net_get_remote_count(uint8_t* rdram, recomp_context* ctx) {
    uint8_t count = bknet::NetworkManager::instance().player_count();
    _return(ctx, static_cast<u32>(count > 0 ? count - 1 : 0));
}

// Get local player ID.
extern "C" void recomp_net_get_local_player_id(uint8_t* rdram, recomp_context* ctx) {
    _return(ctx, static_cast<u32>(bknet::NetworkManager::instance().local_player_id()));
}

// Get number of recent chat messages.
extern "C" void recomp_net_get_chat_count(uint8_t* rdram, recomp_context* ctx) {
    auto msgs = bknet::NetworkManager::instance().get_chat_messages();
    _return(ctx, static_cast<u32>(msgs.size()));
}

// Check if chat input is active.
extern "C" void recomp_net_is_chat_active(uint8_t* rdram, recomp_context* ctx) {
    _return(ctx, bknet::ChatInput::instance().is_active() ? 1u : 0u);
}

// === World state sync (Phase 3) ===

// Send collectible event: type(r4), id(r5), collected(r6), map_id(r7), level_id(stack)
extern "C" void recomp_net_send_collectible(uint8_t* rdram, recomp_context* ctx) {
    u32 type = static_cast<u32>(ctx->r4);
    u32 id = static_cast<u32>(ctx->r5);
    u32 collected = static_cast<u32>(ctx->r6);
    u32 map_id = static_cast<u32>(ctx->r7);
    // level_id passed via stack (5th arg in MIPS o32 ABI)
    u32 level_id = MEM_W(0x10, ctx->r29);
    bknet::NetworkManager::instance().send_collectible(
        static_cast<uint8_t>(type), static_cast<uint16_t>(id),
        static_cast<uint8_t>(collected), map_id, static_cast<uint8_t>(level_id));
}

// Send enemy death: marker_type(r4), spawn_index(r5), map_id(r6)
extern "C" void recomp_net_send_enemy_death(uint8_t* rdram, recomp_context* ctx) {
    u32 marker_type = static_cast<u32>(ctx->r4);
    u32 spawn_index = static_cast<u32>(ctx->r5);
    u32 map_id = static_cast<u32>(ctx->r6);
    bknet::NetworkManager::instance().send_enemy_death(
        static_cast<uint16_t>(marker_type), static_cast<uint16_t>(spawn_index), map_id);
}

// Send flag change: flag_type(r4), flag_index(r5), value(r6), map_id(r7)
extern "C" void recomp_net_send_flag_change(uint8_t* rdram, recomp_context* ctx) {
    u32 flag_type = static_cast<u32>(ctx->r4);
    u32 flag_index = static_cast<u32>(ctx->r5);
    u32 value = static_cast<u32>(ctx->r6);
    u32 map_id = static_cast<u32>(ctx->r7);
    bknet::NetworkManager::instance().send_flag_change(
        static_cast<uint8_t>(flag_type), static_cast<uint16_t>(flag_index),
        static_cast<uint8_t>(value), map_id);
}

// Pop next world event from queue. Returns 1 if event available, 0 if empty.
// Writes event data to output struct at r4.
// Output layout (28 bytes):
//   0x00: u8  event_type (0=collectible, 1=enemy, 2=flag)
//   0x04: event-specific data (up to 24 bytes, matching packet layout without header)
extern "C" void recomp_net_pop_world_event(uint8_t* rdram, recomp_context* ctx) {
    gpr out_ptr = ctx->r4;
    bknet::NetworkManager::WorldEvent evt;
    if (bknet::NetworkManager::instance().pop_world_event(evt)) {
        MEM_BU(0x00, out_ptr) = static_cast<u32>(evt.type);
        switch (evt.type) {
            case bknet::NetworkManager::WorldEvent::COLLECTIBLE:
                MEM_BU(0x04, out_ptr) = evt.collectible.collectible_type;
                MEM_HU(0x06, out_ptr) = evt.collectible.collectible_id;
                MEM_BU(0x08, out_ptr) = evt.collectible.collected;
                MEM_W(0x0C, out_ptr) = evt.collectible.map_id;
                MEM_BU(0x10, out_ptr) = evt.collectible.level_id;
                break;
            case bknet::NetworkManager::WorldEvent::ENEMY:
                MEM_HU(0x04, out_ptr) = evt.enemy.marker_type;
                MEM_HU(0x06, out_ptr) = evt.enemy.spawn_index;
                MEM_W(0x08, out_ptr) = evt.enemy.map_id;
                MEM_BU(0x0C, out_ptr) = evt.enemy.alive;
                MEM_BU(0x0D, out_ptr) = evt.enemy.health;
                break;
            case bknet::NetworkManager::WorldEvent::FLAG:
                MEM_BU(0x04, out_ptr) = evt.flag.flag_type;
                MEM_HU(0x06, out_ptr) = evt.flag.flag_index;
                MEM_BU(0x08, out_ptr) = evt.flag.value;
                MEM_W(0x0C, out_ptr) = evt.flag.map_id;
                break;
        }
        _return(ctx, 1u);
    } else {
        _return(ctx, 0u);
    }
}
