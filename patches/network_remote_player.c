#include "patches.h"
#include "functions.h"
#include "enums.h"
#include "core2/modelRender.h"

// Networking bridge functions
u32 recomp_net_is_connected(void);
u32 recomp_net_get_remote_state(u32 player_id, void* out);
u32 recomp_net_get_local_player_id(void);

extern enum map_e map_get(void);

typedef struct {
    f32 x, y, z;
    f32 yaw;
    f32 pitch;
    f32 scale;
    u32 map_id;
    u16 animation_id;
    u8  _pad1[2];
    f32 anim_timer;
    f32 anim_duration;
    u8  anim_playback_type;
    u8  health;
    u8  health_total;
    u8  lives;
    u8  transformation;
    u8  bs_state;
    u8  _pad2[2];
} RemoteState;

// Globals from ba/model.c
extern void *baModelBin;
extern f32 baModelScale;
extern void func_8029A47C(s32 env_color[3]);
extern void func_8033A280(f32);
extern struct5Bs *D_80363780;
extern void func_8033A450(struct5Bs *);

// Called from player_draw AFTER baModel_draw.
// Draws the ghost using the model with the local player's current animation
// pose applied. Position and rotation come from the remote player's state.
// Animation is mirrored from local player (independent animation requires
// a separate model instance which is a future improvement).
void bkrecomp_net_draw_ghosts(Gfx **gfx, Mtx **mtx, Vtx **vtx) {
    if (!recomp_net_is_connected()) return;
    if (!baModelBin) return;

    u32 local_id = recomp_net_get_local_player_id();
    u32 local_map = (u32)map_get();

    for (u32 pid = 0; pid < 4; pid++) {
        if (pid == local_id) continue;

        RemoteState rs;
        if (!recomp_net_get_remote_state(pid, &rs)) continue;
        if (rs.map_id != local_map) continue;
        if (rs.x < -15000.0f) continue;

        f32 ghost_pos[3];
        ghost_pos[0] = rs.x;
        ghost_pos[1] = rs.y + 2.0f;
        ghost_pos[2] = rs.z;

        f32 ghost_rot[3];
        ghost_rot[0] = 0.0f;
        ghost_rot[1] = rs.yaw;
        ghost_rot[2] = 0.0f;

        f32 ghost_sp38[3];
        ghost_sp38[0] = rs.x;
        ghost_sp38[1] = rs.y;
        ghost_sp38[2] = rs.z;

        s32 env_color[3];
        func_8029A47C(env_color);
        modelRender_setEnvColor(env_color[0], env_color[1], env_color[2], 255);
        func_8033A280(2.0f);
        func_8033A450(D_80363780);
        modelRender_setDepthMode(MODEL_RENDER_DEPTH_FULL);
        modelRender_draw(gfx, mtx, ghost_pos, ghost_rot, baModelScale, ghost_sp38, baModelBin);
    }
}

RECOMP_EXPORT void bkrecomp_net_manage_ghosts(void) {
}

RECOMP_EXPORT void bkrecomp_net_ghost_init(void) {
    recomp_printf("[NetGhost] Ghost system initialized (render mode)\n");
}
