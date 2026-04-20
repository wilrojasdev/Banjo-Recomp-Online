#include "patches.h"
#include "functions.h"
#include "enums.h"
#include <math.h>

/* Pad (0x23D) state machine sync.
 *
 * The original func_802D4388 only reads the LOCAL player's collision with
 * the pad via func_8028FB48, so when a remote ghost stands on the pad the
 * owner (or any other client) never sees the press animation. Extend the
 * "player on pad" check with a proximity test against remote player
 * positions so every client animates the pad whenever ANY player is on it.
 *
 * Scene independence: pads only play a local SFX and state transition —
 * they are NOT blocking cutscenes, so all clients should see them animate.
 */

/* Collision test — the decomp declares this void, but at the recompiled
 * MIPS level it returns u32 via $v0 (bits matching the mask). Declare it
 * with u32 here so the C compiler emits a call that actually uses the
 * returned value. */
u32  func_8028FB48(u32 mask);
extern s32  func_8028F20C(void);
extern s32  player_movementGroup(void);
extern bool func_802D42F8(Actor *this);
extern void func_802D3CE8(Actor *this);
extern enum map_e map_get(void);
extern s32 item_getCount(enum item_e item);
extern s32 mapSpecificFlags_get(s32);

/* Network bridges */
u32  recomp_net_is_connected(void);
u32  recomp_net_get_remote_state(u32 player_id, void *out);
u32  recomp_net_get_local_player_id(void);

typedef struct {
    f32 x, y, z, yaw, pitch, scale;
    u32 map_id;
    u16 animation_id;
    u8  _pad1[2];
    f32 anim_timer, anim_duration;
    u8  anim_playback_type, health, health_total, lives, transformation, bs_state;
    u8  kazooie_flags;
    u8  _pad2;
    f32 horizontal_velocity;
    f32 anim_subrange_start;
    f32 anim_subrange_end;
} RemoteState;

#define PAD_HORIZ_RADIUS_SQ  (100.0f * 100.0f)
#define PAD_VERT_RANGE        120.0f

static bool net_any_ghost_on_pad(Actor *this) {
    u32 local_id, i, cur_map;
    RemoteState rs;

    if (!recomp_net_is_connected()) return FALSE;

    local_id = recomp_net_get_local_player_id();
    cur_map = (u32)map_get();

    for (i = 0; i < 4; i++) {
        f32 dx, dz, dy;
        if (i == local_id) continue;
        if (!recomp_net_get_remote_state(i, &rs)) continue;
        if (rs.map_id != cur_map) continue;

        dx = rs.x - this->position[0];
        dz = rs.z - this->position[2];
        if (SQ(dx) + SQ(dz) >= PAD_HORIZ_RADIUS_SQ) continue;

        dy = rs.y - this->position[1];
        if (dy < -10.0f || dy > PAD_VERT_RANGE) continue;

        return TRUE;
    }
    return FALSE;
}

/* Replicates the decomp's func_802D4388 (pad update) with a ghost
 * proximity check added to the "player on pad" signal. */
RECOMP_PATCH void func_802D4388(Actor *this) {
    u32 local_collision;
    bool anyone_on_pad;

    func_802D3CE8(this);

    this->unk38_0 = (map_get() == MAP_7A_GL_CRYPT
        || item_getCount(ITEM_1C_MUMBO_TOKEN) >= (s32)this->actorTypeSpecificField
        || func_802D42F8(this)) ? TRUE : FALSE;

    /* Local player collision against the pad node set. Mask 0x78000000
     * covers the pad collision bits. */
    local_collision = func_8028FB48(0x78000000);

    anyone_on_pad = (func_8028F20C() && local_collision)
                  || player_movementGroup() == BSGROUP_D_TRANSFORMING
                  || net_any_ghost_on_pad(this);

    mapSpecificFlags_set(0x1F, anyone_on_pad);

    switch (this->state) {
        case 0x12:
            if (this->unk38_0 && mapSpecificFlags_get(0x1F)) {
                subaddie_set_state_with_direction(this, 0x13, 0.0f, 1);
                actor_playAnimationOnce(this);
                func_8030E6D4(SFX_90_SWITCH_PRESS);
            }
            break;

        case 0x13:
            if (0.66f <= anctrl_getAnimTimer(this->anctrl)) {
                subaddie_set_state_with_direction(this, 0x14, 0.66f, 0);
            }
            break;

        case 0x14:
            if (!this->unk38_0 || !mapSpecificFlags_get(0x1F)) {
                subaddie_set_state_with_direction(this, 0x15, 0.66f, 0);
                actor_playAnimationOnce(this);
            }
            break;

        case 0x15:
            if (anctrl_getAnimTimer(this->anctrl) < 0.03f) {
                subaddie_set_state_with_direction(this, 0x12, 0.0f, 1);
            }
            break;
    }

    mapSpecificFlags_set(0x1F, FALSE);
}
