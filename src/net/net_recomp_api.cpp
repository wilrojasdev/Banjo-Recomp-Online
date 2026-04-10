#include <cstring>
#include "recomp.h"
#include "librecomp/helpers.hpp"
#include "net_manager.h"
#include "net_chat.h"

// These functions are called from game patches (game thread) via REGISTER_FUNC.

// NetFullState struct layout (must match patches/network_patches.c):
// Offset  Field               Size
// 0x00    f32 x               4
// 0x04    f32 y               4
// 0x08    f32 z               4
// 0x0C    f32 yaw             4
// 0x10    f32 pitch           4
// 0x14    f32 scale           4
// 0x18    u32 map_id          4
// 0x1C    u16 animation_id    2
// 0x1E    (pad)               2
// 0x20    f32 anim_timer      4
// 0x24    f32 anim_duration   4
// 0x28    u8  anim_playback   1
// 0x29    u8  health          1
// 0x2A    u8  health_total    1
// 0x2B    u8  lives           1
// 0x2C    u8  transformation  1
// 0x2D    u8  bs_state        1
// 0x2E    u8  _pad[2]         2
// Total: 0x30 (48 bytes)

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
