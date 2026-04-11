#include "patches.h"
#include "functions.h"
#include "enums.h"
#include "core2/anctrl.h"

extern AnimCtrl *baanim_getAnimCtrlPtr(void);
extern Animation *anctrl_getAnimPtr(AnimCtrl *this);
extern f32 anctrl_getDuration(AnimCtrl *this);

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

    // Log the REAL animation the local player is using
    static u16 last_logged_anim = 0;
    static u8 last_logged_bs = 0;
    AnimCtrl *ac = baanim_getAnimCtrlPtr();
    if (ac) {
        u16 real_anim = anctrl_getIndex(ac);
        f32 real_dur = anctrl_getDuration(ac);
        u8 real_playback = anctrl_getPlaybackType(ac);
        u8 cur_bs = state.bs_state;
        if (real_anim != last_logged_anim || cur_bs != last_logged_bs) {
            const char *type = "?";
            switch(real_playback) {
                case 1: type = "ONCE"; break;
                case 2: type = "LOOP"; break;
                case 3: type = "STOP"; break;
                case 4: type = "SUBLOOP"; break;
            }
            recomp_printf("[LocalAnim] BS=0x%02X realAnim=0x%03X dur=%.2f %s\n", cur_bs, real_anim, real_dur, type);
            last_logged_anim = real_anim;
            last_logged_bs = cur_bs;
        }
    }
}

// Ghost management (defined in network_remote_player.c)
extern void bkrecomp_net_manage_ghosts(void);

// @recomp Export: called from ncCamera_update each game frame.
RECOMP_EXPORT void bkrecomp_net_sync_frame(void) {
    net_sync_local_state();


    // Manage ghost actors (spawn/despawn/update)
    bkrecomp_net_manage_ghosts();

    // Fire event for mods
    recomp_on_net_frame_update();
}
