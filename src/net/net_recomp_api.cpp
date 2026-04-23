#include <cstdio>
#include <cstring>
#include "recomp.h"
#include "librecomp/helpers.hpp"
#include "net_manager.h"
#include "net_chat.h"
#include "net_nametag_ui.h"

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

// Push level_id separately (called after push_full_state, avoids changing MIPS struct layout)
extern "C" void recomp_net_push_level_id(uint8_t* rdram, recomp_context* ctx) {
    u32 level_id = static_cast<u32>(ctx->r4);
    bknet::NetworkManager::instance().set_local_level_id(level_id);
}

// Push camera state from game thread for nametag projection.
// Args: cam_data_ptr(r4) - pointer to struct:
//   0x00: f32 pos[3]           (12 bytes)
//   0x0C: f32 rot[3]           (12 bytes) - pitch, yaw, roll
//   0x18: f32 fov_y            (4 bytes)
//   0x1C: f32 near             (4 bytes)
//   0x20: s32 fb_width         (4 bytes)
//   0x24: s32 fb_height        (4 bytes)
//   0x28: f32 viewport_aspect  (4 bytes)
//   0x2C: u32 map_id           (4 bytes)
extern "C" void recomp_net_push_camera_state(uint8_t* rdram, recomp_context* ctx) {
    gpr data_ptr = ctx->r4;

    bknet::CameraState cam{};
    cam.position[0] = read_f32(rdram, data_ptr, 0x00);
    cam.position[1] = read_f32(rdram, data_ptr, 0x04);
    cam.position[2] = read_f32(rdram, data_ptr, 0x08);
    cam.rotation[0] = read_f32(rdram, data_ptr, 0x0C);
    cam.rotation[1] = read_f32(rdram, data_ptr, 0x10);
    cam.rotation[2] = read_f32(rdram, data_ptr, 0x14);
    cam.fov_y       = read_f32(rdram, data_ptr, 0x18);
    cam.near_plane  = read_f32(rdram, data_ptr, 0x1C);
    cam.framebuffer_width  = static_cast<int>(MEM_W(0x20, data_ptr));
    cam.framebuffer_height = static_cast<int>(MEM_W(0x24, data_ptr));
    cam.viewport_aspect = read_f32(rdram, data_ptr, 0x28);
    cam.map_id = MEM_W(0x2C, data_ptr);
    cam.valid = true;

    bknet::nametag_set_camera_state(cam);
}

// Host-side debug log bridge. Writes to /tmp/bk64_debug.log so the user can
// `tail -f` the file regardless of how the app was launched (Finder or
// Terminal). recomp_printf in the MIPS runtime is a no-op from the original
// ROM's osSyncPrintf, so any visible debug has to go through here.
extern "C" void bknet_debug_log(uint8_t* rdram, recomp_context* ctx) {
    uint32_t tag = (uint32_t)ctx->r4;
    int32_t  a   = (int32_t) ctx->r5;
    int32_t  b   = (int32_t) ctx->r6;
    int32_t  c   = (int32_t) ctx->r7;
    static FILE* f = nullptr;
    if (!f) {
        f = fopen("/tmp/bk64_debug.log", "a");
        if (f) {
            fprintf(f, "=== BK64 log opened ===\n");
            fflush(f);
        }
    }
    if (f) {
        fprintf(f, "[BKLOG %u] a=%d b=%d c=%d\n", tag, a, b, c);
        fflush(f);
    }
    // Also go to stderr for Terminal launches.
    fprintf(stderr, "[BKLOG %u] a=%d b=%d c=%d\n", tag, a, b, c);
    fflush(stderr);
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

// Returns 1 if online mode is configured, 0 otherwise.
extern "C" void recomp_net_is_online_mode(uint8_t* rdram, recomp_context* ctx) {
    _return(ctx, bknet::get_config().mode != bknet::NetworkMode::Off ? 1u : 0u);
}

// Returns the selected save slot (0-2). Set from launcher Host submenu.
extern "C" void recomp_net_get_save_slot(uint8_t* rdram, recomp_context* ctx) {
    _return(ctx, static_cast<u32>(bknet::get_config().save_slot));
}

// Returns 1 if join mode (either ENet LAN or CoopNet WAN), 0 if host or off.
// NetworkMode has 5 values — originally only Host/Join for ENet; CoopNet added
// CoopNetHost/CoopNetJoin later. The old exact-equality check against ::Join
// silently failed for every CoopNet joiner, so the save-override/autoload
// join branches were bypassed (join was treated like host/off).
extern "C" void recomp_net_is_join_mode(uint8_t* rdram, recomp_context* ctx) {
    auto m = bknet::get_config().mode;
    bool is_join = (m == bknet::NetworkMode::Join)
                || (m == bknet::NetworkMode::CoopNetJoin);
    _return(ctx, is_join ? 1u : 0u);
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

// Send enemy death: marker_type(r4), spawn_index(r5), map_id(r6), pos_ptr(r7)
extern "C" void recomp_net_send_enemy_death(uint8_t* rdram, recomp_context* ctx) {
    u32 marker_type = static_cast<u32>(ctx->r4);
    u32 spawn_index = static_cast<u32>(ctx->r5);
    u32 map_id = static_cast<u32>(ctx->r6);
    gpr pos_ptr = ctx->r7;
    float px = read_f32(rdram, pos_ptr, 0x00);
    float py = read_f32(rdram, pos_ptr, 0x04);
    float pz = read_f32(rdram, pos_ptr, 0x08);
    bknet::NetworkManager::instance().send_enemy_death(
        static_cast<uint16_t>(marker_type), static_cast<uint16_t>(spawn_index),
        map_id, px, py, pz);
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

// === World ownership ===

// Returns 1 if local player owns the given level, 0 otherwise. Args: level_id(r4)
extern "C" void recomp_net_am_i_world_owner(uint8_t* rdram, recomp_context* ctx) {
    u32 level_id = static_cast<u32>(ctx->r4);
    _return(ctx, bknet::NetworkManager::instance().am_i_world_owner(level_id) ? 1u : 0u);
}

// recomp_net_send_owner_transfer, recomp_net_pop_owner_transfer,
// recomp_net_should_resend_kills — REMOVED (centralized kill tracking in C++)

// === Enemy position sync ===

// Returns 1 if hosting, 0 otherwise
extern "C" void recomp_net_is_host(uint8_t* rdram, recomp_context* ctx) {
    _return(ctx, bknet::NetworkManager::instance().is_host() ? 1u : 0u);
}

// Send enemy positions from host: buffer_ptr(r4), count(r5), map_id(r6)
// MIPS layout per entry (20 bytes): u16 spawn_index, u16 marker_id, f32 x, f32 y, f32 z, f32 yaw
extern "C" void recomp_net_send_enemy_positions(uint8_t* rdram, recomp_context* ctx) {
    gpr buf_ptr = ctx->r4;
    u32 count = static_cast<u32>(ctx->r5);
    u32 map_id = static_cast<u32>(ctx->r6);

    if (count == 0 || count > bknet::MAX_ENEMIES_PER_PACKET) return;

    std::vector<bknet::EnemyPositionEntry> entries(count);
    for (u32 i = 0; i < count; i++) {
        gpr entry_addr = buf_ptr + i * 28;
        entries[i].spawn_index = static_cast<uint16_t>(MEM_HU(0x00, entry_addr));
        entries[i].marker_type = static_cast<uint16_t>(MEM_HU(0x02, entry_addr));
        entries[i].x = read_f32(rdram, entry_addr, 0x04);
        entries[i].y = read_f32(rdram, entry_addr, 0x08);
        entries[i].z = read_f32(rdram, entry_addr, 0x0C);
        entries[i].yaw = read_f32(rdram, entry_addr, 0x10);
        entries[i].anim_id = static_cast<uint16_t>(MEM_HU(0x14, entry_addr));
        entries[i].anim_timer = read_f32(rdram, entry_addr, 0x18);
    }

    bknet::NetworkManager::instance().send_enemy_positions(entries.data(), static_cast<uint8_t>(count), map_id);
}

// Get interpolated enemy positions for join: buffer_ptr(r4), count_ptr(r5)
// Writes entries to MIPS memory, sets count at count_ptr
extern "C" void recomp_net_get_enemy_positions(uint8_t* rdram, recomp_context* ctx) {
    gpr buf_ptr = ctx->r4;
    gpr count_ptr = ctx->r5;

    bknet::EnemyInterpolatedState states[bknet::MAX_ENEMIES_PER_PACKET];
    size_t count = bknet::NetworkManager::instance().get_enemy_positions(states, bknet::MAX_ENEMIES_PER_PACKET);

    for (size_t i = 0; i < count; i++) {
        gpr entry_addr = buf_ptr + i * 28;
        MEM_HU(0x00, entry_addr) = states[i].spawn_index;
        MEM_HU(0x02, entry_addr) = states[i].marker_type;
        write_f32(rdram, entry_addr, 0x04, states[i].x);
        write_f32(rdram, entry_addr, 0x08, states[i].y);
        write_f32(rdram, entry_addr, 0x0C, states[i].z);
        write_f32(rdram, entry_addr, 0x10, states[i].yaw);
        MEM_HU(0x14, entry_addr) = states[i].anim_id;
        write_f32(rdram, entry_addr, 0x18, states[i].anim_timer);
    }

    MEM_W(0x00, count_ptr) = static_cast<u32>(count);
}

// === Full state sync ===

// Check if host needs to send full sync. Returns 1 + writes player_id to r4 ptr.
extern "C" void recomp_net_should_send_full_sync(uint8_t* rdram, recomp_context* ctx) {
    gpr out_ptr = ctx->r4;
    uint8_t player_id;
    if (bknet::NetworkManager::instance().should_send_full_sync(player_id)) {
        MEM_BU(0x00, out_ptr) = player_id;
        _return(ctx, 1u);
    } else {
        _return(ctx, 0u);
    }
}

// Send WorldStateFull packet: data_ptr(r4), size(r5), target_player(r6)
extern "C" void recomp_net_send_world_state_full(uint8_t* rdram, recomp_context* ctx) {
    gpr data_ptr = ctx->r4;
    u32 size = static_cast<u32>(ctx->r5);
    u32 target = static_cast<u32>(ctx->r6);

    // Build packet from MIPS memory
    bknet::WorldStateFullPacket pkt{};
    pkt.header.type = bknet::PacketType::WorldStateFull;
    pkt.header.player_id = bknet::NetworkManager::instance().local_player_id();
    pkt.header.sequence = 0;

    // Read fields from MIPS: layout must match MIPS struct (WorldStateFullData)
    pkt.map_id = MEM_W(0x00, data_ptr);
    pkt.level_id = MEM_BU(0x04, data_ptr);
    // jiggy_score at 0x08 (13 bytes)
    for (int i = 0; i < 13; i++) pkt.jiggy_score[i] = MEM_BU(0x08 + i, data_ptr);
    // mumbo_score at 0x15 (16 bytes)
    for (int i = 0; i < 16; i++) pkt.mumbo_score[i] = MEM_BU(0x15 + i, data_ptr);
    // honeycomb_score at 0x25 (3 bytes)
    for (int i = 0; i < 3; i++) pkt.honeycomb_score[i] = MEM_BU(0x25 + i, data_ptr);
    pkt.jinjo_bits = MEM_BU(0x28, data_ptr);
    pkt.picked_bullion_mask = MEM_BU(0x29, data_ptr);
    pkt.note_count = static_cast<uint16_t>(MEM_HU(0x2A, data_ptr));
    pkt.lives = MEM_BU(0x2C, data_ptr);
    pkt.bullions = MEM_BU(0x2D, data_ptr);
    // Flag arrays at 0x2E
    for (int i = 0; i < 37; i++) pkt.file_progress_flags[i] = MEM_BU(0x2E + i, data_ptr);
    for (int i = 0; i < 8; i++) pkt.level_specific_flags[i] = MEM_BU(0x53 + i, data_ptr);
    for (int i = 0; i < 25; i++) pkt.volatile_flags[i] = MEM_BU(0x5B + i, data_ptr);
    pkt.map_specific_flags = MEM_W(0x74, data_ptr);
    pkt.has_flags = MEM_BU(0x78, data_ptr);
    // Abilities at 0x7C (8 bytes)
    for (int i = 0; i < 8; i++) pkt.abilities[i] = MEM_BU(0x7C + i, data_ptr);
    // Note sync (Phase 13): note_scores at 0x84 (11 bytes), level_notes at 0x90 (9*32 bytes)
    for (int i = 0; i < 11; i++) pkt.note_scores[i] = MEM_BU(0x84 + i, data_ptr);
    for (int lvl = 0; lvl < 9; lvl++) {
        for (int b = 0; b < 32; b++) {
            pkt.level_notes[lvl][b] = MEM_BU(0x90 + lvl * 32 + b, data_ptr);
        }
    }

    bknet::NetworkManager::instance().send_world_state_full(
        reinterpret_cast<const uint8_t*>(&pkt), sizeof(pkt), static_cast<uint8_t>(target));
}

// Pop full state event from separate queue. Returns 1 if available.
extern "C" void recomp_net_pop_full_state(uint8_t* rdram, recomp_context* ctx) {
    gpr out_ptr = ctx->r4;
    bknet::WorldStateFullPacket pkt;
    if (bknet::NetworkManager::instance().pop_full_state(pkt)) {
        MEM_W(0x00, out_ptr) = pkt.map_id;
        MEM_BU(0x04, out_ptr) = pkt.level_id;
        for (int i = 0; i < 13; i++) MEM_BU(0x08 + i, out_ptr) = pkt.jiggy_score[i];
        for (int i = 0; i < 16; i++) MEM_BU(0x15 + i, out_ptr) = pkt.mumbo_score[i];
        for (int i = 0; i < 3; i++) MEM_BU(0x25 + i, out_ptr) = pkt.honeycomb_score[i];
        MEM_BU(0x28, out_ptr) = pkt.jinjo_bits;
        MEM_BU(0x29, out_ptr) = pkt.picked_bullion_mask;
        MEM_HU(0x2A, out_ptr) = pkt.note_count;
        MEM_BU(0x2C, out_ptr) = pkt.lives;
        MEM_BU(0x2D, out_ptr) = pkt.bullions;
        // Flag arrays
        for (int i = 0; i < 37; i++) MEM_BU(0x2E + i, out_ptr) = pkt.file_progress_flags[i];
        for (int i = 0; i < 8; i++) MEM_BU(0x53 + i, out_ptr) = pkt.level_specific_flags[i];
        for (int i = 0; i < 25; i++) MEM_BU(0x5B + i, out_ptr) = pkt.volatile_flags[i];
        MEM_W(0x74, out_ptr) = pkt.map_specific_flags;
        MEM_BU(0x78, out_ptr) = pkt.has_flags;
        // Abilities at 0x7C (8 bytes)
        for (int i = 0; i < 8; i++) MEM_BU(0x7C + i, out_ptr) = pkt.abilities[i];
        // Note sync (Phase 13)
        for (int i = 0; i < 11; i++) MEM_BU(0x84 + i, out_ptr) = pkt.note_scores[i];
        for (int lvl = 0; lvl < 9; lvl++) {
            for (int b = 0; b < 32; b++) {
                MEM_BU(0x90 + lvl * 32 + b, out_ptr) = pkt.level_notes[lvl][b];
            }
        }
        _return(ctx, 1u);
    } else {
        _return(ctx, 0u);
    }
}

// === Host EEPROM snapshot (SM64 Coop DX-style save override) ===

// Host polls this; when true, reads real EEPROM and calls send_host_eeprom.
// r4 = out pointer for target_player_id (u8).
extern "C" void recomp_net_should_send_host_eeprom(uint8_t* rdram, recomp_context* ctx) {
    gpr out_ptr = ctx->r4;
    uint8_t player_id;
    if (bknet::NetworkManager::instance().should_send_host_eeprom(player_id)) {
        MEM_BU(0x00, out_ptr) = player_id;
        _return(ctx, 1u);
    } else {
        _return(ctx, 0u);
    }
}

// Host: ship EEPROM snapshot.
//   r4 = data_ptr (u8[2048])
//   r5 = size (u32, expected 2048)
//   r6 = target_player (u8)
//   r7 = current_slot (s32, host's active save slot 0..2)
extern "C" void recomp_net_send_host_eeprom(uint8_t* rdram, recomp_context* ctx) {
    gpr data_ptr = ctx->r4;
    u32 size = static_cast<u32>(ctx->r5);
    u32 target = static_cast<u32>(ctx->r6);
    int16_t slot = static_cast<int16_t>(ctx->r7);

    if (size > bknet::HOST_EEPROM_SIZE) size = static_cast<u32>(bknet::HOST_EEPROM_SIZE);

    uint8_t buffer[bknet::HOST_EEPROM_SIZE] = {};
    for (u32 i = 0; i < size; i++) {
        buffer[i] = MEM_BU(i, data_ptr);
    }

    bknet::NetworkManager::instance().send_host_eeprom(buffer, size, static_cast<uint8_t>(target), slot);
}

// Join: consume a queued HostEeprom into the override buffer.
//   r4 = out_ptr (u8[2048])
//   r5 = max_size (u32)
//   r6 = out_slot_ptr (s16*, receives host's active slot)
// Returns 1 if a packet was consumed, 0 if queue empty.
extern "C" void recomp_net_pop_host_eeprom(uint8_t* rdram, recomp_context* ctx) {
    gpr out_ptr = ctx->r4;
    u32 max_size = static_cast<u32>(ctx->r5);
    gpr out_slot_ptr = ctx->r6;
    bknet::HostEepromPacket pkt;
    if (bknet::NetworkManager::instance().pop_host_eeprom(pkt)) {
        u32 copy = (max_size < bknet::HOST_EEPROM_SIZE) ? max_size : static_cast<u32>(bknet::HOST_EEPROM_SIZE);
        for (u32 i = 0; i < copy; i++) {
            MEM_BU(i, out_ptr) = pkt.eeprom[i];
        }
        if (out_slot_ptr != 0) {
            MEM_HU(0x00, out_slot_ptr) = static_cast<u16>(pkt.current_slot);
        }
        _return(ctx, 1u);
    } else {
        _return(ctx, 0u);
    }
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
                MEM_BU(0x01, out_ptr) = evt.collectible.header.player_id; // 0xFE = resync
                MEM_BU(0x04, out_ptr) = evt.collectible.collectible_type;
                MEM_HU(0x06, out_ptr) = evt.collectible.collectible_id;
                MEM_BU(0x08, out_ptr) = evt.collectible.collected;
                MEM_W(0x0C, out_ptr) = evt.collectible.map_id;
                MEM_BU(0x10, out_ptr) = evt.collectible.level_id;
                write_f32(rdram, out_ptr, 0x14, evt.collectible.pos_x);
                write_f32(rdram, out_ptr, 0x18, evt.collectible.pos_y);
                write_f32(rdram, out_ptr, 0x1C, evt.collectible.pos_z);
                break;
            case bknet::NetworkManager::WorldEvent::ENEMY:
                MEM_BU(0x01, out_ptr) = evt.enemy.header.player_id; // 0xFF = resync (silent)
                MEM_HU(0x04, out_ptr) = evt.enemy.marker_type;
                MEM_HU(0x06, out_ptr) = evt.enemy.spawn_index;
                MEM_W(0x08, out_ptr) = evt.enemy.map_id;
                MEM_BU(0x0C, out_ptr) = evt.enemy.alive;
                MEM_BU(0x0D, out_ptr) = evt.enemy.health;
                write_f32(rdram, out_ptr, 0x10, evt.enemy.pos_x);
                write_f32(rdram, out_ptr, 0x14, evt.enemy.pos_y);
                write_f32(rdram, out_ptr, 0x18, evt.enemy.pos_z);
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

// === Conga orange projectile sync ===

// Send orange spawn event from world owner.
// Args: r4=spawn_pos_ptr (3 floats), r5=vel_ptr (3 floats), r6=map_id
extern "C" void recomp_net_send_conga_orange(uint8_t* rdram, recomp_context* ctx) {
    gpr spawn_ptr = ctx->r4;
    gpr vel_ptr = ctx->r5;
    uint32_t map_id = ctx->r6;

    float sx = read_f32(rdram, spawn_ptr, 0x00);
    float sy = read_f32(rdram, spawn_ptr, 0x04);
    float sz = read_f32(rdram, spawn_ptr, 0x08);
    float vx = read_f32(rdram, vel_ptr, 0x00);
    float vy = read_f32(rdram, vel_ptr, 0x04);
    float vz = read_f32(rdram, vel_ptr, 0x08);

    bknet::NetworkManager::instance().send_conga_orange(sx, sy, sz, vx, vy, vz, map_id);
}

// Pop orange spawn event on non-owner side.
// Args: r4=out_ptr (6 floats: spawn_x/y/z + vel_x/y/z)
// Returns: 1 if event popped, 0 if empty
extern "C" void recomp_net_pop_conga_orange(uint8_t* rdram, recomp_context* ctx) {
    gpr out_ptr = ctx->r4;

    bknet::NetworkManager::CongaOrangeEvent evt;
    if (bknet::NetworkManager::instance().pop_conga_orange(evt)) {
        write_f32(rdram, out_ptr, 0x00, evt.spawn_x);
        write_f32(rdram, out_ptr, 0x04, evt.spawn_y);
        write_f32(rdram, out_ptr, 0x08, evt.spawn_z);
        write_f32(rdram, out_ptr, 0x0C, evt.vel_x);
        write_f32(rdram, out_ptr, 0x10, evt.vel_y);
        write_f32(rdram, out_ptr, 0x14, evt.vel_z);
        _return(ctx, 1u);
    } else {
        _return(ctx, 0u);
    }
}
