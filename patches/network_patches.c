#include "patches.h"
#include "functions.h"
#include "enums.h"
#include "core2/anctrl.h"
#include "core2/particle.h"

extern AnimCtrl *baanim_getAnimCtrlPtr(void);
extern Animation *anctrl_getAnimPtr(AnimCtrl *this);
extern f32 anctrl_getDuration(AnimCtrl *this);
extern f32 anctrl_getAnimTimer(AnimCtrl *this);
extern enum asset_e anctrl_getIndex(AnimCtrl *this);
extern enum anctrl_playback_e anctrl_getPlaybackType(AnimCtrl *this);
extern void anctrl_getSubRange(AnimCtrl *this, f32 *startPtr, f32 *endPtr);

extern enum map_e map_get(void);
extern s32 bs_getState(void);
extern u32 player_getTransformation(void);
extern f32 baphysics_get_horizontal_velocity(void);

// Kazooie visibility globals (from core2/code_16C60.c)
extern u8 D_8037D235; // Kazooie feet
extern u8 D_8037D236; // Kazooie wings
extern u8 D_8037D238; // Kazooie head

// Full local player state struct passed to C++ side via pointer.
// Must match the layout expected in net_recomp_api.cpp.
typedef struct {
    f32 x, y, z;                // 0x00
    f32 yaw;                    // 0x0C
    f32 pitch;                  // 0x10
    f32 scale;                  // 0x14
    u32 map_id;                 // 0x18
    u16 animation_id;           // 0x1C
    f32 anim_timer;             // 0x20
    f32 anim_duration;          // 0x24
    u8  anim_playback_type;     // 0x28
    u8  health;                 // 0x29
    u8  health_total;           // 0x2A
    u8  lives;                  // 0x2B
    u8  transformation;         // 0x2C
    u8  bs_state;               // 0x2D
    u8  kazooie_flags;          // 0x2E — bit0=head, bit1=wings, bit2=feet
    u8  _pad;                   // 0x2F
    f32 horizontal_velocity;    // 0x30
    f32 anim_subrange_start;    // 0x34
    f32 anim_subrange_end;      // 0x38
} NetFullState; // 0x3C (60 bytes)

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
    state.pitch = 0.0f;
    state.scale = 1.0f;
    state.map_id = (u32)map_get();
    state.bs_state = (u8)bs_getState();
    state.transformation = (u8)player_getTransformation();
    state.horizontal_velocity = baphysics_get_horizontal_velocity();

    AnimCtrl *ac = baanim_getAnimCtrlPtr();
    state.animation_id = (u16)anctrl_getIndex(ac);
    state.anim_timer = anctrl_getAnimTimer(ac);
    state.anim_duration = anctrl_getDuration(ac);
    state.anim_playback_type = (u8)anctrl_getPlaybackType(ac);

    f32 sub_start = 0.0f, sub_end = 1.0f;
    anctrl_getSubRange(ac, &sub_start, &sub_end);
    state.anim_subrange_start = sub_start;
    state.anim_subrange_end = sub_end;

    state.kazooie_flags = (D_8037D238 ? 1 : 0)
                        | (D_8037D236 ? 2 : 0)
                        | (D_8037D235 ? 4 : 0);

    state.health = 0;
    state.health_total = 0;
    state.lives = 0;

    recomp_net_push_full_state(&state);
}


// Ghost management (defined in network_remote_player.c)
extern void bkrecomp_net_manage_ghosts(void);
// World state sync (defined in network_world_sync.c)
extern void bkrecomp_net_process_world_events(void);

// @recomp Export: called from ncCamera_update each game frame.
RECOMP_EXPORT void bkrecomp_net_sync_frame(void) {
    net_sync_local_state();

    bkrecomp_net_process_world_events();
    bkrecomp_net_manage_ghosts();
    recomp_on_net_frame_update();
}
