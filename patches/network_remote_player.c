#include "patches.h"
#include "functions.h"
#include "bk_api.h"
#include "prop.h"
#include "enums.h"
#include "core2/anctrl.h"

// Networking bridge functions
u32 recomp_net_is_connected(void);
u32 recomp_net_get_remote_state(u32 player_id, void* out);
u32 recomp_net_get_remote_count(void);
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

// ============================================================
// Ghost Player - custom actor with Banjo model (same as before)
// ============================================================

#define MAX_REMOTE_PLAYERS 3
#define GHOST_DRAW_DISTANCE 5000

static Actor* ghost_actors[MAX_REMOTE_PLAYERS] = {0};
static u8 ghost_player_ids[MAX_REMOTE_PLAYERS] = {0};
static u16 ghost_last_anim[MAX_REMOTE_PLAYERS] = {0};
static u32 ghost_frame_counter = 0;

// No-op update - we control position externally
static void ghost_update(Actor* this) {
    // Do nothing - position set by update_ghost()
}

// Use standard actor draw
static Actor* ghost_draw(ActorMarker* marker, Gfx** gfx, Mtx** mtx, Vtx** vtx) {
    return actor_draw(marker, gfx, mtx, vtx);
}

static ActorInfo ghost_actor_info = {
    .markerId = 0,
    .actorId = 0,
    .modelId = 0x34D, // ASSET_34D_MODEL_BANJOKAZOOIE_LOW_POLY
    .startAnimation = 0x6F, // ASSET_6F_ANIM_BSSTAND_IDLE
    .animations = 0,
    .update_func = ghost_update,
    .update2_func = 0,
    .draw_func = ghost_draw,
    .unk18 = 0,
    .draw_distance = GHOST_DRAW_DISTANCE,
    .shadow_scale = 1.0f,
    .unk20 = 0,
};

static Actor* spawn_ghost(u8 remote_player_id) {
    s32 spawn_pos[3] = {0, 0, 0};

    // Get remote player position for initial spawn
    RemoteState rs;
    if (recomp_net_get_remote_state(remote_player_id, &rs)) {
        spawn_pos[0] = (s32)rs.x;
        spawn_pos[1] = (s32)rs.y;
        spawn_pos[2] = (s32)rs.z;
    }

    Actor* ghost = actor_new(spawn_pos, 0, &ghost_actor_info, 0);
    if (!ghost) {
        recomp_printf("[NetGhost] Failed to spawn for player %d\n", remote_player_id);
        return 0;
    }

    // Create AnimCtrl since actor_new with custom ActorInfo doesn't
    if (!ghost->anctrl) {
        AnimCtrl* ac = anctrl_new(1);
        if (ac) {
            ghost->anctrl = ac;
            anctrl_setIndex(ac, ASSET_6F_ANIM_BSSTAND_IDLE);
            anctrl_setPlaybackType(ac, ANIMCTRL_LOOP);
            anctrl_setDuration(ac, 1.0f);
            _anctrl_start(ac, __FILE__, __LINE__);
        }
    }

    actor_collisionOff(ghost);
    ghost->alpha_124_19 = 180; // semi-transparent
    ghost->scale = 1.0f;

    recomp_printf("[NetGhost] Spawned ghost for P%d (actor=%p anctrl=%p)\n",
                  remote_player_id, ghost, ghost->anctrl);
    return ghost;
}

static void despawn_ghost(u8 slot) {
    if (slot >= MAX_REMOTE_PLAYERS || !ghost_actors[slot]) return;
    if (ghost_actors[slot]->marker) {
        marker_despawn(ghost_actors[slot]->marker);
    }
    ghost_actors[slot] = 0;
    ghost_player_ids[slot] = 0;
    ghost_last_anim[slot] = 0;
}

static void update_ghost(u8 slot) {
    Actor* ghost = ghost_actors[slot];
    if (!ghost) return;

    u8 remote_pid = ghost_player_ids[slot];
    RemoteState rs;
    u32 is_active = recomp_net_get_remote_state(remote_pid, &rs);

    if (!is_active) {
        despawn_ghost(slot);
        return;
    }

    // Hide if on different map
    u32 local_map = (u32)map_get();
    if (rs.map_id != local_map) {
        ghost->position[0] = 0.0f;
        ghost->position[1] = -10000.0f;
        ghost->position[2] = 0.0f;
        return;
    }

    // Update position
    ghost->position[0] = rs.x;
    ghost->position[1] = rs.y;
    ghost->position[2] = rs.z;
    ghost->yaw = rs.yaw;

    // Update animation if changed
    if (ghost->anctrl && rs.animation_id != 0 && rs.animation_id != ghost_last_anim[slot]) {
        anctrl_setIndex(ghost->anctrl, rs.animation_id);
        anctrl_setDuration(ghost->anctrl, rs.anim_duration > 0.0f ? rs.anim_duration : 0.5f);
        anctrl_setPlaybackType(ghost->anctrl, rs.anim_playback_type);
        _anctrl_start(ghost->anctrl, __FILE__, __LINE__);
        ghost_last_anim[slot] = rs.animation_id;
    }
}

// ============================================================
// Per-frame management
// ============================================================

RECOMP_EXPORT void bkrecomp_net_manage_ghosts(void) {
    // TODO: Ghost actor rendering disabled - actor_new with custom ActorInfo
    // causes bus errors. Need to investigate proper actor spawning method
    // for the BK recomp system. Network sync and chat are working.
    return;

    if (!recomp_net_is_connected()) {
        for (int i = 0; i < MAX_REMOTE_PLAYERS; i++) {
            if (ghost_actors[i]) despawn_ghost(i);
        }
        return;
    }

    // Don't spawn during menus/loading
    f32 local_pos[3];
    player_getPosition(local_pos);
    u32 local_map = (u32)map_get();
    bool in_game = (local_map >= 1 && local_map <= 120 && local_pos[0] > -15000.0f);

    if (!in_game) {
        // Despawn all ghosts when leaving game levels
        for (int i = 0; i < MAX_REMOTE_PLAYERS; i++) {
            if (ghost_actors[i]) despawn_ghost(i);
        }
        return;
    }

    u32 local_id = recomp_net_get_local_player_id();
    ghost_frame_counter++;

    for (u8 pid = 0; pid < 4; pid++) {
        if (pid == local_id) continue;

        u8 slot = pid;
        if (pid > local_id) slot--;
        if (slot >= MAX_REMOTE_PLAYERS) continue;

        RemoteState rs;
        u32 is_active = recomp_net_get_remote_state(pid, &rs);
        bool remote_in_game = (is_active && rs.map_id >= 1 && rs.map_id <= 120 && rs.x > -15000.0f);

        if (remote_in_game && !ghost_actors[slot]) {
            ghost_actors[slot] = spawn_ghost(pid);
            ghost_player_ids[slot] = pid;
        } else if (!remote_in_game && ghost_actors[slot]) {
            despawn_ghost(slot);
        }

        if (ghost_actors[slot]) {
            update_ghost(slot);
        }
    }

    // Debug every ~3 seconds
    if ((ghost_frame_counter % 180) == 0) {
        for (int i = 0; i < MAX_REMOTE_PLAYERS; i++) {
            if (ghost_actors[i]) {
                recomp_printf("[Ghost] slot=%d pid=%d pos=(%.0f,%.0f,%.0f) anctrl=%p\n",
                    i, ghost_player_ids[i],
                    ghost_actors[i]->position[0],
                    ghost_actors[i]->position[1],
                    ghost_actors[i]->position[2],
                    ghost_actors[i]->anctrl);
            }
        }
    }
}

RECOMP_EXPORT void bkrecomp_net_ghost_init(void) {
    recomp_printf("[NetGhost] Ghost system initialized\n");
}
