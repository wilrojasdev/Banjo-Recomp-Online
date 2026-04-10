#include "patches.h"
#include "functions.h"
#include "enums.h"

extern enum map_e map_get(void);
extern s32 bs_getState(void);
extern u32 player_getTransformation(void);

// Full local player state struct passed to C++ side via pointer.
// Must match the layout expected in net_recomp_api.cpp.
typedef struct {
    f32 x, y, z;
    f32 yaw;
    f32 pitch;
    f32 scale;
    u32 map_id;
    u16 animation_id;
    f32 anim_timer;
    f32 anim_duration;
    u8  anim_playback_type;
    u8  health;
    u8  health_total;
    u8  lives;
    u8  transformation;
    u8  bs_state;
    u8  _pad[2]; // alignment
} NetFullState;

// Networking bridge functions (registered on C++ side via REGISTER_FUNC)
void recomp_net_push_full_state(NetFullState* state);
u32 recomp_net_is_connected(void);
u32 recomp_net_get_remote_state(u32 player_id, NetFullState* out);
u32 recomp_net_get_remote_count(void);
u32 recomp_net_get_local_player_id(void);

// @recomp Per-frame event for network state synchronization.
RECOMP_DECLARE_EVENT(recomp_on_net_frame_update());

// Internal: read full player state and push to network module
static void net_sync_local_state(void) {
    if (!recomp_net_is_connected()) {
        return;
    }

    NetFullState state;
    f32 pos[3];
    player_getPosition(pos);
    state.x = pos[0];
    state.y = pos[1];
    state.z = pos[2];
    state.yaw = yaw_get();
    state.pitch = 0.0f; // TODO: get pitch when available
    state.scale = 1.0f;
    state.map_id = (u32)map_get();
    state.bs_state = (u8)bs_getState();
    state.transformation = (u8)player_getTransformation();

    // Map BS state to animation asset ID
    s32 bs = bs_getState();
    u16 anim = ASSET_6F_ANIM_BSSTAND_IDLE; // default idle
    u8 playback = 2; // ANIMCTRL_LOOP
    switch (bs) {
        case BS_1_IDLE:       anim = ASSET_6F_ANIM_BSSTAND_IDLE; playback = 2; break;
        case BS_2_WALK_SLOW:  anim = ASSET_2_ANIM_BSWALK_CREEP;  playback = 2; break;
        case BS_WALK:         anim = ASSET_3_ANIM_BSWALK;         playback = 2; break;
        case BS_4_WALK_FAST:  anim = ASSET_C_ANIM_BSWALK_RUN;    playback = 2; break;
        case BS_5_JUMP:       anim = ASSET_8_ANIM_BSJUMP;         playback = 1; break;
        case BS_CLAW:         anim = ASSET_5_ANIM_BSPUNCH;        playback = 1; break;
        case BS_CROUCH:       anim = ASSET_1_ANIM_BSCROUCH_ENTER; playback = 1; break;
        case BS_F_BBUSTER:    anim = ASSET_1D_ANIM_BSBBUSTER;     playback = 1; break;
        case BS_BFLAP:        anim = ASSET_17_ANIM_BSBFLAP;       playback = 2; break;
        case BS_11_BPECK:     anim = ASSET_1A_ANIM_BSBPECK;       playback = 2; break;
        case BS_BBARGE:       anim = ASSET_1C_ANIM_BSBBARGE;      playback = 1; break;
        case BS_15_BTROT_IDLE: anim = ASSET_26_ANIM_BSBTROT_IDLE; playback = 2; break;
        case BS_16_BTROT_WALK: anim = ASSET_15_ANIM_BSBTROT_WALK; playback = 2; break;
        case BS_E_OW:         anim = ASSET_F_ANIM_BSREBOUND;      playback = 1; break;
        default:              anim = ASSET_6F_ANIM_BSSTAND_IDLE;  playback = 2; break;
    }
    state.animation_id = anim;
    state.anim_timer = 0.0f;
    state.anim_duration = 0.5f;
    state.anim_playback_type = playback;

    // Items: These require item_getCount which is a game function
    // For now, use placeholders until item access is resolved
    state.health = 0;
    state.health_total = 0;
    state.lives = 0;

    recomp_net_push_full_state(&state);
}

// Ghost management (defined in network_remote_player.c)
extern void bkrecomp_net_manage_ghosts(void);

static u32 debug_frame_counter = 0;

// @recomp Export: called from ncCamera_update each game frame.
RECOMP_EXPORT void bkrecomp_net_sync_frame(void) {
    net_sync_local_state();

    // Debug: log state every ~2 seconds (120 frames)
    debug_frame_counter++;
    if (recomp_net_is_connected() && (debug_frame_counter % 120) == 0) {
        u32 local_id = recomp_net_get_local_player_id();
        f32 pos[3];
        player_getPosition(pos);
        u32 map = (u32)map_get();
        recomp_printf("[NetDebug] local_id=%d map=%d pos=(%.1f,%.1f,%.1f)\n",
                      local_id, map, pos[0], pos[1], pos[2]);

        // Check remote players
        NetFullState rs;
        for (u32 pid = 0; pid < 4; pid++) {
            if (pid == local_id) continue;
            u32 active = recomp_net_get_remote_state(pid, &rs);
            if (active) {
                recomp_printf("[NetDebug] remote pid=%d active map=%d pos=(%.1f,%.1f,%.1f)\n",
                              pid, rs.map_id, rs.x, rs.y, rs.z);
            }
        }
    }

    // Manage ghost actors (spawn/despawn/update)
    bkrecomp_net_manage_ghosts();

    // Fire event for mods
    recomp_on_net_frame_update();
}
