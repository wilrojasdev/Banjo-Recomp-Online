#include "patches.h"
#include "functions.h"
#include "enums.h"
#include "core2/modelRender.h"
#include "core2/anctrl.h"
#include "animation.h"
#include "transform_ids.h"

u32 recomp_net_is_connected(void);
u32 recomp_net_get_remote_state(u32 player_id, void* out);
u32 recomp_net_get_local_player_id(void);

extern enum map_e map_get(void);
extern s32 cur_drawn_model_transform_id;

#define GHOST_TRANSFORM_ID_START  0x20000000
#define GHOST_TRANSFORM_ID_STRIDE 0x100

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

extern f32 baModelScale;
extern void func_8029A47C(s32 env_color[3]);
extern void func_8033A280(f32);
extern struct5Bs *D_80363780;
extern void func_8033A450(struct5Bs *);
extern void baanim_80289F30(void);
extern void anim_update(Animation *this);
extern void anim_drawSetup(Animation *this);
extern void anim_setIndex(Animation *this, enum asset_e index);
extern Animation *anctrl_getAnimPtr(AnimCtrl *this);

// Shadow model

#define MAX_PLAYERS 4

typedef struct {
    void *model_bin;
    void *shadow_model;
    AnimCtrl *anim_ctrl;
    u16 current_anim;
    f32 smooth_yaw;
    f32 ghost_timer;    // Animation playback timer [0..1]
    f32 anim_duration;  // Current animation duration in seconds
    bool initialized;
} GhostModel;

static GhostModel ghost_models[MAX_PLAYERS] = {0};
static u32 draw_debug_counter = 0;

static f32 lerp_angle(f32 current, f32 target, f32 speed) {
    f32 diff = target - current;
    while (diff > 180.0f) diff -= 360.0f;
    while (diff < -180.0f) diff += 360.0f;
    current += diff * speed;
    while (current >= 360.0f) current -= 360.0f;
    while (current < 0.0f) current += 360.0f;
    return current;
}

static void ghost_ensure_init(u32 pid) {
    GhostModel *gm = &ghost_models[pid];
    if (gm->initialized) return;

    gm->model_bin = assetcache_get(ASSET_34D_MODEL_BANJOKAZOOIE_LOW_POLY);
    if (!gm->model_bin) return;

    gm->shadow_model = assetcache_get(ASSET_3BF_MODEL_PLAYER_SHADOW);

    gm->anim_ctrl = anctrl_new(1);
    if (!gm->anim_ctrl) return;

    anctrl_setIndex(gm->anim_ctrl, ASSET_6F_ANIM_BSSTAND_IDLE);
    anctrl_setPlaybackType(gm->anim_ctrl, ANIMCTRL_LOOP);
    anctrl_setDuration(gm->anim_ctrl, 6.0f);
    _anctrl_start(gm->anim_ctrl, __FILE__, __LINE__);

    gm->current_anim = ASSET_6F_ANIM_BSSTAND_IDLE;
    gm->smooth_yaw = 0.0f;
    gm->ghost_timer = 0.0f;
    gm->anim_duration = 6.0f;
    gm->initialized = TRUE;
}

// Exhaustive BS state -> animation mapping
static void ghost_sync_anim(GhostModel *gm, u8 bs_state) {
    if (!gm->initialized || !gm->anim_ctrl) return;

    u16 anim = ASSET_6F_ANIM_BSSTAND_IDLE;
    u8 playback = ANIMCTRL_LOOP;
    f32 duration = 6.0f;

    switch (bs_state) {
        // === Idle / Movement ===
        case BS_0_NONE:
        case BS_1_IDLE:
        case BS_20_LANDING:    anim = ASSET_6F_ANIM_BSSTAND_IDLE;   duration = 6.0f; playback = ANIMCTRL_LOOP; break;
        case BS_D_TIMEOUT:
        case BS_53_TIMEOUT:    anim = ASSET_77_ANIM_BSTIMEOUT;      duration = 4.0f; playback = ANIMCTRL_LOOP; break;
        case BS_2_WALK_SLOW:
        case BS_WALK_CREEP:    anim = ASSET_2_ANIM_BSWALK_CREEP;    duration = 2.0f; playback = ANIMCTRL_LOOP; break;
        case BS_WALK:          anim = ASSET_3_ANIM_BSWALK;           duration = 1.2f; playback = ANIMCTRL_LOOP; break;
        case BS_4_WALK_FAST:   anim = ASSET_C_ANIM_BSWALK_RUN;      duration = 0.8f; playback = ANIMCTRL_LOOP; break;
        case BS_SKID:          anim = ASSET_E_ANIM_BSTURN;           duration = 0.4f; playback = ANIMCTRL_ONCE; break;
        case BS_SLIDE:         anim = ASSET_5A_ANIM_BSSLIDE_FRONT;  duration = 1.5f; playback = ANIMCTRL_LOOP; break;
        case BS_ROLL:          anim = ASSET_11_ANIM_BSWHIRL_WALK;   duration = 0.5f; playback = ANIMCTRL_LOOP; break;

        // === Jumps / Falls ===
        case BS_5_JUMP:        anim = ASSET_8_ANIM_BSJUMP;           duration = 2.0f; playback = ANIMCTRL_ONCE; break;
        case BS_12_BFLIP:      anim = ASSET_4C_ANIM_BSBFLIP_HOLD;   duration = 1.5f; playback = ANIMCTRL_LOOP; break;
        case BS_2F_FALL:       anim = ASSET_B0_ANIM_BSJUMP_FALL;    duration = 2.0f; playback = ANIMCTRL_LOOP; break;
        case BS_3D_FALL_TUMBLING: anim = ASSET_68_ANIM_BSJUMP_TUMBLE; duration = 0.8f; playback = ANIMCTRL_LOOP; break;
        case BS_4C_LANDING_IN_WATER: anim = ASSET_6F_ANIM_BSSTAND_IDLE; duration = 6.0f; playback = ANIMCTRL_LOOP; break;
        case BS_BSHOCK_CHARGE: anim = ASSET_48_ANIM_BSBSHOCK_CHARGE; duration = 1.0f; playback = ANIMCTRL_LOOP; break;
        case BS_BSHOCK_JUMP:   anim = ASSET_49_ANIM_BSBSHOCK_JUMP;  duration = 1.5f; playback = ANIMCTRL_ONCE; break;

        // === Attacks ===
        case BS_CLAW:          anim = ASSET_5_ANIM_BSPUNCH;          duration = 1.2f; playback = ANIMCTRL_ONCE; break;
        case BS_F_BBUSTER:     anim = ASSET_1D_ANIM_BSBBUSTER;      duration = 1.2f; playback = ANIMCTRL_ONCE; break;
        case BS_BFLAP:         anim = ASSET_17_ANIM_BSBFLAP;         duration = 1.2f; playback = ANIMCTRL_LOOP; break;
        case BS_11_BPECK:      anim = ASSET_1A_ANIM_BSBPECK;         duration = 1.0f; playback = ANIMCTRL_LOOP; break;
        case BS_BBARGE:        anim = ASSET_1C_ANIM_BSBBARGE;        duration = 1.2f; playback = ANIMCTRL_ONCE; break;
        case BS_BOMB:          anim = ASSET_43_ANIM_BSBFLY_BEAKBOMB_START; duration = 1.0f; playback = ANIMCTRL_ONCE; break;

        // === Crouch ===
        case BS_CROUCH:        anim = ASSET_10C_ANIM_BSCROUCH_IDLE;  duration = 6.0f; playback = ANIMCTRL_LOOP; break;

        // === Eggs ===
        case BS_9_EGG_HEAD:    anim = ASSET_2A_ANIM_BSEGGHEAD;       duration = 1.0f; playback = ANIMCTRL_LOOP; break;
        case BS_A_EGG_ASS:     anim = ASSET_2B_ANIM_BSEGGASS;        duration = 1.0f; playback = ANIMCTRL_LOOP; break;

        // === Talon Trot ===
        case BS_14_BTROT_ENTER: anim = ASSET_16_ANIM_BSBTROT_ENTER;  duration = 0.5f; playback = ANIMCTRL_ONCE; break;
        case BS_15_BTROT_IDLE: anim = ASSET_26_ANIM_BSBTROT_IDLE;    duration = 6.0f; playback = ANIMCTRL_LOOP; break;
        case BS_16_BTROT_WALK: anim = ASSET_15_ANIM_BSBTROT_WALK;    duration = 1.0f; playback = ANIMCTRL_LOOP; break;
        case BS_17_BTROT_EXIT: anim = ASSET_7_ANIM_BSBTROT_EXIT;     duration = 0.5f; playback = ANIMCTRL_ONCE; break;
        case BS_8_BTROT_JUMP:  anim = ASSET_27_ANIM_BSBTROR_JUMP;    duration = 1.5f; playback = ANIMCTRL_ONCE; break;
        case BS_45_BTROT_SLIDE: anim = ASSET_44_ANIM_BSBTROT_TURBO_WALK; duration = 0.6f; playback = ANIMCTRL_LOOP; break;

        // === Wonderwing ===
        case BS_1A_WONDERWING_ENTER:
        case BS_1B_WONDERWING_IDLE:
        case BS_1E_WONDERWING_EXIT: anim = ASSET_23_ANIM_BSWONDERWING_IDLE; duration = 2.0f; playback = ANIMCTRL_LOOP; break;
        case BS_1C_WONDERWING_WALK: anim = ASSET_23_ANIM_BSWONDERWING_IDLE; duration = 1.0f; playback = ANIMCTRL_LOOP; break;
        case BS_1D_WONDERWING_JUMP: anim = ASSET_23_ANIM_BSWONDERWING_IDLE; duration = 1.5f; playback = ANIMCTRL_LOOP; break;

        // === Long legs ===
        case BS_25_LONGLEG_ENTER: anim = ASSET_40_ANIM_BSLONGLEG_ENTER_FROM_BTROT; duration = 0.5f; playback = ANIMCTRL_ONCE; break;
        case BS_26_LONGLEG_IDLE: anim = ASSET_41_ANIM_BSLONGLEG_IDLE; duration = 6.0f; playback = ANIMCTRL_LOOP; break;
        case BS_LONGLEG_WALK:  anim = ASSET_42_ANIM_BSLONGLEG_WALK;  duration = 1.0f; playback = ANIMCTRL_LOOP; break;
        case BS_LONGLEG_JUMP:  anim = ASSET_3D_ANIM_BSLONGLEG_JUMP;  duration = 1.5f; playback = ANIMCTRL_ONCE; break;
        case BS_LONGLEG_EXIT:  anim = ASSET_7_ANIM_BSBTROT_EXIT;     duration = 0.5f; playback = ANIMCTRL_ONCE; break;

        // === Flying ===
        case BS_23_FLY_ENTER:  anim = ASSET_45_ANIM_BSBFLY_ENTER;    duration = 1.0f; playback = ANIMCTRL_ONCE; break;
        case BS_24_FLY:        anim = ASSET_38_ANIM_BSBFLY;           duration = 1.5f; playback = ANIMCTRL_LOOP; break;
        case BS_18_FLY_KNOCKBACK:
        case BS_FLY_OW:        anim = ASSET_3E_ANIM_BSBFLY_BEAKBOMB_CRASH; duration = 1.0f; playback = ANIMCTRL_ONCE; break;
        case BS_58_BEAKBOMB_CRASH: anim = ASSET_3E_ANIM_BSBFLY_BEAKBOMB_CRASH; duration = 1.0f; playback = ANIMCTRL_ONCE; break;

        // === Swimming ===
        case BS_2D_SWIM_IDLE:  anim = ASSET_57_ANIM_BSSWIM_IDLE;     duration = 4.0f; playback = ANIMCTRL_LOOP; break;
        case BS_2E_SWIM:       anim = ASSET_39_ANIM_BSSWIM_MOVE;     duration = 1.0f; playback = ANIMCTRL_LOOP; break;
        case BS_30_DIVE_ENTER: anim = ASSET_3C_ANIM_BSSWIM_DIVE_ENTER; duration = 0.8f; playback = ANIMCTRL_ONCE; break;
        case BS_2B_DIVE_IDLE:  anim = ASSET_70_ANIM_BSSWIM_DIVE_IDLE; duration = 4.0f; playback = ANIMCTRL_LOOP; break;
        case BS_2C_DIVE_B:
        case BS_39_DIVE_A:     anim = ASSET_3F_ANIM_BSSWIM_DIVE_MOVE; duration = 1.0f; playback = ANIMCTRL_LOOP; break;
        case BS_54_SWIM_DIE:   anim = ASSET_B9_ANIM_BSSWIM_DIE;      duration = 2.0f; playback = ANIMCTRL_ONCE; break;

        // === Climbing ===
        case BS_4F_CLIMB_IDLE: anim = ASSET_B1_ANIM_BSCLIMB_IDLE_1;  duration = 6.0f; playback = ANIMCTRL_LOOP; break;
        case BS_50_CLIMB_MOVE: anim = ASSET_A_ANIM_BSCLIMB_MOVE;     duration = 1.0f; playback = ANIMCTRL_LOOP; break;

        // === Carrying ===
        case BS_3A_CARRY_IDLE: anim = ASSET_72_ANIM_BSCARRY_IDLE;    duration = 6.0f; playback = ANIMCTRL_LOOP; break;
        case BS_3B_CARRY_WALK: anim = ASSET_73_ANIM_BSCARRY_WALK;    duration = 1.0f; playback = ANIMCTRL_LOOP; break;
        case BS_CARRY_THROW:   anim = ASSET_11B_ANIM_BSTHROW;        duration = 0.8f; playback = ANIMCTRL_ONCE; break;

        // === Damage / Death ===
        case BS_E_OW:          anim = ASSET_4D_ANIM_BSOW;            duration = 1.0f; playback = ANIMCTRL_ONCE; break;
        case BS_56_RECOIL:     anim = ASSET_F_ANIM_BSREBOUND;        duration = 1.0f; playback = ANIMCTRL_ONCE; break;
        case BS_41_DIE:        anim = ASSET_9_ANIM_BSDIE;            duration = 3.0f; playback = ANIMCTRL_ONCE; break;
        case BS_SPLAT:         anim = ASSET_D2_ANIM_BSSPLAT;         duration = 2.0f; playback = ANIMCTRL_ONCE; break;

        // === Talking / Jigs ===
        case BS_3C_TALK:       anim = ASSET_14A_ANIM_BSREST_LISTEN;  duration = 4.0f; playback = ANIMCTRL_LOOP; break;
        case BS_44_JIG_JIGGY:  anim = ASSET_2E_ANIM_BSJIG_JIGGY;    duration = 2.0f; playback = ANIMCTRL_ONCE; break;

        // === Ant (Termite) ===
        case BS_35_ANT_IDLE:   anim = ASSET_5E_ANIM_BSANT_IDLE;      duration = 6.0f; playback = ANIMCTRL_LOOP; break;
        case BS_ANT_WALK:      anim = ASSET_5F_ANIM_BSANT_WALK;      duration = 1.0f; playback = ANIMCTRL_LOOP; break;
        case BS_ANT_JUMP:      anim = ASSET_60_ANIM_BSANT_JUMP;      duration = 1.5f; playback = ANIMCTRL_ONCE; break;
        case BS_38_ANT_FALL:   anim = ASSET_60_ANIM_BSANT_JUMP;      duration = 2.0f; playback = ANIMCTRL_LOOP; break;
        case BS_3E_ANT_OW:     anim = ASSET_28_ANIM_BSANT_OW;        duration = 1.0f; playback = ANIMCTRL_ONCE; break;
        case BS_43_ANT_DIE:    anim = ASSET_29_ANIM_BSANT_DIE;       duration = 3.0f; playback = ANIMCTRL_ONCE; break;

        // === Pumpkin ===
        case BS_48_PUMPKIN_IDLE: anim = ASSET_5E_ANIM_BSANT_IDLE;    duration = 6.0f; playback = ANIMCTRL_LOOP; break;
        case BS_49_PUMPKIN_WALK: anim = ASSET_A0_ANIM_BSPUMPKIN_WALK; duration = 1.0f; playback = ANIMCTRL_LOOP; break;
        case BS_4A_PUMPKIN_JUMP: anim = ASSET_A1_ANIM_BSPUMPKIN_JUMP; duration = 1.5f; playback = ANIMCTRL_ONCE; break;
        case BS_4B_PUMPKIN_FALL: anim = ASSET_A1_ANIM_BSPUMPKIN_JUMP; duration = 2.0f; playback = ANIMCTRL_LOOP; break;
        case BS_4E_PUMPKIN_DIE: anim = ASSET_188_ANIM_BSPUMPKIN_DIE;  duration = 3.0f; playback = ANIMCTRL_ONCE; break;

        // === Crocodile ===
        case BS_5E_CROC_IDLE:  anim = ASSET_E1_ANIM_BSCROC_IDLE;     duration = 6.0f; playback = ANIMCTRL_LOOP; break;
        case BS_CROC_WALK:     anim = ASSET_E0_ANIM_BSCROC_WALK;     duration = 1.0f; playback = ANIMCTRL_LOOP; break;
        case BS_CROC_JUMP:     anim = ASSET_11C_ANIM_BSCROC_JUMP;    duration = 1.5f; playback = ANIMCTRL_ONCE; break;

        // === Walrus ===
        case BS_67_WALRUS_IDLE: anim = ASSET_11F_ANIM_BSWALRUS_IDLE; duration = 6.0f; playback = ANIMCTRL_LOOP; break;
        case BS_WALRUS_WALK:   anim = ASSET_120_ANIM_BSWALRUS_WALK;  duration = 1.0f; playback = ANIMCTRL_LOOP; break;
        case BS_WALRUS_JUMP:   anim = ASSET_121_ANIM_BSWALRUS_JUMP;  duration = 1.5f; playback = ANIMCTRL_ONCE; break;

        default:               anim = ASSET_6F_ANIM_BSSTAND_IDLE;    duration = 6.0f; playback = ANIMCTRL_LOOP; break;
    }

    if (anim != gm->current_anim) {
        gm->current_anim = anim;
        gm->anim_duration = duration;
        gm->ghost_timer = 0.0f; // Reset timer on animation change
    }
}

void bkrecomp_net_draw_ghosts(Gfx **gfx, Mtx **mtx, Vtx **vtx) {
    draw_debug_counter++;
    if (!recomp_net_is_connected()) return;

    AnimCtrl *playerAC = baanim_getAnimCtrlPtr();
    if (!playerAC) return;
    Animation *pa = anctrl_getAnimPtr(playerAC);
    if (!pa) return;

    u32 local_id = recomp_net_get_local_player_id();
    u32 local_map = (u32)map_get();

    for (u32 pid = 0; pid < MAX_PLAYERS; pid++) {
        if (pid == local_id) continue;

        RemoteState rs;
        if (!recomp_net_get_remote_state(pid, &rs)) continue;
        if (rs.map_id != local_map) continue;
        if (rs.x < -15000.0f) continue;

        ghost_ensure_init(pid);
        GhostModel *gm = &ghost_models[pid];
        if (!gm->initialized) continue;

        // Ghost uses the local player's bone transforms
        // (already set by baModel_draw before this function is called)

        // === Position and rotation ===
        gm->smooth_yaw = lerp_angle(gm->smooth_yaw, rs.yaw, 0.25f);

        f32 pos[3] = {rs.x, rs.y, rs.z};
        f32 rot[3] = {rs.pitch, gm->smooth_yaw, 0.0f};
        f32 ref[3] = {0.0f, 0.0f, 0.0f};

        cur_drawn_model_transform_id = GHOST_TRANSFORM_ID_START + (pid * GHOST_TRANSFORM_ID_STRIDE);

        // === Draw shadow ===
        if (gm->shadow_model) {
            f32 sp[3] = {rs.x, rs.y + 4.0f, rs.z};
            f32 sr[3] = {0.0f, 0.0f, 0.0f};
            modelRender_setAlpha(0xFF);
            modelRender_setDepthMode(MODEL_RENDER_DEPTH_COMPARE);
            modelRender_draw(gfx, mtx, sp, sr, 0.43f, 0, gm->shadow_model);
        }

        // === Draw Banjo ===
        s32 env_color[3];
        func_8029A47C(env_color);
        modelRender_setEnvColor(env_color[0], env_color[1], env_color[2], 255);
        func_8033A280(2.0f);
        func_8033A450(D_80363780);
        modelRender_setDepthMode(MODEL_RENDER_DEPTH_FULL);
        modelRender_draw(gfx, mtx, pos, rot, baModelScale, ref, gm->model_bin);

        // No animation restore needed — using local player's bones as-is
    }

    // Restore local player bone transforms
    baanim_80289F30();
}

RECOMP_EXPORT void bkrecomp_net_manage_ghosts(void) {
}

RECOMP_EXPORT void bkrecomp_net_ghost_init(void) {
    recomp_printf("[NetGhost] Ghost system initialized (full anim + shadow)\n");
}
